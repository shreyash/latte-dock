/*
 * Copyright 2017  Smith AR <audoban@openmailbox.org>
 *                 Michail Vourlakos <mvourlakos@gmail.com>
 *
 * This file is part of Latte-Dock
 *
 * Latte-Dock is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * Latte-Dock is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "settingsdialog.h"

// local
#include "universalsettings.h"
#include "ui_settingsdialog.h"
#include "../lattecorona.h"
#include "../screenpool.h"
#include "../layout/centrallayout.h"
#include "../layouts/importer.h"
#include "../layouts/manager.h"
#include "../layouts/synchronizer.h"
#include "../liblatte2/types.h"
#include "../plasma/extended/theme.h"
#include "data/layoutdata.h"
#include "tools/settingstools.h"

// Qt
#include <QButtonGroup>
#include <QColorDialog>
#include <QDir>
#include <QFileDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>

// KDE
#include <KActivities/Controller>
#include <KIO/OpenFileManagerWindowJob>
#include <KLocalizedString>
#include <KWindowSystem>
#include <KNewStuff3/KNS3/DownloadDialog>


#define TWINENABLED "Enabled"
#define TWINVISIBLE "Visible"
#define TWINCHECKED "Checked"

namespace Latte {

const int SettingsDialog::INFORMATIONINTERVAL;
const int SettingsDialog::INFORMATIONWITHACTIONINTERVAL;
const int SettingsDialog::WARNINGINTERVAL;
const int SettingsDialog::ERRORINTERVAL;

SettingsDialog::SettingsDialog(QWidget *parent, Latte::Corona *corona)
    : QDialog(parent),
      m_ui(new Ui::SettingsDialog),
      m_corona(corona)
{
    setAcceptDrops(true);
    m_ui->setupUi(this);

    setAttribute(Qt::WA_DeleteOnClose, true);
    setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint);
    resize(m_corona->universalSettings()->layoutsWindowSize());

    connect(m_ui->buttonBox->button(QDialogButtonBox::Apply), &QPushButton::clicked
            , this, &SettingsDialog::apply);
    connect(m_ui->buttonBox->button(QDialogButtonBox::Reset), &QPushButton::clicked
            , this, &SettingsDialog::reset);
    connect(m_ui->buttonBox->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked
            , this, &SettingsDialog::restoreDefaults);

    m_preferencesHandler = new Settings::Handler::Preferences(this, m_corona);
    m_layoutsController = new Settings::Controller::Layouts(this, m_corona, m_ui->layoutsView);

    m_inMemoryButtons = new QButtonGroup(this);
    m_inMemoryButtons->addButton(m_ui->singleToolBtn, Latte::Types::SingleLayout);
    m_inMemoryButtons->addButton(m_ui->multipleToolBtn, Latte::Types::MultipleLayouts);
    m_inMemoryButtons->setExclusive(true);

    if (KWindowSystem::isPlatformWayland()) {
        m_inMemoryButtons->button(Latte::Types::MultipleLayouts)->setEnabled(false);
    }

    m_ui->messageWidget->setVisible(false);

    //! Global Menu
    initGlobalMenu();

    m_ui->buttonBox->button(QDialogButtonBox::Apply)->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_S));
    m_ui->buttonBox->button(QDialogButtonBox::Reset)->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_L));

    m_openUrlAction = new QAction(i18n("Open Location..."), this);
    connect(m_openUrlAction, &QAction::triggered, this, [&]() {
        QString file = m_openUrlAction->data().toString();

        if (!file.isEmpty()) {
            KIO::highlightInFileManager({file});
        }
    });

    loadSettings();

    //! SIGNALS
    connect(m_ui->layoutsView->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [&]() {
        updatePerLayoutButtonsState();
        updateApplyButtonsState();
    });

    connect(m_layoutsController, &Settings::Controller::Layouts::dataChanged, this, [&]() {
        updateApplyButtonsState();
        updatePerLayoutButtonsState();
    });

    connect(m_inMemoryButtons, static_cast<void(QButtonGroup::*)(int, bool)>(&QButtonGroup::buttonToggled),
            [ = ](int id, bool checked) {

        if (checked) {
            m_layoutsController->setInMultipleMode(id == Latte::Types::MultipleLayouts);
        }
    });

    connect(m_ui->tabWidget, &QTabWidget::currentChanged, this, &SettingsDialog::on_currentPageChanged);

    connect(m_preferencesHandler, &Settings::Handler::Preferences::dataChanged, this, &SettingsDialog::updateApplyButtonsState);
    connect(m_preferencesHandler, &Settings::Handler::Preferences::borderlessMaximizedChanged,  this, [&]() {
        bool noBordersForMaximized = m_ui->noBordersForMaximizedChkBox->isChecked();

        if (noBordersForMaximized) {
            m_ui->layoutsView->setColumnHidden(Settings::Model::Layouts::BORDERSCOLUMN, false);
        } else {
            m_ui->layoutsView->setColumnHidden(Settings::Model::Layouts::BORDERSCOLUMN, true);
        }
    });

    //! timers
    m_activitiesTimer.setSingleShot(true);
    m_activitiesTimer.setInterval(750);
    connect(&m_activitiesTimer, &QTimer::timeout, this, &SettingsDialog::updateWindowActivities);
    m_activitiesTimer.start();

    m_hideInlineMessageTimer.setSingleShot(true);
    m_hideInlineMessageTimer.setInterval(2000);
    connect(&m_hideInlineMessageTimer, &QTimer::timeout, this, [&]() {
        m_ui->messageWidget->animatedHide();
        m_ui->messageWidget->removeAction(m_openUrlAction);
    });

    connect(m_ui->messageWidget, &KMessageWidget::hideAnimationFinished, this, [&]() {
        m_ui->messageWidget->removeAction(m_openUrlAction);
    });
}

SettingsDialog::~SettingsDialog()
{
    qDebug() << Q_FUNC_INFO;

    m_corona->universalSettings()->setLayoutsWindowSize(size());
}

void SettingsDialog::initGlobalMenu()
{
    m_globalMenuBar = new QMenuBar(this);

    layout()->setMenuBar(m_globalMenuBar);

    initFileMenu();
    initLayoutMenu();
    initHelpMenu();
}

void SettingsDialog::initLayoutMenu()
{
    if (!m_layoutMenu) {
        m_layoutMenu = new QMenu(i18n("Layout"), m_globalMenuBar);
        m_globalMenuBar->addMenu(m_layoutMenu);
    }

    m_switchLayoutAction = m_layoutMenu->addAction(i18nc("switch layout","Switch"));
    m_switchLayoutAction->setToolTip(i18n("Switch to selected layout"));
    m_switchLayoutAction->setIcon(QIcon::fromTheme("user-identity"));
    m_switchLayoutAction->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_Tab));
    twinActionWithButton(m_ui->switchButton, m_switchLayoutAction);
    connect(m_switchLayoutAction, &QAction::triggered, this, &SettingsDialog::on_switch_layout);

    m_pauseLayoutAction = m_layoutMenu->addAction(i18nc("pause layout", "&Pause"));
    m_pauseLayoutAction->setToolTip(i18n("Switch to selected layout"));
    m_pauseLayoutAction->setIcon(QIcon::fromTheme("media-playback-pause"));
    m_pauseLayoutAction->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_P));
    twinActionWithButton(m_ui->pauseButton, m_pauseLayoutAction);
    connect(m_pauseLayoutAction, &QAction::triggered, this, &SettingsDialog::on_pause_layout);

    m_layoutMenu->addSeparator();

    m_newLayoutAction = m_layoutMenu->addAction(i18nc("new layout", "&New"));
    m_newLayoutAction->setToolTip(i18n("New layout"));
    m_newLayoutAction->setIcon(QIcon::fromTheme("add"));
    m_newLayoutAction->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_N));
    twinActionWithButton(m_ui->newButton, m_newLayoutAction);
    connect(m_newLayoutAction, &QAction::triggered, this, &SettingsDialog::on_new_layout);

    m_copyLayoutAction = m_layoutMenu->addAction(i18nc("copy layout", "&Copy"));
    m_copyLayoutAction->setToolTip(i18n("Copy selected layout"));
    m_copyLayoutAction->setIcon(QIcon::fromTheme("edit-copy"));
    m_copyLayoutAction->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_C));
    twinActionWithButton(m_ui->copyButton, m_copyLayoutAction);
    connect(m_copyLayoutAction, &QAction::triggered, this, &SettingsDialog::on_copy_layout);

    m_removeLayoutAction = m_layoutMenu->addAction(i18nc("remove layout", "Remove"));
    m_removeLayoutAction->setToolTip(i18n("Remove selected layout"));
    m_removeLayoutAction->setIcon(QIcon::fromTheme("delete"));
    m_removeLayoutAction->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_D));
    twinActionWithButton(m_ui->removeButton, m_removeLayoutAction);
    connect(m_removeLayoutAction, &QAction::triggered, this, &SettingsDialog::on_remove_layout);

    m_layoutMenu->addSeparator();

    m_lockedLayoutAction = m_layoutMenu->addAction(i18nc("locked layout", "&Locked"));
    m_lockedLayoutAction->setToolTip(i18n("Lock/Unlock selected layout and make it read-only"));
    m_lockedLayoutAction->setIcon(QIcon::fromTheme("object-locked"));
    m_lockedLayoutAction->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_L));
    m_lockedLayoutAction->setCheckable(true);
    twinActionWithButton(m_ui->lockedButton, m_lockedLayoutAction);
    connect(m_lockedLayoutAction, &QAction::triggered, this, &SettingsDialog::on_locked_layout);

    m_sharedLayoutAction = m_layoutMenu->addAction(i18nc("shared layout", "Sha&red"));
    m_sharedLayoutAction->setToolTip(i18n("Share selected layout with other central layouts"));
    m_sharedLayoutAction->setIcon(QIcon::fromTheme("document-share"));
    m_sharedLayoutAction->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_R));
    m_sharedLayoutAction->setCheckable(true);
    twinActionWithButton(m_ui->sharedButton, m_sharedLayoutAction);
    connect(m_sharedLayoutAction, &QAction::triggered, this, &SettingsDialog::on_shared_layout);

    m_layoutMenu->addSeparator();

    m_importLayoutAction = m_layoutMenu->addAction(i18nc("import layout", "&Import..."));
    m_importLayoutAction->setToolTip(i18n("Import layout file from your system"));
    m_importLayoutAction->setIcon(QIcon::fromTheme("document-import"));
    m_importLayoutAction->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_I));
    twinActionWithButton(m_ui->importButton, m_importLayoutAction);
    connect(m_importLayoutAction, &QAction::triggered, this, &SettingsDialog::on_import_layout);

    m_exportLayoutAction = m_layoutMenu->addAction(i18nc("export layout", "&Export..."));
    m_exportLayoutAction->setToolTip(i18n("Export selected layout at your system"));
    m_exportLayoutAction->setIcon(QIcon::fromTheme("document-export"));
    m_exportLayoutAction->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT  + Qt::Key_E));
    twinActionWithButton(m_ui->exportButton, m_exportLayoutAction);
    connect(m_exportLayoutAction, &QAction::triggered, this, &SettingsDialog::on_export_layout);

    m_downloadLayoutAction = m_layoutMenu->addAction(i18nc("download layout", "&Download..."));
    m_downloadLayoutAction->setToolTip(i18n("Download community layouts from KDE Store"));
    m_downloadLayoutAction->setIcon(QIcon::fromTheme("get-hot-new-stuff"));
    m_downloadLayoutAction->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_D));
    twinActionWithButton(m_ui->downloadButton, m_downloadLayoutAction);
    connect(m_downloadLayoutAction, &QAction::triggered, this, &SettingsDialog::on_download_layout);
}

void SettingsDialog::initFileMenu()
{
    if (!m_fileMenu) {
        m_fileMenu = new QMenu(i18n("File"), m_globalMenuBar);
        m_globalMenuBar->addMenu(m_fileMenu);
    }

    m_importFullAction = m_fileMenu->addAction(i18n("Import Configuration..."));
    m_importFullAction->setIcon(QIcon::fromTheme("document-import"));
    m_importFullAction->setShortcut(QKeySequence(Qt::CTRL + Qt::ALT + Qt::Key_I));
    m_importFullAction->setToolTip(i18n("Import your full configuration from previous backup"));
    connect(m_importFullAction, &QAction::triggered, this, &SettingsDialog::on_import_fullconfiguration);

    m_exportFullAction = m_fileMenu->addAction(i18n("Export Configuration..."));
    m_exportFullAction->setIcon(QIcon::fromTheme("document-export"));
    m_exportFullAction->setShortcut(QKeySequence(Qt::CTRL + Qt::ALT + Qt::Key_E));
    m_exportFullAction->setToolTip(i18n("Export your full configuration to create backup"));
    connect(m_exportFullAction, &QAction::triggered, this, &SettingsDialog::on_export_fullconfiguration);

    m_fileMenu->addSeparator();

    QAction *screensAction = m_fileMenu->addAction(i18n("Sc&reens..."));
    screensAction->setIcon(QIcon::fromTheme("document-properties"));
    //screensAction->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_R));

    QAction *quitAction = m_fileMenu->addAction(i18n("&Quit Latte"));
    quitAction->setIcon(QIcon::fromTheme("application-exit"));
    quitAction->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_Q));


    //! triggers
    connect(quitAction, &QAction::triggered, this, [&]() {
        close();
        m_corona->quitApplication();
    });

}

void SettingsDialog::initHelpMenu()
{
    if (!m_helpMenu) {
        m_helpMenu = new KHelpMenu(m_globalMenuBar);
        m_globalMenuBar->addMenu(m_helpMenu->menu());
    }

    //! hide help menu actions that are not used
    m_helpMenu->action(KHelpMenu::menuHelpContents)->setVisible(false);
    m_helpMenu->action(KHelpMenu::menuWhatsThis)->setVisible(false);
}

Ui::SettingsDialog *SettingsDialog::ui() const
{
    return m_ui;
}

void SettingsDialog::twinActionWithButton(QPushButton *button, QAction *action)
{
    button->setText(action->text());
    button->setToolTip(action->toolTip());
    button->setWhatsThis(action->whatsThis());
    button->setIcon(action->icon());
    button->setCheckable(action->isCheckable());
    button->setChecked(action->isChecked());

    m_twinActions[action] = button;

    connect(button, &QPushButton::clicked, action, &QAction::trigger);
}

void SettingsDialog::setTwinProperty(QAction *action, const QString &property, QVariant value)
{
    if (!m_twinActions.contains(action)) {
        return;
    }

    if (property == TWINVISIBLE) {
        action->setVisible(value.toBool());
        m_twinActions[action]->setVisible(value.toBool());
    } else if (property == TWINENABLED) {
        action->setEnabled(value.toBool());
        m_twinActions[action]->setEnabled(value.toBool());
    } else if (property == TWINCHECKED) {
        action->setChecked(value.toBool());
        m_twinActions[action]->setChecked(value.toBool());
    }
}

Types::LatteConfigPage SettingsDialog::currentPage()
{
    Types::LatteConfigPage cPage= static_cast<Types::LatteConfigPage>(m_ui->tabWidget->currentIndex());

    return cPage;
}

void SettingsDialog::toggleCurrentPage()
{
    if (m_ui->tabWidget->currentIndex() == 0) {
        m_ui->tabWidget->setCurrentIndex(1);
    } else {
        m_ui->tabWidget->setCurrentIndex(0);
    }
}

void SettingsDialog::setCurrentPage(int page)
{
    m_ui->tabWidget->setCurrentIndex(page);
}

void SettingsDialog::on_currentPageChanged(int page)
{
    Types::LatteConfigPage cPage= static_cast<Types::LatteConfigPage>(page);

    if (cPage == Types::LayoutPage) {
        m_layoutMenu->setEnabled(true);
        m_layoutMenu->menuAction()->setVisible(true);

    } else {
        m_layoutMenu->menuAction()->setVisible(false);
        m_layoutMenu->setEnabled(false);
    }

    updateApplyButtonsState();
}

void SettingsDialog::on_new_layout()
{
    qDebug() << Q_FUNC_INFO;

    if (!m_layoutMenu->isEnabled() || !m_newLayoutAction->isEnabled() || currentPage() != Types::LayoutPage) {
        return;
    }

    //! find Default preset path
    for (const auto &preset : m_corona->layoutsManager()->presetsPaths()) {
        QString presetName = CentralLayout::layoutName(preset);

        if (presetName == "Default") {
            Settings::Data::Layout newlayout = m_layoutsController->addLayoutForFile(preset, presetName, true);
            showInlineMessage(i18nc("settings:layout added successfully","Layout <b>%0</b> added successfully...").arg(newlayout.name),
                              KMessageWidget::Information,
                              SettingsDialog::INFORMATIONINTERVAL);
            break;
        }
    }
}

void SettingsDialog::on_copy_layout()
{
    qDebug() << Q_FUNC_INFO;

    if (!m_layoutMenu->isEnabled() || !m_copyLayoutAction->isEnabled() || currentPage() != Types::LayoutPage) {
        return;
    }

    m_layoutsController->copySelectedLayout();
}

void SettingsDialog::on_download_layout()
{
    qDebug() << Q_FUNC_INFO;

    if (!m_layoutMenu->isEnabled() || !m_downloadLayoutAction->isEnabled() || currentPage() != Types::LayoutPage) {
        return;
    }

    KNS3::DownloadDialog dialog(QStringLiteral("latte-layouts.knsrc"), this);
    dialog.resize(m_corona->universalSettings()->downloadWindowSize());
    dialog.exec();

    if (!dialog.changedEntries().isEmpty() || !dialog.installedEntries().isEmpty()) {
        for (const auto &entry : dialog.installedEntries()) {
            for (const auto &entryFile : entry.installedFiles()) {
                Layouts::Importer::LatteFileVersion version = Layouts::Importer::fileVersion(entryFile);

                if (version == Layouts::Importer::LayoutVersion2) {
                    Settings::Data::Layout downloaded = m_layoutsController->addLayoutForFile(entryFile);
                    showInlineMessage(i18nc("settings:layout downloaded successfully","Layout <b>%0</b> downloaded successfully...").arg(downloaded.name),
                                      KMessageWidget::Information,
                                      SettingsDialog::INFORMATIONINTERVAL);
                    break;
                }
            }
        }
    }

    m_corona->universalSettings()->setDownloadWindowSize(dialog.size());
}

void SettingsDialog::on_remove_layout()
{
    qDebug() << Q_FUNC_INFO;

    if (!m_layoutMenu->isEnabled() || !m_removeLayoutAction->isEnabled() || currentPage() != Types::LayoutPage) {
        return;
    }

    if (!m_layoutsController->hasSelectedLayout()) {
        return;
    }

    Settings::Data::Layout selectedLayout = m_layoutsController->selectedLayoutCurrentData();

    if (selectedLayout.isActive) {
        showInlineMessage(i18nc("settings: active layout remove","<b>Active</b> layouts can not be removed..."),
                          KMessageWidget::Error,
                          SettingsDialog::WARNINGINTERVAL);
        return;
    }

    if (selectedLayout.isLocked) {
        showInlineMessage(i18nc("settings: locked layout remove","Locked layouts can not be removed..."),
                          KMessageWidget::Error,
                          SettingsDialog::WARNINGINTERVAL);
        return;
    }

    qDebug() << Q_FUNC_INFO;

    m_layoutsController->removeSelected();

    updateApplyButtonsState();
}

void SettingsDialog::on_locked_layout()
{
    qDebug() << Q_FUNC_INFO;

    if (!m_layoutMenu->isEnabled() || !m_lockedLayoutAction->isEnabled() || currentPage() != Types::LayoutPage) {
        return;
    }

    m_layoutsController->toggleLockedForSelected();

    updatePerLayoutButtonsState();
    updateApplyButtonsState();
}

void SettingsDialog::on_shared_layout()
{
    qDebug() << Q_FUNC_INFO;

    if (!m_layoutMenu->isEnabled() || !m_sharedLayoutAction->isEnabled() || currentPage() != Types::LayoutPage) {
        return;
    }

    m_layoutsController->toggleSharedForSelected();

    updatePerLayoutButtonsState();
    updateApplyButtonsState();
}

void SettingsDialog::on_import_layout()
{
    qDebug() << Q_FUNC_INFO;

    if (!m_layoutMenu->isEnabled() || !m_importLayoutAction->isEnabled() || currentPage() != Types::LayoutPage) {
        return;
    }

    QFileDialog *importFileDialog = new QFileDialog(this, i18nc("import layout", "Import Layout")
                                                    , QDir::homePath()
                                                    , QStringLiteral("layout.latte"));

    importFileDialog->setWindowIcon(QIcon::fromTheme("document-import"));
    importFileDialog->setLabelText(QFileDialog::Accept, i18nc("import layout","Import"));
    importFileDialog->setFileMode(QFileDialog::AnyFile);
    importFileDialog->setAcceptMode(QFileDialog::AcceptOpen);
    importFileDialog->setDefaultSuffix("layout.latte");

    QStringList filters;
    filters << QString(i18nc("import latte layout", "Latte Dock Layout file v0.2") + "(*.layout.latte)")
            << QString(i18nc("import older latte layout", "Latte Dock Layout file v0.1") + "(*.latterc)");
    importFileDialog->setNameFilters(filters);

    connect(importFileDialog, &QFileDialog::finished, importFileDialog, &QFileDialog::deleteLater);

    connect(importFileDialog, &QFileDialog::fileSelected, this, [&](const QString & file) {
        Layouts::Importer::LatteFileVersion version = Layouts::Importer::fileVersion(file);
        qDebug() << "VERSION :::: " << version;

        if (version == Layouts::Importer::LayoutVersion2) {
            Settings::Data::Layout importedlayout = m_layoutsController->addLayoutForFile(file);
            showInlineMessage(i18nc("settings:layout imported successfully","Layout <b>%0</b> imported successfully...").arg(importedlayout.name),
                              KMessageWidget::Information,
                              SettingsDialog::INFORMATIONINTERVAL);
        } else if (version == Layouts::Importer::ConfigVersion1) {
            if (!m_layoutsController->importLayoutsFromV1ConfigFile(file)) {
                showInlineMessage(i18nc("settings:deprecated layouts import failed","Import layouts from deprecated version <b>failed</b>..."),
                                  KMessageWidget::Error);
            }
        }
    });

    importFileDialog->open();
}

void SettingsDialog::on_import_fullconfiguration()
{
    qDebug() << Q_FUNC_INFO;

    QFileDialog *importFileDialog = new QFileDialog(this, i18nc("import full configuration", "Import Full Configuration")
                                                    , QDir::homePath()
                                                    , QStringLiteral("latterc"));

    importFileDialog->setWindowIcon(QIcon::fromTheme("document-import"));
    importFileDialog->setLabelText(QFileDialog::Accept, i18nc("import full configuration","Import"));
    importFileDialog->setFileMode(QFileDialog::AnyFile);
    importFileDialog->setAcceptMode(QFileDialog::AcceptOpen);
    importFileDialog->setDefaultSuffix("latterc");

    QStringList filters;
    filters << QString(i18nc("import full configuration", "Latte Dock Full Configuration file") + "(*.latterc)");
    importFileDialog->setNameFilters(filters);

    connect(importFileDialog, &QFileDialog::finished, importFileDialog, &QFileDialog::deleteLater);

    connect(importFileDialog, &QFileDialog::fileSelected, this, [&](const QString & file) {
        Layouts::Importer::LatteFileVersion version = Layouts::Importer::fileVersion(file);
        qDebug() << "VERSION :::: " << version;

        if (version == Layouts::Importer::ConfigVersion2
                || version == Layouts::Importer::ConfigVersion1) {
            auto msg = new QMessageBox(this);
            msg->setIcon(QMessageBox::Warning);
            msg->setWindowTitle(i18n("Import: Full Configuration File"));
            msg->setText(i18n("You are importing full configuration file. Be careful, all <b>current settings and layouts will be lost</b>. It is advised to <b>take backup</b> first!<br>"));
            msg->setStandardButtons(QMessageBox::Cancel);

            QPushButton *takeBackupBtn = new QPushButton(msg);
            takeBackupBtn->setText(i18nc("export full configuration", "Take Backup..."));
            takeBackupBtn->setIcon(QIcon::fromTheme("document-export"));
            takeBackupBtn->setToolTip(i18n("Export your full configuration in order to take backup"));

            QPushButton *importBtn = new QPushButton(msg);
            importBtn->setText(i18nc("import full configuration", "Import"));
            importBtn->setIcon(QIcon::fromTheme("document-import"));
            importBtn->setToolTip(i18n("Import your full configuration and drop all your current settings and layouts"));

            msg->addButton(takeBackupBtn, QMessageBox::AcceptRole);
            msg->addButton(importBtn, QMessageBox::AcceptRole);
            msg->setDefaultButton(takeBackupBtn);

            connect(msg, &QFileDialog::finished, msg, &QFileDialog::deleteLater);

            connect(importBtn, &QPushButton::clicked, this, [&, file](bool check) {
                m_corona->importFullConfiguration(file);
            });

            connect(takeBackupBtn, &QPushButton::clicked, this, [&](bool check) {
                on_export_fullconfiguration();
            });

            msg->open();
        }
    });

    importFileDialog->open();
}

void SettingsDialog::on_export_layout()
{
    qDebug() << Q_FUNC_INFO;

    if (!m_layoutMenu->isEnabled() || !m_exportLayoutAction->isEnabled() || currentPage() != Types::LayoutPage) {
        return;
    }

    if (!m_layoutsController->hasSelectedLayout()) {
        return;
    }

    Settings::Data::Layout selectedLayout = m_layoutsController->selectedLayoutCurrentData();

    //! Update ALL active original layouts before exporting,
    m_corona->layoutsManager()->synchronizer()->syncActiveLayoutsToOriginalFiles();

    QFileDialog *exportFileDialog = new QFileDialog(this, i18n("Export Layout"), QDir::homePath(), QStringLiteral("layout.latte"));

    exportFileDialog->setLabelText(QFileDialog::Accept, i18nc("export layout","Export"));
    exportFileDialog->setFileMode(QFileDialog::AnyFile);
    exportFileDialog->setAcceptMode(QFileDialog::AcceptSave);
    exportFileDialog->setDefaultSuffix("layout.latte");

    QStringList filters;
    QString filter1(i18nc("export layout", "Latte Dock Layout file v0.2") + "(*.layout.latte)");

    filters << filter1;

    exportFileDialog->setNameFilters(filters);

    connect(exportFileDialog, &QFileDialog::finished, exportFileDialog, &QFileDialog::deleteLater);

    connect(exportFileDialog, &QFileDialog::fileSelected, this, [ &, selectedLayout](const QString & file) {
        auto showExportLayoutError = [this](const Settings::Data::Layout &layout) {
            showInlineMessage(i18nc("settings:layout export fail","Layout <b>%0</b> export <b>failed</b>...").arg(layout.name),
                              KMessageWidget::Error);
        };

        if (QFile::exists(file) && !QFile::remove(file)) {
            showExportLayoutError(selectedLayout);
            return;
        }

        if (file.endsWith(".layout.latte")) {
            if (!QFile(selectedLayout.id).copy(file)) {
                showExportLayoutError(selectedLayout);
                return;
            }

            QFileInfo newFileInfo(file);

            if (newFileInfo.exists() && !newFileInfo.isWritable()) {
                QFile(file).setPermissions(QFileDevice::ReadUser | QFileDevice::WriteUser | QFileDevice::ReadGroup | QFileDevice::ReadOther);
            }

            CentralLayout layoutS(this, file);
            layoutS.setActivities(QStringList());
            layoutS.clearLastUsedActivity();

            m_openUrlAction->setData(file);
            m_ui->messageWidget->addAction(m_openUrlAction);
            showInlineMessage(i18nc("settings:layout export success","Layout <b>%0</b> export succeeded...").arg(selectedLayout.name),
                              KMessageWidget::Information,
                              SettingsDialog::INFORMATIONWITHACTIONINTERVAL);
        } else if (file.endsWith(".latterc")) {
            auto showExportConfigurationError = [this]() {
                showInlineMessage(i18n("Full configuration export <b>failed</b>..."), KMessageWidget::Error);
            };

            if (m_corona->layoutsManager()->importer()->exportFullConfiguration(file)) {
                m_openUrlAction->setData(file);
                m_ui->messageWidget->addAction(m_openUrlAction);
                showInlineMessage(i18n("Full configuration export succeeded..."),
                                  KMessageWidget::Information,
                                  SettingsDialog::INFORMATIONWITHACTIONINTERVAL);
            } else {
                showExportConfigurationError();
            }
        }
    });

    exportFileDialog->open();
    exportFileDialog->selectFile(selectedLayout.name);
}

void SettingsDialog::on_export_fullconfiguration()
{
    //! Update ALL active original layouts before exporting,
    m_corona->layoutsManager()->synchronizer()->syncActiveLayoutsToOriginalFiles();

    QFileDialog *exportFileDialog = new QFileDialog(this, i18n("Export Full Configuration"),
                                                    QDir::homePath(),
                                                    QStringLiteral("latterc"));

    exportFileDialog->setLabelText(QFileDialog::Accept, i18nc("export full configuration","Export"));
    exportFileDialog->setFileMode(QFileDialog::AnyFile);
    exportFileDialog->setAcceptMode(QFileDialog::AcceptSave);
    exportFileDialog->setDefaultSuffix("latterc");

    QStringList filters;
    QString filter2(i18nc("export full configuration", "Latte Dock Full Configuration file v0.2") + "(*.latterc)");

    filters << filter2;

    exportFileDialog->setNameFilters(filters);

    connect(exportFileDialog, &QFileDialog::finished, exportFileDialog, &QFileDialog::deleteLater);

    connect(exportFileDialog, &QFileDialog::fileSelected, this, [&](const QString & file) {
        auto showExportConfigurationError = [this]() {
            showInlineMessage(i18n("Full configuration export <b>failed</b>..."), KMessageWidget::Error);
        };

        if (m_corona->layoutsManager()->importer()->exportFullConfiguration(file)) {
            m_openUrlAction->setData(file);
            m_ui->messageWidget->addAction(m_openUrlAction);
            showInlineMessage(i18n("Full configuration export succeeded..."),
                              KMessageWidget::Information,
                              SettingsDialog::INFORMATIONWITHACTIONINTERVAL);
        } else {
            showExportConfigurationError();
        }
    });

    exportFileDialog->open();

    QDate currentDate = QDate::currentDate();
    QString proposedName = QStringLiteral("Latte Dock (") + currentDate.toString("yyyy-MM-dd")+")";

    exportFileDialog->selectFile(proposedName);
}

void SettingsDialog::requestImagesDialog(int row)
{
    QStringList mimeTypeFilters;
    mimeTypeFilters << "image/jpeg" // will show "JPEG image (*.jpeg *.jpg)
                    << "image/png";  // will show "PNG image (*.png)"

    QFileDialog dialog(this);
    dialog.setMimeTypeFilters(mimeTypeFilters);

    QString background = "";// m_model->data(m_model->index(row, COLORCOLUMN), Qt::BackgroundRole).toString();

    if (background.startsWith("/") && QFileInfo(background).exists()) {
        dialog.setDirectory(QFileInfo(background).absolutePath());
        dialog.selectFile(background);
    }

    if (dialog.exec()) {
        QStringList files = dialog.selectedFiles();

        if (files.count() > 0) {
            // m_model->setData(m_model->index(row, COLORCOLUMN), files[0], Qt::BackgroundRole);
        }
    }
}

void SettingsDialog::requestColorsDialog(int row)
{
    /*QColorDialog dialog(this);
    QString textColor = m_model->data(m_model->index(row, Settings::Model::Layouts::BACKGROUNDCOLUMN), Qt::UserRole).toString();
    dialog.setCurrentColor(QColor(textColor));

    if (dialog.exec()) {
        qDebug() << dialog.selectedColor().name();
        m_model->setData(m_model->index(row, COLORCOLUMN), dialog.selectedColor().name(), Qt::UserRole);
    }*/
}

