#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

#include "Result.h"
#include "contentstore/StoreFormat.h"

// The write-ahead log of a store's reference table: refs.json holds a snapshot of the table, and numbered journal
// segments hold the records written after it. Every change is written and flushed here before it is applied.
//
// Each segment starts with a header line carrying the store format. Each record is one line of
// "<sequence number> <CRC-32 of the JSON> <JSON>", so a record cut short by a crash is recognized and ignored.
class Journal {
   public:
    static constexpr auto SnapshotFileName = "refs.json";

    struct Contents {
        // the most restrictive format of the snapshot and every segment
        StoreFormat format;
        QJsonObject snapshot;
        // records written after the snapshot, in order
        QList<QJsonObject> records;
        // the sequence number of the last record, in the snapshot or a segment
        qint64 lastSequence = 0;
        // a last record was cut short and ignored
        bool hadTornRecord = false;
    };

    explicit Journal(QString storeDir);

    // Reads the snapshot and the records after it. Fails if a record other than the last one is damaged.
    Result<Contents> load();

    // Durably appends records, in order. load must have been called first.
    Result<> append(const QList<QJsonObject>& records);

    // Replaces the snapshot with one that includes every record so far, then starts a new segment and removes the old
    // ones. Safe to interrupt at any point: loading afterwards gives the same table.
    Result<> compact(const QJsonObject& snapshot, const StoreFormat& format);

    qint64 lastSequence() const { return m_lastSequence; }
    QString storeDir() const { return m_storeDir; }

   private:
    QList<int> segmentNumbers() const;
    QString segmentPath(int number) const;
    Result<> startSegment(int number, const StoreFormat& format);

    QString m_storeDir;
    qint64 m_lastSequence = 0;
    int m_segment = 0;
    // set when there is no segment yet, or the last one ends with a torn record
    bool m_needsNewSegment = true;
    // set after a failed write, which may have left part of a record on disk
    bool m_failed = false;
    StoreFormat m_format;
};
