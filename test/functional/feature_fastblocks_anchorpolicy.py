#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The anchor bid policy of a validator's anchorer (doc/fastblocks-anchor-bids.md): its RPCs, and a congested mainchain.

Three genesis validators with 15-second slots on mock time, as feature_fastblocks_idle.py, a run of 60 slots without
an anchor before blocks of stake halt (-testposparam=max_unanchored_slots@60), and a mainchain whose miners take no
transaction below 50 sat/vB (-blockmintxfee): a congested mempool, whatever the floor (500 sat, ~2.6 sat/vB).

  - not a validator: getanchorbidpolicy explains; setanchorbidpolicy sets the node's one policy all the same (its merged
    miner's, setbmm), which getbmminfo shows with the day's spending;
  - setanchorbidpolicy: checked (floor above max, an unknown strategy, a share over 100 %, a pressure of 0, a falling
    curve), kept in settings.json over the options, read back after a restart; reset: the options' policy again;
  - congested: the floor bids stay out of the mainchain's blocks (missed, the "bid too low" warning, the rebids not
    enough) while the stall pressure p stays below the threshold (0.5); once p crosses it the escalated bid lands,
    the run ends, the bid drops back to the floor, and the chain never halts.
"""

import json
import os

from feature_fastblocks_idle import FastBlocksIdleTest
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error

MAX_UNANCHORED = 60
COIN = 100_000_000


def sats(value):
    return int(round(float(value) * COIN))


class FastBlocksAnchorPolicyTest(FastBlocksIdleTest):
    def set_test_params(self):
        super().set_test_params()
        # 50 sat/vB at least in a block; the mainchain wallet pays its own transactions 100 sat/vB.
        self.main_extra_args = ["-blockmintxfee=0.0005", "-fallbackfee=0.001"]
        self.side_args = self.side_args + [f"-testposparam=max_unanchored_slots@{MAX_UNANCHORED}"]
        self.extra_args = [self.side_args] * self.num_nodes

    def run_test(self):
        try:
            self.run_sidechain_test()
        finally:
            self.stop_mainchain()

    def validator_args(self, i):
        return self.side_args + [f"-validatorkeyfile={self.key_files[i]}", "-validatoranchorbid=0.000005"]

    def restart_validator(self, i):
        self.restart_node(i, extra_args=self.validator_args(i) + [f"-mocktime={self.now}"])
        for other in range(self.num_nodes):
            if other != i:
                self.connect_nodes(i, other)
        self.set_time(self.now)

    def run_sidechain_test(self):
        main = self.main
        n0 = self.nodes[0]
        self.main_address = main.getnewaddress()

        self.log.info("Not a validator: an explanation; the one policy is its merged miner's")
        info = n0.getanchorbidpolicy()
        assert_equal(info["validator"], False)
        assert_equal(info["bidding"], False)
        assert "not a validator" in info["explanation"]
        assert_equal((info["anchortrigger"], info["minetrigger"]), (False, False))
        info = n0.setanchorbidpolicy({"max": 0.0002, "periodcap": 0.003})
        assert_equal(sats(info["policy"]["max"]), 20_000)
        bmm = n0.getbmminfo()["bidpolicy"]
        assert_equal((sats(bmm["max"]), sats(bmm["periodcap"]), sats(bmm["spentperiod"]), sats(bmm["left"])), (20_000, 300_000, 0, 300_000))
        # A bid by hand is at most max.
        assert_raises_rpc_error(-8, "above the most a bid", n0.requestbmmblock, n0.getnewaddress(), 0.0003)
        n0.setanchorbidpolicy({}, True)
        assert_equal(n0.getanchorinfo()["bid"]["validator"], False)

        self.setup_chain_and_validators()

        self.log.info("The options' policy: fees + prevent stalling, the floor given")
        info = n0.getanchorbidpolicy()
        assert_equal((info["validator"], info["bidding"]), (True, True))
        assert_equal(info["policy"]["strategy"], "feespreventstall")
        assert_equal(sats(info["policy"]["floor"]), 500)
        assert_equal(sats(info["policy"]["max"]), 100_000)
        assert_equal(info["policy"]["escalationstart"], 0.5)
        assert_equal(info["overrides"], {})
        assert_equal(info["defaults"], info["policy"])

        self.log.info("setanchorbidpolicy checks what it is given")
        for fields, message in (
            ({"floor": 0.01}, "floor (0.01) above max"),
            ({"strategy": "always"}, "strategy"),
            ({"feeshare": 150}, "feeshare"),
            ({"escalationstart": 0}, "escalationstart"),
            ({"curve": "0.8:2,0.6:3"}, "ascending"),
            ({"max": 30000000}, "max"),
            ({"bid": 1}, "not a field"),
        ):
            assert_raises_rpc_error(-8, message, n0.setanchorbidpolicy, fields)
        assert_equal(n0.getanchorbidpolicy()["overrides"], {})

        self.log.info("Set, kept in settings.json, read back after a restart; reset")
        info = n0.setanchorbidpolicy({"max": 0.002, "curve": "0.5:1,0.75:3", "strategy": "minimal"})
        assert_equal(info["policy"]["strategy"], "minimal")
        assert_equal(info["policy"]["curve"], [{"pressure": 0.5, "multiplier": 1}, {"pressure": 0.75, "multiplier": 3}])
        assert_equal(sorted(info["overrides"]), ["curve", "max", "strategy"])
        assert_equal(info["defaults"]["strategy"], "feespreventstall")
        n0.setanchorbidpolicy(policy={"periodcap": 0})
        with open(os.path.join(n0.chain_path, "settings.json"), encoding="utf8") as f:
            saved = json.load(f)["anchorbidpolicy"]
        assert_equal(sorted(saved), ["curve", "max", "periodcap", "strategy"])
        self.restart_validator(0)
        info = n0.getanchorbidpolicy()
        assert_equal(info["policy"]["strategy"], "minimal")
        assert_equal(sats(info["policy"]["max"]), 200_000)
        assert_equal(sats(info["policy"]["periodcap"]), 0)
        assert_equal(len(info["policy"]["curve"]), 2)
        assert_equal(sorted(info["overrides"]), ["curve", "max", "periodcap", "strategy"])
        info = n0.setanchorbidpolicy(reset=True)
        assert_equal(info["overrides"], {})
        assert_equal(info["policy"], info["defaults"])
        with open(os.path.join(n0.chain_path, "settings.json"), encoding="utf8") as f:
            assert "anchorbidpolicy" not in json.load(f)

        self.log.info("Congested: node 0 the only bidder (the others off), a steep curve from p = 0.5")
        n0.setanchorbidpolicy({"strategy": "feespreventstall", "curve": "0.5:1,0.6:40", "warnmissed": 2, "flatboost": False})
        for n in self.nodes[1:]:
            info = n.setanchorbidpolicy({"strategy": "off"})
            assert_equal(info["bidding"], False)
            assert "relies on other bidders" in info["explanation"]
            assert any("off" in w for w in n.getanchorinfo()["bid"]["warnings"])
        bid = n0.getanchorinfo()["bid"]
        assert_equal((bid["validator"], bid["bidding"], bid["strategy"]), (True, True, "feespreventstall"))

        landed_at = None
        missed_before = 0
        warned = False
        halt_slot = None
        for k in range(150):
            slot = self.next_slot()
            pos = n0.getposinfo()
            first = pos["firstunanchoredslot"]
            if first >= 0 and halt_slot is None:
                halt_slot = first + MAX_UNANCHORED
            if k % 3 != 2:
                continue
            bid = n0.getanchorinfo(20)["bid"]
            p = bid["pressure"]
            committed = self.mine()
            self.log.debug(f"slot {slot}: p {p:.2f}, bid {bid['effective']} ({bid['kind']}), committed {committed}")
            info = n0.getanchorinfo(50)["bid"]
            if any("too low" in w for w in info["warnings"]):
                warned = True
            statuses = [h["status"] for h in info["history"]]
            if landed_at is None and "landed" in statuses:
                landed_at = slot
                landed = next(h for h in info["history"] if h["status"] == "landed")
                self.log.info(f"An escalated bid landed at slot {slot}: {landed['offer']} ({landed['reason']}), p then {landed['pressure']:.2f}")
                assert_equal(landed["kind"], "escalation")
                assert landed["pressure"] >= 0.5
                assert_greater_than(sats(landed["offer"]), 9000)
                # The mainchain committed to its ticket: getanchorticket says the block it anchors (Merged mining's table).
                ticket = n0.getanchorticket(landed["ticket"])
                assert_equal(ticket["known"], True)
                assert_equal(ticket["inactivechain"], True)
                assert_equal(n0.getblockheader(ticket["anchored"])["height"], ticket["anchoredheight"])
                assert_equal(n0.getanchorticket("00" * 32), {"hash": "00" * 32, "known": False})
                missed_before = statuses.count("missed")
                landed_offer = sats(landed["offer"])
            if landed_at is not None and slot > landed_at + 3 * 3:
                break
        assert landed_at is not None, "no escalated bid landed"
        assert_greater_than(missed_before, 1)
        assert warned, "no \"bid too low\" warning"

        self.log.info("The run is over: the floor again; the chain went on past where it would have halted")
        for _ in range(4):
            self.next_slot()
        bid = n0.getanchorinfo()["bid"]
        # (Still congested: the floor misses again, and the rebids raise it, x2 after 3 missed, up to x8: no escalation.)
        assert bid["pressure"] < 0.5
        # (Or 500 sat above the last bid that missed since, which other nodes may still hold on the same coin: replace.)
        assert bid["kind"] in ("floor", "rebid", "replace"), bid["kind"]
        if bid["kind"] != "replace":
            assert sats(bid["effective"]) <= 500 * 8
        else:
            assert_equal(sats(bid["effective"]), sats(bid["stalebid"]) + 500)
        assert_greater_than(landed_offer, sats(bid["effective"]))
        # Past where the first run would have halted, the mempool still congested: the next run escalates and lands
        # again. (next_slot waits for every node's block of the slot: blocks of stake did not halt.)
        slot, k = self.next_slot(), 0
        while slot <= halt_slot + MAX_UNANCHORED:
            slot, k = self.next_slot(), k + 1
            if k % 3 == 0:
                self.mine()
        landed = [h for h in n0.getanchorinfo(50)["bid"]["history"] if h["status"] == "landed"]
        assert_greater_than(len(landed), 1)
        assert_equal(len({n.getbestblockhash() for n in self.nodes}), 1)
        self.log.info(f"Missed {missed_before} mainchain blocks at the floor; landed at slot {landed_at}; the halt would have been at {halt_slot}")

    def setup_chain_and_validators(self):
        """As feature_fastblocks_idle.py: slot active, a deposit to node 0 in a BMM block, the validators on mock time."""
        import time
        from feature_sidechain import ACTIVATION_PERIOD
        from test_framework.fastblocks import KEYS, SLOT
        main = self.main
        n0 = self.nodes[0]
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "FastBlocks", "Blocks of stake between merged mined blocks", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 50)
        self.mine_main()
        block = n0.createbmmblock(n0.getnewaddress())
        main.createbmmrequest(SLOT, block["hash"], 0.001)
        self.mine_main()
        self.wait_until(lambda: all(n.getbestblockhash() == block["hash"] for n in self.nodes))
        self.key_files = []
        for i, key in enumerate(KEYS):
            path = os.path.join(self.options.tmpdir, f"validator{i}.key")
            with open(path, "w") as f:
                f.write(key.hex() + "\n")
            self.key_files.append(path)
        start = max(int(time.time()), main.getblock(main.getbestblockhash())["time"]) + 5
        self.now = start - start % 15
        for i in range(3):
            self.restart_node(i, extra_args=self.validator_args(i) + [f"-mocktime={self.now}"])
        self.connect_all()
        self.set_time(self.now)


if __name__ == '__main__':
    FastBlocksAnchorPolicyTest(__file__).main()
