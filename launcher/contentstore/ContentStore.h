#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QString>

#include <cstdint>
#include <memory>
#include <optional>

#include "FileSystemPrimitives.h"
#include "Result.h"
#include "contentstore/Journal.h"
#include "contentstore/RefTable.h"
#include "contentstore/StoreFormat.h"
#include "contentstore/StoreLock.h"

// A store of files shared between instances, kept once and linked into each instance that uses them.
//
// Only one launcher process changes a store at a time: the one holding its lock. Other launchers using the same store
// keep working with their existing links, but put new files directly into their instances.
class ContentStore {
   public:
    enum class State : std::uint8_t {
        Closed,
        // this launcher holds the store and may change it
        Writable,
        // this launcher holds the store, but its format needs a newer launcher to change it
        ReadOnly,
        // another launcher holds the store
        Busy,
        // the store can't be used, see statusMessage()
        Disabled,
    };

    // How many operations use each stored file, shared with the leases so they can outlive the store
    struct LeaseCounts {
        QMutex mutex;
        QHash<QString, int> counts;
    };

    // Keeps a stored file from being destroyed while an operation uses it. Safe to release after the store closed.
    class Lease {
       public:
        Lease() = default;
        Lease(std::shared_ptr<LeaseCounts> counts, QString hash);
        ~Lease();
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        const QString& hash() const { return m_hash; }

       private:
        void release();

        std::shared_ptr<LeaseCounts> m_counts;
        QString m_hash;
    };

    enum class IngestMode : std::uint8_t {
        // copy the file, leaving it where it is
        Copy,
        // move the file into the store
        Move,
        // hard link the file into the store, so it becomes the stored file without a copy. Only for files nothing but
        // the launcher uses, such as new downloads and files staged for a new instance.
        LinkIn,
    };

    // A SHA-256 computed while the file was written, valid while the file still has this identity. Ingest checks the
    // stored file against it, but always hashes the stored file itself.
    struct PrecomputedDigest {
        QString sha256;
        FS::FileIdentity identity;
    };

    struct IngestResult {
        QString hash;
        qint64 size = 0;
        int generation = 0;
        // an identical file was already stored, so it is used instead
        bool reusedObject = false;
        // the stored file had to be hashed again to be sure it was still intact
        bool rehashedObject = false;
        Lease lease;
    };

    ContentStore(QString storeDir, QString dataDir);

    // Opens the store, or tries again if it was busy
    State open();

    // Takes over a store locked by another machine. Only for when the user confirmed that machine no longer uses it.
    State forceOpen();

    State state() const { return m_state; }
    QString statusMessage() const { return m_statusMessage; }
    bool isWritable() const { return m_state == State::Writable; }

    // Identifies this launcher's data folder among the launchers that use the store
    QString clientId() const { return m_clientId; }
    std::optional<StoreLock::Holder> lockHolder() const { return m_lock.holder(); }

    // Which files are stored and which instances link to them. Loaded while the store is open.
    const RefTable& table() const { return m_table; }

    // Durably journals the records, then applies them to the table. Only while writable.
    Result<> commit(const QList<QJsonObject>& records);

    // Replaces the journal with a snapshot of the table. Only while writable and no placement is in progress.
    Result<> compact();

    // Puts a file into the store, or finds the identical file already stored. Only while writable.
    Result<IngestResult> ingest(const QString& source, IngestMode mode, const std::optional<PrecomputedDigest>& digest = {});

    Lease lease(const QString& hash);
    int leaseCount(const QString& hash) const;

    QString objectPath(const QString& hash) const;
    QString storeDir() const { return m_storeDir; }
    QString objectsDir() const;
    QString temporaryDir() const;
    QString retiredDir() const;

    // The id of the launcher using dataDir, created on first use
    static Result<QString> loadClientId(const QString& dataDir);

   private:
    State openWithLock(StoreLock::Status lockStatus);
    // Loads the table from the journal and, when writable, finishes interrupted placements
    State loadTable(State state);
    Result<> commitLocked(const QList<QJsonObject>& records);
    State setState(State state, const QString& message = {});
    // Stores the candidate file under hash, or finds the stored file already there. Called with m_mutex locked.
    Result<IngestResult> publishLocked(const QString& candidate, const QString& hash, qint64 size);

    QString m_storeDir;
    QString m_dataDir;
    QString m_clientId;
    StoreLock m_lock;
    Journal m_journal;
    RefTable m_table;
    StoreFormat m_format;
    // serializes changes to the journal and table
    QMutex m_mutex;
    std::shared_ptr<LeaseCounts> m_leases = std::make_shared<LeaseCounts>();
    State m_state = State::Closed;
    QString m_statusMessage;
};