void SettingsDialog::accept()
{
    //! disable accept totally in order to avoid closing with ENTER key with no real reason
    qDebug() << Q_FUNC_INFO;
}

void SettingsDialog::apply()
{
    qDebug() << Q_FUNC_INFO;

    if (!m_ui->buttonBox->button(QDialogButtonBox::Apply)->isEnabled()) {
        return;
    }

    saveAllChanges();

    updateApplyButtonsState();
    updatePerLayoutButtonsState();
}

void SettingsDialog::reset()
{
    qDebug() << Q_FUNC_INFO;

    if (!m_ui->buttonBox->button(QDialogButtonBox::Reset)->isEnabled()) {
        return;
    }

    if (m_ui->tabWidget->currentIndex() == Latte::Types::LayoutPage) {
        m_layoutsController->reset();
    } else if (m_ui->tabWidget->currentIndex() == Latte::Types::PreferencesPage) {
        m_preferencesHandler->reset();
    }
}

void SettingsDialog::restoreDefaults()
{
    qDebug() << Q_FUNC_INFO;

    if (m_ui->tabWidget->currentIndex() == Latte::Types::LayoutPage) {
        //! do nothing, should be disabled
    } else if (m_ui->tabWidget->currentIndex() == Latte::Types::PreferencesPage) {
        m_preferencesHandler->resetDefaults();
    }
}

