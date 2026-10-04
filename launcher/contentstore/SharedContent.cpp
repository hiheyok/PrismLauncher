#include "SharedContent.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>

#include "Application.h"
#include "BaseInstance.h"
#include "FileSystem.h"
#include "FileSystemPrimitives.h"
#include "settings/SettingsObject.h"

namespace SharedContent {

namespace {
// The folders of a game folder whose files are shared
const QStringList& contentFolders()
{
    static const QStringList folders{ "mods", "coremods", "resourcepacks", "texturepacks", "shaderpacks", "datapacks" };
    return folders;
}

constexpr auto g_exclusionSetting = "SharedStoreExcluded";
}  // namespace

ContentStore* store()
{
    // in tests the application macro doesn't work
    auto* application = APPLICATION_DYN;
    auto* store = application ? application->contentStore() : nullptr;
    return store && store->isWritable() ? store : nullptr;
}

ContentStore* storeFor(const BaseInstance* instance)
{
    return instance ? store() : nullptr;
}

QString gameRootOf(const QString& instanceRoot)
{
    // the same choice as MinecraftInstance::gameRoot
    const QFileInfo minecraft(QDir(instanceRoot).filePath("minecraft"));
    const QFileInfo dotMinecraft(QDir(instanceRoot).filePath(".minecraft"));
    return dotMinecraft.exists() && !minecraft.exists() ? dotMinecraft.absoluteFilePath() : minecraft.absoluteFilePath();
}

ContentStore::Destination destination(const ContentStore& store, const QString& instanceId, const QString& gameRoot, const QString& path)
{
    return { store.instanceOwner(instanceId), QFileInfo(gameRoot).absoluteFilePath(), QDir(gameRoot).relativeFilePath(path) };
}

bool isShareable(const QString& gameRoot, const QString& path)
{
    const auto relative = QDir(gameRoot).relativeFilePath(path);
    const auto parts = relative.split('/');
    if (parts.size() != 2 || !contentFolders().contains(parts.first())) {
        return false;
    }
    const auto& name = parts.last();
    if (name.startsWith(".prism-") || name.endsWith(".pw.toml")) {
        return false;
    }
    // shader configuration files are edited by the game
    if (parts.first() == "shaderpacks" && name.endsWith(".txt")) {
        return false;
    }
    const QFileInfo info(path);
    return !info.isSymbolicLink() && info.isFile();
}

QStringList shareableFiles(const QString& gameRoot)
{
    QStringList files;
    for (const auto& folder : contentFolders()) {
        const QDir dir(QDir(gameRoot).filePath(folder));
        for (const auto& info : dir.entryInfoList(QDir::Files | QDir::Hidden | QDir::System)) {
            if (isShareable(gameRoot, info.absoluteFilePath())) {
                files.append(info.absoluteFilePath());
            }
        }
    }
    return files;
}

QString exclusionKey(const QString& relativePath)
{
    auto key = QDir::fromNativeSeparators(QDir::cleanPath(relativePath));
    if (key.endsWith(".disabled")) {
        key.chop(9);
    }
    return key;
}

bool isExcluded(BaseInstance* instance, const QString& relativePath)
{
    if (!instance || !instance->settings()->contains(g_exclusionSetting)) {
        return false;
    }
    return instance->settings()->get(g_exclusionSetting).toStringList().contains(exclusionKey(relativePath));
}

void renameExclusion(BaseInstance* instance, const QString& from, const QString& to)
{
    if (!instance || !instance->settings()->contains(g_exclusionSetting)) {
        return;
    }
    auto excluded = instance->settings()->get(g_exclusionSetting).toStringList();
    if (excluded.removeAll(exclusionKey(from)) > 0) {
        excluded.append(exclusionKey(to));
        instance->settings()->set(g_exclusionSetting, excluded);
    }
}

Result<PlacementKind> installFile(ContentStore& store,
                                  const ContentStore::Destination& destination,
                                  const QString& source,
                                  ContentStore::IngestMode mode)
{
    const auto stored = store.ingest(source, mode);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    // the launcher replaces what is there, as installing always did
    std::optional<FS::FileIdentity> replaces;
    if (const QFileInfo info(destination.path()); info.exists() || info.isSymbolicLink()) {
        TRY_INTO(replaces, FS::identity(destination.path()))
    }
    return store.placeAt({ destination, stored->hash, replaces });
}

int shareFreshFiles(ContentStore& store,
                    const QString& instanceId,
                    const QString& gameRoot,
                    const QStringList& paths,
                    const std::function<bool(const QString&)>& excluded)
{
    int shared = 0;
    ContentStore::PlaceOptions options;
    // a copy saves nothing, so a file that can't be linked stays as it is
    options.allowCopy = false;
    for (const auto& path : paths) {
        if (!isShareable(gameRoot, path)) {
            continue;
        }
        const auto target = destination(store, instanceId, gameRoot, path);
        if (excluded && excluded(target.relativePath)) {
            continue;
        }
        std::optional<QString> hash = store.storedHashOf(path);
        // already shared, unless a new file replaced the recorded one, such as a modpack update under the same name
        if (const auto ref = store.refAt(target.key()); ref && hash == ref->hash) {
            continue;
        }
        if (!hash) {
            // other hard links may be another instance's files, which must not become read-only
            if (FS::hardLinkCount(path) != 1) {
                continue;
            }
            // the file itself becomes the stored file, or is replaced by a link to an identical one
            auto stored = store.ingest(path, ContentStore::IngestMode::LinkIn);
            if (!stored) {
                qWarning() << "Shared store: could not share" << path << ":" << stored.error();
                continue;
            }
            hash = stored->hash;
        }
        const auto identity = FS::identity(path);
        if (!identity) {
            continue;
        }
        if (auto placed = store.placeAt({ target, *hash, *identity }, options); placed) {
            shared++;
        } else {
            qWarning() << "Shared store: could not link" << path << ":" << placed.error();
        }
    }
    return shared;
}

Result<> removeFile(ContentStore& store, const RefKey& key, const std::function<bool()>& remove)
{
    const auto ref = store.refAt(key);
    if (ref && ref->kind == LinkKind::Symbolic) {
        // a trashed symbolic link would dangle once its stored file is gone
        TRY(store.unshare(key))
    }
    if (!remove()) {
        return std::unexpected(QString("Could not remove %1").arg(key.relativePath));
    }
    return store.forgetRemoved(key);
}

Result<> removeInstance(ContentStore& store, const QString& owner, const std::function<bool()>& remove)
{
    for (const auto& key : store.symbolicLinksOf(owner)) {
        // stops before anything is removed; files already turned into copies stay copies
        TRY(store.unshare(key))
    }
    if (!remove()) {
        return std::unexpected(QString("Could not remove the folder of %1").arg(owner));
    }
    return store.forgetOwner(owner);
}

}  // namespace SharedContent
