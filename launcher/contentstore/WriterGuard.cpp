#include "WriterGuard.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <utility>

#include "contentstore/ObjectFiles.h"

#if defined(Q_OS_LINUX)
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>
#include <mutex>
#elif defined(Q_OS_MACOS)
#include <libproc.h>
#include <unistd.h>
#include <vector>
#endif

namespace {
bool g_unenforced = false;
std::optional<bool> g_backupsNeedValidation;

#if defined(Q_OS_LINUX)
// Lease breaks are noticed by asking for the lease, not by a signal: the signal is one nothing uses, and ignored, as the
// default one would end the process
int leaseSignal()
{
    static std::once_flag once;
    static int signal = SIGRTMIN + 7;
    std::call_once(once, [] { std::signal(signal, SIG_IGN); });
    return signal;
}

// Whether a process other than this one has the file open, from /proc. Only processes of the same user can be seen,
// which are the ones that can write to the user's files.
bool openedByOthers(const FS::FileId& id)
{
    const auto self = QString::number(getpid());
    for (const auto& pid : QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool isProcess = false;
        pid.toInt(&isProcess);
        if (!isProcess || pid == self) {
            continue;
        }
        const auto fds = QDir("/proc/" + pid + "/fd").entryList(QDir::System | QDir::Files | QDir::NoDotAndDotDot);
        for (const auto& fd : fds) {
            // follows the descriptor to its file
            if (const auto target = FS::fileId("/proc/" + pid + "/fd/" + fd, true); target && *target == id) {
                return true;
            }
        }
    }
    return false;
}
#elif defined(Q_OS_MACOS)
bool openedByOthers(const QString& path)
{
    const auto encoded = QFile::encodeName(path);
    // room for every process, as the processes with the file open are a part of them
    const int processes = proc_listallpids(nullptr, 0);
    if (processes <= 0) {
        return false;
    }
    std::vector<pid_t> pids(static_cast<std::size_t>(processes) * 2);
    // the size of the found ids, in bytes
    const int found =
        proc_listpidspath(PROC_ALL_PIDS, 0, encoded.constData(), 0, pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    for (int i = 0; i < found / static_cast<int>(sizeof(pid_t)); i++) {
        if (pids[static_cast<std::size_t>(i)] != getpid()) {
            return true;
        }
    }
    return false;
}
#endif
}  // namespace

WriterGuard::~WriterGuard()
{
    if (!m_salvageTarget.isEmpty()) {
        // the file is gone once this ends, so what it holds now is all that can be kept
        qWarning() << "Shared store: a program still has a removed backup open; saving what it wrote so far";
        if (auto saved = finishSalvage(); !saved) {
            qWarning() << "Shared store:" << saved.error();
        }
    }
    release();
}

WriterGuard::WriterGuard(WriterGuard&& other) noexcept
    : m_tier(other.m_tier)
    , m_fileId(std::move(other.m_fileId))
    , m_pin(std::move(other.m_pin))
    , m_leaseDescriptor(std::exchange(other.m_leaseDescriptor, -1))
    , m_salvageTarget(std::exchange(other.m_salvageTarget, {}))
    , m_salvageDigest(std::exchange(other.m_salvageDigest, {}))
{
    other.m_pin.reset();
}

WriterGuard& WriterGuard::operator=(WriterGuard&& other) noexcept
{
    if (this != &other) {
        release();
        m_tier = other.m_tier;
        m_fileId = std::move(other.m_fileId);
        m_pin = std::move(other.m_pin);
        other.m_pin.reset();
        m_leaseDescriptor = std::exchange(other.m_leaseDescriptor, -1);
        m_salvageTarget = std::exchange(other.m_salvageTarget, {});
        m_salvageDigest = std::exchange(other.m_salvageDigest, {});
    }
    return *this;
}

WriterGuard::Tier WriterGuard::tierFor([[maybe_unused]] const QString& path)
{
#if defined(Q_OS_WIN)
    return Tier::Enforced;
#elif defined(Q_OS_LINUX)
    // a lease on a new file next to it tells whether the file system and the system settings allow leases at all
    auto probe = QFile::encodeName(QDir(QFileInfo(path).absolutePath()).filePath(".prism-lease-XXXXXX"));
    const int descriptor = mkstemp(probe.data());
    if (descriptor < 0) {
        return Tier::BestEffort;
    }
    close(descriptor);
    const int reader = open(probe.constData(), O_RDONLY | O_CLOEXEC);
    const bool supported = reader >= 0 && fcntl(reader, F_SETSIG, leaseSignal()) == 0 && fcntl(reader, F_SETLEASE, F_WRLCK) == 0;
    if (reader >= 0) {
        if (supported) {
            fcntl(reader, F_SETLEASE, F_UNLCK);
        }
        close(reader);
    }
    unlink(probe.constData());
    return supported ? Tier::Enforced : Tier::BestEffort;
#else
    return Tier::BestEffort;
#endif
}

bool WriterGuard::backupsNeedValidation()
{
    if (g_backupsNeedValidation) {
        return *g_backupsNeedValidation;
    }
#if defined(Q_OS_WIN)
    return false;
#else
    return true;
#endif
}

void WriterGuard::setBackupsNeedValidationForTesting(std::optional<bool> needed)
{
    g_backupsNeedValidation = needed;
}

void WriterGuard::setUnenforcedForTesting(bool unenforced)
{
    g_unenforced = unenforced;
}

Result<WriterGuard> WriterGuard::acquire(const QString& path)
{
    WriterGuard guard;
    TRY_INTO(guard.m_fileId, FS::fileId(path))
    if (g_unenforced) {
        return guard;
    }
#if defined(Q_OS_WIN)
    auto pinned = FS::pinFile(path);
    if (!pinned) {
        return std::unexpected(pinned.error());
    }
    guard.m_pin = std::move(*pinned);
    guard.m_tier = Tier::Enforced;
    return guard;
#else
#if defined(Q_OS_LINUX)
    const int descriptor = open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return std::unexpected(QString("Failed to open %1").arg(path));
    }
    if (fcntl(descriptor, F_SETSIG, leaseSignal()) == 0 && fcntl(descriptor, F_SETLEASE, F_WRLCK) == 0) {
        guard.m_leaseDescriptor = descriptor;
        guard.m_tier = Tier::Enforced;
        return guard;
    }
    const int error = errno;
    close(descriptor);
    if (error == EAGAIN) {
        // only granted while no other program has the file open
        return std::unexpected(QString("%1 is being used by another program").arg(path));
    }
    // leases aren't available here, such as on a network or FUSE file system, or for another user's file
#endif
    guard.m_tier = Tier::BestEffort;
    if (guard.disturbed(path)) {
        return std::unexpected(QString("%1 is being used by another program").arg(path));
    }
    return guard;
#endif
}

