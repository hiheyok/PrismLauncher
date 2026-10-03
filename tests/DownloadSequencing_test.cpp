#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "minecraft/mod/MetadataHandler.h"
#include "minecraft/mod/tasks/LocalResourceUpdateTask.h"
#include "tasks/FunctionTask.h"
#include "tasks/SequentialTask.h"

namespace {
ModPlatform::IndexedPack makePack()
{
    ModPlatform::IndexedPack pack;
    pack.addonId = "test-project";
    pack.slug = "test-mod";
    pack.name = "Test Mod";
    pack.provider = ModPlatform::ResourceProvider::MODRINTH;
    return pack;
}

ModPlatform::IndexedVersion makeVersion(const QString& fileName)
{
    ModPlatform::IndexedVersion version;
    version.addonId = "test-project";
    version.fileId = fileName;
    version.fileName = fileName;
    version.downloadUrl = "https://cdn.modrinth.com/data/test/" + fileName;
    version.hashType = "sha512";
    version.hash = "00";
    return version;
}
}  // namespace

// ResourceDownloadTask chooses where to download in a step after its LocalResourceUpdateTask,
// so that step must already know which resource the download replaces
class DownloadSequencingTest : public QObject {
    Q_OBJECT

   private slots:
    void test_stepAfterUpdateKnowsOldResource()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir indexDir(dir.path());

        auto pack = makePack();
        auto oldVersion = makeVersion("test-mod-1.0.jar");
        auto oldMetadata = Metadata::create(indexDir, pack, oldVersion);
        QVERIFY(oldMetadata.isValid());
        Metadata::update(indexDir, oldMetadata);

        QString oldFilename;
        QString filenameSeenByStep;

        SequentialTask sequence("download");
        auto update = makeShared<LocalResourceUpdateTask>(indexDir, pack, makeVersion("test-mod-2.0.jar"));
        connect(update.get(), &LocalResourceUpdateTask::hasOldResource, this,
                [&oldFilename](const QString&, const QString& filename) { oldFilename = filename; });
        sequence.addTask(update);
        sequence.addTask(makeShared<FunctionTask>([&] {
            filenameSeenByStep = oldFilename;
            return Result<>{};
        }));

        QSignalSpy succeeded(&sequence, &Task::succeeded);
        sequence.start();
        QVERIFY(succeeded.wait());

        QCOMPARE(filenameSeenByStep, "test-mod-1.0.jar");
    }

    void test_failingStepFailsSequence()
    {
        bool laterStepRan = false;
        SequentialTask sequence("download");
        sequence.addTask(makeShared<FunctionTask>([] { return Result<>{ std::unexpected(QString("no target")) }; }));
        sequence.addTask(makeShared<FunctionTask>([&laterStepRan] {
            laterStepRan = true;
            return Result<>{};
        }));

        QSignalSpy failed(&sequence, &Task::failed);
        sequence.start();
        QVERIFY(failed.wait());
        QVERIFY(!laterStepRan);
    }
};

QTEST_GUILESS_MAIN(DownloadSequencingTest)

#include "DownloadSequencing_test.moc"
