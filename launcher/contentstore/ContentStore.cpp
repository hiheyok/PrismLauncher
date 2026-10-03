#include "ContentStore.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <algorithm>

#include "FileSystem.h"
#include "contentstore/Recovery.h"

namespace {
constexpr auto g_lockFileName = ".lock";
constexpr auto g_clientFileName = "store-client.json";

// Whether the store has nothing in it yet, apart from its lock
bool isEmptyStore(const QString& storeDir)
{
    const auto entries = QDir(storeDir).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    return std::ranges::all_of(entries, [](const QString& entry) { return entry == g_lockFileName; });
}
}  // namespace

ContentStore::ContentStore(QString storeDir, QString dataDir)
    : m_storeDir(std::move(storeDir))
    , m_dataDir(std::move(dataDir))
    , m_lock(QDir(m_storeDir).filePath(g_lockFileName))
    , m_journal(m_storeDir)
{}

QString ContentStore::objectsDir() const
{
    return QDir(m_storeDir).filePath("objects");
}

QString ContentStore::temporaryDir() const
{
    return QDir(m_storeDir).filePath("tmp");
}

QString ContentStore::retiredDir() const
{
    return QDir(m_storeDir).filePath("retired");
}

Result<QString> ContentStore::loadClientId(const QString& dataDir)
{
    const auto path = QDir(dataDir).filePath(g_clientFileName);
    if (QFile::exists(path)) {
        TRY_INTO(const auto data, FS::read(path))
        const auto clientId = QJsonDocument::fromJson(data).object().value("clientId").toString();
        if (QUuid::fromString(clientId).isNull()) {
            return std::unexpected(QString("Invalid client id in %1").arg(path));
        }
        return clientId;
    }

    const auto clientId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    TRY(FS::write(path, QJsonDocument(QJsonObject{ { "clientId", clientId } }).toJson()))
    return clientId;
}

ContentStore::State ContentStore::open()
{
    if (m_state == State::Writable || m_state == State::ReadOnly) {
        return m_state;
    }
    if (!QDir().mkpath(m_storeDir)) {
        return setState(State::Disabled, QString("Could not create the store folder %1").arg(m_storeDir));
    }
    auto clientId = loadClientId(m_dataDir);
    if (!clientId) {
        return setState(State::Disabled, clientId.error());
    }
    m_clientId = *clientId;
    return openWithLock(m_lock.tryAcquire());
}

ContentStore::State ContentStore::forceOpen()
{
    if (m_state == State::Writable || m_state == State::ReadOnly) {
        return m_state;
    }
    if (open() != State::Busy) {
        return m_state;
    }
    return openWithLock(m_lock.forceAcquire());
}

ContentStore::State ContentStore::openWithLock(StoreLock::Status lockStatus)
{
    switch (lockStatus) {
        case StoreLock::Status::Acquired:
            break;
        case StoreLock::Status::HeldByOtherProcess:
            return setState(State::Busy, "Another launcher is using the shared store");
        case StoreLock::Status::HeldByOtherHost: {
            const auto holder = m_lock.holder();
            return setState(State::Busy, QString("A launcher on %1 is using the shared store").arg(holder ? holder->hostname : "?"));
        }
        case StoreLock::Status::Error:
            return setState(State::Disabled, QString("Could not lock the shared store at %1").arg(m_lock.path()));
    }

    // the format is only read while holding the lock, so no other launcher is changing it
    auto format = StoreFormatFile::read(m_storeDir);
    if (!format) {
        return setState(State::Disabled, format.error());
    }
    if (!format->has_value()) {
        if (!isEmptyStore(m_storeDir)) {
            return setState(State::ReadOnly, "The shared store has files but no format information");
        }
        if (auto written = StoreFormatFile::write(m_storeDir, StoreFormat{}); !written) {
            return setState(State::Disabled, written.error());
        }
        format = std::optional<StoreFormat>(StoreFormat{});
    }

    m_format = format->value();
    switch (m_format.accessFor()) {
        case StoreAccess::Disabled:
            return setState(State::Disabled, "The shared store was written by a newer version of the launcher");
        case StoreAccess::ReadOnly:
            return loadTable(setState(State::ReadOnly, "The shared store was changed by a newer version of the launcher"));
        case StoreAccess::Writable:
            break;
    }

    for (const auto& dir : { objectsDir(), temporaryDir(), retiredDir() }) {
        if (!QDir().mkpath(dir)) {
            return setState(State::Disabled, QString("Could not create %1").arg(dir));
        }
    }
    return loadTable(State::Writable);
}

ContentStore::State ContentStore::loadTable(State state)
{
    QMutexLocker locker(&m_mutex);

    // always read from disk: another launcher may have changed the store since it was last loaded here
    auto contents = m_journal.load();
    if (!contents) {
        return setState(State::Disabled, contents.error());
    }
    // the most restrictive header wins, so a compacted newer journal still protects its state
    m_format = StoreFormat::mostRestrictive(m_format, contents->format);
    QString message = m_statusMessage;
    switch (m_format.accessFor()) {
        case StoreAccess::Disabled:
            return setState(State::Disabled, "The shared store was written by a newer version of the launcher");
        case StoreAccess::ReadOnly:
            if (state == State::Writable) {
                state = State::ReadOnly;
                message = "The shared store was changed by a newer version of the launcher";
            }
            break;
        case StoreAccess::Writable:
            break;
    }

    auto table = RefTable::fromSnapshot(contents->snapshot);
    if (!table) {
        return setState(State::Disabled, table.error());
    }
    for (const auto& record : contents->records) {
        if (auto applied = table->apply(record); !applied) {
            return setState(State::Disabled, QString("Invalid journal record: %1").arg(applied.error()));
        }
    }
    m_table = std::move(*table);

    if (state != State::Writable) {
        return setState(state, message);
    }

    // finish what a crash interrupted, before anything else changes the store
    m_state = State::Writable;
    QList<QJsonObject> records{ RefRecord::client(m_clientId, m_dataDir, QDateTime::currentSecsSinceEpoch()) };
    records.append(Recovery::finishTransactions(m_table));
    if (auto committed = commitLocked(records); !committed) {
        return setState(State::Disabled, committed.error());
    }
    return setState(State::Writable);
}

Result<> ContentStore::commit(const QList<QJsonObject>& records)
{
    QMutexLocker locker(&m_mutex);
    return commitLocked(records);
}

Result<> ContentStore::commitLocked(const QList<QJsonObject>& records)
{
    if (m_state != State::Writable) {
        return std::unexpected(QString("The shared store can't be changed"));
    }
    // write-ahead: the records are durable before the table changes
    if (auto appended = m_journal.append(records); !appended) {
        setState(State::Disabled, appended.error());
        return appended;
    }
    for (const auto& record : records) {
        if (auto applied = m_table.apply(record); !applied) {
            // the journal and table disagree now, so stop changing the store until it is opened again
            setState(State::Disabled, QString("Invalid shared store record: %1").arg(applied.error()));
            return applied;
        }
    }
    return {};
}

Result<> ContentStore::compact()
{
    QMutexLocker locker(&m_mutex);
    if (m_state != State::Writable) {
        return std::unexpected(QString("The shared store can't be changed"));
    }
    if (!m_table.transactions().isEmpty()) {
        return std::unexpected(QString("Placements are in progress"));
    }
    return m_journal.compact(m_table.snapshot(), m_format);
}

ContentStore::State ContentStore::setState(State state, const QString& message)
{
    m_state = state;
    m_statusMessage = message;
    if (!message.isEmpty()) {
        qWarning() << "Shared store:" << message;
    }
    return state;
}
