#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include "FileSystem.h"
#include "FileSystemPrimitives.h"
#include "contentstore/ContentStore.h"
#include "contentstore/WriterGuard.h"

namespace {
bool writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

bool appendFile(const QString& path, const QByteArray& data)
{
    QFile file(path);
    return file.open(QIODevice::Append) && file.write(data) == data.size();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void makeWritable(const QString& path)
{
    QFile::setPermissions(path,
                          QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser | QFile::ExeOwner | QFile::ExeUser);
}

bool sameFile(const QString& first, const QString& second)
{
    const auto a = FS::fileId(first);
    const auto b = FS::fileId(second);
    return a && b && *a == *b;
}

// Another program that keeps the file open for writing until it is killed
std::unique_ptr<QProcess> holdOpen(const QString& path)
{
    auto process = std::make_unique<QProcess>();
    process->setStandardOutputFile(path, QIODevice::Append);
#if defined(Q_OS_WIN)
    // prints nothing until it times out, and has no child process that would keep the file open after it is killed
    process->start("waitfor", { "/t", "60", "PrismHoldOpenTest" });
#else
    process->start("sleep", { "30" });
#endif
    process->waitForStarted();
    return process;
}

bool finished(QProcess& process, int timeoutMs = 10000)
{
    return process.state() == QProcess::NotRunning || process.waitForFinished(timeoutMs) || process.state() == QProcess::NotRunning;
}

using Step = ContentStore::PlacementStep;
using Operation = FS::Testing::Operation;
}  // namespace

// Pending validations: the backup of a replaced user's file is kept until it is shown unchanged, and put back otherwise
class ValidationTest : public QObject {
    Q_OBJECT

    // in the working directory, so hard links don't cross a tmpfs boundary
    QTemporaryDir m_dir{ QDir::current().filePath("validation_test_XXXXXX") };
    std::unique_ptr<ContentStore> m_store;

    QString path(const QString& name) const { return m_dir.filePath(name); }
    QString userFile() const { return path("a/mods/mod.jar"); }
    ContentStore::Destination destination() const { return { m_store->instanceOwner("a"), path("a"), "mods/mod.jar" }; }

    QString backupPath() const
    {
        const auto files = QDir(path("a/mods")).entryList({ ".prism-bak-*" }, QDir::AllEntries | QDir::Hidden | QDir::System);
        return files.isEmpty() ? QString() : QDir(path("a/mods")).filePath(files.first());
    }

    QStringList leftovers() const { return QDir(path("a/mods")).entryList({ ".prism-*" }, QDir::AllEntries | QDir::Hidden | QDir::System); }

    QStringList recovered() const
    {
        QStringList files;
        QDirIterator it(path("a/.prism-recovered"), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            files.append(it.next());
        }
        return files;
    }

    void reopen(std::optional<Step> stop = std::nullopt)
    {
        m_store.reset();
        m_store = std::make_unique<ContentStore>(path("store"), path("data"));
        if (stop) {
            m_store->setInterruptionForTesting([stop](Step step) { return step == *stop; });
        }
        QCOMPARE(m_store->open(), ContentStore::State::Writable);
        m_store->setInterruptionForTesting(nullptr);
    }

    // Replaces a user's file identical to a stored one, and stops right after the commit, so its backup awaits a
    // validation
    QString replaceAndStop()
    {
        writeFile(path("downloads/source"), "common mod");
        const auto stored = m_store->ingest(path("downloads/source"), ContentStore::IngestMode::Copy);
        writeFile(userFile(), "common mod");
        const auto identity = FS::identity(userFile());
        if (!stored || !identity) {
            return {};
        }
        m_store->setInterruptionForTesting([](Step step) { return step == Step::Committed; });
        // stopped as if the launcher crashed right after the commit
        if (m_store->placeAt({ destination(), stored->hash, *identity, stored->hash })) {
            return {};
        }
        m_store->setInterruptionForTesting(nullptr);
        return stored->hash;
    }

   private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        // validations are what these tests are about, on every platform
        WriterGuard::setBackupsNeedValidationForTesting(true);
    }

