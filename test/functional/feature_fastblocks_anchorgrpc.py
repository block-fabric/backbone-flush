#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Misses that are not ours, through the enforcer's interface alone (-mainchaingrpc), as every mainnet validator reaches
the mainchain: Backbone's interface, or the pool's enforcer.

feature_layers_enforcer.py's setup (three validators against backboned -regtest -testecash -grpc, the nodes with no
JSON-RPC access to it), a second sidechain active in slot 6 whose BMM requests the test makes, and node 0's anchorer
bidding a fixed amount every mainchain block (the others' off). Without getblock, the node learns from GetSidechains
and GetBlockInfo of each active slot whether a mainchain block carried BMM, and its header time:

  - a block without any BMM commitment (generateblock with no transactions): nobmm, no miss, no raise of the next bid;
  - a block with the other slot's BMM, found 1 s after the bid (its header time): toosoon, no miss;
  - a block with the other slot's BMM 10 s after the bid, without ours: a miss, and the next bid raised above it;
  - getbmminfo's anchoring counts them apart.
"""

import os
import time
from decimal import Decimal

from feature_layers_ecash import KEYS
from feature_layers_enforcer import LayersEnforcerTest
from feature_sidechain_ecash import UNUSED_SLOT_THRESHOLD
from test_framework.fastblocks import SLOT
from test_framework.util import assert_equal

OTHER_SLOT = 6
COIN = 100_000_000


def sats(value):
    return int(round(float(value) * COIN))


class FastBlocksAnchorGrpcTest(LayersEnforcerTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        super().run_test()

    def validator_args(self, i):
        bid = ["-validatoranchorbid=0.00001", "-validatoranchorbidstrategy=fixed", "-novalidatoranchorbidpreventstall"] if i == 0 else ["-novalidatoranchorbid"]
        return super().validator_args(i) + bid

    def ours(self):
        """This chain's requests in the mainchain's mempool for the block after its tip."""
        tip = bytes.fromhex(self.main.getbestblockhash())[::-1].hex()
        found = []
        for txid in self.main.getrawmempool():
            for out in self.main.getrawtransaction(txid, True)["vout"]:
                script = out["scriptPubKey"]["hex"]
                if len(script) == 140 and script.startswith("6a4400bf00") and int(script[10:12], 16) == SLOT and script[76:] == tip:
                    found.append(txid)
        return found

    def new_bid(self):
        """Node 0's bid for the next mainchain block, in the mempool."""
        n0, main = self.nodes[0], self.main
        def made():
            history = n0.getanchorinfo(1)["bid"]["history"]
            return self.ours() and history and history[0]["mainheight"] == main.getblockcount() + 1 and history[0]["status"] == "pending"
        self.wait_until(made, timeout=60)
        return n0.getanchorinfo(1)["bid"]["history"][0]

    def settled(self, ticket, status):
        self.wait_until(lambda: any(h["ticket"] == ticket and h["status"] == status for h in self.nodes[0].getanchorinfo(50)["bid"]["history"]), timeout=60)

    def without_ours(self, at, other_bmm):
        """A mainchain block at header time `at` (mock time on the mainchain), without this chain's request, with the other
        slot's if `other_bmm`."""
        main = self.main
        for txid in self.ours():
            main.prioritisetransaction(txid=txid, fee_delta=-100_000_000)
        if other_bmm:
            main.createbmmrequest(OTHER_SLOT, os.urandom(32).hex(), Decimal("0.001"))
        main.setmocktime(at)
        if other_bmm:
            self.mine_main()
        else:
            main.generateblock(self.main_address, [])
            for node in self.nodes:
                node.syncmainchain()
        main.setmocktime(0)

    def run_ecash_test(self):
        main = self.main
        n0 = self.nodes[0]
        for node in self.nodes:
            assert_equal(node.getmainchaininfo()["via"], "enforcer")
        self.main_address = main.getnewaddress()

        self.log.info("Activate this slot and another, a deposit, a BMM block; the validators start, node 0's anchorer bidding")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "Flush", "Tokens, assets and a DEX on a fast chain over eCash", "11" * 32, "22" * 20)
        self.mine_main(UNUSED_SLOT_THRESHOLD + 2)
        # (The other after it: the miner acks one proposal at a time.)
        main.createsidechainproposal(OTHER_SLOT, "Other", "Another sidechain, for BMM without this chain's", "33" * 32, "44" * 20)
        self.mine_main(UNUSED_SLOT_THRESHOLD + 2)
        assert_equal(sorted(s["slot"] for s in main.listactivesidechains()), [SLOT, OTHER_SLOT])
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 50)
        self.mine_main()
        self.bmm_block(n0, n0.getnewaddress())
        self.payouts = [n.getnewaddress() for n in self.nodes]
        for i, key in enumerate(KEYS):
            with open(self.key_file(i), "w") as f:
                f.write("# validator key\n" + key.hex() + "\n")
        for i in range(self.num_nodes):
            self.restart_node(i, extra_args=self.validator_args(i))
        self.connect_all()
        height = n0.getblockcount()
        self.wait_until(lambda: all(n.getblockcount() >= height + 3 for n in self.nodes), timeout=60)
        # One anchor of its own first (its bid lands).
        ticket = self.new_bid()["ticket"]
        self.mine_main()
        self.settled(ticket, "landed")
        # Mainchain blocks at header times this test sets: the median time of the last ones behind the clock first.
        mtp = main.getblockchaininfo()["mediantime"]
        if mtp > time.time() - 5:
            time.sleep(mtp - time.time() + 6)
        before = n0.getbmminfo()["anchoring"]["day"]

        self.log.info("A block without any BMM commitment: nobmm, no miss, the next bid not raised")
        bid = self.new_bid()
        self.without_ours(at=bid["time"] + 10, other_bmm=False)
        self.settled(bid["ticket"], "nobmm")
        bid = self.new_bid()
        info = n0.getanchorinfo()["bid"]
        assert_equal((info["missed"], sats(bid["offer"])), (0, 1000))

        self.log.info("A block with the other slot's BMM found 1 s after the bid (its header time): toosoon, no miss")
        self.without_ours(at=bid["time"] + 1, other_bmm=True)
        self.settled(bid["ticket"], "toosoon")
        bid = self.new_bid()
        assert_equal((n0.getanchorinfo()["bid"]["missed"], sats(bid["offer"])), (0, 1000))

        self.log.info("A block with the other slot's BMM 10 s after the bid, without ours: a miss; the next bid above it")
        self.without_ours(at=bid["time"] + 10, other_bmm=True)
        self.settled(bid["ticket"], "missed")
        bid = self.new_bid()
        assert_equal((n0.getanchorinfo()["bid"]["missed"], sats(bid["offer"]), bid["kind"]), (1, 1500, "replace"))
        day = n0.getbmminfo()["anchoring"]["day"]
        assert_equal((day["nobmm"] - before["nobmm"], day["toosoon"] - before["toosoon"], day["missed"] - before["missed"]), (1, 1, 1))
        # (And then it lands.)
        self.mine_main()
        self.settled(bid["ticket"], "landed")
        # No JSON-RPC call to the mainchain was made: none could be.
        for node in self.nodes:
            assert_equal(node.getmainchaininfo()["via"], "enforcer")
            with open(node.debug_log_path, encoding="utf-8") as log:
                assert f"127.0.0.1:{self.no_rpc_port}" not in log.read()


if __name__ == '__main__':
    FastBlocksAnchorGrpcTest(__file__).main()
