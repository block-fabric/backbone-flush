#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Layers against Backbone in its eCash regtest mode (backboned -regtest -testecash, see
feature_sidechain_ecash.py), with three validators and -testmainenforcer.

  - the slot is proposed and acked to activation (more than 5 acks in 10 blocks), into the OP_NOP8
    treasury a deposit, a BMM block;
  - the validators make blocks of stake; the merged miner's tickets anchor them, one per mainchain block;
  - anchors far apart, as eCash's ten-minute blocks are against 15-second slots (40 slots an anchor): the
    blocks of stake go on between them (max_unanchored_slots), and the next anchor covers them;
  - the fee curves are drawn for ten-minute mainchain blocks (fee_target_spacing 600);
  - a withdrawal made in a block of stake goes into a bundle at an acknowledgement, which the mainchain
    votes up with the anchors' blocks and pays out (more than 5 votes, M6); the peg balance holds throughout.
"""

import os
import time
from decimal import Decimal

from feature_sidechain_ecash import SidechainEcashTest, UNUSED_SLOT_THRESHOLD
from test_framework.fastblocks import SLOT, assert_peg_balance
from test_framework.key import compute_xonly_pubkey
from test_framework.util import assert_equal, assert_greater_than, rpc_port

POS_BLOCK_BIT = 0x10000
KEYS = [bytes([0x11] + [0] * 30 + [i]) for i in (1, 2, 3)]
# The slots between two anchors on eCash: ten minutes of 15-second slots.
SLOTS_PER_ANCHOR = 40


class LayersEcashTest(SidechainEcashTest):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.main_rpc_port = rpc_port(self.num_nodes)
        validators = ",".join(compute_xonly_pubkey(key)[0].hex() for key in KEYS)
        self.side_args = [
            f"-sidechainslot={SLOT}",
            "-testmainenforcer",
            f"-mainchainrpcport={self.main_rpc_port}",
            "-mainchainrpcuser=main",
            "-mainchainrpcpassword=secret",
            "-mainchainrpcwallet=miner",
            f"-posvalidators={validators}",
            "-testactivationheight=feesplit@1",
        ]
        self.extra_args = [self.side_args] * self.num_nodes
        self.main_process = None

    def key_file(self, i):
        return os.path.join(self.options.tmpdir, f"validator{i}.key")

    def validator_args(self, i):
        return self.side_args + [f"-validatorkeyfile={self.key_file(i)}", f"-validatorpayout={self.payouts[i]}"]

    def connect_all(self):
        for a, b in ((0, 1), (1, 2), (0, 2)):
            self.connect_nodes(a, b)

    def bmm_block(self, node, address):
        block = node.createbmmblock(address)
        self.main.createbmmrequest(SLOT, block["hash"], Decimal("0.001"))
        self.mine_main()
        self.wait_until(lambda: all(n.getbestblockhash() == block["hash"] for n in self.nodes))
        return block["hash"]

    def is_pos(self, node, blockhash):
        return bool(node.getblockheader(blockhash)["version"] & POS_BLOCK_BIT)

    def wait_in_sync(self):
        self.wait_until(lambda: len({n.getbestblockhash() for n in self.nodes}) == 1, timeout=60)

    def anchor(self):
        """One more anchor: wait for the merged miner's ticket request, mine it, wait for its acknowledgement."""
        before = [n.getposinfo()["anchors"] for n in self.nodes]
        # A mainchain block may have no commitment of this chain (Backbone left the ticket out of the block
        # that paid a withdrawal bundle): then the next one anchors.
        for _ in range(3):
            self.wait_until(lambda: len(self.main.getrawmempool()) > 0, timeout=60)
            self.mine_main()
            try:
                self.wait_until(lambda: all(n.getposinfo()["anchors"] > b for n, b in zip(self.nodes, before)), timeout=30)
                return
            except AssertionError:
                self.log.debug("No anchor in that mainchain block; one more")
        raise AssertionError("no anchor in three mainchain blocks")

    def run_test(self):
        super().run_test()

    def run_ecash_test(self):
        main = self.main
        n0, n1, n2 = self.nodes
        self.main_address = main.getnewaddress()

        self.log.info("Activate the slot on Backbone's eCash regtest: proposal and acks")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "Layers", "Assets on a fast chain", "11" * 32, "22" * 20)
        self.mine_main(UNUSED_SLOT_THRESHOLD + 2)
        assert_equal([s["slot"] for s in main.listactivesidechains()], [SLOT])
        assert_equal(main.getsidechain(SLOT)["escrowscript"], f"b701{SLOT:02x}51")

        self.log.info("Merged mined: a deposit into the OP_NOP8 treasury, a BMM block")
        info = n0.getposinfo()
        assert_equal(len(info["validators"]), 3)
        # The fee curves for ten-minute mainchain blocks.
        assert_equal(info["feesplit"]["targetspacing"], 600)
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 50)
        self.mine_main()
        first = self.bmm_block(n0, n0.getnewaddress())
        assert not self.is_pos(n0, first)
        assert_equal(n0.getbalance(), 50)
        assert_peg_balance(main, self.nodes)

        self.log.info("The validators start: blocks of stake")
        self.payouts = [n.getnewaddress() for n in self.nodes]
        for i, key in enumerate(KEYS):
            with open(self.key_file(i), "w") as f:
                f.write("# validator key\n" + key.hex() + "\n")
        for i in range(self.num_nodes):
            self.restart_node(i, extra_args=self.validator_args(i))
        self.connect_all()
        height = n0.getblockcount()
        self.wait_until(lambda: all(n.getblockcount() >= height + 6 for n in self.nodes), timeout=60)
        self.wait_in_sync()
        assert self.is_pos(n0, n0.getbestblockhash())

        self.log.info("Anchors: the merged miner's tickets, one per mainchain block")
        n0.setbmm(True, n0.getnewaddress(), True, Decimal("0.0001"))
        for _ in range(2):
            self.anchor()
        self.wait_in_sync()
        assert_equal(n0.getposinfo()["lastanchormainheight"], main.getblockcount())

        self.log.info(f"Anchors {SLOTS_PER_ANCHOR} slots apart, as eCash's blocks against 15-second slots: blocks of stake go on")
        self.wait_until(lambda: len(main.getrawmempool()) > 0, timeout=60)
        anchors = n0.getposinfo()["anchors"]
        height = n0.getblockcount()
        time.sleep(SLOTS_PER_ANCHOR)
        assert_equal(n0.getposinfo()["anchors"], anchors)
        assert_greater_than(n0.getblockcount(), height + SLOTS_PER_ANCHOR // 2)
        assert_equal(n0.getposinfo()["stalled"], False)
        self.anchor()
        self.wait_in_sync()
        assert_peg_balance(main, self.nodes)

        self.log.info("A withdrawal in a block of stake: a bundle, voted up by the anchors' blocks, paid out (M6)")
        payout_address = main.getnewaddress()
        withdrawal = n0.createwithdrawal(payout_address, 3, Decimal("0.01"))
        self.wait_until(lambda: n0.gettransaction(withdrawal["txid"])["confirmations"] > 0, timeout=30)
        for _ in range(8):
            time.sleep(3)
            self.anchor()
            if n0.getwithdrawalbundle()["status"] != "next":
                break
        self.wait_in_sync()
        bundle = n0.getwithdrawalbundle()
        assert_equal(bundle["status"], "pending")
        for _ in range(12):
            if main.getwithdrawalbundle(SLOT, bundle["hash"])["status"] == "paid":
                break
            self.anchor()
        assert_equal(main.getwithdrawalbundle(SLOT, bundle["hash"]), {"status": "paid"})
        self.anchor()
        self.wait_until(lambda: main.getreceivedbyaddress(payout_address) == 3)
        self.wait_until(lambda: n0.listwithdrawals() == [], timeout=60)
        self.wait_in_sync()
        assert_peg_balance(main, self.nodes)
        n0.setbmm(False)


if __name__ == '__main__':
    LayersEcashTest(__file__).main()
