#include "contentstore/ContentStore.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>

#include "FileSystemPrimitives.h"
#include "contentstore/ObjectFiles.h"
#include "contentstore/StoreFiles.h"
#include "contentstore/WriterGuard.h"

namespace {
using StoreFiles::discardFile;
using StoreFiles::inspect;

// Whether path holds what the placement created
bool holdsNewFile(const Transaction& transaction, const QString& path)
{
    const auto state = inspect(path);
    if (transaction.preparedKind == PlacementKind::Symbolic) {
        return state.isSymbolicLink && ObjectFiles::samePath(state.target, transaction.expected);
    }
    const auto id = state.isSymbolicLink ? Result<FS::FileId>(std::unexpected(QString())) : FS::fileId(path);
    return id && fileIdString(*id) == transaction.expected;
}

bool isFile(const QString& path, const StoredIdentity& identity)
{
    const auto id = FS::fileId(path);
    return id && identity.sameFile(*id);
}

// Whether the backup is still exactly the old file: the same file, with the same size and time, and the expected
// contents. Its change time and link count are left out, as making the backup and the swap change them.
bool backupIsIntact(const Transaction& transaction, const WriterGuard* guard = nullptr)
{
    const auto& old = *transaction.oldIdentity;
    const auto identity = FS::identity(transaction.backupPath);
    if (!identity || !old.sameFile(identity->fileId) || identity->size != old.size || identity->modifiedTime != old.modifiedTime) {
        return false;
    }
    // through the guard where there is one, as opening the file again would disturb a Linux lease
    const auto digest = guard ? guard->sha256(transaction.backupPath) : ObjectFiles::sha256(transaction.backupPath);
    return digest && *digest == transaction.expectedDigest;
}

QString interruptedError()
{
    return QString("Interrupted for testing");
}
}  // namespace

namespace {
// Puts the old file back at path by looking at what is there, never by what the record says was done. Returns whether
// path is the old file again.
bool restore(const Transaction& transaction, const QString& path)
{
    const auto& old = *transaction.oldIdentity;
    const bool backupIsOld = isFile(transaction.backupPath, old);
    if (holdsNewFile(transaction, path) && backupIsOld) {
        // the swap happened: the old file goes back over the new one
        if (auto restored = FS::replaceFile(transaction.backupPath, path); !restored) {
            qWarning() << "Shared store:" << restored.error();
        }
    } else if (!inspect(path).exists && backupIsOld) {
        // renamed aside, not swapped yet
        if (auto restored = FS::replaceFile(transaction.backupPath, path); !restored) {
            qWarning() << "Shared store:" << restored.error();
        }
    } else if (isFile(path, old) && backupIsOld) {
        // still in place, and the backup is another link to it
        discardFile(transaction.backupPath);
    }
    discardFile(transaction.temporaryPath);
    // only a durable restore counts: until then a power loss could undo it, so the transaction must stay open
    if (auto flushed = FS::flushDir(QFileInfo(path).absolutePath()); !flushed) {
        qWarning() << "Shared store:" << flushed.error();
        return false;
    }
    return isFile(path, old) && !isFile(transaction.backupPath, old);
}

// Finishes a replacement with a backup that a crash interrupted, and returns the records to journal
QList<QJsonObject> finish(const Transaction& transaction, const QString& path)
{
    const auto& old = *transaction.oldIdentity;
    const bool backupExists = QFileInfo::exists(transaction.backupPath) || QFileInfo(transaction.backupPath).isSymbolicLink();
    if (!transaction.aborting && transaction.preparedKind && holdsNewFile(transaction, path)) {
        if (!backupExists) {
            // it was made before the swap, so something else removed it; the swap is what happened
            qWarning() << "Shared store: the backup" << transaction.backupPath << "of a replacement is gone";
        }
        if (!backupExists || backupIsIntact(transaction)) {
            // committed first; the backup is then removed like that of any committed replacement, and kept pending if
            // that fails
            discardFile(transaction.temporaryPath);
            return { RefRecord::commit(transaction.id) };
        }
    }
    if (holdsNewFile(transaction, path) || isFile(path, old) || (!inspect(path).exists && isFile(transaction.backupPath, old))) {
        if (restore(transaction, path)) {
            return { RefRecord::abort(transaction.id) };
        }
        // the user's file isn't back yet, so the transaction stays open and the next start tries again
        qWarning() << "Shared store: couldn't put" << transaction.backupPath << "back at" << path << "yet";
        return {};
    }
    // neither file is where it is expected: both are left as they are for a person to look at
    qWarning() << "Shared store: couldn't tell how a replacement of" << path << "ended; its backup is" << transaction.backupPath;
    return { RefRecord::abort(transaction.id) };
}
}  // namespace

