#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Upgrade v10 from v10_height H (doc/layers.md "Upgrade v10"; owner decisions 2026-11-11), and a node without it.

The extra markets retired:
  - before H an order, an auction, a stable pool, a concentrated pool with a ranged and a full-range position are made;
  - from H none of them starts: the wallet says why, and the node refuses the transaction itself (bad-ba-retired-order,
    -auction, -pool-kind, -range), in its mempool and in a block (a node without the rule makes one: refused);
  - what closes those there are stays: an order cancelled and one traded through, an auction bid on and collected, a
    stable pool traded through, added to and left, positions reduced and closed, a full-range position added to;
  - the standard pool (constant product, at a fee tier) and a full-range position are still made;
  - a stable pool everyone left is not opened again;
  - a node without the rule follows the chain (the rule only refuses), until it takes what the others refuse;
  - the peg balance holds throughout.
"""

from decimal import Decimal

from feature_layers import LayersTest
from test_framework.fastblocks import SLOT, assert_peg_balance
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error

DEX_HEIGHT = 10
CURVES_HEIGHT = 15
H = 60


class LayersV10Test(LayersTest):
    def set_test_params(self):
        super().set_test_params()
        self.side_args = self.side_args + [f"-testactivationheight=dex@{DEX_HEIGHT}", f"-testactivationheight=dexcurves@{CURVES_HEIGHT}", f"-testactivationheight=feeflow@{CURVES_HEIGHT}",
                                           f"-testactivationheight=v7@{CURVES_HEIGHT}", f"-testactivationheight=v8@{CURVES_HEIGHT}", f"-testactivationheight=v9@{CURVES_HEIGHT}",
                                           f"-testactivationheight=v10@{H}"]
        self.extra_args = [self.side_args] * self.num_nodes

    def run_test(self):
        super().run_test()

    def bmm_on(self, nodes):
        """A BMM block made by the first of `nodes`, which all of them take."""
        self.sync_mempools(nodes)
        block = nodes[0].createbmmblock(self.side_address)
        self.main.createbmmrequest(SLOT, block["hash"], Decimal("0.001"))
        self.mine_main()
        self.wait_until(lambda: all(n.getbestblockhash() == block["hash"] for n in nodes))
        return block["hash"]

    def bmm_to(self, height, nodes=None):
        while self.nodes[0].getblockcount() < height:
            self.bmm_on(nodes or self.nodes)

    def run_sidechain_test(self):
        main = self.main
        n0, n1, n2 = self.nodes
        new = [n0, n1]
        self.main_address = main.getnewaddress()
        self.side_address = n0.getnewaddress()
        from feature_sidechain import ACTIVATION_PERIOD

        self.log.info("Setup: the slot, deposits, two assets (BMM blocks); the third node without v10")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "Layers", "Assets on a fast chain", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        for n, amount in ((n0, 700), (n1, 50), (n2, 50)):
            main.createsidechaindeposit(SLOT, n.getdepositaddress()["depositaddress"], amount)
        self.mine_main()
        self.bmm()
        for n in self.nodes:
            n.sendmany("", {n.getnewaddress(): 1 for _ in range(20)})
        self.bmm()
        for name in ("USDX", "USDY"):
            n0.reserveasset(name)
        self.bmm_to(n0.getblockcount() + 3)
        for name in ("USDX", "USDY"):
            n0.registerasset(name, 10000000, 2)
        self.bmm()
        # A wallet spends a coin of an asset a block: several coins each for the node that makes several at once.
        for n in (n1, n2, n2, n2, n2):
            for name in ("USDX", "USDY"):
                n0.sendasset(n.getnewaddress(), name, 200000)
            self.bmm()
        self.bmm_to(CURVES_HEIGHT)
        self.restart_node(2, extra_args=[a for a in self.extra_args[2] if not a.startswith("-testactivationheight=v10")])
        self.connect_nodes(2, 0)
        self.connect_nodes(2, 1)
        self.sync_blocks()
        info = n0.getdexinfo()
        assert_equal((info["v10_height"], info["v10_active"]), (H, False))
        assert_equal((n2.getdexinfo()["v10_height"], n2.getdexinfo()["v10_active"]), (-1, False))
        x, y = n0.getasset("USDX")["asset"], n0.getasset("USDY")["asset"]
        amounts = {"0x" + x: 50000, "0x" + y: 50000}

        self.log.info("Before H: an order, an auction, a stable pool, a concentrated pool with a ranged and a full-range position")
        order = n1.placeorder("USDX", 1000, "USDY", 2)
        n0.createauction("USDX", 100, "USDY", 100, 100, 5000)
        self.bmm()
        resting = n1.placeorder("USDX", 1000, "USDY", 1)
        stable = n0.createpool("USDX", 50000, "USDY", 50000, {"curve": "stable", "fee": 0.05, "amp": 200})["pool"]
        self.bmm()
        auction = n0.listauctions()[0]["auction"]
        concentrated = n0.createpool("0x" + x, 1, "0x" + y, 1, {"curve": "concentrated", "fee": 0.05})["pool"]
        self.bmm()
        ranged = n0.openposition(concentrated, Decimal("0.98"), Decimal("1.02"), amounts)
        self.bmm()
        full = n0.openposition(concentrated, 0, 0, amounts)
        self.bmm()
        assert_greater_than(H - 1, n0.getblockcount() + 1)
        self.bmm_to(H - 1)
        assert n0.getdexinfo()["v10_active"]
        assert_peg_balance(main, self.nodes)

        self.log.info("From H none of them starts: the wallet says why")
        retired = "retired by upgrade v10"
        assert_raises_rpc_error(-1, retired, n1.placeorder, "USDX", 1000, "USDY", 2)
        assert_raises_rpc_error(-1, retired, n0.createauction, "USDX", 100, "USDY", 100, 100, 50)
        assert_raises_rpc_error(-1, retired, n0.createpool, "USDX", 50000, "USDY", 50000, {"curve": "stable", "fee": 0.05, "amp": 100})
        assert_raises_rpc_error(-1, retired, n0.createpool, "USDX", 50000, "USDY", 50000, {"curve": "weighted", "weight_a": 80})
        assert_raises_rpc_error(-1, retired, n0.createpool, "0x" + x, 1, "0x" + y, 1, {"curve": "concentrated", "fee": 0.3})
        assert_raises_rpc_error(-1, retired, n0.createpool, "USDX", 50000, "USDY", 50000, {"fee": 1, "dynamic_fee": True})
        assert_raises_rpc_error(-1, retired, n0.openposition, concentrated, Decimal("0.98"), Decimal("1.02"), amounts)
        assert_raises_rpc_error(-1, retired, n0.openposition, concentrated, 0, Decimal("1.02"), amounts)
        assert_raises_rpc_error(-1, retired, n0.changeposition, ranged["position"], {"add": amounts})

        self.log.info("And the node refuses the transaction itself: one a node without the rule made")
        refused = {}
        refused["bad-ba-retired-order"] = n2.placeorder("USDX", 1000, "USDY", 2)["txid"]
        refused["bad-ba-retired-auction"] = n2.createauction("USDX", 100, "USDY", 100, 100, 50)["txid"]
        refused["bad-ba-retired-pool-kind"] = n2.createpool("USDX", 50000, "USDY", 50000, {"curve": "stable", "fee": 0.05, "amp": 100})["txid"]
        refused["bad-ba-retired-range"] = n2.openposition(concentrated, Decimal("0.9"), Decimal("1.1"), {"0x" + x: 1000, "0x" + y: 1000})["txid"]
        for reason, txid in refused.items():
            raw = n2.gettransaction(txid)["hex"]
            assert_equal(n0.testmempoolaccept([raw])[0]["reject-reason"], reason)
            assert txid not in n0.getrawmempool()
        # In a block: the node without the rule makes one with them, and the others refuse it.
        tip = n0.getbestblockhash()
        bad = n2.createbmmblock(n2.getnewaddress())
        main.createbmmrequest(SLOT, bad["hash"], Decimal("0.001"))
        self.mine_main()
        self.wait_until(lambda: n2.getbestblockhash() == bad["hash"])
        self.wait_until(lambda: all(any(t["hash"] == bad["hash"] and t["status"] == "invalid" for t in n.getchaintips()) for n in new), timeout=60)
        assert_equal(n0.getbestblockhash(), tip)
        assert_equal(n1.getbestblockhash(), tip)

        self.log.info("The standard pool at a fee tier, the pool addliquidity makes and a full-range position are made as before")
        standard = n0.createpool("USDX", 10000, "USDY", 10000, {"fee": 1})["pool"]
        n1.addliquidity("USDX", 10000, "USDY", 10000)
        self.bmm_on(new)
        full2 = n0.openposition(concentrated, 0, 0, amounts)
        self.bmm_on(new)
        assert_equal(n0.getpool(standard)["curve"], "constant_product")
        assert n0.getpool("USDX", "USDY")["default_kind"]
        assert full2["position"] in {p["position"] for p in n0.listpositions()}

        self.log.info("What closes the markets of before stays: positions")
        liquidity = n0.getposition(full["position"])["liquidity"]
        n0.changeposition(full["position"], {"add": amounts})
        self.bmm_on(new)
        assert_greater_than(int(n0.getposition(full["position"])["liquidity"]), int(liquidity))
        n0.changeposition(ranged["position"], {"remove": 50})
        self.bmm_on(new)
        n0.changeposition(ranged["position"], {"close": True})
        self.bmm_on(new)
        assert_raises_rpc_error(-8, "No such position", n0.getposition, ranged["position"])

        self.log.info("Orders: one cancelled, one traded through (a swap through the book), then collected")
        before = self.holding(n1, "USDX")
        n1.settleorder(order["order"], True)
        self.bmm_on(new)
        assert_equal(self.holding(n1, "USDX") - before, Decimal(1000))
        before = self.holding(n0, "USDX")
        n0.swappools("USDY", 400, [resting["book"]])
        self.bmm_on(new)
        assert_equal(self.holding(n0, "USDX") - before, Decimal(400))
        before = self.holding(n1, "USDY")
        n1.settleorder(resting["order"])
        self.bmm_on(new)
        assert_equal(self.holding(n1, "USDY") - before, Decimal(400))

        self.log.info("Auctions: a bid buys, the receipt collects")
        before = self.holding(n1, "USDX")
        n1.bidauction(auction, 100)
        self.bmm_on(new)
        assert_equal(self.holding(n1, "USDX") - before, Decimal(100))
        before = self.holding(n0, "USDY")
        n0.collectauction(auction)
        self.bmm_on(new)
        assert_equal(self.holding(n0, "USDY") - before, Decimal(100))

        self.log.info("A stable pool of before: traded through, added to, left; once everyone left, not opened again")
        before = self.holding(n1, "USDY")
        n1.swappools("USDX", 100, [stable])
        self.bmm_on(new)
        assert_greater_than(self.holding(n1, "USDY"), before)
        n1.addpoolliquidity(stable, {"USDX": 500})
        self.bmm_on(new)
        n1.removepoolliquidity(stable)
        n0.removepoolliquidity(stable)
        self.bmm_on(new)
        assert n0.getpool(stable)["abandoned"]
        assert_raises_rpc_error(-1, retired, n0.addpoolliquidity, stable, {"USDX": 50000, "USDY": 50000})
        assert_raises_rpc_error(-1, retired, n0.createpool, "USDX", 50000, "USDY", 50000, {"curve": "stable", "fee": 0.05, "amp": 200})
        assert_peg_balance(main, new)


if __name__ == "__main__":
    LayersV10Test(__file__).main()
