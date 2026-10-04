#include "SharedStorePage.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "Application.h"
#include "FileSystem.h"
#include "contentstore/ContentStore.h"
#include "contentstore/SharedContent.h"
#include "contentstore/StoreTasks.h"
#include "settings/SettingsObject.h"
#include "ui/dialogs/CustomMessageBox.h"
#include "ui/dialogs/ProgressDialog.h"

SharedStorePage::SharedStorePage(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("sharedStorePage"));
    auto* layout = new QVBoxLayout(this);

    auto* settingsBox = new QGroupBox(tr("Sharing"), this);
    auto* form = new QFormLayout(settingsBox);
    m_enabled = new QCheckBox(tr("Keep mods, resource packs and shaders once, and share them between instances"), settingsBox);
    form->addRow(m_enabled);
    m_linkMode = new QComboBox(settingsBox);
    m_linkMode->addItem(tr("Hard links, or symbolic links where hard links don't work"), "Auto");
    m_linkMode->addItem(tr("Hard links only"), "HardLinks");
    m_linkMode->addItem(tr("Symbolic links only"), "Symlinks");
    form->addRow(tr("Link files with:"), m_linkMode);
    auto* dirRow = new QHBoxLayout;
    m_storeDir = new QLineEdit(settingsBox);
    auto* browse = new QPushButton(tr("Browse"), settingsBox);
    dirRow->addWidget(m_storeDir);
    dirRow->addWidget(browse);
    form->addRow(tr("Folder:"), dirRow);
    m_reconcileOnStartup = new QCheckBox(tr("Look for changed links when the launcher starts"), settingsBox);
    form->addRow(m_reconcileOnStartup);
    auto* restartNote = new QLabel(tr("Turning sharing on or off and changing the folder take effect when the launcher restarts. "
                                      "Files already shared stay usable either way."),
                                   settingsBox);
    restartNote->setWordWrap(true);
    form->addRow(restartNote);
    layout->addWidget(settingsBox);

    auto* statusBox = new QGroupBox(tr("Status"), this);
    auto* statusLayout = new QVBoxLayout(statusBox);
    m_status = new QLabel(statusBox);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusLayout->addWidget(m_status);
    m_volumeWarning = new QLabel(statusBox);
    m_volumeWarning->setWordWrap(true);
    statusLayout->addWidget(m_volumeWarning);
    m_useAnyway = new QPushButton(tr("Use the store anyway…"), statusBox);
    statusLayout->addWidget(m_useAnyway, 0, Qt::AlignLeft);
    layout->addWidget(statusBox);

    auto* maintenanceBox = new QGroupBox(tr("Maintenance"), this);
    auto* buttons = new QHBoxLayout(maintenanceBox);
    m_verify = new QPushButton(tr("Verify links"), maintenanceBox);
    m_verify->setToolTip(tr("Quickly check that every shared file is still linked where it was"));
    m_reconcile = new QPushButton(tr("Look for links"), maintenanceBox);
    m_reconcile->setToolTip(tr("Scan the instances for moved and new links, and remove shared files nothing uses anymore"));
    m_deepVerify = new QPushButton(tr("Check contents"), maintenanceBox);
    m_deepVerify->setToolTip(tr("Read every shared file to find damaged ones. This can take a while."));
    m_repair = new QPushButton(tr("Repair links"), maintenanceBox);
    m_repair->setToolTip(tr("Point symbolic links whose target is gone, such as after the folder moved, back at their shared files"));
    for (auto* button : { m_verify, m_reconcile, m_deepVerify, m_repair }) {
        buttons->addWidget(button);
    }
    buttons->addStretch();
    layout->addWidget(maintenanceBox);
    layout->addStretch();

    connect(browse, &QPushButton::clicked, this, [this] {
        const auto dir = QFileDialog::getExistingDirectory(this, tr("Shared Files Folder"), m_storeDir->text());
        if (!dir.isEmpty()) {
            m_storeDir->setText(dir);
        }
    });
    connect(m_verify, &QPushButton::clicked, this, [this] {
        auto* store = SharedContent::store();
        auto* task = new VerifyStoreTask(store);
        runTask(task, [task] {
            const auto& report = *task->report();
            return tr("Checked %1 links: %2 missing, %3 replaced, %4 couldn't be checked.")
                .arg(report.checked)
                .arg(report.missing.size())
                .arg(report.replaced.size())
                .arg(report.unknown.size());
        });
    });
    connect(m_reconcile, &QPushButton::clicked, this, [this] {
        auto* store = SharedContent::store();
        auto* task = new ReconcileStoreTask(store, SharedContent::reconcileOptions(*store));
        runTask(task, [task] {
            const auto& report = *task->report();
            auto text = tr("Followed %1 moved links, found %2 new links, released %3 links and removed %4 files nothing uses.")
                            .arg(report.moved)
                            .arg(report.adopted)
                            .arg(report.released)
                            .arg(report.destroyed);
            if (!report.complete) {
                text += "\n\n" + tr("Some folders couldn't be read, so nothing was released:\n%1").arg(report.uncertain.join('\n'));
            }
            return text;
        });
    });
    connect(m_deepVerify, &QPushButton::clicked, this, [this] {
        auto* task = new DeepVerifyStoreTask(SharedContent::store());
        runTask(task, [task] {
            const auto& report = *task->report();
            auto text = tr("Checked %1 files: %2 found damaged, %3 missing.")
                            .arg(report.checked)
                            .arg(report.damaged.size())
                            .arg(report.missing.size());
            if (!report.affected.isEmpty()) {
                text += "\n\n" + tr("%1 files in instances use a damaged copy. They are marked \"Damaged copy\" in their lists, "
                                    "where you can restore the original or keep them as local copies.")
                                     .arg(report.affected.size());
            }
            return text;
        });
    });
    connect(m_repair, &QPushButton::clicked, this, [this] {
        auto* store = SharedContent::store();
        const auto report = store->repairSymbolicLinks();
        if (!report) {
            CustomMessageBox::selectable(this, tr("Repair links"), report.error(), QMessageBox::Warning)->exec();
        } else {
            CustomMessageBox::selectable(
                this, tr("Repair links"),
                tr("Repaired %1 links. %2 links point to files that are gone.").arg(report->retargeted).arg(report->lost.size()),
                QMessageBox::Information)
                ->exec();
        }
        refreshStatus();
    });
    connect(m_useAnyway, &QPushButton::clicked, this, [this] {
        auto* store = APPLICATION->contentStore();
        const auto response =
            CustomMessageBox::selectable(this, tr("Use the store anyway"),
                                         tr("A launcher on another computer is using this store. Only continue if it is no longer "
                                            "running there: two launchers changing the store at once can lose files.\n\nContinue?"),
                                         QMessageBox::Warning, QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
                ->exec();
        if (store && response == QMessageBox::Yes) {
            store->forceOpen();
        }
        refreshStatus();
    });

    loadSettings();
    refreshStatus();
}

