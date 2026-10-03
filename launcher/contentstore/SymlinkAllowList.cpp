#include "SymlinkAllowList.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "FileSystemPrimitives.h"

namespace SymlinkAllowList {

QString path(const QString& gameRoot)
{
    return QDir(gameRoot).filePath("allowed_symlinks.txt");
}

QString entryFor(const QString& dir)
{
    // Minecraft compares the link target as the platform writes paths, and "prefix" matches everything inside dir
    auto prefix = QDir::toNativeSeparators(QDir::cleanPath(QFileInfo(dir).absoluteFilePath()));
    if (!prefix.endsWith(QDir::separator())) {
        prefix += QDir::separator();
    }
    return "[prefix]" + prefix;
}

Result<> allow(const QString& gameRoot, const QString& dir)
{
    const auto file = path(gameRoot);
    const auto entry = entryFor(dir);

    QByteArray contents;
    if (QFileInfo::exists(file)) {
        QFile existing(file);
        if (!existing.open(QIODevice::ReadOnly)) {
            return std::unexpected(QString("Failed to read %1: %2").arg(file, existing.errorString()));
        }
        contents = existing.readAll();
        for (const auto& line : QString::fromUtf8(contents).split('\n')) {
            if (line.trimmed() == entry) {
                return {};
            }
        }
        if (!contents.isEmpty() && !contents.endsWith('\n')) {
            contents.append('\n');
        }
    }
    contents.append(entry.toUtf8());
    contents.append('\n');

    TRY_INTO(const auto temporary, FS::reserveTemporarySibling(file, "new"))
    const auto discard = [&temporary](const QString& error) -> Result<> {
        if (auto deleted = FS::deleteLink(temporary); !deleted) {
            return std::unexpected(QString("%1, and %2 was left behind").arg(error, temporary));
        }
        return std::unexpected(error);
    };
    {
        QFile output(temporary);
        if (!output.open(QIODevice::WriteOnly | QIODevice::ExistingOnly) || output.write(contents) != contents.size() || !output.flush()) {
            return discard(QString("Failed to write %1: %2").arg(temporary, output.errorString()));
        }
    }
    if (auto written = FS::flushFile(temporary).and_then([&] { return FS::replaceFile(temporary, file); }); !written) {
        return discard(written.error());
    }
    return {};
}

}  // namespace SymlinkAllowList