void SettingsDialog::loadSettings()
{
    bool inMultiple{m_corona->layoutsManager()->memoryUsage() == Types::MultipleLayouts};

    if (inMultiple) {
        m_ui->multipleToolBtn->setChecked(true);
    } else {
        m_ui->singleToolBtn->setChecked(true);
    }

    updatePerLayoutButtonsState();
    updateApplyButtonsState();
}

QList<int> SettingsDialog::currentSettings()
{
    QList<int> settings;
    settings << (int)m_ui->autostartChkBox->isChecked();
    settings << (int)m_ui->badges3DStyleChkBox->isChecked();
    settings << (int)m_ui->infoWindowChkBox->isChecked();
    settings << (int)m_ui->metaPressChkBox->isChecked();
    settings << (int)m_ui->metaPressHoldChkBox->isChecked();
    settings << (int)m_ui->noBordersForMaximizedChkBox->isChecked();
    settings << m_mouseSensitivityButtons->checkedId();
    settings << m_ui->screenTrackerSpinBox->value();
    settings << m_ui->outlineSpinBox->value();

    return settings;
}

void SettingsDialog::on_switch_layout()
{
    if (!m_layoutMenu->isEnabled() || !m_switchLayoutAction->isEnabled() || currentPage() != Types::LayoutPage) {
        return;
    }

    Settings::Data::Layout selectedLayoutCurrent = m_layoutsController->selectedLayoutCurrentData();
    Settings::Data::Layout selectedLayoutOriginal = m_layoutsController->selectedLayoutOriginalData();
    selectedLayoutOriginal = selectedLayoutOriginal.isEmpty() ? selectedLayoutCurrent : selectedLayoutOriginal;

    if (m_layoutsController->dataAreChanged()) {
        showInlineMessage(i18nc("settings:not permitted switching layout","You need to <b>apply</b> your changes first to switch layout..."),
                          KMessageWidget::Warning,
                          WARNINGINTERVAL);
        return;
    }

    if (!m_layoutsController->selectedLayoutIsCurrentActive()) {
        bool appliedShared = m_layoutsController->inMultipleMode() && selectedLayoutCurrent.isShared();
        bool freeActivitiesLayoutUpdated{false};

        if (!appliedShared && selectedLayoutCurrent.activities.isEmpty()) {
            m_layoutsController->setOriginalLayoutForFreeActivities(selectedLayoutOriginal.id);
            freeActivitiesLayoutUpdated = true;
        }

        if (m_layoutsController->inMultipleMode()) {
            m_corona->layoutsManager()->switchToLayout(selectedLayoutOriginal.name);
        } else {
            if (freeActivitiesLayoutUpdated) {
                m_corona->layoutsManager()->switchToLayout(selectedLayoutOriginal.name);
            } else {
                CentralLayout singleLayout(this, selectedLayoutCurrent.id);

                QString switchToActivity = selectedLayoutCurrent.isForFreeActivities() ? singleLayout.lastUsedActivity() : selectedLayoutCurrent.activities[0];

                if (!m_corona->activitiesConsumer()->runningActivities().contains(switchToActivity)) {
                    m_corona->layoutsManager()->synchronizer()->activitiesController()->startActivity(switchToActivity);
                }

                m_corona->layoutsManager()->synchronizer()->activitiesController()->setCurrentActivity(switchToActivity);
            }
        }
    }

    updatePerLayoutButtonsState();
}

