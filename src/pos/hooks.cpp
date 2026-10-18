// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/hooks.h>

#include <addresstype.h>
#include <arith_uint256.h>
#include <common/args.h>
#include <chain.h>
#include <consensus/validation.h>
#include <kernel/chainparams.h>
#include <logging.h>
#include <net.h>
#include <net_processing.h>
#include <node/context.h>
#include <pos/feesplit.h>
#include <pos/finality.h>
#include <pos/forkchoice.h>
#include <pos/producer.h>
#include <sidechain/follower.h>
#include <sidechain/mainchain.h>
#include <sidechain/state.h>
#include <sidechain/store.h>
#include <streams.h>
#include <util/chaintype.h>
#include <util/strencodings.h>
#include <util/moneystr.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <array>
#include <ios>
#include <tuple>

namespace pos {

namespace {
//! FastBlocks' tables of the sidechain store (0x50 to 0x5f are FastBlocks'; see sidechain/state.h).
const sidechain::Cell<FastState> FAST_STATE{0x55, FastState{}};
//! The pot accrued as of a block, by height: written by the PoS blocks that add to it, erased once paid.
const sidechain::Table<uint32_t, int64_t> POT_ACCRUED{0x56};
//! The seed of each epoch the blocks of stake of the chain read (for their leaders, and those they
//! charged for the slots they skipped), by epoch: what CheckSeeds compares the record with. In the
//! state, so that what it says is what the blocks connected read, after a restart too. Of every epoch,
//! not the last few: a mainchain reorg may reach the seed of any, and a node that takes the chain in
//! afresh reads the record as it is (review 7). One entry an epoch with blocks of stake.
const sidechain::Table<uint64_t, uint256> EPOCH_SEEDS{0x5e};
//! The validator pool (fee split): the validators' share of fees, by the epoch whose set it is shared
//! among, erased once paid (the remainder of the rounding to the next epoch's).
const sidechain::Table<uint64_t, int64_t> POOL{0x5f};
//! Epochs whose pool a peg step pays, at most (the rest at the next ones).
constexpr size_t MAX_POOL_EPOCHS_PER_STEP{16};

void AddToPool(sidechain::StoreOverlay& out, int64_t epoch, int64_t amount)
{
    if (amount <= 0) return;
    const uint64_t key{static_cast<uint64_t>(std::max<int64_t>(epoch, 0))};
    POOL.Put(out, key, POOL.Get(out, key).value_or(0) + amount);
}

FastBlocksHooks* g_installed{nullptr};

//! Validator sets of anchors a node keeps in memory, at most.
constexpr size_t MAX_KNOWN_SETS{4096};

bool Fail(std::string& reject_reason, const char* reason)
{
    reject_reason = reason;
    return false;
}

/** The ancestor of `index` at `height`, if `index` is that high. */
const CBlockIndex* AncestorAt(const CBlockIndex* index, int height)
{
    if (!index || height < 0 || height > index->nHeight) return nullptr;
    return index->GetAncestor(height);
}

/** Whether `block` is `ancestor` or descends from it. */
bool Descends(const CBlockIndex* block, const CBlockIndex* ancestor)
{
    return block && ancestor && AncestorAt(block, ancestor->nHeight) == ancestor;
}
} // namespace

FastState GetFastState(const sidechain::StoreView& view) { return FAST_STATE.Get(view); }

int64_t PotAccruedAt(const sidechain::StoreView& view, int32_t height)
{
    int64_t accrued{FAST_STATE.Get(view).pot_paid};
    if (height < 0) return accrued;
    POT_ACCRUED.ForEach(view, [&](const uint32_t& at, const int64_t& value) {
        if (at > static_cast<uint32_t>(height)) return false;
        accrued = value;
        return true;
    });
    return accrued;
}

std::vector<std::pair<int64_t, int64_t>> PoolEntries(const sidechain::StoreView& view)
{
    std::vector<std::pair<int64_t, int64_t>> entries;
    POOL.ForEach(view, [&](const uint64_t& epoch, const int64_t& amount) {
        entries.emplace_back(static_cast<int64_t>(epoch), amount);
        return true;
    });
    return entries;
}

int64_t PoolTotal(const sidechain::StoreView& view)
{
    int64_t total{0};
    for (const auto& [epoch, amount] : PoolEntries(view)) total += amount;
    return total;
}

FastBlocksHooks::FastBlocksHooks(const CChainParams& params)
    : m_chain_params{params},
      m_params{params.GetConsensus().pos},
      m_genesis{params.GetConsensus().hashGenesisBlock},
      m_genesis_set{params.GetConsensus().pos},
      m_staking{params.GetConsensus().pos, params.GetConsensus().hashGenesisBlock},
      m_pos_bits{PosBits(params.GetConsensus().powLimit)}
{
}

FastBlocksHooks::~FastBlocksHooks() = default;

sidechain::BlockKind FastBlocksHooks::Kind(const CBlockHeader& header) const
{
    return IsPosHeader(header) ? sidechain::BlockKind::PLUGIN : sidechain::BlockKind::BMM;
}

bool FastBlocksHooks::AckInOrder(const CBlockIndex& prev, const CBlockHeader& header)
{
    return header.nNonce == 0 || header.nNonce > ChainKey::FromWork(prev.nChainWork).last_anchor_height;
}

std::optional<int64_t> FastBlocksHooks::LastPosSlot(const CBlockIndex* index, const Consensus::PosParams& params)
{
    for (; index; index = index->pprev) {
        if (index->m_ext) return SlotOf(index->nTime, params);
    }
    return std::nullopt;
}

bool FastBlocksHooks::CheckHeader(const CBlockHeader& header, const CBlockIndex& prev, const ChainstateManager& chainman, BlockValidationState& state) const
{
    AssertLockHeld(::cs_main);
    const auto invalid{[&](const char* reason, const char* debug) { return state.Invalid(BlockValidationResult::BLOCK_INVALID_HEADER, reason, debug); }};
    if (header.nBits != m_pos_bits) return invalid("bad-pos-bits", "a block of stake has the chain's proof-of-work limit as nBits");
    const int64_t time{header.nTime};
    if (time <= m_params.genesis_time || (time - m_params.genesis_time) % m_params.slot_seconds != 0) {
        return invalid("bad-pos-time", "a block of stake has the time its slot starts");
    }
    const int64_t slot{SlotOf(time, m_params)};
    // An acknowledgement counts as an anchor in the order of chains from its header on: it comes after
    // the last anchor of the chain below it (the state's bad-pos-ack-order, as far as the headers tell),
    // so that a branch counts each commitment on record once at most, in order.
    if (!AckInOrder(prev, header)) {
        return invalid("bad-pos-ack-order", "an acknowledgement comes after the last anchor of the chain below it");
    }
    // A slot to come: the header may wait for it, if its leader signed it (anyone can make the others,
    // and those kept would push out the headers that wait for good reasons).
    const bool future{header.Time() > NodeClock::now() + std::chrono::seconds{m_params.max_future_seconds}};
    // The signature before anything that reads more of the chain, or keeps the header to try again
    // later: only a leader makes the node look further. The leader is that of the validator set of the
    // slot's epoch, which an anchor before the epoch before it made, and of the epoch's seed, which the
    // record of the mainchain gives.
    const SlotLeader leader{LeaderOf(prev, slot)};
    // A header HeaderSpare just found spare (from a peer; its signer has two headers of the slot in the
    // block index for good already): its signature can change nothing, so it costs none. Alone it is
    // refused (header-spare), whatever it is; with a header on top of it, or as a tip the node may need,
    // it is taken unchecked, and its block's signature is checked when connected (AfterBlock). It makes
    // no proof the node has not (HeaderSpare: the two kept are it). Else a spare header sent again and
    // again, which nothing remembers, cost signature checks each time (review 7).
    const bool spare{m_spare_header == header.GetHash()};
    m_spare_header.SetNull();
    switch (leader.status) {
    case LeaderStatus::INACTIVE:
        return invalid("bad-pos-inactive", "no blocks of stake in this epoch: too few validators");
    case LeaderStatus::SEED_UNKNOWN:
        if (future) return state.Invalid(BlockValidationResult::BLOCK_TIME_FUTURE, "pos-time-future-unchecked", "a slot to come, whose leader is not known yet");
        return state.Invalid(BlockValidationResult::BLOCK_TIME_FUTURE, "pos-seed-unknown", "the mainchain block that seeds the epoch is not on record yet");
    case LeaderStatus::SET_UNKNOWN: {
        // The set of an anchor of a branch this node has not connected (a node catching up has the
        // headers of the chain before its blocks): taken without its signature checked, which the
        // block's is when it is connected (AfterBlock), as that anchor is then. What such a header can
        // do is bounded: it counts one anchor at most, for a commitment on record after the last one
        // below it (above); there is an anchor on its branch that the node took in checked (a signed
        // acknowledgement, or a BMM block, which a commitment of its own made); and it does not take
        // back a final block, nor reach below the window of sets the state keeps:
        if (future) return state.Invalid(BlockValidationResult::BLOCK_TIME_FUTURE, "pos-time-future-unchecked", "a slot to come, whose leader is not known yet");
        const ChainstateManager* manager{Chainman()};
        const CChain* active{manager ? &manager->ActiveChain() : nullptr};
        // ... the anchor that made the set is on the active chain, and the state pruned the set: a
        // branch from deeper than the window for proofs;
        const bool pruned{leader.set_anchor && active && active->Contains(*leader.set_anchor)};
        // ... or the branch leaves the active chain below its final block.
        const CBlockIndex* fork{active ? active->FindFork(prev) : nullptr};
        const bool below_final{fork && fork != active->Tip() && fork->nHeight < FinalHeight()};
        if (pruned || below_final) {
            // (Not the peer's fault: its chain may be another; this node does not take it.)
            return state.Invalid(BlockValidationResult::BLOCK_HEADER_LOW_WORK, "bad-pos-fork-too-deep", "a header whose signature cannot be checked, on a branch that leaves a final block or the window of sets");
        }
        // How many such headers the node holds, and from whom, is the template's (HeaderCheckable:
        // unchecked headers belong to the peers that send them, within their budget).
        break;
    }
    case LeaderStatus::OK:
        if (spare) {
            // (Nor kept waiting for its slot: the node keeps no more of its signer's headers of the slot.)
            if (future) return state.Invalid(BlockValidationResult::BLOCK_HEADER_LOW_WORK, "header-spare", "the node keeps no more headers like this one");
            break;
        }
        if (!CheckBlockSig(header, leader.pubkey, Genesis())) return invalid("bad-pos-sig", "a block of stake is signed by the leader of its slot");
        m_checked_signer = {header.GetHash(), leader.pubkey};
        break;
    }
    if (future) return state.Invalid(BlockValidationResult::BLOCK_TIME_FUTURE, "pos-time-future", "the slot of the block of stake has not started");
    // A second header the leader signed for this slot is the proof that it signed twice, whatever
    // else is wrong with it.
    if (leader.status == LeaderStatus::OK && !spare) {
        if (const auto proof{m_evidence.NoteSigned(header, slot, leader.pubkey)}) {
            std::string reason;
            if (const auto offence{CheckEvidence(*proof, leader.pubkey, Genesis(), m_params, reason)}) {
                if (m_evidence.Add(*offence, *proof)) LogWarning("FastBlocks: validator %s signed two blocks for slot %d", HexStr(offence->pubkey), offence->slot);
            }
        }
    }
    if (const auto last{LastPosSlot(&prev, m_params)}; last && slot <= *last) {
        return invalid("bad-pos-slot", "a block of stake comes in a later slot than the one before it");
    }
    // No more than max_unanchored_slots after the first block of stake no anchor covers, as the
    // state checks (AfterBlock), as far as the headers tell: this bounds the headers a node takes
    // in without anchors. Without an acknowledgement, that block is the first block of stake after
    // the last BMM block, or at or before the last acknowledgement (which covers at most up to its
    // parent). (An acknowledgement may cover much: the state checks it.)
    if (header.nNonce == 0) {
        int64_t first{slot};
        int64_t walked{0};
        for (const CBlockIndex* index{&prev}; index && index->nHeight > 0 && index->m_ext; index = index->pprev) {
            first = SlotOf(index->nTime, m_params);
            // (Slots grow block by block: so many blocks back, the bound is passed.)
            if (index->nNonce != 0 || ++walked > m_params.max_unanchored_slots) break;
        }
        if (slot - first >= m_params.max_unanchored_slots) {
            return invalid("bad-pos-unanchored", "a block of stake comes within max_unanchored_slots of the first one no anchor covers");
        }
    }
    // An acknowledgement names a mainchain block with a commitment: not known yet, the block may be
    // tried again later (it is checked in full with the block).
    if (header.nNonce != 0 && (!chainman.m_mainchain || !chainman.m_mainchain->BmmAt(static_cast<int>(header.nNonce)))) {
        return state.Invalid(BlockValidationResult::BLOCK_TIME_FUTURE, "pos-ack-unknown", "no commitment on record at the height the block acknowledges");
    }
    return true;
}

bool FastBlocksHooks::HeaderCheckable(const CBlockHeader& header, const CBlockIndex& prev) const
{
    AssertLockHeld(::cs_main);
    if (!IsPosHeader(header)) return true;
    const int64_t time{header.nTime};
    if (time <= m_params.genesis_time || (time - m_params.genesis_time) % m_params.slot_seconds != 0) return true;
    // Not after the median time of the chain before it: refused next (time-too-old), its leader not
    // looked for. A slot anyone can name, far below `prev`, would otherwise cost a walk down the chain
    // for its set, under cs_main, before the time is checked (review 7). Past it, the slot is that of
    // the last blocks before `prev`, and the walk as short as the chain's own.
    if (time <= prev.GetMedianTimePast()) return true;
    return LeaderOf(prev, SlotOf(time, m_params)).status != LeaderStatus::SET_UNKNOWN;
}

bool FastBlocksHooks::HeaderSpare(const CBlockHeader& header, const CBlockIndex& prev) const
{
    AssertLockHeld(::cs_main);
    m_spare_header.SetNull();
    if (!IsPosHeader(header)) return false;
    const int64_t time{header.nTime};
    if (time <= m_params.genesis_time || (time - m_params.genesis_time) % m_params.slot_seconds != 0) return false;
    // (Refused next, time-too-old: as HeaderCheckable.)
    if (time <= prev.GetMedianTimePast()) return false;
    const int64_t slot{SlotOf(time, m_params)};
    const auto found{m_slot_headers.find(slot)};
    // (Fewer than that of the slot at all: none is spare, nothing more to read.)
    if (found == m_slot_headers.end() || found->second.size() < MAX_SLOT_HEADERS) return false;
    // By its signer: the leader of the slot on its branch (as CheckHeader checks next). A signer that
    // signed two of the slot kept already, the proof that it signed twice, has nothing more to give;
    // another leader of the slot (another branch's set, or the record's seed moved) has its own two.
    const SlotLeader leader{LeaderOf(prev, slot)};
    if (leader.status != LeaderStatus::OK) return false;
    const ChainstateManager* chainman{Chainman()};
    std::vector<uint256> signed_by;
    for (SlotHeader& kept : found->second) {
        if (!kept.signer && chainman) {
            // (Kept checked, against the leader of its time: its signature says whether it is this one.)
            const CBlockIndex* index{chainman->m_blockman.LookupBlockIndex(kept.hash)};
            if (index && CheckBlockSig(index->GetBlockHeader(), leader.pubkey, Genesis())) kept.signer = leader.pubkey;
        }
        if (kept.signer != leader.pubkey) continue;
        signed_by.push_back(kept.hash);
        if (signed_by.size() < MAX_SLOT_HEADERS) continue;
        // The proof that it signed twice is the two kept, whose signatures were checked: noted if the
        // pool does not have one of the offence (after a restart, say), so that the spare header,
        // whose own signature is not checked (CheckHeader), takes no proof away.
        const Offence offence{.pubkey = leader.pubkey, .slot = slot};
        if (!m_evidence.Get(offence) && chainman) {
            const CBlockIndex* a{chainman->m_blockman.LookupBlockIndex(signed_by[0])};
            const CBlockIndex* b{chainman->m_blockman.LookupBlockIndex(signed_by[1])};
            DoubleProposal proof;
            std::string reason;
            if (a && b) proof = {.a = a->GetBlockHeader(), .b = b->GetBlockHeader()};
            if (a && b && CheckEvidenceForm(proof, m_params, reason) && m_evidence.Add(offence, proof)) {
                LogWarning("FastBlocks: validator %s signed two blocks for slot %d", HexStr(offence.pubkey), offence.slot);
            }
        }
        m_spare_header = header.GetHash();
        return true;
    }
    return false;
}

void FastBlocksHooks::HeaderKept(const CBlockIndex& index) const
{
    AssertLockHeld(::cs_main);
    if (!index.m_ext) return;
    const int64_t time{index.nTime};
    if (time <= m_params.genesis_time || (time - m_params.genesis_time) % m_params.slot_seconds != 0) return;
    // Each once (an entry may be told of twice), with its signer if its signature was just checked
    // (CheckHeader, AfterBlock); else found when HeaderSpare asks. Bounded: two a signer of the slot
    // (HeaderSpare), and those of blocks connected.
    const uint256 hash{index.GetBlockHash()};
    const std::optional<uint256> signer{m_checked_signer.first == hash ? std::optional<uint256>{m_checked_signer.second} : std::nullopt};
    std::vector<SlotHeader>& kept{m_slot_headers[SlotOf(time, m_params)]};
    for (SlotHeader& entry : kept) {
        if (entry.hash != hash) continue;
        if (!entry.signer) entry.signer = signer;
        return;
    }
    kept.push_back({.hash = hash, .signer = signer});
}

sidechain::ChainHooks::UncheckedBudget FastBlocksHooks::UncheckedLimits() const
{
    return {.per_peer = static_cast<size_t>(m_params.unchecked_headers_per_peer),
            .block_bytes_per_peer = static_cast<size_t>(m_params.unchecked_block_bytes_per_peer),
            .per_inbound_peer = static_cast<size_t>(m_params.unchecked_headers_per_inbound_peer),
            .block_bytes_per_inbound_peer = static_cast<size_t>(m_params.unchecked_block_bytes_per_inbound_peer),
            .inbound_holders = static_cast<size_t>(m_params.unchecked_inbound_holders)};
}

bool FastBlocksHooks::CheckBlock(const CBlock& block, BlockValidationState& state) const
{
    const auto invalid{[&](const char* reason, const char* debug) { return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, reason, debug); }};
    size_t pots{0};
    for (const CTxOut& out : block.vtx.at(0)->vout) pots += IsPotScript(out.scriptPubKey);
    // Proofs in the coinbase of a BMM block (a block of stake has them in its sections): their form (the
    // chain checks the rest: AfterBlock).
    size_t coinbase_proofs{0};
    for (const CTxOut& out : block.vtx.at(0)->vout) {
        if (!IsEvidenceScript(out.scriptPubKey)) continue;
        if (IsPosHeader(block)) return invalid("bad-pos-evidence-not-bmm", "a block of stake carries proofs in its sections, not its coinbase");
        if (++coinbase_proofs > MAX_BMM_EVIDENCE) return invalid("bad-pos-evidence-count", "too many proofs in the coinbase of a BMM block");
        const auto proof{ParseEvidenceScript(out.scriptPubKey)};
        if (!proof) return invalid("bad-pos-evidence-output", "a proof in the coinbase of a BMM block does not read");
        std::string reason;
        if (!CheckEvidenceForm(*proof, m_params, reason)) return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, reason, "a proof that a leader signed twice does not have the form of one");
    }
    if (!IsPosHeader(block)) {
        if (pots) return invalid("bad-pos-pot-not-pos", "only a block of stake puts coins in the fee pot");
        return true;
    }
    if (pots > 1) return invalid("bad-pos-pot-multiple", "one fee pot output at most");
    PosSections sections;
    if (!PosSections::Decode(block.vSections, sections)) return invalid("bad-pos-sections", "the sections of a block of stake do not read");
    // The form of its proofs (the chain checks the rest: AfterBlock).
    for (const DoubleProposal& proof : sections.evidence) {
        std::string reason;
        if (!CheckEvidenceForm(proof, m_params, reason)) return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, reason, "a proof that a leader signed twice does not have the form of one");
    }
    if (!sections.ack) {
        if (block.nNonce != 0) return invalid("bad-pos-ack-nonce", "a block with no acknowledgement has nNonce 0");
        return true;
    }
    const AnchorAck& ack{*sections.ack};
    if (ack.main_height <= 0 || static_cast<uint32_t>(ack.main_height) != block.nNonce) {
        return invalid("bad-pos-ack-nonce", "nNonce is the mainchain height of the acknowledged commitment");
    }
    if (ack.ticket.version != 1) return invalid("bad-pos-ticket-version", "unknown ticket version");
    if (!IsValidPayee(ack.ticket.payee)) return invalid("bad-pos-ticket-payee", "a ticket pays the pot to a standard script of 80 bytes or less");
    // Noted (to find the blocks that rest on a commitment the mainchain drops) only if the leader of
    // its slot signed it: anyone can make a block that reads, and enough of them would push the
    // acknowledgements that count out of memory. (The header rules turn the others away. A block whose
    // leader this node cannot tell yet is noted when it is connected: AfterBlock.)
    const int64_t time{block.nTime};
    if (time > m_params.genesis_time && (time - m_params.genesis_time) % m_params.slot_seconds == 0 && Chainman()) {
        LOCK(::cs_main);
        if (const CBlockIndex* prev{Chainman()->m_blockman.LookupBlockIndex(block.hashPrevBlock)}) {
            // (Not walking down the whole chain for a block that names a slot far below its parent's: the
            // header rules turn it away; noted when connected otherwise.)
            const SlotLeader leader{LeaderOf(*prev, SlotOf(time, m_params), nullptr, EvidenceWithin())};
            if (leader.status == LeaderStatus::OK && CheckBlockSig(block, leader.pubkey, Genesis())) NoteAck(block.GetHash(), ack);
        }
    }
    return true;
}

