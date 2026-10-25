#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The fee-flow release (v5) from fee_flow_height H, and a node without it.

  - before H a sale of GOLD for ECX pays its fee in GOLD (the rule before);
  - from H a pool of ECX takes its fee in ECX: a sale's out of the ECX it gets, 85 % back to the pool, 15 % to security;
  - a node without the rule (FLUSHD_OLD: a flushd of photon-v4; else this one with feeflow never) follows the chain until
    the first block with such a sale, and forks off there;
  - a launch made from H with fee_destination burn and keep_control: its control coin to the creator, mintable; buys
    burn the creator's part; at graduation a locked full-range position of a concentrated pool at 1 %.
"""

from decimal import Decimal
import os

from feature_layers import LayersTest
from test_framework.util import assert_equal, assert_greater_than

DEX_HEIGHT = 10
CURVES_HEIGHT = 15
H = 60


class LayersV5Test(LayersTest):
    def set_test_params(self):
        super().set_test_params()
        self.side_args = self.side_args + [f"-testactivationheight=dex@{DEX_HEIGHT}", f"-testactivationheight=dexcurves@{CURVES_HEIGHT}",
                                           "-testactivationheight=smallpayouts@20", "-testactivationheight=notreasury@20",
                                           "-testactivationheight=launchparams@20", f"-testactivationheight=feeflow@{H}"]
        self.extra_args = [self.side_args] * self.num_nodes

    def run_test(self):
        super().run_test()

    def bmm_to(self, height):
        while self.nodes[0].getblockcount() < height:
            self.bmm()

    def bmm_on(self, nodes):
        from test_framework.fastblocks import SLOT
        self.sync_mempools(nodes)
        block = nodes[0].createbmmblock(self.side_address)
        self.main.createbmmrequest(SLOT, block["hash"], Decimal("0.001"))
        self.mine_main()
        self.wait_until(lambda: all(n.getbestblockhash() == block["hash"] for n in nodes))
        return block["hash"]

    def run_sidechain_test(self):
        main = self.main
        n0, n1, n2 = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = n0.getnewaddress()
        from feature_sidechain import ACTIVATION_PERIOD
        from test_framework.fastblocks import SLOT, assert_peg_balance

        self.log.info("Setup: the slot, deposits, GOLD and its pool with ECX")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "Layers", "Assets on a fast chain", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        for n, amount in ((n0, 600), (n1, 50), (n2, 20)):
            main.createsidechaindeposit(SLOT, n.getdepositaddress()["depositaddress"], amount)
        self.mine_main()
        self.bmm()
        for n in self.nodes:
            n.sendmany("", {n.getnewaddress(): 1 for _ in range(10)})
        self.bmm()
        n0.reserveasset("GOLD")
        self.bmm_to(n0.getblockcount() + 3)
        n0.registerasset("GOLD", 10000000, 2)
        self.bmm()
        n0.sendasset(n1.getnewaddress(), "GOLD", 100000)
        self.bmm_to(CURVES_HEIGHT)
        n0.addliquidity("GOLD", 10000, "ECX", 5)
        self.bmm()

        self.log.info("A node without the rule (FLUSHD_OLD, or feeflow never)")
        old = os.environ.get("FLUSHD_OLD")
        if old:
            n2.args[0] = old
            self.restart_node(2, extra_args=[a for a in self.extra_args[2] if not a.startswith("-testactivationheight=feeflow")])
        else:
            self.restart_node(2, extra_args=[a for a in self.extra_args[2] if not a.startswith("-testactivationheight=feeflow")] + ["-testactivationheight=feeflow@1000000"])
        self.connect_nodes(2, 0)
        self.connect_nodes(2, 1)
        self.bmm()
        n0.reserveasset("MOON")
        self.bmm_to(H)
        self.sync_blocks()
        assert_equal(n2.getblockcount(), H)
        assert n0.getlaunchstats()["rules"]["fee_flow_active"]

        self.log.info("A sale of GOLD from H: its fee in ECX; the node without the rule forks off at that block")
        security = n0.getdexinfo().get("security_paid", 0)
        n1.swapasset("GOLD", 1000, "ECX")
        fork = self.bmm_on([n0, n1])
        self.wait_until(lambda: any(t["hash"] == fork and t["status"] == "invalid" for t in n2.getchaintips()), timeout=60)
        assert_greater_than(n0.getblockcount(), n2.getblockcount())

        self.log.info("From H: a launch that burns its creator's fees and keeps its control coin")
        launched = n0.launchtoken("MOON", {"fee_destination": "burn", "keep_control": True, "first_buy": 1})
        self.bmm_on([n0, n1])
        launch = n0.getlaunch("MOON")
        assert launch["fee_flow"] and launch["mintable"]
        assert_equal((launch["fee_destination"], launch["pool_kind"]), ("burn", "concentrated"))
        assert_equal(launched["ticker"], "MOON")

        self.log.info("Buys burn the creator's part; past graduation, the locked position")
        for _ in range(2):
            n0.buyfromlaunch("MOON", 30)
            self.bmm_on([n0, n1])
        self.bmm_on([n0, n1])
        assert_greater_than(Decimal(n0.getlaunch("MOON")["burned"]), 0)
        n0.buyfromlaunch("MOON", 460)
        self.bmm_on([n0, n1])
        self.bmm_on([n0, n1])
        launch = n0.getlaunch("MOON")
        assert_equal(launch["status"], "graduated")
        assert "position" in launch
        assert_peg_balance(main, [n0, n1])
        del security


if __name__ == '__main__':
    LayersV5Test(__file__).main()
