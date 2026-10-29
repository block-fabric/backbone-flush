#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The first block of every network (Flush's on eCash, and Layers' on Chains, kept for development): what flushd has at height 0 (getblockhash 0) is what flush-util
getchainparams reports and what the test data of the util (data/util/getchainparams-*.json) and
doc/layers.md say.

flush-util once printed the hash in the bytes' internal order (upstream's HexStr of the uint256), the
reverse of every RPC's: the test network's 252a6441...aff5 showed as f5afe992...2a25. This test makes
both agree on every network, so that a change of either is seen.
"""
import json
import subprocess
from pathlib import Path

from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal, p2p_port, write_config

# The first blocks of Layers on Chains, which Flush keeps for development (-chainsnetworks; the test
# network's is the one of its restart of 19/Oct/2026), and of Flush on eCash.
GENESIS = {
    "main": "164aeae68926b8814474e6f1580a6253df0c04d53826ea715400a8070dab0857",
    "testnet": "6ad3a3ff068fecfba95fc4aeec8404ac1db31cb840b852164db054ff31daa382",
    "signet": "68c4854ae0f132e80760c9089066ffb4a7ef6ded09387e941f3430edbf45e101",
    "regtest": "0f9188f13cb7b2c71f2a335e3a4fc328bf5beb436012afca590b1a11466e2206",
    "ecash": "19b25cff92c2beec1032a50d1eff87b01e1d67e658e25ca099fe599213843662",
    "ecashbeta": "7d66b10e351e5ad86c562ba761b5476f75c6f1b9f13aa7cf469df3a8d5701b75",
    "ecashtest": "2ee6a1d1e193d4115dedda242b367454e028804ba88618ba96b728789466f38d",
}
# The networks: the directory (and the test framework's name), -chain=, the section of the configuration file,
# the util's options, the data file, whether it is a Chains network (-chainsnetworks), the document that lists it.
NETWORKS = [
    ("", "main", "main", ["-chain=main", "-chainsnetworks"], "mainnet", "main", True, "doc/layers.md"),
    ("testnet", "test", "test", ["-testnet", "-chainsnetworks"], "testnet", "testnet", True, "doc/layers.md"),
    ("signet", "signet", "signet", ["-signet", "-chainsnetworks"], "signet", "signet", True, "doc/layers.md"),
    ("regtest", "regtest", "regtest", ["-regtest"], "regtest", "regtest", False, None),
    ("", "ecash", "ecash", ["-chain=ecash"], "ecash", "ecash", False, "README.md"),
    ("betanet", "ecashbeta", "ecashbeta", ["-chain=ecashbeta"], "ecashbeta", "ecashbeta", False, "README.md"),
    ("testnet4", "ecashtest", "ecashtest", ["-chain=ecashtest"], "ecashtest", "ecashtest", False, "README.md"),
]


class FlushGenesisTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_bitcoin_util()

    def run_test(self):
        node = self.nodes[0]
        datadir = Path(self.config["environment"]["SRCDIR"]) / "test" / "functional" / "data" / "util"
        srcdir = Path(self.config["environment"]["SRCDIR"])
        # No mainchain: a port nobody listens on, a user and a password so that no cookie of a running
        # mainchain node is read.
        no_mainchain = [f"-mainchainrpcport={p2p_port(1)}", "-mainchainrpcuser=none", "-mainchainrpcpassword=none"]
        node.stop_node()
        conf = node.datadir_path / "flush.conf"
        for chain_dir, chain_arg, section, util_args, data_name, name, chains_network, doc in NETWORKS:
            self.log.info(f"The first block of {name}")
            write_config(conf, n=0, chain="regtest")
            text = conf.read_text().replace("chainsnetworks=1\n", "", 1).replace("regtest=1\n[regtest]\n", f"chain={chain_arg}\n" + ("chainsnetworks=1\n" if chains_network else "") + f"[{section}]\n", 1)
            conf.write_text(text)
            node.chain = chain_dir
            if name == "main":
                # Chains' main network refuses to start until the slot's activation height is named
                # (doc/layers.md): its first block is checked against the chain parameters by the unit
                # test layers_tests/genesis_hashes instead. eCash mainnet names it (slot 46, block 978,331).
                node.assert_start_raises_init_error(no_mainchain, "it must not run on the main network", match=ErrorMatch.PARTIAL_REGEX)
                daemon = GENESIS[name]
            else:
                node.start(extra_args=no_mainchain)
                node.wait_for_rpc_connection()
                daemon = node.getblockhash(0)
                node.stop_node()
            util = json.loads(subprocess.run(self.get_binaries().util_argv() + util_args + ["getchainparams"],
                                             capture_output=True, text=True, check=True).stdout)["genesis"]
            data = json.loads((datadir / f"getchainparams-{data_name}.json").read_text())["genesis"]
            assert_equal(daemon, GENESIS[name])
            assert_equal(util, daemon)
            assert_equal(data, daemon)
            if doc:
                assert daemon in (srcdir / doc).read_text(), f"{doc} lacks the first block of {name}"
            if chains_network:
                # Flush: a Chains network only with -chainsnetworks.
                write_config(conf, n=0, chain="regtest")
                conf.write_text(conf.read_text().replace("chainsnetworks=1\n", "", 1).replace("regtest=1\n[regtest]\n", f"chain={chain_arg}\n[{section}]\n", 1))
                node.assert_start_raises_init_error(no_mainchain, "needs -chainsnetworks", match=ErrorMatch.PARTIAL_REGEX)
        # Without a network, ecash (Flush's default: its mainnet): the util says so.
        util = json.loads(subprocess.run(self.get_binaries().util_argv() + ["getchainparams"], capture_output=True, text=True, check=True).stdout)
        assert_equal(util["genesis"], GENESIS["ecash"])
        # Back to the test's network, for the framework's end of the test.
        write_config(node.datadir_path / "flush.conf", n=0, chain=self.chain)
        node.chain = self.chain
        node.start()
        node.wait_for_rpc_connection()


if __name__ == '__main__':
    FlushGenesisTest(__file__).main()