    void cleanup()
    {
        FS::Testing::setFaultHook(nullptr);
        WriterGuard::setUnenforcedForTesting(false);
        if (m_store) {
            m_store->setInterruptionForTesting(nullptr);
        }
    }

    void init()
    {
        m_store.reset();
        QDirIterator it(m_dir.path(), QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            makeWritable(it.next());
        }
        QVERIFY(FS::deleteContents(m_dir.path()));
        for (const auto* dir : { "data", "downloads", "a/mods" }) {
            QVERIFY(QDir(m_dir.path()).mkpath(dir));
        }
        m_store = std::make_unique<ContentStore>(path("store"), path("data"));
        QCOMPARE(m_store->open(), ContentStore::State::Writable);
    }

    void test_validatedAtTheEndOfTheBatch()
    {
        writeFile(path("downloads/source"), "common mod");
        const auto stored = m_store->ingest(path("downloads/source"), ContentStore::IngestMode::Copy);
        QVERIFY(stored);
        writeFile(userFile(), "common mod");
        const auto identity = FS::identity(userFile());
        QVERIFY(identity);
        QVERIFY(m_store->placeAt({ destination(), stored->hash, *identity, stored->hash }));
        // nothing had it open, so it was validated: released, or renamed aside where no pin protects it and released
        // by the next validation
        QCOMPARE(*m_store->validatePendingBackups(), 0);
        QVERIFY(m_store->table().pendingBackups().isEmpty());
        QVERIFY(leftovers().isEmpty());
        QVERIFY(sameFile(userFile(), m_store->objectPath(stored->hash)));
    }

    void test_unchangedBackupIsReleasedAtStartup()
    {
        const auto hash = replaceAndStop();
        QCOMPARE(m_store->table().pendingBackups().size(), 1);
        reopen();
        QCOMPARE(*m_store->validatePendingBackups(), 0);
        QVERIFY(m_store->table().pendingBackups().isEmpty());
        QVERIFY(leftovers().isEmpty());
        QVERIFY(sameFile(userFile(), m_store->objectPath(hash)));
        QCOMPARE(m_store->table().ref(destination().key())->hash, hash);
        QVERIFY(m_store->takeRestoredFiles().isEmpty());
    }

    void test_changedBackupIsRestored()
    {
        const auto hash = replaceAndStop();
        const auto backup = backupPath();
        const auto before = FS::fileId(backup);
        // written to while the launcher was closed
        QVERIFY(appendFile(backup, " and an edit"));
        reopen();
        // the user's own file is back, with the edit, and the instance no longer uses the stored file
        QCOMPARE(FS::fileId(userFile()), before);
        QCOMPARE(readFile(userFile()), "common mod and an edit");
        QVERIFY(!m_store->table().ref(destination().key()));
        QVERIFY(m_store->table().pendingBackups().isEmpty());
        // nothing uses the stored file anymore: it is orphaned, or already gone
        QVERIFY(!m_store->table().entries().contains(hash) || m_store->table().entries()[hash].orphanSince);
        QVERIFY(leftovers().isEmpty());
        const auto restored = m_store->takeRestoredFiles();
        QCOMPARE(restored.size(), 1);
        QCOMPARE(QFileInfo(restored.first().path), QFileInfo(userFile()));
        QVERIFY(restored.first().recoveredPath.isEmpty());
    }

    void test_restoreSurvivesCrashes_data()
    {
        QTest::addColumn<Step>("crash");
        QTest::newRow("after RESTORE_BEGIN") << Step::RestoreBegun;
        QTest::newRow("after putting it back") << Step::Restored;
    }

    void test_restoreSurvivesCrashes()
    {
        QFETCH(Step, crash);
        replaceAndStop();
        const auto before = FS::fileId(backupPath());
        QVERIFY(appendFile(backupPath(), " and an edit"));
        reopen(crash);
        QCOMPARE(m_store->table().pendingBackups().size(), 1);
        QVERIFY(m_store->table().pendingBackups().first().restoring);

        reopen();
        QCOMPARE(FS::fileId(userFile()), before);
        QCOMPARE(readFile(userFile()), "common mod and an edit");
        QVERIFY(!m_store->table().ref(destination().key()));
        QVERIFY(m_store->table().pendingBackups().isEmpty());
        QVERIFY(leftovers().isEmpty());
    }

