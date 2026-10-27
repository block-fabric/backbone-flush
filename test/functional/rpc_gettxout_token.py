#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""gettxout's token (7.0.1): what an output carries, from the chain's state.

  - an asset's coin and its control coin, then a pool's shares: kind, kind_name, id, amount_units (exact text);
  - a plain coin: token null; a coin of the mempool: no token key (known once a block applies it);
  - decodeassettx names every kind and gives amount_units next to amount, and the operation's counts as <name>_units.
"""

from decimal import Decimal

from feature_bitassets import BitAssetsTest
from feature_sidechain import ACTIVATION_PERIOD, SLOT
from test_framework.util import assert_equal

KINDS = ["asset", "control", "reservation", "lp", "receipt", "position", "order"]


class GettxoutTokenTest(BitAssetsTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        super().run_test()

    def check_outputs(self, node, txid):
        """Every output the marker lists that carries a token: gettxout says the same as decodeassettx."""
        decoded = node.decodeassettx(node.gettransaction(txid)["hex"])
        seen = []
        for o in decoded["outputs"]:
            if o["carries"] == "result":
                # What the operation gave (shares, a swap's asset): in the chain's state once its block applied it.
                out = node.gettxout(txid, o["n"])
                if out and out["token"]:
                    assert_equal(KINDS[out["token"]["kind"]], out["token"]["kind_name"])
                    seen.append(out["token"])
                continue
            assert_equal(o["amount_units"], str(o["amount"]))
            out = node.gettxout(txid, o["n"])
            token = out["token"]
            assert_equal(token["kind_name"], o["carries"])
            assert_equal(KINDS[token["kind"]], token["kind_name"])
            assert_equal(token["id"], o["id"])
            assert_equal(token["amount_units"], o["amount_units"])
            seen.append(token)
        return decoded, seen

    def run_sidechain_test(self):
        main = self.main
        side, other = self.nodes
        self.main_address = main.getnewaddress()
        self.side_address = side.getnewaddress()

        self.log.info("Set up the sidechain, fund the wallet")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "BitAssets", "Assets")
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, side.getnewaddress(), 20)
        self.mine_main()
        self.bmm()
        side.sendmany("", {side.getnewaddress(): 1 for _ in range(10)})
        self.mine_txs()

        self.log.info("An asset's coin and its control coin")
        side.reserveasset("GOLD")
        self.mine_txs()
        self.mine_txs()
        registered = side.registerasset("GOLD", 1000000, 2)
        self.mine_txs()
        decoded, tokens = self.check_outputs(side, registered["txid"])
        assert_equal(sorted(t["kind_name"] for t in tokens), ["asset", "control"])
        coin = next(t for t in tokens if t["kind_name"] == "asset")
        assert_equal(coin["amount_units"], "100000000")  # 1,000,000.00 at 2 places
        assert_equal(next(t for t in tokens if t["kind_name"] == "control")["amount_units"], "1")
        assert_equal(decoded["operation"]["supply_units"], str(decoded["operation"]["supply"]))

        self.log.info("A pool's shares")
        added = side.addliquidity("GOLD", 1000, "ECX", 1)
        self.mine_txs()
        _, tokens = self.check_outputs(side, added["txid"])
        assert "lp" in [t["kind_name"] for t in tokens], tokens
        assert int(next(t for t in tokens if t["kind_name"] == "lp")["amount_units"]) > 0

        self.log.info("A plain coin: null; a coin of the mempool: no token")
        plain = side.sendtoaddress(side.getnewaddress(), Decimal("0.5"))
        self.mine_txs()
        raw = side.decoderawtransaction(side.gettransaction(plain)["hex"])
        for vout in raw["vout"]:
            out = side.gettxout(plain, vout["n"])
            if out is not None:
                assert out["token"] is None, out
        pending = side.sendtoaddress(side.getnewaddress(), Decimal("0.25"))
        out = side.gettxout(pending, 0, True)
        assert out is not None
        assert "token" not in out, out


if __name__ == '__main__':
    GettxoutTokenTest(__file__).main()
