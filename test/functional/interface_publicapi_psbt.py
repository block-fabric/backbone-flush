#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The public API's unsigned transactions (8.3.0, doc/public-api.md): a launch's two steps and a registration as PSBTs
for a wallet that signs what it is given (the ECX Wallet extension), on a regtest Flush chain.

  - POST psbt/reserve for an address of a wallet the node does not hold the key of in the API (another wallet signs):
    the PSBT's fields (version 0, every input's previous transaction and witness_utxo, nothing else), the commitment
    as HMAC-SHA256(nonce, SHA256(ticker) || script) computed here, its coins the address's only, its change back;
    refusals (a ticker taken, a bad ticker, a P2PKH address, an address without coins);
  - signed by that wallet without finalizing (partial signatures, as the extension), broadcast as {psbt}: the node
    finalizes it; an unsigned PSBT refused;
  - POST psbt/launch before the reservation is deep enough (in the mempool, then one block): 409, reason
    reservation_not_deep, its confirmations and the confirmations needed; a wrong nonce, another address, refused;
  - then the launch with build/launch's parameters in the query (a first buy, a description and links, a logo
    uploaded by its SHA-256): its burn is the first buy, max_burn_units below it refused, at it taken; getlaunch: the
    launch, its creator the address, the first buy's tokens at the address;
  - POST psbt/register on the same pattern: the asset registered, its supply and control coin at the address;
  - the wallet's own launchtoken alongside, unchanged (the shared code).
