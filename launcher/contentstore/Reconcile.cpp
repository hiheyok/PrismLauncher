#include "contentstore/ContentStore.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

#include <filesystem>
#include <set>
#include <system_error>

#include "FileSystem.h"
#include "FileSystemPrimitives.h"
#include "StringUtils.h"
#include "contentstore/ObjectFiles.h"

namespace fs = std::filesystem;

namespace {
// The folders of a game folder whose files are shared
const QStringList& contentFolders()
{
    static const QStringList folders{ "mods", "coremods", "resourcepacks", "texturepacks", "shaderpacks", "datapacks" };
    return folders;
}

// Leftovers of interrupted ingests older than this are removed
constexpr qint64 g_temporaryFileAgeSeconds = qint64(24) * 60 * 60;

// A form of a path that is equal for the same location
QString pathKey(const QString& path)
{
    auto key = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#if defined(Q_OS_WIN)
    key = key.toLower();
#endif
    return key;
}

QString fileIdKey(const QString& volume, const QString& id)
{
    return volume + ':' + id;
}

QString fileIdKey(const FS::FileId& id)
{
    return fileIdKey(QString::number(id.volume, 16), QString::fromLatin1(id.id.toHex()));
}

bool isNotFound(const std::error_code& error)
{
    if (!error || error == std::errc::no_such_file_or_directory) {
        return true;
    }
#if defined(Q_OS_WIN)
    // file and path not found, as some standard libraries report them
    return error.category() == std::system_category() && (error.value() == 2 || error.value() == 3);
#else
    return false;
#endif
}

bool isSha256(const QString& name)
{
    static const QRegularExpression pattern("^[0-9a-f]{64}$");
    return pattern.match(name).hasMatch();
}

// What a link found in a folder points to
struct Linked {
    QString hash;
    int generation = 0;
    LinkKind kind = LinkKind::Hard;
};
}  // namespace

qint64 ContentStore::now() const
{
    return m_clock ? m_clock() : QDateTime::currentSecsSinceEpoch();
}

bool ContentStore::isOwnOwner(const QString& owner) const
{
    return owner.startsWith(m_clientId + ':');
}

QString ContentStore::ownerPath(const RefKey& key) const
{
    return QDir(m_table.owners().value(key.owner)).absoluteFilePath(key.relativePath);
}

