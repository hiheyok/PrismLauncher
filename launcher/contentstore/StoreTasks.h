#pragma once

#include <QFutureWatcher>

#include <optional>

#include "contentstore/ContentStore.h"
#include "tasks/Task.h"

// Checks the recorded links of this launcher in the background, see ContentStore::verify
class VerifyStoreTask : public Task {
    Q_OBJECT
   public:
    explicit VerifyStoreTask(ContentStore* store) : m_store(store) {}

    const std::optional<ContentStore::VerifyReport>& report() const { return m_report; }

   protected:
    void executeTask() override;

   private:
    ContentStore* m_store;
    std::optional<ContentStore::VerifyReport> m_report;
    QFutureWatcher<Result<ContentStore::VerifyReport>> m_watcher;
};

// Hashes every stored file in the background, see ContentStore::deepVerify
class DeepVerifyStoreTask : public Task {
    Q_OBJECT
   public:
    explicit DeepVerifyStoreTask(ContentStore* store) : m_store(store) {}

    const std::optional<ContentStore::DeepVerifyReport>& report() const { return m_report; }

   protected:
    void executeTask() override;

   private:
    ContentStore* m_store;
    std::optional<ContentStore::DeepVerifyReport> m_report;
    QFutureWatcher<Result<ContentStore::DeepVerifyReport>> m_watcher;
};

// Scans the folders of this launcher's owners in the background, see ContentStore::reconcile
class ReconcileStoreTask : public Task {
    Q_OBJECT
   public:
    ReconcileStoreTask(ContentStore* store, ContentStore::ReconcileOptions options) : m_store(store), m_options(std::move(options)) {}

    const std::optional<ContentStore::ReconcileReport>& report() const { return m_report; }

   protected:
    void executeTask() override;

   private:
    ContentStore* m_store;
    ContentStore::ReconcileOptions m_options;
    std::optional<ContentStore::ReconcileReport> m_report;
    QFutureWatcher<Result<ContentStore::ReconcileReport>> m_watcher;
};
