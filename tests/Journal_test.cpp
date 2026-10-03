#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <filesystem>
#include <system_error>

#include "FileSystem.h"
#include "FileSystemPrimitives.h"
#include "StringUtils.h"
#include "contentstore/ContentStore.h"
#include "contentstore/Journal.h"
#include "contentstore/Recovery.h"
#include "contentstore/RefTable.h"

namespace {
const QString g_hashA = QString(64, 'a');
const QString g_hashB = QString(64, 'b');

bool writeFile(const QString& path, const QByteArray& data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool hardLink(const QString& target, const QString& link)
{
    std::error_code error;
    std::filesystem::create_hard_link(StringUtils::toStdString(target), StringUtils::toStdString(link), error);
    return !error;
}

bool symlink(const QString& target, const QString& link)
{
    std::error_code error;
    std::filesystem::create_symlink(StringUtils::toStdString(target), StringUtils::toStdString(link), error);
    return !error;
}

Generation generationFor(const QString& path, int id = 1)
{
    return { id, StoredIdentity::from(FS::identity(path).value()), false, {} };
}

QString fileIdOf(const QString& path)
{
    return fileIdString(FS::fileId(path).value());
}

// Applies records to a table, failing the test on the first error
#define APPLY(table, records)                                          \
    for (const auto& record : QList<QJsonObject>(records)) {           \
        const auto applied = (table).apply(record);                    \
        QVERIFY2(applied, applied ? "" : qPrintable(applied.error())); \
    }
}  // namespace

class JournalTest : public QObject {
    Q_OBJECT

    // in the working directory, so hard links don't cross a tmpfs boundary
    QTemporaryDir m_dir{ QDir::current().filePath("journal_test_XXXXXX") };

    QString path(const QString& name) const { return m_dir.filePath(name); }
    QString store() const { return path("store"); }
    QString instance() const { return path("instance"); }

   private slots:
    void init()
    {
        QVERIFY(m_dir.isValid());
        QVERIFY(FS::deleteContents(m_dir.path()));
        for (const auto& dir : { "store", "files", "instance/mods", "data" }) {
            QVERIFY(QDir(m_dir.path()).mkpath(dir));
        }
    }

    void cleanup()
    {
        FS::Testing::setFaultHook(nullptr);
        FS::Testing::setCallRecorder(nullptr);
    }

    // Journal

    void test_appendAndLoad()
    {
        Journal journal(store());
        QVERIFY(journal.load());
        QVERIFY(journal.append({ RefRecord::owner("client:a", instance()) }));
        QVERIFY(journal.append({ RefRecord::owner("client:b", path("b")), RefRecord::owner("client:c", path("c")) }));

        Journal reopened(store());
        const auto contents = reopened.load();
        QVERIFY(contents);
        QCOMPARE(contents->records.size(), 3);
        QCOMPARE(contents->lastSequence, 3);
        QCOMPARE(contents->records[2]["owner"].toString(), "client:c");
        QVERIFY(!contents->hadTornRecord);
    }

    void test_tornLastRecordIsIgnored()
    {
        Journal journal(store());
        QVERIFY(journal.load());
        QVERIFY(journal.append({ RefRecord::owner("client:a", instance()), RefRecord::owner("client:b", path("b")) }));

        // a crash while writing the second record
        const auto segment = QDir(store()).filePath("refs.journal.1");
        auto data = readFile(segment);
        data.chop(10);
        QFile::remove(segment);
        QVERIFY(writeFile(segment, data));

        Journal reopened(store());
        const auto contents = reopened.load();
        QVERIFY(contents);
        QVERIFY(contents->hadTornRecord);
        QCOMPARE(contents->records.size(), 1);

        // later records go into a new segment, never after the torn line
        QVERIFY(reopened.append({ RefRecord::owner("client:c", path("c")) }));
        QVERIFY(QFile::exists(QDir(store()).filePath("refs.journal.2")));
        const auto again = Journal(store()).load();
        QVERIFY(again);
        QCOMPARE(again->records.size(), 2);
        QCOMPARE(again->records[1]["owner"].toString(), "client:c");
    }

