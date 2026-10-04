#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "FileSystem.h"
#include "FileSystemPrimitives.h"
#include "contentstore/ContentStore.h"
#include "contentstore/ObjectFiles.h"
#include "contentstore/StoreTasks.h"

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
}  // namespace

class DeepVerifyTest : public QObject {
    Q_OBJECT

    // in the working directory, so hard links don't cross a tmpfs boundary
    QTemporaryDir m_dir{ QDir::current().filePath("deep_verify_test_XXXXXX") };
    std::unique_ptr<ContentStore> m_store;
    qint64 m_time = 0;
    bool m_canCreateSymbolicLinks = false;

    QString path(const QString& name) const { return m_dir.filePath(name); }

    ContentStore::Destination destination(const QString& relativePath, const QString& instance = "a") const
    {
        return { m_store->instanceOwner(instance), path(instance), relativePath };
    }

    RefKey key(const QString& relativePath, const QString& instance = "a") const { return destination(relativePath, instance).key(); }

    QString store(const QByteArray& data)
    {
        const auto source = path("downloads/" + sha256Of(data));
        if (!writeFile(source, data)) {
            return {};
        }
        const auto result = m_store->ingest(source, ContentStore::IngestMode::Copy);
        if (!result) {
            qWarning() << result.error();
        }
        return result ? result->hash : QString();
    }

    bool place(const QString& hash, const QString& relativePath, const QString& instance, LinkKind kind)
    {
        ContentStore::PlaceOptions options;
        options.mode = kind == LinkKind::Hard ? ContentStore::LinkMode::HardLinks : ContentStore::LinkMode::SymbolicLinks;
        options.allowCopy = false;
        const auto placed = m_store->placeAt({ destination(relativePath, instance), hash, {} }, options);
        if (!placed) {
            qWarning() << placed.error();
        }
        return placed.has_value();
    }

    // Changes the stored file's bytes in place, through the same file, as an edit through a link would
    void tamper(const QString& hash, const QByteArray& data)
    {
        const auto object = m_store->objectPath(hash);
        makeWritable(object);
        writeFile(object, data);
        ObjectFiles::makeReadOnly(object);
    }

    // Records the stored file's current identity, as if the change went unnoticed by the quick check
    void hideChange(const QString& hash)
    {
        const auto identity = FS::identity(m_store->objectPath(hash));
        QVERIFY(identity);
        QVERIFY(m_store->commit(
            { RefRecord::updateIdentity(hash, m_store->table().entries()[hash].current->id, StoredIdentity::from(*identity)) }));
    }

    QString retiredPath(const QString& hash, int generation) const
    {
        return QDir(path("store")).absoluteFilePath(QString("retired/%1/%2.%3").arg(hash.left(2), hash).arg(generation));
    }

    QString linkTarget(const QString& relativePath, const QString& instance = "a") const
    {
        return QFileInfo(path(instance + "/" + relativePath)).symLinkTarget();
    }

    int currentGeneration(const QString& hash) const { return m_store->table().entries()[hash].current->id; }

    QList<RefKey> affectedKeys(const ContentStore::DeepVerifyReport& report) const
    {
        QList<RefKey> keys;
        for (const auto& affected : report.affected) {
            keys.append(affected.key);
        }
        std::sort(keys.begin(), keys.end());
        return keys;
    }

    ContentStore::DeepVerifyReport deepVerify()
    {
        const auto report = m_store->deepVerify();
        if (!report) {
            qWarning() << report.error();
            return {};
        }
        return *report;
    }

    void openStore()
    {
        m_store = std::make_unique<ContentStore>(path("store"), path("data"));
        m_store->setClockForTesting([this] { return m_time; });
        QCOMPARE(m_store->open(), ContentStore::State::Writable);
    }

    void reopen()
    {
        FS::Testing::setFaultHook(nullptr);
        m_store.reset();
        openStore();
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
        for (const auto* dir : { "data", "downloads", "a/mods", "b/mods" }) {
            QVERIFY(QDir(m_dir.path()).mkpath(dir));
        }
        m_time = QDateTime::currentSecsSinceEpoch();
        openStore();
    }

