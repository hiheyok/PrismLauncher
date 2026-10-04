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

#if !defined(Q_OS_WIN)
#include <unistd.h>
#endif

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

constexpr qint64 g_hour = qint64(60) * 60;
constexpr qint64 g_day = 24 * g_hour;
}  // namespace

class ReconcileTest : public QObject {
    Q_OBJECT

    // in the working directory, so hard links don't cross a tmpfs boundary
    QTemporaryDir m_dir{ QDir::current().filePath("reconcile_test_XXXXXX") };
    std::unique_ptr<ContentStore> m_store;
    qint64 m_time = 0;
    bool m_canCreateSymbolicLinks = false;

    QString path(const QString& name) const { return m_dir.filePath(name); }

    ContentStore::Destination destination(const QString& relativePath, const QString& instance = "a") const
    {
        return { m_store->instanceOwner(instance), path(instance), relativePath };
    }

    RefKey key(const QString& relativePath, const QString& instance = "a") const { return destination(relativePath, instance).key(); }

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

    QString place(const QByteArray& data, const QString& relativePath, const QString& instance = "a")
    {
        const auto hash = store(data);
        const auto placed = m_store->placeAt({ destination(relativePath, instance), hash, {} });
        return placed ? hash : QString();
    }

    void openStore()
    {
        m_store = std::make_unique<ContentStore>(path("store"), path("data"));
        m_store->setClockForTesting([this] { return m_time; });
        QCOMPARE(m_store->open(), ContentStore::State::Writable);
    }

    void reopen()
    {
        m_store.reset();
        openStore();
    }

    ContentStore::ReconcileReport reconcileAt(qint64 time, const ContentStore::ReconcileOptions& options = {})
    {
        m_time = time;
        const auto report = m_store->reconcile(options);
        if (!report) {
            qWarning() << report.error();
            return {};
        }
        return *report;
    }

    bool isStored(const QString& hash) const
    {
        return QFileInfo::exists(m_store->objectPath(hash)) && m_store->table().entries().contains(hash);
    }

    // Puts a file into the store's folder without recording it, as a crash before its publication was recorded would
    QString putUnrecorded(const QByteArray& data)
    {
        const auto hash = sha256Of(data);
        const auto file = m_store->objectPath(hash);
        QDir().mkpath(QFileInfo(file).absolutePath());
        writeFile(file, data);
        ObjectFiles::makeReadOnly(file);
        return hash;
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
        QVERIFY(QDir(m_dir.path()).mkpath("data"));
        QVERIFY(QDir(m_dir.path()).mkpath("downloads"));
        QVERIFY(QDir(m_dir.path()).mkpath("a/mods"));
        QVERIFY(QDir(m_dir.path()).mkpath("b/mods"));
        m_time = QDateTime::currentSecsSinceEpoch();
        openStore();
    }

    void cleanup()
    {
        FS::Testing::setFaultHook(nullptr);
        FS::Testing::setCallRecorder(nullptr);
        if (m_store) {
            m_store->setUnreadableForTesting(nullptr);
        }
    }

    // Verification

    void test_verifyFindsMissingLinks()
    {
        const auto hash = place("mod", "mods/mod.jar");
        QVERIFY(FS::deleteLink(path("a/mods/mod.jar")));
        const auto report = m_store->verify();
        QVERIFY(report);
        QCOMPARE(report->checked, 1);
        QCOMPARE(report->missing, QList<RefKey>{ key("mods/mod.jar") });
        QCOMPARE(m_store->table().ref(key("mods/mod.jar"))->state, RefState::Missing);
        // verification never releases or destroys anything
        QVERIFY(isStored(hash));
    }

    void test_verifyTellsReplacedFromMissing()
    {
        place("mod", "mods/mod.jar");
        // an editor saves by renaming a new file over the old one
        QVERIFY(writeFile(path("a/mods/new.tmp"), "edited"));
        QVERIFY(FS::replaceFile(path("a/mods/new.tmp"), path("a/mods/mod.jar")));
        const auto report = m_store->verify();
        QVERIFY(report);
        QCOMPARE(report->replaced, QList<RefKey>{ key("mods/mod.jar") });
        QCOMPARE(m_store->table().ref(key("mods/mod.jar"))->state, RefState::Replaced);
    }

