#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <filesystem>
#include <system_error>

#include "FileSystem.h"
#include "FileSystemPrimitives.h"
#include "StringUtils.h"
#include "contentstore/ContentStore.h"
#include "contentstore/SymlinkAllowList.h"

namespace {
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

QString sha256Of(const QByteArray& data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

void makeWritable(const QString& path)
{
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser);
}

bool sameFile(const QString& first, const QString& second)
{
    const auto a = FS::fileId(first);
    const auto b = FS::fileId(second);
    return a && b && *a == *b;
}

using Step = ContentStore::PlacementStep;
}  // namespace

class PlacementTest : public QObject {
    Q_OBJECT

    // in the working directory, so hard links don't cross a tmpfs boundary
    QTemporaryDir m_dir{ QDir::current().filePath("placement_test_XXXXXX") };
    std::unique_ptr<ContentStore> m_store;
    bool m_canCreateSymbolicLinks = false;

    QString path(const QString& name) const { return m_dir.filePath(name); }

    ContentStore::Destination destination(const QString& relativePath, const QString& instance = "a") const
    {
        return { m_store->instanceOwner(instance), path(instance), relativePath };
    }

    // Stores the bytes and returns their hash
    QString store(const QByteArray& data)
    {
        const auto source = path("downloads/" + sha256Of(data));
        if (!writeFile(source, data)) {
            return {};
        }
        const auto result = m_store->ingest(source, ContentStore::IngestMode::Copy);
        return result ? result->hash : QString();
    }

    // Temporary files left next to the destinations
    QStringList leftovers(const QString& dir) const
    {
        return QDir(path(dir)).entryList({ ".prism-new-*" }, QDir::AllEntries | QDir::Hidden | QDir::System);
    }

    void reopen()
    {
        FS::Testing::setFaultHook(nullptr);
        m_store.reset();
        m_store = std::make_unique<ContentStore>(path("store"), path("data"));
        QCOMPARE(m_store->open(), ContentStore::State::Writable);
    }

    // Makes hard and symbolic links fail, so placements fall back to a copy
    static void disableLinks()
    {
        FS::Testing::setFaultHook([](FS::Testing::Operation operation, const QString&) {
            return operation == FS::Testing::Operation::HardLink || operation == FS::Testing::Operation::SymbolicLink;
        });
    }

    static void disableHardLinks()
    {
        FS::Testing::setFaultHook(
            [](FS::Testing::Operation operation, const QString&) { return operation == FS::Testing::Operation::HardLink; });
    }

    // The identity recorded for the stored file matches the file, so reusing it needs no hashing
    bool identityIsCurrent(const QString& hash) const
    {
        const auto identity = FS::identity(m_store->objectPath(hash));
        return identity && m_store->table().entries()[hash].current->identity == StoredIdentity::from(*identity);
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
        // stored files are read-only
        QDirIterator it(m_dir.path(), QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            makeWritable(it.next());
        }
        QVERIFY(FS::deleteContents(m_dir.path()));
        QVERIFY(QDir(m_dir.path()).mkpath("data"));
        QVERIFY(QDir(m_dir.path()).mkpath("downloads"));
        QVERIFY(QDir(m_dir.path()).mkpath("a/mods"));
        QVERIFY(QDir(m_dir.path()).mkpath("b/mods"));
        m_store = std::make_unique<ContentStore>(path("store"), path("data"));
        QCOMPARE(m_store->open(), ContentStore::State::Writable);
    }

    void cleanup()
    {
        FS::Testing::setFaultHook(nullptr);
        if (m_store) {
            m_store->setInterruptionForTesting(nullptr);
        }
    }

    void test_placeHardLink()
    {
        const auto hash = store("mod");
        const auto placed = m_store->placeAt({ destination("mods/mod.jar"), hash, {} });
        QVERIFY2(placed, placed ? "" : qPrintable(placed.error()));
        QCOMPARE(*placed, PlacementKind::Hard);

        QVERIFY(sameFile(path("a/mods/mod.jar"), m_store->objectPath(hash)));
        QCOMPARE(readFile(path("a/mods/mod.jar")), "mod");
        // the stored file stays read-only through every link
        QVERIFY(!QFileInfo(path("a/mods/mod.jar")).isWritable());

        const auto ref = m_store->table().ref(destination("mods/mod.jar").key());
        QVERIFY(ref);
        QCOMPARE(ref->hash, hash);
        QCOMPARE(ref->kind, LinkKind::Hard);
        QCOMPARE(ref->state, RefState::Live);
        QCOMPARE(ref->generation, m_store->table().entries()[hash].current->id);
        QCOMPARE(m_store->table().owners()[m_store->instanceOwner("a")], path("a"));
        QVERIFY(m_store->table().transactions().isEmpty());
        QVERIFY(leftovers("a/mods").isEmpty());
        // linking changed the stored file's change time, which was recorded
        QVERIFY(identityIsCurrent(hash));
    }

