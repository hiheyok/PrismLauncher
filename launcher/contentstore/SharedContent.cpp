#include "SharedContent.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include "Application.h"
#include "BaseInstance.h"
#include "FileSystem.h"
#include "FileSystemPrimitives.h"
#include "InstanceList.h"
#include "contentstore/ObjectFiles.h"
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
constexpr auto g_unsharedFromSetting = "SharedStoreUnsharedFrom";
constexpr auto g_instanceSharesSetting = "SharedStoreInstanceShares";

ContentStore* s_testStore = nullptr;

// the stored files local copies were made from, by exclusion key
QJsonObject unsharedFromMap(BaseInstance* instance)
{
    if (!instance->settings()->contains(g_unsharedFromSetting)) {
        return {};
    }
    return QJsonDocument::fromJson(instance->settings()->get(g_unsharedFromSetting).toString().toUtf8()).object();
}

void setUnsharedFromMap(BaseInstance* instance, const QJsonObject& map)
{
    if (instance->settings()->contains(g_unsharedFromSetting)) {
        instance->settings()->set(g_unsharedFromSetting, QString::fromUtf8(QJsonDocument(map).toJson(QJsonDocument::Compact)));
    }
}
}  // namespace

void setStoreForTesting(ContentStore* store)
{
    s_testStore = store;
}

ContentStore* store()
{
    if (s_testStore) {
        return s_testStore->isWritable() ? s_testStore : nullptr;
    }
    // in tests the application macro doesn't work
    auto* application = APPLICATION_DYN;
    auto* store = application ? application->contentStore() : nullptr;
    return store && store->isWritable() ? store : nullptr;
}

ContentStore::ReconcileOptions reconcileOptions(const ContentStore& store)
{
    ContentStore::ReconcileOptions options;
    auto* application = APPLICATION_DYN;
    if (!application || !application->instances()) {
        return options;
    }
    auto* instances = application->instances();
    for (int i = 0; i < instances->count(); i++) {
        auto* instance = instances->at(i);
        options.knownOwners.insert(store.instanceOwner(instance->id()), QFileInfo(instance->gameRoot()).absoluteFilePath());
    }
    return options;
}

bool instanceShares(BaseInstance* instance)
{
    return !instance || !instance->settings()->contains(g_instanceSharesSetting) ||
           instance->settings()->get(g_instanceSharesSetting).toBool();
}

void setInstanceShares(BaseInstance* instance, bool shares)
{
    if (instance && instance->settings()->contains(g_instanceSharesSetting)) {
        instance->settings()->set(g_instanceSharesSetting, shares);
    }
}

ContentStore* storeFor(BaseInstance* instance)
{
    return instance && instanceShares(instance) ? store() : nullptr;
}

ContentStore* linkedStoreFor(BaseInstance* instance)
{
    return instance ? store() : nullptr;
}

