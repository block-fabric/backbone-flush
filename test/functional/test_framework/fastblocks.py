#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""FastBlocks helpers for functional tests: the validator keys of the tests, the peg balance
against the mainchain's escrow, and blocks of stake as bytes (to sign a second one for a slot)."""

import time
from decimal import Decimal
from io import BytesIO

from .key import TaggedHash, compute_xonly_pubkey, sign_schnorr
from .segwit_addr import decode_segwit_address
from .messages import CBlock, CTransaction, deser_compact_size, deser_vector, hash256, ser_compact_size, ser_uint256, ser_vector
from .util import assert_equal

SLOT = 5
POS_BLOCK_BIT = 0x10000
KEYS = [bytes([0x11] + [0] * 30 + [i]) for i in (1, 2, 3)]
XONLY = [compute_xonly_pubkey(key)[0].hex() for key in KEYS]
#: The size of the header of a block of stake: Bitcoin's 80 bytes, hashSections and the signature.
POS_HEADER_SIZE = 176


def escrow_at(main, height):
    """What the mainchain held in escrow for the slot after its block at `height` (0 before any deposit)."""
    total = Decimal(0)
    tip = main.getblockcount()
    for change in main.listsidechaindeposits(SLOT):
        if tip - change["confirmations"] + 1 <= height:
            total = change["total"]
    return total


def assert_peg_balance(main, nodes):
    """Design 6.3: every node's coins (spendable, withdrawals, the pot, bonded and unbonding stake, the
    pot's reserve of penalties, rewards and payouts owed) are the escrow on the mainchain after the
    mainchain block it acted on, up to. (The tests leave no fee unclaimed and burn no coins.)"""
    for node in nodes:
        node.syncmainchain()
        balance = node.getsidechainstate()["pegbalance"]
        # Layers: and the CHN the pools and the auctions hold ("assets").
        parts = ("utxos", "withdrawals", "pot", "bonded", "unbonding", "reserve", "rewards", "payouts", "assets")
        assert_equal(balance["total"], sum(balance[part] for part in parts))
        assert_equal(balance["total"], escrow_at(main, balance["mainheight"]))
        # The node's own check (audit 7, finding 3) says the same, when the tip did not move meanwhile.
        check = node.checkpegbalance()
        if check["mainheight"] == balance["mainheight"]:
            assert_equal(check["status"], "ok")
            assert_equal(check["pegbalance"], balance["total"])
            assert_equal(check["escrow"], balance["total"])
            assert_equal(check["difference"], Decimal(0))
    return balance


def stop_bmm(main, main_address, node, nodes):
    """The merged miner of `node` off, and the ticket requests it made meanwhile dropped: a mainchain
    block without them makes them stale. (Its follower may be making one as it is switched off: the
    mempool has to stay empty for a while.)"""
    node.setbmm(False)
    quiet = 0
    while quiet < 3:
        time.sleep(0.5)
        if main.getrawmempool():
            main.generateblock(main_address, [])
            for n in nodes:
                n.syncmainchain()
            quiet = 0
        else:
            quiet += 1
    assert_equal(main.getrawmempool(), [])


class PosBlock:
    """A block of stake as bytes: its extended header, its transactions, its sections."""

    def __init__(self, raw_hex):
        f = BytesIO(bytes.fromhex(raw_hex))
        self.header = bytearray(f.read(POS_HEADER_SIZE))
        assert int.from_bytes(self.header[0:4], "little") & POS_BLOCK_BIT
        self.vtx = deser_vector(f, CTransaction)
        self.sections = f.read(deser_compact_size(f))
        assert_equal(f.read(), b"")

    @property
    def hash(self):
        return hash256(bytes(self.header[:112]))[::-1].hex()

    def resign(self, key, genesis_hash):
        """The merkle root of the transactions, and the signature of `key` over the block hash (on the
        chain of the genesis block `genesis_hash`, hex)."""
        block = CBlock()
        block.vtx = self.vtx
        self.header[36:68] = ser_uint256(block.calc_merkle_root())
        self.header[112:176] = sign_schnorr(key, block_sighash(self.header, genesis_hash))

    def serialize(self):
        return bytes(self.header) + ser_vector(self.vtx, "serialize_with_witness") + ser_compact_size(len(self.sections)) + self.sections


def block_sighash(header, genesis_hash):
    """What the leader of a slot signs: TaggedHash("hpos/block", block hash || genesis hash), of the
    first 112 bytes of an extended header; `genesis_hash` in hex, as RPCs give it."""
    return TaggedHash("hpos/block", hash256(bytes(header[:112])) + bytes.fromhex(genesis_hash)[::-1])


def bond_pop(key, owner_address, genesis_hash, hrp="rsc"):
    """The proof of possession a bond to `key` owned by `owner_address` (P2WPKH) needs: the key's
    BIP340 signature of TaggedHash("hpos/bond", owner key hash || genesis hash)."""
    version, program = decode_segwit_address(hrp, owner_address)
    assert version == 0 and len(program) == 20
    return sign_schnorr(key, TaggedHash("hpos/bond", bytes(program) + bytes.fromhex(genesis_hash)[::-1]))