uint256 FastBlocksHooks::SectionsHash(std::span<const unsigned char> sections) const { return pos::SectionsHash(sections); }

sidechain::OtherExtension FastBlocksHooks::CheckOtherExtension(const CBlockHeader& header, const CBlockIndex& index) const
{
    AssertLockHeld(::cs_main);
    // (The block hash covers hashSections: only the signature can differ.)
    if (!IsPosHeader(header) || !index.m_ext || !index.pprev || header.ext.hashSections != index.m_ext->hashSections) return sidechain::OtherExtension::INVALID;
    const SlotLeader leader{LeaderOf(*index.pprev, SlotOf(header.nTime, m_params))};
    switch (leader.status) {
    case LeaderStatus::SET_UNKNOWN:
    case LeaderStatus::SEED_UNKNOWN:
        return sidechain::OtherExtension::UNKNOWN;
    case LeaderStatus::INACTIVE:
        return sidechain::OtherExtension::INVALID;
    case LeaderStatus::OK:
        break;
    }
    if (!CheckBlockSig(header, leader.pubkey, Genesis())) return sidechain::OtherExtension::INVALID;
    // A valid signature, and the node's own (taken before it could tell the leader: a forged one,
    // which the block hash does not cover) is not: this one replaces it.
    if (!CheckBlockSig(index.GetBlockHeader(), leader.pubkey, Genesis())) return sidechain::OtherExtension::REPLACE;
    return sidechain::OtherExtension::EQUIVALENT;
}

int64_t FastBlocksHooks::MaxFutureBlockTime() const
{
    // While the network has blocks of stake: from the start (genesis validators), or while the set the
    // newest anchor of the active chain made is active (bonds) -- the state of the tip's, not what this
    // node saw since it started: a node that restarts, or reorgs, agrees with the others.
    if (m_genesis_set.Active()) return MAX_FUTURE_BLOCK_TIME_POS;
    ChainstateManager* chainman{Chainman()};
    if (!chainman) return ChainHooks::MaxFutureBlockTime();
    LOCK(::cs_main);
    const CBlockIndex* tip{chainman->ActiveChain().Tip()};
    if (!tip) return ChainHooks::MaxFutureBlockTime();
    bool active;
    {
        LOCK(m_staking_mutex);
        if (m_staking_for == tip->GetBlockHash()) return m_staking_seen ? MAX_FUTURE_BLOCK_TIME_POS : ChainHooks::MaxFutureBlockTime();
    }
    {
        const sidechain::StoreView& view{chainman->ActiveChainstate().SideCache()};
        const FastState fast{FAST_STATE.Get(view)};
        const auto set{fast.anchor_point_height >= 0 ? AnchorSetOf(fast.anchor_point, view, nullptr, /*from_state=*/true) : nullptr};
        active = set && set->Active();
    }
    LOCK(m_staking_mutex);
    m_staking_for = tip->GetBlockHash();
    m_staking_seen = active;
    return active ? MAX_FUTURE_BLOCK_TIME_POS : ChainHooks::MaxFutureBlockTime();
}

bool FastBlocksHooks::RecordIndependent(const std::string& reject_reason) const
{
    // What a block breaks whatever the record says, given the chain below it: the form of its
    // sections, coinbase and proofs, its producer's share, where its acknowledgement's block is, how
    // long its chain went without anchors, the staking rules of its transactions. Not what reads a
    // leader (the seed is the record's: bad-pos-sig, the proofs' signatures and window), a set (made
    // at an anchor's slot, which the record's median time may set), a commitment, or the mainchain's
    // times.
    static const std::set<std::string> independent{
        "bad-pos-sections", "bad-pos-producer-share", "bad-pos-pot-not-pos", "bad-pos-pot-multiple", "bad-pos-pool-multiple", "bad-pos-ack-nonce", "bad-pos-ticket-version",
        "bad-pos-ticket-payee", "bad-pos-ack-anchored", "bad-pos-ack-chain", "bad-pos-unanchored",
        "bad-pos-evidence-not-bmm", "bad-pos-evidence-count", "bad-pos-evidence-output", "bad-pos-evidence-kind", "bad-pos-evidence-slot",
        "bad-pos-evidence-same", "bad-pos-evidence-late", "bad-pos-evidence-known", "bad-pos-evidence-banned",
        "bad-pos-bond", "bad-pos-bond-amount", "bad-pos-bond-owner", "bad-pos-bond-owner-sig", "bad-pos-bond-pop", "bad-pos-bond-tombstoned", "bad-pos-bond-unanchored",
        "bad-pos-unbond", "bad-pos-unbond-value", "bad-pos-unbond-unknown", "bad-pos-unbond-sequence", "bad-pos-unbond-amount",
        "bad-pos-unbond-remainder", "bad-pos-unbond-sig",
    };
    return independent.contains(reject_reason);
}

int FastBlocksHooks::FinalHeight() const
{
    AssertLockHeld(::cs_main);
    ChainstateManager* chainman{Chainman()};
    if (!chainman) return -1;
    const CBlockIndex* tip{chainman->ActiveChain().Tip()};
    if (!tip) return -1;
    const std::pair<uint256, uint256> as_of{tip->GetBlockHash(), chainman->m_mainchain ? chainman->m_mainchain->TipHash() : uint256{}};
    {
        LOCK(m_final_mutex);
        if (m_final_for == as_of) return m_final_height;
    }
    const auto heights{AnchoredHeights(*this, *tip, m_params.final_anchors)};
    const int height{m_params.final_anchors > 0 && heights.size() >= static_cast<size_t>(m_params.final_anchors) ? heights[m_params.final_anchors - 1] : -1};
    LOCK(m_final_mutex);
    m_final_for = as_of;
    m_final_height = height;
    return height;
}

bool FastBlocksHooks::IsWaitReason(const std::string& reject_reason) const
{
    // A BMM block the mainchain has not committed to yet; a block of stake whose slot has not started
    // by this node's clock, or that acknowledges a commitment not on this node's record yet.
    // Or whose epoch's seed is not on record yet.
    return reject_reason == "bmm-unknown" || reject_reason == "pos-time-future" || reject_reason == "pos-ack-unknown" || reject_reason == "pos-seed-unknown";
}

