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
#include "BaseInstance.h"
#include "FileSystem.h"
#include "InstanceList.h"
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
    m_shareExisting = new QCheckBox(tr("Share the content of existing instances when the launcher starts"), settingsBox);
    m_shareExisting->setToolTip(
        tr("Runs \"Share All Content\" for every instance that isn't running. Instances on network drives "
           "are left out, as they need to be confirmed."));
    form->addRow(m_shareExisting);
    auto* restartNote = new QLabel(tr("Turning sharing on or off takes effect when the launcher restarts. Changing the folder moves "
                                      "the shared files there right away. Files already shared stay usable either way."),
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
    m_cleanUp = new QPushButton(tr("Clean up"), maintenanceBox);
    m_cleanUp->setToolTip(tr("Remove the shared files no instance uses anymore, after making sure no link to them is left"));
    for (auto* button : { m_verify, m_reconcile, m_deepVerify, m_repair, m_cleanUp }) {
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
    connect(m_cleanUp, &QPushButton::clicked, this, [this] {
        auto* store = SharedContent::store();
        auto* task = new ReconcileStoreTask(store, SharedContent::reconcileOptions(*store));
        runTask(task, [task, store] {
            const auto& report = *task->report();
            const QLocale locale;
            const auto stats = store->stats();
            QStringList text{ tr("Removed %n file(s) no instance used.", "", report.destroyed) };
            if (!report.complete) {
                text.append(tr("Some folders couldn't be read, so files that may still be linked from there were kept:\n%1")
                                .arg(report.uncertain.join('\n')));
            }
            if (stats.unusedFound > 0) {
                text.append(tr("%n file(s) found in the folder without a record are kept for %1 days after they were found, as a "
                               "launcher may still be about to link them.",
                               "", stats.unusedFound)
                                .arg(ContentStore::OrphanAgeSeconds / (24 * 60 * 60)));
            }
            if (stats.unusedFiles > 0) {
                text.append(
                    tr("%n unused file(s) are left, taking %1.", "", stats.unusedFiles).arg(locale.formattedDataSize(stats.unusedBytes)));
            }
            return text.join("\n\n");
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
    m_shareExisting->setChecked(settings->get("SharedStoreShareExisting").toBool());
}

bool SharedStorePage::apply()
{
    auto settings = APPLICATION->settings();
    // a new folder applies right away, once the shared files moved there
    if (APPLICATION->contentStoreDir(m_storeDir->text()) != APPLICATION->contentStoreDir(settings->get("SharedStoreDir").toString()) &&
        !moveStore(m_storeDir->text())) {
        return false;
    }
    settings->set("SharedStoreEnabled", m_enabled->isChecked());
    settings->set("SharedStoreLinkMode", m_linkMode->currentData().toString());
    settings->set("SharedStoreDir", m_storeDir->text());
    settings->set("SharedStoreReconcileOnStartup", m_reconcileOnStartup->isChecked());
    settings->set("SharedStoreShareExisting", m_shareExisting->isChecked());
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
    for (auto* button : { m_verify, m_reconcile, m_deepVerify, m_repair, m_cleanUp }) {
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
            if (stats.unusedFiles > 0) {
                text += "\n\n" + tr("%n file(s) no instance uses take %1. \"Clean up\" removes them.", "", stats.unusedFiles)
                                     .arg(locale.formattedDataSize(stats.unusedBytes));
            }
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

bool SharedStorePage::moveStore(const QString& setting)
{
    auto* current = APPLICATION->contentStore();
    // without a store in use, the folder is used when sharing is next turned on
    if (!current || !current->isWritable()) {
        return true;
    }
    const auto warn = [this](const QString& text) {
        CustomMessageBox::selectable(this, tr("Change the folder"), text, QMessageBox::Warning)->exec();
        return false;
    };
    if (APPLICATION->contentStoreBusy()) {
        return warn(tr("Shared files are being checked or shared in the background. Change the folder once that finished."));
    }
    if (!current->isIdle()) {
        // files whose sharing isn't finished, such as replaced files still in use, may finish now
        current->validatePendingBackups();
        if (!current->isIdle()) {
            return warn(tr("Some files are still being shared, as another program has them open. Close it, and change the folder then."));
        }
    }
    const auto oldDir = current->storeDir();
    const bool otherLaunchers = current->clients().size() > 1;
    auto next = APPLICATION->openContentStore(APPLICATION->contentStoreDir(setting));
    if (!next->isWritable()) {
        return warn(tr("The folder %1 can't be used: %2").arg(next->storeDir(), next->statusMessage()));
    }

    QStringList notes;
    const auto links = current->refsSnapshot().size();
    if (links > 0) {
        const auto answer =
            CustomMessageBox::selectable(this, tr("Change the folder"),
                                         tr("%n shared file(s) are linked from %1. They are moved to %2: each is copied there and linked "
                                            "again in its place, with a hard link where the instance is on the same drive.\n\nContinue?",
                                            "", links)
                                             .arg(oldDir, next->storeDir()),
                                         QMessageBox::Question, QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes)
                ->exec();
        if (answer != QMessageBox::Yes) {
            return false;
        }
        MoveStoreTask task(current, next.get());
        ProgressDialog dialog(this);
        dialog.execWithTask(&task);
        const auto& report = task.report();
        if (!report) {
            return warn(tr("The shared files couldn't be moved: %1").arg(task.failReason()));
        }
        notes.append(tr("%n shared file(s) were moved.", "", report->moved));
        if (!report->failed.isEmpty()) {
            notes.append(tr("These files couldn't be moved, and still link to the old folder:\n%1").arg(report->failed.join('\n')));
        }
    }
    APPLICATION->replaceContentStore(std::move(next));

    // the old folder, unless something still needs it
    QStringList gameRoots;
    for (int i = 0; i < APPLICATION->instances()->count(); i++) {
        gameRoots.append(APPLICATION->instances()->at(i)->gameRoot());
    }
    const auto stillLinked = SharedContent::linksInto(oldDir, gameRoots);
    if (otherLaunchers) {
        notes.append(tr("The old folder %1 is kept, as other launchers use it too.").arg(oldDir));
    } else if (!stillLinked.isEmpty()) {
        notes.append(tr("The old folder %1 is kept, as %n file(s) still link into it.", "", stillLinked.size()).arg(oldDir));
    } else if (QDir(oldDir).exists()) {
        const auto answer = CustomMessageBox::selectable(this, tr("Change the folder"),
                                                         (notes.isEmpty() ? QString() : notes.join("\n\n") + "\n\n") +
                                                             tr("Nothing uses the old folder %1 anymore. Delete it?").arg(oldDir),
                                                         QMessageBox::Question, QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes)
                                ->exec();
        if (answer == QMessageBox::Yes) {
            if (auto deleted = FS::deleteTree(oldDir); !deleted) {
                notes = { tr("The old folder %1 couldn't be deleted: %2").arg(oldDir, deleted.error()) };
            } else {
                notes.clear();
            }
        } else {
            notes.clear();
        }
    }
    if (!notes.isEmpty()) {
        CustomMessageBox::selectable(this, tr("Change the folder"), notes.join("\n\n"), QMessageBox::Information)->exec();
    }
    refreshStatus();
    return true;
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