void SettingsDialog::on_pause_layout()
{
    qDebug() << Q_FUNC_INFO;

    if (!m_layoutMenu->isEnabled() || !m_pauseLayoutAction->isEnabled() || currentPage() != Types::LayoutPage) {
        return;
    }

    setTwinProperty(m_pauseLayoutAction, TWINENABLED, false);

    Settings::Data::Layout selectedLayoutCurrent = m_layoutsController->selectedLayoutCurrentData();
    Settings::Data::Layout selectedLayoutOriginal = m_layoutsController->selectedLayoutOriginalData();
    selectedLayoutOriginal = selectedLayoutOriginal.isEmpty() ? selectedLayoutCurrent : selectedLayoutOriginal;

    m_corona->layoutsManager()->synchronizer()->pauseLayout(selectedLayoutOriginal.name);
}

void SettingsDialog::updateApplyButtonsState()
{
    bool changed{false};

    //! Ok, Apply Buttons

    if ((currentPage() == Latte::Types::LayoutPage && m_layoutsController->dataAreChanged())
            ||(currentPage() == Latte::Types::PreferencesPage && m_preferencesHandler->dataAreChanged())) {
        changed = true;
    }

    if (changed) {
        m_ui->buttonBox->button(QDialogButtonBox::Apply)->setEnabled(true);
        m_ui->buttonBox->button(QDialogButtonBox::Reset)->setEnabled(true);
    } else {
        m_ui->buttonBox->button(QDialogButtonBox::Apply)->setEnabled(false);
        m_ui->buttonBox->button(QDialogButtonBox::Reset)->setEnabled(false);
    }

    //! RestoreDefaults Button
    if (m_ui->tabWidget->currentIndex() == Latte::Types::LayoutPage) {
        m_ui->buttonBox->button(QDialogButtonBox::RestoreDefaults)->setVisible(false);
    } else if (m_ui->tabWidget->currentIndex() == Latte::Types::PreferencesPage) {
        m_ui->buttonBox->button(QDialogButtonBox::RestoreDefaults)->setVisible(true);

        //! Defaults for general Latte settings
        if (m_preferencesHandler->inDefaultValues() ) {
            m_ui->buttonBox->button(QDialogButtonBox::RestoreDefaults)->setEnabled(false);
        } else {
            m_ui->buttonBox->button(QDialogButtonBox::RestoreDefaults)->setEnabled(true);
        }
    }
}

