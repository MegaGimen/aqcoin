// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Copyright (c) 2026 The AQCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow.h>

#include <arith_uint256.h>
#include <chain.h>
#include <pow_aq_table.h>
#include <primitives/block.h>
#include <uint256.h>
#include <util/check.h>

#include <algorithm>
#include <array>
#include <vector>

uint32_t AQValue(const uint256& hash)
{
    uint32_t sum{0};
    for (const unsigned char b : hash) {
        sum += (b >> 4) + (b & 0x0F);
    }
    return sum;
}

arith_uint256 AQWork(uint32_t threshold)
{
    if (threshold > AQ_MAX_THRESHOLD) return arith_uint256{0};
    return UintToArith256(AQ_WORK_TABLE[threshold]);
}

namespace {

/** Full 512-bit product of two 256-bit numbers, as 16 little-endian 32-bit limbs. */
std::array<uint32_t, 16> MulFull(const arith_uint256& a, const arith_uint256& b)
{
    std::array<uint32_t, 8> la, lb;
    for (int i = 0; i < 8; ++i) {
        la[i] = static_cast<uint32_t>((a >> (32 * i)).GetLow64());
        lb[i] = static_cast<uint32_t>((b >> (32 * i)).GetLow64());
    }
    std::array<uint32_t, 16> r{};
    for (int i = 0; i < 8; ++i) {
        uint64_t carry{0};
        for (int j = 0; j < 8; ++j) {
            const uint64_t cur{uint64_t{la[i]} * lb[j] + r[i + j] + carry};
            r[i + j] = static_cast<uint32_t>(cur);
            carry = cur >> 32;
        }
        for (int k = i + 8; carry && k < 16; ++k) {
            const uint64_t cur{uint64_t{r[k]} + carry};
            r[k] = static_cast<uint32_t>(cur);
            carry = cur >> 32;
        }
    }
    return r;
}

/** Compare two 512-bit limb arrays: returns <0, 0, >0. */
int Cmp512(const std::array<uint32_t, 16>& a, const std::array<uint32_t, 16>& b)
{
    for (int i = 15; i >= 0; --i) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

} // namespace

uint32_t AQThresholdForWork(const arith_uint256& target_work)
{
    // Largest x with work(x) <= target_work (work is monotonically non-decreasing).
    if (target_work < AQWork(0)) return 0;
    uint32_t lo{0}, hi{AQ_MAX_THRESHOLD};
    while (lo < hi) {
        const uint32_t mid{lo + (hi - lo + 1) / 2};
        if (AQWork(mid) <= target_work) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    const uint32_t x{lo};
    if (x == AQ_MAX_THRESHOLD) return x;
    // Now work(x) <= target_work < work(x+1). Pick the nearest in log space:
    // target/work(x) <= work(x+1)/target  <=>  target^2 <= work(x) * work(x+1).
    const arith_uint256 w0{AQWork(x)}, w1{AQWork(x + 1)};
    return Cmp512(MulFull(target_work, target_work), MulFull(w0, w1)) <= 0 ? x : x + 1;
}

bool AQThresholdInRange(unsigned int nBits, const Consensus::Params& params)
{
    return nBits >= params.nAQMinThreshold && nBits <= AQ_MAX_THRESHOLD;
}

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader* pblock, const Consensus::Params& params)
{
    assert(pindexLast != nullptr);
    if (params.fPowNoRetargeting) return pindexLast->nBits;
    // Not enough history for a full window (N blocks plus the timestamp of the one before).
    if (pindexLast->nHeight < params.nLWMAWindow) return params.nAQInitThreshold;
    return CalculateNextWorkRequired(pindexLast, params);
}

unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, const Consensus::Params& params)
{
    if (params.fPowNoRetargeting) return pindexLast->nBits;

    const int64_t N{params.nLWMAWindow};
    const int64_t T{params.nPowTargetSpacing};
    assert(N > 0 && T > 0);
    assert(pindexLast->nHeight >= N);

    // Blocks j = 1..N (oldest to newest) are at heights h-N+1..h; t_0 belongs to height h-N.
    std::vector<const CBlockIndex*> blocks(N + 1);
    const CBlockIndex* pindex{pindexLast};
    for (int64_t j = N; j >= 0; --j) {
        assert(pindex);
        blocks[j] = pindex;
        pindex = pindex->pprev;
    }

    int64_t weighted_time{0};
    arith_uint256 work_quot{0}; // sum of floor(work_j / N)
    arith_uint256 work_rem{0};  // sum of work_j mod N
    const arith_uint256 bnN{static_cast<uint64_t>(N)};
    for (int64_t j = 1; j <= N; ++j) {
        int64_t st{blocks[j]->GetBlockTime() - blocks[j - 1]->GetBlockTime()};
        st = std::clamp<int64_t>(st, 1, 6 * T);
        weighted_time += j * st;

        const arith_uint256 w{AQWork(blocks[j]->nBits)};
        const arith_uint256 q{w / bnN};
        work_quot += q;
        work_rem += w - q * bnN;
    }
    // avg_work = floor(sum_work / N), computed without overflowing 256 bits.
    const arith_uint256 avg_work{work_quot + work_rem / bnN};

    // target_work = floor(avg_work * K / weighted_time), K = T * N(N+1)/2.
    // Split avg_work = q * weighted_time + r so that no intermediate overflows;
    // saturate at 2^256 - 1 if the exact result would not fit.
    const arith_uint256 K{static_cast<uint64_t>(T * (N * (N + 1) / 2))};
    const arith_uint256 W{static_cast<uint64_t>(weighted_time)};
    const arith_uint256 q{avg_work / W};
    const arith_uint256 r{avg_work - q * W};
    const arith_uint256 max_value{~arith_uint256{0}};
    arith_uint256 target_work;
    if (q > max_value / K) {
        target_work = max_value;
    } else {
        target_work = q * K;
        const arith_uint256 extra{r * K / W};
        if (target_work > max_value - extra) {
            target_work = max_value;
        } else {
            target_work += extra;
        }
    }

    const uint32_t x_next{AQThresholdForWork(target_work)};
    return std::clamp<uint32_t>(x_next, params.nAQMinThreshold, AQ_MAX_THRESHOLD);
}

bool PermittedDifficultyTransition(const Consensus::Params& params, int64_t height, uint32_t old_nbits, uint32_t new_nbits)
{
    // See header: LWMA can legitimately move to any in-range threshold, so only
    // the range is checked here.
    if (params.fPowNoRetargeting) return old_nbits == new_nbits;
    return AQThresholdInRange(new_nbits, params);
}

// Bypasses the actual proof of work check during fuzz testing with a simplified validation checking whether
// the most significant bit of the last byte of the hash is set.
bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    if (EnableFuzzDeterminism()) return (hash.data()[31] & 0x80) == 0;
    return CheckProofOfWorkImpl(hash, nBits, params);
}

bool CheckProofOfWorkImpl(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    if (!AQThresholdInRange(nBits, params)) return false;
    return AQValue(hash) >= nBits;
}
