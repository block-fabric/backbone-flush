#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Demand-driven anchoring (8.3.7, doc/fastblocks-anchor-bids.md, "Since 8.3.7: when the anchorer bids").

Three genesis validators, 15-second slots on mock time, a run of 60 slots without an anchor before blocks of stake halt,
as feature_fastblocks_anchorpolicy.py, but a mainchain that takes every request (no congestion), and a second sidechain
active in slot 6, whose BMM requests the test makes itself: a mainchain block can carry BMM without this chain's
commitment. The anchorers of nodes 1 and 2 are off unless said.

  - the new fields in getanchorbidpolicy, setanchorbidpolicy and getbmminfo's anchoring; an anchorbids.json of 8.3.6
    still loads;
  - misses that are not ours: a block without any BMM commitment (nobmm) or found less than 4 s after the bid
    (toosoon) is no miss and makes no stale bid to raise above; a block with BMM 4 s or more after it is a miss
    (then the next bid is raised above it);
  - batches: Fees bids no fee share below batch x the landing price, and bids it at or above;
  - value at risk: a transaction moving more than valueatrisk in a block no anchor covers makes a bid;
  - the anchor reserve: a fee bid keeps back reservepct of its share, credited when it lands; kept across a restart;
  - the heartbeat after heartbeathours without an anchor, not held back by a commitment of an unknown ticket; landed,
    debited from the reserve;
  - taking turns: a validator not designated holds its bid; a block found less than 4 s into the wait does not count
    toward the backup, blocks 4 s or more into it do, and after turnbackup of them it bids as the backup; a stall bid
    never waits.
