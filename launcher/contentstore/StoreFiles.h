#pragma once

#include <QString>

#include "Result.h"

// Helpers for the files the store creates next to instance files
namespace StoreFiles {

struct PathState {
    bool exists = false;
    bool isSymbolicLink = false;
    bool isDirectory = false;
    // for a symbolic link, the absolute path it points to
    QString target;
};

// What is at path, without following a symbolic link
PathState inspect(const QString& path);

// A name for a new file in dir that is hidden from mod loaders and the resource lists
QString temporaryName(const QString& dir);

Result<> createEmptyFile(const QString& path);

// Removes a temporary file or link, which may be read-only
void discardFile(const QString& path);

}  // namespace StoreFiles