ContentStore::BackupSwap ContentStore::swapWithBackupLocked(Transaction& transaction,
                                                            const QString& path,
                                                            const QString& expectedDigest,
                                                            std::optional<WriterGuard>& guard,
                                                            QString& error)
{
    const auto dir = QFileInfo(path).absolutePath();
    const auto& old = *transaction.oldIdentity;
    const auto fail = [&error](const QString& message, BackupSwap result) {
        error = message;
        return result;
    };

    // no other program can start writing to the file until it is swapped (only enforced on Windows)
    // other programs are kept from writing to the file, or noticed when they open it, until it was replaced and its
    // backup released
    {
        auto acquired = WriterGuard::acquire(path);
        if (!acquired) {
            return fail(acquired.error(), BackupSwap::RolledBack);
        }
        guard = std::move(*acquired);
    }
    if (const auto now = FS::identity(path); !now || StoredIdentity::from(*now) != old) {
        return fail(QString("%1 changed while it was being replaced").arg(path), BackupSwap::RolledBack);
    }

    // 1. the intent, durable before anything on disk changes
    transaction.backupMethod = "link";
    transaction.backupPath = QDir(dir).filePath(QString(".prism-bak-%1").arg(transaction.id));
    transaction.expectedDigest = expectedDigest;
    if (auto journaled = commitLocked({ RefRecord::backup(transaction.id, "link", transaction.backupPath, expectedDigest) }); !journaled) {
        return fail(journaled.error(), BackupSwap::Unresolved);
    }
    if (interrupted(PlacementStep::BackupIntent)) {
        return fail(interruptedError(), BackupSwap::Interrupted);
    }

    const auto rollBack = [&](const QString& message) {
        return fail(message, restore(transaction, path) ? BackupSwap::RolledBack : BackupSwap::Unresolved);
    };

    // 2. the backup: another link to the old file, or the old file itself renamed aside where hard links don't work
    if (auto linked = FS::createHardLink(path, transaction.backupPath); !linked) {
        transaction.backupMethod = "rename";
        if (auto journaled = commitLocked({ RefRecord::backup(transaction.id, "rename", transaction.backupPath, expectedDigest) });
            !journaled) {
            return fail(journaled.error(), BackupSwap::Unresolved);
        }
        if (auto renamed = FS::replaceFile(path, transaction.backupPath); !renamed) {
            return rollBack(renamed.error());
        }
    }
    if (auto flushed = FS::flushDir(dir); !flushed) {
        return rollBack(flushed.error());
    }
    if (interrupted(PlacementStep::BackedUp)) {
        return fail(interruptedError(), BackupSwap::Interrupted);
    }

    // 3. the swap
    if (auto swapped = FS::replaceFile(transaction.temporaryPath, path).and_then([&] { return FS::flushDir(dir); }); !swapped) {
        return rollBack(swapped.error());
    }
    if (interrupted(PlacementStep::Swapped)) {
        return fail(interruptedError(), BackupSwap::Interrupted);
    }

    // 4. the backup must still be exactly the old file, and no other program may have opened it meanwhile; otherwise
    // it goes back. A program waiting on a Linux lease gets the file only after it is back.
    if (!backupIsIntact(transaction, &*guard) || guard->disturbed(transaction.backupPath)) {
        transaction.aborting = true;
        if (auto journaled = commitLocked({ RefRecord::aborting(transaction.id) }); !journaled) {
            return fail(journaled.error(), BackupSwap::Unresolved);
        }
        if (interrupted(PlacementStep::Aborting)) {
            return fail(interruptedError(), BackupSwap::Interrupted);
        }
        return rollBack(QString("%1 was opened by another program while it was being replaced; it was put back").arg(path));
    }
    return BackupSwap::Swapped;
}

Result<> ContentStore::releaseBackupsLocked(const QSet<qint64>& kept)
{
    QList<QJsonObject> records;
    for (auto it = m_table.pendingBackups().begin(); it != m_table.pendingBackups().end(); ++it) {
        if (kept.contains(it.key())) {
            continue;
        }
        discardFile(it->backupPath);
        if (QFileInfo::exists(it->backupPath)) {
            continue;
        }
        // released only once the removal is durable; until then it stays pending and is removed again later
        if (auto flushed = FS::flushDir(QFileInfo(it->backupPath).absolutePath()); !flushed) {
            qWarning() << "Shared store:" << flushed.error();
            continue;
        }
        records.append(RefRecord::backupReleased(it.key()));
    }
    return commitLocked(records);
}

Result<> ContentStore::finishBackupsLocked()
{
    QList<QJsonObject> records;
    for (const auto& transaction : m_table.transactions()) {
        if (transaction.backupPath.isEmpty() || !transaction.oldIdentity || !m_table.owners().contains(transaction.key.owner)) {
            continue;
        }
        records.append(finish(transaction, ownerPath(transaction.key)));
    }
    TRY(commitLocked(records))
    // backups of replacements that committed, checked before they did
    return releaseBackupsLocked();
}
