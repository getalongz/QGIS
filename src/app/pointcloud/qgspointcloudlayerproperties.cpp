/***************************************************************************
  qgspointcloudlayerproperties.cpp
  --------------------------------------
  Date                 : October 2020
  Copyright            : (C) 2020 by Nyall Dawson
  Email                : nyall dot dawson at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgspointcloudlayerproperties.h"

#include "qgsfileutils.h"
#include "qgshelp.h"
#include "qgsmaplayerstylemanager.h"
#include "qgsmaplayerstyleguiutils.h"
#include "qgspointcloudlayer.h"
#include "qgsgui.h"
#include "qgsnative.h"
#include "qgsapplication.h"
#include "qgsmetadatawidget.h"
#include "qgsmaplayerloadstyledialog.h"
#include <QFileDialog>
#include <QMenu>
#include <QMessageBox>
#include <QDesktopServices>

QgsPointCloudLayerProperties::QgsPointCloudLayerProperties( QgsPointCloudLayer *lyr, QgsMapCanvas *canvas, QgsMessageBar *, QWidget *parent, Qt::WindowFlags flags )
  : QgsOptionsDialogBase( QStringLiteral( "PointCloudLayerProperties" ), parent, flags )
  , mLayer( lyr )
  , mMapCanvas( canvas )
{
  setupUi( this );

  connect( this, &QDialog::accepted, this, &QgsPointCloudLayerProperties::apply );
  connect( this, &QDialog::rejected, this, &QgsPointCloudLayerProperties::onCancel );
  connect( buttonBox->button( QDialogButtonBox::Apply ), &QAbstractButton::clicked, this, &QgsPointCloudLayerProperties::apply );
  connect( buttonBox, &QDialogButtonBox::helpRequested, this, &QgsPointCloudLayerProperties::showHelp );

  // QgsOptionsDialogBase handles saving/restoring of geometry, splitter and current tab states,
  // switching vertical tabs between icon/text to icon-only modes (splitter collapsed to left),
  // and connecting QDialogButtonBox's accepted/rejected signals to dialog's accept/reject slots
  initOptionsBase( false );

  mOptsPage_Information->setContentsMargins( 0, 0, 0, 0 );

  QVBoxLayout *layout = new QVBoxLayout( metadataFrame );
  layout->setContentsMargins( 0, 0, 0, 0 );
  metadataFrame->setContentsMargins( 0, 0, 0, 0 );
  mMetadataWidget = new QgsMetadataWidget( this, mLayer );
  mMetadataWidget->layout()->setContentsMargins( 0, 0, 0, 0 );
  mMetadataWidget->setMapCanvas( mMapCanvas );
  layout->addWidget( mMetadataWidget );
  metadataFrame->setLayout( layout );
  mOptsPage_Metadata->setContentsMargins( 0, 0, 0, 0 );

  mAttributeComboBox->setFilters( QgsPointCloudAttributeProxyModel::Numeric );
  mAttributeComboBox->setLayer( mLayer );

  // update based on lyr's current state
  syncToLayer();

  QgsSettings settings;
  if ( !settings.contains( QStringLiteral( "/Windows/PointCloudLayerProperties/tab" ) ) )
  {
    settings.setValue( QStringLiteral( "Windows/PointCloudLayerProperties/tab" ),
                       mOptStackedWidget->indexOf( mOptsPage_Information ) );
  }

  QString title = tr( "Layer Properties - %1" ).arg( mLayer->name() );

  mBtnStyle = new QPushButton( tr( "Style" ) );
  QMenu *menuStyle = new QMenu( this );
  menuStyle->addAction( tr( "Load Style…" ), this, &QgsPointCloudLayerProperties::loadStyle );
  menuStyle->addAction( tr( "Save Style…" ), this, &QgsPointCloudLayerProperties::saveStyleAs );
  menuStyle->addSeparator();
  menuStyle->addAction( tr( "Save as Default" ), this, &QgsPointCloudLayerProperties::saveDefaultStyle );
  menuStyle->addAction( tr( "Restore Default" ), this, &QgsPointCloudLayerProperties::loadDefaultStyle );
  mBtnStyle->setMenu( menuStyle );
  connect( menuStyle, &QMenu::aboutToShow, this, &QgsPointCloudLayerProperties::aboutToShowStyleMenu );

  buttonBox->addButton( mBtnStyle, QDialogButtonBox::ResetRole );

  mBtnMetadata = new QPushButton( tr( "Metadata" ), this );
  QMenu *menuMetadata = new QMenu( this );
  mActionLoadMetadata = menuMetadata->addAction( tr( "Load Metadata…" ), this, &QgsPointCloudLayerProperties::loadMetadata );
  mActionSaveMetadataAs = menuMetadata->addAction( tr( "Save Metadata…" ), this, &QgsPointCloudLayerProperties::saveMetadataAs );
  menuMetadata->addSeparator();
  menuMetadata->addAction( tr( "Save as Default" ), this, &QgsPointCloudLayerProperties::saveDefaultMetadata );
  menuMetadata->addAction( tr( "Restore Default" ), this, &QgsPointCloudLayerProperties::loadDefaultMetadata );

  mBtnMetadata->setMenu( menuMetadata );
  buttonBox->addButton( mBtnMetadata, QDialogButtonBox::ResetRole );

  if ( !mLayer->styleManager()->isDefault( mLayer->styleManager()->currentStyle() ) )
    title += QStringLiteral( " (%1)" ).arg( mLayer->styleManager()->currentStyle() );
  restoreOptionsBaseUi( title );
}

void QgsPointCloudLayerProperties::apply()
{
  mMetadataWidget->acceptMetadata();

  // TODO -- move to proper widget classes!
  mLayer->setCustomProperty( QStringLiteral( "pcAttribute" ), mAttributeComboBox->currentAttribute() );
  mLayer->setCustomProperty( QStringLiteral( "pcMin" ), mMinZSpin->value() );
  mLayer->setCustomProperty( QStringLiteral( "pcMax" ), mMaxZSpin->value() );
  mLayer->setCustomProperty( QStringLiteral( "pcRamp" ), mBtnColorRamp->colorRampName().isEmpty() ? QStringLiteral( "Viridis" ) : mBtnColorRamp->colorRampName() );
  mLayer->triggerRepaint();
}

void QgsPointCloudLayerProperties::onCancel()
{
  if ( mOldStyle.xmlData() != mLayer->styleManager()->style( mLayer->styleManager()->currentStyle() ).xmlData() )
  {
    // need to reset style to previous - style applied directly to the layer (not in apply())
    QString myMessage;
    QDomDocument doc( QStringLiteral( "qgis" ) );
    int errorLine, errorColumn;
    doc.setContent( mOldStyle.xmlData(), false, &myMessage, &errorLine, &errorColumn );
    mLayer->importNamedStyle( doc, myMessage );
    syncToLayer();
  }
}

void QgsPointCloudLayerProperties::syncToLayer()
{
  /*
   * Information Tab
   */
  QString myStyle = QgsApplication::reportStyleSheet();
  myStyle.append( QStringLiteral( "body { margin: 10px; }\n " ) );
  mInformationTextBrowser->clear();
  mInformationTextBrowser->document()->setDefaultStyleSheet( myStyle );
  mInformationTextBrowser->setHtml( mLayer->htmlMetadata() );
  mInformationTextBrowser->setOpenLinks( false );
  connect( mInformationTextBrowser, &QTextBrowser::anchorClicked, this, &QgsPointCloudLayerProperties::urlClicked );

  // TODO -- move to proper widget classes!
  mAttributeComboBox->setAttribute( mLayer->customProperty( QStringLiteral( "pcAttribute" ), QStringLiteral( "Z" ) ).toString() );
  mMinZSpin->setValue( mLayer->customProperty( QStringLiteral( "pcMin" ), 400 ).toInt() );
  mMaxZSpin->setValue( mLayer->customProperty( QStringLiteral( "pcMax" ), 600 ).toInt() );
  mBtnColorRamp->setColorRampFromName( mLayer->customProperty( QStringLiteral( "pcRamp" ), QStringLiteral( "Viridis" ) ).toString() );
  mBtnColorRamp->setColorRampName( mLayer->customProperty( QStringLiteral( "pcRamp" ), QStringLiteral( "Viridis" ) ).toString() );
}


