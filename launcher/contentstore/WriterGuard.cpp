#include "WriterGuard.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <utility>

#include "contentstore/ObjectFiles.h"

#if defined(Q_OS_LINUX)
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>
#include <ctime>
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
// The helper process that saves a removed file. It is forked from a process with other threads, so it may only make
// async-signal-safe calls: no Qt, no allocation, no locks.
void sleepMs(long ms)
{
    timespec time{ ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&time, nullptr);
}

bool writeAll(int descriptor, const char* data, ssize_t size)
{
    while (size > 0) {
        const auto written = write(descriptor, data, static_cast<size_t>(size));
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data += written;
        size -= written;
    }
    return true;
}

// Copies the file to partial, then puts it at target, both durably
bool saveCopy(int source, const char* partial, const char* target, const char* dir)
{
    const int copy = open(partial, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (copy < 0) {
        return false;
    }
    static char buffer[64 * 1024];
    off_t offset = 0;
    bool copied = true;
    while (true) {
        const auto bytesRead = pread(source, buffer, sizeof buffer, offset);
        if (bytesRead < 0 && errno == EINTR) {
            continue;
        }
        if (bytesRead <= 0) {
            copied = bytesRead == 0;
            break;
        }
        if (!writeAll(copy, buffer, bytesRead)) {
            copied = false;
            break;
        }
        offset += bytesRead;
    }
    copied = copied && fsync(copy) == 0;
    close(copy);
    if (!copied || rename(partial, target) != 0) {
        unlink(partial);
        return false;
    }
    const int folder = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (folder < 0) {
        return false;
    }
    const bool synced = fsync(folder) == 0;
    close(folder);
    return synced;
}

[[noreturn]] void runSalvage(int source, int lock, int maxDescriptor, const char* partial, const char* target, const char* dir)
{
    // It may outlive the launcher, so it keeps nothing of it open but the removed file and its own lock, such as the
    // store's lock
    for (int descriptor = 3; descriptor < maxDescriptor; descriptor++) {
        if (descriptor != source && descriptor != lock) {
            close(descriptor);
        }
    }
    // Waits for the program for as long as it keeps the file open. Saving is tried for a day at most, so a folder that
    // never becomes writable doesn't keep it forever.
    for (int failures = 0; failures < 24 * 60 * 60;) {
        // A lease is only granted while no other program has the file open: the program closed it. Held while copying,
        // so the copy is consistent.
        if (fcntl(source, F_SETLEASE, F_WRLCK) != 0) {
            sleepMs(200);
            continue;
        }
        const bool saved = saveCopy(source, partial, target, dir);
        fcntl(source, F_SETLEASE, F_UNLCK);
        if (saved) {
            _exit(0);
        }
        failures++;
        sleepMs(1000);
    }
    _exit(1);
}

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
        // the removed file is gone once this ends, so this is the last chance to hand it to a helper
        if (auto handed = salvage(m_salvageTarget); !handed) {
            qWarning() << "Shared store: what a program wrote to a removed backup is lost:" << handed.error();
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

Result<> WriterGuard::salvage([[maybe_unused]] const QString& target)
{
#if defined(Q_OS_LINUX)
    if (m_leaseDescriptor < 0) {
        return {};
    }
    m_salvageTarget = target;
    const auto dir = QFileInfo(target).absolutePath();
    QDir().mkpath(dir);
    // everything the helper needs, prepared before it is forked
    const auto targetPath = QFile::encodeName(target);
    const auto partialPath = targetPath + ".part";
    const auto lockPath = targetPath + ".lock";
    const auto dirPath = QFile::encodeName(dir);
    // locked for as long as the helper runs: it inherits the lock, which ends when it exits
    const int lock = open(lockPath.constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lock < 0) {
        return std::unexpected(QString("Failed to create %1").arg(QString::fromLocal8Bit(lockPath)));
    }
    if (flock(lock, LOCK_EX | LOCK_NB) != 0) {
        close(lock);
        return std::unexpected(QString("Another salvage to %1 still runs").arg(target));
    }
    const long openMax = sysconf(_SC_OPEN_MAX);
    const int maxDescriptor = openMax > 0 && openMax < 1 << 20 ? static_cast<int>(openMax) : 1 << 20;
    const pid_t child = fork();
    if (child < 0) {
        close(lock);
        return std::unexpected(QString("Failed to start the helper that saves %1").arg(target));
    }
    if (child == 0) {
        // the helper: forked once more and detached, so it is neither a child of the launcher nor ends with it
        const pid_t helper = fork();
        if (helper != 0) {
            _exit(helper > 0 ? 0 : 1);
        }
        setsid();
        runSalvage(m_leaseDescriptor, lock, maxDescriptor, partialPath.constData(), targetPath.constData(), dirPath.constData());
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    close(lock);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::unexpected(QString("Failed to start the helper that saves %1").arg(target));
    }
    // The helper keeps the removed file now. Only then may the program that opened it go ahead.
    m_salvageTarget.clear();
    release();
    return {};
#else
    return {};
#endif
}

bool WriterGuard::salvageRunning([[maybe_unused]] const QString& target)
{
#if defined(Q_OS_LINUX)
    const int lock = open(QFile::encodeName(target + ".lock").constData(), O_RDWR | O_CLOEXEC);
    if (lock < 0) {
        return false;
    }
    const bool running = flock(lock, LOCK_EX | LOCK_NB) != 0;
    close(lock);
    return running;
#else
    return false;
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
