#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Layers' launches (doc/layers.md, "Launches"): a token on a bonding curve, from a reservation.

  - launchtoken from a reserved ticker: a billion tokens on the curve, a first buy in its own block;
  - buys and a sale on the curve, each what quotelaunch said; the sale's CHN by the coinbase;
  - pools of the token refused while it is on its curve;
  - a buy past the curve's end: the curve's last tokens, the rest of its CHN back; the launch graduates in that
    block, having raised exactly what it graduates at (500 CHN, within the rounding of a few satoshis), into its
    pool with CHN, whose shares nobody holds; trading goes on there;
  - a reorganisation across the graduation takes it back (the curve again), and forward again;
  - claimlaunch by anyone pays the creator its part of the fees and its allocation;
  - getlaunchlog, listlaunches, getlaunchstats; the peg balance throughout.
"""

from decimal import Decimal

from feature_layers import LayersTest
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error

DEX_HEIGHT = 10
CURVES_HEIGHT = 15


class LayersLaunchTest(LayersTest):
    def set_test_params(self):
        super().set_test_params()
        self.side_args = self.side_args + [f"-testactivationheight=dex@{DEX_HEIGHT}", f"-testactivationheight=dexcurves@{CURVES_HEIGHT}", "-testactivationheight=ticker@0", "-txindex"]
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

        self.log.info("Setup: the slot, deposits (BMM blocks)")
        self.mine_main(300)
        main.createsidechainproposal(SLOT, "Layers", "Assets on a fast chain", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 900)
        main.createsidechaindeposit(SLOT, n1.getdepositaddress()["depositaddress"], 50)
        main.createsidechaindeposit(SLOT, n2.getdepositaddress()["depositaddress"], 50)
        self.mine_main()
        self.bmm()
        for n in self.nodes:
            n.sendmany("", {n.getnewaddress(): 1 for _ in range(10)})
        self.bmm()
        self.bmm_to(CURVES_HEIGHT)
        stats = n0.getlaunchstats()
        assert stats["rules"]["active"]
        assert_equal(stats["rules"]["graduation"], Decimal("500"))
        assert_equal(stats["launches"], 0)

        self.log.info("Launch: a reserved ticker, a billion tokens on its curve, a first buy in its own block")
        n0.reserveasset("MOON")
        self.bmm_to(n0.getblockcount() + 3)
        made = n0.launchtoken("MOON", {"display_name": "To the moon", "info": "https://example.org/moon", "allocation": 2, "first_buy": 1})
        block = self.bmm()
        launch = n1.getlaunch("MOON")
        assert_equal(launch["status"], "curve")
        assert_equal(launch["ticker"], "MOON")
        assert_equal(launch["display_name"], "To the moon")
        assert_equal(launch["pool"], made["pool"])
        assert_equal(launch["supply"], Decimal("1000000000"))
        assert_equal(launch["allocation"], Decimal("2"))
        assert_greater_than(launch["raised"], Decimal("0.98"))
        assert_greater_than(self.holding(n0, "MOON"), 0)
        log = n1.getlaunchlog(n0.getblockheader(block)["height"])
        assert_equal(len(log), 1)
        assert log[0]["created"]
        assert_equal(n1.decodeassettx(made["txid"])["operation"]["name"], "launch_token")
        # No pool of it but its curve while it is on its curve.
        assert_raises_rpc_error(-4, "not accepted", n0.addliquidity, "MOON", 1000, "CHN", 1)
        assert_peg_balance(main, self.nodes)

        self.log.info("Buys and a sale on the curve, each what quotelaunch said")
        quote = n1.quotelaunch("MOON", 5)
        n1.buyfromlaunch("0x" + made["token"], 5)
        self.bmm()
        got = self.holding(n1, "MOON")
        assert_equal(got, quote["amount_out"])
        quote = n1.quotelaunch("MOON", got / 2, "sell")
        sold = n1.selltolaunch("0x" + made["token"], got / 2)
        block = self.bmm()
        assert self.paid_in(n1, block, sold["quote"]), "the sale's CHN by the coinbase"
        assert_equal(sold["quote"], quote["amount_out"])
        assert_peg_balance(main, self.nodes)

        self.log.info("A buy past the curve's end: the last tokens, the rest back; it graduates at exactly 500")
        launch = n0.getlaunch("MOON")
        quote = n0.quotelaunch("MOON", 600)
        assert quote["graduates"]
        assert_greater_than(quote["back"], 0)
        whale = n0.buyfromlaunch("0x" + made["token"], 600)
        grad_block = self.bmm()
        grad_height = n0.getblockheader(grad_block)["height"]
        done = n1.getlaunch("MOON")
        assert_equal(done["status"], "graduated")
        assert_equal(done["graduated_height"], grad_height)
        assert Decimal("500") <= done["raised"] <= Decimal("500.00001"), done["raised"]
        assert_equal(done["progress"], 100)
        assert_equal(done["curve_tokens_left"], 0)
        pool = n1.getpool("MOON", "CHN")
        assert pool["default_kind"]
        assert_equal(pool["pool"], done["pool"])
        # The raised CHN and the fees for the pool; the tokens: a quarter of the supply (plus what the curve kept, none).
        assert_greater_than(pool["reserve_b"], Decimal("500"))
        assert_equal(pool["reserve_a"], Decimal("250000000"))
        assert not any(p.get("pool") == pool["pool"] for p in n0.listmyassets().get("pools", []))
        assert self.paid_in(n0, grad_block, whale["back"]), "what the curve's end did not take, back"
        log = {r["token"]: r for r in n1.getlaunchlog(grad_height)}
        assert log[made["token"]]["graduated"]
        assert_equal(n1.listlaunches({"status": "graduated"})["total"], 1)
        assert_equal(n1.listlaunches({"status": "curve"})["total"], 0)
        assert_peg_balance(main, self.nodes)

        self.log.info("A reorganisation across the graduation takes it back, and forward again")
        tip = n0.getbestblockhash()
        for n in self.nodes:
            n.invalidateblock(grad_block)
        back = n1.getlaunch("MOON")
        assert_equal(back["status"], "curve")
        assert_raises_rpc_error(-8, "no pool", n1.getpool, "MOON", "CHN")
        for n in self.nodes:
            n.reconsiderblock(grad_block)
        self.wait_until(lambda: all(n.getbestblockhash() == tip for n in self.nodes))
        assert_equal(n1.getlaunch("MOON"), done)

        self.log.info("Trading in the pool; the creator's claim, sent by anyone")
        before = self.holding(n1, "MOON")
        n1.swapasset("CHN", 1, "MOON")
        self.bmm()
        assert_greater_than(self.holding(n1, "MOON"), before)
        assert_raises_rpc_error(-8, "graduated", n1.buyfromlaunch, "0x" + made["token"], 1)
        owed = n1.getlaunch("MOON")["creator_fees_owed"]
        assert_greater_than(owed, Decimal("1"))
        mine = self.holding(n0, "MOON")
        n2.claimlaunch("0x" + made["token"])
        block = self.bmm()
        assert self.paid_in(n0, block, owed), "the creator's fees by the coinbase"
        assert_equal(self.holding(n0, "MOON") - mine, Decimal("20000000"))
        claimed = n1.getlaunch("MOON")
        assert claimed["allocation_claimed"]
        assert_equal(claimed["creator_fees_owed"], 0)
        assert_equal(claimed["supply"], Decimal("1020000000"))
        assert_raises_rpc_error(-8, "Nothing is owed", n2.claimlaunch, "0x" + made["token"])
        stats = n1.getlaunchstats()
        assert_equal((stats["launches"], stats["graduated"]), (1, 1))
        assert_peg_balance(main, self.nodes)


if __name__ == "__main__":
    LayersLaunchTest(__file__).main()
