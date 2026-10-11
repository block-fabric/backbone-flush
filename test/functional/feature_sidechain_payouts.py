#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that the miner fills a block around the bound on the refunds of its transactions.

The transactions of a block ask for MAX_REFUNDS_PER_BLOCK (100) refunds of withdrawals at most (and
give rise to bitassets::MAX_TX_PAYOUTS_PER_BLOCK CHN payouts at most). Transactions that pay well for
many refunds each must not leave the rest of the block empty: the miner leaves out those beyond the
bound and fills the block with the others, rather than dropping them after it was filled.

The mainchain node is a stand-in (test_framework/sidechain_standin.py).
"""

import base64
from decimal import Decimal

from test_framework.messages import CTransaction, CTxOut, tx_from_hex
from test_framework.script import CScript, OP_RETURN
from test_framework.sidechain_standin import StandInMainchain
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, rpc_port

SLOT = 3
WITHDRAWAL_TAG = bytes.fromhex("d8574452")
REFUND_TAG = bytes.fromhex("d952464e")
MIN_WITHDRAWAL = 10000
WITHDRAWAL_FEE = 1000
MAIN_SCRIPT = bytes.fromhex("0014" + "11" * 20)
MAX_REFUNDS_PER_BLOCK = 100
REFUNDS = 45
HEAVY = 6
LIGHT = 20
# Room for about four of the heavy transactions (of about 21 000 weight each); two of them make 90
# refunds, and the third would go beyond.
BLOCK_MAX_WEIGHT = 100000


class SidechainPayoutsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.main_rpc_port = rpc_port(self.num_nodes)
        self.extra_args = [[
            f"-sidechainslot={SLOT}",
            f"-mainchainrpcport={self.main_rpc_port}",
            "-mainchainrpcuser=main",
            "-mainchainrpcpassword=secret",
            "-mainchainrpcwallet=my wallet",
            f"-blockmaxweight={BLOCK_MAX_WEIGHT}",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.main = StandInMainchain(SLOT)
        self.main.serve(self.main_rpc_port)
        self.setup_nodes()

    def run_test(self):
        try:
            self.run_payouts_test()
        finally:
            self.main.shutdown()

    def mine(self):
        """A block the node builds itself, from its mempool, committed to by the next mainchain block."""
        candidate = self.node.createbmmblock(self.address)
        self.main.add_block(bmm=candidate["hash"])
        self.node.syncmainchain()
        assert_equal(self.node.getbestblockhash(), candidate["hash"])
        return candidate["hash"]

    def send(self, outputs, fee_rate):
        """A transaction of the wallet with these outputs, funded by confirmed coins, sent."""
        tx = CTransaction()
        tx.vout = list(outputs)
        funded = self.node.fundrawtransaction(tx.serialize_without_witness().hex(), {"minconf": 1, "fee_rate": fee_rate, "changePosition": len(outputs)})["hex"]
        signed = self.node.signrawtransactionwithwallet(funded)
        assert signed["complete"]
        return self.node.sendrawtransaction(signed["hex"], 0, 1)

    def run_payouts_test(self):
        self.node = node = self.nodes[0]
        self.address = node.getnewaddress()
        for _ in range(3):
            self.main.add_block()
        node.syncmainchain()
        self.main.add_block(deposits=[{"destination": self.address, "amount": "50", "txid": "ab" * 32, "burnindex": 0}])
        node.syncmainchain()
        self.mine()

        self.log.info("A bundle pending, so that new withdrawals wait and can be taken back")
        node.createwithdrawal("mainaddress", 1)
        self.mine()
        self.mine()
        assert_equal(node.getwithdrawalbundle()["status"], "pending")

        self.log.info(f"{HEAVY * REFUNDS} withdrawals, refundable by a key of the wallet; coins of their own for every transaction")
        refund_address = node.getnewaddress(address_type="bech32")
        keyhash = bytes.fromhex(node.getaddressinfo(refund_address)["witness_program"])
        data = WITHDRAWAL_TAG + WITHDRAWAL_FEE.to_bytes(8, "little") + keyhash + MAIN_SCRIPT
        withdrawals = self.send([CTxOut(MIN_WITHDRAWAL + WITHDRAWAL_FEE, CScript([OP_RETURN, data])) for _ in range(HEAVY * REFUNDS)], 1)
        node.sendmany("", {node.getnewaddress(address_type="bech32"): Decimal("0.5") for _ in range(HEAVY + LIGHT)})
        self.mine()
        assert_equal(sum(1 for w in node.listwithdrawals() if w["txid"] == withdrawals and w["status"] == "waiting"), HEAVY * REFUNDS)

        self.log.info(f"Transactions that pay well for {REFUNDS} refunds each, and plain ones that pay little")
        heavy = set()
        for i in range(HEAVY):
            outputs = []
            for n in range(i * REFUNDS, (i + 1) * REFUNDS):
                signature = base64.b64decode(node.signmessage(refund_address, f"Refund withdrawal {withdrawals}:{n}"))
                outputs.append(CTxOut(0, CScript([OP_RETURN, REFUND_TAG + bytes.fromhex(withdrawals)[::-1] + n.to_bytes(4, "little") + signature])))
            heavy.add(self.send(outputs, 50))
        light = set()
        for _ in range(LIGHT):
            light.add(self.send([CTxOut(1000000, bytes.fromhex(node.getaddressinfo(node.getnewaddress(address_type="bech32"))["scriptPubKey"]))], 1))
        assert_equal(set(node.getrawmempool()), heavy | light)
        heavy_weight = sum(tx_from_hex(node.getrawtransaction(t)).get_weight() for t in heavy)
        assert heavy_weight > BLOCK_MAX_WEIGHT, "the heavy transactions alone would fill the block"

        self.log.info("The block takes as many of the heavy ones as the bound allows, and fills the rest with the others")
        block = node.getblock(self.mine(), 2)
        mined = set(t["txid"] for t in block["tx"][1:])
        taken = MAX_REFUNDS_PER_BLOCK // REFUNDS
        assert_equal(len(mined & heavy), taken)
        assert_equal(mined & light, light)
        # The refunds, each paid by the coinbase (the queue had nothing else to pay).
        coinbase = block["tx"][0]
        refund_script = node.getaddressinfo(refund_address)["scriptPubKey"]
        assert_equal(sum(1 for o in coinbase["vout"] if o["scriptPubKey"]["hex"] == refund_script), taken * REFUNDS)

        self.log.info("The rest goes in the next blocks, as many as the bound allows in each")
        block = node.getblock(self.mine(), 1)
        assert_equal(len(set(block["tx"][1:]) & heavy), taken)
        block = node.getblock(self.mine(), 1)
        assert_equal(set(block["tx"][1:]), heavy - mined - set(node.getblock(node.getblockhash(node.getblockcount() - 1), 1)["tx"]))
        assert_equal(node.getrawmempool(), [])
        assert_equal(sum(1 for w in node.listwithdrawals() if w["txid"] == withdrawals), 0)


if __name__ == '__main__':
    SidechainPayoutsTest(__file__).main()
