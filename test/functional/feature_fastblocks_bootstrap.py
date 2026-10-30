#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The bootstrap of the validator sets (eCash mainnet's bootstrap_end_epoch, doc/ecash-sidechains.md, "The bootstrap"),
scaled down on regtest: the long-gap rule from the first epoch with a set lead of LONG_LEAD epochs, and a bootstrap
until epoch B.

  - the fork height (eCash mainnet's fork_height, block 10): below it no block of stake, though the sets are active
    (bonds anchored, as on mainnet blocks 3 and 4, and merged mined blocks after them); from it blocks of stake at once,
    by the sets the anchors below the fork made;
  - during the bootstrap the set lead is 0: bonds count in the set of the epoch after next after the anchor that makes
    their set, far sooner than the long lead would let them, and blocks of stake start then;
  - from B on the long lead: a bond anchored at or after the bootstrap's last boundary (the first slot of epoch B - 2)
    is out of every set until the long lead passes its anchor; the epochs B - 1 to B + LONG_LEAD - 2 share one set
    (no set fixed is taken back, none skipped), then the long lead alone.
"""

import os
import time
from decimal import Decimal

from feature_sidechain import ACTIVATION_PERIOD, SidechainTest
from test_framework.fastblocks import KEYS, SLOT, XONLY, assert_peg_balance, bond_pop
from test_framework.util import assert_equal, assert_greater_than, rpc_port

EPOCH = 20                   # slots of a second (regtest)
LONG_LEAD = 10               # epochs: the long lead (32 hours on eCash mainnet)
REGTEST_GENESIS_TIME = 1296688602
BOOTSTRAP_EPOCHS = 16        # the bootstrap ends this many epochs after the test starts
FORK_HEIGHT = 7              # blocks of stake from this height (blocks 1 to 6 merged mined)


class FastBlocksBootstrapTest(SidechainTest):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.main_rpc_port = rpc_port(self.num_nodes)
        self.bootstrap_end = (int(time.time()) - REGTEST_GENESIS_TIME) // EPOCH + BOOTSTRAP_EPOCHS
        self.side_args = [
            f"-sidechainslot={SLOT}",
            f"-mainchainrpcport={self.main_rpc_port}",
            "-mainchainrpcuser=main",
            "-mainchainrpcpassword=secret",
            "-mainchainrpcwallet=miner",
            f"-testposparam=slots_per_epoch@{EPOCH}",
            "-testposparam=gap_epoch@0",
            "-testposparam=gap_max_unanchored_slots@100",
            "-testposparam=gap_seed_lookahead_seconds@3600",
            f"-testposparam=gap_set_lead_seconds@{LONG_LEAD * EPOCH}",
            f"-testposparam=bootstrap_end_epoch@{self.bootstrap_end}",
            f"-testposparam=fork_height@{FORK_HEIGHT}",
        ]
        self.extra_args = [self.side_args] * self.num_nodes
        self.main_process = None

    def bmm_block(self):
        self.sync_mempools()
        block = self.nodes[0].createbmmblock(self.side_address)
        self.main.createbmmrequest(SLOT, block["hash"], Decimal("0.001"))
        self.mine_main()
        self.wait_until(lambda: all(n.getbestblockhash() == block["hash"] for n in self.nodes))
        return block["hash"]

    def anchor(self):
        before = [n.getposinfo()["anchors"] for n in self.nodes]
        self.wait_until(lambda: len(self.main.getrawmempool()) > 0, timeout=60)
        self.mine_main()
        self.wait_until(lambda: all(n.getposinfo()["anchors"] > b for n, b in zip(self.nodes, before)), timeout=60)

    def keys(self, epoch):
        info = self.nodes[0].getvalidators(epoch)
        assert info["known"], epoch
        return sorted(v["pubkey"] for v in info["validators"])

    def run_test(self):
        try:
            self.run_sidechain_test()
        finally:
            self.stop_mainchain()

    def run_sidechain_test(self):
        main = self.main
        n0, n1 = self.nodes
        B = self.bootstrap_end
        self.main_address = main.getnewaddress()
        self.side_address = n0.getnewaddress()

        self.log.info(f"Activate the slot, a deposit, the validator keys; the bootstrap until epoch {B}")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "FastBlocks", "Blocks of stake between merged mined blocks", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 50)
        self.mine_main()
        self.bmm_block()
        for i in range(self.num_nodes):
            path = os.path.join(self.options.tmpdir, f"validator{i}.key")
            with open(path, "w") as f:
                f.write(KEYS[i].hex() + "\n")
            self.restart_node(i, extra_args=self.side_args + [f"-validatorkeyfile={path}", f"-validatorpayout={self.nodes[i].getnewaddress()}"])
        self.connect_nodes(0, 1)
        info = n0.getposinfo()
        assert_greater_than(B - 2, info["epoch"])
        assert_equal(info["setleadseconds"], 0)
        assert_equal(info["gapepoch"], 0)

        self.log.info(f"Bonds anchored below the fork height ({FORK_HEIGHT}): the sets are active, no block of stake")
        n0.sendtoaddress(n1.getnewaddress(), 1)
        self.bmm_block()
        n0.createbond(XONLY[0], Decimal("0.01"))
        n1.createbond(XONLY[1], Decimal("0.01"))
        self.bmm_block()
        self.bmm_block()
        assert_equal(n0.getblockcount(), 4)
        # The anchor's epoch as the chain takes it (AnchorSlot): of the later of its time and the median time of the
        # mainchain block that committed it, which runs ahead of the clock here (more than a hundred mainchain blocks
        # in a few seconds, each at least a second after the median of the ones before): up to an epoch later.
        anchor = n0.getblock(n0.getbestblockhash())
        main_median = main.getblock(main.getbestblockhash())["mediantime"]
        anchor_epoch = (max(anchor["time"], main_median) - REGTEST_GENESIS_TIME) // EPOCH
        assert_greater_than(B - 4, anchor_epoch)
        # The set of the epoch after next has them: fixed a set lead of 0 ahead (the long lead: LONG_LEAD epochs); known
        # once the clock is past its boundary (an anchor of a later epoch than the clock's: a few seconds on).
        self.wait_until(lambda: n0.getvalidators(anchor_epoch + 2)["known"], timeout=2 * EPOCH + 30)
        assert_equal(self.keys(anchor_epoch + 2), sorted(XONLY[:2]))
        assert n0.getvalidators(anchor_epoch + 2)["active"]
        while n0.getposinfo()["epoch"] < anchor_epoch + 3:
            time.sleep(2)
        assert_equal(n0.getblockcount(), 4)
        assert_equal([n.getstakinginfo()["produced"] for n in self.nodes], [0, 0])
        self.bmm_block()
        self.bmm_block()
        assert_equal(n0.getblockcount(), FORK_HEIGHT - 1)

        self.log.info("From the fork height: blocks of stake at once, by the sets the anchors below it made")
        last_bmm_epoch = n0.getposinfo()["epoch"]
        self.wait_until(lambda: all(n.getstakinginfo()["produced"] > 0 for n in self.nodes), timeout=2 * EPOCH + 30)
        first = n0.getblock(n0.getblockhash(FORK_HEIGHT))
        assert_equal(first["slot"] // EPOCH <= last_bmm_epoch + 1, True)
        assert_equal(n0.getposinfo()["anchors"], FORK_HEIGHT - 1)  # no anchor at or above the fork yet
        first_pos_epoch = n0.getposinfo()["epoch"]
        self.log.info(f"  bonds anchored in epoch {anchor_epoch}, blocks of stake from height {FORK_HEIGHT}, epoch {first_pos_epoch} (the long lead: {anchor_epoch + LONG_LEAD} at the earliest)")
        assert_greater_than(anchor_epoch + LONG_LEAD, first_pos_epoch)
        n0.setbmm(True, n0.getnewaddress(), True, Decimal("0.0001"))
        self.anchor()
        assert_peg_balance(main, self.nodes)

        self.log.info(f"A third key bonded once the bootstrap's last boundary (epoch {B - 2}) is passed")
        while n0.getposinfo()["epoch"] < B - 2:
            time.sleep(2)
            if len(main.getrawmempool()) > 0:
                self.mine_main()
        genesis = n0.getblockhash(0)
        owner = n0.getnewaddress()
        bond = n0.createbond(XONLY[2], Decimal("0.01"), bond_pop(KEYS[2], owner, genesis).hex(), owner)
        self.wait_until(lambda: n0.gettransaction(bond["txid"])["confirmations"] > 0, timeout=60)
        self.anchor()
        late_slot = n0.getposinfo()["slot"]
        late_epoch = late_slot // EPOCH
        assert late_epoch >= B - 2, (late_epoch, B)
        assert_equal(n0.getvalidator(XONLY[2])["bond"]["bonded"], Decimal("0.01"))

        self.log.info(f"From epoch {B}: the long lead; epochs {B - 1} to {B + LONG_LEAD - 2} share the bootstrap's last set")
        # Until the long lead passes the late anchor (a set is known once the chain is past its boundary).
        last = (late_slot + EPOCH) // EPOCH + LONG_LEAD + 1
        while n0.getposinfo()["epoch"] <= late_epoch + 2:
            time.sleep(2)
            if len(main.getrawmempool()) > 0:
                self.mine_main()
        self.anchor()
        assert_equal(n0.getposinfo()["setleadseconds"], LONG_LEAD * EPOCH)
        plateau = self.keys(B - 1)
        assert_equal(plateau, sorted(XONLY[:2]))
        for epoch in range(B - 1, B + LONG_LEAD - 1):
            assert_equal(self.keys(epoch), plateau)
        joined = None
        for epoch in range(B + LONG_LEAD - 1, last + 1):
            if XONLY[2] in self.keys(epoch):
                joined = epoch
                break
        self.log.info(f"  the late bond (anchored in epoch {late_epoch}) is in the sets from epoch {joined}")
        assert joined is not None
        # Not before the long lead passes its anchor's slot.
        assert_greater_than(joined, late_epoch + LONG_LEAD - 1)
        for epoch in range(joined, last + 1):
            assert XONLY[2] in self.keys(epoch), epoch
        self.wait_until(lambda: len({n.getbestblockhash() for n in self.nodes}) == 1, timeout=60)
        assert_peg_balance(main, self.nodes)
        n0.setbmm(False)


if __name__ == '__main__':
    FastBlocksBootstrapTest(__file__).main()