    void test_conflictKeepsBoth_data()
    {
        QTest::addColumn<QString>("change");
        QTest::newRow("keep local copy") << "unshare";
        QTest::newRow("deleted") << "delete";
        QTest::newRow("updated") << "update";
    }

    void test_conflictKeepsBoth()
    {
        QFETCH(QString, change);
        replaceAndStop();
        const auto backup = backupPath();
        const auto id = QFileInfo(backup).fileName().section('-', -1);
        QVERIFY(appendFile(backup, " and an edit"));
        // before the validation runs, something newer is put at the path
        if (change == "unshare") {
            QVERIFY(m_store->unshare(destination().key()));
        } else if (change == "delete") {
            QVERIFY(FS::deleteLink(userFile()));
        } else {
            writeFile(path("downloads/update"), "updated mod");
            const auto update = m_store->ingest(path("downloads/update"), ContentStore::IngestMode::Copy);
            QVERIFY(update);
            QVERIFY(m_store->placeAt({ destination(), update->hash }));
        }
        const auto atPath = readFile(userFile());
        const bool existed = QFileInfo::exists(userFile());
        // the backup wasn't quiet, or was just found changed by the placement: either way it isn't lost
        reopen();
        QCOMPARE(QFileInfo::exists(userFile()), existed);
        QCOMPARE(readFile(userFile()), atPath);
        const auto files = recovered();
        QCOMPARE(files.size(), 1);
        QCOMPARE(QFileInfo(files.first()).fileName(), "mod.jar." + id);
        QCOMPARE(readFile(files.first()), "common mod and an edit");
        QVERIFY(m_store->table().pendingBackups().isEmpty());
        QVERIFY(leftovers().isEmpty());
    }

    void test_conflictSurvivesCrashes_data()
    {
        QTest::addColumn<Step>("crash");
        QTest::newRow("after RESTORE_CONFLICT_BEGIN") << Step::ConflictBegun;
        QTest::newRow("after moving it") << Step::ConflictMoved;
    }

    void test_conflictSurvivesCrashes()
    {
        QFETCH(Step, crash);
        replaceAndStop();
        QVERIFY(appendFile(backupPath(), " and an edit"));
        QVERIFY(FS::deleteLink(userFile()));
        reopen(crash);
        QCOMPARE(m_store->table().pendingBackups().size(), 1);
        QVERIFY(!m_store->table().pendingBackups().first().recoveredPath.isEmpty());

        reopen();
        const auto files = recovered();
        QCOMPARE(files.size(), 1);
        QCOMPARE(readFile(files.first()), "common mod and an edit");
        QVERIFY(m_store->table().pendingBackups().isEmpty());
        QVERIFY(leftovers().isEmpty());
    }

    void test_busyBackupWaits()
    {
        const auto hash = replaceAndStop();
        const auto backup = backupPath();
        // another program has the backup open, so it can't be validated yet
        auto holder = holdOpen(backup);
        QVERIFY(holder->state() == QProcess::Running);
        reopen();
        QCOMPARE(m_store->table().pendingBackups().size(), 1);
        // nor by the next placement
        writeFile(path("downloads/other"), "other mod");
        const auto other = m_store->ingest(path("downloads/other"), ContentStore::IngestMode::Copy);
        QVERIFY(other);
        QVERIFY(m_store->placeAt({ { m_store->instanceOwner("a"), path("a"), "mods/other.jar" }, other->hash }));
        QCOMPARE(m_store->table().pendingBackups().size(), 1);
        QVERIFY(QFileInfo::exists(backup));
        QCOMPARE(*m_store->validatePendingBackups(), 1);

        holder->kill();
        QVERIFY(finished(*holder));
        m_store->validatePendingBackups();
        QCOMPARE(*m_store->validatePendingBackups(), 0);
        QVERIFY(m_store->table().pendingBackups().isEmpty());
        QVERIFY(!QFileInfo::exists(backup));
        QVERIFY(sameFile(userFile(), m_store->objectPath(hash)));
    }