void QgsPointCloudLayerProperties::loadDefaultStyle()
{
  bool defaultLoadedFlag = false;
  QString myMessage = mLayer->loadDefaultStyle( defaultLoadedFlag );
  // reset if the default style was loaded OK only
  if ( defaultLoadedFlag )
  {
    syncToLayer();
  }
  else
  {
    // otherwise let the user know what went wrong
    QMessageBox::information( this,
                              tr( "Default Style" ),
                              myMessage
                            );
  }
}

void QgsPointCloudLayerProperties::saveDefaultStyle()
{
  apply(); // make sure the style to save is up-to-date

  // a flag passed by reference
  bool defaultSavedFlag = false;
  // after calling this the above flag will be set true for success
  // or false if the save operation failed
  QString myMessage = mLayer->saveDefaultStyle( defaultSavedFlag );
  if ( !defaultSavedFlag )
  {
    // let the user know what went wrong
    QMessageBox::information( this,
                              tr( "Default Style" ),
                              myMessage
                            );
  }
}

void QgsPointCloudLayerProperties::loadStyle()
{
  QgsSettings settings;
  QString lastUsedDir = settings.value( QStringLiteral( "style/lastStyleDir" ), QDir::homePath() ).toString();

  QString fileName = QFileDialog::getOpenFileName(
                       this,
                       tr( "Load layer properties from style file" ),
                       lastUsedDir,
                       tr( "QGIS Layer Style File" ) + " (*.qml)" );
  if ( fileName.isEmpty() )
    return;

  // ensure the user never omits the extension from the file name
  if ( !fileName.endsWith( QLatin1String( ".qml" ), Qt::CaseInsensitive ) )
    fileName += QLatin1String( ".qml" );

  mOldStyle = mLayer->styleManager()->style( mLayer->styleManager()->currentStyle() );

  bool defaultLoadedFlag = false;
  QString message = mLayer->loadNamedStyle( fileName, defaultLoadedFlag );
  if ( defaultLoadedFlag )
  {
    settings.setValue( QStringLiteral( "style/lastStyleDir" ), QFileInfo( fileName ).absolutePath() );
    syncToLayer();
  }
  else
  {
    QMessageBox::information( this, tr( "Load Style" ), message );
  }
}

