#include "Journal.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>

#include <algorithm>

#include <zlib.h>

#include "FileSystemPrimitives.h"

namespace {
constexpr auto g_segmentPrefix = "refs.journal.";

QString checksum(const QByteArray& data)
{
    const auto crc = crc32(0, reinterpret_cast<const Bytef*>(data.constData()), static_cast<uInt>(data.size()));
    return QString::number(crc, 16).rightJustified(8, '0');
}

QByteArray recordLine(qint64 sequence, const QJsonObject& record)
{
    const auto json = QJsonDocument(record).toJson(QJsonDocument::Compact);
    return QString("%1 %2 ").arg(sequence).arg(checksum(json)).toUtf8() + json + '\n';
}

struct ParsedLine {
    qint64 sequence = 0;
    QJsonObject record;
};

// A record line, or nothing if it is damaged
std::optional<ParsedLine> parseLine(const QByteArray& line)
{
    const auto first = line.indexOf(' ');
    const auto second = first < 0 ? -1 : line.indexOf(' ', first + 1);
    if (second < 0) {
        return std::nullopt;
    }
    bool ok = false;
    const auto sequence = line.left(first).toLongLong(&ok);
    const auto json = line.mid(second + 1);
    if (!ok || line.mid(first + 1, second - first - 1) != checksum(json).toUtf8()) {
        return std::nullopt;
    }
    const auto document = QJsonDocument::fromJson(json);
    if (!document.isObject()) {
        return std::nullopt;
    }
    return ParsedLine{ sequence, document.object() };
}

Result<StoreFormat> formatFrom(const QJsonValue& value, const QString& file)
{
    auto format = StoreFormat::fromJson(value.toObject());
    if (!format) {
        return std::unexpected(QString("Invalid format header in %1: %2").arg(file, format.error()));
    }
    return format;
}

Result<> writeDurably(const QString& path, const QByteArray& data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::ExistingOnly | QIODevice::Truncate)) {
        return std::unexpected(QString("Failed to open %1: %2").arg(path, file.errorString()));
    }
    if (file.write(data) != data.size() || !file.flush()) {
        return std::unexpected(QString("Failed to write %1: %2").arg(path, file.errorString()));
    }
    file.close();
    return FS::flushFile(path);
}
}  // namespace

Journal::Journal(QString storeDir) : m_storeDir(std::move(storeDir)) {}

QString Journal::segmentPath(int number) const
{
    return QDir(m_storeDir).filePath(g_segmentPrefix + QString::number(number));
}

QList<int> Journal::segmentNumbers() const
{
    QList<int> numbers;
    for (const auto& name : QDir(m_storeDir).entryList({ QString(g_segmentPrefix) + "*" }, QDir::Files | QDir::Hidden)) {
        bool ok = false;
        const auto number = name.mid(QString(g_segmentPrefix).size()).toInt(&ok);
        if (ok) {
            numbers.append(number);
        }
    }
    std::ranges::sort(numbers);
    return numbers;
}

Result<Journal::Contents> Journal::load()
{
    Contents contents;

    QFile snapshotFile(QDir(m_storeDir).filePath(SnapshotFileName));
    if (snapshotFile.exists()) {
        if (!snapshotFile.open(QIODevice::ReadOnly)) {
            return std::unexpected(QString("Failed to open %1: %2").arg(snapshotFile.fileName(), snapshotFile.errorString()));
        }
        const auto document = QJsonDocument::fromJson(snapshotFile.readAll());
        if (!document.isObject()) {
            return std::unexpected(QString("Invalid snapshot %1").arg(snapshotFile.fileName()));
        }
        const auto json = document.object();
        TRY_INTO(const auto format, formatFrom(json["format"], snapshotFile.fileName()))
        contents.format = StoreFormat::mostRestrictive(contents.format, format);
        contents.snapshot = json["table"].toObject();
        contents.lastSequence = json["lastSequence"].toInteger();
    }
    const auto snapshotSequence = contents.lastSequence;

    const auto numbers = segmentNumbers();
    for (const auto number : numbers) {
        QFile file(segmentPath(number));
        if (!file.open(QIODevice::ReadOnly)) {
            return std::unexpected(QString("Failed to open %1: %2").arg(file.fileName(), file.errorString()));
        }
        auto lines = file.readAll().split('\n');
        // a complete file ends with a line break, leaving an empty last entry; anything else there is a torn line
        const bool endsCleanly = lines.last().isEmpty();
        if (endsCleanly) {
            lines.removeLast();
        }
        if (lines.isEmpty()) {
            // created but its header was never written
            continue;
        }

        const auto header = QJsonDocument::fromJson(lines.first());
        if (!header.isObject()) {
            if (lines.size() == 1 && !endsCleanly) {
                continue;
            }
            return std::unexpected(QString("Invalid header in %1").arg(file.fileName()));
        }
        TRY_INTO(const auto format, formatFrom(header.object()["format"], file.fileName()))
        contents.format = StoreFormat::mostRestrictive(contents.format, format);

        for (int i = 1; i < lines.size(); i++) {
            const auto parsed = parseLine(lines[i]);
            if (!parsed) {
                // only the line written last can be cut short by a crash
                if (i == lines.size() - 1 && !endsCleanly) {
                    contents.hadTornRecord = true;
                    break;
                }
                return std::unexpected(QString("Damaged record %1 in %2").arg(i).arg(file.fileName()));
            }
            if (parsed->sequence <= snapshotSequence) {
                // already part of the snapshot
                continue;
            }
            if (parsed->sequence != contents.lastSequence + 1) {
                return std::unexpected(QString("Missing records before %1 in %2").arg(parsed->sequence).arg(file.fileName()));
            }
            contents.records.append(parsed->record);
            contents.lastSequence = parsed->sequence;
        }
    }

    m_lastSequence = contents.lastSequence;
    // never append after a torn line: the next record goes into a new segment
    m_segment = numbers.isEmpty() ? 0 : numbers.last();
    m_needsNewSegment = numbers.isEmpty() || contents.hadTornRecord;
    m_format = contents.format;
    return contents;
}