    void test_writeAfterTheCheckIsKept()
    {
        // a guard no pin backs, as on Linux and macOS
        WriterGuard::setUnenforcedForTesting(true);
        replaceAndStop();
        const auto before = FS::fileId(backupPath());
        reopen();
        // validated, and renamed aside instead of removed
        QCOMPARE(m_store->table().pendingBackups().size(), 1);
        const auto pending = m_store->table().pendingBackups().first();
        QVERIFY(!pending.trashedFrom.isEmpty());
        QVERIFY(!QFileInfo::exists(pending.trashedFrom));
        QCOMPARE(FS::fileId(pending.backupPath), before);
        // a program that had looked the backup up before writes to it now: the file still has a name
        QVERIFY(appendFile(pending.backupPath, " and an edit"));
        QCOMPARE(*m_store->validatePendingBackups(), 0);
        QCOMPARE(FS::fileId(userFile()), before);
        QCOMPARE(readFile(userFile()), "common mod and an edit");
        QVERIFY(!m_store->table().ref(destination().key()));
        QVERIFY(leftovers().isEmpty());
    }

    void test_renameAsideSurvivesACrash()
    {
        WriterGuard::setUnenforcedForTesting(true);
        const auto hash = replaceAndStop();
        // the rename aside is recorded, then fails as if the launcher crashed before it
        FS::Testing::setFaultHook([](Operation operation, const QString& target) {
            return operation == Operation::Replace && QFileInfo(target).fileName().startsWith(".prism-del-");
        });
        QCOMPARE(*m_store->validatePendingBackups(), 1);
        const auto pending = m_store->table().pendingBackups().first();
        QVERIFY(!QFileInfo::exists(pending.backupPath));
        QVERIFY(QFileInfo::exists(pending.trashedFrom));
        FS::Testing::setFaultHook(nullptr);

        reopen();
        // finished, then removed
        QCOMPARE(*m_store->validatePendingBackups(), 0);
        QVERIFY(m_store->table().pendingBackups().isEmpty());
        QVERIFY(leftovers().isEmpty());
        QVERIFY(sameFile(userFile(), m_store->objectPath(hash)));
    }

    void test_conflictMoveMustBeDurable()
    {
        replaceAndStop();
        QVERIFY(appendFile(backupPath(), " and an edit"));
        QVERIFY(FS::deleteLink(userFile()));
        // the backup is moved aside, but the recovered folder can't be flushed
        FS::Testing::setFaultHook(
            [](Operation operation, const QString& dir) { return operation == Operation::FlushDir && dir.contains(".prism-recovered"); });
        reopen();
        QCOMPARE(m_store->table().pendingBackups().size(), 1);
        QCOMPARE(recovered().size(), 1);
        // retried, and still not durable: the validation stays open
        QCOMPARE(*m_store->validatePendingBackups(), 1);
        QCOMPARE(m_store->table().pendingBackups().size(), 1);

        FS::Testing::setFaultHook(nullptr);
        QCOMPARE(*m_store->validatePendingBackups(), 0);
        QVERIFY(m_store->table().pendingBackups().isEmpty());
        QCOMPARE(readFile(recovered().first()), "common mod and an edit");
    }

