#pragma once

#include <QFutureWatcher>

#include <atomic>
#include <optional>

#include "contentstore/ContentStore.h"
#include "contentstore/SharedContent.h"
#include "tasks/Task.h"

// Checks the recorded links of this launcher in the background, see ContentStore::verify
class VerifyStoreTask : public Task {
    Q_OBJECT
   public:
    explicit VerifyStoreTask(ContentStore* store) : m_store(store) {}
    // the background work uses the store, which must outlive it
    ~VerifyStoreTask() override { m_watcher.waitForFinished(); }

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
    ~DeepVerifyStoreTask() override { m_watcher.waitForFinished(); }

    const std::optional<ContentStore::DeepVerifyReport>& report() const { return m_report; }

   protected:
    void executeTask() override;

   private:
    ContentStore* m_store;
    std::optional<ContentStore::DeepVerifyReport> m_report;
    QFutureWatcher<Result<ContentStore::DeepVerifyReport>> m_watcher;
};

// Moves the shared files from one store to another in the background, see SharedContent::moveShares
class MoveStoreTask : public Task {
    Q_OBJECT
   public:
    MoveStoreTask(ContentStore* from, ContentStore* to) : m_from(from), m_to(to) {}
    // the background work uses both stores, which must outlive it
    ~MoveStoreTask() override
    {
        m_aborted = true;
        m_watcher.waitForFinished();
    }

    // Not stopped halfway: the launcher then uses the new store, and links left in the old one would have nobody to look
    // after them
    const std::optional<SharedContent::MoveReport>& report() const { return m_report; }

   protected:
    void executeTask() override;

   private:
    ContentStore* m_from;
    ContentStore* m_to;
    std::optional<SharedContent::MoveReport> m_report;
    std::atomic<bool> m_aborted = false;
    QFutureWatcher<SharedContent::MoveReport> m_watcher;
};

// Scans the folders of this launcher's owners in the background, see ContentStore::reconcile
class ReconcileStoreTask : public Task {
    Q_OBJECT
   public:
    ReconcileStoreTask(ContentStore* store, ContentStore::ReconcileOptions options) : m_store(store), m_options(std::move(options)) {}
    ~ReconcileStoreTask() override { m_watcher.waitForFinished(); }

    const std::optional<ContentStore::ReconcileReport>& report() const { return m_report; }

   protected:
    void executeTask() override;

   private:
    ContentStore* m_store;
    ContentStore::ReconcileOptions m_options;
    std::optional<ContentStore::ReconcileReport> m_report;
    QFutureWatcher<Result<ContentStore::ReconcileReport>> m_watcher;
};
