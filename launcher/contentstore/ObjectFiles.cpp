#include "ObjectFiles.h"
#include "contentstore/Sha256.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "FileSystemPrimitives.h"

namespace ObjectFiles {

namespace {
constexpr qint64 g_chunkSize = qint64(1024) * 1024;

// The parts of a file's identity that change when its contents change
bool sameContents(const FS::FileIdentity& first, const FS::FileIdentity& second)
{
    return first.fileId == second.fileId && first.size == second.size && first.modifiedTime == second.modifiedTime;
}
}  // namespace

QString objectPath(const QString& objectsDir, const QString& hash)
{
    return QDir(objectsDir).filePath(hash.left(2) + "/" + hash);
}

Result<QString> sha256(const QString& path)
{
    TRY_INTO(const auto before, FS::identity(path))
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::unexpected(QString("Failed to open %1: %2").arg(path, file.errorString()));
    }
    Sha256 hash;
    QByteArray buffer(g_chunkSize, Qt::Uninitialized);
    qint64 total = 0;
    while (true) {
        const auto bytesRead = file.read(buffer.data(), g_chunkSize);
        if (bytesRead < 0) {
            return std::unexpected(QString("Failed to read %1: %2").arg(path, file.errorString()));
        }
        if (bytesRead == 0) {
            break;
        }
        hash.addData(QByteArrayView(buffer.constData(), bytesRead));
        total += bytesRead;
    }
    file.close();
    TRY_INTO(const auto after, FS::identity(path))
    if (total != before.size || !sameContents(before, after)) {
        return std::unexpected(QString("%1 changed while it was read").arg(path));
    }
    return hash.hexResult();
}

Result<QString> copyAndHash(const QString& source, const QString& target)
{
    TRY_INTO(const auto before, FS::identity(source))
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) {
        return std::unexpected(QString("Failed to open %1: %2").arg(source, input.errorString()));
    }
    QFile output(target);
    if (!output.open(QIODevice::WriteOnly | QIODevice::ExistingOnly | QIODevice::Truncate)) {
        return std::unexpected(QString("Failed to open %1: %2").arg(target, output.errorString()));
    }
    Sha256 hash;
    QByteArray buffer(g_chunkSize, Qt::Uninitialized);
    qint64 total = 0;
    while (true) {
        const auto bytesRead = input.read(buffer.data(), g_chunkSize);
        if (bytesRead < 0) {
            return std::unexpected(QString("Failed to read %1: %2").arg(source, input.errorString()));
        }
        if (bytesRead == 0) {
            break;
        }
        if (output.write(buffer.constData(), bytesRead) != bytesRead) {
            return std::unexpected(QString("Failed to write %1: %2").arg(target, output.errorString()));
        }
        hash.addData(QByteArrayView(buffer.constData(), bytesRead));
        total += bytesRead;
    }
    if (!output.flush()) {
        return std::unexpected(QString("Failed to write %1: %2").arg(target, output.errorString()));
    }
    input.close();
    TRY_INTO(const auto after, FS::identity(source))
    if (total != before.size || !sameContents(before, after)) {
        return std::unexpected(QString("%1 changed while it was copied").arg(source));
    }
    return hash.hexResult();
}

bool makeReadOnly(const QString& path)
{
    return QFile::setPermissions(path, QFile::ReadOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther);
}

bool samePath(const QString& first, const QString& second)
{
#if defined(Q_OS_WIN)
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    return QDir::cleanPath(QFileInfo(first).absoluteFilePath())
               .compare(QDir::cleanPath(QFileInfo(second).absoluteFilePath()), sensitivity) == 0;
}

}  // namespace ObjectFiles
