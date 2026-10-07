#pragma once

#include <QFutureWatcher>
#include <QList>
#include <QStringList>

#include <functional>

#include "Result.h"
#include "tasks/Task.h"

// Runs the file work of a sharing action on the files of an instance, such as "Keep local copy", in the background, as
// it copies and hashes files. Each job runs in the background and may return what to do on the UI thread afterwards,
// such as changing the instance's settings, which belong to that thread.
class SharingActionTask : public Task {
    Q_OBJECT
   public:
    using FollowUp = std::function<void()>;
    using Job = std::function<Result<FollowUp>()>;

    SharingActionTask(QString status, QList<Job> jobs) : m_status(std::move(status)), m_jobs(std::move(jobs)) {}
    // the jobs use the store, which must outlive them
    ~SharingActionTask() override { m_watcher.waitForFinished(); }

    // what went wrong, per file, once it finished
    const QStringList& errors() const { return m_errors; }

   protected:
    void executeTask() override;

   private:
    QString m_status;
    QList<Job> m_jobs;
    QStringList m_errors;
    QFutureWatcher<QList<Result<FollowUp>>> m_watcher;
};