"""

import json
import os
import time
from decimal import Decimal

from feature_fastblocks_anchorpolicy import OTHER_SLOT, FastBlocksAnchorPolicyTest, sats
from feature_fastblocks_idle import FastBlocksIdleTest
from test_framework.fastblocks import REGTEST_GENESIS_TIME, SLOT
from test_framework.util import assert_equal, assert_greater_than

MAX_UNANCHORED = 60


class FastBlocksAnchorDemandTest(FastBlocksAnchorPolicyTest):
    def set_test_params(self):
        FastBlocksIdleTest.set_test_params(self)
        # (No -blockmintxfee: every request goes in, unless the test pushes it out.)
        self.main_extra_args = ["-fallbackfee=0.001"]
        self.side_args = self.side_args + [f"-testposparam=max_unanchored_slots@{MAX_UNANCHORED}"]
        self.extra_args = [self.side_args] * self.num_nodes

    def run_test(self):
        super().run_test()

    def ours(self):
        """The txids of this chain's requests in the mainchain's mempool for the block after its tip (not the other slot's,
        nor those pushed out; a request for an older block cannot be mined any more)."""
        tip = bytes.fromhex(self.main.getbestblockhash())[::-1].hex()
        found = []
        for txid in self.main.getrawmempool():
            if txid in self.other_requests or txid in self.pushed_out:
                continue
            for out in self.main.getrawtransaction(txid, True)["vout"]:
                script = out["scriptPubKey"]["hex"]
                # (OP_RETURN, a push of 68: 00 bf 00, the slot, the commitment, the mainchain block it follows.)
                if len(script) == 140 and script.startswith("6a4400bf00") and script[76:] == tip:
                    found.append(txid)
        return found

    def wait_bid(self, kind=None, timeout=120):
        """This chain's bid for the next mainchain block (node 0's, in its history, for `kind` if given: its basis), in the mempool."""
        def made():
            history = self.nodes[0].getanchorinfo(1)["bid"]["history"]
            return (self.ours() and history and history[0]["mainheight"] == self.main.getblockcount() + 1 and history[0]["status"] == "pending"
                    and (kind is None or history[0]["basis"] == kind))
        self.wait_until(made, timeout=timeout)
        return self.last_bid(self.nodes[0])

    def mine_block(self, lead, other_bmm, ours_out=True):
        """A mainchain block `lead` seconds on (from now): with the other slot's BMM request if `other_bmm`, and with this
        chain's requests pushed out (prioritisetransaction) if `ours_out`."""
        if ours_out:
            for txid in self.ours():
                self.main.prioritisetransaction(txid=txid, fee_delta=-100_000_000)
                self.pushed_out.add(txid)
        if other_bmm:
            self.other_requests.add(self.main.createbmmrequest(OTHER_SLOT, os.urandom(32).hex(), Decimal("0.0001"))["txid"])
        self.set_time(self.now + lead)
        self.mine_main()

    def settled(self, n, ticket, status):
        self.wait_until(lambda: any(h["ticket"] == ticket and h["status"] == status for h in n.getanchorinfo(50)["bid"]["history"]))

    def last_bid(self, n):
        return n.getanchorinfo(1)["bid"]["history"][0]

    def run_sidechain_test(self):
        main, n0 = self.main, self.nodes[0]
        self.main_address = main.getnewaddress()
        self.other_requests = set()
        self.pushed_out = set()
        self.setup_chain_and_validators()
        for n in self.nodes[1:]:
            n.setanchorbidpolicy({"strategy": "off"})

        self.log.info("The new fields: in the policy, settable, and getbmminfo's anchoring")
        policy = n0.getanchorbidpolicy()["policy"]
        assert_equal((policy["batch"], policy["heartbeathours"], sats(policy["valueatrisk"]), policy["reservepct"], sats(policy["reservetarget"]),
                      policy["turns"], policy["turnbackup"]), (10, 6, 100_000_000, 20, 1_000_000, True, 2))
        info = n0.setanchorbidpolicy({"batch": 3, "heartbeathours": 1.5, "valueatrisk": "0.5", "reservepct": 30, "reservetarget": 0.002,
                                      "turns": False, "turnbackup": 3})
        assert_equal(sorted(info["overrides"]), ["batch", "heartbeathours", "preventstall", "reservepct", "reservetarget", "turnbackup", "turns", "valueatrisk"])
        with open(os.path.join(n0.chain_path, "settings.json"), encoding="utf8") as f:
            saved = json.load(f)["anchorbidpolicy"]
        assert_equal((saved["batch"], saved["turns"], sats(saved["reservetarget"])), (3, False, 200_000))
        anchoring = n0.getbmminfo()["anchoring"]
        assert_equal(sorted(anchoring), sorted(["batchneeded", "day", "deficit", "pot", "reserve", "reservetarget", "turn", "valueatrisk", "week"] +
                                               (["nextheartbeat"] if "nextheartbeat" in anchoring else [])))
        assert_equal(sorted(anchoring["day"]), ["anchors", "collected", "missed", "nobmm", "paid", "toosoon"])
        assert_equal(sorted(anchoring["turn"]), ["designated", "mainheight"])
        assert_equal(sats(anchoring["reservetarget"]), 200_000)

        self.log.info("An anchorbids.json of 8.3.6 (without the new fields) loads")
        self.stop_node(0)
        old = {"version": 1, "landed": [[self.now - 60, 1234]], "missed": 0,
               "history": [{"mainheight": main.getblockcount() - 1, "maintip": main.getblockhash(main.getblockcount() - 1), "ticket": "ab" * 32,
                            "offer": 1234, "kind": "fixed", "reason": "8.3.6", "pressure": 0, "status": 1, "time": self.now - 60, "trigger": "anchor", "work": False}]}
        with open(os.path.join(n0.chain_path, "anchorbids.json"), "w", encoding="utf8") as f:
            json.dump(old, f)
        self.restart_validator(0)
        bid = n0.getanchorinfo(50)["bid"]
        assert_equal(sats(bid["spentperiod"]), 1234)
        entry = next(h for h in bid["history"] if h["ticket"] == "ab" * 32)
        assert_equal((entry["status"], sats(entry["pot"]), sats(entry["holdback"]), entry["basis"]), ("landed", 0, 0, "fixed"))
        assert_equal(sats(n0.getbmminfo()["anchoring"]["reserve"]), 0)
        n0.setanchorbidpolicy(reset=True)

        self.log.info("Misses that are not ours: a block without BMM, a block too soon; then a real miss")
        n0.setanchorbidpolicy({"strategy": "fixed", "fixed": 0.00002, "preventstall": False, "periodcap": 0, "turns": False})
        # (The requests made before out of a block first: the bids below are each for the block after the tip.)
        self.mine_block(lead=10, other_bmm=False)
        self.wait_bid()
        before = n0.getbmminfo()["anchoring"]["day"]
        # Without any BMM commitment: nobmm, no miss; the next bid not raised.
        ticket = self.wait_bid()["ticket"]
        self.mine_block(lead=10, other_bmm=False)
        self.settled(n0, ticket, "nobmm")
        self.wait_bid()
        bid = n0.getanchorinfo()["bid"]
        assert_equal((bid["missed"], bid["kind"], sats(bid["effective"])), (0, "fixed", 2000))
        # With BMM (the other slot's) 1 s after the bid: too soon, no miss.
        ticket = self.wait_bid()["ticket"]
        self.mine_block(lead=1, other_bmm=True)
        self.settled(n0, ticket, "toosoon")
        self.wait_bid()
        bid = n0.getanchorinfo()["bid"]
        assert_equal((bid["missed"], bid["kind"], sats(bid["effective"])), (0, "fixed", 2000))
        # With BMM 10 s after it: a miss of ours; the next bid above it (which other nodes may still hold).
        ticket = self.wait_bid()["ticket"]
        self.mine_block(lead=10, other_bmm=True)
        self.settled(n0, ticket, "missed")
        self.wait_until(lambda: n0.getanchorinfo()["bid"]["kind"] == "replace")
        bid = n0.getanchorinfo()["bid"]
        assert_equal((bid["missed"], sats(bid["effective"])), (1, 2500))
        day = n0.getbmminfo()["anchoring"]["day"]
        assert_equal((day["missed"] - before["missed"], day["nobmm"] - before["nobmm"], day["toosoon"] - before["toosoon"]), (1, 1, 1))
        # Then one lands (the other slot's request too: one each).
        ticket = self.wait_bid()["ticket"]
        self.mine_block(lead=10, other_bmm=True, ours_out=False)
        self.settled(n0, ticket, "landed")
        self.next_slot(settle=1)
        self.wait_until(lambda: n0.getanchorinfo()["bid"]["missed"] == 0)

        self.log.info("Batches: no fee share below batch x the landing price; at or above it, the share")
        n0.setanchorbidpolicy({"strategy": "fees", "preventstall": True, "floor": 0, "valueatrisk": 0, "heartbeathours": 0, "turns": False,
                               "reservepct": 0, "batch": 1000})
        self.mine_block(lead=10, other_bmm=False, ours_out=True)
        txid = n0.sendtoaddress(address=self.nodes[1].getnewaddress(), amount=0.1, fee_rate=200)
        for _ in range(10):
            self.next_slot(settle=1)
            if n0.gettransaction(txid)["confirmations"] > 0:
                break
        # (The pot of a ticket for the next mainchain block, which anchors a block after the transaction's: mainchain
        # blocks without BMM until there is one.)
        for _ in range(6):
            self.mine_block(lead=10, other_bmm=False)
            self.next_slot(settle=1)
            if sats(n0.getbmminfo()["anchoring"]["pot"]) > 0:
                break
        self.log.info(f"pot: {n0.getposinfo()['pot']}")
        self.wait_until(lambda: sats(n0.getbmminfo()["anchoring"]["pot"]) > 0)
        anchoring = n0.getbmminfo()["anchoring"]
        share, needed = sats(anchoring["pot"]), sats(anchoring["batchneeded"])
        self.log.info(f"fee share {share} sat, a batch of 1000 needs {needed}")
        assert_greater_than(needed, share)
        bid = n0.getanchorinfo()["bid"]
        assert_equal(bid["kind"], "idle")
        assert "waits for a batch" in bid["reason"], bid["reason"]
        # The landing price (the relay minimum here, the mainchain's relay rate x 250 vB): a batch the share reaches.
        landing = needed // 1000
        batch = share // landing
        assert 2 <= batch < 1000, (share, landing)
        n0.setanchorbidpolicy({"batch": batch})
        self.wait_until(lambda: n0.getanchorinfo()["bid"]["kind"] == "fees")
        assert_equal(sats(n0.getbmminfo()["anchoring"]["batchneeded"]), max(500, landing * batch))

        self.log.info("The anchor reserve: a fee bid keeps back reservepct, credited when it lands, kept across a restart")
        n0.setanchorbidpolicy({"reservepct": 50, "reservetarget": 1})
        # (The bid of the whole share for this mainchain block out of a block without BMM: a new one for the next.)
        self.mine_block(lead=10, other_bmm=False)
        self.wait_until(lambda: sats(n0.getanchorinfo()["bid"]["effective"]) == share - share // 2)
        made = self.wait_bid(kind="fees")
        assert_equal((sats(made["holdback"]), sats(made["offer"])), (share // 2, share - share // 2))
        self.mine_block(lead=10, other_bmm=False, ours_out=False)
        self.settled(n0, made["ticket"], "landed")
        anchoring = n0.getbmminfo()["anchoring"]
        assert_equal(sats(anchoring["reserve"]), share // 2)
        assert_equal(anchoring["day"]["anchors"].get("fees"), 1)
        assert_equal(sats(anchoring["day"]["paid"]) >= share - share // 2, True)
        reserve = sats(anchoring["reserve"])
        self.restart_validator(0)
        assert_equal(sats(n0.getbmminfo()["anchoring"]["reserve"]), reserve)

        self.log.info("Value at risk: a transaction moving more than valueatrisk, in a block no anchor covers, makes a bid")
        n0.setanchorbidpolicy({"reservepct": 0, "batch": 1000, "valueatrisk": 2})
        self.next_slot(settle=1)
        txid = n0.sendtoaddress(address=self.nodes[1].getnewaddress(), amount=3, fee_rate=1)
        for _ in range(10):
            self.next_slot(settle=1)
            if n0.gettransaction(txid)["confirmations"] > 0:
                break
        self.wait_until(lambda: sats(n0.getbmminfo()["anchoring"]["valueatrisk"]) >= 300_000_000)
        self.wait_until(lambda: n0.getanchorinfo()["bid"]["kind"] in ("valueatrisk", "throttled"))
        assert_equal(n0.getanchorinfo()["bid"]["pendingactions"], False)
        made = self.wait_bid(kind="valueatrisk")
        self.mine_block(lead=10, other_bmm=False, ours_out=False)
        self.settled(n0, made["ticket"], "landed")
        # Anchored (acknowledged in a block of stake) up to the transaction's block (a ticket may anchor a block before it,
        # then bid for again): nothing at risk but what came after.
        for _ in range(8):
            self.next_slot(settle=1)
            if sats(n0.getbmminfo()["anchoring"]["valueatrisk"]) < 200_000_000:
                break
            if self.ours():
                self.mine_block(lead=10, other_bmm=False, ours_out=False)
        self.wait_until(lambda: sats(n0.getbmminfo()["anchoring"]["valueatrisk"]) < 200_000_000)

        self.log.info("The heartbeat: after heartbeathours without an anchor (here 12 slots), the competitive base; debited from the reserve")
        n0.setanchorbidpolicy({"valueatrisk": 0, "heartbeathours": 0.05})
        self.mine_block(lead=10, other_bmm=False, ours_out=True)
        self.wait_until(lambda: n0.getanchorinfo()["bid"]["kind"] == "idle")
        first = n0.getanchorinfo()["bid"]["firstunanchoredslot"]
        assert "nextheartbeat" in n0.getbmminfo()["anchoring"] or first < 0
        while n0.getanchorinfo()["bid"]["unanchoredslots"] < 12:
            assert_equal(n0.getanchorinfo()["bid"]["kind"], "idle")
            self.next_slot(settle=1)
        self.wait_until(lambda: n0.getanchorinfo()["bid"]["kind"] == "heartbeat")
        assert_equal(n0.getbmminfo()["anchoring"]["nextheartbeat"], REGTEST_GENESIS_TIME + (n0.getanchorinfo()["bid"]["firstunanchoredslot"] + 12) * 15)
        # A commitment of this chain whose ticket nobody knows (a bogus one, which anyone can land): it does not hold the
        # heartbeat back as a commitment waiting for its acknowledgement would.
        before = self.wait_bid(kind="heartbeat")
        for txid in self.ours():
            self.main.prioritisetransaction(txid=txid, fee_delta=-100_000_000)
            self.pushed_out.add(txid)
        self.pushed_out.add(main.createbmmrequest(SLOT, os.urandom(32).hex(), Decimal("0.0001"))["txid"])
        self.set_time(self.now + 10)
        self.mine_main()
        # (In the block: another commitment of this chain than the bid's, which settles outbid.)
        self.settled(n0, before["ticket"], "outbid")
        made = self.wait_bid(kind="heartbeat")
        # (What the reserve holds now: the value at risk's bids, without fees, landed above and were debited from it too.)
        reserve, deficit = sats(n0.getbmminfo()["anchoring"]["reserve"]), sats(n0.getbmminfo()["anchoring"]["deficit"])
        assert_greater_than(reserve, 0)
        self.mine_block(lead=10, other_bmm=False, ours_out=False)
        self.settled(n0, made["ticket"], "landed")
        anchoring = n0.getbmminfo()["anchoring"]
        assert_equal(sats(anchoring["reserve"]), max(reserve - sats(made["offer"]), 0))
        assert_equal(sats(anchoring["deficit"]), deficit + max(sats(made["offer"]) - reserve, 0))
        assert_equal(anchoring["day"]["anchors"].get("heartbeat"), 1)

        self.log.info("Taking turns: a validator not designated waits; only blocks 4 s or more into the wait count toward the backup")
        n0.setanchorbidpolicy({"heartbeathours": 0, "valueatrisk": 0.5, "turns": True, "turnbackup": 2})
        self.next_slot(settle=1)
        self.mine_block(lead=10, other_bmm=False)
        txid = n0.sendtoaddress(address=self.nodes[1].getnewaddress(), amount=1, fee_rate=1)
        for _ in range(10):
            self.next_slot(settle=1)
            if n0.gettransaction(txid)["confirmations"] > 0:
                break
        # (The others' anchorers are off, but they are in the set: two in three blocks are another's turn, which changes
        # with every mainchain block.) Until it bids as the backup: on its own turn it bids at once (pushed out here, in a
        # block without BMM); on another's it holds, and a block found 1 s into the wait does not count, one 10 s into it
        # does (with BMM or without: one nobody bid for counts too).
        def state():
            self.wait_until(lambda: n0.getbmminfo()["anchoring"]["turn"]["mainheight"] == main.getblockcount() + 1)
            bid = n0.getanchorinfo()["bid"]
            self.log.debug(f"mainchain block {main.getblockcount() + 1}: {bid['kind']}: {bid['reason']}")
            assert bid["kind"] != "stall", "the stall bid came first"
            return bid, n0.getbmminfo()["anchoring"]["turn"]["designated"]

        backed = soon_tested = False
        for _ in range(60):
            bid, designated = state()
            if "the backup" in bid["reason"]:
                assert not designated
                backed = True
                break
            if bid["kind"] == "turn":
                assert not designated
                assert_equal(self.ours(), [])
                if "in 2 mainchain blocks" in bid["reason"] and not soon_tested:
                    self.mine_block(lead=1, other_bmm=True)
                    after, designated = state()
                    if after["kind"] == "turn":
                        assert "in 2 mainchain blocks" in after["reason"], after["reason"]
                        soon_tested = True
                    continue
                self.mine_block(lead=10, other_bmm=False)
            else:
                if bid["kind"] == "valueatrisk":
                    assert designated, bid["reason"]
                    assert "this validator's turn" in bid["reason"], bid["reason"]
                self.mine_block(lead=10, other_bmm=False)
        assert backed, "never a backup bid"
        assert soon_tested, "never a block too soon in a wait"
        # A stall bid never waits for a turn.
        while n0.getanchorinfo()["bid"]["pressure"] < 0.5:
            self.next_slot()
        self.wait_until(lambda: n0.getanchorinfo()["bid"]["kind"] in ("stall", "replace", "periodcap"))
        assert "turn" not in n0.getanchorinfo()["bid"]["reason"]
        self.wait_bid()
        self.mine_block(lead=10, other_bmm=False, ours_out=False)
        self.next_slot(settle=1)
        assert_equal(len({n.getbestblockhash() for n in self.nodes}), 1)


if __name__ == '__main__':
    FastBlocksAnchorDemandTest(__file__).main()
