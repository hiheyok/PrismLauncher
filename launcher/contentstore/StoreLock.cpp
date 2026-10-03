#include "StoreLock.h"

#include <QFile>
#include <QSysInfo>

namespace {
// Host names can differ in case and in whether the domain is included, for example between the NetBIOS name and
// the DNS name of the same Windows machine
QString normalizedHostName(const QString& hostname)
{
    return hostname.trimmed().section('.', 0, 0).toLower();
}
}  // namespace

bool StoreLock::isThisMachine(const QString& hostname)
{
    return normalizedHostName(hostname) == normalizedHostName(QSysInfo::machineHostName());
}

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
    if (lockHolder && !lockHolder->hostname.isEmpty() && !isThisMachine(lockHolder->hostname)) {
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
