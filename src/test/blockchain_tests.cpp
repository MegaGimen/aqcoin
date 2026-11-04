// Copyright (c) 2017-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <node/blockstorage.h>
#include <rpc/blockchain.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <util/string.h>

#include <boost/test/unit_test.hpp>

#include <cstdlib>

using util::ToString;

/* Equality between doubles is imprecise. Comparison should be done
 * with a small threshold of tolerance, rather than exact equality.
 */
static bool DoubleEquals(double a, double b, double epsilon)
{
    return std::abs(a - b) < epsilon;
}

static CBlockIndex* CreateBlockIndexWithNbits(uint32_t nbits)
{
    CBlockIndex* block_index = new CBlockIndex();
    block_index->nHeight = 46367;
    block_index->nTime = 1269211443;
    block_index->nBits = nbits;
    return block_index;
}

static void RejectDifficultyMismatch(double difficulty, double expected_difficulty) {
     BOOST_CHECK_MESSAGE(
        DoubleEquals(difficulty / expected_difficulty, 1.0, 0.00001),
        "Difficulty was " + ToString(difficulty)
            + " but was expected to be " + ToString(expected_difficulty));
}

/* Given a BlockIndex with the provided AQ threshold, verify the difficulty
 * work(x) / work(x_min) for mainnet's x_min = 560 (work(560) = 64).
 */
static void TestDifficulty(uint32_t nbits, double expected_difficulty)
{
    CBlockIndex* block_index = CreateBlockIndexWithNbits(nbits);
    double difficulty = GetDifficulty(*block_index, 560);
    delete block_index;

    RejectDifficultyMismatch(difficulty, expected_difficulty);
}

BOOST_FIXTURE_TEST_SUITE(blockchain_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(get_difficulty_at_min_threshold)
{
    TestDifficulty(560, 1.0);
}

BOOST_AUTO_TEST_CASE(get_difficulty_at_init_threshold)
{
    TestDifficulty(700, 0x818809e6 / 64.0);
}

BOOST_AUTO_TEST_CASE(get_difficulty_at_max_threshold)
{
    TestDifficulty(960, 1.157920892373162e77 / 64.0);
}

BOOST_AUTO_TEST_SUITE_END()
