#include "contentstore/ContentStore.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include "FileSystem.h"
#include "FileSystemPrimitives.h"

namespace {
QString interruptedError()
{
    return QString("Interrupted for testing");
}

// Gives the user's file its permissions back when a conversion doesn't complete
class ConversionGuard {
   public:
    ConversionGuard(QString path, FS::FileId fileId, QFile::Permissions permissions)
        : m_path(std::move(path)), m_fileId(std::move(fileId)), m_permissions(permissions)
    {}
    ~ConversionGuard()
    {
        if (m_active) {
            restore();
        }
    }
    ConversionGuard(const ConversionGuard&) = delete;
    ConversionGuard& operator=(const ConversionGuard&) = delete;

    // the conversion completed, or the file isn't the user's anymore
    void dismiss() { m_active = false; }

    void restore()
    {
        // only the same file: a file that replaced it isn't the conversion's to touch
        if (const auto id = FS::fileId(m_path); id && *id == m_fileId && QFile::permissions(m_path) != m_permissions) {
            QFile::setPermissions(m_path, m_permissions);
        }
        m_active = false;
    }

   private:
    QString m_path;
    FS::FileId m_fileId;
    QFile::Permissions m_permissions;
    bool m_active = true;
};

constexpr bool g_canReplaceUserFiles =
#if defined(Q_OS_WIN)
    // pinning the file keeps writers away until it is replaced
    true;
#else
    // needs the backup protocol, so a write landing during the swap isn't lost
    false;
#endif
}  // namespace

StoreFormat ContentStore::format() const
{
    QMutexLocker locker(&m_mutex);
    return m_format;
}

Result<> ContentStore::raiseWriterVersionLocked()
{
    if (m_format.minWriterVersion >= StoreFormat::ConversionWriterVersion) {
        return {};
    }
    auto raised = m_format;
    raised.minWriterVersion = StoreFormat::ConversionWriterVersion;
    // format.json first, then every snapshot and segment from now on: the most restrictive header wins, so the store
    // is protected from the first write on
    TRY(StoreFormatFile::write(m_storeDir, raised))
    m_format = raised;
    return m_journal.compact(m_table.snapshot(), raised);
}

Result<> ContentStore::lowerWriterVersionLocked()
{
    if (m_state != State::Writable || m_format.minWriterVersion != StoreFormat::ConversionWriterVersion || m_format.minReaderVersion > 1 ||
        m_format.hasUnknownFields || !m_table.freezes().isEmpty() || !m_table.transactions().isEmpty()) {
        return {};
    }
    auto lowered = m_format;
    lowered.minWriterVersion = 1;
    // the snapshot and a new segment first, then the old segments go, and format.json last: until then it still says
    // version 2, so a crash anywhere leaves the store protected, and the next start finishes lowering it
    TRY(m_journal.compact(m_table.snapshot(), lowered))
    TRY(StoreFormatFile::write(m_storeDir, lowered))
    m_format = lowered;
    return {};
}

Result<> ContentStore::finishConversions()
{
    QMutexLocker locker(&m_mutex);
    return lowerWriterVersionLocked();
}

Result<> ContentStore::finishFreezesLocked()
{
    QList<QJsonObject> records;
    for (auto it = m_table.freezes().begin(); it != m_table.freezes().end(); ++it) {
        const auto& freeze = *it;
        const auto id = FS::fileId(freeze.path);
        if (!id || fileIdString(*id) != freeze.fileId) {
            // the file is gone, or another file is there now
            records.append(RefRecord::unfreeze(it.key(), "gone"));
            continue;
        }
        const bool isStored = std::ranges::any_of(
            m_table.entries(), [&id](const StoreEntry& entry) { return entry.current && entry.current->identity.sameFile(*id); });
        if (isStored) {
            // the conversion completed: the user's file became the stored file, which stays read-only
            records.append(RefRecord::unfreeze(it.key(), "completed"));
            continue;
        }
        QFile::setPermissions(freeze.path, static_cast<QFile::Permissions>(freeze.permissions));
        records.append(RefRecord::unfreeze(it.key(), "restored"));
    }
    return commitLocked(records);
}

