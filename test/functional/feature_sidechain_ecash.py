#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The sidechain against Backbone in its eCash regtest mode: the mainchain of the eCash networks.

The mainchain node is backboned -regtest -testecash (the rules of the eCash networks: OP_DRIVECHAIN as
OP_NOP8, the drivechain rules of the bip300301_enforcer with its regtest thresholds -- a proposal activates
with more than 5 acks in 10 blocks, a bundle is paid with more than 5 votes and fails older than 10 blocks
-- and no single payout), looked for in the directory named by the environment variable MAINCHAIN_BIN_DIR,
or else in ../chains-ecash/build_node/bin next to this source tree. The test is skipped without one. The
nodes of this sidechain run with -testmainenforcer: the parameters of a sidechain of such a mainchain
(SidechainParams::main_single_payout false).

- The sidechain is proposed (M1) and acked (M2) until it activates; its treasury script is OP_NOP8's.
- Blocks by blind merged mining (M7/M8); a deposit (M5) into the OP_NOP8 treasury, paid on the sidechain.
- A withdrawal goes in a bundle (M3), which the mainchain votes up (M4) and pays (M6); the escrow and the
  payout match the bundle.
- A bundle the mainchain does not vote for fails at its maximum age; its withdrawal is refunded.
- Single payout on the sidechain's side: a bundle of another branch, left pending by a mainchain reorg with
  the same withdrawal, holds a new bundle back while it has support ("held"), where on the Chains mainchain a
  new bundle would be made (the mainchain failing one of the two). Paid, its payout is matched: the
  withdrawal is paid once, and the escrow pays it once.
