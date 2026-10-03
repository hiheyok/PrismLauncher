#include "FileSystemPrimitives.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <vector>

#include "FileSystem.h"
#include "StringUtils.h"

#if defined(Q_OS_WIN)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace fs = std::filesystem;

namespace FS {

namespace Testing {
namespace {
FaultHook s_faultHook;
CallRecorder s_callRecorder;
}  // namespace

void setFaultHook(FaultHook hook)
{
    s_faultHook = std::move(hook);
}

void setCallRecorder(CallRecorder recorder)
{
    s_callRecorder = std::move(recorder);
}
}  // namespace Testing

namespace {
Result<> checkFaultHook(Testing::Operation operation, const QString& path)
{
    if (Testing::s_callRecorder) {
        Testing::s_callRecorder(operation, path);
    }
    if (Testing::s_faultHook && Testing::s_faultHook(operation, path)) {
        return std::unexpected(QString("Injected failure for %1").arg(path));
    }
    return {};
}

#if defined(Q_OS_WIN)

// Values from the Windows SDK, defined here because older headers don't have them
constexpr auto FILE_DISPOSITION_INFO_EX_CLASS = static_cast<FILE_INFO_BY_HANDLE_CLASS>(21);
constexpr auto FILE_RENAME_INFO_EX_CLASS = static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);
constexpr auto FILE_ID_INFO_CLASS = static_cast<FILE_INFO_BY_HANDLE_CLASS>(18);
constexpr DWORD DISPOSITION_DELETE = 0x1;
constexpr DWORD DISPOSITION_POSIX_SEMANTICS = 0x2;
constexpr DWORD DISPOSITION_IGNORE_READONLY_ATTRIBUTE = 0x10;
constexpr DWORD RENAME_REPLACE_IF_EXISTS = 0x1;
constexpr DWORD RENAME_POSIX_SEMANTICS = 0x2;
constexpr DWORD RENAME_IGNORE_READONLY_ATTRIBUTE = 0x40;

struct DispositionInfoEx {
    DWORD flags;
};

struct RenameInfoEx {
    DWORD flags;
    HANDLE rootDirectory;
    DWORD fileNameLength;
    WCHAR fileName[1];
};

struct IdInfo {
    ULONGLONG volumeSerialNumber;
    BYTE fileId[16];
};

class Handle {
   public:
    explicit Handle(HANDLE handle) : m_handle(handle) {}
    ~Handle()
    {
        if (isValid()) {
            CloseHandle(m_handle);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    bool isValid() const { return m_handle != INVALID_HANDLE_VALUE; }
    HANDLE get() const { return m_handle; }

   private:
    HANDLE m_handle;
};

std::wstring nativePath(const QString& path)
{
    return QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()).toStdWString();
}

QString errorString(DWORD error)
{
    return QString::fromStdString(std::system_category().message(static_cast<int>(error)));
}

Result<> lastError(const QString& action, const QString& path)
{
    return std::unexpected(QString("%1 %2: %3").arg(action, path, errorString(GetLastError())));
}

constexpr DWORD SHARE_ALL = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

Handle openEntry(const QString& path, DWORD access, bool followLinks = false)
{
    const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | (followLinks ? 0 : FILE_FLAG_OPEN_REPARSE_POINT);
    return Handle(CreateFileW(nativePath(path).c_str(), access, SHARE_ALL, nullptr, OPEN_EXISTING, flags, nullptr));
}

bool isUnsupported(DWORD error)
{
    return error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION;
}

// Clears the read-only attribute of a file while it is replaced or deleted, and restores it on the file afterwards.
// Attributes belong to the file, not the path, so this restores them for every other hard link to it.
class ScopedWritable {
   public:
    explicit ScopedWritable(const QString& path) : m_handle(openEntry(path, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES))
    {
        if (!m_handle.isValid() || !GetFileInformationByHandleEx(m_handle.get(), FileBasicInfo, &m_info, sizeof(m_info))) {
            return;
        }
        if (m_info.FileAttributes & FILE_ATTRIBUTE_READONLY) {
            FILE_BASIC_INFO writable = m_info;
            writable.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
            if (writable.FileAttributes == 0) {
                writable.FileAttributes = FILE_ATTRIBUTE_NORMAL;
            }
            m_cleared = SetFileInformationByHandle(m_handle.get(), FileBasicInfo, &writable, sizeof(writable));
        }
    }
    ~ScopedWritable()
    {
        if (m_cleared) {
            // fails harmlessly if this was the last link and the file is gone
            SetFileInformationByHandle(m_handle.get(), FileBasicInfo, &m_info, sizeof(m_info));
        }
    }
    ScopedWritable(const ScopedWritable&) = delete;
    ScopedWritable& operator=(const ScopedWritable&) = delete;

