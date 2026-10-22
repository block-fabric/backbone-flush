#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Layers' DEX (doc/layers.md, "The DEX"), from -testactivationheight=dex@<height>:

  - before its height: swaps trade one after another (two of a block at two prices), no routes, no TWAP;
  - routes of 2 and 4 hops, each giving what quoteroute said;
  - batch clearing in a BMM block: two orders of the same side at one price; opposing orders at the
    uniform price; an order whose min_out the batch misses is refunded (its fee paid), the other fills;
  - the TWAP of a pool over windows: constant price after the last change, and a reorganisation that
    takes a trade back takes the accumulators back with it;
  - oracles: the key is the oracle (names are labels): one with the wallet's key, one with an outside
    key (its proof of possession and attestation signed here, over the documented messages); a
    squatter of the same name with another key changes nothing for the honest one; nobody registers
    another's key without its proof; contracts in CHN and in an asset, settled to either outcome, and one refunded
    after its expiry;
  - batch clearing in a block of stake: two opposing orders, one block, the uniform price;
  - the peg balance throughout (the oracles' fee is a withdrawal, the contracts' CHN in "assets");
  - (the wallet) a plain send while an asset transaction of the same wallet is unconfirmed spends its
    change, and the mempool takes it (its token outputs are no ephemeral dust).
