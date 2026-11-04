// Copyright (c) 2015-present The Bitcoin Core developers
// Copyright (c) 2026 The AQCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <pow.h>
#include <pow_aq_table.h>
#include <random.h>
#include <test/util/common.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/chaintype.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(pow_tests, BasicTestingSetup)

namespace {

/** floor(2^256 / c) capped at 2^256 - 1, for 1 <= c < 2^256. */
arith_uint256 TwoPow256Div(const arith_uint256& c)
{
    const arith_uint256 max{~arith_uint256{0}};
    const arith_uint256 q{max / c};
    const arith_uint256 r{max - q * c}; // (2^256 - 1) mod c
    if (r + 1 == c) {
        // c divides 2^256 exactly.
        if (q == max) return max;
        return q + arith_uint256{1};
    }
    return q;
}

/** Independent recomputation of work(x) by convolution of 64 hex digits. */
arith_uint256 RecomputeWork(uint32_t x)
{
    if (x == 0) return arith_uint256{1}; // C(0) = 2^256, not representable.
    std::vector<arith_uint256> dist(1, arith_uint256{1});
    for (int digit = 0; digit < 64; ++digit) {
        std::vector<arith_uint256> next(dist.size() + 15, arith_uint256{0});
        for (size_t s = 0; s < dist.size(); ++s) {
            for (int v = 0; v < 16; ++v) next[s + v] += dist[s];
        }
        dist = std::move(next);
    }
    arith_uint256 c{0};
    for (size_t s = x; s < dist.size(); ++s) c += dist[s];
    return TwoPow256Div(c);
}

uint256 HashFromNibbles(const std::vector<unsigned char>& nibbles)
{
    uint256 h;
    for (size_t i = 0; i < 64; ++i) {
        const unsigned char n{nibbles[i % nibbles.size()]};
        unsigned char& byte{h.data()[i / 2]};
        byte = (i % 2) ? (byte | n) : static_cast<unsigned char>(n << 4);
    }
    return h;
}

/** Expected number of hashes to find a block at threshold x: 2^256 / C(x) ~ work(x). */
double ExpectedHashes(uint32_t x) { return AQWork(x).getdouble(); }

struct SimResult {
    std::vector<uint32_t> thresholds; // threshold of each simulated block
    std::vector<int64_t> intervals;   // solve time of each simulated block
};

/**
 * Simulate a chain on the given consensus params. hashrate(height) returns
 * hashes per second while the block at that height is being mined.
 */
template <typename F>
SimResult SimulateChain(const Consensus::Params& params, uint32_t genesis_bits, int n_blocks, F hashrate, FastRandomContext& rng)
{
    std::vector<CBlockIndex> blocks(n_blocks + 1);
    blocks[0].nHeight = 0;
    blocks[0].nTime = 1'700'000'000;
    blocks[0].nBits = genesis_bits;
    blocks[0].pprev = nullptr;
    SimResult res;
    for (int h = 1; h <= n_blocks; ++h) {
        CBlockHeader header;
        const uint32_t bits{GetNextWorkRequired(&blocks[h - 1], &header, params)};
        // Exponentially distributed solve time.
        const double mean{ExpectedHashes(bits) / hashrate(h)};
        double u{rng.randrange(1'000'000'000) / 1e9};
        const double solve{-std::log(1.0 - u) * mean};
        const int64_t interval{std::max<int64_t>(1, std::llround(solve))};
        blocks[h].pprev = &blocks[h - 1];
        blocks[h].nHeight = h;
        blocks[h].nTime = blocks[h - 1].nTime + interval;
        blocks[h].nBits = bits;
        res.thresholds.push_back(bits);
        res.intervals.push_back(interval);
    }
    return res;
}

double MeanInterval(const SimResult& r, size_t from, size_t to)
{
    double sum{0};
    for (size_t i = from; i < to; ++i) sum += r.intervals[i];
    return sum / (to - from);
}

} // namespace

/* ---------- Phase 1: AQ value and work table ---------- */

BOOST_AUTO_TEST_CASE(aq_value_vectors)
{
    BOOST_CHECK_EQUAL(AQValue(uint256::ZERO), 0U);
    BOOST_CHECK_EQUAL(AQValue(uint256{"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"}), 960U);
    BOOST_CHECK_EQUAL(AQValue(uint256::ONE), 1U);
    BOOST_CHECK_EQUAL(AQValue(HashFromNibbles({0xa})), 640U);
    BOOST_CHECK_EQUAL(AQValue(HashFromNibbles({1, 2, 3, 4})), 160U);
    // SHA256("aq0".."aq4") reference values from Python (aq_of_hash in contrib/aq/gen_work_table.py).
    BOOST_CHECK_EQUAL(AQValue(uint256{"1459a91759c500d8d40fba6314c339818e4b8f301eb303022c136a91e62b27ff"}), 414U);
    BOOST_CHECK_EQUAL(AQValue(uint256{"3fa90e9b019d42d2b26628f8bf06382cab39cf50dbaabf700c28dd9ae369facc"}), 520U);
    BOOST_CHECK_EQUAL(AQValue(uint256{"527ffcc3f80e52d916dfe28f053b72b412350d68d13c256b1354e8c7c65af315"}), 456U);
    BOOST_CHECK_EQUAL(AQValue(uint256{"e29d8eca5d5153ea495bbe72be5d35660991c268fdd5dbc20bb83d6705363f64"}), 496U);
    BOOST_CHECK_EQUAL(AQValue(uint256{"a2045be8b0910c122be6e602fb7eff1e710564deb5cc83ffab8489e5a0ba49f0"}), 496U);

    // Byte order does not matter.
    for (int i = 0; i < 100; ++i) {
        uint256 h{m_rng.rand256()};
        uint256 rev;
        std::reverse_copy(h.begin(), h.end(), rev.begin());
        BOOST_CHECK_EQUAL(AQValue(h), AQValue(rev));
        // Equals the hex digit sum of the display string.
        uint32_t sum{0};
        for (char c : h.GetHex()) sum += (c <= '9') ? c - '0' : c - 'a' + 10;
        BOOST_CHECK_EQUAL(AQValue(h), sum);
    }
}

BOOST_AUTO_TEST_CASE(aq_work_table)
{
    BOOST_CHECK(AQWork(0) == arith_uint256{1});
    BOOST_CHECK(AQWork(960) == ~arith_uint256{0});
    BOOST_CHECK(AQWork(961) == arith_uint256{0});
    BOOST_CHECK(AQWork(0xffffffff) == arith_uint256{0});
    BOOST_CHECK(AQWork(480) == arith_uint256{1});
    BOOST_CHECK(AQWork(537) == arith_uint256{15});
    BOOST_CHECK(AQWork(560) == arith_uint256{64});
    BOOST_CHECK(AQWork(666) == arith_uint256{0x670f7d});
    BOOST_CHECK(AQWork(700) == arith_uint256{0x818809e6});

    // Monotone non-decreasing everywhere, strictly increasing from 537.
    for (uint32_t x = 0; x < 960; ++x) {
        BOOST_CHECK(AQWork(x) <= AQWork(x + 1));
        if (x >= 537) BOOST_CHECK(AQWork(x) < AQWork(x + 1));
    }
    BOOST_CHECK(AQWork(536) == AQWork(537));

    // Independent recomputation.
    for (uint32_t x : {0U, 1U, 480U, 537U, 560U, 600U, 666U, 700U, 800U, 900U, 959U, 960U}) {
        BOOST_CHECK_MESSAGE(RecomputeWork(x) == AQWork(x), "work mismatch at x=" << x);
    }
}

BOOST_AUTO_TEST_CASE(aq_threshold_for_work)
{
    // Exact table values map back to the largest x with that work.
    for (uint32_t x = 537; x <= 960; ++x) {
        BOOST_CHECK_EQUAL(AQThresholdForWork(AQWork(x)), x);
    }
    BOOST_CHECK_EQUAL(AQThresholdForWork(arith_uint256{0}), 0U);
    BOOST_CHECK_EQUAL(AQThresholdForWork(~arith_uint256{0}), 960U);
    // Nearest in log space between neighbours.
    for (uint32_t x = 560; x < 960; ++x) {
        const arith_uint256 w0{AQWork(x)}, w1{AQWork(x + 1)};
        const double geo{std::sqrt(w0.getdouble() * w1.getdouble())};
        const arith_uint256 just_below{w0 + (w1 - w0) / 100};
        const arith_uint256 just_above{w1 - (w1 - w0) / 100};
        BOOST_CHECK_EQUAL(AQThresholdForWork(just_below), x);
        BOOST_CHECK_EQUAL(AQThresholdForWork(just_above), x + 1);
        (void)geo;
    }
    // Explicit geometric-mean boundary: work(700)=0x818809e6, work(701)
    const arith_uint256 w700{AQWork(700)}, w701{AQWork(701)};
    const uint64_t a{w700.GetLow64()}, b{w701.GetLow64()};
    // floor(sqrt(a*b)) maps to 700 (target^2 <= a*b), +1 maps to 701.
    uint64_t s{static_cast<uint64_t>(std::sqrt(static_cast<long double>(a) * b))};
    while ((unsigned __int128)s * s > (unsigned __int128)a * b) --s;
    while ((unsigned __int128)(s + 1) * (s + 1) <= (unsigned __int128)a * b) ++s;
    BOOST_CHECK_EQUAL(AQThresholdForWork(arith_uint256{s}), 700U);
    BOOST_CHECK_EQUAL(AQThresholdForWork(arith_uint256{s + 1}), 701U);
}

/* ---------- Phase 2: proof of work check and LWMA ---------- */

BOOST_AUTO_TEST_CASE(check_proof_of_work_aq)
{
    const auto main_params = CreateChainParams(*m_node.args, ChainType::MAIN);
    const auto& consensus = main_params->GetConsensus();
    const uint256 h640{HashFromNibbles({0xa})}; // AQ = 640
    BOOST_CHECK(CheckProofOfWorkImpl(h640, 640, consensus));
    BOOST_CHECK(CheckProofOfWorkImpl(h640, 600, consensus));
    BOOST_CHECK(!CheckProofOfWorkImpl(h640, 641, consensus));
    // Below the network minimum is invalid even if the hash qualifies.
    BOOST_CHECK(!CheckProofOfWorkImpl(h640, consensus.nAQMinThreshold - 1, consensus));
    BOOST_CHECK(CheckProofOfWorkImpl(h640, consensus.nAQMinThreshold, consensus));
    // Above 960 is always invalid.
    const uint256 hmax{"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
    BOOST_CHECK(CheckProofOfWorkImpl(hmax, 960, consensus));
    BOOST_CHECK(!CheckProofOfWorkImpl(hmax, 961, consensus));
    BOOST_CHECK(!CheckProofOfWorkImpl(hmax, 0x1d00ffff, consensus));

    // Regtest: threshold 0, any hash is valid.
    const auto reg_params = CreateChainParams(*m_node.args, ChainType::REGTEST);
    BOOST_CHECK(CheckProofOfWorkImpl(uint256::ZERO, 0, reg_params->GetConsensus()));
    BOOST_CHECK(CheckProofOfWorkImpl(m_rng.rand256(), 0, reg_params->GetConsensus()));
}

BOOST_AUTO_TEST_CASE(block_proof_is_aq_work)
{
    CBlockIndex index;
    for (uint32_t x : {0U, 560U, 700U, 960U}) {
        index.nBits = x;
        BOOST_CHECK(GetBlockProof(index) == AQWork(x));
    }
    index.nBits = 961;
    BOOST_CHECK(GetBlockProof(index) == arith_uint256{0});
}

BOOST_AUTO_TEST_CASE(permitted_difficulty_transition_aq)
{
    const auto chain_params = CreateChainParams(*m_node.args, ChainType::MAIN);
    const auto& consensus = chain_params->GetConsensus();
    BOOST_CHECK(PermittedDifficultyTransition(consensus, 100, 700, 560));
    BOOST_CHECK(PermittedDifficultyTransition(consensus, 100, 560, 960));
    BOOST_CHECK(!PermittedDifficultyTransition(consensus, 100, 700, 559));
    BOOST_CHECK(!PermittedDifficultyTransition(consensus, 100, 700, 961));
    const auto reg_params = CreateChainParams(*m_node.args, ChainType::REGTEST);
    BOOST_CHECK(PermittedDifficultyTransition(reg_params->GetConsensus(), 100, 0, 0));
    BOOST_CHECK(!PermittedDifficultyTransition(reg_params->GetConsensus(), 100, 0, 1));
}

BOOST_AUTO_TEST_CASE(lwma_initial_window_and_regtest)
{
    const auto chain_params = CreateChainParams(*m_node.args, ChainType::MAIN);
    const auto& consensus = chain_params->GetConsensus();
    BOOST_CHECK_EQUAL(consensus.nAQMinThreshold, 560U);
    BOOST_CHECK_EQUAL(consensus.nAQInitThreshold, 700U);
    BOOST_CHECK_EQUAL(consensus.nLWMAWindow, 60);
    BOOST_CHECK_EQUAL(consensus.nPowTargetSpacing, 600);

    std::vector<CBlockIndex> blocks(100);
    for (int i = 0; i < 100; ++i) {
        blocks[i].pprev = i ? &blocks[i - 1] : nullptr;
        blocks[i].nHeight = i;
        blocks[i].nTime = 1'700'000'000 + i * 600;
        blocks[i].nBits = 700;
    }
    CBlockHeader header;
    // Heights 0..N-1 as pindexLast: not enough history -> x_init.
    for (int i = 0; i < 60; ++i) {
        BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks[i], &header, consensus), 700U);
    }
    // Perfectly on-target window -> unchanged threshold.
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks[60], &header, consensus), 700U);
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks[99], &header, consensus), 700U);

    // All blocks 1s apart (clamped at 1): work target = 600x -> much harder.
    for (int i = 0; i < 100; ++i) blocks[i].nTime = 1'700'000'000 + i;
    const uint32_t fast{GetNextWorkRequired(&blocks[99], &header, consensus)};
    BOOST_CHECK_EQUAL(fast, AQThresholdForWork(AQWork(700) * 600));
    BOOST_CHECK(fast > 730);

    // Timestamps going backwards are clamped to 1s, not negative.
    for (int i = 0; i < 100; ++i) blocks[i].nTime = 1'700'000'000 - i;
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks[99], &header, consensus), fast);

    // Huge gaps are clamped at 6T: target work = work/6, never below x_min.
    for (int i = 0; i < 100; ++i) blocks[i].nTime = 1'700'000'000 + i * 1'000'000;
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks[99], &header, consensus), AQThresholdForWork(AQWork(700) / 6));
    for (int i = 0; i < 100; ++i) blocks[i].nBits = 560;
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks[99], &header, consensus), 560U);

    // Max thresholds don't overflow: result saturates at 960.
    for (int i = 0; i < 100; ++i) {
        blocks[i].nBits = 960;
        blocks[i].nTime = 1'700'000'000 + i;
    }
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks[99], &header, consensus), 960U);

    // Regtest never retargets.
    const auto reg_params = CreateChainParams(*m_node.args, ChainType::REGTEST);
    for (int i = 0; i < 100; ++i) blocks[i].nBits = 0;
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks[99], &header, reg_params->GetConsensus()), 0U);
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks[5], &header, reg_params->GetConsensus()), 0U);
}

