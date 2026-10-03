#pragma once

#include <QString>

#include "Result.h"

// File operations for store objects
namespace ObjectFiles {

// Where the object with this SHA-256 is kept: objects/<first two characters>/<hash>
QString objectPath(const QString& objectsDir, const QString& hash);

// The SHA-256 of a whole file. Fails on a read error, or if the file changed size while it was read.
Result<QString> sha256(const QString& path);

// Copies source into the existing file target and returns the SHA-256 of the bytes copied.
// Fails if the source changed while it was copied.
Result<QString> copyAndHash(const QString& source, const QString& target);

bool makeReadOnly(const QString& path);

// Whether both paths name the same location, compared the way the platform compares names
bool samePath(const QString& first, const QString& second);

}  // namespace ObjectFiles
