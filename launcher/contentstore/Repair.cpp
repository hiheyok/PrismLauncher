#include "contentstore/ContentStore.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>

#include "FileSystemPrimitives.h"
#include "contentstore/ObjectFiles.h"
#include "contentstore/StoreFiles.h"

namespace {
using StoreFiles::discardFile;
using StoreFiles::inspect;
using StoreFiles::temporaryName;

QString interruptedError()
{
    return QString("Interrupted for testing");
}
}  // namespace

bool ContentStore::hasPendingRetargets(const QString& hash) const
{
    const auto entry = m_table.entries().find(hash);
    if (entry == m_table.entries().end()) {
        return false;
    }
    for (auto it = m_table.refs().begin(); it != m_table.refs().end(); ++it) {
        const auto* generation = it->hash == hash && it->kind == LinkKind::Symbolic ? entry->generation(it->generation) : nullptr;
        if (!generation || generation->retiredPath.isEmpty() || !m_table.owners().contains(it.key().owner)) {
            continue;
        }
        const auto state = inspect(ownerPath(it.key()));
        if (state.isSymbolicLink && ObjectFiles::samePath(state.target, objectPath(hash))) {
            return true;
        }
    }
    return false;
}

Result<> ContentStore::retargetLocked(const RefKey& key, const Ref& ref, int generation, const QString& target)
{
    const auto path = ownerPath(key);
    const auto dir = QFileInfo(path).absolutePath();
    TRY_INTO(const auto before, FS::identity(path))
    Transaction transaction{
        m_table.nextTransactionId(), key, temporaryName(dir), ref.hash, StoredIdentity::from(before), ref.hash, std::nullopt, {}, generation
    };
    TRY(commitLocked({ RefRecord::begin(transaction) }))

    const auto rollback = [&](const QString& error) -> Result<> {
        discardFile(transaction.temporaryPath);
        if (auto aborted = commitLocked({ RefRecord::abort(transaction.id) }); !aborted) {
            return std::unexpected(QString("%1, and %2").arg(error, aborted.error()));
        }
        return std::unexpected(error);
    };

    auto linked = FS::createSymbolicLink(target, transaction.temporaryPath);
    if (!linked && linked.error().failure == FS::LinkFailure::NeedsPrivilege && m_privilegedLinker) {
        const auto elevated = m_privilegedLinker({ { target, transaction.temporaryPath } });
        if (elevated.isEmpty() || !elevated.first()) {
            return rollback(elevated.isEmpty() ? linked.error().message : elevated.first().error());
        }
    } else if (!linked) {
        return rollback(linked.error().message);
    }
    const auto created = inspect(transaction.temporaryPath);
    if (!created.isSymbolicLink || !ObjectFiles::samePath(created.target, target)) {
        return rollback(QString("The link created for %1 doesn't point to %2").arg(path, target));
    }
    if (auto flushed = FS::flushDir(dir); !flushed) {
        return rollback(flushed.error());
    }
    if (interrupted(PlacementStep::Created)) {
        return std::unexpected(interruptedError());
    }
    TRY(commitLocked({ RefRecord::prepared(transaction.id, PlacementKind::Symbolic, QFileInfo(target).absoluteFilePath()) }))
    if (interrupted(PlacementStep::Prepared)) {
        return std::unexpected(interruptedError());
    }

    auto swapped = FS::identity(path).and_then([&](const FS::FileIdentity& now) -> Result<> {
        if (now != before) {
            return std::unexpected(QString("%1 changed while its link was being replaced").arg(path));
        }
        return FS::replaceFile(transaction.temporaryPath, path);
    });
    if (!swapped) {
        return rollback(swapped.error());
    }
    if (interrupted(PlacementStep::Swapped)) {
        return std::unexpected(interruptedError());
    }
    if (auto flushed = FS::flushDir(dir); !flushed) {
        qWarning() << "Shared store:" << flushed.error();
    }
    return commitLocked({ RefRecord::commit(transaction.id) });
}

