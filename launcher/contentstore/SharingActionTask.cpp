#include "SharingActionTask.h"

#include <QtConcurrentRun>

void SharingActionTask::executeTask()
{
    setStatus(m_status);
    connect(&m_watcher, &QFutureWatcher<QList<Result<FollowUp>>>::finished, this, [this] {
        for (const auto& result : m_watcher.result()) {
            if (!result) {
                m_errors.append(result.error());
            } else if (*result) {
                (*result)();
            }
        }
        emitSucceeded();
    });
    m_watcher.setFuture(QtConcurrent::run([this] {
        QList<Result<FollowUp>> results;
        for (int i = 0; i < m_jobs.size(); i++) {
            QMetaObject::invokeMethod(this, [this, i] { setProgress(i, m_jobs.size()); }, Qt::QueuedConnection);
            results.append(m_jobs[i]());
        }
        return results;
    }));
}
