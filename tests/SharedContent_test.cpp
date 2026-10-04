#include <QCryptographicHash>
#include <QDateTime>
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
}  // namespace

class SharedContentTest : public QObject {
    Q_OBJECT

    // in the working directory, so hard links don't cross a tmpfs boundary
    QTemporaryDir m_dir{ QDir::current().filePath("shared_content_test_XXXXXX") };
    std::unique_ptr<ContentStore> m_store;
    qint64 m_time = 0;
    bool m_canCreateSymbolicLinks = false;

    QString path(const QString& name) const { return m_dir.filePath(name); }
    // the game folder of an instance
    QString game(const QString& instance) const { return path("instances/" + instance + "/minecraft"); }
    QString file(const QString& instance, const QString& relativePath) const { return game(instance) + "/" + relativePath; }

    ContentStore::Destination destination(const QString& instance, const QString& relativePath) const
    {
        return SharedContent::destination(*m_store, instance, game(instance), file(instance, relativePath));
    }

    RefKey key(const QString& instance, const QString& relativePath) const { return destination(instance, relativePath).key(); }

    // Places a stored copy of the bytes at the path, with the given kind of link
    QString place(const QString& instance, const QString& relativePath, const QByteArray& data, LinkKind kind = LinkKind::Hard)
    {
        const auto source = path("downloads/" + sha256Of(data));
        writeFile(source, data);
        const auto stored = m_store->ingest(source, ContentStore::IngestMode::Copy);
        if (!stored) {
            return {};
        }
        ContentStore::PlaceOptions options;
        options.mode = kind == LinkKind::Hard ? ContentStore::LinkMode::HardLinks : ContentStore::LinkMode::SymbolicLinks;
        options.allowCopy = false;
        QDir().mkpath(QFileInfo(file(instance, relativePath)).absolutePath());
        return m_store->placeAt({ destination(instance, relativePath), stored->hash, {} }, options) ? stored->hash : QString();
    }

    bool isStored(const QString& hash) const
    {
        return QFileInfo::exists(m_store->objectPath(hash)) && m_store->table().entries().contains(hash);
    }

