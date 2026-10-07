#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include "FileSystem.h"
#include "FileSystemPrimitives.h"
#include "contentstore/ContentStore.h"

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

QString sha256Of(const QByteArray& data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
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

using Step = ContentStore::PlacementStep;
using Operation = FS::Testing::Operation;
}  // namespace

// The backup protocol: a user's file replaced by a link is kept aside until it is proven that nothing wrote to it
class BackupTest : public QObject {
    Q_OBJECT

    // in the working directory, so hard links don't cross a tmpfs boundary
    QTemporaryDir m_dir{ QDir::current().filePath("backup_test_XXXXXX") };
    std::unique_ptr<ContentStore> m_store;

    QString path(const QString& name) const { return m_dir.filePath(name); }
    QString userFile() const { return path("a/mods/mod.jar"); }

    ContentStore::Destination destination() const { return { m_store->instanceOwner("a"), path("a"), "mods/mod.jar" }; }

    // A user's file identical to a stored one, so converting it replaces it by a link: the backup protocol
    QString prepare(const QByteArray& data = "common mod")
    {
        writeFile(path("downloads/source"), data);
        const auto stored = m_store->ingest(path("downloads/source"), ContentStore::IngestMode::Copy);
        writeFile(userFile(), data);
        return stored ? stored->hash : QString();
    }

    QStringList leftovers() const { return QDir(path("a/mods")).entryList({ ".prism-*" }, QDir::AllEntries | QDir::Hidden | QDir::System); }

    void reopen()
    {
        FS::Testing::setFaultHook(nullptr);
        FS::Testing::setCallRecorder(nullptr);
        m_store.reset();
        m_store = std::make_unique<ContentStore>(path("store"), path("data"));
        QCOMPARE(m_store->open(), ContentStore::State::Writable);
    }

    // The single backup an interrupted replacement made, if any
    QString backupPath() const
    {
        const auto files = QDir(path("a/mods")).entryList({ ".prism-bak-*" }, QDir::AllEntries | QDir::Hidden | QDir::System);
        return files.isEmpty() ? QString() : QDir(path("a/mods")).filePath(files.first());
    }

    // Replaces the user's file with the backup protocol directly, without the conversion's own pin, so another program
    // can write to the file once it is swapped
    Result<PlacementKind> replace(const QString& hash)
    {
        const auto identity = FS::identity(userFile());
        if (!identity) {
            return std::unexpected(identity.error());
        }
        return m_store->placeAt({ destination(), hash, *identity, hash });
    }

    // Stops the conversion at the step, after doing what a test wants there
    void stopAt(Step stop, const std::function<void()>& before = {})
    {
        m_store->setInterruptionForTesting([stop, before](Step step) {
            if (step != stop) {
                return false;
            }
            if (before) {
                before();
            }
            return true;
        });
    }

    // Makes hard links into backups fail, so the old file is renamed aside instead
    static void noBackupLinks()
    {
        FS::Testing::setFaultHook([](Operation operation, const QString& link) {
            return operation == Operation::HardLink && QFileInfo(link).fileName().startsWith(".prism-bak-");
        });
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
        FS::Testing::setFaultHook(nullptr);
        FS::Testing::setCallRecorder(nullptr);
        if (m_store) {
            m_store->setInterruptionForTesting(nullptr);
        }
    }

    void test_replacementCommitsAndReleasesTheBackup()
    {
        const auto hash = prepare();
        const auto converted = m_store->convert(destination());
        QVERIFY2(converted, converted ? "" : qPrintable(converted.error()));
        QCOMPARE(converted->outcome, ContentStore::ConvertOutcome::Shared);
        QVERIFY(sameFile(userFile(), m_store->objectPath(hash)));
        QVERIFY(leftovers().isEmpty());
        QVERIFY(m_store->table().pendingBackups().isEmpty());
        QVERIFY(m_store->table().transactions().isEmpty());
    }

