#include "contentstore/ContentStore.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <filesystem>
#include <system_error>

#include "FileSystemPrimitives.h"
#include "StringUtils.h"
#include "contentstore/ObjectFiles.h"

namespace {
bool hardLink(const QString& target, const QString& link)
{
    std::error_code error;
    std::filesystem::create_hard_link(StringUtils::toStdString(target), StringUtils::toStdString(link), error);
    return !error;
}

// Removes a candidate file, which may already be read-only
void discardFile(const QString& path)
{
    if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) {
        if (auto deleted = FS::deleteLink(path); !deleted) {
            qWarning() << "Shared store:" << deleted.error();
        }
    }
}

bool sameIdentity(const FS::FileIdentity& first, const FS::FileIdentity& second)
{
    return first == second;
}
}  // namespace

ContentStore::Lease::Lease(ContentStore* store, QString hash) : m_store(store), m_hash(std::move(hash))
{
    QMutexLocker locker(&m_store->m_leaseMutex);
    m_store->m_leases[m_hash]++;
}

ContentStore::Lease::~Lease()
{
    release();
}

ContentStore::Lease::Lease(Lease&& other) noexcept : m_store(other.m_store), m_hash(std::move(other.m_hash))
{
    other.m_store = nullptr;
}

ContentStore::Lease& ContentStore::Lease::operator=(Lease&& other) noexcept
{
    if (this != &other) {
        release();
        m_store = other.m_store;
        m_hash = std::move(other.m_hash);
        other.m_store = nullptr;
    }
    return *this;
}

void ContentStore::Lease::release()
{
    if (!m_store) {
        return;
    }
    QMutexLocker locker(&m_store->m_leaseMutex);
    if (--m_store->m_leases[m_hash] <= 0) {
        m_store->m_leases.remove(m_hash);
    }
    m_store = nullptr;
}

ContentStore::Lease ContentStore::lease(const QString& hash)
{
    return { this, hash };
}

int ContentStore::leaseCount(const QString& hash) const
{
    QMutexLocker locker(&m_leaseMutex);
    return m_leases.value(hash);
}

QString ContentStore::objectPath(const QString& hash) const
{
    return ObjectFiles::objectPath(objectsDir(), hash);
}

Result<ContentStore::IngestResult> ContentStore::ingest(const QString& source,
                                                        IngestMode mode,
                                                        const std::optional<PrecomputedDigest>& digest)
{
    if (!isWritable()) {
        return std::unexpected(QString("The shared store can't be changed"));
    }
    TRY_INTO(const auto sourceIdentity, FS::identity(source))

    {
        // the file is already a stored file, linked into place earlier
        QMutexLocker locker(&m_mutex);
        for (const auto& entry : m_table.entries()) {
            // only while the stored file is still in place, which a failed publication may have undone
            const auto stored = FS::fileId(objectPath(entry.hash));
            if (entry.current && entry.current->identity.sameFile(sourceIdentity.fileId) && stored &&
                entry.current->identity.sameFile(*stored)) {
                return publishLocked({}, entry.hash, entry.size);
            }
        }
    }

    // A digest only describes the file it was computed for, unchanged. It is only used to check the hash computed here:
    // the file could change between checking its identity and moving or linking it, so the stored file is always
    // hashed after it was made read-only, and its name always matches its contents.
    const bool digestIsValid = digest && sameIdentity(digest->identity, sourceIdentity);
    // the permissions to give back if the file is returned to the caller
    const auto originalPermissions = QFile::permissions(source);

    TRY_INTO(auto candidate, FS::reserveTemporarySibling(QDir(temporaryDir()).filePath("object"), "ingest"))
    QString hash;
    bool sourceMoved = false;
    bool sourceLinked = false;
    std::optional<QFile::Permissions> sourcePermissions;

    // undoes everything done to the source, for when the file can't be stored
    const auto undo = [&](const QString& error) -> Result<IngestResult> {
        if (sourceMoved) {
            if (auto restored = FS::replaceFile(candidate, source); !restored) {
                return std::unexpected(
                    QString("%1, and the file couldn't be moved back from %2: %3").arg(error, candidate, restored.error()));
            }
        } else {
            discardFile(candidate);
        }
        if (sourcePermissions) {
            QFile::setPermissions(source, *sourcePermissions);
        }
        return std::unexpected(error);
    };

    if (mode == IngestMode::LinkIn) {
        QFile::remove(candidate);
        sourceLinked = hardLink(source, candidate);
        if (!sourceLinked) {
            // another volume, or a file system without hard links: copy instead
            TRY_INTO(candidate, FS::reserveTemporarySibling(QDir(temporaryDir()).filePath("object"), "ingest"))
        }
    } else if (mode == IngestMode::Move) {
        sourceMoved = FS::replaceFile(source, candidate).has_value();
    }

    if (sourceLinked || sourceMoved) {
        // the candidate is the source file itself, so making it read-only also changes the source
        sourcePermissions = originalPermissions;
    } else {
        // copy, also the fallback when the file couldn't be linked or moved
        auto copied = ObjectFiles::copyAndHash(source, candidate);
        if (!copied) {
            return undo(copied.error());
        }
        hash = *copied;
    }

    if (auto flushed = FS::flushFile(candidate); !flushed) {
        return undo(flushed.error());
    }
    if (!ObjectFiles::makeReadOnly(candidate)) {
        return undo(QString("Could not make %1 read-only").arg(candidate));
    }

    if (hash.isEmpty()) {
        auto hashed = ObjectFiles::sha256(candidate);
        if (!hashed) {
            return undo(hashed.error());
        }
        hash = *hashed;
    }
    if (digestIsValid && digest->sha256 != hash) {
        return undo(QString("%1 doesn't match its expected checksum").arg(source));
    }

    QMutexLocker locker(&m_mutex);
    auto published = publishLocked(candidate, hash, sourceIdentity.size);
    if (!published) {
        locker.unlock();
        return undo(published.error());
    }

    if (published->reusedObject && sourcePermissions) {
        // the identical stored file is used instead, so the source isn't part of the store and stays writable
        QFile::setPermissions(source, *sourcePermissions);
    }
    if (mode == IngestMode::Move && !sourceMoved) {
        // copied because it couldn't be moved: the source is only removed once it is safely stored
        QFile::remove(source);
    }
    return published;
}

