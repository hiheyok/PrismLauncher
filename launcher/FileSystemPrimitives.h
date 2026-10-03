#pragma once

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <functional>

#include "Result.h"

// Low level file operations with exact semantics, for code that must never write through a link or lose a file
namespace FS {

// Identifies a file on disk: two paths with the same FileId are hard links to the same file
struct FileId {
    quint64 volume = 0;
    // 8 bytes, or 16 bytes on Windows file systems with 128 bit ids such as ReFS
    QByteArray id;

    bool isValid() const { return !id.isEmpty(); }
    bool operator==(const FileId&) const = default;
};

struct FileIdentity {
    FileId fileId;
    qint64 size = 0;
    // nanoseconds, only comparable with values from the same machine
    qint64 modifiedTime = 0;
    // metadata change time: changes on writes, permission changes and when hard links are added or removed
    qint64 changeTime = 0;

    bool operator==(const FileIdentity&) const = default;
};

/**
 * The id of the file at path. Without followLinks a symbolic link is identified itself, not its target.
 */
Result<FileId> fileId(const QString& path, bool followLinks = false);

/**
 * Id, size and modification times of the file at path, without following a symbolic link.
 */
Result<FileIdentity> identity(const QString& path);

/**
 * Whether both paths, or their nearest existing ancestors, are on the same volume.
 */
bool sameVolume(const QString& first, const QString& second);

/**
 * Whether a file in sourceDir can be hard linked into targetDir, tested by actually creating and removing a link.
 */
bool probeHardLink(const QString& sourceDir, const QString& targetDir);

/**
 * Atomically puts source at target, replacing what is there. A symbolic link at target is replaced, not followed.
 * Both paths must be on the same volume. On Windows a read-only target is replaced without changing its attributes,
 * so other hard links to it stay read-only.
 */
Result<> replaceFile(const QString& source, const QString& target);

/**
 * Removes one directory entry: a file, one hard link to it or a symbolic link (never its target).
 * On Windows a read-only file is removed without changing its attributes, so other hard links to it stay read-only.
 */
Result<> deleteLink(const QString& path);

/**
 * Removes a directory tree like deletePath, but also removes read-only files on Windows using deleteLink.
 */
Result<> deleteTree(const QString& path);

/**
 * Creates a new empty file next to target, named "<target>.<tag>-<random>", and returns its path.
 * It is created exclusively, so the path is never an existing file or a symbolic link.
 */
Result<QString> reserveTemporarySibling(const QString& target, const QString& tag);

/**
 * Writes the file's data to disk. Must be called before making the file read-only, as Windows needs write access.
 */
Result<> flushFile(const QString& path);

/**
 * Writes the directory's entries to disk, making creating, renaming and removing files in it durable.
 * On Windows this is best effort: some file systems refuse it, and NTFS journals directory changes anyway.
 */
Result<> flushDir(const QString& path);

namespace Testing {
enum class Operation : std::uint8_t { Replace, Delete, FlushFile, FlushDir };

// Called before each operation. Returning true makes the operation fail without doing anything.
using FaultHook = std::function<bool(Operation operation, const QString& path)>;
void setFaultHook(FaultHook hook);

// Called before each operation, to check the order of operations
using CallRecorder = std::function<void(Operation operation, const QString& path)>;
void setCallRecorder(CallRecorder recorder);
}  // namespace Testing

}  // namespace FS