bool WriterGuard::disturbed([[maybe_unused]] const QString& path) const
{
    if (g_unenforced) {
        return false;
    }
#if defined(Q_OS_WIN)
    // pinned: no program can open the file for writing
    return false;
#elif defined(Q_OS_LINUX)
    if (m_leaseDescriptor >= 0) {
        // a program that opens the file meanwhile breaks the lease, and waits until it is released
        return fcntl(m_leaseDescriptor, F_GETLEASE) != F_WRLCK;
    }
    return openedByOthers(m_fileId);
#elif defined(Q_OS_MACOS)
    return openedByOthers(path);
#else
    return false;
#endif
}

Result<QString> WriterGuard::sha256(const QString& path) const
{
#if defined(Q_OS_LINUX)
    if (m_leaseDescriptor >= 0) {
        // through the lease's own descriptor: opening the file again would break the lease
        QCryptographicHash hash(QCryptographicHash::Sha256);
        QByteArray buffer(1024 * 1024, Qt::Uninitialized);
        off_t offset = 0;
        while (true) {
            const auto bytesRead = pread(m_leaseDescriptor, buffer.data(), static_cast<std::size_t>(buffer.size()), offset);
            if (bytesRead < 0) {
                return std::unexpected(QString("Failed to read %1").arg(path));
            }
            if (bytesRead == 0) {
                break;
            }
            hash.addData(QByteArrayView(buffer.constData(), bytesRead));
            offset += bytesRead;
        }
        return QString::fromLatin1(hash.result().toHex());
    }
#endif
    return ObjectFiles::sha256(path);
}

void WriterGuard::beginSalvage([[maybe_unused]] const QString& target, [[maybe_unused]] const QString& expectedDigest)
{
#if defined(Q_OS_LINUX)
    if (m_leaseDescriptor < 0) {
        return;
    }
    // The program counts as a writer before it waits on the lease, so a lease is only granted again once it closed
    // the file. Its write can't be lost meanwhile: this descriptor keeps the removed file.
    fcntl(m_leaseDescriptor, F_SETLEASE, F_UNLCK);
    m_salvageTarget = target;
    m_salvageDigest = expectedDigest;
#endif
}

Result<WriterGuard::Salvage> WriterGuard::trySalvage()
{
#if defined(Q_OS_LINUX)
    if (m_salvageTarget.isEmpty()) {
        return Salvage::Unchanged;
    }
    if (fcntl(m_leaseDescriptor, F_SETLEASE, F_WRLCK) != 0) {
        return Salvage::Waiting;
    }
    return finishSalvage();
#else
    return Salvage::Unchanged;
#endif
}

Result<WriterGuard::Salvage> WriterGuard::finishSalvage()
{
#if defined(Q_OS_LINUX)
    const auto target = std::exchange(m_salvageTarget, {});
    const auto expectedDigest = std::exchange(m_salvageDigest, {});
    auto digest = sha256(target);
    if (!digest) {
        release();
        return std::unexpected(digest.error());
    }
    if (*digest == expectedDigest) {
        release();
        return Salvage::Unchanged;
    }
    QDir().mkpath(QFileInfo(target).absolutePath());
    QFile file(target);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        release();
        return std::unexpected(QString("Failed to create %1").arg(target));
    }
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    off_t offset = 0;
    while (true) {
        const auto bytesRead = pread(m_leaseDescriptor, buffer.data(), static_cast<std::size_t>(buffer.size()), offset);
        if (bytesRead < 0) {
            release();
            return std::unexpected(QString("Failed to read a removed backup"));
        }
        if (bytesRead == 0) {
            break;
        }
        if (file.write(buffer.constData(), bytesRead) != bytesRead) {
            release();
            return std::unexpected(QString("Failed to write %1").arg(target));
        }
        offset += bytesRead;
    }
    file.close();
    release();
    TRY(FS::flushFile(target))
    TRY(FS::flushDir(QFileInfo(target).absolutePath()))
    return Salvage::Saved;
#else
    return Salvage::Unchanged;
#endif
}

void WriterGuard::release()
{
    m_pin.reset();
#if defined(Q_OS_LINUX)
    if (m_leaseDescriptor >= 0) {
        fcntl(m_leaseDescriptor, F_SETLEASE, F_UNLCK);
        close(m_leaseDescriptor);
        m_leaseDescriptor = -1;
    }
#endif
}
