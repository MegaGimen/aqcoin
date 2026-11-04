#!/usr/bin/env python3
# Copyright (c) 2026 The AQCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the AQ proof of work and per-block LWMA retargeting on the test network.

Runs on -testnet (regtest has a fixed threshold of 0). Blocks are built and
solved in Python with chosen timestamps, starting from the genesis time, so
that the test controls solve times without mocktime.

Scenarios:
1. One node mines, the other two sync; all agree on the best block. The first
   N blocks use x_init, after which LWMA adjusts the threshold (up for fast
   blocks, down for slow blocks, never below x_min).
2. A partitioned node mines a competing chain; after reconnecting, all nodes
   follow the chain with the most cumulative work.
3. Blocks with insufficient AQ, wrong nBits, or timestamps too far in the
   future are rejected.
"""
import time

from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import aq_value
from test_framework.script import CScript, OP_RETURN
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than, assert_greater_than_or_equal

GENESIS_TIME = 1793750400
X_MIN = 560
X_INIT_TESTNET = 600
LWMA_N = 60
T = 600
MAX_FUTURE_BLOCK_TIME = 30 * 60


class AQPowTest(BitcoinTestFramework):
    def set_test_params(self):
        self.chain = "testnet"
        self.num_nodes = 3
        self.setup_clean_chain = True
        # Blocks carry old timestamps (from the genesis time on); keep the
        # nodes from considering themselves in initial block download.
        self.extra_args = [["-maxtipage=1000000000"]] * self.num_nodes

    def skip_test_if_missing_module(self):
        pass

    def next_bits(self, node):
        return node.getmininginfo()["next"]["aq_threshold"]

    def build_block(self, node, ntime, *, bits=None, solve=True, low_aq=False):
        tip = node.getbestblockhash()
        height = node.getblockcount() + 1
        # Tag the coinbase with the node index so that different nodes build
        # different blocks from the same parent and timestamp.
        coinbase = create_coinbase(height, halving_period=210000,
                                   extra_output_script=CScript([OP_RETURN, bytes([node.index])]))
        block = create_block(int(tip, 16), coinbase, ntime=ntime)
        block.nBits = self.next_bits(node) if bits is None else bits
        if low_aq:
            # Find a nonce whose hash does NOT meet the threshold.
            while aq_value(block.hash_int) >= block.nBits:
                block.nNonce += 1
        elif solve:
            block.solve()
        return block

    def mine(self, node, n, spacing):
        """Mine n blocks on node with the given timestamp spacing."""
        t = node.getblockheader(node.getbestblockhash())["time"]
        bits_seen = []
        for _ in range(n):
            t += spacing
            block = self.build_block(node, t)
            assert_equal(node.submitblock(block.serialize().hex()), None)
            assert_equal(node.getbestblockhash(), block.hash_hex)
            bits_seen.append(block.nBits)
        return bits_seen

    def run_test(self):
        n0, n1, n2 = self.nodes
        assert_greater_than(time.time(), GENESIS_TIME)
        genesis = n0.getblockheader(n0.getblockhash(0))
        assert_equal(genesis["time"], GENESIS_TIME)
        assert_equal(genesis["aq_threshold"], X_INIT_TESTNET)

        self.log.info("Scenario 1: one node mines, the others sync")
        bits = self.mine(n0, LWMA_N + 1, T)
        # Blocks 1..N use x_init; block N+1 is the first LWMA block and with
        # perfectly on-target spacing the threshold stays at x_init.
        assert_equal(bits[:LWMA_N], [X_INIT_TESTNET] * LWMA_N)
        assert_equal(bits[LWMA_N], X_INIT_TESTNET)
        self.sync_blocks()
        assert_equal(n0.getbestblockhash(), n1.getbestblockhash())
        assert_equal(n0.getbestblockhash(), n2.getbestblockhash())
        for h in (1, LWMA_N, LWMA_N + 1):
            hdr = n2.getblockheader(n2.getblockhash(h))
            assert_greater_than_or_equal(aq_value(int(hdr["hash"], 16)), hdr["aq_threshold"])

        self.log.info("LWMA raises the threshold for fast blocks")
        x_before = self.next_bits(n0)
        fast = self.mine(n0, 15, T // 10)
        assert_greater_than(fast[-1], x_before)
        x_fast = self.next_bits(n0)
        assert_greater_than(x_fast, x_before)
        self.log.info(f"  threshold {x_before} -> {x_fast} after 15 blocks at T/10")
        self.sync_blocks()

        self.log.info("LWMA lowers the threshold for slow blocks (clamped at 6T)")
        slow = self.mine(n0, 5, 6 * T)
        x_slow = self.next_bits(n0)
        assert x_slow < x_fast, (x_slow, x_fast)
        assert_greater_than_or_equal(min(slow + [x_slow]), X_MIN)
        self.log.info(f"  threshold {x_fast} -> {x_slow} after 5 blocks at 6T")
        self.sync_blocks()

        self.log.info("Scenario 2: fork and reorg to the chain with more work")
        self.disconnect_nodes(0, 1)
        self.disconnect_nodes(1, 2)
        fork_base = n0.getbestblockhash()
        self.mine(n0, 2, T)
        self.mine(n1, 4, T)
        assert n0.getbestblockhash() != n1.getbestblockhash()
        work0 = int(n0.getblockheader(n0.getbestblockhash())["chainwork"], 16)
        work1 = int(n1.getblockheader(n1.getbestblockhash())["chainwork"], 16)
        assert_greater_than(work1, work0)
        self.connect_nodes(0, 1)
        self.connect_nodes(1, 2)
        self.sync_blocks()
        best = n1.getbestblockhash()
        assert_equal(n0.getbestblockhash(), best)
        assert_equal(n2.getbestblockhash(), best)
        assert_equal(n0.getblockheader(best)["height"], n0.getblockheader(fork_base)["height"] + 4)
        tips = n0.getchaintips()
        self.log.info(f"  chain tips on node0: {[(t['height'], t['branchlen'], t['status']) for t in tips]}")
        assert any(t["branchlen"] == 2 and t["status"] != "active" for t in tips)

        self.log.info("Scenario 3: invalid blocks are rejected")
        t = n0.getblockheader(n0.getbestblockhash())["time"] + T
        x = self.next_bits(n0)

        self.log.info("  AQ value below the threshold -> high-hash")
        block = self.build_block(n0, t, low_aq=True)
        assert aq_value(block.hash_int) < block.nBits
        assert_equal(n0.submitblock(block.serialize().hex()), "high-hash")

        self.log.info("  nBits different from GetNextWorkRequired -> bad-diffbits")
        for wrong in (x + 1, x - 1):
            block = self.build_block(n0, t, bits=wrong)
            assert_equal(n0.submitblock(block.serialize().hex()), "bad-diffbits")

        self.log.info("  nBits below x_min or above 960 -> high-hash (invalid threshold)")
        for out_of_range in (X_MIN - 1, 961):
            block = self.build_block(n0, t, bits=out_of_range, solve=False)
            assert_equal(n0.submitblock(block.serialize().hex()), "high-hash")

        self.log.info("  timestamp beyond now + 30 minutes -> time-too-new")
        block = self.build_block(n0, int(time.time()) + MAX_FUTURE_BLOCK_TIME + 120)
        assert_equal(n0.submitblock(block.serialize().hex()), "time-too-new")

        self.log.info("  timestamp not above median time past -> time-too-old")
        mtp = n0.getblockheader(n0.getbestblockhash())["mediantime"]
        block = self.build_block(n0, mtp)
        assert_equal(n0.submitblock(block.serialize().hex()), "time-too-old")

        self.log.info("  a valid block is still accepted")
        block = self.build_block(n0, t)
        assert_equal(n0.submitblock(block.serialize().hex()), None)
        self.sync_blocks()
        assert_equal(n2.getbestblockhash(), block.hash_hex)


if __name__ == '__main__':
    AQPowTest(__file__).main()