def slot_of(time_):
    """The slot of a time on regtest (1-second slots from the genesis time of the parameters)."""
    return time_ - REGTEST_GENESIS_TIME


#: Regtest's PosParams::genesis_time: slot s starts at this time plus s.
REGTEST_GENESIS_TIME = 1296688602


#: max_unanchored_slots on regtest: without an anchor, blocks of stake stop.
REGTEST_MAX_UNANCHORED_SLOTS = 100
#: stall_seconds on regtest: a mainchain block this long after the last block of stake shows the chain
#: stalled, and merged mined blocks take over.
REGTEST_STALL_SECONDS = 30


class SlotClock:
    """Mock time for the nodes of a test and its mainchain, moved on a slot at a time, and only when
    every validator that leads the slot (on its own chain, staking, not stalled) made its block: a
    test runs the same slots whatever the load of the machine (a busy machine only takes longer).
    Every wait is on a condition, bounded generously."""

    def __init__(self, test, nodes, slot_seconds=1, produce_timeout=120):
        self.test = test
        self.nodes = nodes
        self.slot_seconds = slot_seconds
        self.produce_timeout = produce_timeout
        self.now = None

    def set_time(self, t):
        self.now = t
        self.test.main.setmocktime(t)
        for n in self.nodes:
            if n.running:
                n.setmocktime(t)

    def slot(self):
        return (self.now - REGTEST_GENESIS_TIME) // self.slot_seconds

    def made_their_blocks(self, slot):
        """Whether each running validator that leads `slot` on its chain made its block of it."""
        for n in self.nodes:
            if not n.running:
                continue
            staking = n.getstakinginfo()
            if not staking["validator"] or not staking["enabled"] or staking["slot"] != slot:
                if staking["validator"] and staking["enabled"]:
                    return False
                continue
            info = n.getposinfo()
            keys = {k["pubkey"] for k in staking["keys"]}
            first = info["firstunanchoredslot"]
            stalled = first >= 0 and slot - first >= REGTEST_MAX_UNANCHORED_SLOTS
            if info.get("leader") in keys and info["active"] and not stalled and staking["lastslot"] != slot:
                return False
        return True

    def peers(self, node):
        """The test's nodes `node` is connected to."""
        indices = set()
        for peer in node.getpeerinfo():
            subver = peer.get("subver", "")
            if "(testnode" in subver:
                indices.add(int(subver.split("(testnode")[1].split(")")[0]))
        return [n for n in self.nodes if n.index in indices and n.running]

    @staticmethod
    def has_on_chain(node, block_hash):
        try:
            return node.getblockheader(block_hash)["confirmations"] > 0
        except Exception:
            return False

    def relayed(self, slot):
        """Whether each block of `slot` is on the chain of every node connected to its maker: the next
        leader builds on it (else it would leave it out, and its maker be jailed for a slot it led)."""
        for n in self.nodes:
            if not n.running or n.getstakinginfo()["lastslot"] != slot:
                continue
            tip = n.getbestblockhash()
            if any(not self.has_on_chain(peer, tip) for peer in self.peers(n)):
                return False
        return True

    def step(self):
        """The clock on to the next slot; returns it once its leaders made their blocks and their peers
        have them (or by produce_timeout: a block that could not be made, which the test sees as a
        missing block)."""
        slot = self.slot() + 1
        self.set_time(REGTEST_GENESIS_TIME + slot * self.slot_seconds)
        deadline = time.time() + self.produce_timeout
        while not (self.made_their_blocks(slot) and self.relayed(slot)):
            if time.time() > deadline:
                self.test.log.warning(f"slot {slot}: a leader made no block, or its peers do not have it")
                break
            time.sleep(0.05)
        return slot

    def advance(self, count):
        for _ in range(count):
            self.step()

    def advance_until(self, predicate, max_slots):
        """Slot after slot until `predicate` holds; it must within `max_slots`."""
        for _ in range(max_slots):
            if predicate():
                return
            self.step()
        if not predicate():
            for n in self.nodes:
                if n.running:
                    self.test.log.error(f"node {n.index}: {n.getposinfo()} {n.getstakinginfo()} tip {n.getblockcount()}")
            raise AssertionError(f"not true after {max_slots} slots")


class RawMessage:
    """A P2P message kept as its bytes: FastBlocks' own, and those with extended headers (the
    framework's block and headers have Bitcoin's 80-byte header)."""
    msgtype = b""

    def __init__(self, data=b""):
        self.data = data

    def deserialize(self, f):
        self.data = f.read()

    def serialize(self):
        return self.data

    def __repr__(self):
        return f"{self.msgtype.decode()}({len(self.data)} bytes)"


class MsgTicket(RawMessage):
    msgtype = b"fbticket"


class MsgGetTickets(RawMessage):
    msgtype = b"fbgettickets"


class MsgEvidence(RawMessage):
    msgtype = b"fbevidence"


