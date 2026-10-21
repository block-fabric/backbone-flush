#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""FastBlocks with -txindex: transactions of a block of stake are found.

A block of stake has an extended header (176 bytes: the 80 of a header, hashSections and the leader's
signature). The txindex once counted every header as 80 bytes, so getrawtransaction answered "No such
mempool or blockchain transaction" for each transaction of a block of stake.

  - a transaction mined in a BMM block and one mined in a block of stake: getrawtransaction finds both
    (without the block hash), and the coinbases of both blocks;
  - after a restart, too;
  - the upgrade: an index written by a node of before the fix (FASTBLOCKS_PREVIOUS_BIN, a directory
    with its fastblocksd, or Layers' flushd; the step is skipped without it) does not find the transaction of the block
    of stake; the node of now rebuilds that index when it starts, and finds both.
"""

import os
import shutil
from decimal import Decimal

from feature_sidechain import SidechainTest
from test_framework.fastblocks import SLOT, XONLY, activate_and_fund, bmm_block, write_key_file
from test_framework.util import assert_equal, assert_raises_rpc_error, rpc_port

POS_BLOCK_BIT = 0x10000
RPC_INVALID_ADDRESS_OR_KEY = -5


class FastBlocksTxindexTest(SidechainTest):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.main_rpc_port = rpc_port(self.num_nodes)
        self.side_args = [
            f"-sidechainslot={SLOT}",
            f"-mainchainrpcport={self.main_rpc_port}",
            "-mainchainrpcuser=main",
            "-mainchainrpcpassword=secret",
            "-mainchainrpcwallet=miner",
            f"-posvalidators={XONLY[0]}",
            "-txindex",
        ]
        self.extra_args = [self.side_args]
        self.main_process = None

    def setup_network(self):
        self.start_mainchain()
        self.setup_nodes()

    def run_test(self):
        try:
            self.run_sidechain_test()
        finally:
            self.stop_mainchain()

    def is_pos(self, blockhash):
        return bool(self.nodes[0].getblockheader(blockhash)["version"] & POS_BLOCK_BIT)

    def wait_synced_index(self):
        node = self.nodes[0]
        self.wait_until(lambda: node.getindexinfo("txindex")["txindex"]["synced"], timeout=60)
        assert_equal(node.getindexinfo("txindex")["txindex"]["best_block_height"], node.getblockcount())

    def assert_found(self, txs):
        node = self.nodes[0]
        for txid, blockhash in txs:
            tx = node.getrawtransaction(txid, True)
            assert_equal(tx["txid"], txid)
            assert_equal(tx["blockhash"], blockhash)

    def run_sidechain_test(self):
        main = self.main
        node = self.nodes[0]
        self.main_address = main.getnewaddress()

        self.log.info("A transaction mined in a BMM block (no validator running yet)")
        activate_and_fund(self)
        txid_bmm = node.sendtoaddress(node.getnewaddress(), Decimal("1"))
        bmm_hash = bmm_block(self, node)
        assert not self.is_pos(bmm_hash)
        assert txid_bmm in node.getblock(bmm_hash)["tx"]

        self.log.info("A transaction mined in a block of stake")
        validator_args = self.side_args + [f"-validatorkeyfile={write_key_file(self, 0)}"]
        self.restart_node(0, extra_args=validator_args)
        txid_pos = node.sendtoaddress(node.getnewaddress(), Decimal("2"))
        self.wait_until(lambda: node.gettransaction(txid_pos)["confirmations"] > 0, timeout=60)
        pos_hash = node.gettransaction(txid_pos)["blockhash"]
        assert self.is_pos(pos_hash)
        txs = [
            (txid_bmm, bmm_hash),
            (txid_pos, pos_hash),
            (node.getblock(bmm_hash)["tx"][0], bmm_hash),
            (node.getblock(pos_hash)["tx"][0], pos_hash),
        ]
        self.wait_synced_index()
        self.assert_found(txs)

        self.log.info("After a restart")
        self.restart_node(0)
        self.wait_synced_index()
        self.assert_found(txs)

        previous = os.environ.get("FASTBLOCKS_PREVIOUS_BIN")
        if not previous:
            self.log.info("No FASTBLOCKS_PREVIOUS_BIN: the upgrade from an index of before the fix is not tested")
            return
        self.log.info("An index written by a node of before the fix misses the transaction of the block of stake")
        self.stop_node(0)
        shutil.rmtree(node.chain_path / "indexes" / "txindex")
        argv_len = len(node.binaries.node_argv())
        args = node.args
        # fastblocksd, or the daemon of a chain built on FastBlocks (Layers: flushd).
        daemon = next((os.path.join(previous, d) for d in ("fastblocksd", "flushd") if os.path.exists(os.path.join(previous, d))), os.path.join(previous, "fastblocksd"))
        node.args = [daemon] + args[argv_len:]
        node.start()
        node.wait_for_rpc_connection()
        self.wait_synced_index()
        assert_raises_rpc_error(RPC_INVALID_ADDRESS_OR_KEY, "No such mempool or blockchain transaction", node.getrawtransaction, txid_pos)
        self.assert_found([txs[0], txs[2]])
        self.stop_node(0)
        node.args = args

        self.log.info("The node of now rebuilds it when it starts, and finds both")
        with node.assert_debug_log(["it is rebuilt from the genesis block"]):
            self.start_node(0)
        self.wait_synced_index()
        self.assert_found(txs)
        # Once: the rebuilt index is kept at the next start.
        with node.assert_debug_log([], unexpected_msgs=["it is rebuilt from the genesis block"]):
            self.restart_node(0)
        self.wait_synced_index()
        self.assert_found(txs)


if __name__ == '__main__':
    FastBlocksTxindexTest(__file__).main()