void SettingsDialog::updatePerLayoutButtonsState()
{
    if (!m_layoutsController->hasSelectedLayout()) {
        return;
    }

    Settings::Data::Layout selectedLayout = m_layoutsController->selectedLayoutCurrentData();

    //! Switch Button
    if ((m_layoutsController->inMultipleMode() && selectedLayout.isShared())
            || m_layoutsController->selectedLayoutIsCurrentActive()) {
        setTwinProperty(m_switchLayoutAction, TWINENABLED, false);
    } else {
        setTwinProperty(m_switchLayoutAction, TWINENABLED, true);
    }

    //! Pause Button
    if (!m_layoutsController->inMultipleMode()) {
        //! Single Layout mode
        setTwinProperty(m_pauseLayoutAction, TWINVISIBLE, false);
    } else {
        setTwinProperty(m_pauseLayoutAction, TWINVISIBLE, true);

        if (selectedLayout.isActive
                && !selectedLayout.isForFreeActivities()
                && !selectedLayout.isShared()) {
            setTwinProperty(m_pauseLayoutAction, TWINENABLED, true);
        } else {
            setTwinProperty(m_pauseLayoutAction, TWINENABLED, false);
        }
    }

    //! Remove Layout Button
    /* if (selectedLayout.isActive || selectedLayout.isLocked) {
        m_ui->removeButton->setEnabled(false);
    } else {
        m_ui->removeButton->setEnabled(true);
    }*/

    //! Layout Locked Button
    if (selectedLayout.isLocked) {
        setTwinProperty(m_lockedLayoutAction, TWINCHECKED, true);
    } else {
        setTwinProperty(m_lockedLayoutAction, TWINCHECKED, false);
    }

    //! UI Elements that need to be enabled/disabled
    if (m_layoutsController->inMultipleMode()) {
        setTwinProperty(m_sharedLayoutAction, TWINVISIBLE, true);
    } else {
        setTwinProperty(m_sharedLayoutAction, TWINVISIBLE, false);
    }

    //! Layout Shared Button
    if (selectedLayout.isShared()) {
        setTwinProperty(m_sharedLayoutAction, TWINCHECKED, true);
    } else {
        setTwinProperty(m_sharedLayoutAction, TWINCHECKED, false);
    }
}