void QgsPointCloudLayerProperties::saveStyleAs()
{
  QgsSettings settings;
  QString lastUsedDir = settings.value( QStringLiteral( "style/lastStyleDir" ), QDir::homePath() ).toString();

  QString outputFileName = QFileDialog::getSaveFileName(
                             this,
                             tr( "Save layer properties as style file" ),
                             lastUsedDir,
                             tr( "QGIS Layer Style File" ) + " (*.qml)" );
  if ( outputFileName.isEmpty() )
    return;

  // ensure the user never omits the extension from the file name
  outputFileName = QgsFileUtils::ensureFileNameHasExtension( outputFileName, QStringList() << QStringLiteral( "qml" ) );

  apply(); // make sure the style to save is up-to-date

  // then export style
  bool defaultLoadedFlag = false;
  QString message;
  message = mLayer->saveNamedStyle( outputFileName, defaultLoadedFlag );

  if ( defaultLoadedFlag )
  {
    settings.setValue( QStringLiteral( "style/lastStyleDir" ), QFileInfo( outputFileName ).absolutePath() );
  }
  else
    QMessageBox::information( this, tr( "Save Style" ), message );
}

void QgsPointCloudLayerProperties::aboutToShowStyleMenu()
{
  QMenu *m = qobject_cast<QMenu *>( sender() );

  QgsMapLayerStyleGuiUtils::instance()->removesExtraMenuSeparators( m );
  // re-add style manager actions!
  m->addSeparator();
  QgsMapLayerStyleGuiUtils::instance()->addStyleManagerActions( m, mLayer );
}

