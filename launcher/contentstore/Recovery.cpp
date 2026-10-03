#include "Recovery.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QSet>

#include "FileSystemPrimitives.h"
#include "contentstore/ObjectFiles.h"

namespace Recovery {

namespace {
struct PathState {
    bool exists = false;
    bool isSymbolicLink = false;
    QString target;
    std::optional<FS::FileId> fileId;
};

PathState inspect(const QString& path)
{
    PathState state;
    const QFileInfo info(path);
    state.isSymbolicLink = info.isSymLink();
    state.exists = info.exists() || state.isSymbolicLink;
    if (state.isSymbolicLink) {
        state.target = info.symLinkTarget();
    }
    if (auto id = FS::fileId(path); id) {
        state.fileId = *id;
    }
    return state;
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

bool holdsPlacement(const Transaction& transaction, const PathState& state)
{
    if (!transaction.preparedKind) {
        // the swap only happens after the placement was prepared
        return false;
    }
    if (*transaction.preparedKind == PlacementKind::Symbolic) {
        return state.isSymbolicLink && samePath(state.target, transaction.expected);
    }
    return !state.isSymbolicLink && state.fileId && fileIdString(*state.fileId) == transaction.expected;
}

bool holdsOldFile(const Transaction& transaction, const PathState& state)
{
    if (!transaction.oldIdentity) {
        return !state.exists;
    }
    return state.fileId && transaction.oldIdentity->sameFile(*state.fileId);
}
}  // namespace

QList<QJsonObject> finishTransactions(const RefTable& table)
{
    QList<QJsonObject> records;
    for (const auto& transaction : table.transactions()) {
        const auto root = table.owners().find(transaction.key.owner);
        if (root == table.owners().end()) {
            continue;
        }
        const auto path = QDir(*root).filePath(transaction.key.relativePath);
        const auto state = inspect(path);

        if (holdsPlacement(transaction, state)) {
            records.append(RefRecord::commit(transaction.id));
            continue;
        }

        // the placement didn't happen: its temporary file is only ours to remove
        if (!transaction.temporaryPath.isEmpty() &&
            (QFileInfo::exists(transaction.temporaryPath) || QFileInfo(transaction.temporaryPath).isSymLink())) {
            if (auto deleted = FS::deleteLink(transaction.temporaryPath); !deleted) {
                qWarning() << "Shared store:" << deleted.error();
            }
        }
        records.append(RefRecord::abort(transaction.id));

        if (!holdsOldFile(transaction, state) && table.ref(transaction.key)) {
            records.append(RefRecord::setRefState(transaction.key, state.exists ? RefState::Replaced : RefState::Missing));
        }
    }
    return records;
}

QList<QJsonObject> finishPublications(const RefTable& table, const QString& objectsDir)
{
    QList<QJsonObject> records;
    if (!QFileInfo(objectsDir).isDir()) {
        // the store isn't readable, so nothing is definitely gone
        return records;
    }
    // stored files that links, or placements still in progress, depend on
    QSet<QString> linked;
    for (const auto& ref : table.refs()) {
        linked.insert(ref.hash);
    }
    for (const auto& transaction : table.transactions()) {
        linked.insert(transaction.newHash);
        if (transaction.oldHash) {
            linked.insert(*transaction.oldHash);
        }
    }
    for (const auto& entry : table.entries()) {
        if (!entry.current || linked.contains(entry.hash)) {
            continue;
        }
        const auto path = ObjectFiles::objectPath(objectsDir, entry.hash);
        if (!QFileInfo::exists(path) && !QFileInfo(path).isSymLink()) {
            records.append(RefRecord::destroyed(entry.hash, entry.current->id));
        }
    }
    return records;
}

}  // namespace Recovery
