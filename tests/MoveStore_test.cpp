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

// Changing the store's folder moves what is shared to the new store
class MoveStoreTest : public QObject {
    Q_OBJECT

    // in the working directory, so hard links don't cross a tmpfs boundary
    QTemporaryDir m_dir{ QDir::current().filePath("move_store_test_XXXXXX") };
    std::unique_ptr<ContentStore> m_from;
    std::unique_ptr<ContentStore> m_to;

    QString path(const QString& name) const { return m_dir.filePath(name); }

    // shares the files of an instance through a store, as "Share all content" does
    SharedContent::ShareReport share(ContentStore& store, const QString& instance)
    {
        return SharedContent::shareInstance(store, instance, path(instance), {});
    }

    RefKey key(ContentStore& store, const QString& instance, const QString& relativePath)
    {
        return { store.instanceOwner(instance), relativePath };
    }

   private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        WriterGuard::setBackupsNeedValidationForTesting(false);
    }

    void init()
    {
        m_from.reset();
        m_to.reset();
        QDirIterator it(m_dir.path(), QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            makeWritable(it.next());
        }
        QVERIFY(FS::deleteContents(m_dir.path()));
        QVERIFY(QDir(m_dir.path()).mkpath("data"));
        m_from = std::make_unique<ContentStore>(path("old store"), path("data"));
        QCOMPARE(m_from->open(), ContentStore::State::Writable);
        m_to = std::make_unique<ContentStore>(path("new store"), path("data"));
        QCOMPARE(m_to->open(), ContentStore::State::Writable);
    }

    void test_movesTheLinks()
    {
        for (const auto* instance : { "a", "b" }) {
            QVERIFY(writeFile(path(QString("%1/mods/common.jar").arg(instance)), "common mod"));
        }
        QVERIFY(writeFile(path("a/resourcepacks/pack.zip"), "a resource pack"));
        QCOMPARE(share(*m_from, "a").shared, 2);
        QCOMPARE(share(*m_from, "b").shared, 1);
        // a file that is kept local isn't linked, and stays as it is
        QVERIFY(writeFile(path("a/mods/local.jar"), "a local mod"));
        const auto localBefore = FS::identity(path("a/mods/local.jar"));

        const auto report = SharedContent::moveShares(*m_from, *m_to);
        QCOMPARE(report.moved, 3);
        QVERIFY2(report.failed.isEmpty(), qPrintable(report.failed.join('\n')));

        // linked from the new store, with the same contents
        const auto common = m_to->table().ref(key(*m_to, "a", "mods/common.jar"));
        QVERIFY(common);
        QVERIFY(sameFile(path("a/mods/common.jar"), m_to->objectPath(common->hash)));
        QVERIFY(sameFile(path("b/mods/common.jar"), m_to->objectPath(common->hash)));
        QCOMPARE(readFile(path("a/mods/common.jar")), "common mod");
        QCOMPARE(readFile(path("a/resourcepacks/pack.zip")), "a resource pack");
        QCOMPARE(FS::identity(path("a/mods/local.jar")), localBefore);

        // the old store records nothing anymore, and its files went with their last links
        QVERIFY(m_from->refsSnapshot().isEmpty());
        QCOMPARE(m_from->stats().files, 0);
    }

    void test_aLinkIsOnlyReleasedOnceItMoved()
    {
        QVERIFY(writeFile(path("a/mods/mod.jar"), "a mod"));
        QCOMPARE(share(*m_from, "a").shared, 1);
        const auto mod = key(*m_from, "a", "mods/mod.jar");
        // still the old store's link
        QVERIFY(!m_from->releaseMoved(mod));
        QVERIFY(m_from->table().ref(mod));
    }

    // an update that replaces a file while it is copied isn't undone by the copy taken before
    void test_aFileReplacedDuringTheMoveIsKept()
    {
        QVERIFY(writeFile(path("a/mods/mod.jar"), "a mod"));
        QCOMPARE(share(*m_from, "a").shared, 1);
        SharedContent::setMoveHookForTesting([](SharedContent::MoveStep step, const QString& file) {
            if (step == SharedContent::MoveStep::BeforeCopy && FS::deleteLink(file)) {
                writeFile(file, "the updated mod");
            }
        });
        const auto report = SharedContent::moveShares(*m_from, *m_to);
        SharedContent::setMoveHookForTesting({});
        QCOMPARE(report.moved, 0);
        QCOMPARE(report.failed.size(), 1);
        QCOMPARE(readFile(path("a/mods/mod.jar")), "the updated mod");
        QVERIFY(m_to->refsSnapshot().isEmpty());
    }

    // shares a.jar through a symbolic link to the old store, and returns the file it links to
    QString shareSymbolically()
    {
        writeFile(path("probe/target"), "probe");
        if (!FS::createSymbolicLink(QFileInfo(path("probe/target")).absoluteFilePath(), path("probe/link"))) {
            return {};
        }
        m_from->setLinkMode(ContentStore::LinkMode::SymbolicLinks);
        writeFile(path("a/mods/mod.jar"), "old bytes");
        if (share(*m_from, "a").shared != 1 || !QFileInfo(path("a/mods/mod.jar")).isSymLink()) {
            return {};
        }
        return QFileInfo(path("a/mods/mod.jar")).symLinkTarget();
    }

    // the file a symbolic link points at changes, in place, while it is moved: what it holds then is what is moved
    void test_aChangedLinkTargetIsMovedAgain_data()
    {
        QTest::addColumn<SharedContent::MoveStep>("step");
        QTest::newRow("while copied") << SharedContent::MoveStep::Copied;
        QTest::newRow("while placed") << SharedContent::MoveStep::Placed;
    }
    void test_aChangedLinkTargetIsMovedAgain()
    {
        QFETCH(SharedContent::MoveStep, step);
        const auto target = shareSymbolically();
        if (target.isEmpty()) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        bool changed = false;
        SharedContent::setMoveHookForTesting([&](SharedContent::MoveStep at, const QString&) {
            if (at == step && !changed) {
                changed = true;
                makeWritable(target);
                writeFile(target, "new bytes");
            }
        });
        const auto report = SharedContent::moveShares(*m_from, *m_to);
        SharedContent::setMoveHookForTesting({});
        QVERIFY(changed);
        QVERIFY2(report.failed.isEmpty(), qPrintable(report.failed.join('\n')));
        QCOMPARE(report.moved, 1);
        QCOMPARE(readFile(path("a/mods/mod.jar")), "new bytes");
    }

    // and if it keeps changing, the old store keeps it
    void test_aLinkTargetThatKeepsChangingIsKept()
    {
        const auto target = shareSymbolically();
        if (target.isEmpty()) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        int changes = 0;
        SharedContent::setMoveHookForTesting([&](SharedContent::MoveStep at, const QString&) {
            if (at == SharedContent::MoveStep::Placed) {
                makeWritable(target);
                writeFile(target, QByteArray("change ") + QByteArray::number(++changes));
            }
        });
        const auto report = SharedContent::moveShares(*m_from, *m_to);
        SharedContent::setMoveHookForTesting({});
        QCOMPARE(report.moved, 0);
        QCOMPARE(report.failed.size(), 1);
        QVERIFY(report.failed.first().contains(target));
        // not released, so the newest bytes are still there
        QVERIFY(m_from->table().ref(key(*m_from, "a", "mods/mod.jar")));
        const auto newest = QByteArray("change ") + QByteArray::number(changes);
        QCOMPARE(readFile(target), newest);
        // and linked again, not left with an older copy from the new store, which forgot it
        QVERIFY(QFileInfo(path("a/mods/mod.jar")).isSymLink());
        QCOMPARE(readFile(path("a/mods/mod.jar")), newest);
        QVERIFY(m_to->refsSnapshot().isEmpty());
        QCOMPARE(SharedContent::linksInto(m_from->storeDir(), { path("a") }).size(), 1);
    }

    // and a file an update put there between two attempts isn't replaced by the link put back
    void test_aFileReplacedBetweenAttemptsIsKept()
    {
        const auto target = shareSymbolically();
        if (target.isEmpty()) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        int copies = 0;
        SharedContent::setMoveHookForTesting([&](SharedContent::MoveStep at, const QString& file) {
            if (at == SharedContent::MoveStep::Placed) {
                makeWritable(target);
                writeFile(target, "changed again");
            } else if (at == SharedContent::MoveStep::BeforeCopy && ++copies == 2 && FS::deleteLink(file)) {
                writeFile(file, "the updated mod");
            }
        });
        const auto report = SharedContent::moveShares(*m_from, *m_to);
        SharedContent::setMoveHookForTesting({});
        QCOMPARE(copies, 2);
        QCOMPARE(report.moved, 0);
        QCOMPARE(report.failed.size(), 1);
        QVERIFY(!QFileInfo(path("a/mods/mod.jar")).isSymLink());
        QCOMPARE(readFile(path("a/mods/mod.jar")), "the updated mod");
        // what the old store holds is kept too
        QVERIFY(m_from->table().ref(key(*m_from, "a", "mods/mod.jar")));
        QCOMPARE(readFile(target), "changed again");
        QVERIFY(m_to->refsSnapshot().isEmpty());
    }

    void test_foldersOverlap()
    {
        QVERIFY(SharedContent::foldersOverlap(path("store"), path("store")));
        QVERIFY(SharedContent::foldersOverlap(path("store"), path("store/new-store")));
        QVERIFY(SharedContent::foldersOverlap(path("store/new-store"), path("store")));
        QVERIFY(SharedContent::foldersOverlap(path("store"), path("store/a/../new-store/")));
        QVERIFY(!SharedContent::foldersOverlap(path("store"), path("store-2")));
        QVERIFY(!SharedContent::foldersOverlap(path("store"), path("other/store")));
        // through a link to the folder in use
        QVERIFY(QDir().mkpath(path("store")));
        if (FS::createSymbolicLink(path("store"), path("link"))) {
            QVERIFY(SharedContent::foldersOverlap(path("store"), path("link/new-store")));
        }
    }

    void test_goneLinksAreReleased()
    {
        QVERIFY(writeFile(path("a/mods/mod.jar"), "a mod"));
        QCOMPARE(share(*m_from, "a").shared, 1);
        QVERIFY(FS::deleteLink(path("a/mods/mod.jar")));
        const auto report = SharedContent::moveShares(*m_from, *m_to);
        QCOMPARE(report.moved, 0);
        QCOMPARE(report.gone, 1);
        QVERIFY(m_from->refsSnapshot().isEmpty());
        QVERIFY(m_to->refsSnapshot().isEmpty());
    }

    void test_symbolicLinksBecomeHardLinks()
    {
        QVERIFY(writeFile(path("probe/target"), "probe"));
        if (!FS::createSymbolicLink(QFileInfo(path("probe/target")).absoluteFilePath(), path("probe/link"))) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        // shared with symbolic links, as when the old store was on another drive
        m_from->setLinkMode(ContentStore::LinkMode::SymbolicLinks);
        QVERIFY(writeFile(path("a/mods/mod.jar"), "a mod"));
        QCOMPARE(share(*m_from, "a").shared, 1);
        QVERIFY(QFileInfo(path("a/mods/mod.jar")).isSymLink());
        QCOMPARE(SharedContent::linksInto(m_from->storeDir(), { path("a") }).size(), 1);

        const auto report = SharedContent::moveShares(*m_from, *m_to);
        QVERIFY2(report.failed.isEmpty(), qPrintable(report.failed.join('\n')));
        QCOMPARE(report.moved, 1);
        QVERIFY(!QFileInfo(path("a/mods/mod.jar")).isSymLink());
        const auto ref = m_to->table().ref(key(*m_to, "a", "mods/mod.jar"));
        QVERIFY(ref);
        QVERIFY(sameFile(path("a/mods/mod.jar"), m_to->objectPath(ref->hash)));
        // nothing points at the old store anymore, so it can go
        QVERIFY(SharedContent::linksInto(m_from->storeDir(), { path("a") }).isEmpty());
        // its file waits for a scan, as a symbolic link to it could be anywhere
        QCOMPARE(m_from->stats().unusedFiles, 1);
        QCOMPARE(m_from->stats().unusedAwaitingScan, 1);
    }

    void test_unusedSpace()
    {
        QVERIFY(writeFile(path("downloads/mod.jar"), "a stored mod"));
        QVERIFY(m_to->ingest(path("downloads/mod.jar"), ContentStore::IngestMode::Copy));
        const auto stats = m_to->stats();
        QCOMPARE(stats.unusedFiles, 1);
        QCOMPARE(stats.unusedBytes, qint64(QByteArray("a stored mod").size()));
    }
};

QTEST_GUILESS_MAIN(MoveStoreTest)

#include "MoveStore_test.moc"