void SharedStorePage::loadSettings()
{
    auto settings = APPLICATION->settings();
    m_enabled->setChecked(settings->get("SharedStoreEnabled").toBool());
    m_linkMode->setCurrentIndex(std::max(0, m_linkMode->findData(settings->get("SharedStoreLinkMode").toString())));
    m_storeDir->setText(settings->get("SharedStoreDir").toString());
    m_reconcileOnStartup->setChecked(settings->get("SharedStoreReconcileOnStartup").toBool());
}

bool SharedStorePage::apply()
{
    auto settings = APPLICATION->settings();
    settings->set("SharedStoreEnabled", m_enabled->isChecked());
    settings->set("SharedStoreLinkMode", m_linkMode->currentData().toString());
    settings->set("SharedStoreDir", m_storeDir->text());
    settings->set("SharedStoreReconcileOnStartup", m_reconcileOnStartup->isChecked());
    if (auto* store = APPLICATION->contentStore()) {
        // the link mode applies right away
        store->setLinkMode(ContentStore::linkModeFromSetting(m_linkMode->currentData().toString()));
    }
    return true;
}

void SharedStorePage::refreshStatus()
{
    auto* store = APPLICATION->contentStore();
    const bool writable = store && store->isWritable();
    for (auto* button : { m_verify, m_reconcile, m_deepVerify, m_repair }) {
        button->setEnabled(writable);
    }
    m_useAnyway->setVisible(store && store->state() == ContentStore::State::Busy && store->lockHolder() &&
                            !store->lockHolder()->hostname.isEmpty());

    if (!store) {
        m_status->setText(tr("Sharing is off."));
        m_volumeWarning->clear();
        return;
    }

    QString text;
    switch (store->state()) {
        case ContentStore::State::Writable: {
            const auto stats = store->stats();
            const QLocale locale;
            text = tr("%1 files are shared, taking %2. Sharing them saves %3 compared to separate copies.")
                       .arg(stats.files)
                       .arg(locale.formattedDataSize(stats.bytes), locale.formattedDataSize(stats.savedBytes));
            break;
        }
        case ContentStore::State::ReadOnly:
        case ContentStore::State::Busy:
        case ContentStore::State::Disabled:
        case ContentStore::State::Closed:
            text = tr("New files aren't shared right now: %1").arg(store->statusMessage());
            break;
    }
    // other launchers sharing this store
    const auto clients = store->clients();
    if (clients.size() > 1) {
        QStringList others;
        for (auto it = clients.begin(); it != clients.end(); ++it) {
            if (it.key() != store->clientId()) {
                others.append(it->dataDir);
            }
        }
        text += "\n\n" + tr("Also used by the launchers in:\n%1").arg(others.join('\n'));
    }
    m_status->setText(text);

    // flushes aren't reliable there, so a power loss can lose recent changes
    const auto type = FS::statFS(store->storeDir()).fsType;
    if (type == FS::FilesystemType::FAT || type == FS::FilesystemType::NFS) {
        m_volumeWarning->setText(tr("The folder is on a drive (%1) that can't guarantee changes survive a power loss. "
                                    "If the computer turns off while files are being shared, run \"Check contents\".")
                                     .arg(FS::statFS(store->storeDir()).fsTypeName));
    } else {
        m_volumeWarning->clear();
    }
}

void SharedStorePage::runTask(Task* task, const std::function<QString()>& summary)
{
    std::unique_ptr<Task> owned(task);
    ProgressDialog dialog(this);
    dialog.execWithTask(owned.get());
    if (task->wasSuccessful()) {
        CustomMessageBox::selectable(this, tr("Shared Files"), summary(), QMessageBox::Information)->exec();
    } else {
        CustomMessageBox::selectable(this, tr("Shared Files"), task->failReason(), QMessageBox::Warning)->exec();
    }
    refreshStatus();
}
