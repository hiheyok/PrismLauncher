#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>

#include "Result.h"

// What a launcher may do with a store, decided by the store's format header
enum class StoreAccess : std::uint8_t {
    Writable,
    // a newer launcher wrote state this version can't safely change
    ReadOnly,
    // a newer launcher wrote state this version can't read
    Disabled,
};

// The compatibility header of a content store, kept in format.json and later also in every snapshot and journal segment
struct StoreFormat {
    // the format version written by this build
    static constexpr int CurrentVersion = 1;

    int formatVersion = CurrentVersion;
    // the oldest launcher format version that can read the store
    int minReaderVersion = 1;
    // the oldest launcher format version that can change the store
    int minWriterVersion = 1;
    QStringList features;
    // the header had fields this version doesn't know, so it may describe state this version doesn't understand
    bool hasUnknownFields = false;

    bool operator==(const StoreFormat&) const = default;

    QJsonObject toJson() const;
    static Result<StoreFormat> fromJson(const QJsonObject& json);

    // A header that is at least as restrictive as both, for when several copies of the header disagree
    static StoreFormat mostRestrictive(const StoreFormat& first, const StoreFormat& second);

    StoreAccess accessFor(int version = CurrentVersion) const;
};

namespace StoreFormatFile {
constexpr auto g_fileName = "format.json";

// The store's format.json, or an empty optional if it doesn't exist
Result<std::optional<StoreFormat>> read(const QString& storeDir);

// Atomically replaces the store's format.json, durably
Result<> write(const QString& storeDir, const StoreFormat& format);
}  // namespace StoreFormatFile