    void cleanup()
    {
        FS::Testing::setFaultHook(nullptr);
        if (m_store) {
            m_store->setInterruptionForTesting(nullptr);
        }
    }

    void test_intactStoreIsClean()
    {
        const auto hash = store("intact");
        QVERIFY(place(hash, "mods/mod.jar", "a", LinkKind::Hard));
        const auto report = deepVerify();
        QCOMPARE(report.checked, 1);
        QVERIFY(report.damaged.isEmpty());
        QVERIFY(report.affected.isEmpty());
        QVERIFY(m_store->table().entries()[hash].current);
    }

    void test_deepVerifyFindsWhatTheQuickCheckMisses()
    {
        const auto hash = store("original");
        QVERIFY(place(hash, "mods/mod.jar", "a", LinkKind::Hard));
        tamper(hash, "tampered");
        hideChange(hash);

        // the quick check trusts the recorded identity
        QVERIFY(writeFile(path("downloads/again.jar"), "original"));
        const auto again = m_store->ingest(path("downloads/again.jar"), ContentStore::IngestMode::Copy);
        QVERIFY(again);
        QVERIFY(again->reusedObject && !again->rehashedObject);

        const auto generation = currentGeneration(hash);
        const auto report = deepVerify();
        QCOMPARE(report.damaged, QStringList{ hash });
        const auto& entry = m_store->table().entries()[hash];
        QVERIFY(!entry.current);
        QCOMPARE(entry.retired.size(), 1);
        QVERIFY(entry.retired.first().corrupt);
        QCOMPARE(readFile(retiredPath(hash, generation)), "tampered");
        QCOMPARE(affectedKeys(report), QList<RefKey>{ key("mods/mod.jar") });
        // the link keeps the bytes it had
        QVERIFY(sameFile(path("a/mods/mod.jar"), retiredPath(hash, generation)));
        // and the file can't be placed until an intact copy is stored
        QVERIFY(!m_store->placeAt({ destination("mods/other.jar"), hash, {} }));
    }

    void test_inPlaceEditReachesEveryHardLink()
    {
        const auto hash = store("shared");
        QVERIFY(place(hash, "mods/mod.jar", "a", LinkKind::Hard));
        QVERIFY(place(hash, "mods/mod.jar", "b", LinkKind::Hard));
        makeWritable(path("a/mods/mod.jar"));
        QVERIFY(writeFile(path("a/mods/mod.jar"), "edited in a"));

        // still the same file at both paths, so the links are intact
        const auto verified = m_store->verify();
        QVERIFY(verified);
        QVERIFY(verified->missing.isEmpty() && verified->replaced.isEmpty());

        const auto report = deepVerify();
        QCOMPARE(report.damaged, QStringList{ hash });
        QCOMPARE(affectedKeys(report), QList<RefKey>({ key("mods/mod.jar", "a"), key("mods/mod.jar", "b") }));
        QCOMPARE(readFile(path("b/mods/mod.jar")), "edited in a");
    }

