// Copyright (c) 2026 The AQCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AQMINER_AQ_HASH_H
#define AQMINER_AQ_HASH_H

#include <crypto/sha256.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>

namespace aqminer {

/** AQ value: sum of the 64 hex digits of a 32-byte hash (byte order irrelevant). */
inline uint32_t AQ(const unsigned char* h)
{
    uint32_t sum{0};
    for (int i = 0; i < 32; ++i) sum += (h[i] >> 4) + (h[i] & 0x0F);
    return sum;
}

using Header80 = std::array<unsigned char, 80>;

inline void WriteLE32(unsigned char* p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

/** Double SHA-256 of an 80-byte header, given a hasher that already absorbed the first 64 bytes. */
inline void HashTail(const CSHA256& midstate, const unsigned char* tail16, unsigned char* out32)
{
    unsigned char first[32];
    CSHA256 h{midstate};
    h.Write(tail16, 16).Finalize(first);
    CSHA256().Write(first, 32).Finalize(out32);
}

/**
 * Search nonces [nonce_begin, nonce_end) (stepping by `step`) for a header
 * whose double-SHA256 has AQ >= threshold. Increments `hashes` periodically.
 * Stops early when `stop` becomes true. Returns the winning nonce.
 */
inline std::optional<uint32_t> Grind(const Header80& header, uint32_t threshold, uint64_t nonce_begin, uint64_t nonce_end,
                                     uint64_t step, const std::atomic<bool>& stop, std::atomic<uint64_t>& hashes)
{
    CSHA256 mid;
    mid.Write(header.data(), 64);
    unsigned char tail[16];
    std::memcpy(tail, header.data() + 64, 16);
    unsigned char out[32];
    uint64_t local{0};
    for (uint64_t n = nonce_begin; n < nonce_end; n += step) {
        WriteLE32(tail + 12, static_cast<uint32_t>(n));
        HashTail(mid, tail, out);
        if (AQ(out) >= threshold) {
            hashes += local + 1;
            return static_cast<uint32_t>(n);
        }
        if (++local == 65536) {
            hashes += local;
            local = 0;
            if (stop.load(std::memory_order_relaxed)) return std::nullopt;
        }
    }
    hashes += local;
    return std::nullopt;
}

} // namespace aqminer

#endif // AQMINER_AQ_HASH_H
