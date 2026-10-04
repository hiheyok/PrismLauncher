#include "contentstore/ContentStore.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QUuid>

#include <set>
#include <vector>

#include "FileSystemPrimitives.h"
#include "contentstore/ObjectFiles.h"
#include "contentstore/StoreFiles.h"
#include "contentstore/SymlinkAllowList.h"

#if defined(Q_OS_WIN)
#include <QEventLoop>

#include "FileSystem.h"
#endif

namespace {
using StoreFiles::createEmptyFile;
using StoreFiles::discardFile;
using StoreFiles::inspect;
using StoreFiles::temporaryName;

// Whether the destination still holds the file found when the placement began. Linking or unlinking a stored file
// changes the change time of all its links, so it is only compared when the destination isn't a stored file.
bool unchanged(const StoredIdentity& recorded, const FS::FileIdentity& now, bool compareChangeTime)
{
    const auto current = StoredIdentity::from(now);
    return current.volume == recorded.volume && current.fileId == recorded.fileId && current.size == recorded.size &&
           current.modifiedTime == recorded.modifiedTime && (!compareChangeTime || current.changeTime == recorded.changeTime);
}

QString interruptedError()
{
    return QString("Interrupted for testing");
}
}  // namespace

ContentStore::LinkMode ContentStore::linkModeFromSetting(const QString& value)
{
    if (value == "HardLinks") {
        return LinkMode::HardLinks;
    }
    if (value == "Symlinks") {
        return LinkMode::SymbolicLinks;
    }
    return LinkMode::Auto;
}

QString ContentStore::Destination::path() const
{
    return QDir(root).absoluteFilePath(relativePath);
}

QString ContentStore::generationPath(const QString& hash, const Generation& generation) const
{
    return generation.retiredPath.isEmpty() ? objectPath(hash) : QDir(m_storeDir).absoluteFilePath(generation.retiredPath);
}

bool ContentStore::holdsLink(const QString& path, const Ref& ref) const
{
    const auto entry = m_table.entries().find(ref.hash);
    if (entry == m_table.entries().end()) {
        return false;
    }
    const auto* generation = entry->generation(ref.generation);
    if (!generation) {
        return false;
    }
    const auto state = inspect(path);
    if (ref.kind == LinkKind::Symbolic) {
        if (!state.isSymbolicLink) {
            return false;
        }
        if (ObjectFiles::samePath(state.target, generationPath(ref.hash, *generation))) {
            return true;
        }
        // A link to a damaged copy that couldn't be pointed at the kept copy yet still points at the canonical path,
        // which holds the same damaged file until an intact copy is stored, and that waits for the link.
        const auto canonical = FS::fileId(objectPath(ref.hash));
        return !generation->retiredPath.isEmpty() && !entry->current && ObjectFiles::samePath(state.target, objectPath(ref.hash)) &&
               canonical && generation->identity.sameFile(*canonical);
    }
    if (!state.exists || state.isSymbolicLink) {
        return false;
    }
    const auto id = FS::fileId(path);
    return id && generation->identity.sameFile(*id);
}

bool ContentStore::identityMatches(const QString& hash, int generation) const
{
    const auto entry = m_table.entries().find(hash);
    const auto* recorded = entry != m_table.entries().end() ? entry->generation(generation) : nullptr;
    if (!recorded) {
        return false;
    }
    const auto identity = FS::identity(generationPath(hash, *recorded));
    return identity && StoredIdentity::from(*identity) == recorded->identity;
}

std::optional<QJsonObject> ContentStore::recaptureIdentity(const QString& hash, int generation, bool matchedBefore) const
{
    if (!matchedBefore) {
        return std::nullopt;
    }
    const auto entry = m_table.entries().find(hash);
    if (entry == m_table.entries().end()) {
        return std::nullopt;
    }
    const auto* recorded = entry->generation(generation);
    if (!recorded) {
        return std::nullopt;
    }
    const auto identity = FS::identity(generationPath(hash, *recorded));
    // only for the same file: a different one is for verification to look at
    if (!identity || !recorded->identity.sameFile(identity->fileId)) {
        return std::nullopt;
    }
    const auto current = StoredIdentity::from(*identity);
    // adding or removing a link only changes the change time; anything else means the contents may have changed too
    if (current == recorded->identity || current.size != recorded->identity.size ||
        current.modifiedTime != recorded->identity.modifiedTime) {
        return std::nullopt;
    }
    return RefRecord::updateIdentity(hash, generation, current);
}

