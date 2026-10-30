#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""FastBlocks: the fewer-validators hard fork (PosParams::min_validators_fork_epoch).

Three validators (two keys on node 0, one on node 1), a minimum of 4 before the fork epoch and 3 from it
(-testposparam): no block of stake before the fork epoch, blocks of stake from it. Node 2 has no fork (a
minimum of 4 for ever, as a node of before the fork): it refuses those blocks of stake (bad-pos-inactive) and
stays on the merged mined chain.
"""

import os
import time
from decimal import Decimal

from feature_sidechain import ACTIVATION_PERIOD, SidechainTest
from test_framework.fastblocks import KEYS, REGTEST_GENESIS_TIME, SLOT, XONLY
from test_framework.util import assert_equal, rpc_port

POS_BLOCK_BIT = 0x10000
SLOTS_PER_EPOCH = 20


class FastBlocksMinValidatorsTest(SidechainTest):
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
            "-testposparam=min_active_validators@4",
            "-testposparam=min_active_validators_after@3",
        ]
        self.extra_args = [self.side_args] * self.num_nodes
        self.main_process = None

    def setup_network(self):
        self.start_mainchain()
        self.setup_nodes()
        self.connect_all()

    def connect_all(self):
        for a, b in ((0, 1), (1, 2), (0, 2)):
            self.connect_nodes(a, b)

    def is_pos(self, node, blockhash):
        return bool(node.getblockheader(blockhash)["version"] & POS_BLOCK_BIT)

    def run_test(self):
        try:
            self.run_sidechain_test()
        finally:
            self.stop_mainchain()

    def run_sidechain_test(self):
        main = self.main
        n0, n1, n2 = self.nodes
        self.main_address = main.getnewaddress()

        self.log.info("Activate the slot; a set of 3 with a minimum of 4: not active")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "FastBlocks", "Blocks of stake between merged mined blocks", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        assert_equal(n0.getposinfo()["active"], False)
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 50)
        self.mine_main()
        block = n0.createbmmblock(n0.getnewaddress())
        main.createbmmrequest(SLOT, block["hash"], Decimal("0.001"))
        self.mine_main()
        self.wait_until(lambda: all(n.getbestblockhash() == block["hash"] for n in self.nodes))
        bmm_tip = block["hash"]

        # The fork a few epochs ahead of the clock (an epoch is 20 one-second slots on regtest).
        fork_epoch = (int(time.time()) - REGTEST_GENESIS_TIME) // SLOTS_PER_EPOCH + 3
        fork_args = [f"-testposparam=min_validators_fork_epoch@{fork_epoch}"]
        self.log.info(f"Start the validators, the fork at epoch {fork_epoch} on nodes 0 and 1; node 2 without it")
        files = []
        for i, keys in enumerate((KEYS[:2], KEYS[2:])):
            path = os.path.join(self.options.tmpdir, f"validator{i}.key")
            with open(path, "w") as f:
                f.write("".join(key.hex() + "\n" for key in keys))
            files.append(path)
        self.restart_node(0, extra_args=self.side_args + fork_args + [f"-validatorkeyfile={files[0]}"])
        self.restart_node(1, extra_args=self.side_args + fork_args + [f"-validatorkeyfile={files[1]}"])
        self.restart_node(2, extra_args=self.side_args)
        self.connect_all()

        self.log.info("Before the fork epoch: no block of stake, on any node")
        assert_equal(n0.getposinfo()["active"], False)
        time.sleep(3)
        for n in self.nodes:
            assert_equal(n.getbestblockhash(), bmm_tip)

        self.log.info("From the fork epoch: blocks of stake on the forked nodes; node 2 refuses them")
        with n2.assert_debug_log(["bad-pos-inactive"], timeout=180):
            self.wait_until(lambda: n0.getblockcount() >= n2.getblockcount() + 3, timeout=180)
        assert_equal(n0.getposinfo()["active"], True)
        assert_equal(n2.getposinfo()["active"], False)
        self.wait_until(lambda: n1.getbestblockhash() == n0.getbestblockhash(), timeout=60)
        assert self.is_pos(n0, n0.getbestblockhash())
        tip_slot = n0.getblock(n0.getbestblockhash())["slot"]
        assert tip_slot // SLOTS_PER_EPOCH >= fork_epoch
        # The first block of stake is in the fork epoch or later; node 2 stays on the merged mined chain.
        assert_equal(n2.getbestblockhash(), bmm_tip)


if __name__ == '__main__':
    FastBlocksMinValidatorsTest(__file__).main()
