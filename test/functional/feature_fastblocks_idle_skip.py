#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""FastBlocks' anchorers bid only when needed ("Fees + prevent stalling", the default: skip_idle).

The validators of feature_fastblocks_idle.py, on mock time, with the default policy (no -validatoranchorbidstrategy):

  - a quiet chain (no fees, no peg or staking action waiting): no bid ("skipped: nothing to anchor") while the stall
    pressure is below its threshold (0.5 of max_unanchored_slots), even across epochs (no anchor an epoch);
  - from the threshold: a bid, the boost (4 x the base);
  - once anchored, quiet again: no bid; a deposit on the mainchain: a bid at once.
"""

from feature_fastblocks_anchorpolicy import FastBlocksAnchorPolicyTest
from test_framework.util import assert_equal, assert_greater_than

MAX_UNANCHORED = 40
EPOCH = 8


class FastBlocksIdleSkipTest(FastBlocksAnchorPolicyTest):
    def set_test_params(self):
        super().set_test_params()
        self.main_extra_args = []
        self.side_args = [a for a in self.side_args if not a.startswith("-testposparam=max_unanchored_slots")]
        self.side_args += [f"-testposparam=max_unanchored_slots@{MAX_UNANCHORED}", f"-testposparam=slots_per_epoch@{EPOCH}"]
        self.extra_args = [self.side_args] * self.num_nodes

    def run_test(self):
        super().run_test()

    def validator_args(self, i):
        # The default policy (fees + prevent stalling, skip_idle); the others' anchorers off, node 0 alone bids.
        return self.side_args + [f"-validatorkeyfile={self.key_files[i]}"] + (["-validatoranchorbid=0"] if i == 0 else ["-novalidatoranchorbid"])

    def run_sidechain_test(self):
        main = self.main
        n0 = self.nodes[0]
        self.main_address = main.getnewaddress()
        self.setup_chain_and_validators()
        # The chain's first peg steps (the deposit, the genesis set): anchored as needed.
        for _ in range(12):
            self.next_slot()
            if self.bid_waiting(3):
                self.set_time(self.now + 1)
                self.mine_main()
        self.log.info("Quiet: no bid below the threshold, across epochs")
        quiet = 0
        skipped = False
        while True:
            self.next_slot()
            bid = n0.getanchorinfo()["bid"]
            self.log.info(f"  p {bid['pressure']:.2f} {bid['kind']}: {bid.get('reason', '')}")
            if bid["pressure"] >= 0.5:
                break
            if bid["kind"] == "idle":
                skipped = True
                assert not self.bid_waiting(2), f"a bid on a quiet chain at p {bid['pressure']:.2f}"
                quiet += 1
            self.set_time(self.now + 1)
            self.mine_main()
        assert skipped, "never skipped"
        assert_greater_than(quiet, 3)
        self.log.info(f"{quiet} quiet mainchain blocks without a bid; from p = 0.5 the boost")
        assert self.bid_waiting(60), "no bid past the threshold"
        bid = n0.getanchorinfo()["bid"]
        assert_equal(bid["kind"], "boost")
        self.set_time(self.now + 1)
        self.mine_main()
        # Anchored: quiet again.
        for _ in range(6):
            self.next_slot()
            if n0.getanchorinfo()["bid"]["pressure"] < 0.5:
                break
        self.log.info("A deposit: a bid at once")
        main.createsidechaindeposit(5, n0.getdepositaddress()["depositaddress"], 1)
        self.set_time(self.now + 1)
        self.mine_main()
        self.next_slot()
        ok = self.bid_waiting(30)
        bid = n0.getanchorinfo()["bid"]
        self.log.info(f"  after the deposit: p {bid['pressure']:.2f} {bid['kind']}: {bid.get('reason', '')}; mempool {main.getrawmempool()}")
        assert ok, "no bid for a deposit"


if __name__ == '__main__':
    FastBlocksIdleSkipTest(__file__).main()