    void test_verifyChecksSymbolicLinks()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto hash = store("pack");
        ContentStore::PlaceOptions options;
        options.mode = ContentStore::LinkMode::SymbolicLinks;
        QVERIFY(m_store->placeAt({ destination("resourcepacks/pack.zip"), hash, {} }, options));
        QVERIFY(FS::deleteLink(path("a/resourcepacks/pack.zip")));
        // a file with no hard links to count still has its missing link found
        QCOMPARE(m_store->verify()->missing, QList<RefKey>{ key("resourcepacks/pack.zip") });
    }

    void test_verifyFindsLinksAgain()
    {
        const auto hash = place("mod", "mods/mod.jar");
        QVERIFY(FS::deleteLink(path("a/mods/mod.jar")));
        QVERIFY(m_store->verify());
        QVERIFY(FS::createHardLink(m_store->objectPath(hash), path("a/mods/mod.jar")));
        QVERIFY(m_store->verify());
        QCOMPARE(m_store->table().ref(key("mods/mod.jar"))->state, RefState::Live);
    }

    void test_verifyReportsUnrecordedLinks()
    {
        const auto hash = place("mod", "mods/mod.jar");
        QVERIFY(m_store->verify()->unrecordedLinks.isEmpty());
        QVERIFY(QDir(m_dir.path()).mkpath("elsewhere"));
        QVERIFY(FS::createHardLink(m_store->objectPath(hash), path("elsewhere/mod.jar")));
        QCOMPARE(m_store->verify()->unrecordedLinks, QStringList{ hash });
    }

    void test_unreadableFolderIsUnknown()
    {
        place("mod", "mods/mod.jar");
        const auto root = QFileInfo(path("a")).absoluteFilePath();
        m_store->setUnreadableForTesting([root](const QString& candidate) { return candidate == root; });
        const auto report = m_store->verify();
        QCOMPARE(report->unknown, QList<RefKey>{ key("mods/mod.jar") });
        QCOMPARE(m_store->table().ref(key("mods/mod.jar"))->state, RefState::Unknown);
    }

    void test_watcherHint()
    {
        place("mod", "mods/mod.jar");
        m_store->noteRemoved(key("mods/mod.jar"));
        QCOMPARE(m_store->table().ref(key("mods/mod.jar"))->state, RefState::Live);
        QVERIFY(FS::deleteLink(path("a/mods/mod.jar")));
        m_store->noteRemoved(key("mods/mod.jar"));
        QCOMPARE(m_store->table().ref(key("mods/mod.jar"))->state, RefState::Missing);
    }

    // Reconciliation

    void test_reconcileFollowsRenamedLinks()
    {
        const auto hash = place("mod", "mods/mod.jar");
        QVERIFY(QFile::rename(path("a/mods/mod.jar"), path("a/mods/renamed.jar")));
        const auto report = reconcileAt(m_time);
        QVERIFY(report.complete);
        QCOMPARE(report.moved, 1);
        QVERIFY(!m_store->table().ref(key("mods/mod.jar")));
        const auto ref = m_store->table().ref(key("mods/renamed.jar"));
        QVERIFY(ref);
        QCOMPARE(ref->hash, hash);
        QCOMPARE(ref->state, RefState::Live);
        QVERIFY(isStored(hash));
    }

    void test_reconcileAdoptsUnrecordedLinks()
    {
        const auto hash = place("mod", "mods/mod.jar");
        QVERIFY(QDir(path("a")).mkpath("resourcepacks"));
        QVERIFY(FS::createHardLink(m_store->objectPath(hash), path("a/resourcepacks/copy.zip")));
        const auto report = reconcileAt(m_time);
        QCOMPARE(report.adopted, 1);
        QCOMPARE(m_store->table().ref(key("resourcepacks/copy.zip"))->hash, hash);
        QVERIFY(m_store->verify()->unrecordedLinks.isEmpty());
    }

    void test_releaseNeedsTwoCompleteScansADayApart()
    {
        const auto hash = place("mod", "mods/mod.jar");
        QVERIFY(FS::deleteLink(path("a/mods/mod.jar")));
        const auto start = m_time;

        auto report = reconcileAt(start);
        QVERIFY(report.complete);
        QCOMPARE(report.released, 0);
        QCOMPARE(m_store->table().ref(key("mods/mod.jar"))->lostSince, start);
        QVERIFY(isStored(hash));

        // a drive that was briefly disconnected gets a day to come back
        report = reconcileAt(start + g_hour);
        QCOMPARE(report.released, 0);
        QVERIFY(isStored(hash));

        report = reconcileAt(start + g_day + g_hour);
        QCOMPARE(report.released, 1);
        QCOMPARE(report.destroyed, 1);
        QVERIFY(!m_store->table().ref(key("mods/mod.jar")));
        QVERIFY(!isStored(hash));
        QVERIFY(!QFileInfo::exists(m_store->objectPath(hash)));
    }

    void test_linkFoundAgainStartsOver()
    {
        const auto hash = place("mod", "mods/mod.jar");
        QVERIFY(FS::deleteLink(path("a/mods/mod.jar")));
        reconcileAt(m_time);
        QVERIFY(FS::createHardLink(m_store->objectPath(hash), path("a/mods/mod.jar")));
        reconcileAt(m_time + g_hour);
        QVERIFY(!m_store->table().ref(key("mods/mod.jar"))->lostSince);
        QVERIFY(FS::deleteLink(path("a/mods/mod.jar")));
        // the earlier loss doesn't count anymore
        const auto report = reconcileAt(m_time + 2 * g_day);
        QCOMPARE(report.released, 0);
        QVERIFY(isStored(hash));
    }

    void test_replacedResourceIsReleasedButNotAdopted()
    {
        const auto hash = place("mod", "mods/mod.jar");
        QVERIFY(writeFile(path("a/mods/new.tmp"), "the user's own file"));
        QVERIFY(FS::replaceFile(path("a/mods/new.tmp"), path("a/mods/mod.jar")));

        reconcileAt(m_time);
        QCOMPARE(m_store->table().ref(key("mods/mod.jar"))->state, RefState::Replaced);
        QVERIFY(isStored(hash));
        const auto report = reconcileAt(m_time + g_day);
        QCOMPARE(report.released, 1);
        QVERIFY(!isStored(hash));
        // the new file is left alone, as the user may be editing it
        QCOMPARE(readFile(path("a/mods/mod.jar")), "the user's own file");
        QVERIFY(!m_store->table().ref(key("mods/mod.jar")));
    }

    void test_incompleteScanNeverReleases()
    {
        const auto hash = place("mod", "mods/mod.jar", "a");
        place("other", "mods/other.jar", "b");
        QVERIFY(FS::deleteLink(path("a/mods/mod.jar")));
        const auto unreadable = QFileInfo(path("b")).absoluteFilePath();
        m_store->setUnreadableForTesting([unreadable](const QString& candidate) { return candidate == unreadable; });

        auto report = reconcileAt(m_time);
        QVERIFY(!report.complete);
        QVERIFY(report.uncertain.contains(unreadable));
        report = reconcileAt(m_time + 3 * g_day);
        QVERIFY(!report.complete);
        QCOMPARE(report.released, 0);
        QVERIFY(m_store->table().ref(key("mods/mod.jar")));
        QVERIFY(m_store->table().ref(key("mods/other.jar", "b")));
        QVERIFY(isStored(hash));
        QVERIFY(!m_store->table().clients()[m_store->clientId()].lastCompleteReconcile);
    }

    void test_deletedInstanceIsReleased()
    {
        const auto hash = place("mod", "mods/mod.jar");
        QVERIFY(FS::deleteTree(path("a")));
        reconcileAt(m_time);
        QVERIFY(isStored(hash));
        const auto report = reconcileAt(m_time + g_day);
        QCOMPARE(report.released, 1);
        QVERIFY(!isStored(hash));
        QVERIFY(!m_store->table().owners().contains(m_store->instanceOwner("a")));
    }

    void test_instanceInTheListIsNeverReleased()
    {
        const auto hash = place("mod", "mods/mod.jar");
        QVERIFY(FS::deleteTree(path("a")));
        ContentStore::ReconcileOptions options;
        options.knownOwners.insert(m_store->instanceOwner("a"), path("a"));
        reconcileAt(m_time, options);
        const auto report = reconcileAt(m_time + 2 * g_day, options);
        QVERIFY(!report.complete);
        QCOMPARE(report.released, 0);
        QVERIFY(isStored(hash));
    }

    void test_disconnectedDriveIsUncertain()
    {
        const auto hash = place("mod", "mods/mod.jar");
        // the folder was on another volume, so the folder left behind isn't where it was
        QVERIFY(m_store->commit({ RefRecord::owner(m_store->instanceOwner("a"), path("a"), "ffffffff") }));
        QVERIFY(FS::deleteTree(path("a")));

        QCOMPARE(m_store->verify()->unknown, QList<RefKey>{ key("mods/mod.jar") });
        auto report = reconcileAt(m_time);
        QVERIFY(!report.complete);
        report = reconcileAt(m_time + 2 * g_day);
        QCOMPARE(report.released, 0);
        QVERIFY(isStored(hash));
    }

