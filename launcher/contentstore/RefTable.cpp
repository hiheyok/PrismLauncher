#include "RefTable.h"

#include <QJsonArray>

#include <algorithm>

namespace {
QString toString(LinkKind kind)
{
    return kind == LinkKind::Hard ? "hard" : "symbolic";
}

Result<LinkKind> linkKindFromString(const QString& value)
{
    if (value == "hard") {
        return LinkKind::Hard;
    }
    if (value == "symbolic") {
        return LinkKind::Symbolic;
    }
    return std::unexpected(QString("Unknown link kind %1").arg(value));
}

QString toString(RefState state)
{
    switch (state) {
        case RefState::Live:
            return "live";
        case RefState::Missing:
            return "missing";
        case RefState::Replaced:
            return "replaced";
        case RefState::Unknown:
            return "unknown";
    }
    return {};
}

Result<RefState> refStateFromString(const QString& value)
{
    for (auto state : { RefState::Live, RefState::Missing, RefState::Replaced, RefState::Unknown }) {
        if (toString(state) == value) {
            return state;
        }
    }
    return std::unexpected(QString("Unknown ref state %1").arg(value));
}

QString toString(PlacementKind kind)
{
    switch (kind) {
        case PlacementKind::Hard:
            return "hard";
        case PlacementKind::Symbolic:
            return "symbolic";
        case PlacementKind::Local:
            return "local";
    }
    return {};
}

Result<PlacementKind> placementKindFromString(const QString& value)
{
    for (auto kind : { PlacementKind::Hard, PlacementKind::Symbolic, PlacementKind::Local }) {
        if (toString(kind) == value) {
            return kind;
        }
    }
    return std::unexpected(QString("Unknown placement kind %1").arg(value));
}

QJsonObject keyToJson(const RefKey& key)
{
    return { { "owner", key.owner }, { "path", key.relativePath } };
}

RefKey keyFromJson(const QJsonObject& json)
{
    return { json["owner"].toString(), json["path"].toString() };
}

QJsonObject generationToJson(const Generation& generation)
{
    QJsonObject json{ { "id", generation.id }, { "identity", generation.identity.toJson() } };
    if (generation.corrupt) {
        json["corrupt"] = true;
    }
    if (!generation.retiredPath.isEmpty()) {
        json["retiredPath"] = generation.retiredPath;
    }
    return json;
}

Generation generationFromJson(const QJsonObject& json)
{
    return { json["id"].toInt(), StoredIdentity::fromJson(json["identity"].toObject()), json["corrupt"].toBool(),
             json["retiredPath"].toString() };
}

QJsonObject transactionToJson(const Transaction& transaction)
{
    QJsonObject json{ { "id", transaction.id },
                      { "key", keyToJson(transaction.key) },
                      { "temporary", transaction.temporaryPath },
                      { "newHash", transaction.newHash } };
    if (transaction.oldHash) {
        json["oldHash"] = *transaction.oldHash;
    }
    if (transaction.oldIdentity) {
        json["oldIdentity"] = transaction.oldIdentity->toJson();
    }
    if (transaction.preparedKind) {
        json["prepared"] = toString(*transaction.preparedKind);
        json["expected"] = transaction.expected;
    }
    return json;
}

Result<Transaction> transactionFromJson(const QJsonObject& json)
{
    Transaction transaction;
    transaction.id = json["id"].toInteger();
    if (transaction.id <= 0) {
        return std::unexpected(QString("Invalid transaction id"));
    }
    transaction.key = keyFromJson(json["key"].toObject());
    transaction.temporaryPath = json["temporary"].toString();
    transaction.newHash = json["newHash"].toString();
    if (json.contains("oldHash")) {
        transaction.oldHash = json["oldHash"].toString();
    }
    if (json.contains("oldIdentity")) {
        transaction.oldIdentity = StoredIdentity::fromJson(json["oldIdentity"].toObject());
    }
    if (json.contains("prepared")) {
        TRY_INTO(transaction.preparedKind, placementKindFromString(json["prepared"].toString()))
        transaction.expected = json["expected"].toString();
    }
    return transaction;
}

Generation* findGeneration(StoreEntry& entry, int id)
{
    if (entry.current && entry.current->id == id) {
        return &*entry.current;
    }
    for (auto& generation : entry.retired) {
        if (generation.id == id) {
            return &generation;
        }
    }
    return nullptr;
}
}  // namespace