Result<> ContentStore::finishRetargetsLocked(const QString& hash)
{
    const auto entry = m_table.entries().find(hash);
    if (entry == m_table.entries().end() || entry->retired.isEmpty()) {
        return {};
    }
    QString errors;
    // a copy, as retargeting changes the refs
    const auto refs = m_table.refs();
    for (auto it = refs.begin(); it != refs.end(); ++it) {
        if (it->hash != hash || it->kind != LinkKind::Symbolic || !isOwnOwner(it.key().owner)) {
            continue;
        }
        const auto* generation = entry->generation(it->generation);
        if (!generation || generation->retiredPath.isEmpty()) {
            continue;
        }
        const auto state = inspect(ownerPath(it.key()));
        if (!state.isSymbolicLink || !ObjectFiles::samePath(state.target, objectPath(hash))) {
            continue;
        }
        if (auto retargeted = retargetLocked(it.key(), *it, generation->id, generationPath(hash, *generation)); !retargeted) {
            errors += retargeted.error() + '\n';
        }
    }
    if (!errors.isEmpty()) {
        return std::unexpected(errors.trimmed());
    }
    return {};
}

Result<> ContentStore::retireLocked(const QString& hash)
{
    const auto entry = m_table.entries().find(hash);
    if (entry == m_table.entries().end() || !entry->current) {
        return {};
    }
    const auto generation = *entry->current;
    const auto relativePath = QString("retired/%1/%2.%3").arg(hash.left(2), hash).arg(generation.id);
    const auto retiredPath = QDir(m_storeDir).absoluteFilePath(relativePath);
    const auto dir = QFileInfo(retiredPath).absolutePath();
    if (!QDir().mkpath(dir)) {
        return std::unexpected(QString("Could not create %1").arg(dir));
    }

    // 1. The damaged file gets a path of its own, so its bytes outlive the canonical path being replaced
    const auto existing = FS::fileId(retiredPath);
    if (!existing) {
        TRY(FS::createHardLink(objectPath(hash), retiredPath))
    } else if (!generation.identity.sameFile(*existing)) {
        return std::unexpected(QString("%1 holds a different file").arg(retiredPath));
    }
    TRY(FS::flushDir(dir))
    if (interrupted(PlacementStep::Begun)) {
        return std::unexpected(interruptedError());
    }

    // 2. The generation is retired, keeping the hard links that use it on it
    TRY(commitLocked({ RefRecord::retire(hash, generation.id, relativePath) }))

    // 3. Symbolic links that use it point at the kept copy. Until they all do, no intact copy can take the canonical path,
    // and the next check tries again.
    if (auto retargeted = finishRetargetsLocked(hash); !retargeted) {
        qWarning() << "Shared store:" << retargeted.error();
    }
    return {};
}