#if !defined(Q_OS_WIN)
    void test_folderWithoutPermissionIsUncertain()
    {
        if (geteuid() == 0) {
            QSKIP("Permissions don't apply to root");
        }
        const auto hash = place("mod", "mods/mod.jar");
        QVERIFY(QFile::setPermissions(path("a"), QFileDevice::Permissions()));
        auto report = reconcileAt(m_time);
        reconcileAt(m_time + 2 * g_day);
        QFile::setPermissions(path("a"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QVERIFY(!report.complete);
        QVERIFY(m_store->table().ref(key("mods/mod.jar")));
        QVERIFY(isStored(hash));
    }
#endif

    void test_otherLaunchersLinksAreNeverReleased()
    {
        const auto hash = store("shared with another launcher");
        const RefKey other{ "other-client:instance", "mods/mod.jar" };
        QVERIFY(m_store->commit({ RefRecord::owner(other.owner, path("gone")),
                                  RefRecord::addRef(other, { hash, LinkKind::Hard, 1, RefState::Live, std::nullopt }) }));
        reconcileAt(m_time);
        reconcileAt(m_time + 3 * g_day);
        QVERIFY(m_store->table().ref(other));
        QVERIFY(isStored(hash));
    }

    // Destruction

    void test_lastLinkRemovedDestroys()
    {
        const auto hash = place("mod", "mods/mod.jar");
        QVERIFY(m_store->unshare(key("mods/mod.jar")));
        QVERIFY(!isStored(hash));
        QVERIFY(!QFileInfo::exists(m_store->objectPath(hash)));
        QCOMPARE(readFile(path("a/mods/mod.jar")), "mod");
    }

    void test_otherLinksKeepTheFile()
    {
        const auto hash = place("mod", "mods/mod.jar", "a");
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar", "b"), hash, {} }));
        QVERIFY(m_store->unshare(key("mods/mod.jar")));
        QVERIFY(isStored(hash));
    }

    void test_updateDestroysTheOldVersion()
    {
        const auto oldHash = place("version 1", "mods/mod.jar");
        const auto newHash = store("version 2");
        QVERIFY(m_store->placeAt({ destination("mods/mod.jar"), newHash, {} }));
        QVERIFY(!isStored(oldHash));
        QVERIFY(isStored(newHash));
    }

    void test_leaseKeepsTheFile()
    {
        const auto hash = place("mod", "mods/mod.jar");
        {
            // an operation in progress uses it
            const auto lease = m_store->lease(hash);
            QVERIFY(m_store->unshare(key("mods/mod.jar")));
            QVERIFY(isStored(hash));
        }
        QCOMPARE(*m_store->destroyUnused(), 1);
        QVERIFY(!isStored(hash));
    }

    void test_unrecordedLinkKeepsTheFile()
    {
        const auto hash = place("mod", "mods/mod.jar");
        // for example a hard link in the system trash
        QVERIFY(QDir(m_dir.path()).mkpath("trash"));
        QVERIFY(FS::createHardLink(m_store->objectPath(hash), path("trash/mod.jar")));
        QVERIFY(m_store->unshare(key("mods/mod.jar")));
        QVERIFY(isStored(hash));
        QVERIFY(FS::deleteLink(path("trash/mod.jar")));
        QCOMPARE(*m_store->destroyUnused(), 1);
    }

    void test_symbolicLinksNeedACompleteScan()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto hash = store("pack");
        ContentStore::PlaceOptions options;
        options.mode = ContentStore::LinkMode::SymbolicLinks;
        QVERIFY(m_store->placeAt({ destination("resourcepacks/pack.zip"), hash, {} }, options));
        QVERIFY(m_store->unshare(key("resourcepacks/pack.zip")));
        // a symbolic link nobody recorded may still point at it
        QVERIFY(isStored(hash));
        QCOMPARE(*m_store->destroyUnused(), 0);

        const auto report = reconcileAt(m_time + 1);
        QVERIFY(report.complete);
        QCOMPARE(report.destroyed, 1);
        QVERIFY(!isStored(hash));
    }

    void test_symbolicLinkFoundByAScanKeepsTheFile()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto hash = store("pack");
        ContentStore::PlaceOptions options;
        options.mode = ContentStore::LinkMode::SymbolicLinks;
        QVERIFY(m_store->placeAt({ destination("resourcepacks/pack.zip"), hash, {} }, options));
        // copied by the user, with the link kept
        QVERIFY(FS::createSymbolicLink(m_store->objectPath(hash), path("a/resourcepacks/copy.zip")));
        QVERIFY(m_store->unshare(key("resourcepacks/pack.zip")));

        const auto report = reconcileAt(m_time + 1);
        QCOMPARE(report.adopted, 1);
        QCOMPARE(report.destroyed, 0);
        QVERIFY(isStored(hash));
    }

    void test_everyLauncherMustScan()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto hash = store("pack");
        ContentStore::PlaceOptions options;
        options.mode = ContentStore::LinkMode::SymbolicLinks;
        QVERIFY(m_store->placeAt({ destination("resourcepacks/pack.zip"), hash, {} }, options));
        // another launcher uses the store too, and hasn't scanned its folders since
        QVERIFY(m_store->commit({ RefRecord::client("other-client", path("other"), m_time) }));
        QVERIFY(m_store->unshare(key("resourcepacks/pack.zip")));

        reconcileAt(m_time + 1);
        QVERIFY(isStored(hash));
        QVERIFY(m_store->commit({ RefRecord::reconciled("other-client", m_time + 2) }));
        QCOMPARE(*m_store->destroyUnused(), 1);
    }

    void test_replacementSymbolicLinkKeepsItsTarget()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto oldHash = place("version 1", "mods/mod.jar");
        const auto newHash = store("version 2");
        // replaced outside the launcher by a link to another stored file, which nothing records
        QVERIFY(FS::deleteLink(path("a/mods/mod.jar")));
        QVERIFY(FS::createSymbolicLink(m_store->objectPath(newHash), path("a/mods/mod.jar")));

        auto report = reconcileAt(m_time + 1);
        QVERIFY(report.complete);
        QVERIFY(isStored(newHash));
        QCOMPARE(*m_store->destroyUnused(), 0);
        QVERIFY(isStored(newHash));

        // once the old link is released, the new one is recorded in its place
        reconcileAt(m_time + 2 * g_day);
        report = reconcileAt(m_time + 4 * g_day);
        QVERIFY(isStored(newHash));
        QVERIFY(!isStored(oldHash));
        const auto ref = m_store->table().ref(key("mods/mod.jar"));
        QVERIFY(ref);
        QCOMPARE(ref->hash, newHash);
        QCOMPARE(ref->kind, LinkKind::Symbolic);
        QCOMPARE(readFile(path("a/mods/mod.jar")), "version 2");
    }

    void test_incompleteScanDefersDestruction()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto hash = store("pack");
        place("other", "mods/other.jar", "b");
        ContentStore::PlaceOptions options;
        options.mode = ContentStore::LinkMode::SymbolicLinks;
        QVERIFY(m_store->placeAt({ destination("resourcepacks/pack.zip"), hash, {} }, options));
        {
            // in use during a complete scan, so that scan doesn't destroy it
            const auto lease = m_store->lease(hash);
            QVERIFY(m_store->unshare(key("resourcepacks/pack.zip")));
            QVERIFY(reconcileAt(m_time + 1).complete);
        }
        // a link nobody recorded appears in a folder that can't be read
        QVERIFY(QDir(path("b")).mkpath("resourcepacks"));
        QVERIFY(FS::createSymbolicLink(m_store->objectPath(hash), path("b/resourcepacks/pack.zip")));
        const auto unreadable = QFileInfo(path("b")).absoluteFilePath();
        m_store->setUnreadableForTesting([unreadable](const QString& candidate) { return candidate == unreadable; });

        const auto report = reconcileAt(m_time + 2);
        QVERIFY(!report.complete);
        QCOMPARE(report.destroyed, 0);
        QCOMPARE(*m_store->destroyUnused(), 0);
        QVERIFY(isStored(hash));
        QCOMPARE(readFile(path("b/resourcepacks/pack.zip")), "pack");

        // the incomplete scan is remembered when the store is opened again
        reopen();
        m_store->setUnreadableForTesting([unreadable](const QString& candidate) { return candidate == unreadable; });
        QCOMPARE(*m_store->destroyUnused(), 0);
        QVERIFY(isStored(hash));
        QVERIFY(m_store->compact());
        reopen();
        QCOMPARE(*m_store->destroyUnused(), 0);
        QVERIFY(isStored(hash));

        // a complete scan finds the link and records it
        m_store->setUnreadableForTesting(nullptr);
        QVERIFY(reconcileAt(m_time + 3).complete);
        QVERIFY(m_store->table().ref(key("resourcepacks/pack.zip", "b")));
        QVERIFY(isStored(hash));
    }

    void test_incompleteScanAfterTheClockWentBack()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto hash = store("pack");
        place("other", "mods/other.jar", "b");
        ContentStore::PlaceOptions options;
        options.mode = ContentStore::LinkMode::SymbolicLinks;
        QVERIFY(m_store->placeAt({ destination("resourcepacks/pack.zip"), hash, {} }, options));
        const auto start = m_time;
        {
            const auto lease = m_store->lease(hash);
            QVERIFY(m_store->unshare(key("resourcepacks/pack.zip")));
            QVERIFY(reconcileAt(start + g_hour).complete);
        }
        QVERIFY(QDir(path("b")).mkpath("resourcepacks"));
        QVERIFY(FS::createSymbolicLink(m_store->objectPath(hash), path("b/resourcepacks/pack.zip")));
        const auto unreadable = QFileInfo(path("b")).absoluteFilePath();
        m_store->setUnreadableForTesting([unreadable](const QString& candidate) { return candidate == unreadable; });

        // the clock was set back before the next scan, which is still the latest one
        QVERIFY(!reconcileAt(start + 1).complete);
        m_time = start + 2 * g_hour;
        QCOMPARE(*m_store->destroyUnused(), 0);
        reopen();
        QCOMPARE(*m_store->destroyUnused(), 0);
        QVERIFY(m_store->compact());
        reopen();
        QCOMPARE(*m_store->destroyUnused(), 0);
        QVERIFY(isStored(hash));
    }

    void test_fileUnusedAfterTheClockWentBack()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto hash = store("pack");
        ContentStore::PlaceOptions options;
        options.mode = ContentStore::LinkMode::SymbolicLinks;
        QVERIFY(m_store->placeAt({ destination("resourcepacks/pack.zip"), hash, {} }, options));
        const auto start = m_time;
        QVERIFY(reconcileAt(start + g_day).complete);

        // the clock is set back, then the file stops being used: the earlier scan came before that, whatever the clock says
        m_time = start;
        QVERIFY(m_store->unshare(key("resourcepacks/pack.zip")));
        QCOMPARE(*m_store->destroyUnused(), 0);
        reopen();
        QCOMPARE(*m_store->destroyUnused(), 0);
        QVERIFY(isStored(hash));
    }

    void test_incompleteScanOfAnotherLauncherDefersDestruction()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        const auto hash = store("pack");
        ContentStore::PlaceOptions options;
        options.mode = ContentStore::LinkMode::SymbolicLinks;
        QVERIFY(m_store->placeAt({ destination("resourcepacks/pack.zip"), hash, {} }, options));
        QVERIFY(m_store->commit({ RefRecord::client("other-client", path("other"), m_time) }));
        {
            const auto lease = m_store->lease(hash);
            QVERIFY(m_store->unshare(key("resourcepacks/pack.zip")));
            QVERIFY(m_store->commit({ RefRecord::reconciled("other-client", m_time + 1) }));
            QVERIFY(reconcileAt(m_time + 1).complete);
        }
        // the other launcher's latest scan couldn't read all its folders
        QVERIFY(m_store->commit({ RefRecord::reconciled("other-client", m_time + 2, false) }));
        QCOMPARE(*m_store->destroyUnused(), 0);
        QVERIFY(isStored(hash));
        QVERIFY(m_store->commit({ RefRecord::reconciled("other-client", m_time + 3) }));
        QCOMPARE(*m_store->destroyUnused(), 1);
    }

    void test_destructionOrder()
    {
        const auto hash = place("mod", "mods/mod.jar");
        QStringList calls;
        const auto object = QFileInfo(m_store->objectPath(hash)).absoluteFilePath();
        FS::Testing::setCallRecorder([&calls, &object](FS::Testing::Operation operation, const QString& target) {
            const auto absolute = QFileInfo(target).absoluteFilePath();
            if (QFileInfo(target).fileName().startsWith("refs.journal") && operation == FS::Testing::Operation::FlushFile) {
                calls.append("journal");
            } else if (absolute == object && operation == FS::Testing::Operation::Delete) {
                calls.append("delete");
            } else if (absolute == QFileInfo(object).absolutePath() && operation == FS::Testing::Operation::FlushDir) {
                calls.append("flush");
            }
        });
        QVERIFY(m_store->unshare(key("mods/mod.jar")));
        FS::Testing::setCallRecorder(nullptr);
        // destroying is durable before the removal, which is durable before destroyed
        const auto removal = calls.indexOf("delete");
        QVERIFY(removal > 0);
        QCOMPARE(calls[removal - 1], "journal");
        QCOMPARE(calls.mid(removal, 3), QStringList({ "delete", "flush", "journal" }));
    }

    void test_interruptedDestructionIsFinished()
    {
        const auto hash = store("unused");
        const auto generation = m_store->table().entries()[hash].current->id;
        QVERIFY(m_store->commit({ RefRecord::destroying(hash, generation) }));
        reopen();
        QVERIFY(!isStored(hash));
        QVERIFY(m_store->table().destroying().isEmpty());
    }

    void test_interruptedDestructionOfAFileInUseIsAbandoned()
    {
        const auto hash = place("used", "mods/mod.jar");
        const auto generation = m_store->table().entries()[hash].current->id;
        QVERIFY(m_store->commit({ RefRecord::destroying(hash, generation) }));
        reopen();
        QVERIFY(isStored(hash));
        QVERIFY(m_store->table().destroying().isEmpty());
        QCOMPARE(readFile(path("a/mods/mod.jar")), "used");
    }

    void test_interruptedDestructionAfterTheRemoval()
    {
        const auto hash = store("unused");
        const auto generation = m_store->table().entries()[hash].current->id;
        QVERIFY(m_store->commit({ RefRecord::destroying(hash, generation) }));
        makeWritable(m_store->objectPath(hash));
        QVERIFY(FS::deleteLink(m_store->objectPath(hash)));
        reopen();
        QVERIFY(!m_store->table().entries().contains(hash));
        QVERIFY(m_store->table().destroying().isEmpty());
    }

    // Orphans

    void test_unrecordedFileWaitsTwoWeeks()
    {
        const auto hash = putUnrecorded("left by a crash");
        auto report = reconcileAt(m_time);
        QCOMPARE(report.adoptedFiles, 1);
        QVERIFY(m_store->table().entries()[hash].unrecorded);
        QVERIFY(isStored(hash));

        report = reconcileAt(m_time + 13 * g_day);
        QCOMPARE(report.destroyed, 0);
        // never moved while it waits
        QCOMPARE(readFile(m_store->objectPath(hash)), "left by a crash");

        report = reconcileAt(m_time + 15 * g_day);
        QCOMPARE(report.destroyed, 1);
        QVERIFY(!isStored(hash));
    }

    void test_unrecordedFileWithLinksIsAdopted()
    {
        const auto hash = putUnrecorded("linked before the crash");
        // the owner of a is known from an earlier placement
        place("other", "mods/other.jar");
        QVERIFY(FS::createHardLink(m_store->objectPath(hash), path("a/mods/mod.jar")));
        const auto report = reconcileAt(m_time);
        QCOMPARE(report.adoptedFiles, 1);
        QCOMPARE(report.adopted, 1);
        QCOMPARE(m_store->table().ref(key("mods/mod.jar"))->hash, hash);
        reconcileAt(m_time + 30 * g_day);
        QVERIFY(isStored(hash));
    }

    void test_unrecordedFileNeedsEveryLaunchersScan()
    {
        const auto hash = putUnrecorded("left by a crash");
        const auto start = m_time;
        QVERIFY(m_store->commit({ RefRecord::client("other-client", path("other"), start) }));
        reconcileAt(start);
        auto report = reconcileAt(start + 15 * g_day);
        QCOMPARE(report.destroyed, 0);
        QVERIFY(isStored(hash));

        // a complete scan by the other launcher that came before the file was found doesn't count
        QVERIFY(m_store->commit({ RefRecord::reconciled("other-client", start - 1) }));
        report = reconcileAt(start + 16 * g_day);
        QCOMPARE(report.destroyed, 0);

        QVERIFY(m_store->commit({ RefRecord::reconciled("other-client", start + 16 * g_day) }));
        report = reconcileAt(start + 17 * g_day);
        QCOMPARE(report.destroyed, 1);
    }

    void test_damagedFileInTheStoreIsKept()
    {
        const auto name = sha256Of("expected contents");
        const auto file = m_store->objectPath(name);
        QVERIFY(QDir().mkpath(QFileInfo(file).absolutePath()));
        QVERIFY(writeFile(file, "different contents"));
        const auto report = reconcileAt(m_time + 30 * g_day);
        QCOMPARE(report.damagedFiles.size(), 1);
        QCOMPARE(QFileInfo(report.damagedFiles.first()), QFileInfo(file));
        QVERIFY(QFileInfo::exists(file));
        QVERIFY(!m_store->table().entries().contains(name));
    }

    void test_leftoverIngestFilesAreRemoved()
    {
        QVERIFY(writeFile(QDir(m_store->temporaryDir()).filePath("object.ingest-leftover"), "partial"));
        reconcileAt(m_time + g_hour);
        QVERIFY(QFileInfo::exists(QDir(m_store->temporaryDir()).filePath("object.ingest-leftover")));
        reconcileAt(m_time + 2 * g_day);
        QVERIFY(!QFileInfo::exists(QDir(m_store->temporaryDir()).filePath("object.ingest-leftover")));
    }

    void test_reconcileSurvivesReopening()
    {
        const auto hash = place("mod", "mods/mod.jar");
        QVERIFY(FS::deleteLink(path("a/mods/mod.jar")));
        reconcileAt(m_time);
        const auto lostSince = m_store->table().ref(key("mods/mod.jar"))->lostSince;
        QVERIFY(lostSince);
        const auto table = m_store->table();
        reopen();
        QCOMPARE(m_store->table().ref(key("mods/mod.jar"))->lostSince, lostSince);
        QCOMPARE(m_store->table().clients()[m_store->clientId()].lastCompleteReconcile, m_time);
        // and the same from a snapshot
        QVERIFY(m_store->compact());
        reopen();
        QCOMPARE(m_store->table().ref(key("mods/mod.jar"))->lostSince, lostSince);
        QCOMPARE(m_store->table().ownerVolume(m_store->instanceOwner("a")), table.ownerVolume(m_store->instanceOwner("a")));
        QVERIFY(!m_store->table().ownerVolume(m_store->instanceOwner("a")).isEmpty());
        QVERIFY(isStored(hash));
    }

    // Tasks

    void test_tasks()
    {
        place("mod", "mods/mod.jar");
        QVERIFY(FS::deleteLink(path("a/mods/mod.jar")));

        VerifyStoreTask verify(m_store.get());
        QSignalSpy verified(&verify, &Task::finished);
        verify.start();
        QVERIFY(verified.count() == 1 || verified.wait());
        QVERIFY(verify.wasSuccessful());
        QCOMPARE(verify.report()->missing.size(), 1);

        ReconcileStoreTask reconcile(m_store.get(), {});
        QSignalSpy reconciled(&reconcile, &Task::finished);
        reconcile.start();
        QVERIFY(reconciled.count() == 1 || reconciled.wait());
        QVERIFY(reconcile.wasSuccessful());
        QVERIFY(reconcile.report()->complete);
    }
};

QTEST_GUILESS_MAIN(ReconcileTest)

#include "Reconcile_test.moc"
