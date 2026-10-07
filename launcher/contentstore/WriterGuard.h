#pragma once

#include <QString>

#include <cstdint>
#include <optional>

#include "FileSystemPrimitives.h"
#include "Result.h"

// Keeps other programs from writing to a user's file while the store replaces it, as far as the system allows.
//
// - Windows (enforced): the file is pinned, so no program can open it for writing.
// - Linux with file leases (enforced): a write lease is only granted while no other program has the file open, and any
//   program that opens it afterwards waits until the lease is released. That open is noticed, so the replacement can be
//   undone before the program gets the file.
// - Elsewhere (best effort): the programs that have the file open are looked for. A program that opens the file, writes
//   and closes it between two looks isn't noticed.
class WriterGuard {
   public:
    enum class Tier : std::uint8_t { Enforced, BestEffort };

    WriterGuard() = default;
    ~WriterGuard();
    WriterGuard(WriterGuard&& other) noexcept;
    WriterGuard& operator=(WriterGuard&& other) noexcept;
    WriterGuard(const WriterGuard&) = delete;
    WriterGuard& operator=(const WriterGuard&) = delete;

    // Guards the file at path. Fails if another program has it open, and the file must be left alone.
    static Result<WriterGuard> acquire(const QString& path);

    // The tier the system offers for files like the one at path, without guarding it
    static Tier tierFor(const QString& path);

    Tier tier() const { return m_tier; }

    // Whether another program opened the file since it was guarded, or has it open now. path is where the file is
    // now, which changes when it is renamed aside.
    bool disturbed(const QString& path) const;

    // The SHA-256 of the guarded file. Reads it without opening it again, which would disturb a lease.
    Result<QString> sha256(const QString& path) const;

    // Ends the guard; anything waiting to open the file goes ahead
    void release();

    // Whether no program can open the file for writing at all while it is guarded, as with a Windows pin. Only then can
    // the guarded file be removed at once; a program can still reach a file under a Linux lease as it is removed.
    bool keepsEveryWriterOut() const { return m_pin.has_value(); }

    enum class Salvage : std::uint8_t { Waiting, Unchanged, Saved };
    // For a file removed while guarded, after disturbed() found a program that opened it as it was removed: lets that
    // program go ahead, while this guard keeps the removed file, and saves its contents at target once the program
    // closed it, if they no longer hash to expectedDigest. Only a Linux lease keeps hold of a removed file; elsewhere
    // nothing is saved.
    void beginSalvage(const QString& target, const QString& expectedDigest);
    // Whether the program closed the file yet, and what was done then. While it is Waiting, the guard must be kept, as
    // the file is gone once it ends. A guard that ends while waiting saves what the file holds by then.
    Result<Salvage> trySalvage();

    // the descriptor holding a Linux lease, or -1, so tests can open the file the way another program would
    int descriptorForTesting() const { return m_leaseDescriptor; }

    // Whether the backup of a replaced file must wait for a later validation instead of being removed right away.
    // Only a Windows pin keeps every program away until the backup is gone. A Linux lease can't: a program that opens
    // the file between the last look at the lease and its release gets the backup, and would lose its write if the
    // backup were removed. The answer is journaled with each backup, so it also holds after a restart.
    static bool backupsNeedValidation();
    // Overrides backupsNeedValidation for tests; nullopt restores it
    static void setBackupsNeedValidationForTesting(std::optional<bool> needed);

    // With true, guards neither keep programs away nor notice them, like a system where a program writes between two
    // looks. Tests use it to check that such writes are still found.
    static void setUnenforcedForTesting(bool unenforced);

   private:
    Tier m_tier = Tier::BestEffort;
    FS::FileId m_fileId;
    std::optional<FS::PinnedFile> m_pin;
    // the descriptor holding a Linux write lease
    int m_leaseDescriptor = -1;
    // set while a removed file is being salvaged
    QString m_salvageTarget;
    QString m_salvageDigest;

    Result<Salvage> finishSalvage();
};
