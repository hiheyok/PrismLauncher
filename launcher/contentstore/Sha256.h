#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <cstddef>
#include <cstdint>

// SHA-256, with the processor's SHA instructions where it has them: SHA-NI on x86-64, the SHA-2 extension on ARM64.
// Several times faster than QCryptographicHash there, which matters as the shared store hashes every file it stores,
// converts and verifies. Elsewhere it is a portable implementation.
class Sha256 {
   public:
    enum class Implementation : std::uint8_t { Portable, X86, Arm };

    Sha256();

    void addData(QByteArrayView data);
    // the 32 bytes of the hash; nothing may be added afterwards
    QByteArray result();
    // the hash as lowercase hex, as the store names its files
    QString hexResult() { return QString::fromLatin1(result().toHex()); }

    // what this processor uses
    static Implementation implementation();
    // Uses the given implementation instead, for tests that compare them; ignored where it isn't available. Applies to
    // hashes started afterwards.
    static void setImplementationForTesting(Implementation implementation);
    static bool isAvailable(Implementation implementation);

   private:
    using ProcessBlocks = void (*)(std::uint32_t state[8], const std::uint8_t* data, std::size_t blocks);

    ProcessBlocks m_process;
    std::uint32_t m_state[8];
    std::uint64_t m_length = 0;
    std::uint8_t m_buffer[64];
    std::size_t m_buffered = 0;
};
