#pragma once

#include <QString>

#include "Result.h"

// Minecraft 1.20 and later refuse symbolically linked packs unless their target is listed in allowed_symlinks.txt
namespace SymlinkAllowList {

// The file in a game folder
QString path(const QString& gameRoot);

// The line that allows every link whose target is inside dir
QString entryFor(const QString& dir);

// Adds the entry for dir to the game folder's list, unless it is already there, and makes the change durable.
// The list is replaced, never written through, so a list that is itself a link to another instance's list is left alone.
Result<> allow(const QString& gameRoot, const QString& dir);

}  // namespace SymlinkAllowList
