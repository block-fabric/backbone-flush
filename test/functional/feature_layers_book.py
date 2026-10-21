#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Layers' order books (doc/layers.md, "Order books"):

  - placeorder: limit orders resting in the book of two assets (getbook's ladder, listorders);
  - orders that cross trade at the block's end at one price, never past either limit;
  - a taker through the book (swappools), what getswapquote said; one larger than the book: refused by the wallet;
  - a route through a pool and the book;
  - settleorder: collecting, cancelling; an expired order back to its holder;
  - a reorganisation across a crossing takes the book back, and forward again; the peg balance throughout;
  - verified issuers: a curator verifies an asset (verifyasset) and takes it back; getverifications.
"""

from decimal import Decimal

from feature_layers import LayersTest
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error

DEX_HEIGHT = 10
CURVES_HEIGHT = 15


class LayersBookTest(LayersTest):
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
        for name in ("GOLD", "USDX"):
            n0.reserveasset(name)
        self.bmm_to(n0.getblockcount() + 3)
        for name in ("GOLD", "USDX"):
            n0.registerasset(name, 10000000, 2)
        self.bmm()
        for n in (n1, n2):
            for name in ("GOLD", "USDX"):
                n0.sendasset(n.getnewaddress(), name, 100000)
            self.bmm()
        self.bmm_to(CURVES_HEIGHT)

        self.log.info("Asks of GOLD in USDX; a bid that crosses one of them")
        a2 = n1.placeorder("GOLD", 1000, "USDX", 2)
        self.bmm()
        a3 = n1.placeorder("GOLD", 1000, "USDX", 3)
        self.bmm()
        book = n0.getbook("GOLD", "USDX")
        assert_equal(book["book"], a2["book"])
        assert_equal(book["open_orders"], 2)
        assert_equal(book["best_ask"], Decimal("2"))
        assert_equal([l["price"] for l in book["asks"]], [Decimal("2"), Decimal("3")])
        assert_equal({o["order"] for o in n1.listorders()}, {a2["order"], a3["order"]})
        # Bids sell USDX for GOLD at least 0.4 GOLD per USDX: up to 2.5 USDX per GOLD.
        bid = n2.placeorder("USDX", 3000, "GOLD", Decimal("0.4"))
        cross_block = self.bmm()
        ask = n0.getorder(a2["order"])
        assert_equal(ask["remaining"], 0)
        assert Decimal("2000") <= ask["got"] <= Decimal("2500"), ask
        got = n0.getorder(bid["order"])
        assert_equal(got["got"], Decimal("1000"))
        assert_equal(n0.getorder(a3["order"])["remaining"], Decimal("1000"))
        assert_peg_balance(main, self.nodes)

        self.log.info("A reorganisation across the crossing, and forward again")
        tip = n0.getbestblockhash()
        after = n0.getbook("GOLD", "USDX")
        for n in self.nodes:
            n.invalidateblock(cross_block)
        assert_equal(n0.getorder(a2["order"])["remaining"], Decimal("1000"))
        assert_raises_rpc_error(-8, "No such order", n0.getorder, bid["order"])
        for n in self.nodes:
            n.reconsiderblock(cross_block)
        self.wait_until(lambda: all(n.getbestblockhash() == tip for n in self.nodes))
        assert_equal(n0.getbook("GOLD", "USDX"), after)

        self.log.info("Collect what an order got; the bid's GOLD to its holder")
        before = self.holding(n1, "USDX")
        n1.settleorder(a2["order"])
        self.bmm()
        assert_equal(self.holding(n1, "USDX") - before, ask["got"])
        assert_raises_rpc_error(-8, "No such order", n0.getorder, a2["order"])
        before = self.holding(n2, "GOLD")
        n2.settleorder(bid["order"], True)
        self.bmm()
        assert_equal(self.holding(n2, "GOLD") - before, Decimal("1000"))

        self.log.info("A taker through the book, what getswapquote said; one larger than the book refused")
        quote = n2.getswapquote("USDX", 300, "GOLD")
        q = [p for p in quote["pools"] if p["pool"] == a3["book"]]
        assert q, quote
        before = self.holding(n2, "GOLD")
        n2.swappools("USDX", 300, [a3["book"]])
        self.bmm()
        assert_equal(self.holding(n2, "GOLD") - before, q[0]["amount_out"])
        assert_equal(q[0]["amount_out"], Decimal("100"))
        # Larger than the book: it would fill not at all (all or nothing); the wallet says so (the block refunds it whole: dex_book_tests).
        assert_raises_rpc_error(-8, "buys nothing", n2.swappools, "USDX", 50000, [a3["book"]])
        assert_peg_balance(main, self.nodes)

        self.log.info("Cancel: what is left comes back")
        before = self.holding(n1, "GOLD")
        n1.settleorder(a3["order"], True)
        self.bmm()
        assert_equal(self.holding(n1, "GOLD") - before, Decimal("900"))
        assert_equal(n0.getbook("GOLD", "USDX")["open_orders"], 0)

        self.log.info("An order that expires: back to its holder")
        short = n1.placeorder("GOLD", 500, "USDX", 5, 1)
        self.bmm()
        self.bmm()
        self.bmm()
        assert any(o["expired"] for o in n1.listorders() if o["order"] == short["order"])
        before = self.holding(n1, "GOLD")
        n1.settleorder(short["order"])
        self.bmm()
        assert_equal(self.holding(n1, "GOLD") - before, Decimal("500"))
        assert_equal(n1.listorders(), [])
        assert_peg_balance(main, self.nodes)

        self.log.info("Verified issuers: a curator (an oracle) verifies GOLD, then takes it back")
        curator = n0.registeroracle("curator")["oracle"]
        self.bmm()
        assert_equal(n2.getverifications("GOLD")["verified"], False)
        done = n0.verifyasset(curator, "GOLD")
        assert_equal(done["round"], 1)
        self.bmm()
        seen = n2.getverifications("GOLD", [curator])
        assert seen["verified"]
        assert_equal(seen["curators"][0]["round"], 1)
        assert_equal(n2.getverifications("GOLD")["curators"][0]["curator"], curator)
        assert_raises_rpc_error(-8, "Verified already", n0.verifyasset, curator, "GOLD")
        n0.verifyasset(curator, "GOLD", False)
        self.bmm()
        seen = n2.getverifications("GOLD", [curator])
        assert not seen["verified"]
        assert_equal(seen["curators"][0]["round"], 2)
        assert_equal(n2.getverifications("USDX")["curators"], [])


if __name__ == "__main__":
    LayersBookTest(__file__).main()
