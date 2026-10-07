#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "FileSystem.h"
#include "FileSystemPrimitives.h"
#include "contentstore/ContentStore.h"
#include "contentstore/SharedContent.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/mod/ResourcePackFolderModel.h"
#include "settings/INISettingsObject.h"

namespace {
bool writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

void makeWritable(const QString& path)
{
    QFile::setPermissions(path,
                          QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser | QFile::ExeOwner | QFile::ExeUser);
}
}  // namespace

// The sharing column of the resource lists shows what the store knows about each file
class SharedColumnTest : public QObject {
    Q_OBJECT

    // in the working directory, so hard links don't cross a tmpfs boundary
    QTemporaryDir m_dir{ QDir::current().filePath("shared_column_test_XXXXXX") };
    std::unique_ptr<SettingsObject> m_globalSettings;
    std::unique_ptr<ContentStore> m_store;
    bool m_canCreateSymbolicLinks = false;

    QString path(const QString& name) const { return m_dir.filePath(name); }

    // A Minecraft instance with the settings it reads from the launcher's
    std::unique_ptr<MinecraftInstance> makeInstance(const QString& name)
    {
        QDir().mkpath(path("instances/" + name + "/minecraft"));
        auto settings = std::make_unique<INISettingsObject>(path("instances/" + name + "/instance.cfg"));
        settings->registerSetting("InstanceType", "OneSix");
        return std::make_unique<MinecraftInstance>(m_globalSettings.get(), std::move(settings), path("instances/" + name));
    }

    // Places a stored copy of the bytes at the path in the instance, with the given kind of link
    QString place(MinecraftInstance& instance, const QString& relativePath, const QByteArray& data, LinkKind kind)
    {
        const auto source = path("downloads/" + QString::number(qHash(data)));
        writeFile(source, data);
        const auto stored = m_store->ingest(source, ContentStore::IngestMode::Copy);
        if (!stored) {
            return {};
        }
        ContentStore::PlaceOptions options;
        options.mode = kind == LinkKind::Hard ? ContentStore::LinkMode::HardLinks : ContentStore::LinkMode::SymbolicLinks;
        options.allowCopy = false;
        const auto file = QDir(instance.gameRoot()).filePath(relativePath);
        QDir().mkpath(QFileInfo(file).absolutePath());
        const auto destination = SharedContent::destination(*m_store, instance.id(), instance.gameRoot(), file);
        return m_store->placeAt({ destination, stored->hash, {} }, options) ? stored->hash : QString();
    }

    // What the sharing column shows for each file name, once the model loaded the folder
    QMap<QString, QString> sharedColumn(ResourcePackFolderModel& model)
    {
        QEventLoop loop;
        connect(&model, &ResourceFolderModel::updateFinished, &loop, &QEventLoop::quit);
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        model.update();
        loop.exec();
        QMap<QString, QString> shown;
        for (int row = 0; row < model.rowCount({}); row++) {
            const auto name = model.at(row).fileinfo().fileName();
            shown[name] = model.data(model.index(row, ResourcePackFolderModel::SharedColumn), Qt::DisplayRole).toString();
        }
        return shown;
    }

   private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_globalSettings = std::make_unique<INISettingsObject>(QDir(m_dir.path()).filePath("launcher.cfg"));
        for (const auto* setting : { "AutoCloseConsole",
                                     "CloseAfterLaunch",
                                     "ConsoleMaxLines",
                                     "ConsoleOverflowStop",
                                     "CustomGLFWPath",
                                     "CustomOpenALPath",
                                     "CustomSDLPath",
                                     "EnableFeralGamemode",
                                     "EnableMangoHud",
                                     "Env",
                                     "IgnoreJavaCompatibility",
                                     "JavaArchitecture",
                                     "JavaPath",
                                     "JavaRealArchitecture",
                                     "JavaSignature",
                                     "JavaVendor",
                                     "JavaVersion",
                                     "JvmArgs",
                                     "LaunchMaximized",
                                     "LogPrePostOutput",
                                     "LowMemWarning",
                                     "MaxMemAlloc",
                                     "MinMemAlloc",
                                     "MinecraftWinHeight",
                                     "MinecraftWinWidth",
                                     "OnlineFixes",
                                     "PermGen",
                                     "PostExitCommand",
                                     "PreLaunchCommand",
                                     "PreLoadCommand",
                                     "QuitAfterGameStop",
                                     "RecordGameTime",
                                     "ShowConsole",
                                     "ShowConsoleOnError",
                                     "ShowGameTime",
                                     "UseDiscreteGpu",
                                     "UseNativeGLFW",
                                     "UseNativeOpenAL",
                                     "UseNativeSDL",
                                     "UseZink",
                                     "WrapperCommand" }) {
            m_globalSettings->registerSetting(setting, QVariant());
        }
        const auto target = path("symlink-target");
        QVERIFY(writeFile(target, "target"));
        m_canCreateSymbolicLinks = FS::createSymbolicLink(QFileInfo(target).absoluteFilePath(), path("symlink-probe")).has_value();
        QFile::remove(path("symlink-probe"));
        QFile::remove(target);
    }

    void init()
    {
        SharedContent::setStoreForTesting(nullptr);
        m_store.reset();
        QDirIterator it(m_dir.path(), QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            makeWritable(it.next());
        }
        QVERIFY(FS::deleteContents(m_dir.path()));
        QVERIFY(QDir(m_dir.path()).mkpath("data"));
        m_store = std::make_unique<ContentStore>(path("store"), path("data"));
        QCOMPARE(m_store->open(), ContentStore::State::Writable);
        SharedContent::setStoreForTesting(m_store.get());
    }

    void cleanupTestCase() { SharedContent::setStoreForTesting(nullptr); }

    void test_resourcePacks()
    {
        if (!m_canCreateSymbolicLinks) {
            QSKIP("This system doesn't allow creating symbolic links");
        }
        auto instance = makeInstance("a");
        // as in the real instances: a pack linked symbolically, one with a folder of a similar name, a hard-linked one, a local
        // one, and one kept local
        QVERIFY(!place(*instance, "resourcepacks/Legacy IronChest Textures.zip", "iron chests", LinkKind::Symbolic).isEmpty());
        QVERIFY(!place(*instance, "resourcepacks/bipedal-e2550.zip", "bipedal", LinkKind::Symbolic).isEmpty());
        QVERIFY(QDir().mkpath(QDir(instance->gameRoot()).filePath("resourcepacks/Bipedal")));
        QVERIFY(!place(*instance, "resourcepacks/hard.zip", "hard", LinkKind::Hard).isEmpty());
        QVERIFY(writeFile(QDir(instance->gameRoot()).filePath("resourcepacks/local.zip"), "local"));

        ResourcePackFolderModel model(QDir(QDir(instance->gameRoot()).filePath("resourcepacks")), instance.get(), true, true);
        const auto shown = sharedColumn(model);
        qInfo() << shown;
        QCOMPARE(shown.value("Legacy IronChest Textures.zip"), "Shared");
        QCOMPARE(shown.value("bipedal-e2550.zip"), "Shared");
        QCOMPARE(shown.value("hard.zip"), "Shared");
        QCOMPARE(shown.value("local.zip"), "Local");
        QCOMPARE(shown.value("Bipedal"), QString());
    }
};

QTEST_GUILESS_MAIN(SharedColumnTest)

#include "SharedColumn_test.moc"