std::optional<int64_t> FastBlocksHooks::WaitingReady(const CBlockHeader& header, const sidechain::Mainchain& record) const
{
    ChainstateManager* chainman{Chainman()};
    if (!chainman) return ChainHooks::WaitingReady(header, record);
    AssertLockHeld(::cs_main);
    // Ready when it is, and so is every waiting header between it and a block the node knows: then
    // its height orders it after its parent (blocks of both kinds wait, so commitment heights would not).
    const auto ready{[&](const CBlockHeader& waiting) {
        if (!IsPosHeader(waiting)) return record.CommittedHeight(waiting.GetHash()).has_value();
        if (waiting.Time() > NodeClock::now() + std::chrono::seconds{m_params.max_future_seconds}) return false;
        if (!EpochSeed(EpochOf(SlotOf(waiting.nTime, m_params), m_params), m_params, record)) return false;
        return waiting.nNonce == 0 || record.BmmAt(static_cast<int>(waiting.nNonce)).has_value();
    }};
    const CBlockHeader* current{&header};
    for (int64_t depth{1}; depth <= static_cast<int64_t>(chainman->m_bmm_waiting.size()) + 1; ++depth) {
        if (!ready(*current)) return std::nullopt;
        if (const CBlockIndex* prev{chainman->m_blockman.LookupBlockIndex(current->hashPrevBlock)}) return prev->nHeight + depth;
        const auto parent{chainman->m_bmm_waiting.find(current->hashPrevBlock)};
        if (parent == chainman->m_bmm_waiting.end()) return std::nullopt;
        current = &parent->second.header;
    }
    return std::nullopt;
}

arith_uint256 FastBlocksHooks::ChainWork(const CBlockIndex* prev, const CBlockHeader& header) const
{
    if (!prev) return ChainKey{}.Work();
    ChainKey key{ChainKey::FromWork(prev->nChainWork)};
    key.height = static_cast<uint64_t>(prev->nHeight) + 1;
    if (IsPosHeader(header)) {
        // An acknowledgement is an anchor; the mainchain height of its commitment is in the header.
        if (header.nNonce != 0) {
            ++key.anchors;
            key.last_anchor_height = header.nNonce;
        }
    } else {
        // A BMM block is an anchor, committed where the record says (the worst place if it does not).
        ++key.anchors;
        key.last_anchor_height = 0xFFFFFFFFu;
        if (const ChainstateManager* chainman{Chainman()}; chainman && chainman->m_mainchain) {
            if (const auto height{chainman->m_mainchain->BmmHeight(header.GetHash())}) key.last_anchor_height = static_cast<uint32_t>(*height);
        }
    }
    return key.Work();
}

void FastBlocksHooks::NoteAck(const uint256& block_hash, const AnchorAck& ack) const
{
    const uint256 commitment{ack.ticket.Hash()};
    LOCK(m_acks_mutex);
    if (!m_ack_of.emplace(block_hash, commitment).second) return;
    m_anchored_by.emplace(block_hash, ack.ticket.anchored);
    m_acks_by_commitment[commitment].insert(block_hash);
    m_ack_order.emplace_back(block_hash, commitment);
    // Bounded: the oldest go first. A cache only: CommitmentOf reads a block from disk when it has to,
    // and BlocksCommittedBy finds the blocks that matter from the chain, so a flood that pushes the
    // entries that count out changes nothing but speed.
    while (m_ack_order.size() > MAX_NOTED_ACKS) {
        const auto& [old_block, old_commitment] = m_ack_order.front();
        m_ack_of.erase(old_block);
        m_anchored_by.erase(old_block);
        if (auto found{m_acks_by_commitment.find(old_commitment)}; found != m_acks_by_commitment.end()) {
            found->second.erase(old_block);
            if (found->second.empty()) m_acks_by_commitment.erase(found);
        }
        m_ack_order.pop_front();
    }
}

std::optional<AnchorAck> FastBlocksHooks::ReadAck(const CBlockIndex& index) const
{
    ChainstateManager* chainman{Chainman()};
    if (!index.m_ext || index.nNonce == 0 || !chainman) return std::nullopt;
    CBlock block;
    const bool have{WITH_LOCK(::cs_main, return (index.nStatus & BLOCK_HAVE_DATA) && chainman->m_blockman.ReadBlock(block, index))};
    PosSections sections;
    if (!have || !PosSections::Decode(block.vSections, sections) || !sections.ack) return std::nullopt;
    NoteAck(index.GetBlockHash(), *sections.ack);
    return sections.ack;
}

const CBlockIndex* FastBlocksHooks::AnchoredBy(const CBlockIndex& index) const
{
    AssertLockHeld(::cs_main);
    if (!index.m_ext) return index.nHeight > 0 ? index.pprev : nullptr;
    if (index.nNonce == 0) return nullptr;
    ChainstateManager* chainman{Chainman()};
    if (!chainman) return nullptr;
    std::optional<uint256> anchored;
    {
        LOCK(m_acks_mutex);
        if (const auto found{m_anchored_by.find(index.GetBlockHash())}; found != m_anchored_by.end()) anchored = found->second;
    }
    if (!anchored) {
        const auto ack{ReadAck(index)};
        if (!ack) return nullptr;
        anchored = ack->ticket.anchored;
    }
    // On the chain of the acknowledgement, below it (a valid one names such a block).
    const CBlockIndex* target{chainman->m_blockman.LookupBlockIndex(*anchored)};
    if (!target || target->nHeight >= index.nHeight || index.GetAncestor(target->nHeight) != target) return nullptr;
    return target;
}

std::optional<uint256> FastBlocksHooks::CommitmentOf(const CBlockIndex& index) const
{
    // A BMM block needs a commitment to itself; a PoS block one to the ticket it acknowledges, if any.
    if (!index.m_ext) return index.GetBlockHash();
    if (index.nNonce == 0) return std::nullopt;
    {
        LOCK(m_acks_mutex);
        if (const auto found{m_ack_of.find(index.GetBlockHash())}; found != m_ack_of.end()) return found->second;
    }
    ChainstateManager* chainman{Chainman()};
    if (chainman) {
        if (const auto ack{ReadAck(index)}) return ack->ticket.Hash();
        // A header without its block: what the record has at the height it names.
        if (chainman->m_mainchain) {
            if (const auto bmm{chainman->m_mainchain->BmmAt(static_cast<int>(index.nNonce))}) return *bmm;
        }
    }
    // Nothing on record: a commitment nobody made.
    return index.GetBlockHash();
}

std::vector<uint256> FastBlocksHooks::BlocksCommittedBy(const uint256& commitment, std::optional<int> main_height) const
{
    std::vector<uint256> blocks{commitment};
    std::set<uint256> seen{commitment};
    const auto add{[&](const uint256& hash) {
        if (seen.insert(hash).second) blocks.push_back(hash);
    }};
    ChainstateManager* chainman{Chainman()};
    // The blocks that matter, from the chain (the acknowledgements noted in memory are bounded, and a
    // leader can sign blocks enough to push them all out: attack8_acks_pushed_out_still_taken_back).
    // Those of the active chain, which the node would keep on an anchor the mainchain dropped; and those
    // that failed against the record, which it would not take back when the commitment returns. Any
    // other block is checked against the record when it is connected.
    // Only an acknowledgement whose nNonce is `main_height` rests on the commitment (a valid one
    // acknowledges the first commitment to its ticket, at the height its nNonce is: bad-pos-ack-height),
    // and only such a block is read (review 8: reading every acknowledgement above the height and every
    // block failed against the record, for each commitment the mainchain makes or drops and each
    // ticket a producer may acknowledge, was a denial of service once a leader pushed them out of memory).
    if (chainman && main_height && *main_height > 0) {
        LOCK(::cs_main);
        const auto rests{[&](const CBlockIndex& index) {
            return index.m_ext && static_cast<int64_t>(index.nNonce) == *main_height && CommitmentOf(index) == commitment;
        }};
        // A commitment to a BMM block of ours: that block alone rests on it (acknowledgements name tickets).
        const CBlockIndex* own{chainman->m_blockman.LookupBlockIndex(commitment)};
        // On the active chain, the acknowledgements are in the order of the mainchain, strictly: the
        // state's bad-pos-ack-order, on nNonce, which the block's hash covers. From the tip down to the
        // first one below the height. Not stopped by a BMM block: where the record has it now is not
        // where it had it when the block was connected (review 8: a mainchain reorg that commits again,
        // lower, to a BMM block above the acknowledgement stopped the walk above it, a split between
        // the nodes that had it noted and the others). Visited: the blocks above the last
        // acknowledgement below the height -- each BMM block among them a commitment of its own, and
        // at most max_unanchored_slots blocks of stake after each anchor (bad-pos-unanchored).
        for (const CBlockIndex* index{own ? nullptr : chainman->ActiveChain().Tip()}; index && index->nHeight > 0; index = index->pprev) {
            if (!index->m_ext || index->nNonce == 0) continue;
            if (static_cast<int64_t>(index->nNonce) < *main_height) break;
            if (rests(*index)) {
                add(index->GetBlockHash());
                break;
            }
        }
        // Those that failed against the record, whatever chain they are on: from the block index,
        // read only if their nNonce is the height.
        if (chainman->m_mainchain) {
            for (const uint256& hash : chainman->m_mainchain->RecordFailures()) {
                const CBlockIndex* index{chainman->m_blockman.LookupBlockIndex(hash)};
                if (index && rests(*index)) add(hash);
            }
        }
    }
    // And the others noted: blocks that read and that the leader of their slot signed.
    LOCK(m_acks_mutex);
    if (const auto found{m_acks_by_commitment.find(commitment)}; found != m_acks_by_commitment.end()) {
        for (const uint256& hash : found->second) add(hash);
    }
    return blocks;
}

bool FastBlocksHooks::CheckExtension(const sidechain::State& state, const CBlock& block, std::string& reject_reason) const
{
    if (!IsPosHeader(block)) return true;
    ChainstateManager* chainman{Chainman()};
    if (!chainman) return true;
    LOCK(::cs_main);
    const CBlockIndex* prev{chainman->m_blockman.LookupBlockIndex(block.hashPrevBlock)};
    if (!prev) return true;
    // Signed by the leader of its slot, before the record is read (GetPegStep): a forged signature is
    // the copy's failure (ExtensionFailure), not the block's against the record. Only where the state
    // before the block knows the leader; AfterBlock checks again with the state after it, which knows
    // no other leader (a block adds the set of its own anchor, which leads no slot of its own epoch,
    // and prunes only sets no later slot needs): this fails no block AfterBlock would take.
    const SlotLeader leader{LeaderOf(*prev, SlotOf(block.nTime, m_params), &state.View())};
    if (leader.status == LeaderStatus::OK && !CheckBlockSig(block, leader.pubkey, Genesis())) return Fail(reject_reason, "bad-pos-sig");
    return true;
}

bool FastBlocksHooks::GetPegStep(const CBlock& block, const sidechain::Mainchain& mainchain, std::optional<sidechain::PegStep>& step, std::string& reject_reason) const
{
    if (!IsPosHeader(block)) return ChainHooks::GetPegStep(block, mainchain, step, reject_reason);
    PosSections sections;
    if (!PosSections::Decode(block.vSections, sections)) return Fail(reject_reason, "bad-pos-sections");
    step.reset();
    // A PoS block without an acknowledgement takes no peg actions.
    if (!sections.ack) return true;
    const AnchorAck& ack{*sections.ack};
    // The mainchain committed to the ticket, first, at the height the acknowledgement says (on record:
    // what block assembly assumes is for the block's own hash).
    const auto committed{mainchain.CommittedHeight(ack.ticket.Hash())};
    if (!committed) return Fail(reject_reason, "bmm-unknown");
    if (*committed != ack.main_height) return Fail(reject_reason, "bad-pos-ack-height");
    ChainstateManager* chainman{Chainman()};
    if (!chainman) return Fail(reject_reason, "bad-pos-context");
    LOCK(::cs_main);
    const CBlockIndex* anchored{chainman->m_blockman.LookupBlockIndex(ack.ticket.anchored)};
    const CBlockIndex* prev{chainman->m_blockman.LookupBlockIndex(block.hashPrevBlock)};
    if (!anchored || !prev) return Fail(reject_reason, "bad-pos-ack-anchored");
    step = AckPegStep(ack, *anchored, *prev);
    return true;
}

sidechain::PegStep FastBlocksHooks::AckPegStep(const AnchorAck& ack, const CBlockIndex& anchored, const CBlockIndex& prev) const
{
    AssertLockHeld(::cs_main);
    // The block that took the last peg actions on the chain below: the newest acknowledgement or BMM
    // block (the state's anchor point).
    const CBlockIndex* point{&prev};
    while (point && point->m_ext && point->nNonce == 0) point = point->pprev;
    // As if the block followed the mainchain up to the block before the commitment, with the bundle
    // built from the block anchored: its withdrawals, its hash as the nonce, its height as the lock
    // time and as the height a bundle failure counts from (design 4.3.4: every block that
    // acknowledges the same anchor builds the same bundle, whatever its own height). That holds only
    // if no peg actions came between the block anchored and this one (refunds, another bundle): a
    // ticket made before the last commitment was acknowledged anchors a block below that
    // acknowledgement, and its anchor builds no bundle (no withdrawal is at or below height -1).
    const bool after_point{!point || AncestorAt(&anchored, point->nHeight) == point};
    return sidechain::PegStep{.main_height = ack.main_height - 1, .nonce = anchored.GetBlockHash(), .max_withdrawal_height = after_point ? anchored.nHeight : -1, .peg_height = anchored.nHeight};
}