    void test_intentIsDurableBeforeTheBackup()
    {
        prepare();
        QStringList calls;
        const auto mods = QFileInfo(path("a/mods")).absoluteFilePath();
        FS::Testing::setCallRecorder([&calls, &mods](Operation operation, const QString& target) {
            const QFileInfo info(target);
            if (operation == Operation::FlushFile && info.fileName().startsWith("refs.journal")) {
                calls.append("journal");
            } else if (operation == Operation::HardLink && info.fileName().startsWith(".prism-bak-")) {
                calls.append("backup");
            } else if (operation == Operation::Replace && info.absolutePath() == mods && info.fileName() == "mod.jar") {
                calls.append("swap");
            } else if (operation == Operation::Delete && info.fileName().startsWith(".prism-bak-")) {
                calls.append("release");
            }
        });
        QVERIFY(m_store->convert(destination()));
        FS::Testing::setCallRecorder(nullptr);
        const auto backup = calls.indexOf("backup");
        QVERIFY(backup > 0);
        // the intent is journaled before the backup is made, and the commit before the backup goes
        QCOMPARE(calls[backup - 1], "journal");
        QVERIFY(calls.indexOf("swap") > backup);
        const auto release = calls.indexOf("release");
        QVERIFY(release > calls.indexOf("swap"));
        QCOMPARE(calls[release - 1], "journal");
    }

    void test_writeDuringTheSwapIsKept()
    {
        const auto hash = prepare("common mod");
        const auto before = FS::fileId(userFile());
        // another program writes to the user's file right after the swap, through the file itself
        m_store->setInterruptionForTesting([this](Step step) {
            if (step == Step::Swapped) {
                appendFile(backupPath(), " and a late write");
            }
            return false;
        });
        QVERIFY(!replace(hash));
        m_store->setInterruptionForTesting(nullptr);
        // the user's own file is back, with the write
        QCOMPARE(FS::fileId(userFile()), before);
        QCOMPARE(readFile(userFile()), "common mod and a late write");
        QVERIFY(!m_store->table().ref(destination().key()));
        QVERIFY(m_store->table().transactions().isEmpty());
        QVERIFY(leftovers().isEmpty());
    }

    void test_renamedBackup()
    {
        const auto hash = prepare();
        noBackupLinks();
        const auto converted = m_store->convert(destination());
        QVERIFY2(converted, converted ? "" : qPrintable(converted.error()));
        QVERIFY(sameFile(userFile(), m_store->objectPath(hash)));
        QVERIFY(leftovers().isEmpty());
    }

    void test_failedSwapPutsTheRenamedFileBack()
    {
        prepare();
        const auto before = FS::fileId(userFile());
        const auto target = QFileInfo(userFile()).absoluteFilePath();
        bool failed = false;
        // no backup links, and the swap fails once
        FS::Testing::setFaultHook([&](Operation operation, const QString& link) {
            if (operation == Operation::HardLink && QFileInfo(link).fileName().startsWith(".prism-bak-")) {
                return true;
            }
            if (!failed && operation == Operation::Replace && QFileInfo(link).absoluteFilePath() == target) {
                failed = true;
                return true;
            }
            return false;
        });
        QVERIFY(!m_store->convert(destination()));
        QVERIFY(failed);
        QCOMPARE(FS::fileId(userFile()), before);
        QCOMPARE(readFile(userFile()), "common mod");
        QVERIFY(leftovers().isEmpty());
        QVERIFY(m_store->table().transactions().isEmpty());
    }

    void test_failureAfterTheSwapPutsTheFileBack()
    {
        prepare();
        const auto before = FS::fileId(userFile());
        const auto mods = QFileInfo(path("a/mods")).absoluteFilePath();
        int flushes = 0;
        // the flush after the backup works, the one after the swap fails
        FS::Testing::setFaultHook([&](Operation operation, const QString& dir) {
            return operation == Operation::FlushDir && QFileInfo(dir).absoluteFilePath() == mods && ++flushes == 2;
        });
        QVERIFY(!m_store->convert(destination()));
        QCOMPARE(FS::fileId(userFile()), before);
        QCOMPARE(readFile(userFile()), "common mod");
        QVERIFY(leftovers().isEmpty());
        QVERIFY(m_store->table().transactions().isEmpty());
    }

    // Crashes, then the store opens again

    void test_crashAfterTheIntent()
    {
        prepare();
        const auto before = FS::fileId(userFile());
        stopAt(Step::BackupIntent);
        QVERIFY(!m_store->convert(destination()));
        reopen();
        QCOMPARE(FS::fileId(userFile()), before);
        QVERIFY(leftovers().isEmpty());
        QVERIFY(m_store->table().transactions().isEmpty());
    }