"""

import base64
import hashlib
import hmac
import os
import time
from decimal import Decimal
from urllib.parse import quote, urlencode

from feature_layers import LayersTest
from test_framework.publicapi import PublicApi
from test_framework.util import assert_equal, assert_greater_than
from test_framework.test_framework import SkipTest

DEX_HEIGHT = 10
CURVES_HEIGHT = 15


def api_port(n):
    from test_framework.util import p2p_port
    return p2p_port(n) + 470


class PublicApiPsbtTest(LayersTest):
    def set_test_params(self):
        super().set_test_params()
        self.num_nodes = 1
        common = self.side_args + [f"-testactivationheight=dex@{DEX_HEIGHT}", f"-testactivationheight=dexcurves@{CURVES_HEIGHT}", "-testactivationheight=ticker@0"]
        self.extra_args = [common + [f"-publicapi=127.0.0.1:{api_port(0)}", "-publicapirate=1000", "-publicapiburst=5000"]]

    def skip_test_if_missing_module(self):
        super().skip_test_if_missing_module()
        self.skip_if_no_wallet()
        if not self.config["components"].getboolean("ENABLE_PUBLICAPI", fallback=False):
            raise SkipTest("built without the public DEX API (WITH_PUBLICAPI)")

    def run_test(self):
        super().run_test()

    def bmm_to(self, height):
        while self.nodes[0].getblockcount() < height:
            self.bmm()

    def post(self, api, path, body, query=None):
        qs = ("?" + urlencode(query, doseq=True, quote_via=quote)) if query else ""
        return api.post(path + qs, body)

    def broadcast(self, api, body):
        """POST broadcast, waiting out its limit (3 at once, 6 a minute per client)."""
        while True:
            r = api.post("broadcast", body)
            if r.status != 429:
                return r
            time.sleep(int(r.headers.get("retry-after", "1")))

    def ok(self, reply):
        assert_equal(reply.status, 200)
        return reply.json()

    def check_psbt(self, node, psbt, address):
        """The PSBT as the extension reads it: every input its previous transaction and the output it spends, the
        address's only; no derivation, no unknown or proprietary field; version 0."""
        decoded = node.decodepsbt(psbt)
        assert_equal(decoded["psbt_version"], 0)
        assert_equal(decoded["unknown"], {})
        assert "proprietary" not in decoded or decoded["proprietary"] == []
        assert "global_xpubs" not in decoded or decoded["global_xpubs"] == []
        for i, inp in enumerate(decoded["inputs"]):
            assert "non_witness_utxo" in inp, inp
            assert "witness_utxo" in inp, inp
            assert_equal(inp["witness_utxo"]["scriptPubKey"]["address"], address)
            prev = inp["non_witness_utxo"]
            vin = decoded["tx"]["vin"][i]
            assert_equal(prev["txid"], vin["txid"])
            assert_equal(prev["vout"][vin["vout"]]["scriptPubKey"]["address"], address)
            assert "bip32_derivs" not in inp
            assert "unknown" not in inp or inp["unknown"] == {}
        for out in decoded["outputs"]:
            assert "bip32_derivs" not in out
            assert "unknown" not in out or out["unknown"] == {}
        # Everything that is not the marker comes back to the address.
        for out in decoded["tx"]["vout"]:
            if out["scriptPubKey"]["type"] != "nulldata":
                assert_equal(out["scriptPubKey"]["address"], address)
        return decoded

    def sign(self, wallet, psbt):
        """As the extension: partial signatures only, not finalized."""
        signed = wallet.walletprocesspsbt(psbt, True, "ALL", False, False)
        assert "hex" not in signed
        return signed["psbt"]

    def run_sidechain_test(self):
        main = self.main
        n0 = self.nodes[0]
        self.main_address = main.getnewaddress()
        self.side_address = n0.getnewaddress()
        from feature_sidechain import ACTIVATION_PERIOD
        from test_framework.fastblocks import SLOT
        from feature_bitassets_logo import b64, make_png
        api = PublicApi(api_port(0))

        self.log.info("Setup: the slot, a deposit, the DEX's curves")
        self.mine_main(300)
        main.createsidechainproposal(SLOT, "Flush", "Tokens on a fast chain", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 1200)
        self.mine_main()
        self.bmm()
        self.bmm_to(CURVES_HEIGHT)

        # The external wallet: its own key; the API is only given its address.
        n0.createwallet("ext")
        ext = n0.get_wallet_rpc("ext")
        funder = n0.get_wallet_rpc(self.default_wallet_name)
        address = ext.getnewaddress("", "bech32")
        script = bytes.fromhex(ext.getaddressinfo(address)["scriptPubKey"])
        empty = ext.getnewaddress("", "bech32")
        for amount in (3, 2):
            funder.sendtoaddress(address, amount)
        # A coin elsewhere of the same wallet: never spent by a build for `address`.
        funder.sendtoaddress(ext.getnewaddress("", "bech32"), 1)
        self.bmm()
        status = api.rest("status")

        self.log.info("psbt/reserve: refusals")
        nonce = os.urandom(32).hex()
        assert_equal(self.post(api, "psbt/reserve", {"address": address, "ticker": "x", "nonce": nonce}).status, 400)
        assert_equal(self.post(api, "psbt/reserve", {"address": funder.getnewaddress("", "legacy"), "ticker": "PSBTX", "nonce": nonce}).status, 400)
        assert_equal(self.post(api, "psbt/reserve", {"address": address, "ticker": "PSBTX", "nonce": "00"}).status, 400)
        no_coins = self.post(api, "psbt/reserve", {"address": empty, "ticker": "PSBTX", "nonce": nonce})
        assert_equal(no_coins.status, 400)
        assert "fund it" in no_coins.json()["error"]["message"]
        # A GET of a PSBT build is not one.
        assert_equal(api.get("psbt/reserve").status, 501)

        self.log.info("psbt/reserve: the commitment, the coins, the PSBT")
        reserve = self.ok(self.post(api, "psbt/reserve", {"address": address, "ticker": "psbtx", "nonce": nonce}))
        assert_equal(reserve["ticker"], "PSBTX")
        name_hash = hashlib.sha256(b"PSBTX").digest()
        assert_equal(reserve["asset"], name_hash[::-1].hex())
        commitment = hmac.new(bytes.fromhex(nonce), name_hash + script, hashlib.sha256).digest()
        assert_equal(reserve["commitment"], commitment[::-1].hex())
        assert_equal(reserve["reservation_vout"], 0)
        assert_equal(reserve["reservation"], reserve["txid"] + ":0")
        assert_equal(reserve["burn_units"], "0")
        assert_equal(reserve["confirmations_needed"], 2)  # regtest's reveal depth
        decoded = self.check_psbt(n0, reserve["psbt"], address)
        assert_equal(decoded["tx"]["txid"], reserve["txid"])
        assert_equal(decoded["tx"]["vout"][0]["value"], 0)
        assert_equal(decoded["tx"]["vout"][1]["scriptPubKey"]["type"], "nulldata")
        assert_equal(Decimal(reserve["fee_units"]), decoded["fee"] * 100000000)
        # The wallet's fee rate: no estimate on a new chain, its fallback of 20 sat/vB.
        assert 19.5 < reserve["fee_rate"] < 21, reserve["fee_rate"]
        # The largest coin is enough: one input.
        assert_equal(len(decoded["tx"]["vin"]), 1)

        self.log.info("Broadcast: an unsigned PSBT refused, the signed one (not finalized) taken")
        unsigned = self.broadcast(api, {"psbt": reserve["psbt"]})
        assert_equal(unsigned.status, 400)
        assert "not complete" in unsigned.json()["error"]["message"]
        assert_equal(self.broadcast(api, {"psbt": reserve["psbt"], "hex": "00"}).status, 400)
        signed = self.sign(ext, reserve["psbt"])
        sent = self.ok(self.broadcast(api, {"psbt": signed}))
        assert_equal(sent["txid"], reserve["txid"])

        launch_query = {"name": "Psbt Coin", "description": "Launched from an external wallet", "links": ["https://example.org/psbtx"],
                        "first_buy_ecx": "1", "slippage_bps": "100"}
        png = make_png([10, 120, 200, 240, 240, 10])
        logo = self.ok(api.post("logos", {"base64": b64(png)}))
        launch_query["logo_sha256"] = logo["sha256"]
        body = {"address": address, "ticker": "PSBTX", "nonce": nonce, "reservation": reserve["reservation"]}

        self.log.info("psbt/launch: not deep enough (in the mempool, then one block): 409 and the confirmations")
        waiting = self.post(api, "psbt/launch", body, launch_query)
        assert_equal(waiting.status, 409)
        err = waiting.json()["error"]
        assert_equal((err["code"], err["reason"], err["confirmations"], err["needed"]), ("aborted", "reservation_not_deep", 0, 2))
        self.bmm()
        waiting = self.post(api, "psbt/launch", body, launch_query)
        assert_equal(waiting.status, 409)
        err = waiting.json()["error"]
        assert_equal((err["reason"], err["confirmations"], err["needed"]), ("reservation_not_deep", 1, 2))
        # Not found at all: the same wait (the site may poll before its broadcast reached this node).
        missing = self.post(api, "psbt/launch", dict(body, reservation="11" * 32 + ":0"), launch_query)
        assert_equal((missing.status, missing.json()["error"]["confirmations"]), (409, 0))
        self.bmm()

        self.log.info("psbt/launch: a wrong nonce, another address, refused")
        assert_equal(self.post(api, "psbt/launch", dict(body, nonce=os.urandom(32).hex()), launch_query).status, 400)
        assert_equal(self.post(api, "psbt/launch", dict(body, address=empty), launch_query).status, 400)
        # The logo must be one uploaded here.
        assert_equal(self.post(api, "psbt/launch", body, dict(launch_query, logo_sha256="ab" * 32)).status, 400)

        self.log.info("psbt/launch: the launch, its first buy's burn")
        launch = self.ok(self.post(api, "psbt/launch", body, launch_query))
        assert_equal(launch["burn_units"], "100000000")
        assert_equal(launch["burn"], 1)
        assert launch["summary"].startswith("Launch PSBTX (Psbt Coin)"), launch["summary"]
        decoded = self.check_psbt(n0, launch["psbt"], address)
        # The reservation first, then the coins.
        assert_equal(decoded["tx"]["vin"][0]["txid"] + ":" + str(decoded["tx"]["vin"][0]["vout"]), reserve["reservation"])
        # Its first buy's result output, then the marker holding the first buy, then the change.
        assert_equal(decoded["tx"]["vout"][0]["value"], 0)
        assert_equal(decoded["tx"]["vout"][1]["value"], 1)
        assert_equal(decoded["tx"]["vout"][1]["scriptPubKey"]["type"], "nulldata")
        signed = self.sign(ext, launch["psbt"])
        low = self.broadcast(api, {"psbt": signed, "max_burn_units": "99999999"})
        assert_equal(low.status, 400)
        assert "max_burn_units" in low.json()["error"]["message"]
        sent = self.ok(self.broadcast(api, {"psbt": signed, "max_burn_units": launch["burn_units"]}))
        assert_equal(sent["txid"], launch["txid"])
        self.bmm()
        self.bmm()

        got = n0.getlaunch("PSBTX")
        assert_equal(got["token"], launch["asset"])
        assert_equal(got["creator"], address)
        assert_equal(got["ticker"], "PSBTX")
        assert_equal(api.rest("launches/PSBTX")["name"], "Psbt Coin")
        assert_equal(api.rest("launches/PSBTX")["links"], [{"label": "Website", "url": "https://example.org/psbtx"}])
        assert api.rest("launches/PSBTX")["logo"] is not None
        # The first buy's tokens: at the address (the external wallet's).
        held = {a["label"]: a["balance"] for a in ext.listmyassets()["assets"]}
        assert_greater_than(held.get("PSBTX", 0), 0)
        # The ticker is taken now.
        assert_equal(self.post(api, "psbt/reserve", {"address": address, "ticker": "PSBTX", "nonce": nonce}).status, 409)

        self.log.info("psbt/register: reserve, then register, as hex this time (walletprocesspsbt finalizing)")
        nonce2 = os.urandom(32).hex()
        reserve2 = self.ok(self.post(api, "psbt/reserve", {"address": address, "ticker": "REGX", "nonce": nonce2}))
        self.check_psbt(n0, reserve2["psbt"], address)
        done = ext.walletprocesspsbt(self.sign(ext, reserve2["psbt"]))
        assert done["complete"]
        self.ok(self.broadcast(api, {"hex": done["hex"]}))
        self.bmm_to(n0.getblockcount() + 2)
        register = self.ok(self.post(api, "psbt/register", {"address": address, "ticker": "REGX", "nonce": nonce2, "reservation": reserve2["reservation"],
                                                             "name": "Registered X", "supply": "5000", "decimals": 2}))
        decoded = self.check_psbt(n0, register["psbt"], address)
        assert_equal(register["burn_units"], "0")
        self.ok(self.broadcast(api, {"psbt": self.sign(ext, register["psbt"])}))
        self.bmm()
        asset = n0.getasset("REGX")
        assert_equal(asset["supply"], 5000)
        assert_equal(asset["decimals"], 2)
        held = {a["label"]: a for a in ext.listmyassets()["assets"]}
        assert_equal(held["REGX"]["balance"], 5000)

        self.log.info("The coins named by the client: checked against the UTXO set and the address")
        nonce3 = os.urandom(32).hex()
        utxo = [u for u in ext.listunspent() if u["address"] == address and u["amount"] > 0][0]
        named = self.ok(self.post(api, "psbt/reserve", {"address": address, "ticker": "NAMEDX", "nonce": nonce3, "coins": [f"{utxo['txid']}:{utxo['vout']}"]}))
        assert_equal([f"{v['txid']}:{v['vout']}" for v in n0.decodepsbt(named["psbt"])["tx"]["vin"]], [f"{utxo['txid']}:{utxo['vout']}"])
        other = [u for u in ext.listunspent() if u["address"] != address][0]
        assert_equal(self.post(api, "psbt/reserve", {"address": address, "ticker": "NAMEDX", "nonce": nonce3, "coins": [f"{other['txid']}:{other['vout']}"]}).status, 400)

        self.log.info("The wallet's own launch alongside (the shared code)")
        funder.reserveasset("OWNX")
        self.bmm_to(n0.getblockcount() + 2)
        own = funder.launchtoken("OWNX", {"display_name": "Own", "first_buy": 1, "keep_control": False})
        self.bmm()
        assert_equal(n0.getlaunch("OWNX")["token"], own["token"])
        del status


if __name__ == '__main__':
    PublicApiPsbtTest(__file__).main()
