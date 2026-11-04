// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Copyright (c) 2026 The AQCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POW_H
#define BITCOIN_POW_H

#include <consensus/params.h>

#include <cstdint>

class CBlockHeader;
class CBlockIndex;
class uint256;
class arith_uint256;

/** Largest possible AQ value of a 256-bit hash (64 hex digits * 15). */
inline constexpr uint32_t AQ_MAX_THRESHOLD{960};

/**
 * AQ value of a hash: the sum of its 64 hexadecimal digits (0..960).
 * Independent of byte order.
 */
uint32_t AQValue(const uint256& hash);

/**
 * work(x) = min(floor(2^256 / C(x)), 2^256 - 1), where C(x) is the number of
 * 256-bit hashes with AQ >= x. Returns 0 for x > AQ_MAX_THRESHOLD.
 */
arith_uint256 AQWork(uint32_t threshold);

/**
 * Map a target amount of work to the integer threshold x whose work(x) is
 * nearest to it on a logarithmic scale (ties resolve to the lower x).
 * The result is in [0, AQ_MAX_THRESHOLD] and is not clamped to any network minimum.
 */
uint32_t AQThresholdForWork(const arith_uint256& target_work);

/**
 * Return the AQ threshold (stored in nBits) the block following pindexLast
 * must use. Per-block LWMA retargeting, see CalculateNextWorkRequired.
 */
unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader* pblock, const Consensus::Params&);

/**
 * LWMA difficulty retarget over the last params.nLWMAWindow blocks ending at
 * pindexLast. Requires pindexLast->nHeight >= nLWMAWindow.
 */
unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, const Consensus::Params&);

/** Whether nBits is an acceptable threshold on this network: nAQMinThreshold <= nBits <= 960. */
bool AQThresholdInRange(unsigned int nBits, const Consensus::Params&);

/** Check whether a block hash satisfies the AQ proof-of-work requirement specified by nBits */
bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params&);
bool CheckProofOfWorkImpl(uint256 hash, unsigned int nBits, const Consensus::Params&);

/**
 * Return false if the proof-of-work requirement specified by new_nbits at a
 * given height is not possible, given the proof-of-work on the prior block as
 * specified by old_nbits.
 *
 * With per-block LWMA retargeting the next threshold depends on the full
 * window of prior timestamps and thresholds, which are not available here,
 * and any threshold within the network's range is reachable after a suitable
 * sequence of timestamps. This check therefore only verifies that new_nbits is
 * within [nAQMinThreshold, 960]; the exact value is enforced by
 * ContextualCheckBlockHeader once the header connects to a known chain.
 */
bool PermittedDifficultyTransition(const Consensus::Params& params, int64_t height, uint32_t old_nbits, uint32_t new_nbits);

#endif // BITCOIN_POW_H