    void test_openedAsItIsRemoved()
    {
#if !defined(Q_OS_LINUX)
        QSKIP("Only a Linux lease keeps hold of a removed file");
#else
        if (WriterGuard::tierFor(userFile()) != WriterGuard::Tier::Enforced) {
            QSKIP("This file system doesn't offer leases");
        }
        replaceAndStop();
        reopen();
        // renamed aside by the first validation
        const auto trash = m_store->table().pendingBackups().first().backupPath;
        QVERIFY(QFileInfo(trash).fileName().startsWith(".prism-del-"));
        // as the second removes it, a program opens it, and only writes a while later
        std::unique_ptr<QProcess> writer;
        m_store->setInterruptionForTesting([&](Step step) {
            if (step == Step::Removing && !writer) {
                writer = std::make_unique<QProcess>();
                writer->start("sh", { "-c", QString("exec 3>>'%1'; sleep 1; printf late >&3").arg(trash) });
                writer->waitForStarted();
                QTest::qWait(300);
            }
            return false;
        });
        QCOMPARE(*m_store->validatePendingBackups(), 1);
        m_store->setInterruptionForTesting(nullptr);
        QVERIFY(writer);
        // gone, but still pending, as the program may still write
        QVERIFY(!QFileInfo::exists(trash));
        QCOMPARE(m_store->table().pendingBackups().size(), 1);

        QVERIFY(finished(*writer));
        QTRY_COMPARE_WITH_TIMEOUT(*m_store->validatePendingBackups(), 0, 10000);
        const auto files = recovered();
        QCOMPARE(files.size(), 1);
        QCOMPARE(readFile(files.first()), "common modlate");
        const auto restored = m_store->takeRestoredFiles();
        QCOMPARE(restored.size(), 1);
        QCOMPARE(restored.first().recoveredPath, files.first());
#endif
    }

    void test_salvageSurvivesClosing()
    {
#if !defined(Q_OS_LINUX)
        QSKIP("Only a Linux lease keeps hold of a removed file");
#else
        if (WriterGuard::tierFor(userFile()) != WriterGuard::Tier::Enforced) {
            QSKIP("This file system doesn't offer leases");
        }
        replaceAndStop();
        reopen();
        const auto trash = m_store->table().pendingBackups().first().backupPath;
        // a program opens it as it is removed, and only writes after the launcher closed
        std::unique_ptr<QProcess> writer;
        m_store->setInterruptionForTesting([&](Step step) {
            if (step == Step::Removing && !writer) {
                writer = std::make_unique<QProcess>();
                writer->start("sh", { "-c", QString("exec 3>>'%1'; sleep 7; printf late >&3").arg(trash) });
                writer->waitForStarted();
                QTest::qWait(300);
            }
            return false;
        });
        QCOMPARE(*m_store->validatePendingBackups(), 1);
        m_store->setInterruptionForTesting(nullptr);
        QVERIFY(!m_store->table().pendingBackups().first().salvagePath.isEmpty());

        // closed and opened again while the program still has it open: the helper keeps it, and it stays pending
        reopen();
        QCOMPARE(m_store->table().pendingBackups().size(), 1);
        QVERIFY(finished(*writer, 20000));
        QTRY_COMPARE_WITH_TIMEOUT(*m_store->validatePendingBackups(), 0, 10000);
        const auto files = recovered();
        QCOMPARE(files.size(), 1);
        QCOMPARE(readFile(files.first()), "common modlate");
        const auto restored = m_store->takeRestoredFiles();
        QCOMPARE(restored.size(), 1);
        QCOMPARE(restored.first().recoveredPath, files.first());
        QVERIFY(!restored.first().incomplete);
#endif
    }

    void test_pendingSurvivesCompaction()
    {
        replaceAndStop();
        const auto before = FS::fileId(backupPath());
        // kept from the validation, then compacted into the snapshot
        auto holder = holdOpen(backupPath());
        reopen();
        QVERIFY(m_store->compact());
        QCOMPARE(m_store->table().pendingBackups().size(), 1);
        const auto pending = m_store->table().pendingBackups().first();
        QVERIFY(pending.awaitValidation);
        QVERIFY(pending.transaction.oldIdentity);
        QVERIFY(pending.newRef);
        holder->kill();
        holder->waitForFinished();

        QVERIFY(appendFile(backupPath(), " and an edit"));
        reopen();
        QCOMPARE(FS::fileId(userFile()), before);
        QCOMPARE(readFile(userFile()), "common mod and an edit");
        QVERIFY(m_store->table().pendingBackups().isEmpty());
    }
};

QTEST_GUILESS_MAIN(ValidationTest)

#include "Validation_test.moc"