void SettingsDialog::showLayoutInformation()
{
    /*  int currentRow = m_ui->layoutsView->currentIndex().row();

    QString id = m_model->data(m_model->index(currentRow, IDCOLUMN), Qt::DisplayRole).toString();
    QString name = m_model->data(m_model->index(currentRow, NAMECOLUMN), Qt::DisplayRole).toString();

    Layout::GenericLayout *genericActive= m_corona->layoutsManager()->synchronizer()->layout(o_layoutsOriginalData[id].originalName());
    Layout::GenericLayout *generic = genericActive ? genericActive : m_layouts[id];

    auto msg = new QMessageBox(this);
    msg->setWindowTitle(name);
    msg->setText(generic->reportHtml(m_corona->screenPool()));

    msg->open();*/
}

void SettingsDialog::showScreensInformation()
{
    /*  QList<int> assignedScreens;

    for (int i = 0; i < m_model->rowCount(); ++i) {
        QString id = m_model->data(m_model->index(i, IDCOLUMN), Qt::DisplayRole).toString();
        QString name = m_model->data(m_model->index(i, NAMECOLUMN), Qt::DisplayRole).toString();

        Layout::GenericLayout *genericActive= m_corona->layoutsManager()->synchronizer()->layout(o_layoutsOriginalData[id].originalName());
        Layout::GenericLayout *generic = genericActive ? genericActive : m_layouts[id];

        QList<int> vScreens = generic->viewsScreens();

        for (const int scrId : vScreens) {
            if (!assignedScreens.contains(scrId)) {
                assignedScreens << scrId;
            }
        }
    }

    auto msg = new QMessageBox(this);
    msg->setWindowTitle(i18n("Screens Information"));
    msg->setText(m_corona->screenPool()->reportHtml(assignedScreens));

    msg->open();*/
}

