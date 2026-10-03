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

// Removes stored files that were recorded but whose file is definitely gone, and that nothing links to.
//
// A publication can be recorded even though writing its record was reported as failed, after which the file was
// moved back to where it came from. Files that instances still link to are left for reconciliation.
QList<QJsonObject> finishPublications(const RefTable& table, const QString& objectsDir);

}  // namespace Recovery