Result<PlacementKind> ContentStore::placeAt(const Placement& placement, const PlaceOptions& options)
{
    return place({ placement }, options).first();
}

QList<Result<PlacementKind>> ContentStore::place(const QList<Placement>& placements, const PlaceOptions& options)
{
    struct Item {
        Placement placement;
        QString path;
        QString dir;
        qint64 size = 0;
        Transaction transaction;
        Lease lease;
        // the recorded link that was at the destination
        std::optional<Ref> oldRef;
        bool oldIsRegularFile = false;
        // the destination is a stored file, whose change time the batch's own link changes may change
        bool oldIsStoredFile = false;
        bool begun = false;
        bool prepared = false;
        bool swapped = false;
        std::optional<PlacementKind> created;
        std::optional<QString> error;

        void fail(const QString& message)
        {
            if (!error) {
                error = message;
            }
        }
    };

    const auto mode = options.mode.value_or(m_linkMode);
    std::vector<Item> items(placements.size());
    QMap<std::pair<QString, int>, bool> matchedBefore;
    const auto results = [&items] {
        QList<Result<PlacementKind>> list;
        for (const auto& item : items) {
            if (item.error) {
                list.append(std::unexpected(*item.error));
            } else if (item.created) {
                list.append(*item.created);
            } else {
                list.append(std::unexpected(QString("%1 wasn't placed").arg(item.path)));
            }
        }
        return list;
    };
    const auto interruptedResults = [&items] {
        return QList<Result<PlacementKind>>(static_cast<qsizetype>(items.size()), std::unexpected(interruptedError()));
    };

    // 1. Record what is at each destination, and begin the placements. Nothing on disk changes yet.
    {
        QMutexLocker locker(&m_mutex);
        std::set<RefKey> busy;
        for (const auto& transaction : m_table.transactions()) {
            busy.insert(transaction.key);
        }
        QMap<QString, QString> ownerRoots = m_table.owners();
        QList<QJsonObject> records;
        qint64 nextId = m_table.nextTransactionId();
        QSet<QString> storedFiles;
        for (const auto& entry : m_table.entries()) {
            if (entry.current) {
                storedFiles.insert(entry.current->identity.volume + ':' + entry.current->identity.fileId);
            }
            for (const auto& generation : entry.retired) {
                storedFiles.insert(generation.identity.volume + ':' + generation.identity.fileId);
            }
        }
        // whether each stored file the batch links or unlinks had its recorded identity before any of that
        const auto noteIdentity = [this, &matchedBefore](const QString& hash, int generation) {
            if (!matchedBefore.contains({ hash, generation })) {
                matchedBefore[{ hash, generation }] = identityMatches(hash, generation);
            }
        };

        for (qsizetype i = 0; i < placements.size(); i++) {
            auto& item = items[static_cast<std::size_t>(i)];
            item.placement = placements[i];
            const auto& destination = item.placement.destination;
            const auto& hash = item.placement.hash;
            item.path = destination.path();
            item.dir = QFileInfo(item.path).absolutePath();

            const auto begin = [&]() -> Result<> {
                if (m_state != State::Writable) {
                    return std::unexpected(QString("The shared store can't be changed"));
                }
                if (!busy.insert(destination.key()).second) {
                    return std::unexpected(QString("%1 is already being changed").arg(item.path));
                }
                const auto entry = m_table.entries().find(hash);
                if (entry == m_table.entries().end() || !entry->current) {
                    return std::unexpected(QString("%1 isn't stored").arg(hash));
                }
                const auto objectId = FS::fileId(objectPath(hash));
                if (!objectId || !entry->current->identity.sameFile(*objectId)) {
                    return std::unexpected(QString("The stored copy of %1 is missing or was replaced").arg(hash));
                }
                item.size = entry->size;

                const auto state = inspect(item.path);
                if (state.isDirectory) {
                    return std::unexpected(QString("%1 is a folder").arg(item.path));
                }
                std::optional<StoredIdentity> oldIdentity;
                const auto ref = m_table.ref(destination.key());
                if (state.exists) {
                    TRY_INTO(const auto identity, FS::identity(item.path))
                    if (ref && holdsLink(item.path, *ref)) {
                        item.oldRef = ref;
                    } else if (!item.placement.replaces) {
                        return std::unexpected(QString("%1 isn't a shared file, and replacing it wasn't confirmed").arg(item.path));
                    } else if (*item.placement.replaces != identity) {
                        return std::unexpected(QString("%1 changed since it was chosen to be replaced").arg(item.path));
                    }
                    oldIdentity = StoredIdentity::from(identity);
                    item.oldIsRegularFile = !state.isSymbolicLink;
                    item.oldIsStoredFile = !state.isSymbolicLink && storedFiles.contains(oldIdentity->volume + ':' + oldIdentity->fileId);
                    if (item.oldRef && item.oldRef->kind == LinkKind::Hard) {
                        noteIdentity(item.oldRef->hash, item.oldRef->generation);
                    }
                }
                noteIdentity(hash, entry->current->id);
                if (!QDir().mkpath(item.dir)) {
                    return std::unexpected(QString("Could not create %1").arg(item.dir));
                }

                item.transaction = { nextId++,
                                     destination.key(),
                                     temporaryName(item.dir),
                                     ref ? std::optional(ref->hash) : std::nullopt,
                                     oldIdentity,
                                     hash,
                                     std::nullopt,
                                     {} };
                item.lease = lease(hash);
                if (ownerRoots.value(destination.owner) != destination.root) {
                    const auto rootId = FS::fileId(destination.root, true);
                    records.append(
                        RefRecord::owner(destination.owner, destination.root, rootId ? QString::number(rootId->volume, 16) : QString()));
                    ownerRoots[destination.owner] = destination.root;
                }
                records.append(RefRecord::begin(item.transaction));
                return {};
            };
            if (auto begun = begin(); !begun) {
                item.fail(begun.error());
            } else {
                item.begun = true;
            }
        }

        if (!records.isEmpty()) {
            if (auto committed = commitLocked(records); !committed) {
                for (auto& item : items) {
                    item.begun = false;
                    item.fail(committed.error());
                }
            }
        }
    }
    if (interrupted(PlacementStep::Begun)) {
        return interruptedResults();
    }

    // 2. Create the new file or link next to each destination. Without the lock, as it can wait for the user.
    const auto createCopy = [this](Item& item) {
        auto copied = createEmptyFile(item.transaction.temporaryPath).and_then([&] {
            return ObjectFiles::copyAndHash(objectPath(item.placement.hash), item.transaction.temporaryPath);
        });
        if (!copied) {
            item.fail(copied.error());
            return;
        }
        if (*copied != item.placement.hash || QFileInfo(item.transaction.temporaryPath).size() != item.size) {
            item.fail(QString("The copy of %1 doesn't match it").arg(item.placement.hash));
            return;
        }
        if (auto flushed = FS::flushFile(item.transaction.temporaryPath); !flushed) {
            item.fail(flushed.error());
            return;
        }
        item.created = PlacementKind::Local;
    };
    const auto fallBack = [&options, &createCopy](Item& item, const QString& linkError) {
        if (options.allowCopy) {
            createCopy(item);
        } else {
            item.fail(linkError);
        }
    };

    QList<Item*> elevated;
    for (auto& item : items) {
        if (!item.begun) {
            continue;
        }
        const auto object = objectPath(item.placement.hash);
        const auto& temporary = item.transaction.temporaryPath;
        QString linkError;
        if (mode != LinkMode::SymbolicLinks) {
            auto linked = FS::createHardLink(object, temporary);
            if (linked) {
                item.created = PlacementKind::Hard;
                continue;
            }
            linkError = linked.error();
        }
        if (mode != LinkMode::HardLinks) {
            auto linked = FS::createSymbolicLink(object, temporary);
            if (linked) {
                item.created = PlacementKind::Symbolic;
                continue;
            }
            if (linked.error().failure == FS::LinkFailure::NeedsPrivilege && m_privilegedLinker) {
                elevated.append(&item);
                continue;
            }
            linkError = linked.error().message;
        }
        fallBack(item, linkError);
    }

    if (!elevated.isEmpty()) {
        // one request for every link of the batch, so the user is asked once
        QList<std::pair<QString, QString>> links;
        for (const auto* item : elevated) {
            links.append({ objectPath(item->placement.hash), item->transaction.temporaryPath });
        }
        const auto linked = m_privilegedLinker(links);
        for (qsizetype i = 0; i < elevated.size(); i++) {
            auto& item = *elevated[i];
            if (i < linked.size() && linked[i]) {
                item.created = PlacementKind::Symbolic;
            } else {
                discardFile(item.transaction.temporaryPath);
                fallBack(item, i < linked.size() ? linked[i].error() : QString("Could not link %1").arg(item.path));
            }
        }
    }
    if (interrupted(PlacementStep::Created)) {
        return interruptedResults();
    }

    QMutexLocker locker(&m_mutex);
    if (m_state != State::Writable) {
        for (auto& item : items) {
            item.fail(QString("The shared store can't be changed"));
        }
        return results();
    }

    // 3. Check what was created and durably record it, so a crash after the swap is finished from the journal
    // stored files whose links changed, by hash and generation
    std::set<std::pair<QString, int>> recapture;
    for (auto& item : items) {
        if (!item.begun || item.error || !item.created) {
            continue;
        }
        const auto& temporary = item.transaction.temporaryPath;
        const auto entry = m_table.entries().find(item.placement.hash);
        if (*item.created == PlacementKind::Hard && entry != m_table.entries().end() && entry->current) {
            // linking changed the stored file's change time
            recapture.insert({ item.placement.hash, entry->current->id });
        }
        if (*item.created == PlacementKind::Symbolic) {
            const auto state = inspect(temporary);
            if (!state.isSymbolicLink || !ObjectFiles::samePath(state.target, objectPath(item.placement.hash))) {
                item.fail(QString("The link created for %1 doesn't point to the stored file").arg(item.path));
                continue;
            }
            item.transaction.expected = QFileInfo(objectPath(item.placement.hash)).absoluteFilePath();
            continue;
        }
        const auto id = FS::fileId(temporary);
        if (!id) {
            item.fail(id.error());
            continue;
        }
        if (*item.created == PlacementKind::Hard &&
            (entry == m_table.entries().end() || !entry->current || !entry->current->identity.sameFile(*id))) {
            item.fail(QString("The link created for %1 isn't the stored file").arg(item.path));
            continue;
        }
        item.transaction.expected = fileIdString(*id);
    }
    QMap<QString, Result<>> flushedDirs;
    for (auto& item : items) {
        if (item.begun && !item.error) {
            if (!flushedDirs.contains(item.dir)) {
                flushedDirs[item.dir] = FS::flushDir(item.dir);
            }
            if (const auto& flushed = flushedDirs[item.dir]; !flushed) {
                item.fail(flushed.error());
            }
        }
    }
    QList<QJsonObject> records;
    for (auto& item : items) {
        if (!item.begun) {
            continue;
        }
        if (item.error) {
            discardFile(item.transaction.temporaryPath);
            records.append(RefRecord::abort(item.transaction.id));
            item.begun = false;
            continue;
        }
        records.append(RefRecord::prepared(item.transaction.id, *item.created, item.transaction.expected));
        item.prepared = true;
    }
    if (!records.isEmpty()) {
        if (auto committed = commitLocked(records); !committed) {
            // the open placements are finished from the journal when the store is opened again
            for (auto& item : items) {
                if (item.prepared) {
                    item.fail(committed.error());
                }
            }
            return results();
        }
    }
    if (interrupted(PlacementStep::Prepared)) {
        return interruptedResults();
    }

    // 4. Swap each new file into place, if the destination still holds what was recorded
    records.clear();

    // Minecraft refuses symbolically linked packs unless their target is allowed, so that is durable before any swap
    QMap<QString, Result<>> allowedRoots;
    for (auto& item : items) {
        if (!item.prepared || *item.created != PlacementKind::Symbolic) {
            continue;
        }
        const auto& root = item.placement.destination.root;
        if (!allowedRoots.contains(root)) {
            allowedRoots[root] = SymlinkAllowList::allow(root, m_storeDir);
        }
        if (const auto& allowed = allowedRoots[root]; !allowed) {
            item.fail(allowed.error());
            item.prepared = false;
            discardFile(item.transaction.temporaryPath);
            records.append(RefRecord::abort(item.transaction.id));
        }
    }

    for (auto& item : items) {
        if (!item.prepared) {
            continue;
        }
        const auto swap = [&item]() -> Result<> {
            // no other program can start writing to the destination between the check and the swap
            std::optional<FS::PinnedFile> pin;
            if (item.transaction.oldIdentity) {
                if (item.oldIsRegularFile) {
                    auto pinned = FS::pinFile(item.path);
                    if (!pinned) {
                        return std::unexpected(pinned.error());
                    }
                    pin = std::move(*pinned);
                }
                TRY_INTO(const auto now, FS::identity(item.path))
                if (!unchanged(*item.transaction.oldIdentity, now, !item.oldIsStoredFile)) {
                    return std::unexpected(QString("%1 changed while it was being replaced").arg(item.path));
                }
            } else if (inspect(item.path).exists) {
                return std::unexpected(QString("A file appeared at %1 while it was being placed").arg(item.path));
            }
            return FS::replaceFile(item.transaction.temporaryPath, item.path);
        };
        if (auto swapped = swap(); !swapped) {
            item.fail(swapped.error());
            discardFile(item.transaction.temporaryPath);
            records.append(RefRecord::abort(item.transaction.id));
            continue;
        }
        item.swapped = true;
        // Renaming a hard link over another link to the same file does nothing on POSIX, which leaves the temporary
        // link, such as when a stored file is placed where it already is
        discardFile(item.transaction.temporaryPath);
        if (interrupted(PlacementStep::Swapped)) {
            return interruptedResults();
        }
        if (item.oldRef && item.oldRef->kind == LinkKind::Hard) {
            // the stored file it linked to lost a link, which changes its change time
            recapture.insert({ item.oldRef->hash, item.oldRef->generation });
        }
    }

    flushedDirs.clear();
    for (auto& item : items) {
        if (item.swapped && !flushedDirs.contains(item.dir)) {
            // the swap can't be undone without the old file, so it is recorded even if this fails
            flushedDirs[item.dir] = FS::flushDir(item.dir);
            if (const auto& flushed = flushedDirs[item.dir]; !flushed) {
                qWarning() << "Shared store:" << flushed.error();
            }
        }
    }
    for (auto& item : items) {
        if (item.swapped) {
            records.append(RefRecord::commit(item.transaction.id));
        }
    }
    for (const auto& [hash, generation] : recapture) {
        if (auto record = recaptureIdentity(hash, generation, matchedBefore.value({ hash, generation }))) {
            records.append(*record);
        }
    }
    if (!records.isEmpty()) {
        if (auto committed = commitLocked(records); !committed) {
            for (auto& item : items) {
                if (item.swapped) {
                    item.fail(committed.error());
                }
            }
            return results();
        }
    }

    // the files that were replaced may not be used by anything anymore
    QSet<QString> replaced;
    for (const auto& item : items) {
        // also a ref whose link was already gone, which the commit replaced
        if (item.swapped && item.transaction.oldHash) {
            replaced.insert(*item.transaction.oldHash);
        }
    }
    if (auto released = releaseLocked(replaced); !released) {
        qWarning() << "Shared store:" << released.error();
    }
    return results();
}

