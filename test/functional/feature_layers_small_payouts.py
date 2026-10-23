#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Smaller payouts (smallpayouts, bitassets::MinChnPayout and MinBookChn):

  - before small_payout_height: the least ECX paid out is 0.0005 and an order of a pair with ECX is worth 0.01 on
    its ECX side (getbitassetsinfo's min_chn_payout, min_book_chn); an order of 0.0001 ECX is refused;
  - from it: 0.00005 ECX for both; an order of 0.0001 ECX is placed, crosses another, and pays its 0.0001 out;
  - a node without the rule (FLUSHD_OLD: a flushd of v3; else this one with smallpayouts never) follows the chain
    until the first block with an order or payout under the old minimum, and forks off there.
"""

from decimal import Decimal
import os

from feature_layers import LayersTest
from test_framework.util import assert_equal, assert_raises_rpc_error

DEX_HEIGHT = 10
CURVES_HEIGHT = 15
SMALL_HEIGHT = 60


class LayersSmallPayoutsTest(LayersTest):
    def set_test_params(self):
        super().set_test_params()
        self.side_args = self.side_args + [f"-testactivationheight=dex@{DEX_HEIGHT}", f"-testactivationheight=dexcurves@{CURVES_HEIGHT}",
                                           f"-testactivationheight=smallpayouts@{SMALL_HEIGHT}"]
        self.extra_args = [self.side_args] * self.num_nodes

    def run_test(self):
        super().run_test()

    def bmm_to(self, height):
        while self.nodes[0].getblockcount() < height:
            self.bmm()

    def bmm_on(self, nodes):
        """A BMM block by n0 that the nodes `nodes` take (a node that forked off never does)."""
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

        self.log.info("Setup: the slot, deposits, an asset (BMM blocks)")
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
        n0.reserveasset("GOLD")
        self.bmm_to(n0.getblockcount() + 3)
        n0.registerasset("GOLD", 10000000, 2)
        self.bmm()
        n0.sendasset(n1.getnewaddress(), "GOLD", 100000)
        self.bmm_to(CURVES_HEIGHT)

        self.log.info("Before small_payout_height: 0.0005 ECX paid out at least, an order worth 0.01 ECX at least")
        assert n0.getblockcount() + 1 < SMALL_HEIGHT
        info = n0.getbitassetsinfo()
        assert_equal((info["min_chn_payout"], info["min_book_chn"], info["small_payout_height"]), (Decimal("0.0005"), Decimal("0.01"), SMALL_HEIGHT))
        assert_raises_rpc_error(None, "dust", n0.placeorder, "ECX", Decimal("0.0001"), "GOLD", 1000)

        self.log.info("A node without the rule (an old node: smallpayouts never, or FLUSHD_OLD) follows the chain until then")
        old = os.environ.get("FLUSHD_OLD")
        if old:
            n2.args[0] = old
            self.restart_node(2, extra_args=[a for a in self.extra_args[2] if not a.startswith("-testactivationheight=smallpayouts")])
        else:
            self.restart_node(2, extra_args=self.extra_args[2] + ["-testactivationheight=smallpayouts@1000000"])
        self.connect_nodes(2, 0)
        self.connect_nodes(2, 1)
        self.bmm()

        self.log.info("From it: 0.00005 ECX for both; an order of 0.0001 ECX, and the old node forks off at its block")
        self.bmm_to(SMALL_HEIGHT - 1)
        info = n0.getbitassetsinfo()
        assert_equal((info["min_chn_payout"], info["min_book_chn"]), (Decimal("0.00005"), Decimal("0.00005")))
        assert_raises_rpc_error(None, "dust", n0.placeorder, "ECX", Decimal("0.00004999"), "GOLD", 1000)
        buy = n0.placeorder("ECX", Decimal("0.0001"), "GOLD", 1000)
        assert buy["txid"] not in n2.getrawmempool()
        fork = self.bmm_on([n0, n1])
        assert_equal(n0.getblockheader(fork)["height"], SMALL_HEIGHT)
        self.wait_until(lambda: any(t["hash"] == fork and t["status"] == "invalid" for t in n2.getchaintips()))
        assert_equal(n2.getblockcount(), SMALL_HEIGHT - 1)

        self.log.info("It crosses an order selling GOLD for 0.0001 ECX: the seller collects its 0.0001 ECX")
        sell = n1.placeorder("GOLD", Decimal("0.1"), "ECX", Decimal("0.001"))
        self.bmm_on([n0, n1])
        got = n1.getorder(sell["order"])
        assert_equal(got["to_collect"], Decimal("0.0001"))
        n1.settleorder(sell["order"], True)
        block = self.bmm_on([n0, n1])
        assert any(o["value"] == Decimal("0.0001") for o in n1.getblock(block, 2)["tx"][0]["vout"]), "0.0001 ECX paid out by the coinbase"
        assert_peg_balance(main, [n0, n1])


if __name__ == "__main__":
    LayersSmallPayoutsTest(__file__).main()