    void test_segmentWithoutHeader()
    {
        Journal journal(store());
        QVERIFY(journal.load());
        QVERIFY(journal.append({ RefRecord::owner("client:a", instance()) }));

        // a crash after creating the next segment, before or while its header was written
        for (const QByteArray& header : { QByteArray(), QByteArray("{\"format\":{\"formatVer") }) {
            const auto segments = QDir(store()).entryList({ "refs.journal.*" }, QDir::Files);
            const auto segment = QDir(store()).filePath(QString("refs.journal.%1").arg(segments.size() + 1));
            QVERIFY(writeFile(segment, header));

            Journal reopened(store());
            QVERIFY(reopened.load());
            QVERIFY(reopened.append({ RefRecord::owner("client:" + QString::number(header.size()), path("b")) }));

            const auto contents = Journal(store()).load();
            QVERIFY2(contents, contents ? "" : qPrintable(contents.error()));
            QCOMPARE(contents->records.last()["owner"].toString(), "client:" + QString::number(header.size()));
        }
    }

    void test_damagedRecordIsAnError()
    {
        Journal journal(store());
        QVERIFY(journal.load());
        QVERIFY(journal.append({ RefRecord::owner("client:a", instance()), RefRecord::owner("client:b", path("b")) }));

        const auto segment = QDir(store()).filePath("refs.journal.1");
        auto data = readFile(segment);
        data.replace("client:a", "client:x");
        QFile::remove(segment);
        QVERIFY(writeFile(segment, data));

        QVERIFY(!Journal(store()).load());
    }

    void test_compaction()
    {
        Journal journal(store());
        QVERIFY(journal.load());
        RefTable table;
        const QList<QJsonObject> records{ RefRecord::owner("client:a", instance()), RefRecord::owner("client:b", path("b")) };
        QVERIFY(journal.append(records));
        APPLY(table, records);

        QVERIFY(journal.compact(table.snapshot(), StoreFormat{}));
        QVERIFY(!QFile::exists(QDir(store()).filePath("refs.journal.1")));
        QVERIFY(QFile::exists(QDir(store()).filePath("refs.journal.2")));

        const QList<QJsonObject> later{ RefRecord::owner("client:c", path("c")) };
        QVERIFY(journal.append(later));
        APPLY(table, later);

        const auto contents = Journal(store()).load();
        QVERIFY(contents);
        auto loaded = RefTable::fromSnapshot(contents->snapshot);
        QVERIFY(loaded);
        APPLY(*loaded, contents->records);
        QVERIFY(*loaded == table);
        QCOMPARE(contents->lastSequence, 3);
    }

    void test_compactionCrashesKeepTheTable()
    {
        // fail each step of compaction in turn: loading afterwards must always give the same table
        for (int failAt = 0; failAt < 8; failAt++) {
            QVERIFY(FS::deleteContents(store()));
            Journal journal(store());
            QVERIFY(journal.load());
            RefTable table;
            const QList<QJsonObject> records{ RefRecord::owner("client:a", instance()), RefRecord::owner("client:b", path("b")) };
            QVERIFY(journal.append(records));
            APPLY(table, records);

            int operation = 0;
            FS::Testing::setFaultHook([&operation, failAt](FS::Testing::Operation, const QString&) { return operation++ == failAt; });
            journal.compact(table.snapshot(), StoreFormat{});
            FS::Testing::setFaultHook(nullptr);

            const auto contents = Journal(store()).load();
            QVERIFY2(contents, qPrintable(QString("failing at %1: %2").arg(failAt).arg(contents ? "" : contents.error())));
            auto loaded = RefTable::fromSnapshot(contents->snapshot);
            QVERIFY(loaded);
            APPLY(*loaded, contents->records);
            QVERIFY2(*loaded == table, qPrintable(QString("failing at operation %1").arg(failAt)));
        }
    }

    void test_compactionFlushOrder()
    {
        Journal journal(store());
        QVERIFY(journal.load());
        QVERIFY(journal.append({ RefRecord::owner("client:a", instance()) }));

        QList<QPair<FS::Testing::Operation, QString>> calls;
        FS::Testing::setCallRecorder(
            [&calls](FS::Testing::Operation operation, const QString& path) { calls.append({ operation, QFileInfo(path).fileName() }); });
        QVERIFY(journal.compact(RefTable().snapshot(), StoreFormat{}));
        FS::Testing::setCallRecorder(nullptr);

        // the snapshot's data is flushed before it replaces the old one, and the directory after
        using Op = FS::Testing::Operation;
        QVERIFY(calls.size() >= 4);
        QCOMPARE(calls[0].first, Op::FlushFile);
        QCOMPARE(calls[1].first, Op::Replace);
        QCOMPARE(calls[1].second, "refs.json");
        QCOMPARE(calls[2].first, Op::FlushDir);
        // old segments are only removed after the new snapshot and segment are durable
        const auto firstDelete = std::find_if(calls.begin(), calls.end(), [](const auto& call) { return call.first == Op::Delete; });
        QVERIFY(firstDelete != calls.end());
        QVERIFY(std::any_of(calls.begin(), firstDelete, [](const auto& call) { return call.second == "refs.journal.2"; }));
    }

