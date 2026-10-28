#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Layers: BitAssets' assets on FastBlocks' blocks, three validators against a mainchain node.

  - an asset is reserved, registered and pooled with CHN in BMM blocks (before the validators start);
  - with blocks of stake: swaps both ways, a send, a Dutch auction (made, bid on, collected) in blocks of
    stake; the asset legs at once, the CHN a pool or an auction pays with the next anchor (a block that
    takes the peg actions);
  - finality: the block of stake with a swap goes from soft to final with the anchors;
  - a reorganisation across a BMM block and the blocks of stake after it, each with asset operations,
    takes the assets' state back to what it was, and forward again;
  - the peg balance holds throughout, with the CHN the pools and the auctions hold ("assets").
"""

import os
import time
from decimal import Decimal

from feature_sidechain import ACTIVATION_PERIOD, SidechainTest
from test_framework.fastblocks import assert_peg_balance, stop_bmm
from test_framework.key import compute_xonly_pubkey
from test_framework.util import assert_equal, assert_greater_than, rpc_port

# The slot of the regtest sidechain of the framework (test_framework/fastblocks.py): -sidechainslot, as on regtest the
# slot is an option (slot 7 is that of the main and test networks).
from test_framework.fastblocks import SLOT  # noqa: E402
POS_BLOCK_BIT = 0x10000
KEYS = [bytes([0x11] + [0] * 30 + [i]) for i in (1, 2, 3)]


class LayersTest(SidechainTest):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.main_rpc_port = rpc_port(self.num_nodes)
        validators = ",".join(compute_xonly_pubkey(key)[0].hex() for key in KEYS)
        self.side_args = [
            f"-sidechainslot={SLOT}",
            f"-mainchainrpcport={self.main_rpc_port}",
            "-mainchainrpcuser=main",
            "-mainchainrpcpassword=secret",
            "-mainchainrpcwallet=miner",
            f"-posvalidators={validators}",
        ]
        self.extra_args = [self.side_args] * self.num_nodes
        self.main_process = None

    def run_test(self):
        try:
            self.run_sidechain_test()
        finally:
            self.stop_mainchain()

    def key_file(self, i):
        return os.path.join(self.options.tmpdir, f"validator{i}.key")

    def validator_args(self, i):
        return self.side_args + [f"-validatorkeyfile={self.key_file(i)}", f"-validatorpayout={self.payouts[i]}"]

    def connect_all(self):
        for a, b in ((0, 1), (1, 2), (0, 2)):
            self.connect_nodes(a, b)

    def bmm(self, node=None, expect=True):
        """A BMM block by hand: build it, have the mainchain commit to it, every node on it."""
        node = node or self.nodes[0]
        self.sync_mempools()
        block = node.createbmmblock(self.side_address)
        self.main.createbmmrequest(SLOT, block["hash"], Decimal("0.001"))
        self.mine_main()
        self.wait_until(lambda: all(n.getbestblockhash() == block["hash"] for n in self.nodes))
        return block["hash"]

    def is_pos(self, node, blockhash):
        return bool(node.getblockheader(blockhash)["version"] & POS_BLOCK_BIT)

    def wait_in_sync(self):
        self.wait_until(lambda: len({n.getbestblockhash() for n in self.nodes}) == 1, timeout=60)

    def anchor(self):
        """One more anchor: wait for the merged miner's ticket request, mine it, wait for its acknowledgement."""
        before = [n.getposinfo()["anchors"] for n in self.nodes]
        self.wait_until(lambda: len(self.main.getrawmempool()) > 0, timeout=60)
        self.mine_main()
        self.wait_until(lambda: all(n.getposinfo()["anchors"] > b for n, b in zip(self.nodes, before)), timeout=60)

    def mined(self, node, txid, timeout=30):
        """The block a transaction of the wallet of `node` went into (a block of stake comes every second)."""
        self.wait_until(lambda: node.gettransaction(txid)["confirmations"] > 0, timeout=timeout)
        return node.gettransaction(txid)["blockhash"]

    def holding(self, node, label):
        for a in node.listmyassets()["assets"]:
            if a["label"] == label:
                return a["balance"]
        return Decimal(0)

    def moves(self, node, txid):
        """The rows of listassettransactions of a transaction: (category, label, sign, amount or None, flags), in the node's order."""
        rows = [r for r in node.listassettransactions(1000) if r["txid"] == txid]
        assert rows, f"no row of {txid} in the wallet's asset history"
        flags = lambda r: tuple(f for f in ("at_least", "at_most", "refund", "coin") if r.get(f)) + ((r["via"],) if r.get("via") else ())
        return [(r["category"], r.get("label"), r["sign"], Decimal(str(r["amount"])) if "amount" in r else None, flags(r)) for r in rows]

    def payout_of(self, node, source):
        """The coinbase payouts of the wallet of `node` that listtransactions ties to `source`: (income, amount)."""
        return [(e["income"], Decimal(str(e["amount"]))) for e in node.listtransactions("*", 10000) if e.get("source") == source]

    def paid_in(self, node, blockhash, amount):
        """Whether the coinbase of a block pays `amount` to an address of the wallet of `node`, beyond its first output."""
        block = node.getblock(blockhash, 2)
        for out in block["tx"][0]["vout"][1:]:
            address = out["scriptPubKey"].get("address")
            if address and out["value"] == amount and node.getaddressinfo(address)["ismine"]:
                return True
        return False

    def paid_after(self, node, height, amount):
        """The block above `height` whose coinbase pays `amount` to the wallet of `node` (none yet: None)."""
        for h in range(height + 1, node.getblockcount() + 1):
            blockhash = node.getblockhash(h)
            if self.paid_in(node, blockhash, amount):
                return blockhash
        return None

    def asset_state(self, node):
        """What the chain says of its assets, pools and auctions."""
        info = node.getbitassetsinfo()
        return (
            {k: info[k] for k in ("assets", "pools", "auctions", "reservations")},
            [(p["pool"], p["reserve_a"], p["reserve_b"], p["shares"]) for p in node.listpools()],
            [(a["auction"], a["remaining"], a["proceeds"], a["status"]) for a in node.listauctions()],
            node.getasset("GOLD")["supply"],
        )

    def run_sidechain_test(self):
        main = self.main
        n0, n1, n2 = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = n0.getnewaddress()

        self.log.info("Activate the slot on the mainchain, deposits")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "Flush", "Tokens, assets and a DEX on a fast chain over eCash", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        assert_equal([s["slot"] for s in main.listactivesidechains()], [SLOT])
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 50)
        main.createsidechaindeposit(SLOT, n1.getdepositaddress()["depositaddress"], 20)
        self.mine_main()
        first = self.bmm()
        assert not self.is_pos(n0, first)
        n0.sendmany("", {n0.getnewaddress(): 1 for _ in range(10)})
        n1.sendmany("", {n1.getnewaddress(): 1 for _ in range(10)})
        self.bmm()
        assert_peg_balance(main, self.nodes)

        self.log.info("An asset reserved, registered and pooled with ECX in BMM blocks")
        reserved = n0.reserveasset("GOLD")
        self.bmm()
        self.bmm()
        registered = n0.registerasset("GOLD", 1000000, 2, {"info": "One gram of gold"})
        assert_equal(registered["asset"], reserved["asset"])
        block = self.bmm()
        assert not self.is_pos(n0, block)
        assert_equal(self.holding(n0, "GOLD"), Decimal("1000000.00"))
        n0.addliquidity("GOLD", 10000, "ECX", 5)
        block = self.bmm()
        assert not self.is_pos(n0, block)
        assert_equal(len(n0.listpools()), 1)
        # The pool's CHN are out of the UTXO set, in the state: in the peg balance as "assets".
        assert_equal(assert_peg_balance(main, self.nodes)["assets"], Decimal(5))

        self.log.info("Start the validators: blocks of stake, anchored by the merged miner's tickets")
        self.payouts = [n.getnewaddress() for n in self.nodes]
        for i, key in enumerate(KEYS):
            with open(self.key_file(i), "w") as f:
                f.write("# validator key\n" + key.hex() + "\n")
        for i in range(self.num_nodes):
            self.restart_node(i, extra_args=self.validator_args(i))
        self.connect_all()
        height = n0.getblockcount()
        self.wait_until(lambda: all(n.getblockcount() >= height + 5 for n in self.nodes), timeout=60)
        self.wait_in_sync()
        assert self.is_pos(n0, n0.getbestblockhash())
        ticket_address = n0.getnewaddress()
        n0.setbmm(True, ticket_address, True, Decimal("0.0001"))
        self.anchor()

        self.log.info("Swaps in blocks of stake: the asset at once, the ECX with the next anchor")
        quote = n1.quoteswap("ECX", "0.1", "GOLD")
        swap = n1.swapasset("ECX", "0.1", "GOLD")
        swap_block = self.mined(n1, swap["txid"])
        assert self.is_pos(n1, swap_block)
        # The asset leg is in the block of stake.
        self.wait_until(lambda: self.holding(n1, "GOLD") == quote["amount_out"])
        quote = n0.quoteswap("GOLD", 200, "ECX")
        before = n0.getblockcount()
        swap = n0.swapasset("GOLD", 200, "ECX")
        sold_block = self.mined(n0, swap["txid"])
        assert self.is_pos(n0, sold_block)
        sold_height = n0.getblockheader(sold_block)["height"]
        assert_greater_than(sold_height, before)
        # The CHN: paid by the coinbase of a block that takes the peg actions (an acknowledgement, or a BMM block).
        for _ in range(4):
            if self.paid_after(n0, sold_height - 1, quote["amount_out"]):
                break
            self.anchor()
        paid = self.paid_after(n0, sold_height - 1, quote["amount_out"])
        assert paid is not None
        assert self.is_pos(n0, paid)
        assert n0.getblock(paid)["nonce"] != 0, "a block of stake pays only if it acknowledges an anchor"
        self.wait_in_sync()
        assert_peg_balance(main, self.nodes)

        self.log.info("A send and a Dutch auction in blocks of stake")
        sent = n0.sendasset(n2.getnewaddress(), "GOLD", "12.5")
        assert self.is_pos(n0, self.mined(n0, sent["txid"]))
        self.wait_until(lambda: self.holding(n2, "GOLD") == Decimal("12.50"))
        # 100 GOLD from 10 CHN down to 1 over 600 blocks (ten minutes of 1-second slots on regtest).
        # (Starting 5 blocks on: a block of stake a second, the transaction may wait a slot or two to be mined.)
        made = n0.createauction("GOLD", 100, "ECX", 10, 1, 600, 5)
        assert self.is_pos(n0, self.mined(n0, made["txid"]))
        self.wait_until(lambda: len(n1.listauctions()) == 1 and n1.listauctions()[0]["status"] == "open")
        auction = n1.listauctions()[0]["auction"]
        bid = n1.bidauction(auction, None, True)
        assert self.is_pos(n1, self.mined(n1, bid["txid"]))
        self.wait_until(lambda: n0.getauction(auction)["status"] == "sold out")
        proceeds = n0.getauction(auction)["proceeds"]
        # At the price of the block that took it, which may be later than the one quoted: never more.
        assert 0 < proceeds <= bid["pays"], (proceeds, bid)
        # The auction's CHN are in the state until collected.
        self.wait_in_sync()
        assert_equal(assert_peg_balance(main, self.nodes)["assets"], n0.listpools()[0]["reserve_b"] + proceeds)
        collected = n0.collectauction(auction)
        collect_height = n0.getblockheader(self.mined(n0, collected["txid"]))["height"]
        for _ in range(4):
            if self.paid_after(n0, collect_height - 1, proceeds):
                break
            self.anchor()
        assert self.paid_after(n0, collect_height - 1, proceeds) is not None
        self.wait_in_sync()
        assert_peg_balance(main, self.nodes)

        self.log.info("Finality: the block of stake with a swap, soft, then final with the anchors")
        fresh = n1.swapasset("ECX", "0.05", "GOLD")
        fresh_block = self.mined(n1, fresh["txid"])
        assert_equal(n1.getblock(fresh_block)["finality"]["status"], "soft")
        for _ in range(6):
            if n1.getblock(fresh_block)["finality"]["status"] == "final":
                break
            self.anchor()
        assert_equal(n1.getblock(fresh_block)["finality"]["status"], "final")
        assert_equal(n1.gettransaction(fresh["txid"])["finality"], "final")

        self.log.info("A reorganisation across a BMM block and blocks of stake takes the assets back")
        stop_bmm(main, self.main_address, n0, self.nodes)
        for n in self.nodes:
            n.setstaking(False)
        time.sleep(2)
        self.wait_in_sync()
        # A BMM block carries transactions only once the blocks of stake stalled (stall_seconds of the
        # mainchain's median time without one): mainchain blocks until then.
        deadline = time.time() + 240
        while not n0.getposinfo()["stalled"]:
            assert time.time() < deadline, "the blocks of stake did not stall"
            self.mine_main()
            time.sleep(1)
        state_before = self.asset_state(n0)
        # A BMM block with a swap...
        swap = n1.swapasset("ECX", "0.2", "GOLD")
        bmm_block = self.bmm()
        assert not self.is_pos(n0, bmm_block)
        assert swap["txid"] in n0.getblock(bmm_block)["tx"]
        # ... then blocks of stake with more: a pool share taken out, an asset burned, an auction made.
        for n in self.nodes:
            n.setstaking(True)
        removed = n0.removeliquidity("GOLD", "ECX", 25)
        burned = n0.burnasset("GOLD", 5)
        pos_blocks = {self.mined(n0, removed["txid"]), self.mined(n0, burned["txid"])}
        assert all(self.is_pos(n0, b) for b in pos_blocks)
        auctioned = n0.createauction("GOLD", 10, "ECX", 2, 1, 600, 5)
        pos_blocks.add(self.mined(n0, auctioned["txid"]))
        for n in self.nodes:
            n.setstaking(False)
        time.sleep(2)
        self.wait_in_sync()
        state_after = self.asset_state(n0)
        assert state_after != state_before
        # Taken back: every block from the BMM block up, on one node.
        n0.invalidateblock(bmm_block)
        assert_equal(n0.getbestblockhash(), n0.getblockheader(bmm_block)["previousblockhash"])
        assert_equal(self.asset_state(n0), state_before)
        assert_equal(n0.getasset("GOLD")["supply"], state_before[3])
        # The swap of the BMM block is back in its mempool; what the blocks of stake took is checked again against the
        # tip (the assets' recheck of every transaction after a block is taken back): what spends tokens of outputs
        # no block has any more, or reads what changed, goes.
        assert swap["txid"] in n0.getrawmempool()
        # And forward again: the same state as the other nodes.
        n0.reconsiderblock(bmm_block)
        self.wait_in_sync()
        assert_equal(self.asset_state(n0), state_after)
        assert_equal(self.asset_state(n1), state_after)

        self.log.info("Blocks of stake again; the peg balance with the assets")
        for n in self.nodes:
            n.setstaking(True)
        n0.setbmm(True, ticket_address, True, Decimal("0.0001"))
        self.anchor()
        self.wait_in_sync()
        balance = assert_peg_balance(main, self.nodes)
        assert_greater_than(balance["assets"], 0)
        n0.setbmm(False)
        for n in self.nodes:
            n.setstaking(False)


if __name__ == "__main__":
    LayersTest(__file__).main()