void SettingsDialog::dragEnterEvent(QDragEnterEvent *event)
{
    event->acceptProposedAction();
}

void SettingsDialog::dropEvent(QDropEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        QList<QUrl> urlList = event->mimeData()->urls();

        QStringList layoutNames;

        for (int i = 0; i < qMin(urlList.size(), 20); ++i) {
            QString layoutPath = urlList[i].path();

            if (layoutPath.endsWith(".layout.latte")) {
                Settings::Data::Layout importedlayout = m_layoutsController->addLayoutForFile(layoutPath);
                layoutNames << importedlayout.name;
            }
        }

        if (layoutNames.count() == 1) {
            showInlineMessage(i18nc("settings:layout imported successfully","Layout <b>%0</b> imported successfully...").arg(layoutNames[0]),
                    KMessageWidget::Information,
                    SettingsDialog::INFORMATIONINTERVAL);
        } else if (layoutNames.count() > 1) {
            showInlineMessage(i18nc("settings:layouts imported successfully","Layouts <b>%0</b> imported successfully...").arg(layoutNames.join(", )")),
                              KMessageWidget::Information,
                              SettingsDialog::INFORMATIONINTERVAL);
        }
    }
}

void SettingsDialog::keyPressEvent(QKeyEvent *event)
{
    if (event && event->key() == Qt::Key_Escape) {
        if (m_ui->messageWidget->isVisible()) {
            m_hideInlineMessageTimer.stop();
            m_ui->messageWidget->animatedHide();
            m_ui->messageWidget->removeAction(m_openUrlAction);
            return;
        }
    }

    QDialog::keyPressEvent(event);
}

