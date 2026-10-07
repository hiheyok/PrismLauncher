#include "contentstore/ContentStore.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QThread>

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
    transaction.awaitValidation = WriterGuard::backupsNeedValidation();
    if (auto journaled = commitLocked(
            { RefRecord::backup(transaction.id, "link", transaction.backupPath, expectedDigest, transaction.awaitValidation) });
        !journaled) {
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
        if (auto journaled = commitLocked(
                { RefRecord::backup(transaction.id, "rename", transaction.backupPath, expectedDigest, transaction.awaitValidation) });
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

Result<> ContentStore::releaseBackupsLocked()
{
    QList<QJsonObject> records;
    for (auto it = m_table.pendingBackups().begin(); it != m_table.pendingBackups().end(); ++it) {
        // a program may still write to it; only a validation may remove it
        if (it->awaitValidation) {
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

namespace {
// Whether the ref at the path is still the one the commit made, apart from what reconciliations noticed about it
bool sameRef(const std::optional<Ref>& first, const std::optional<Ref>& second)
{
    if (!first || !second) {
        return !first && !second;
    }
    return first->hash == second->hash && first->kind == second->kind && first->generation == second->generation;
}

// Inside the owner's folder, so on the same volume as the path, and outside every content folder, so mod loaders don't
// see it. The transaction id keeps two of the same file apart.
QString recoveredPathFor(const QString& root, const QString& relativePath, qint64 id)
{
    return QDir(root).filePath(QString(".prism-recovered/%1.%2").arg(relativePath).arg(id));
}
}  // namespace

Result<int> ContentStore::validatePendingBackups()
{
    QMutexLocker locker(&m_mutex);
    if (m_state != State::Writable) {
        return std::unexpected(QString("The shared store can't be changed"));
    }
    return validateBackupsLocked();
}

QList<ContentStore::RestoredFile> ContentStore::takeRestoredFiles()
{
    QMutexLocker locker(&m_mutex);
    return std::exchange(m_restoredFiles, {});
}

int ContentStore::validateBackupsLocked()
{
    int left = 0;
    QSet<QString> unused;
    for (const auto id : m_table.pendingBackups().keys()) {
        const auto& pending = m_table.pendingBackups()[id];
        if (!pending.awaitValidation && !pending.restoring) {
            // removed by the ordinary release
            continue;
        }
        if (pending.transaction.id == 0 || !pending.transaction.oldIdentity || !m_table.owners().contains(pending.key.owner)) {
            // nothing to validate it against, or its instance isn't known anymore: kept for a person to look at
            left++;
            continue;
        }
        auto validated = validateBackupLocked(id, unused);
        if (!validated) {
            qWarning() << "Shared store:" << validated.error();
        }
        if (!validated || !*validated) {
            left++;
        }
    }
    if (auto released = releaseLocked(unused); !released) {
        qWarning() << "Shared store:" << released.error();
    }
    return left;
}

Result<bool> ContentStore::validateBackupLocked(qint64 id, QSet<QString>& unused)
{
    const auto pending = m_table.pendingBackups()[id];
    if (pending.restoring) {
        return restoreBackupLocked(id, unused);
    }
    if (const auto salvage = m_salvages.find(id); salvage != m_salvages.end()) {
        // removed, but a program that opened it as it was removed still has it open
        return finishSalvageLocked(id, salvage->second);
    }
    if (!pending.trashedFrom.isEmpty() && !inspect(pending.backupPath).exists && inspect(pending.trashedFrom).exists) {
        // the rename aside was recorded, but a crash came first
        TRY(FS::replaceFile(pending.trashedFrom, pending.backupPath))
        TRY(FS::flushDir(QFileInfo(pending.backupPath).absolutePath()))
        return false;
    }
    if (!inspect(pending.backupPath).exists) {
        // removed after the last check, before that was recorded
        if (auto flushed = FS::flushDir(QFileInfo(pending.backupPath).absolutePath()); !flushed) {
            return std::unexpected(flushed.error());
        }
        TRY(commitLocked({ RefRecord::backupReleased(id) }))
        return true;
    }
    // only while no other program has it open; until then it stays pending
    auto guard = WriterGuard::acquire(pending.backupPath);
    if (!guard) {
        return false;
    }
    auto transaction = pending.transaction;
    transaction.backupPath = pending.backupPath;
    if (!backupIsIntact(transaction, &*guard) || guard->disturbed(pending.backupPath)) {
        // something wrote to it after the replacement: the user's file goes back
        TRY(commitLocked({ RefRecord::restoreBegin(id) }))
        if (interrupted(PlacementStep::RestoreBegun)) {
            return std::unexpected(interruptedError());
        }
        guard->release();
        return restoreBackupLocked(id, unused);
    }

    if (!guard->keepsEveryWriterOut() && pending.trashedFrom.isEmpty()) {
        // A program that looked the backup up before it was guarded can still open it, and would write to a file with no
        // name if it were removed now. So it is first renamed to a name no program knows, and released while it still
        // has a name; a later validation removes it, once it is unchanged and quiet again.
        const auto trash = QDir(QFileInfo(pending.backupPath).absolutePath()).filePath(QString(".prism-del-%1").arg(id));
        TRY(commitLocked({ RefRecord::backupTrashed(id, trash) }))
        TRY(FS::replaceFile(pending.backupPath, trash))
        TRY(FS::flushDir(QFileInfo(trash).absolutePath()))
        return false;
    }

    if (interrupted(PlacementStep::Removing)) {
        return std::unexpected(interruptedError());
    }
    discardFile(pending.backupPath);
    if (inspect(pending.backupPath).exists) {
        return false;
    }
    if (guard->disturbed(pending.backupPath)) {
        // A program opened it as it was removed, and waits on the lease with a file that has no name. The guard keeps
        // that file, and what the program writes is saved once it closes it; until then the backup stays pending.
        guard->beginSalvage(recoveredPathFor(m_table.owners().value(pending.key.owner), pending.key.relativePath, id),
                            pending.transaction.expectedDigest);
        const auto salvage = m_salvages.emplace(id, std::move(*guard)).first;
        // a moment for one that writes and closes right away
        return finishSalvageLocked(id, salvage->second, 10);
    }
    if (auto flushed = FS::flushDir(QFileInfo(pending.backupPath).absolutePath()); !flushed) {
        return std::unexpected(flushed.error());
    }
    TRY(commitLocked({ RefRecord::backupReleased(id) }))
    return true;
}

Result<bool> ContentStore::finishSalvageLocked(qint64 id, WriterGuard& guard, int attempts)
{
    const auto pending = m_table.pendingBackups()[id];
    auto state = guard.trySalvage();
    for (int attempt = 1; attempt < attempts && state && *state == WriterGuard::Salvage::Waiting; attempt++) {
        QThread::msleep(20);
        state = guard.trySalvage();
    }
    if (!state) {
        return std::unexpected(state.error());
    }
    if (*state == WriterGuard::Salvage::Waiting) {
        return false;
    }
    if (*state == WriterGuard::Salvage::Saved) {
        m_restoredFiles.append(
            { ownerPath(pending.key), recoveredPathFor(m_table.owners().value(pending.key.owner), pending.key.relativePath, id) });
    }
    m_salvages.erase(id);
    TRY(FS::flushDir(QFileInfo(pending.backupPath).absolutePath()))
    TRY(commitLocked({ RefRecord::backupReleased(id) }))
    return true;
}

Result<bool> ContentStore::restoreBackupLocked(qint64 id, QSet<QString>& unused)
{
    const auto pending = m_table.pendingBackups()[id];
    const auto path = ownerPath(pending.key);
    const auto& old = *pending.transaction.oldIdentity;
    const bool backupExists = inspect(pending.backupPath).exists;
    const auto finished = [&](const QJsonObject& record, const QString& recoveredPath) -> Result<bool> {
        TRY(commitLocked({ record }))
        if (record["type"].toString() == "restoreCommit" && pending.newRef) {
            unused.insert(pending.newRef->hash);
        }
        m_restoredFiles.append({ path, recoveredPath });
        return true;
    };

    if (!pending.recoveredPath.isEmpty()) {
        // moving it aside was recorded: finish that
        if (!backupExists && isFile(pending.recoveredPath, old)) {
            // moved before; only done once both folders are durable, which a failed flush left undone
            TRY(FS::flushDir(QFileInfo(pending.recoveredPath).absolutePath()))
            TRY(FS::flushDir(QFileInfo(pending.backupPath).absolutePath()))
            return finished(RefRecord::restoreConflict(id), pending.recoveredPath);
        }
        if (!backupExists) {
            return std::unexpected(
                QString("The changed backup of %1 is neither at %2 nor at %3").arg(path, pending.backupPath, pending.recoveredPath));
        }
        QDir().mkpath(QFileInfo(pending.recoveredPath).absolutePath());
        TRY(FS::replaceFile(pending.backupPath, pending.recoveredPath))
        if (interrupted(PlacementStep::ConflictMoved)) {
            return std::unexpected(interruptedError());
        }
        TRY(FS::flushDir(QFileInfo(pending.recoveredPath).absolutePath()))
        TRY(FS::flushDir(QFileInfo(pending.backupPath).absolutePath()))
        return finished(RefRecord::restoreConflict(id), pending.recoveredPath);
    }

    if (!backupExists && isFile(path, old)) {
        // put back before a crash, which came before it was recorded
        TRY(FS::flushDir(QFileInfo(path).absolutePath()))
        return finished(RefRecord::restoreCommit(id), {});
    }
    if (!backupExists) {
        return std::unexpected(QString("The changed backup %1 of %2 is gone").arg(pending.backupPath, path));
    }

    // The path must still hold what the replacement put there, with the ref it made; anything else is newer and is
    // never overwritten. The path is held still while it is checked and replaced.
    std::optional<WriterGuard> pathGuard;
    if (!inspect(path).isSymbolicLink && inspect(path).exists) {
        auto acquired = WriterGuard::acquire(path);
        if (!acquired) {
            // in use, such as by a running game; tried again later
            return false;
        }
        pathGuard = std::move(*acquired);
    }
    auto transaction = pending.transaction;
    if (holdsNewFile(transaction, path) && sameRef(m_table.ref(pending.key), pending.newRef)) {
        TRY(FS::replaceFile(pending.backupPath, path))
        if (interrupted(PlacementStep::Restored)) {
            return std::unexpected(interruptedError());
        }
        TRY(FS::flushDir(QFileInfo(path).absolutePath()))
        return finished(RefRecord::restoreCommit(id), {});
    }
    pathGuard.reset();

    // something newer is there: the user's changed file is saved aside instead, at a path recorded first
    const auto recovered = recoveredPathFor(m_table.owners().value(pending.key.owner), pending.key.relativePath, id);
    TRY(commitLocked({ RefRecord::restoreConflictBegin(id, recovered) }))
    if (interrupted(PlacementStep::ConflictBegun)) {
        return std::unexpected(interruptedError());
    }
    return restoreBackupLocked(id, unused);
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
