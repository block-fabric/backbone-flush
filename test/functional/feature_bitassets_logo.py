#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the logos of assets (doc/bitassets.md, "Logos").

- Before logo_height: the wallet refuses a logo, and the mempool a marker that sets one (bad-ba-logo-inactive).
- From it: a logo set at the registration, shown by getasset, listassets and getassetlogo (bytes, hash, MIME).
- The holder of the control coin changes it (PNG, then JPEG) and removes it; anyone else is refused, by the
  wallet and by the rules (bad-ba-no-control); a logo the rules refuse is refused (bad-ba-logo-*).
- A reorganization puts the logo before back; both nodes have the same state.
- A node without the block files still serves the logo: it is kept with the state, not read from the blocks.
"""

import base64
import hashlib
import os
import re
import struct
import zlib

from feature_bitassets import BitAssetsTest
from feature_bitassets_invalid import CONTROL, asset_id, marker, marker_script, script_of, token
from feature_sidechain import ACTIVATION_PERIOD, SLOT
from test_framework.messages import CTransaction, CTxIn, CTxOut, COutPoint, ser_string
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error

SET_LOGO = 32
LOGO_HEIGHT = 12


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def make_png(colors, pattern=0):
    """A palette PNG of 256 x 256 pixels, 1 bit deep, its two colours given."""
    rows = b"".join(b"\x00" + bytes(((x * 8 + y + pattern) & 0x80) and 0xFF or (0x0F if (y // 32) % 2 else 0xF0) for x in range(32)) for y in range(256))
    ihdr = struct.pack(">IIBBBBB", 256, 256, 1, 3, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"PLTE", bytes(colors)) + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")


def b64(data):
    return base64.b64encode(data).decode()


class BitAssetsLogoTest(BitAssetsTest):
    def set_test_params(self):
        super().set_test_params()
        self.extra_args = [args + [f"-testactivationheight=logo@{LOGO_HEIGHT}"] for args in self.extra_args]

    def run_test(self):
        super().run_test()

    def sample(self, name):
        """A logo of the unit tests (src/test/bitassets_logo_samples.h)."""
        path = os.path.join(self.config["environment"]["SRCDIR"], "src", "test", "bitassets_logo_samples.h")
        text = open(path, encoding="utf8").read()
        body = re.search(r"LOGO_" + name + r"\{(.*?)\};", text, re.S).group(1)
        return bytes.fromhex("".join(re.findall(r'"([0-9a-f]*)"', body)))

    def set_logo_tx(self, node, asset, logo, control):
        """A transaction of `node`'s wallet whose marker sets the logo of an asset, spending `control` ((txid, n)), or nothing."""
        data = marker(SET_LOGO, asset_id(asset) + ser_string(logo), [(0, token(CONTROL, asset_id(asset)))])
        tx = CTransaction()
        if control:
            tx.vin.append(CTxIn(COutPoint(int(control[0], 16), control[1])))
        tx.vout.append(CTxOut(0, script_of(node, node.getnewaddress())))
        tx.vout.append(CTxOut(0, marker_script(data)))
        funded = node.fundrawtransaction(tx.serialize().hex(), {"changePosition": 2, "fee_rate": 2})["hex"]
        signed = node.signrawtransactionwithwallet(funded)
        assert signed["complete"]
        return signed["hex"]

    def assert_refused(self, node, tx_hex, reason):
        result = node.testmempoolaccept([tx_hex])[0]
        assert_equal(result["allowed"], False)
        assert_equal(result["reject-reason"], reason)

    def assert_logo(self, node, name, logo):
        asset = node.getasset(name)
        digest = hashlib.sha256(logo).hexdigest()
        mime = "image/png" if logo.startswith(b"\x89PNG") else "image/jpeg"
        assert_equal({k: asset["logo"][k] for k in ("hash", "mime", "size")}, {"hash": digest, "mime": mime, "size": len(logo)})
        served = node.getassetlogo(name)
        assert_equal(base64.b64decode(served["base64"]), logo)
        assert_equal(served["hash"], digest)
        assert_equal(served["mime"], mime)
        assert_equal(served["asset"], asset["asset"])
        assert_equal(served["txid"], asset["logo"]["txid"])
        listed = next(a for a in node.listassets() if a["asset"] == asset["asset"])
        assert_equal(listed["logo"], asset["logo"])

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
        assert_equal(side.getbitassetsinfo()["logo_height"], LOGO_HEIGHT)
        assert_equal(side.getbitassetsinfo()["max_logo_bytes"], 32768)

        logo1 = make_png([20, 30, 60, 240, 180, 40])
        logo2 = make_png([200, 20, 20, 250, 250, 250], pattern=3)
        jpeg = self.sample("JPEG_BASELINE")
        assert_greater_than(32768, len(logo1))

        self.log.info("Before logo_height: no logo")
        side.reserveasset("EARLY")
        side.reserveasset("BADGE")
        self.mine_txs()
        self.mine_txs()
        assert_greater_than(LOGO_HEIGHT - 1, side.getblockcount() + 1)
        assert_raises_rpc_error(-4, "not in the rules of the chain yet", side.registerasset, "BADGE", 100, 0, {}, True, b64(logo1))
        side.registerasset("EARLY", 100)
        self.mine_txs()
        assert_raises_rpc_error(-4, "not in the rules of the chain yet", side.setassetlogo, "EARLY", b64(logo1))
        control = side.getasset("EARLY")["control"]
        early_tx = self.set_logo_tx(side, side.getasset("EARLY")["asset"], logo1, (control["txid"], control["vout"]))
        self.assert_refused(side, early_tx, "bad-ba-logo-inactive")
        assert "logo" not in side.getasset("EARLY")
        assert_raises_rpc_error(-8, "has no logo", side.getassetlogo, "EARLY")

        while side.getblockcount() + 1 < LOGO_HEIGHT:
            self.mine_txs()
        self.log.info("From logo_height: the marker of before is taken")
        side.sendrawtransaction(early_tx)
        self.mine_txs()
        self.assert_logo(side, "EARLY", logo1)
        self.assert_logo(other, "EARLY", logo1)

        self.log.info("A logo set at the registration")
        registered = side.registerasset("BADGE", 1000, 0, {"info": "A badge"}, True, b64(logo1))
        self.mine_txs()
        self.assert_logo(side, "BADGE", logo1)
        badge = side.getasset("BADGE")
        assert_equal(badge["logo"]["txid"], registered["txid"])
        assert_equal(badge["logo"]["height"], badge["registered"])

        self.log.info("The controller changes it")
        changed = side.setassetlogo("BADGE", b64(logo2), {"fee_rate": 3})
        assert_equal(changed["hash"], hashlib.sha256(logo2).hexdigest())
        assert_equal(changed["size"], len(logo2))
        # The logo goes whole into the transaction, at the fee rate asked.
        vsize = side.getmempoolentry(changed["txid"])["vsize"]
        assert_greater_than(vsize, len(logo2))
        assert_equal(changed["fee"], side.getmempoolentry(changed["txid"])["fees"]["base"])
        self.mine_txs()
        self.assert_logo(side, "BADGE", logo2)
        self.assert_logo(other, "BADGE", logo2)
        change_block = side.getblockhash(side.getasset("BADGE")["logo"]["height"])
        side.setassetlogo("BADGE", b64(jpeg))
        self.mine_txs()
        self.assert_logo(side, "BADGE", jpeg)
        jpeg_block = side.getblockhash(side.getasset("BADGE")["logo"]["height"])

        self.log.info("Nobody else can")
        assert_raises_rpc_error(-6, "control coin", other.setassetlogo, "BADGE", b64(logo1))
        stolen = self.set_logo_tx(other, badge["asset"], logo1, None)
        self.assert_refused(other, stolen, "bad-ba-no-control")

        self.log.info("Logos the rules refuse")
        for bad, reason in [
            (logo1 + b"<script>", "bad-ba-logo-trailing"),
            (logo1[:-20], "bad-ba-logo-png-length"),
            (logo1[:-12], "bad-ba-logo-png-truncated"),
            (logo1[:33] + chunk(b"tEXt", b"Comment\x00hi") + logo1[33:], "bad-ba-logo-png-chunk"),
            (b"<svg xmlns='http://www.w3.org/2000/svg'/>", "bad-ba-logo-format"),
            (b"GIF89a" + bytes(64), "bad-ba-logo-format"),
            (jpeg[:2] + b"\xff\xfe\x00\x04hi" + jpeg[2:], "bad-ba-logo-jpeg-metadata"),
            (logo1 + bytes(32768), "bad-ba-logo-oversize"),
        ]:
            assert_raises_rpc_error(-8, reason, side.setassetlogo, "BADGE", b64(bad))
        control = side.getasset("BADGE")["control"]
        self.assert_refused(side, self.set_logo_tx(side, badge["asset"], logo1 + b"\x00", (control["txid"], control["vout"])), "bad-ba-logo-trailing")
        wrong_size = logo1[:16] + struct.pack(">I", 255) + logo1[20:29]
        wrong_size += struct.pack(">I", zlib.crc32(wrong_size[12:29])) + logo1[33:]
        assert_raises_rpc_error(-8, "bad-ba-logo-size", side.setassetlogo, "BADGE", b64(wrong_size))
        assert_raises_rpc_error(-8, "not base64", side.setassetlogo, "BADGE", "!!!")
        assert_raises_rpc_error(-8, "No such asset", side.getassetlogo, "NOPE")

        self.log.info("A reorganization puts the logo before back")
        side.invalidateblock(jpeg_block)
        self.assert_logo(side, "BADGE", logo2)
        side.invalidateblock(change_block)
        self.assert_logo(side, "BADGE", logo1)
        side.reconsiderblock(change_block)
        self.assert_logo(side, "BADGE", jpeg)
        self.check_in_sync()

        self.log.info("The controller removes it")
        side.setassetlogo("BADGE", "")
        self.mine_txs()
        assert "logo" not in side.getasset("BADGE")
        assert_raises_rpc_error(-8, "has no logo", side.getassetlogo, "BADGE")
        assert_raises_rpc_error(-8, "no logo to remove", side.setassetlogo, "BADGE", "")
        side.setassetlogo("BADGE", b64(logo2))
        self.mine_txs()
        self.check_in_sync()

        self.log.info("Its block wiped on disk, the node still serves the logo: it is kept with the state, not read from the blocks")
        # (A sidechain node prunes nothing a test can make: it keeps 10000 MiB and 2880 blocks. The bytes of the logo are
        # wiped from the block files instead, as pruning would remove them, and the node started checking only its tip.)
        self.mine_txs()
        self.mine_txs()
        self.stop_node(1)
        blocks = os.path.join(other.chain_path, "blocks")
        key = open(os.path.join(blocks, "xor.dat"), "rb").read() if os.path.exists(os.path.join(blocks, "xor.dat")) else bytes(8)
        key = key or bytes(8)
        wiped = []
        for name in sorted(os.listdir(blocks)):
            if not re.fullmatch(r"blk\d+\.dat", name):
                continue
            path = os.path.join(blocks, name)
            raw = bytearray(open(path, "rb").read())
            plain = bytes(b ^ key[i % len(key)] for i, b in enumerate(raw))
            at = plain.find(logo2)
            while at >= 0:
                for i in range(at, at + len(logo2)):
                    raw[i] = 0 ^ key[i % len(key)]
                wiped.append((name, at))
                at = plain.find(logo2, at + 1)
            open(path, "wb").write(raw)
        assert wiped
        self.start_node(1, self.extra_args[1] + ["-checkblocks=1", "-checklevel=0", "-connect=0"])
        logo_block = other.getblockhash(other.getasset("BADGE")["logo"]["height"])
        assert logo2.hex() not in other.getblock(logo_block, 0)
        self.assert_logo(other, "BADGE", logo2)
        self.assert_logo(other, "EARLY", logo1)
        self.stop_node(1)
        # The blocks put back as they were (from the other node), for the end of the test.
        for name, at in wiped:
            path = os.path.join(blocks, name)
            raw = bytearray(open(path, "rb").read())
            for i in range(at, at + len(logo2)):
                raw[i] = logo2[i - at] ^ key[i % len(key)]
            open(path, "wb").write(raw)
        self.start_node(1)
        assert logo2.hex() in other.getblock(logo_block, 0)
        self.connect_nodes(0, 1)
        self.check_in_sync()

if __name__ == "__main__":
    BitAssetsLogoTest(__file__).main()
