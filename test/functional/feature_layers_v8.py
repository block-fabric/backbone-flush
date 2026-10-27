#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Upgrade v8 from v8_height H (slot 99's upgrade v8): the buybacks scanned MAX_BUYBACK_SCAN (64) a block, and a node without it.

  - more launches waiting to buy back than 64: from H a block buys back 64 of them, the next block the rest;
  - a node without the rule (FLUSHD_OLD: a flushd of 7.0.1; else this one with v8 never) buys all of them back in one
    block: its curves differ from then on, and it forks off at the first block whose coinbase pays a sale on one of those
    curves (its ECX differs);
  - the peg balance holds throughout.
"""

from decimal import Decimal
import os

from feature_layers import LayersTest
from test_framework.fastblocks import assert_peg_balance
from test_framework.util import assert_equal

H = 60
N = 66  # more than the 64 a block looks at


class LayersV8Test(LayersTest):
    def set_test_params(self):
        super().set_test_params()
        self.side_args = self.side_args + ["-testactivationheight=dex@10", "-testactivationheight=dexcurves@15", "-testactivationheight=feeflow@15",
                                           "-testactivationheight=v7@15", f"-testactivationheight=v8@{H}"]
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

        self.log.info(f"Setup: the slot, a deposit; {N} launches, all their tunable fee to buyback and burn")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "Layers", "Assets on a fast chain", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 600)
        self.mine_main()
        self.bmm()
        n0.sendmany("", {n0.getnewaddress(): 2 for _ in range(N + 10)})
        self.bmm()
        tickers = [f"BB{i}" for i in range(N)]
        for t in tickers:
            n0.reserveasset(t)
        self.bmm_to(20)
        for t in tickers:
            n0.launchtoken(t, {"fee_split": [{"to": "burn", "bps": 10000}]})
        self.bmm()
        assert all(n0.getlaunch(t)["fee_split"] == [{"to": "burn", "kind": "burn", "bps": 10000, "paid": 0, "owed": 0}] for t in tickers)

        self.log.info("A node without v8 follows until it buys back more than 64 at once")
        old = os.environ.get("FLUSHD_OLD")
        args = [a for a in self.extra_args[2] if not a.startswith("-testactivationheight=v8")]
        if old:
            n2.args[0] = old
            self.restart_node(2, extra_args=args)
        else:
            self.restart_node(2, extra_args=args + ["-testactivationheight=v8@1000000"])
        self.connect_nodes(2, 0)
        self.connect_nodes(2, 1)
        self.bmm_to(H)
        self.sync_blocks()

        self.log.info(f"From H: a buy on each of the {N} curves; the next block buys 64 back, the one after the rest")
        for t in tickers:
            n0.buyfromlaunch(t, Decimal("0.1"))
        self.bmm_on([n0, n1])
        assert all(n0.getlaunch(t)["buyback_pending"] >= Decimal("0.00005") for t in tickers)
        self.bmm_on([n0, n1])
        burned = [t for t in tickers if n0.getlaunch(t)["burned"] > 0]
        assert_equal(len(burned), 64)
        assert_peg_balance(main, [n0, n1])

        self.log.info("A sale on each curve: the old node, which bought all back at once, forks off")
        held = {a["label"]: a["balance"] for a in n0.listmyassets()["assets"]}
        for t in tickers:
            n0.selltolaunch(t, (held[t] / 2).quantize(Decimal("1")))
        fork = self.bmm_on([n0, n1])
        self.wait_until(lambda: any(tip["hash"] == fork and tip["status"] == "invalid" for tip in n2.getchaintips()), timeout=60)
        self.bmm_on([n0, n1])
        assert all(n0.getlaunch(t)["burned"] > 0 for t in tickers)
        assert_peg_balance(main, [n0, n1])


if __name__ == '__main__':
    LayersV8Test(__file__).main()
