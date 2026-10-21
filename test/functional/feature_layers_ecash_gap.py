#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Flush through a gap of the mainchain of six hours, and a week of unbonding, on eCash's rules scaled down.

Against Backbone in its eCash regtest mode (backboned -regtest -testecash, see feature_layers_ecash.py), with the
anchoring parameters of the eCash networks (src/kernel/chainparams.cpp, CEcashSidechainParams) scaled by the
mainchain's block interval: there ten minutes, 40 slots of 15 s; here MAIN_SECONDS seconds, MAIN_SECONDS slots of a
second. Scaled (-testposparam, -posseedlookahead): 8 hours of slots without an anchor, a seed taken 9 hours ahead,
the set fixed as long ahead, one-hour epochs, an unbonding of a few anchors and proofs taken one anchor less.

  - anchors every MAIN_SECONDS seconds, a mainchain block each, with the median time behind as eCash's;
  - the gap: no mainchain block for six hours' worth: the blocks of stake go on (their seeds were known before
    it, their sets fixed before), soft, none anchored or final, and the merged mined fallback stays off;
  - the mainchain comes back: the ticket made before the gap is acknowledged in time, the next anchors cover the
    blocks of the gap, which become final; the coins of every node are the escrow's;
  - unbonding: the coins are paid exactly UNBONDING anchors after the block of the unbond, not one before;
  - a proof of double signing of an offence near the end of the window (one anchor before it closes) is
    carried and slashes; a proof of an offence long out of the window is never carried.
