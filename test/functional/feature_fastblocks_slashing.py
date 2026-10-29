#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""FastBlocks slashing: a leader that signs two blocks for a slot loses 10 % of its stake and is banned
for good; the block that carries the proof is paid 1/8 of the penalty. Three validators (the genesis
set, a bootstrap of regtest), one key per node, each with a bond:

  - equivocation: a leader signs a second block for a slot it made a block of; the nodes find the
    proof in the headers, and a leader puts it in a block of stake;
  - the penalty: 10 % of the offender's stake goes, the rest is unbonded; it is banned (no set, no
    bond); an unbond of its owner waiting in the mempools can no longer be mined and leaves them; the reward is owed to the script of the first output of the block that carried the proof,
    and paid at the next peg step, with a share of the penalty's rest (the reserve) to the anchor's payee;
  - invalid evidence stays refused, and a proof is punished once;
  - banned, a validator leads no more from the next block (its slots stay empty), and a proof of
    another of its offences takes nothing more (slashed once);
  - a leader that carries the proof against itself is paid nothing for it (the share to the reserve);
  - with every validator stopped, a merged mined block carries a proof, and its payee is paid the reward;
  - after every step, the coins of every node are the mainchain's escrow (pegbalance), the reserve
    and the rewards owed included.
"""

import os
import time
from decimal import Decimal

from feature_sidechain import ACTIVATION_PERIOD, SidechainTest
from test_framework.fastblocks import KEYS, SLOT, XONLY, PosBlock, assert_peg_balance
from test_framework.messages import CTxIn
from test_framework.script import CScript
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error, rpc_port

BOND = Decimal("1")
PENALTY = BOND / 10
REWARD = PENALTY / 8
#: The tag of a proof in a BMM block's coinbase (OP_RETURN 0xDD'EVD' | the proof).
EVIDENCE_TAG = "dd455644"


class FastBlocksSlashingTest(SidechainTest):
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
        ]
        self.extra_args = [self.side_args] * self.num_nodes
        self.main_process = None

    def connect_all(self):
        for a, b in ((0, 1), (1, 2), (0, 2)):
            self.connect_nodes(a, b)

    def wait_in_sync(self):
        self.wait_until(lambda: len({n.getbestblockhash() for n in self.nodes}) == 1, timeout=60)

    def check_peg(self):
        return assert_peg_balance(self.main, self.nodes)

    def anchor(self):
        """One more anchor: wait for the merged miner's ticket request, mine it, wait for its acknowledgement."""
        before = [n.getposinfo()["anchors"] for n in self.nodes]
        self.wait_until(lambda: len(self.main.getrawmempool()) > 0, timeout=60)
        self.mine_main()
        self.wait_until(lambda: all(n.getposinfo()["anchors"] > b for n, b in zip(self.nodes, before)), timeout=60)

    def run_test(self):
        try:
            self.run_sidechain_test()
        finally:
            self.stop_mainchain()

    def run_sidechain_test(self):
        main = self.main
        n0, n1, n2 = self.nodes
        self.main_address = main.getnewaddress()

        self.log.info("Activate slot 5; a deposit; the validators start; each bonds to its key")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "FastBlocks", "Blocks of stake between merged mined blocks", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 50)
        self.mine_main()
        block = n0.createbmmblock(n0.getnewaddress())
        main.createbmmrequest(SLOT, block["hash"], Decimal("0.001"))
        self.mine_main()
        self.wait_until(lambda: all(n.getbestblockhash() == block["hash"] for n in self.nodes))
        self.payouts = [n.getnewaddress() for n in self.nodes]
        for i, key in enumerate(KEYS):
            path = os.path.join(self.options.tmpdir, f"validator{i}.key")
            with open(path, "w") as f:
                f.write(key.hex() + "\n")
            self.restart_node(i, extra_args=self.side_args + [f"-validatorkeyfile={path}", f"-validatorpayout={self.payouts[i]}"])
        self.connect_all()
        self.wait_until(lambda: all(n.getstakinginfo()["produced"] > 0 for n in self.nodes), timeout=120)
        for n in (n1, n2):
            txid = n0.sendtoaddress(n.getnewaddress(), 10)
            self.wait_until(lambda: n0.gettransaction(txid)["confirmations"] > 0, timeout=60)
        self.wait_in_sync()
        for i, n in enumerate(self.nodes):
            n.createbond(XONLY[i], BOND)
        self.wait_until(lambda: all(n.getposinfo()["staking"]["bonded"] == 3 * BOND for n in self.nodes), timeout=60)
        n0.setbmm(True, n0.getnewaddress(), True, Decimal("0.0001"))
        self.anchor()
        self.wait_in_sync()
        self.check_peg()

        self.log.info("Equivocation: a leader signs a second block for a slot it made a block of")
        # Its node stops making blocks first: another validator carries the proof (and is paid for it).
        tip = self.pos_tip(n0)
        offender = tip["proposer"]
        offender_node = self.nodes[XONLY.index(offender)]
        offender_node.setstaking(False)
        # Its owner unbonds part of the stake; no block takes it before the proof does (its node keeps
        # it, at a priority below any fee; the others would refuse it so). The penalty unbonds the
        # rest: the unbond can then never be mined, and must leave the mempool, its inputs free again.
        offender_node.setnetworkactive(False)
        self.wait_until(lambda: offender_node.getconnectioncount() == 0, timeout=30)
        unbond = offender_node.createunbond(offender, BOND / 2)
        for n in self.nodes:
            n.prioritisetransaction(txid=unbond["txid"], fee_delta=-100000000)
        offender_node.setnetworkactive(True)
        for other in range(3):
            if other != XONLY.index(offender):
                self.connect_nodes(XONLY.index(offender), other)
        assert unbond["txid"] in offender_node.getrawmempool()
        original, rival = self.rival(n0, tip["hash"])
        # Invalid evidence stays refused: the second header signed by another key is no proof.
        forged = PosBlock(n0.getblock(tip["hash"], 0))
        forged.vtx = rival.vtx
        forged.resign(KEYS[(XONLY.index(offender) + 1) % 3], n0.getblockhash(0))
        assert_raises_rpc_error(-26, "bad-pos-evidence-sig", n0.submitevidence, bytes(original.header).hex() + bytes(forged.header).hex())
        # The rival block, on the same parent: a branch; the node sees two headers its leader signed.
        assert_equal(n1.submitblock(rival.serialize().hex()), "inconclusive")
        # (A node the proof reaches after a block carried it and banned the offender does not keep it.)
        self.wait_until(lambda: all(len(n.listevidence()) == 1 or n.getvalidator(offender)["bond"]["banned"] for n in self.nodes), timeout=60)

        self.log.info("A leader puts the proof in a block: 10 % of the offender's stake goes, the rest is unbonded, it is banned")
        self.wait_until(lambda: all(n.getvalidator(offender)["bond"]["banned"] for n in self.nodes), timeout=60)
        self.wait_in_sync()
        bond = n0.getvalidator(offender)["bond"]
        assert_equal(bond["bonded"], 0)
        assert_equal(bond["unbonding"], BOND - PENALTY)
        assert_equal(bond["eligible"], False)
        staking = n0.getposinfo()["staking"]
        assert_equal(staking["slashed"], PENALTY)
        assert_equal(staking["bonded"], 2 * BOND)
        assert_equal(staking["unbonding"], BOND - PENALTY)
        # The block that carried it, and who it pays: its first output.
        carrying = self.carrying(n0)
        assert carrying["proposer"] != offender
        assert_equal(carrying["evidence"][0]["slot"], tip["slot"])
        assert_equal(sorted(carrying["evidence"][0]["hashes"]), sorted([original.hash, rival.hash]))
        reporter = carrying["tx"][0]["vout"][0]["scriptPubKey"]["hex"]
        self.check_peg()
        # The unbond the penalty made invalid left the mempool; its wallet can abandon it.
        self.wait_until(lambda: unbond["txid"] not in offender_node.getrawmempool(), timeout=60)
        offender_node.abandontransaction(unbond["txid"])
        details = offender_node.gettransaction(unbond["txid"])["details"]
        assert details and all(d["abandoned"] for d in details), details
        # Not twice: the same proof is no longer one this chain takes, and the offender's key no longer bonds.
        assert_raises_rpc_error(-8, "This validator is banned", n0.createbond, offender, BOND)

        self.log.info("Banned, it leads no more, from the block after the one that banned it")
        offender_node.setstaking(True)
        schedule = n0.getleaderschedule()["slots"]
        assert all("leader" not in s for s in schedule if s.get("banned") == offender)
        assert any(s.get("banned") == offender for s in schedule + n0.getleaderschedule(n0.getleaderschedule()["epoch"] + 1)["slots"])
        assert all(s.get("leader") != offender for s in schedule)
        after = n0.getblockcount()
        self.wait_until(lambda: n0.getblockcount() >= after + 8, timeout=120)
        self.wait_until(lambda: "banned" in offender_node.getstakinginfo()["error"], timeout=120)
        self.wait_in_sync()
        blockhash = n0.getbestblockhash()
        while blockhash != carrying["hash"]:
            block = n0.getblock(blockhash)
            assert block.get("proposer") != offender
            blockhash = block["previousblockhash"]

        self.log.info("Slashed once: a proof of another of its offences is not kept, and takes nothing more")
        earlier = n0.getblock(carrying["previousblockhash"])
        while earlier.get("proposer") != offender or earlier["slot"] == tip["slot"]:
            earlier = n0.getblock(earlier["previousblockhash"])
        first, second = self.rival(n0, earlier["hash"])
        assert_raises_rpc_error(-26, "pos-evidence-banned", n0.submitevidence, bytes(first.header).hex() + bytes(second.header).hex())
        assert all(len(n.listevidence()) <= 1 for n in self.nodes)
        after = n0.getblockcount()
        self.wait_until(lambda: n0.getblockcount() >= after + 4, timeout=120)
        self.wait_in_sync()
        assert_equal(n0.getposinfo()["staking"]["slashed"], PENALTY)
        assert_equal(n0.getvalidator(offender)["bond"]["unbonding"], BOND - PENALTY)
        assert_equal(self.carrying(n0)["hash"], carrying["hash"])
        self.check_peg()

        self.log.info("The next peg step pays the reward to the block that carried the proof")
        self.anchor()
        self.anchor()
        self.wait_in_sync()
        hashes = []
        blockhash = n0.getbestblockhash()
        while blockhash != carrying["hash"]:
            hashes.append(blockhash)
            blockhash = n0.getblock(blockhash)["previousblockhash"]
        assert_equal(sum(self.pays(n0, h, REWARD, reporter) for h in hashes), 1)
        # The wallet it is paid to (the payout address of the node that made the block) says what it is.
        reporter_node = self.nodes[XONLY.index(carrying["proposer"])]
        self.wait_until(lambda: any(t.get("income") == "reporter" and t["amount"] == REWARD for t in reporter_node.listtransactions("*", 1000)), timeout=60)
        staking = n0.getposinfo()["staking"]
        assert_equal(staking["rewardsowed"], 0)
        # The rest of the penalty is in the reserve, paid out a share per anchor with the pot.
        reserve = n0.getposinfo()["pot"]["reserve"]
        assert_greater_than(PENALTY - REWARD, reserve)
        self.check_peg()

        self.log.info("The offender leaves the validator sets made from then on")
        def offender_gone():
            if offender not in [v["pubkey"] for v in n0.getposinfo()["validators"]]:
                return True
            self.anchor()
            return False
        self.wait_until(offender_gone, timeout=240)
        self.wait_in_sync()
        self.check_peg()

        self.log.info("A leader that carries the proof against itself: the penalty, no reward (all of it to the reserve)")
        others = [k for k in XONLY if k != offender]
        self_reporter, third = others
        third_node = self.nodes[XONLY.index(third)]
        self.settle_sets(n0)
        third_node.setstaking(False)
        self.wait_in_sync()
        made = self.pos_tip(n0)
        while made["proposer"] != self_reporter:
            self.wait_until(lambda: n0.getbestblockhash() != made["hash"], timeout=60)
            made = self.pos_tip(n0)
        reserve = n0.getposinfo()["pot"]["reserve"]
        slashed = n0.getposinfo()["staking"]["slashed"]
        first, second = self.rival(n0, made["hash"])
        assert_equal(n1.submitblock(second.serialize().hex()), "inconclusive")
        self.wait_until(lambda: all(n.getvalidator(self_reporter)["bond"]["banned"] for n in self.nodes), timeout=60)
        self.wait_in_sync()
        carrying2 = self.carrying(n0)
        assert_equal(carrying2["proposer"], self_reporter)
        staking = n0.getposinfo()["staking"]
        assert_equal(staking["slashed"], slashed + PENALTY)
        assert_equal(staking["rewardsowed"], 0)
        assert_equal(n0.getposinfo()["pot"]["reserve"], reserve + PENALTY)
        self.check_peg()
        third_node.setstaking(True)
        self.anchor()
        self.wait_in_sync()
        reporter2 = carrying2["tx"][0]["vout"][0]["scriptPubKey"]["hex"]
        blockhash = n0.getbestblockhash()
        while blockhash != carrying2["hash"]:
            assert not self.pays(n0, blockhash, REWARD, reporter2)
            blockhash = n0.getblock(blockhash)["previousblockhash"]
        self.check_peg()

        self.log.info("A merged mined block carries a proof: the same rules, the reward to its payee")
        self.settle_sets(n0)
        self.wait_until(lambda: self.pos_tip(n0)["proposer"] == third, timeout=60)
        for n in self.nodes:
            n.setstaking(False)
        self.wait_in_sync()
        made = self.pos_tip(n0)
        assert_equal(made["proposer"], third)
        first, second = self.rival(n0, made["hash"])
        assert_equal(n1.submitblock(second.serialize().hex()), "inconclusive")
        self.wait_until(lambda: all(any(e["pubkey"] == third for e in n.listevidence()) for n in self.nodes), timeout=60)
        slashed = n0.getposinfo()["staking"]["slashed"]
        # No block of stake comes: after stall_seconds the merged miner makes BMM blocks, with the proof.
        deadline = time.time() + 240
        while not n0.getvalidator(third)["bond"]["banned"]:
            assert time.time() < deadline, "no BMM block carried the proof"
            self.mine_main()
            time.sleep(1)
        self.wait_until(lambda: all(n.getvalidator(third)["bond"]["banned"] for n in self.nodes), timeout=60)
        self.wait_in_sync()
        bmm = n0.getblock(n0.getbestblockhash(), 2)
        while not any(out["scriptPubKey"]["hex"].startswith("6a4d") and EVIDENCE_TAG in out["scriptPubKey"]["hex"][:16] for out in bmm["tx"][0]["vout"]):
            bmm = n0.getblock(bmm["previousblockhash"], 2)
        assert_equal(bmm["kind"], "bmm")
        payee = bmm["tx"][0]["vout"][0]["scriptPubKey"]["hex"]
        staking = n0.getposinfo()["staking"]
        assert_equal(staking["slashed"], slashed + PENALTY)
        assert_equal(staking["rewardsowed"], REWARD)
        self.check_peg()
        # Paid to its payee by the next merged mined block (the next peg step).
        tip = n0.getbestblockhash()
        deadline = time.time() + 120
        while n0.getposinfo()["staking"]["rewardsowed"] != 0:
            assert time.time() < deadline, "the reward was not paid"
            self.mine_main()
            time.sleep(1)
        self.wait_in_sync()
        hashes = []
        blockhash = n0.getbestblockhash()
        while blockhash != bmm["hash"]:
            hashes.append(blockhash)
            blockhash = n0.getblock(blockhash)["previousblockhash"]
        assert_equal(sum(self.pays(n0, h, REWARD, payee) for h in hashes), 1)
        self.check_peg()

    def settle_sets(self, node):
        """Two epochs without an anchor: the set of the epoch is then the last anchor's, and a proof of
        a slot of it is within the window for proofs (evidence_window_anchors, 10 on regtest), however
        many anchors the test made before."""
        epoch = node.getleaderschedule()["epoch"]
        self.wait_until(lambda: node.getleaderschedule()["epoch"] >= epoch + 2, timeout=120)
        self.wait_in_sync()

    def pos_tip(self, node):
        """The newest block of stake of the node's chain, once its tip is one."""
        tip = node.getblock(node.getbestblockhash())
        while tip["kind"] != "pos":
            self.wait_until(lambda: node.getbestblockhash() != tip["hash"], timeout=30)
            tip = node.getblock(node.getbestblockhash())
        return tip

    def rival(self, node, blockhash):
        """The block of stake and a second one its leader signs for its slot (another coinbase)."""
        original = PosBlock(node.getblock(blockhash, 0))
        rival = PosBlock(node.getblock(blockhash, 0))
        coinbase = rival.vtx[0]
        coinbase.vin[0] = CTxIn(coinbase.vin[0].prevout, CScript(bytes(coinbase.vin[0].scriptSig) + bytes([1, 0x42])), coinbase.vin[0].nSequence)
        rival.resign(KEYS[XONLY.index(node.getblock(blockhash)["proposer"])], node.getblockhash(0))
        assert rival.hash != original.hash
        return original, rival

    def carrying(self, node):
        """The newest block of the chain that carried a proof in its sections."""
        block = node.getblock(node.getbestblockhash(), 2)
        while "evidence" not in block:
            block = node.getblock(block["previousblockhash"], 2)
        return block

    @staticmethod
    def pays(node, blockhash, amount, script):
        block = node.getblock(blockhash, 2)
        return any(out["value"] == amount and out["scriptPubKey"]["hex"] == script for out in block["tx"][0]["vout"])


if __name__ == '__main__':
    FastBlocksSlashingTest(__file__).main()