QString fileIdString(const FS::FileId& id)
{
    return QString::number(id.volume, 16) + ':' + QString::fromLatin1(id.id.toHex());
}

StoredIdentity StoredIdentity::from(const FS::FileIdentity& identity)
{
    return { QString::number(identity.fileId.volume, 16), QString::fromLatin1(identity.fileId.id.toHex()), identity.size,
             identity.modifiedTime, identity.changeTime };
}

bool StoredIdentity::sameFile(const FS::FileId& id) const
{
    return volume == QString::number(id.volume, 16) && fileId == QString::fromLatin1(id.id.toHex());
}

QJsonObject StoredIdentity::toJson() const
{
    return { { "volume", volume },
             { "fileId", fileId },
             { "size", size },
             { "modifiedTime", QString::number(modifiedTime) },
             { "changeTime", QString::number(changeTime) } };
}

StoredIdentity StoredIdentity::fromJson(const QJsonObject& json)
{
    // the times are strings, as nanosecond times don't fit the doubles that JSON numbers are read as
    return { json["volume"].toString(), json["fileId"].toString(), json["size"].toInteger(), json["modifiedTime"].toString().toLongLong(),
             json["changeTime"].toString().toLongLong() };
}

namespace RefRecord {
QJsonObject client(const QString& clientId, const QString& dataDir, qint64 lastSeen)
{
    return { { "type", "client" }, { "client", clientId }, { "dataDir", dataDir }, { "lastSeen", lastSeen } };
}

QJsonObject owner(const QString& owner, const QString& root)
{
    return { { "type", "owner" }, { "owner", owner }, { "root", root } };
}

QJsonObject publish(const QString& hash, qint64 size, const Generation& generation)
{
    return { { "type", "publish" }, { "hash", hash }, { "size", size }, { "generation", generationToJson(generation) } };
}

QJsonObject updateIdentity(const QString& hash, int generation, const StoredIdentity& identity)
{
    return { { "type", "updateIdentity" }, { "hash", hash }, { "generation", generation }, { "identity", identity.toJson() } };
}

QJsonObject begin(const Transaction& transaction)
{
    return { { "type", "begin" }, { "transaction", transactionToJson(transaction) } };
}

QJsonObject prepared(qint64 transactionId, PlacementKind kind, const QString& expected)
{
    return { { "type", "prepared" }, { "transaction", transactionId }, { "kind", toString(kind) }, { "expected", expected } };
}

QJsonObject commit(qint64 transactionId)
{
    return { { "type", "commit" }, { "transaction", transactionId } };
}

QJsonObject abort(qint64 transactionId)
{
    return { { "type", "abort" }, { "transaction", transactionId } };
}

QJsonObject removeRef(const RefKey& key)
{
    return { { "type", "removeRef" }, { "key", keyToJson(key) } };
}

QJsonObject setRefState(const RefKey& key, RefState state)
{
    return { { "type", "setRefState" }, { "key", keyToJson(key) }, { "state", toString(state) } };
}

QJsonObject destroying(const QString& hash, int generation)
{
    return { { "type", "destroying" }, { "hash", hash }, { "generation", generation } };
}

QJsonObject destroyed(const QString& hash, int generation)
{
    return { { "type", "destroyed" }, { "hash", hash }, { "generation", generation } };
}
}  // namespace RefRecord

std::optional<Ref> RefTable::ref(const RefKey& key) const
{
    const auto it = m_refs.find(key);
    if (it == m_refs.end()) {
        return std::nullopt;
    }
    return *it;
}