    void test_instancesShareOneFile()
    {
        const auto hash = store("shared");
        const auto results =
            m_store->place({ { destination("mods/mod.jar", "a"), hash, {} }, { destination("mods/mod.jar", "b"), hash, {} } });
        QCOMPARE(results.size(), 2);
        QVERIFY(results[0] && results[1]);
        QVERIFY(sameFile(path("a/mods/mod.jar"), path("b/mods/mod.jar")));
        QVERIFY(sameFile(path("a/mods/mod.jar"), m_store->objectPath(hash)));
        QCOMPARE(m_store->table().refs().size(), 2);

        // placing again needs no hashing of the stored file
        QVERIFY(writeFile(path("downloads/again.jar"), "shared"));
        const auto again = m_store->ingest(path("downloads/again.jar"), ContentStore::IngestMode::Copy);
        QVERIFY(again);
        QVERIFY(again->reusedObject);
        QVERIFY(!again->rehashedObject);
    }

    void test_placeCreatesMissingFolders()
    {
        const auto hash = store("deep");
        QVERIFY(m_store->placeAt({ destination("resourcepacks/pack.zip"), hash, {} }));
        QVERIFY(sameFile(path("a/resourcepacks/pack.zip"), m_store->objectPath(hash)));
    }

    void test_symbolicLinkFallback()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto hash = store("symbolic");
        disableHardLinks();
        const auto placed = m_store->placeAt({ destination("resourcepacks/pack.zip"), hash, {} });
        QVERIFY2(placed, placed ? "" : qPrintable(placed.error()));
        QCOMPARE(*placed, PlacementKind::Symbolic);

        const QFileInfo info(path("a/resourcepacks/pack.zip"));
        QVERIFY(info.isSymbolicLink());
        QVERIFY(QFileInfo(info.symLinkTarget()) == QFileInfo(m_store->objectPath(hash)));
        QCOMPARE(readFile(info.filePath()), "symbolic");
        const auto ref = m_store->table().ref(destination("resourcepacks/pack.zip").key());
        QVERIFY(ref);
        QCOMPARE(ref->kind, LinkKind::Symbolic);
        QVERIFY(m_store->table().entries()[hash].hadSymbolicLinks);