FileState fileState(ContentStore* store, BaseInstance* instance, const QString& gameRoot, const QString& path)
{
    const QFileInfo info(path);
    if (!store || info.isDir()) {
        return FileState::NotShareable;
    }
    const auto relativePath = QDir(gameRoot).relativeFilePath(path);
    if (isExcluded(instance, relativePath)) {
        return FileState::KeptLocal;
    }
    const RefKey key{ store->instanceOwner(instance->id()), relativePath };
    if (!store->refAt(key)) {
        return FileState::Local;
    }
    return store->usesDamagedCopy(key) ? FileState::Damaged : FileState::Shared;
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

int excludedCount(BaseInstance* instance)
{
    if (!instance || !instance->settings()->contains(g_exclusionSetting)) {
        return 0;
    }
    return static_cast<int>(instance->settings()->get(g_exclusionSetting).toStringList().size());
}

void clearExclusions(BaseInstance* instance)
{
    if (!instance || !instance->settings()->contains(g_exclusionSetting)) {
        return;
    }
    instance->settings()->set(g_exclusionSetting, QStringList());
    instance->settings()->set(g_unsharedFromSetting, QString());
}

void setExcluded(BaseInstance* instance, const QString& relativePath, bool excluded, const QString& unsharedFrom)
{
    if (!instance || !instance->settings()->contains(g_exclusionSetting)) {
        return;
    }
    const auto key = exclusionKey(relativePath);
    auto list = instance->settings()->get(g_exclusionSetting).toStringList();
    list.removeAll(key);
    if (excluded) {
        list.append(key);
    }
    instance->settings()->set(g_exclusionSetting, list);

    auto map = unsharedFromMap(instance);
    map.remove(key);
    if (excluded && !unsharedFrom.isEmpty()) {
        map[key] = unsharedFrom;
    }
    setUnsharedFromMap(instance, map);
}

QString unsharedFrom(BaseInstance* instance, const QString& relativePath)
{
    return instance ? unsharedFromMap(instance).value(exclusionKey(relativePath)).toString() : QString();
}

Result<QString> unshareToLocal(ContentStore& store, const ContentStore::Destination& destination)
{
    if (!store.refAt(destination.key())) {
        return QString();
    }
    TRY_INTO(const auto unshared, store.unshare(destination.key()))
    return unshared.hash;
}

Result<> keepLocal(ContentStore& store, BaseInstance* instance, const ContentStore::Destination& destination)
{
    TRY_INTO(const auto hash, unshareToLocal(store, destination))
    setExcluded(instance, destination.relativePath, true, hash);
    return {};
}

bool canRevert(ContentStore& store, BaseInstance* instance, const QString& relativePath)
{
    const auto hash = unsharedFrom(instance, relativePath);
    return isExcluded(instance, relativePath) && !hash.isEmpty() && store.hasIntactCopy(hash);
}

Result<> revertToShared(ContentStore& store,
                        BaseInstance* instance,
                        const ContentStore::Destination& destination,
                        const FS::FileIdentity& replaces)
{
    const auto hash = unsharedFrom(instance, destination.relativePath);
    if (hash.isEmpty() || !store.hasIntactCopy(hash)) {
        return std::unexpected(QString("The shared version of %1 is no longer stored").arg(destination.relativePath));
    }
    // the user confirmed discarding exactly this file
    TRY(store.placeAt({ destination, hash, replaces }))
    setExcluded(instance, destination.relativePath, false);
    return {};
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

ShareReport shareInstance(ContentStore& store,
                          const QString& instanceId,
                          const QString& gameRoot,
                          const ContentStore::ConvertOptions& options,
                          const std::function<bool(const QString&)>& excluded,
                          const std::function<bool(int done, int total)>& progress)
{
    ShareReport report;
    const auto files = shareableFiles(gameRoot);
    // what is stored already, so a file that becomes a link to it saves its size
    auto stored = store.storedHashes();
    for (int done = 0; done < files.size(); done++) {
        if (progress && !progress(done, static_cast<int>(files.size()))) {
            report.stopped = true;
            break;
        }
        const auto& path = files[done];
        const auto target = destination(store, instanceId, gameRoot, path);
        if (excluded && excluded(target.relativePath)) {
            continue;
        }
        const auto size = QFileInfo(path).size();
        const auto converted = store.convert(target, options);
        if (!converted) {
            report.failed.append(converted.error());
            continue;
        }
        switch (converted->outcome) {
            case ContentStore::ConvertOutcome::Shared:
                report.shared++;
                if (stored.contains(converted->hash)) {
                    report.bytesSaved += size;
                }
                stored.insert(converted->hash);
                break;
            case ContentStore::ConvertOutcome::AlreadyShared:
                report.alreadyShared++;
                break;
            case ContentStore::ConvertOutcome::Skipped:
                report.skipped.append({ path, converted->skip });
                break;
        }
    }
    if (progress && !report.stopped) {
        progress(static_cast<int>(files.size()), static_cast<int>(files.size()));
    }
    return report;
}

Result<ContentStore::ConvertResult> shareFile(ContentStore& store,
                                              BaseInstance* instance,
                                              const ContentStore::Destination& destination,
                                              const ContentStore::ConvertOptions& options)
{
    auto converted = store.convert(destination, options);
    if (converted && converted->outcome != ContentStore::ConvertOutcome::Skipped) {
        // shared now, so it isn't kept local anymore
        setExcluded(instance, destination.relativePath, false);
    }
    return converted;
}

namespace {
// this launcher's links that includes accepts
QList<std::pair<RefKey, Ref>> linksOf(ContentStore& store, const std::function<bool(const QString& owner)>& includes)
{
    QList<std::pair<RefKey, Ref>> links;
    for (const auto& link : store.refsSnapshot()) {
        if (link.first.owner.startsWith(store.clientId() + ':') && (!includes || includes(link.first.owner))) {
            links.append(link);
        }
    }
    return links;
}
}  // namespace

StopReport stopSharing(ContentStore& store,
                       const std::function<bool(const QString& owner)>& includes,
                       const std::function<bool(int done, int total)>& progress)
{
    StopReport report;
    const auto links = linksOf(store, includes);
    for (int done = 0; done < links.size(); done++) {
        if (progress && !progress(done, static_cast<int>(links.size()))) {
            report.stopped = true;
            break;
        }
        const auto& key = links[done].first;
        if (auto unshared = store.unshare(key); !unshared) {
            report.failed.append(QString("%1: %2").arg(key.relativePath, unshared.error()));
            continue;
        }
        report.unshared++;
    }
    if (progress && !report.stopped) {
        progress(static_cast<int>(links.size()), static_cast<int>(links.size()));
    }
    return report;
}

qint64 linkedSize(ContentStore& store, const std::function<bool(const QString& owner)>& includes)
{
    qint64 size = 0;
    for (const auto& link : linksOf(store, includes)) {
        size += store.storedSize(link.second.hash);
    }
    return size;
}

int linkCount(ContentStore& store, const std::function<bool(const QString& owner)>& includes)
{
    return static_cast<int>(linksOf(store, includes).size());
}

namespace {
std::function<void(MoveStep, const QString&)> s_moveHook;

void moveStep(MoveStep step, const QString& path)
{
    if (s_moveHook) {
        s_moveHook(step, path);
    }
}
}  // namespace

void setMoveHookForTesting(std::function<void(MoveStep step, const QString& path)> hook)
{
    s_moveHook = std::move(hook);
}

MoveReport moveShares(ContentStore& from, ContentStore& to, const std::function<bool(int done, int total)>& progress)
{
    MoveReport report;
    const auto refs = from.refsSnapshot();
    for (int done = 0; done < refs.size(); done++) {
        if (progress && !progress(done, static_cast<int>(refs.size()))) {
            report.stopped = true;
            break;
        }
        const auto& key = refs[done].first;
        // other launchers using the old store keep their links there
        if (!key.owner.startsWith(from.clientId() + ':')) {
            report.otherLaunchers++;
            continue;
        }
        const auto root = from.ownerRoot(key.owner);
        if (root.isEmpty()) {
            continue;
        }
        const ContentStore::Destination destination{ key.owner, root, key.relativePath };
        const auto path = destination.path();
        const QFileInfo info(path);
        if (!info.exists() && !info.isSymbolicLink()) {
            // nothing to move: the old store releases it like any link that is gone
            if (from.forgetRemoved(key)) {
                report.gone++;
            }
            continue;
        }
        // the bytes the instance sees, a damaged copy as it is: from the file a symbolic link points at, as copying checks
        // the size of the file it reads, and a link's own size is that of its target's path
        const bool symbolic = info.isSymLink();
        const auto source = symbolic ? info.symLinkTarget() : path;
        // the old store's file isn't kept anyway, so where no link works a copy of it is better than nothing
        ContentStore::PlaceOptions options;
        options.allowCopy = true;
        QString error;
        bool placedCurrent = false;
        // A file put at the path meanwhile, such as by an update, differs from the identity taken before the copy, and
        // isn't replaced. A symbolic link stays the same while its target changes, so that is hashed again once replaced:
        // what changed is still there, and is copied again. If it keeps changing, the old store's file isn't released.
        constexpr int attempts = 3;
        for (int attempt = 0; attempt < attempts && !placedCurrent; attempt++) {
            const auto identity = FS::identity(path);
            if (!identity) {
                error = identity.error();
                break;
            }
            moveStep(MoveStep::BeforeCopy, path);
            const auto stored = to.ingest(source, ContentStore::IngestMode::Copy);
            if (!stored) {
                error = stored.error();
                break;
            }
            moveStep(MoveStep::Copied, path);
            // after the first attempt, the path holds the new store's own link, which needs no confirmation
            const auto replaces = attempt == 0 ? std::optional(*identity) : std::nullopt;
            if (auto placed = to.placeAt({ destination, stored->hash, replaces }, options); !placed) {
                error = placed.error();
                break;
            }
            moveStep(MoveStep::Placed, path);
            if (!symbolic) {
                placedCurrent = true;
                break;
            }
            const auto now = ObjectFiles::sha256(source);
            if (!now) {
                error = now.error();
                break;
            }
            placedCurrent = *now == stored->hash;
            if (!placedCurrent) {
                error = QString("%1 kept changing while it was moved").arg(source);
            }
        }
        if (!placedCurrent) {
            report.failed.append(QString("%1: %2").arg(path, error));
            continue;
        }
        if (auto released = from.releaseMoved(key); !released) {
            report.failed.append(QString("%1: %2").arg(path, released.error()));
            continue;
        }
        report.moved++;
    }
    if (progress && !report.stopped) {
        progress(static_cast<int>(refs.size()), static_cast<int>(refs.size()));
    }
    return report;
}

namespace {
// The folder with links resolved, as far as it exists: the rest of the path is appended as it is
QString resolvedFolder(const QString& dir)
{
    const auto absolute = QDir::cleanPath(QFileInfo(dir).absoluteFilePath());
    auto existing = absolute;
    QString rest;
    while (!QFileInfo::exists(existing)) {
        const auto parent = QFileInfo(existing).path();
        if (parent == existing) {
            return absolute;
        }
        rest = QFileInfo(existing).fileName() + (rest.isEmpty() ? QString() : '/' + rest);
        existing = parent;
    }
    const auto canonical = QFileInfo(existing).canonicalFilePath();
    return QDir::cleanPath(rest.isEmpty() ? canonical : canonical + '/' + rest);
}
}  // namespace

bool foldersOverlap(const QString& first, const QString& second)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    // the file systems there usually ignore case
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    const auto a = resolvedFolder(first);
    const auto b = resolvedFolder(second);
    const auto within = [&](const QString& inner, const QString& outer) {
        return inner.compare(outer, sensitivity) == 0 || inner.startsWith(outer.endsWith('/') ? outer : outer + '/', sensitivity);
    };
    return within(a, b) || within(b, a);
}

QStringList linksInto(const QString& dir, const QStringList& gameRoots)
{
    const auto prefix = QDir::cleanPath(QFileInfo(dir).absoluteFilePath()) + '/';
    QStringList links;
    for (const auto& gameRoot : gameRoots) {
        for (const auto& folder : contentFolders()) {
            const QDir content(QDir(gameRoot).filePath(folder));
            for (const auto& info : content.entryInfoList(QDir::Files | QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
                if (info.isSymLink() && QDir::cleanPath(info.symLinkTarget()).startsWith(prefix, Qt::CaseInsensitive)) {
                    links.append(info.absoluteFilePath());
                }
            }
        }
    }
    return links;
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