   private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        const auto target = path("symlink-target");
        QVERIFY(writeFile(target, "target"));
        m_canCreateSymbolicLinks = FS::createSymbolicLink(QFileInfo(target).absoluteFilePath(), path("symlink-probe")).has_value();
        QFile::remove(path("symlink-probe"));
        QFile::remove(target);
    }

    void init()
    {
        m_store.reset();
        QDirIterator it(m_dir.path(), QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            makeWritable(it.next());
        }
        QVERIFY(FS::deleteContents(m_dir.path()));
        for (const auto* dir : { "data", "downloads", "trash", "instances/a/minecraft/mods", "instances/b/minecraft/mods" }) {
            QVERIFY(QDir(m_dir.path()).mkpath(dir));
        }
        m_time = QDateTime::currentSecsSinceEpoch();
        m_store = std::make_unique<ContentStore>(path("store"), path("data"));
        m_store->setClockForTesting([this] { return m_time; });
        QCOMPARE(m_store->open(), ContentStore::State::Writable);
    }

    void test_whatIsShared()
    {
        QVERIFY(writeFile(file("a", "mods/mod.jar"), "mod"));
        QVERIFY(writeFile(file("a", "mods/mod.jar.disabled"), "disabled"));
        QVERIFY(writeFile(file("a", "resourcepacks/pack.zip"), "pack"));
        QVERIFY(writeFile(file("a", "shaderpacks/shader.zip"), "shader"));
        QVERIFY(writeFile(file("a", "shaderpacks/shader.zip.txt"), "settings the game edits"));
        QVERIFY(writeFile(file("a", "mods/.index/mod.pw.toml"), "metadata"));
        QVERIFY(writeFile(file("a", "mods/sub/nested.jar"), "nested"));
        QVERIFY(writeFile(file("a", "mods/.prism-new-123"), "temporary"));
        QVERIFY(writeFile(file("a", "config/mod.toml"), "config"));
        QVERIFY(writeFile(file("a", "saves/world/datapacks/pack.zip"), "a world's datapack"));
        QVERIFY(QDir().mkpath(file("a", "resourcepacks/folder")));

        auto shareable = SharedContent::shareableFiles(game("a"));
        for (auto& found : shareable) {
            found = QDir(game("a")).relativeFilePath(found);
        }
        std::sort(shareable.begin(), shareable.end());
        QCOMPARE(shareable, QStringList({ "mods/mod.jar", "mods/mod.jar.disabled", "resourcepacks/pack.zip", "shaderpacks/shader.zip" }));
    }

    void test_exclusionKey()
    {
        QCOMPARE(SharedContent::exclusionKey("mods/mod.jar.disabled"), "mods/mod.jar");
        QCOMPARE(SharedContent::exclusionKey("resourcepacks/pack.zip"), "resourcepacks/pack.zip");
    }

    void test_gameRoot()
    {
        QCOMPARE(QFileInfo(SharedContent::gameRootOf(path("instances/a"))), QFileInfo(game("a")));
        QVERIFY(QDir().mkpath(path("instances/c/.minecraft")));
        QCOMPARE(QFileInfo(SharedContent::gameRootOf(path("instances/c"))).fileName(), ".minecraft");
    }

    // Installs and downloads

    void test_installCopiesTheUsersFile()
    {
        QVERIFY(writeFile(path("downloads/chosen.jar"), "chosen by the user"));
        const auto placed = SharedContent::installFile(*m_store, destination("a", "mods/chosen.jar"), path("downloads/chosen.jar"),
                                                       ContentStore::IngestMode::Copy);
        QVERIFY2(placed, placed ? "" : qPrintable(placed.error()));
        QCOMPARE(*placed, PlacementKind::Hard);
        QCOMPARE(readFile(file("a", "mods/chosen.jar")), "chosen by the user");
        QVERIFY(m_store->table().ref(key("a", "mods/chosen.jar")));
        // the user's own file isn't touched
        QVERIFY(QFileInfo(path("downloads/chosen.jar")).isWritable());
        QVERIFY(!sameFile(path("downloads/chosen.jar"), file("a", "mods/chosen.jar")));
    }

    void test_installReplacesALocalFile()
    {
        QVERIFY(writeFile(file("a", "mods/mod.jar"), "old local version"));
        QVERIFY(writeFile(path("downloads/mod.jar"), "new version"));
        QVERIFY(SharedContent::installFile(*m_store, destination("a", "mods/mod.jar"), path("downloads/mod.jar"),
                                           ContentStore::IngestMode::Copy));
        QCOMPARE(readFile(file("a", "mods/mod.jar")), "new version");
    }

    void test_downloadIsMovedIntoTheStore()
    {
        // a download lands in the store's temporary folder, so storing it is a rename
        const auto download = QDir(m_store->temporaryDir()).filePath("download-test");
        QVERIFY(writeFile(download, "downloaded"));
        const auto downloadId = FS::fileId(download);
        QVERIFY(SharedContent::installFile(*m_store, destination("a", "mods/mod.jar"), download, ContentStore::IngestMode::Move));
        QVERIFY(!QFileInfo::exists(download));
        QCOMPARE(FS::fileId(m_store->objectPath(sha256Of("downloaded"))), downloadId);
        QVERIFY(sameFile(file("a", "mods/mod.jar"), m_store->objectPath(sha256Of("downloaded"))));
    }

    void test_updateReplacesTheLinkAndReleasesTheOldVersion()
    {
        const auto oldHash = place("a", "mods/mod.jar", "version 1");
        QVERIFY(writeFile(path("downloads/mod.jar"), "version 2"));
        QVERIFY(SharedContent::installFile(*m_store, destination("a", "mods/mod.jar"), path("downloads/mod.jar"),
                                           ContentStore::IngestMode::Copy));
        QCOMPARE(readFile(file("a", "mods/mod.jar")), "version 2");
        QVERIFY(!isStored(oldHash));
    }

    // Imports and copies

    void test_importedInstancesShareTheirFiles()
    {
        // two instances from the same modpack
        for (const auto* instance : { "a", "b" }) {
            QVERIFY(writeFile(file(instance, "mods/common.jar"), "common mod"));
            QVERIFY(writeFile(file(instance, "resourcepacks/pack.zip"), "common pack"));
            QVERIFY(writeFile(file(instance, "config/common.toml"), "config"));
        }
        QVERIFY(writeFile(file("b", "mods/only-b.jar"), "only in b"));

        QCOMPARE(SharedContent::shareFreshFiles(*m_store, "a", game("a"), SharedContent::shareableFiles(game("a"))), 2);
        QCOMPARE(SharedContent::shareFreshFiles(*m_store, "b", game("b"), SharedContent::shareableFiles(game("b"))), 3);

        QVERIFY(sameFile(file("a", "mods/common.jar"), file("b", "mods/common.jar")));
        QVERIFY(sameFile(file("a", "resourcepacks/pack.zip"), file("b", "resourcepacks/pack.zip")));
        QVERIFY(sameFile(file("a", "mods/common.jar"), m_store->objectPath(sha256Of("common mod"))));
        QCOMPARE(m_store->table().refs().size(), 5);
        QCOMPARE(m_store->table().entries().size(), 3);
        // no temporary links are left next to the files
        for (const auto* folder : { "a/mods", "a/resourcepacks", "b/mods", "b/resourcepacks" }) {
            QVERIFY(QDir(path("instances/") + QString(folder).replace("/", "/minecraft/"))
                        .entryList({ ".prism-*" }, QDir::AllEntries | QDir::Hidden)
                        .isEmpty());
        }
        QCOMPARE(readFile(file("b", "mods/only-b.jar")), "only in b");
        // configuration isn't shared
        QVERIFY(!sameFile(file("a", "config/common.toml"), file("b", "config/common.toml")));
        QVERIFY(QFileInfo(file("a", "config/common.toml")).isWritable());
    }

    void test_sharingTwiceChangesNothing()
    {
        QVERIFY(writeFile(file("a", "mods/mod.jar"), "mod"));
        QCOMPARE(SharedContent::shareFreshFiles(*m_store, "a", game("a"), SharedContent::shareableFiles(game("a"))), 1);
        QCOMPARE(SharedContent::shareFreshFiles(*m_store, "a", game("a"), SharedContent::shareableFiles(game("a"))), 0);
        QCOMPARE(m_store->table().refs().size(), 1);
    }

    void test_updatedFileWithTheSameNameIsShared()
    {
        QVERIFY(writeFile(file("a", "mods/mod.jar"), "version 1"));
        QCOMPARE(SharedContent::shareFreshFiles(*m_store, "a", game("a"), SharedContent::shareableFiles(game("a"))), 1);
        const auto oldHash = sha256Of("version 1");

        // a modpack update puts a new version under the same name
        QVERIFY(FS::deleteLink(file("a", "mods/mod.jar")));
        QVERIFY(writeFile(file("a", "mods/mod.jar"), "version 2"));
        QCOMPARE(SharedContent::shareFreshFiles(*m_store, "a", game("a"), SharedContent::shareableFiles(game("a"))), 1);

        QCOMPARE(m_store->table().ref(key("a", "mods/mod.jar"))->hash, sha256Of("version 2"));
        QVERIFY(sameFile(file("a", "mods/mod.jar"), m_store->objectPath(sha256Of("version 2"))));
        QVERIFY(!isStored(oldHash));
        const auto verified = m_store->verify();
        QVERIFY(verified);
        QVERIFY(verified->replaced.isEmpty() && verified->missing.isEmpty());
    }

    void test_filesKeptLocalAreNotShared()
    {
        QVERIFY(writeFile(file("a", "mods/mod.jar.disabled"), "kept local"));
        QVERIFY(writeFile(file("a", "mods/other.jar"), "shared"));
        const auto shared = SharedContent::shareFreshFiles(
            *m_store, "a", game("a"), SharedContent::shareableFiles(game("a")),
            [](const QString& relativePath) { return SharedContent::exclusionKey(relativePath) == "mods/mod.jar"; });
        QCOMPARE(shared, 1);
        QVERIFY(QFileInfo(file("a", "mods/mod.jar.disabled")).isWritable());
        QVERIFY(!m_store->table().ref(key("a", "mods/mod.jar.disabled")));
    }

    void test_copyWithHardLinksIsLeftAlone()
    {
        // copied with the "hard links" option: the copy's files are the source instance's own files
        QVERIFY(writeFile(file("a", "mods/mod.jar"), "the user's file"));
        QVERIFY(FS::createHardLink(file("a", "mods/mod.jar"), file("b", "mods/mod.jar")));
        QCOMPARE(SharedContent::shareFreshFiles(*m_store, "b", game("b"), SharedContent::shareableFiles(game("b"))), 0);
        // it must not become read-only in the source instance
        QVERIFY(QFileInfo(file("a", "mods/mod.jar")).isWritable());
        QVERIFY(m_store->table().entries().isEmpty());
    }

    void test_copyOfASharedInstanceRecordsItsLinks()
    {
        const auto hash = place("a", "mods/mod.jar", "shared");
        // a copy with hard links, of a file that is already a stored file
        QVERIFY(FS::createHardLink(file("a", "mods/mod.jar"), file("b", "mods/mod.jar")));
        QCOMPARE(SharedContent::shareFreshFiles(*m_store, "b", game("b"), SharedContent::shareableFiles(game("b"))), 1);
        QCOMPARE(m_store->table().ref(key("b", "mods/mod.jar"))->hash, hash);
        // and a plain copy becomes a link to the same stored file
        QVERIFY(QDir().mkpath(path("instances/c/minecraft/mods")));
        QVERIFY(writeFile(file("c", "mods/mod.jar"), "shared"));
        QCOMPARE(SharedContent::shareFreshFiles(*m_store, "c", game("c"), SharedContent::shareableFiles(game("c"))), 1);
        QVERIFY(sameFile(file("c", "mods/mod.jar"), m_store->objectPath(hash)));
    }

    void test_symbolicLinksAreLeftAlone()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        QVERIFY(writeFile(path("elsewhere/mod.jar"), "outside"));
        QVERIFY(FS::createSymbolicLink(QFileInfo(path("elsewhere/mod.jar")).absoluteFilePath(), file("a", "mods/mod.jar")));
        QCOMPARE(SharedContent::shareFreshFiles(*m_store, "a", game("a"), { file("a", "mods/mod.jar") }), 0);
        QVERIFY(QFileInfo(path("elsewhere/mod.jar")).isWritable());
    }

    // Removing files

    void test_trashedHardLinkKeepsTheFile()
    {
        const auto hash = place("a", "mods/mod.jar", "mod");
        const auto removed = SharedContent::removeFile(*m_store, key("a", "mods/mod.jar"),
                                                       [this] { return FS::move(file("a", "mods/mod.jar"), path("trash/mod.jar")); });
        QVERIFY2(removed, removed ? "" : qPrintable(removed.error()));
        QVERIFY(!m_store->table().ref(key("a", "mods/mod.jar")));
        // the trash holds a link to it
        QVERIFY(isStored(hash));
        QCOMPARE(readFile(path("trash/mod.jar")), "mod");
        QVERIFY(FS::deleteLink(path("trash/mod.jar")));
        QCOMPARE(*m_store->destroyUnused(), 1);
    }

    void test_trashedSymbolicLinkIsACopy()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        place("a", "resourcepacks/pack.zip", "pack", LinkKind::Symbolic);
        bool wasLink = true;
        QVERIFY(SharedContent::removeFile(*m_store, key("a", "resourcepacks/pack.zip"), [&] {
            wasLink = QFileInfo(file("a", "resourcepacks/pack.zip")).isSymbolicLink();
            return FS::move(file("a", "resourcepacks/pack.zip"), path("trash/pack.zip"));
        }));
        QVERIFY(!wasLink);
        QCOMPARE(readFile(path("trash/pack.zip")), "pack");
    }

    void test_failedRemovalKeepsTheLink()
    {
        const auto hash = place("a", "mods/mod.jar", "mod");
        QVERIFY(!SharedContent::removeFile(*m_store, key("a", "mods/mod.jar"), [] { return false; }));
        QVERIFY(m_store->table().ref(key("a", "mods/mod.jar")));
        QVERIFY(isStored(hash));
    }

    void test_forgettingAFileThatIsThereFails()
    {
        place("a", "mods/mod.jar", "mod");
        QVERIFY(!m_store->forgetRemoved(key("a", "mods/mod.jar")));
        QVERIFY(m_store->table().ref(key("a", "mods/mod.jar")));
    }

    void test_trashedInstanceCanBeRestored()
    {
        const auto hash = place("a", "mods/mod.jar", "mod");
        const auto owner = m_store->instanceOwner("a");
        QVERIFY(SharedContent::removeInstance(*m_store, owner, [this] { return FS::move(path("instances/a"), path("trash/a")); }));
        QVERIFY(!m_store->table().owners().contains(owner));
        QVERIFY(m_store->table().refs().isEmpty());
        // the trashed instance's hard link keeps the stored file
        QVERIFY(isStored(hash));

        // restored, then found by a scan of the instances in the list
        QVERIFY(FS::move(path("trash/a"), path("instances/a")));
        ContentStore::ReconcileOptions options;
        options.knownOwners.insert(owner, QFileInfo(game("a")).absoluteFilePath());
        const auto report = m_store->reconcile(options);
        QVERIFY(report);
        QCOMPARE(report->adopted, 1);
        QCOMPARE(m_store->table().ref(key("a", "mods/mod.jar"))->hash, hash);
    }

    void test_failedInstanceTrashKeepsEverything()
    {
        const auto hash = place("a", "mods/mod.jar", "mod");
        QVERIFY(!SharedContent::removeInstance(*m_store, m_store->instanceOwner("a"), [] { return false; }));
        QVERIFY(m_store->table().ref(key("a", "mods/mod.jar")));
        QVERIFY(isStored(hash));
    }

    void test_instanceTrashStopsIfALinkCantBeCopied()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        place("a", "resourcepacks/1.zip", "first", LinkKind::Symbolic);
        place("a", "resourcepacks/2.zip", "second", LinkKind::Symbolic);
        place("a", "resourcepacks/3.zip", "third", LinkKind::Symbolic);
        // the second can't be turned into a copy: something else is at its path now
        QVERIFY(FS::deleteLink(file("a", "resourcepacks/2.zip")));
        QVERIFY(writeFile(file("a", "resourcepacks/2.zip"), "something else"));

        bool trashed = false;
        QVERIFY(!SharedContent::removeInstance(*m_store, m_store->instanceOwner("a"), [&] { return trashed = true; }));
        QVERIFY(!trashed);
        // the first is a copy now, the third is untouched
        QVERIFY(!QFileInfo(file("a", "resourcepacks/1.zip")).isSymbolicLink());
        QCOMPARE(readFile(file("a", "resourcepacks/1.zip")), "first");
        QVERIFY(QFileInfo(file("a", "resourcepacks/3.zip")).isSymbolicLink());
        QVERIFY(m_store->table().ref(key("a", "resourcepacks/3.zip")));
    }

    void test_deletedInstanceReleasesItsFiles()
    {
        const auto hash = place("a", "mods/mod.jar", "mod");
        QVERIFY(
            SharedContent::removeInstance(*m_store, m_store->instanceOwner("a"), [this] { return FS::deletePath(path("instances/a")); }));
        QVERIFY(!isStored(hash));
    }

    void test_forgettingAnInstanceThatIsThereFails()
    {
        place("a", "mods/mod.jar", "mod");
        QVERIFY(!m_store->forgetOwner(m_store->instanceOwner("a")));
        QVERIFY(m_store->table().ref(key("a", "mods/mod.jar")));
    }

    // Renaming

    void test_renamedInstanceKeepsItsLinks()
    {
        const auto hash = place("a", "mods/mod.jar", "mod");
        QVERIFY(QFile::rename(path("instances/a"), path("instances/renamed")));
        QVERIFY(m_store->renameOwner(m_store->instanceOwner("a"), m_store->instanceOwner("renamed"),
                                     SharedContent::gameRootOf(path("instances/renamed"))));
        QVERIFY(!m_store->table().owners().contains(m_store->instanceOwner("a")));
        QCOMPARE(m_store->table().ref(key("renamed", "mods/mod.jar"))->hash, hash);
        const auto verified = m_store->verify();
        QVERIFY(verified);
        QVERIFY(verified->missing.isEmpty() && verified->replaced.isEmpty());
    }
};

QTEST_GUILESS_MAIN(SharedContentTest)

#include "SharedContent_test.moc"
