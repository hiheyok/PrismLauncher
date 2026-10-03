#include "StoreLock.h"

#include <QFile>
#include <QSysInfo>

StoreLock::StoreLock(const QString& path) : m_path(path), m_lock(path)
{
    // only a lock whose holder died on this machine is stale, never one that is just old
    m_lock.setStaleLockTime(0);
}

StoreLock::~StoreLock()
{
    release();
}

StoreLock::Status StoreLock::tryAcquire()
{
    if (m_lock.isLocked() || m_lock.tryLock(0)) {
        return Status::Acquired;
    }
    if (m_lock.error() != QLockFile::LockFailedError) {
        return Status::Error;
    }
    const auto lockHolder = holder();
    if (lockHolder && !lockHolder->hostname.isEmpty() && lockHolder->hostname != QSysInfo::machineHostName()) {
        return Status::HeldByOtherHost;
    }
    return Status::HeldByOtherProcess;
}

void StoreLock::release()
{
    if (m_lock.isLocked()) {
        m_lock.unlock();
    }
}

std::optional<StoreLock::Holder> StoreLock::holder() const
{
    Holder result;
    if (!m_lock.getLockInfo(&result.pid, &result.hostname, &result.appname)) {
        return std::nullopt;
    }
    return result;
}

StoreLock::Status StoreLock::forceAcquire()
{
    const auto status = tryAcquire();
    if (status != Status::HeldByOtherHost) {
        return status;
    }
    if (!QFile::remove(m_path)) {
        return Status::Error;
    }
    return tryAcquire();
}