Result<> RefTable::commitTransaction(qint64 transactionId)
{
    const auto it = m_transactions.find(transactionId);
    if (it == m_transactions.end()) {
        return std::unexpected(QString("Commit of unknown transaction %1").arg(transactionId));
    }
    const auto transaction = *it;
    if (!transaction.preparedKind) {
        return std::unexpected(QString("Commit of transaction %1 before it was prepared").arg(transactionId));
    }

    if (*transaction.preparedKind == PlacementKind::Local) {
        // the path now holds a local copy, which nothing in the store depends on
        m_refs.remove(transaction.key);
    } else {
        auto entry = m_entries.find(transaction.newHash);
        if (entry == m_entries.end() || !entry->current) {
            return std::unexpected(QString("Commit of transaction %1 for unknown object %2").arg(transactionId).arg(transaction.newHash));
        }
        const auto kind = *transaction.preparedKind == PlacementKind::Hard ? LinkKind::Hard : LinkKind::Symbolic;
        if (kind == LinkKind::Symbolic) {
            entry->hadSymbolicLinks = true;
        }
        entry->orphanSince.reset();
        // a single assignment, so replacing a ref with one to the same object never loses it
        m_refs[transaction.key] = { transaction.newHash, kind, entry->current->id, RefState::Live };
    }
    m_transactions.erase(it);
    return {};
}

Result<> RefTable::apply(const QJsonObject& record)
{
    const auto type = record["type"].toString();

    if (type == "client") {
        auto& client = m_clients[record["client"].toString()];
        client.dataDir = record["dataDir"].toString();
        client.lastSeen = record["lastSeen"].toInteger();
        return {};
    }
    if (type == "owner") {
        m_owners[record["owner"].toString()] = record["root"].toString();
        return {};
    }
    if (type == "publish") {
        const auto hash = record["hash"].toString();
        auto& entry = m_entries[hash];
        entry.hash = hash;
        entry.size = record["size"].toInteger();
        entry.current = generationFromJson(record["generation"].toObject());
        entry.nextGeneration = std::max(entry.nextGeneration, entry.current->id + 1);
        return {};
    }
    if (type == "updateIdentity") {
        auto entry = m_entries.find(record["hash"].toString());
        Generation* generation = entry == m_entries.end() ? nullptr : findGeneration(*entry, record["generation"].toInt());
        if (!generation) {
            return std::unexpected(QString("Identity update for an unknown generation"));
        }
        generation->identity = StoredIdentity::fromJson(record["identity"].toObject());
        return {};
    }
    if (type == "begin") {
        TRY_INTO(const auto transaction, transactionFromJson(record["transaction"].toObject()))
        m_transactions[transaction.id] = transaction;
        m_nextTransactionId = std::max(m_nextTransactionId, transaction.id + 1);
        return {};
    }
    if (type == "prepared") {
        auto it = m_transactions.find(record["transaction"].toInteger());
        if (it == m_transactions.end()) {
            return std::unexpected(QString("Prepared record for an unknown transaction"));
        }
        TRY_INTO(it->preparedKind, placementKindFromString(record["kind"].toString()))
        it->expected = record["expected"].toString();
        return {};
    }
    if (type == "commit") {
        return commitTransaction(record["transaction"].toInteger());
    }
    if (type == "abort") {
        m_transactions.remove(record["transaction"].toInteger());
        return {};
    }
    if (type == "removeRef") {
        m_refs.remove(keyFromJson(record["key"].toObject()));
        return {};
    }
    if (type == "setRefState") {
        auto it = m_refs.find(keyFromJson(record["key"].toObject()));
        if (it == m_refs.end()) {
            return std::unexpected(QString("State change for an unknown ref"));
        }
        TRY_INTO(it->state, refStateFromString(record["state"].toString()))
        return {};
    }
    if (type == "destroying") {
        m_destroying[record["hash"].toString()] = record["generation"].toInt();
        return {};
    }
    if (type == "destroyed") {
        const auto hash = record["hash"].toString();
        const auto id = record["generation"].toInt();
        m_destroying.remove(hash);
        auto entry = m_entries.find(hash);
        if (entry == m_entries.end()) {
            return {};
        }
        if (entry->current && entry->current->id == id) {
            entry->current.reset();
        }
        entry->retired.removeIf([id](const Generation& generation) { return generation.id == id; });
        if (!entry->current && entry->retired.isEmpty()) {
            m_entries.erase(entry);
        }
        return {};
    }
    return std::unexpected(QString("Unknown journal record type %1").arg(type));
}

