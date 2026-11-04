#!/usr/bin/env python3
# Copyright (c) 2026 The AQCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Generate src/pow_aq_table.h: work(x) for every AQ threshold x in [0, 960].

AQ(h) is the sum of the 64 hex digits of a 256-bit hash (0..960).
C(x)    = number of 256-bit hashes with AQ(h) >= x   (out of 2**256)
work(x) = min(2**256 // C(x), 2**256 - 1)

Usage: contrib/aq/gen_work_table.py > src/pow_aq_table.h
"""


def aq_of_hash(h: bytes) -> int:
    """AQ value of a 32-byte hash: sum of its hex digits (byte order irrelevant)."""
    assert len(h) == 32
    return sum((b >> 4) + (b & 0x0F) for b in h)


def tail_counts():
    """C[x] = number of hashes with AQ(h) >= x, out of 2**256 hashes."""
    dist = [1]
    for _ in range(64):  # 64 hex digits
        new = [0] * (len(dist) + 15)
        for s, c in enumerate(dist):
            for v in range(16):
                new[s + v] += c
        dist = new
    C = [0] * 962
    for x in range(960, -1, -1):
        C[x] = C[x + 1] + dist[x]
    assert C[0] == 2**256
    return C


C = tail_counts()


def work(x: int) -> int:
    return min(2**256 // C[x], 2**256 - 1)


assert work(0) == 1
assert work(960) == 2**256 - 1
assert all(work(x) <= work(x + 1) for x in range(960))
# Integer rounding leaves plateaus (equal neighbouring values) below x = 537;
# work is strictly increasing from x = 537 on. Mainnet/testnet x_min must
# therefore be >= 540.
assert all(work(x) < work(x + 1) for x in range(537, 960))
assert work(536) == work(537)


def main():
    print("// Copyright (c) 2026 The AQCoin developers")
    print("// Distributed under the MIT software license, see the accompanying")
    print("// file COPYING or http://www.opensource.org/licenses/mit-license.php.")
    print()
    print("// Automatically generated, do not edit: contrib/aq/gen_work_table.py")
    print()
    print("#ifndef BITCOIN_POW_AQ_TABLE_H")
    print("#define BITCOIN_POW_AQ_TABLE_H")
    print()
    print("#include <uint256.h>")
    print()
    print("#include <array>")
    print()
    print("/** AQ_WORK_TABLE[x] = work(x) = min(floor(2^256 / C(x)), 2^256 - 1),")
    print(" *  C(x) = number of 256-bit hashes whose hex digit sum is >= x. */")
    print("inline constexpr std::array<uint256, 961> AQ_WORK_TABLE{")
    for x in range(961):
        # uint256 hex constructor takes the big-endian (display) representation.
        print(f'    uint256{{"{work(x):064x}"}}, // {x}')
    print("};")
    print()
    print("#endif // BITCOIN_POW_AQ_TABLE_H")


if __name__ == "__main__":
    main()
