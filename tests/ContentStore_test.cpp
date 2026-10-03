#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QTemporaryDir>
#include <QTest>

#include "FileSystem.h"
#include "FileSystemPrimitives.h"
#include "contentstore/ContentStore.h"
#include "contentstore/StoreFormat.h"
#include "contentstore/StoreLock.h"

namespace {
QStringList storeEntries(const QString& dir)
{
    return QDir(dir).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

// Changes one line of a lock file written by QLockFile: 0 is the process id, 2 the host name, 3 the machine id
bool editLockFile(const QString& path, const QMap<int, QByteArray>& lines)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    auto contents = file.readAll().split('\n');
    file.close();
    for (auto it = lines.begin(); it != lines.end(); ++it) {
        if (it.key() >= contents.size()) {
            return false;
        }
        contents[it.key()] = it.value();
    }
    QFile::remove(path);
    return writeFile(path, contents.join('\n'));
}

// Leaves a lock file behind, as a crashed or remote launcher would. Returns its path, or nothing on failure.
QString leaveLockFile(const QString& storeDir)
{
    const auto path = QDir(storeDir).filePath(".lock");
    QByteArray contents;
    {
        QLockFile lock(path);
        if (!lock.tryLock(0)) {
            return {};
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        contents = file.readAll();
    }
    return writeFile(path, contents) ? path : QString();
}

// A process id that isn't running
const QByteArray g_deadPid = "2147480000";
}  // namespace

class ContentStoreTest : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;

    QString path(const QString& name) const { return m_dir.filePath(name); }

   private slots:
    void init()
    {
        QVERIFY(m_dir.isValid());
        QVERIFY(FS::deleteContents(m_dir.path()));
        QVERIFY(QDir(m_dir.path()).mkpath("data"));
        QVERIFY(QDir(m_dir.path()).mkpath("store"));
    }

    void cleanup() { FS::Testing::setFaultHook(nullptr); }

    // StoreFormat

    void test_formatRoundTrip()
    {
        StoreFormat format;
        format.minWriterVersion = 2;
        format.features = { "pending-backups" };
        const auto parsed = StoreFormat::fromJson(format.toJson());
        QVERIFY(parsed);
        QVERIFY(*parsed == format);
        QVERIFY(!parsed->hasUnknownFields);
    }

    void test_formatAccess()
    {
        QCOMPARE(StoreFormat{}.accessFor(), StoreAccess::Writable);

        StoreFormat newerWriter;
        newerWriter.minWriterVersion = StoreFormat::CurrentVersion + 1;
        QCOMPARE(newerWriter.accessFor(), StoreAccess::ReadOnly);

        StoreFormat newerReader;
        newerReader.minReaderVersion = StoreFormat::CurrentVersion + 1;
        QCOMPARE(newerReader.accessFor(), StoreAccess::Disabled);

        auto json = StoreFormat{}.toJson();
        json["somethingNew"] = true;
        const auto unknown = StoreFormat::fromJson(json);
        QVERIFY(unknown && unknown->hasUnknownFields);
        QCOMPARE(unknown->accessFor(), StoreAccess::ReadOnly);
    }

    void test_formatMostRestrictive()
    {
        StoreFormat first;
        first.minWriterVersion = 2;
        StoreFormat second;
        second.minReaderVersion = 3;
        second.hasUnknownFields = true;
        const auto combined = StoreFormat::mostRestrictive(first, second);
        QCOMPARE(combined.minWriterVersion, 2);
        QCOMPARE(combined.minReaderVersion, 3);
        QVERIFY(combined.hasUnknownFields);
        QCOMPARE(combined.accessFor(), StoreAccess::Disabled);
    }

    void test_formatRejectsInvalidJson()
    {
        QVERIFY(!StoreFormat::fromJson(QJsonObject{ { "formatVersion", 1 } }));
        QVERIFY(writeFile(path("store/format.json"), "{ not json"));
        QVERIFY(!StoreFormatFile::read(path("store")));
    }

    void test_formatFile()
    {
        const auto missing = StoreFormatFile::read(path("store"));
        QVERIFY(missing && !missing->has_value());

        StoreFormat format;
        format.minWriterVersion = 2;
        QVERIFY(StoreFormatFile::write(path("store"), format));
        const auto read = StoreFormatFile::read(path("store"));
        QVERIFY(read && read->has_value());
        QVERIFY(read->value() == format);
        QCOMPARE(storeEntries(path("store")), QStringList{ "format.json" });
    }

    void test_failedFormatWriteKeepsOldHeader()
    {
        QVERIFY(StoreFormatFile::write(path("store"), StoreFormat{}));
        FS::Testing::setFaultHook(
            [](FS::Testing::Operation operation, const QString&) { return operation == FS::Testing::Operation::Replace; });
        StoreFormat newer;
        newer.minWriterVersion = 2;
        QVERIFY(!StoreFormatFile::write(path("store"), newer));
        FS::Testing::setFaultHook(nullptr);

        QVERIFY(StoreFormatFile::read(path("store"))->value() == StoreFormat{});
        QCOMPARE(storeEntries(path("store")), QStringList{ "format.json" });
    }

    // StoreLock

