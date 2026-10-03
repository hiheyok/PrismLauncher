#pragma once

#include <QJsonObject>
#include <QList>

#include "contentstore/RefTable.h"

namespace Recovery {

// Finishes the placements that a crash interrupted, by looking at what is at their paths now, and returns the
// records to journal. Removes leftover temporary files.
//
// - Prepared, and the path holds what the placement created: the swap happened, so it is committed.
// - The path still holds the old file (or nothing, if there was none): the swap didn't happen, so it is aborted.
//   The old file being there never counts as success for the new link.
// - Anything else: aborted, and the old ref is marked replaced or missing for reconciliation. Nothing is deleted.
//
// Placements whose owner has no known root are left open.
QList<QJsonObject> finishTransactions(const RefTable& table);

}  // namespace Recovery