bool FastBlocksHooks::BeforePegStep(sidechain::State& state, const CBlock& block, int height, const sidechain::PegStep& step, const Consensus::SidechainParams&,
                                    std::vector<CTxOut>& owed_tx, std::string& reject_reason) const
{
    ChainstateManager* chainman{Chainman()};
    if (!chainman) return Fail(reject_reason, "bad-pos-context");
    FastState fast{FAST_STATE.Get(state.View())};
    CScript payee;
    uint256 anchored;
    int32_t anchored_height;
    int32_t main_height;
    // (Block assembly gives the block without the PoS header bit, but with its sections: only PoS blocks have any.)
    if (IsPosHeader(block) || !block.vSections.empty()) {
        PosSections sections;
        if (!PosSections::Decode(block.vSections, sections) || !sections.ack) return Fail(reject_reason, "bad-pos-sections");
        const AnchorAck& ack{*sections.ack};
        // Anchors are acknowledged in the order the mainchain committed to them.
        if (ack.main_height <= fast.last_main_height) return Fail(reject_reason, "bad-pos-ack-order");
        LOCK(::cs_main);
        const CBlockIndex* prev{chainman->m_blockman.LookupBlockIndex(block.hashPrevBlock)};
        const CBlockIndex* target{chainman->m_blockman.LookupBlockIndex(ack.ticket.anchored)};
        // The block anchored is on this chain, at or below the parent.
        if (!target || !Descends(prev, target)) return Fail(reject_reason, "bad-pos-ack-anchored");
        // ... and it is, or comes after, the block the last anchor anchored: anchors cover the chain in
        // order. (It may be below the block that took the last peg actions: a ticket made for the
        // next mainchain block before the last commitment was acknowledged. Its anchor then builds
        // no bundle: AckPegStep.)
        if (fast.last_anchored_height >= 0) {
            const CBlockIndex* last{AncestorAt(target, fast.last_anchored_height)};
            if (!last || last->GetBlockHash() != fast.last_anchored) return Fail(reject_reason, "bad-pos-ack-chain");
        }
        payee = ack.ticket.payee;
        anchored = target->GetBlockHash();
        anchored_height = target->nHeight;
        main_height = ack.main_height;
    } else {
        // A BMM block anchors its parent, and its coinbase's first output takes the pot.
        payee = block.vtx.at(0)->vout.empty() ? CScript{} : block.vtx[0]->vout[0].scriptPubKey;
        anchored = block.hashPrevBlock;
        anchored_height = height - 1;
        main_height = step.main_height + 1;
    }

    // The pot accrued up to the block anchored, less what was paid, goes to the payee.
    sidechain::StoreOverlay& out{state.Writable()};
    const int64_t accrued{PotAccruedAt(state.View(), anchored_height)};
    // (At least what was paid: PotAccruedAt starts there, and the pot only grows.)
    const int64_t pay{accrued - fast.pot_paid};
    std::vector<uint32_t> paid_entries;
    POT_ACCRUED.ForEach(state.View(), [&](const uint32_t& at, const int64_t&) {
        if (at > static_cast<uint32_t>(anchored_height)) return false;
        paid_entries.push_back(at);
        return true;
    });
    for (const uint32_t at : paid_entries) POT_ACCRUED.Erase(out, at);
    fast.pot_paid = accrued;

    ++fast.anchors;
    fast.last_main_height = main_height;
    fast.last_anchored = anchored;
    fast.last_anchored_height = anchored_height;
    fast.anchor_point = block.GetHash();
    fast.anchor_point_height = height;
    FAST_STATE.Put(out, fast);

    // Staking: unbonded coins and rewards for proofs paid, a share of the penalties with the pot,
    // validators that missed their slots jailed, and the validator set of this anchor made. (A BMM
    // block's time may be ahead of the clock, by two hours at most: its epoch at the earliest, then,
    // for liveness and pruning, which must count only epochs that are over.)
    const int64_t slot{SlotOf(block.nTime, m_params)};
    const bool pos_block{IsPosHeader(block) || !block.vSections.empty()};
    const int64_t earliest{pos_block ? slot : std::max<int64_t>(0, slot - MAX_FUTURE_BLOCK_TIME / m_params.slot_seconds)};
    // The anchor's slot, for the epochs whose set it makes (SetFor): not before the median time of the
    // mainchain block that committed to it (AnchorSlot), which whoever makes the block cannot set back.
    // The set is made as of the epoch of that median time (or of `earliest`, if later), which they
    // cannot move ahead either, and which is not ahead of the clock: not as of `earliest`, which for
    // a BMM block is two hours before its time (review of audit 6, item 9: a validator whose jail was
    // over stayed out of every set for two hours, and blocks of stake could not start again).
    int64_t anchor_slot{slot};
    int64_t set_epoch{EpochOf(earliest, m_params)};
    if (const auto main{chainman->m_mainchain ? chainman->m_mainchain->GetBlock(main_height) : std::nullopt}) {
        anchor_slot = std::max(anchor_slot, SlotOf(main->time, m_params));
        set_epoch = std::max(set_epoch, EpochOf(SlotOf(main->time, m_params), m_params));
    }
    // The validator pool of the epochs that are over (before the earliest epoch of the anchor), with
    // their sets read before the staking peg actions, which may prune them (fee split).
    std::vector<std::pair<int64_t, std::shared_ptr<const ValidatorSet>>> pool_sets;
    if (FeeSplitActive(height)) {
        LOCK(::cs_main);
        const CBlockIndex* prev{chainman->m_blockman.LookupBlockIndex(block.hashPrevBlock)};
        for (const auto& [epoch, amount] : PoolEntries(state.View())) {
            if (epoch >= EpochOf(earliest, m_params) || pool_sets.size() >= MAX_POOL_EPOCHS_PER_STEP) break;
            pool_sets.emplace_back(epoch, prev ? SetFor(*prev, epoch, &state.View()) : nullptr);
        }
    }
    std::vector<CTxOut> staking_owed;
    const auto staked{m_staking.PegStep(out, block.GetHash(), fast.anchors, anchor_slot, EpochOf(earliest, m_params), set_epoch, staking_owed)};
    // Each epoch's pool shared by stake among the validators of its set not jailed or banned now, to
    // their bond owners (P2WPKH of the owner key hash); a genesis validator without a bond's share to
    // the reserve; what rounding leaves, or a pool no validator can take, carried to the next epoch.
    std::vector<CTxOut> pool_shares;
    for (const auto& [epoch, set] : pool_sets) {
        const int64_t amount{POOL.Get(out, static_cast<uint64_t>(epoch)).value_or(0)};
        POOL.Erase(out, static_cast<uint64_t>(epoch));
        std::vector<PoolMember> members;
        if (set) {
            for (const Consensus::PosValidator& validator : set->Validators()) {
                const auto record{GetValidator(out, validator.pubkey)};
                PoolMember member{.pubkey = validator.pubkey, .weight = validator.stake, .excluded = false, .payee = std::nullopt};
                member.excluded = !record || record->tombstoned || record->jailed_until_epoch > set_epoch;
                if (record && !record->owner.IsNull()) member.payee = GetScriptForDestination(WitnessV0KeyHash{record->owner});
                members.push_back(std::move(member));
            }
        }
        const PoolDistribution paid{DistributePool(amount, members)};
        pool_shares.insert(pool_shares.end(), paid.shares.begin(), paid.shares.end());
        m_staking.ToReserve(out, paid.to_reserve);
        // (To the pool of the anchor's earliest epoch, the next one not over: an old epoch's remainder
        // does not walk an epoch an anchor, behind the others.)
        AddToPool(out, std::max(epoch + 1, EpochOf(earliest, m_params)), paid.remainder);
    }
    const int64_t paid_out{pay + staked.release};
    // A BMM block's first coinbase output takes the pot only if it is an address (IsRewardPayee): a
    // payout, when the queue is behind, is owed at later peg steps, and a script no block can carry
    // would stop them all (audit 7, finding 1). Else the pot goes to the reserve, which pays it out
    // with the pots to come. (A ticket's payee is checked when the ticket is: bad-pos-ticket-payee.)
    if (paid_out > 0 && !pos_block && !IsRewardPayee(payee)) {
        m_staking.ToReserve(out, paid_out);
    } else if (paid_out > 0) {
        owed_tx.emplace_back(paid_out, payee);
    }
    owed_tx.insert(owed_tx.end(), staking_owed.begin(), staking_owed.end());
    // The shares of the pool last, after a marker that counts them (for wallets: CoinbaseRoles).
    if (!pool_shares.empty()) {
        owed_tx.emplace_back(0, PoolSharesMarker(static_cast<uint32_t>(pool_shares.size())));
        owed_tx.insert(owed_tx.end(), pool_shares.begin(), pool_shares.end());
    }
    return true;
}

bool FastBlocksHooks::ApplyTx(sidechain::State& state, const CTransaction& tx, int height, bool, std::vector<CTxOut>&, std::string& reject_reason) const
{
    // Bonds and unbonds: an unbond is paid unbonding_anchors after the anchors of the chain so far.
    ++m_tx_rule_checks;
    const FastState fast{FAST_STATE.Get(state.View())};
    return m_staking.ApplyTx(state.Writable(), tx, height, fast.anchors, reject_reason, fast.anchor_point);
}

std::optional<std::vector<std::vector<unsigned char>>> FastBlocksHooks::TxRuleKeys(const CTransaction& tx) const { return ValidatorRuleKeys(tx); }

bool FastBlocksHooks::HasTxRules(const CTransaction& tx) const
{
    return std::any_of(tx.vout.begin(), tx.vout.end(), [](const CTxOut& out) { return IsBondScript(out.scriptPubKey) || IsUnbondScript(out.scriptPubKey); });
}

bool FastBlocksHooks::Stalled(const FastState& fast, int64_t main_time, bool pos_active) const
{
    if (!pos_active) return true;
    const int64_t last{fast.last_pos_slot >= 0 ? SlotTime(fast.last_pos_slot, m_params) : m_params.genesis_time};
    return main_time - last >= m_params.stall_seconds;
}

