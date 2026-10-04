#pragma once

#include <QString>
#include <QStringList>

#include <functional>

#include "Result.h"
#include "contentstore/ContentStore.h"

class BaseInstance;

// Connects instances to the shared store: which files are shared, and how the launcher's own file operations keep the
// store's records right
namespace SharedContent {

// The store, if sharing is on and this launcher can change the store
ContentStore* store();

// The store to share an instance's files through, as store()
ContentStore* storeFor(const BaseInstance* instance);

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
