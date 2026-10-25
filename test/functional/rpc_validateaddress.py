#!/usr/bin/env python3
# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test validateaddress for main chain"""

from test_framework.address import HRP_BY_CHAIN
from test_framework.segwit_addr import encode_segwit_address
from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.test_node import FailedToStartError

from test_framework.util import assert_equal

# Addresses of the main network that are off by one character. They are made here,
# so that they follow the address prefix of the chain.
_V0 = encode_segwit_address(HRP_BY_CHAIN["main"], 0, bytes.fromhex("751e76e8199196d454941c45d1b3a323f1433bd6"))
ONE_ERROR = _V0[:-1] + ("q" if _V0[-1] != "q" else "p")
_V1 = encode_segwit_address(HRP_BY_CHAIN["main"], 1, bytes.fromhex("3c4a41d0c52be9f3b33e34ac657e0d0825c210cf5ad2c3c154af98d1a5146300"))
INVALID_CHARACTER = _V1[:-3] + "o" + _V1[-2:]
# Upper case, but for the last letter.
_LAST_LETTER = max(i for i, c in enumerate(_V0) if c.isalpha())
MIXED_CASE = _V0[:_LAST_LETTER].upper() + _V0[_LAST_LETTER] + _V0[_LAST_LETTER + 1:].upper()

