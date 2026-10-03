#include "StoreFormat.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>

#include "FileSystemPrimitives.h"

namespace {
const QStringList g_knownFields{ "formatVersion", "minReaderVersion", "minWriterVersion", "features" };

Result<int> readVersion(const QJsonObject& json, const QString& key)
{
    const auto value = json.value(key);
    if (!value.isDouble() || value.toInt(-1) < 1) {
        return std::unexpected(QString("Invalid or missing %1").arg(key));
    }
    return value.toInt();
}
}  // namespace

QJsonObject StoreFormat::toJson() const
{
    return {
        { "formatVersion", formatVersion },
        { "minReaderVersion", minReaderVersion },
        { "minWriterVersion", minWriterVersion },
        { "features", QJsonArray::fromStringList(features) },
    };
}

Result<StoreFormat> StoreFormat::fromJson(const QJsonObject& json)
{
    StoreFormat format;
    TRY_INTO(format.formatVersion, readVersion(json, "formatVersion"))
    TRY_INTO(format.minReaderVersion, readVersion(json, "minReaderVersion"))
    TRY_INTO(format.minWriterVersion, readVersion(json, "minWriterVersion"))
    format.features.clear();
    for (const auto& feature : json.value("features").toArray()) {
        format.features.append(feature.toString());
    }
    format.hasUnknownFields = std::ranges::any_of(json.keys(), [](const QString& key) { return !g_knownFields.contains(key); });
    return format;
}

StoreFormat StoreFormat::mostRestrictive(const StoreFormat& first, const StoreFormat& second)
{
    StoreFormat result;
    result.formatVersion = std::max(first.formatVersion, second.formatVersion);
    result.minReaderVersion = std::max(first.minReaderVersion, second.minReaderVersion);
    result.minWriterVersion = std::max(first.minWriterVersion, second.minWriterVersion);
    result.features = first.features;
    for (const auto& feature : second.features) {
        if (!result.features.contains(feature)) {
            result.features.append(feature);
        }
    }
    result.hasUnknownFields = first.hasUnknownFields || second.hasUnknownFields;
    return result;
}

StoreAccess StoreFormat::accessFor(int version) const
{
    if (minReaderVersion > version) {
        return StoreAccess::Disabled;
    }
    if (minWriterVersion > version || hasUnknownFields) {
        return StoreAccess::ReadOnly;
    }
    return StoreAccess::Writable;
}

namespace StoreFormatFile {

Result<std::optional<StoreFormat>> read(const QString& storeDir)
{
    QFile file(QDir(storeDir).filePath(g_fileName));
    if (!file.exists()) {
        return std::optional<StoreFormat>();
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return std::unexpected(QString("Failed to open %1: %2").arg(file.fileName(), file.errorString()));
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::unexpected(QString("Invalid %1: %2").arg(file.fileName(), error.errorString()));
    }
    TRY_INTO(auto format, StoreFormat::fromJson(document.object()))
    return std::optional<StoreFormat>(std::move(format));
}

Result<> write(const QString& storeDir, const StoreFormat& format)
{
    const auto path = QDir(storeDir).filePath(g_fileName);
    TRY_INTO(const auto temporary, FS::reserveTemporarySibling(path, "prism-new"))

    const auto writeTemporary = [&]() -> Result<> {
        QFile file(temporary);
        if (!file.open(QIODevice::WriteOnly | QIODevice::ExistingOnly | QIODevice::Truncate)) {
            return std::unexpected(QString("Failed to open %1: %2").arg(temporary, file.errorString()));
        }
        const auto data = QJsonDocument(format.toJson()).toJson();
        if (file.write(data) != data.size() || !file.flush()) {
            return std::unexpected(QString("Failed to write %1: %2").arg(temporary, file.errorString()));
        }
        return {};
    };

    auto result = writeTemporary()
                      .and_then([&] { return FS::flushFile(temporary); })
                      .and_then([&] { return FS::replaceFile(temporary, path); })
                      .and_then([&] { return FS::flushDir(storeDir); });
    if (!result) {
        QFile::remove(temporary);
    }
    return result;
}

}  // namespace StoreFormatFile
