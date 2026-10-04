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
