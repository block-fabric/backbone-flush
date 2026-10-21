#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Layers' concentrated liquidity and dynamic fees (doc/layers.md, "Concentrated liquidity", "Dynamic fees"):

  - createpool of the concentrated curve: made at a price, nothing in;
  - positions in range and out of it (openposition): their tokens in the wallet (listpositions), what they hold;
  - orders through the pool in the block's batch, crossing ticks; each what getswapquote said alone;
  - fees to the positions in range only; collected (changeposition); a position closed;
  - a reorganisation across a clearing that crossed ticks takes the pool's state back, and forward again;
  - a pool with a dynamic fee: its fee rises after volatile blocks (getpool's fee_now);
  - the peg balance throughout.
"""

from decimal import Decimal

from feature_layers import LayersTest
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error

DEX_HEIGHT = 10
CURVES_HEIGHT = 15


class LayersClTest(LayersTest):
    def set_test_params(self):
        super().set_test_params()
        self.side_args = self.side_args + [f"-testactivationheight=dex@{DEX_HEIGHT}", f"-testactivationheight=dexcurves@{CURVES_HEIGHT}"]
        self.extra_args = [self.side_args] * self.num_nodes

    def run_test(self):
        super().run_test()

    def bmm_to(self, height):
        while self.nodes[0].getblockcount() < height:
            self.bmm()

    def run_sidechain_test(self):
        main = self.main
        n0, n1, n2 = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = n0.getnewaddress()
        from feature_sidechain import ACTIVATION_PERIOD
        from test_framework.fastblocks import SLOT, assert_peg_balance

        self.log.info("Setup: the slot, deposits, two assets (BMM blocks)")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "Layers", "Assets on a fast chain", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        for n, amount in ((n0, 60), (n1, 20), (n2, 20)):
            main.createsidechaindeposit(SLOT, n.getdepositaddress()["depositaddress"], amount)
        self.mine_main()
        self.bmm()
        for n in self.nodes:
            n.sendmany("", {n.getnewaddress(): 1 for _ in range(10)})
        self.bmm()
        for name in ("USDX", "USDY"):
            n0.reserveasset(name)
        self.bmm_to(n0.getblockcount() + 3)
        for name in ("USDX", "USDY"):
            n0.registerasset(name, 10000000, 2)
        self.bmm()
        for n in (n1, n2):
            for name in ("USDX", "USDY"):
                n0.sendasset(n.getnewaddress(), name, 200000)
            self.bmm()
        self.bmm_to(CURVES_HEIGHT)
        x, y = n0.getasset("USDX")["asset"], n0.getasset("USDY")["asset"]
        x_first = int(x, 16) < int(y, 16)

        self.log.info("A concentrated pool at 1:1, 0.05 % (tick spacing 10); positions in and out of range")
        pool = n0.createpool("0x" + x, 1, "0x" + y, 1, {"curve": "concentrated", "fee": 0.05})["pool"]
        self.bmm()
        info = n0.getpool(pool)
        assert_equal(info["curve"], "concentrated")
        assert_equal(info["concentrated"]["tick"], 0)
        assert_equal(info["concentrated"]["tick_spacing"], 10)
        amounts = {"0x" + x: 50000, "0x" + y: 50000}
        preview = n0.previewposition(pool, Decimal("0.98"), Decimal("1.02"), amounts)
        assert preview["in_range"]
        near = n0.openposition(pool, Decimal("0.98"), Decimal("1.02"), amounts)
        self.bmm()
        wide = n0.openposition(pool, Decimal("0.5"), Decimal("2"), amounts)
        self.bmm()
        above = n0.openposition(pool, Decimal("1.5"), Decimal("1.8"), amounts)
        self.bmm()
        mine = {p["position"]: p for p in n0.listpositions()}
        assert_equal(set(mine), {near["position"], wide["position"], above["position"]})
        assert mine[near["position"]]["in_range"]
        assert not mine[above["position"]]["in_range"]
        # Out of range above: asset0 alone went in.
        held = mine[above["position"]]
        assert (held["amount0"] > 0) and held["amount1"] == 0
        ticks = n0.getpoolticks(pool)["ticks"]
        assert_equal(len(ticks), 6)
        assert_peg_balance(main, self.nodes)

        self.log.info("Orders that cross ticks, each what getswapquote said alone")
        for i in range(6):
            sell, buy = ("USDX", "USDY") if i % 2 == 0 else ("USDY", "USDX")
            quote = n1.getswapquote(sell, 3000, buy)
            q = [p for p in quote["pools"] if p["pool"] == pool][0]
            before = self.holding(n1, buy)
            n1.swappools(sell, 3000, [pool])
            self.bmm()
            assert_equal(self.holding(n1, buy) - before, q["amount_out"])
        # A big one past the near range, into the wide one alone.
        tick_before = n0.getpool(pool)["concentrated"]["tick"]
        n2.swappools("USDX", 150000, [pool])
        cross_block = self.bmm()
        tick_after = n0.getpool(pool)["concentrated"]["tick"]
        assert abs(tick_after - tick_before) > 200, (tick_before, tick_after)

        self.log.info("Fees to the positions in range only; collected")
        fees = {p["position"]: p for p in n0.listpositions()}
        assert fees[near["position"]]["fees0"] + fees[near["position"]]["fees1"] > 0
        assert_equal(fees[above["position"]]["fees0"] + fees[above["position"]]["fees1"], 0)
        x0, y0 = self.holding(n0, "USDX"), self.holding(n0, "USDY")
        n0.changeposition(near["position"], {"collect": True})
        self.bmm()
        self.bmm()
        assert self.holding(n0, "USDX") + self.holding(n0, "USDY") > x0 + y0
        assert_equal(n0.getposition(near["position"])["fees0"], 0)

        self.log.info("A reorganisation across a clearing that crossed ticks")
        tip = n0.getbestblockhash()
        state_after = n0.getpool(pool)
        for n in self.nodes:
            n.invalidateblock(cross_block)
        assert_equal(n0.getpool(pool)["concentrated"]["tick"], tick_before)
        for n in self.nodes:
            n.reconsiderblock(cross_block)
        self.wait_until(lambda: all(n.getbestblockhash() == tip for n in self.nodes))
        assert_equal(n0.getpool(pool), state_after)

        self.log.info("A position closed: all its liquidity out, all it is owed collected, its token gone")
        before = self.holding(n0, "USDX") + self.holding(n0, "USDY")
        n0.changeposition(wide["position"], {"close": True})
        self.bmm()
        assert_raises_rpc_error(-8, "No such position", n0.getposition, wide["position"])
        assert self.holding(n0, "USDX") + self.holding(n0, "USDY") > before
        assert_peg_balance(main, self.nodes)

        self.log.info("A dynamic fee rises after volatile blocks")
        dyn = n0.createpool("0x" + x, 100000, "0x" + y, 100000, {"curve": "constant_product", "fee": 0.3, "dynamic_fee": True})["pool"]
        self.bmm()
        assert n0.getpool(dyn)["dynamic_fee"]
        assert_equal(n0.getpool(dyn)["fee_now"], Decimal("0.3"))
        for i in range(6):
            n1.swappools("USDX" if i % 2 == 0 else "USDY", 20000, [dyn])
            self.bmm()
        assert_greater_than(n0.getpool(dyn)["fee_now"], Decimal("0.3"))
        assert_peg_balance(main, self.nodes)


if __name__ == "__main__":
    LayersClTest(__file__).main()