bool FastBlocksHooks::AfterBlock(sidechain::State& state, const CBlock& block, int height, const std::optional<sidechain::PegStep>& step, CAmount minted,
                                 const Consensus::SidechainParams&, std::string& reject_reason) const
{
    ChainstateManager* chainman{Chainman()};
    if (!chainman) return Fail(reject_reason, "bad-pos-context");
    sidechain::StoreOverlay& out{state.Writable()};
    FastState fast{FAST_STATE.Get(state.View())};
    const CTransaction& coinbase{*block.vtx.at(0)};
    LOCK(::cs_main);
    const CBlockIndex* prev{chainman->m_blockman.LookupBlockIndex(block.hashPrevBlock)};
    if (!prev) return Fail(reject_reason, "bad-pos-context");
    const int64_t slot{SlotOf(block.nTime, m_params)};
    if (IsPosHeader(block)) {
        // Signed by the leader of its slot, as this chain and the record say now: the header may have
        // come before the node knew the set of its epoch (CheckHeader), and the record may have moved
        // the seed since.
        const SlotLeader leader{LeaderOf(*prev, slot, &state.View())};
        switch (leader.status) {
        case LeaderStatus::INACTIVE: return Fail(reject_reason, "bad-pos-inactive");
        case LeaderStatus::SEED_UNKNOWN: return Fail(reject_reason, "pos-seed-unknown");
        case LeaderStatus::SET_UNKNOWN: return Fail(reject_reason, "bad-pos-set-unknown");
        case LeaderStatus::OK: break;
        }
        if (!CheckBlockSig(block, leader.pubkey, Genesis())) return Fail(reject_reason, "bad-pos-sig");
        m_checked_signer = {block.GetHash(), leader.pubkey};
        // A banned leader leads no more: from the block after the one that punished it, its slots
        // stay empty (the sets made before its ban still name it; the state of the chain below the
        // block says it is banned, so every node agrees). Its slots are not given to others: who
        // carries a proof would otherwise choose the leaders of the slots to come.
        if (const auto record{GetValidator(state.View(), leader.pubkey)}; record && record->tombstoned) return Fail(reject_reason, "bad-pos-leader-banned");

        CAmount total{0}, pot{0}, pool{0};
        int pools{0};
        for (const CTxOut& txout : coinbase.vout) {
            total += txout.nValue;
            if (IsPotScript(txout.scriptPubKey)) pot += txout.nValue;
            if (IsPoolScript(txout.scriptPubKey)) {
                pool += txout.nValue;
                ++pools;
            }
        }
        if (FeeSplitActive(height)) {
            // The fee split (pos/feesplit.h): the producer keeps nothing; the fees are the pot's (the
            // bidder's) and the validator pool's, as CheckFees checks against the fees. One pool output.
            if (pools > 1) return Fail(reject_reason, "bad-pos-pool-multiple");
            if (total - minted - pot - pool != 0) return Fail(reject_reason, "bad-pos-producer-share");
            AddToPool(out, EpochOf(slot, m_params), pool);
        } else {
            // Before it: what the producer keeps (all of the coinbase but the payouts and the pot) is at
            // most producer_share_bps of it and the pot together. Fees it leaves unclaimed are lost.
            // (Not negative: the payouts and the pot are outputs of the coinbase, which has no negative ones.)
            const CAmount take{total - minted - pot};
            arith_uint256 kept{static_cast<uint64_t>(take)}, share{static_cast<uint64_t>(take + pot)};
            kept *= arith_uint256{10000};
            share *= arith_uint256{static_cast<uint64_t>(m_params.producer_share_bps)};
            if (kept > share) return Fail(reject_reason, "bad-pos-producer-share");
        }
        if (pot > 0) {
            fast.pot_accrued += pot;
            POT_ACCRUED.Put(out, static_cast<uint32_t>(height), fast.pot_accrued);
        }

        // PoS blocks stop when no anchor covers them for too long: the first one no anchor covers
        // starts the count. An acknowledgement covers what is at or below the block it anchors.
        const int64_t before_step_unanchored{fast.first_unanchored_slot};
        if (step) {
            int64_t first{slot};
            for (const CBlockIndex* index{prev}; index && index->nHeight > fast.last_anchored_height; index = index->pprev) {
                if (index->m_ext) first = SlotOf(index->nTime, m_params);
            }
            fast.first_unanchored_slot = first;
        } else if (fast.first_unanchored_slot < 0) {
            fast.first_unanchored_slot = slot;
        }
        if (slot - fast.first_unanchored_slot >= m_params.max_unanchored_slots) return Fail(reject_reason, "bad-pos-unanchored");

        // Liveness: the block's leader made it; the leaders of the slots since the last block of stake
        // (an epoch of them at most) made none -- of the slots at which a block could be made: not
        // those max_unanchored_slots or more after the first block of stake no anchor covered (blocks
        // of stake had stopped for want of anchors), nor those up to a BMM block the block follows
        // (which may end such a stop, or a stall, and whose slot is the earliest the leaders could
        // build on it). Leaders are not jailed for slots nobody could fill.
        int64_t charged_from{std::max(fast.last_pos_slot + 1, slot - m_params.slots_per_epoch)};
        int64_t charged_until{slot};
        if (prev->nHeight > 0 && !prev->m_ext) {
            charged_from = std::max(charged_from, SlotOf(prev->nTime, m_params) + 1);
        } else if (before_step_unanchored >= 0) {
            charged_until = std::min(charged_until, before_step_unanchored + m_params.max_unanchored_slots);
        }
        std::vector<std::pair<int64_t, uint256>> missed;
        std::map<int64_t, SlotLeader> epochs;
        for (int64_t at{charged_from}; at < charged_until; ++at) {
            const int64_t epoch{EpochOf(at, m_params)};
            auto found{epochs.find(epoch)};
            if (found == epochs.end()) found = epochs.emplace(epoch, LeaderOf(*prev, epoch * m_params.slots_per_epoch, &state.View())).first;
            const SlotLeader& schedule{found->second};
            if (schedule.status != LeaderStatus::OK) continue;
            if (const auto index{schedule.set->Leader(at, schedule.seed)}) missed.emplace_back(at, schedule.set->Validators()[*index].pubkey);
        }
        // The seeds the chain below read agree with the record: else a mainchain reorg moved them, and
        // the blocks that read them are to be connected again first (CheckSeeds, next): the block waits
        // (a failure against the record, given back then). So the seeds of the state are those of one
        // record, and CheckSeeds can stop at the first that has not moved. (The newest: that of the last
        // block of stake's epoch. If it has not moved, nor have those before it.)
        if (fast.last_pos_slot >= 0) {
            const int64_t last_epoch{EpochOf(fast.last_pos_slot, m_params)};
            if (const auto used{EPOCH_SEEDS.Get(state.View(), static_cast<uint64_t>(last_epoch))}) {
                const auto now{chainman->m_mainchain ? EpochSeed(last_epoch, m_params, *chainman->m_mainchain) : std::nullopt};
                if (now && *now != *used) return Fail(reject_reason, "pos-seed-moved");
            }
        }
        m_staking.NoteLiveness(out, slot, leader.pubkey, missed);
        fast.last_pos_slot = slot;
        // The seeds read, as the record gave them (CheckSeeds), of every epoch.
        {
            std::map<int64_t, uint256> read{{EpochOf(slot, m_params), leader.seed}};
            for (const auto& [charged, schedule] : epochs) {
                if (schedule.status == LeaderStatus::OK) read.emplace(charged, schedule.seed);
            }
            for (const auto& [at, seed] : read) {
                if (EPOCH_SEEDS.Get(state.View(), static_cast<uint64_t>(at)) != seed) EPOCH_SEEDS.Put(out, static_cast<uint64_t>(at), seed);
            }
        }

        // Proofs that leaders signed twice: punished, and the block paid for them.
        PosSections sections;
        if (!PosSections::Decode(block.vSections, sections)) return Fail(reject_reason, "bad-pos-sections");
        for (const DoubleProposal& proof : sections.evidence) {
            const auto offence{CheckChainEvidence(proof, *prev, slot, state.View(), fast.anchors, reject_reason)};
            if (!offence) return false;
            // The offender carrying the proof against itself is paid nothing for it.
            const std::optional<CScript> reporter{offence->pubkey == leader.pubkey ? std::nullopt : std::optional<CScript>{coinbase.vout.at(0).scriptPubKey}};
            if (!m_staking.Punish(out, *offence, reporter, fast.anchors, reject_reason)) return false;
            LogDebug(BCLog::VALIDATION, "FastBlocks: validator %s punished for signing two blocks at slot %d\n", HexStr(offence->pubkey), offence->slot);
        }
        if (sections.ack) NoteAck(block.GetHash(), *sections.ack);
    } else {
        // A BMM block carries transactions only when the PoS blocks stalled: no active validator set
        // for its epoch, or, as of the mainchain block before its commitment, none for stall_seconds.
        if (block.vtx.size() > 1) {
            const auto set{SetFor(*prev, EpochOf(slot, m_params), &state.View())};
            const bool pos_active{!set || set->Active()};
            // (A BMM block's commitment is on record, so is the block before.)
            const auto main{step && chainman->m_mainchain ? chainman->m_mainchain->GetBlock(step->main_height) : std::nullopt};
            if (pos_active && (!main || !Stalled(fast, main->time, pos_active))) return Fail(reject_reason, "bad-pos-bmm-not-stalled");
        }
        // The fee split: the validators' share of its fees in one pool output (CheckFees checks the amount).
        if (FeeSplitActive(height)) {
            CAmount pool{0};
            int pools{0};
            for (const CTxOut& txout : coinbase.vout) {
                if (!IsPoolScript(txout.scriptPubKey)) continue;
                pool += txout.nValue;
                ++pools;
            }
            if (pools > 1) return Fail(reject_reason, "bad-pos-pool-multiple");
            AddToPool(out, EpochOf(slot, m_params), pool);
        }
        // Proofs in its coinbase, as in a block of stake: punished, and its payee (the coinbase's first
        // output, which the pot is paid to) paid for them at the next peg step.
        for (const CTxOut& txout : coinbase.vout) {
            if (!IsEvidenceScript(txout.scriptPubKey)) continue;
            const auto proof{ParseEvidenceScript(txout.scriptPubKey)};
            if (!proof) return Fail(reject_reason, "bad-pos-evidence-output");
            const auto offence{CheckChainEvidence(*proof, *prev, slot, state.View(), fast.anchors, reject_reason)};
            if (!offence) return false;
            if (!m_staking.Punish(out, *offence, coinbase.vout.at(0).scriptPubKey, fast.anchors, reject_reason)) return false;
            LogDebug(BCLog::VALIDATION, "FastBlocks: validator %s punished for signing two blocks at slot %d (proof in a BMM block)\n", HexStr(offence->pubkey), offence->slot);
        }
        // It anchors everything below it.
        fast.first_unanchored_slot = -1;
    }
    FAST_STATE.Put(out, fast);
    return true;
}

std::optional<int> FastBlocksHooks::FeeSplitMainHeight(bool pos_block, int64_t slot, int bmm_main_height) const
{
    if (!pos_block) return bmm_main_height;
    const ChainstateManager* chainman{Chainman()};
    if (!chainman || !chainman->m_mainchain) return std::nullopt;
    return SeedHeight(EpochOf(slot, m_params), m_params, *chainman->m_mainchain);
}

int64_t FastBlocksHooks::FeeSplitSpacing(int main_height) const
{
    const ChainstateManager* chainman{Chainman()};
    if (!chainman || !chainman->m_mainchain) return FEE_SPLIT_TARGET_SPACING;
    return MedianSpacingAt(*chainman->m_mainchain, main_height, m_params.main_activation_height);
}

bool FastBlocksHooks::CheckFees(const sidechain::State&, const CBlock& block, int height, const std::optional<sidechain::PegStep>& step,
                                const std::vector<CAmount>& fees, std::string& reject_reason) const
{
    if (!FeeSplitActive(height)) return true;
    const bool pos_block{IsPosHeader(block)};
    // (Every merged mined block takes the peg actions; a block of stake's seed is known: AfterBlock read its leader.)
    if (!pos_block && !step) return Fail(reject_reason, "bad-pos-context");
    const auto main_height{FeeSplitMainHeight(pos_block, SlotOf(block.nTime, m_params), step ? step->main_height : -1)};
    if (!main_height) return Fail(reject_reason, "pos-seed-unknown");
    const FeeSplit split{SplitFees(block.vtx, fees, /*fees_without_coinbase=*/false, FeeSplitSpacing(*main_height), FeeCurveAt(height))};
    CAmount pot{0}, pool{0};
    for (const CTxOut& txout : block.vtx.at(0)->vout) {
        if (IsPotScript(txout.scriptPubKey)) pot += txout.nValue;
        if (IsPoolScript(txout.scriptPubKey)) pool += txout.nValue;
    }
    // The validators' share to the pool, exactly; for a block of stake the bidder's to the pot (its
    // producer keeps nothing: AfterBlock). A merged miner keeps the bidder's share itself (bad-cb-amount).
    if (pool != split.validators) return Fail(reject_reason, "bad-pos-fee-split");
    if (pos_block && pot != split.Bidder()) return Fail(reject_reason, "bad-pos-fee-split");
    return true;
}

CAmount FastBlocksHooks::MergeMinedFeeOutputs(const CBlock& block, int height, int main_height, const std::vector<CAmount>& fees, std::vector<CTxOut>& outputs) const
{
    if (!FeeSplitActive(height)) return 0;
    const FeeSplit split{SplitFees(block.vtx, fees, /*fees_without_coinbase=*/true, FeeSplitSpacing(main_height), FeeCurveAt(height))};
    if (split.validators > 0) outputs.emplace_back(split.validators, PoolScript());
    return split.validators;
}

void FastBlocksHooks::ForgetKnownSets() const
{
    LOCK(m_sets_mutex);
    m_known_sets.clear();
    m_known_order.clear();
}

std::shared_ptr<const ValidatorSet> FastBlocksHooks::AnchorSetOf(const uint256& anchor, const sidechain::StoreView& view, uint64_t* set_anchors, bool from_state) const
{
    // From memory only for a header (the active chain's state is no state of its branch): the rules
    // of a block read the state of its chain, so that what a node holds in memory -- a set the state
    // pruned since, or one an earlier record made (CheckSeeds) -- does not change the outcome.
    if (!from_state) {
        LOCK(m_sets_mutex);
        if (const auto found{m_known_sets.find(anchor)}; found != m_known_sets.end()) {
            if (set_anchors) *set_anchors = found->second.anchors;
            return found->second.set;
        }
    }
    const auto entry{GetAnchorSet(view, anchor)};
    if (!entry) return nullptr;
    const auto validators{GetSetVersion(view, entry->version)};
    if (!validators) return nullptr;
    auto set{std::make_shared<const ValidatorSet>(*validators, m_params)};
    if (set_anchors) *set_anchors = entry->anchors;
    // Kept in memory, for the headers of branches that are not active (bounded: the oldest go first).
    LOCK(m_sets_mutex);
    if (const auto [found, inserted]{m_known_sets.insert_or_assign(anchor, KnownSet{.set = set, .anchors = entry->anchors})}; inserted) {
        m_known_order.push_back(anchor);
        while (m_known_order.size() > MAX_KNOWN_SETS) {
            m_known_sets.erase(m_known_order.front());
            m_known_order.pop_front();
        }
    }
    return set;
}

int64_t FastBlocksHooks::AnchorSlot(const CBlockIndex& anchor) const
{
    int64_t time{anchor.nTime};
    if (const ChainstateManager* chainman{Chainman()}; chainman && chainman->m_mainchain) {
        const sidechain::Mainchain& record{*chainman->m_mainchain};
        const auto height{anchor.m_ext ? std::optional<int>{static_cast<int>(anchor.nNonce)} : record.BmmHeight(anchor.GetBlockHash())};
        if (const auto main{height ? record.GetBlock(*height) : std::nullopt}) time = std::max<int64_t>(time, main->time);
    }
    return SlotOf(time, m_params);
}

std::shared_ptr<const ValidatorSet> FastBlocksHooks::SetFor(const CBlockIndex& prev, int64_t epoch, const sidechain::StoreView* view, uint64_t* set_anchors,
                                                            const CBlockIndex** anchor_out, std::optional<uint64_t> within) const
{
    AssertLockHeld(::cs_main);
    // The newest anchor at or below prev whose slot is before the epoch before: the stake was fixed an
    // epoch before the seed of the epoch exists.
    const int64_t boundary{(epoch - 1) * m_params.slots_per_epoch};
    uint64_t passed{0};
    for (const CBlockIndex* index{&prev}; index; index = index->pprev) {
        ++m_index_walks;
        if (index->nHeight == 0) {
            if (set_anchors) *set_anchors = 0;
            return std::shared_ptr<const ValidatorSet>{std::shared_ptr<const ValidatorSet>{}, &m_genesis_set};
        }
        const bool anchor{!index->m_ext || index->nNonce != 0};
        if (!anchor) continue;
        if (SlotOf(index->nTime, m_params) >= boundary || AnchorSlot(*index) >= boundary) {
            // (Each anchor counts one in the state: the set of one further down has more than `within`
            // anchors after it, here.)
            if (within && ++passed > *within) return nullptr;
            continue;
        }
        if (anchor_out) *anchor_out = index;
        if (view) return AnchorSetOf(index->GetBlockHash(), *view, set_anchors, /*from_state=*/true);
        ChainstateManager* chainman{Chainman()};
        if (!chainman) return nullptr;
        return AnchorSetOf(index->GetBlockHash(), chainman->ActiveChainstate().SideCache(), set_anchors, /*from_state=*/false);
    }
    return nullptr;
}

FastBlocksHooks::SlotLeader FastBlocksHooks::LeaderOf(const CBlockIndex& prev, int64_t slot, const sidechain::StoreView* view, std::optional<uint64_t> within) const
{
    AssertLockHeld(::cs_main);
    SlotLeader leader;
    const int64_t epoch{EpochOf(slot, m_params)};
    leader.set = SetFor(prev, epoch, view, &leader.set_anchors, &leader.set_anchor, within);
    if (!leader.set) {
        leader.status = LeaderStatus::SET_UNKNOWN;
        return leader;
    }
    if (!leader.set->Active()) {
        leader.status = LeaderStatus::INACTIVE;
        return leader;
    }
    const ChainstateManager* chainman{Chainman()};
    const auto seed{chainman && chainman->m_mainchain ? EpochSeed(epoch, m_params, *chainman->m_mainchain) : std::nullopt};
    if (!seed) {
        leader.status = LeaderStatus::SEED_UNKNOWN;
        return leader;
    }
    leader.seed = *seed;
    const auto index{leader.set->Leader(slot, *seed)};
    if (!index) {
        leader.status = LeaderStatus::INACTIVE;
        return leader;
    }
    leader.pubkey = leader.set->Validators()[*index].pubkey;
    leader.status = LeaderStatus::OK;
    return leader;
}

