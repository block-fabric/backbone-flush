#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Graduation at 100 ECX for launches made from graduation100_height H (slot 99's upgrade v6), and a node without it.

  - a launch made before H keeps 500 (its terms as made): getlaunch says graduation 500 after H too;
  - a launch made from H graduates at 100, its curve scaled: past 100 ECX raised it graduates;
  - a node without the rule (FLUSHD_OLD: a flushd of v5; else this one with graduation100 never) follows the chain past
    the first launch made at or after H (a block's validity does not depend on its curve), and forks off at the first
    block whose coinbase pays out from that curve (a sale: its ECX differs).
"""

from decimal import Decimal
import os

from feature_layers import LayersTest
from test_framework.util import assert_equal

H = 40


class LayersV6Test(LayersTest):
    def set_test_params(self):
        super().set_test_params()
        self.side_args = self.side_args + ["-testactivationheight=dex@10", "-testactivationheight=dexcurves@15",
                                           f"-testactivationheight=graduation100@{H}"]
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
        from test_framework.fastblocks import SLOT

        self.log.info("Setup: the slot, a deposit; a launch made before H")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "Layers", "Assets on a fast chain", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 600)
        self.mine_main()
        self.bmm()
        n0.reserveasset("OLD")
        n0.reserveasset("NEW")
        self.bmm_to(20)
        n0.launchtoken("OLD")
        self.bmm()
        assert_equal(n0.getlaunch("OLD")["graduation"], 500)

        self.log.info("A node without the rule follows until the first launch made from H")
        old = os.environ.get("FLUSHD_OLD")
        args = [a for a in self.extra_args[2] if not a.startswith("-testactivationheight=graduation100")]
        if old:
            n2.args[0] = old
            self.restart_node(2, extra_args=args)
        else:
            self.restart_node(2, extra_args=args + ["-testactivationheight=graduation100@1000000"])
        self.connect_nodes(2, 0)
        self.connect_nodes(2, 1)
        self.bmm_to(H)
        self.sync_blocks()
        assert_equal(n0.getlaunchstats()["rules"]["graduation"], 100)

        self.log.info("From H: a launch at 100 ECX; the old node forks off at the first block its scaled curve pays out from")
        n0.launchtoken("NEW", {"first_buy": 1})
        self.bmm_on([n0, n1])
        n0.selltolaunch("NEW", 10000000)
        fork = self.bmm_on([n0, n1])
        self.wait_until(lambda: any(t["hash"] == fork and t["status"] == "invalid" for t in n2.getchaintips()), timeout=60)
        assert_equal(n0.getlaunch("NEW")["graduation"], 100)
        assert_equal(n0.getlaunch("OLD")["graduation"], 500)

        self.log.info("It graduates once 100 ECX are raised; the one before H does not")
        n0.buyfromlaunch("NEW", 110)
        self.bmm_on([n0, n1])
        self.bmm_on([n0, n1])
        assert_equal(n0.getlaunch("NEW")["status"], "graduated")
        n0.buyfromlaunch("OLD", 110)
        self.bmm_on([n0, n1])
        assert_equal(n0.getlaunch("OLD")["status"], "curve")


if __name__ == '__main__':
    LayersV6Test(__file__).main()
