#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The logos of assets on Layers, with the rules of its DEX in force (dex_height 0): the whole of feature_bitassets_logo.py
(before logo_height, at the registration, changed, refused, a reorganization, served without the blocks), then a route of
the DEX beside them, which the asset transactions of the wallet list."""

from feature_bitassets_logo import LOGO_HEIGHT, BitAssetsLogoTest
from test_framework.util import assert_equal


class LayersLogoTest(BitAssetsLogoTest):
    def set_test_params(self):
        super().set_test_params()
        self.extra_args = [args + ["-testactivationheight=dex@0"] for args in self.extra_args]

    def run_test(self):
        super().run_test()

    def run_sidechain_test(self):
        super().run_sidechain_test()
        side = self.nodes[0]
        info = side.getbitassetsinfo()
        assert_equal((info["logo_height"], side.getdexinfo()["dex_height"]), (LOGO_HEIGHT, 0))
        # The logos are untouched by the DEX's tables, and both nodes have one state.
        assert_equal(side.getasset("BADGE")["logo"]["mime"], "image/png")
        self.check_in_sync()
        kinds = {e["category"] for e in side.listassettransactions()}
        assert {"register", "logo"} <= kinds, kinds


if __name__ == "__main__":
    LayersLogoTest(__file__).main()