    void test_repairThenVerifyAgain()
    {
        const auto hash = store("original");
        QVERIFY(place(hash, "mods/mod.jar", "a", LinkKind::Hard));
        QVERIFY(place(hash, "mods/mod.jar", "b", LinkKind::Hard));
        const auto damagedGeneration = currentGeneration(hash);
        tamper(hash, "edited");
        QCOMPARE(deepVerify().damaged, QStringList{ hash });

        // an intact copy is stored again
        QVERIFY(!store("original").isEmpty());
        QVERIFY(m_store->table().entries()[hash].current);
        QCOMPARE(readFile(m_store->objectPath(hash)), "original");
        QCOMPARE(readFile(retiredPath(hash, damagedGeneration)), "edited");

        // the damaged copy is still reported for both, until they choose
        auto report = deepVerify();
        QVERIFY(report.damaged.isEmpty());
        QCOMPARE(report.affected.size(), 2);

        // restore original in a
        const auto restored = m_store->restoreOriginal(key("mods/mod.jar", "a"));
        QVERIFY2(restored, restored ? "" : qPrintable(restored.error()));
        QCOMPARE(readFile(path("a/mods/mod.jar")), "original");
        QCOMPARE(m_store->table().ref(key("mods/mod.jar", "a"))->generation, currentGeneration(hash));
        report = deepVerify();
        QCOMPARE(affectedKeys(report), QList<RefKey>{ key("mods/mod.jar", "b") });

        // keep the edited version locally in b
        const auto kept = m_store->unshare(key("mods/mod.jar", "b"));
        QVERIFY2(kept, kept ? "" : qPrintable(kept.error()));
        QCOMPARE(kept->contentHash, sha256Of("edited"));
        QCOMPARE(readFile(path("b/mods/mod.jar")), "edited");

        // nothing uses the damaged copy anymore, so it is gone
        QVERIFY(m_store->table().entries()[hash].retired.isEmpty());
        QVERIFY(!QFileInfo::exists(retiredPath(hash, damagedGeneration)));
        report = deepVerify();
        QVERIFY(report.damaged.isEmpty() && report.affected.isEmpty());
        QCOMPARE(readFile(path("a/mods/mod.jar")), "original");
    }

    void test_damagedCopyNobodyUsesIsDestroyed()
    {
        const auto hash = store("original");
        QVERIFY(place(hash, "mods/mod.jar", "a", LinkKind::Hard));
        tamper(hash, "edited");
        deepVerify();
        // kept locally, the damaged copy and its canonical path go
        QVERIFY(m_store->unshare(key("mods/mod.jar")));
        QVERIFY(!m_store->table().entries().contains(hash));
        QVERIFY(!QFileInfo::exists(m_store->objectPath(hash)));
        QCOMPARE(readFile(path("a/mods/mod.jar")), "edited");
    }

    void test_symbolicLinksKeepTheEditedBytes()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto hash = store("original pack");
        QVERIFY(place(hash, "resourcepacks/pack.zip", "a", LinkKind::Symbolic));
        QVERIFY(place(hash, "resourcepacks/pack.zip", "b", LinkKind::Symbolic));
        const auto damagedGeneration = currentGeneration(hash);
        tamper(hash, "edited pack");

        const auto report = deepVerify();
        QCOMPARE(report.damaged, QStringList{ hash });
        QCOMPARE(readFile(retiredPath(hash, damagedGeneration)), "edited pack");
        QVERIFY(ObjectFiles::samePath(linkTarget("resourcepacks/pack.zip", "a"), retiredPath(hash, damagedGeneration)));
        QVERIFY(ObjectFiles::samePath(linkTarget("resourcepacks/pack.zip", "b"), retiredPath(hash, damagedGeneration)));

        // an intact copy doesn't change what they see
        QVERIFY(!store("original pack").isEmpty());
        QCOMPARE(readFile(path("a/resourcepacks/pack.zip")), "edited pack");
        QCOMPARE(readFile(path("b/resourcepacks/pack.zip")), "edited pack");

        QVERIFY(m_store->unshare(key("resourcepacks/pack.zip", "a")));
        QCOMPARE(readFile(path("a/resourcepacks/pack.zip")), "edited pack");
        QVERIFY(!QFileInfo(path("a/resourcepacks/pack.zip")).isSymbolicLink());

        QVERIFY(m_store->restoreOriginal(key("resourcepacks/pack.zip", "b")));
        QCOMPARE(readFile(path("b/resourcepacks/pack.zip")), "original pack");
        QVERIFY(ObjectFiles::samePath(linkTarget("resourcepacks/pack.zip", "b"), m_store->objectPath(hash)));