Result<> Journal::startSegment(int number, const StoreFormat& format)
{
    const auto path = segmentPath(number);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        return std::unexpected(QString("Failed to create %1: %2").arg(path, file.errorString()));
    }
    const auto header = QJsonDocument(QJsonObject{ { "format", format.toJson() }, { "segment", number } }).toJson(QJsonDocument::Compact);
    if (file.write(header + '\n') != header.size() + 1 || !file.flush()) {
        return std::unexpected(QString("Failed to write %1: %2").arg(path, file.errorString()));
    }
    file.close();
    TRY(FS::flushFile(path))
    TRY(FS::flushDir(m_storeDir))
    m_segment = number;
    m_needsNewSegment = false;
    return {};
}

Result<> Journal::append(const QList<QJsonObject>& records)
{
    if (records.isEmpty()) {
        return {};
    }
    if (m_failed) {
        return std::unexpected(QString("The journal failed earlier and can't be written"));
    }
    if (m_needsNewSegment) {
        TRY(startSegment(m_segment + 1, m_format))
    }

    QByteArray data;
    auto sequence = m_lastSequence;
    for (const auto& record : records) {
        data += recordLine(++sequence, record);
    }

    const auto path = segmentPath(m_segment);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::ExistingOnly)) {
        m_failed = true;
        return std::unexpected(QString("Failed to open %1: %2").arg(path, file.errorString()));
    }
    if (file.write(data) != data.size() || !file.flush()) {
        // part of the batch may be on disk: stop, so nothing is appended after a partial record
        m_failed = true;
        return std::unexpected(QString("Failed to write %1: %2").arg(path, file.errorString()));
    }
    file.close();
    if (auto flushed = FS::flushFile(path); !flushed) {
        m_failed = true;
        return flushed;
    }
    m_lastSequence = sequence;
    return {};
}

Result<> Journal::compact(const QJsonObject& snapshot, const StoreFormat& format)
{
    if (m_failed) {
        return std::unexpected(QString("The journal failed earlier and can't be compacted"));
    }

    // 1. the snapshot, with everything up to the last record
    const auto snapshotPath = QDir(m_storeDir).filePath(SnapshotFileName);
    TRY_INTO(const auto temporary, FS::reserveTemporarySibling(snapshotPath, "prism-new"))
    const auto data =
        QJsonDocument(QJsonObject{ { "format", format.toJson() }, { "lastSequence", m_lastSequence }, { "table", snapshot } }).toJson();
    auto written = writeDurably(temporary, data).and_then([&] { return FS::replaceFile(temporary, snapshotPath); });
    if (!written) {
        QFile::remove(temporary);
        return written;
    }
    TRY(FS::flushDir(m_storeDir))

    // 2. a new segment for the records after it
    const auto oldSegments = segmentNumbers();
    TRY(startSegment(m_segment + 1, format))
    m_format = format;

    // 3. the old segments, which the snapshot now covers
    for (const auto number : oldSegments) {
        if (number < m_segment) {
            TRY(FS::deleteLink(segmentPath(number)))
        }
    }
    return FS::flushDir(m_storeDir);
}
