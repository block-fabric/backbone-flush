#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Releases and order books, on both sides of upgrade v9 (SidechainParams::v9_height):

  - before v9, an asset whose book has no open order is released while an order of it is not settled; the settle of that
    order is refused (bad-ba-no-book): by the mempool when it comes after the release, in a block that has both, and when
    it waits in a mempool as the release is mined (taken out of it). Every node stays on one tip;
  - getasset and the wallet's releaseasset wait for the unsettled orders on both sides of the height;
  - from v9 the release itself waits for the unsettled orders of the asset's books (bad-ba-release-alive), and is valid
    once they are settled.
"""

from decimal import Decimal

from feature_layers import LayersTest
from test_framework.messages import CBlock, from_hex, ser_compact_size, tx_from_hex, uint256_from_str
from test_framework.blocktools import WITNESS_COMMITMENT_HEADER, get_witness_script
from test_framework.script import OP_RETURN
from test_framework.util import assert_equal, assert_raises_rpc_error

DEX_HEIGHT = 10
CURVES_HEIGHT = 15
V9_HEIGHT = 40
RELEASE = 12
NAMES = ("DEADA", "DEADB", "DEADC", "DEADD")


class LayersReleaseBookTest(LayersTest):
    def set_test_params(self):
        super().set_test_params()
        self.side_args = self.side_args + [f"-testactivationheight=dex@{DEX_HEIGHT}", f"-testactivationheight=dexcurves@{CURVES_HEIGHT}",
                                           f"-testactivationheight=v9@{V9_HEIGHT}"]
        # The third node's wallet makes transactions without sending them: the test sends them when it wants.
        self.extra_args = [self.side_args, self.side_args, self.side_args + ["-walletbroadcast=0", "-spendzeroconfchange=0"]]

    def run_test(self):
        super().run_test()

    def bmm_to(self, height):
        while self.nodes[0].getblockcount() < height:
            self.bmm()

    def hex_of(self, node, result):
        return node.gettransaction(result["txid"])["hex"]

    def send(self, node, result):
        """Send a transaction the wallet of `node` made without sending it."""
        return node.sendrawtransaction(self.hex_of(node, result))

    def release_hex(self, node, name):
        """A release of `name` (ReleaseAsset), funded by the wallet of `node`, not sent."""
        import hashlib
        data = b"BAST" + bytes([0, RELEASE]) + hashlib.sha256(name.encode()).digest() + ser_compact_size(0)
        raw = node.createrawtransaction([], [{"data": data.hex()}])
        raw = node.fundrawtransaction(raw, {"changePosition": 1, "fee_rate": 10})["hex"]
        return node.signrawtransactionwithwallet(raw)["hex"]

    def assert_alive_on(self, tip):
        self.wait_until(lambda: all(n.getbestblockhash() == tip for n in self.nodes))

    def recommit(self, block):
        """The witness commitment of a block changed by the test, in its place in the coinbase."""
        coinbase = block.vtx[0]
        nonce = 0
        if coinbase.wit.vtxinwit and coinbase.wit.vtxinwit[0].scriptWitness.stack:
            nonce = uint256_from_str(coinbase.wit.vtxinwit[0].scriptWitness.stack[0])
        script = get_witness_script(block.calc_witness_merkle_root(), nonce)
        found = False
        for out in coinbase.vout:
            if bytes(out.scriptPubKey).startswith(bytes([OP_RETURN, 0x24]) + WITNESS_COMMITMENT_HEADER):
                out.scriptPubKey = script
                found = True
        assert found
        block.hashMerkleRoot = block.calc_merkle_root()
        block.solve()

    def run_sidechain_test(self):
        main = self.main
        n0, n1, n2 = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = n0.getnewaddress()
        from feature_sidechain import ACTIVATION_PERIOD
        from test_framework.fastblocks import SLOT

        self.log.info("Setup: the slot, deposits; DEADA to DEADD held by the third node, each with its supply fixed")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "Layers", "Assets on a fast chain", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        for n, amount in ((n0, 60), (n1, 20), (n2, 20)):
            main.createsidechaindeposit(SLOT, n.getdepositaddress()["depositaddress"], amount)
        self.mine_main()
        self.bmm()
        for n in (n0, n1):
            n.sendmany("", {n.getnewaddress(): 1 for _ in range(10)})
        self.send(n2, {"txid": n2.sendmany("", {n2.getnewaddress(): 1 for _ in range(10)})})
        self.bmm()
        for name in NAMES + ("USDX",):
            n0.reserveasset(name)
        self.bmm_to(n0.getblockcount() + 3)
        for name in NAMES:
            n0.registerasset(name, 1000, 0)
        n0.registerasset("USDX", 1000000, 0)
        self.bmm()
        for name in NAMES:
            n0.sendasset(n2.getnewaddress(), name, 1000)
            n0.fixassetsupply(name, True)
        self.bmm()
        self.bmm_to(CURVES_HEIGHT)

        self.log.info("Each sold whole in an order that expires at once: its book has no open order, the order is not settled")
        orders = {}
        for name in NAMES:
            placed = n2.placeorder(name, 1000, "USDX", 1, 1)
            self.send(n2, placed)
            orders[name] = placed
        self.bmm()
        self.bmm()
        self.bmm()
        for name in NAMES:
            book = n0.getbook(name, "USDX")
            assert_equal(book["open_orders"], 0)
            # Whoever asks without a height (getasset, the wallet) waits for the order, on either side of v9.
            assert_equal(n0.getasset(name)["releasable"], False)
            assert_raises_rpc_error(-8, "not dead", n1.releaseasset, name)
        # The settles, made now and not sent.
        settles = {name: self.hex_of(n2, n2.settleorder(orders[name]["order"])) for name in NAMES[:3]}
        assert n0.getblockcount() + 4 < V9_HEIGHT

        self.log.info("Before v9: the release is valid; the settle after it is refused by the mempool")
        n1.sendrawtransaction(self.release_hex(n1, "DEADA"))
        tip = self.bmm()
        assert_raises_rpc_error(None, "", n0.getasset, "DEADA")
        for n in self.nodes:
            result = n.testmempoolaccept([settles["DEADA"]])[0]
            assert_equal(result["allowed"], False)
            assert_equal(result["reject-reason"], "bad-ba-no-book")
        assert_raises_rpc_error(-26, "bad-ba-no-book", n2.sendrawtransaction, settles["DEADA"])
        self.assert_alive_on(tip)

        self.log.info("Before v9: a block with the release and then the settle is refused")
        release_b = self.release_hex(n1, "DEADB")
        assert_raises_rpc_error(-25, "TestBlockValidity failed: bad-ba-no-book", self.generateblock, n0, self.side_address, [release_b, settles["DEADB"]], submit=False, sync_fun=self.no_op)
        block = from_hex(CBlock(), self.generateblock(n0, self.side_address, [release_b], submit=False, sync_fun=self.no_op)["hex"])
        block.vtx.append(tx_from_hex(settles["DEADB"]))
        self.recommit(block)
        main.createbmmrequest(SLOT, block.hash_hex, Decimal("0.001"))
        self.mine_main()
        assert_equal(n0.submitblock(block.serialize().hex()), "bad-ba-no-book")
        assert_equal(n0.getbestblockhash(), tip)
        n1.sendrawtransaction(release_b)
        tip = self.bmm()
        assert_raises_rpc_error(-26, "bad-ba-no-book", n0.sendrawtransaction, settles["DEADB"])

        self.log.info("Before v9: a settle waiting in a mempool as the release is mined is taken out of it")
        for a in (0, 1):
            self.disconnect_nodes(a, 2)
        n2.sendrawtransaction(settles["DEADC"])
        release_c = n1.sendrawtransaction(self.release_hex(n1, "DEADC"))
        self.wait_until(lambda: release_c in n0.getrawmempool())
        made = n0.createbmmblock(self.side_address)
        main.createbmmrequest(SLOT, made["hash"], Decimal("0.001"))
        self.mine_main()
        self.wait_until(lambda: n0.getbestblockhash() == made["hash"] and n1.getbestblockhash() == made["hash"])
        assert release_c in n0.getblock(made["hash"])["tx"]
        self.connect_all()
        self.assert_alive_on(made["hash"])
        self.wait_until(lambda: tx_from_hex(settles["DEADC"]).txid_hex not in n2.getrawmempool())
        assert_raises_rpc_error(None, "", n2.getasset, "DEADC")
        for n in self.nodes:
            assert_equal(n.testmempoolaccept([settles["DEADC"]])[0]["reject-reason"], "bad-ba-no-book")

        self.log.info("From v9: the release waits for the unsettled orders of the asset's books")
        self.bmm_to(V9_HEIGHT)
        release_d = self.release_hex(n1, "DEADD")
        result = n1.testmempoolaccept([release_d])[0]
        assert_equal(result["allowed"], False)
        assert_equal(result["reject-reason"], "bad-ba-release-alive")
        assert_raises_rpc_error(-26, "bad-ba-release-alive", n1.sendrawtransaction, release_d)
        settled = n2.settleorder(orders["DEADD"]["order"])
        self.send(n2, settled)
        self.bmm()
        assert_equal(n0.getasset("DEADD")["releasable"], False)
        self.send(n2, n2.burnasset("DEADD", 1000))
        self.bmm()
        assert_equal(n0.getasset("DEADD")["releasable"], True)
        n1.sendrawtransaction(self.release_hex(n1, "DEADD"))
        tip = self.bmm()
        assert_raises_rpc_error(None, "", n0.getasset, "DEADD")
        self.assert_alive_on(tip)


if __name__ == '__main__':
    LayersReleaseBookTest(__file__).main()
