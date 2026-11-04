#!/usr/bin/env python3
# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test validateaddress for main chain"""

from test_framework.test_framework import BitcoinTestFramework

from test_framework.util import assert_equal

INVALID_DATA = [
    # BIP 173
    (
        "tc1qw508d6qejxtdg4y5r3zarvary0c5xw7kg3g4ty",
        "Invalid or unsupported Segwit (Bech32) or Base58 encoding.",  # Invalid hrp
        [],
    ),
    ("bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t5", "Invalid Bech32 checksum", [41]),
    (
        "AQ13W508D6QEJXTDG4Y5R3ZARVARY0C5XW7KX948CE",
        "Version 1+ witness address must use Bech32m checksum",
        [],
    ),
    (
        "aq1rw535033p",
        "Version 1+ witness address must use Bech32m checksum",  # Invalid program length
        [],
    ),
    (
        "aq10w508d6qejxtdg4y5r3zarvary0c5xw7kw508d6qejxtdg4y5r3zarvary0c5xw7kw5dkjdhr",
        "Version 1+ witness address must use Bech32m checksum",  # Invalid program length
        [],
    ),
    (
        "AQ1QR508D6QEJXTDG4Y5R3ZARVARYV28906T",
        "Invalid Bech32 v0 address program size (16 bytes), per BIP141",
        [],
    ),
    (
        "taq1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3qgd4r8r",
        "Invalid or unsupported Segwit (Bech32) or Base58 encoding.",  # tb1, Mixed case
        [],
    ),
    (
        "aq1qw508d6qejxtdg4y5r3zarvary0c5xw7kehnc6x",
        "Invalid character or mixed case",  # bc1, Mixed case, not in BIP 173 test vectors
        [40],
    ),
    (
        "aq1zw508d6qejxtdg4y5r3zarvaryvqmz8jcy",
        "Version 1+ witness address must use Bech32m checksum",  # Wrong padding
        [],
    ),
    (
        "taq1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3p4mpk63",
        "Invalid or unsupported Segwit (Bech32) or Base58 encoding.",  # tb1, Non-zero padding in 8-to-5 conversion
        [],
    ),
    ("aq1l3welg", "Empty Bech32 data section", []),
    # BIP 350
    (
        "tc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vq5zuyut",
        "Invalid or unsupported Segwit (Bech32) or Base58 encoding.",  # Invalid human-readable part
        [],
    ),
    (
        "aq1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqxx2pds",
        "Version 1+ witness address must use Bech32m checksum",  # Invalid checksum (Bech32 instead of Bech32m)
        [],
    ),
    (
        "taq1z0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vq0zpfj5",
        "Invalid or unsupported Segwit (Bech32) or Base58 encoding.",  # tb1, Invalid checksum (Bech32 instead of Bech32m)
        [],
    ),
    (
        "AQ1S0XLXVLHEMJA6C4DQV22UAPCTQUPFHLXM9H8Z3K2E72Q4K9HCZ7VQ9EQX9Z",
        "Version 1+ witness address must use Bech32m checksum",  # Invalid checksum (Bech32 instead of Bech32m)
        [],
    ),
    (
        "aq1qw508d6qejxtdg4y5r3zarvary0c5xw7kvtr5ly",
        "Version 0 witness address must use Bech32 checksum",  # Invalid checksum (Bech32m instead of Bech32)
        [],
    ),
    (
        "taq1q0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqdgc0yr",
        "Invalid or unsupported Segwit (Bech32) or Base58 encoding.",  # tb1, Invalid checksum (Bech32m instead of Bech32)
        [],
    ),
    (
        "bc1p38j9r5y49hruaue7wxjce0updqjuyyx0kh56v8s25huc6995vvpql3jow4",
        "Invalid Base 32 character",  # Invalid character in checksum
        [59],
    ),
    (
        "AQ130XLXVLHEMJA6C4DQV22UAPCTQUPFHLXM9H8Z3K2E72Q4K9HCZ7VQ0WQ0A7",
        "Invalid Bech32 address witness version",
        [],
    ),
    ("aq1pw5qvd606", "Invalid Bech32 address program size (1 byte)", []),
    (
        "aq1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7v8n0nx0muaewav25ltgy0q",
        "Invalid Bech32 address program size (41 bytes)",
        [],
    ),
    (
        "AQ1QR508D6QEJXTDG4Y5R3ZARVARYV28906T",
        "Invalid Bech32 v0 address program size (16 bytes), per BIP141",
        [],
    ),
    (
        "taq1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqjrg2ea",
        "Invalid or unsupported Segwit (Bech32) or Base58 encoding.",  # tb1, Mixed case
        [],
    ),
    (
        "aq1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7v07qc8htdn",
        "Invalid padding in Bech32 data section",  # zero padding of more than 4 bits
        [],
    ),
    (
        "taq1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vp04uly0",
        "Invalid or unsupported Segwit (Bech32) or Base58 encoding.",  # tb1, Non-zero padding in 8-to-5 conversion
        [],
    ),
    ("aq1l3welg", "Empty Bech32 data section", []),
]
VALID_DATA = [
    # BIP 350
    (
        "AQ1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KEHNC6X",
        "0014751e76e8199196d454941c45d1b3a323f1433bd6",
    ),
    # (
    #   "taq1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3qgd4r8r",
    #   "00201863143c14c5166804bd19203356da136c985678cd4d27a1b8c6329604903262",
    # ),
    (
        "aq1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3qf58ykv",
        "00201863143c14c5166804bd19203356da136c985678cd4d27a1b8c6329604903262",
    ),
    (
        "aq1pw508d6qejxtdg4y5r3zarvary0c5xw7kw508d6qejxtdg4y5r3zarvary0c5xw7kladued",
        "5128751e76e8199196d454941c45d1b3a323f1433bd6751e76e8199196d454941c45d1b3a323f1433bd6",
    ),
    ("AQ1SW50QSZ5UL0", "6002751e"),
    ("aq1zw508d6qejxtdg4y5r3zarvaryvjxtu86", "5210751e76e8199196d454941c45d1b3a323"),
    # (
    #   "taq1qqqqqp399et2xygdj5xreqhjjvcmzhxw4aywxecjdzew6hylgvsesymadxe",
    #   "0020000000c4a5cad46221b2a187905e5266362b99d5e91c6ce24d165dab93e86433",
    # ),
    (
        "aq1qqqqqp399et2xygdj5xreqhjjvcmzhxw4aywxecjdzew6hylgvses9z02hk",
        "0020000000c4a5cad46221b2a187905e5266362b99d5e91c6ce24d165dab93e86433",
    ),
    # (
    #   "taq1pqqqqp399et2xygdj5xreqhjjvcmzhxw4aywxecjdzew6hylgvseswvay79",
    #   "5120000000c4a5cad46221b2a187905e5266362b99d5e91c6ce24d165dab93e86433",
    # ),
    (
        "aq1pqqqqp399et2xygdj5xreqhjjvcmzhxw4aywxecjdzew6hylgvses040r02",
        "5120000000c4a5cad46221b2a187905e5266362b99d5e91c6ce24d165dab93e86433",
    ),
    (
        "aq1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqn66dgj",
        "512079be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798",
    ),
    # PayToAnchor(P2A)
    (
        "aq1pfeesgvtcr5",
        "51024e73",
    ),
]


class ValidateAddressMainTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.chain = ""  # main
        self.num_nodes = 1
        self.extra_args = [["-prune=899"]] * self.num_nodes

    def check_valid(self, addr, spk):
        info = self.nodes[0].validateaddress(addr)
        assert_equal(info["isvalid"], True)
        assert_equal(info["scriptPubKey"], spk)
        assert "error" not in info
        assert "error_locations" not in info

    def check_invalid(self, addr, error_str, error_locations):
        res = self.nodes[0].validateaddress(addr)
        assert_equal(res["isvalid"], False)
        assert_equal(res["error"], error_str)
        assert_equal(res["error_locations"], error_locations)

    def test_validateaddress(self):
        for (addr, error, locs) in INVALID_DATA:
            self.check_invalid(addr, error, locs)
        for (addr, spk) in VALID_DATA:
            self.check_valid(addr, spk)

    def run_test(self):
        self.test_validateaddress()


if __name__ == "__main__":
    ValidateAddressMainTest(__file__).main()
