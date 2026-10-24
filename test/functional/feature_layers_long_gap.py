#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Flush through 23 hours without BMM, and halting at 24, under the long-gap rule (doc/ecash-sidechains.md, "A gap of the
mainchain"), on eCash's rules scaled down; and the rule's activation at an epoch, which a node without it does not follow.

Against Backbone in its eCash regtest mode (see feature_layers_ecash.py, feature_layers_ecash_gap.py), the eCash
networks' parameters scaled by the mainchain's block interval: there ten minutes, 40 slots of 15 s; here MAIN_SECONDS
seconds, MAIN_SECONDS slots of a second. Before the rule's epoch: 8 hours of slots without an anchor, the seed and the
set 9 hours ahead (the values of before); from it: 24 hours, and 48 hours ahead.

  - the rule from epoch GAP_EPOCH, a few epochs after the validators start: before it every node follows the chain; from
    it, a node without the rule (FLUSHD_OLD: a flushd of photon-v4; else this one with the rule never) reads other seeds
    and leaders, and follows no more; restarted with the rule it takes the chain;
  - a silence of the mainchain of 23 hours' worth: blocks of stake go on (getposinfo's gap says until when), soft; the
    mainchain comes back, the ticket made before the silence anchors in time, the run becomes final;
  - a silence of 25 hours' worth: blocks of stake stop at 24 hours (no anchor for too long; the seeds would have gone on),
    the merged mined fallback stays off while the mainchain is silent; it comes back: the chain is anchored and goes on.
