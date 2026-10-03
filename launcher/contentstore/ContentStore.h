#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

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

    // Which kinds of links placements try, after which they fall back to a copy
    enum class LinkMode : std::uint8_t {
        // hard links, then symbolic links
        Auto,
        HardLinks,
        SymbolicLinks,
    };
    // Reads the SharedStoreLinkMode setting
    static LinkMode linkModeFromSetting(const QString& value);

    // A path in the folder of a ref owner. For instances, the root is the game folder, so the relative paths are like
    // "mods/foo.jar".
    struct Destination {
        QString owner;
        QString root;
        QString relativePath;

        RefKey key() const { return { owner, relativePath }; }
        QString path() const;
    };

    struct Placement {
        Destination destination;
        QString hash;
        // The file the user chose to replace, when the destination doesn't hold a link to the store. The placement
        // fails if anything else is there by the time it is replaced.
        std::optional<FS::FileIdentity> replaces;
    };

    struct PlaceOptions {
        // the store's link mode when not set
        std::optional<LinkMode> mode;
        // put a writable copy at the destination when it can't be linked, instead of failing
        bool allowCopy = true;
    };

    struct UnshareResult {
        // the stored file the destination was linked to
        QString hash;
        int generation = 0;
        // the SHA-256 of the bytes now in the local copy, which differs from hash for a damaged stored file
        QString contentHash;
    };

    // Creates symbolic links, each given as a target and a link, with elevated rights. Called once for all the links of
    // a batch, so the user is asked once. Returns the result of each link.
    using PrivilegedLinker = std::function<QList<Result<>>(const QList<std::pair<QString, QString>>& links)>;

    // Points in a placement after which a test can stop it, as if the launcher crashed there
    enum class PlacementStep : std::uint8_t {
        Begun,
        // the temporary file or link was created
        Created,
        Prepared,
        Swapped,
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

    // Puts stored files at the destinations, replacing what is there, and records the links. Each placement either
    // completes or leaves its destination as it was. A placement that ends up as a copy records no link. Placements
    // that need a symbolic link the user isn't allowed to create share one request for elevated rights.
    QList<Result<PlacementKind>> place(const QList<Placement>& placements, const PlaceOptions& options);
    QList<Result<PlacementKind>> place(const QList<Placement>& placements) { return place(placements, PlaceOptions{}); }
    Result<PlacementKind> placeAt(const Placement& placement, const PlaceOptions& options);
    Result<PlacementKind> placeAt(const Placement& placement) { return placeAt(placement, PlaceOptions{}); }

    // Replaces the link at key with a writable copy of the bytes it shows, and forgets the link. Fails without changing
    // anything if the path doesn't hold the recorded link.
    Result<UnshareResult> unshare(const RefKey& key);

    // Records that the link at from was renamed to newRelativePath in the same owner, such as when a mod is disabled.
    // Does nothing if from isn't a recorded link.
    Result<> renameRef(const RefKey& from, const QString& newRelativePath);

    // The owner of the links of an instance used by this launcher
    QString instanceOwner(const QString& instanceId) const { return m_clientId + ":" + instanceId; }

    void setLinkMode(LinkMode mode) { m_linkMode = mode; }
    LinkMode linkMode() const { return m_linkMode; }
    void setPrivilegedLinker(PrivilegedLinker linker) { m_privilegedLinker = std::move(linker); }
    // Creates the links with the launcher's elevated helper, on Windows; null elsewhere
    static PrivilegedLinker defaultPrivilegedLinker();

    void setInterruptionForTesting(std::function<bool(PlacementStep)> interruption) { m_interruption = std::move(interruption); }

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
    // Where the file of a generation is
    QString generationPath(const QString& hash, const Generation& generation) const;
    // Whether path holds the link that ref records
    bool holdsLink(const QString& path, const Ref& ref) const;
    // Records the identity of the current file of hash again, after the launcher changed its links
    std::optional<QJsonObject> recaptureIdentity(const QString& hash, int generation) const;
    bool interrupted(PlacementStep step) const { return m_interruption && m_interruption(step); }

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
    LinkMode m_linkMode = LinkMode::Auto;
    PrivilegedLinker m_privilegedLinker;
    std::function<bool(PlacementStep)> m_interruption;
};
