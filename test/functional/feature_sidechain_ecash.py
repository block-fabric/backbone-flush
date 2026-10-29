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
- A bundle the mainchain does not vote for fails at its maximum age; its withdrawal is refunded (not with
  --mainchaingrpc: the enforcer's block producer upvotes every bundle handed to it).
- A bundle of another branch, left pending by a mainchain reorg with the same withdrawal: under the rules of the
  eCash networks (no scores, SidechainParams::main_events_only) it holds nothing back; the new bundle is vouched
  for and paid, the other downvoted and failed: the withdrawal is paid once, and the escrow pays it once.
"""

import atexit
import os
import socket
import subprocess
import time
from decimal import Decimal
from http.client import HTTPException

from test_framework.authproxy import AuthServiceProxy, JSONRPCException
from test_framework import util as fw_util
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


def fresh_connections(proxy):
    """A connection per call: a node closes a connection left idle for a while, which a busy test can do."""
    proxy.reuse_http_connections = False
    return proxy


def free_port():
    """A free port for Backbone's gRPC server (49000-49999 on this machine, not 49x54 or 4966x), none of the nodes'."""
    nodes = {f(n) for f in (p2p_port, rpc_port, lambda n: p2p_port(n) + 2 * fw_util.PORT_RANGE) for n in range(12)}
    start = 49000 + (p2p_port(0) * 7) % 1000
    for i in range(1000):
        port = 49000 + (start - 49000 + i) % 1000
        if port % 100 == 54 or 49660 <= port <= 49669 or port in nodes:
            continue
        with socket.socket() as s:
            try:
                s.bind(("127.0.0.1", port))
                return port
            except OSError:
                continue
    raise AssertionError("no free port")


class SidechainEcashTest(BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_argument("--mainchaingrpc", action="store_true", default=False,
                            help="Send the sidechain's requests (BMM, bundles, vouching) through the enforcer's interface Backbone serves (-mainchaingrpcport)")

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
        self.grpc_port = None


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
        if self.options.mainchaingrpc:
            self.main_args += ["-grpc", f"-grpcport={self.grpc_port}", "-grpcwallet=miner", "-grpcauth=tok"]
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
        if self.options.mainchaingrpc:
            self.grpc_port = free_port()
            for args in self.extra_args:
                args += [f"-mainchaingrpcport={self.grpc_port}", "-mainchaingrpcauth=tok"]
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
        if self.options.mainchaingrpc:
            # The sidechain node asks for it itself, through the enforcer's interface
            # (WalletService.CreateBmmCriticalDataTransaction).
            request = node.requestbmmblock(self.side_address, Decimal("0.001"))
            assert_equal(request["mainchainheight"], self.main.getblockcount())
            assert request["mainchaintxid"] in self.main.getrawmempool()
            self.mine_main()
            if expect:
                assert_equal(node.getbestblockhash(), request["hash"])
                self.sync_blocks()
            return request["hash"]
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
        # The sidechain node gave the bundle to its mainchain node, which proposes it and upvotes it. Through the
        # enforcer's interface, only once the block that made it is bundle_handoff_depth (2) mainchain blocks deep:
        # handed after the next block, proposed in the one after.
        self.mine_main()
        if self.options.mainchaingrpc:
            assert_equal(main.listwithdrawalbundles(SLOT), [])
            self.mine_main()
        pending = main.listwithdrawalbundles(SLOT)
        assert_equal([b["hash"] for b in pending], [bundle["hash"]])
        assert_equal(pending[0]["vote"], "upvote")
        # getpeginfo over JSON-RPC (or -mainchaingrpcport, the same): what the pages read of the peg, the enforcer's rules
        # (more votes than the threshold, within the maximum age), the bundle as Backbone has it, and how it votes.
        drivechain = self.main_node.getdrivechaininfo()
        sidechain = main.getsidechain(SLOT)
        peg = side.getpeginfo()
        assert_equal(peg["via"], "json-rpc+enforcer" if self.options.mainchaingrpc else "json-rpc")
        assert_equal((peg["height"], peg["rules"], peg["withdrawalperiod"], peg["withdrawalminscore"]),
                     (main.getblockcount(), "enforcer", drivechain["bundlemaxage"], drivechain["bundlethreshold"] + 1))
        assert_equal(peg["sidechain"], {"slot": SLOT, "active": True, "activationheight": sidechain["activationheight"],
                                        "proposalhash": sidechain["proposalhash"], "title": "Testchain"})
        assert_equal(peg["escrow"]["amount"], Decimal(sidechain["escrow"]["amount"]))
        assert_equal(peg["bundles"], [{k: b[k] for k in ("hash", "height", "score", "blocksleft", "payable", "vote")} for b in pending])
        assert_equal(peg["votehistory"], True)
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

        if self.options.mainchaingrpc:
            # The enforcer's block producer upvotes every bundle handed to it, whatever setwithdrawalvote says:
            # a bundle of this chain does not fail for want of votes here.
            self.log.info("(Not through the enforcer's interface: a bundle the mainchain does not vote for, whose producer upvotes what it is handed)")
        else:
            self.run_unvoted_bundle_test(payout_address)

        self.run_single_payout_test()

    def run_unvoted_bundle_test(self, payout_address):
        """A bundle the mainchain does not vote for fails at its maximum age; its withdrawal is refunded."""
        main = self.main
        side = self.nodes[0]
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

    def run_single_payout_test(self):
        """A bundle left pending on the mainchain by a reorg holds nothing back; the new one is paid, the withdrawal once."""
        main = self.main
        side, other = self.nodes
        self.log.info("A bundle of another branch, pending with the same withdrawal: the new one paid, the other failed")
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
        if self.options.mainchaingrpc:
            # Handed over once deep enough, proposed in the next block (the reorg below is deeper still).
            assert_equal(main.listwithdrawalbundles(SLOT), [])
            self.mine_main()
        assert_equal([b["hash"] for b in main.listwithdrawalbundles(SLOT)], [x["hash"]])

        # The mainchain drops the blocks that committed to the blocks with the withdrawal and with X (as in
        # feature_sidechain.py's double payout test): X goes on the sidechain, and stays pending on the
        # mainchain's new branch, proposed again by its node.
        main.invalidateblock(withdrawal_commitment)
        main.vouchwithdrawalbundle(SLOT, x["hash"])
        main.generateblock(main.getnewaddress(), [])
        if self.options.mainchaingrpc:
            # The sidechain node takes its word on X back itself (over JSON-RPC: the enforcer's interface
            # cannot), once it sees that X is not its bundle on the new branch: X is no longer upvoted.
            for node in self.nodes:
                node.syncmainchain()
            self.wait_until(lambda: main.listwithdrawalbundles(SLOT)[0]["vote"] != "upvote")
        else:
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
        # The rules of the eCash networks take no scores (SidechainParams::main_events_only): X holds nothing back,
        # with support or without. A new bundle Y is made; the sidechain node vouches for it, and its mainchain node
        # upvotes Y and downvotes X, which fails. (Where the bundle goes through an enforcer that cannot be told so,
        # it is handed over only once deep enough for no reorg to leave it behind: feature_sidechain_enforcer.py.)
        assert_equal(side.getwithdrawalbundle()["status"], "next")
        escrow = self.escrow()
        # Through the enforcer's interface, Y is handed over only once deep enough; meanwhile the mainchain node
        # takes the word for none (it takes no word on a bundle it was not handed), and X stays not upvoted.
        with side.assert_debug_log([], unexpected_msgs=["did not take the word of this chain"]):
            self.bmm()
            y = side.getwithdrawalbundle()
            if self.options.mainchaingrpc:
                side.syncmainchain()
                assert_equal(main.listwithdrawalbundles(SLOT)[0]["vote"] != "upvote", True)
        assert_equal(y["status"], "pending")
        assert x["hash"] != y["hash"]
        for _ in range(3 * BUNDLE_MAX_AGE):
            if main.getwithdrawalbundle(SLOT, y["hash"])["status"] == "paid":
                break
            self.mine_main()
        assert_equal(main.getwithdrawalbundle(SLOT, y["hash"]), {"status": "paid"})
        self.bmm()
        assert_equal(side.listwithdrawals(), [])
        bundle = side.getwithdrawalbundle()
        assert_equal((bundle["status"], bundle["waiting"]), ("none", 0))
        while main.getwithdrawalbundle(SLOT, x["hash"])["status"] == "pending":
            self.mine_main()
        assert_equal(main.getwithdrawalbundle(SLOT, x["hash"]), {"status": "failed"})
        assert_equal(main.getreceivedbyaddress(payout_address), 2)
        assert_equal(self.escrow(), escrow - 2 - fee)
        main.reconsiderblock(withdrawal_commitment)
        self.bmm()
        self.check_in_sync()
        # Every word the sidechain nodes gave, the mainchain node took, all along: before a hand-off, the word for
        # none (Backbone takes no word on a bundle it was not handed).
        for node in self.nodes:
            with open(node.debug_log_path, encoding="utf-8", errors="replace") as log:
                assert "did not take the word of this chain" not in log.read()


if __name__ == '__main__':
    SidechainEcashTest(__file__).main()