"""

import os
import time

from feature_layers_ecash import LayersEcashTest
from feature_sidechain_ecash import UNUSED_SLOT_THRESHOLD
from test_framework.fastblocks import SLOT, assert_peg_balance
from test_framework.util import assert_equal, assert_greater_than

MAIN_SECONDS = 4
SCALE = MAIN_SECONDS / 600
HOUR = round(3600 * SCALE)  # 24 slots of a second
EPOCH = HOUR
MAX_UNANCHORED = 8 * HOUR
LEAD = 9 * HOUR
GAP_MAX_UNANCHORED = 24 * HOUR
GAP_LEAD = 48 * HOUR
# The rule's epoch: this many epochs after the validators start.
EPOCHS_BEFORE = 4


class LayersLongGapTest(LayersEcashTest):
    def set_test_params(self):
        super().set_test_params()
        self.base_args = [
            f"-testposparam=max_unanchored_slots@{MAX_UNANCHORED}",
            f"-testposparam=slots_per_epoch@{EPOCH}",
            f"-testposparam=set_lead_seconds@{LEAD}",
            f"-posseedlookahead={LEAD}",
            "-testposparam=unbonding_anchors@400",
            "-testposparam=evidence_window_anchors@399",
        ]
        self.side_args += self.base_args
        self.extra_args = [self.side_args] * self.num_nodes
        self.gap_args = []

    def validator_args(self, i):
        return super().validator_args(i) + self.gap_args

    def run_test(self):
        super().run_test()

    def tick(self, count=1):
        for _ in range(count):
            time.sleep(MAIN_SECONDS)
            self.mine_main()

    def run_ecash_test(self):
        main = self.main
        n0, n1, n2 = self.nodes
        self.main_address = main.getnewaddress()
        genesis_time = n0.getblockheader(n0.getblockhash(0))["time"]

        self.log.info("Activate the slot, a deposit; the validators, with the long-gap rule from a few epochs on")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "Flush", "Tokens, assets and a DEX on a fast chain over eCash", "11" * 32, "22" * 20)
        self.mine_main(UNUSED_SLOT_THRESHOLD + 2)
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 50)
        self.mine_main()
        self.bmm_block(n0, n0.getnewaddress())
        self.payouts = [n.getnewaddress() for n in self.nodes]
        from feature_layers_ecash import KEYS
        for i, key in enumerate(KEYS):
            with open(self.key_file(i), "w") as f:
                f.write("# validator key\n" + key.hex() + "\n")
        now_slot = int(time.time()) - genesis_time
        gap_epoch = now_slot // EPOCH + EPOCHS_BEFORE
        self.gap_args = [f"-testposparam=gap_epoch@{gap_epoch}", f"-testposparam=gap_max_unanchored_slots@{GAP_MAX_UNANCHORED}",
                         f"-testposparam=gap_seed_lookahead_seconds@{GAP_LEAD}", f"-testposparam=gap_set_lead_seconds@{GAP_LEAD}"]
        for i in range(2):
            self.restart_node(i, extra_args=self.validator_args(i))
        old = os.environ.get("FLUSHD_OLD")
        old_args = self.side_args + [f"-validatorkeyfile={self.key_file(2)}", f"-validatorpayout={self.payouts[2]}"]
        if old:
            n2.args[0] = old
            self.restart_node(2, extra_args=old_args)
        else:
            self.restart_node(2, extra_args=old_args + ["-testposparam=gap_epoch@100000000"])
        self.connect_all()
        self.wait_until(lambda: all(n.getstakinginfo()["produced"] > 0 for n in self.nodes), timeout=120)
        n0.setbmm(True, n0.getnewaddress(), True, __import__("decimal").Decimal("0.0001"))
        self.tick(6)
        self.wait_in_sync()

        self.log.info(f"From epoch {gap_epoch}: the rule's values (the node without it follows while the seeds and limits agree)")
        info = n0.getposinfo()
        assert_equal(info["gapepoch"], gap_epoch)
        assert_equal((info["maxunanchoredslots"], info["setleadseconds"], info["seedlookaheadseconds"]), (MAX_UNANCHORED, LEAD, LEAD))
        while n0.getposinfo()["epoch"] < gap_epoch + 1:
            self.tick()
        info = n0.getposinfo()
        assert_equal((info["maxunanchoredslots"], info["setleadseconds"], info["seedlookaheadseconds"]), (GAP_MAX_UNANCHORED, GAP_LEAD, GAP_LEAD))
        # (On a young chain both leads take the seed of a time before the slot's activation, the same: the old node
        # follows until the limits part, below. On slot 99 the seeds part at once, at epoch 67.)
        self.wait_in_sync()
        assert_peg_balance(main, self.nodes)

        self.log.info(f"A silence of the mainchain of 23 hours ({23 * HOUR} s): blocks of stake go on, soft")
        self.wait_until(lambda: len(main.getrawmempool()) > 0, timeout=60)
        before = n0.getanchorinfo()
        height = n0.getblockcount()
        gap = n0.getposinfo()["gap"]
        # The seeds go on past the unanchored limit: the anchors alone stop the chain.
        assert_greater_than(gap["seedsuntil"], gap["unanchoreduntil"])
        assert_equal(gap["blocksuntil"], gap["unanchoreduntil"])
        start = time.time()
        while time.time() - start < 23 * HOUR:
            time.sleep(10)
            for n in (n0, n1):
                error = n.getposinfo()["producer"]["error"]
                assert "seed" not in error and "too long" not in error, error
            assert_equal(n0.getposinfo()["stalled"], False)
        self.sync_blocks([n0, n1])
        assert_greater_than(n0.getblockcount() - height, 23 * HOUR // 2)
        self.log.info("The node without the rule followed no further than 8 hours into the silence; restarted with it, it takes the chain")
        stuck = n2.getblockcount()
        assert_greater_than(n0.getblockcount(), stuck + 5 * HOUR)
        stuck_info = n2.getposinfo()
        assert_greater_than(MAX_UNANCHORED, n2.getblock(n2.getbestblockhash())["slot"] - stuck_info["firstunanchoredslot"])
        if old:
            n2.args[0] = n0.args[0]
        self.restart_node(2, extra_args=self.validator_args(2))
        self.connect_all()
        # It had found the blocks past its limit invalid (bad-pos-unanchored), which its block index keeps: taken back.
        self.wait_until(lambda: any(t["hash"] == n0.getbestblockhash() for t in n2.getchaintips()) or n2.getblockcount() >= n0.getblockcount() - 2, timeout=60)
        n2.reconsiderblock(n0.getbestblockhash())
        self.wait_until(lambda: n2.getblockcount() >= n0.getblockcount() - 2, timeout=180)
        after = n0.getanchorinfo()
        assert_equal(after["anchors"], before["anchors"])
        gap_tip = n0.getblockcount()
        assert_equal(n0.getblock(n0.getbestblockhash())["finality"]["status"], "soft")

        self.log.info("The mainchain comes back: the ticket made before the silence anchors in time, the run becomes final")
        self.tick()
        self.wait_until(lambda: n0.getposinfo()["anchors"] > after["anchors"], timeout=60)
        for _ in range(8):
            self.tick()
            if n0.getanchorinfo()["finalheight"] >= gap_tip:
                break
        self.wait_until(lambda: n0.getanchorinfo()["finalheight"] >= gap_tip, timeout=60)
        self.wait_in_sync()
        assert_peg_balance(main, self.nodes)

        self.log.info(f"A silence of 25 hours ({25 * HOUR} s): blocks of stake stop at 24 hours, the fallback stays off")
        self.tick(2)
        self.wait_until(lambda: len(main.getrawmempool()) > 0, timeout=60)
        start = time.time()
        stopped = None
        while time.time() - start < 25 * HOUR:
            time.sleep(5)
            info = n0.getposinfo()
            assert_equal(info["stalled"], False)
            if stopped is None and "too long" in info["producer"]["error"]:
                stopped = time.time() - start
        assert stopped is not None, "blocks of stake did not stop"
        info = n0.getposinfo()
        self.log.info(f"  stopped {stopped:.0f} s in ({stopped / HOUR:.1f} h); the last block of stake {info['lastposslot'] - info['firstunanchoredslot']} slots after the first unanchored")
        assert_greater_than(stopped, 23 * HOUR)
        assert_greater_than(GAP_MAX_UNANCHORED, info["lastposslot"] - info["firstunanchoredslot"])
        halted = n0.getblockcount()
        time.sleep(10)
        assert_equal(n0.getblockcount(), halted)

        self.log.info("The mainchain comes back: the chain is anchored and goes on")
        for _ in range(10):
            self.tick()
            if n0.getblockcount() > halted + 5:
                break
        self.wait_until(lambda: n0.getblockcount() > halted + 5, timeout=120)
        self.wait_until(lambda: n0.getposinfo()["producer"]["error"] == "", timeout=120)
        self.wait_in_sync()
        assert_peg_balance(main, self.nodes)
        n0.setbmm(False)


if __name__ == '__main__':
    LayersLongGapTest(__file__).main()