"""

import atexit
import os
import subprocess
import time
from decimal import Decimal
from http.client import HTTPException

from test_framework.authproxy import AuthServiceProxy, JSONRPCException
from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
    p2p_port,
    rpc_port,
)

# The node program of the mainchain of the eCash networks.
MAINCHAIN_DAEMON = "backbone" + "d"

SLOT = 3
# The enforcer's regtest thresholds (Thresholds::SHORT), those of backboned -testecash.
UNUSED_SLOT_THRESHOLD = 5
BUNDLE_THRESHOLD = 5
BUNDLE_MAX_AGE = 10
# Regtest parameters of this chain with -testmainenforcer.
BUNDLE_RETRY_DELAY = 5
PENDING_MIN_SCORE = 2


def fresh_connections(proxy):
    """A connection per call: a node closes a connection left idle for a while, which a busy test can do."""
    proxy.reuse_http_connections = False
    return proxy


class SidechainEcashTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.main_rpc_port = rpc_port(self.num_nodes)
        side_args = [
            f"-sidechainslot={SLOT}",
            "-testmainenforcer",
            f"-mainchainrpcport={self.main_rpc_port}",
            "-mainchainrpcuser=main",
            "-mainchainrpcpassword=secret",
            "-mainchainrpcwallet=miner",
        ]
        self.extra_args = [side_args, side_args]
        self.main_process = None

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def start_mainchain(self):
        bin_dir = os.environ.get("MAINCHAIN_BIN_DIR", os.path.join(self.config["environment"]["SRCDIR"], "..", "chains-ecash", "build_node", "bin"))
        binary = os.path.join(bin_dir, MAINCHAIN_DAEMON)
        if not os.path.isfile(binary):
            raise SkipTest(f"no Backbone node at {binary}; set MAINCHAIN_BIN_DIR")
        if "-testecash" not in subprocess.run([binary, "-help-debug"], capture_output=True, text=True).stdout:
            raise SkipTest(f"{binary} has no eCash regtest mode (-testecash)")
        datadir = os.path.join(self.options.tmpdir, "mainchain")
        os.makedirs(datadir, exist_ok=True)
        self.main_args = [
            binary, "-regtest", "-testecash", f"-datadir={datadir}", f"-rpcport={self.main_rpc_port}", f"-port={p2p_port(self.num_nodes)}",
            "-rpcuser=main", "-rpcpassword=secret", "-fallbackfee=0.0002", "-listen=0", "-server", "-printtoconsole=0",
        ]
        self.main_process = subprocess.Popen(self.main_args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # Whatever happens to the test, the mainchain node must not outlive it.
        atexit.register(self.kill_mainchain, self.main_process)
        url = f"http://main:secret@127.0.0.1:{self.main_rpc_port}"
        for _ in range(240):
            try:
                node = fresh_connections(AuthServiceProxy(url, timeout=120))
                node.getblockcount()
                break
            except (OSError, HTTPException, JSONRPCException):
                time.sleep(0.25)
        else:
            raise AssertionError("the mainchain node did not start")
        if "miner" not in node.listwallets():
            node.createwallet("miner")
        self.main_node = node
        self.main = fresh_connections(AuthServiceProxy(url + "/wallet/miner", timeout=120))

    @staticmethod
    def kill_mainchain(process):
        if process.poll() is None:
            process.kill()

    def stop_mainchain(self):
        if self.main_process is None:
            return
        try:
            self.main.stop()
        except (OSError, HTTPException, JSONRPCException):
            self.main_process.terminate()
        self.main_process.wait(timeout=120)
        self.main_process = None

    def setup_network(self):
        self.start_mainchain()
        super().setup_network()

    def mine_main(self, count=1):
        hashes = self.main.generatetoaddress(count, self.main_address)
        for node in self.nodes:
            node.syncmainchain()
        return hashes

    def bmm(self, node=None, expect=True):
        """Mine one block of the sidechain: build it, have the mainchain commit to it."""
        node = node or self.nodes[0]
        block = node.createbmmblock(self.side_address)
        assert_equal(block["mainchainblockhash"], self.main.getbestblockhash())
        request = self.main.createbmmrequest(SLOT, block["hash"], Decimal("0.001"))
        assert_equal(request["prevblockhash"], block["mainchainblockhash"])
        self.mine_main()
        if expect:
            assert_equal(node.getbestblockhash(), block["hash"])
            self.sync_blocks()
        return block["hash"]

    def check_in_sync(self):
        self.sync_blocks()
        states = [n.getsidechainstate() for n in self.nodes]
        assert_equal(len({(st["bestblock"], st["hash"]) for st in states}), 1)
        assert_equal(self.nodes[0].listwithdrawals(), self.nodes[1].listwithdrawals())
        assert_equal(self.nodes[0].getwithdrawalbundle(), self.nodes[1].getwithdrawalbundle())

    def escrow(self):
        return self.main.getsidechain(SLOT)["escrow"]["amount"]

    def run_test(self):
        try:
            self.run_ecash_test()
        finally:
            self.stop_mainchain()

    def run_ecash_test(self):
        main = self.main
        side, other = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = side.getnewaddress()

        self.log.info("The nodes follow Backbone in its eCash regtest mode")
        info = self.main_node.getdrivechaininfo()
        assert_equal(info["rules"], "enforcer")
        assert_equal(info["singlepayout"], False)
        assert_equal(self.main_node.getblockchaininfo()["network"], "regtest")
        self.mine_main(110)
        info = side.getmainchaininfo()
        assert_equal(info["connected"], True)
        assert_equal(info["height"], 110)

        self.log.info("Proposal (M1) and acks (M2) until the sidechain activates: more than 5 acks in 10 blocks")
        main.createsidechainproposal(SLOT, "Testchain", "The sidechain of this test", "11" * 32, "22" * 20)
        self.mine_main(1)
        assert_equal(main.listactivesidechains(), [])
        # The proposing block's own ack does not count: the 6th ack after it activates.
        self.mine_main(UNUSED_SLOT_THRESHOLD)
        assert_equal(main.listactivesidechains(), [])
        self.mine_main(1)
        assert_equal([s["slot"] for s in main.listactivesidechains()], [SLOT])
        # The treasury script of the eCash networks: OP_NOP8 0x01 <slot> OP_TRUE.
        assert_equal(main.getsidechain(SLOT)["escrowscript"], f"b701{SLOT:02x}51")

        self.log.info("Blocks by blind merged mining (M7/M8)")
        self.bmm()
        assert_equal(side.getblockcount(), 1)
        assert_equal(other.getblockcount(), 1)

        self.log.info("A deposit (M5) into the OP_NOP8 treasury")
        deposit = side.getdepositaddress()
        txid = main.createsidechaindeposit(SLOT, deposit["depositaddress"], 50)["txid"]
        self.mine_main()
        escrow = main.getsidechain(SLOT)["escrow"]
        assert_equal(escrow["txid"], txid)
        assert_equal(escrow["amount"], 50)
        assert_equal(self.main_node.gettxout(txid, escrow["n"])["scriptPubKey"]["hex"], f"b701{SLOT:02x}51")
        self.bmm()
        assert_equal(side.getbalance(), 50)
        self.check_in_sync()

        self.log.info("A withdrawal: bundle (M3), votes (M4), payout (M6)")
        payout_address = main.getnewaddress()
        withdrawal = side.createwithdrawal(payout_address, 3, Decimal("0.01"))
        self.sync_mempools()
        self.bmm()
        assert_equal(side.getwithdrawalbundle()["status"], "next")
        self.bmm()
        bundle = side.getwithdrawalbundle()
        assert_equal(bundle["status"], "pending")
        # The sidechain node gave the bundle to its mainchain node, which proposes it and upvotes it.
        self.mine_main()
        pending = main.listwithdrawalbundles(SLOT)
        assert_equal([b["hash"] for b in pending], [bundle["hash"]])
        assert_equal(pending[0]["vote"], "upvote")
        # The bundle hash the sidechain computed is the one the mainchain votes on and pays (BIP300 M6id).
        escrow_before = self.escrow()
        for _ in range(BUNDLE_MAX_AGE):
            if main.getwithdrawalbundle(SLOT, bundle["hash"])["status"] != "pending":
                break
            self.mine_main()
        assert_equal(main.getwithdrawalbundle(SLOT, bundle["hash"]), {"status": "paid"})
        assert_equal(main.getreceivedbyaddress(payout_address), 3)
        assert_equal(self.escrow(), escrow_before - Decimal("3.01"))
        self.bmm()
        assert_equal(side.listwithdrawals(), [])
        assert_equal(side.getwithdrawalbundle(), {"status": "none", "waiting": 0})
        self.check_in_sync()

        self.log.info("A bundle the mainchain does not vote for fails at its maximum age; the withdrawal is refunded")
        main.setwithdrawalvote(SLOT, "abstain")
        again = side.createwithdrawal(payout_address, 2)
        self.sync_mempools()
        self.bmm()
        self.bmm()
        failed = side.getwithdrawalbundle()
        assert_equal(failed["status"], "pending")
        assert_raises_rpc_error(-4, "not accepted into the mempool", side.refundwithdrawal, again["txid"], again["vout"])
        self.mine_main(BUNDLE_MAX_AGE + 2)
        assert_equal(main.getwithdrawalbundle(SLOT, failed["hash"]), {"status": "failed"})
        self.bmm()
        assert_equal(side.getwithdrawalbundle()["status"], "none")
        assert_equal(side.listwithdrawals()[0]["status"], "waiting")
        again_fee = side.listwithdrawals()[0]["mainchainfee"]
        balance = side.getbalance()
        refund = side.refundwithdrawal(again["txid"], again["vout"])
        assert_equal(refund["amount"], 2 + again_fee)
        self.sync_mempools()
        self.bmm()
        assert_equal(side.listwithdrawals(), [])
        assert_greater_than(side.getbalance(), balance + Decimal("1.99"))
        main.setwithdrawalvote(SLOT, "default")
        self.check_in_sync()

        self.run_single_payout_test()

    def run_single_payout_test(self):
        """A bundle left pending on the mainchain by a reorg holds a new one back while it has support; paid, the withdrawal is paid once."""
        main = self.main
        side, other = self.nodes
        self.log.info("Single payout, kept by the sidechain: a bundle of another branch, pending with the same withdrawal")
        # A clear slot, and a retry delay passed.
        while main.listwithdrawalbundles(SLOT):
            self.mine_main()
        for _ in range(BUNDLE_RETRY_DELAY):
            self.bmm()
        assert_equal(side.getwithdrawalbundle()["status"], "none")

        payout_address = main.getnewaddress()
        side.createwithdrawal(payout_address, 2)
        self.sync_mempools()
        self.bmm()
        withdrawal_commitment = main.getbestblockhash()
        fee = side.listwithdrawals()[0]["mainchainfee"]
        committed = self.bmm()
        x = side.getwithdrawalbundle()
        assert_equal(x["status"], "pending")
        self.mine_main()
        assert_equal([b["hash"] for b in main.listwithdrawalbundles(SLOT)], [x["hash"]])

        # The mainchain drops the blocks that committed to the blocks with the withdrawal and with X (as in
        # feature_sidechain.py's double payout test): X goes on the sidechain, and stays pending on the
        # mainchain's new branch, proposed again by its node.
        main.invalidateblock(withdrawal_commitment)
        main.vouchwithdrawalbundle(SLOT, x["hash"])
        main.generateblock(main.getnewaddress(), [])
        main.vouchwithdrawalbundle(SLOT)
        for node in self.nodes:
            node.syncmainchain()
            assert committed != node.getbestblockhash()
        self.mine_main()
        assert_equal([b["hash"] for b in main.listwithdrawalbundles(SLOT)], [x["hash"]])
        # The withdrawal is made again on the new branch, from the mempool.
        self.sync_mempools()
        self.bmm()
        assert_equal(side.listwithdrawals()[0]["status"], "waiting")
        # Without support X holds nothing back...
        assert main.getwithdrawalbundle(SLOT, x["hash"])["score"] < PENDING_MIN_SCORE
        assert_equal(side.getwithdrawalbundle()["status"], "next")
        # ...with it, it holds the refund back (as on Chains) and, without single payout on the mainchain, a new
        # bundle too: both could be paid, and the withdrawal twice.
        main.vouchwithdrawalbundle(SLOT, x["hash"])
        while main.getwithdrawalbundle(SLOT, x["hash"])["score"] < PENDING_MIN_SCORE:
            self.mine_main()
        assert_equal(side.getwithdrawalbundle()["status"], "held")
        escrow = self.escrow()
        self.bmm()
        assert_equal(side.getwithdrawalbundle()["status"], "held")
        assert_equal(side.listwithdrawals()[0]["status"], "waiting")
        # X is paid out (by the mainchain node, which has it): the sidechain matches its payout to the withdrawal.
        while main.getwithdrawalbundle(SLOT, x["hash"])["status"] == "pending":
            self.mine_main()
        assert_equal(main.getwithdrawalbundle(SLOT, x["hash"]), {"status": "paid"})
        main.vouchwithdrawalbundle(SLOT)
        self.bmm()
        assert_equal(side.listwithdrawals(), [])
        bundle = side.getwithdrawalbundle()
        assert_equal((bundle["status"], bundle["waiting"]), ("none", 0))
        for _ in range(3):
            self.mine_main()
        assert_equal(main.getreceivedbyaddress(payout_address), 2)
        assert_equal(self.escrow(), escrow - 2 - fee)
        main.reconsiderblock(withdrawal_commitment)
        self.bmm()
        self.check_in_sync()


if __name__ == '__main__':
    SidechainEcashTest(__file__).main()