FastBlocksHooks::SlotLeader FastBlocksHooks::ActiveLeaderOf(int64_t slot) const
{
    ChainstateManager* chainman{Chainman()};
    if (!chainman) return {};
    LOCK(::cs_main);
    const CBlockIndex* tip{chainman->ActiveChain().Tip()};
    if (!tip) return {};
    return LeaderOf(*tip, slot);
}

bool FastBlocksHooks::PosActiveNow() const
{
    ChainstateManager* chainman{Chainman()};
    if (!chainman) return m_genesis_set.Active();
    const int64_t slot{SlotOf(TicksSinceEpoch<std::chrono::seconds>(NodeClock::now()), m_params)};
    LOCK(::cs_main);
    const CBlockIndex* tip{chainman->ActiveChain().Tip()};
    if (!tip) return m_genesis_set.Active();
    const auto set{SetFor(*tip, EpochOf(slot, m_params))};
    return set && set->Active();
}

std::optional<Offence> FastBlocksHooks::CheckChainEvidence(const DoubleProposal& proof, const CBlockIndex& prev, int64_t slot, const sidechain::StoreView& view,
                                                           uint64_t anchors, std::string& reason, size_t* sig_budget) const
{
    AssertLockHeld(::cs_main);
    const auto fail{[&](const char* why) -> std::optional<Offence> {
        reason = why;
        return std::nullopt;
    }};
    if (!CheckEvidenceForm(proof, m_params, reason)) return std::nullopt;
    const int64_t offence_slot{SlotOf(proof.a.nTime, m_params)};
    if (offence_slot > slot) return fail("bad-pos-evidence-late");
    // The leader of the slot on this chain. (Not walking further down than the window reaches: a slot
    // anyone can name, far below the block, would otherwise cost a walk down the whole chain.)
    const SlotLeader leader{LeaderOf(prev, offence_slot, &view, EvidenceWithin())};
    switch (leader.status) {
    case LeaderStatus::SEED_UNKNOWN: return fail("pos-seed-unknown");
    case LeaderStatus::SET_UNKNOWN: return fail("bad-pos-evidence-old");
    case LeaderStatus::INACTIVE: return fail("bad-pos-evidence-sig");
    case LeaderStatus::OK: break;
    }
    // Within the window: while the stake it signed with is still bonded or unbonding.
    if (anchors > leader.set_anchors + static_cast<uint64_t>(m_params.evidence_window_anchors)) return fail("bad-pos-evidence-old");
    // Both signed by the leader (checked once per proof and leader; within the caller's budget).
    if (sig_budget && !m_evidence.SigsKnown(proof, leader.pubkey)) {
        if (*sig_budget == 0) return fail("pos-evidence-later");
        --*sig_budget;
    }
    if (!m_evidence.CheckSigs(proof, leader.pubkey, Genesis())) return fail("bad-pos-evidence-sig");
    const std::optional<Offence> offence{Offence{.pubkey = leader.pubkey, .slot = offence_slot}};
    if (OffencePunished(view, offence->pubkey, offence->slot)) return fail("bad-pos-evidence-known");
    // Slashed once: no proof against a banned validator takes anything more.
    if (const auto record{GetValidator(view, offence->pubkey)}; record && record->tombstoned) return fail("bad-pos-evidence-banned");
    return offence;
}

void FastBlocksHooks::CheckSeeds() const
{
    ChainstateManager* chainman{Chainman()};
    if (!chainman || !chainman->m_mainchain) return;
    const sidechain::Mainchain& record{*chainman->m_mainchain};
    const uint256 main_tip{record.TipHash()};
    {
        LOCK(m_sets_mutex);
        if (m_seeds_checked_for == main_tip) return;
        m_seeds_checked_for = main_tip;
    }
    // Whether the record agrees with the leader of a block of stake (the set of its epoch is that of
    // the chain below it, which the record does not change).
    const auto agrees{[&](const CBlockIndex& index) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        const SlotLeader leader{LeaderOf(*index.pprev, SlotOf(index.nTime, m_params))};
        if (leader.status == LeaderStatus::SET_UNKNOWN) return true;
        return leader.status == LeaderStatus::OK && CheckBlockSig(index.GetBlockHeader(), leader.pubkey, Genesis());
    }};
    CBlockIndex* lowest{nullptr};
    CBlockIndex* moved{nullptr};
    std::vector<CBlockIndex*> again;
    {
        LOCK(::cs_main);
        // An epoch whose seed, as the blocks of the active chain read it (its state: what they were
        // connected with, before a restart too), the record moved since: the blocks of stake from its
        // start on are connected again, even if their own leaders did not change, so that their state
        // is the one the record gives now. (Not a copy in memory of what an earlier check saw: a node
        // that restarted, or whose record moved before its first check, would see no change.)
        //
        // Newest first, down to the first epoch whose seed the record still gives: the seeds before it
        // are the same too. (Its seed block is the same block, so the record is the same up to it; the
        // seed of an earlier epoch is a block at or below it, the highest of a median time at or before
        // a time no later, which that same prefix of the record tells. A seed is the hash of the epoch
        // and its block.) So the check reaches as deep as the reorg, however deep, and costs one look
        // a mainchain tip when nothing moved. The epochs whose seeds the blocks of stake of the active
        // chain read: that of each, and the one before (the slots it charged).
        std::optional<int64_t> changed;
        {
            const sidechain::StoreView& view{chainman->ActiveChainstate().SideState().View()};
            // (No further down than the oldest epoch noted: none at all, no walk.)
            std::optional<int64_t> oldest;
            EPOCH_SEEDS.ForEach(view, [&](const uint64_t& at, const uint256&) {
                oldest = static_cast<int64_t>(at);
                return false;
            });
            std::optional<int64_t> next;
            bool same{!oldest};
            for (const CBlockIndex* index{chainman->ActiveChain().Tip()}; index && index->nHeight > 0 && !same; index = index->pprev) {
                if (!index->m_ext) continue;
                const int64_t epoch{EpochOf(SlotOf(index->nTime, m_params), m_params)};
                if (epoch < *oldest) break;
                for (int64_t at{next ? std::min(*next, epoch) : epoch}; at >= std::max<int64_t>(epoch - 1, 0) && !same; --at) {
                    next = at - 1;
                    const auto used{EPOCH_SEEDS.Get(view, static_cast<uint64_t>(at))};
                    if (!used) continue;
                    const auto now{EpochSeed(at, m_params, record)};
                    if (!now) continue;
                    if (*now == *used) {
                        same = true;
                    } else {
                        changed = at;
                    }
                }
            }
        }
        // The newest blocks of the active chain (a mainchain reorg reaches the seeds of the last epochs
        // only), and those from the start of the epoch whose seed moved: their blocks of stake.
        std::vector<CBlockIndex*> blocks;
        CBlockIndex* index{chainman->ActiveChain().Tip()};
        for (int64_t i{0}; index && index->nHeight > 0; ++i, index = index->pprev) {
            const bool recent{i < 2 * m_params.slots_per_epoch + m_params.max_unanchored_slots};
            if (!recent && !(changed && SlotOf(index->nTime, m_params) >= *changed * m_params.slots_per_epoch)) break;
            if (index->m_ext) blocks.push_back(index);
        }
        for (CBlockIndex* block : blocks) {
            if (!agrees(*block)) lowest = block;
        }
        std::vector<CBlockIndex*> failed;
        {
            LOCK(m_sets_mutex);
            for (auto it{m_seed_failed.begin()}; it != m_seed_failed.end();) {
                CBlockIndex* block{chainman->m_blockman.LookupBlockIndex(*it)};
                if (!block || !(block->nStatus & BLOCK_FAILED_VALID)) {
                    it = m_seed_failed.erase(it);
                } else {
                    failed.push_back(block);
                    ++it;
                }
            }
            // The sets anchors made may change with the state: read again.
            if (changed || lowest) {
                m_known_sets.clear();
                m_known_order.clear();
            }
        }
        if (changed) {
            for (CBlockIndex* block : blocks) {
                if (SlotOf(block->nTime, m_params) >= *changed * m_params.slots_per_epoch) moved = block;
            }
        }
        // (Not with m_sets_mutex held: the leaders read the sets.)
        for (CBlockIndex* block : failed) {
            if (block->pprev && agrees(*block)) again.push_back(block);
        }
        if (!again.empty()) {
            LOCK(m_sets_mutex);
            for (CBlockIndex* block : again) m_seed_failed.erase(block->GetBlockHash());
        }
    }
    if (lowest) {
        LogInfo("FastBlocks: the record moved the seed of the epoch of block %s: it and the blocks after it are taken back", lowest->GetBlockHash().ToString());
        WITH_LOCK(m_sets_mutex, m_seed_failed.insert(lowest->GetBlockHash()));
    }
    // From the lower of the two: a block whose leader the record moved stays failed until the record
    // agrees again (when connected again, it fails too: m_seed_failed keeps it); one below it whose
    // seed moved only is checked again at once.
    const bool recheck{moved && (!lowest || moved->nHeight < lowest->nHeight)};
    if (CBlockIndex* start{recheck ? moved : lowest}) {
        if (recheck) LogInfo("FastBlocks: the record moved the seed of the epoch of block %s: it and the blocks after it are checked again", moved->GetBlockHash().ToString());
        BlockValidationState state;
        chainman->ActiveChainstate().InvalidateBlock(state, start, /*by_record=*/true);
        if (recheck) WITH_LOCK(::cs_main, chainman->ActiveChainstate().ReconsiderRecordFailure(start));
    }
    if (!again.empty()) {
        LOCK(::cs_main);
        for (CBlockIndex* block : again) {
            if (chainman->ActiveChainstate().ReconsiderRecordFailure(block)) LogInfo("FastBlocks: the record gives the seed of block %s back", block->GetBlockHash().ToString());
        }
    }
    if (lowest || recheck || !again.empty()) {
        BlockValidationState state;
        chainman->ActiveChainstate().ActivateBestChain(state);
    }
}

const CBlockIndex* FastBlocksHooks::TicketTarget(const CBlockIndex& tip, const FastState& fast, int64_t slot, bool after_anchor_point) const
{
    AssertLockHeld(::cs_main);
    // The block just before the newest (ticket depth 1), unless that is before the last anchor point
    // (or the last block anchored).
    const CBlockIndex* target{tip.pprev ? tip.pprev : &tip};
    if (target->nHeight < (after_anchor_point ? fast.anchor_point_height : fast.last_anchored_height)) return &tip;
    // A ticket for it that could not be acknowledged any more (blocks of stake stopped): the tip, which
    // no block will follow before an anchor covers it.
    if (!InTime(tip, *target, slot + 1)) return &tip;
    return target;
}

bool FastBlocksHooks::InTime(const CBlockIndex& tip, const CBlockIndex& target, int64_t slot) const
{
    int64_t first{slot};
    for (const CBlockIndex* index{&tip}; index && index->nHeight > target.nHeight; index = index->pprev) {
        if (index->m_ext) first = SlotOf(index->nTime, m_params);
    }
    return slot - first < m_params.max_unanchored_slots;
}

bool FastBlocksHooks::Acknowledgeable(const AnchorTicket& ticket, const CBlockIndex& tip, const FastState& fast, int64_t slot) const
{
    AssertLockHeld(::cs_main);
    ChainstateManager* chainman{Chainman()};
    if (!chainman || ticket.version != 1 || !IsValidPayee(ticket.payee)) return false;
    // On this chain (at or below the tip: the parent of the block that would carry it).
    const CBlockIndex* target{chainman->m_blockman.LookupBlockIndex(ticket.anchored)};
    if (!target || !Descends(&tip, target)) return false;
    // At or after the block the last anchor anchored (the state checks it: bad-pos-ack-chain).
    if (fast.last_anchored_height >= 0) {
        const CBlockIndex* last{AncestorAt(target, fast.last_anchored_height)};
        if (!last || last->GetBlockHash() != fast.last_anchored) return false;
    }
    // Not so old that the blocks of stake above it would have been uncovered too long (the state
    // checks it: bad-pos-unanchored).
    return InTime(tip, *target, slot);
}

std::optional<AnchorAck> FastBlocksHooks::NextAck(const CBlockIndex& tip, const FastState& fast, int64_t slot, const std::set<uint256>* skip) const
{
    AssertLockHeld(::cs_main);
    ChainstateManager* chainman{Chainman()};
    if (!chainman || !chainman->m_mainchain) return std::nullopt;
    const sidechain::Mainchain& record{*chainman->m_mainchain};
    std::optional<AnchorAck> best;
    for (const AnchorTicket& ticket : Tickets()) {
        if (skip && skip->contains(ticket.Hash())) continue;
        const auto committed{record.CommittedHeight(ticket.Hash())};
        if (!committed || *committed <= fast.last_main_height) continue;
        if (best && best->main_height <= *committed) continue;
        if (!Acknowledgeable(ticket, tip, fast, slot)) continue;
        // A block that acknowledged it failed when the record lost the commitment, and the follower is
        // about to give it back now that the record has it again: acknowledging it here too would only
        // fork the chain (both branches with the same anchor; the longer wins). (One at the height the
        // record has it now: one at another fails again, whatever this node noted.)
        const auto acked{BlocksCommittedBy(ticket.Hash(), *committed)};
        if (std::any_of(acked.begin() + 1, acked.end(), [&](const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
                const CBlockIndex* index{chainman->m_blockman.LookupBlockIndex(hash)};
                return index && index->m_ext && static_cast<int64_t>(index->nNonce) == *committed && (index->nStatus & BLOCK_FAILED_VALID) &&
                       record.GetFailure(hash) == sidechain::Mainchain::Failure::RECORD;
            })) {
            continue;
        }
        best = AnchorAck{.ticket = ticket, .main_height = *committed};
    }
    return best;
}

