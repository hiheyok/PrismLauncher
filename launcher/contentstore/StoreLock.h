#pragma once

#include <QLockFile>
#include <QString>

#include <cstdint>
#include <optional>

// Exclusive ownership of a content store by one launcher process, held for the lifetime of the process.
// The lock only becomes stale when its holder on this machine has died: with the default age-based staleness, a lock
// held for longer than 30 seconds could be stolen on file systems without native locking.
class StoreLock {
   public:
    enum class Status : std::uint8_t {
        Acquired,
        // another process on this machine holds the store
        HeldByOtherProcess,
        // a process on another machine holds the store, so it can't be checked and is never broken automatically
        HeldByOtherHost,
        Error,
    };

    struct Holder {
        qint64 pid = 0;
        QString hostname;
        QString appname;
    };

    explicit StoreLock(const QString& path);
    ~StoreLock();
    StoreLock(const StoreLock&) = delete;
    StoreLock& operator=(const StoreLock&) = delete;

    Status tryAcquire();
    bool isHeld() const { return m_lock.isLocked(); }
    void release();

    // Who holds the lock, if anyone
    std::optional<Holder> holder() const;

    // Removes a lock held by another machine and acquires it. Only for when the user confirmed that machine no longer
    // uses the store: if it does, both would change the store at the same time.
    Status forceAcquire();

    QString path() const { return m_path; }

   private:
    QString m_path;
    QLockFile m_lock;
};
