#!/usr/bin/env python3
# Copyright (c) 2026 The AQCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Build and mine an AQCoin genesis block.

The genesis coinbase follows Bitcoin's layout (scriptSig = 486604799, 4,
<message>) and pays 50 coins to an OP_RETURN output, which is unspendable (the
genesis coinbase is never added to the UTXO set anyway).

Usage:
  contrib/aq/genesis.py --time T --bits X [--nonce N] [--grinder PATH] [--threads N]

Without --nonce, the grinder (contrib/aq/genesis_grind) searches for a nonce;
when the 2^32 nonce space is exhausted, the time is incremented.
"""
import argparse
import hashlib
import struct
import subprocess

MESSAGE = b"Nothing human makes it out of the near-future"
COIN = 100_000_000


def sha256d(b: bytes) -> bytes:
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()


def push(data: bytes) -> bytes:
    assert len(data) < 76
    return bytes([len(data)]) + data


def coinbase_tx() -> bytes:
    script_sig = push(struct.pack("<I", 486604799)) + push(b"\x04") + push(MESSAGE)
    script_pubkey = b"\x6a"  # OP_RETURN
    tx = struct.pack("<i", 1)
    tx += b"\x01" + b"\x00" * 32 + b"\xff\xff\xff\xff"
    tx += bytes([len(script_sig)]) + script_sig + b"\xff\xff\xff\xff"
    tx += b"\x01" + struct.pack("<q", 50 * COIN) + bytes([len(script_pubkey)]) + script_pubkey
    tx += struct.pack("<I", 0)
    return tx


def header(time: int, bits: int, nonce: int) -> bytes:
    merkle = sha256d(coinbase_tx())
    return struct.pack("<i", 1) + b"\x00" * 32 + merkle + struct.pack("<III", time, bits, nonce)


def aq(h: bytes) -> int:
    return sum((b >> 4) + (b & 0x0F) for b in h)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--time", type=int, required=True)
    p.add_argument("--bits", type=int, required=True)
    p.add_argument("--nonce", type=int)
    p.add_argument("--grinder", default="contrib/aq/genesis_grind")
    p.add_argument("--threads", type=int, default=0)
    a = p.parse_args()
    t = a.time
    nonce = a.nonce
    while nonce is None:
        hdr = header(t, a.bits, 0)
        cmd = [a.grinder, hdr.hex(), str(a.bits)] + ([str(a.threads)] if a.threads else [])
        out = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout.strip()
        if out != "none":
            nonce = int(out)
        else:
            t += 1
    hdr = header(t, a.bits, nonce)
    h = sha256d(hdr)
    merkle = sha256d(coinbase_tx())
    print(f"time={t} bits={a.bits} nonce={nonce}")
    print(f"hash={h[::-1].hex()} aq={aq(h)}")
    print(f"merkle={merkle[::-1].hex()}")
    assert a.nonce is not None or aq(h) >= a.bits


if __name__ == "__main__":
    main()