"""

import time
from decimal import Decimal

from feature_layers_ecash import KEYS, LayersEcashTest
from feature_sidechain_ecash import UNUSED_SLOT_THRESHOLD
from test_framework.fastblocks import SLOT, PosBlock, assert_peg_balance
from test_framework.key import compute_xonly_pubkey
from test_framework.messages import CTxIn
from test_framework.script import CScript
from test_framework.util import assert_equal, assert_greater_than

XONLY = [compute_xonly_pubkey(key)[0].hex() for key in KEYS]
# The scale: a mainchain block every MAIN_SECONDS seconds stands for eCash's ten minutes (40 slots of 15 s).
MAIN_SECONDS = 4
SCALE = MAIN_SECONDS / 600
GAP = round(6 * 3600 * SCALE)  # six hours: 144 s
MAX_UNANCHORED = round(8 * 3600 * SCALE)  # 192 slots of a second
LEAD = round(9 * 3600 * SCALE)  # 216 s
EPOCH = round(3600 * SCALE)  # 24 slots
# Unbonding in anchors, and proofs one anchor less: past the anchors a validator still leads after it unbonds (the set
# lead and two epochs: 66 here), as on eCash (1008 and 1007 against about 60).
UNBONDING = 80
WINDOW = UNBONDING - 1
LEAD_ANCHORS = (LEAD + 2 * EPOCH) // MAIN_SECONDS


class LayersEcashGapTest(LayersEcashTest):
    def set_test_params(self):
        super().set_test_params()
        self.side_args += [
            f"-testposparam=max_unanchored_slots@{MAX_UNANCHORED}",
            f"-testposparam=slots_per_epoch@{EPOCH}",
            f"-testposparam=set_lead_seconds@{LEAD}",
            f"-posseedlookahead={LEAD}",
            f"-testposparam=unbonding_anchors@{UNBONDING}",
            f"-testposparam=evidence_window_anchors@{WINDOW}",
        ]
        self.extra_args = [self.side_args] * self.num_nodes

    def run_test(self):
        super().run_test()

    def tick(self, count=1):
        """Mainchain blocks at eCash's pace, scaled: an anchor each once the merged miner asks."""
        for _ in range(count):
            time.sleep(MAIN_SECONDS)
            self.mine_main()

    def run_ecash_test(self):
        main = self.main
        n0, n1, n2 = self.nodes
        self.main_address = main.getnewaddress()
        self.genesis = n0.getblockhash(0)
        self.genesis_time = n0.getblockheader(self.genesis)["time"]

        self.log.info("Activate the slot, a deposit, the validators with eCash's anchoring scaled")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "Flush", "Tokens, assets and a DEX on a fast chain over eCash", "11" * 32, "22" * 20)
        self.mine_main(UNUSED_SLOT_THRESHOLD + 2)
        info = n0.getposinfo()
        assert_equal((info["maxunanchoredslots"], info["setleadseconds"], info["seedlookaheadseconds"]), (MAX_UNANCHORED, LEAD, LEAD))
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 50)
        self.mine_main()
        self.bmm_block(n0, n0.getnewaddress())
        self.payouts = [n.getnewaddress() for n in self.nodes]
        for i, key in enumerate(KEYS):
            with open(self.key_file(i), "w") as f:
                f.write("# validator key\n" + key.hex() + "\n")
        for i in range(self.num_nodes):
            self.restart_node(i, extra_args=self.validator_args(i))
        self.connect_all()
        self.wait_until(lambda: all(n.getstakinginfo()["produced"] > 0 for n in self.nodes), timeout=120)
        n0.setbmm(True, n0.getnewaddress(), True, Decimal("0.0001"))
        # A dozen mainchain blocks at the pace: the median time lags the tip as eCash's (about five blocks).
        self.tick(14)
        self.wait_in_sync()
        assert_greater_than(n0.getposinfo()["anchors"], 6)
        assert_peg_balance(main, self.nodes)
        # An offence kept for the end: by then long out of the window for proofs.
        old_proof = self.offence(n0)

        self.log.info(f"A gap of the mainchain of six hours ({GAP} s): blocks of stake go on, soft")
        self.wait_until(lambda: len(main.getrawmempool()) > 0, timeout=60)
        before = n0.getanchorinfo()
        height = n0.getblockcount()
        start = time.time()
        while time.time() - start < GAP:
            time.sleep(10)
            for n in self.nodes:
                error = n.getposinfo()["producer"]["error"]
                assert "seed" not in error and "too long" not in error, error
            assert_equal(n0.getposinfo()["stalled"], False)
        self.wait_in_sync()
        made = n0.getblockcount() - height
        # A block a slot, as before the gap (some slots of a missing leader or a race may be empty).
        assert_greater_than(made, GAP * 3 // 4)
        info = n0.getposinfo()
        assert_greater_than(info["slot"] - info["firstunanchoredslot"], GAP)
        assert_greater_than(MAX_UNANCHORED, info["slot"] - info["firstunanchoredslot"])
        after = n0.getanchorinfo()
        assert_equal((after["anchors"], after["anchoredheight"], after["finalheight"]), (before["anchors"], before["anchoredheight"], before["finalheight"]))
        gap_tip = n0.getblockcount()
        assert_equal(n0.getblock(n0.getbestblockhash())["finality"]["status"], "soft")
        assert all(n0.getblock(n0.getblockhash(h))["kind"] == "pos" for h in range(height + 1, gap_tip + 1))

        self.log.info("The mainchain comes back: the ticket made before the gap anchors in time, the next ones the rest")
        anchors = after["anchors"]
        self.tick()
        self.wait_until(lambda: n0.getposinfo()["anchors"] > anchors, timeout=60)
        for _ in range(6):
            self.tick()
            if n0.getanchorinfo()["finalheight"] >= gap_tip:
                break
        self.wait_until(lambda: n0.getanchorinfo()["finalheight"] >= gap_tip, timeout=60)
        self.wait_in_sync()
        assert_peg_balance(main, self.nodes)

        self.log.info(f"Unbonding: paid {UNBONDING} anchors after the unbond, not one before")
        bond = n0.createbond(XONLY[0], Decimal("0.01"))
        self.wait_until(lambda: n0.gettransaction(bond["txid"])["confirmations"] > 0, timeout=60)
        unbond = n0.createunbond(XONLY[0], "all")
        self.wait_until(lambda: n0.gettransaction(unbond["txid"])["confirmations"] > 0, timeout=60)
        block = n0.gettransaction(unbond["txid"])["blockhash"]
        release = n0.listbonds()[0]["unbonds"][0]["releaseanchor"]
        assert_equal(release, self.anchors_at(n0, block) + UNBONDING)
        owner = unbond["owner"]
        received = n0.getreceivedbyaddress(owner, 0)
        # Meanwhile, once the sets are those of anchors after the gap: an offence (a validator other than the bond's),
        # its proof given one anchor before its window closes.
        resumed = anchors
        proof = None
        submitted = False
        while n0.getposinfo()["anchors"] < release or not submitted:
            now = n0.getposinfo()["anchors"]
            if now < release:
                # (The owner's address may be paid other things meanwhile: fees of the pool.)
                assert_equal(n0.listbonds()[0]["unbonding"], Decimal("0.01"))
                assert n0.getreceivedbyaddress(owner, 0) - received < Decimal("0.01")
            if proof is None and now >= resumed + LEAD_ANCHORS + 4:
                proof = self.offence(n0, exclude=XONLY[0])
                proof["set"] = self.set_anchors(n0, proof["slot"], proof["height"])
                assert_greater_than(proof["set"], resumed)
                assert now <= proof["set"] + WINDOW - 1
            if proof and not submitted and now >= proof["set"] + WINDOW - 1:
                self.log.info(f"A proof {proof['set'] + WINDOW - now} anchor(s) before its window closes: taken, carried, the offender slashed")
                assert now <= proof["set"] + WINDOW
                n0.submitevidence(proof["hex"])
                self.wait_until(lambda: self.carried(n0, proof["slot"]), timeout=60)
                submitted = True
                continue
            self.tick()
            self.wait_in_sync()
        assert submitted
        self.wait_until(lambda: n0.getreceivedbyaddress(owner, 0) - received >= Decimal("0.01") and n0.listbonds()[0]["unbonding"] == 0, timeout=60)
        self.wait_in_sync()
        assert_peg_balance(main, self.nodes)

        self.log.info("A proof long out of its window is never carried")
        n0.submitevidence(old_proof["hex"])
        self.tick(3)
        assert not self.carried(n0, old_proof["slot"])
        n0.setbmm(False)

    def offence(self, node, exclude=None):
        """A block of stake of the chain and a second one its leader signs for its slot: the proof (not given yet)."""
        blockhash = node.getbestblockhash()
        while True:
            info = node.getblock(blockhash)
            if info["kind"] == "pos" and info.get("proposer") and info["proposer"] != exclude:
                break
            blockhash = info["previousblockhash"]
        original = PosBlock(node.getblock(blockhash, 0))
        rival = PosBlock(node.getblock(blockhash, 0))
        coinbase = rival.vtx[0]
        coinbase.vin[0] = CTxIn(coinbase.vin[0].prevout, CScript(bytes(coinbase.vin[0].scriptSig) + bytes([1, 0x42])), coinbase.vin[0].nSequence)
        rival.resign(KEYS[XONLY.index(info["proposer"])], self.genesis)
        return {"hex": bytes(original.header).hex() + bytes(rival.header).hex(), "slot": info["slot"], "height": info["height"]}

    def slot_of(self, t):
        return max(0, t - self.genesis_time)

    def anchors_at(self, node, blockhash):
        """The anchors of the chain up to a block (it included)."""
        height = node.getblock(blockhash)["height"]
        info = node.getanchorinfo(100)
        return info["anchors"] - sum(1 for a in info["recent"] if a["height"] > height)

    def set_anchors(self, node, slot, height):
        """The anchors of the chain at the anchor that fixed the set of `slot`'s epoch (pos::SetBoundary, SetFor)."""
        epoch = slot // EPOCH
        boundary = min((epoch - 1) * EPOCH, self.slot_of(self.genesis_time + epoch * EPOCH - LEAD))
        info = node.getanchorinfo(100)
        for i, a in enumerate(info["recent"]):
            if a["height"] >= height:
                continue
            main_time = self.main.getblockheader(self.main.getblockhash(a["mainheight"]))["mediantime"]
            anchor_slot = max(self.slot_of(node.getblock(a["block"])["time"]), self.slot_of(main_time))
            if anchor_slot < boundary:
                return info["anchors"] - i
        raise AssertionError(f"no anchor fixed the set of slot {slot} (boundary {boundary}): " + str([(info["anchors"] - i, a["height"], a["mainheight"]) for i, a in enumerate(info["recent"])][:40]))

    def carried(self, node, slot):
        """Whether a block of the last few dozen carries a proof of `slot`."""
        blockhash = node.getbestblockhash()
        for _ in range(80):
            block = node.getblock(blockhash, 2)
            if any(e.get("slot") == slot for e in block.get("evidence", [])):
                return True
            blockhash = block["previousblockhash"]
        return False


if __name__ == '__main__':
    LayersEcashGapTest(__file__).main()