BOOST_AUTO_TEST_CASE(lwma_simulation_stable_and_steps)
{
    const auto chain_params = CreateChainParams(*m_node.args, ChainType::MAIN);
    const auto& consensus = chain_params->GetConsensus();
    FastRandomContext rng{uint256{1}};
    const double T{static_cast<double>(consensus.nPowTargetSpacing)};

    // Hashrate that finds an x=650 block every T seconds on average.
    const double H{ExpectedHashes(650) / T};

    // 1. Stable hashrate: average interval near T.
    {
        const auto r{SimulateChain(consensus, 650, 3000, [&](int) { return H; }, rng)};
        const double mean{MeanInterval(r, 500, 3000)};
        BOOST_TEST_MESSAGE("stable: mean interval " << mean);
        BOOST_CHECK(mean > 0.85 * T && mean < 1.15 * T);
        const auto [lo, hi] = std::minmax_element(r.thresholds.begin() + 500, r.thresholds.end());
        BOOST_TEST_MESSAGE("stable: threshold range [" << *lo << ", " << *hi << "]");
        // Exponential solve times make the 60-block estimate wobble by roughly
        // +-15% in hashrate, i.e. a few threshold steps around 650.
        BOOST_CHECK(*lo >= 630 && *hi <= 670);
        size_t near{0};
        for (size_t i = 500; i < r.thresholds.size(); ++i) near += (r.thresholds[i] >= 645 && r.thresholds[i] <= 655);
        BOOST_TEST_MESSAGE("stable: " << near << " of " << r.thresholds.size() - 500 << " thresholds within 650 +- 5");
        BOOST_CHECK(near * 10 >= (r.thresholds.size() - 500) * 8);
    }
    // 2. Hashrate jumps 10x at height 1000.
    {
        const auto r{SimulateChain(consensus, 650, 3000, [&](int h) { return h < 1000 ? H : 10 * H; }, rng)};
        // Converges: threshold at 10x hashrate is where work is 10x larger.
        const uint32_t expected_x{AQThresholdForWork(AQWork(650) * 10)};
        int converged_at{-1};
        for (int i = 1000; i < 3000; ++i) {
            if (r.thresholds[i] + 2 >= expected_x) { converged_at = i; break; }
        }
        BOOST_TEST_MESSAGE("x10: expected x " << expected_x << " reached after " << (converged_at - 1000) << " blocks");
        BOOST_CHECK(converged_at >= 0 && converged_at - 1000 <= 150);
        const double mean{MeanInterval(r, 1300, 3000)};
        BOOST_TEST_MESSAGE("x10: mean interval after convergence " << mean);
        BOOST_CHECK(mean > 0.85 * T && mean < 1.15 * T);
    }
    // 3. Hashrate drops to 1/10 at height 1000.
    {
        const auto r{SimulateChain(consensus, 650, 3000, [&](int h) { return h < 1000 ? H : H / 10; }, rng)};
        const uint32_t expected_x{AQThresholdForWork(AQWork(650) / 10)};
        int converged_at{-1};
        for (int i = 1000; i < 3000; ++i) {
            if (r.thresholds[i] <= expected_x + 2) { converged_at = i; break; }
        }
        BOOST_TEST_MESSAGE("x0.1: expected x " << expected_x << " reached after " << (converged_at - 1000) << " blocks");
        BOOST_CHECK(converged_at >= 0 && converged_at - 1000 <= 150);
        const double mean{MeanInterval(r, 1300, 3000)};
        BOOST_TEST_MESSAGE("x0.1: mean interval after convergence " << mean);
        BOOST_CHECK(mean > 0.85 * T && mean < 1.15 * T);
    }
}