        // Minecraft is told to follow links into the store
        const auto allowed = QString::fromUtf8(readFile(SymlinkAllowList::path(path("a"))));
        QVERIFY(allowed.contains(SymlinkAllowList::entryFor(path("store"))));
    }

    void test_allowListIsWrittenBeforeTheSwap()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto hash = store("symbolic");
        ContentStore::PlaceOptions options;
        options.mode = ContentStore::LinkMode::SymbolicLinks;
        m_store->setInterruptionForTesting([](Step reached) { return reached == Step::Swapped; });
        QVERIFY(!m_store->placeAt({ destination("resourcepacks/pack.zip"), hash, {} }, options));
        reopen();

        // the crash came after the swap, so the link is kept, and Minecraft is allowed to follow it
        QCOMPARE(m_store->table().ref(destination("resourcepacks/pack.zip").key())->kind, LinkKind::Symbolic);
        const auto allowed = QString::fromUtf8(readFile(SymlinkAllowList::path(path("a"))));
        QVERIFY(allowed.contains(SymlinkAllowList::entryFor(path("store"))));
    }

    void test_symbolicLinkNeedsTheAllowList()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto hash = store("symbolic");
        // only the list can't be written
        FS::Testing::setFaultHook([](FS::Testing::Operation operation, const QString& target) {
            return operation == FS::Testing::Operation::Replace && QFileInfo(target).fileName() == "allowed_symlinks.txt";
        });
        ContentStore::PlaceOptions options;
        options.mode = ContentStore::LinkMode::SymbolicLinks;
        const auto placed = m_store->placeAt({ destination("resourcepacks/pack.zip"), hash, {} }, options);
        QVERIFY(!placed);
        QVERIFY2(placed.error().contains("allowed_symlinks.txt"), qPrintable(placed.error()));
        // a link Minecraft would refuse is never put in place
        QVERIFY(!QFileInfo(path("a/resourcepacks/pack.zip")).isSymbolicLink());
        QVERIFY(!QFileInfo::exists(path("a/resourcepacks/pack.zip")));
        QVERIFY(m_store->table().refs().isEmpty());
        QVERIFY(m_store->table().transactions().isEmpty());
        QVERIFY(leftovers("a/resourcepacks").isEmpty());
    }

    void test_symbolicLinkMode()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto hash = store("mode");
        ContentStore::PlaceOptions options;
        options.mode = ContentStore::LinkMode::SymbolicLinks;
        const auto placed = m_store->placeAt({ destination("mods/mod.jar"), hash, {} }, options);
        QVERIFY(placed);
        QCOMPARE(*placed, PlacementKind::Symbolic);
    }

    void test_copyFallbackLeavesNoRef()
    {
        const auto hash = store("copied");
        disableLinks();
        const auto placed = m_store->placeAt({ destination("mods/mod.jar"), hash, {} });
        QVERIFY2(placed, placed ? "" : qPrintable(placed.error()));
        QCOMPARE(*placed, PlacementKind::Local);

        QCOMPARE(readFile(path("a/mods/mod.jar")), "copied");
        QVERIFY(!sameFile(path("a/mods/mod.jar"), m_store->objectPath(hash)));
        QVERIFY(QFileInfo(path("a/mods/mod.jar")).isWritable());
        QVERIFY(!m_store->table().ref(destination("mods/mod.jar").key()));
        QVERIFY(m_store->table().transactions().isEmpty());
        QVERIFY(leftovers("a/mods").isEmpty());
    }

    void test_noCopyWhenNotAllowed()
    {
        const auto hash = store("linked only");
        disableLinks();
        ContentStore::PlaceOptions options;
        options.allowCopy = false;
        const auto placed = m_store->placeAt({ destination("mods/mod.jar"), hash, {} }, options);
        QVERIFY(!placed);
        QVERIFY(!QFileInfo::exists(path("a/mods/mod.jar")));
        QVERIFY(m_store->table().refs().isEmpty());
        QVERIFY(m_store->table().transactions().isEmpty());
        QVERIFY(leftovers("a/mods").isEmpty());
    }

    void test_elevatedLinksAreRequestedOnce()
    {
        const auto first = store("first");
        const auto second = store("second");
        disableLinks();
        int requests = 0;
        qsizetype linksRequested = 0;
        m_store->setPrivilegedLinker([&](const QList<std::pair<QString, QString>>& links) {
            requests++;
            linksRequested += links.size();
            // the user declined
            return QList<Result<>>(links.size(), std::unexpected(QString("Declined")));
        });
        const auto results = m_store->place({ { destination("mods/first.jar"), first, {} },
                                              { destination("mods/second.jar"), second, {} },
                                              { destination("mods/first.jar", "b"), first, {} } });
        QCOMPARE(requests, 1);
        QCOMPARE(linksRequested, 3);
        // declined links fall back to copies
        for (const auto& result : results) {
            QVERIFY(result);
            QCOMPARE(*result, PlacementKind::Local);
        }
        QCOMPARE(readFile(path("a/mods/second.jar")), "second");
        QVERIFY(m_store->table().refs().isEmpty());
        QVERIFY(leftovers("a/mods").isEmpty());
    }

    void test_elevatedLinksAreRecorded()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto hash = store("elevated");
        disableLinks();
        m_store->setPrivilegedLinker([](const QList<std::pair<QString, QString>>& links) {
            QList<Result<>> results;
            for (const auto& [target, link] : links) {
                std::error_code error;
                std::filesystem::create_symlink(StringUtils::toStdString(target), StringUtils::toStdString(link), error);
                results.append(error ? Result<>(std::unexpected(QString::fromStdString(error.message()))) : Result<>{});
            }
            return results;
        });
        const auto placed = m_store->placeAt({ destination("mods/mod.jar"), hash, {} });
        QVERIFY2(placed, placed ? "" : qPrintable(placed.error()));
        QCOMPARE(*placed, PlacementKind::Symbolic);
        QCOMPARE(m_store->table().ref(destination("mods/mod.jar").key())->kind, LinkKind::Symbolic);
    }

    void test_updateReplacesLink()
    {
        const auto oldHash = store("version 1");
        const auto newHash = store("version 2");
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar"), oldHash, {} }));
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar", "b"), oldHash, {} }));

        const auto placed = m_store->placeAt({ destination("mods/mod.jar"), newHash, {} });
        QVERIFY2(placed, placed ? "" : qPrintable(placed.error()));
        QVERIFY(sameFile(path("a/mods/mod.jar"), m_store->objectPath(newHash)));
        QCOMPARE(m_store->table().ref(destination("mods/mod.jar").key())->hash, newHash);
        // the other instance keeps the old version, which stays read-only
        QCOMPARE(readFile(path("b/mods/mod.jar")), "version 1");
        QVERIFY(!QFileInfo(m_store->objectPath(oldHash)).isWritable());
        QVERIFY(!QFileInfo(path("b/mods/mod.jar")).isWritable());
        QVERIFY(identityIsCurrent(oldHash));
        QVERIFY(identityIsCurrent(newHash));
    }

    void test_samePlacementAgainKeepsRef()
    {
        const auto hash = store("again");
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar"), hash, {} }));
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar"), hash, {} }));
        const auto ref = m_store->table().ref(destination("mods/mod.jar").key());
        QVERIFY(ref);
        QCOMPARE(ref->hash, hash);
        QVERIFY(sameFile(path("a/mods/mod.jar"), m_store->objectPath(hash)));
    }

    void test_sameHashHardToSymbolic()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto hash = store("switch");
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar"), hash, {} }));
        ContentStore::PlaceOptions options;
        options.mode = ContentStore::LinkMode::SymbolicLinks;
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar"), hash, {} }, options));
        const auto ref = m_store->table().ref(destination("mods/mod.jar").key());
        QVERIFY(ref);
        QCOMPARE(ref->kind, LinkKind::Symbolic);
        QVERIFY(QFileInfo(path("a/mods/mod.jar")).isSymbolicLink());
        QVERIFY(identityIsCurrent(hash));
    }

    void test_unconfirmedLocalFileIsKept()
    {
        const auto hash = store("shared");
        QVERIFY(writeFile(path("a/mods/mod.jar"), "local edits"));
        const auto placed = m_store->placeAt({ destination("mods/mod.jar"), hash, {} });
        QVERIFY(!placed);
        QCOMPARE(readFile(path("a/mods/mod.jar")), "local edits");
        QVERIFY(m_store->table().refs().isEmpty());
        QVERIFY(m_store->table().transactions().isEmpty());
    }

    void test_confirmedLocalFileIsReplaced()
    {
        const auto hash = store("shared");
        QVERIFY(writeFile(path("a/mods/mod.jar"), "local"));
        const auto chosen = FS::identity(path("a/mods/mod.jar"));
        QVERIFY(chosen);
        const auto placed = m_store->placeAt({ destination("mods/mod.jar"), hash, *chosen });
        QVERIFY2(placed, placed ? "" : qPrintable(placed.error()));
        QVERIFY(sameFile(path("a/mods/mod.jar"), m_store->objectPath(hash)));
        QVERIFY(m_store->table().ref(destination("mods/mod.jar").key()));
    }

    void test_localFileChangedAfterConfirmationIsKept()
    {
        const auto hash = store("shared");
        QVERIFY(writeFile(path("a/mods/mod.jar"), "local"));
        const auto chosen = FS::identity(path("a/mods/mod.jar"));
        QVERIFY(chosen);
        QVERIFY(writeFile(path("a/mods/mod.jar"), "edited after the user confirmed"));
        const auto placed = m_store->placeAt({ destination("mods/mod.jar"), hash, *chosen });
        QVERIFY(!placed);
        QCOMPARE(readFile(path("a/mods/mod.jar")), "edited after the user confirmed");
        QVERIFY(m_store->table().refs().isEmpty());
    }

    void test_folderIsNeverReplaced()
    {
        const auto hash = store("file");
        QVERIFY(QDir(path("a")).mkpath("shaderpacks/pack"));
        QVERIFY(!m_store->placeAt({ destination("shaderpacks/pack"), hash, {} }));
        QVERIFY(QFileInfo(path("a/shaderpacks/pack")).isDir());
    }

    void test_failedLinkKeepsDestination()
    {
        const auto oldHash = store("old");
        const auto newHash = store("new");
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar"), oldHash, {} }));
        const auto before = FS::fileId(path("a/mods/mod.jar"));

        disableLinks();
        ContentStore::PlaceOptions options;
        options.allowCopy = false;
        QVERIFY(!m_store->placeAt({ destination("mods/mod.jar"), newHash, {} }, options));
        QCOMPARE(FS::fileId(path("a/mods/mod.jar")), before);
        QCOMPARE(m_store->table().ref(destination("mods/mod.jar").key())->hash, oldHash);
        QVERIFY(m_store->table().transactions().isEmpty());
        QVERIFY(leftovers("a/mods").isEmpty());
    }

    void test_failedSwapKeepsDestination()
    {
        const auto oldHash = store("old");
        const auto newHash = store("new");
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar"), oldHash, {} }));
        const auto before = FS::fileId(path("a/mods/mod.jar"));
        const auto destinationPath = path("a/mods/mod.jar");
        FS::Testing::setFaultHook([&destinationPath](FS::Testing::Operation operation, const QString& target) {
            return operation == FS::Testing::Operation::Replace &&
                   QFileInfo(target).absoluteFilePath() == QFileInfo(destinationPath).absoluteFilePath();
        });

        QVERIFY(!m_store->placeAt({ destination("mods/mod.jar"), newHash, {} }));
        QCOMPARE(FS::fileId(path("a/mods/mod.jar")), before);
        QCOMPARE(readFile(path("a/mods/mod.jar")), "old");
        QCOMPARE(m_store->table().ref(destination("mods/mod.jar").key())->hash, oldHash);
        QVERIFY(m_store->table().transactions().isEmpty());
        QVERIFY(leftovers("a/mods").isEmpty());
        // the failed swap left every link read-only
        QVERIFY(!QFileInfo(path("a/mods/mod.jar")).isWritable());
        QVERIFY(!QFileInfo(m_store->objectPath(newHash)).isWritable());
    }

    void test_batchFailuresAreIndependent()
    {
        const auto hash = store("batch");
        QVERIFY(writeFile(path("a/mods/local.jar"), "local"));
        const auto results = m_store->place({ { destination("mods/local.jar"), hash, {} }, { destination("mods/new.jar"), hash, {} } });
        QVERIFY(!results[0]);
        QVERIFY(results[1]);
        QCOMPARE(readFile(path("a/mods/local.jar")), "local");
        QVERIFY(sameFile(path("a/mods/new.jar"), m_store->objectPath(hash)));
    }

    void test_duplicateDestinationInBatch()
    {
        const auto hash = store("twice");
        const auto results = m_store->place({ { destination("mods/mod.jar"), hash, {} }, { destination("mods/mod.jar"), hash, {} } });
        QVERIFY(results[0]);
        QVERIFY(!results[1]);
        QCOMPARE(m_store->table().refs().size(), 1);
    }

    void test_replayAtTheSamePath_data()
    {
        QTest::addColumn<Step>("step");
        QTest::addColumn<bool>("swapped");
        QTest::newRow("begun") << Step::Begun << false;
        QTest::newRow("created") << Step::Created << false;
        QTest::newRow("prepared") << Step::Prepared << false;
        QTest::newRow("swapped") << Step::Swapped << true;
    }

    void test_replayAtTheSamePath()
    {
        QFETCH(Step, step);
        QFETCH(bool, swapped);
        const auto oldHash = store("old");
        const auto newHash = store("new");
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar"), oldHash, {} }));

        m_store->setInterruptionForTesting([step](Step reached) { return reached == step; });
        QVERIFY(!m_store->placeAt({ destination("mods/mod.jar"), newHash, {} }));
        reopen();

        const auto expected = swapped ? newHash : oldHash;
        QVERIFY(sameFile(path("a/mods/mod.jar"), m_store->objectPath(expected)));
        const auto ref = m_store->table().ref(destination("mods/mod.jar").key());
        QVERIFY(ref);
        QCOMPARE(ref->hash, expected);
        QCOMPARE(ref->state, RefState::Live);
        QVERIFY(m_store->table().transactions().isEmpty());
        QVERIFY(leftovers("a/mods").isEmpty());
    }

    void test_replayOfANewPath_data()
    {
        QTest::addColumn<Step>("step");
        QTest::addColumn<bool>("swapped");
        QTest::newRow("begun") << Step::Begun << false;
        QTest::newRow("created") << Step::Created << false;
        QTest::newRow("prepared") << Step::Prepared << false;
        QTest::newRow("swapped") << Step::Swapped << true;
    }

    void test_replayOfANewPath()
    {
        QFETCH(Step, step);
        QFETCH(bool, swapped);
        const auto hash = store("new");
        m_store->setInterruptionForTesting([step](Step reached) { return reached == step; });
        QVERIFY(!m_store->placeAt({ destination("mods/mod.jar"), hash, {} }));
        reopen();

        QCOMPARE(QFileInfo::exists(path("a/mods/mod.jar")), swapped);
        QCOMPARE(m_store->table().ref(destination("mods/mod.jar").key()).has_value(), swapped);
        QVERIFY(m_store->table().transactions().isEmpty());
        QVERIFY(leftovers("a/mods").isEmpty());
    }

    void test_replayOfACopyUsesThePreparedKind()
    {
        const auto oldHash = store("old");
        const auto newHash = store("new");
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar"), oldHash, {} }));

        disableLinks();
        m_store->setInterruptionForTesting([](Step reached) { return reached == Step::Swapped; });
        QVERIFY(!m_store->placeAt({ destination("mods/mod.jar"), newHash, {} }));
        // the copy is the user's file now, and changing it doesn't stop the placement from being recognized
        {
            QFile file(path("a/mods/mod.jar"));
            QVERIFY(file.open(QIODevice::Append));
            file.write(" and more");
        }
        reopen();

        QCOMPARE(readFile(path("a/mods/mod.jar")), "new and more");
        QVERIFY(!m_store->table().ref(destination("mods/mod.jar").key()));
        QVERIFY(m_store->table().transactions().isEmpty());
    }

    void test_copyCrashBeforePreparedRollsBack()
    {
        const auto hash = store("copy");
        disableLinks();
        m_store->setInterruptionForTesting([](Step reached) { return reached == Step::Created; });
        QVERIFY(!m_store->placeAt({ destination("mods/mod.jar"), hash, {} }));
        // the copy exists next to the destination
        QCOMPARE(leftovers("a/mods").size(), 1);
        reopen();
        QVERIFY(!QFileInfo::exists(path("a/mods/mod.jar")));
        QVERIFY(leftovers("a/mods").isEmpty());
        QVERIFY(m_store->table().transactions().isEmpty());
    }

    void test_localCommitRemovesTheRef()
    {
        const auto oldHash = store("old");
        const auto newHash = store("new");
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar"), oldHash, {} }));
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar", "b"), oldHash, {} }));
        QCOMPARE(m_store->table().refs().size(), 2);

        disableLinks();
        const auto placed = m_store->placeAt({ destination("mods/mod.jar"), newHash, {} });
        QVERIFY(placed);
        QCOMPARE(*placed, PlacementKind::Local);
        QVERIFY(!m_store->table().ref(destination("mods/mod.jar").key()));
        QCOMPARE(m_store->table().refs().size(), 1);
        QCOMPARE(readFile(path("a/mods/mod.jar")), "new");
    }

    void test_unshareKeepsTheBytes()
    {
        const auto hash = store("shared bytes");
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar"), hash, {} }));
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar", "b"), hash, {} }));

        const auto unshared = m_store->unshare(destination("mods/mod.jar").key());
        QVERIFY2(unshared, unshared ? "" : qPrintable(unshared.error()));
        QCOMPARE(unshared->hash, hash);
        QCOMPARE(unshared->contentHash, hash);

        QCOMPARE(readFile(path("a/mods/mod.jar")), "shared bytes");
        QVERIFY(!sameFile(path("a/mods/mod.jar"), m_store->objectPath(hash)));
        QVERIFY(QFileInfo(path("a/mods/mod.jar")).isWritable());
        QVERIFY(!m_store->table().ref(destination("mods/mod.jar").key()));
        // the other instance and the stored file are untouched
        QVERIFY(sameFile(path("b/mods/mod.jar"), m_store->objectPath(hash)));
        QVERIFY(!QFileInfo(m_store->objectPath(hash)).isWritable());
        QVERIFY(identityIsCurrent(hash));
        QVERIFY(leftovers("a/mods").isEmpty());

        // editing the local copy doesn't reach the other instance
        QVERIFY(writeFile(path("a/mods/mod.jar"), "edited"));
        QCOMPARE(readFile(path("b/mods/mod.jar")), "shared bytes");
    }

    void test_unshareKeepsEditedBytes()
    {
        const auto hash = store("original");
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar"), hash, {} }));
        // edited in place through the link, so the stored file no longer has the bytes its hash names
        makeWritable(path("a/mods/mod.jar"));
        QVERIFY(writeFile(path("a/mods/mod.jar"), "edited in place"));

        const auto unshared = m_store->unshare(destination("mods/mod.jar").key());
        QVERIFY2(unshared, unshared ? "" : qPrintable(unshared.error()));
        QCOMPARE(unshared->hash, hash);
        QCOMPARE(unshared->contentHash, sha256Of("edited in place"));
        QCOMPARE(readFile(path("a/mods/mod.jar")), "edited in place");
        QVERIFY(!sameFile(path("a/mods/mod.jar"), m_store->objectPath(hash)));
    }

    void test_unshareOfAReplacedFileChangesNothing()
    {
        const auto hash = store("shared");
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar"), hash, {} }));
        QVERIFY(FS::deleteLink(path("a/mods/mod.jar")));
        QVERIFY(writeFile(path("a/mods/mod.jar"), "someone else's file"));

        QVERIFY(!m_store->unshare(destination("mods/mod.jar").key()));
        QCOMPARE(readFile(path("a/mods/mod.jar")), "someone else's file");
        QCOMPARE(m_store->table().ref(destination("mods/mod.jar").key())->state, RefState::Replaced);
        QVERIFY(m_store->table().transactions().isEmpty());
        QVERIFY(leftovers("a/mods").isEmpty());
    }

    void test_unshareOfAMissingFile()
    {
        const auto hash = store("shared");
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar"), hash, {} }));
        QVERIFY(FS::deleteLink(path("a/mods/mod.jar")));
        QVERIFY(!m_store->unshare(destination("mods/mod.jar").key()));
        QCOMPARE(m_store->table().ref(destination("mods/mod.jar").key())->state, RefState::Missing);
    }

    void test_unshareChecksTheCopy()
    {
        const auto hash = store("shared");
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar"), hash, {} }));
        const auto before = FS::fileId(path("a/mods/mod.jar"));
        // damage the copy after it was written, before it is checked
        FS::Testing::setFaultHook([](FS::Testing::Operation operation, const QString& target) {
            if (operation == FS::Testing::Operation::FlushFile && QFileInfo(target).fileName().startsWith(".prism-new-")) {
                writeFile(target, "damaged");
            }
            return false;
        });

        QVERIFY(!m_store->unshare(destination("mods/mod.jar").key()));
        QCOMPARE(FS::fileId(path("a/mods/mod.jar")), before);
        QCOMPARE(m_store->table().ref(destination("mods/mod.jar").key())->hash, hash);
        QVERIFY(m_store->table().transactions().isEmpty());
        QVERIFY(leftovers("a/mods").isEmpty());
    }

    void test_unshareAbortsWhenTheFileIsReplaced()
    {
        const auto hash = store("shared");
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar"), hash, {} }));
        const auto destinationPath = path("a/mods/mod.jar");
        // replaced while the copy is made
        bool replaced = false;
        FS::Testing::setFaultHook([&destinationPath, &replaced](FS::Testing::Operation operation, const QString& target) {
            if (!replaced && operation == FS::Testing::Operation::FlushFile && QFileInfo(target).fileName().startsWith(".prism-new-")) {
                replaced = true;
                if (!FS::deleteLink(destinationPath)) {
                    return true;
                }
                writeFile(destinationPath, "replaced meanwhile");
            }
            return false;
        });

        QVERIFY(!m_store->unshare(destination("mods/mod.jar").key()));
        QCOMPARE(readFile(destinationPath), "replaced meanwhile");
        QVERIFY(m_store->table().transactions().isEmpty());
        QVERIFY(leftovers("a/mods").isEmpty());
    }

    void test_unshareOfASymbolicLink()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto hash = store("linked pack");
        ContentStore::PlaceOptions options;
        options.mode = ContentStore::LinkMode::SymbolicLinks;
        QVERIFY(m_store->placeAt({ destination("resourcepacks/pack.zip"), hash, {} }, options));

        const auto unshared = m_store->unshare(destination("resourcepacks/pack.zip").key());
        QVERIFY2(unshared, unshared ? "" : qPrintable(unshared.error()));
        const QFileInfo info(path("a/resourcepacks/pack.zip"));
        QVERIFY(!info.isSymbolicLink());
        QVERIFY(info.isWritable());
        QCOMPARE(readFile(info.filePath()), "linked pack");
        QVERIFY(!m_store->table().ref(destination("resourcepacks/pack.zip").key()));
    }

    void test_damagedFileIsNotTrustedAfterUnsharing()
    {
        const auto hash = store("original");
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar"), hash, {} }));
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar", "b"), hash, {} }));
        // edited in place through a link, which damages the stored file
        makeWritable(path("a/mods/mod.jar"));
        QVERIFY(writeFile(path("a/mods/mod.jar"), "damaged"));
        QVERIFY(m_store->unshare(destination("mods/mod.jar").key()));
        // removing the link must not make the damaged file look checked
        QVERIFY(!identityIsCurrent(hash));

        QVERIFY(writeFile(path("downloads/again.jar"), "original"));
        const auto again = m_store->ingest(path("downloads/again.jar"), ContentStore::IngestMode::Copy);
        QVERIFY(!again || again->rehashedObject);
        if (again) {
            QCOMPARE(readFile(m_store->objectPath(again->hash)), "original");
        }
    }

    void test_damagedFileIsNotTrustedAfterPlacing()
    {
        const auto hash = store("original");
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar"), hash, {} }));
        makeWritable(path("a/mods/mod.jar"));
        QVERIFY(writeFile(path("a/mods/mod.jar"), "damaged"));
        // linking it again must not record the damaged file as checked
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar", "b"), hash, {} }));
        QVERIFY(!identityIsCurrent(hash));
    }

    void test_unshareOfASymbolicLinkWhoseTargetChanges()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto hash = store("linked pack");
        ContentStore::PlaceOptions options;
        options.mode = ContentStore::LinkMode::SymbolicLinks;
        QVERIFY(m_store->placeAt({ destination("resourcepacks/pack.zip"), hash, {} }, options));
        const auto object = m_store->objectPath(hash);
        // the stored file is edited after it was copied
        bool edited = false;
        FS::Testing::setFaultHook([&object, &edited](FS::Testing::Operation operation, const QString& target) {
            if (!edited && operation == FS::Testing::Operation::FlushFile && QFileInfo(target).fileName().startsWith(".prism-new-")) {
                edited = true;
                makeWritable(object);
                writeFile(object, "edited after the copy");
            }
            return false;
        });

        QVERIFY(!m_store->unshare(destination("resourcepacks/pack.zip").key()));
        QVERIFY(edited);
        QVERIFY(QFileInfo(path("a/resourcepacks/pack.zip")).isSymbolicLink());
        QVERIFY(m_store->table().ref(destination("resourcepacks/pack.zip").key()));
        QVERIFY(m_store->table().transactions().isEmpty());
        QVERIFY(leftovers("a/resourcepacks").isEmpty());
    }

    void test_batchUpdateOfASharedFile()
    {
        const auto oldHash = store("version 1");
        const auto newHash = store("version 2");
        QVERIFY(m_store->place({ { destination("mods/mod.jar", "a"), oldHash, {} }, { destination("mods/mod.jar", "b"), oldHash, {} } })
                    .size() == 2);
        // updating both in one batch: each swap removes a link to the old file, changing its change time
        const auto results =
            m_store->place({ { destination("mods/mod.jar", "a"), newHash, {} }, { destination("mods/mod.jar", "b"), newHash, {} } });
        for (const auto& result : results) {
            QVERIFY2(result, result ? "" : qPrintable(result.error()));
        }
        QVERIFY(sameFile(path("a/mods/mod.jar"), m_store->objectPath(newHash)));
        QVERIFY(sameFile(path("b/mods/mod.jar"), m_store->objectPath(newHash)));
    }

    void test_batchLinksTheFileItReplaces()
    {
        const auto oldHash = store("version 1");
        const auto newHash = store("version 2");
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar", "a"), oldHash, {} }));
        // linking the old file elsewhere in the same batch changes the change time of the link being replaced
        const auto results =
            m_store->place({ { destination("mods/old.jar", "b"), oldHash, {} }, { destination("mods/mod.jar", "a"), newHash, {} } });
        for (const auto& result : results) {
            QVERIFY2(result, result ? "" : qPrintable(result.error()));
        }
        QVERIFY(sameFile(path("a/mods/mod.jar"), m_store->objectPath(newHash)));
    }

    void test_unshareReplay_data()
    {
        QTest::addColumn<Step>("step");
        QTest::addColumn<bool>("swapped");
        QTest::newRow("begun") << Step::Begun << false;
        QTest::newRow("created") << Step::Created << false;
        QTest::newRow("prepared") << Step::Prepared << false;
        QTest::newRow("swapped") << Step::Swapped << true;
    }

    void test_unshareReplay()
    {
        QFETCH(Step, step);
        QFETCH(bool, swapped);
        const auto hash = store("shared");
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar"), hash, {} }));
        m_store->setInterruptionForTesting([step](Step reached) { return reached == step; });
        QVERIFY(!m_store->unshare(destination("mods/mod.jar").key()));
        reopen();

        QCOMPARE(readFile(path("a/mods/mod.jar")), "shared");
        QCOMPARE(sameFile(path("a/mods/mod.jar"), m_store->objectPath(hash)), !swapped);
        QCOMPARE(m_store->table().ref(destination("mods/mod.jar").key()).has_value(), !swapped);
        QVERIFY(m_store->table().transactions().isEmpty());
        QVERIFY(leftovers("a/mods").isEmpty());
    }

    void test_renameFollowsTheLink()
    {
        const auto hash = store("toggled");
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar"), hash, {} }));
        QVERIFY(QFile::rename(path("a/mods/mod.jar"), path("a/mods/mod.jar.disabled")));

        const auto renamed = m_store->renameRef(destination("mods/mod.jar").key(), "mods/mod.jar.disabled");
        QVERIFY2(renamed, renamed ? "" : qPrintable(renamed.error()));
        QVERIFY(!m_store->table().ref(destination("mods/mod.jar").key()));
        QCOMPARE(m_store->table().ref(destination("mods/mod.jar.disabled").key())->hash, hash);

        // the rename is journaled
        reopen();
        QCOMPARE(m_store->table().ref(destination("mods/mod.jar.disabled").key())->hash, hash);
        QVERIFY(!m_store->table().ref(destination("mods/mod.jar").key()));
    }

    void test_renameNeedsTheLinkAtTheNewPath()
    {
        const auto hash = store("toggled");
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar"), hash, {} }));
        QVERIFY(writeFile(path("a/mods/other.jar"), "other"));
        QVERIFY(!m_store->renameRef(destination("mods/mod.jar").key(), "mods/other.jar"));
        QVERIFY(m_store->table().ref(destination("mods/mod.jar").key()));
        QVERIFY(!m_store->table().ref(destination("mods/other.jar").key()));
    }

    void test_renameOfAnUnsharedFileDoesNothing()
    {
        QVERIFY(m_store->renameRef(destination("mods/local.jar").key(), "mods/local.jar.disabled"));
        QVERIFY(m_store->table().refs().isEmpty());
    }

    void test_allowListKeepsExistingEntries()
    {
        QVERIFY(writeFile(SymlinkAllowList::path(path("a")), "[prefix]/somewhere/else"));
        QVERIFY(SymlinkAllowList::allow(path("a"), path("store")));
        QVERIFY(SymlinkAllowList::allow(path("a"), path("store")));
        const auto lines = QString::fromUtf8(readFile(SymlinkAllowList::path(path("a")))).split('\n', Qt::SkipEmptyParts);
        QCOMPARE(lines, QStringList({ "[prefix]/somewhere/else", SymlinkAllowList::entryFor(path("store")) }));
        QVERIFY(SymlinkAllowList::entryFor(path("store")).endsWith(QDir::separator()));
    }

    void test_allowListThatIsALinkIsReplaced()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        // an instance copied with links shares the list of the instance it came from
        QVERIFY(writeFile(SymlinkAllowList::path(path("b")), "original\n"));
        QVERIFY(FS::createSymbolicLink(QFileInfo(SymlinkAllowList::path(path("b"))).absoluteFilePath(), SymlinkAllowList::path(path("a"))));
        QVERIFY(SymlinkAllowList::allow(path("a"), path("store")));
        QVERIFY(!QFileInfo(SymlinkAllowList::path(path("a"))).isSymbolicLink());
        QCOMPARE(readFile(SymlinkAllowList::path(path("b"))), "original\n");
    }

#if defined(Q_OS_WIN)
    void test_fileInUseIsNotReplaced()
    {
        const auto hash = store("shared");
        QVERIFY(writeFile(path("a/mods/mod.jar"), "local"));
        const auto chosen = FS::identity(path("a/mods/mod.jar"));
        QVERIFY(chosen);
        // another program has it open for writing
        QFile writer(path("a/mods/mod.jar"));
        QVERIFY(writer.open(QIODevice::ReadWrite));

        QVERIFY(!m_store->placeAt({ destination("mods/mod.jar"), hash, *chosen }));
        writer.close();
        QCOMPARE(readFile(path("a/mods/mod.jar")), "local");
        QVERIFY(m_store->table().refs().isEmpty());
        QVERIFY(leftovers("a/mods").isEmpty());
    }
#endif
};

QTEST_GUILESS_MAIN(PlacementTest)

#include "Placement_test.moc"
