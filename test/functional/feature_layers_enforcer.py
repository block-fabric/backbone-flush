#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Layers following its mainchain through the bip300301_enforcer's interface alone (-mainchaingrpc).

feature_layers_ecash.py's scenario (three validators against backboned -regtest -testecash: activation, a
deposit, blocks of stake, anchors one per mainchain block and 40 slots apart, a withdrawal from a block of
stake bundled, voted and paid out, the peg balance), with the Layers nodes reaching Backbone through its
enforcer interface (-grpc) only: no JSON-RPC access (-mainchainrpcport names a port nothing listens on, no
cookie), BMM blocks and the anchors' tickets through CreateBmmCriticalDataTransaction, the bundle handed over
(ProposeWithdrawalBundle) once its commitment is two mainchain blocks deep, the rules of the eCash networks
(doc/ecash-enforcer.md).
"""

import os
from decimal import Decimal

from feature_layers_ecash import LayersEcashTest
from feature_sidechain_enforcer import free_port
from test_framework.authproxy import JSONRPCException
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class LayersEnforcerTest(LayersEcashTest):
    def set_test_params(self):
        super().set_test_params()
        # No JSON-RPC access to the mainchain: no user, password or wallet (the port is set in setup_network).
        self.side_args[:] = [arg for arg in self.side_args if not arg.startswith("-mainchainrpc")]

    def setup_network(self):
        self.grpc_port = free_port()
        self.no_rpc_port = free_port(avoid=(self.grpc_port,))
        no_cookie_dir = os.path.join(self.options.tmpdir, "no-mainchain-datadir")
        os.makedirs(no_cookie_dir, exist_ok=True)
        self.side_args += [
            f"-mainchaingrpc=127.0.0.1:{self.grpc_port}",
            "-mainchaingrpcauth=tok",
            f"-mainchainrpcport={self.no_rpc_port}",
            f"-mainchaindatadir={no_cookie_dir}",
        ]
        # Backbone with its enforcer interface (-grpc, its wallet, the token): SidechainEcashTest.start_mainchain.
        self.options.mainchaingrpc = True
        self.start_mainchain()
        BitcoinTestFramework.setup_network(self)

    def bmm_block(self, node, address):
        """A BMM block the node asks for itself (CreateBmmCriticalDataTransaction)."""
        request = node.requestbmmblock(address, Decimal("0.001"))
        self.mine_main()
        self.wait_until(lambda: all(n.getbestblockhash() == request["hash"] for n in self.nodes))
        return request["hash"]

    def run_test(self):
        super().run_test()

    @staticmethod
    def tip_only_check(node):
        def check():
            try:
                # (Looked up at each call: a restarted node has a new connection.)
                return node.__getattr__("checkpegbalance")()
            except JSONRPCException as e:
                assert "known at the mainchain tip only" in e.error["message"], e.error
                return {"mainheight": -1}
        return check

    def run_ecash_test(self):
        for node in self.nodes:
            assert_equal(node.getmainchaininfo()["via"], "enforcer")
            # checkpegbalance knows the escrow at the mainchain tip only (GetCtip): asked while the chain has acted
            # on an earlier block, it says so, and the peg balance is checked against the test's view of the escrow.
            node.checkpegbalance = self.tip_only_check(node)
        super().run_ecash_test()
        # No JSON-RPC call to the mainchain was made: none could be.
        for node in self.nodes:
            assert_equal(node.getmainchaininfo()["via"], "enforcer")
            with open(node.debug_log_path, encoding="utf-8") as log:
                assert f"127.0.0.1:{self.no_rpc_port}" not in log.read()


if __name__ == '__main__':
    LayersEnforcerTest(__file__).main()