bool FastBlocksHooks::AddTicket(const AnchorTicket& ticket, bool own) const
{
    const uint256 hash{ticket.Hash()};
    const ChainstateManager* chainman{Chainman()};
    const sidechain::Mainchain* record{chainman ? chainman->m_mainchain.get() : nullptr};
    LOCK(m_tickets_mutex);
    if (own) m_own_tickets.insert(hash);
    if (!m_tickets.emplace(hash, ticket).second) return false;
    m_ticket_order.push_back(hash);
    // Bounded: the oldest go first, but not this node's own nor those the mainchain committed to, so
    // that a peer sending tickets by the thousand cannot push out the ones that count. (Tickets
    // committed to long ago go too, with the oldest, once there are enough of them.)
    for (size_t i{0}; m_ticket_order.size() > MAX_TICKETS && i < m_ticket_order.size();) {
        const uint256 candidate{m_ticket_order[i]};
        const bool keep{m_ticket_order.size() <= 2 * MAX_TICKETS && (m_own_tickets.contains(candidate) || (record && record->CommittedHeight(candidate)))};
        if (keep) {
            ++i;
            continue;
        }
        m_tickets.erase(candidate);
        m_own_tickets.erase(candidate);
        m_ticket_order.erase(m_ticket_order.begin() + i);
    }
    return true;
}

bool FastBlocksHooks::TicketAllowed(int64_t peer) const
{
    const ChainstateManager* chainman{Chainman()};
    const uint256 main_tip{chainman && chainman->m_mainchain ? chainman->m_mainchain->TipHash() : uint256{}};
    // By the peer's network group, not its id: a peer that connects again has a new id, and would have
    // a new count each time (and each ticket it sends goes to every peer). Its group stays. All peers
    // whose addresses are not routable (local, as Tor's inbound connections are, or private) are one
    // group, as Bitcoin Core counts onion and local addresses: by peer, a reconnection over Tor would
    // be a new budget (audit 6 review, item 13). And a peer the node cannot find (gone meanwhile) is
    // counted by its id, all such peers within a small budget together: it may be anyone.
    std::pair<int, uint64_t> who{TICKETS_BY_UNKNOWN_PEER, static_cast<uint64_t>(peer)};
    if (m_node && m_node->connman) {
        m_node->connman->ForNode(peer, [&](CNode* node) {
            who = node->addr.IsRoutable() ? std::pair<int, uint64_t>{TICKETS_BY_GROUP, node->nKeyedNetGroup} : std::pair<int, uint64_t>{TICKETS_BY_NONROUTABLE, 0};
            return true;
        });
    }
    const bool unknown{who.first == TICKETS_BY_UNKNOWN_PEER};
    const size_t limit{who.first == TICKETS_BY_NONROUTABLE ? MAX_NONROUTABLE_TICKETS : MAX_PEER_TICKETS};
    LOCK(m_tickets_mutex);
    // Counted per mainchain block: all counts start again with a new one.
    if (m_ticket_counts_for != main_tip) {
        m_ticket_counts.clear();
        m_unknown_ticket_count = 0;
        m_ticket_count = 0;
        m_ticket_counts_for = main_tip;
    }
    const auto found{m_ticket_counts.find(who)};
    const size_t count{found == m_ticket_counts.end() ? 0 : found->second};
    if (count >= limit || (unknown && m_unknown_ticket_count >= MAX_UNKNOWN_PEER_TICKETS)) return false;
    // In all, too, first come; past that, a group's first one (its share: a few groups that came first
    // do not crowd an honest bidder's relayed ticket out, audit 6 review, item 12), up to a bound.
    if (m_ticket_count >= MAX_UNCOMMITTED_TICKETS && (count > 0 || m_ticket_count >= MAX_UNCOMMITTED_TICKETS_SHARED)) return false;
    ++m_ticket_counts[who];
    ++m_ticket_count;
    if (unknown) ++m_unknown_ticket_count;
    return true;
}

std::optional<AnchorTicket> FastBlocksHooks::GetTicket(const uint256& hash) const
{
    LOCK(m_tickets_mutex);
    const auto found{m_tickets.find(hash)};
    if (found == m_tickets.end()) return std::nullopt;
    return found->second;
}

std::vector<AnchorTicket> FastBlocksHooks::Tickets() const
{
    LOCK(m_tickets_mutex);
    std::vector<AnchorTicket> tickets;
    for (const uint256& hash : m_ticket_order) tickets.push_back(m_tickets.at(hash));
    return tickets;
}

void FastBlocksHooks::RelayTicket(const AnchorTicket& ticket, std::optional<int64_t> except) const
{
    if (!m_node || !m_node->peerman) return;
    DataStream s{};
    s << ticket;
    m_node->peerman->BroadcastSidechainMessage(MSG_TICKET, {UCharCast(s.data()), UCharCast(s.data()) + s.size()}, except);
}

bool FastBlocksHooks::ProcessMessage(int64_t peer, const std::string& msg_type, DataStream& recv, std::string& misbehaving) const
{
    ChainstateManager* chainman{Chainman()};
    if (msg_type == MSG_TICKET) {
        AnchorTicket ticket;
        try {
            recv >> ticket;
        } catch (const std::ios_base::failure&) {
            misbehaving = "malformed anchor ticket";
            return true;
        }
        if (ticket.version != 1 || !IsValidPayee(ticket.payee)) return true;
        // Only tickets for a block this node knows, or that the mainchain committed to (a node that is
        // still taking in the chain needs those of the newest anchors): others cannot be acknowledged
        // here anyway, and anyone can make them.
        if (!chainman) return true;
        const bool committed{chainman->m_mainchain && chainman->m_mainchain->CommittedHeight(ticket.Hash())};
        if (!committed) {
            if (!WITH_LOCK(::cs_main, return chainman->m_blockman.LookupBlockIndex(ticket.anchored) != nullptr)) return true;
            // One the node has: nothing to take nor relay, and not counted (honest peers relay each
            // new ticket to one another).
            if (GetTicket(ticket.Hash())) return true;
            // Anyone can make tickets for a block, by the thousand, and each new one goes to every
            // peer: a few per network group of peers and mainchain block (bidders make one each; all
            // peers on addresses that are not routable are one group), a few more in all, and a
            // group's first one past that, the rest dropped (TicketAllowed). Those the mainchain committed to
            // cost a commitment each, and are always taken: a bidder's ticket the node dropped comes
            // when it counts (AskMissingTickets).
            if (!TicketAllowed(peer)) {
                LogDebug(BCLog::NET, "FastBlocks: anchor ticket from peer=%d dropped: too many for this mainchain block\n", peer);
                return true;
            }
        }
        if (AddTicket(ticket)) RelayTicket(ticket, peer);
        return true;
    }
    if (msg_type == MSG_GETTICKETS) {
        std::vector<uint256> wanted;
        try {
            // (Not more than asked for at most: a longer list is not read.)
            const uint64_t count{ReadCompactSize(recv, /*range_check=*/false)};
            if (count > MAX_TICKETS_ASKED) {
                misbehaving = "too many anchor tickets asked for";
                return true;
            }
            wanted.resize(count);
            for (uint256& hash : wanted) recv >> hash;
        } catch (const std::ios_base::failure&) {
            misbehaving = "malformed request for anchor tickets";
            return true;
        }
        if (!m_node || !m_node->peerman) return true;
        for (const uint256& hash : wanted) {
            const auto ticket{GetTicket(hash)};
            if (!ticket) continue;
            DataStream s{};
            s << *ticket;
            m_node->peerman->SendSidechainMessage(peer, MSG_TICKET, {UCharCast(s.data()), UCharCast(s.data()) + s.size()});
        }
        return true;
    }
    if (msg_type == MSG_EVIDENCE) {
        DoubleProposal proof;
        try {
            recv >> proof;
        } catch (const std::ios_base::failure&) {
            misbehaving = "malformed evidence";
            return true;
        }
        // Relayed only if it is a proof (one this node does not know yet). Only one whose form is
        // wrong (any node sees it, whatever chain it has) is the peer's fault. The leader a signature
        // is checked against depends on what chain a node has: a new node (an inactive set at its
        // tip, none of the headers' parents), or one on another branch, may find another leader, or
        // none, for a proof an honest peer checked. Dropped, not blamed (a few signature checks: the
        // first header's fails, and the second is not checked).
        std::string reason;
        if (!CheckEvidenceForm(proof, m_params, reason)) {
            misbehaving = "invalid evidence: " + reason;
            return true;
        }
        // Since a proof whose signatures fail is not the peer's fault, a peer could have the node check
        // signatures without end: a rate per peer of proofs checked (a known one costs no check). Past
        // it, proofs are dropped unchecked, and the peer is not blamed for that either.
        if (!m_evidence.Known(proof) && !m_evidence_rate.Allow(peer, std::chrono::duration_cast<std::chrono::milliseconds>(NodeClock::now().time_since_epoch()))) {
            LogDebug(BCLog::NET, "FastBlocks: evidence from peer=%d dropped unchecked: more than its rate\n", peer);
            return true;
        }
        if (!AddEvidence(proof, reason, peer)) {
            LogDebug(BCLog::NET, "FastBlocks: evidence from peer=%d dropped: %s\n", peer, reason);
        }
        return true;
    }
    return false;
}

std::optional<Offence> FastBlocksHooks::AddEvidence(const DoubleProposal& proof, std::string& reason, std::optional<int64_t> except) const
{
    if (!CheckEvidenceForm(proof, m_params, reason)) return std::nullopt;
    // One this node keeps already: nothing to check again (two signatures), nothing to relay.
    if (const auto known{m_evidence.Known(proof)}) return known;
    // The leader of the slot: as the chain below either header says, or the active chain.
    std::optional<Offence> offence;
    bool unknown{false};
    // (Each leader once: the chains below both headers and the active one most often name one.)
    std::set<uint256> checked;
    if (ChainstateManager* chainman{Chainman()}) {
        LOCK(::cs_main);
        const int64_t slot{SlotOf(proof.a.nTime, m_params)};
        const std::array<const CBlockIndex*, 3> prevs{chainman->m_blockman.LookupBlockIndex(proof.a.hashPrevBlock), chainman->m_blockman.LookupBlockIndex(proof.b.hashPrevBlock),
                                                      chainman->ActiveChain().Tip()};
        for (const CBlockIndex* prev : prevs) {
            if (!prev) continue;
            // (Within the window, as a block that carries it checks: a proof of an older slot is of no
            // use, and anyone can name one, at the cost of a walk down the whole chain otherwise.)
            const SlotLeader leader{LeaderOf(*prev, slot, nullptr, EvidenceWithin())};
            if (leader.status == LeaderStatus::SET_UNKNOWN || leader.status == LeaderStatus::SEED_UNKNOWN) unknown = true;
            if (leader.status != LeaderStatus::OK || checked.contains(leader.pubkey)) continue;
            checked.insert(leader.pubkey);
            // (Checked once per proof and leader: a proof sent again costs no signature check.)
            if (m_evidence.CheckSigs(proof, leader.pubkey, Genesis())) {
                offence = Offence{.pubkey = leader.pubkey, .slot = slot};
                break;
            }
        }
    }
    if (!offence) {
        // Not the peer's fault if this node cannot tell the leader yet.
        reason = unknown ? "pos-evidence-unknown" : "bad-pos-evidence-sig";
        return std::nullopt;
    }
    if (!m_evidence.Add(*offence, proof)) return offence;
    LogWarning("FastBlocks: validator %s signed two blocks for slot %d", HexStr(offence->pubkey), offence->slot);
    m_evidence.MarkRelayed(*offence);
    if (m_node && m_node->peerman) {
        DoubleProposal normal{proof};
        normal.Normalize();
        DataStream s{};
        s << normal;
        m_node->peerman->BroadcastSidechainMessage(MSG_EVIDENCE, {UCharCast(s.data()), UCharCast(s.data()) + s.size()}, except);
    }
    return offence;
}

void FastBlocksHooks::RelayNewEvidence() const
{
    const auto proofs{m_evidence.TakeUnrelayed()};
    if (proofs.empty() || !m_node || !m_node->peerman) return;
    for (const DoubleProposal& proof : proofs) {
        DataStream s{};
        s << proof;
        m_node->peerman->BroadcastSidechainMessage(MSG_EVIDENCE, {UCharCast(s.data()), UCharCast(s.data()) + s.size()});
    }
}

std::vector<uint256> FastBlocksHooks::AskMissingTickets() const
{
    ChainstateManager* chainman{Chainman()};
    if (!chainman || !chainman->m_mainchain) return {};
    const sidechain::Mainchain& record{*chainman->m_mainchain};
    const int height{record.Height()};
    const uint256 main_tip{record.TipHash()};
    {
        LOCK(m_anchor_mutex);
        if (m_tickets_asked_for == main_tip) return {};
    }
    std::vector<uint256> missing;
    {
        LOCK(::cs_main);
        const FastState fast{FAST_STATE.Get(chainman->ActiveChainstate().SideState().View())};
        const int from{std::max(fast.last_main_height + 1, height - static_cast<int>(MAX_TICKETS_ASKED) + 1)};
        for (int h{std::max(from, 1)}; h <= height; ++h) {
            const auto bmm{record.BmmAt(h)};
            // A commitment to a block (one known, or waiting for it), or of a ticket known, is no ticket to ask for.
            if (!bmm || chainman->m_blockman.LookupBlockIndex(*bmm) || chainman->m_bmm_waiting.contains(*bmm)) continue;
            if (GetTicket(*bmm)) continue;
            missing.push_back(*bmm);
        }
    }
    {
        LOCK(m_anchor_mutex);
        m_tickets_asked_for = main_tip;
    }
    if (missing.empty() || !m_node || !m_node->peerman) return missing;
    DataStream s{};
    s << missing;
    m_node->peerman->BroadcastSidechainMessage(MSG_GETTICKETS, {UCharCast(s.data()), UCharCast(s.data()) + s.size()});
    LogDebug(BCLog::NET, "FastBlocks: asked the peers for %u anchor ticket(s)\n", missing.size());
    return missing;
}