BOOST_AUTO_TEST_CASE(lwma_simulation_from_init_low_hashrate)
{
    // Phase 4 requirement: start at x_init with only 1/100 of the hashrate
    // assumed by x_init (one x_init block per T), LWMA walks the threshold
    // down and never goes below x_min.
    const auto chain_params = CreateChainParams(*m_node.args, ChainType::MAIN);
    const auto& consensus = chain_params->GetConsensus();
    FastRandomContext rng{uint256{2}};
    const double T{static_cast<double>(consensus.nPowTargetSpacing)};
    const double H{ExpectedHashes(consensus.nAQInitThreshold) / T / 100};
    const auto r{SimulateChain(consensus, consensus.nAQInitThreshold, 2000, [&](int) { return H; }, rng)};
    const uint32_t expected_x{AQThresholdForWork(AQWork(consensus.nAQInitThreshold) / 100)};
    int converged_at{-1};
    for (size_t i = 0; i < r.thresholds.size(); ++i) {
        BOOST_CHECK(r.thresholds[i] >= consensus.nAQMinThreshold);
        if (converged_at < 0 && r.thresholds[i] <= expected_x + 2) converged_at = i;
    }
    BOOST_TEST_MESSAGE("init/100: expected x " << expected_x << " reached at height " << converged_at);
    BOOST_CHECK(converged_at > 0 && converged_at <= 400);
    BOOST_CHECK(MeanInterval(r, 800, 2000) > 0.85 * T && MeanInterval(r, 800, 2000) < 1.15 * T);

    // Essentially no hashrate: floor at x_min.
    const double tiny{ExpectedHashes(consensus.nAQMinThreshold) / T / 1000};
    const auto r2{SimulateChain(consensus, consensus.nAQInitThreshold, 1500, [&](int) { return tiny; }, rng)};
    for (uint32_t x : r2.thresholds) BOOST_CHECK(x >= consensus.nAQMinThreshold);
    BOOST_CHECK_EQUAL(r2.thresholds.back(), consensus.nAQMinThreshold);
}

