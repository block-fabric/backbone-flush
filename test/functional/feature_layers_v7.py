#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Upgrade v7 from v7_height H (slot 99's upgrade v7): instant DEX payouts and the launch fee split; a node without it.

  - before H, in blocks of stake: the ECX a swap pays waits for a block that takes the peg actions (an anchor);
  - from H: the ECX a swap pays is in the coinbase of the block of stake with the swap itself, anchor or not; the peg
    balance holds;
  - a node without the rule (FLUSHD_OLD: a flushd of v6; else this one with v7 never) follows until the first block whose
    coinbase pays a swap's ECX without the peg actions, and forks off there;
  - from H a launch's fee is 25 % security, 75 % split as it says: an address of the split is paid by the coinbase (no
    claim), in the block that traded, and getlaunch shows the split with what each recipient was paid.
"""

from decimal import Decimal
import os

from feature_layers import KEYS, LayersTest
from feature_sidechain import ACTIVATION_PERIOD
from test_framework.fastblocks import SLOT, assert_peg_balance
from test_framework.key import compute_xonly_pubkey
from test_framework.util import assert_equal, assert_greater_than, rpc_port

OLD = 3


class LayersV7Test(LayersTest):
    def set_test_params(self):
        self.num_nodes = 4
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
            "-testactivationheight=dex@10", "-testactivationheight=dexcurves@15", "-testactivationheight=feeflow@15",
        ]
        self.extra_args = [self.side_args] * self.num_nodes
        self.main_process = None

    def run_test(self):
        super().run_test()

    def connect_all(self):
        for a, b in ((0, 1), (1, 2), (0, 2), (OLD, 0), (OLD, 1)):
            self.connect_nodes(a, b)

    def bmm(self, node=None, expect=True):
        node = node or self.nodes[0]
        self.sync_mempools(self.nodes[:3])
        block = node.createbmmblock(self.side_address)
        self.main.createbmmrequest(SLOT, block["hash"], Decimal("0.001"))
        self.mine_main()
        self.wait_until(lambda: all(n.getbestblockhash() == block["hash"] for n in self.nodes))
        return block["hash"]

    def wait_in_sync(self):
        self.wait_until(lambda: len({n.getbestblockhash() for n in self.nodes[:3]}) == 1, timeout=60)

    def anchor(self):
        before = [n.getposinfo()["anchors"] for n in self.nodes[:3]]
        self.wait_until(lambda: len(self.main.getrawmempool()) > 0, timeout=60)
        self.mine_main()
        self.wait_until(lambda: all(n.getposinfo()["anchors"] > b for n, b in zip(self.nodes[:3], before)), timeout=60)

    def run_sidechain_test(self):
        main = self.main
        n0, n1, n2, old = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = n0.getnewaddress()

        self.log.info("Setup: the slot, deposits, a pool with ECX in BMM blocks")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "Flush", "Tokens, assets and a DEX on a fast chain over eCash", "11" * 32, "22" * 20)
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 50)
        main.createsidechaindeposit(SLOT, n1.getdepositaddress()["depositaddress"], 20)
        self.mine_main()
        self.bmm()
        n0.sendmany("", {n0.getnewaddress(): 1 for _ in range(10)})
        n1.sendmany("", {n1.getnewaddress(): 1 for _ in range(10)})
        self.bmm()
        n0.reserveasset("GOLD")
        n0.reserveasset("SPLT")
        while n0.getblockcount() < 16:
            self.bmm()
        n0.registerasset("GOLD", 1000000, 2)
        self.bmm()
        n0.addliquidity("GOLD", 10000, "ECX", 5)
        self.bmm()

        self.log.info("Validators: blocks of stake; H a little ahead; the old node without v7")
        height = n0.getblockcount()
        H = height + 40
        self.payouts = [n.getnewaddress() for n in self.nodes]
        for i, key in enumerate(KEYS):
            with open(self.key_file(i), "w") as f:
                f.write("# validator key\n" + key.hex() + "\n")
        for i in range(3):
            self.restart_node(i, extra_args=self.validator_args(i) + [f"-testactivationheight=v7@{H}"])
        old_binary = os.environ.get("FLUSHD_OLD")
        if old_binary:
            old.args[0] = old_binary
            self.restart_node(OLD, extra_args=self.side_args)
        else:
            self.restart_node(OLD, extra_args=self.side_args + ["-testactivationheight=v7@1000000"])
        self.connect_all()
        self.wait_until(lambda: all(n.getblockcount() >= height + 5 for n in self.nodes[:3]), timeout=60)
        self.wait_in_sync()
        n0.setbmm(True, n0.getnewaddress(), True, Decimal("0.0001"))
        self.anchor()

        self.log.info("Before H: a swap's ECX waits for a block with the peg actions")
        assert_greater_than(H - 5, n0.getblockcount())
        assert not n0.getlaunchstats()["rules"]["instant_payouts_active"]
        quote = n0.quoteswap("GOLD", 100, "ECX")
        swap = n0.swapasset("GOLD", 100, "ECX")
        block = self.mined(n0, swap["txid"])
        if n0.getblock(block)["nonce"] == 0:
            assert not self.paid_in(n0, block, quote["amount_out"]), "before v7 a block of stake without an anchor pays nothing"

        self.log.info("From H: a swap's ECX in its own block of stake; the old node forks off at the first such block")
        self.wait_until(lambda: n0.getblockcount() >= H, timeout=120)
        assert n0.getlaunchstats()["rules"]["instant_payouts_active"]
        forked = False
        for _ in range(8):
            quote = n0.quoteswap("GOLD", 100, "ECX")
            swap = n0.swapasset("GOLD", 100, "ECX")
            block = self.mined(n0, swap["txid"])
            assert self.is_pos(n0, block)
            # (Its ECX at the block's one price: within a hair of the quote alone.)
            info = n0.getblock(block, 2)
            outs = [o for o in info["tx"][0]["vout"][1:] if o["scriptPubKey"].get("address") and n0.getaddressinfo(o["scriptPubKey"]["address"])["ismine"]]
            assert any(abs(o["value"] - quote["amount_out"]) <= quote["amount_out"] / 100 for o in outs), ("the swap's ECX in its block", quote, outs)
            if n0.getblock(block)["nonce"] == 0:
                # Without the peg actions: the old node cannot take it.
                self.wait_until(lambda: any(t["hash"] == block and t["status"] == "invalid" for t in old.getchaintips()), timeout=60)
                forked = True
                break
        assert forked, "no swap landed in a block of stake without an anchor"
        self.wait_in_sync()
        assert_peg_balance(main, self.nodes[:3])

        self.log.info("From H: a launch's fee 25 % security, 75 % split; the split's address paid by the coinbase")
        platform = n1.getnewaddress()
        n0.launchtoken("SPLT", {"fee_split": [{"to": "creator", "bps": 5000}, {"to": platform, "bps": 4000}, {"to": "burn", "bps": 1000}]})
        self.wait_until(lambda: self._has_launch(n0, "SPLT"), timeout=30)
        launch = n0.getlaunch("SPLT")
        assert_equal(launch["security_share"], 25)
        assert_equal([(s["to"], s["kind"], s["bps"]) for s in launch["fee_split"]],
                     [("creator", "creator", 5000), (platform, "address", 4000), ("burn", "burn", 1000)])
        buy = n0.buyfromlaunch("SPLT", 3)
        block = self.mined(n0, buy["txid"])
        # 3 ECX at 1 %: 0.0225 ECX the split's, 0.009 the address's, paid in the block that traded.
        self.wait_until(lambda: n0.getlaunch("SPLT")["fee_split"][1]["paid"] > 0, timeout=30)
        launch = n0.getlaunch("SPLT")
        paid = launch["fee_split"][1]["paid"]
        assert_equal(launch["fee_split"][1]["owed"], 0)
        coinbase = n0.getblock(block, 2)["tx"][0]["vout"][1:]
        assert any(o["scriptPubKey"].get("address") == platform and o["value"] == paid for o in coinbase), "the address paid in the block of the buy"
        assert_greater_than(launch["fee_split"][0]["owed"], 0)
        self.wait_in_sync()
        assert_peg_balance(main, self.nodes[:3])

    @staticmethod
    def _has_launch(node, ticker):
        try:
            node.getlaunch(ticker)
            return True
        except Exception:
            return False


if __name__ == '__main__':
    LayersV7Test(__file__).main()