Result<ContentStore::ConvertResult> ContentStore::convert(const Destination& destination, const ConvertOptions& options)
{
    const auto path = destination.path();
    const auto skipped = [](const QString& reason) { return ConvertResult{ ConvertOutcome::Skipped, reason, {} }; };
    if (!isWritable()) {
        return std::unexpected(QString("The shared store can't be changed"));
    }
    const QFileInfo info(path);
    if (info.isSymbolicLink() || !info.isFile()) {
        return skipped(QString("%1 isn't a regular file").arg(path));
    }

    // already a stored file: a recorded link, or one nobody recorded yet
    if (const auto hash = storedHashOf(path)) {
        if (const auto ref = refAt(destination.key()); ref && ref->hash == *hash) {
            return ConvertResult{ ConvertOutcome::AlreadyShared, {}, *hash };
        }
        TRY_INTO(const auto identity, FS::identity(path))
        TRY(placeAt({ destination, *hash, identity }))
        return ConvertResult{ ConvertOutcome::Shared, {}, *hash };
    }

    // other hard links may be another program's or another instance's own files, which must stay as they are
    const auto links = FS::hardLinkCount(path);
    if (links != 1 && !options.adoptHardLinked) {
        return skipped(QString("%1 has other hard links").arg(path));
    }
    const bool linkIn = links == 1 && FS::sameVolume(path, temporaryDir());
    if (!linkIn && !g_canReplaceUserFiles) {
        return skipped(QString("Replacing %1 isn't supported on this system yet").arg(path));
    }
    // The user's own file becoming the stored file is written to disk first: once pinned, flushing it would need write
    // access the pin refuses.
    if (linkIn) {
        TRY(FS::flushFile(path))
    }
    // No other program may be writing to it, and none can start until the conversion is over: making the file read-only
    // wouldn't stop a writer that opened it before (only enforced on Windows)
    auto pin = FS::pinFile(path);
    if (!pin) {
        return skipped(pin.error());
    }
    TRY_INTO(const auto before, FS::identity(path))

    if (!linkIn) {
        // Copy mode: the user's file is only read, then replaced only if it is still exactly what was copied
        const auto stored = ingestFile(path, IngestMode::Copy, std::nullopt, before.fileId);
        if (!stored) {
            return std::unexpected(stored.error());
        }
        if (interrupted(PlacementStep::Ingested)) {
            return std::unexpected(interruptedError());
        }
        TRY(placeAt({ destination, stored->hash, before }))
        return ConvertResult{ ConvertOutcome::Shared, {}, stored->hash };
    }

    // LinkIn: the user's own file becomes the stored file, so its permissions change; that is recorded first
    const auto conversion = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto permissions = QFile::permissions(path);
    {
        QMutexLocker locker(&m_mutex);
        TRY(raiseWriterVersionLocked())
        TRY(commitLocked({ RefRecord::freeze(conversion, { path, fileIdString(before.fileId), static_cast<int>(permissions) }) }))
    }
    ConversionGuard guard(path, before.fileId, permissions);
    const auto finish = [this, &conversion](const QString& outcome) {
        QMutexLocker locker(&m_mutex);
        if (auto closed = commitLocked({ RefRecord::unfreeze(conversion, outcome) }); !closed) {
            qWarning() << "Shared store:" << closed.error();
        }
    };
    if (interrupted(PlacementStep::Frozen)) {
        guard.dismiss();
        return std::unexpected(interruptedError());
    }

    const auto stored = ingestFile(path, IngestMode::LinkIn, std::nullopt, before.fileId);
    if (!stored) {
        guard.restore();
        finish("failed");
        return std::unexpected(stored.error());
    }
    if (interrupted(PlacementStep::Ingested)) {
        guard.dismiss();
        return std::unexpected(interruptedError());
    }
    const auto hash = stored->hash;

    // Whether the user's own file became the stored file. Not when an identical file was already stored, nor when linking
    // it failed and the store copied it instead: then the user's file is replaced like any other.
    const auto storedId = FS::fileId(objectPath(hash));
    if (storedId && *storedId == before.fileId) {
        // the user's file became the stored file, read-only like every stored file: linking it records the link
        guard.dismiss();
        TRY_INTO(const auto identity, FS::identity(path))
        // Only the file that became the stored file may be linked in place of itself. Another file at the path, such as
        // an editor saving by renaming, is left alone: the stored file stays, and nothing records a link to it.
        if (identity.fileId != before.fileId) {
            finish("completed");
            return std::unexpected(QString("%1 was replaced while it was being shared").arg(path));
        }
        auto placed = placeAt({ destination, hash, identity });
        finish("completed");
        if (!placed) {
            // still the stored file, which reconciliation records as a link
            return std::unexpected(placed.error());
        }
        return ConvertResult{ ConvertOutcome::Shared, {}, hash };
    }

    // another file is stored, an identical one or a copy: the user's file is replaced by a link to it
    guard.restore();
    if (!g_canReplaceUserFiles) {
        finish("skipped");
        return skipped(QString("Replacing %1 isn't supported on this system yet").arg(path));
    }
    TRY_INTO(const auto current, FS::identity(path))
    // only the permissions changed since the file was hashed; a write would have changed its size or time
    if (current.fileId != before.fileId || current.size != before.size || current.modifiedTime != before.modifiedTime) {
        finish("failed");
        return std::unexpected(QString("%1 changed while it was being shared").arg(path));
    }
    auto placed = placeAt({ destination, hash, current });
    finish(placed ? "completed" : "failed");
    if (!placed) {
        return std::unexpected(placed.error());
    }
    return ConvertResult{ ConvertOutcome::Shared, {}, hash };
}