std::vector<std::pair<Offence, DoubleProposal>> FastBlocksHooks::ChainEvidence(const CBlockIndex& prev, int64_t slot, const sidechain::StoreView& view, uint64_t anchors,
                                                                                size_t max, const std::set<Offence>& skip) const
{
    AssertLockHeld(::cs_main);
    using Entry = std::pair<Offence, DoubleProposal>;
    // The cheap checks, by the pool's offence (the signatures say it is the only one the proof can be):
    // a later slot, punished already, banned (whether each validator met is: read once each).
    std::vector<Entry> candidates;
    std::map<uint256, bool> banned;
    for (auto& entry : m_evidence.All()) {
        const Offence& offence{entry.first};
        if (skip.contains(offence) || offence.slot > slot) continue;
        if (OffencePunished(view, offence.pubkey, offence.slot)) continue;
        auto is_banned{banned.find(offence.pubkey)};
        if (is_banned == banned.end()) {
            const auto record{GetValidator(view, offence.pubkey)};
            is_banned = banned.emplace(offence.pubkey, record && record->tombstoned).first;
        }
        if (is_banned->second) continue;
        candidates.push_back(std::move(entry));
    }
    // By value: the oldest slot first, the nearest the end of its window (then the pool's order).
    std::ranges::sort(candidates, [](const Entry& x, const Entry& y) { return std::tie(x.first.slot, x.first) < std::tie(y.first.slot, y.first); });
    std::vector<Entry> chosen;
    std::set<uint256> offenders;
    const auto take{[&](const Entry& entry, size_t& budget, std::string& reason) {
        const auto checked{CheckChainEvidence(entry.second, prev, slot, view, anchors, reason, &budget)};
        if (!checked) return false;
        offenders.insert(checked->pubkey);
        chosen.emplace_back(*checked, entry.second);
        return true;
    }};
    // Those that cost no signature check (a result kept: a good one stays while the pool keeps the
    // proof), or fail before it (the set of their slot not known here, out of the window).
    std::vector<const Entry*> unchecked;
    size_t none{0};
    for (const Entry& entry : candidates) {
        if (chosen.size() >= max) break;
        if (offenders.contains(entry.first.pubkey)) continue;
        std::string reason;
        if (!take(entry, none, reason) && reason == "pos-evidence-later") unchecked.push_back(&entry);
    }
    // Then the signatures of a few not checked before, round robin in the pool's order from the last
    // one checked: each is checked within EVIDENCE_CHECK_ROUNDS blocks, however many others there are
    // (a flood that pushes their results out keeps them unchecked, and first in no other order).
    std::ranges::sort(unchecked, [](const Entry* x, const Entry* y) { return x->first < y->first; });
    const auto next{m_evidence_cursor ? std::ranges::upper_bound(unchecked, *m_evidence_cursor, {}, [](const Entry* e) { return e->first; }) : unchecked.begin()};
    std::ranges::rotate(unchecked, next);
    size_t budget{MAX_EVIDENCE_SIG_CHECKS};
    for (const Entry* entry : unchecked) {
        if (chosen.size() >= max || budget == 0) break;
        if (offenders.contains(entry->first.pubkey)) continue;
        std::string reason;
        take(*entry, budget, reason);
        m_evidence_cursor = entry->first;
    }
    // By value, as above.
    std::ranges::sort(chosen, [](const Entry& x, const Entry& y) { return std::tie(x.first.slot, x.first) < std::tie(y.first.slot, y.first); });
    return chosen;
}

std::vector<CTxOut> FastBlocksHooks::MergeMinedOutputs(const sidechain::StoreView& view, const CBlockIndex& prev, int64_t time) const
{
    AssertLockHeld(::cs_main);
    // The proofs this node knows that the chain has not punished, one per validator (a validator is
    // slashed once), a few at most.
    std::vector<CTxOut> outputs;
    for (const auto& [offence, proof] : ChainEvidence(prev, SlotOf(time, m_params), view, FAST_STATE.Get(view).anchors, MAX_MINED_EVIDENCE)) {
        outputs.emplace_back(0, EvidenceScript(proof));
    }
    return outputs;
}

bool FastBlocksHooks::FollowerRound(sidechain::Follower& follower) const
{
    ChainstateManager* chainman{Chainman()};
    if (!chainman || !chainman->m_mainchain) return false;
    // Proofs of leaders that signed twice, found by watching headers; and the tickets of the newest
    // commitments, for a node that does not have them. Blocks of stake whose epoch's seed the record
    // moved are taken back.
    RelayNewEvidence();
    AskMissingTickets();
    CheckSeeds();
    const sidechain::Mainchain& record{*chainman->m_mainchain};
    const sidechain::Follower::Mining mining{follower.GetMining()};
    const int main_height{record.Height()};
    const uint256 main_tip{record.TipHash()};
    const auto main_block{record.GetBlock(main_height)};
    if (!main_block) return false;

    FastState fast;
    const CBlockIndex* tip;
    {
        LOCK(::cs_main);
        tip = chainman->ActiveChain().Tip();
        fast = FAST_STATE.Get(chainman->ActiveChainstate().SideState().View());
    }
    // Who bids: the merged miner of this node (setbmm), or else a validator's own anchorer
    // (-validatoranchorbid, on by default for validators): anchors must not wait for fees, nor for an
    // operator, or an idle chain halts (blocks of stake stop at max_unanchored_slots).
    const auto validator{ValidatorAnchoring()};
    // No PoS blocks for a while (or none at all on this chain, or no active validator set): merged mined
    // blocks, as the template's -- by a validator too, for its bid whatever the fees (a BMM block
    // starts blocks of stake again).
    if (Stalled(fast, main_block->time, PosActiveNow())) {
        if (mining.enabled || !validator) return false;
        sidechain::Follower::Mining own;
        own.enabled = true;
        own.coinbase_script = validator->first;
        own.always = true;
        own.amount = validator->second;
        follower.MineWith(own);
        return true;
    }
    // Otherwise anchors: a ticket per mainchain block.
    if (!mining.enabled && !validator) return true;
    const CScript payee{mining.enabled ? mining.coinbase_script : validator->first};
    if (!IsValidPayee(payee)) return true;
    const int64_t slot{SlotOf(TicksSinceEpoch<std::chrono::seconds>(NodeClock::now()), m_params)};
    // A commitment of the last mainchain block not acknowledged yet. If it is this node's ticket, it goes
    // to the peers again, once per mainchain block, in case the leaders lost it.
    const bool pending{record.BmmAt(main_height) && fast.last_main_height < main_height};
    if (pending) {
        const uint256 last{*record.BmmAt(main_height)};
        const auto ticket{GetTicket(last)};
        if (ticket && WITH_LOCK(m_tickets_mutex, return m_own_tickets.contains(last))) {
            LOCK(m_anchor_mutex);
            if (m_ticket_relayed_for != main_tip) {
                m_ticket_relayed_for = main_tip;
                RelayTicket(*ticket);
            }
        }
    }
    // A ticket for the next mainchain block, at once: not after the acknowledgement of the last one,
    // which may come after the mainchain's next block (then that block had no commitment of this
    // chain). Its block must be at or after the last block anchored; while a commitment waits for its
    // acknowledgement, below it, and its anchor builds no bundle (AckPegStep). So once that
    // acknowledgement is in, if a bundle could start, the ticket is made again for a block after it,
    // bidding a little more, once.
    bool again{false};
    CAmount last_offer{0};
    {
        LOCK(m_anchor_mutex);
        // (No ticket yet, for want of a block to anchor after the waiting commitment's: tried again
        // at each round, as blocks come.)
        if (m_bid.main_tip == main_tip && m_bid.made) {
            if (pending || m_bid.after_anchor_point || m_bid.again) return true;
            again = true;
            last_offer = m_bid.offer;
        }
    }
    if (again) {
        const bool bundle{WITH_LOCK(::cs_main, return chainman->ActiveChainstate().SideState().NextBundle(tip->nHeight + 1, tip->GetBlockHash(), chainman->GetConsensus().sidechain).has_value())};
        if (!bundle) {
            LOCK(m_anchor_mutex);
            m_bid.again = true;
            return true;
        }
    }

    AnchorTicket ticket;
    {
        LOCK(::cs_main);
        const CBlockIndex* target{TicketTarget(*tip, fast, slot, /*after_anchor_point=*/!pending)};
        // After the block the waiting commitment's ticket anchors, if this node knows it: anchors are
        // acknowledged in the order of their commitments, each at or after the block the one before
        // anchored -- and one ticket committed twice is one anchor (the second commitment is
        // skipped). Two mainchain blocks within a slot: the tip, the second time.
        if (const auto waiting{pending ? GetTicket(*record.BmmAt(main_height)) : std::nullopt}) {
            const CBlockIndex* before{chainman->m_blockman.LookupBlockIndex(waiting->anchored)};
            if (before && target->nHeight <= before->nHeight) target = tip;
            if (before && target->nHeight <= before->nHeight) {
                LOCK(m_anchor_mutex);
                m_bid = Bid{.main_tip = main_tip, .made = false};
                return true;
            }
        }
        ticket.anchored = target->GetBlockHash();
    }
    ticket.payee = payee;
    // Bid what the pot will pay, but the operator's share, or what the operator said (for tests); a
    // validator at least its own bid.
    const int64_t expected{WITH_LOCK(::cs_main, return PotAccruedAt(chainman->ActiveChainstate().SideState().View(),
                                                                    chainman->m_blockman.LookupBlockIndex(ticket.anchored)->nHeight)) - fast.pot_paid};
    CAmount offer{mining.enabled && mining.always ? mining.amount : sidechain::Follower::Offer(expected)};
    if (!(mining.enabled && mining.always) && validator) offer = std::max(offer, validator->second);
    // (Made again: more than the bid it replaces in the mainchain's mempool.)
    if (again) offer = std::max(offer, last_offer + sidechain::Follower::MIN_OFFER);
    {
        LOCK(m_anchor_mutex);
        if (again) {
            m_bid.again = true;
        } else {
            m_bid = Bid{.main_tip = main_tip, .offer = offer, .after_anchor_point = !pending, .made = true};
        }
    }
    if (offer < sidechain::Follower::MIN_OFFER) return true;
    std::string error;
    // (Whatever mainchain block the request follows: a ticket does not depend on it. A request made as
    // the mainchain moved on is in its mempool all the same, for the next block: its ticket must be
    // known, or that block's commitment is one nobody can acknowledge.)
    const auto request{follower.RequestCommitment(ticket.Hash(), uint256{}, offer, error)};
    {
        LOCK(m_anchor_mutex);
        m_ticket_requested_for = main_tip;
    }
    if (!request) {
        LogDebug(BCLog::NET, "FastBlocks: no anchor ticket for mainchain block %s: %s\n", main_tip.ToString(), error);
        return true;
    }
    {
        LOCK(m_anchor_mutex);
        m_bid.offer = offer;
    }
    AddTicket(ticket, /*own=*/true);
    RelayTicket(ticket);
    LogInfo("FastBlocks: anchor ticket %s for block %s, %d offered%s", ticket.Hash().ToString(), ticket.anchored.ToString(), offer, again ? " (again, for a block after the last acknowledgement)" : "");
    return true;
}

std::optional<std::pair<CScript, CAmount>> FastBlocksHooks::ValidatorAnchoring() const
{
    LOCK(m_anchor_mutex);
    if (!m_validator_payee || m_validator_bid <= 0) return std::nullopt;
    return std::make_pair(*m_validator_payee, m_validator_bid);
}

void FastBlocksHooks::StartServices(node::NodeContext& node) const
{
    m_node = &node;
    std::string error;
    m_producer = Producer::FromArgs(node, *this, error);
    m_producer_error = error;
    if (!error.empty()) {
        LogError("FastBlocks: no block producer: %s", error);
        return;
    }
    if (m_producer) {
        m_producer->Start();
        // Its own anchorer: a ticket for every mainchain block, its bid whatever the fees, the pot to the producer's payout.
        CAmount bid{m_chain_params.GetChainType() == ChainType::REGTEST ? 0 : DEFAULT_VALIDATOR_ANCHOR_BID};
        if (node.args && node.args->IsArgSet("-validatoranchorbid")) {
            const auto parsed{ParseMoney(node.args->GetArg("-validatoranchorbid", ""))};
            if (!parsed || *parsed < 0) {
                LogError("FastBlocks: -validatoranchorbid is not an amount: the validator's anchorer is off");
                bid = 0;
            } else {
                bid = *parsed;
            }
        }
        if (bid > 0 && bid < sidechain::Follower::MIN_OFFER) {
            LogWarning("FastBlocks: -validatoranchorbid below %s does not pay for a request's place in a mainchain block: %s bid", FormatMoney(sidechain::Follower::MIN_OFFER), FormatMoney(sidechain::Follower::MIN_OFFER));
            bid = sidechain::Follower::MIN_OFFER;
        }
        LOCK(m_anchor_mutex);
        m_validator_bid = bid;
        m_validator_payee = m_producer->Payout();
        if (bid > 0) LogInfo("FastBlocks: the validator's anchorer bids %s a mainchain block at least (-validatoranchorbid)", FormatMoney(bid));
    }
    // The acknowledgements of the recent blocks of the active chain, so that a commitment the mainchain
    // drops takes the blocks that rest on it with it.
    if (ChainstateManager* chainman{Chainman()}) {
        std::vector<const CBlockIndex*> recent;
        {
            LOCK(::cs_main);
            const CBlockIndex* index{chainman->ActiveChain().Tip()};
            for (int i{0}; index && i < 2 * static_cast<int>(m_params.max_unanchored_slots) + 1000; ++i, index = index->pprev) {
                if (index->m_ext && index->nNonce != 0) recent.push_back(index);
            }
        }
        for (const CBlockIndex* index : recent) CommitmentOf(*index);
    }
}

void FastBlocksHooks::StopServices(node::NodeContext&) const
{
    if (m_producer) m_producer->Stop();
    {
        LOCK(m_anchor_mutex);
        m_validator_payee.reset();
        m_validator_bid = 0;
    }
    m_producer.reset();
    m_node = nullptr;
}

FastBlocksHooks* InstalledHooks() { return g_installed; }

void UninstallHooks()
{
    g_installed = nullptr;
    sidechain::SetHooks(nullptr);
}

void InstallHooks(const CChainParams& params)
{
    if (!params.GetConsensus().sidechain.enabled) {
        if (g_installed) UninstallHooks();
        return;
    }
    auto hooks{std::make_unique<FastBlocksHooks>(params)};
    g_installed = hooks.get();
    sidechain::SetHooks(std::move(hooks));
}

} // namespace pos
