#pragma once

#include <QFutureWatcher>
#include <QPointer>
#include <QSet>

#include <atomic>
#include <optional>

#include "BaseInstance.h"
#include "contentstore/ContentStore.h"
#include "contentstore/SharedContent.h"
#include "tasks/Task.h"

// "Share all content": shares the existing files of an instance in place, in the background, see
// SharedContent::shareInstance. Refuses an instance that is running, as the game has its files open.
class DeduplicateInstanceTask : public Task {
    Q_OBJECT
   public:
    DeduplicateInstanceTask(ContentStore* store, BaseInstance* instance, ContentStore::ConvertOptions options = {});
    // the background work uses the store, which must outlive it
    ~DeduplicateInstanceTask() override;

    const std::optional<SharedContent::ShareReport>& report() const { return m_report; }
    // the user's files that pending validations found changed, once it finished
    const QList<ContentStore::RestoredFile>& restoredFiles() const { return m_restoredFiles; }

    bool canAbort() const override { return true; }

   public slots:
    bool abort() override;

   protected:
    void executeTask() override;

   private:
    ContentStore* m_store;
    QPointer<BaseInstance> m_instance;
    ContentStore::ConvertOptions m_options;
    std::optional<SharedContent::ShareReport> m_report;
    QList<ContentStore::RestoredFile> m_restoredFiles;
    std::atomic<bool> m_aborted = false;
    QFutureWatcher<SharedContent::ShareReport> m_watcher;
};