    void test_headerOfNewerWriterIsKept()
    {
        Journal journal(store());
        QVERIFY(journal.load());
        StoreFormat newer;
        newer.minWriterVersion = StoreFormat::CurrentVersion + 1;
        QVERIFY(journal.compact(RefTable().snapshot(), newer));
        // the newer header survives in the snapshot and segment even without format.json
        const auto contents = Journal(store()).load();
        QVERIFY(contents);
        QCOMPARE(contents->format.accessFor(), StoreAccess::ReadOnly);
    }

    // RefTable

    void test_placementCommits()
    {
        QVERIFY(writeFile(path("files/a"), "object a"));
        RefTable table;
        Transaction transaction;
        transaction.id = 1;
        transaction.key = { "client:i", "mods/a.jar" };
        transaction.newHash = g_hashA;
        APPLY(table, (QList<QJsonObject>{ RefRecord::publish(g_hashA, 8, generationFor(path("files/a"))), RefRecord::begin(transaction),
                                          RefRecord::prepared(1, PlacementKind::Hard, "id"), RefRecord::commit(1) }));
        QVERIFY(table.transactions().isEmpty());
        const auto ref = table.ref(transaction.key);
        QVERIFY(ref);
        QCOMPARE(ref->hash, g_hashA);
        QCOMPARE(ref->kind, LinkKind::Hard);
        QCOMPARE(ref->generation, 1);

        // a same-hash replacement (hard to symbolic) overwrites the ref instead of losing it
        transaction.id = 2;
        APPLY(table, (QList<QJsonObject>{ RefRecord::begin(transaction), RefRecord::prepared(2, PlacementKind::Symbolic, "target"),
                                          RefRecord::commit(2) }));
        QCOMPARE(table.ref(transaction.key)->kind, LinkKind::Symbolic);
        QVERIFY(table.entries()[g_hashA].hadSymbolicLinks);

        // a local copy removes the ref and creates none
        transaction.id = 3;
        APPLY(table, (QList<QJsonObject>{ RefRecord::begin(transaction), RefRecord::prepared(3, PlacementKind::Local, "id"),
                                          RefRecord::commit(3) }));
        QVERIFY(!table.ref(transaction.key));
    }

    void test_commitWithoutPrepareFails()
    {
        RefTable table;
        Transaction transaction;
        transaction.id = 1;
        transaction.newHash = g_hashA;
        QVERIFY(table.apply(RefRecord::begin(transaction)));
        QVERIFY(!table.apply(RefRecord::commit(1)));
        QVERIFY(!table.apply(QJsonObject{ { "type", "somethingNew" } }));
    }

    void test_snapshotRoundTrip()
    {
        QVERIFY(writeFile(path("files/a"), "object a"));
        RefTable table;
        Transaction open;
        open.id = 2;
        open.key = { "client:i", "mods/b.jar" };
        open.newHash = g_hashA;
        open.oldHash = g_hashB;
        open.oldIdentity = StoredIdentity::from(FS::identity(path("files/a")).value());
        APPLY(table, (QList<QJsonObject>{ RefRecord::client("client", path("data"), 1000), RefRecord::owner("client:i", instance()),
                                          RefRecord::publish(g_hashA, 8, generationFor(path("files/a"))), RefRecord::begin(open),
                                          RefRecord::prepared(2, PlacementKind::Symbolic, "target"), RefRecord::destroying(g_hashB, 3) }));
        const auto restored = RefTable::fromSnapshot(table.snapshot());
        QVERIFY(restored);
        QVERIFY(*restored == table);
    }

    // Recovery

    void test_recoveryAfterSwap()
    {
        // a hard link placement that swapped in, but crashed before COMMIT
        QVERIFY(writeFile(path("files/a"), "object a"));
        QVERIFY(writeFile(instance() + "/mods/a.jar", "old"));
        const auto oldIdentity = StoredIdentity::from(FS::identity(instance() + "/mods/a.jar").value());
        QVERIFY(hardLink(path("files/a"), instance() + "/mods/.a.jar.prism-new"));
        QVERIFY(FS::replaceFile(instance() + "/mods/.a.jar.prism-new", instance() + "/mods/a.jar"));

        RefTable table;
        Transaction transaction{ 1,
                                 { "client:i", "mods/a.jar" },
                                 instance() + "/mods/.a.jar.prism-new",
                                 std::nullopt,
                                 oldIdentity,
                                 g_hashA,
                                 PlacementKind::Hard,
                                 fileIdOf(path("files/a")) };
        APPLY(table, (QList<QJsonObject>{ RefRecord::owner("client:i", instance()),
                                          RefRecord::publish(g_hashA, 8, generationFor(path("files/a"))), RefRecord::begin(transaction) }));
        // the prepared state is part of the begin record here, as replay would see after PREPARED
        QVERIFY(table.transactions()[1].preparedKind);

        const auto records = Recovery::finishTransactions(table);
        QCOMPARE(records.size(), 1);
        QCOMPARE(records[0]["type"].toString(), "commit");
        APPLY(table, records);
        QCOMPARE(table.ref({ "client:i", "mods/a.jar" })->hash, g_hashA);
    }

