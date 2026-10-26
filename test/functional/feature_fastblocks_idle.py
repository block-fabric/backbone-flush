#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""FastBlocks on an idle chain: validators only, no transactions, no setbmm.

Three genesis validators with 15-second slots (-posslotseconds=15), as on the networks, their anchorers on
(-validatoranchorbid), on mock time (every node and the mainchain), a slot at a time:

  - an hour and more with no fees and no merged miner: the validators' anchorers bid for every
    mainchain block whatever the fees, so blocks of stake go on (they halt after max_unanchored_slots,
    25 minutes here, without an anchor) and anchors keep coming;
  - the rate: mainchain blocks that come in pairs, the second before any leader could acknowledge the
    first one's commitment: the anchorers bid for every mainchain block all the same (they waited for
    the acknowledgement before), and at least 90 % of the mainchain blocks have a commitment, each
    acknowledged after;
  - blocks of stake stopped (staking off everywhere): after stall_seconds the validators merge mine
    BMM blocks for their bid, whatever the fees;
  - the validators restarted keep bidding (no setbmm to call again).

Every wait is on a condition (a block of each slot, a bid in the mainchain's mempool), bounded
generously: the clock moves only when the test moves it, so a busy machine makes the test slower, not
different. (It waited 10 seconds for a bid and counted a block without one as uncommitted: under load,
the rate fell below 90 %.)
"""

import os
import time

from feature_sidechain import ACTIVATION_PERIOD, SidechainTest
from test_framework.fastblocks import KEYS, REGTEST_GENESIS_TIME, SLOT, XONLY, assert_peg_balance
from test_framework.util import assert_equal, assert_greater_than, assert_greater_than_or_equal, rpc_port

SLOT_SECONDS = 15
#: max_unanchored_slots on regtest.
MAX_UNANCHORED_SLOTS = 100


class FastBlocksIdleTest(SidechainTest):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.main_rpc_port = rpc_port(self.num_nodes)
        self.side_args = [
            f"-sidechainslot={SLOT}",
            f"-mainchainrpcport={self.main_rpc_port}",
            "-mainchainrpcuser=main",
            "-mainchainrpcpassword=secret",
            "-mainchainrpcwallet=miner",
            f"-posvalidators={','.join(XONLY)}",
            f"-posslotseconds={SLOT_SECONDS}",
        ]
        self.extra_args = [self.side_args] * self.num_nodes
        self.main_process = None
        self.rpc_timeout = 120

    def setup_network(self):
        self.start_mainchain()
        self.setup_nodes()
        self.connect_all()

    def connect_all(self):
        for a, b in ((0, 1), (1, 2), (0, 2)):
            self.connect_nodes(a, b)

    def validator_args(self, i):
        return self.side_args + [f"-validatorkeyfile={self.key_files[i]}", "-validatoranchorbid=0.00001", "-validatoranchorbidstrategy=fixed"]

    def set_time(self, t):
        self.now = t
        self.main.setmocktime(t)
        for n in self.nodes:
            n.setmocktime(t)

    def tip_slot(self, node):
        block = node.getblock(node.getbestblockhash())
        return block.get("slot", -1)

    def next_slot(self, settle=0):
        """The clock on to the next slot: each node has its leader's block of it. Then `settle` seconds
        (real time) for the anchorers, which look once a second: slots are 15 seconds apart."""
        slot = (self.now - REGTEST_GENESIS_TIME) // SLOT_SECONDS + 1
        self.set_time(REGTEST_GENESIS_TIME + slot * SLOT_SECONDS)
        self.wait_until(lambda: all(self.tip_slot(n) == slot for n in self.nodes), timeout=120)
        time.sleep(settle)
        return slot

    def bid_waiting(self, timeout=120):
        """Whether a ticket request of this chain is in the mainchain's mempool (an anchorer's bid), waiting for one a while."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.main.getrawmempool():
                return True
            time.sleep(0.2)
        return False

    def mine(self, bid_timeout=120):
        """A mainchain block, a second on; whether it has a commitment of this chain (a bid is waited
        for, bid_timeout at most: one is due, except where blocks of stake stopped)."""
        committed = self.bid_waiting(bid_timeout)
        if not committed:
            self.log.debug(f"no bid after mainchain block {self.main.getbestblockhash()} at {self.main.getblockcount()}")
        self.set_time(self.now + 1)
        self.mine_main()
        return committed

    def run_test(self):
        try:
            self.run_sidechain_test()
        finally:
            self.stop_mainchain()

    def run_sidechain_test(self):
        main = self.main
        n0, n1, n2 = self.nodes
        self.main_address = main.getnewaddress()

        self.log.info("Activate slot 5; a deposit in a BMM block; the validators start, their anchorers on")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "FastBlocks", "Blocks of stake between merged mined blocks", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 50)
        self.mine_main()
        block = n0.createbmmblock(n0.getnewaddress())
        main.createbmmrequest(SLOT, block["hash"], 0.001)
        self.mine_main()
        self.wait_until(lambda: all(n.getbestblockhash() == block["hash"] for n in self.nodes))
        self.key_files = []
        for i, key in enumerate(KEYS):
            path = os.path.join(self.options.tmpdir, f"validator{i}.key")
            with open(path, "w") as f:
                f.write(key.hex() + "\n")
            self.key_files.append(path)
        # Mock time from here: the mainchain's clock first (mining many blocks put its times ahead).
        start = max(int(time.time()), main.getblock(main.getbestblockhash())["time"]) + 5
        self.now = start - start % SLOT_SECONDS
        for i in range(3):
            self.restart_node(i, extra_args=self.validator_args(i) + [f"-mocktime={self.now}"])
        self.connect_all()
        self.set_time(self.now)

        self.log.info("Over an hour, idle: blocks of stake and anchors go on")
        first_slot = self.next_slot()
        anchors_before = n0.getposinfo()["anchors"]
        committed = 0
        mined = 0
        hour = 3600 // SLOT_SECONDS + 10
        for k in range(hour):
            slot = self.next_slot()
            if k % 3 == 2:
                committed += self.mine()
                mined += 1
        assert_greater_than(slot - first_slot, MAX_UNANCHORED_SLOTS + 10)
        assert_greater_than_or_equal(slot, first_slot + 3600 // SLOT_SECONDS)
        anchors = n0.getposinfo()["anchors"] - anchors_before
        self.log.info(f"{hour} slots: {mined} mainchain blocks, {committed} with a commitment, {anchors} anchors")
        assert_greater_than(anchors, mined * 9 // 10 - 2)
        assert_equal(len({n.getbestblockhash() for n in self.nodes}), 1)
        assert_peg_balance(main, self.nodes)

        self.log.info("The rate: mainchain blocks in pairs, a commitment in at least 90 % of them")
        anchors_before = n0.getposinfo()["anchors"]
        pairs = 15
        committed = 0
        for _ in range(pairs):
            self.next_slot(settle=2)
            committed += self.mine()
            # The second before any leader acknowledged the first one's commitment.
            committed += self.mine()
            self.next_slot(settle=2)
        for _ in range(4):
            self.next_slot()
        self.log.info(f"{2 * pairs} mainchain blocks, {committed} with a commitment")
        assert_greater_than_or_equal(committed * 10, 2 * pairs * 9)
        # Each acknowledged.
        anchors = n0.getposinfo()["anchors"] - anchors_before
        assert_greater_than_or_equal(anchors, committed - 1)
        assert_peg_balance(main, self.nodes)

        self.log.info("Blocks of stake stopped (staking off everywhere): the validators merge mine, whatever the fees")
        for n in self.nodes:
            n.setstaking(False)
        bmm_before = n0.getblockcount()
        # stall_seconds is 30 slots, from the last block of stake to a mainchain block: more.
        for k in range(42):
            self.set_time(self.now + SLOT_SECONDS)
            time.sleep(0.3)
            if k % 3 == 2:
                # (No bid is due before the stall: no block to anchor after the last ticket's.)
                self.mine(bid_timeout=3)
        def merged_mined():
            if n0.getblock(n0.getbestblockhash())["kind"] == "bmm" and n0.getblockcount() > bmm_before:
                return True
            if self.bid_waiting(timeout=3):
                self.mine()
            return False
        self.wait_until(merged_mined, timeout=60)
        for n in self.nodes:
            n.setstaking(True)
        self.next_slot()
        assert_equal(len({n.getbestblockhash() for n in self.nodes}), 1)
        assert_peg_balance(main, self.nodes)

        self.log.info("The validators restarted: they bid again, with no setbmm called")
        for i in range(3):
            self.restart_node(i, extra_args=self.validator_args(i) + [f"-mocktime={self.now}"])
        self.connect_all()
        self.set_time(self.now)
        anchors_before = n0.getposinfo()["anchors"]
        committed = 0
        for k in range(9):
            self.next_slot()
            if k % 3 == 1:
                committed += self.mine()
        assert_equal(committed, 3)
        assert_greater_than_or_equal(n0.getposinfo()["anchors"] - anchors_before, 2)
        assert_peg_balance(main, self.nodes)

if __name__ == '__main__':
    FastBlocksIdleTest(__file__).main()
