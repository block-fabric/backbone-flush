#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The sidechain following its mainchain through the bip300301_enforcer's interface alone (-mainchaingrpc).

The mainchain is backboned -regtest -testecash -grpc (the rules of the eCash networks, and the enforcer's
interface served by Backbone, byte for byte the enforcer's: Backbone's doc/ecash-grpc.md), looked for in
MAINCHAIN_BIN_DIR or ../chains-ecash/build_node/bin; skipped without one. The sidechain nodes have no JSON-RPC
access to it at all: -mainchainrpcport names a port nothing listens on, and there are no credentials (no
cookie in -mainchaindatadir). Everything they know of the mainchain, and everything they ask of it, goes
through the interface (GetChainTip, GetBlockHeaderInfo, GetBlockInfo, GetSidechains, GetChainInfo,
CreateBmmCriticalDataTransaction, ProposeWithdrawalBundle). The test itself drives the mainchain over its
JSON-RPC, as a miner and a depositor would.

The rules are those of the eCash networks (SidechainParams::main_events_only, doc/ecash-enforcer.md):

- activation; blocks by blind merged mining, requested by the sidechain node; a deposit;
- a withdrawal bundle handed to the block producer only once its commitment is bundle_handoff_depth (2 on
  regtest) mainchain blocks deep; proposed (Submitted), voted and paid (Succeeded), and the withdrawal gone;
- a bundle that fails at its maximum age (Failed): the withdrawal refunded;
- a reorg of the mainchain across a deposit: credited once, on the branch that has it;
- a reorg of the mainchain across a bundle's commitment before its hand-off: the bundle is never proposed,
  the one of the new branch is, and the withdrawal is paid once;