        // symbolic links may have been copied anywhere, so the kept copy waits for a complete scan
        QVERIFY(QFileInfo::exists(retiredPath(hash, damagedGeneration)));
        m_time += 1;
        QVERIFY(m_store->reconcile({})->complete);
        QVERIFY(!QFileInfo::exists(retiredPath(hash, damagedGeneration)));
        QCOMPARE(readFile(path("b/resourcepacks/pack.zip")), "original pack");
    }

    void test_intactCopyWaitsForLinksToBePointedAside()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto hash = store("original pack");
        QVERIFY(place(hash, "resourcepacks/pack.zip", "a", LinkKind::Symbolic));
        tamper(hash, "edited pack");
        // the link can't be changed right now
        FS::Testing::setFaultHook(
            [](FS::Testing::Operation operation, const QString&) { return operation == FS::Testing::Operation::SymbolicLink; });
        QCOMPARE(deepVerify().damaged, QStringList{ hash });
        FS::Testing::setFaultHook(nullptr);
        QVERIFY(ObjectFiles::samePath(linkTarget("resourcepacks/pack.zip"), m_store->objectPath(hash)));

        // storing an intact copy now would switch the link's bytes without its user choosing
        QVERIFY(writeFile(path("downloads/again.zip"), "original pack"));
        QVERIFY(!m_store->ingest(path("downloads/again.zip"), ContentStore::IngestMode::Copy));
        QCOMPARE(readFile(path("a/resourcepacks/pack.zip")), "edited pack");

        // the next check finishes pointing the link aside
        deepVerify();
        QVERIFY(m_store->ingest(path("downloads/again.zip"), ContentStore::IngestMode::Copy));
        QCOMPARE(readFile(path("a/resourcepacks/pack.zip")), "edited pack");
    }

    void test_pendingLinkIsKeptByReconciliation()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto hash = store("original pack");
        QVERIFY(place(hash, "resourcepacks/pack.zip", "a", LinkKind::Symbolic));
        const auto generation = currentGeneration(hash);
        tamper(hash, "edited pack");
        FS::Testing::setFaultHook(
            [](FS::Testing::Operation operation, const QString&) { return operation == FS::Testing::Operation::SymbolicLink; });
        deepVerify();
        // the link still points at the canonical path, which is still the damaged file
        QVERIFY(ObjectFiles::samePath(linkTarget("resourcepacks/pack.zip"), m_store->objectPath(hash)));

        const auto verified = m_store->verify();
        QVERIFY(verified);
        QVERIFY(verified->replaced.isEmpty() && verified->missing.isEmpty());
        for (int day = 1; day <= 3; day++) {
            m_time += 2 * 24 * 60 * 60;
            QVERIFY(m_store->reconcile({}));
        }
        QVERIFY(m_store->table().ref(key("resourcepacks/pack.zip")));
        QVERIFY(QFileInfo::exists(retiredPath(hash, generation)));
        QCOMPARE(readFile(path("a/resourcepacks/pack.zip")), "edited pack");

        // once the link can be changed, it is pointed at the kept copy
        FS::Testing::setFaultHook(nullptr);
        deepVerify();
        QVERIFY(ObjectFiles::samePath(linkTarget("resourcepacks/pack.zip"), retiredPath(hash, generation)));
    }

    void test_unreadableLinkBlocksTheIntactCopy()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto hash = store("original pack");
        QVERIFY(place(hash, "resourcepacks/pack.zip", "a", LinkKind::Symbolic));
        tamper(hash, "edited pack");
        FS::Testing::setFaultHook(
            [](FS::Testing::Operation operation, const QString&) { return operation == FS::Testing::Operation::SymbolicLink; });
        deepVerify();
        FS::Testing::setFaultHook(nullptr);

        QVERIFY(writeFile(path("downloads/again.zip"), "original pack"));

        // the instance folder can't be read for a while, so its link can't be checked
        const auto root = QFileInfo(path("a")).absoluteFilePath();
        m_store->setUnreadableForTesting([root](const QString& candidate) { return candidate == root; });
        QVERIFY(!m_store->ingest(path("downloads/again.zip"), ContentStore::IngestMode::Copy));
        m_store->setUnreadableForTesting(nullptr);

        // or is gone for a while, such as on a drive that was unplugged
        QVERIFY(QFile::rename(path("a"), path("a-away")));
        QVERIFY(!m_store->ingest(path("downloads/again.zip"), ContentStore::IngestMode::Copy));
        QVERIFY(QFile::rename(path("a-away"), path("a")));
        QCOMPARE(readFile(path("a/resourcepacks/pack.zip")), "edited pack");
    }

    void test_twoSuccessiveDamages()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto hash = store("original");
        QVERIFY(place(hash, "mods/a1.jar", "a", LinkKind::Symbolic));
        QVERIFY(place(hash, "mods/a2.jar", "a", LinkKind::Symbolic));
        QVERIFY(place(hash, "mods/a3.jar", "a", LinkKind::Hard));
        const auto first = currentGeneration(hash);
        tamper(hash, "first damage");
        deepVerify();

        QVERIFY(!store("original").isEmpty());
        const auto second = currentGeneration(hash);
        QVERIFY(place(hash, "mods/b1.jar", "b", LinkKind::Symbolic));
        QVERIFY(place(hash, "mods/b2.jar", "b", LinkKind::Hard));
        tamper(hash, "second damage");
        const auto report = deepVerify();
        QCOMPARE(report.damaged, QStringList{ hash });

        // only the links of the second damaged copy moved to it
        QVERIFY(ObjectFiles::samePath(linkTarget("mods/b1.jar", "b"), retiredPath(hash, second)));
        QCOMPARE(m_store->table().ref(key("mods/b2.jar", "b"))->generation, second);
        QVERIFY(ObjectFiles::samePath(linkTarget("mods/a1.jar"), retiredPath(hash, first)));
        QVERIFY(ObjectFiles::samePath(linkTarget("mods/a2.jar"), retiredPath(hash, first)));
        QCOMPARE(m_store->table().ref(key("mods/a3.jar"))->generation, first);
        QCOMPARE(readFile(path("a/mods/a1.jar")), "first damage");
        QCOMPARE(readFile(path("a/mods/a3.jar")), "first damage");
        QCOMPARE(readFile(path("b/mods/b1.jar")), "second damage");
        QCOMPARE(report.affected.size(), 5);

        // resolving the first group frees only the first damaged copy
        for (const auto* name : { "mods/a1.jar", "mods/a2.jar", "mods/a3.jar" }) {
            QVERIFY(m_store->unshare(key(name)));
        }
        m_time += 1;
        QVERIFY(m_store->reconcile({})->complete);
        QVERIFY(!QFileInfo::exists(retiredPath(hash, first)));
        QVERIFY(QFileInfo::exists(retiredPath(hash, second)));
        QCOMPARE(readFile(path("b/mods/b1.jar")), "second damage");
    }

    void test_retirementReplay_data()
    {
        QTest::addColumn<Step>("step");
        // after the kept copy got its own path, before the retirement was recorded
        QTest::newRow("kept copy linked") << Step::Begun;
        // while pointing a symbolic link at the kept copy
        QTest::newRow("link created") << Step::Created;
        QTest::newRow("link prepared") << Step::Prepared;
        QTest::newRow("link swapped") << Step::Swapped;
    }

    void test_retirementReplay()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        QFETCH(Step, step);
        const auto hash = store("original pack");
        QVERIFY(place(hash, "resourcepacks/pack.zip", "a", LinkKind::Symbolic));
        const auto generation = currentGeneration(hash);
        tamper(hash, "edited pack");

        m_store->setInterruptionForTesting([step](Step reached) { return reached == step; });
        // the launcher stops at the step, as if it crashed
        (void)m_store->deepVerify();
        reopen();
        // the user's link never sees other bytes, whatever the store's state
        QCOMPARE(readFile(path("a/resourcepacks/pack.zip")), "edited pack");
        QVERIFY(m_store->table().transactions().isEmpty());

        // the next check finishes the retirement
        const auto report = deepVerify();
        QCOMPARE(affectedKeys(report), QList<RefKey>{ key("resourcepacks/pack.zip") });
        QVERIFY(ObjectFiles::samePath(linkTarget("resourcepacks/pack.zip"), retiredPath(hash, generation)));
        QVERIFY(!store("original pack").isEmpty());
        QCOMPARE(readFile(path("a/resourcepacks/pack.zip")), "edited pack");
        QCOMPARE(m_store->table().ref(key("resourcepacks/pack.zip"))->generation, generation);
    }

    void test_danglingSymbolicLinkIsRepaired()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto hash = store("pack");
        QVERIFY(place(hash, "resourcepacks/pack.zip", "a", LinkKind::Symbolic));
        // the store was in another folder when the link was made
        QVERIFY(FS::deleteLink(path("a/resourcepacks/pack.zip")));
        QVERIFY(FS::createSymbolicLink(QDir(path("old-store/objects")).absoluteFilePath(hash.left(2) + "/" + hash),
                                       path("a/resourcepacks/pack.zip")));
        QCOMPARE(m_store->verify()->replaced.size(), 1);

        const auto repaired = m_store->repairSymbolicLinks();
        QVERIFY(repaired);
        QCOMPARE(repaired->retargeted, 1);
        QCOMPARE(readFile(path("a/resourcepacks/pack.zip")), "pack");
        QCOMPARE(m_store->table().ref(key("resourcepacks/pack.zip"))->state, RefState::Live);
        QVERIFY(m_store->verify()->replaced.isEmpty());
    }

    void test_unrelatedLinkIsNotRepaired()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto hash = store("pack");
        QVERIFY(place(hash, "resourcepacks/pack.zip", "a", LinkKind::Symbolic));
        QVERIFY(FS::deleteLink(path("a/resourcepacks/pack.zip")));
        QVERIFY(FS::createSymbolicLink(QDir(path("elsewhere")).absoluteFilePath("something-else.zip"), path("a/resourcepacks/pack.zip")));
        QCOMPARE(m_store->repairSymbolicLinks()->retargeted, 0);
        QVERIFY(
            ObjectFiles::samePath(linkTarget("resourcepacks/pack.zip"), QDir(path("elsewhere")).absoluteFilePath("something-else.zip")));
    }

    void test_lostFileIsReported()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto hash = store("pack");
        QVERIFY(place(hash, "resourcepacks/pack.zip", "a", LinkKind::Symbolic));
        makeWritable(m_store->objectPath(hash));
        QVERIFY(FS::deleteLink(m_store->objectPath(hash)));
        QCOMPARE(m_store->repairSymbolicLinks()->lost, QList<RefKey>{ key("resourcepacks/pack.zip") });
        QCOMPARE(deepVerify().missing, QStringList{ hash });
    }

    void test_deepVerifyTrustsWhatItHashed()
    {
        const auto hash = store("intact");
        QVERIFY(place(hash, "mods/mod.jar", "a", LinkKind::Hard));
        // something changed the metadata only, such as a backup tool
        QFile::setPermissions(m_store->objectPath(hash), QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser);
        ObjectFiles::makeReadOnly(m_store->objectPath(hash));
        QVERIFY(deepVerify().damaged.isEmpty());
        QVERIFY(writeFile(path("downloads/again.jar"), "intact"));
        const auto again = m_store->ingest(path("downloads/again.jar"), ContentStore::IngestMode::Copy);
        QVERIFY(again);
        QVERIFY(!again->rehashedObject);
    }

    void test_task()
    {
        const auto hash = store("original");
        QVERIFY(place(hash, "mods/mod.jar", "a", LinkKind::Hard));
        tamper(hash, "edited");
        DeepVerifyStoreTask task(m_store.get());
        QSignalSpy finished(&task, &Task::finished);
        task.start();
        QVERIFY(finished.count() == 1 || finished.wait());
        QVERIFY(task.wasSuccessful());
        QCOMPARE(task.report()->damaged, QStringList{ hash });
    }
};

QTEST_GUILESS_MAIN(DeepVerifyTest)

#include "DeepVerify_test.moc"