"""

import hashlib
import struct
import time
from decimal import Decimal

from feature_layers import KEYS, LayersTest
from test_framework.key import compute_xonly_pubkey, sign_schnorr
from test_framework.messages import ser_string
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error

DEX_HEIGHT = 14
INT64_MIN = -(2**63)
INT64_MAX = 2**63 - 1
COIN = 10**8


def units(amount, decimals):
    return int(Decimal(amount) * 10**decimals)


def uniform(r_x, r_y, pay_x, pay_y):
    """The batch's price (bitassets::dex::ClearPool): what each order paying X gets of Y, and each paying Y gets of X."""
    num = r_y * 1000 + sum(pay_y) * 997
    den = r_x * 1000 + sum(pay_x) * 997
    return [a * 997 * num // (den * 1000) for a in pay_x], [b * 997 * den // (num * 1000) for b in pay_y]


def registration_hash(genesis_hex, name, pubkey_hex):
    """What an oracle's key signs to register (its proof of possession): SHA256 of the serialized tag, the chain's first block, the name, the key."""
    return hashlib.sha256(ser_string(b"Layers oracle registration") + bytes.fromhex(genesis_hex)[::-1] + ser_string(name.encode()) + bytes.fromhex(pubkey_hex)).digest()


def attestation_hash(oracle_hex, event_hex, value, height):
    """What an oracle signs: SHA256 of the serialized tag, oracle id, event id (as stored), value (int64) and height (int32)."""
    data = ser_string(b"Layers oracle attestation") + bytes.fromhex(oracle_hex)[::-1] + bytes.fromhex(event_hex)[::-1]
    data += struct.pack("<q", value) + struct.pack("<i", height)
    return hashlib.sha256(data).digest()



def taken(node, amount, fee_millionths):
    """The security and protocol parts of the swap fee on `amount` (units) at `fee_millionths`: what leaves the pool (doc/layers.md "Fees of the DEX")."""
    parts = node.getdexinfo()["swap_fee_parts"]
    fee = amount * fee_millionths // 1000000
    out = 0
    for key in ("security", "protocol"):
        share = int(Decimal(str(parts[key])) * 10000)
        out += fee * share // 1000000
    return out

class LayersDexTest(LayersTest):
    def set_test_params(self):
        super().set_test_params()
        # The fee split too (pos/feesplit.h): the security parts of the DEX's fees in the chain's coin are split with the block's fees.
        self.side_args = self.side_args + [f"-testactivationheight=dex@{DEX_HEIGHT}", "-testactivationheight=feesplit@1"]
        self.extra_args = [self.side_args] * self.num_nodes

    def run_test(self):
        super().run_test()

    def reserves(self, node, a, b):
        pool = node.getpool(a, b)
        return units(pool["reserve_a"], 2 if a != "ECX" else 8), units(pool["reserve_b"], 2 if b != "ECX" else 8)

    def gold(self, node):
        return units(self.holding(node, "GOLD"), 2)

    def bmm_to(self, height):
        while self.nodes[0].getblockcount() < height:
            self.bmm()

    def run_sidechain_test(self):
        main = self.main
        n0, n1, n2 = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = n0.getnewaddress()
        from feature_sidechain import ACTIVATION_PERIOD
        from test_framework.fastblocks import SLOT, assert_peg_balance

        self.log.info("Setup: the slot, deposits, four assets, four pools (BMM blocks)")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "Flush", "Tokens, assets and a DEX on a fast chain over eCash", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 60)
        main.createsidechaindeposit(SLOT, n1.getdepositaddress()["depositaddress"], 20)
        main.createsidechaindeposit(SLOT, n2.getdepositaddress()["depositaddress"], 20)
        self.mine_main()
        self.bmm()
        for n in self.nodes:
            n.sendmany("", {n.getnewaddress(): 1 for _ in range(10)})
        self.bmm()
        assert_equal(n0.getdexinfo()["dex_height"], DEX_HEIGHT)
        assert not n0.getdexinfo()["active"]
        names = ["GOLD", "SILVER", "OIL", "WOOD"]
        for name in names:
            n0.reserveasset(name)
        self.bmm_to(n0.getblockcount() + 3)
        for name in names:
            n0.registerasset(name, 1000000, 2)
        self.bmm()
        n0.addliquidity("GOLD", 10000, "ECX", 20)
        n0.addliquidity("SILVER", 10000, "ECX", 10)
        n0.addliquidity("OIL", 5000, "WOOD", 5000)
        self.bmm()
        n0.addliquidity("SILVER", 5000, "OIL", 5000)
        n0.sendasset(n1.getnewaddress(), "GOLD", 1000)
        self.bmm()
        n0.sendasset(n2.getnewaddress(), "GOLD", 1000)
        self.bmm()
        assert_equal(len(n0.listpools()), 4)
        assert_peg_balance(main, self.nodes)

        self.log.info("Before the DEX's height: two swaps of a block trade one after the other; no routes, no TWAP")
        assert_greater_than(DEX_HEIGHT - 1, n0.getblockcount() + 1)
        assert_raises_rpc_error(-1, "do not apply yet", n0.swaproute, ["GOLD", "ECX", "SILVER"], 10)
        pool = n0.getpool("GOLD", "ECX")["pool"]
        assert_raises_rpc_error(-1, "no observations", n0.getpooltwap, pool, None, 60)
        g1, g2 = self.gold(n1), self.gold(n2)
        n1.swapasset("ECX", "0.2", "GOLD", 5)
        n2.swapasset("ECX", "0.2", "GOLD", 5)
        self.bmm()
        assert_equal(n0.getrawmempool(), [])
        got1, got2 = self.gold(n1) - g1, self.gold(n2) - g2
        assert got1 > 0 and got2 > 0 and got1 != got2, "one after the other: the second at the price the first left"
        self.bmm_to(DEX_HEIGHT - 1)
        assert n0.getdexinfo()["active"]

        self.log.info("Batch: two orders of the same side, one block, one price")
        r_g, r_c = self.reserves(n0, "GOLD", "ECX")
        g1, g2 = self.gold(n1), self.gold(n2)
        n1.swapasset("ECX", "0.2", "GOLD", 5)
        n2.swapasset("ECX", "0.2", "GOLD", 5)
        block = self.bmm()
        assert_equal(n0.getblockheader(block)["height"], DEX_HEIGHT)
        assert_equal(len(n0.getblock(block)["tx"]), 3)
        (e1, e2), _ = uniform(r_c, r_g, [units("0.2", 8)] * 2, [])
        assert_equal(self.gold(n1) - g1, e1)
        assert_equal(self.gold(n2) - g2, e2)
        assert_equal(e1, e2)

        self.log.info("Routes: two hops, four hops, each what quoteroute said")
        quote = n0.quoteroute("GOLD", 10, "SILVER")
        assert_equal(quote["path"], ["GOLD", "ECX", "SILVER"])
        before = self.holding(n0, "SILVER")
        n0.swaproute(quote["path"], 10)
        self.bmm()
        assert_equal(self.holding(n0, "SILVER") - before, quote["amount_out"])
        quote = n0.quoteroute("GOLD", 10, "WOOD")
        assert_equal(quote["path"], ["GOLD", "ECX", "SILVER", "OIL", "WOOD"])
        assert_equal(len(quote["routes"][0]["hops"]), 4)
        assert_equal(n0.quoteroute("GOLD", 10, "WOOD", 3)["routes"], [])
        before = self.holding(n0, "WOOD")
        n0.swaproute(quote["path"], 10)
        self.bmm()
        assert_equal(self.holding(n0, "WOOD") - before, quote["amount_out"])

        self.log.info("Batch: opposing orders at the uniform price (BMM block)")
        r_g, r_c = self.reserves(n0, "GOLD", "ECX")
        alone_sell = n1.quoteswap("GOLD", 50, "ECX")["amount_out"]
        alone_buy = n2.quoteswap("ECX", "0.2", "GOLD")["amount_out"]
        g2 = self.gold(n2)
        n1.swapasset("GOLD", 50, "ECX")
        n2.swapasset("ECX", "0.2", "GOLD")
        block = self.bmm()
        (chn_out,), (gold_out,) = uniform(r_g, r_c, [5000], [units("0.2", 8)])
        assert_equal(self.gold(n2) - g2, gold_out)
        assert self.paid_in(n1, block, Decimal(chn_out) / COIN)
        # Each better than alone: the other's flow nets against it.
        assert_greater_than(Decimal(chn_out) / COIN, alone_sell)
        assert_greater_than(Decimal(gold_out) / 100, alone_buy)
        # What came in and went out, less the security and protocol parts of each side's 0.3 % fee (which left the pool:
        # the coin's security part paid with the block's fees, GOLD's held by the treasury).
        assert_equal(self.reserves(n0, "GOLD", "ECX"), (r_g + 5000 - gold_out - taken(n0, 5000, 3000), r_c + units("0.2", 8) - chn_out - taken(n0, units("0.2", 8), 3000)))
        assert_greater_than(n0.gettreasury()["security_paid"], 0)

        self.log.info("Batch: an order whose min_out the batch misses is refunded; the other fills as alone")
        alone = n1.quoteswap("GOLD", 50, "ECX")["amount_out"]
        g1 = self.gold(n1)
        tight = n1.swapasset("GOLD", 50, "ECX", 0)
        n2.swapasset("GOLD", 50, "ECX", 1)
        block = self.bmm()
        assert tight["txid"] in n0.getblock(block)["tx"]
        assert_equal(self.gold(n1), g1)  # its GOLD back
        assert not self.paid_in(n1, block, alone)
        assert self.paid_in(n2, block, alone)
        assert_peg_balance(main, self.nodes)

        self.log.info("The wallet: a send spends the change of an unconfirmed asset transaction, and the mempool takes it")
        n0.createwallet("solo")
        solo = n0.get_wallet_rpc("solo")
        n0.get_wallet_rpc(self.default_wallet_name).sendtoaddress(solo.getnewaddress(), 2)
        n0.get_wallet_rpc(self.default_wallet_name).sendasset(solo.getnewaddress(), "GOLD", 5)
        self.bmm()
        asset_tx = solo.sendasset(n1.getnewaddress(), "GOLD", 1)["txid"]
        sent = solo.sendtoaddress(n1.getnewaddress(), 1)
        assert asset_tx in n0.getrawmempool()
        assert sent in n0.getrawmempool(), "the send was kept out of the mempool"
        assert asset_tx in [i["txid"] for i in n0.getrawtransaction(sent, True)["vin"]]
        self.bmm()
        n0.unloadwallet("solo")

        self.log.info("TWAP: windows, and a reorganisation takes the accumulators back")
        time.sleep(2)
        self.bmm()
        # After its last change the pool's price held: the average of the last second is the spot price.
        twap = n0.getpooltwap(pool, None, 1)
        assert abs(twap["twap_a"] / twap["spot_a"] - 1) < Decimal("1e-9"), twap
        whole = n0.getpooltwap(pool, DEX_HEIGHT)
        assert_greater_than(whole["seconds"], 0)
        assert_greater_than(whole["observations"], 4)
        assert whole["twap_a"] != whole["spot_a"]
        assert_raises_rpc_error(-8, "Name the window", n0.getpooltwap, pool)
        before = n0.getpooltwap(pool, DEX_HEIGHT)
        n1.swapasset("GOLD", 100, "ECX")
        time.sleep(2)
        swap_block = self.bmm()
        time.sleep(2)
        self.bmm()
        after = n0.getpooltwap(pool, DEX_HEIGHT)
        assert after["cumulative_a"] != before["cumulative_a"]
        tip = n0.getbestblockhash()
        n0.invalidateblock(swap_block)
        assert_equal(n0.getpooltwap(pool, DEX_HEIGHT), before)
        n0.reconsiderblock(swap_block)
        assert_equal(n0.getbestblockhash(), tip)
        assert_equal(n0.getpooltwap(pool, DEX_HEIGHT), after)
        assert_equal(n1.getpooltwap(pool, DEX_HEIGHT), after)

        self.log.info("Oracles: registered (a fee to mainchain miners), attested by the wallet's key and by an outside one")
        prices = n0.registeroracle("prices")["oracle"]
        outside_key = bytes([0x22] * 32)
        outside_pub = compute_xonly_pubkey(outside_key)[0].hex()
        genesis = n0.getblockhash(0)
        # Another's key, without its proof, or with a proof for another name: refused.
        assert_raises_rpc_error(-4, "does not have the key", n1.registeroracle, "outside", outside_pub)
        bad_proof = sign_schnorr(outside_key, registration_hash(genesis, "other", outside_pub))
        assert_raises_rpc_error(-8, "not the key's", n1.registeroracle, "outside", outside_pub, bad_proof.hex())
        proof = sign_schnorr(outside_key, registration_hash(genesis, "outside", outside_pub))
        outside = n0.registeroracle("outside", outside_pub, proof.hex())["oracle"]
        # A squatter of the name "prices" with its own key: another oracle, the honest one untouched.
        squatter = n1.registeroracle("prices")["oracle"]
        assert squatter != prices
        self.bmm()
        oracles = {o["oracle"]: o for o in n1.listoracles()}
        assert_equal(sorted(o["name"] for o in oracles.values()), ["outside", "prices", "prices"])
        assert_equal(oracles[outside]["pubkey"], outside_pub)
        assert_raises_rpc_error(-8, "is registered", n0.registeroracle, "outside", outside_pub, proof.hex())
        # By its key as well as by its id.
        assert_equal(n1.getoraclevalue(outside_pub, "E2")["oracle"], outside)
        assert_peg_balance(main, self.nodes)
        height = n0.getblockcount()
        to_low, to_high, refund = n1.getnewaddress(), n2.getnewaddress(), n0.getnewaddress()
        outcomes = [{"min": INT64_MIN, "max": 99, "address": to_low}, {"min": 100, "max": INT64_MAX, "address": to_high}]
        c_chn = n0.createoraclecontract(prices, "E1", "ECX", 1, outcomes, refund, height + 100)["txid"]
        c_gold = n0.createoraclecontract(prices, "E1", "GOLD", 20, outcomes, refund, height + 100)["txid"]
        self.bmm()
        c_outside = n0.createoraclecontract(outside, "E2", "ECX", "0.5", outcomes, refund, height + 100)["txid"]
        c_expiring = n0.createoraclecontract(prices, "E3", "ECX", "0.5", outcomes, refund, height + 3)["txid"]
        self.bmm()
        assert_equal(n2.getoraclecontract(c_chn)["status"], "pending")
        assert_raises_rpc_error(-8, "waits", n2.settleoraclecontract, c_chn)
        # The contracts' CHN are in the state, with the pools': the peg balance's "assets".
        pools_chn = sum(p["reserve_a"] if p["asset_a"] == "ECX" else p["reserve_b"] if p["asset_b"] == "ECX" else 0 for p in n0.listpools())
        # The coin the treasury holds (the protocol's parts of the fees) counts too.
        treasury_chn = sum(b["balance"] for b in n0.gettreasury()["balances"] if b["asset_id"] == "00" * 32)
        assert_equal(assert_peg_balance(main, self.nodes)["assets"], pools_chn + 2 + treasury_chn)

        n0.publishoracle(prices, "E1", 150)
        # The squatter's key cannot attest for the honest oracle; its own attestation is its own oracle's.
        assert_raises_rpc_error(-4, "does not have the oracle's key", n1.publishoracle, prices, "E1", 1)
        n1.publishoracle(squatter, "E1", 1)
        event = n0.getoraclevalue(outside, "E2")["event"]
        oracle = outside
        signed_at = n0.getblockcount() + 1
        signature = sign_schnorr(outside_key, attestation_hash(oracle, event, 50, signed_at))
        n0.publishoracle(outside, "E2", 50, signed_at, signature.hex())
        # A signature over another value is refused.
        assert_raises_rpc_error(-8, "not the oracle's", n0.publishoracle, outside, "E9", 50, signed_at, signature.hex())
        self.bmm()
        assert_equal(n1.getoraclevalue(prices, "E1")["value"], 150)
        assert_equal(n1.getoraclevalue(squatter, "E1")["value"], 1)
        assert_equal(n1.getoraclevalue(outside, "E2")["value"], 50)
        assert_equal(n1.getoraclecontract(c_chn)["status"], "settleable")
        assert_equal(n1.getoraclecontract(c_chn)["winner"], to_high)

        self.log.info("Contracts settled, by anyone: 150 pays the high outcome, 50 the low one, the expired one its refund")
        g2 = self.gold(n2)
        n1.settleoraclecontract(c_chn)
        self.bmm()
        n1.settleoraclecontract(c_gold)
        n2.settleoraclecontract(c_outside)
        n2.settleoraclecontract(c_expiring)
        block = self.bmm()
        assert_equal(self.gold(n2) - g2, 2000)
        assert self.paid_in(n1, block, Decimal("0.5"))  # the outside oracle's 50: low
        assert self.paid_in(n0, block, Decimal("0.5"))  # the expired one: refund
        chn_block = n0.getblockhash(n0.getblockcount() - 1)
        assert self.paid_in(n2, chn_block, Decimal(1))
        for c in (c_chn, c_gold, c_outside, c_expiring):
            assert_equal(n0.getoraclecontract(c)["status"], "settled")
        assert_equal(n0.getoraclecontract(c_expiring)["winner"], refund)
        assert_raises_rpc_error(-8, "is settled", n1.settleoraclecontract, c_chn)
        self.check_states()
        assert_peg_balance(main, self.nodes)

        self.log.info("Batch in a block of stake: opposing orders, one block, the uniform price")
        self.payouts = [n.getnewaddress() for n in self.nodes]
        for i, key in enumerate(KEYS):
            with open(self.key_file(i), "w") as f:
                f.write("# validator key\n" + key.hex() + "\n")
        for i in range(self.num_nodes):
            self.restart_node(i, extra_args=self.validator_args(i))
        self.connect_all()
        height = n0.getblockcount()
        self.wait_until(lambda: all(n.getblockcount() >= height + 3 for n in self.nodes), timeout=60)
        self.wait_in_sync()
        for n in self.nodes:
            n.setstaking(False)
        time.sleep(2)
        self.wait_in_sync()
        sent_s = n0.sendasset(n1.getnewaddress(), "SILVER", 100)["txid"]
        sent_o = n0.sendasset(n2.getnewaddress(), "OIL", 100)["txid"]
        for n in self.nodes:
            n.setstaking(True)
        self.mined(n0, sent_s)
        self.mined(n0, sent_o)
        self.wait_until(lambda: self.holding(n1, "SILVER") > 0 and self.holding(n2, "OIL") > 0, timeout=60)
        for n in self.nodes:
            n.setstaking(False)
        time.sleep(2)
        self.wait_in_sync()
        r_s, r_o = self.reserves(n0, "SILVER", "OIL")
        o1, s2 = self.holding(n1, "OIL"), self.holding(n2, "SILVER")
        a = n1.swapasset("SILVER", 40, "OIL")["txid"]
        b = n2.swapasset("OIL", 30, "SILVER")["txid"]
        self.sync_mempools()
        for n in self.nodes:
            n.setstaking(True)
        block_a, block_b = self.mined(n1, a), self.mined(n2, b)
        assert_equal(block_a, block_b)
        assert self.is_pos(n0, block_a)
        (oil_out,), (silver_out,) = uniform(r_s, r_o, [4000], [3000])
        self.wait_until(lambda: units(self.holding(n1, "OIL") - o1, 2) == oil_out)
        self.wait_until(lambda: units(self.holding(n2, "SILVER") - s2, 2) == silver_out)
        for n in self.nodes:
            n.setstaking(False)
        time.sleep(2)
        self.wait_in_sync()
        assert_equal(self.reserves(n0, "SILVER", "OIL"), (r_s + 4000 - silver_out - taken(n0, 4000, 3000), r_o + 3000 - oil_out - taken(n0, 3000, 3000)))
        self.check_states()
        assert_peg_balance(main, self.nodes)

        self.log.info("A swap paying the coin in a block of stake: the security part of its fee joins the block's fees (pot and pool)")
        paid_before = n0.gettreasury()["security_paid"]
        for n in self.nodes:
            n.setstaking(True)
        c = n1.swapasset("ECX", "0.5", "GOLD")["txid"]
        block_c = self.mined(n1, c)
        assert self.is_pos(n0, block_c)
        self.wait_until(lambda: n0.gettreasury()["security_paid"] > paid_before)
        for n in self.nodes:
            n.setstaking(False)
        time.sleep(2)
        self.wait_in_sync()
        # The coinbase of that block: its pool output holds the validators' part of the fees and of the security part.
        coinbase = n0.getblock(block_c, 2)["tx"][0]
        assert any(o["scriptPubKey"]["hex"].startswith("6a04de56504c") for o in coinbase["vout"]), coinbase["vout"]
        self.check_states()
        assert_peg_balance(main, self.nodes)

    def check_states(self):
        self.wait_in_sync()
        states = [n.getsidechainstate() for n in self.nodes]
        assert_equal(len({(s["bestblock"], s["hash"]) for s in states}), 1)


if __name__ == "__main__":
    LayersDexTest(__file__).main()
