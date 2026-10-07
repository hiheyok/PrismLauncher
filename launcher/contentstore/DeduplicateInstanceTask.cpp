#include "DeduplicateInstanceTask.h"

#include <QDir>
#include <QtConcurrentRun>

DeduplicateInstanceTask::DeduplicateInstanceTask(ContentStore* store, BaseInstance* instance, ContentStore::ConvertOptions options)
    : m_store(store), m_instance(instance), m_options(options)
{}

DeduplicateInstanceTask::~DeduplicateInstanceTask()
{
    m_aborted = true;
    m_watcher.waitForFinished();
}

bool DeduplicateInstanceTask::abort()
{
    // the file being shared is finished first, so no file is left half shared
    m_aborted = true;
    return true;
}

void DeduplicateInstanceTask::executeTask()
{
    if (!m_instance) {
        emitFailed(tr("The instance is gone."));
        return;
    }
    if (m_instance->isRunning()) {
        emitFailed(tr("Close the instance before sharing its content: the game has its files open."));
        return;
    }
    if (!m_store->isWritable()) {
        emitFailed(tr("The shared store can't be changed."));
        return;
    }
    setStatus(tr("Sharing the content of %1").arg(m_instance->name()));

    // which files the user keeps local, read here, as the instance's settings belong to this thread
    const auto gameRoot = m_instance->gameRoot();
    QSet<QString> excluded;
    for (const auto& path : SharedContent::shareableFiles(gameRoot)) {
        const auto relativePath = QDir(gameRoot).relativeFilePath(path);
        if (SharedContent::isExcluded(m_instance.get(), relativePath)) {
            excluded.insert(SharedContent::exclusionKey(relativePath));
        }
    }

    connect(&m_watcher, &QFutureWatcher<SharedContent::ShareReport>::finished, this, [this] {
        m_report = m_watcher.result();
        // the backups of the replaced files are validated now that nothing else runs; those still in use are later
        if (auto left = m_store->validatePendingBackups(); !left) {
            qWarning() << "Shared store:" << left.error();
        }
        m_restoredFiles = m_store->takeRestoredFiles();
        if (m_report->stopped) {
            emitAborted();
            return;
        }
        emitSucceeded();
    });
    m_watcher.setFuture(QtConcurrent::run([this, id = m_instance->id(), gameRoot, excluded] {
        return SharedContent::shareInstance(
            *m_store, id, gameRoot, m_options,
            [excluded](const QString& relativePath) { return excluded.contains(SharedContent::exclusionKey(relativePath)); },
            [this](int done, int total) {
                QMetaObject::invokeMethod(this, [this, done, total] { setProgress(done, total); }, Qt::QueuedConnection);
                return !m_aborted;
            });
    }));
}
