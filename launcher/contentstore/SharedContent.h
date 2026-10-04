#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>
#include <functional>

#include "Result.h"
#include "contentstore/ContentStore.h"

class BaseInstance;

// Connects instances to the shared store: which files are shared, and how the launcher's own file operations keep the
// store's records right
namespace SharedContent {

// The store, if sharing is on and this launcher can change the store
ContentStore* store();

// The store to share an instance's files through: store(), unless the instance doesn't share its files
ContentStore* storeFor(BaseInstance* instance);

// Whether the instance's files are shared, as the instance setting says
bool instanceShares(BaseInstance* instance);
void setInstanceShares(BaseInstance* instance, bool shares);

// Uses this store instead of the application's, for tests
void setStoreForTesting(ContentStore* store);

// The instances that exist, for reconciliation to scan
ContentStore::ReconcileOptions reconcileOptions(const ContentStore& store);

// How a resource file of an instance relates to the store
enum class FileState : std::uint8_t {
    // a folder, or sharing is off
    NotShareable,
    // a link to a stored file
    Shared,
    // a link to a stored file that was found damaged, until the user chooses a repair
    Damaged,
    // the user keeps it local
    KeptLocal,
    // a local file, such as one that was there before sharing was on
    Local,
};
FileState fileState(ContentStore* store, BaseInstance* instance, const QString& gameRoot, const QString& path);

// The game folder of an instance folder, which may not be loaded as an instance yet
QString gameRootOf(const QString& instanceRoot);

// Where a file of an instance is, for the store. The root is the game folder.
ContentStore::Destination destination(const ContentStore& store, const QString& instanceId, const QString& gameRoot, const QString& path);

// Whether a file may be shared: a file directly in a content folder of the game folder, and not metadata, a shader
// configuration or a temporary file
bool isShareable(const QString& gameRoot, const QString& path);

// The files of the content folders of a game folder that may be shared
QStringList shareableFiles(const QString& gameRoot);

// The name an exclusion is kept under: the path in the game folder, without ".disabled"
QString exclusionKey(const QString& relativePath);
// Whether the user chose to keep the file at relativePath local, so it is never shared
bool isExcluded(BaseInstance* instance, const QString& relativePath);
// Moves the exclusion of a file to its new name, such as after an update
void renameExclusion(BaseInstance* instance, const QString& from, const QString& to);
// Keeps a file local or not. unsharedFrom is the stored file a local copy was made from, to revert to.
void setExcluded(BaseInstance* instance, const QString& relativePath, bool excluded, const QString& unsharedFrom = {});
// The stored file a file kept local was copied from
QString unsharedFrom(BaseInstance* instance, const QString& relativePath);

// "Keep local copy": turns a shared file into a writable local copy of the bytes it shows, and keeps it local from now on,
// also through updates. A file that already is local is just kept local.
Result<> keepLocal(ContentStore& store, BaseInstance* instance, const ContentStore::Destination& destination);

// Whether "Revert to shared version" can work: the file was kept local and the stored file it came from is intact
bool canRevert(ContentStore& store, BaseInstance* instance, const QString& relativePath);

// "Revert to shared version": replaces the local file with a link to the stored file it was copied from, discarding
// local changes, and shares it again. Fails if the file changed since replaces was taken, when the user confirmed.
Result<> revertToShared(ContentStore& store,
                        BaseInstance* instance,
                        const ContentStore::Destination& destination,
                        const FS::FileIdentity& replaces);

// Stores the file at source and links it at the destination, replacing what is there, as an install or download does
Result<PlacementKind> installFile(ContentStore& store,
                                  const ContentStore::Destination& destination,
                                  const QString& source,
                                  ContentStore::IngestMode mode);

// Shares files the launcher just produced in a game folder, such as by importing a modpack or copying an instance:
// each becomes a stored file, or a link to an identical one. Files with other hard links, links, and files that can't
// be shared are left alone. excluded tells which relative paths stay local. Returns how many were shared.
int shareFreshFiles(ContentStore& store,
                    const QString& instanceId,
                    const QString& gameRoot,
                    const QStringList& paths,
                    const std::function<bool(const QString&)>& excluded = {});

// Removes a resource file of an instance: a symbolic link is first turned into a local copy, so a trashed file never
// points into the store, then remove runs, and only if it succeeded the link is forgotten
Result<> removeFile(ContentStore& store, const RefKey& key, const std::function<bool()>& remove);

// Removes an instance folder the same way: all its symbolic links are turned into local copies first, and if any of that
// fails nothing is removed
Result<> removeInstance(ContentStore& store, const QString& owner, const std::function<bool()>& remove);

}  // namespace SharedContent
