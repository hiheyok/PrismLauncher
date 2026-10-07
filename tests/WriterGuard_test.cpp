#include <QCryptographicHash>
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
    process->start("cmd", { "/c", "ping -n 30 127.0.0.1" });
#else
    process->start("sleep", { "30" });
#endif
    process->waitForStarted();
    return process;
}

// Another program that appends to the file, waiting as long as the system makes it wait to open it
std::unique_ptr<QProcess> appendTo(const QString& path, const QString& text)
{
    auto process = std::make_unique<QProcess>();
#if defined(Q_OS_WIN)
    process->start("cmd", { "/c", QString("echo %1>>\"%2\"").arg(text, QDir::toNativeSeparators(path)) });
#else
    process->start("sh", { "-c", QString("printf %1 >> '%2'").arg(text, path) });
#endif
    process->waitForStarted();
    return process;
}

using Step = ContentStore::PlacementStep;
}  // namespace

class WriterGuardTest : public QObject {
    Q_OBJECT

    // in the working directory, so hard links don't cross a tmpfs boundary
    QTemporaryDir m_dir{ QDir::current().filePath("writer_guard_test_XXXXXX") };
    std::unique_ptr<ContentStore> m_store;

    QString path(const QString& name) const { return m_dir.filePath(name); }
    QString userFile() const { return path("a/mods/mod.jar"); }
    ContentStore::Destination destination() const { return { m_store->instanceOwner("a"), path("a"), "mods/mod.jar" }; }

    QString backupPath() const
    {
        const auto files = QDir(path("a/mods")).entryList({ ".prism-bak-*" }, QDir::AllEntries | QDir::Hidden | QDir::System);
        return files.isEmpty() ? QString() : QDir(path("a/mods")).filePath(files.first());
    }

    // A user's file identical to a stored one, so sharing it replaces it with the backup protocol
    QString prepare()
    {
        writeFile(path("downloads/source"), "common mod");
        const auto stored = m_store->ingest(path("downloads/source"), ContentStore::IngestMode::Copy);
        writeFile(userFile(), "common mod");
        return stored ? stored->hash : QString();
    }

   private slots:
    void initTestCase() { QVERIFY(m_dir.isValid()); }

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

    void cleanup()
    {
        WriterGuard::setBackupsNeedValidationForTesting(std::nullopt);
        if (m_store) {
            m_store->setInterruptionForTesting(nullptr);
        }
    }

    void test_localDriveIsNotANetworkDrive() { QVERIFY(!FS::isNetworkVolume(path("a/mods"))); }

    void test_tierOfThisSystem()
    {
        const auto tier = WriterGuard::tierFor(userFile());
#if defined(Q_OS_WIN)
        QCOMPARE(tier, WriterGuard::Tier::Enforced);
#elif defined(Q_OS_MACOS)
        QCOMPARE(tier, WriterGuard::Tier::BestEffort);
#else
        // enforced with file leases, which the file system and the system settings decide
        qInfo() << "Linux tier:" << (tier == WriterGuard::Tier::Enforced ? "leases" : "best effort");
#endif
        // the probe leaves nothing behind
        QVERIFY(QDir(path("a/mods")).entryList({ ".prism-*" }, QDir::AllEntries | QDir::Hidden).isEmpty());
    }

    void test_guardReadsTheFile()
    {
        QVERIFY(writeFile(userFile(), "common mod"));
        auto guard = WriterGuard::acquire(userFile());
        QVERIFY2(guard, guard ? "" : qPrintable(guard.error()));
        const auto digest = guard->sha256(userFile());
        QVERIFY(digest);
        QCOMPARE(*digest, QString::fromLatin1(QCryptographicHash::hash("common mod", QCryptographicHash::Sha256).toHex()));
        // reading through the guard doesn't count as another program
        QVERIFY(!guard->disturbed(userFile()));
    }

    void test_fileOpenElsewhereIsLeftAlone()
    {
        const auto hash = prepare();
        auto holder = holdOpen(userFile());
        QVERIFY(holder->state() == QProcess::Running);
        const auto converted = m_store->convert(destination());
        holder->kill();
        holder->waitForFinished();
        // refused, whether by the pin, the lease or the look for programs that have it open
        QVERIFY(!converted || converted->outcome == ContentStore::ConvertOutcome::Skipped);
        QVERIFY(!sameFile(userFile(), m_store->objectPath(hash)));
        QVERIFY(m_store->table().transactions().isEmpty());
        QVERIFY(!QFileInfo::exists(backupPath()));
    }

