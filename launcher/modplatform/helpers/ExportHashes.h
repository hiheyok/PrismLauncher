#pragma once

#include <QJsonArray>
#include <QMap>
#include <QString>

#include "minecraft/mod/MetadataHandler.h"
#include "modplatform/helpers/HashUtils.h"

namespace ExportHashes {

using FileHashes = QMap<Hashing::Algorithm, QString>;

// Whether the hash saved in the metadata matches the file, meaning the file is unchanged since it was downloaded
bool matchesMetadata(const Metadata::ModStruct& metadata, const FileHashes& fileHashes);

// Whether one of the hashes of a CurseForge file object matches the file
bool matchesCurseForgeFile(const QJsonArray& curseForgeHashes, const FileHashes& fileHashes);

}  // namespace ExportHashes
