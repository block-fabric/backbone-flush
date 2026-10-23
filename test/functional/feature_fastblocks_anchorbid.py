#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A validator's anchorer bidding 0, and passing the fees into its bid (-validatoranchorbidmode=fees).

The mainchain is a cooperating miner: Backbone with -bmmzerofee (it takes BMM requests that pay no fee), looked
for in MAINCHAIN_BIN_DIR (a build of the bmm-zero branch); the test is skipped without one. Three genesis
validators, 15-second slots on mock time, as feature_fastblocks_idle.py:

  - -validatoranchorbid=0, no fees: every request pays the mainchain 0, and anchors land;
  - fees mode, floor 0: a transaction's fees on the sidechain; the requests pay what the pot pays the
    ticket's payee for the anchor (on this chain, as before), and the pot accrued is the bidder's share of
    the transaction's fee (BidderShare at the next block's bidderbps): the share passed into the bid, once;
  - fees mode with a cap (-validatoranchorbidmax): the request pays the cap, the payee still the whole pot.
"""

from decimal import Decimal

import feature_sidechain
from feature_fastblocks_idle import FastBlocksIdleTest
from test_framework.util import assert_equal, assert_greater_than, assert_greater_than_or_equal

COIN = 100_000_000
# Backbone's node program.
feature_sidechain.MAINCHAIN_DAEMON = "backbone" + "d"


def sats(value):
    return int(Decimal(value) * COIN)


class FastBlocksAnchorBidTest(FastBlocksIdleTest):
    def set_test_params(self):
        super().set_test_params()
        self.main_extra_args = ["-bmmzerofee"]
        self.bid_args = ["-validatoranchorbid=0"]

    def run_test(self):
        try:
            self.run_sidechain_test()
        finally:
            self.stop_mainchain()

    def validator_args(self, i):
        return self.side_args + [f"-validatorkeyfile={self.key_files[i]}"] + self.bid_args

    def restart_validators(self, bid_args):
        self.bid_args = bid_args
        for i in range(3):
            self.restart_node(i, extra_args=self.validator_args(i) + [f"-mocktime={self.now}"])
        self.connect_all()
        self.set_time(self.now)

    def request_fee(self, timeout=120):
        """The fee, in satoshis, of the ticket request in the mainchain's mempool (one per sidechain)."""
        self.wait_until(lambda: self.main.getrawmempool(), timeout=timeout)
        entries = self.main.getrawmempool(True)
        assert_equal(len(entries), 1)
        return sats(next(iter(entries.values()))["fees"]["base"])

    def pot(self):
        info = self.nodes[0].getposinfo()
        return sats(info["pot"]["accrued"]), sats(info["pot"]["paid"]), info["anchors"]

    def window(self, blocks):
        """`blocks` mainchain blocks, each with the request in the mempool for it, three slots apart, then slots until
        every commitment is acknowledged: the requests' fees, and what the anchors paid out of the pot."""
        _, paid, anchors = self.pot()
        fees = []
        for _ in range(blocks):
            fees.append(self.request_fee())
            self.set_time(self.now + 1)
            self.mine_main()
            for _ in range(3):
                self.next_slot()
        for _ in range(10):
            if self.pot()[2] >= anchors + blocks:
                break
            self.next_slot()
        _, paid_after, anchors_after = self.pot()
        # (A commitment now and then is not acknowledged: two mainchain blocks may commit to one ticket, as in
        # feature_fastblocks_idle.py, which asks for 90 %.)
        assert_greater_than_or_equal(anchors_after, anchors + blocks - 1)
        return fees, paid_after - paid

    def confirm(self, txid):
        n0 = self.nodes[0]
        for _ in range(20):
            if n0.gettransaction(txid)["confirmations"] > 0:
                return
            self.next_slot()
        raise AssertionError(f"{txid} not in a block")

    def run_sidechain_test(self):
        main = self.main
        n0, n1, n2 = self.nodes
        self.main_address = main.getnewaddress()
        self.setup_chain_and_validators()

        self.log.info("-validatoranchorbid=0, no fees: requests of 0, mined by the cooperating miner; anchors land")
        self.next_slot()
        fees, paid = self.window(4)
        assert_equal((fees, paid), ([0] * 4, 0))
        assert_equal(len({n.getbestblockhash() for n in self.nodes}), 1)

        self.log.info("Fees mode, floor 0: the bidder's share of the fees passed into the bid, once")
        self.restart_validators(["-validatoranchorbid=0", "-validatoranchorbidmode=fees"])
        self.next_slot()
        self.window(2)
        accrued_before, paid_before, _ = self.pot()
        assert_equal(accrued_before, paid_before)
        bps = n0.getposinfo()["feesplit"]["bidderbps"]
        txid = n0.sendtoaddress(address=n1.getnewaddress(), amount=1, fee_rate=50)
        tx_fee = -sats(n0.gettransaction(txid)["fee"])
        self.confirm(txid)
        fees, paid = self.window(3)
        accrued, out, _ = self.pot()
        share = tx_fee * bps // 10000
        self.log.info(f"fee {tx_fee} sat at {bps} bps: share {share}, accrued {accrued - accrued_before}, bids {fees}, paid {paid}")
        assert_greater_than(share, 0)
        assert_equal(accrued - accrued_before, share)
        assert_equal(accrued, out)
        assert_equal(paid, share)
        # The request whose anchor paid the pot bid it whole; the others nothing (no fees left to pass on).
        assert_equal(sorted(fees), [0] * (len(fees) - 1) + [share])

        self.log.info("Fees mode with a cap: the request pays the cap; the payee still the whole pot")
        cap = 7
        self.restart_validators(["-validatoranchorbid=0", "-validatoranchorbidmode=fees", f"-validatoranchorbidmax=0.0000000{cap}"])
        self.next_slot()
        self.window(2)
        txid = n0.sendtoaddress(address=n1.getnewaddress(), amount=1, fee_rate=50)
        self.confirm(txid)
        fees, paid = self.window(3)
        self.log.info(f"bids {fees}, paid {paid}")
        assert_greater_than(paid, cap)
        assert all(f <= cap for f in fees)
        assert_equal(max(fees), cap)
        assert_equal(len({n.getbestblockhash() for n in self.nodes}), 1)

    def setup_chain_and_validators(self):
        """As feature_fastblocks_idle.py: slot active, a deposit to node 0 in a BMM block, the validators on mock time."""
        import os
        import time
        from feature_sidechain import ACTIVATION_PERIOD
        from test_framework.fastblocks import KEYS, SLOT
        main = self.main
        n0 = self.nodes[0]
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
        start = max(int(time.time()), main.getblock(main.getbestblockhash())["time"]) + 5
        self.now = start - start % 15
        self.restart_validators(self.bid_args)


if __name__ == '__main__':
    FastBlocksAnchorBidTest(__file__).main()