BOOST_AUTO_TEST_CASE(GetBlockProofEquivalentTime_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    std::vector<CBlockIndex> blocks(10000);
    for (int i = 0; i < 10000; i++) {
        blocks[i].pprev = i ? &blocks[i - 1] : nullptr;
        blocks[i].nHeight = i;
        blocks[i].nTime = 1269211443 + i * chainParams->GetConsensus().nPowTargetSpacing;
        blocks[i].nBits = 600;
        blocks[i].nChainWork = i ? blocks[i - 1].nChainWork + GetBlockProof(blocks[i - 1]) : arith_uint256(0);
    }

    for (int j = 0; j < 1000; j++) {
        CBlockIndex *p1 = &blocks[m_rng.randrange(10000)];
        CBlockIndex *p2 = &blocks[m_rng.randrange(10000)];
        CBlockIndex *p3 = &blocks[m_rng.randrange(10000)];

        int64_t tdiff = GetBlockProofEquivalentTime(*p1, *p2, *p3, chainParams->GetConsensus());
        BOOST_CHECK_EQUAL(tdiff, p1->GetBlockTime() - p2->GetBlockTime());
    }
}

void sanity_check_chainparams(const ArgsManager& args, ChainType chain_type)
{
    const auto chainParams = CreateChainParams(args, chain_type);
    const auto consensus = chainParams->GetConsensus();

    // hash genesis is correct
    BOOST_CHECK_EQUAL(consensus.hashGenesisBlock, chainParams->GenesisBlock().GetHash());

    // target timespan is an even multiple of spacing
    BOOST_CHECK_EQUAL(consensus.nPowTargetTimespan % consensus.nPowTargetSpacing, 0);

    // genesis threshold is the initial threshold and within range
    BOOST_CHECK_EQUAL(chainParams->GenesisBlock().nBits, consensus.nAQInitThreshold);
    BOOST_CHECK(AQThresholdInRange(chainParams->GenesisBlock().nBits, consensus));
    BOOST_CHECK(consensus.nAQInitThreshold >= consensus.nAQMinThreshold);

    if (!consensus.fPowNoRetargeting) {
        // work must be strictly increasing above x_min (see contrib/aq/gen_work_table.py)
        BOOST_CHECK(consensus.nAQMinThreshold >= 540);
        BOOST_CHECK(consensus.nLWMAWindow > 0);
    }
}

BOOST_AUTO_TEST_CASE(ChainParams_MAIN_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::MAIN);
}

BOOST_AUTO_TEST_CASE(ChainParams_REGTEST_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::REGTEST);
}

BOOST_AUTO_TEST_CASE(ChainParams_TESTNET_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::TESTNET);
}

BOOST_AUTO_TEST_SUITE_END()