   private:
    Handle m_handle;
    FILE_BASIC_INFO m_info{};
    bool m_cleared = false;
};

qint64 fileTimeToNanoseconds(LARGE_INTEGER time)
{
    // 100 nanosecond intervals
    return time.QuadPart * 100;
}

#else

QByteArray encodedPath(const QString& path)
{
    return QFile::encodeName(path);
}

Result<> lastError(const QString& action, const QString& path)
{
    return std::unexpected(QString("%1 %2: %3").arg(action, path, QString::fromStdString(std::generic_category().message(errno))));
}

Result<struct stat> statEntry(const QString& path, bool followLinks)
{
    struct stat info{};
    const auto encoded = encodedPath(path);
    const int result = followLinks ? stat(encoded.constData(), &info) : lstat(encoded.constData(), &info);
    if (result != 0) {
        TRY(lastError("Failed to stat", path))
    }
    return info;
}

FileId fileIdFromStat(const struct stat& info)
{
    const auto inode = static_cast<quint64>(info.st_ino);
    return { static_cast<quint64>(info.st_dev), QByteArray(reinterpret_cast<const char*>(&inode), sizeof(inode)) };
}

qint64 toNanoseconds(const struct timespec& time)
{
    return static_cast<qint64>(time.tv_sec) * 1000000000 + time.tv_nsec;
}

Result<> syncDescriptor(int descriptor, const QString& path)
{
#if defined(Q_OS_MACOS)
    // fsync doesn't flush the drive's cache on macOS
    if (fcntl(descriptor, F_FULLFSYNC) == 0) {
        return {};
    }
#endif
    if (fsync(descriptor) != 0) {
        TRY(lastError("Failed to flush", path))
    }
    return {};
}

Result<> flushPath(const QString& path, int flags)
{
    const int descriptor = open(encodedPath(path).constData(), flags | O_CLOEXEC);
    if (descriptor < 0) {
        TRY(lastError("Failed to open", path))
    }
    auto result = syncDescriptor(descriptor, path);
    close(descriptor);
    return result;
}

#endif
}  // namespace

Result<FileId> fileId(const QString& path, bool followLinks)
{
#if defined(Q_OS_WIN)
    const auto handle = openEntry(path, FILE_READ_ATTRIBUTES, followLinks);
    if (!handle.isValid()) {
        TRY(lastError("Failed to open", path))
    }
    IdInfo idInfo{};
    if (GetFileInformationByHandleEx(handle.get(), FILE_ID_INFO_CLASS, &idInfo, sizeof(idInfo))) {
        return FileId{ idInfo.volumeSerialNumber, QByteArray(reinterpret_cast<const char*>(idInfo.fileId), sizeof(idInfo.fileId)) };
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle.get(), &info)) {
        TRY(lastError("Failed to identify", path))
    }
    const quint64 index = (static_cast<quint64>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    return FileId{ info.dwVolumeSerialNumber, QByteArray(reinterpret_cast<const char*>(&index), sizeof(index)) };
#else
    TRY_INTO(const auto info, statEntry(path, followLinks))
    return fileIdFromStat(info);
#endif
}

Result<FileIdentity> identity(const QString& path)
{
#if defined(Q_OS_WIN)
    TRY_INTO(auto id, fileId(path))
    const auto handle = openEntry(path, FILE_READ_ATTRIBUTES);
    FILE_BASIC_INFO basicInfo{};
    FILE_STANDARD_INFO standardInfo{};
    if (!handle.isValid() || !GetFileInformationByHandleEx(handle.get(), FileBasicInfo, &basicInfo, sizeof(basicInfo)) ||
        !GetFileInformationByHandleEx(handle.get(), FileStandardInfo, &standardInfo, sizeof(standardInfo))) {
        TRY(lastError("Failed to read the metadata of", path))
    }
    return FileIdentity{ std::move(id), standardInfo.EndOfFile.QuadPart, fileTimeToNanoseconds(basicInfo.LastWriteTime),
                         fileTimeToNanoseconds(basicInfo.ChangeTime) };
#else
    TRY_INTO(const auto info, statEntry(path, false))
#if defined(Q_OS_MACOS)
    return FileIdentity{ fileIdFromStat(info), static_cast<qint64>(info.st_size), toNanoseconds(info.st_mtimespec),
                         toNanoseconds(info.st_ctimespec) };
#else
    return FileIdentity{ fileIdFromStat(info), static_cast<qint64>(info.st_size), toNanoseconds(info.st_mtim),
                         toNanoseconds(info.st_ctim) };
#endif
#endif
}

bool sameVolume(const QString& first, const QString& second)
{
    const auto firstId = fileId(nearestExistentAncestor(first), true);
    const auto secondId = fileId(nearestExistentAncestor(second), true);
    return firstId && secondId && firstId->volume == secondId->volume;
}

bool probeHardLink(const QString& sourceDir, const QString& targetDir)
{
    const auto name = QString(".prism-probe-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto source = QDir(sourceDir).filePath(name);
    const auto target = QDir(targetDir).filePath(name + "-link");

    QFile file(source);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.close();

    std::error_code error;
    fs::create_hard_link(StringUtils::toStdString(source), StringUtils::toStdString(target), error);
    const bool linked = !error;
    if (linked) {
        QFile::remove(target);
    }
    QFile::remove(source);
    return linked;
}

Result<> replaceFile(const QString& source, const QString& target)
{
    TRY(checkFaultHook(Testing::Operation::Replace, target))
#if defined(Q_OS_WIN)
    {
        const auto handle = openEntry(source, DELETE | SYNCHRONIZE);
        if (!handle.isValid()) {
            TRY(lastError("Failed to open", source))
        }
        const auto targetPath = nativePath(target);
        const auto nameBytes = static_cast<DWORD>(targetPath.size() * sizeof(WCHAR));
        std::vector<char> buffer(sizeof(RenameInfoEx) + nameBytes);
        auto* info = reinterpret_cast<RenameInfoEx*>(buffer.data());
        info->flags = RENAME_REPLACE_IF_EXISTS | RENAME_POSIX_SEMANTICS | RENAME_IGNORE_READONLY_ATTRIBUTE;
        info->rootDirectory = nullptr;
        info->fileNameLength = nameBytes;
        memcpy(info->fileName, targetPath.c_str(), nameBytes);
        if (SetFileInformationByHandle(handle.get(), FILE_RENAME_INFO_EX_CLASS, info, static_cast<DWORD>(buffer.size()))) {
            return {};
        }
        if (const auto error = GetLastError(); !isUnsupported(error)) {
            return std::unexpected(QString("Failed to replace %1 with %2: %3").arg(target, source, errorString(error)));
        }
    }
    // older Windows versions and file systems without the extended rename
    ScopedWritable writable(target);
    if (!MoveFileExW(nativePath(source).c_str(), nativePath(target).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        TRY(lastError("Failed to replace", target))
    }
    return {};
#else
    if (rename(encodedPath(source).constData(), encodedPath(target).constData()) != 0) {
        TRY(lastError("Failed to replace", target))
    }
    return {};
#endif
}

Result<> deleteLink(const QString& path)
{
    TRY(checkFaultHook(Testing::Operation::Delete, path))
#if defined(Q_OS_WIN)
    {
        const auto handle = openEntry(path, DELETE);
        if (!handle.isValid()) {
            TRY(lastError("Failed to open", path))
        }
        DispositionInfoEx info{ DISPOSITION_DELETE | DISPOSITION_POSIX_SEMANTICS | DISPOSITION_IGNORE_READONLY_ATTRIBUTE };
        if (SetFileInformationByHandle(handle.get(), FILE_DISPOSITION_INFO_EX_CLASS, &info, sizeof(info))) {
            return {};
        }
        if (const auto error = GetLastError(); !isUnsupported(error)) {
            return std::unexpected(QString("Failed to delete %1: %2").arg(path, errorString(error)));
        }
    }
    // older Windows versions and file systems without the extended disposition
    ScopedWritable writable(path);
    const auto native = nativePath(path);
    const auto attributes = GetFileAttributesW(native.c_str());
    const bool isDirectoryLink =
        attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
    if (!(isDirectoryLink ? RemoveDirectoryW(native.c_str()) : DeleteFileW(native.c_str()))) {
        TRY(lastError("Failed to delete", path))
    }
    return {};
#else
    if (unlink(encodedPath(path).constData()) != 0) {
        TRY(lastError("Failed to delete", path))
    }
    return {};
#endif
}

Result<> deleteTree(const QString& path)
{
    const auto root = StringUtils::toStdString(path);
    std::error_code error;
    const auto status = fs::symlink_status(root, error);
    if (error || status.type() == fs::file_type::not_found) {
        return {};
    }
    if (status.type() != fs::file_type::directory) {
        return deleteLink(path);
    }

    // collect the entries first, then delete the deepest ones first so directories are empty when removed
    std::vector<fs::path> entries;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::none, error); !error && it != fs::end(it);
         it.increment(error)) {
        entries.push_back(it->path());
    }
    if (error) {
        return std::unexpected(QString("Failed to list %1: %2").arg(path, QString::fromStdString(error.message())));
    }
    std::sort(entries.begin(), entries.end(),
              [](const fs::path& a, const fs::path& b) { return std::distance(a.begin(), a.end()) > std::distance(b.begin(), b.end()); });
    entries.push_back(root);

    for (const auto& entry : entries) {
        const auto entryStatus = fs::symlink_status(entry, error);
        if (!error && entryStatus.type() == fs::file_type::directory) {
            fs::remove(entry, error);
            if (error) {
                return std::unexpected(QString("Failed to delete %1: %2").arg(path, QString::fromStdString(error.message())));
            }
        } else {
            TRY(deleteLink(StringUtils::fromStdString(entry.native())))
        }
    }
    return {};
}

Result<> flushFile(const QString& path)
{
    TRY(checkFaultHook(Testing::Operation::FlushFile, path))
#if defined(Q_OS_WIN)
    const Handle handle(
        CreateFileW(nativePath(path).c_str(), GENERIC_WRITE, SHARE_ALL, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle.isValid()) {
        TRY(lastError("Failed to open", path))
    }
    if (!FlushFileBuffers(handle.get())) {
        TRY(lastError("Failed to flush", path))
    }
    return {};
#else
    return flushPath(path, O_RDONLY);
#endif
}

Result<> flushDir(const QString& path)
{
    TRY(checkFaultHook(Testing::Operation::FlushDir, path))
#if defined(Q_OS_WIN)
    const Handle handle(
        CreateFileW(nativePath(path).c_str(), GENERIC_WRITE, SHARE_ALL, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (handle.isValid() && FlushFileBuffers(handle.get())) {
        return {};
    }
    const auto error = GetLastError();
    if (error == ERROR_ACCESS_DENIED || isUnsupported(error)) {
        // some file systems can't flush directories; NTFS journals directory changes
        return {};
    }
    return std::unexpected(QString("Failed to flush %1: %2").arg(path, errorString(error)));
#else
    return flushPath(path, O_RDONLY | O_DIRECTORY);
#endif
}

}  // namespace FS
