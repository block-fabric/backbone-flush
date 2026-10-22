#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Layers' own questions through the host over RPC (src/webui/remotehost.h), as the Chains app asks them:

  - the assets (BitAssets): a send of an asset, a reservation with its registration in one question (the
    registration then runs once on its authorization, without a question), described with their own rows;
  - the DEX: a swap and liquidity, with their amounts; a swap whose parameters were changed after the
    question is refused, its token used up;
  - staking (FastBlocks): a bond, asked through the host method fastblocks.bond, run with its token;
  - a withdrawal to the mainchain (the kit's describer of a sidechain).
"""

from decimal import Decimal

from feature_layers import KEYS, LayersTest
from test_framework.key import compute_xonly_pubkey
from feature_sidechain import ACTIVATION_PERIOD
from test_framework.fastblocks import SLOT
from test_framework.util import assert_equal, assert_raises_rpc_error

REFUSED = -32802


def rows(description):
    return {r["label"]: r["value"] for r in description["rows"]}


class LayersDescribeConfirmationTest(LayersTest):
    def set_test_params(self):
        super().set_test_params()
        self.num_nodes = 1
        # The DEX from the first block: a route (swaproute) is one of its orders.
        self.side_args = self.side_args + ["-testactivationheight=dex@0"]
        self.extra_args = [self.side_args]

    def run_test(self):
        super().run_test()

    def run_sidechain_test(self):
        main = self.main
        n0 = self.nodes[0]
        self.main_address = main.getnewaddress()
        self.side_address = n0.getnewaddress()

        self.log.info("Setup: the slot, a deposit, an asset and a pool")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "Flush", "Tokens, assets and a DEX on a fast chain over eCash", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 50)
        self.mine_main()
        self.bmm()
        n0.sendmany("", {n0.getnewaddress(): 1 for _ in range(10)})
        self.bmm()
        n0.reserveasset("GOLD")
        for _ in range(4):
            self.bmm()
        n0.registerasset("GOLD", 1000000, 2)
        self.bmm()
        n0.addliquidity("GOLD", 10000, "ECX", 20)
        self.bmm()
        wallet = n0.listwallets()[0]
        # A confirmation names an asset by its id only ("0x" and its hash): never a name that may look alike.
        gold = "0x" + n0.getasset("GOLD")["asset"]
        chn = "ECX"

        self.log.info("An asset sent: described with its destination and amount, run with its token")
        to = n0.getnewaddress()
        d = n0.describeconfirmation("sendasset", [to, gold, 25], wallet)
        assert_equal(d["needed"], True)
        assert_equal(d["title"], "Send an asset")
        assert_equal(rows(d)["address"], to)
        kinds = {r["label"]: r["kind"] for r in d["rows"]}
        assert_equal(kinds["address"], "address")
        result = n0.webuihost(d["request"], d["token"])["result"]
        txid = result["txid"] if isinstance(result, dict) else result
        assert txid in n0.getrawmempool()
        assert_raises_rpc_error(REFUSED, "not known", n0.webuihost, d["request"], d["token"])
        self.bmm()

        self.log.info("A swap: its amounts; changed after the question, refused")
        d = n0.describeconfirmation("swapasset", [gold, 100, chn, 1, 10, "0.1"], wallet)
        assert_equal(d["title"], "Swap")
        assert "min_out" in rows(d)
        tampered = dict(d["request"])
        tampered["params"] = dict(d["request"]["params"], params=[gold, 100, chn, 1, 10, "0"])
        assert_raises_rpc_error(REFUSED, "not the one that was confirmed", n0.webuihost, tampered, d["token"])
        assert_raises_rpc_error(REFUSED, "not known", n0.webuihost, d["request"], d["token"])
        d = n0.describeconfirmation("swapasset", [gold, 100, chn, 1, 10, "0.1"], wallet)
        n0.webuihost(d["request"], d["token"])
        self.bmm()

        self.log.info("A route through pools, by its assets and by its pools' ids: each resolved in the question; reads of the DEX for the pages")
        n0.reserveasset("SILV")
        for _ in range(4):
            self.bmm()
        n0.registerasset("SILV", 1000000, 2)
        self.bmm()
        n0.addliquidity("SILV", 10000, "ECX", 20)
        self.bmm()
        silv = "0x" + n0.getasset("SILV")["asset"]
        read = lambda method, params: n0.webuihost({"kind": "rpc", "method": method, "params": params})["result"]
        quote = read("quoteroute", [gold, 10, silv])
        assert_equal(quote["path"], ["GOLD", "ECX", "SILV"])
        assert_equal(len(quote["routes"][0]["hops"]), 2)
        assert read("getdexinfo", [])["active"]
        d = n0.describeconfirmation("swaproute", [[gold, chn, silv], 10, 1, (quote["amount_out"] * Decimal("0.98")).quantize(Decimal("0.01"))], wallet)
        assert_equal(d["title"], "Trade along a route")
        shown = rows(d)
        assert any(gold[2:10] in v for v in shown.values()), shown
        assert "min_out" in shown and "amount_in" in shown
        result = n0.webuihost(d["request"], d["token"])["result"]
        assert result["txid"] in n0.getrawmempool()
        self.bmm()
        # The same route by its pools' ids (swappools): each pool named in the question by its id.
        quote = read("quoteroute", [gold, 10, silv])
        pools = [h["pool"] for h in quote["routes"][0]["hops"]]
        d = n0.describeconfirmation("swappools", [gold, 10, pools, 1, (quote["amount_out"] * Decimal("0.98")).quantize(Decimal("0.01"))], wallet)
        shown = rows(d)
        assert pools[0][:8] in shown["Pool 1"] and pools[1][:8] in shown["Pool 2"], shown
        result = n0.webuihost(d["request"], d["token"])["result"]
        assert result["txid"] in n0.getrawmempool()
        self.bmm()

        self.log.info("Liquidity: both amounts shown")
        d = n0.describeconfirmation("addliquidity", [gold, 100, chn, "0.2"], wallet)
        assert_equal(d["title"], "Add liquidity")
        assert {"amount_a", "amount_b"} <= set(rows(d))

        self.log.info("A reservation and its registration, in one question")
        request = {"kind": "host", "method": "rpc.confirmed",
                   "params": {"method": "reserveasset", "params": ["SILVER"], "wallet": wallet,
                              "then": {"method": "registerasset", "params": ["SILVER", 5000, 2]}}}
        ask = n0.webuihost(request)["confirm"]
        assert "Step 1 of 2" in [r["label"] for r in ask["rows"]]
        assert "Step 2 of 2" in [r["label"] for r in ask["rows"]]
        result = n0.webuihost(request, ask["token"])["result"]
        assert_equal(result["authorized"], 3600)
        for _ in range(4):
            self.bmm()
        # The second step, exactly as shown, runs on its authorization: no question.
        second = {"kind": "host", "method": "rpc.confirmed",
                  "params": {"method": "registerasset", "params": ["SILVER", 5000, 2], "wallet": wallet, "authorizedOnly": True}}
        assert "result" in n0.webuihost(second)
        self.bmm()
        assert "SILVER" in [a["ticker"] if "ticker" in a else a.get("name") for a in n0.listassets()]

        self.log.info("A bond (staking): the host method asks, with the key and the amount, before anything")
        pubkey = compute_xonly_pubkey(KEYS[0])[0].hex()
        bond = {"kind": "host", "method": "fastblocks.bond", "params": {"wallet": wallet, "pubkey": pubkey, "amount": "1"}}
        ask = n0.webuihost(bond)["confirm"]
        assert "bond" in ask["title"].lower()
        assert pubkey in [r["value"] for r in ask["rows"]]
        assert ask["token"]

        self.log.info("A withdrawal to the mainchain (the kit's describer)")
        d = n0.describeconfirmation("createwithdrawal", [main.getnewaddress(), "1", "0.0001"], wallet)
        assert_equal(d["title"], "Withdraw to eCash")
        assert_equal(rows(d)["Leaves this wallet"].split(" ")[0], "1.00010000")


if __name__ == "__main__":
    LayersDescribeConfirmationTest(__file__).main()
