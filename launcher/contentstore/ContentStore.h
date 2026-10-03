#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QSet>
#include <QString>
#include <QStringList>

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

    struct VerifyReport {
        int checked = 0;
        QList<RefKey> missing;
        QList<RefKey> replaced;
        // couldn't be checked, such as on a drive that isn't connected
        QList<RefKey> unknown;
        // stored files with more hard links than recorded, which only reconciliation can find
        QStringList unrecordedLinks;
    };

    struct ReconcileOptions {
        // the owners of this launcher that still exist, such as the instances in the instance list. Links of other
        // owners whose folder is definitely gone are released.
        QSet<QString> knownOwners;
    };

    struct ReconcileReport {
        // every folder could be read, so missing links count towards releasing them
        bool complete = false;
        // folders that couldn't be read
        QStringList uncertain;
        int moved = 0;
        int adopted = 0;
        int released = 0;
        int adoptedFiles = 0;
        int destroyed = 0;
        // files in the store that don't match their name, kept for a person to look at
        QStringList damagedFiles;
    };

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

    // Checks that every link of this launcher is still where it was recorded, and records which ones aren't. Only looks
    // at the recorded paths, and never releases or destroys anything.
    Result<VerifyReport> verify();

    // Scans the folders of this launcher's owners: follows links that were moved, records links nobody recorded,
    // releases links that two complete scans at least a day apart found gone, and destroys stored files nothing uses.
    Result<ReconcileReport> reconcile(const ReconcileOptions& options);

    // Destroys the stored files that nothing uses, when that is certain. Returns how many were destroyed.
    Result<int> destroyUnused();

    // A hint that the file at key was removed, such as from a folder watcher. Marks its link missing if it is gone.
    void noteRemoved(const RefKey& key);

    // How long a link must be found gone before it is released, and how old a found file must be before it is destroyed
    static constexpr qint64 LossGraceSeconds = qint64(24) * 60 * 60;
    static constexpr qint64 OrphanAgeSeconds = qint64(14) * 24 * 60 * 60;

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
    // seconds since the epoch, for tests that need time to pass
    void setClockForTesting(std::function<qint64()> clock) { m_clock = std::move(clock); }
    // paths for which reading fails, as on a drive that isn't connected or a folder without permission
    void setUnreadableForTesting(std::function<bool(const QString&)> unreadable) { m_unreadable = std::move(unreadable); }

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
    // Whether the file of a generation still has the identity recorded for it, so its contents count as checked
    bool identityMatches(const QString& hash, int generation) const;
    // Records the new change time of a generation's file after the launcher added or removed one of its links. Only if
    // its identity matched the record before (matchedBefore) and nothing but the change time differs now, so a file
    // changed by something else stays marked for hashing.
    std::optional<QJsonObject> recaptureIdentity(const QString& hash, int generation, bool matchedBefore) const;
    bool interrupted(PlacementStep step) const { return m_interruption && m_interruption(step); }
    qint64 now() const;

    enum class Presence : std::uint8_t { Present, Absent, Uncertain };
    // Whether a path definitely exists or definitely doesn't. Absence is only certain when the nearest existing folder
    // can be read and is on the expected volume, so a drive that isn't connected is uncertain.
    Presence presence(const QString& path, const QString& volume) const;
    QString ownerPath(const RefKey& key) const;
    bool isOwnOwner(const QString& owner) const;

    Result<VerifyReport> verifyLocked();
    Result<int> destroyUnusedLocked(const QSet<QString>& hashes = {});
    // Marks unused files as orphans, then destroys those it can. For after links were removed.
    Result<> releaseLocked(const QSet<QString>& hashes);
    // The hashes that refs or open placements use
    QSet<QString> usedHashes() const;
    bool canDestroy(const StoreEntry& entry, const QSet<QString>& used) const;
    // Finishes or abandons destructions a crash interrupted
    Result<> finishDestructionsLocked();

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
    std::function<qint64()> m_clock;
    std::function<bool(const QString&)> m_unreadable;
};
