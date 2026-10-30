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
#include <util/fs_helpers.h>
#include <util/strencodings.h>
#include <util/moneystr.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <ios>
#include <sstream>
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
    // slot's epoch, which an anchor before the epoch before it (and before its seed, SetBoundary) made, and of the epoch's seed, which the
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
        // (Not kept of a validator the active chain banned already: punished once, it takes nothing more.)
        // (Nor of one it keeps a proof of already: one per offender, all slashing takes.)
        if (const auto proof{m_evidence.NoteSigned(header, slot, leader.pubkey)}; proof && !Banned(chainman, leader.pubkey) && !m_evidence.HasOffender(leader.pubkey)) {
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
            if (index->nNonce != 0 || ++walked > m_params.MaxUnanchoredSlots(slot)) break;
        }
        if (slot - first >= m_params.MaxUnanchoredSlots(slot)) {
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
    // (Active as of the epoch of the clock: PosParams::MinActiveValidators.)
    const int64_t now_epoch{EpochOf(SlotOf(TicksSinceEpoch<std::chrono::seconds>(NodeClock::now()), m_params), m_params)};
    if (m_genesis_set.Active(now_epoch)) return MAX_FUTURE_BLOCK_TIME_POS;
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
        active = set && set->Active(now_epoch);
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
    const auto staked{m_staking.PegStep(out, block.GetHash(), fast.anchors, anchor_slot, EpochOf(earliest, m_params), set_epoch, staking_owed, JailFixActive(height))};
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
        if (slot - fast.first_unanchored_slot >= m_params.MaxUnanchoredSlots(slot)) return Fail(reject_reason, "bad-pos-unanchored");

        // Liveness: the block's leader made it; the leaders of the slots since the last block of stake
        // (an epoch of them at most) made none -- of the slots at which a block could be made: not
        // those max_unanchored_slots or more after the first block of stake no anchor covered (blocks
        // of stake had stopped for want of anchors), nor those up to a BMM block the block follows
        // (which may end such a stop, or a stall, and whose slot is the earliest the leaders could
        // build on it). Leaders are not jailed for slots nobody could fill.
        // From jail_fix_height, the first block of stake charges no slot: before it the chain had none, and the slots
        // since its first block (or a BMM block long before) were nobody's to fill (on betanet slot 99 the first one
        // came hours after the slot origin, and charged every genesis validator an epoch of misses).
        int64_t charged_from{std::max(fast.last_pos_slot + 1, slot - m_params.slots_per_epoch)};
        if (fast.last_pos_slot < 0 && JailFixActive(height)) charged_from = slot;
        int64_t charged_until{slot};
        if (prev->nHeight > 0 && !prev->m_ext) {
            charged_from = std::max(charged_from, SlotOf(prev->nTime, m_params) + 1);
        } else if (before_step_unanchored >= 0) {
            charged_until = std::min(charged_until, before_step_unanchored + m_params.MaxUnanchoredSlots(slot));
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
            const bool pos_active{!set || set->Active(EpochOf(slot, m_params))};
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
    const int64_t spacing{MedianSpacingAt(*chainman->m_mainchain, main_height, m_params.main_activation_height)};
    // In units of the network's target (PosParams::fee_target_spacing): exact where the target is the curves' own (60 s).
    return m_params.fee_target_spacing == FEE_SPLIT_TARGET_SPACING ? spacing : spacing * FEE_SPLIT_TARGET_SPACING / m_params.fee_target_spacing;
}

bool FastBlocksHooks::CheckFees(const sidechain::State& state, const CBlock& block, int height, const std::optional<sidechain::PegStep>& step,
                                const std::vector<CAmount>& fees, std::string& reject_reason) const
{
    if (!FeeSplitActive(height)) return true;
    const bool pos_block{IsPosHeader(block)};
    // (Every merged mined block takes the peg actions; a block of stake's seed is known: AfterBlock read its leader.)
    if (!pos_block && !step) return Fail(reject_reason, "bad-pos-context");
    const auto main_height{FeeSplitMainHeight(pos_block, SlotOf(block.nTime, m_params), step ? step->main_height : -1)};
    if (!main_height) return Fail(reject_reason, "pos-seed-unknown");
    // The DEX's security parts of its fees (in the chain's coin) are ordinary fees of the block (pos/feesplit.h).
    const FeeSplit split{SplitFees(block.vtx, fees, /*fees_without_coinbase=*/false, FeeSplitSpacing(*main_height), FeeCurveAt(height), state.BitAssets().BlockSecurityFees(height))};
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

CAmount FastBlocksHooks::MergeMinedFeeOutputs(const CBlock& block, int height, int main_height, const std::vector<CAmount>& fees, std::vector<CTxOut>& outputs, CAmount extra) const
{
    if (!FeeSplitActive(height)) return 0;
    const FeeSplit split{SplitFees(block.vtx, fees, /*fees_without_coinbase=*/true, FeeSplitSpacing(main_height), FeeCurveAt(height), extra)};
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
    // The newest anchor at or below prev whose slot is before the epoch before (and before the time the seed is
    // taken at, set_lead_seconds): the stake was fixed before the seed of the epoch exists (SetBoundary).
    const int64_t boundary{SetBoundary(epoch, m_params)};
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
    // Below fork_height no block of stake (eCash mainnet: the blocks of 8.3.4, all merged mined, stay as they were).
    if (!leader.set->Active(epoch) || prev.nHeight + 1 < m_params.fork_height) {
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
    const int64_t slot{SlotOf(TicksSinceEpoch<std::chrono::seconds>(NodeClock::now()), m_params)};
    const int64_t epoch{EpochOf(slot, m_params)};
    if (!chainman) return m_genesis_set.Active(epoch);
    LOCK(::cs_main);
    const CBlockIndex* tip{chainman->ActiveChain().Tip()};
    if (!tip) return m_genesis_set.Active(epoch);
    const auto set{SetFor(*tip, epoch)};
    return set && set->Active(epoch);
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
            const bool recent{i < 2 * m_params.slots_per_epoch + m_params.MaxUnanchoredSlotsEver()};
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
    return slot - first < m_params.MaxUnanchoredSlots(slot);
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

bool FastBlocksHooks::Banned(const ChainstateManager& chainman, const uint256& pubkey)
{
    AssertLockHeld(::cs_main);
    const auto record{GetValidator(chainman.ActiveChainstate().SideState().View(), pubkey)};
    return record && record->tombstoned;
}

std::optional<Offence> FastBlocksHooks::AddEvidence(const DoubleProposal& proof, std::string& reason, std::optional<int64_t> except) const
{
    if (!CheckEvidenceForm(proof, m_params, reason)) return std::nullopt;
    // One this node keeps already: nothing to check again (two signatures), nothing to relay.
    if (const auto known{m_evidence.Known(proof)}) return known;
    // Of a slot to come (beyond the headers' margin and a slot): no block of it can be on any chain yet, and none
    // of a later slot is taken. Not the peer's fault (its clock may be ahead).
    if (proof.a.Time() > NodeClock::now() + std::chrono::seconds{m_params.max_future_seconds + m_params.slot_seconds}) {
        reason = "pos-evidence-future";
        return std::nullopt;
    }
    // The leader of the slot: as the chain below either header says, or the active chain.
    std::optional<Offence> offence;
    bool unknown{false};
    // (Each leader once: the chains below both headers and the active one most often name one.)
    std::set<uint256> checked;
    bool banned{false};
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
            // A leader the active chain banned already: punished once, its proofs are of no use (nor kept).
            if (Banned(*chainman, leader.pubkey)) {
                banned = true;
                continue;
            }
            // (Checked once per proof and leader: a proof sent again costs no signature check.)
            if (m_evidence.CheckSigs(proof, leader.pubkey, Genesis())) {
                offence = Offence{.pubkey = leader.pubkey, .slot = slot};
                break;
            }
        }
    }
    if (!offence) {
        // Not the peer's fault if this node cannot tell the leader yet, nor if it banned the leader already.
        reason = banned ? "pos-evidence-banned" : unknown ? "pos-evidence-unknown" : "bad-pos-evidence-sig";
        return std::nullopt;
    }
    // One proof per offender: one is all slashing takes (an offender is punished once), so that no validator's own
    // proofs of many slots fill the pool. A second one is a proof all the same, not kept nor relayed.
    if (m_evidence.HasOffender(offence->pubkey) || !m_evidence.Add(*offence, proof)) return offence;
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

bool HasPendingAnchorActions(const sidechain::State& side, const sidechain::Mainchain& record)
{
    // Peg in and peg out (the owner's rule, 2026-10-30; set changes: NeedsAnchor): a deposit waiting to be credited or
    // withdrawals waiting (a bundle). Not the staking accounting: the pot's reserve, the fee pool (PoolTotal), the
    // rewards owed and the unbond release countdown advance with the anchors that land (one at least every 8-16 h:
    // stall prevention). A fee pool of 0.0001479 ECX counted here made a node bid every mainchain block, its daily cap
    // spent in 4 hours.
    bool withdrawals{side.Bundle().has_value()};
    side.ForEachWithdrawal([&](const sidechain::Withdrawal&) { withdrawals = true; return false; });
    if (withdrawals) return true;
    const int from{std::max(side.MainHeight() + 1, record.Base())};
    const int height{record.Height()};
    // Bound an unexpectedly long catch-up scan by bidding conservatively.
    if (height - from > 144) return true;
    for (int at{from}; at <= height; ++at) {
        const auto block{record.GetBlock(at)};
        if (!block || !block->deposits.empty() || !block->bundles.empty()) return true;
    }
    return false;
}

bool NeedsAnchor(const sidechain::State& side, const sidechain::Mainchain& record, const Staking& staking, int64_t epoch)
{
    // A bond, an unbond or a jail in a block no anchor covers yet: the set the state makes differs from the last anchor's,
    // until an anchor's peg step makes it -- a validator joining or leaving, or a stake moved by 10 % (or min_stake)
    // at least (Staking::SetChangedMaterially): a small top-up anyone can make every block sets off no bid.
    return HasPendingAnchorActions(side, record) || staking.SetChangedMaterially(side.View(), epoch);
}

bool FastBlocksHooks::FollowerRound(sidechain::Follower& follower) const
{
    ChainstateManager* chainman{Chainman()};
    // (true: nothing for the follower's own merged mining either, which would bid without the node's policy.)
    if (!chainman || !chainman->m_mainchain) return true;
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
    // No mainchain block yet: no bid (true: not the follower's merged mining either, which offers its fee share without the
    // node's policy, past Off, max and the cap).
    if (!main_block) return true;

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
    // Its policy (pos/anchorbid.h) may be off: no bids, the chain relies on other bidders.
    // Both triggers ("Merged-mine as a backup producer": setbmm; "Anchor my validator") bid under the node's one policy,
    // from the same mainchain wallet, one request per mainchain block (doc/fastblocks-anchor-bids.md, "Triggers").
    std::optional<ValidatorBid> validator{ValidatorAnchoring()};
    const AnchorBidPolicy policy{BidPolicy()};
    const int64_t slot{SlotOf(TicksSinceEpoch<std::chrono::seconds>(NodeClock::now()), m_params)};
    if (validator && (!validator->policy.Bids() || !validator->policy.anchor)) {
        const bool off{!validator->policy.Bids()};
        validator.reset();
        LOCK(m_anchor_mutex);
        if (!m_bid_off_warned) {
            m_bid_off_warned = true;
            LogWarning("FastBlocks: this validator's anchorer is off (%s): the chain relies on other bidders for its anchors, and its blocks of stake halt after %d slots without one",
                       off ? "bid strategy off" : "\"Anchor my validator\" off", m_params.MaxUnanchoredSlots(slot));
        }
    }
    // These reads precede m_anchor_mutex: never acquire cs_main while holding the anchor lock.
    // A quiet chain needs no anchor of its own (user decision 2026-10-26): leaders and seeds come from the mainchain record
    // and the newest anchor's set, however old (SetFor), so epochs go on. What bids without fees is work waiting for an
    // anchor (the owner's rule, 2026-10-30: deposits, withdrawals, and bonds, unbonds and jails: NeedsAnchor), and stall
    // prevention near the unanchored limit (the pressure, in DecideBid); not a stall of blocks of stake by itself, nor the
    // staking accounting (the pot's reserve, the fee pool, rewards owed, the unbond release countdown).
    const bool stalled{Stalled(fast, main_block->time, PosActiveNow())};
    const bool needs_anchor{WITH_LOCK(::cs_main, {
        const sidechain::State side{chainman->ActiveChainstate().SideState()};
        return NeedsAnchor(side, record, m_staking, EpochOf(slot, m_params));
    })};
    // Value at risk (8.3.7): what the transactions of the blocks of stake no anchor covers move, read once a block.
    const CAmount at_risk{validator || mining.enabled ? ValueAtRisk(tip, fast.last_anchored_height) : 0};
    // Taking turns (8.3.7, AnchorBidPolicy::turns): of the active validators sorted by key, the one DesignatedBidder
    // names for the next mainchain block from the epoch's seed (else the mainchain tip, which every node following the
    // mainchain knows too) bids at once; a node of several keys has the turn if any of them has it. Not known (no set,
    // say): the turn is this node's, as before 8.3.7.
    bool designated{true};
    if (validator && policy.turns) {
        std::vector<uint256> active;
        std::optional<uint256> seed;
        {
            LOCK(::cs_main);
            const int64_t epoch{EpochOf(slot, m_params)};
            if (const auto set{SetFor(*tip, epoch)}; set && set->Active(epoch)) {
                const sidechain::StoreView& view{chainman->ActiveChainstate().SideState().View()};
                for (const Consensus::PosValidator& v : set->Validators()) {
                    // (A validator the chain banned leads no slot, and would never take its turn.)
                    if (const auto known{GetValidator(view, v.pubkey)}; !known || !known->tombstoned) active.push_back(v.pubkey);
                }
            }
            seed = EpochSeed(epoch, m_params, record);
        }
        if (const auto chosen{DesignatedBidder(active, seed.value_or(main_tip), main_height + 1)}) {
            designated = std::find(validator->keys.begin(), validator->keys.end(), *chosen) != validator->keys.end();
        }
    }
    // The mainchain's next-block fee estimate and the bid wallet's balance (once per mainchain block), the bids
    // made settled with the record; and the bid of the policy for a pot.
    if (validator || mining.enabled) PollBidSignals(follower, main_tip, policy);
    const auto decide{[&](CAmount pot) EXCLUSIVE_LOCKS_REQUIRED(m_anchor_mutex) {
        const int64_t now{TicksSinceEpoch<std::chrono::seconds>(NodeClock::now())};
        SettleBids(record);
        m_bid_inputs.pot = pot;
        m_bid_inputs.pending_actions = needs_anchor;
        m_bid_inputs.min_rate_per_kvb = m_params.min_bid_rate_per_kvb;
        m_bid_inputs.slot = slot;
        m_bid_inputs.first_unanchored_slot = fast.first_unanchored_slot;
        m_bid_inputs.max_unanchored_slots = m_params.MaxUnanchoredSlots(slot);
        m_bid_inputs.missed = m_bid_missed;
        m_bid_inputs.spent_in_period = SpentInPeriod(policy, now);
        m_bid_inputs.value_at_risk = at_risk;
        m_bid_inputs.reserve = m_anchor_reserve;
        m_bid_inputs.slot_seconds = m_params.slot_seconds;
        const BidTriggers triggers{.anchor = validator.has_value(), .mine = mining.enabled, .mine_always = mining.enabled && mining.always,
                                   .mine_amount = mining.amount, .stale = StaleLocked(now)};
        BidDecision d{DecideNodeBid(policy, m_bid_inputs, triggers)};
        // Work waiting without fees (peg work, set changes: anyone can make them cheaply): one such bid a mainchain block at
        // most, and while the last did not land (pending, or missed), one in 3 mainchain blocks. A stall bid is never held.
        if (d.bid && d.work) {
            const auto last{std::find_if(m_bid_history.rbegin(), m_bid_history.rend(), [](const AnchorBidRecord& r) { return r.work; })};
            if (last != m_bid_history.rend()) {
                const bool unsettled{last->status == AnchorBidRecord::Status::PENDING || last->status == AnchorBidRecord::Status::MISSED};
                if (last->main_tip == main_tip || (unsettled && main_height < last->main_height + WORK_BID_SPACING)) {
                    d.bid = false;
                    d.amount = 0;
                    d.kind = "throttled";
                    d.reason = strprintf("no bid: work waits, but a bid for it was made for mainchain block %d and did not land: one in %d mainchain blocks (%s)",
                                         last->main_height + 1, WORK_BID_SPACING, d.reason);
                }
            }
        }
        // Taking turns: a validator not designated for the block holds its bid until the need has gone turnbackup
        // mainchain blocks without an anchor landing, from anyone (the designated validator's bid, or another's); then it
        // bids as the backup. An anchor that lands starts the wait again. A stall bid (d.turns false) never waits, nor
        // the BMM blocks of a stall of blocks of stake (liveness), nor the merged miner of a node that is no validator.
        m_turn_designated = designated;
        m_turn_main_height = main_height + 1;
        if (d.bid && d.turns && validator && !stalled) {
            if (designated) {
                m_turn_held_since = -1;
                d.reason += ", this validator's turn";
            } else {
                bool landed{m_turn_held_since < 0 || m_turn_held_since > main_height};
                for (int at{m_turn_held_since + 1}; !landed && at <= main_height; ++at) landed = record.BmmAt(at).has_value();
                if (landed) m_turn_held_since = main_height;
                const int waited{main_height - m_turn_held_since};
                if (waited < policy.turn_backup) {
                    d.reason = strprintf("no bid: another validator's turn for mainchain block %d; this one bids if no anchor lands in %d mainchain block%s (%s)",
                                         main_height + 1, policy.turn_backup - waited, policy.turn_backup - waited == 1 ? "" : "s", d.reason);
                    d.bid = false;
                    d.amount = 0;
                    d.kind = "turn";
                } else {
                    d.reason += strprintf(", the backup: %d mainchain block%s without an anchor on other validators' turns", waited, waited == 1 ? "" : "s");
                }
            }
        } else if (!d.bid && d.kind != "throttled") {
            // No need any more (or none that takes turns): the wait starts again with the next.
            m_turn_held_since = -1;
        }
        return d;
    }};
    // A bid made, in the history (settled with the record, counted in the period's spending).
    const auto note{[&](int height, const uint256& tip, const uint256& commitment, CAmount offer, const BidDecision& decision) EXCLUSIVE_LOCKS_REQUIRED(m_anchor_mutex) {
        m_bid_history.push_back(AnchorBidRecord{.main_height = height, .main_tip = tip, .ticket = commitment, .offer = offer,
                                                .kind = decision.kind, .reason = decision.reason, .pressure = decision.pressure,
                                                .time = TicksSinceEpoch<std::chrono::seconds>(NodeClock::now()), .trigger = decision.trigger, .work = decision.work,
                                                .pot = m_bid_inputs.pot, .holdback = decision.holdback, .basis = decision.basis, .unfunded = decision.unfunded});
        while (m_bid_history.size() > BID_HISTORY) m_bid_history.pop_front();
        SaveBidsLocked();
    }};
    // No PoS blocks for a while (or none at all on this chain, or no active validator set): merged mined
    // blocks, as the template's -- by a validator too, for its bid whatever the fees (a BMM block
    // starts blocks of stake again).
    // The bid for such a block is the policy's for its fees (what the block pays its merged miner), when the block is built.
    if (stalled) {
        if (!mining.enabled && !validator) return false;
        sidechain::Follower::Mining own;
        if (mining.enabled) {
            // The merged miner's address, and its "always"; its counts are the node's (getbmminfo).
            own = mining;
        } else {
            own.enabled = true;
            own.coinbase_script = validator->payee;
            own.always = true;
        }
        BidDecision decision;
        own.decide = [&](CAmount fees) -> std::optional<CAmount> {
            LOCK(m_anchor_mutex);
            decision = decide(fees);
            if (!decision.bid) return std::nullopt;
            return decision.amount;
        };
        if (const auto made{follower.MineWith(own)}) {
            LOCK(m_anchor_mutex);
            note(made->main_prev_height, made->main_prev_hash, made->commitment, made->offer, decision);
            LogInfo("FastBlocks: BMM bid %s for mainchain block %d (%s): %s", FormatMoney(made->offer), made->main_prev_height + 1, decision.trigger, decision.reason);
        }
        return true;
    }
    // Otherwise anchors: a ticket per mainchain block.
    if (!mining.enabled && !validator) return true;
    // One ticket serves both triggers: its anchor anchors the chain (what the validator's anchorer bids for) and pays
    // its pot to the payee, the merged miner's address if setbmm is on (the operator's word), else the validator's payout.
    const CScript payee{mining.enabled ? mining.coinbase_script : validator->payee};
    if (!IsValidPayee(payee)) return true;
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
    // A validator's own bid is also made again, for the same mainchain block, when its policy bids a tenth more
    // than the bid in the mempool (the stall pressure rose, blocks were missed, the policy changed): mainchain
    // blocks may be hours apart.
    bool again{false};
    bool raise{false};
    CAmount last_offer{0};
    {
        LOCK(m_anchor_mutex);
        // (No ticket yet, for want of a block to anchor after the waiting commitment's: tried again
        // at each round, as blocks come.)
        if (m_bid.main_tip == main_tip && m_bid.made) {
            const BidDecision now{decide(m_bid_inputs.pot)};
            raise = now.bid && now.amount >= m_bid.offer + sidechain::Follower::MIN_OFFER && now.amount > m_bid.offer + m_bid.offer / 10;
            if (raise) {
                last_offer = m_bid.offer;
            } else {
                if (pending || m_bid.after_anchor_point || m_bid.again) return true;
                again = true;
                last_offer = m_bid.offer;
            }
        }
    }
    if (again) {
        const bool bundle{WITH_LOCK(::cs_main, {
            const sidechain::State side{chainman->ActiveChainstate().SideState()};
            const Consensus::SidechainParams& params{chainman->GetConsensus().sidechain};
            // Held back by a supported bundle of another branch where the mainchain has no single payout.
            return side.NextBundle(tip->nHeight + 1, tip->GetBlockHash(), params, nullptr, std::nullopt,
                                   chainman->m_mainchain && side.MainPendingNext(*chainman->m_mainchain, params)).has_value();
        })};
        if (!bundle) {
            LOCK(m_anchor_mutex);
            m_bid.again = true;
            return true;
        }
    }

    AnchorTicket ticket;
    // The height of the block the waiting commitment's ticket anchors, if this node knows it: that anchor pays
    // the pot up to it, before this one is acknowledged.
    std::optional<int> waiting_height;
    {
        LOCK(::cs_main);
        const CBlockIndex* target{TicketTarget(*tip, fast, slot, /*after_anchor_point=*/!pending)};
        // After the block the waiting commitment's ticket anchors, if this node knows it: anchors are
        // acknowledged in the order of their commitments, each at or after the block the one before
        // anchored -- and one ticket committed twice is one anchor (the second commitment is
        // skipped). Two mainchain blocks within a slot: the tip, the second time.
        if (const auto waiting{pending ? GetTicket(*record.BmmAt(main_height)) : std::nullopt}) {
            const CBlockIndex* before{chainman->m_blockman.LookupBlockIndex(waiting->anchored)};
            if (before) waiting_height = before->nHeight;
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
    // validator at least its own bid. A validator's own anchorer bids as its options say (ValidatorBid::For:
    // in fees mode the pot whole, the bidder's share of the fees this anchor earns), and always, at 0 too.
    // (Less what the waiting commitment's anchor will pay first: its pot is not this one's, nor bid twice.)
    const int64_t expected{WITH_LOCK(::cs_main, {
        const sidechain::StoreView& view{chainman->ActiveChainstate().SideState().View()};
        const int64_t paid_first{waiting_height ? std::max<int64_t>(fast.pot_paid, PotAccruedAt(view, *waiting_height)) : fast.pot_paid};
        return PotAccruedAt(view, chainman->m_blockman.LookupBlockIndex(ticket.anchored)->nHeight) - paid_first;
    })};
    const BidDecision decision{WITH_LOCK(m_anchor_mutex, return decide(expected))};
    if (!decision.bid) {
        LOCK(m_anchor_mutex);
        if (!raise && !again) {
            // Retry this mainchain block when new fees or peg actions arrive, or pressure rises.
            if (m_bid.main_tip != main_tip && decision.kind != "idle") LogInfo("FastBlocks: no anchor bid for mainchain block %d: %s", main_height + 1, decision.reason);
            m_bid = Bid{.main_tip = main_tip, .made = false};
        }
        return true;
    }
    CAmount offer{decision.amount};
    // (Made again: more than the bid it replaces in the mainchain's mempool, within max and the period's cap.)
    if (again || raise) {
        const auto replacement{WITH_LOCK(m_anchor_mutex, return ReplacementBid(policy, offer, last_offer,
            SpentInPeriod(policy, TicksSinceEpoch<std::chrono::seconds>(NodeClock::now())), decision.SpendsReserve()))};
        if (!replacement) return true;
        offer = *replacement;
    }
    {
        LOCK(m_anchor_mutex);
        if (again) {
            m_bid.again = true;
        } else if (!raise) {
            m_bid = Bid{.main_tip = main_tip, .offer = offer, .after_anchor_point = !pending, .made = true};
        }
    }
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
        // Made again for the same mainchain block: the request replaced the one before in the mempool.
        if ((again || raise) && !m_bid_history.empty() && m_bid_history.back().main_tip == main_tip) m_bid_history.pop_back();
        note(main_height, main_tip, ticket.Hash(), offer, decision);
    }
    AddTicket(ticket, /*own=*/true);
    RelayTicket(ticket);
    LogInfo("FastBlocks: anchor ticket %s for block %s, %d offered%s", ticket.Hash().ToString(), ticket.anchored.ToString(), offer, again ? " (again, for a block after the last acknowledgement)" : raise ? " (raised, for the same mainchain block)" : "");
    LogInfo("FastBlocks: anchor bid %s for mainchain block %d (%s, stall pressure %.2f): %s", FormatMoney(offer), main_height + 1, decision.trigger, decision.pressure, decision.reason);
    return true;
}

CAmount FastBlocksHooks::ValueAtRisk(const CBlockIndex* tip, int anchored_height) const
{
    ChainstateManager* chainman{Chainman()};
    if (!chainman || !tip) return 0;
    // The blocks above the last one anchored: blocks of stake (and the acknowledgements among them), max_unanchored_slots
    // of them at most (3840 on eCash mainnet). Each is read once while it is in the run; the run's total once a tip.
    {
        LOCK(m_risk_mutex);
        if (m_risk_for == tip->GetBlockHash() && m_risk_anchored == anchored_height) return m_risk_value;
    }
    std::vector<const CBlockIndex*> run;
    {
        LOCK(::cs_main);
        for (const CBlockIndex* index{tip}; index && index->nHeight > anchored_height && run.size() <= static_cast<size_t>(m_params.MaxUnanchoredSlotsEver());
             index = index->pprev) {
            run.push_back(index);
        }
    }
    std::map<uint256, CAmount> moved;
    CAmount total{0};
    for (const CBlockIndex* index : run) {
        const uint256 hash{index->GetBlockHash()};
        std::optional<CAmount> value{WITH_LOCK(m_risk_mutex, const auto it{m_risk_moved.find(hash)}; return it != m_risk_moved.end() ? std::optional{it->second} : std::nullopt)};
        if (!value) {
            // The value the block's transactions move: the outputs of all but the coinbase (change counts too: an
            // approximation, on the side of bidding). A block not on disk moves nothing that is known.
            CBlock block;
            const bool read{WITH_LOCK(::cs_main, return (index->nStatus & BLOCK_HAVE_DATA) && chainman->m_blockman.ReadBlock(block, *index))};
            CAmount sum{0};
            for (size_t i{1}; read && i < block.vtx.size(); ++i) {
                for (const CTxOut& out : block.vtx[i]->vout) {
                    if (MoneyRange(out.nValue)) sum = std::min(sum + out.nValue, MAX_MONEY);
                }
            }
            value = sum;
        }
        moved.emplace(hash, *value);
        total = std::min(total + *value, MAX_MONEY);
    }
    LOCK(m_risk_mutex);
    m_risk_moved = std::move(moved);
    m_risk_for = tip->GetBlockHash();
    m_risk_anchored = anchored_height;
    m_risk_value = total;
    return total;
}

std::optional<FastBlocksHooks::ValidatorBid> FastBlocksHooks::ValidatorAnchoring() const
{
    LOCK(m_anchor_mutex);
    return m_validator;
}

namespace {
/** Whether the options say anything of the anchor bid (else, on regtest, no anchorer). */
bool AnchorBidOptionsGiven(const ArgsManager& args)
{
    return args.IsArgSet("-validatoranchorbid") || args.IsArgSet("-validatoranchorbidstrategy") || args.IsArgSet("-validatoranchorbidfixed") ||
           args.IsArgSet("-validatoranchorbidpreventstall");
}
} // namespace

UniValue LegacyBidSettings(const UniValue& saved, const AnchorBidPolicy& defaults, const ArgsManager& args, ChainType chain)
{
    UniValue fields{saved};
    if (!fields.find_value("strategy").isNull()) return fields;
    if (chain == ChainType::REGTEST && !AnchorBidOptionsGiven(args)) return fields;
    // 8.3.5 saved only the fields changed: without a strategy, the options' (8.3.5's default, feespreventstall, without
    // one), which bid at least the floor: Fees + minimum with the floor of the settings above 0.
    std::optional<StrategyWord> word;
    if (args.IsArgSet("-validatoranchorbidstrategy") && !args.IsArgNegated("-validatoranchorbidstrategy")) {
        word = ParseStrategyWord(args.GetArg("-validatoranchorbidstrategy", ""), StrategyContext::OPTIONS);
    } else if (args.GetArg("-validatoranchorbidmode", "") == "fees") {
        word = ParseStrategyWord("fees", StrategyContext::LEGACY_SETTINGS);
    } else {
        word = ParseStrategyWord("feespreventstall");
    }
    if (!word || !word->fees_or_floor) return fields;
    fields.pushKV("strategy", "feespreventstall");
    // (What the options say of stall prevention: feespreventstall's on, 8.3.5's fees off, or -validatoranchorbidpreventstall.)
    if (fields.find_value("preventstall").isNull()) fields.pushKV("preventstall", defaults.prevent_stall);
    return fields;
}

UniValue NormalizedLegacySettings(const UniValue& fields, const AnchorBidPolicy& policy)
{
    const UniValue said{PolicyToUniValue(policy)};
    UniValue out{UniValue::VOBJ};
    std::optional<StrategyWord> word;
    if (const UniValue& strategy{fields.find_value("strategy")}; strategy.isStr()) word = ParseStrategyWord(strategy.get_str(), StrategyContext::LEGACY_SETTINGS);
    for (const std::string& key : fields.getKeys()) {
        if (key == "strategy" || key == "preventstall" || key == "anchor") continue;
        out.pushKV(key, said.find_value(key));
    }
    if (word && word->anchor_off) {
        out.pushKV("anchor", false);
    } else if (word) {
        // A strategy that depends on the floor stays the word that says so (feespreventstall, whatever preventstall says).
        out.pushKV("strategy", word->fees_or_floor ? std::string{"feespreventstall"} : StrategyName(policy.strategy));
    }
    out.pushKV("preventstall", policy.prevent_stall);
    return out;
}

AnchorBidPolicy AnchorBidPolicyFromArgs(const ArgsManager& args, ChainType chain)
{
    AnchorBidPolicy policy;
    // On regtest unless given: no anchorer, and no period cap (the merged miner of a regtest node bids under this policy
    // too, for as many blocks as a test mines): its fee share, without stall prevention, as setbmm did before 8.3.5.
    const bool given{AnchorBidOptionsGiven(args)};
    if (chain == ChainType::REGTEST && !given) {
        policy.anchor = false;
        policy.prevent_stall = false;
        policy.period_cap = 0;
        // (Nor 8.3.7's batches, anchor reserve or turns: the fee share whenever it pays for a request.)
        policy.batch = 0;
        policy.reserve_pct = 0;
        policy.turns = false;
    }
    // The strategy the options say before -validatoranchorbidstrategy (which wins): the old default, feespreventstall,
    // is Fees + minimum with a floor above 0, else Fees (ApplyPolicyFields); -validatoranchorbidmode=fees, the pot whole.
    std::string strategy;
    if (args.IsArgNegated("-validatoranchorbid")) {
        // The anchorer off; the merged miner (setbmm) bids all the same, as before.
        policy.anchor = false;
    } else if (args.IsArgSet("-validatoranchorbid")) {
        const std::string value{args.GetArg("-validatoranchorbid", "")};
        const auto parsed{ParseMoney(value)};
        if (value == "off") {
            policy.anchor = false;
        } else if (!parsed || !MoneyRange(*parsed)) {
            LogError("FastBlocks: -validatoranchorbid is not an amount: the validator's anchorer is off");
            policy.anchor = false;
        } else {
            policy.floor = *parsed;
            policy.fixed = *parsed;
            if (*parsed > 0) strategy = "feespreventstall";
        }
    }
    const std::string mode{args.GetArg("-validatoranchorbidmode", "")};
    if (mode == "fees") {
        // The pot whole, passed into the bid; no stall prevention.
        strategy = "fees";
        policy.fee_share_bps = 10000;
    } else if (mode == "standard") {
        strategy = "feespreventstall";
    } else if (!mode.empty()) {
        LogError("FastBlocks: -validatoranchorbidmode is standard or fees, not %s: standard", mode);
    }
    if (!strategy.empty()) {
        // (The words of the older options, in their meaning: -validatoranchorbidmode=fees is 8.3.5's fees, at least the
        // floor and no stall prevention.)
        UniValue one{UniValue::VOBJ};
        one.pushKV("strategy", strategy);
        std::string error;
        ApplyPolicyFields(policy, one, error, StrategyContext::LEGACY_SETTINGS);
    }
    // The other options, as setanchorbidpolicy's fields (preventstall after the strategy, whose old names say it too).
    UniValue fields{UniValue::VOBJ};
    for (const auto& [option, field] : std::initializer_list<std::pair<const char*, const char*>>{
             {"-validatoranchorbidstrategy", "strategy"}, {"-validatoranchorbidpreventstall", "preventstall"}, {"-validatoranchorbidmax", "max"}, {"-validatoranchorbidfixed", "fixed"},
             {"-validatoranchorbidshare", "feeshare"}, {"-validatoranchorbidescalation", "escalationstart"}, {"-validatoranchorbidcurve", "curve"},
             {"-validatoranchorbidinterpolate", "interpolate"}, {"-validatoranchorbidfeeestimate", "usefeeestimate"}, {"-validatoranchorbidrebid", "rebidafter"},
             {"-validatoranchorbidperiodcap", "periodcap"}, {"-validatoranchorbidlowbalance", "lowbalanceblocks"},
             {"-validatoranchorbidstallreserve", "stallreserve"}, {"-validatoranchorbidbatch", "batch"}, {"-validatoranchorbidheartbeathours", "heartbeathours"},
             {"-validatoranchorbidvalueatrisk", "valueatrisk"}, {"-validatoranchorbidreservepct", "reservepct"}, {"-validatoranchorbidreservetarget", "reservetarget"},
             {"-validatoranchorbidturns", "turns"}, {"-validatoranchorbidturnbackup", "turnbackup"}}) {
        if (!args.IsArgSet(option)) continue;
        UniValue one{UniValue::VOBJ};
        // (-novalidatoranchorbidpreventstall: false.)
        std::string value{args.IsArgNegated(option) ? std::string{"0"} : args.GetArg(option, "")};
        // A switch given alone (-validatoranchorbidpreventstall): on.
        const std::string_view name{field};
        if (value.empty() && (name == "preventstall" || name == "interpolate" || name == "usefeeestimate" || name == "turns")) value = "1";
        one.pushKV(field, value);
        std::string error;
        AnchorBidPolicy changed{policy};
        // (-validatoranchorbidstrategy: the new names, but off, 8.3.5's: the anchorer off alone.)
        if (!ApplyPolicyFields(changed, one, error, StrategyContext::OPTIONS)) {
            LogError("FastBlocks: %s: %s (not used)", option, error);
        } else {
            policy = changed;
        }
    }
    if (const std::string error{CheckPolicy(policy)}; !error.empty()) {
        // A floor or a fixed bid above the cap: the cap raised to them.
        policy.max = std::max({policy.max, policy.floor, policy.fixed});
        if (const std::string again{CheckPolicy(policy)}; !again.empty()) {
            LogError("FastBlocks: the anchor bid options do not make a policy (%s): the defaults", again);
            const AnchorBidPolicy chosen{policy};
            policy = AnchorBidPolicy{};
            policy.strategy = chosen.strategy;
            policy.prevent_stall = chosen.prevent_stall;
            policy.anchor = chosen.anchor;
        } else {
            LogWarning("FastBlocks: %s: the most the anchorer bids is %s", error, FormatMoney(policy.max));
        }
    }
    return policy;
}

std::string AnchorBidPolicySummary(const AnchorBidPolicy& p)
{
    std::string what;
    switch (p.strategy) {
    case BidStrategy::OFF: return "no bids (strategy off)";
    case BidStrategy::FEES:
        what = strprintf("the fee share (%.2f %%) when there are fees, no minimum (fees)", p.fee_share_bps / 100.0);
        if (p.batch > 0) what += strprintf(", in batches of %g x the landing price", p.batch);
        if (p.turns) what += strprintf(", taking turns (the backup after %d mainchain blocks)", p.turn_backup);
        break;
    case BidStrategy::FEES_MINIMUM: what = strprintf("the fee share (%.2f %%), at least the minimum (the floor %s or the relay minimum), every mainchain block (feesminimum)", p.fee_share_bps / 100.0, FormatMoney(p.floor)); break;
    case BidStrategy::MINIMUM: what = strprintf("the minimum (the floor %s or the relay minimum) every mainchain block (minimum)", FormatMoney(p.floor)); break;
    case BidStrategy::FIXED: what = strprintf("%s every mainchain block (fixed)", FormatMoney(p.fixed)); break;
    }
    what += p.prevent_stall ? strprintf(", preventing stalls from stall pressure %.2f", p.escalation_start) : ", no stall prevention";
    if (p.prevent_stall && (p.strategy == BidStrategy::FEES || p.strategy == BidStrategy::FEES_MINIMUM)) {
        if (p.heartbeat_hours > 0) what += strprintf(", a heartbeat after %g h without an anchor", p.heartbeat_hours);
        if (p.value_at_risk > 0) what += strprintf(", a bid when %s is at risk", FormatMoney(p.value_at_risk));
    }
    if (p.reserve_pct > 0 && p.reserve_target > 0 && (p.strategy == BidStrategy::FEES || p.strategy == BidStrategy::FEES_MINIMUM)) {
        what += strprintf(", %d %% of fee bids kept for the anchor reserve up to %s", p.reserve_pct, FormatMoney(p.reserve_target));
    }
    what += strprintf(", at most %s a bid", FormatMoney(p.max));
    if (p.period_cap > 0) what += strprintf(" and %s in %d h", FormatMoney(p.period_cap), p.period_hours);
    if (!p.anchor) what += "; the validator's anchorer off (merged mining alone)";
    return what;
}

void FastBlocksHooks::SettleBids(const sidechain::Mainchain& record) const
{
    const int height{record.Height()};
    bool settled{false};
    for (AnchorBidRecord& bid : m_bid_history) {
        if (bid.status != AnchorBidRecord::Status::PENDING || bid.main_height >= height) continue;
        settled = true;
        const auto at{record.HashAt(bid.main_height)};
        const auto bmm{record.BmmAt(bid.main_height + 1)};
        if (!at || *at != bid.main_tip) {
            // The mainchain went another way: the request followed a block it no longer has (not counted).
            bid.status = AnchorBidRecord::Status::OUTBID;
        } else if (bmm && *bmm == bid.ticket) {
            bid.status = AnchorBidRecord::Status::LANDED;
            m_bid_landed.emplace_back(bid.time, bid.offer);
            m_anchors_landed.push_back(LandedAnchor{.time = bid.time, .offer = bid.offer, .pot = bid.pot, .basis = bid.basis.empty() ? bid.kind : bid.basis});
            m_bid_missed = 0;
            // The anchor reserve: a fee bid's holdback in (up to the target), a bid without fees behind it out (not below 0;
            // what it spent past an empty reserve is the deficit, reported: an empty reserve never stops such a bid).
            const CAmount target{PolicyLocked().reserve_target};
            if (bid.holdback > 0) m_anchor_reserve = std::min(m_anchor_reserve + bid.holdback, std::max(target, m_anchor_reserve));
            if (bid.unfunded) {
                const CAmount covered{std::min(m_anchor_reserve, bid.offer)};
                m_anchor_reserve -= covered;
                m_anchor_deficit += bid.offer - covered;
            }
            if (m_anchor_reserve >= target) m_anchor_deficit = 0;
        } else if (bmm) {
            // Another commitment of this chain got in: anchored all the same.
            bid.status = AnchorBidRecord::Status::OUTBID;
            m_bid_missed = 0;
        } else {
            bid.status = AnchorBidRecord::Status::MISSED;
            ++m_bid_missed;
        }
    }
    if (settled) SaveBidsLocked();
    if (m_bid_missed == 0) m_bid_missed_warned = 0;
    const int warn{m_validator ? m_validator->policy.warn_missed : 0};
    if (warn > 0 && m_bid_missed >= warn && m_bid_missed != m_bid_missed_warned) {
        m_bid_missed_warned = m_bid_missed;
        const AnchorBidRecord& last{m_bid_history.back()};
        // A bid of 10 x the relay minimum or more that misses is not too low: the pool may not be including BMM commitments
        // at all (its enforcer, or its side), and raising the bid would not help.
        const CAmount relay_minimum{RelayMinimum(PolicyLocked(), m_bid_inputs)};
        if (relay_minimum > 0 && last.offer >= POOL_WARNING_RELAY_MULTIPLE * relay_minimum) {
            LogWarning("FastBlocks: %d mainchain blocks in a row came without this validator's commitment, though the last bid, %s (about %.2f sat/vB), is %d times the relay minimum (%s) "
                       "or more: the mainchain's pool may not be including BMM commitments (its enforcer, or the pool's side), rather than the bid being too low",
                       m_bid_missed, FormatMoney(last.offer), static_cast<double>(last.offer) / (m_validator ? m_validator->policy.request_vsize : DEFAULT_ANCHOR_REQUEST_VSIZE),
                       POOL_WARNING_RELAY_MULTIPLE, FormatMoney(relay_minimum));
            return;
        }
        LogWarning("FastBlocks: your anchor bid is too low: %d mainchain blocks in a row came without this validator's commitment (the last bid %s, about %.2f sat/vB%s); "
                   "raise the floor or the escalation (setanchorbidpolicy), or the chain halts after %d slots without an anchor",
                   m_bid_missed, FormatMoney(last.offer), static_cast<double>(last.offer) / (m_validator ? m_validator->policy.request_vsize : DEFAULT_ANCHOR_REQUEST_VSIZE),
                   m_bid_inputs.fee_rate_per_kvb ? strprintf(", the mainchain's next-block estimate %.2f sat/vB", *m_bid_inputs.fee_rate_per_kvb / 1000.0) : "",
                   m_params.MaxUnanchoredSlots(SlotOf(TicksSinceEpoch<std::chrono::seconds>(NodeClock::now()), m_params)));
    }
}

CAmount FastBlocksHooks::SpentInPeriod(const AnchorBidPolicy& policy, int64_t now) const
{
    // Every landed bid of the node, whatever set it off (both triggers, and by hand): one budget. Kept a month at
    // most (the longest period), not only the history's last bids (a day has 144 mainchain blocks or more).
    while (!m_bid_landed.empty() && m_bid_landed.front().first <= now - BID_LANDED_SECONDS) m_bid_landed.pop_front();
    CAmount spent{0};
    for (const auto& [time, offer] : m_bid_landed) {
        if (time > now - int64_t{policy.period_hours} * 3600) spent += offer;
    }
    return spent;
}

CAmount FastBlocksHooks::StaleLocked(int64_t now) const
{
    std::vector<PastBid> bids;
    bids.reserve(m_bid_history.size());
    for (const AnchorBidRecord& bid : m_bid_history) {
        bids.push_back(PastBid{.status = static_cast<PastBid::Status>(bid.status), .offer = bid.offer, .time = bid.time});
    }
    return StaleOffer(bids, now);
}

AnchorBidPolicy FastBlocksHooks::BidPolicy() const
{
    LOCK(m_anchor_mutex);
    return PolicyLocked();
}

void FastBlocksHooks::NoteBid(int main_height, const uint256& main_tip, const uint256& commitment, CAmount offer, const std::string& trigger) const
{
    LOCK(m_anchor_mutex);
    m_bid_history.push_back(AnchorBidRecord{.main_height = main_height, .main_tip = main_tip, .ticket = commitment, .offer = offer, .kind = trigger,
                                            .reason = "by hand (requestbmmblock)", .time = TicksSinceEpoch<std::chrono::seconds>(NodeClock::now()), .trigger = trigger,
                                            .basis = "hand"});
    while (m_bid_history.size() > BID_HISTORY) m_bid_history.pop_front();
    SaveBidsLocked();
}

void FastBlocksHooks::SaveBidsLocked() const
{
    if (m_bid_file.empty()) return;
    UniValue landed{UniValue::VARR};
    const int64_t now{TicksSinceEpoch<std::chrono::seconds>(NodeClock::now())};
    for (const auto& [time, offer] : m_bid_landed) {
        if (time <= now - BID_LANDED_SECONDS) continue;
        UniValue entry{UniValue::VARR};
        entry.push_back(time);
        entry.push_back(offer);
        landed.push_back(std::move(entry));
    }
    UniValue history{UniValue::VARR};
    for (const AnchorBidRecord& bid : m_bid_history) {
        UniValue entry{UniValue::VOBJ};
        entry.pushKV("mainheight", bid.main_height);
        entry.pushKV("maintip", bid.main_tip.GetHex());
        entry.pushKV("ticket", bid.ticket.GetHex());
        entry.pushKV("offer", bid.offer);
        entry.pushKV("kind", bid.kind);
        entry.pushKV("reason", bid.reason);
        entry.pushKV("pressure", bid.pressure);
        entry.pushKV("status", static_cast<int>(bid.status));
        entry.pushKV("time", bid.time);
        entry.pushKV("trigger", bid.trigger);
        entry.pushKV("work", bid.work);
        entry.pushKV("pot", bid.pot);
        entry.pushKV("holdback", bid.holdback);
        entry.pushKV("basis", bid.basis);
        entry.pushKV("unfunded", bid.unfunded);
        history.push_back(std::move(entry));
    }
    // (8.3.7: what anchoring collected and paid, and the anchor reserve, beside what 8.3.6 reads, which it leaves alone.)
    UniValue anchors{UniValue::VARR};
    for (const LandedAnchor& anchor : m_anchors_landed) {
        if (anchor.time <= now - ANCHORS_LANDED_SECONDS) continue;
        UniValue entry{UniValue::VARR};
        entry.push_back(anchor.time);
        entry.push_back(anchor.offer);
        entry.push_back(anchor.pot);
        entry.push_back(anchor.basis);
        anchors.push_back(std::move(entry));
    }
    UniValue out{UniValue::VOBJ};
    out.pushKV("version", 1);
    out.pushKV("landed", std::move(landed));
    out.pushKV("history", std::move(history));
    out.pushKV("missed", m_bid_missed);
    out.pushKV("anchors", std::move(anchors));
    out.pushKV("reserve", m_anchor_reserve);
    out.pushKV("deficit", m_anchor_deficit);
    // Written whole beside it, then moved over it: a crash leaves the old file or the new one.
    fs::path temp{m_bid_file};
    temp += ".new";
    {
        std::ofstream file{static_cast<const std::filesystem::path&>(temp), std::ios::out | std::ios::trunc};
        file << out.write(1) << "\n";
        if (!file.good()) {
            LogWarning("FastBlocks: the bids could not be written to %s: the period's spending starts again after a restart", fs::PathToString(temp));
            return;
        }
    }
    if (!RenameOver(temp, m_bid_file)) LogWarning("FastBlocks: the bids could not be written to %s", fs::PathToString(m_bid_file));
}

void FastBlocksHooks::LoadBids(const fs::path& path) const
{
    UniValue in;
    {
        std::ifstream file{static_cast<const std::filesystem::path&>(path)};
        if (file.is_open()) {
            std::stringstream text;
            text << file.rdbuf();
            if (!in.read(text.str()) || !in.isObject()) {
                LogWarning("FastBlocks: %s is not a file of bids: not read (written again with the next bid)", fs::PathToString(path));
                in = UniValue{};
            }
        }
    }
    const int64_t now{TicksSinceEpoch<std::chrono::seconds>(NodeClock::now())};
    LOCK(m_anchor_mutex);
    m_bid_file = path;
    if (!in.isObject()) return;
    const auto number{[](const UniValue& v) -> std::optional<int64_t> {
        if (!v.isNum()) return std::nullopt;
        try {
            return v.getInt<int64_t>();
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }};
    m_bid_landed.clear();
    if (const UniValue& landed{in.find_value("landed")}; landed.isArray()) {
        for (const UniValue& entry : landed.getValues()) {
            if (!entry.isArray() || entry.size() != 2) continue;
            const auto time{number(entry[0])};
            const auto offer{number(entry[1])};
            if (!time || !offer || !MoneyRange(*offer) || *time <= now - BID_LANDED_SECONDS) continue;
            m_bid_landed.emplace_back(*time, *offer);
        }
    }
    std::sort(m_bid_landed.begin(), m_bid_landed.end());
    m_bid_history.clear();
    if (const UniValue& history{in.find_value("history")}; history.isArray()) {
        for (const UniValue& entry : history.getValues()) {
            if (!entry.isObject()) continue;
            const auto height{number(entry.find_value("mainheight"))};
            const auto offer{number(entry.find_value("offer"))};
            const auto status{number(entry.find_value("status"))};
            const auto time{number(entry.find_value("time"))};
            const UniValue& tip{entry.find_value("maintip")};
            const UniValue& ticket{entry.find_value("ticket")};
            if (!height || !offer || !MoneyRange(*offer) || !status || *status < 0 || *status > static_cast<int>(AnchorBidRecord::Status::OUTBID) || !time ||
                !tip.isStr() || !ticket.isStr()) {
                continue;
            }
            const auto tip_hash{uint256::FromHex(tip.get_str())};
            const auto ticket_hash{uint256::FromHex(ticket.get_str())};
            if (!tip_hash || !ticket_hash) continue;
            const UniValue& pressure{entry.find_value("pressure")};
            const auto text{[&](const char* key) { const UniValue& v{entry.find_value(key)}; return v.isStr() ? v.get_str() : std::string{}; }};
            // (8.3.7's fields: none in the files of 8.3.6, which read as bids without a pot nor a holdback.)
            const auto amount{[&](const char* key) { const auto v{number(entry.find_value(key))}; return v && MoneyRange(*v) ? *v : CAmount{0}; }};
            m_bid_history.push_back(AnchorBidRecord{.main_height = static_cast<int>(*height), .main_tip = *tip_hash, .ticket = *ticket_hash, .offer = *offer,
                                                    .kind = text("kind"), .reason = text("reason"), .pressure = pressure.isNum() ? pressure.get_real() : 0,
                                                    .status = static_cast<AnchorBidRecord::Status>(*status), .time = *time, .trigger = text("trigger"),
                                                    .work = entry.find_value("work").isTrue(), .pot = amount("pot"), .holdback = amount("holdback"),
                                                    .basis = text("basis"), .unfunded = entry.find_value("unfunded").isTrue()});
        }
    }
    while (m_bid_history.size() > BID_HISTORY) m_bid_history.pop_front();
    if (const auto missed{number(in.find_value("missed"))}; missed && *missed >= 0 && *missed < 1'000'000) m_bid_missed = static_cast<int>(*missed);
    m_anchors_landed.clear();
    if (const UniValue& anchors{in.find_value("anchors")}; anchors.isArray()) {
        for (const UniValue& entry : anchors.getValues()) {
            if (!entry.isArray() || entry.size() != 4 || !entry[3].isStr()) continue;
            const auto time{number(entry[0])};
            const auto offer{number(entry[1])};
            const auto pot{number(entry[2])};
            if (!time || !offer || !pot || !MoneyRange(*offer) || !MoneyRange(*pot) || *time <= now - ANCHORS_LANDED_SECONDS) continue;
            m_anchors_landed.push_back(LandedAnchor{.time = *time, .offer = *offer, .pot = *pot, .basis = entry[3].get_str()});
        }
    }
    std::sort(m_anchors_landed.begin(), m_anchors_landed.end(), [](const LandedAnchor& a, const LandedAnchor& b) { return a.time < b.time; });
    if (const auto reserve{number(in.find_value("reserve"))}; reserve && MoneyRange(*reserve)) m_anchor_reserve = *reserve;
    if (const auto deficit{number(in.find_value("deficit"))}; deficit && MoneyRange(*deficit)) m_anchor_deficit = *deficit;
    LogInfo("FastBlocks: %d bids read from %s (%d landed in the last %d days; the anchor reserve %s)", m_bid_history.size(), fs::PathToString(path), m_bid_landed.size(),
            BID_LANDED_SECONDS / 86400, FormatMoney(m_anchor_reserve));
}

void FastBlocksHooks::PollBidSignals(const sidechain::Follower& follower, const uint256& main_tip, const AnchorBidPolicy& policy) const
{
    {
        LOCK(m_anchor_mutex);
        if (m_bid_polled_for == main_tip) return;
        m_bid_polled_for = main_tip;
    }
    const sidechain::MainClient& client{follower.Client()};
    const auto amount{[](const UniValue& value) -> std::optional<CAmount> {
        if (!value.isNum()) return std::nullopt;
        const auto parsed{ParseMoney(value.getValStr())};
        return parsed && MoneyRange(*parsed) ? parsed : std::nullopt;
    }};
    // The mainchain's fee rate for its next block (coins per 1000 vbytes): estimatesmartfee (Backbone, Bitcoin
    // Core), else estimatefee (Bitcoin ABC); none through the enforcer's interface alone (the floor then).
    std::optional<CAmount> rate;
    std::string source{"none: the floor"};
    if (policy.use_fee_estimate && policy.PreventsStall()) {
        try {
            UniValue params{UniValue::VARR};
            params.push_back(1);
            const UniValue estimate{client.Call("estimatesmartfee", params)};
            if (estimate.isObject()) rate = amount(estimate.find_value("feerate"));
            if (rate && *rate <= 0) rate.reset();
            if (rate) source = "estimatesmartfee";
        } catch (const std::exception&) {
        } catch (const UniValue&) {
        }
        if (!rate) {
            try {
                const UniValue estimate{client.Call("estimatefee", UniValue{UniValue::VARR})};
                rate = amount(estimate);
                if (rate && *rate <= 0) rate.reset();
                if (rate) source = "estimatefee";
            } catch (const std::exception&) {
            } catch (const UniValue&) {
            }
        }
    } else {
        source = "not used";
    }
    CAmount relay_rate{1000}; // 1 sat/vB if the backend exposes no relay policy.
    try {
        const UniValue network{client.Call("getnetworkinfo", UniValue{UniValue::VARR})};
        if (const auto relay{amount(network.find_value("relayfee"))}) relay_rate = *relay;
    } catch (const std::exception&) {
    } catch (const UniValue&) {
    }
    // The mempool's least fee for entry raises the relay floor (it is no next-block estimate: an escalation never
    // multiplies it).
    try {
        const UniValue mempool{client.Call("getmempoolinfo", UniValue{UniValue::VARR})};
        if (const auto least{amount(mempool.find_value("mempoolminfee"))}) relay_rate = std::max(relay_rate, *least);
    } catch (const std::exception&) {
    } catch (const UniValue&) {
    }
    // The bid wallet: the mainchain node's (or the enforcer's), which pays the requests.
    std::optional<CAmount> balance;
    std::string balance_error;
    try {
        const UniValue balances{client.Call("getbalances", UniValue{UniValue::VARR}, /*wallet=*/true)};
        // What it can spend: its confirmed coins and its unconfirmed ones (the enforcer's interface gives no "trusted",
        // only confirmed and pending: in a mainchain gap every coin of a wallet that bids is the unconfirmed change of
        // its last bid, which it spends all the same; read as confirmed alone it was "0.00", lab 2026-10-24).
        const UniValue& mine{balances.find_value("mine")};
        balance = amount(mine.find_value("trusted"));
        if (balance) {
            if (const auto pending{amount(mine.find_value("untrusted_pending"))}; pending && *pending > 0) *balance += *pending;
        }
        if (!balance) balance_error = "the mainchain wallet said no balance";
    } catch (const std::exception& e) {
        balance_error = e.what();
    } catch (const UniValue&) {
        balance_error = "the mainchain wallet could not be read";
    }
    // What the last mainchain block took for well-formed BMM requests (any slot), for a node whose mainchain node
    // answers getblock (not through the enforcer's interface alone): kept for the last 12 blocks (RobustIncludedRate).
    std::optional<std::vector<BmmRequestSeen>> seen;
    try {
        UniValue args{UniValue::VARR};
        args.push_back(main_tip.GetHex());
        args.push_back(2);
        const UniValue block{client.Call("getblock", args)};
        const auto prev{uint256::FromHex(block.find_value("previousblockhash").isStr() ? block.find_value("previousblockhash").get_str() : "")};
        const UniValue& txs{block.find_value("tx")};
        if (prev && txs.isArray()) {
            seen.emplace();
            for (size_t i{0}; i < txs.size(); ++i) {
                const UniValue& tx{txs[i]};
                const UniValue& vouts{tx.find_value("vout")};
                std::optional<uint8_t> slot;
                for (size_t j{0}; vouts.isArray() && j < vouts.size() && !slot; ++j) {
                    const UniValue& spk{vouts[j].find_value("scriptPubKey").find_value("hex")};
                    if (spk.isStr()) slot = BmmRequestSlot(ParseHex(spk.get_str()), *prev);
                }
                const auto fee{amount(tx.find_value("fee"))};
                const UniValue& vsize{tx.find_value("vsize").isNum() ? tx.find_value("vsize") : tx.find_value("size")};
                if (!slot || !fee || !vsize.isNum() || vsize.getInt<int64_t>() <= 0) continue;
                seen->push_back({*slot, *fee * 1000 / vsize.getInt<int64_t>()});
            }
        }
    } catch (const std::exception&) {
    } catch (const UniValue&) {
    }
    // The sidechains active on the mainchain (their slots alone count).
    std::set<uint8_t> active;
    try {
        const UniValue list{client.Call("listactivesidechains", UniValue{UniValue::VARR})};
        for (size_t i{0}; list.isArray() && i < list.size(); ++i) {
            const UniValue& slot{list[i].find_value("slot")};
            if (slot.isNum() && slot.getInt<int>() >= 0 && slot.getInt<int>() <= 255) active.insert(static_cast<uint8_t>(slot.getInt<int>()));
        }
    } catch (const std::exception&) {
    } catch (const UniValue&) {
    }
    LOCK(m_anchor_mutex);
    m_bid_inputs.fee_rate_per_kvb = rate;
    if (seen) {
        m_bid_seen.push_back(std::move(*seen));
        while (m_bid_seen.size() > 12) m_bid_seen.pop_front();
    }
    const uint32_t own_slot{::Params().GetConsensus().sidechain.slot};
    m_bid_inputs.included_rate_per_kvb = RobustIncludedRate({m_bid_seen.begin(), m_bid_seen.end()}, active, own_slot <= 255 ? std::optional<uint8_t>{static_cast<uint8_t>(own_slot)} : std::nullopt);
    m_bid_inputs.relay_rate_per_kvb = relay_rate;
    m_bid_fee_source = source;
    m_bid_balance = balance;
    m_bid_balance_error = balance_error;
    // Low balance: fewer mainchain blocks covered than the policy wants, at the last bid.
    const CAmount last{m_bid.offer > 0 ? m_bid.offer : std::max<CAmount>(policy.floor, 1)};
    const bool low{balance && policy.low_balance_blocks > 0 && *balance / last < policy.low_balance_blocks};
    if (low && !m_bid_low_warned) {
        LogWarning("FastBlocks: the anchor bid wallet holds %s: %d mainchain blocks at the current bid of %s, fewer than %d (lowbalanceblocks): fund it, or the anchorer stops bidding",
                   FormatMoney(*balance), *balance / last, FormatMoney(last), policy.low_balance_blocks);
    }
    m_bid_low_warned = low;
}

FastBlocksHooks::AnchorBidReport FastBlocksHooks::GetAnchorBidReport(int64_t slot, int64_t first_unanchored_slot, const sidechain::Follower::Mining* mining) const
{
    LOCK(m_anchor_mutex);
    AnchorBidReport report;
    report.validator = m_validator.has_value();
    report.bidder = m_validator;
    report.defaults = m_bid_defaults;
    report.overrides = m_bid_overrides;
    report.history.assign(m_bid_history.rbegin(), m_bid_history.rend());
    report.missed = m_bid_missed;
    report.balance = m_bid_balance;
    report.balance_error = m_bid_balance_error;
    report.fee_source = m_bid_fee_source;
    report.inputs = m_bid_inputs;
    report.inputs.slot = slot;
    report.inputs.first_unanchored_slot = first_unanchored_slot;
    report.inputs.max_unanchored_slots = m_params.MaxUnanchoredSlots(slot);
    report.inputs.missed = m_bid_missed;
    const AnchorBidPolicy& policy{PolicyLocked()};
    report.policy = policy;
    const int64_t now{TicksSinceEpoch<std::chrono::seconds>(NodeClock::now())};
    report.spent = SpentInPeriod(policy, now);
    report.inputs.spent_in_period = report.spent;
    report.stale = StaleLocked(now);
    report.inputs.reserve = m_anchor_reserve;
    report.inputs.slot_seconds = m_params.slot_seconds;
    report.reserve = m_anchor_reserve;
    report.deficit = m_anchor_deficit;
    report.designated = m_turn_designated;
    report.turn_main_height = m_turn_main_height;
    if (const int64_t beat{HeartbeatSlots(policy, m_params.slot_seconds)}; beat > 0 && first_unanchored_slot >= 0) {
        report.next_heartbeat = SlotTime(first_unanchored_slot + beat, m_params);
    }
    // What anchoring collected and paid: the landed anchors of the last 24 hours and 7 days, by what they were bid for.
    while (!m_anchors_landed.empty() && m_anchors_landed.front().time <= now - ANCHORS_LANDED_SECONDS) m_anchors_landed.pop_front();
    for (const LandedAnchor& anchor : m_anchors_landed) {
        for (AnchoringTotals* totals : {&report.week, &report.day}) {
            if (totals == &report.day && anchor.time <= now - 24 * 3600) continue;
            totals->collected += anchor.pot;
            totals->paid += anchor.offer;
            ++totals->anchors[anchor.basis];
        }
    }
    report.anchor_trigger = m_validator && policy.Bids() && policy.anchor;
    report.mine_trigger = mining && mining->enabled && policy.Bids();
    report.decision = DecideNodeBid(policy, report.inputs, BidTriggers{.anchor = report.anchor_trigger, .mine = report.mine_trigger,
                                                                       .mine_always = report.mine_trigger && mining->always,
                                                                       .mine_amount = mining ? mining->amount : 0, .stale = report.stale});
    if (report.stale > 0 && report.decision.kind != "replace" && report.decision.bid && report.decision.amount < report.stale + REPLACEMENT_BUMP) {
        report.warnings.push_back(strprintf("A bid of %s that did not land may still be held by other nodes: a new bid on the same coin must pay more, and max or the cap keep this one below it.", FormatMoney(report.stale)));
    }
    if (!m_validator) {
        if (report.decision.kind == "periodcap") report.warnings.push_back("The spending cap of the period is reached: the bid is cut.");
        return report;
    }
    if (!policy.Bids()) {
        report.warnings.push_back("Bidding is off: this node bids for no anchors, and the chain relies on other bidders; blocks of stake halt after max_unanchored_slots without one.");
    } else if (!policy.anchor && !report.mine_trigger) {
        report.warnings.push_back("\"Anchor my validator\" is off: this validator bids for no anchors, and the chain relies on other bidders; blocks of stake halt after max_unanchored_slots without one.");
    } else if (!policy.prevent_stall && !policy.BidsEveryBlock()) {
        report.warnings.push_back("Stall prevention is off: without fees this node does not bid, and if no one else does, blocks of stake halt after max_unanchored_slots without an anchor.");
    }
    if (policy.warn_missed > 0 && m_bid_missed >= policy.warn_missed) {
        // (A bid of 10 x the relay minimum or more: the pool's side, not the bid, as the log says.)
        const CAmount relay_minimum{RelayMinimum(policy, report.inputs)};
        const auto last{std::find_if(m_bid_history.rbegin(), m_bid_history.rend(), [](const AnchorBidRecord& r) { return r.status == AnchorBidRecord::Status::MISSED; })};
        if (last != m_bid_history.rend() && relay_minimum > 0 && last->offer >= POOL_WARNING_RELAY_MULTIPLE * relay_minimum) {
            report.warnings.push_back(strprintf("%d mainchain blocks in a row came without this validator's commitment, though the last bid (%s) is %d times the relay minimum or more: "
                                                "the mainchain's pool may not be including BMM commitments (its enforcer, or the pool's side).",
                                                m_bid_missed, FormatMoney(last->offer), POOL_WARNING_RELAY_MULTIPLE));
        } else {
            report.warnings.push_back(strprintf("Your bid is too low: %d mainchain blocks in a row came without this validator's commitment.", m_bid_missed));
        }
    }
    const CAmount rate{report.decision.amount > 0 ? report.decision.amount : 0};
    if (report.balance && rate > 0 && policy.low_balance_blocks > 0 && *report.balance / rate < policy.low_balance_blocks) {
        report.warnings.push_back(strprintf("The bid wallet is low: %s covers %d mainchain blocks at the current bid.", FormatMoney(*report.balance), *report.balance / rate));
    }
    if (report.decision.kind == "periodcap") report.warnings.push_back("The spending cap of the period is reached: the bid is cut.");
    return report;
}

bool FastBlocksHooks::SetAnchorBidPolicy(const AnchorBidPolicy& policy, const UniValue& overrides, std::string& error) const
{
    error = CheckPolicy(policy);
    if (!error.empty()) return false;
    LOCK(m_anchor_mutex);
    // One policy for the node: a validator's anchorer and the merged miner of any node (setbmm).
    if (m_validator) m_validator->policy = policy;
    m_node_policy = policy;
    m_bid_overrides = overrides;
    m_bid_off_warned = false;
    m_bid_missed_warned = 0;
    LogInfo("FastBlocks: the node's bid policy, as setanchorbidpolicy says: %s", AnchorBidPolicySummary(policy));
    return true;
}

void FastBlocksHooks::StartServices(node::NodeContext& node) const
{
    m_node = &node;
    // The node's bid policy, for both triggers (a validator's anchorer, and the merged miner of any node): the options'
    // policy, with the fields setanchorbidpolicy saved in the settings.
    const AnchorBidPolicy defaults{node.args ? AnchorBidPolicyFromArgs(*node.args, m_chain_params.GetChainType()) : AnchorBidPolicy{}};
    AnchorBidPolicy policy{defaults};
    UniValue overrides{UniValue::VOBJ};
    if (node.args) {
        if (const common::SettingsValue saved{node.args->GetSetting(ANCHOR_BID_SETTING)}; saved.isObject()) {
            const bool legacy{IsLegacySettings(saved)};
            UniValue fields{saved};
            if (legacy) fields = LegacyBidSettings(saved, defaults, *node.args, m_chain_params.GetChainType());
            std::string error;
            AnchorBidPolicy changed{defaults};
            if (!ApplyPolicyFields(changed, fields, error, legacy ? StrategyContext::LEGACY_SETTINGS : StrategyContext::CURRENT) || !(error = CheckPolicy(changed)).empty()) {
                LogError("FastBlocks: the anchor bid policy in the settings (%s) is not used: %s", ANCHOR_BID_SETTING, error);
            } else {
                policy = changed;
                // 8.3.5's settings, in the words of 8.3.6 from now on (what setanchorbidpolicy saves next): the same policy
                // read back after a restart, and a floor set later the same before and after one.
                overrides = legacy ? NormalizedLegacySettings(fields, changed) : UniValue{saved};
                if (legacy) LogInfo("FastBlocks: the anchor bid policy in the settings is 8.3.5's (no preventstall): read as 8.3.5 meant it: %s", AnchorBidPolicySummary(changed));
            }
        }
    }
    {
        LOCK(m_anchor_mutex);
        m_bid_defaults = defaults;
        m_bid_overrides = overrides;
        m_node_policy = policy;
    }
    // The bids made and what landed cost, as before the restart (the period's cap, the stale rule).
    if (node.args) LoadBids(node.args->GetDataDirNet() / BID_FILE);
    std::string error;
    m_producer = Producer::FromArgs(node, *this, error);
    m_producer_error = error;
    if (!error.empty()) {
        LogError("FastBlocks: no block producer: %s", error);
        return;
    }
    if (m_producer) {
        m_producer->Start();
        // Its own anchorer: a ticket for every mainchain block, its bid as the node's policy says (pos/anchorbid.h), the
        // pot to the producer's payout.
        ValidatorBid own{.payee = m_producer->Payout(), .policy = policy, .keys = m_producer->Keys()};
        LogInfo("FastBlocks: the validator's anchorer: %s%s", AnchorBidPolicySummary(policy), overrides.getKeys().empty() ? "" : " (with the settings of setanchorbidpolicy)");
        if (!policy.Bids() || !policy.anchor) LogWarning("FastBlocks: this validator's anchorer bids nothing (%s): the chain relies on other bidders for its anchors", policy.Bids() ? "\"Anchor my validator\" off" : "strategy off");
        if (policy.Bids() && policy.floor == 0) LogInfo("FastBlocks: a bid of 0 pays the mainchain no fee: only a miner that takes such requests includes it (Backbone -bmmzerofee, or -minrelaytxfee=0)");
        LOCK(m_anchor_mutex);
        m_validator = own;
    }
    // The acknowledgements of the recent blocks of the active chain, so that a commitment the mainchain
    // drops takes the blocks that rest on it with it.
    if (ChainstateManager* chainman{Chainman()}) {
        std::vector<const CBlockIndex*> recent;
        {
            LOCK(::cs_main);
            const CBlockIndex* index{chainman->ActiveChain().Tip()};
            for (int i{0}; index && i < 2 * static_cast<int>(m_params.MaxUnanchoredSlotsEver()) + 1000; ++i, index = index->pprev) {
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
        m_validator.reset();
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