class MsgRawHeaders(RawMessage):
    msgtype = b"headers"


class MsgRawBlock(RawMessage):
    msgtype = b"block"


class MsgRawCompactBlock(RawMessage):
    msgtype = b"cmpctblock"


def use_raw_messages():
    """Have the framework's peers read FastBlocks' messages, and blocks and headers, as bytes."""
    from .p2p import MESSAGEMAP
    for message in (MsgTicket, MsgGetTickets, MsgEvidence, MsgRawHeaders, MsgRawBlock, MsgRawCompactBlock):
        MESSAGEMAP[message.msgtype] = message


def headers_message(headers):
    """A headers message of serialized headers, each as a block without transactions (a transaction
    count of 0; and for an extended header, no sections)."""
    return MsgRawHeaders(ser_compact_size(len(headers)) + b"".join(bytes(h) + (b"\x00\x00" if len(h) == POS_HEADER_SIZE else b"\x00") for h in headers))


def ticket_bytes(anchored_hex, payee_script):
    """A serialized anchor ticket: version 1, the block anchored, the payee."""
    return b"\x01" + bytes.fromhex(anchored_hex)[::-1] + ser_compact_size(len(payee_script)) + payee_script


def ticket_hash(ticket):
    """h* of a serialized ticket, in hex as RPCs give it."""
    return TaggedHash("hpos/ticket", ticket)[::-1].hex()


def pos_header(prev_hex, slot, nonce=0, merkle=b"\x00" * 32, sections=b"\x00" * 32, sig=b"\x00" * 64, bits=None):
    """An extended header of a block of stake (unsigned unless `sig`), on `prev_hex`, of `slot` (regtest)."""
    header = (0x20000000 | POS_BLOCK_BIT).to_bytes(4, "little") + bytes.fromhex(prev_hex)[::-1] + merkle
    header += (REGTEST_GENESIS_TIME + slot).to_bytes(4, "little") + (bits if bits is not None else REGTEST_POS_BITS).to_bytes(4, "little")
    header += nonce.to_bytes(4, "little") + sections + sig
    assert_equal(len(header), POS_HEADER_SIZE)
    return bytearray(header)


#: nBits of a block of stake on regtest: the proof-of-work limit's.
REGTEST_POS_BITS = 0x207fffff


def header_hash(header):
    return hash256(bytes(header[:112]))[::-1].hex()


def chain_headers(node, start_height=1):
    """The serialized headers of `node`'s active chain from a height on."""
    return [bytes.fromhex(node.getblockheader(node.getblockhash(h), False)) for h in range(start_height, node.getblockcount() + 1)]


def activate_and_fund(test, funded=0, nodes=None):
    """Activate slot 5 on the mainchain, deposit 50 to node `funded` and take it in with a BMM block
    (which every node of `nodes` takes). `test` is a SidechainTest whose nodes follow its mainchain."""
    main = test.main
    test.mine_main(110)
    main.createsidechainproposal(SLOT, "FastBlocks", "Blocks of stake between merged mined blocks", "11" * 32, "22" * 20)
    test.mine_main(20 + 1)
    node = test.nodes[funded]
    main.createsidechaindeposit(SLOT, node.getdepositaddress()["depositaddress"], 50)
    test.mine_main()
    return bmm_block(test, node, nodes)


def bmm_block(test, node, nodes=None):
    """A BMM block by `node`, with its mempool; the mainchain commits to it; every node of `nodes` takes it."""
    nodes = test.nodes if nodes is None else nodes
    block = node.createbmmblock(node.getnewaddress())
    test.main.createbmmrequest(SLOT, block["hash"], Decimal("0.001"))
    test.mine_main()
    test.wait_until(lambda: all(n.getbestblockhash() == block["hash"] for n in nodes), timeout=60)
    return block["hash"]


def write_key_file(test, i, keys=None):
    """A key file with KEYS[i] (or `keys`), in the test's directory (not a node's: it outlives a wiped one)."""
    import os
    path = os.path.join(test.options.tmpdir, f"validator{i}.key")
    with open(path, "w") as f:
        f.write("".join(k.hex() + "\n" for k in (keys or [KEYS[i]])))
    return path


def bond_validators(test, funder, key_indices, amount=Decimal("0.01"), nodes=None):
    """`funder`'s wallet bonds `amount` to each KEYS[i] (the proofs of possession made here); BMM blocks
    take the bonds in and make a validator set with them (no validators before: BMM blocks carry the
    transactions). Returns when every node of `nodes` has an active set."""
    nodes = test.nodes if nodes is None else nodes
    genesis = funder.getblockhash(0)
    for i in key_indices:
        owner = funder.getnewaddress()
        funder.createbond(XONLY[i], amount, bond_pop(KEYS[i], owner, genesis).hex(), owner)
    test.sync_mempools(nodes)
    bmm_block(test, funder, nodes)
    assert_equal(funder.getrawmempool(), [])
    bmm_block(test, funder, nodes)
    test.wait_until(lambda: all(n.getposinfo()["active"] for n in nodes), timeout=120)
