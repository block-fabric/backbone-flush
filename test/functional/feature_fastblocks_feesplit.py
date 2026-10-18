#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""FastBlocks' fee split (doc/fastblocks.md, "Fees, the pot and the validator pool"), from
-testactivationheight=feesplit@<height>. Two validators (genesis keys, each also bonded by its node's
wallet, with different stakes), against a mainchain node, on mock time (SlotClock) that the test moves
on, and jumps for the mainchain's spacing:

  - before the height the old rule: the producer keeps half the fees, the pot takes the rest;
  - from it: the producer keeps nothing; the BMM bidder's share b = max(10 %, 50 % x min(1, 60 / S))
    of the ordinary fees goes to the pot, the rest to the validator pool; S is the median spacing of
    the last 11 mainchain blocks at the epoch's seed block (worked out here from the mainchain's own
    median times): a one-minute mainchain (exactly 50/50), a slow one (the 10 % floor);
  - a merged mined block's own fees: the validators' share to the pool, by S at its mainchain block;
  - a withdrawal's fee goes to the bidder whole;
  - withholding acknowledgements changes nothing: the split of the blocks without an anchor is the
    seed's;
  - the pool of an epoch is paid at the first anchor after it ends, to each validator's bond owner, by
    stake (floor), the remainder carried to the next epoch; the wallet calls it "validator" income;
  - a reorg across the distribution takes it back and pays it again; a restart keeps the pool;
  - after every step, the coins of every node are the mainchain's escrow (pegbalance, the pool in it).