- a bundle of the slot that is not this chain's, pending: it holds back neither a new bundle nor a refund;
- a reorg of the mainchain across a payout (Succeeded taken back, then again);
- two nodes in agreement all along.
"""

import atexit
import os
import socket
import subprocess
import time
from decimal import Decimal
from http.client import HTTPException
from io import BytesIO

from test_framework.authproxy import AuthServiceProxy, JSONRPCException
from test_framework.messages import CTxOut, deser_compact_size, ser_compact_size
from test_framework import util as fw_util
from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
    p2p_port,
    rpc_port,
)

MAINCHAIN_DAEMON = "backbone" + "d"

SLOT = 3
UNUSED_SLOT_THRESHOLD = 5
BUNDLE_MAX_AGE = 10
# Regtest parameters of this chain with -testmainenforcer.
BUNDLE_RETRY_DELAY = 5
HANDOFF_DEPTH = 2


def fresh_connections(proxy):
    proxy.reuse_http_connections = False
    return proxy


def free_port(avoid=()):
    """A free port in 49000-49999, not 49x54 or 4966x (this machine's rules)."""
    nodes = {f(n) for f in (p2p_port, rpc_port, lambda n: p2p_port(n) + 2 * fw_util.PORT_RANGE) for n in range(12)}
    start = 49000 + (p2p_port(0) * 7) % 1000
    for i in range(1000):
        port = 49000 + (start - 49000 + i) % 1000
        if port % 100 == 54 or 49660 <= port <= 49669 or port in avoid or port in nodes:
            continue
        with socket.socket() as s:
            try:
                s.bind(("127.0.0.1", port))
                return port
            except OSError:
                continue
    raise AssertionError("no free port")


class SidechainEnforcerTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.main_rpc_port = rpc_port(self.num_nodes)
        self.main_process = None

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.grpc_port = free_port()
        # Where a JSON-RPC mainchain node would be: nothing listens there, and there is no cookie.
        self.no_rpc_port = free_port(avoid=(self.grpc_port,))
        self.no_cookie_dir = os.path.join(self.options.tmpdir, "no-mainchain-datadir")
        os.makedirs(self.no_cookie_dir, exist_ok=True)
        side_args = [
            f"-sidechainslot={SLOT}",
            "-testmainenforcer",
            f"-mainchaingrpc=http://127.0.0.1:{self.grpc_port}",
            "-mainchaingrpcauth=tok",
            f"-mainchainrpcport={self.no_rpc_port}",
            f"-mainchaindatadir={self.no_cookie_dir}",
        ]
        self.extra_args = [list(side_args), list(side_args)]
        self.start_mainchain()
        super().setup_network()

    def start_mainchain(self):
        bin_dir = os.environ.get("MAINCHAIN_BIN_DIR", os.path.join(self.config["environment"]["SRCDIR"], "..", "chains-ecash", "build_node", "bin"))
        binary = os.path.join(bin_dir, MAINCHAIN_DAEMON)
        if not os.path.isfile(binary):
            raise SkipTest(f"no Backbone node at {binary}; set MAINCHAIN_BIN_DIR")
        help_text = subprocess.run([binary, "-help-debug"], capture_output=True, text=True).stdout
        if "-testecash" not in help_text or "-grpcport" not in help_text:
            raise SkipTest(f"{binary} has no eCash regtest mode with the enforcer's interface")
        datadir = os.path.join(self.options.tmpdir, "mainchain")
        os.makedirs(datadir, exist_ok=True)
        args = [
            binary, "-regtest", "-testecash", f"-datadir={datadir}", f"-rpcport={self.main_rpc_port}", f"-port={p2p_port(self.num_nodes)}",
            "-rpcuser=main", "-rpcpassword=secret", "-fallbackfee=0.0002", "-listen=0", "-server", "-printtoconsole=0",
            "-grpc", f"-grpcport={self.grpc_port}", "-grpcwallet=miner", "-grpcauth=tok",
        ]
        self.main_process = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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

    def sync_main(self):
        for node in self.nodes:
            node.syncmainchain()

    def mine_main(self, count=1):
        hashes = self.main.generatetoaddress(count, self.main_address)
        self.sync_main()
        return hashes

    def bmm(self, node=None, expect=True):
        """A block of the sidechain, which its node asks the mainchain to commit to (CreateBmmCriticalDataTransaction)."""
        node = node or self.nodes[0]
        request = node.requestbmmblock(self.side_address, Decimal("0.001"))
        assert_equal(request["mainchainheight"], self.main.getblockcount())
        assert request["mainchaintxid"] in self.main.getrawmempool()
        self.mine_main()
        if expect:
            assert_equal(node.getbestblockhash(), request["hash"])
            self.sync_blocks()
        return request["hash"]

    def check_in_sync(self):
        self.sync_blocks()
        states = [n.getsidechainstate() for n in self.nodes]
        assert_equal(len({(st["bestblock"], st["hash"]) for st in states}), 1)
        assert_equal(self.nodes[0].listwithdrawals(), self.nodes[1].listwithdrawals())
        assert_equal(self.nodes[0].getwithdrawalbundle(), self.nodes[1].getwithdrawalbundle())

    def escrow(self):
        return self.main.getsidechain(SLOT)["escrow"]["amount"]

    def pending_on_main(self):
        return [b["hash"] for b in self.main.listwithdrawalbundles(SLOT)]

    def run_test(self):
        try:
            self.run_enforcer_test()
        finally:
            self.stop_mainchain()

    def run_enforcer_test(self):
        main = self.main
        side, other = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = side.getnewaddress()

        self.log.info("The nodes follow the mainchain through the enforcer's interface alone")
        self.mine_main(110)
        for node in self.nodes:
            info = node.getmainchaininfo()
            assert_equal(info["via"], "enforcer")
            assert_equal(info["node"], f"127.0.0.1:{self.grpc_port}")
            assert_equal(info["connected"], True)
            assert_equal(info["height"], 110)
            assert_equal(info["mainchain_network"], "regtest")
            assert_equal(info["bestblockhash"], main.getbestblockhash())

        self.log.info("Activation (M1, M2), seen through GetSidechains")
        main.createsidechainproposal(SLOT, "Testchain", "The sidechain of this test", "11" * 32, "22" * 20)
        self.mine_main(1 + UNUSED_SLOT_THRESHOLD + 1)
        assert_equal([s["slot"] for s in main.listactivesidechains()], [SLOT])

        self.log.info("Blocks by blind merged mining, requested by the sidechain node (CreateBmmCriticalDataTransaction)")
        self.bmm()
        self.bmm(other)
        assert_equal(side.getblockcount(), 2)

        self.log.info("A deposit (Deposit event)")
        deposit = side.getdepositaddress()
        main.createsidechaindeposit(SLOT, deposit["depositaddress"], 50)
        self.mine_main()
        self.bmm()
        assert_equal(side.getbalance(), 50)
        self.check_in_sync()

        self.log.info("A reorg of the mainchain across a deposit: credited once, on the branch that has it")
        main.createsidechaindeposit(SLOT, deposit["depositaddress"], 7)
        deposit_block = self.mine_main()[0]
        committed = self.bmm()
        assert_equal(side.getbalance(), 57)
        main.invalidateblock(deposit_block)
        # Two empty blocks: the deposit and the BMM request wait in the mempool, the branch is longer.
        main.generateblock(self.main_address, [])
        main.generateblock(self.main_address, [])
        self.sync_main()
        for node in self.nodes:
            assert committed != node.getbestblockhash()
        assert_equal(side.getbalance(), 50)
        # The deposit is mined again on the new branch; credited once.
        self.mine_main()
        self.bmm()
        assert_equal(side.getbalance(), 57)
        main.reconsiderblock(deposit_block)
        self.sync_main()
        self.bmm()
        assert_equal(side.getbalance(), 57)
        self.check_in_sync()

        self.log.info("A withdrawal: the bundle handed over once its commitment is deep enough, Submitted, voted, Succeeded")
        payout_address = main.getnewaddress()
        side.createwithdrawal(payout_address, 3, Decimal("0.01"))
        self.sync_mempools()
        self.bmm()
        assert_equal(side.getwithdrawalbundle()["status"], "next")
        self.bmm()
        bundle = side.getwithdrawalbundle()
        assert_equal(bundle["status"], "pending")
        # One block deep: not handed over yet, so not in the next block, after which it is two deep and handed
        # over (ProposeWithdrawalBundle); the block producer proposes it in the one after.
        self.mine_main()
        assert bundle["hash"] not in self.pending_on_main()
        self.mine_main()
        assert_equal(self.pending_on_main(), [bundle["hash"]])
        escrow_before = self.escrow()
        for _ in range(BUNDLE_MAX_AGE):
            if main.getwithdrawalbundle(SLOT, bundle["hash"])["status"] != "pending":
                break
            self.mine_main()
        assert_equal(main.getwithdrawalbundle(SLOT, bundle["hash"]), {"status": "paid"})
        assert_equal(main.getreceivedbyaddress(payout_address), 3)
        assert_equal(self.escrow(), escrow_before - Decimal("3.01"))
        paid_block = main.getbestblockhash()
        self.bmm()
        assert_equal(side.listwithdrawals(), [])
        assert_equal(side.getwithdrawalbundle(), {"status": "none", "waiting": 0})
        self.check_in_sync()

        self.log.info("A reorg of the mainchain across the payout: the bundle pending again, then paid again with the branch back")
        # Take back the blocks from the payout on (the payout and the BMM block after it).
        main.invalidateblock(paid_block)
        # (A branch of its own: a node merely behind the record is waited for, not followed back.)
        main.generateblock(self.main_address, [])
        self.sync_main()
        assert_equal(side.getwithdrawalbundle()["status"], "pending")
        assert_equal(len(side.listwithdrawals()), 1)
        main.reconsiderblock(paid_block)
        self.sync_main()
        self.bmm()
        assert_equal(side.listwithdrawals(), [])
        assert_equal(main.getreceivedbyaddress(payout_address), 3)
        self.check_in_sync()

        self.log.info("A bundle the mainchain does not vote for fails at its maximum age (Failed): the withdrawal refunded")
        main.setwithdrawalvote(SLOT, "abstain")
        for _ in range(BUNDLE_RETRY_DELAY):
            self.bmm()
        again = side.createwithdrawal(payout_address, 2)
        self.sync_mempools()
        self.bmm()
        self.bmm()
        failed = side.getwithdrawalbundle()
        assert_equal(failed["status"], "pending")
        assert_raises_rpc_error(-4, "not accepted into the mempool", side.refundwithdrawal, again["txid"], again["vout"])
        for _ in range(HANDOFF_DEPTH + 1):
            self.mine_main()
        assert_equal(self.pending_on_main(), [failed["hash"]])
        self.mine_main(BUNDLE_MAX_AGE + 1)
        assert_equal(main.getwithdrawalbundle(SLOT, failed["hash"]), {"status": "failed"})
        self.bmm()
        assert_equal(side.getwithdrawalbundle()["status"], "none")
        assert_equal(side.listwithdrawals()[0]["status"], "waiting")
        fee = side.listwithdrawals()[0]["mainchainfee"]
        balance = side.getbalance()
        refund = side.refundwithdrawal(again["txid"], again["vout"])
        assert_equal(refund["amount"], 2 + fee)
        self.sync_mempools()
        self.bmm()
        assert_equal(side.listwithdrawals(), [])
        assert_greater_than(side.getbalance(), balance + Decimal("1.99"))
        main.setwithdrawalvote(SLOT, "default")
        self.check_in_sync()

        self.run_reorg_before_handoff_test()
        self.run_foreign_bundle_test()

    def run_reorg_before_handoff_test(self):
        """A reorg of the mainchain takes back a bundle's commitment before it was handed over: never proposed."""
        main = self.main
        side = self.nodes[0]
        self.log.info("A reorg of the mainchain across a bundle's commitment, before the hand-off: paid once")
        for _ in range(BUNDLE_RETRY_DELAY):
            self.bmm()
        payout_address = main.getnewaddress()
        side.createwithdrawal(payout_address, 2)
        self.sync_mempools()
        self.bmm()
        withdrawal_commitment = main.getbestblockhash()
        fee = side.listwithdrawals()[0]["mainchainfee"]
        committed = self.bmm()
        x = side.getwithdrawalbundle()
        assert_equal(x["status"], "pending")
        # The mainchain drops the blocks that committed to the blocks with the withdrawal and with X, before X
        # is deep enough to be handed over.
        main.invalidateblock(withdrawal_commitment)
        main.generateblock(self.main_address, [])
        main.generateblock(self.main_address, [])
        main.generateblock(self.main_address, [])
        self.sync_main()
        for node in self.nodes:
            assert committed != node.getbestblockhash()
        # The withdrawal is made again on the new branch, from the mempool, and a new bundle Y.
        escrow = self.escrow()
        self.sync_mempools()
        self.bmm()
        assert_equal(side.listwithdrawals()[0]["status"], "waiting")
        self.bmm()
        y = side.getwithdrawalbundle()
        assert_equal(y["status"], "pending")
        assert x["hash"] != y["hash"]
        while main.getwithdrawalbundle(SLOT, y["hash"])["status"] != "paid":
            assert x["hash"] not in self.pending_on_main()
            self.mine_main()
        self.bmm()
        assert_equal(side.listwithdrawals(), [])
        for _ in range(BUNDLE_MAX_AGE):
            assert x["hash"] not in self.pending_on_main()
            self.mine_main()
        assert_equal(main.getreceivedbyaddress(payout_address), 2)
        assert_equal(self.escrow(), escrow - 2 - fee)
        main.reconsiderblock(withdrawal_commitment)
        self.sync_main()
        self.bmm()
        self.check_in_sync()

    def run_foreign_bundle_test(self):
        """Another bundle of the slot pending, upvoted: it holds back neither a new bundle nor a refund (no scores)."""
        main = self.main
        side = self.nodes[0]
        self.log.info("A bundle of the slot that is not this chain's: it holds back neither a new bundle nor a refund")
        for _ in range(BUNDLE_RETRY_DELAY):
            self.bmm()
        payout_address = main.getnewaddress()
        side.createwithdrawal(payout_address, 1)
        self.sync_mempools()
        self.bmm()
        nxt = side.getwithdrawalbundle()
        assert_equal(nxt["status"], "next")
        # A bundle nobody on this chain made: the next one's form, paying another address.
        # (Read by hand: a transaction without inputs would be taken for the witness form.)
        blind = BytesIO(bytes.fromhex(nxt["hex"]))
        version = blind.read(4)
        assert_equal(blind.read(1), b"\x00")
        outputs = []
        for _ in range(deser_compact_size(blind)):
            out = CTxOut()
            out.deserialize(blind)
            outputs.append(out)
        locktime = blind.read(4)
        outputs[1].scriptPubKey = bytes.fromhex(main.getaddressinfo(main.getnewaddress())["scriptPubKey"])
        junk = version + b"\x00" + ser_compact_size(len(outputs)) + b"".join(o.serialize() for o in outputs) + locktime
        junk_hash = main.receivewithdrawalbundle(SLOT, junk.hex())["hash"]
        main.setwithdrawalvote(SLOT, "upvote", junk_hash)
        self.mine_main(3)
        assert_equal(self.pending_on_main(), [junk_hash])
        assert_greater_than(main.getwithdrawalbundle(SLOT, junk_hash)["score"], 1)
        # Upvoted, leading, rising: under the rules with scores it would hold both back; here neither.
        assert_equal(side.getwithdrawalbundle()["status"], "next")
        # (Downvoted from now on, so that it is not paid: it pays nobody of this chain.)
        main.setwithdrawalvote(SLOT, "downvote")
        self.bmm()
        ours = side.getwithdrawalbundle()
        assert_equal(ours["status"], "pending")
        # A withdrawal made after the bundle waits for the next one, and can be taken back meanwhile.
        refundable = side.createwithdrawal(payout_address, 1)
        self.sync_mempools()
        self.bmm()
        assert_equal(sorted(w["status"] for w in side.listwithdrawals()), ["bundled", "waiting"])
        side.refundwithdrawal(refundable["txid"], refundable["vout"])
        self.sync_mempools()
        self.bmm()
        assert_equal(len(side.listwithdrawals()), 1)
        # Handed over by now, and proposed: the block producer upvotes it (the last one handed), the other down.
        assert_equal(self.pending_on_main(), [junk_hash, ours["hash"]])
        main.setwithdrawalvote(SLOT, "default")
        escrow = self.escrow()
        while main.getwithdrawalbundle(SLOT, ours["hash"])["status"] == "pending":
            self.mine_main()
        assert_equal(main.getwithdrawalbundle(SLOT, ours["hash"]), {"status": "paid"})
        self.bmm()
        assert_equal(side.listwithdrawals(), [])
        while junk_hash in self.pending_on_main():
            self.mine_main()
        assert_equal(main.getwithdrawalbundle(SLOT, junk_hash), {"status": "failed"})
        assert_equal(self.escrow(), escrow - 1 - Decimal(nxt["mainchainfee"]))
        self.bmm()
        self.check_in_sync()
        # No JSON-RPC call to the mainchain was made: none could be.
        for node in self.nodes:
            with open(node.debug_log_path, encoding="utf-8") as log:
                assert f"127.0.0.1:{self.no_rpc_port}" not in log.read()


if __name__ == '__main__':
    SidechainEnforcerTest(__file__).main()
