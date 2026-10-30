#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Idle automatic bidding wakes for deposits/withdrawals and the stall threshold."""
from decimal import Decimal

from feature_fastblocks_anchorpolicy import FastBlocksAnchorPolicyTest, sats
from test_framework.fastblocks import SLOT
from test_framework.util import assert_equal


class FastBlocksAnchorActivityTest(FastBlocksAnchorPolicyTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        super().run_test()

    def run_sidechain_test(self):
        main, n0 = self.main, self.nodes[0]
        self.main_address = main.getnewaddress()
        self.setup_chain_and_validators()
        for n in self.nodes[1:]:
            n.setanchorbidpolicy({"strategy": "off"})
        # Clear the startup requests and establish a fresh anchor with no pending peg work.
        n0.setanchorbidpolicy({"strategy": "fixed", "fixed": 0.0001})
        for _ in range(3):
            self.next_slot(settle=1)
        self.mine()
        self.next_slot(settle=1)
        # Fees with Prevent stalling (the floor raises only stall prevention's base: Fees has no minimum).
        n0.setanchorbidpolicy({"strategy": "fees", "preventstall": True, "floor": 0.0001,
                               "flatboost": True, "periodcap": 0})
        self.mine_main()
        self.next_slot(settle=1)
        self.wait_until(lambda: n0.getanchorinfo()["bid"]["kind"] == "idle")
        idle = n0.getanchorinfo()["bid"]
        assert_equal(idle["pendingactions"], False)
        assert_equal(idle["reason"], "no bid: no fees, nothing waiting to anchor")
        assert_equal(sats(idle["effective"]), 0)
        assert_equal(main.getrawmempool(), [])

        self.log.info("A new deposit wakes an idle anchorer")
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 1)
        self.mine_main()
        self.wait_until(lambda: n0.getanchorinfo()["bid"]["pendingactions"])
        self.wait_until(lambda: sats(n0.getanchorinfo()["bid"]["effective"]) >= 10000)
        self.mine()
        self.next_slot(settle=1)
        self.wait_until(lambda: not n0.getanchorinfo()["bid"]["pendingactions"])

        self.log.info("Idle below the threshold, then a flat boost with no fees or peg work")
        self.mine_main()
        self.next_slot(settle=1)
        self.wait_until(lambda: n0.getanchorinfo()["bid"]["kind"] == "idle")
        while n0.getanchorinfo()["bid"]["pressure"] < 0.5:
            self.next_slot()
        self.wait_until(lambda: n0.getanchorinfo()["bid"]["kind"] == "stall")
        boost = n0.getanchorinfo()["bid"]
        assert_equal(boost["pendingactions"], False)
        assert_equal(sats(boost["effective"]), 40000)
        self.mine()
        self.next_slot(settle=1)

        self.log.info("Withdrawals keep peg work active even without relying on a fee pot")
        n0.createwithdrawal(main.getnewaddress(), 1, Decimal("0.01"))
        # Whichever validator leads the slot mines it: one that has not had the withdrawal yet (relayed as the clock
        # moves on) leaves it to a later slot.
        for _ in range(4):
            self.next_slot(settle=1)
            if n0.getanchorinfo()["bid"]["pendingactions"]:
                break
        self.wait_until(lambda: n0.getanchorinfo()["bid"]["pendingactions"])
        assert sats(n0.getanchorinfo()["bid"]["effective"]) > 0
        for _ in range(3):
            self.mine()
            self.next_slot(settle=1)
            if n0.getwithdrawalbundle()["status"] != "next":
                break
        assert n0.getwithdrawalbundle()["status"] != "next"


if __name__ == '__main__':
    FastBlocksAnchorActivityTest(__file__).main()