    void test_recoveryBeforeSwapKeepsOldFile()
    {
        // prepared, but the swap didn't happen: the old file's presence must never count as success
        QVERIFY(writeFile(path("files/a"), "object a"));
        QVERIFY(writeFile(instance() + "/mods/a.jar", "old"));
        const auto oldIdentity = StoredIdentity::from(FS::identity(instance() + "/mods/a.jar").value());
        const auto temporary = instance() + "/mods/.a.jar.prism-new";
        QVERIFY(hardLink(path("files/a"), temporary));

        for (bool prepared : { false, true }) {
            RefTable table;
            Transaction transaction{ 1, { "client:i", "mods/a.jar" }, temporary, std::nullopt, oldIdentity, g_hashA, std::nullopt, {} };
            if (prepared) {
                transaction.preparedKind = PlacementKind::Hard;
                transaction.expected = fileIdOf(path("files/a"));
            }
            APPLY(table,
                  (QList<QJsonObject>{ RefRecord::owner("client:i", instance()),
                                       RefRecord::publish(g_hashA, 8, generationFor(path("files/a"))), RefRecord::begin(transaction) }));

            const auto records = Recovery::finishTransactions(table);
            QCOMPARE(records.size(), 1);
            QCOMPARE(records[0]["type"].toString(), "abort");
            QVERIFY(!QFileInfo::exists(temporary));
            QCOMPARE(readFile(instance() + "/mods/a.jar"), "old");
            APPLY(table, records);
            QVERIFY(!table.ref({ "client:i", "mods/a.jar" }));
            QVERIFY(hardLink(path("files/a"), temporary));
        }
    }

    void test_recoveryOfLocalCopyMatchesByFileIdOnly()
    {
        // a local copy swapped in, then edited (changing its size) before the crash was recovered
        QVERIFY(writeFile(instance() + "/mods/a.jar", "copy"));
        const auto expected = fileIdOf(instance() + "/mods/a.jar");
        QFile file(instance() + "/mods/a.jar");
        QVERIFY(file.open(QIODevice::Append));
        file.write(" with edits");
        file.close();

        RefTable table;
        Transaction transaction{ 1, { "client:i", "mods/a.jar" }, {}, g_hashA, std::nullopt, g_hashA, PlacementKind::Local, expected };
        APPLY(table, (QList<QJsonObject>{ RefRecord::owner("client:i", instance()), RefRecord::begin(transaction) }));
        const auto records = Recovery::finishTransactions(table);
        QCOMPARE(records.size(), 1);
        QCOMPARE(records[0]["type"].toString(), "commit");
    }

    void test_recoveryOfSymbolicLink()
    {
        QVERIFY(writeFile(path("files/a"), "object a"));
        if (!symlink(path("files/a"), instance() + "/mods/a.jar")) {
            QSKIP("Can't create symbolic links here");
        }
        RefTable table;
        Transaction transaction{
            1, { "client:i", "mods/a.jar" }, {}, std::nullopt, std::nullopt, g_hashA, PlacementKind::Symbolic, path("files/a")
        };
        APPLY(table, (QList<QJsonObject>{ RefRecord::owner("client:i", instance()),
                                          RefRecord::publish(g_hashA, 8, generationFor(path("files/a"))), RefRecord::begin(transaction) }));
        const auto records = Recovery::finishTransactions(table);
        QCOMPARE(records.size(), 1);
        QCOMPARE(records[0]["type"].toString(), "commit");
        APPLY(table, records);
        QCOMPARE(table.ref(transaction.key)->kind, LinkKind::Symbolic);
    }

