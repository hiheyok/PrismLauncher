#include "StoreFiles.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include "FileSystemPrimitives.h"

namespace StoreFiles {

PathState inspect(const QString& path)
{
    PathState state;
    const QFileInfo info(path);
    state.isSymbolicLink = info.isSymbolicLink();
    state.exists = info.exists() || state.isSymbolicLink;
    state.isDirectory = !state.isSymbolicLink && info.isDir();
    if (state.isSymbolicLink) {
        state.target = info.symLinkTarget();
    }
    return state;
}

QString temporaryName(const QString& dir)
{
    return QDir(dir).filePath(".prism-new-" + QUuid::createUuid().toString(QUuid::Id128).left(12));
}

Result<> createEmptyFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        return std::unexpected(QString("Failed to create %1: %2").arg(path, file.errorString()));
    }
    return {};
}

void discardFile(const QString& path)
{
    if (QFileInfo::exists(path) || QFileInfo(path).isSymbolicLink()) {
        if (auto deleted = FS::deleteLink(path); !deleted) {
            qWarning() << "Shared store:" << deleted.error();
        }
    }
}

}  // namespace StoreFiles
