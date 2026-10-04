#pragma once

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>

#include <cstdint>
#include <optional>

#include "FileSystemPrimitives.h"
#include "Result.h"

// A file id as text, as recorded for the expected result of a placement
QString fileIdString(const FS::FileId& id);

// The identity of a file as stored in the table, see FS::FileIdentity
struct StoredIdentity {
    QString volume;
    QString fileId;
    qint64 size = 0;
    qint64 modifiedTime = 0;
    qint64 changeTime = 0;

    static StoredIdentity from(const FS::FileIdentity& identity);
    bool sameFile(const FS::FileId& id) const;
    bool operator==(const StoredIdentity&) const = default;

    QJsonObject toJson() const;
    static StoredIdentity fromJson(const QJsonObject& json);
};

// One version of a stored object. A generation is retired when its file turns out to be damaged, and kept for the
// instances still using it until they choose how to repair it.
struct Generation {
    int id = 0;
    StoredIdentity identity;
    bool corrupt = false;
    // where a retired generation is kept; empty for the current one, which is at the object's canonical path
    QString retiredPath;

    bool operator==(const Generation&) const = default;
};

// A stored file, identified by the SHA-256 of its contents
struct StoreEntry {
    QString hash;
    qint64 size = 0;
    std::optional<Generation> current;
    QList<Generation> retired;
    int nextGeneration = 1;
    // set while no instance uses the file, see the orphan rules
    std::optional<qint64> orphanSince;
    bool hadSymbolicLinks = false;
    // found in the store without a record, so links to it may exist that no scan has seen yet
    bool unrecorded = false;

    // the current or a retired generation
    const Generation* generation(int id) const;

    bool operator==(const StoreEntry&) const = default;
};

enum class LinkKind : std::uint8_t { Hard, Symbolic };
enum class RefState : std::uint8_t { Live, Missing, Replaced, Unknown };

// A link from an instance to a stored file
struct Ref {
    QString hash;
    LinkKind kind = LinkKind::Hard;
    int generation = 0;
    RefState state = RefState::Live;
    // when a complete reconciliation first found the link definitely gone
    std::optional<qint64> lostSince;

    bool operator==(const Ref&) const = default;
};

// Where a ref lives: a path relative to the root of its owner, an instance or a staging folder of one launcher
struct RefKey {
    QString owner;
    QString relativePath;

    bool operator<(const RefKey& other) const { return owner != other.owner ? owner < other.owner : relativePath < other.relativePath; }
    bool operator==(const RefKey&) const = default;
};

enum class PlacementKind : std::uint8_t { Hard, Symbolic, Local };

// A placement of store content at a path that was started but not finished, see PLAN rule 1
struct Transaction {
    qint64 id = 0;
    RefKey key;
    QString temporaryPath;
    // the file at the path before the transaction
    std::optional<QString> oldHash;
    std::optional<StoredIdentity> oldIdentity;
    QString newHash;
    // set once the temporary link was created and validated
    std::optional<PlacementKind> preparedKind;
    // what the path holds once the swap happened: a file id for hard links and local copies, a target for symbolic links
    QString expected;

    bool operator==(const Transaction&) const = default;
};

struct ClientInfo {
    QString dataDir;
    qint64 lastSeen = 0;
    std::optional<qint64> lastCompleteReconcile;
    // a reconciliation that couldn't read every folder
    std::optional<qint64> lastIncompleteReconcile;
    // The latest reconciliation, in journal order, couldn't read every folder, so links may be hidden in them. Kept
    // apart from the times, which can go backwards when the clock is set back.
    bool latestReconcileIncomplete = false;

    bool operator==(const ClientInfo&) const = default;
};

// Builders for the journal records that change the table
namespace RefRecord {
QJsonObject client(const QString& clientId, const QString& dataDir, qint64 lastSeen);
// volume identifies the volume of the root, to tell a deleted root from one on a drive that isn't connected
QJsonObject owner(const QString& owner, const QString& root, const QString& volume = {});
QJsonObject removeOwner(const QString& owner);
// unrecorded: the file was found in the store without a record
QJsonObject publish(const QString& hash, qint64 size, const Generation& generation, bool unrecorded = false);
QJsonObject updateIdentity(const QString& hash, int generation, const StoredIdentity& identity);
QJsonObject begin(const Transaction& transaction);
QJsonObject prepared(qint64 transactionId, PlacementKind kind, const QString& expected);
QJsonObject commit(qint64 transactionId);
QJsonObject abort(qint64 transactionId);
QJsonObject addRef(const RefKey& key, const Ref& ref);
QJsonObject removeRef(const RefKey& key);
// the link was renamed within its owner, such as when a mod is disabled
QJsonObject moveRef(const RefKey& key, const QString& relativePath);
QJsonObject setRefState(const RefKey& key, RefState state);
QJsonObject refLost(const RefKey& key, qint64 since);
// a reconciliation of a client finished; complete if it could read every folder
QJsonObject reconciled(const QString& clientId, qint64 time, bool complete = true);
// since is when nothing used the file anymore; nullopt when something does again
QJsonObject orphan(const QString& hash, std::optional<qint64> since);
// a scan found a link to the file that isn't recorded yet, so it counts as used until then
QJsonObject linkSeen(const QString& hash, qint64 time, bool symbolic);
QJsonObject destroying(const QString& hash, int generation);
QJsonObject destroyed(const QString& hash, int generation);
// the destruction was given up, as the file turned out to be in use
QJsonObject destroyAborted(const QString& hash);
}  // namespace RefRecord

// Which stored files exist and which instances link to them. Changed only by applying journal records, so it can always
// be rebuilt from the last snapshot and the records after it.
class RefTable {
   public:
    Result<> apply(const QJsonObject& record);

    QJsonObject snapshot() const;
    static Result<RefTable> fromSnapshot(const QJsonObject& snapshot);

    const QMap<QString, StoreEntry>& entries() const { return m_entries; }
    const QMap<RefKey, Ref>& refs() const { return m_refs; }
    const QMap<QString, QString>& owners() const { return m_owners; }
    // the volume of an owner's root, as text, if known
    QString ownerVolume(const QString& owner) const { return m_ownerVolumes.value(owner); }
    const QMap<QString, ClientInfo>& clients() const { return m_clients; }
    const QMap<qint64, Transaction>& transactions() const { return m_transactions; }
    // generations whose destruction started but wasn't confirmed, by hash
    const QMap<QString, int>& destroying() const { return m_destroying; }

    std::optional<Ref> ref(const RefKey& key) const;
    // the latest time any record carried, so the store's clock never goes back behind what it recorded
    qint64 latestTime() const { return m_latestTime; }
    qint64 nextTransactionId() const { return m_nextTransactionId; }

    bool operator==(const RefTable&) const = default;

   private:
    Result<> commitTransaction(qint64 transactionId);

    QMap<QString, StoreEntry> m_entries;
    QMap<RefKey, Ref> m_refs;
    QMap<QString, QString> m_owners;
    QMap<QString, QString> m_ownerVolumes;
    QMap<QString, ClientInfo> m_clients;
    QMap<qint64, Transaction> m_transactions;
    QMap<QString, int> m_destroying;
    qint64 m_nextTransactionId = 1;
    qint64 m_latestTime = 0;
};