Result<ContentStore::DeepVerifyReport> ContentStore::deepVerify()
{
    struct Candidate {
        QString hash;
        int generation = 0;
    };
    QList<Candidate> candidates;
    DeepVerifyReport report;
    {
        QMutexLocker locker(&m_mutex);
        if (m_state != State::Writable) {
            return std::unexpected(QString("The shared store can't be changed"));
        }
        // retirements a crash or a missing permission left unfinished
        for (const auto& entry : m_table.entries()) {
            if (auto finished = finishRetargetsLocked(entry.hash); !finished) {
                qWarning() << "Shared store:" << finished.error();
            }
            if (entry.current) {
                candidates.append({ entry.hash, entry.current->id });
            }
        }
    }

    // hashed without the lock, as it reads every stored file
    for (const auto& [hash, generationId] : candidates) {
        const auto path = objectPath(hash);
        const auto before = FS::identity(path);
        if (!before) {
            if (!QFileInfo::exists(path)) {
                report.missing.append(hash);
            } else {
                report.unreadable.append(hash);
            }
            continue;
        }
        const auto digest = ObjectFiles::sha256(path);
        if (!digest) {
            report.unreadable.append(hash);
            continue;
        }

        QMutexLocker locker(&m_mutex);
        const auto entry = m_table.entries().find(hash);
        if (entry == m_table.entries().end() || !entry->current || entry->current->id != generationId) {
            continue;
        }
        report.checked++;
        if (*digest != hash) {
            TRY(retireLocked(hash))
            report.damaged.append(hash);
            continue;
        }
        // the contents were just proven, so this identity is trustworthy for quick checks
        const auto after = FS::identity(path);
        if (after && *after == *before && entry->current->identity.sameFile(after->fileId) &&
            StoredIdentity::from(*after) != entry->current->identity) {
            TRY(commitLocked({ RefRecord::updateIdentity(hash, generationId, StoredIdentity::from(*after)) }))
        }
    }

    QMutexLocker locker(&m_mutex);
    for (auto it = m_table.refs().begin(); it != m_table.refs().end(); ++it) {
        const auto entry = m_table.entries().find(it->hash);
        const auto* generation = entry != m_table.entries().end() ? entry->generation(it->generation) : nullptr;
        if (generation && generation->corrupt) {
            report.affected.append({ it.key(), it->hash, it->generation, it->kind });
        }
    }
    TRY(markUnusedGenerationsLocked({}))
    TRY(destroyUnusedLocked())
    return report;
}

Result<ContentStore::RepairReport> ContentStore::repairSymbolicLinks()
{
    QMutexLocker locker(&m_mutex);
    if (m_state != State::Writable) {
        return std::unexpected(QString("The shared store can't be changed"));
    }
    RepairReport report;
    // a copy, as repairing changes the refs
    const auto refs = m_table.refs();
    for (auto it = refs.begin(); it != refs.end(); ++it) {
        if (it->kind != LinkKind::Symbolic || !isOwnOwner(it.key().owner) || !m_table.owners().contains(it.key().owner)) {
            continue;
        }
        const auto entry = m_table.entries().find(it->hash);
        const auto* generation = entry != m_table.entries().end() ? entry->generation(it->generation) : nullptr;
        const auto state = inspect(ownerPath(it.key()));
        if (!generation || !state.isSymbolicLink) {
            // a missing or replaced link is for reconciliation
            continue;
        }
        const auto target = generationPath(it->hash, *generation);
        if (ObjectFiles::samePath(state.target, target)) {
            if (!QFileInfo::exists(target)) {
                report.lost.append(it.key());
            }
            continue;
        }
        // only a dangling link to a file of the same name, such as in the store's previous folder
        if (QFileInfo::exists(state.target) || QFileInfo(state.target).fileName() != QFileInfo(target).fileName()) {
            continue;
        }
        if (!QFileInfo::exists(target)) {
            report.lost.append(it.key());
            continue;
        }
        TRY(retargetLocked(it.key(), *it, generation->id, target))
        report.retargeted++;
    }
    return report;
}

Result<PlacementKind> ContentStore::restoreOriginal(const RefKey& key)
{
    Placement placement;
    PlaceOptions options;
    {
        QMutexLocker locker(&m_mutex);
        const auto ref = m_table.ref(key);
        const auto root = m_table.owners().find(key.owner);
        if (!ref || root == m_table.owners().end()) {
            return std::unexpected(QString("%1 isn't a shared file").arg(key.relativePath));
        }
        const auto entry = m_table.entries().find(ref->hash);
        if (entry == m_table.entries().end() || !entry->current) {
            return std::unexpected(QString("There is no intact copy of %1 yet").arg(key.relativePath));
        }
        if (entry->current->id == ref->generation) {
            return std::unexpected(QString("%1 already uses the intact copy").arg(key.relativePath));
        }
        placement = { { key.owner, *root, key.relativePath }, ref->hash, std::nullopt };
        if (ref->kind == LinkKind::Symbolic) {
            // the same kind of link as before
            options.mode = LinkMode::SymbolicLinks;
        }
    }
    // the link to the damaged copy is replaced, as the user chose
    return placeAt(placement, options);
}
