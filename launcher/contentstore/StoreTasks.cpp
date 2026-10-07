#include "StoreTasks.h"

#include <QtConcurrentRun>

void VerifyStoreTask::executeTask()
{
    setStatus(tr("Checking shared files"));
    connect(&m_watcher, &QFutureWatcher<Result<ContentStore::VerifyReport>>::finished, this, [this] {
        const auto result = m_watcher.result();
        if (!result) {
            emitFailed(result.error());
            return;
        }
        m_report = *result;
        emitSucceeded();
    });
    m_watcher.setFuture(QtConcurrent::run([store = m_store] { return store->verify(); }));
}

void DeepVerifyStoreTask::executeTask()
{
    setStatus(tr("Checking the contents of shared files"));
    connect(&m_watcher, &QFutureWatcher<Result<ContentStore::DeepVerifyReport>>::finished, this, [this] {
        const auto result = m_watcher.result();
        if (!result) {
            emitFailed(result.error());
            return;
        }
        m_report = *result;
        emitSucceeded();
    });
    m_watcher.setFuture(QtConcurrent::run([store = m_store] { return store->deepVerify(); }));
}

void ReconcileStoreTask::executeTask()
{
    setStatus(tr("Looking for shared files"));
    connect(&m_watcher, &QFutureWatcher<Result<ContentStore::ReconcileReport>>::finished, this, [this] {
        const auto result = m_watcher.result();
        if (!result) {
            emitFailed(result.error());
            return;
        }
        m_report = *result;
        emitSucceeded();
    });
    m_watcher.setFuture(QtConcurrent::run([store = m_store, options = m_options] { return store->reconcile(options); }));
}

void MoveStoreTask::executeTask()
{
    setStatus(tr("Moving the shared files"));
    connect(&m_watcher, &QFutureWatcher<SharedContent::MoveReport>::finished, this, [this] {
        m_report = m_watcher.result();
        if (m_report->stopped) {
            emitAborted();
            return;
        }
        emitSucceeded();
    });
    m_watcher.setFuture(QtConcurrent::run([this] {
        return SharedContent::moveShares(*m_from, *m_to, [this](int done, int total) {
            QMetaObject::invokeMethod(this, [this, done, total] { setProgress(done, total); }, Qt::QueuedConnection);
            return !m_aborted;
        });
    }));
}

void StopSharingTask::executeTask()
{
    setStatus(tr("Making shared files local"));
    connect(&m_watcher, &QFutureWatcher<SharedContent::StopReport>::finished, this, [this] {
        m_report = m_watcher.result();
        if (m_report->stopped) {
            emitAborted();
            return;
        }
        emitSucceeded();
    });
    m_watcher.setFuture(QtConcurrent::run([this] {
        return SharedContent::stopSharing(*m_store, m_includes, [this](int done, int total) {
            QMetaObject::invokeMethod(this, [this, done, total] { setProgress(done, total); }, Qt::QueuedConnection);
            return !m_aborted;
        });
    }));
}