ContentStore::Presence ContentStore::presence(const QString& path, const QString& volume) const
{
    const auto isUnreadable = [this](const QString& candidate) {
        return m_unreadable && m_unreadable(QFileInfo(candidate).absoluteFilePath());
    };
    // a folder that can't be read hides whether anything below it exists
    for (auto current = QFileInfo(path).absoluteFilePath();;) {
        if (isUnreadable(current)) {
            return Presence::Uncertain;
        }
        const auto parent = QFileInfo(current).absolutePath();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    std::error_code error;
    const auto status = fs::symlink_status(StringUtils::toStdString(path), error);
    if (status.type() != fs::file_type::not_found) {
        return error ? Presence::Uncertain : Presence::Present;
    }
    if (!isNotFound(error)) {
        return Presence::Uncertain;
    }

    // Gone, if the nearest existing folder above it can be read and is on the volume the path was on. A drive that
    // isn't connected leaves only the folder it would be mounted in, on another volume.
    auto ancestor = QFileInfo(path).absolutePath();
    while (true) {
        error.clear();
        const auto ancestorStatus = fs::status(StringUtils::toStdString(ancestor), error);
        if (ancestorStatus.type() == fs::file_type::directory && !error) {
            break;
        }
        if (ancestorStatus.type() != fs::file_type::not_found || !isNotFound(error)) {
            return Presence::Uncertain;
        }
        const auto parent = QFileInfo(ancestor).absolutePath();
        if (parent == ancestor) {
            return Presence::Uncertain;
        }
        ancestor = parent;
    }
    fs::directory_iterator listing(StringUtils::toStdString(ancestor), error);
    if (error) {
        return Presence::Uncertain;
    }
    if (!volume.isEmpty()) {
        const auto id = FS::fileId(ancestor, true);
        if (!id || QString::number(id->volume, 16) != volume) {
            return Presence::Uncertain;
        }
    }
    return Presence::Absent;
}

Result<ContentStore::VerifyReport> ContentStore::verify()
{
    QMutexLocker locker(&m_mutex);
    return verifyLocked();
}

Result<ContentStore::VerifyReport> ContentStore::verifyLocked()
{
    if (m_state != State::Writable) {
        return std::unexpected(QString("The shared store can't be changed"));
    }
    VerifyReport report;
    QList<QJsonObject> records;
    for (auto it = m_table.refs().begin(); it != m_table.refs().end(); ++it) {
        const auto& key = it.key();
        if (!isOwnOwner(key.owner) || !m_table.owners().contains(key.owner)) {
            // other launchers check their own links
            continue;
        }
        report.checked++;
        const auto path = ownerPath(key);
        RefState state = RefState::Live;
        switch (presence(path, m_table.ownerVolume(key.owner))) {
            case Presence::Uncertain:
                state = RefState::Unknown;
                report.unknown.append(key);
                break;
            case Presence::Absent:
                state = RefState::Missing;
                report.missing.append(key);
                break;
            case Presence::Present:
                if (!holdsLink(path, *it)) {
                    // a different file is there now, such as one put in place by an editor
                    state = RefState::Replaced;
                    report.replaced.append(key);
                }
                break;
        }
        if (state != it->state) {
            records.append(RefRecord::setRefState(key, state));
        }
    }
    TRY(commitLocked(records))

    // hard links nobody recorded keep their stored file alive, and only a scan of the folders finds them
    QHash<QString, int> hardLinks;
    QSet<QString> changing;
    for (const auto& ref : m_table.refs()) {
        const auto entry = m_table.entries().find(ref.hash);
        if (ref.kind == LinkKind::Hard && (ref.state == RefState::Live || ref.state == RefState::Unknown) &&
            entry != m_table.entries().end() && entry->current && entry->current->id == ref.generation) {
            hardLinks[ref.hash]++;
        }
    }
    for (const auto& transaction : m_table.transactions()) {
        changing.insert(transaction.newHash);
    }
    for (const auto& entry : m_table.entries()) {
        if (!entry.current || changing.contains(entry.hash)) {
            continue;
        }
        const auto links = FS::hardLinkCount(objectPath(entry.hash));
        if (links > static_cast<uintmax_t>(1 + hardLinks.value(entry.hash))) {
            report.unrecordedLinks.append(entry.hash);
        }
    }
    return report;
}

void ContentStore::noteRemoved(const RefKey& key)
{
    QMutexLocker locker(&m_mutex);
    if (m_state != State::Writable || !m_table.owners().contains(key.owner)) {
        return;
    }
    const auto ref = m_table.ref(key);
    if (!ref || ref->state == RefState::Missing) {
        return;
    }
    if (presence(ownerPath(key), m_table.ownerVolume(key.owner)) == Presence::Absent) {
        if (auto marked = commitLocked({ RefRecord::setRefState(key, RefState::Missing) }); !marked) {
            qWarning() << "Shared store:" << marked.error();
        }
    }
}

bool ContentStore::canDestroy(const StoreEntry& entry, const QSet<QString>& used) const
{
    if (m_state != State::Writable || !entry.current || used.contains(entry.hash) || leaseCount(entry.hash) > 0) {
        return false;
    }
    // links nobody recorded, anywhere, including in the system trash
    if (FS::hardLinkCount(objectPath(entry.hash)) != 1) {
        return false;
    }
    // symbolic links can't be counted, so only complete scans by every launcher using the store rule them out
    const auto everyoneScannedSince = [this](qint64 since) {
        return std::ranges::all_of(m_table.clients(), [since](const ClientInfo& client) {
            return client.lastCompleteReconcile && *client.lastCompleteReconcile > since;
        });
    };
    if ((entry.unrecorded || entry.hadSymbolicLinks) && m_scanIncomplete) {
        // an older complete scan can't vouch for folders the latest one couldn't read
        return false;
    }
    if (entry.unrecorded) {
        return entry.orphanSince && now() - *entry.orphanSince >= OrphanAgeSeconds && everyoneScannedSince(*entry.orphanSince);
    }
    if (entry.hadSymbolicLinks) {
        return entry.orphanSince && everyoneScannedSince(*entry.orphanSince);
    }
    return true;
}

QSet<QString> ContentStore::usedHashes() const
{
    QSet<QString> used;
    for (const auto& ref : m_table.refs()) {
        used.insert(ref.hash);
    }
    for (const auto& transaction : m_table.transactions()) {
        used.insert(transaction.newHash);
        if (transaction.oldHash) {
            used.insert(*transaction.oldHash);
        }
    }
    return used;
}

Result<int> ContentStore::destroyUnused()
{
    QMutexLocker locker(&m_mutex);
    return destroyUnusedLocked();
}

Result<int> ContentStore::destroyUnusedLocked(const QSet<QString>& hashes)
{
    const auto used = usedHashes();
    int destroyed = 0;
    // a copy, as destroying changes the table
    const auto entries = m_table.entries();
    for (const auto& entry : entries) {
        if ((!hashes.isEmpty() && !hashes.contains(entry.hash)) || !canDestroy(entry, used)) {
            continue;
        }
        const int generation = entry.current->id;
        const auto path = objectPath(entry.hash);
        TRY(commitLocked({ RefRecord::destroying(entry.hash, generation) }))
        if (auto deleted = FS::deleteLink(path); !deleted) {
            qWarning() << "Shared store:" << deleted.error();
            TRY(commitLocked({ RefRecord::destroyAborted(entry.hash) }))
            continue;
        }
        if (auto flushed = FS::flushDir(QFileInfo(path).absolutePath()); !flushed) {
            // if the removal is lost, the file comes back without a record and is found again by reconciliation
            qWarning() << "Shared store:" << flushed.error();
        }
        TRY(commitLocked({ RefRecord::destroyed(entry.hash, generation) }))
        destroyed++;
    }
    return destroyed;
}

Result<> ContentStore::releaseLocked(const QSet<QString>& hashes)
{
    if (hashes.isEmpty()) {
        return {};
    }
    const auto used = usedHashes();
    QList<QJsonObject> records;
    for (const auto& hash : hashes) {
        const auto entry = m_table.entries().find(hash);
        if (entry != m_table.entries().end() && entry->current && !used.contains(hash) && !entry->orphanSince) {
            records.append(RefRecord::orphan(hash, now()));
        }
    }
    TRY(commitLocked(records))
    TRY(destroyUnusedLocked(hashes))
    return {};
}

Result<> ContentStore::finishDestructionsLocked()
{
    const auto used = usedHashes();
    QList<QJsonObject> records;
    const auto destroying = m_table.destroying();
    for (auto it = destroying.begin(); it != destroying.end(); ++it) {
        const auto& hash = it.key();
        const int generation = *it;
        const auto path = objectPath(hash);
        if (!QFileInfo::exists(path)) {
            // the file was removed before the crash
            records.append(RefRecord::destroyed(hash, generation));
            continue;
        }
        // every condition is checked again, as something may have started using the file since
        const auto entry = m_table.entries().find(hash);
        if (entry != m_table.entries().end() && entry->current && entry->current->id == generation && canDestroy(*entry, used) &&
            FS::deleteLink(path)) {
            if (auto flushed = FS::flushDir(QFileInfo(path).absolutePath()); !flushed) {
                qWarning() << "Shared store:" << flushed.error();
            }
            records.append(RefRecord::destroyed(hash, generation));
        } else {
            records.append(RefRecord::destroyAborted(hash));
        }
    }
    return commitLocked(records);
}

Result<ContentStore::ReconcileReport> ContentStore::reconcile(const ReconcileOptions& options)
{
    QMutexLocker locker(&m_mutex);
    if (m_state != State::Writable) {
        return std::unexpected(QString("The shared store can't be changed"));
    }
    ReconcileReport report;
    report.complete = true;
    const auto time = now();
    const auto uncertain = [&report](const QString& path) {
        report.complete = false;
        report.uncertain.append(path);
    };

    // 1. Files in the store without a record, such as from a crash before their publication was recorded. They are
    // recorded, never moved: a symbolic link may point at them.
    {
        QList<QJsonObject> records;
        std::error_code error;
        for (fs::directory_iterator shard(StringUtils::toStdString(objectsDir()), error), end; !error && shard != end;
             shard.increment(error)) {
            std::error_code shardError;
            if (!shard->is_directory(shardError)) {
                continue;
            }
            for (fs::directory_iterator file(shard->path(), shardError); !shardError && file != end; file.increment(shardError)) {
                const auto path = StringUtils::fromStdString(file->path().native());
                const auto name = QFileInfo(path).fileName();
                if (m_table.entries().contains(name) || m_table.destroying().contains(name)) {
                    continue;
                }
                const auto hash = ObjectFiles::sha256(path);
                const auto identity = FS::identity(path);
                // only an intact file at the place its name gives is a stored file
                if (!isSha256(name) || QFileInfo(path).dir().dirName() != name.left(2) || !hash || *hash != name || !identity) {
                    report.damagedFiles.append(path);
                    continue;
                }
                records.append(RefRecord::publish(name, identity->size, { 1, StoredIdentity::from(*identity), false, {} }, true));
                records.append(RefRecord::orphan(name, time));
                report.adoptedFiles++;
            }
            if (shardError) {
                uncertain(StringUtils::fromStdString(shard->path().native()));
            }
        }
        if (error) {
            uncertain(objectsDir());
        }
        TRY(commitLocked(records))
    }

    // 2. Check every recorded link of this launcher where it was recorded
    TRY(verifyLocked())

    // 3. What a link to each stored file looks like
    QHash<QString, Linked> byFileId;
    QHash<QString, Linked> byTarget;
    for (const auto& entry : m_table.entries()) {
        QList<Generation> generations = entry.retired;
        if (entry.current) {
            generations.append(*entry.current);
        }
        for (const auto& generation : generations) {
            byFileId[fileIdKey(generation.identity.volume, generation.identity.fileId)] = { entry.hash, generation.id, LinkKind::Hard };
            byTarget[pathKey(generationPath(entry.hash, generation))] = { entry.hash, generation.id, LinkKind::Symbolic };
        }
    }

    // 4. Scan the content folders of this launcher's owners for links to stored files
    struct Found {
        RefKey key;
        Linked link;
    };
    QList<Found> found;
    QSet<QString> goneOwners;
    for (auto it = m_table.owners().begin(); it != m_table.owners().end(); ++it) {
        const auto& owner = it.key();
        const auto& root = *it;
        if (!isOwnOwner(owner)) {
            continue;
        }
        const auto volume = m_table.ownerVolume(owner);
        const auto rootPresence = presence(root, volume);
        if (rootPresence == Presence::Uncertain) {
            uncertain(root);
            continue;
        }
        if (rootPresence == Presence::Absent) {
            if (options.knownOwners.contains(owner)) {
                // the instance still exists, so its folder being gone isn't to be trusted
                uncertain(root);
            } else {
                goneOwners.insert(owner);
            }
            continue;
        }

        QSet<QString> folders(contentFolders().begin(), contentFolders().end());
        for (auto ref = m_table.refs().begin(); ref != m_table.refs().end(); ++ref) {
            if (ref.key().owner == owner) {
                folders.insert(QFileInfo(ref.key().relativePath).path());
            }
        }
        for (const auto& folder : folders) {
            const auto dir = QDir(root).absoluteFilePath(folder);
            const auto folderPresence = presence(dir, volume);
            if (folderPresence == Presence::Absent) {
                continue;
            }
            if (folderPresence == Presence::Uncertain) {
                uncertain(dir);
                continue;
            }
            std::error_code error;
            for (fs::directory_iterator file(StringUtils::toStdString(dir), error), end; !error && file != end; file.increment(error)) {
                const auto path = StringUtils::fromStdString(file->path().native());
                const QFileInfo info(path);
                if (info.fileName().startsWith(".prism-")) {
                    continue;
                }
                const RefKey key{ owner, QDir(root).relativeFilePath(path) };
                if (info.isSymbolicLink()) {
                    if (const auto link = byTarget.find(pathKey(info.symLinkTarget())); link != byTarget.end()) {
                        found.append({ key, *link });
                    }
                } else if (std::error_code typeError; file->is_regular_file(typeError)) {
                    const auto id = FS::fileId(path);
                    if (!id) {
                        uncertain(path);
                    } else if (const auto link = byFileId.find(fileIdKey(*id)); link != byFileId.end()) {
                        found.append({ key, *link });
                    }
                }
            }
            if (error) {
                uncertain(dir);
            }
        }
    }

    // 5. Follow links that were moved within their owner, and record links nobody recorded. Both only add to what is
    // known, so they also happen when the scan was incomplete.
    {
        QList<QJsonObject> records;
        QList<RefKey> lost;
        for (auto it = m_table.refs().begin(); it != m_table.refs().end(); ++it) {
            if (isOwnOwner(it.key().owner) && (it->state == RefState::Missing || it->state == RefState::Replaced)) {
                lost.append(it.key());
            }
        }
        std::set<RefKey> claimed;
        for (const auto& [key, link] : found) {
            if (const auto existing = m_table.ref(key)) {
                // A recorded path, which verification already judged. If it now links to another stored file, that file
                // is in use, though it can only be recorded once the old link is released.
                if (existing->hash != link.hash || existing->generation != link.generation || existing->kind != link.kind) {
                    records.append(RefRecord::linkSeen(link.hash, time, link.kind == LinkKind::Symbolic));
                }
                continue;
            }
            if (!claimed.insert(key).second) {
                continue;
            }
            const auto moved = std::ranges::find_if(lost, [&](const RefKey& candidate) {
                const auto ref = m_table.ref(candidate);
                return candidate.owner == key.owner && ref && ref->hash == link.hash && ref->generation == link.generation &&
                       ref->kind == link.kind;
            });
            if (moved != lost.end()) {
                records.append(RefRecord::moveRef(*moved, key.relativePath));
                records.append(RefRecord::setRefState(key, RefState::Live));
                lost.erase(moved);
                report.moved++;
            } else {
                records.append(RefRecord::addRef(key, { link.hash, link.kind, link.generation, RefState::Live, std::nullopt }));
                report.adopted++;
            }
        }
        TRY(commitLocked(records))
    }

    // 6. Release links that are definitely gone, once two complete scans a day apart found them gone. Never on an
    // incomplete scan: a folder that couldn't be read proves nothing.
    if (report.complete) {
        QList<QJsonObject> records;
        for (auto it = m_table.refs().begin(); it != m_table.refs().end(); ++it) {
            const auto& key = it.key();
            const bool gone = goneOwners.contains(key.owner) || it->state == RefState::Missing || it->state == RefState::Replaced;
            if (!isOwnOwner(key.owner) || !gone) {
                continue;
            }
            if (!it->lostSince) {
                records.append(RefRecord::refLost(key, time));
            } else if (time - *it->lostSince >= LossGraceSeconds) {
                records.append(RefRecord::removeRef(key));
                report.released++;
            }
        }
        records.append(RefRecord::reconciled(m_clientId, time));
        TRY(commitLocked(records))

        // owners whose folder is gone, once all their links are released
        records.clear();
        for (const auto& owner : goneOwners) {
            const bool hasRefs = std::ranges::any_of(m_table.refs().keys(), [&owner](const RefKey& key) { return key.owner == owner; });
            if (!hasRefs) {
                records.append(RefRecord::removeOwner(owner));
            }
        }
        TRY(commitLocked(records))
    }

    // 7. Stored files nothing uses
    {
        const auto used = usedHashes();
        QList<QJsonObject> records;
        for (const auto& entry : m_table.entries()) {
            if (entry.current && !used.contains(entry.hash) && !entry.orphanSince) {
                records.append(RefRecord::orphan(entry.hash, time));
            }
        }
        TRY(commitLocked(records))
    }
    m_scanIncomplete = !report.complete;
    if (report.complete) {
        TRY_INTO(report.destroyed, destroyUnusedLocked())
    }

    // 8. Leftovers of interrupted ingests. Only links or copies, never the only copy of anything.
    std::error_code error;
    for (fs::directory_iterator file(StringUtils::toStdString(temporaryDir()), error), end; !error && file != end; file.increment(error)) {
        const auto path = StringUtils::fromStdString(file->path().native());
        const auto identity = FS::identity(path);
        // a hard link to an old file keeps its modification time, but linking it changed its change time
        if (identity && time - std::max(identity->modifiedTime, identity->changeTime) / 1000000000 > g_temporaryFileAgeSeconds) {
            if (auto deleted = FS::deleteLink(path); !deleted) {
                qWarning() << "Shared store:" << deleted.error();
            }
        }
    }
    return report;
}
