#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include "FileSystem.h"
#include "FileSystemPrimitives.h"
#include "contentstore/ContentStore.h"
#include "contentstore/SharedContent.h"
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
}  // namespace

// "Share all content": the existing files of an instance are shared in place
class ShareInstanceTest : public QObject {
    Q_OBJECT

    // in the working directory, so hard links don't cross a tmpfs boundary
    QTemporaryDir m_dir{ QDir::current().filePath("share_instance_test_XXXXXX") };
    std::unique_ptr<ContentStore> m_store;

    QString path(const QString& name) const { return m_dir.filePath(name); }

    SharedContent::ShareReport share(const QString& instance,
                                     const ContentStore::ConvertOptions& options = {},
                                     const std::function<bool(const QString&)>& excluded = {},
                                     const std::function<bool(int, int)>& progress = {})
    {
        return SharedContent::shareInstance(*m_store, instance, path(instance), options, excluded, progress);
    }

   private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        // the backups of replaced files are released at once, as on Windows
        WriterGuard::setBackupsNeedValidationForTesting(false);
    }

    void init()
    {
        m_store.reset();
        QDirIterator it(m_dir.path(), QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            makeWritable(it.next());
        }
        QVERIFY(FS::deleteContents(m_dir.path()));
        QVERIFY(QDir(m_dir.path()).mkpath("data"));
        m_store = std::make_unique<ContentStore>(path("store"), path("data"));
        QCOMPARE(m_store->open(), ContentStore::State::Writable);
    }

    void test_twoInstancesShareTheirFiles()
    {
        for (const auto* instance : { "a", "b" }) {
            QVERIFY(writeFile(path(QString("%1/mods/common.jar").arg(instance)), "common mod"));
            QVERIFY(writeFile(path(QString("%1/resourcepacks/pack.zip").arg(instance)), "a resource pack"));
        }
        QVERIFY(writeFile(path("b/mods/own.jar"), "only in b"));

        const auto first = share("a");
        QCOMPARE(first.shared, 2);
        // its files became the stored files: nothing was stored before
        QCOMPARE(first.bytesSaved, 0);
        QVERIFY(first.skipped.isEmpty() && first.failed.isEmpty());

        const auto second = share("b");
        QCOMPARE(second.shared, 3);
        // the two files a stored already
        QCOMPARE(second.bytesSaved, qint64(QByteArray("common mod").size() + QByteArray("a resource pack").size()));
        QVERIFY(sameFile(path("a/mods/common.jar"), path("b/mods/common.jar")));
        QVERIFY(sameFile(path("a/resourcepacks/pack.zip"), path("b/resourcepacks/pack.zip")));
        QCOMPARE(readFile(path("b/mods/own.jar")), "only in b");
        QVERIFY(m_store->table().ref({ m_store->instanceOwner("b"), "mods/own.jar" }));

        // nothing left to do
        const auto again = share("b");
        QCOMPARE(again.shared, 0);
        QCOMPARE(again.alreadyShared, 3);
    }

    void test_filesKeptLocalStayLocal()
    {
        QVERIFY(writeFile(path("a/mods/kept.jar"), "kept"));
        QVERIFY(writeFile(path("a/mods/shared.jar"), "shared"));
        const auto before = FS::fileId(path("a/mods/kept.jar"));
        const auto report = share("a", {}, [](const QString& relativePath) { return relativePath == "mods/kept.jar"; });
        QCOMPARE(report.shared, 1);
        QCOMPARE(FS::fileId(path("a/mods/kept.jar")), before);
        QVERIFY(!m_store->table().ref({ m_store->instanceOwner("a"), "mods/kept.jar" }));
        QVERIFY(QFileInfo(path("a/mods/kept.jar")).isWritable());
    }

    void test_hardLinkedFilesNeedAdopting()
    {
        QVERIFY(writeFile(path("elsewhere/mod.jar"), "linked mod"));
        QVERIFY(QDir().mkpath(path("a/mods")));
        QVERIFY(FS::createHardLink(path("elsewhere/mod.jar"), path("a/mods/mod.jar")));
        const auto skipped = share("a");
        QCOMPARE(skipped.shared, 0);
        QCOMPARE(skipped.skipped.size(), 1);
        QCOMPARE(skipped.skipped.first().reason, ContentStore::ConvertSkip::HardLinked);
        // the other path keeps its file, writable
        QVERIFY(sameFile(path("elsewhere/mod.jar"), path("a/mods/mod.jar")));

        ContentStore::ConvertOptions adopt;
        adopt.adoptHardLinked = true;
        const auto adopted = share("a", adopt);
        QCOMPARE(adopted.shared, 1);
        // copied into the store: the instance's file no longer is the other path's
        QVERIFY(!sameFile(path("elsewhere/mod.jar"), path("a/mods/mod.jar")));
        QCOMPARE(readFile(path("a/mods/mod.jar")), "linked mod");
        QVERIFY(QFileInfo(path("elsewhere/mod.jar")).isWritable());
    }

    void test_stopping()
    {
        QVERIFY(writeFile(path("a/mods/one.jar"), "one"));
        QVERIFY(writeFile(path("a/mods/two.jar"), "two"));
        int calls = 0;
        const auto report = share("a", {}, {}, [&calls](int done, int total) {
            calls++;
            Q_UNUSED(total)
            return done < 1;
        });
        QVERIFY(report.stopped);
        QCOMPARE(report.shared, 1);
        QCOMPARE(calls, 2);
        // the other one is untouched
        QCOMPARE(m_store->table().refs().size(), 1);
    }

    void test_shareFileSharesOne()
    {
        QVERIFY(writeFile(path("a/mods/mod.jar"), "a mod"));
        const auto shared =
            SharedContent::shareFile(*m_store, nullptr, SharedContent::destination(*m_store, "a", path("a"), path("a/mods/mod.jar")));
        QVERIFY(shared);
        QCOMPARE(shared->outcome, ContentStore::ConvertOutcome::Shared);
        QVERIFY(m_store->table().ref({ m_store->instanceOwner("a"), "mods/mod.jar" }));
    }
};

QTEST_GUILESS_MAIN(ShareInstanceTest)

#include "ShareInstance_test.moc"