    void test_lockIsExclusive()
    {
        StoreLock first(path("store/.lock"));
        StoreLock second(path("store/.lock"));
        QCOMPARE(first.tryAcquire(), StoreLock::Status::Acquired);
        QCOMPARE(second.tryAcquire(), StoreLock::Status::HeldByOtherProcess);
        QVERIFY(second.holder());
        QCOMPARE(second.holder()->pid, QCoreApplication::applicationPid());

        first.release();
        QCOMPARE(second.tryAcquire(), StoreLock::Status::Acquired);
    }

    void test_oldLockOfRunningProcessIsNotStolen()
    {
        // a lock file of this process, which is running, older than QLockFile's default stale time of 30 seconds
        const auto lockPath = leaveLockFile(path("store"));
        QVERIFY(!lockPath.isEmpty());
        QFile lockFile(lockPath);
        QVERIFY(lockFile.open(QIODevice::ReadWrite));
        QVERIFY(lockFile.setFileTime(QDateTime::currentDateTime().addSecs(-600), QFileDevice::FileModificationTime));
        lockFile.close();

        StoreLock lock(lockPath);
        QCOMPARE(lock.tryAcquire(), StoreLock::Status::HeldByOtherProcess);
        QVERIFY(QFile::exists(lockPath));
    }

    void test_lockOfDeadProcessIsTakenOver()
    {
        const auto lockPath = leaveLockFile(path("store"));
        QVERIFY(!lockPath.isEmpty());
        QVERIFY(editLockFile(lockPath, { { 0, g_deadPid } }));

        StoreLock lock(lockPath);
        QCOMPARE(lock.tryAcquire(), StoreLock::Status::Acquired);
    }

    void test_lockOfOtherHostIsNeverBroken()
    {
        const auto lockPath = leaveLockFile(path("store"));
        QVERIFY(!lockPath.isEmpty());
        QVERIFY(editLockFile(lockPath, { { 0, g_deadPid }, { 2, "another-machine" }, { 3, "0123456789abcdef" } }));

        StoreLock lock(lockPath);
        QCOMPARE(lock.tryAcquire(), StoreLock::Status::HeldByOtherHost);
        QCOMPARE(lock.holder()->hostname, "another-machine");
        QCOMPARE(lock.forceAcquire(), StoreLock::Status::Acquired);
    }

    // ContentStore

    void test_openEmptyStore()
    {
        ContentStore store(path("store"), path("data"));
        QCOMPARE(store.open(), ContentStore::State::Writable);
        QVERIFY(QFile::exists(path("store/format.json")));
        QVERIFY(QFileInfo(store.objectsDir()).isDir());
        QVERIFY(QFileInfo(store.temporaryDir()).isDir());
        QVERIFY(QFileInfo(store.retiredDir()).isDir());
        QVERIFY(!store.clientId().isEmpty());
    }

    void test_secondLauncherIsBusy()
    {
        auto first = std::make_unique<ContentStore>(path("store"), path("data"));
        QCOMPARE(first->open(), ContentStore::State::Writable);

        QVERIFY(QDir(m_dir.path()).mkpath("data2"));
        ContentStore second(path("store"), path("data2"));
        QCOMPARE(second.open(), ContentStore::State::Busy);
        QVERIFY(!second.isWritable());

        first.reset();
        QCOMPARE(second.open(), ContentStore::State::Writable);
    }

    void test_openNewerStore()
    {
        StoreFormat newerWriter;
        newerWriter.minWriterVersion = StoreFormat::CurrentVersion + 1;
        QVERIFY(StoreFormatFile::write(path("store"), newerWriter));
        {
            ContentStore store(path("store"), path("data"));
            QCOMPARE(store.open(), ContentStore::State::ReadOnly);
            // read-only stores aren't changed
            QVERIFY(!QFileInfo::exists(store.objectsDir()));
        }

        StoreFormat newerReader;
        newerReader.minReaderVersion = StoreFormat::CurrentVersion + 1;
        QVERIFY(StoreFormatFile::write(path("store"), newerReader));
        ContentStore store(path("store"), path("data"));
        QCOMPARE(store.open(), ContentStore::State::Disabled);
    }

    void test_storeWithoutFormatIsReadOnly()
    {
        QVERIFY(QDir(path("store")).mkpath("objects/ab"));
        ContentStore store(path("store"), path("data"));
        QCOMPARE(store.open(), ContentStore::State::ReadOnly);
        QVERIFY(!QFile::exists(path("store/format.json")));
    }

    void test_corruptFormatDisablesStore()
    {
        QVERIFY(writeFile(path("store/format.json"), "{ not json"));
        ContentStore store(path("store"), path("data"));
        QCOMPARE(store.open(), ContentStore::State::Disabled);
        QVERIFY(!store.statusMessage().isEmpty());
    }

    void test_clientId()
    {
        const auto first = ContentStore::loadClientId(path("data"));
        QVERIFY(first);
        QCOMPARE(ContentStore::loadClientId(path("data")).value(), *first);
        QVERIFY(QDir(m_dir.path()).mkpath("other-data"));
        QVERIFY(ContentStore::loadClientId(path("other-data")).value() != *first);
    }
};

QTEST_GUILESS_MAIN(ContentStoreTest)

#include "ContentStore_test.moc"