Result<ContentStore::UnshareResult> ContentStore::unshare(const RefKey& key)
{
    QMutexLocker locker(&m_mutex);
    if (m_state != State::Writable) {
        return std::unexpected(QString("The shared store can't be changed"));
    }
    const auto ref = m_table.ref(key);
    const auto root = m_table.owners().find(key.owner);
    if (!ref || root == m_table.owners().end()) {
        return std::unexpected(QString("%1 isn't a shared file").arg(key.relativePath));
    }
    for (const auto& transaction : m_table.transactions()) {
        if (transaction.key == key) {
            return std::unexpected(QString("%1 is already being changed").arg(key.relativePath));
        }
    }
    const auto path = QDir(*root).absoluteFilePath(key.relativePath);
    const auto entry = m_table.entries().find(ref->hash);
    const auto* generation = entry != m_table.entries().end() ? entry->generation(ref->generation) : nullptr;
    if (!generation) {
        return std::unexpected(QString("%1 links to a file the store doesn't know").arg(path));
    }

    // copy only from the recorded link, never from a file that replaced it
    if (!holdsLink(path, *ref)) {
        TRY(commitLocked({ RefRecord::setRefState(key, inspect(path).exists ? RefState::Replaced : RefState::Missing) }))
        return std::unexpected(QString("%1 no longer holds the shared file").arg(path));
    }
    TRY_INTO(const auto before, FS::identity(path))
    const bool symbolic = ref->kind == LinkKind::Symbolic;
    // the bytes the instance sees, which for a damaged stored file aren't the bytes its hash names
    const auto source = symbolic ? generationPath(ref->hash, *generation) : path;
    TRY_INTO(const auto sourceBefore, FS::identity(source))
    const bool matchedBefore = identityMatches(ref->hash, ref->generation);
    const auto dir = QFileInfo(path).absolutePath();

    const Transaction transaction{ m_table.nextTransactionId(),  key, temporaryName(dir), ref->hash,
                                   StoredIdentity::from(before), {},  std::nullopt,       {} };
    auto held = lease(ref->hash);
    TRY(commitLocked({ RefRecord::begin(transaction) }))
    if (interrupted(PlacementStep::Begun)) {
        return std::unexpected(interruptedError());
    }

    const auto rollback = [&](const QString& error) -> Result<UnshareResult> {
        discardFile(transaction.temporaryPath);
        if (auto aborted = commitLocked({ RefRecord::abort(transaction.id) }); !aborted) {
            return std::unexpected(QString("%1, and %2").arg(error, aborted.error()));
        }
        return std::unexpected(error);
    };

    const auto& temporary = transaction.temporaryPath;
    QString contentHash;
    auto copied = createEmptyFile(temporary)
                      .and_then([&] { return ObjectFiles::copyAndHash(source, temporary); })
                      .and_then([&](const QString& hash) {
                          contentHash = hash;
                          return FS::flushFile(temporary);
                      })
                      // the copy is checked against the bytes read, not the stored hash
                      .and_then([&] { return ObjectFiles::sha256(temporary); })
                      .and_then([&](const QString& copyHash) -> Result<> {
                          if (copyHash != contentHash) {
                              return std::unexpected(QString("The copy of %1 doesn't match it").arg(path));
                          }
                          return FS::flushDir(dir);
                      })
                      .and_then([&] { return FS::fileId(temporary); });
    if (!copied) {
        return rollback(copied.error());
    }
    if (interrupted(PlacementStep::Created)) {
        return std::unexpected(interruptedError());
    }
    TRY(commitLocked({ RefRecord::prepared(transaction.id, PlacementKind::Local, fileIdString(*copied)) }))
    if (interrupted(PlacementStep::Prepared)) {
        return std::unexpected(interruptedError());
    }

    auto swapped = [&]() -> Result<> {
        // the file that was copied, which for a symbolic link is its target: no writer can open it until the swap
        auto pin = FS::pinFile(source);
        if (!pin) {
            return std::unexpected(pin.error());
        }
        TRY_INTO(const auto now, FS::identity(path))
        TRY_INTO(const auto sourceNow, FS::identity(source))
        if (now != before || sourceNow != sourceBefore) {
            return std::unexpected(QString("%1 changed while it was being copied").arg(path));
        }
        return FS::replaceFile(temporary, path);
    }();
    if (!swapped) {
        return rollback(swapped.error());
    }
    if (interrupted(PlacementStep::Swapped)) {
        return std::unexpected(interruptedError());
    }
    if (auto flushed = FS::flushDir(dir); !flushed) {
        qWarning() << "Shared store:" << flushed.error();
    }

    QList<QJsonObject> records{ RefRecord::commit(transaction.id) };
    if (!symbolic) {
        // the stored file lost a link, which changes its change time
        if (auto record = recaptureIdentity(ref->hash, ref->generation, matchedBefore)) {
            records.append(*record);
        }
    }
    TRY(commitLocked(records))
    // this placement is done with it, so it doesn't keep the file it unlinked
    held = Lease();
    if (auto released = releaseLocked({ ref->hash }); !released) {
        qWarning() << "Shared store:" << released.error();
    }
    return UnshareResult{ ref->hash, ref->generation, contentHash };
}