void SettingsDialog::keyReleaseEvent(QKeyEvent *event)
{
    if (event && event->key() == Qt::Key_Delete && currentPage() == Types::LayoutPage){
        on_remove_layout();
    }

    QDialog::keyReleaseEvent(event);
}

void SettingsDialog::updateWindowActivities()
{
    if (KWindowSystem::isPlatformX11()) {
        KWindowSystem::setOnActivities(winId(), QStringList());
    }
}

void SettingsDialog::saveAllChanges()
{
    if (currentPage() == Latte::Types::LayoutPage) {
        m_layoutsController->save();
    } else if (currentPage() == Latte::Types::PreferencesPage) {
        m_preferencesHandler->save();
    }
}

void SettingsDialog::showInlineMessage(const QString &msg, const KMessageWidget::MessageType &type, const int &hideInterval)
{
    if (msg.isEmpty()) {
        return;
    }

    m_hideInlineMessageTimer.stop();

    if (m_ui->messageWidget->isVisible()) {
        m_ui->messageWidget->animatedHide();
    }

    m_ui->messageWidget->setText(msg);

    // TODO: wrap at arbitrary character positions once QLabel can do this
    // https://bugreports.qt.io/browse/QTBUG-1276
    m_ui->messageWidget->setWordWrap(true);
    m_ui->messageWidget->setMessageType(type);
    m_ui->messageWidget->setWordWrap(false);

    const int unwrappedWidth = m_ui->messageWidget->sizeHint().width();
    m_ui->messageWidget->setWordWrap(unwrappedWidth > size().width());

    m_ui->messageWidget->animatedShow();

    if (hideInterval > 0) {
        m_hideInlineMessageTimer.setInterval(hideInterval);
        m_hideInlineMessageTimer.start();
    }
}

}//end of namespace
