#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Layers' DEX pool kinds (doc/layers.md, "Pool kinds"), from -testactivationheight=dexcurves@<height>:

  - before their height (the DEX in force): createpool and the other pool-kind commands are refused, and the
    rules refuse their markers;
  - previewpool, then createpool of a stable pool (A 200, 0.05 %), a weighted one (80/20, 0.3 %) and a
    constant-product one at 1 % beside the pool addliquidity makes: four pools of one pair or more, by id;
  - a swap through the stable pool alone gives exactly getswapquote's quote, which the reference
    (contrib/dex-curves/reference.py) computes the same;
  - a batch of opposing orders through the stable pool in one block: each order what the reference's
    uniform-price clearing of both gives;
  - a route across kinds (constant product at 1 %, then weighted) gives quoteroute's quote;
  - liquidity of one side in and out (addpoolliquidity, removepoolliquidity) as the wallet quoted;
  - the TWAP of a stable pool at its curve's price; a reorganisation across a clearing takes the pools and
    their accumulators back, and forward again;
  - the peg balance throughout.
"""

import os
import sys
import time
from decimal import Decimal

from feature_layers import LayersTest
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error

sys.path.insert(0, os.path.join(os.path.dirname(os.path.realpath(__file__)), "..", "..", "contrib", "dex-curves"))
import reference  # noqa: E402

DEX_HEIGHT = 10
CURVES_HEIGHT = 20


def units(amount, decimals):
    return int(Decimal(str(amount)) * 10**decimals)



def taken(node, amount, fee_millionths):
    """The security and protocol parts of the swap fee on `amount` (units) at `fee_millionths`: what leaves the pool (doc/layers.md "Fees of the DEX")."""
    parts = node.getdexinfo()["swap_fee_parts"]
    fee = amount * fee_millionths // 1000000
    out = 0
    for key in ("security", "protocol"):
        share = int(Decimal(str(parts[key])) * 10000)
        out += fee * share // 1000000
    return out

class LayersDexCurvesTest(LayersTest):
    def set_test_params(self):
        super().set_test_params()
        # The rules with the treasury (before no_treasury_height): its parts checked here; feature_layers_v4 checks those after.
        self.side_args = self.side_args + [f"-testactivationheight=dex@{DEX_HEIGHT}", f"-testactivationheight=dexcurves@{CURVES_HEIGHT}", "-testactivationheight=notreasury@1000000"]
        self.extra_args = [self.side_args] * self.num_nodes

    def run_test(self):
        super().run_test()

    def bmm_to(self, height):
        while self.nodes[0].getblockcount() < height:
            self.bmm()

    def held(self, node, label, decimals=2):
        return units(self.holding(node, label), decimals)

    def kind(self, pool):
        """A pool's kind as the reference takes it (getpool's fields; the scales from the decimals)."""
        curve = {"constant_product": 0, "stable": 1, "weighted": 2}[pool["curve"]]
        return {"curve": curve, "fee": pool["fee_millionths"], "amp": pool.get("amp", 0), "weight0": pool.get("weight_a", 0) if curve == 2 else 0,
                "scale0": 0, "scale1": 0}

    def raw(self, node, pool_id):
        """A pool in units and the order of its assets (asset0 first: getpool by id shows them so, CHN last)."""
        pool = node.getpool(pool_id)
        a, b = pool["asset_a_id"], pool["asset_b_id"]
        da = 8 if int(a, 16) == 0 else node.getasset("0x" + a)["decimals"]
        db = 8 if int(b, 16) == 0 else node.getasset("0x" + b)["decimals"]
        ra, rb = units(pool["reserve_a"], da), units(pool["reserve_b"], db)
        # The reference's asset0 is the smaller id (CHN, the null hash, first).
        if int(a, 16) < int(b, 16):
            return pool, (ra, rb), True
        return pool, (rb, ra), False

    def run_sidechain_test(self):
        main = self.main
        n0, n1, n2 = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = n0.getnewaddress()
        from feature_sidechain import ACTIVATION_PERIOD
        from test_framework.fastblocks import SLOT, assert_peg_balance

        self.log.info("Setup: the slot, deposits, five assets (BMM blocks)")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "Layers", "Assets on a fast chain", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 60)
        main.createsidechaindeposit(SLOT, n1.getdepositaddress()["depositaddress"], 20)
        main.createsidechaindeposit(SLOT, n2.getdepositaddress()["depositaddress"], 20)
        self.mine_main()
        self.bmm()
        for n in self.nodes:
            n.sendmany("", {n.getnewaddress(): 1 for _ in range(10)})
        self.bmm()
        names = ["GOLD", "SILVER", "USDX", "USDY"]
        for name in names:
            n0.reserveasset(name)
        self.bmm_to(n0.getblockcount() + 3)
        for name in names:
            n0.registerasset(name, 10000000, 2)
        self.bmm()
        for n in (n1, n2):
            for name in names:
                n0.sendasset(n.getnewaddress(), name, 100000)
            self.bmm()
        self.bmm_to(DEX_HEIGHT)
        info = n0.getdexinfo()
        assert info["active"]
        assert not info["curves_active"]
        assert_equal(info["curves_height"], CURVES_HEIGHT)
        assert_equal(info["fee_tiers"], [Decimal("0.01"), Decimal("0.05"), Decimal("0.3"), Decimal("1")])

        self.log.info("Before the pool kinds' height: refused")
        assert_greater_than(CURVES_HEIGHT - 1, n0.getblockcount() + 1)
        assert_raises_rpc_error(-1, "do not apply yet", n0.createpool, "USDX", 1000, "USDY", 1000, {"curve": "stable"})
        n0.addliquidity("GOLD", 10000, "SILVER", 10000)
        self.bmm_to(CURVES_HEIGHT - 1)
        assert n0.getdexinfo()["curves_active"]

        self.log.info("Pools of every kind: previewed, then made; the default kind is the pool addliquidity made")
        preview = n0.previewpool("USDX", 50000, "USDY", 50000, {"curve": "stable", "fee": 0.05, "amp": 200})
        assert not preview["exists"]
        assert_equal(preview["curve"], "stable")
        assert_equal(preview["shares"], 10000000 - 1000)
        assert_equal(len(preview["depth"]), 12)
        made = n0.createpool("USDX", 50000, "USDY", 50000, {"curve": "stable", "fee": 0.05, "amp": 200})
        assert_equal(made["pool"], preview["pool"])
        assert_equal(made["shares"], preview["shares"])
        stable = made["pool"]
        self.bmm()
        weighted = n0.createpool("GOLD", 8000, "CHN", 2, {"curve": "weighted", "weight_a": 80})["pool"]
        self.bmm()
        cp1 = n0.createpool("GOLD", 10000, "SILVER", 10000, {"fee": 1})["pool"]
        self.bmm()
        assert_raises_rpc_error(-8, "exists", n0.createpool, "USDY", 1000, "USDX", 1000, {"curve": "stable", "fee": 0.05, "amp": 200})
        legacy = n0.getpool("GOLD", "SILVER")
        assert legacy["default_kind"]
        assert_equal(legacy["fee"], Decimal("0.3"))
        assert legacy["pool"] not in (stable, weighted, cp1)
        assert_equal(n0.getpool(stable)["amp"], 200)
        w = n0.getpool(weighted)
        assert_equal((w["curve"], w["weight_a"] + w["weight_b"]), ("weighted", 100))
        # At 8000 GOLD (80 %) and 2 CHN (20 %): 1 GOLD = 0.001 CHN.
        assert_equal(Decimal(str(w["price"])).quantize(Decimal("1e-9")), Decimal("0.001"))
        assert_equal(len(n0.listpools("GOLD")), 3)
        assert_peg_balance(main, self.nodes)

        self.log.info("A swap through the stable pool alone: getswapquote's quote, the reference's curve")
        quote = n1.getswapquote("USDX", 1000, "USDY")
        assert_equal(quote["best_pool"], stable)
        q = quote["pools"][0]
        pool, (r0, r1), x_first = self.raw(n1, stable)
        k = self.kind(pool)
        usdx0 = int(n1.getasset("USDX")["asset"], 16) < int(n1.getasset("USDY")["asset"], 16)
        expect = reference.clear(k, k["fee"], r0, r1, [(0 if usdx0 else 1, 100000)])[0][0]
        assert_equal(units(q["amount_out"], 2), expect)
        before = self.held(n1, "USDY")
        n1.swappools("USDX", 1000, [stable])
        self.bmm()
        assert_equal(self.held(n1, "USDY") - before, expect)
        # Near balance, a stable pool trades near one for one: less the 0.05 % fee, very little impact.
        assert_greater_than(expect, 99900)

        self.log.info("A batch through the stable pool: opposing orders, one block, the reference's uniform price")
        pool, (r0, r1), _ = self.raw(n0, stable)
        y1, x2 = self.held(n1, "USDY"), self.held(n2, "USDX")
        n1.swappools("USDX", 3000, [stable], 5)
        n2.swappools("USDY", 1000, [stable], 5)
        batch_block = self.bmm()
        assert_equal(len(n0.getblock(batch_block)["tx"]), 3)
        orders = [(0 if usdx0 else 1, 300000), (1 if usdx0 else 0, 100000)]
        outs, e0, e1 = reference.clear(self.kind(pool), pool["fee_millionths"], r0, r1, orders)
        assert_equal(self.held(n1, "USDY") - y1, outs[0])
        assert_equal(self.held(n2, "USDX") - x2, outs[1])
        _, (a0, a1), _ = self.raw(n0, stable)
        # The round's reserves less the security and protocol parts of each side's fee (which left the pool).
        fee = pool["fee_millionths"]
        in0, in1 = (300000, 100000) if usdx0 else (100000, 300000)
        assert_equal((a0, a1), (e0 - taken(n0, in0, fee), e1 - taken(n0, in1, fee)))
        simulated = n0.simulateclearing(stable, [{"asset": "USDX", "amount": 3000}, {"asset": "USDY", "amount": 1000}])
        assert_equal(len(simulated["outs"]), 2)

        self.log.info("A route across kinds: SILVER to GOLD at 1 %, then GOLD to CHN on the weighted pool")
        route = n2.quoteroute("SILVER", 50, "CHN", 2)
        best = route["routes"][0]
        assert not best["default_pools"]
        assert_equal(best["pools"][-1], weighted)
        assert_equal([h["curve"] for h in best["hops"]][-1], "weighted")
        before = n2.getbalance()
        n2.swaproute(best["path"], 50, 0.5, None, best["pools"])
        block = self.bmm()
        assert self.paid_in(n2, block, best["amount_out"]), "the route's CHN by the coinbase, as quoted"
        assert_greater_than(n2.getbalance() + Decimal("0.001"), before)

        self.log.info("Liquidity of one side, in and out, as the wallet quoted")
        added = n2.addpoolliquidity(stable, {"USDX": 500})
        self.bmm()
        assert_greater_than(added["shares"], 0)
        x_before = self.held(n2, "USDX")
        removed = n2.removepoolliquidity(stable, 100, "USDX")
        self.bmm()
        got = self.held(n2, "USDX") - x_before
        assert_equal(got, units(removed["amounts"]["USDX"], 2))
        # Less than went in: the fees of both halves stay with the pool.
        assert_greater_than(50000, got)
        assert_greater_than(got, 49800)

        self.log.info("TWAP at the curve's price, and a reorganisation across a clearing")
        time.sleep(2)
        self.bmm()
        twap = n0.getpooltwap(stable, None, 1)
        assert abs(twap["twap_a"] / twap["spot_a"] - 1) < Decimal("1e-9"), twap
        pools_before = [n0.getpool(p) for p in (stable, weighted, cp1)]
        twap_before = n0.getpooltwap(stable, CURVES_HEIGHT)
        n1.swappools("USDY", 2000, [stable])
        time.sleep(1)
        swap_block = self.bmm()
        time.sleep(1)
        self.bmm()
        tip = n0.getbestblockhash()
        assert n0.getpool(stable) != pools_before[0]
        for n in self.nodes:
            n.invalidateblock(swap_block)
        assert_equal([n0.getpool(p) for p in (stable, weighted, cp1)], pools_before)
        assert_equal(n0.getpooltwap(stable, CURVES_HEIGHT), twap_before)
        for n in self.nodes:
            n.reconsiderblock(swap_block)
        self.wait_until(lambda: all(n.getbestblockhash() == tip for n in self.nodes))
        assert n0.getpool(stable) != pools_before[0]
        assert_peg_balance(main, self.nodes)


if __name__ == "__main__":
    LayersDexCurvesTest(__file__).main()
