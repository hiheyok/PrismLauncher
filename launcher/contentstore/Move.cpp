#include "contentstore/ContentStore.h"

#include <QDir>
#include <QFileInfo>

#include "FileSystemPrimitives.h"

QList<std::pair<RefKey, Ref>> ContentStore::refsSnapshot() const
{
    QMutexLocker locker(&m_mutex);
    QList<std::pair<RefKey, Ref>> refs;
    for (auto it = m_table.refs().begin(); it != m_table.refs().end(); ++it) {
        refs.append({ it.key(), *it });
    }
    return refs;
}

QString ContentStore::ownerRoot(const QString& owner) const
{
    QMutexLocker locker(&m_mutex);
    return m_table.owners().value(owner);
}

qint64 ContentStore::storedSize(const QString& hash) const
{
    QMutexLocker locker(&m_mutex);
    const auto entry = m_table.entries().find(hash);
    return entry != m_table.entries().end() ? entry->size : 0;
}

bool ContentStore::isIdle() const
{
    QMutexLocker locker(&m_mutex);
    return m_table.transactions().isEmpty() && m_table.pendingBackups().isEmpty();
}

Result<> ContentStore::releaseMoved(const RefKey& key)
{
    QMutexLocker locker(&m_mutex);
    if (m_state != State::Writable) {
        return std::unexpected(QString("The shared store can't be changed"));
    }
    const auto ref = m_table.ref(key);
    if (!ref || !m_table.owners().contains(key.owner)) {
        return {};
    }
    const auto path = ownerPath(key);
    const QFileInfo info(path);
    // still a symbolic link into this store
    if (info.isSymLink() && QDir::cleanPath(info.symLinkTarget()).startsWith(QDir::cleanPath(m_storeDir) + '/', Qt::CaseInsensitive)) {
        return std::unexpected(QString("%1 still links to this store").arg(path));
    }
    // still a hard link to one of this store's files
    if (const auto id = FS::fileId(path); id && !info.isSymLink()) {
        const auto entry = m_table.entries().find(ref->hash);
        if (entry != m_table.entries().end()) {
            const auto* generation = entry->generation(ref->generation);
            if (generation && generation->identity.sameFile(*id)) {
                return std::unexpected(QString("%1 still links to this store").arg(path));
            }
        }
    }
    TRY(commitLocked({ RefRecord::removeRef(key) }))
    return releaseLocked({ ref->hash });
}