Result<ContentStore::IngestResult> ContentStore::publishLocked(const QString& candidate, const QString& hash, qint64 size)
{
    if (m_state != State::Writable) {
        return std::unexpected(QString("The shared store can't be changed"));
    }
    IngestResult result;
    result.hash = hash;
    result.size = size;
    result.lease = lease(hash);

    const auto path = objectPath(hash);
    const auto discardCandidate = [&candidate] {
        if (!candidate.isEmpty()) {
            discardFile(candidate);
        }
    };

    const auto entry = m_table.entries().find(hash);
    if (entry != m_table.entries().end() && entry->current) {
        auto current = FS::identity(path);
        if (current && StoredIdentity::from(*current) == entry->current->identity) {
            discardCandidate();
            result.generation = entry->current->id;
            result.reusedObject = true;
            return result;
        }
        if (current) {
            // metadata changed since it was recorded, for example by a backup tool: check the contents
            TRY_INTO(const auto storedHash, ObjectFiles::sha256(path))
            if (storedHash != hash) {
                return std::unexpected(QString("The stored copy of %1 is damaged").arg(hash));
            }
            TRY(commitLocked({ RefRecord::updateIdentity(hash, entry->current->id, StoredIdentity::from(*current)) }))
            discardCandidate();
            result.generation = entry->current->id;
            result.reusedObject = true;
            result.rehashedObject = true;
            return result;
        }
    }
    if (candidate.isEmpty()) {
        return std::unexpected(QString("The stored copy of %1 is missing").arg(hash));
    }

    const int generation = entry != m_table.entries().end() ? entry->nextGeneration : 1;
    const auto dir = QFileInfo(path).absolutePath();
    if (!QFileInfo::exists(dir)) {
        if (!QDir().mkpath(dir)) {
            return std::unexpected(QString("Could not create %1").arg(dir));
        }
        TRY(FS::flushDir(objectsDir()))
    }

    if (QFileInfo::exists(path)) {
        // stored before a crash, but never recorded: keep it if it is intact
        auto storedHash = ObjectFiles::sha256(path);
        if (storedHash && *storedHash == hash) {
            TRY_INTO(const auto identity, FS::identity(path))
            TRY(commitLocked({ RefRecord::publish(hash, size, { generation, StoredIdentity::from(identity), false, {} }) }))
            discardCandidate();
            result.generation = generation;
            result.reusedObject = true;
            return result;
        }
    }

    TRY(FS::replaceFile(candidate, path))
    auto recorded =
        FS::flushDir(dir)
            // recorded after the swap, which changes the file's change time on some file systems
            .and_then([&] { return FS::identity(path); })
            .and_then([&](const FS::FileIdentity& identity) {
                return commitLocked({ RefRecord::publish(hash, size, { generation, StoredIdentity::from(identity), false, {} }) });
            });
    if (!recorded) {
        // not recorded, so put it back where the caller can undo the ingest
        if (auto restored = FS::replaceFile(path, candidate); !restored) {
            return std::unexpected(QString("%1, and %2 was left unrecorded in the store").arg(recorded.error(), path));
        }
        return std::unexpected(recorded.error());
    }
    result.generation = generation;
    return result;
}
