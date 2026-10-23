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
  - getlaunchlog, listlaunches, getlaunchstats; the peg balance throughout;
  - launches with parameters (launchparams): a supply, decimals and fee of its own, to graduation (its pool a quarter of
    its supply); an existing asset put on a curve by its controller (curveasset: the control coin goes, the supply fixed),
    its issuer's retained units refused by the curve beyond what it sold, to graduation;
  - allocations of up to 25 % (allocation25): 5 % at most before ALLOC25_HEIGHT, 25 % from it (getlaunchstats'
    rules.max_allocation), minted on top of the supply at graduation and claimed by the creator; a node without the
    rule (an old node: allocation25 never) follows the chain until the first launch past 5 %, and forks off there.
"""

from decimal import Decimal
import os

from feature_layers import LayersTest
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error

DEX_HEIGHT = 10
CURVES_HEIGHT = 15
# Past the blocks the parts before take (about 60).
ALLOC25_HEIGHT = 120


class LayersLaunchTest(LayersTest):
    def set_test_params(self):
        super().set_test_params()
        self.side_args = self.side_args + [f"-testactivationheight=dex@{DEX_HEIGHT}", f"-testactivationheight=dexcurves@{CURVES_HEIGHT}", "-testactivationheight=ticker@0", f"-testactivationheight=allocation25@{ALLOC25_HEIGHT}", "-txindex"]
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
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 2500)
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
        half = (got / 2).quantize(Decimal("0.000001"))
        quote = n1.quotelaunch("MOON", half, "sell")
        sold = n1.selltolaunch("0x" + made["token"], half)
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

        self.log.info("A launch with parameters: 21 million of 8 decimals at 2 %, to graduation")
        rules = n0.getlaunchstats()["rules"]
        assert rules["params_active"]
        assert_equal((rules["fee_min"], rules["fee_max"], rules["supply_multiple_units"]), (Decimal("0.3"), Decimal("5"), 8))
        assert_raises_rpc_error(-8, "0.3 to 5 percent", n0.launchtoken, "WIDE", {"supply": 21000000, "decimals": 8, "fee": 6})
        n0.reserveasset("WIDE")
        self.bmm_to(n0.getblockcount() + 3)
        assert_raises_rpc_error(-8, "multiple of 8 units", n0.launchtoken, "WIDE", {"supply": 1000003, "decimals": 0})
        wide = n0.launchtoken("WIDE", {"supply": 21000000, "decimals": 8, "fee": 2, "first_buy": 1})
        assert_equal((wide["supply"], wide["decimals"], wide["fee"]), (Decimal("21000000"), 8, Decimal("2")))
        self.bmm()
        launch = n1.getlaunch("WIDE")
        assert_equal((launch["supply"], launch["decimals"], launch["fee"]), (Decimal("21000000"), 8, Decimal("2")))
        assert_equal((launch["curve_supply"], launch["pool_supply"], launch["from_asset"]), (Decimal("15750000"), Decimal("5250000"), False))
        assert_equal(n1.decodeassettx(wide["txid"])["operation"]["fee_ppm"], 20000)
        quote = n1.quotelaunch("WIDE", 2)
        n1.buyfromlaunch("WIDE", 2)
        self.bmm()
        assert_equal(self.holding(n1, "WIDE"), quote["amount_out"])
        n0.buyfromlaunch("WIDE", 600)
        self.bmm()
        assert_equal(n1.getlaunch("WIDE")["status"], "graduated")
        assert_equal(n1.getpool("WIDE", "CHN")["reserve_a"], Decimal("5250000"))
        assert_peg_balance(main, self.nodes)

        self.log.info("An existing asset on a curve: its controller's, the control coin gone, retained units refused beyond what it sold")
        n2.reserveasset("GOLD")
        self.bmm_to(n0.getblockcount() + 3)
        n2.registerasset("GOLD", 1000000, 6)
        self.bmm()
        assert_raises_rpc_error(-8, "multiple of 8 units", n2.curveasset, "GOLD", "0.000001")
        put = n2.curveasset("GOLD", 800000, {"fee": 1})
        assert_equal((put["curve_supply"], put["pool_supply"]), (Decimal("600000"), Decimal("200000")))
        self.bmm()
        gold = n1.getasset("GOLD")
        assert gold["fixed"]
        assert "control" not in gold
        curve = n1.getlaunch("GOLD")
        assert curve["from_asset"]
        assert_equal(curve["supply"], Decimal("1000000"))
        assert_raises_rpc_error(-8, "launched or put on a curve already", n2.curveasset, "GOLD", 8)
        # The issuer's 200 000 retained: nothing sold yet, so the curve takes none back.
        assert_raises_rpc_error(-8, "Too little", n2.selltolaunch, "GOLD", 1000)
        n1.buyfromlaunch("GOLD", 10)
        self.bmm()
        bought = self.holding(n1, "GOLD")
        assert_raises_rpc_error(-8, "Too little", n2.selltolaunch, "GOLD", bought * 2)
        back = n2.selltolaunch("GOLD", (bought / 2).quantize(Decimal("0.000001")))
        block = self.bmm()
        assert self.paid_in(n2, block, back["quote"])
        n0.buyfromlaunch("GOLD", 600)
        self.bmm()
        assert_equal(n1.getlaunch("GOLD")["status"], "graduated")
        assert_equal(n1.getpool("GOLD", "CHN")["reserve_a"], Decimal("200000"))
        assert_peg_balance(main, self.nodes)

        self.allocation_up_to_25(assert_peg_balance)

    def bmm_on(self, nodes):
        """A BMM block by n0 that the nodes `nodes` take (a node that forked off never does)."""
        from test_framework.fastblocks import SLOT
        self.sync_mempools(nodes)
        block = nodes[0].createbmmblock(self.side_address)
        self.main.createbmmrequest(SLOT, block["hash"], Decimal("0.001"))
        self.mine_main()
        self.wait_until(lambda: all(n.getbestblockhash() == block["hash"] for n in nodes))
        return block["hash"]

    def allocation_up_to_25(self, assert_peg_balance):
        main = self.main
        n0, n1, n2 = self.nodes
        self.log.info("Allocations of up to 25 %: 5 % at most before allocation25_height")
        assert_greater_than(ALLOC25_HEIGHT, n0.getblockcount() + 4)
        rules = n0.getlaunchstats()["rules"]
        assert_equal((rules["max_allocation"], rules["allocation25_height"]), (Decimal("5"), ALLOC25_HEIGHT))
        n0.reserveasset("QUARTER")
        n0.reserveasset("EARLY")
        self.bmm_to(n0.getblockcount() + 3)
        assert_raises_rpc_error(-8, "0 to 5 percent", n0.launchtoken, "QUARTER", {"allocation": 25})
        assert_raises_rpc_error(-8, "0 to 5 percent", n0.launchtoken, "QUARTER", {"allocation": 5.01})
        # 5 % itself, before the height: valid on every node, the old one too.
        self.log.info("A node without the rule (allocation25 never, as an old node) follows a 5 % launch")
        # FLUSHD_OLD: a flushd of before the rule (v2) as that node, a rehearsal of the upgrade; else this one without it.
        old = os.environ.get("FLUSHD_OLD")
        if old:
            n2.args[0] = old
            self.restart_node(2, extra_args=[a for a in self.extra_args[2] if not a.startswith("-testactivationheight=allocation25")])
        else:
            self.restart_node(2, extra_args=self.extra_args[2] + ["-testactivationheight=allocation25@1000000"])
        self.connect_nodes(2, 0)
        self.connect_nodes(2, 1)
        if not old:
            assert_equal(n2.getlaunchstats()["rules"]["allocation25_height"], 1000000)
        n0.launchtoken("EARLY", {"allocation": 5})
        self.bmm()
        assert_equal(n2.getlaunch("EARLY")["allocation"], Decimal("5"))

        self.log.info("From allocation25_height: 25 %, never more; the old node forks off at the first launch past 5 %")
        self.bmm_to(ALLOC25_HEIGHT - 1)
        rules = n0.getlaunchstats()["rules"]
        assert_equal(rules["max_allocation"], Decimal("25"))
        assert_equal(n2.getlaunchstats()["rules"]["max_allocation"], Decimal("5"))
        assert_raises_rpc_error(-8, "0 to 25 percent", n0.launchtoken, "QUARTER", {"allocation": 25.01})
        # (At 25 % a supply with its allocation past MAX_AMOUNT is refused by the rules, dex_launch_tests: the RPC's
        # amounts stop below 10^18 units, short of it.)
        made = n0.launchtoken("QUARTER", {"allocation": 25})
        # The old node never takes it: not in its mempool, nor the block with it.
        assert made["txid"] not in n2.getrawmempool()
        fork = self.bmm_on([n0, n1])
        assert_equal(n0.getblockheader(fork)["height"], ALLOC25_HEIGHT)
        self.wait_until(lambda: any(t["hash"] == fork and t["status"] == "invalid" for t in n2.getchaintips()))
        assert_equal(n2.getblockcount(), ALLOC25_HEIGHT - 1)
        launch = n1.getlaunch("QUARTER")
        assert_equal((launch["allocation"], launch["supply"]), (Decimal("25"), Decimal("1000000000")))

        self.log.info("Its graduation as any (500 raised, a quarter of the supply into its pool); 25 % minted on top, to the creator")
        n0.buyfromlaunch("QUARTER", 600)
        self.bmm_on([n0, n1])
        done = n1.getlaunch("QUARTER")
        assert_equal(done["status"], "graduated")
        assert Decimal("500") <= done["raised"] <= Decimal("500.00001"), done["raised"]
        assert_equal(n1.getpool("QUARTER", "CHN")["reserve_a"], Decimal("250000000"))
        before = self.holding(n0, "QUARTER")
        n1.claimlaunch("QUARTER")
        self.bmm_on([n0, n1])
        assert_equal(self.holding(n0, "QUARTER") - before, Decimal("250000000"))
        claimed = n1.getlaunch("QUARTER")
        assert claimed["allocation_claimed"]
        assert_equal(claimed["supply"], Decimal("1250000000"))
        assert_equal(n1.getpool("QUARTER", "CHN")["reserve_a"], Decimal("250000000"))
        assert_raises_rpc_error(-8, "Nothing is owed", n1.claimlaunch, "QUARTER")
        assert_peg_balance(main, [n0, n1])


if __name__ == "__main__":
    LayersLaunchTest(__file__).main()