INVALID_DATA = [
    # BIP 173
    (
        "tc1qw508d6qejxtdg4y5r3zarvary0c5xw7kg3g4ty",
        "Invalid or unsupported Segwit (Bech32) or Base58 encoding.",  # Invalid hrp
        [],
    ),
    (ONE_ERROR, "Invalid Bech32 checksum", [len(ONE_ERROR) - 1]),
    (
        "LY13W508D6QEJXTDG4Y5R3ZARVARY0C5XW7KC94WLQ",
        "Version 1+ witness address must use Bech32m checksum",
        [],
    ),
    (
        "ly1rw5fzuetl",
        "Version 1+ witness address must use Bech32m checksum",  # Invalid program length
        [],
    ),
    (
        "ly10w508d6qejxtdg4y5r3zarvary0c5xw7kw508d6qejxtdg4y5r3zarvary0c5xw7kw50u0vwy",
        "Version 1+ witness address must use Bech32m checksum",  # Invalid program length
        [],
    ),
    (
        "LY1QR508D6QEJXTDG4Y5R3ZARVARYVS2PF87",
        "Invalid Bech32 v0 address program size (16 bytes), per BIP141",
        [],
    ),
    (
        "tchn1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3q0sL5k7",
        "Invalid or unsupported Segwit (Bech32) or Base58 encoding.",  # tchn1, Mixed case
        [],
    ),
    (
        MIXED_CASE,
        "Invalid character or mixed case",  # Mixed case, not in BIP 173 test vectors
        [_LAST_LETTER],
    ),
    (
        "ly1zw508d6qejxtdg4y5r3zarvaryvqwkvc48",
        "Version 1+ witness address must use Bech32m checksum",  # Wrong padding
        [],
    ),
    (
        "tly1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3ps7yvkw",
        "Invalid or unsupported Segwit (Bech32) or Base58 encoding.",  # tchn1, Non-zero padding in 8-to-5 conversion
        [],
    ),
    ("ly1cfnn9j", "Empty Bech32 data section", []),
    # BIP 350
    (
        "tc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vq5zuyut",
        "Invalid or unsupported Segwit (Bech32) or Base58 encoding.",  # Invalid human-readable part
        [],
    ),
    (
        "ly1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqrr0mp0",
        "Version 1+ witness address must use Bech32m checksum",  # Invalid checksum (Bech32 instead of Bech32m)
        [],
    ),
    (
        "tly1z0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vq28yn7t",
        "Invalid or unsupported Segwit (Bech32) or Base58 encoding.",  # tchn1, Invalid checksum (Bech32 instead of Bech32m)
        [],
    ),
    (
        "LY1S0XLXVLHEMJA6C4DQV22UAPCTQUPFHLXM9H8Z3K2E72Q4K9HCZ7VQQU9UFA",
        "Version 1+ witness address must use Bech32m checksum",  # Invalid checksum (Bech32 instead of Bech32m)
        [],
    ),
    (
        "ly1qw508d6qejxtdg4y5r3zarvary0c5xw7kjtraca",
        "Version 0 witness address must use Bech32 checksum",  # Invalid checksum (Bech32m instead of Bech32)
        [],
    ),
    (
        "tly1q0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqgda4gu",
        "Invalid or unsupported Segwit (Bech32) or Base58 encoding.",  # tchn1, Invalid checksum (Bech32m instead of Bech32)
        [],
    ),
    (
        INVALID_CHARACTER,
        "Invalid Base 32 character",  # Invalid character in checksum
        [len(INVALID_CHARACTER) - 3],
    ),
    (
        "LY130XLXVLHEMJA6C4DQV22UAPCTQUPFHLXM9H8Z3K2E72Q4K9HCZ7VQ2T943P",
        "Invalid Bech32 address witness version",
        [],
    ),
    ("ly1pw5c67j4y", "Invalid Bech32 address program size (1 byte)", []),
    (
        "ly1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7v8n0nx0muaewav25ap49k8",
        "Invalid Bech32 address program size (41 bytes)",
        [],
    ),
    (
        "LY1QR508D6QEJXTDG4Y5R3ZARVARYVS2PF87",
        "Invalid Bech32 v0 address program size (16 bytes), per BIP141",
        [],
    ),
    (
        "tchn1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vq47Zagq",
        "Invalid or unsupported Segwit (Bech32) or Base58 encoding.",  # tchn1, Mixed case
        [],
    ),
    (
        "ly1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7v07q4nhw39",
        "Invalid padding in Bech32 data section",  # zero padding of more than 4 bits
        [],
    ),
    (
        "tly1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vp2se9gs",
        "Invalid or unsupported Segwit (Bech32) or Base58 encoding.",  # tchn1, Non-zero padding in 8-to-5 conversion
        [],
    ),
    ("ly1cfnn9j", "Empty Bech32 data section", []),
]
VALID_DATA = [
    # BIP 350
    (
        "LY1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7K8HN3AL",
        "0014751e76e8199196d454941c45d1b3a323f1433bd6",
    ),
    # (
    #   "tly1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3qdgsetu",
    #   "00201863143c14c5166804bd19203356da136c985678cd4d27a1b8c6329604903262",
    # ),
    (
        "ly1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3qv3z76n",
        "00201863143c14c5166804bd19203356da136c985678cd4d27a1b8c6329604903262",
    ),
    (
        "ly1pw508d6qejxtdg4y5r3zarvary0c5xw7kw508d6qejxtdg4y5r3zarvary0c5xw7k6rz735",
        "5128751e76e8199196d454941c45d1b3a323f1433bd6751e76e8199196d454941c45d1b3a323f1433bd6",
    ),
    ("LY1SW50Q00QWGF", "6002751e"),
    ("ly1zw508d6qejxtdg4y5r3zarvaryvgt0660", "5210751e76e8199196d454941c45d1b3a323"),
    # (
    #   "tly1qqqqqp399et2xygdj5xreqhjjvcmzhxw4aywxecjdzew6hylgvsesp7ch2x",
    #   "0020000000c4a5cad46221b2a187905e5266362b99d5e91c6ce24d165dab93e86433",
    # ),
    (
        "ly1qqqqqp399et2xygdj5xreqhjjvcmzhxw4aywxecjdzew6hylgvsesq82smf",
        "0020000000c4a5cad46221b2a187905e5266362b99d5e91c6ce24d165dab93e86433",
    ),
    # (
    #   "tly1pqqqqp399et2xygdj5xreqhjjvcmzhxw4aywxecjdzew6hylgvsestfc7j6",
    #   "5120000000c4a5cad46221b2a187905e5266362b99d5e91c6ce24d165dab93e86433",
    # ),
    (
        "ly1pqqqqp399et2xygdj5xreqhjjvcmzhxw4aywxecjdzew6hylgvses2s2er4",
        "5120000000c4a5cad46221b2a187905e5266362b99d5e91c6ce24d165dab93e86433",
    ),
    (
        "ly1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqkllhyd",
        "512079be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798",
    ),
    # PayToAnchor(P2A)
    (
        "ly1pfeeshpl25j",
        "51024e73",
    ),
]


class ValidateAddressMainTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.chain = ""  # main
        self.num_nodes = 1
        self.extra_args = [["-prune=24000"]] * self.num_nodes  # (the least a sidechain takes, MIN_SIDECHAIN_PRUNE_TARGET)

    def setup_nodes(self):
        # Until its slot's activation height (SidechainParams::main_activation_height) is set, this
        # release refuses to start on the main network: there is no main node to ask until then.
        try:
            super().setup_nodes()
        except FailedToStartError as e:
            if "it must not run on the main network" not in str(e):
                raise
            for node in self.nodes:  # it exited: nothing to stop
                node.process.wait()
                node.running = False
                node.process = None
            raise SkipTest("this release does not run on the main network until its slot's activation height is set")

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