QJsonObject RefTable::snapshot() const
{
    QJsonArray entries;
    for (const auto& entry : m_entries) {
        QJsonObject json{ { "hash", entry.hash }, { "size", entry.size }, { "nextGeneration", entry.nextGeneration } };
        if (entry.current) {
            json["current"] = generationToJson(*entry.current);
        }
        QJsonArray retired;
        for (const auto& generation : entry.retired) {
            retired.append(generationToJson(generation));
        }
        if (!retired.isEmpty()) {
            json["retired"] = retired;
        }
        if (entry.orphanSince) {
            json["orphanSince"] = *entry.orphanSince;
        }
        if (entry.hadSymbolicLinks) {
            json["hadSymbolicLinks"] = true;
        }
        entries.append(json);
    }

    QJsonArray refs;
    for (auto it = m_refs.begin(); it != m_refs.end(); ++it) {
        refs.append(QJsonObject{ { "key", keyToJson(it.key()) },
                                 { "hash", it->hash },
                                 { "kind", toString(it->kind) },
                                 { "generation", it->generation },
                                 { "state", toString(it->state) } });
    }

    QJsonObject owners;
    for (auto it = m_owners.begin(); it != m_owners.end(); ++it) {
        owners[it.key()] = *it;
    }

    QJsonObject clients;
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        QJsonObject client{ { "dataDir", it->dataDir }, { "lastSeen", it->lastSeen } };
        if (it->lastCompleteReconcile) {
            client["lastCompleteReconcile"] = *it->lastCompleteReconcile;
        }
        clients[it.key()] = client;
    }

    QJsonArray transactions;
    for (const auto& transaction : m_transactions) {
        transactions.append(transactionToJson(transaction));
    }

    QJsonObject destroying;
    for (auto it = m_destroying.begin(); it != m_destroying.end(); ++it) {
        destroying[it.key()] = *it;
    }

    return { { "entries", entries },
             { "refs", refs },
             { "owners", owners },
             { "clients", clients },
             { "transactions", transactions },
             { "destroying", destroying },
             { "nextTransactionId", m_nextTransactionId } };
}

Result<RefTable> RefTable::fromSnapshot(const QJsonObject& snapshot)
{
    RefTable table;
    for (const auto& value : snapshot["entries"].toArray()) {
        const auto json = value.toObject();
        StoreEntry entry;
        entry.hash = json["hash"].toString();
        entry.size = json["size"].toInteger();
        entry.nextGeneration = json["nextGeneration"].toInt(1);
        if (json.contains("current")) {
            entry.current = generationFromJson(json["current"].toObject());
        }
        for (const auto& generation : json["retired"].toArray()) {
            entry.retired.append(generationFromJson(generation.toObject()));
        }
        if (json.contains("orphanSince")) {
            entry.orphanSince = json["orphanSince"].toInteger();
        }
        entry.hadSymbolicLinks = json["hadSymbolicLinks"].toBool();
        table.m_entries[entry.hash] = entry;
    }
    for (const auto& value : snapshot["refs"].toArray()) {
        const auto json = value.toObject();
        Ref ref;
        ref.hash = json["hash"].toString();
        TRY_INTO(ref.kind, linkKindFromString(json["kind"].toString()))
        ref.generation = json["generation"].toInt();
        TRY_INTO(ref.state, refStateFromString(json["state"].toString()))
        table.m_refs[keyFromJson(json["key"].toObject())] = ref;
    }
    const auto owners = snapshot["owners"].toObject();
    for (auto it = owners.begin(); it != owners.end(); ++it) {
        table.m_owners[it.key()] = it->toString();
    }
    const auto clients = snapshot["clients"].toObject();
    for (auto it = clients.begin(); it != clients.end(); ++it) {
        const auto json = it->toObject();
        ClientInfo client{ json["dataDir"].toString(), json["lastSeen"].toInteger(), std::nullopt };
        if (json.contains("lastCompleteReconcile")) {
            client.lastCompleteReconcile = json["lastCompleteReconcile"].toInteger();
        }
        table.m_clients[it.key()] = client;
    }
    for (const auto& value : snapshot["transactions"].toArray()) {
        TRY_INTO(const auto transaction, transactionFromJson(value.toObject()))
        table.m_transactions[transaction.id] = transaction;
    }
    const auto destroying = snapshot["destroying"].toObject();
    for (auto it = destroying.begin(); it != destroying.end(); ++it) {
        table.m_destroying[it.key()] = it->toInt();
    }
    table.m_nextTransactionId = snapshot["nextTransactionId"].toInteger(1);
    return table;
}