Result<> ContentStore::renameRef(const RefKey& from, const QString& newRelativePath)
{
    QMutexLocker locker(&m_mutex);
    if (m_state != State::Writable) {
        return std::unexpected(QString("The shared store can't be changed"));
    }
    const auto ref = m_table.ref(from);
    const RefKey to{ from.owner, newRelativePath };
    if (!ref || to == from) {
        return {};
    }
    if (m_table.ref(to)) {
        return std::unexpected(QString("%1 is already a shared file").arg(newRelativePath));
    }
    for (const auto& transaction : m_table.transactions()) {
        if (transaction.key == from || transaction.key == to) {
            return std::unexpected(QString("%1 is being changed").arg(from.relativePath));
        }
    }
    const auto root = m_table.owners().find(from.owner);
    if (root == m_table.owners().end()) {
        return std::unexpected(QString("The folder of %1 isn't known").arg(from.owner));
    }
    if (!holdsLink(QDir(*root).absoluteFilePath(newRelativePath), *ref)) {
        return std::unexpected(QString("%1 doesn't hold the shared file of %2").arg(newRelativePath, from.relativePath));
    }
    return commitLocked({ RefRecord::moveRef(from, newRelativePath) });
}

ContentStore::PrivilegedLinker ContentStore::defaultPrivilegedLinker()
{
#if defined(Q_OS_WIN)
    return [](const QList<std::pair<QString, QString>>& links) {
        QList<FS::LinkPair> pairs;
        for (const auto& [target, link] : links) {
            pairs.append({ .src = target, .dst = link });
        }
        FS::create_link linker(pairs);
        linker.useHardLinks(false).linkRecursively(false);

        // the helper asks the user for elevated rights once, then creates every link
        QEventLoop loop;
        bool finished = false;
        QObject::connect(&linker, &FS::create_link::finishedPrivileged, &loop, [&finished, &loop](bool) {
            finished = true;
            loop.quit();
        });
        linker.runPrivileged();
        if (!finished) {
            loop.exec();
        }

        QList<Result<>> results;
        const auto created = linker.getResults();
        for (const auto& [target, link] : links) {
            Result<> result = std::unexpected(QString("Could not link %1 to %2 with elevated rights").arg(link, target));
            for (const auto& linked : created) {
                if (ObjectFiles::samePath(linked.dst, link)) {
                    result = linked.err_value == 0 ? Result<>{} : Result<>(std::unexpected(linked.err_msg));
                }
            }
            results.append(result);
        }
        return results;
    };
#else
    return nullptr;
#endif
}