void QgsPointCloudLayerProperties::loadMetadata()
{
  QgsSettings myQSettings;  // where we keep last used filter in persistent state
  QString myLastUsedDir = myQSettings.value( QStringLiteral( "style/lastStyleDir" ), QDir::homePath() ).toString();

  QString myFileName = QFileDialog::getOpenFileName( this, tr( "Load layer metadata from metadata file" ), myLastUsedDir,
                       tr( "QGIS Layer Metadata File" ) + " (*.qmd)" );
  if ( myFileName.isNull() )
  {
    return;
  }

  QString myMessage;
  bool defaultLoadedFlag = false;
  myMessage = mLayer->loadNamedMetadata( myFileName, defaultLoadedFlag );

  //reset if the default style was loaded OK only
  if ( defaultLoadedFlag )
  {
    mMetadataWidget->setMetadata( &mLayer->metadata() );
  }
  else
  {
    //let the user know what went wrong
    QMessageBox::warning( this, tr( "Load Metadata" ), myMessage );
  }

  QFileInfo myFI( myFileName );
  QString myPath = myFI.path();
  myQSettings.setValue( QStringLiteral( "style/lastStyleDir" ), myPath );

  activateWindow(); // set focus back to properties dialog
}

void QgsPointCloudLayerProperties::saveMetadataAs()
{
  QgsSettings myQSettings;  // where we keep last used filter in persistent state
  QString myLastUsedDir = myQSettings.value( QStringLiteral( "style/lastStyleDir" ), QDir::homePath() ).toString();

  QString myOutputFileName = QFileDialog::getSaveFileName( this, tr( "Save Layer Metadata as QMD" ),
                             myLastUsedDir, tr( "QMD File" ) + " (*.qmd)" );
  if ( myOutputFileName.isNull() ) //dialog canceled
  {
    return;
  }

  mMetadataWidget->acceptMetadata();

  //ensure the user never omitted the extension from the file name
  if ( !myOutputFileName.endsWith( QgsMapLayer::extensionPropertyType( QgsMapLayer::Metadata ), Qt::CaseInsensitive ) )
  {
    myOutputFileName += QgsMapLayer::extensionPropertyType( QgsMapLayer::Metadata );
  }

  bool defaultLoadedFlag = false;
  QString message = mLayer->saveNamedMetadata( myOutputFileName, defaultLoadedFlag );
  if ( defaultLoadedFlag )
    myQSettings.setValue( QStringLiteral( "style/lastStyleDir" ), QFileInfo( myOutputFileName ).absolutePath() );
  else
    QMessageBox::information( this, tr( "Save Metadata" ), message );
}

void QgsPointCloudLayerProperties::saveDefaultMetadata()
{
  mMetadataWidget->acceptMetadata();

  bool defaultSavedFlag = false;
  QString errorMsg = mLayer->saveDefaultMetadata( defaultSavedFlag );
  if ( !defaultSavedFlag )
  {
    QMessageBox::warning( this, tr( "Default Metadata" ), errorMsg );
  }
}

void QgsPointCloudLayerProperties::loadDefaultMetadata()
{
  bool defaultLoadedFlag = false;
  QString myMessage = mLayer->loadNamedMetadata( mLayer->metadataUri(), defaultLoadedFlag );
  //reset if the default metadata was loaded OK only
  if ( defaultLoadedFlag )
  {
    mMetadataWidget->setMetadata( &mLayer->metadata() );
  }
  else
  {
    QMessageBox::information( this, tr( "Default Metadata" ), myMessage );
  }
}

void QgsPointCloudLayerProperties::showHelp()
{
  const QVariant helpPage = mOptionsStackedWidget->currentWidget()->property( "helpPage" );

  if ( helpPage.isValid() )
  {
    QgsHelp::openHelp( helpPage.toString() );
  }
  else
  {
    QgsHelp::openHelp( QStringLiteral( "working_with_vector_tiles/vector_tiles_properties.html" ) );
  }
}

void QgsPointCloudLayerProperties::urlClicked( const QUrl &url )
{
  QFileInfo file( url.toLocalFile() );
  if ( file.exists() && !file.isDir() )
    QgsGui::instance()->nativePlatformInterface()->openFileExplorerAndSelectFile( url.toLocalFile() );
  else
    QDesktopServices::openUrl( url );
}

void QgsPointCloudLayerProperties::optionsStackedWidget_CurrentChanged( int index )
{
  QgsOptionsDialogBase::optionsStackedWidget_CurrentChanged( index );

  bool isMetadataPanel = ( index == mOptStackedWidget->indexOf( mOptsPage_Metadata ) );
  mBtnStyle->setVisible( ! isMetadataPanel );
  mBtnMetadata->setVisible( isMetadataPanel );
}