    void test_recoveryOfUnexpectedFileMarksRefReplaced()
    {
        // the path holds neither the old nor the new file: abort, and mark the old ref for reconciliation
        QVERIFY(writeFile(path("files/a"), "object a"));
        QVERIFY(writeFile(path("files/b"), "object b"));
        QVERIFY(hardLink(path("files/b"), instance() + "/mods/a.jar"));
        const auto oldIdentity = StoredIdentity::from(FS::identity(instance() + "/mods/a.jar").value());
        QFile::remove(instance() + "/mods/a.jar");
        QVERIFY(writeFile(instance() + "/mods/a.jar", "someone else's file"));

        RefTable table;
        Transaction old{
            1, { "client:i", "mods/a.jar" }, {}, std::nullopt, std::nullopt, g_hashB, PlacementKind::Hard, fileIdOf(path("files/b"))
        };
        Transaction transaction{
            2, { "client:i", "mods/a.jar" }, {}, g_hashB, oldIdentity, g_hashA, PlacementKind::Hard, fileIdOf(path("files/a"))
        };
        APPLY(table,
              (QList<QJsonObject>{ RefRecord::owner("client:i", instance()), RefRecord::publish(g_hashA, 8, generationFor(path("files/a"))),
                                   RefRecord::publish(g_hashB, 8, generationFor(path("files/b"))), RefRecord::begin(old),
                                   RefRecord::commit(1), RefRecord::begin(transaction) }));

        const auto records = Recovery::finishTransactions(table);
        APPLY(table, records);
        QVERIFY(table.transactions().isEmpty());
        QCOMPARE(table.ref(transaction.key)->hash, g_hashB);
        QCOMPARE(table.ref(transaction.key)->state, RefState::Replaced);
        QCOMPARE(readFile(instance() + "/mods/a.jar"), "someone else's file");
    }

    // ContentStore

    void test_storeReplaysJournalOnOpen()
    {
        QVERIFY(writeFile(path("files/a"), "object a"));
        {
            ContentStore store(this->store(), path("data"));
            QCOMPARE(store.open(), ContentStore::State::Writable);
            QVERIFY(
                store.commit({ RefRecord::owner("client:i", instance()), RefRecord::publish(g_hashA, 8, generationFor(path("files/a"))) }));
        }
        {
            ContentStore reopened(store(), path("data"));
            QCOMPARE(reopened.open(), ContentStore::State::Writable);
            QVERIFY(reopened.table().entries().contains(g_hashA));
            QCOMPARE(reopened.table().owners().value("client:i"), instance());
            QVERIFY(reopened.table().clients().contains(reopened.clientId()));
            QVERIFY(reopened.compact());
        }
        ContentStore compacted(store(), path("data"));
        QCOMPARE(compacted.open(), ContentStore::State::Writable);
        QVERIFY(compacted.table().entries().contains(g_hashA));
        QCOMPARE(compacted.table().owners().value("client:i"), instance());
    }

    void test_storeFinishesInterruptedPlacementOnOpen()
    {
        QVERIFY(writeFile(path("files/a"), "object a"));
        QVERIFY(hardLink(path("files/a"), instance() + "/mods/a.jar"));
        {
            ContentStore store(this->store(), path("data"));
            QCOMPARE(store.open(), ContentStore::State::Writable);
            Transaction transaction{
                1, { "client:i", "mods/a.jar" }, {}, std::nullopt, std::nullopt, g_hashA, PlacementKind::Hard, fileIdOf(path("files/a"))
            };
            // the crash came after the swap and before COMMIT
            QVERIFY(store.commit({ RefRecord::owner("client:i", instance()), RefRecord::publish(g_hashA, 8, generationFor(path("files/a"))),
                                   RefRecord::begin(transaction) }));
        }
        ContentStore reopened(store(), path("data"));
        QCOMPARE(reopened.open(), ContentStore::State::Writable);
        QVERIFY(reopened.table().transactions().isEmpty());
        QCOMPARE(reopened.table().ref({ "client:i", "mods/a.jar" })->hash, g_hashA);
    }

    void test_storeWithNewerJournalIsReadOnly()
    {
        {
            ContentStore store(this->store(), path("data"));
            QCOMPARE(store.open(), ContentStore::State::Writable);
        }
        // a newer launcher compacted its state into the journal; format.json alone doesn't say so
        Journal journal(store());
        QVERIFY(journal.load());
        StoreFormat newer;
        newer.minWriterVersion = StoreFormat::CurrentVersion + 1;
        QVERIFY(journal.compact(RefTable().snapshot(), newer));

        ContentStore store(this->store(), path("data"));
        QCOMPARE(store.open(), ContentStore::State::ReadOnly);
        QVERIFY(!store.commit({ RefRecord::owner("client:i", instance()) }));
    }
};

QTEST_GUILESS_MAIN(JournalTest)

#include "Journal_test.moc"
