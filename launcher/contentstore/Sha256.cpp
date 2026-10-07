#include "Sha256.h"

#include <atomic>
#include <cstring>
#include <optional>

#if defined(__x86_64__) || defined(_M_X64)
#define SHA256_X86 1
#include <immintrin.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
// MSVC compiles the intrinsics without a target attribute
#define SHA256_X86_TARGET
#else
#include <cpuid.h>
#define SHA256_X86_TARGET __attribute__((target("sha,sse4.1,ssse3")))
#endif
#endif

// ARM64 builds that may assume the SHA-2 extension, such as every Apple Silicon build
#if (defined(__aarch64__) || defined(_M_ARM64)) && defined(__ARM_FEATURE_SHA2)
#define SHA256_ARM 1
#include <arm_neon.h>
#endif

namespace {
constexpr std::uint32_t g_roundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr std::uint32_t g_initialState[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

constexpr std::uint32_t rotateRight(std::uint32_t value, int bits)
{
    return (value >> bits) | (value << (32 - bits));
}

void processPortable(std::uint32_t state[8], const std::uint8_t* data, std::size_t blocks)
{
    for (; blocks > 0; blocks--, data += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = (std::uint32_t(data[i * 4]) << 24) | (std::uint32_t(data[i * 4 + 1]) << 16) | (std::uint32_t(data[i * 4 + 2]) << 8) |
                   std::uint32_t(data[i * 4 + 3]);
        }
        for (int i = 16; i < 64; i++) {
            const auto s0 = rotateRight(w[i - 15], 7) ^ rotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const auto s1 = rotateRight(w[i - 2], 17) ^ rotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; i++) {
            const auto s1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
            const auto choose = (e & f) ^ (~e & g);
            const auto t1 = h + s1 + choose + g_roundConstants[i] + w[i];
            const auto s0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto t2 = s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }
}

#if SHA256_X86
bool x86HasShaExtensions()
{
#if defined(_MSC_VER) && !defined(__clang__)
    int info[4];
    __cpuid(info, 0);
    if (info[0] < 7) {
        return false;
    }
    __cpuid(info, 1);
    const bool sse = (info[2] & (1 << 19)) && (info[2] & (1 << 9));
    __cpuidex(info, 7, 0);
    return sse && (info[1] & (1 << 29));
#else
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid_max(0, nullptr) < 7 || !__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        return false;
    }
    // SSE4.1 and SSSE3
    const bool sse = (ecx & (1u << 19)) && (ecx & (1u << 9));
    __cpuid_count(7, 0, eax, ebx, ecx, edx);
    return sse && (ebx & (1u << 29));
#endif
}

// Four rounds at a time: the state is kept as ABEF and CDGH, as the SHA-NI round instruction wants it, and the message
// schedule as four vectors of four words, each computed from the ones before
SHA256_X86_TARGET void processX86(std::uint32_t state[8], const std::uint8_t* data, std::size_t blocks)
{
    const __m128i byteSwap = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
    __m128i tmp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&state[0]));
    __m128i state1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&state[4]));
    tmp = _mm_shuffle_epi32(tmp, 0xB1);                // CDAB
    state1 = _mm_shuffle_epi32(state1, 0x1B);          // EFGH
    __m128i state0 = _mm_alignr_epi8(tmp, state1, 8);  // ABEF
    state1 = _mm_blend_epi16(state1, tmp, 0xF0);       // CDGH

    for (; blocks > 0; blocks--, data += 64) {
        const __m128i abefSave = state0;
        const __m128i cdghSave = state1;
        __m128i w[4];
        for (int i = 0; i < 4; i++) {
            w[i] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i * 16)), byteSwap);
        }
        for (int group = 0; group < 16; group++) {
            __m128i& current = w[group % 4];
            __m128i message = _mm_add_epi32(current, _mm_loadu_si128(reinterpret_cast<const __m128i*>(&g_roundConstants[group * 4])));
            state1 = _mm_sha256rnds2_epu32(state1, state0, message);
            if (group >= 3 && group <= 14) {
                // the next four words, from the two groups before and the partial schedule
                __m128i& next = w[(group + 1) % 4];
                next = _mm_add_epi32(next, _mm_alignr_epi8(current, w[(group + 3) % 4], 4));
                next = _mm_sha256msg2_epu32(next, current);
            }
            message = _mm_shuffle_epi32(message, 0x0E);
            state0 = _mm_sha256rnds2_epu32(state0, state1, message);
            if (group >= 1 && group <= 12) {
                __m128i& previous = w[(group + 3) % 4];
                previous = _mm_sha256msg1_epu32(previous, current);
            }
        }
        state0 = _mm_add_epi32(state0, abefSave);
        state1 = _mm_add_epi32(state1, cdghSave);
    }

    tmp = _mm_shuffle_epi32(state0, 0x1B);        // FEBA
    state1 = _mm_shuffle_epi32(state1, 0xB1);     // DCHG
    state0 = _mm_blend_epi16(tmp, state1, 0xF0);  // DCBA
    state1 = _mm_alignr_epi8(state1, tmp, 8);     // ABEF
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&state[0]), state0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&state[4]), state1);
}
#endif

