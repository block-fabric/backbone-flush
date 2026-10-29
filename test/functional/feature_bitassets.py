#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the assets: registration, transfers, mint and burn, pools, auctions, undo."""

from decimal import Decimal

from feature_sidechain import ACTIVATION_PERIOD, SLOT, SidechainTest
from test_framework.messages import COIN
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error


class BitAssetsTest(SidechainTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        super().run_test()

    def mine_txs(self):
        self.sync_mempools()
        return self.nodes[0].getblock(self.bmm(), 2)

    def holding(self, node, label):
        for a in node.listmyassets()["assets"]:
            if a["label"] == label:
                return a
        return {"balance": Decimal(0), "control": False}

    def payouts_to(self, node, block):
        """The ECX the coinbase of a block pays to addresses of the wallet of `node`, beyond the first output."""
        total = Decimal(0)
        for out in block["tx"][0]["vout"][1:]:
            address = out["scriptPubKey"].get("address")
            if address and node.getaddressinfo(address)["ismine"]:
                total += out["value"]
        return total

    def marker_data(self, node, txid):
        """The data the marker of a transaction pushes."""
        raw = node.getrawtransaction(txid, True)
        marker = bytes.fromhex(next(o for o in raw["vout"] if o["scriptPubKey"]["hex"].startswith("6a"))["scriptPubKey"]["hex"])
        # OP_RETURN, then one push: its data.
        assert_equal(marker[0], 0x6a)
        data = marker[2:] if marker[1] < 0x4c else marker[3:]
        assert_equal(len(data), marker[1] if marker[1] < 0x4c else marker[2])
        return data

    def run_sidechain_test(self):
        main = self.main
        side, other = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = side.getnewaddress()

        self.log.info("Set up the sidechain and fund both wallets")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "BitAssets", "Assets")
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, side.getnewaddress(), 20)
        main.createsidechaindeposit(SLOT, other.getnewaddress(), 20)
        self.mine_main()
        self.bmm()
        side.sendmany("", {side.getnewaddress(): 1 for _ in range(10)})
        other.sendmany("", {other.getnewaddress(): 1 for _ in range(10)})
        self.mine_txs()

        self.log.info("Reserve, then register an asset")
        assert_raises_rpc_error(-8, "no reservation", side.registerasset, "GOLD", 1000000, 2)
        # A name that would read as another asset's number where names are shown with their numbers.
        for look_alike in ("GOLD (#1739-0029)", "GOLD #2", "GOLD 1739-0029"):
            assert_raises_rpc_error(-8, "would read as the number of another asset", side.reserveasset, look_alike)
        # Text shaped as a number that is none: an error, not the asset of that name.
        assert_raises_rpc_error(-8, "is not the number of an asset", side.getasset, "0000-1739-0029")
        reserved = side.reserveasset("GOLD")
        self.mine_txs()
        assert_equal(side.getbitassetsinfo()["reservations"], 1)
        reservation = side.listmyassets()["reservations"][0]
        assert_equal(reservation["name"], "GOLD")
        # Revealed only once deep enough (2 blocks on regtest), so that whoever makes a block cannot
        # reserve the name itself and register it first, in its own block.
        assert_equal(side.getbitassetsinfo()["reveal_depth"], 2)
        assert_equal(reservation["registers_from"], reservation["height"] + 2)
        assert_equal(reservation["wait"], 1)
        assert_raises_rpc_error(-4, "too recent", side.registerasset, "GOLD", 1000000, 2)
        self.mine_txs()
        assert_equal(side.listmyassets()["reservations"][0]["wait"], 0)
        registered = side.registerasset("GOLD", 1000000, 2, {"info": "One gram of gold"})
        assert_equal(registered["asset"], reserved["asset"])
        self.mine_txs()
        gold = side.getasset("GOLD")
        assert_equal(gold["name"], "GOLD")
        assert_equal(gold["decimals"], 2)
        assert_equal(gold["supply"], Decimal("1000000.00"))
        assert_equal(gold["data"], {"info": "One gram of gold"})
        assert_equal(gold["fixed"], False)
        assert_equal(side.getasset(gold["seq"])["asset"], gold["asset"])
        assert_equal(side.getasset("0x" + gold["asset"])["name"], "GOLD")
        assert_equal(self.holding(side, "GOLD")["balance"], Decimal("1000000.00"))
        assert_equal(self.holding(side, "GOLD")["control"], True)
        assert_equal(side.listmyassets()["reservations"], [])
        assert_equal(side.getbitassetsinfo()["assets"], 1)
        # Taken.
        assert_raises_rpc_error(-8, "is registered", other.reserveasset, "GOLD")
        # Names that read as another asset: CHN, a number, a hash.
        for name in ["ECX", "chn", gold["seq"], "1739-0029", "0x" + gold["asset"][:16], "0xGOLD"]:
            assert_raises_rpc_error(-8, "reads as another asset", other.reserveasset, name)

        self.log.info("An asset without supply yet, and a private one")
        side.reserveasset("SILVER")
        other.reserveasset("SECRET")
        self.mine_txs()
        self.mine_txs()
        # The wallet's reservation of another name is not one of this name.
        assert_raises_rpc_error(-8, "no reservation", side.registerasset, "BRONZE", 1)
        side.registerasset("SILVER", 0)
        other.registerasset("SECRET", 5, 0, {}, False)
        self.mine_txs()
        assert_equal(side.getasset("SILVER")["supply"], 0)
        assert_equal(self.holding(side, "SILVER")["control"], True)
        assert "name" not in side.getasset("SECRET")
        assert_equal(sorted(a["label"] for a in side.listassets()), sorted(["GOLD", "SILVER", "0x" + side.getasset("SECRET")["asset"]]))
        assert_equal(len(side.listassets(10, 0, "gol")), 1)

        self.log.info("Send an asset")
        assert_raises_rpc_error(-6, "too little", side.sendasset, other.getnewaddress(), "GOLD", 2000000)
        assert_raises_rpc_error(-3, "at most 2 decimals", side.sendasset, other.getnewaddress(), "GOLD", "1.001")
        side.sendasset(other.getnewaddress(), "GOLD", "250.5")
        # The change is not in a block yet.
        assert_raises_rpc_error(-4, "waiting for the next block", side.sendasset, other.getnewaddress(), "GOLD", 1)
        # It shows as on its way, at both ends, until the block.
        assert_equal(self.holding(side, "GOLD")["balance"], 0)
        assert_equal(self.holding(side, "GOLD")["pending"], Decimal("999749.50"))
        self.sync_mempools()
        assert_equal(self.holding(other, "GOLD")["pending"], Decimal("250.50"))
        self.mine_txs()
        assert_equal(self.holding(other, "GOLD")["balance"], Decimal("250.50"))
        assert_equal(self.holding(side, "GOLD")["balance"], Decimal("999749.50"))
        assert_equal(self.holding(other, "GOLD")["control"], False)

        self.log.info("Mint, burn, change data: only with the control coin")
        assert_raises_rpc_error(-6, "does not hold the control coin", other.mintasset, "GOLD", 10)
        # By its number, as the node's commands take it.
        side.mintasset(side.getasset("GOLD")["seq"], 1000)
        side.mintasset("SILVER", 500)
        self.mine_txs()
        assert_equal(side.getasset("GOLD")["supply"], Decimal("1001000.00"))
        # To an address given: of another wallet.
        assert_raises_rpc_error(-5, "Invalid address", side.mintasset, "GOLD", 5, "nonsense")
        other_gold = self.holding(other, "GOLD")["balance"]
        side.mintasset("GOLD", 5, other.getnewaddress())
        self.mine_txs()
        assert_equal(self.holding(other, "GOLD")["balance"], other_gold + 5)
        assert_equal(self.holding(side, "GOLD")["control"], True)
        assert_equal(side.getasset("GOLD")["supply"], Decimal("1001005.00"))
        # Too little to open a pool: fewer shares than a pool opens with.
        assert_raises_rpc_error(-8, "Too little to open a pool", side.addliquidity, "SILVER", 100, "ECX", "0.001")
        assert_raises_rpc_error(-8, "it makes no shares", side.addliquidity, "SILVER", 1, "ECX", "0.000001")
        side.burnasset("GOLD", 100)
        side.updateasset("GOLD", {"info": "One gram of gold, in Zurich"})
        self.mine_txs()
        gold = side.getasset("GOLD")
        assert_equal(gold["supply"], Decimal("1000905.00"))
        assert_equal(gold["burned"], Decimal("100.00"))
        assert_equal(gold["data"]["info"], "One gram of gold, in Zurich")
        assert_equal([h["value"] for h in side.getassethistory("GOLD")["info"]], ["One gram of gold", "One gram of gold, in Zurich"])

        self.log.info("Every field of an asset's data, set and deleted")
        key = side.getaddressinfo(side.getnewaddress())["pubkey"]
        data = {
            "commitment": "ab" * 32,
            "ipv4": "203.0.113.7:8333",
            "ipv6": "[2001:db8::7]:8333",
            "encryptionkey": key,
            "signingkey": key[2:],
        }
        side.updateasset("GOLD", data)
        set_height = self.mine_txs()["height"]
        assert_equal(side.getasset("GOLD")["data"], {"info": "One gram of gold, in Zurich", **data})
        side.updateasset("GOLD", {"ipv4": None, "ipv6": None, "encryptionkey": None, "signingkey": None, "commitment": None})
        deleted_height = self.mine_txs()["height"]
        assert_equal(side.getasset("GOLD")["data"], {"info": "One gram of gold, in Zurich"})
        history = side.getassethistory("GOLD")
        for field, value in data.items():
            assert_equal([h["value"] for h in history[field]], [None, value, None])
            assert_equal([h["height"] for h in history[field]], [gold["registered"], set_height, deleted_height])
        assert_equal(side.getasset("GOLD", set_height)["data"], {"info": "One gram of gold, in Zurich", **data})
        assert_equal(side.getasset("GOLD", deleted_height)["data"], {"info": "One gram of gold, in Zurich"})
        assert "data" not in side.getasset("GOLD", gold["registered"] - 1)
        for bad, message in [({"ipv4": "2001:db8::7"}, "ipv4: an IPv4 address"), ({"signingkey": "00"}, "signingkey: an x-only public key"), ({"commitment": "ab"}, "commitment: 32 bytes"), ({"colour": "gold"}, "Unknown field")]:
            assert_raises_rpc_error(-8, message, side.updateasset, "GOLD", bad)
        assert_raises_rpc_error(-8, "Nothing to change", side.updateasset, "GOLD", {})

        self.log.info("Control changes hands; a fixed supply cannot change")
        side.transferassetcontrol("SILVER", other.getnewaddress())
        self.mine_txs()
        assert_equal(self.holding(other, "SILVER")["control"], True)
        assert_equal(self.holding(side, "SILVER")["control"], False)
        assert_raises_rpc_error(-8, "Not confirmed", other.fixassetsupply, "SILVER", False)
        other.fixassetsupply("SILVER", True)
        self.mine_txs()
        assert_equal(side.getasset("SILVER")["fixed"], True)
        assert "control" not in side.getasset("SILVER")
        assert_raises_rpc_error(-8, "fixed", other.mintasset, "SILVER", 1)

        self.log.info("A pool of GOLD and ECX")
        assert_raises_rpc_error(-8, "no pool yet", side.addliquidity, "GOLD", 10000, "ECX")
        added = side.addliquidity("GOLD", 10000, "ECX", 5)
        self.mine_txs()
        pool = side.getpool("GOLD", "ECX")
        assert_equal(pool["reserve_a"], Decimal("10000.00"))
        assert_equal(pool["reserve_b"], Decimal("5.00000000"))
        assert_equal(pool["price"], Decimal("0.0005"))
        assert_equal(side.listpools()[0]["asset_a"], "GOLD")
        # Those of an asset, which comes first.
        assert_equal(side.listpools("GOLD"), side.listpools())
        assert_equal(side.listpools("ECX")[0]["asset_a"], "ECX")
        assert_equal(side.listpools("ECX")[0]["reserve_a"], pool["reserve_b"])
        assert_equal(side.listpools("SILVER"), [])
        liquidity = side.listmyassets()["liquidity"]
        assert_equal(len(liquidity), 1)
        assert_equal(liquidity[0]["shares"], added["shares"])
        # A deposit that was to make the pool, which someone made meanwhile, is refused, not signed at
        # their price (and one that was to go in at the pool's price is refused for a pool there is not).
        assert_raises_rpc_error(-8, "Someone opened this pool meanwhile", other.addliquidity, "GOLD", 100, "ECX", 1, 1, True)
        assert_raises_rpc_error(-8, "which was not expected", other.addliquidity, "GOLD", 100, "SILVER", 1, 1, False)
        # More, at the pool's price.
        more = other.addliquidity("GOLD", 100, "ECX", None, 1, False)
        assert_equal(more["amount_b"], Decimal("0.05000000"))
        self.mine_txs()
        # Amounts off the pool's price: only what its price takes goes in, not the excess, which would
        # be given away to its providers.
        clamped = other.addliquidity("GOLD", 10, "ECX", 1)
        assert_equal(clamped["amount_a"], Decimal("10.00"))
        # What the shares 10 GOLD gives are worth: a few satoshis under 0.005 CHN, rounded down shares.
        assert Decimal("0.00499900") < clamped["amount_b"] <= Decimal("0.00500000")
        self.mine_txs()
        clamped = other.addliquidity("GOLD", 1000, "ECX", "0.005")
        assert_equal((clamped["amount_a"], clamped["amount_b"]), (Decimal("10.00"), Decimal("0.00500000")))
        self.mine_txs()

        self.log.info("Trade in it")
        # Less CHN out than is paid out: refused.
        assert_raises_rpc_error(-8, "the least ECX paid out is 0.0005", side.swapasset, "GOLD", "0.5", "ECX")
        quote = other.quoteswap("ECX", "0.1", "GOLD")
        assert_greater_than(quote["amount_out"], 0)
        assert_greater_than(quote["price_impact"], 0)
        before = self.holding(other, "GOLD")["balance"]
        # The least the caller showed is what it signs, at the least: more than the quote, less the
        # slippage, gives is refused.
        assert_raises_rpc_error(-8, "The pool moved", other.swapasset, "ECX", "0.1", "GOLD", 0.5, 10, quote["amount_out"])
        least = (quote["amount_out"] * Decimal("0.995")).quantize(Decimal("0.01"), rounding="ROUND_DOWN")
        swap = other.swapasset("ECX", "0.1", "GOLD", 0.5, 10, str(least))
        assert_equal(swap["quote"], quote["amount_out"])
        assert swap["min_out"] >= least
        self.mine_txs()
        assert_equal(self.holding(other, "GOLD")["balance"], before + quote["amount_out"])
        # GOLD for CHN: the coinbase pays.
        quote = side.quoteswap("GOLD", 200, "ECX")
        side.swapasset("GOLD", 200, "ECX")
        block = self.mine_txs()
        assert_equal(self.payouts_to(side, block), quote["amount_out"])
        assert_equal(side.getpool("GOLD", "ECX")["swaps"], 2)
        # A trade the pool moved away from fails, and leaves the mempool.
        exact = side.quoteswap("ECX", 1, "GOLD", True)
        assert exact["amount_out"] >= Decimal("1.00")

        self.log.info("Pools are listed a page at a time")
        assert_equal(side.getbitassetsinfo()["pools"], 1)
        assert_equal(side.listpools(None, 1, 0), side.listpools())
        assert_equal(side.listpools(None, 1, 1), [])
        assert_equal(side.listpools("GOLD", 0), [])
        assert_raises_rpc_error(-8, "At most 1000", side.listpools, None, 1001)

        self.log.info("Take liquidity out")
        shares = side.listmyassets()["liquidity"][0]["shares"]
        removed = side.removeliquidity("GOLD", "ECX", 50)
        assert_equal(removed["shares"], shares // 2)
        gold_before = self.holding(side, "GOLD")["balance"]
        block = self.mine_txs()
        assert_equal(self.payouts_to(side, block), removed["amount_b"])
        assert_equal(self.holding(side, "GOLD")["balance"], gold_before + removed["amount_a"])
        assert_equal(side.listmyassets()["liquidity"][0]["shares"], shares - shares // 2)

        self.log.info("A Dutch auction: 100 GOLD, from 10 ECX down to 1, over 5 blocks")
        side.createauction("GOLD", 100, "ECX", 10, 1, 5)
        self.mine_txs()
        auction = side.listauctions()[0]
        assert_equal(auction["status"], "open")
        assert_equal(auction["remaining"], Decimal("100.00"))
        assert_equal(auction["price"], Decimal("10.00000000"))
        assert_equal(len(side.listmyassets()["receipts"]), 1)
        # A bid of 2.5 CHN at 10 for all: 25 GOLD.
        assert_equal(side.quotebid(auction["auction"], "2.5")["buys"], Decimal("25.00"))
        before = self.holding(other, "GOLD")["balance"]
        bid = other.bidauction(auction["auction"], "2.5")
        assert_equal(bid["buys"], Decimal("25.00"))
        self.mine_txs()
        assert_equal(self.holding(other, "GOLD")["balance"], before + Decimal(25))
        auction = side.getauction(auction["auction"])
        assert_equal(auction["remaining"], Decimal("75.00"))
        assert_equal(auction["proceeds"], Decimal("2.50000000"))
        # What all that is left costs: not the price of all the auction sold.
        assert_equal(auction["cost_of_remaining"], side.quotebid(auction["auction"], 1)["cost_of_remaining"])
        assert auction["cost_of_remaining"] < auction["price"]
        assert_raises_rpc_error(-8, "running", side.collectauction, auction["auction"])
        # The price fell: all that is left, for less.
        cost = other.getauction(auction["auction"])["cost_of_remaining"]
        all_left = other.bidauction(auction["auction"], None, True)
        assert_equal(all_left["buys"], Decimal("75.00"))
        assert_equal(all_left["pays"], cost)
        self.mine_txs()
        auction = side.getauction(auction["auction"])
        assert_equal(auction["status"], "sold out")
        assert_raises_rpc_error(-8, "The auction takes no more bids", other.bidauction, auction["auction"], 1)
        proceeds = auction["proceeds"]
        block_with_collect = None
        side.collectauction(auction["auction"])
        block_with_collect = self.mine_txs()
        assert_equal(self.payouts_to(side, block_with_collect), proceeds)
        assert_equal(side.getauction(auction["auction"])["status"], "closed")
        assert_equal(side.listmyassets()["receipts"], [])
        assert_equal(side.listauctions(), [])
        assert_equal(len(side.listauctions(True)), 1)

        self.log.info("An auction without bids is canceled, everything back")
        before = self.holding(side, "GOLD")["balance"]
        side.createauction("GOLD", 10, "ECX", 5, 5, 3, 2)
        self.mine_txs()
        auction = side.listauctions()[0]
        assert_equal(auction["status"], "upcoming")
        assert_raises_rpc_error(-8, "starts at height", other.bidauction, auction["auction"], 1)
        side.collectauction(auction["auction"])
        self.mine_txs()
        assert_equal(self.holding(side, "GOLD")["balance"], before)

        self.log.info("Buying all that is left pays what it costs, no more")
        # 1 SILVER (no decimals) for 1 CHN: the most that buys no more than 1 was 1.99999999 CHN.
        side.createauction("SILVER", 1, "ECX", 1, 1, 5)
        self.mine_txs()
        auction = side.listauctions()[0]
        assert_equal(auction["cost_of_remaining"], Decimal("1.00000000"))
        assert_raises_rpc_error(-8, "more than the most given", other.bidauction, auction["auction"], "0.99999999", True)
        all_left = other.bidauction(auction["auction"], 1, True)
        assert_equal(all_left["pays"], auction["cost_of_remaining"])
        assert_equal(all_left["buys"], 1)
        self.mine_txs()
        assert_equal(side.getauction(auction["auction"])["proceeds"], Decimal("1.00000000"))
        assert_equal(self.holding(other, "SILVER")["balance"], 1)
        side.collectauction(auction["auction"])
        self.mine_txs()
        # 300 SILVER for 0.07 GOLD (7 units): a unit of GOLD buys 42 SILVER, so after a bid no amount
        # buys exactly the 258 left (6 units buy 257, 7 buy 300): buying all pays 7 units for the 258.
        side.createauction("SILVER", 300, "GOLD", "0.07", "0.07", 5)
        self.mine_txs()
        auction = side.listauctions()[0]
        assert_equal(auction["cost_of_remaining"], Decimal("0.07"))
        assert_equal(other.bidauction(auction["auction"], "0.01")["buys"], 42)
        self.mine_txs()
        all_left = other.bidauction(auction["auction"], None, True)
        assert_equal((all_left["pays"], all_left["buys"]), (Decimal("0.07"), 258))
        self.mine_txs()
        assert_equal(side.getauction(auction["auction"])["status"], "sold out")
        side.collectauction(auction["auction"])
        # A bid of more than all that is left costs pays only that.
        side.createauction("SILVER", 10, "ECX", "0.5", "0.5", 5)
        self.mine_txs()
        auction = side.listauctions()[0]
        overpaid = other.bidauction(auction["auction"], "0.52")
        assert_equal(overpaid["pays"], Decimal("0.50000000"))
        assert_equal(overpaid["buys"], 10)
        self.mine_txs()
        side.collectauction(auction["auction"])
        self.mine_txs()

        self.log.info("A bid for all that is left, taken blocks later, pays what it costs then and gets the rest back")
        side.createauction("SILVER", 10, "ECX", 1, "0.5", 5)
        self.mine_txs()
        auction = side.listauctions()[0]
        late = other.bidauction(auction["auction"], None, True)
        assert_equal(late["buys"], 10)
        # Held out of the next two blocks (the miner is node 0), as if they were full.
        self.sync_mempools()
        side.prioritisetransaction(late["txid"], 0, -COIN)
        self.mine_txs()
        self.mine_txs()
        assert late["txid"] in side.getrawmempool()
        side.prioritisetransaction(late["txid"], 0, COIN)
        block = self.mine_txs()
        assert late["txid"] in [t["txid"] for t in block["tx"]]
        auction = side.getauction(auction["auction"])
        assert_equal(auction["status"], "sold out")
        assert auction["proceeds"] < late["pays"]
        assert_equal(self.payouts_to(other, block), late["pays"] - auction["proceeds"])
        side.collectauction(auction["auction"])
        self.mine_txs()

        self.log.info("Auctions are listed a page at a time")
        info = side.getbitassetsinfo()
        every = side.listauctions(True)
        assert_equal(len(every), info["auctions_all"])
        assert_greater_than(len(every), 3)
        assert_equal(len(side.listauctions()), info["auctions"])
        pages = []
        for skip in range(0, len(every), 2):
            pages += side.listauctions(True, 2, skip)
        assert_equal(pages, every)
        assert_equal(side.listauctions(True, 10, len(every)), [])
        assert_equal(side.listauctions(True, 1000, 0, "SILVER"), [a for a in every if "SILVER" in (a["base"], a["quote"])])

        self.log.info("Reservations are released")
        side.reserveasset("SPARE")
        self.mine_txs()
        side.releaseassetreservation("SPARE")
        self.mine_txs()
        assert_equal(side.listmyassets()["reservations"], [])
        # Named in lower case, as the wallet reserves names from ticker_height (uppercased).
        side.reserveasset("SPARETWO")
        self.mine_txs()
        side.releaseassetreservation("sparetwo")
        self.mine_txs()
        assert_equal(side.listmyassets()["reservations"], [])
        assert_equal(side.getbitassetsinfo()["reservations"], 0)
        activity = side.listassetactivity()
        assert_greater_than(len(activity), 10)
        assert "burn" in [a["operation"] for a in activity]

        self.log.info("Trades too large for a pool, and pools nobody provides for, are refused")
        assert_raises_rpc_error(-8, "too large for this pool", side.swapasset, "ECX", 100, "GOLD")
        assert side.quoteswap("ECX", 100, "GOLD")["price_impact"] > 10
        assert_equal(side.getpool("GOLD", "ECX")["abandoned"], False)
        side.removeliquidity("GOLD", "ECX")
        other.removeliquidity("GOLD", "ECX")
        self.mine_txs()
        assert_equal(side.getpool("GOLD", "ECX")["abandoned"], True)
        assert_equal(side.listpools()[0]["abandoned"], True)
        assert_raises_rpc_error(-8, "Nobody provides liquidity", side.swapasset, "ECX", "0.001", "GOLD")
        # Reopened as a new pool is made: both amounts, which set its price, not the price it was left at.
        assert_raises_rpc_error(-8, "Give both amounts", side.addliquidity, "GOLD", 100, "ECX")
        reopened = side.addliquidity("GOLD", 1000, "ECX", 2)
        assert_equal(reopened["sets_price"], True)
        self.mine_txs()
        assert_equal(side.getpool("GOLD", "ECX")["abandoned"], False)
        assert_equal(other.addliquidity("GOLD", 10, "ECX")["sets_price"], False)
        self.mine_txs()
        side.removeliquidity("GOLD", "ECX")
        other.removeliquidity("GOLD", "ECX")
        self.mine_txs()
        assert_equal(side.getpool("GOLD", "ECX")["abandoned"], True)

        self.log.info("Reopening a pool cannot be front-run")
        dust = side.getpool("GOLD", "ECX")
        assert_greater_than(dust["reserve_a"], 0)
        # The dust counts toward what the pool reopens with: amounts not above it are refused.
        assert_raises_rpc_error(-8, "count toward what it reopens with", side.addliquidity, "GOLD", dust["reserve_a"], "ECX", 1)
        gold_before = self.holding(side, "GOLD")["balance"]
        # Alice reopens it at 0.01 CHN a GOLD; her transaction waits while Mallory's, at the price of the
        # dust (0.002), is mined first.
        alice = side.addliquidity("GOLD", 200, "ECX", 2)
        assert_equal(alice["sets_price"], True)
        assert_equal(alice["amount_a"], Decimal(200) - dust["reserve_a"])
        assert_equal(alice["amount_b"], Decimal(2) - dust["reserve_b"])
        side.prioritisetransaction(alice["txid"], 0, -100000000)
        mallory = other.addliquidity("GOLD", 25, "ECX", "0.05")
        self.sync_mempools()
        block = self.mine_txs()
        assert mallory["txid"] in [tx["txid"] for tx in block["tx"]]
        # Mallory's amounts, the dust merged in, are what the pool reopened with: her price.
        pool = side.getpool("GOLD", "ECX")
        assert_equal((pool["reserve_a"], pool["reserve_b"]), (Decimal("25.00"), Decimal("0.05000000")))
        assert_equal(pool["price"], Decimal("0.002"))
        # Alice's deposit would now go in at Mallory's price, for far fewer shares than quoted: it
        # left the mempool, and nothing of hers went to Mallory.
        assert alice["txid"] not in side.getrawmempool()
        side.abandontransaction(alice["txid"])
        assert_equal(self.holding(side, "GOLD")["balance"], gold_before)
        assert_equal([l["pool"] for l in side.listmyassets()["liquidity"]], [])
        # Mallory's shares are worth what she put in, not more.
        mine = other.listmyassets()["liquidity"][0]
        assert mine["value_a"] <= Decimal("25.00") and mine["value_b"] <= Decimal("0.05000000")

        self.log.info("A dead asset is retired: its pools' ECX go to mainchain miners")
        side.reserveasset("DEAD")
        self.mine_txs()
        self.mine_txs()
        side.registerasset("DEAD", 1000)
        self.mine_txs()
        side.addliquidity("DEAD", 500, "ECX", 1)
        self.mine_txs()
        assert_equal(side.getasset("DEAD")["releasable"], False)
        assert_raises_rpc_error(-8, "not dead", side.releaseasset, "DEAD")
        side.removeliquidity("DEAD", "ECX")
        side.fixassetsupply("DEAD", True)
        self.mine_txs()
        side.burnasset("DEAD", self.holding(side, "DEAD")["balance"])
        self.mine_txs()
        dead = side.getasset("DEAD")
        assert_equal(dead["releasable"], True)
        fee = dead["release_fee"]
        assert_greater_than(fee, 0)
        withdrawals = len(side.listwithdrawals())
        dead_number = dead["seq"]
        release = side.releaseasset("DEAD")["txid"]
        assert_raises_rpc_error(-4, "being retired already", side.releaseasset, "DEAD")
        # Anyone's release competes with the one waiting, under the replacement rules: one paying
        # less is refused, one paying more replaces it (a cheap release cannot hold the asset).
        self.sync_mempools()
        data = self.marker_data(other, release)
        def release_paying(fee_rate):
            raw = other.createrawtransaction([], [{"data": data.hex()}])
            raw = other.fundrawtransaction(raw, {"changePosition": 1, "fee_rate": fee_rate})["hex"]
            return other.signrawtransactionwithwallet(raw)["hex"]
        assert_raises_rpc_error(-26, "insufficient fee", other.sendrawtransaction, release_paying(1))
        replacement = other.sendrawtransaction(release_paying(100))
        self.sync_mempools()
        assert release not in side.getrawmempool()
        assert replacement in side.getrawmempool()
        # A package tested as a whole replaces nothing: a release in it that competes with the one
        # waiting is refused, as is any package carrying BitAssets markers, whose rules see the
        # mempool and not the other members of the package.
        raw = other.createrawtransaction([], [{"data": data.hex()}])
        raw = other.fundrawtransaction(raw, {"changePosition": 1, "fee_rate": 200, "lockUnspents": True})["hex"]
        competing = other.signrawtransactionwithwallet(raw)["hex"]
        raw = other.createrawtransaction([], [{other.getnewaddress(): 1}])
        unrelated = other.signrawtransactionwithwallet(other.fundrawtransaction(raw)["hex"])["hex"]
        other.lockunspent(True)
        tested = other.testmempoolaccept([competing, unrelated])
        assert_equal(tested[0]["package-error"], "package-drivechain-tx")
        assert "allowed" not in tested[0] or tested[0]["allowed"] is False
        self.mine_txs()
        assert_raises_rpc_error(-8, "No such asset", side.getasset, "DEAD")
        # Its number names no asset now: never the asset whose name the number is.
        assert_raises_rpc_error(-8, "No asset has the number", side.getasset, dead_number)
        assert "DEAD" not in [a["label"] for a in side.listassets()]
        assert_raises_rpc_error(-8, "no pool", side.getpool, "DEAD", "ECX")
        new = side.listwithdrawals()
        assert_equal(len(new), withdrawals + 1)
        payout = [w for w in new if w["mainchainfee"] == fee - Decimal("0.00000001")]
        assert_equal(len(payout), 1)
        assert_equal(payout[0]["amount"], Decimal("0.00000001"))
        # The name is free: it can be an asset again.
        other.reserveasset("DEAD")
        self.mine_txs()
        self.mine_txs()
        other.registerasset("DEAD", 5)
        self.mine_txs()
        assert_equal(side.getasset("DEAD")["supply"], 5)
        assert_equal([a["label"] for a in side.listassets()].count("DEAD"), 1)

        self.log.info("A copy of a reservation, made from the mempool, does not stop its registration")
        reserved = side.reserveasset("COPIED")
        self.sync_mempools()
        data = self.marker_data(other, reserved["txid"])
        copy = other.createrawtransaction([], [{other.getnewaddress(): 0}, {"data": data.hex()}])
        copy = other.fundrawtransaction(copy, {"changePosition": 2, "fee_rate": 100})["hex"]
        copy_txid = other.sendrawtransaction(other.signrawtransactionwithwallet(copy)["hex"])
        self.mine_txs()
        assert_equal(other.gettransaction(copy_txid)["confirmations"], 1)
        self.mine_txs()
        side.registerasset("COPIED", 7)
        self.mine_txs()
        assert_equal(side.getasset("COPIED")["supply"], 7)

        self.log.info("Undoing blocks undoes the assets")
        tip = side.getbestblockhash()
        pool = side.getpool("GOLD", "ECX")
        gold = side.getasset("GOLD")
        mine = side.listmyassets()
        first = side.getblockhash(gold["registered"])
        side.invalidateblock(first)
        assert_raises_rpc_error(-8, "No such asset", side.getasset, "GOLD")
        side.reconsiderblock(first)
        assert_equal(side.getbestblockhash(), tip)
        assert_equal(side.getpool("GOLD", "ECX"), pool)
        assert_equal(side.getasset("GOLD"), gold)
        assert_equal(side.listmyassets(), mine)
        self.check_in_sync()

        self.log.info("An auction selling ECX takes no bid that buys ECX dust")
        sells_chn = other.createauction("ECX", "0.01", "GOLD", 10, 10, 5)["txid"]
        self.mine_txs()
        assert_raises_rpc_error(-8, "the least ECX paid out is 0.0005", side.bidauction, sells_chn, "0.1")
        assert_equal(side.quotebid(sells_chn, 1)["buys"], Decimal("0.00100000"))
        # Nor one that leaves CHN dust, which nobody could buy.
        assert_raises_rpc_error(-8, "It would leave less than 0.0005 ECX", side.bidauction, sells_chn, "9.99")
        other.collectauction(sells_chn)
        # An auction sells 0.0005 CHN at least; a bid in CHN pays 0.0005 CHN at least.
        assert_raises_rpc_error(-8, "an auction sells 0.0005 ECX at least", other.createauction, "ECX", "0.0004", "GOLD", 10, 10, 5)
        for_chn = side.createauction("GOLD", 1, "ECX", "0.001", "0.001", 5)["txid"]
        self.mine_txs()
        assert_raises_rpc_error(-8, "a bid pays 0.0005 ECX at least", other.bidauction, for_chn, "0.0004")
        side.collectauction(for_chn)
        self.mine_txs()

        self.log.info("What a wallet did, in words")
        activity = other.listassetactivity(100)
        operations = [a["operation"] for a in activity]
        for operation in ["bid", "release", "register", "auction", "collect", "add liquidity", "remove liquidity", "swap", "transfer"]:
            assert operation in operations, operation
        bids = [a["summary"] for a in activity if a["operation"] == "bid"]
        assert "Bid 2.50000000 ECX for at least 25.00 GOLD" in bids, bids
        assert_equal([a["summary"] for a in activity if a["operation"] == "release"], ["Retired a dead asset (its pools' ECX go to mainchain miners)"])
        assert_equal(len(other.listassetactivity(2)), 2)

        self.log.info("sendall leaves the outputs that carry tokens alone")
        held = other.listmyassets()
        assert_greater_than(len(held["assets"]), 0)
        other.sendall([side.getnewaddress()])
        self.mine_txs()
        assert_equal(other.getbalances()["mine"]["trusted"], 0)
        assert_equal(other.listmyassets()["assets"], held["assets"])
        assert_equal(other.listmyassets()["liquidity"], held["liquidity"])

        self.log.info("With -walletbroadcast=0 a BitAssets call keeps its transaction for the user to send, not abandoned")
        self.restart_node(0, extra_args=self.extra_args[0] + ["-walletbroadcast=0"])
        self.connect_nodes(0, 1)
        side = self.nodes[0]
        kept = side.reserveasset("KEPT")["txid"]
        assert kept not in side.getrawmempool()
        wtx = side.gettransaction(kept)
        assert_equal(wtx["confirmations"], 0)
        assert all(not d.get("abandoned", False) for d in wtx["details"])
        # The user sends it: it is a valid reservation.
        side.sendrawtransaction(wtx["hex"])
        assert kept in side.getrawmempool()
        self.mine_txs()
        assert_equal(side.gettransaction(kept)["confirmations"], 1)
        self.restart_node(0)
        self.connect_nodes(0, 1)


if __name__ == "__main__":
    BitAssetsTest(__file__).main()
