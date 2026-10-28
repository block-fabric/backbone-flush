#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The public DEX API (-publicapi, doc/public-api.md) against the node's own RPC methods, on a regtest Flush chain.

  - launches (one with a first buy, a creator's text with unsafe links), buys and sales from other wallets, a
    graduation, trading in the graduated pool: /launches (filters, sorts, pages), /launches/<id> against getlaunch,
    its trades (one per fill, amounts as the wallets got them), its candles (volume and close), its holders (against
    the wallets' own holdings), /launches/stats, /launches/log, /stats, /status;
  - quotes against quotelaunch, getswapquote, quoteroute, quotepoolliquidity, previewposition (the same code);
  - builds: the flush: URI exactly as the app's links take it, the payload and its hash; a wallet then signs for the
    build's floor; a broadcast of a transaction signed elsewhere (and refusals); a logo uploaded, served by its hash,
    and named by a build's logo_url (8.1.0);
  - pools of every kind: a concentrated pool and its positions and ticks, a book and its depth, verified issuers per
    curator list, holdings of an address;
  - a reorganisation across the graduation: trades, candles, holders and launches back, then forward again;
  - orders' outcomes (8.2.0): a sale and a buy past the curve's end, a route, a missed swap and its refund, limit orders
    filled by crossings, collected, settled, cancelled and expired, by txid (a settle's too) and by address, a page at a
    time, and across a reorganisation;
  - CORS (an allowed origin, another refused, a preflight, no credentials), rate limits (429 and Retry-After), page
    and size bounds, unknown parameters, the GET/POST rules, no wallet method reachable;
  - the Connect protocol (JSON, GET form, binary through protoc) and gRPC (flush-grpc-client, gRPC's own library):
    the same answers as REST; health and reflection;
  - a node that turns -dexindex on derives the market index again from the blocks: the same trades and candles.
"""

import hashlib
import json
import os
import time
from decimal import Decimal

from feature_layers import LayersTest
from test_framework.publicapi import (PublicApi, URI_PARAMS, action_hash, decode, encode, grpc_call, parse_uri, protoc_available)
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error
from test_framework.test_framework import SkipTest

DEX_HEIGHT = 10
CURVES_HEIGHT = 15
ORIGIN = "https://flush.markets"


def api_port(n):
    # Clear of the framework's own ports (p2p, rpc, tor); within the range the tests may use.
    from test_framework.util import p2p_port
    return p2p_port(n) + 470


class PublicApiTest(LayersTest):
    def set_test_params(self):
        super().set_test_params()
        common = self.side_args + [f"-testactivationheight=dex@{DEX_HEIGHT}", f"-testactivationheight=dexcurves@{CURVES_HEIGHT}", "-testactivationheight=ticker@0", "-txindex"]
        self.extra_args = [
            common + [f"-publicapi=127.0.0.1:{api_port(0)}", f"-publicapiorigin={ORIGIN}", "-publicapirate=1000", "-publicapiburst=5000"],
            common + [f"-publicapi={api_port(1)}", "-publicapirate=1", "-publicapiburst=3", "-publicapiproxy=127.0.0.1", "-publicapiproxyheader=X-Real-IP"],
            common,
        ]
        self.common_args = common

    def add_options(self, parser):
        super().add_options(parser)
        parser.add_argument("--site", help="a checkout of flush-web: its end-to-end test (scripts/e2e-live.mjs) against this chain's API")
        parser.add_argument("--site-shots", help="where its pictures go")
        parser.add_argument("--playwright", help="the playwright package (node) the site's test drives Chromium with")
        parser.add_argument("--hold", help="(development) write the API's port to this file and wait until it is removed, the chain running")

    def skip_test_if_missing_module(self):
        super().skip_test_if_missing_module()
        if not self.config["components"].getboolean("ENABLE_PUBLICAPI", fallback=False):
            raise SkipTest("built without the public DEX API (WITH_PUBLICAPI)")

    def run_test(self):
        super().run_test()

    def bmm_to(self, height):
        while self.nodes[0].getblockcount() < height:
            self.bmm()

    def mine_blocks(self, count):
        for _ in range(count):
            self.bmm()

    def holdings_by_address(self, node, label):
        """What a wallet holds of an asset, by address (from its unspent outputs and the tokens they carry)."""
        total = Decimal(0)
        for a in node.listmyassets()["assets"]:
            if a["label"] == label:
                total = a["balance"]
        return total

    def run_sidechain_test(self):
        main = self.main
        n0, n1, n2 = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = n0.getnewaddress()
        from feature_sidechain import ACTIVATION_PERIOD
        from test_framework.fastblocks import SLOT
        api = PublicApi(api_port(0))
        strict = PublicApi(api_port(1))

        self.log.info("Setup: the slot, deposits, two assets (BMM blocks)")
        self.mine_main(300)
        main.createsidechainproposal(SLOT, "Flush", "Tokens on a fast chain", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 1200)
        main.createsidechaindeposit(SLOT, n1.getdepositaddress()["depositaddress"], 100)
        main.createsidechaindeposit(SLOT, n2.getdepositaddress()["depositaddress"], 100)
        self.mine_main()
        self.bmm()
        for n in self.nodes:
            n.sendmany("", {n.getnewaddress(): 2 for _ in range(10)})
        self.bmm()
        for name in ("USDX", "USDY"):
            n0.reserveasset(name)
        self.bmm_to(n0.getblockcount() + 3)
        for name in ("USDX", "USDY"):
            n0.registerasset(name, 10000000, 2)
        self.bmm()
        self.bmm_to(CURVES_HEIGHT)

        self.log.info("Status, stats, fees: the chain's own")
        status = api.rest("status")
        assert_equal(status["network"], "regtest")
        assert_equal(status["flush"]["height"], n0.getblockcount())
        assert_equal(status["flush"]["hash"], n0.getbestblockhash())
        assert_equal(status["flush"]["block_target"], 1)  # regtest: a slot a second (15 on the eCash networks)
        assert status["market_index"]
        assert_equal(status["slot"], SLOT)
        stats = api.rest("stats")
        rules = n0.getlaunchstats()["rules"]
        assert_equal(Decimal(str(stats["graduation_ecx"])), rules["graduation"])
        assert_equal(stats["launches"], 0)
        assert_equal(Decimal(str(stats["fees"]["trade_bps"])), rules["fee"] * 100)
        assert_equal(stats["curve"]["virtual_ecx"], 250)
        assert_equal(stats["curve"]["supply"], 1e9)
        # The launch fee's parts as the rules say (getlaunchstats): the creator's, the treasury's if any, the chain's security, the pool's rest.
        split = {s["to"]: Decimal(str(s["bps"])) for s in stats["fees"]["split"]}
        assert_equal(split["creator"], rules["creator_share"] * 100)
        assert_equal(split.get("security", 0), rules["security_share"] * 100)
        assert_equal(sum(split.values()), 10000)
        fees = api.rest("fees")
        assert_equal(fees["pool_fee_tiers_ppm"], [100, 500, 3000, 10000])
        # No treasury: no such endpoint, no such part.
        assert_equal(api.rest_status("treasury"), 404) if hasattr(api, "rest_status") else None
        assert "treasury" not in {s["to"] for s in fees["swap_split"]} and "protocol_fee_share_ppm" not in fees
        swap_split = {s["to"]: Decimal(str(s["bps"])) for s in fees["swap_split"]}
        assert_equal(swap_split["security"], n0.getdexinfo()["swap_fee_parts"]["security"] * 100)

        self.log.info("Launches: MOON with a first buy and a creator's text, SUN without")
        for t in ("MOON", "SUN"):
            n0.reserveasset(t)
        self.bmm_to(n0.getblockcount() + 3)
        info = "To the moon and back\nhttps://example.org/moon\njavascript:alert(1)\nhttp://plain.example/x\nhttps://user:pw@evil.example/"
        moon = n0.launchtoken("MOON", {"display_name": "Moon Coin", "info": info, "first_buy": 1})
        self.bmm()
        sun = n0.launchtoken("SUN", {"display_name": "Sun"})
        self.bmm()
        detail = api.rest("launches/MOON")
        rpc = n0.getlaunch("MOON")
        assert_equal(detail["id"], rpc["token"])
        assert_equal(detail["ticker"], "MOON")
        assert_equal(detail["name"], "Moon Coin")
        assert_equal(detail["state"], "curve")
        assert_equal(Decimal(str(detail["raised_ecx"])), rpc["raised"])
        assert_equal(detail["create_txid"], moon["txid"])
        assert_equal(detail["creator"], rpc["creator"])
        assert_equal(detail["pool"], None)
        assert_equal(detail["logo"], None)
        # The creator's text: plain data; only the safe link is a link (https, no user).
        assert_equal(detail["links"], [{"label": "Website", "url": "https://example.org/moon"}])
        assert "To the moon and back" in detail["description"]
        # The first buy: 1 ECX paid in, the fee included.
        assert_equal(detail["first_buy_ecx"], 1)
        # The split once, as getlaunch has it (the detail listed every share twice before 8.0.1).
        assert_equal([(s["kind"], s["bps"]) for s in detail["fee_split"]], [(s["kind"], s["bps"]) for s in rpc["fee_split"]])
        assert_equal(api.rest("launches/" + rpc["token"])["ticker"], "MOON")
        assert_equal(api.rest("launches/moon")["ticker"], "MOON")
        assert_equal(api.get("launches/NOPE").status, 404)

        self.log.info("Trades from other wallets: buys and a sale, each what quotelaunch said")
        q = api.rest("launches/MOON/quote", {"side": "buy", "amount": "5"})
        rq = n1.quotelaunch("MOON", 5)
        assert_equal(Decimal(str(q["tokens"])), rq["amount_out"])
        assert_equal(int(q["amount_out_units"]), int(rq["amount_out"] * 10**6))
        assert_equal(int(q["min_out_units"]), int(q["amount_out_units"]) * 9900 // 10000)
        assert_equal(Decimal(str(q["fee_ecx"])), rq["fee"])
        assert_equal(q["slippage_bps"], 100)
        built = api.rest("build/buy", {"launch": "MOON", "amount": "5", "slippage_bps": "100"})
        verb, params, links = parse_uri(built["uri"])
        assert_equal(verb, "buy")
        assert_equal(set(params) - {"network", "exp"}, URI_PARAMS["buy"])
        assert_equal(params["network"], "regtest")
        assert_equal(params["launch"], "MOON")
        assert_equal(params["asset"], rpc["token"])
        assert_equal(params["ecx"], "5")
        assert_equal(Decimal(params["min_tokens"]), Decimal(built["launch_quote"]["min_out_units"]) / 10**6)
        assert 0 < int(params["exp"]) - time.time() <= 600 + 5
        assert_equal(built["payload_hash"], action_hash(built["payload"]))
        assert_equal(built["check_code"], built["payload_hash"][:8])
        assert built["app_ready"]
        # The app's role: it signs for the floor the build gave (min_out), nothing less.
        n1.buyfromlaunch("0x" + rpc["token"], 5, 1, Decimal(params["min_tokens"]))
        self.bmm()
        got1 = self.holding(n1, "MOON")
        assert_equal(got1, rq["amount_out"])
        n2.buyfromlaunch("MOON", 3)
        n2.buyfromlaunch("SUN", Decimal("0.5"))
        self.bmm()
        got2 = self.holding(n2, "MOON")
        # A quarter that fits the token's 6 decimals (what the launch fee leaves need not divide by 4).
        quarter = (got1 / 4).quantize(Decimal("0.000001"))
        sold = n1.selltolaunch("MOON", quarter)
        self.bmm()
        trades = api.rest("launches/MOON/trades")
        # The first buy, two buys and a sale, newest first.
        assert_equal([t["side"] for t in trades["items"]], ["sell", "buy", "buy", "buy"])
        assert_equal(trades["total"], 4)
        assert_equal(trades["next_cursor"], None)
        sale = trades["items"][0]
        assert_equal(Decimal(str(sale["tokens"])), quarter)
        assert_equal(Decimal(str(sale["ecx"])), sold["quote"])
        assert_equal(sale["venue"], "curve")
        assert_equal(sale["txid"], sold["txid"])
        assert n1.getaddressinfo(sale["address"])["ismine"]
        assert_equal(Decimal(str(trades["items"][2]["tokens"])), got1)
        assert n1.getaddressinfo(trades["items"][2]["address"])["ismine"]
        assert_equal(Decimal(str(trades["items"][1]["tokens"])), got2)
        # Pages of one: the same trades.
        paged, cursor = [], None
        while True:
            page = api.rest("launches/MOON/trades", {"limit": 1, **({"cursor": cursor} if cursor else {})})
            paged += page["items"]
            cursor = page["next_cursor"]
            if not cursor:
                break
        assert_equal(paged, trades["items"])
        # Before a height: the older ones.
        assert_equal(api.rest("launches/MOON/trades", {"before": trades["items"][1]["height"]})["items"], trades["items"][2:])

        self.log.info("Candles: the volume of the trades, the close at the curve's price")
        candles = api.rest("launches/MOON/candles", {"interval": "1d"})["items"]
        assert_equal(len(candles), 1)
        volume = sum(Decimal(str(t["ecx"])) for t in trades["items"])
        assert abs(Decimal(str(candles[0]["v"])) - volume) < Decimal("0.0001"), (candles, volume)
        price = n0.getlaunch("MOON")["price"]
        assert abs(Decimal(str(candles[0]["c"])) - price) / price < Decimal("0.000001")
        assert candles[0]["h"] >= candles[0]["c"] >= candles[0]["l"] > 0
        for interval in ("5m", "1h", "4h"):
            items = api.rest("launches/MOON/candles", {"interval": interval})["items"]
            assert abs(sum(Decimal(str(c["v"])) for c in items) - volume) < Decimal("0.0001")
            assert_equal(items[-1]["c"], candles[0]["c"])
        assert_equal(api.get("launches/MOON/candles", {"interval": "2m"}).status, 400)

        self.log.info("Holders: by address, against the wallets' own")
        holders = api.rest("launches/MOON/holders", {"limit": 100})
        tagged = {h["tag"]: h for h in holders["items"] if h["tag"]}
        assert_equal(Decimal(str(tagged["curve"]["amount"])), n0.getlaunch("MOON")["curve_tokens_left"])
        for node in (n0, n1, n2):
            mine = sum(Decimal(str(h["amount"])) for h in holders["items"] if not h["address"].startswith(("curve:", "pool:")) and node.getaddressinfo(h["address"])["ismine"])
            assert_equal(mine, self.holding(node, "MOON"))
        amounts = [h["amount"] for h in holders["items"] if not h["tag"] == "curve"]
        assert_equal(amounts, sorted(amounts, reverse=True))
        assert_equal(holders["total"], len([h for h in holders["items"] if h["tag"] != "curve"]))
        # The creator's tag: on the creator's own address only (its first buy went to another address of its wallet).
        assert all(h["address"] == detail["creator"] for h in holders["items"] if h["tag"] == "creator")
        page1 = api.rest("launches/MOON/holders", {"limit": 1})
        page2 = api.rest("launches/MOON/holders", {"limit": 1, "cursor": page1["next_cursor"]})
        assert_equal(page2["items"][0], [h for h in holders["items"] if h["tag"] != "curve"][1])

        self.log.info("The list of launches: filters, sorts, pages, against listlaunches")
        listed = api.rest("launches")
        assert_equal([l["ticker"] for l in listed["items"]], ["SUN", "MOON"])
        assert_equal(listed["total"], 2)
        rpc_list = {l["ticker"]: l for l in n0.listlaunches()["launches"]}
        for l in listed["items"]:
            assert_equal(Decimal(str(l["raised_ecx"])), rpc_list[l["ticker"]]["raised"])
            assert_equal(Decimal(str(round(l["progress"], 8))), Decimal(str(round(float(rpc_list[l["ticker"]]["progress"]), 8))))
        moon_row = next(l for l in listed["items"] if l["ticker"] == "MOON")
        assert_equal(moon_row["trades_24h"], 4)
        assert abs(Decimal(str(moon_row["volume_24h"])) - volume) < Decimal("0.0001")
        assert_equal(moon_row["holders"], holders["total"])
        assert_equal([l["ticker"] for l in api.rest("launches", {"sort": "volume"})["items"]], ["MOON", "SUN"])
        assert_equal([l["ticker"] for l in api.rest("launches", {"q": "moo"})["items"]], ["MOON"])
        assert_equal(api.rest("launches", {"filter": "graduated"})["total"], 0)
        first = api.rest("launches", {"limit": 1})
        assert_equal(first["next_cursor"], "1")
        assert_equal(api.rest("launches", {"limit": 1, "cursor": first["next_cursor"]})["items"][0]["ticker"], "MOON")
        assert_equal(api.get("launches", {"limit": 101}).status, 400)
        assert_equal(api.get("launches", {"filter": "bogus"}).status, 400)
        assert_equal(api.get("launches", {"nope": 1}).status, 400)
        lstats = api.rest("launches/stats")
        assert_equal(lstats["launches"], 2)
        assert_equal(lstats["on_curve"], 2)
        log = api.rest("launches/log", {"from_height": rpc["height"], "to_height": n0.getblockcount()})
        assert_equal(len(log["items"]), len(n0.getlaunchlog(rpc["height"], n0.getblockcount())))

        # The chain's security parts of the launches' fees, paid with the blocks.
        assert_greater_than(n0.getdexinfo()["security_paid"], 0)

        self.log.info("Graduation: MOON's curve to its end; its pool, locked; trading there")
        whale = n0.buyfromlaunch("MOON", 600)
        grad_block = self.bmm()
        done = api.rest("launches/MOON")
        assert_equal(done["state"], "graduated")
        assert_equal(done["graduated_height"], n0.getblockcount())
        assert_equal(done["pool"]["asset_a"], "MOON")
        assert_equal(done["pool"]["asset_b"], "ECX")
        pool = api.rest(f"pools/{done['pool']['pool']}")
        assert_equal(pool["launch"], "MOON")
        assert pool["locked"]
        assert_equal(pool["curve"], "constant_product")
        rpc_pool = n0.getpool("MOON", "CHN")
        assert_equal(Decimal(str(pool["reserve_a"])), rpc_pool["reserve_a"])
        assert_equal(api.rest("pools/MOON/ECX")["pool"], pool["pool"])
        pools = api.rest("pools")
        assert pool["pool"] in [p["pool"] for p in pools["items"]]
        q = api.rest("quote/swap", {"from": "ECX", "to": "MOON", "amount": "2"})
        rq = n2.getswapquote("CHN", 2, "MOON", {"max_hops": 0})
        assert_equal(q["best"]["pools"], [rq["best_pool"]])
        assert_equal(Decimal(q["amount_out_units"]) / 10**6, rq["pools"][0]["amount_out"])
        built = api.rest("build/swap", {"from": "ECX", "to": "MOON", "amount": "2", "slippage_bps": "50"})
        verb, params, _ = parse_uri(built["uri"])
        assert_equal(verb, "swap")
        assert_equal(set(params) - {"network", "exp"}, URI_PARAMS["swap"])
        assert_equal(params["pool"], pool["pool"])
        assert_equal((params["from"], params["to"], params["amount"]), ("ECX", "MOON", "2"))
        floor = Decimal(params["min_out"])
        n2.swapasset("ECX", "2", "MOON", 0.5, 100, str(floor))
        self.bmm()
        ptrades = api.rest(f"pools/{pool['pool']}/trades")
        assert_equal(len(ptrades["items"]), 1)
        assert_equal(ptrades["items"][0]["venue"], "pool")
        assert Decimal(str(ptrades["items"][0]["tokens"])) >= floor
        # The launch's trades: its curve's, then its pool's.
        assert_equal(api.rest("launches/MOON/trades")["total"], 6)
        assert_equal(api.rest("launches", {"filter": "graduated"})["items"][0]["ticker"], "MOON")

        self.log.info("A reorganisation across the graduation: everything back, then forward again")
        snapshot = {
            "trades": api.rest("launches/MOON/trades"),
            "candles": api.rest("launches/MOON/candles", {"interval": "5m"}),
            "holders": api.rest("launches/MOON/holders", {"limit": 100}),
            "launch": api.rest("launches/MOON"),
        }
        tip = n0.getbestblockhash()
        for n in self.nodes:
            n.invalidateblock(grad_block)
        back = api.rest("launches/MOON")
        assert_equal(back["state"], "curve")
        assert_equal(api.rest("launches/MOON/trades")["total"], 4)
        assert_equal(api.rest("launches/MOON/trades")["items"], trades["items"])
        assert_equal(api.get(f"pools/{pool['pool']}").status, 404)
        reorg_holders = api.rest("launches/MOON/holders", {"limit": 100})
        assert_equal(Decimal(str({h["tag"]: h for h in reorg_holders["items"] if h["tag"]}["curve"]["amount"])), n0.getlaunch("MOON")["curve_tokens_left"])
        for n in self.nodes:
            n.reconsiderblock(grad_block)
        self.wait_until(lambda: all(n.getbestblockhash() == tip for n in self.nodes))
        assert_equal(api.rest("launches/MOON/trades"), snapshot["trades"])
        assert_equal(api.rest("launches/MOON/candles", {"interval": "5m"}), snapshot["candles"])
        assert_equal(api.rest("launches/MOON/holders", {"limit": 100}), snapshot["holders"])
        again = api.rest("launches/MOON")
        for key in ("state", "raised_ecx", "graduated_height", "pool", "price_ecx"):
            assert_equal(again[key], snapshot["launch"][key])

        self.log.info("Pools of every kind: liquidity quotes, a concentrated pool, a book, positions, ticks")
        n0.addliquidity("USDX", 100000, "ECX", 50)
        self.bmm()
        x, y = n0.getasset("USDX")["asset"], n0.getasset("USDY")["asset"]
        cp = api.rest("pools/USDX/ECX")
        assert_equal((cp["asset_a"], cp["asset_b"]), ("USDX", "ECX"))
        dq = api.rest("quote/deposit", {"pool": cp["pool"], "amount_a": "100", "amount_b": "0.05"})
        amounts = {("0x" + x): 100, "CHN": Decimal("0.05")}
        rdq = n0.quotepoolliquidity(cp["pool"], amounts)
        assert_equal(int(dq["shares_units"]), rdq["shares"])
        wq = api.rest("quote/withdraw", {"pool": cp["pool"], "shares": "1000"})
        rwq = n0.quotepoolliquidity(cp["pool"], None, 1000)
        assert_equal(int(wq["shares_units"]), rwq["shares"])
        decimals = {"ECX": 8, "USDX": 2}
        built = api.rest("build/deposit", {"pool": cp["pool"], "amount_a": str(Decimal(dq["amount_a_units"]).scaleb(-decimals[cp["asset_a"]])),
                                           "amount_b": str(Decimal(dq["amount_b_units"]).scaleb(-decimals[cp["asset_b"]]))})
        assert_equal(built["verb"], "deposit")
        assert built["app_ready"]
        assert_equal(built["payload_hash"], action_hash(built["payload"]))
        clpool = n0.createpool("0x" + x, 1, "0x" + y, 1, {"curve": "concentrated", "fee": 0.05})["pool"]
        self.bmm()
        amounts = {"0x" + x: 50000, "0x" + y: 50000}
        pq = api.rest("quote/position", {"pool": clpool, "price_lower": "0.98", "price_upper": "1.02", "amount_a": "50000", "amount_b": "50000"})
        rpq = n0.previewposition(clpool, Decimal("0.98"), Decimal("1.02"), amounts)
        assert_equal(pq["liquidity"], rpq["liquidity"])
        assert_equal((pq["tick_lower"], pq["tick_upper"]), (rpq["tick_lower"], rpq["tick_upper"]))
        # The price the caller showed binds (a pool opened or moved first is refused); the full range is 0 and 0.
        assert_raises_rpc_error(-8, "not the", n0.openposition, clpool, 0, 0, amounts, 1, 2)
        full_range = n0.openposition(clpool, 0, 0, {"0x" + x: 100, "0x" + y: 100}, 1, 1)
        self.bmm()
        assert_equal(n0.getposition(full_range["position"])["tick_lower"], -887270)
        near = n0.openposition(clpool, Decimal("0.98"), Decimal("1.02"), amounts)
        self.bmm()
        position = api.rest(f"positions/{near['position']}")
        rposition = n0.getposition(near["position"])
        assert_equal(position["liquidity"], rposition["liquidity"])
        assert_equal(position["in_range"], rposition["in_range"])
        ticks = api.rest(f"pools/{clpool}/ticks")
        rticks = n0.getpoolticks(clpool)
        assert_equal([t["tick"] for t in ticks["items"]], [t["tick"] for t in rticks["ticks"]])
        assert_equal(ticks["liquidity"], rticks["liquidity"])
        kinds = api.rest("pools", {"kind": "concentrated"})
        assert_equal([p["pool"] for p in kinds["items"]], [clpool])
        assert_equal(kinds["items"][0]["concentrated"]["tick_spacing"], 10)
        assert_equal(api.rest("pools", {"fee_ppm": 500})["items"][0]["pool"], clpool)
        # Every kind by default (betanet, 2026-10-23: `searchpools {}` answered none): the pools with ECX by depth, then
        # the others (this concentrated pool of two assets), newest first; '{...}' first is the filters, not a text.
        listed = [p["pool"] for p in n0.searchpools()["pools"]]
        assert clpool in listed and cp["pool"] in listed
        assert_greater_than(listed.index(clpool), listed.index(cp["pool"]))
        assert_equal([p["pool"] for p in n0.searchpools("{}")["pools"]], listed)
        assert_equal([p["pool"] for p in n0.searchpools('{"exclude_abandoned": true}')["pools"]], listed)
        assert_equal([p["pool"] for p in n0.searchpools("", {}, "depth", "", 1)["pools"]], listed[:1])
        paged, cursor = [], ""
        while True:
            page = n0.searchpools("", {}, "depth", cursor, 1)
            paged += [p["pool"] for p in page["pools"]]
            cursor = page["next_cursor"]
            if not cursor:
                break
        assert_equal(paged, listed)
        assert_equal([p["pool"] for p in api.rest("pools")["items"]], [p for p in listed if p in {q["pool"] for q in api.rest("pools", {"sort": "newest"})["items"]}])
        assert clpool in [p["pool"] for p in api.rest("pools")["items"]]
        n0.placeorder("USDX", 1000, "USDY", 2)
        n0.placeorder("USDY", 3000, "USDX", Decimal("0.4"))
        self.bmm()
        book = api.rest("books/USDX/USDY", {"depth": 5})
        rbook = n0.getbook("USDX", "USDY", 5)
        assert_equal(book["book"], rbook["book"])
        assert_equal([l["price"] for l in book["asks"]], [float(l["price"]) for l in rbook["asks"]])
        assert_equal([l["amount"] for l in book["bids"]], [float(l["amount"]) for l in rbook["bids"]])
        assert_equal(api.get("books/USDX/USDY", {"depth": 101}).status, 400)
        # Exact prices: the orders' limits as text, the amounts in units.
        assert_equal([l["price_exact"] for l in book["asks"]], [l["price_exact"] for l in rbook["asks"]])
        assert_equal([l["amount_units"] for l in book["bids"]], [l["amount_units"] for l in rbook["bids"]])
        assert_equal(book.get("best_ask_exact"), rbook.get("best_ask_exact"))
        assert_equal(book.get("best_bid_exact"), rbook.get("best_bid_exact"))
        assert_equal(n0.getpoolcandles(rbook["book"], "1h"), n0.getpoolcandles(rbook["book"]))

        self.log.info("The asset registry: every asset with its markets; builds of the new verbs")
        # A book of USDX with ECX, both sides (not crossing): the site's book page draws it.
        n0.placeorder("USDX", 100, "ECX", Decimal("0.001"))
        n0.placeorder("ECX", Decimal("0.1"), "USDX", 2000)
        n0.reserveasset("PLAIN")
        self.bmm_to(n0.getblockcount() + 3)
        n0.registerasset("PLAIN", 1000000, 6)
        self.bmm()
        plain = api.rest("assets/PLAIN")
        assert_equal((plain["market"]["status"], plain["market"]["kinds"]), ("none", []))
        # 7.0.1: a logo's URL has its version (?v=, its SHA-256's first 12 hex digits); the endpoint takes and ignores it.
        from feature_bitassets_logo import b64, make_png
        png = make_png([200, 40, 40, 40, 40, 200])
        n0.setassetlogo("PLAIN", b64(png))
        self.bmm()
        logo_url = api.rest("assets/PLAIN")["logo"]
        assert_equal(logo_url, f"/api/v1/assets/{plain['id']}/logo?v={hashlib.sha256(png).hexdigest()[:12]}")
        fetched = api.request("GET", logo_url)
        assert_equal(fetched.status, 200)
        assert_equal(fetched.body, png)
        assert_equal(api.get(f"assets/{plain['id']}/logo", {"v": "anything"}).status, 200)
        assert plain["can_curve"]["allowed"], plain["can_curve"]
        built = api.rest("build/curve", {"asset": "PLAIN", "amount": "800000", "fee_bps": "100"})
        assert_equal(parse_uri(built["uri"])[1]["ticker"], "PLAIN")
        status = api.rest("status")
        assert status["launch_params"]["active"]
        assert_equal((status["launch_params"]["fee_min_bps"], status["launch_params"]["fee_max_bps"]), (30, 500))
        # Allocations of up to 10 % (regtest: from the first block).
        assert_equal((status["launch_params"]["allocation_max_bps"], status["launch_params"]["allocation10_height"]), (1000, 0))
        listed = api.rest("assets", {"limit": 100})
        by_ticker = {a["ticker"]: a for a in listed["items"]}
        assert {"MOON", "SUN", "USDX", "USDY"} <= set(by_ticker)
        assert_equal(by_ticker["SUN"]["market"]["status"], "curve")
        assert_greater_than(by_ticker["SUN"]["market"]["progress"], 0)
        assert_equal(by_ticker["MOON"]["market"]["status"], "pool")
        assert "book" in by_ticker["USDX"]["market"]["kinds"]
        assert_equal(by_ticker["USDX"]["supply_units"], str(int(n0.getasset("USDX")["supply"] * 100)))
        curves = api.rest("assets", {"market": "curve", "limit": 100})
        assert all(a["market"]["status"] == "curve" for a in curves["items"]) and curves["items"]
        detail = api.rest("assets/USDX")
        assert_equal(detail["id"], x)
        assert detail["books"] and detail["pools"]
        assert rbook["book"] in [b["id"] for b in detail["books"]]
        assert not detail["launch"]
        assert detail["can_curve"]["allowed"] is False  # it has a pool with ECX
        assert_equal(api.get("assets/NOPE99").status, 404)
        assert_equal(api.get("assets", {"market": "elsewhere"}).status, 400)
        for verb, query in [
            ("register", {"ticker": "REG1", "name": "Reg One", "supply": "21000000", "decimals": "8"}),
            ("launch", {"ticker": "WIDE1", "name": "Wide", "supply": "21000000", "decimals": "8", "fee_bps": "200", "allocation_bps": "100", "first_buy_ecx": "1"}),
            ("pool", {"asset_a": "USDY", "asset_b": "ECX", "kind": "stable", "fee_ppm": "500", "amount_a": "10", "amount_b": "1", "amp": "100"}),
            ("auction", {"base": "USDY", "amount": "10", "quote": "ECX", "start_price": "5", "end_price": "1", "duration": "100"}),
            ("order", {"sell": "USDY", "buy": "ECX", "amount": "5", "price": "0.25", "expiry_blocks": "1000"}),
        ]:
            built = api.rest(f"build/{verb}", query)
            got, params, _ = parse_uri(built["uri"])
            assert_equal(got, verb)
            assert set(params) - {"network", "exp"} <= URI_PARAMS[verb], (verb, params)
            assert built["app_ready"]
            assert_equal(built["payload_hash"], action_hash(built["payload"]))
        assert_equal(api.get("build/launch", {"ticker": "WIDE2", "name": "Wide", "fee_bps": "600"}).status, 400)
        assert_equal(api.get("build/curve", {"asset": "USDX", "amount": "8", "fee_bps": "100"}).status, 400)

        self.log.info("Verified issuers: per curator list given by the client")
        curator = n0.registeroracle("curator")["oracle"]
        self.bmm()
        n0.verifyasset(curator, "USDX")
        self.bmm()
        v = api.rest("verifications", {"assets": "USDX,USDY", "curators": curator})
        assert_equal([i["verified"] for i in v["items"]], [True, False])
        assert_equal(v["items"][0]["curators"][0]["name"], "curator")
        assert_equal(n0.getverifications("USDX", [curator])["verified"], True)
        assert_equal(api.get("verifications", {"assets": "USDX"}).status, 400)

        self.log.info("Holdings of an address, tickers")
        address = next(h["address"] for h in api.rest("launches/MOON/holders", {"limit": 100})["items"] if not h["address"].startswith(("curve:", "pool:")) and n2.getaddressinfo(h["address"])["ismine"])
        held = api.rest(f"addresses/{address}/holdings")
        moon_held = next(i for i in held["items"] if i["ticker"] == "MOON")
        assert_greater_than(moon_held["amount"], 0)
        assert_greater_than(moon_held["value_ecx"], 0)
        assert_equal(api.get("addresses/notanaddress/holdings").status, 400)

        self.log.info("The site's DEX: what an address holds and made (portfolio), what a link's call brings in going to it, builds of each verb")
        mine = n0.getnewaddress("flush.markets")
        # What the actions bring in goes to the address while it is set (as while the app runs a link of the site).
        n0.setlinkdestination(mine)
        assert_raises_rpc_error(-5, "Not an address of this wallet", n0.setlinkdestination, n1.getnewaddress())
        pos = n0.openposition(clpool, Decimal("0.99"), Decimal("1.01"), {"0x" + x: 1000, "0x" + y: 1000})
        order = n0.placeorder("USDX", 10, "USDY", 3)
        auction = n0.createauction("USDY", 50, "ECX", 5, 1, 100)
        n0.setlinkdestination()
        self.bmm()
        folio = api.rest(f"addresses/{mine}/portfolio")
        assert_equal(folio["address"], mine)
        assert_equal([p["id"] for p in folio["positions"]], [pos["position"]])
        assert_equal(folio["positions"][0]["liquidity"], n0.getposition(pos["position"])["liquidity"])
        assert_equal([o["order"] for o in folio["orders"]], [order["order"]])
        assert_equal((folio["orders"][0]["sell"]["ticker"], folio["orders"][0]["buy"]["ticker"], folio["orders"][0]["status"]), ("USDX", "USDY", "open"))
        assert_equal([a["auction"] for a in folio["auctions"]], [auction["txid"]])
        assert_equal(folio["auctions"][0]["base"]["ticker"], "USDY")
        assert_equal(api.get("addresses/notanaddress/portfolio").status, 400)
        # A launch's creator: MOON's (n0's "launch creator" address).
        creator = n0.getlaunch("MOON")["creator"]
        made = api.rest(f"addresses/{creator}/portfolio")
        moon = next(l for l in made["launches"] if l["ticker"] == "MOON")
        assert_equal(moon["state"], n0.getlaunch("MOON")["status"])
        assert_equal(moon["claimable_fees"], n0.getlaunch("MOON")["creator_fees_owed"] >= Decimal("0.0005"))
        # 7.0.1: several addresses at once (a wallet's): the assets summed, the rest with
        # its owner; GET with a short list, POST with a long one.
        both = api.rest("addresses/portfolio", {"addresses": f"{mine},{creator}"})
        assert_equal(both["addresses"], [mine, creator])
        assert_equal(both["address"], mine)
        assert_equal([p["owner"] for p in both["positions"]], [mine])
        assert_equal([o["owner"] for o in both["orders"]], [mine])
        assert_equal([l["owner"] for l in both["launches"] if l["ticker"] == "MOON"], [creator])
        for a in both["assets"]:
            alone = [x for f in (folio, made) for x in f.get("assets", []) if x["asset_id"] == a["asset_id"]]
            assert_equal(int(a["amount_units"]), sum(int(x["amount_units"]) for x in alone))
            assert_equal(a["holders"], len(alone))
        posted = api.post("addresses/portfolio", {"addresses": [mine, creator, mine]})
        assert_equal(posted.status, 200)
        assert_equal(posted.json()["addresses"], [mine, creator])
        assert_equal(api.get("addresses/portfolio", {"addresses": "notanaddress"}).status, 400)
        # More than 50 addresses in one call: refused before any work (the site reads a wallet's several at a time).
        over = api.post("addresses/portfolio", {"addresses": [n0.getnewaddress() for _ in range(51)]})
        assert_equal(over.status, 400)
        assert "at most 50" in over.json()["error"]["message"], over.json()
        assert_equal(api.post("addresses/portfolio", {"addresses": [n0.getnewaddress() for _ in range(50)]}).status, 200)
        main_address = self.main.getnewaddress()
        # For the website's test: the address with a position, an order and an auction, and an address of eCash.
        self.e2e_env = {"E2E_ADDRESS": mine, "E2E_ECASH_ADDRESS": main_address}
        for verb, query in [
            ("position", {"pool": clpool, "price_lower": "0.98", "price_upper": "1.02", "amount_a": "100", "amount_b": "100", "slippage_bps": "100"}),
            ("position", {"pool": clpool, "price_lower": "0", "price_upper": "0", "amount_a": "100", "amount_b": "100", "slippage_bps": "100", "expected_price": "1"}),
            ("reposition", {"position": pos["position"], "action": "remove", "percent": "50", "slippage_bps": "100"}),
            ("reposition", {"position": pos["position"], "action": "collect"}),
            ("settle", {"order": order["order"], "cancel": "true"}),
            ("bid", {"auction": auction["txid"], "amount": "1"}),
            ("collect", {"auction": auction["txid"]}),
            ("claim", {"launch": "MOON"}),
            ("pegin", {"amount": "1.5", "address": mine}),
            ("pegout", {"address": main_address, "amount": "1"}),
            ("withdraw", {"pool": cp["pool"], "shares": "1000", "only": "a"}),
        ]:
            built = api.rest(f"build/{verb}", query)
            got, params, _ = parse_uri(built["uri"])
            assert_equal(got, verb)
            assert set(params) - {"network", "exp"} <= URI_PARAMS[verb], (verb, params)
            assert built["app_ready"]
            assert_equal(built["payload_hash"], action_hash(built["payload"]))
            if verb == "position" and query.get("expected_price"):
                assert_equal(params["price"], "1")
            if verb == "pegin":
                assert_equal(built["deposit_address"], n0.getdepositaddress(mine)["depositaddress"])
            if verb in {"reposition", "settle", "bid", "claim", "pegout"}:
                assert built["facts"], verb
        assert_equal(api.get("build/reposition", {"position": pos["position"], "action": "remove", "percent": "101"}).status, 400)
        assert_equal(api.get("build/pegout", {"address": main_address, "amount": "0.00000001"}).status, 400)
        assert api.get("build/settle", {"order": "00" * 32}).status in (400, 404)
        # The calls the app's link host makes for these verbs, in exactly its shapes (src/webui/linkhost.cpp Resolve).
        # (Nothing owed yet: a collect alone is refused by the wallet, with what it is.)
        assert_raises_rpc_error(-8, "Nothing to collect yet", n0.changeposition, pos["position"], {"collect": True})
        n0.changeposition(pos["position"], {"remove": 25, "collect": True, "slippage": 0.5})
        n0.settleorder(order["order"], True)
        held_shares = n0.listmyassets()["pools"] if "pools" in n0.listmyassets() else []
        quoted = n0.quotepoolliquidity(cp["pool"], {"0x" + x: 10, "ECX": Decimal("0.005")})["shares"]
        # The link's floor binds: refused above what the pool gives now, taken at it.
        assert_raises_rpc_error(-8, "fewer than the", n0.addpoolliquidity, cp["pool"], {"0x" + x: 10, "ECX": Decimal("0.005")}, 1.0, quoted + 1)
        added = n0.addpoolliquidity(cp["pool"], {"0x" + x: 10, "ECX": Decimal("0.005")}, 1.0, quoted)
        self.bmm()
        part = added["shares"] // 2
        out = n0.quotepoolliquidity(cp["pool"], None, part)["amounts"]
        too_much = {k: v * 2 + 1 for k, v in out.items()}
        assert_raises_rpc_error(-8, "less than the least", n0.removepoolliquidity, cp["pool"], 100, None, 2.0, part, too_much)
        taken = n0.removepoolliquidity(cp["pool"], 100, None, 2.0, part, out)
        assert_equal(taken["shares"], part)
        assert_raises_rpc_error(-8, "fewer than", n0.removepoolliquidity, cp["pool"], 100, None, 1, 10**15)
        n0.collectauction(auction["txid"])
        self.bmm()
        assert added["txid"] and held_shares is not None
        # Left at the address for the website's test: a position, an open order and an open auction.
        n0.setlinkdestination(mine)
        n0.openposition(clpool, Decimal("0.99"), Decimal("1.01"), {"0x" + x: 500, "0x" + y: 500})
        n0.placeorder("USDX", 10, "USDY", 3)
        n0.createauction("USDY", 50, "ECX", 5, 1, 1000)
        n0.setlinkdestination()
        self.bmm()
        folio = api.rest(f"addresses/{mine}/portfolio")
        assert folio["positions"] and folio["orders"] and folio["auctions"]
        # A route through two pools: the link verb route, which the app now takes.
        routed = api.rest("build/swap", {"from": "ECX", "to": "USDY", "amount": "0.01", "slippage_bps": "100", "max_hops": "3"})
        assert routed["app_ready"]
        assert_equal(parse_uri(routed["uri"])[0], routed["verb"])
        assert_equal(api.rest("tickers/MOON"), {"ticker": "MOON", "available": False, "reason": "taken"})
        assert_equal(api.rest("tickers/fresh1")["available"], True)
        assert_equal(api.rest("tickers/a")["available"], False)
        built = api.rest("build/launch", {"ticker": "NEW1", "name": "New One", "description": "A new one", "links": "https://new.example", "first_buy_ecx": "2"})
        verb, params, links = parse_uri(built["uri"])
        assert_equal(verb, "launch")
        assert set(params) - {"network", "exp"} <= URI_PARAMS["launch"]
        assert_equal(links, ["https://new.example"])
        assert_equal(params["ecx"], "2")
        assert_equal(params["slippage_bps"], "100")
        # Without a first buy: no slippage in the link or the payload (the app refuses one there: 7.0.1).
        bare = api.rest("build/launch", {"ticker": "NEW3", "name": "No buy", "supply": "1000000000", "decimals": "6", "fee_bps": "115"})
        verb, params, _ = parse_uri(bare["uri"])
        assert_equal(verb, "launch")
        assert "slippage_bps" not in params, params
        assert "slippage_bps" not in json.loads(bare["payload"]), bare["payload"]
        assert_equal(api.get("build/launch", {"ticker": "MOON", "name": "x"}).status, 409)
        assert_equal(api.get("build/launch", {"ticker": "NEW2", "name": "x", "links": "http://insecure.example"}).status, 400)

        self.log.info("Logos by URL (8.1.0): an upload, the same bytes served by their hash, builds with logo_url")
        from feature_bitassets_logo import b64, make_png
        site_logo = make_png([10, 120, 200, 250, 250, 10])
        digest = hashlib.sha256(site_logo).hexdigest()
        r = api.post("logos", {"base64": b64(site_logo)})
        assert_equal(r.status, 200)
        assert_equal(r.headers["cache-control"], "no-store")
        uploaded = r.json()
        assert_equal(uploaded, {"sha256": digest, "url": f"/api/v1/logos/{digest}"})
        served = api.request("GET", uploaded["url"])
        assert_equal(served.status, 200)
        assert_equal(served.body, site_logo)
        assert_equal(served.headers["content-type"], "image/png")
        assert_equal(served.headers["cache-control"], "public, max-age=31536000, immutable")
        assert_equal(api.get("logos/" + "0" * 64).status, 404)
        r = api.post("logos", {"base64": b64(b"not a logo")})
        assert_equal(r.status, 400)
        assert "bad-ba-logo" in r.json()["error"]["message"], r
        site_url = "https://flush.markets" + uploaded["url"]
        built = api.rest("build/launch", {"ticker": "NEW4", "name": "Logo by URL", "logo_sha256": digest, "logo_url": site_url})
        verb, params, _ = parse_uri(built["uri"])
        assert_equal(verb, "launch")
        assert set(params) - {"network", "exp"} <= URI_PARAMS["launch"]
        assert_equal((params["logo"], params["logo_url"]), ("sha256:" + digest, site_url))
        assert_equal(json.loads(built["payload"])["logo_sha256"], digest)
        built = api.rest("build/register", {"ticker": "NEW5", "name": "Logo by URL", "supply": "1000", "decimals": "2", "logo_sha256": digest, "logo_url": site_url})
        assert_equal(parse_uri(built["uri"])[1]["logo_url"], site_url)
        # logo_url only with logo_sha256, and only an https address of a DNS name.
        assert_equal(api.get("build/launch", {"ticker": "NEW6", "name": "x", "logo_url": site_url}).status, 400)
        for bad in ("http://flush.markets/a.png", "https://127.0.0.1/a.png", "https://localhost/a.png"):
            assert_equal(api.get("build/launch", {"ticker": "NEW6", "name": "x", "logo_sha256": digest, "logo_url": bad}).status, 400)

        self.log.info("Broadcast: a transaction signed elsewhere; refusals")
        raw = n2.createrawtransaction([], {n0.getnewaddress(): 1})
        funded = n2.fundrawtransaction(raw)["hex"]
        signed = n2.signrawtransactionwithwallet(funded)["hex"]
        r = api.post("broadcast", {"hex": signed})
        assert_equal(r.status, 200)
        txid = r.json()["txid"]
        assert txid in n0.getrawmempool()
        assert_equal(api.post("broadcast", {"hex": "00"}).status, 400)
        assert_equal(api.post("broadcast", {"hex": signed}).status in (200, 400), True)
        burn = n2.createrawtransaction([], [{"data": "00"}, {n0.getnewaddress(): 1}])
        burn = n2.fundrawtransaction(burn)["hex"]
        assert_equal(api.get("broadcast").status, 501)
        assert_equal(api.request("POST", "/api/v1/launches", body="{}", headers={"Content-Type": "application/json"}).status, 501)
        self.bmm()

        self.log.info("CORS: the allowed origin, another refused, a preflight; never credentials")
        r = api.get("status", headers={"Origin": ORIGIN})
        assert_equal(r.status, 200)
        assert_equal(r.headers["access-control-allow-origin"], ORIGIN)
        assert "access-control-allow-credentials" not in r.headers
        assert_equal(r.headers["vary"], "Origin")
        assert_equal(r.headers["x-content-type-options"], "nosniff")
        assert_equal(r.headers["cache-control"], "public, max-age=5")
        r = api.get("status", headers={"Origin": "https://evil.example"})
        assert_equal(r.status, 403)
        assert "access-control-allow-origin" not in r.headers
        r = api.request("OPTIONS", "/api/v1/status", headers={"Origin": ORIGIN, "Access-Control-Request-Method": "GET"})
        assert_equal(r.status, 204)
        assert_equal(r.headers["access-control-allow-origin"], ORIGIN)
        assert "GET" in r.headers["access-control-allow-methods"]
        r = api.request("OPTIONS", "/api/v1/status", headers={"Origin": "https://evil.example"})
        assert_equal(r.status, 403)
        # A node without an origin allowed answers no page at all.
        assert_equal(strict.get("status", headers={"Origin": ORIGIN}).status, 403)
        # ETag: the same reply at the same tip is not sent again.
        r = api.get("launches")
        etag = r.headers["etag"]
        r2 = api.get("launches", headers={"If-None-Match": etag})
        assert_equal(r2.status, 304)
        assert_equal(r2.body, b"")

        self.log.info("Rate limits: a client over its budget gets 429 and Retry-After")
        codes = [strict.get("status").status for _ in range(6)]
        assert_equal(codes[:3], [200, 200, 200])
        assert 429 in codes[3:]
        r = strict.get("status")
        assert_equal(r.status, 429)
        assert int(r.headers["retry-after"]) >= 1
        assert_equal(r.json()["error"]["code"], "resource_exhausted")
        # The proxy the operator named (here: localhost, with X-Real-IP) names the client: another client has its own budget.
        assert_equal(strict.get("status", headers={"X-Real-IP": "203.0.113.7"}).status, 200)
        # An IPv6 client is its /64 (-publicapiipv6prefix): rotating addresses in it gets no fresh budget.
        codes = [strict.get("status", headers={"X-Real-IP": f"2a02:6b8:7:8:{i:x}::{i * 3:x}"}).status for i in range(1, 7)]
        assert_equal(codes[:3], [200, 200, 200])
        assert 429 in codes[3:]
        assert_equal(strict.get("status", headers={"X-Real-IP": "2a02:6b8:7:9::1"}).status, 200)

        self.log.info("Slow clients: a request not all in within 15 s closes the connection; 16 connections an address")
        import socket
        slow = socket.create_connection(("127.0.0.1", api_port(0)))
        slow.sendall(b"GET /api/v1/status HTTP/1.1\r\n")
        start = time.time()
        closed = False
        while time.time() - start < 40:
            try:
                slow.sendall(b"X-A: b\r\n")  # a header line every second: never complete
            except OSError:
                closed = True
                break
            slow.settimeout(1)
            try:
                if slow.recv(1) == b"":
                    closed = True
                    break
            except socket.timeout:
                pass
        assert closed, "a request that never completes holds the connection"
        assert 14 <= time.time() - start <= 25, time.time() - start
        slow.close()
        held = [socket.create_connection(("127.0.0.1", api_port(0))) for _ in range(16)]
        extra = socket.create_connection(("127.0.0.1", api_port(0)))
        extra.settimeout(5)
        assert_equal(extra.recv(1), b"")  # refused: closed at once
        for c in held + [extra]:
            c.close()
        self.wait_until(lambda: api.get("status").status == 200)

        self.log.info("No wallet method is reachable; unknown procedures and paths")
        assert_equal(api.connect("ListUnspent", {})[0], 501)
        assert_equal(api.request("POST", "/flush.dex.v1.DexService/../wallet", body="{}", headers={"Content-Type": "application/json"}).status, 501)
        assert_equal(api.get("wallet/balance").status, 404)
        assert_equal(api.get("launches/MOON/../../wallet").status, 404)

        self.log.info("Orders' outcomes (8.2.0): swaps filled and missed, a launch buy past the curve's end, limit orders, by address; a reorganisation")
        moon_id = api.rest("launches/MOON")["id"]
        moon_pool = done["pool"]["pool"]
        # The sale on MOON's curve: filled, ECX out by the coinbase to the seller.
        out = api.rest(f"trades/{sold['txid']}")
        assert_equal((out["kind"], out["status"], out["swap"]["path"]), ("swap", "filled", ["MOON", "ECX"]))
        assert_equal(out["swap"]["hops"][0]["curve"], "launch")
        assert_equal(out["swap"]["hops"][0]["launch"], moon_id)
        assert_equal(out["swap"]["payouts"][0]["what"], "out")
        assert_equal(out["swap"]["payouts"][0]["via"], "coinbase")
        assert_equal(Decimal(out["swap"]["payouts"][0]["amount_units"]) / 10**8, sold["quote"])
        assert n1.getaddressinfo(out["address"])["ismine"]
        # The whale's buy took the curve to its end: what it could not spend came back (back), by the coinbase.
        big = api.rest(f"trades/{whale['txid']}")
        assert_equal((big["status"], big["swap"]["hops"][0]["venue"]), ("filled", "curve"))
        assert_greater_than(int(big["swap"]["back_units"]), 0)
        assert_equal(Decimal(big["swap"]["back_units"]) / 10**8, whale["back"])
        assert_equal([(p["what"], p["via"]) for p in big["swap"]["payouts"]], [("out", "output"), ("back", "coinbase")])
        # The wallet of a node that keeps the index says it exactly too: the coin in, the tokens out, the coin back.
        rows = self.moves(n0, whale["txid"])
        assert_equal([(r[0], r[2], r[4]) for r in rows], [("swap", -1, ("coin",)), ("swap", 1, ()), ("swap", 1, ("refund", "coin", "coinbase"))])
        assert_equal(rows[1][3], Decimal(big["swap"]["payouts"][0]["amount_units"]) / 10**6)
        assert_equal(rows[2][3], whale["back"])
        assert_equal(big["swap"]["payouts"][0]["outpoint"].split(":")[0], whale["txid"])
        # What the actions bring in goes to one address: its outcomes, newest first.
        trader = n0.getnewaddress()
        n0.setlinkdestination(trader)
        route = n0.swappools("USDX", 100, [cp["pool"], moon_pool])
        self.bmm()
        routed = api.rest(f"trades/{route['txid']}")
        assert_equal((routed["status"], routed["address"]), ("filled", trader))
        assert_equal(routed["swap"]["path"], ["USDX", "ECX", "MOON"])
        assert_equal([h["pool"] for h in routed["swap"]["hops"]], [cp["pool"], moon_pool])
        assert_equal(len(routed["swap"]["amounts_units"]), 3)
        assert_equal(routed["swap"]["amounts_units"][0], "10000")
        assert_equal(routed["swap"]["hops"][1]["amount_in_units"], routed["swap"]["amounts_units"][1])
        assert routed["swap"]["payouts"][0]["outpoint"].startswith(route["txid"] + ":")
        assert Decimal(routed["swap"]["payouts"][0]["amount_units"]) >= Decimal(str(route["min_out"])) * 10**6
        assert_equal(self.moves(n0, route["txid"]), [("swap", "USDX", -1, Decimal(100), ()), ("swap", "MOON", 1, Decimal(routed["swap"]["payouts"][0]["amount_units"]) / 10**6, ())])
        # Two buys in one block, one price: the small one at no slippage misses, refunded by the coinbase.
        tight = n0.swappools("ECX", 1, [moon_pool], 0)
        n0.swappools("ECX", 50, [moon_pool], 50)
        self.bmm()
        miss = api.rest(f"trades/{tight['txid']}")
        assert_equal(miss["status"], "missed")
        assert_equal(miss["swap"]["amounts_units"], ["100000000"])
        assert_equal([(p["what"], p["asset"], p["amount_units"], p["via"]) for p in miss["swap"]["payouts"]], [("refund", "ECX", "100000000", "coinbase")])
        # In the wallet: the coin in, and the same back by the coinbase (not filled), exactly.
        assert_equal([(r[0], r[2], r[3], r[4]) for r in self.moves(n0, tight["txid"])], [("swap", -1, Decimal(1), ("coin",)), ("swap", 1, Decimal(1), ("refund", "coin", "coinbase"))])
        # Limit orders of USDY for ECX: an ask partly filled by a crossing, collected, filled; one cancelled, one expired.
        ask = n0.placeorder("USDY", 100, "ECX", Decimal("0.01"))
        n0.placeorder("ECX", Decimal("0.4"), "USDY", 100)
        self.bmm()
        placed = api.rest(f"trades/{ask['txid']}")
        assert_equal((placed["kind"], placed["status"], placed["address"]), ("order", "open", trader))
        o = placed["order"]
        assert_equal((o["order_id"], o["book"], o["side"], o["sell"], o["buy"]), (ask["order"], ask["book"], "ask", "USDY", "ECX"))
        assert_equal((o["amount_units"], o["fill_count"], len(o["fills"])), ("10000", 1, 1))
        assert_equal(o["fills"][0]["sold_units"], "4000")
        assert_equal(o["remaining_units"], "6000")
        assert o["owner_outpoint"].startswith(ask["txid"] + ":")
        collect = n0.settleorder(ask["order"])
        self.bmm()
        n0.placeorder("ECX", Decimal("0.6"), "USDY", 100)
        self.bmm()
        filled = api.rest(f"trades/{ask['txid']}")
        assert_equal((filled["status"], filled["order"]["fill_count"], filled["order"]["remaining_units"]), ("filled", 2, "0"))
        assert_equal([c["txid"] for c in filled["order"]["collected"]], [collect["txid"]])
        assert_equal(filled["order"]["collected"][0]["payouts"][0]["what"], "proceeds")
        assert_equal(filled["order"]["collected"][0]["payouts"][0]["via"], "coinbase")
        close = n0.settleorder(ask["order"])
        ask2 = n0.placeorder("USDY", 100, "ECX", 1)
        ask3 = n0.placeorder("USDY", 100, "ECX", 1, 1)
        self.bmm()
        cancel = n0.settleorder(ask2["order"], True)
        self.bmm()
        self.bmm()
        n0.setlinkdestination()
        settled = api.rest(f"trades/{ask['txid']}")
        assert_equal(settled["status"], "settled")
        assert_equal(settled["order"]["settle"]["txid"], close["txid"])
        assert_equal(settled["settle_txid"], None)
        # A settle's txid finds its order.
        assert_equal(api.rest(f"trades/{collect['txid']}")["txid"], ask["txid"])
        assert_equal(api.rest(f"trades/{close['txid']}")["settle_txid"], close["txid"])
        cancelled = api.rest(f"trades/{ask2['txid']}")
        assert_equal(cancelled["status"], "cancelled")
        assert_equal([(p["what"], p["amount_units"], p["via"]) for p in cancelled["order"]["settle"]["payouts"]], [("left", "10000", "output")])
        assert_equal(cancelled["order"]["settle"]["txid"], cancel["txid"])
        assert_equal(self.moves(n0, ask2["txid"]), [("order_place", "USDY", -1, Decimal(100), ())])
        assert_equal(self.moves(n0, cancel["txid"]), [("order_cancel", "USDY", 1, Decimal(100), ("refund",))])
        assert_equal(api.rest(f"trades/{ask3['txid']}")["status"], "expired")
        # The address's, newest first (its three swaps, the two bids, the three asks), a page at a time.
        listed = api.rest(f"addresses/{trader}/trades")
        txids = [t["txid"] for t in listed["items"]]
        assert_equal(listed["total"], len(txids))
        assert_equal(len(txids), 8)
        assert {route["txid"], tight["txid"], ask["txid"], ask2["txid"], ask3["txid"]} <= set(txids)
        heights = [t["height"] for t in listed["items"]]
        assert_equal(heights, sorted(heights, reverse=True))
        paged, cursor = [], None
        while True:
            page = api.rest(f"addresses/{trader}/trades", {"limit": 2, **({"cursor": cursor} if cursor else {})})
            paged += page["items"]
            cursor = page["next_cursor"]
            if not cursor:
                break
        assert_equal(paged, listed["items"])
        assert_equal(api.get(f"addresses/{trader}/trades", {"limit": 101}).status, 400)
        assert_equal(api.get(f"addresses/{trader}/trades", {"cursor": "00"}).status, 400)
        assert_equal(api.get("addresses/notanaddress/trades").status, 400)
        # Not an order: 404 (a registration, an unknown txid); not a txid: 400.
        assert_equal(api.get(f"trades/{n0.getbestblockhash()}").status, 404)
        assert_equal(api.get(f"trades/{route['txid'][:-2]}").status, 400)
        # A reorganisation takes the cancel's block back, then gives it again.
        cancel_block = n0.getblock(n0.getbestblockhash())["previousblockhash"]
        tip = n0.getbestblockhash()
        for n in self.nodes:
            n.invalidateblock(cancel_block)
        assert_equal(api.rest(f"trades/{ask2['txid']}")["status"], "open")
        assert_equal(api.get(f"trades/{cancel['txid']}").status, 404)
        for n in self.nodes:
            n.reconsiderblock(cancel_block)
        self.wait_until(lambda: all(n.getbestblockhash() == tip for n in self.nodes))
        assert_equal(api.rest(f"trades/{ask2['txid']}"), cancelled)
        assert_equal(api.rest(f"addresses/{trader}/trades"), listed)
        self.outcomes = [route["txid"], tight["txid"], ask["txid"], close["txid"], whale["txid"]]
        self.trader = trader

        self.log.info("Connect (JSON, GET, binary) and gRPC: the same answers as REST")
        code, reply = api.connect("GetLaunch", {"id": "MOON"})
        assert_equal(code, 200)
        rest = api.rest("launches/MOON")
        assert_equal(reply["ticker"], rest["ticker"])
        assert_equal(reply["raisedEcx"], rest["raised_ecx"])
        assert_equal(reply["createdHeight"], rest["created_height"])
        code, reply2 = api.connect_get("GetLaunch", {"id": "MOON"})
        assert_equal((code, reply2), (200, reply))
        code, err = api.connect("GetLaunch", {"id": "NOPE"})
        assert_equal((code, err["code"]), (404, "not_found"))
        assert_equal(api.connect("Broadcast", {"hex": "00"})[0], 400)
        # GET for a method with side effects is refused (Connect's rule).
        assert_equal(api.connect_get("Broadcast", {"hex": "00"})[0], 400)
        if protoc_available():
            request = encode("GetLaunchRequest", 'id: "MOON"')
            code, body = api.connect_proto("GetLaunch", request)
            assert_equal(code, 200)
            text = decode("LaunchDetail", body)
            assert 'ticker: "MOON"' in text and f'id: "{rest["id"]}"' in text
            client = os.path.join(self.config["environment"]["BUILDDIR"], "bin", "flush-grpc-client")
            if os.path.exists(client):
                status, message, replies = grpc_call(client, api_port(0), "GetLaunch", request)
                assert_equal(status, 0)
                assert_equal(replies[0], body)
                status, message, replies = grpc_call(client, api_port(0), "GetLaunch", encode("GetLaunchRequest", 'id: "NOPE"'))
                assert_equal(status, 5)
                status, _, replies = grpc_call(client, api_port(0), "grpc.health.v1.Health/Check", b"")
                assert_equal((status, replies[0].hex()), (0, "0801"))
                # Server reflection is off on the public API unless asked for (-publicapireflection).
                status, _, _ = grpc_call(client, api_port(0), "grpc.reflection.v1.ServerReflection/ServerReflectionInfo", b"")
                assert_equal(status, 12)
                trades_req = encode("ListTradesRequest", 'id: "MOON" limit: 2')
                status, _, replies = grpc_call(client, api_port(0), "ListLaunchTrades", trades_req)
                assert_equal(status, 0)
                assert_equal(decode("TradePage", replies[0]).count("items {"), 2)
            else:
                self.log.info("(no flush-grpc-client: gRPC's library not installed; gRPC round trips skipped)")
        else:
            self.log.info("(no protoc: binary round trips skipped)")

        if self.options.hold:
            with open(self.options.hold, "w") as f:
                f.write(str(api_port(0)))
            self.log.info(f"Holding: the API on {api_port(0)} until {self.options.hold} is removed")
            self.wait_until(lambda: not os.path.exists(self.options.hold), timeout=7200)
        if self.options.site:
            self.log.info("The website against this API (flush-web scripts/e2e-live.mjs)")
            import subprocess
            env = dict(os.environ, E2E_API=f"http://127.0.0.1:{api_port(0)}", E2E_PORT=str(api_port(0) + 10), E2E_CURVE="SUN", E2E_GRADUATED="MOON", E2E_PLAIN="PLAIN", E2E_BOOK="USDX",
                       E2E_SHOTS=self.options.site_shots or os.path.join(self.options.tmpdir, "site-shots"), PLAYWRIGHT=self.options.playwright or "", **getattr(self, "e2e_env", {}))
            result = subprocess.run(["node", "scripts/e2e-live.mjs"], cwd=self.options.site, env=env, capture_output=True, text=True, timeout=600)
            for line in result.stdout.splitlines():
                self.log.info("site: " + line)
            if result.returncode != 0:
                raise AssertionError(result.stderr[-3000:])

        self.log.info("A node that turns -dexindex on derives the index again: the same trades and candles")
        self.sync_all()
        self.restart_node(2, extra_args=self.common_args + [f"-publicapi={api_port(2)}", "-publicapirate=1000", "-publicapiburst=5000"])
        self.connect_all()
        third = PublicApi(api_port(2))
        self.wait_until(lambda: third.get("status").status == 200)
        assert_equal(third.rest("launches/MOON/trades"), api.rest("launches/MOON/trades"))
        assert_equal(third.rest("launches/MOON/candles", {"interval": "1h"}), api.rest("launches/MOON/candles", {"interval": "1h"}))
        assert_equal(third.rest("launches/MOON/holders", {"limit": 100}), api.rest("launches/MOON/holders", {"limit": 100}))
        assert_equal(third.rest(f"pools/{pool['pool']}/trades"), api.rest(f"pools/{pool['pool']}/trades"))
        for txid in self.outcomes:
            assert_equal(third.rest(f"trades/{txid}"), api.rest(f"trades/{txid}"))
        assert_equal(third.rest(f"addresses/{self.trader}/trades"), api.rest(f"addresses/{self.trader}/trades"))


if __name__ == '__main__':
    PublicApiTest(__file__).main()