    void test_openDuringTheReplacement()
    {
        const auto hash = prepare();
        std::unique_ptr<QProcess> writer;
        // right after the swap, another program opens the old file (now the backup) to append to it
        m_store->setInterruptionForTesting([&](Step step) {
            if (step == Step::Swapped && !writer) {
                writer = appendTo(backupPath(), "late");
#if !defined(Q_OS_WIN)
                // let it reach the open, where a lease makes it wait
                QTest::qWait(500);
#endif
            }
            return false;
        });
        const auto identity = FS::identity(userFile());
        QVERIFY(identity);
        const auto placed = m_store->placeAt({ destination(), hash, *identity, hash });
        m_store->setInterruptionForTesting(nullptr);
        QVERIFY(writer);
        writer->waitForFinished(10000);
#if defined(Q_OS_WIN)
        // pinned: the other program couldn't open the file, and the replacement went ahead
        QVERIFY2(placed, placed ? "" : qPrintable(placed.error()));
        QVERIFY(sameFile(userFile(), m_store->objectPath(hash)));
#else
        // noticed: the user's file was put back before the other program got it, so its write is in the user's file
        QVERIFY(!placed);
        QVERIFY(!sameFile(userFile(), m_store->objectPath(hash)));
        QCOMPARE(readFile(userFile()), "common modlate");
#endif
        QVERIFY(m_store->table().transactions().isEmpty());
        QVERIFY(!QFileInfo::exists(backupPath()));
    }

    void test_backupsAwaitValidationWithoutAPin()
    {
#if defined(Q_OS_WIN)
        QVERIFY(!WriterGuard::backupsNeedValidation());
#else
        QVERIFY(WriterGuard::backupsNeedValidation());
#endif
    }

    void test_lateWriterKeepsItsWrite()
    {
        WriterGuard::setBackupsNeedValidationForTesting(true);
        const auto hash = prepare();
        std::unique_ptr<QProcess> writer;
        QString backup;
        // the replacement was checked; then, while it is cleaned up, another program opens the old file, now the
        // backup. With a Linux lease it waits until the guard is released.
        m_store->setInterruptionForTesting([&](Step step) {
            if (step == Step::Committed && !writer) {
                backup = backupPath();
#if !defined(Q_OS_WIN)
                writer = appendTo(backup, "late");
                QTest::qWait(500);
#endif
            }
            return false;
        });
        const auto identity = FS::identity(userFile());
        QVERIFY(identity);
        QVERIFY(m_store->placeAt({ destination(), hash, *identity, hash }));
        m_store->setInterruptionForTesting(nullptr);
        if (writer) {
            QVERIFY(writer->waitForFinished(10000));
            // its write is in the backup, which still exists
            QCOMPARE(readFile(backup), "common modlate");
        }
        QVERIFY(sameFile(userFile(), m_store->objectPath(hash)));
        QCOMPARE(m_store->table().pendingBackups().size(), 1);
        QVERIFY(m_store->table().pendingBackups().first().awaitValidation);

        // neither the next operation nor a restart removes it
        writeFile(path("downloads/other"), "other mod");
        QVERIFY(m_store->ingest(path("downloads/other"), ContentStore::IngestMode::Copy));
        const auto other = m_store->ingest(path("downloads/other"), ContentStore::IngestMode::Copy);
        QVERIFY(other);
        QVERIFY(m_store->placeAt({ { m_store->instanceOwner("a"), path("a"), "mods/other.jar" }, other->hash }));
        QVERIFY(QFileInfo::exists(backup));
        m_store.reset();
        m_store = std::make_unique<ContentStore>(path("store"), path("data"));
        QCOMPARE(m_store->open(), ContentStore::State::Writable);
        QVERIFY(QFileInfo::exists(backup));
        QCOMPARE(m_store->table().pendingBackups().size(), 1);
        QVERIFY(m_store->table().pendingBackups().first().awaitValidation);
        // and it keeps the store at format version 2
        QCOMPARE(m_store->format().accessFor(1), StoreAccess::ReadOnly);
    }

    void test_pinnedBackupIsReleasedRightAway()
    {
        WriterGuard::setBackupsNeedValidationForTesting(false);
        const auto hash = prepare();
        const auto identity = FS::identity(userFile());
        QVERIFY(identity);
        QVERIFY(m_store->placeAt({ destination(), hash, *identity, hash }));
        QVERIFY(m_store->table().pendingBackups().isEmpty());
        QVERIFY(backupPath().isEmpty());
    }

    void test_networkDrivesNeedConfirmation()
    {
        // the check itself is covered on local files; a conversion that is told it is fine goes ahead
        prepare();
        ContentStore::ConvertOptions options;
        options.allowNetworkVolumes = true;
        const auto converted = m_store->convert(destination(), options);
        QVERIFY2(converted, converted ? "" : qPrintable(converted.error()));
        QCOMPARE(converted->outcome, ContentStore::ConvertOutcome::Shared);
    }
};

QTEST_GUILESS_MAIN(WriterGuardTest)

#include "WriterGuard_test.moc"
