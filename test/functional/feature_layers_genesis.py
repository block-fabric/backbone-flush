#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The first block of every network: what layersd has at height 0 (getblockhash 0) is what layers-util
getchainparams reports and what the test data of the util (data/util/getchainparams-*.json) and
doc/layers.md say.

layers-util once printed the hash in the bytes' internal order (upstream's HexStr of the uint256), the
reverse of every RPC's: the test network's 252a6441...aff5 showed as f5afe992...2a25. This test makes
both agree on every network, so that a change of either is seen.
"""
import json
import subprocess
from pathlib import Path

from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal, p2p_port, write_config

# The first blocks of Layers (the test network's is the one of its restart of 19/Oct/2026: it changes
# only with a restart of the test network).
GENESIS = {
    "main": "164aeae68926b8814474e6f1580a6253df0c04d53826ea715400a8070dab0857",
    "testnet": "6ad3a3ff068fecfba95fc4aeec8404ac1db31cb840b852164db054ff31daa382",
    "signet": "68c4854ae0f132e80760c9089066ffb4a7ef6ded09387e941f3430edbf45e101",
    "regtest": "0f9188f13cb7b2c71f2a335e3a4fc328bf5beb436012afca590b1a11466e2206",
}
# The networks' names: of the directory (and the test framework), of the util's option, of the data file.
NETWORKS = [("", "-chain=main", "mainnet", "main"), ("testnet", "-testnet", "testnet", "testnet"),
            ("signet", "-signet", "signet", "signet"), ("regtest", "-regtest", "regtest", "regtest")]


class LayersGenesisTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_bitcoin_util()

    def run_test(self):
        node = self.nodes[0]
        datadir = Path(self.config["environment"]["SRCDIR"]) / "test" / "functional" / "data" / "util"
        doc = (Path(self.config["environment"]["SRCDIR"]) / "doc" / "layers.md").read_text()
        # No mainchain: a port nobody listens on, a user and a password so that no cookie of a running
        # mainchain node is read.
        no_mainchain = [f"-mainchainrpcport={p2p_port(1)}", "-mainchainrpcuser=none", "-mainchainrpcpassword=none"]
        node.stop_node()
        for chain_dir, util_arg, data_name, name in NETWORKS:
            self.log.info(f"The first block of {name}")
            write_config(node.datadir_path / "layers.conf", n=0, chain=chain_dir)
            node.chain = chain_dir
            if name == "main":
                # The main network refuses to start until slot 7's activation height is named
                # (doc/layers.md): its first block is checked against the chain parameters by the unit
                # test layers_tests/genesis_hashes instead.
                node.assert_start_raises_init_error(no_mainchain, "it must not run on the main network", match=ErrorMatch.PARTIAL_REGEX)
                daemon = GENESIS[name]
            else:
                node.start(extra_args=no_mainchain)
                node.wait_for_rpc_connection()
                daemon = node.getblockhash(0)
                node.stop_node()
            util = json.loads(subprocess.run(self.get_binaries().util_argv() + [util_arg, "getchainparams"],
                                             capture_output=True, text=True, check=True).stdout)["genesis"]
            data = json.loads((datadir / f"getchainparams-{data_name}.json").read_text())["genesis"]
            assert_equal(daemon, GENESIS[name])
            assert_equal(util, daemon)
            assert_equal(data, daemon)
            if name != "regtest":
                assert daemon in doc, f"doc/layers.md lacks the first block of {name}"
        # Back to the test's network, for the framework's end of the test.
        write_config(node.datadir_path / "layers.conf", n=0, chain=self.chain)
        node.chain = self.chain
        node.start()
        node.wait_for_rpc_connection()


if __name__ == '__main__':
    LayersGenesisTest(__file__).main()