    void test_crashAfterTheBackupLink()
    {
        prepare();
        const auto before = FS::fileId(userFile());
        stopAt(Step::BackedUp);
        QVERIFY(!m_store->convert(destination()));
        QVERIFY(!backupPath().isEmpty());
        reopen();
        QCOMPARE(FS::fileId(userFile()), before);
        QVERIFY(leftovers().isEmpty());
        QVERIFY(m_store->table().transactions().isEmpty());
    }

    void test_crashAfterRenamingAside()
    {
        prepare();
        const auto before = FS::fileId(userFile());
        noBackupLinks();
        stopAt(Step::BackedUp);
        QVERIFY(!m_store->convert(destination()));
        // the user's file is only at the backup path now
        QVERIFY(!QFileInfo::exists(userFile()));
        reopen();
        QCOMPARE(FS::fileId(userFile()), before);
        QCOMPARE(readFile(userFile()), "common mod");
        QVERIFY(leftovers().isEmpty());
    }

    void test_crashAfterTheSwap()
    {
        const auto hash = prepare();
        stopAt(Step::Swapped);
        QVERIFY(!m_store->convert(destination()));
        reopen();
        // the backup is intact, so the replacement completes
        QVERIFY(sameFile(userFile(), m_store->objectPath(hash)));
        QCOMPARE(m_store->table().ref(destination().key())->hash, hash);
        QVERIFY(leftovers().isEmpty());
        QVERIFY(m_store->table().pendingBackups().isEmpty());
    }

    void test_writeWhileTheLauncherWasDown()
    {
        prepare();
        const auto before = FS::fileId(userFile());
        stopAt(Step::Swapped);
        QVERIFY(!m_store->convert(destination()));
        // written through the backup, which is the user's file, before the launcher starts again
        QVERIFY(appendFile(backupPath(), " and an edit"));
        reopen();
        QCOMPARE(FS::fileId(userFile()), before);
        QCOMPARE(readFile(userFile()), "common mod and an edit");
        QVERIFY(!m_store->table().ref(destination().key()));
        QVERIFY(leftovers().isEmpty());
    }

    void test_crashWhileAborting()
    {
        const auto hash = prepare();
        const auto before = FS::fileId(userFile());
        // the write is noticed, then the launcher stops before putting the file back
        m_store->setInterruptionForTesting([this](Step step) {
            if (step == Step::Swapped) {
                appendFile(backupPath(), " and a late write");
            }
            return step == Step::Aborting;
        });
        QVERIFY(!replace(hash));
        reopen();
        QCOMPARE(FS::fileId(userFile()), before);
        QCOMPARE(readFile(userFile()), "common mod and a late write");
        QVERIFY(leftovers().isEmpty());
    }

    void test_crashAfterTheCommit()
    {
        const auto hash = prepare();
        stopAt(Step::Committed);
        QVERIFY(!m_store->convert(destination()));
        QCOMPARE(m_store->table().pendingBackups().size(), 1);
        // a backup that wasn't removed keeps the store at format version 2
        QCOMPARE(m_store->format().accessFor(1), StoreAccess::ReadOnly);
        reopen();
        QVERIFY(m_store->table().pendingBackups().isEmpty());
        QVERIFY(leftovers().isEmpty());
        QVERIFY(sameFile(userFile(), m_store->objectPath(hash)));
        QCOMPARE(m_store->format().accessFor(1), StoreAccess::Writable);
    }

    void test_noFalseRecovery()
    {
        // without another writer, the launcher's own metadata changes never make a replacement roll back
        writeFile(path("downloads/source"), "common mod");
        QVERIFY(m_store->ingest(path("downloads/source"), ContentStore::IngestMode::Copy));
        for (int i = 0; i < 30; i++) {
            const auto name = QString("mods/mod-%1.jar").arg(i);
            QVERIFY(writeFile(path("a/" + name), "common mod"));
            const auto converted = m_store->convert({ m_store->instanceOwner("a"), path("a"), name });
            QVERIFY2(converted, converted ? "" : qPrintable(converted.error()));
            QCOMPARE(converted->outcome, ContentStore::ConvertOutcome::Shared);
        }
        QVERIFY(leftovers().isEmpty());
        QCOMPARE(m_store->table().refs().size(), 30);
    }
};

QTEST_GUILESS_MAIN(BackupTest)

#include "Backup_test.moc"