"""

import os
from decimal import Decimal

from feature_sidechain import SidechainTest
from test_framework.fastblocks import (
    KEYS,
    SLOT,
    XONLY,
    SlotClock,
    activate_and_fund,
    assert_peg_balance,
    stop_bmm,
)
from test_framework.util import assert_equal, assert_greater_than, rpc_port

COIN = 100_000_000
POT_SCRIPT = "6a04de504f54"
POOL_SCRIPT = "6a04de56504c"
MARKER_PREFIX = "6a08de565053"
EPOCH = 20
SEED_LOOKAHEAD = 3600
#: The fee split from this height of the sidechain on.
FEE_SPLIT_HEIGHT = 30
BONDS = (Decimal("0.01"), Decimal("0.03"))


def sats(value):
    return int(Decimal(value) * COIN)


def bidder_share(fee, spacing):
    """B = max(floor(F / 10), min(floor(F / 2), floor(30 F / S))); F / 2 for S <= 60."""
    if spacing <= 60:
        return fee // 2
    return max(fee // 10, min(fee // 2, 30 * fee // spacing))


class FastBlocksFeeSplitTest(SidechainTest):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.main_rpc_port = rpc_port(self.num_nodes)
        self.side_args = [
            f"-sidechainslot={SLOT}",
            f"-mainchainrpcport={self.main_rpc_port}",
            "-mainchainrpcuser=main",
            "-mainchainrpcpassword=secret",
            "-mainchainrpcwallet=miner",
            f"-posvalidators={XONLY[0]},{XONLY[1]}",
            f"-testactivationheight=feesplit@{FEE_SPLIT_HEIGHT}",
        ]
        self.extra_args = [self.side_args] * self.num_nodes
        self.main_process = None
        self.rpc_timeout = 120

    def setup_network(self):
        self.start_mainchain()
        self.setup_nodes()
        self.connect_nodes(0, 1)

    def run_test(self):
        try:
            self.run_sidechain_test()
        finally:
            self.stop_mainchain()

    # Helpers

    def check_peg(self):
        return assert_peg_balance(self.main, self.nodes)

    def in_sync(self):
        self.wait_until(lambda: len({n.getbestblockhash() for n in self.nodes}) == 1, timeout=120)

    def node_args(self, i):
        return self.side_args + [f"-validatorkeyfile={self.key_files[i]}", f"-mocktime={self.clock.now}"]

    def sync_mempools_slots(self):
        """Slots until the mempools agree (relay runs on the mock clock)."""
        for _ in range(60):
            if len({tuple(sorted(n.getrawmempool())) for n in self.nodes}) == 1:
                return
            self.clock.step()
        raise AssertionError("mempools not in sync")

    def send(self):
        """A payment of n0 to n1; its txid."""
        n0, n1 = self.nodes
        try:
            return n0.sendtoaddress(n1.getnewaddress(), 1)
        except Exception:
            bad = [t for t in n0.listtransactions("*", 1000) if t["confirmations"] <= 0]
            for t in bad:
                self.log.error(f"unconfirmed: {n0.gettransaction(t['txid'])}")
            self.log.error(f"tip {n0.getblockcount()} {n0.getbestblockhash()} chaintips {n0.getchaintips()}")
            raise

    def anchor(self):
        """One more anchor: the merged miner's ticket request mined, slots until a leader acknowledged it."""
        before = [n.getposinfo()["anchors"] for n in self.nodes]
        # (A request mined may be for a block of the mainchain before: its ticket is then too old, and the
        # merged miner asks again for the next one.)
        for _ in range(5):
            self.wait_until(lambda: len(self.main.getrawmempool()) > 0, timeout=120)
            self.mine_main()
            for _ in range(20):
                if all(n.getposinfo()["anchors"] > b for n, b in zip(self.nodes, before)):
                    return
                self.clock.step()
        raise AssertionError("no anchor")

    def confirm(self, node, txid, max_slots=60):
        """Slots until the transaction is in a block of every node; its block."""
        for _ in range(max_slots):
            if node.gettransaction(txid)["confirmations"] > 0:
                block = node.gettransaction(txid)["blockhash"]
                self.wait_until(lambda: all(n.getbestblockhash() == node.getbestblockhash() for n in self.nodes), timeout=60)
                return node.getblock(block, 2)
            self.clock.step()
        raise AssertionError(f"{txid} not confirmed")

    @staticmethod
    def outputs(block, script):
        return sum(sats(o["value"]) for o in block["tx"][0]["vout"] if o["scriptPubKey"]["hex"] == script)

    def fee_of(self, node, txid):
        return -sats(node.gettransaction(txid)["fee"])

    def main_spacing(self, height):
        """S at a mainchain height, from the mainchain's median times (the record keeps those)."""
        times = [self.main.getblockheader(self.main.getblockhash(h))["mediantime"] for h in range(max(height - 11, self.activation), height + 1)]
        spacings = sorted(b - a for a, b in zip(times, times[1:]))
        return spacings[len(spacings) // 2] if spacings else 60

    def seed_height(self, epoch):
        """The epoch's seed block: the highest whose median time is at or before its start less the lookahead."""
        seed_time = self.genesis_time + epoch * EPOCH - SEED_LOOKAHEAD
        best = -1
        for h in range(self.main.getblockcount() + 1):
            if self.main.getblockheader(self.main.getblockhash(h))["mediantime"] <= seed_time:
                best = h
        return best if best >= self.activation else -1

    def mainchain_spaced(self, spacing, count):
        """The validators stop, the merged miner off; `count` mainchain blocks `spacing` seconds apart (the
        clock jumps with them); a merged mined block with the mempool (the chain stalled); the validators
        and the merged miner on again."""
        main = self.main
        for n in self.nodes:
            n.setstaking(False)
        stop_bmm(main, self.main_address, self.nodes[0], self.nodes)
        self.in_sync()
        for _ in range(count):
            self.clock.set_time(self.clock.now + spacing)
            main.generateblock(self.main_address, [])
        for n in self.nodes:
            n.syncmainchain()

    def resume(self):
        """The validators on again; once blocks of stake follow the merged mined block, a mainchain block after
        them (the chain no longer stalled), and only then the merged miner: else it would make merged mined
        blocks still, which win over the blocks of stake made meanwhile."""
        n0 = self.nodes[0]
        height = n0.getblockcount()
        for n in self.nodes:
            n.setstaking(True)
        self.clock.advance_until(lambda: n0.getblockcount() >= height + 2, 40)
        self.mine_main()
        self.clock.step()
        assert_equal(n0.getposinfo()["stalled"], False)
        n0.setbmm(True, self.ticket_address, True, Decimal("0.0001"))

    def bmm_block(self):
        """A merged mined block of n0 with its mempool, committed by the next mainchain block."""
        n0 = self.nodes[0]
        for _ in range(60):
            if len({tuple(sorted(n.getrawmempool())) for n in self.nodes}) == 1:
                break
            self.clock.set_time(self.clock.now + 1)
        block = n0.createbmmblock(self.bmm_address)
        self.clock.set_time(self.clock.now + 1)
        self.main.createbmmrequest(SLOT, block["hash"], Decimal("0.001"))
        self.mine_main()
        self.wait_until(lambda: all(n.getbestblockhash() == block["hash"] for n in self.nodes), timeout=60)
        return n0.getblock(block["hash"], 2)

    def check_split(self, block, fee, peg_fee=0):
        """A block of stake's coinbase: nothing to the producer, the bidder's share and the peg fees in the
        pot, the validators' share in the pool, by S of its epoch's seed block (worked out here)."""
        assert_equal(block["kind"], "pos")
        epoch = block["slot"] // EPOCH
        spacing = self.main_spacing(self.seed_height(epoch))
        bidder = bidder_share(fee, spacing)
        assert_equal(sats(block["tx"][0]["vout"][0]["value"]), 0)
        assert_equal(self.outputs(block, POT_SCRIPT), bidder + peg_fee)
        assert_equal(self.outputs(block, POOL_SCRIPT), fee - bidder)
        return spacing

    def pool_of(self, node, epoch):
        for entry in node.getposinfo()["feesplit"]["poolepochs"]:
            if entry["epoch"] == epoch:
                return sats(entry["amount"])
        return 0

    def run_sidechain_test(self):
        main = self.main
        n0, n1 = self.nodes
        self.main_address = main.getnewaddress()

        self.log.info("Activate slot 5, a deposit; two genesis validators start, on mock time")
        activate_and_fund(self, funded=0)
        # (-sidechainactivationheight is not given: the record's blocks count from 0.)
        self.activation = 0
        self.genesis_time = n0.getblockheader(n0.getblockhash(0))["time"]
        self.key_files = []
        for i in range(2):
            path = os.path.join(self.options.tmpdir, f"validator{i}.key")
            with open(path, "w") as f:
                f.write(KEYS[i].hex() + "\n")
            self.key_files.append(path)
        self.clock = SlotClock(self, self.nodes)
        self.clock.now = main.getblock(main.getbestblockhash())["time"] + 5
        for i in range(2):
            self.restart_node(i, extra_args=self.node_args(i))
        self.connect_nodes(0, 1)
        self.clock.set_time(self.clock.now)
        self.ticket_address = n0.getnewaddress()
        self.bmm_address = n0.getnewaddress()
        n0.setbmm(True, self.ticket_address, True, Decimal("0.0001"))
        self.clock.advance(3)
        self.anchor()
        split = n0.getposinfo()["feesplit"]
        assert_equal(split["active"], False)
        assert_equal(split["activationheight"], FEE_SPLIT_HEIGHT)
        self.check_peg()

        self.log.info("Before the height: the producer keeps half the fees, the pot the rest; no pool")
        txid = n0.sendtoaddress(n1.getnewaddress(), 10)
        block = self.confirm(n0, txid)
        assert_greater_than(FEE_SPLIT_HEIGHT, block["height"])
        fee = self.fee_of(n0, txid)
        assert_equal(block["kind"], "pos")
        assert_equal(sats(block["tx"][0]["vout"][0]["value"]), fee // 2)
        assert_equal(self.outputs(block, POT_SCRIPT), fee - fee // 2)
        assert_equal(self.outputs(block, POOL_SCRIPT), 0)
        self.check_peg()

        self.log.info("Bonds: each wallet bonds to its node's key (different stakes: owners for the pool's shares)")
        for node, bond in zip(self.nodes, BONDS):
            node.createbond(XONLY[self.nodes.index(node)], bond)
        self.sync_mempools_slots()
        for _ in range(3):
            self.anchor()
            self.clock.advance(EPOCH)
        owners = {b["validator"]: b["owner"] for node in self.nodes for b in node.listbonds()}
        assert_equal(set(owners), {XONLY[0], XONLY[1]})
        self.clock.advance_until(lambda: n0.getblockcount() > FEE_SPLIT_HEIGHT + 1, 3 * FEE_SPLIT_HEIGHT)
        assert_equal(n0.getposinfo()["feesplit"]["active"], True)
        self.check_peg()

        self.log.info("A one-minute mainchain: S = 60, exactly 50/50")
        # (Enough blocks that the seed, an hour back, lies among them.)
        self.mainchain_spaced(60, 75)
        self.bmm_block()
        self.resume()
        self.clock.advance(2)
        info = n0.getposinfo()
        assert_equal(info["feesplit"]["spacing"], 60)
        assert_equal(info["feesplit"]["bidderbps"], 5000)
        assert_equal(info["feesplit"]["validatorsbps"], 5000)
        assert_equal(info["feesplit"]["mainheight"], self.seed_height(info["epoch"]))
        txid = self.send()
        block = self.confirm(n0, txid)
        assert_equal(self.check_split(block, self.fee_of(n0, txid)), 60)
        self.anchor()
        self.check_peg()

        self.log.info("A slow mainchain: S = 600, the bidder's floor of 10 %; a BMM block's own fees split by S at its mainchain block")
        self.mainchain_spaced(600, 20)
        # The chain stalled: the merged mined block carries a transaction.
        txid = self.send()
        bmm = self.bmm_block()
        assert_equal(bmm["kind"], "bmm")
        assert txid in [tx["txid"] for tx in bmm["tx"]]
        fee = self.fee_of(n0, txid)
        bmm_spacing = self.main_spacing(main.getblockcount() - 1)
        assert_equal(bmm_spacing, 600)
        assert_equal(self.outputs(bmm, POOL_SCRIPT), fee - bidder_share(fee, bmm_spacing))
        assert_equal(sats(bmm["tx"][0]["vout"][0]["value"]), bidder_share(fee, bmm_spacing))
        self.check_peg()
        self.resume()
        self.clock.advance(2)
        info = n0.getposinfo()
        assert_equal(info["feesplit"]["spacing"], 600)
        assert_equal(info["feesplit"]["bidderbps"], 1000)
        txid = self.send()
        block = self.confirm(n0, txid)
        assert_equal(self.check_split(block, self.fee_of(n0, txid)), 600)

        self.log.info("A withdrawal: its fee to the bidder whole")
        withdrawal = n0.createwithdrawal(main.getnewaddress(), 3, Decimal("0.01"))
        block = self.confirm(n0, withdrawal["txid"])
        assert_equal(len(block["tx"]), 2)
        self.check_split(block, 0, peg_fee=self.fee_of(n0, withdrawal["txid"]))
        self.anchor()
        self.check_peg()

        self.log.info("Withholding acknowledgements changes nothing: blocks without an anchor split by the seed's S")
        stop_bmm(main, self.main_address, n0, self.nodes)
        anchors = n0.getposinfo()["anchors"]
        for _ in range(3):
            txid = self.send()
            block = self.confirm(n0, txid)
            self.check_split(block, self.fee_of(n0, txid))
            # Mainchain blocks meanwhile (no tickets): the seed, an hour back, does not move.
            main.generateblock(self.main_address, [])
            for n in self.nodes:
                n.syncmainchain()
        assert_equal(n0.getposinfo()["anchors"], anchors)
        n0.setbmm(True, self.ticket_address, True, Decimal("0.0001"))
        self.anchor()
        self.check_peg()

        self.log.info("The pool of an epoch: paid at the first anchor after it ends, by stake, to the bonds' owners")
        # Fees in one epoch only, then none until its pool is paid; the pools of the epochs before paid first.
        self.clock.advance_until(lambda: n0.getposinfo()["slot"] % EPOCH < 5, 2 * EPOCH)
        epoch = n0.getposinfo()["epoch"]
        while any(e["epoch"] < epoch for e in n0.getposinfo()["feesplit"]["poolepochs"]):
            self.anchor()
        assert_equal(n0.getposinfo()["epoch"], epoch)
        for _ in range(2):
            txid = self.send()
            block = self.confirm(n0, txid)
            assert_equal(block["slot"] // EPOCH, epoch)
        validators = {v["pubkey"]: v["stake"] for v in n0.getvalidators(epoch)["validators"]}
        assert_equal(set(validators), {XONLY[0], XONLY[1]})
        assert_equal(validators[XONLY[1]], validators[XONLY[0]] * 3 - 2)
        self.clock.advance_until(lambda: n0.getposinfo()["epoch"] > epoch, 2 * EPOCH)
        height = n0.getblockcount()
        self.anchor()
        self.in_sync()
        assert_equal(self.pool_of(n0, epoch), 0)
        carried = self.pool_of(n0, epoch + 1)
        paid_in = None
        for h in range(height, n0.getblockcount() + 1):
            candidate = n0.getblock(n0.getblockhash(h), 2)
            vout = candidate["tx"][0]["vout"]
            for i, o in enumerate(vout):
                if o["scriptPubKey"]["hex"].startswith(MARKER_PREFIX):
                    count = int.from_bytes(bytes.fromhex(o["scriptPubKey"]["hex"][len(MARKER_PREFIX):]), "little")
                    paid_in = (candidate, vout[i + 1:i + 1 + count])
        assert paid_in is not None, "no distribution"
        dist_block, shares = paid_in
        assert_equal(len(shares), 2)
        total = sum(validators.values())
        pool = sum(sats(s["value"]) for s in shares) + carried
        expected = {owners[k]: pool * w // total for k, w in validators.items()}
        assert_equal({s["scriptPubKey"]["address"]: sats(s["value"]) for s in shares}, expected)
        assert_equal(carried, pool - sum(expected.values()))
        # The wallet: validator income.
        coinbase = dist_block["tx"][0]["txid"]
        details = n0.gettransaction(coinbase)["details"]
        assert [d for d in details if d.get("income") == "validator" and d["address"] == owners[XONLY[0]]], details
        self.check_peg()

        self.log.info("A reorg across the distribution takes it back; taken again, it pays again")
        stop_bmm(main, self.main_address, n0, self.nodes)
        for n in self.nodes:
            n.setstaking(False)
        self.in_sync()
        tip = n0.getbestblockhash()
        for n in self.nodes:
            n.invalidateblock(dist_block["hash"])
        assert_equal(self.pool_of(n0, epoch), pool)
        self.check_peg()
        for n in self.nodes:
            n.reconsiderblock(dist_block["hash"])
        self.wait_until(lambda: all(n.getbestblockhash() == tip for n in self.nodes), timeout=60)
        assert_equal(self.pool_of(n0, epoch), 0)
        assert_equal(self.pool_of(n0, epoch + 1), carried)
        self.check_peg()

        self.log.info("A restart keeps the pool")
        before = n0.getposinfo()["feesplit"]
        self.restart_node(0, extra_args=self.node_args(0))
        self.connect_nodes(0, 1)
        after = n0.getposinfo()["feesplit"]
        assert_equal(after["pool"], before["pool"])
        assert_equal(after["poolepochs"], before["poolepochs"])
        self.resume()
        self.clock.advance(3)
        self.anchor()
        self.check_peg()


if __name__ == '__main__':
    FastBlocksFeeSplitTest(__file__).main()