#if SHA256_ARM
void processArm(std::uint32_t state[8], const std::uint8_t* data, std::size_t blocks)
{
    uint32x4_t state0 = vld1q_u32(&state[0]);
    uint32x4_t state1 = vld1q_u32(&state[4]);
    for (; blocks > 0; blocks--, data += 64) {
        const uint32x4_t abcdSave = state0;
        const uint32x4_t efghSave = state1;
        uint32x4_t w[4];
        for (int i = 0; i < 4; i++) {
            w[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + i * 16)));
        }
        uint32x4_t message = vaddq_u32(w[0], vld1q_u32(&g_roundConstants[0]));
        for (int group = 0; group < 16; group++) {
            uint32x4_t& current = w[group % 4];
            if (group <= 11) {
                current = vsha256su0q_u32(current, w[(group + 1) % 4]);
            }
            const uint32x4_t abcd = state0;
            uint32x4_t nextMessage = message;
            if (group <= 14) {
                nextMessage = vaddq_u32(w[(group + 1) % 4], vld1q_u32(&g_roundConstants[(group + 1) * 4]));
            }
            state0 = vsha256hq_u32(state0, state1, message);
            state1 = vsha256h2q_u32(state1, abcd, message);
            if (group <= 11) {
                current = vsha256su1q_u32(current, w[(group + 2) % 4], w[(group + 3) % 4]);
            }
            message = nextMessage;
        }
        state0 = vaddq_u32(state0, abcdSave);
        state1 = vaddq_u32(state1, efghSave);
    }
    vst1q_u32(&state[0], state0);
    vst1q_u32(&state[4], state1);
}
#endif

std::atomic<int> g_forced = -1;

Sha256::Implementation detected()
{
#if SHA256_ARM
    return Sha256::Implementation::Arm;
#elif SHA256_X86
    static const bool hasSha = x86HasShaExtensions();
    return hasSha ? Sha256::Implementation::X86 : Sha256::Implementation::Portable;
#else
    return Sha256::Implementation::Portable;
#endif
}
}  // namespace

bool Sha256::isAvailable(Implementation implementation)
{
    return implementation == Implementation::Portable || implementation == detected();
}

Sha256::Implementation Sha256::implementation()
{
    if (const int forced = g_forced; forced >= 0 && isAvailable(static_cast<Implementation>(forced))) {
        return static_cast<Implementation>(forced);
    }
    return detected();
}

void Sha256::setImplementationForTesting(Implementation implementation)
{
    g_forced = static_cast<int>(implementation);
}

Sha256::Sha256() : m_process(processPortable)
{
    switch (implementation()) {
#if SHA256_X86
        case Implementation::X86:
            m_process = processX86;
            break;
#endif
#if SHA256_ARM
        case Implementation::Arm:
            m_process = processArm;
            break;
#endif
        default:
            break;
    }
    std::memcpy(m_state, g_initialState, sizeof m_state);
}

void Sha256::addData(QByteArrayView data)
{
    auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data());
    auto size = static_cast<std::size_t>(data.size());
    m_length += size;
    if (m_buffered > 0) {
        const auto taken = std::min(size, sizeof m_buffer - m_buffered);
        std::memcpy(m_buffer + m_buffered, bytes, taken);
        m_buffered += taken;
        bytes += taken;
        size -= taken;
        if (m_buffered < sizeof m_buffer) {
            return;
        }
        m_process(m_state, m_buffer, 1);
        m_buffered = 0;
    }
    if (const auto blocks = size / 64; blocks > 0) {
        m_process(m_state, bytes, blocks);
        bytes += blocks * 64;
        size -= blocks * 64;
    }
    std::memcpy(m_buffer, bytes, size);
    m_buffered = size;
}

QByteArray Sha256::result()
{
    // padding: a 1 bit, zeros, and the length in bits, to a multiple of 64 bytes
    const std::uint64_t bits = m_length * 8;
    std::uint8_t padding[72] = { 0x80 };
    const std::size_t zeros = (m_buffered < 56 ? 56 : 120) - m_buffered;
    for (int i = 0; i < 8; i++) {
        padding[zeros + i] = static_cast<std::uint8_t>(bits >> (56 - i * 8));
    }
    addData(QByteArrayView(reinterpret_cast<const char*>(padding), static_cast<qsizetype>(zeros + 8)));

    QByteArray digest(32, Qt::Uninitialized);
    for (int i = 0; i < 8; i++) {
        digest[i * 4] = static_cast<char>(m_state[i] >> 24);
        digest[i * 4 + 1] = static_cast<char>(m_state[i] >> 16);
        digest[i * 4 + 2] = static_cast<char>(m_state[i] >> 8);
        digest[i * 4 + 3] = static_cast<char>(m_state[i]);
    }
    return digest;
}
