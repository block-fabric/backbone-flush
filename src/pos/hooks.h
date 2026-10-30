// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_HOOKS_H
#define BITCOIN_POS_HOOKS_H

#include <pos/anchorbid.h>
#include <pos/evidence.h>
#include <pos/feesplit.h>
#include <pos/finality.h>
#include <pos/header.h>
#include <pos/schedule.h>
#include <pos/staking.h>
#include <sidechain/follower.h>
#include <sidechain/hooks.h>
#include <consensus/amount.h>
#include <kernel/cs_main.h>
#include <sidechain/store.h>
#include <sync.h>
#include <uint256.h>
#include <univalue.h>
#include <util/chaintype.h>
#include <util/fs.h>

#include <array>
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class CBlockIndex;
class CChainParams;
class ChainstateManager;

class ArgsManager;

namespace pos {

class Producer;

//! The P2P message that relays anchor tickets.
inline constexpr const char* MSG_TICKET{"fbticket"};
//! The P2P message that asks peers for the tickets of commitments: a list of h*.
inline constexpr const char* MSG_GETTICKETS{"fbgettickets"};
//! Tickets asked for in one message, at most; and the newest mainchain blocks a node asks for the tickets of.
inline constexpr size_t MAX_TICKETS_ASKED{16};
//! Tickets a node keeps, at most: the oldest go first.
inline constexpr size_t MAX_TICKETS{1000};
//! Acknowledging blocks whose commitment a node keeps in memory, at most (others are read from disk).
inline constexpr size_t MAX_NOTED_ACKS{20000};
//! Tickets the mainchain has not committed to that a node takes from one peer per mainchain block, at
//! most: counted by the peer's network group (a peer that reconnects, or comes from many addresses of
//! one network, is one) ...
inline constexpr size_t MAX_PEER_TICKETS{8};
//! ... from all the peers whose addresses are not routable together (local, as Tor's inbound
//! connections are, or private: one group, as Bitcoin Core's for onion and local addresses; a peer
//! that reconnects is no new budget). Sized for honest relay: a bidder makes one ticket a mainchain
//! block, and a ticket the node has is not counted ...
inline constexpr size_t MAX_NONROUTABLE_TICKETS{32};
//! ... from all the peers the node cannot find (gone meanwhile: anyone; MAX_PEER_TICKETS each) together ...
inline constexpr size_t MAX_UNKNOWN_PEER_TICKETS{32};
//! ... and from all peers together, first come (each one taken goes whole to every peer) ...
inline constexpr size_t MAX_UNCOMMITTED_TICKETS{64};
//! ... but past that, the first ticket of a group that has none yet (its fair share: an honest bidder's
//! relayed ticket is not crowded out by a few groups that came first), up to this many in all.
inline constexpr size_t MAX_UNCOMMITTED_TICKETS_SHARED{2 * MAX_UNCOMMITTED_TICKETS};
/**
 * How far ahead of the node's clock a block may be, in seconds, while blocks of stake are on (Bitcoin's
 * two hours otherwise). BMM blocks with times far ahead would push the median time past ahead, and
 * blocks of stake, whose time is that of their slot, must come after it: a bidder who wins a few BMM
 * auctions in a row could stop them for two hours.
 */
inline constexpr int64_t MAX_FUTURE_BLOCK_TIME_POS{10 * 60};

/**
 * What the state of the chain says about anchors, the fee pot and PoS blocks, after a block: one
 * entry of the sidechain store (table byte 0x55), so that the store's journal is its undo.
 */
struct FastState {
    //! Anchors on the chain: acknowledged tickets and BMM blocks.
    uint64_t anchors{0};
    //! The last anchor: the mainchain height of its commitment, and the block it anchors (P).
    int32_t last_main_height{-1};
    uint256 last_anchored;
    int32_t last_anchored_height{-1};
    //! The last block that took the peg actions (the acknowledgement, or the BMM block): the next
    //! ticket must anchor it or a block after it.
    uint256 anchor_point;
    int32_t anchor_point_height{-1};
    //! Coins put in the fee pot by PoS blocks since the chain began, and paid out of it.
    int64_t pot_accrued{0};
    int64_t pot_paid{0};
    //! The slot of the last PoS block, its time, and the slot of the first PoS block no anchor covers.
    int64_t last_pos_slot{-1};
    int64_t first_unanchored_slot{-1};

    SERIALIZE_METHODS(FastState, obj)
    {
        READWRITE(obj.anchors, obj.last_main_height, obj.last_anchored, obj.last_anchored_height, obj.anchor_point, obj.anchor_point_height,
                  obj.pot_accrued, obj.pot_paid, obj.last_pos_slot, obj.first_unanchored_slot);
    }
    friend bool operator==(const FastState&, const FastState&) = default;
};

/** The FastBlocks part of a chain's state. */
FastState GetFastState(const sidechain::StoreView& view);
/**
 * Peg work waiting for an anchor, independent of fees and stall pressure: a deposit to credit (in the mainchain record
 * above the chain's), withdrawals or a bundle. Not the staking accounting (the pot's reserve, the fee pool, rewards owed,
 * unbonds), which advances only as anchors land.
 */
bool HasPendingAnchorActions(const sidechain::State& state, const sidechain::Mainchain& record);
/**
 * What makes a bid without fees (doc/fastblocks-anchor-bids.md, "Prevent stalling"): peg work (HasPendingAnchorActions),
 * or a bond, an unbond or a jail no anchor covers yet (Staking::SetChanged for `epoch`: once per change, until an anchor
 * makes the new set). Not the unbond release countdown, rewards owed, the pot's reserve or the fee pool.
 */
bool NeedsAnchor(const sidechain::State& state, const sidechain::Mainchain& record, const Staking& staking, int64_t epoch);
/** The pot accrued as of the block at `height` on the chain of `view`, at least what was paid. */
int64_t PotAccruedAt(const sidechain::StoreView& view, int32_t height);
/**
 * The validator pool (fee split, pos/feesplit.h): the validators' share of fees not paid yet, by the
 * epoch whose validators it is to be shared among (table 0x5f), and their sum.
 */
std::vector<std::pair<int64_t, int64_t>> PoolEntries(const sidechain::StoreView& view);
int64_t PoolTotal(const sidechain::StoreView& view);

/**
 * FastBlocks on the template's hooks (sidechain/hooks.h): PoS blocks signed by the leader of their
 * slot, anchors, the order of chains, the fee pot, the producer and the anchorer.
 * See doc/fastblocks.md.
 */
class FastBlocksHooks : public sidechain::ChainHooks
{
public:
    explicit FastBlocksHooks(const CChainParams& params);
    ~FastBlocksHooks() override;

    /** The chainstate manager the hooks read the block index and the mainchain record from (none to forget it). */
    void Attach(ChainstateManager* chainman) { m_chainman.store(chainman); }
    ChainstateManager* Chainman() const { return m_chainman.load(); }
    /** The validators the chain starts with (PosParams::genesis_validators): the set of its first epochs. */
    const ValidatorSet& GenesisSet() const { return m_genesis_set; }
    const Staking& GetStaking() const { return m_staking; }
    const Consensus::PosParams& Params() const { return m_params; }
    /** The hash of the chain's genesis block, which the leaders' signatures commit to. */
    const uint256& Genesis() const { return m_genesis; }
    /** Forget the validator sets read before (as a node that restarts): tests. */
    void ForgetKnownSets() const EXCLUSIVE_LOCKS_REQUIRED(!m_sets_mutex);

    /** What the chain says of the leader of a slot. */
    enum class LeaderStatus { OK, INACTIVE, SET_UNKNOWN, SEED_UNKNOWN };
    struct SlotLeader {
        LeaderStatus status{LeaderStatus::SET_UNKNOWN};
        //! The leader's key (OK only).
        uint256 pubkey;
        //! The validator set of the slot's epoch, and the anchors of the chain at the anchor that made it.
        std::shared_ptr<const ValidatorSet> set;
        uint64_t set_anchors{0};
        uint256 seed;
        //! The anchor that made the set (none: the genesis set, or none found).
        const CBlockIndex* set_anchor{nullptr};
    };
    /**
     * The slot of an anchor, for the epochs whose set it makes: of its time, or of the median time of
     * the mainchain block that committed to it (from the record) if that is later. A block's time can
     * be set back as far as the median time of the chain before it, which a merged miner that makes
     * every block after a stall controls: as the slot of an anchor, it would let one fix the set of an
     * epoch whose seed is known already (stake grinding). Called with cs_main held.
     */
    int64_t AnchorSlot(const CBlockIndex& anchor) const;
    /**
     * The validator set of `epoch` for a block on top of `prev`: the one the newest anchor at or below
     * `prev` whose slot (AnchorSlot) is before pos::SetBoundary(`epoch`) made (none: the genesis set). nullptr if this
     * node does not know it: the anchor is on a branch whose blocks it has not connected. `view`: the
     * state of the chain of `prev`, to read the set from (that state alone); none: the sets this node
     * read before, or else the active chain's state (for headers). `anchor`: set to the anchor that made
     * it (left alone for the genesis set). `within`: if given, nullptr too once the walk down the chain
     * passed more anchors than that (for a proof: a set out of its window, EvidenceWithin), so that
     * the walk for a slot anyone names (a proof's) costs no more than the window, not the chain.
     * Called with cs_main held.
     */
    std::shared_ptr<const ValidatorSet> SetFor(const CBlockIndex& prev, int64_t epoch, const sidechain::StoreView* view = nullptr, uint64_t* set_anchors = nullptr,
                                               const CBlockIndex** anchor = nullptr, std::optional<uint64_t> within = std::nullopt) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_sets_mutex);
    /** The leader of `slot` for a block on top of `prev`: its epoch's set, and seed (from the record). `within`: as SetFor's. Called with cs_main held. */
    SlotLeader LeaderOf(const CBlockIndex& prev, int64_t slot, const sidechain::StoreView* view = nullptr, std::optional<uint64_t> within = std::nullopt) const
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_sets_mutex);
    /**
     * Anchors the walk for the set of a proof's slot passes at most (SetFor's `within`): past them, the
     * set is out of the window for proofs (more than evidence_window_anchors anchors before the chain's:
     * bad-pos-evidence-old, as when it is found), or a block's parent is far below its slot.
     */
    uint64_t EvidenceWithin() const { return static_cast<uint64_t>(m_params.evidence_window_anchors) + 1; }
    /** The leader of `slot` on the active chain's tip. */
    SlotLeader ActiveLeaderOf(int64_t slot) const EXCLUSIVE_LOCKS_REQUIRED(!m_sets_mutex);
    /** Whether blocks of stake can be made now: the active chain has an active set for the current epoch. */
    bool PosActiveNow() const EXCLUSIVE_LOCKS_REQUIRED(!m_sets_mutex);
    /**
     * Check a proof that a block on top of `prev`, of `slot`, carries: the leader of the proof's slot
     * on this chain signed both headers, within the window, and was not punished for it before.
     * Reasons: those of CheckEvidence; bad-pos-evidence-late (a slot after the block's),
     * bad-pos-evidence-old (more than evidence_window_anchors anchors since the set of its epoch was
     * made, or a set the state no longer has), bad-pos-evidence-known, pos-seed-unknown. Called with
     * cs_main held. `anchors`: those of the chain with the block. The signatures are checked last
     * (the window first: both read the record; whether the offence was punished, or its validator
     * banned, only once the signatures say whose it is: those read the state alone, RecordIndependent),
     * and once per proof and leader (EvidencePool::CheckSigs). `sig_budget`: if given, how many
     * proofs whose signatures were not checked before may be checked; none left, the proof is not
     * taken (pos-evidence-later), and the count goes down by those checked.
     */
    std::optional<Offence> CheckChainEvidence(const DoubleProposal& proof, const CBlockIndex& prev, int64_t slot, const sidechain::StoreView& view, uint64_t anchors,
                                              std::string& reason, size_t* sig_budget = nullptr) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_sets_mutex);
    /**
     * The proofs of the pool a block on top of `prev`, of `slot`, can carry (CheckChainEvidence), one per
     * validator, at most `max`, those of `skip` left out. Under cs_main, with a pool of MAX_EVIDENCE
     * proofs, so the cheap checks come first, by the offence the pool keeps a proof under (its
     * signatures were checked: wherever the proof is one, it is that offence): punished already, its
     * validator banned, a slot after the block's. Then, by value (the oldest slot first: the nearest
     * the end of its window), those whose signatures cost nothing (a result kept: the pool keeps the
     * good result of each of its proofs while it keeps the proof), and the set of whose slot this chain
     * has. Then the signatures of at most MAX_EVIDENCE_SIG_CHECKS proofs not checked before, round
     * robin (from where the last block left off, in the pool's order): each one is checked within
     * EVIDENCE_CHECK_ROUNDS blocks, however many others a flood keeps unchecked (audit 6 review,
     * item 11).
     */
    std::vector<std::pair<Offence, DoubleProposal>> ChainEvidence(const CBlockIndex& prev, int64_t slot, const sidechain::StoreView& view, uint64_t anchors, size_t max,
                                                                  const std::set<Offence>& skip = {}) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_sets_mutex);

    bool BlockExtensions() const override { return true; }
    sidechain::BlockKind Kind(const CBlockHeader& header) const override;
    bool CheckHeader(const CBlockHeader& header, const CBlockIndex& prev, const ChainstateManager& chainman, BlockValidationState& state) const override EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_sets_mutex, !m_final_mutex);
    /**
     * A header of a block of stake whose leader the node cannot tell yet (LeaderOf: SET_UNKNOWN, the set
     * of its epoch made by an anchor the node has not connected) is taken unchecked, within the
     * template's budget (UncheckedLimits: unchecked_headers_per_peer, unchecked_block_bytes_per_peer). See
     * doc/fastblocks.md, "Headers and blocks the node cannot check yet". Called with cs_main held.
     */
    bool HeaderCheckable(const CBlockHeader& header, const CBlockIndex& prev) const override EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_sets_mutex);
    UncheckedBudget UncheckedLimits() const override;
    /**
     * At most MAX_SLOT_HEADERS headers of blocks of stake of a slot signed by one leader are in the block
     * index for good: the first, and a second, the proof that it signed twice. A further one of that
     * signer, from a peer, is spare: it signs as many as it likes, at the cost of a signature each, so it
     * is taken only on the branch of a header on top of it, or as a tip the node may need (template
     * SpareWanted), and then unchecked (template: "Spare headers"). Another leader's header of the slot
     * (on a branch whose set, or a record whose seed, names another) is not spare. A header found spare
     * has no signature checked (CheckHeader, next; m_spare_header), and the proof against its signer is
     * the two kept (put in the pool if it has none). A header not after the median time of the chain
     * before it is not (time-too-old next): no leader is looked for. See doc/fastblocks.md, "Headers of
     * a slot". Called with cs_main held.
     */
    bool HeaderSpare(const CBlockHeader& header, const CBlockIndex& prev) const override EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    void HeaderKept(const CBlockIndex& index) const override EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    static constexpr size_t MAX_SLOT_HEADERS{2};
    bool CheckBlock(const CBlock& block, BlockValidationState& state) const override EXCLUSIVE_LOCKS_REQUIRED(!m_sets_mutex, !m_acks_mutex);
    bool IsWaitReason(const std::string& reject_reason) const override;
    //! A node a little behind on the record after a mainchain reorg must not cut itself off from the peers that took an anchor back first.
    bool PardonRecordFailures() const override { return true; }
    std::optional<int64_t> WaitingReady(const CBlockHeader& header, const sidechain::Mainchain& record) const override EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    uint256 SectionsHash(std::span<const unsigned char> sections) const override;
    /**
     * Another signature of the slot's leader over a block of stake the node has the header of is the
     * same block: EQUIVALENT if both verify (the node keeps its own), REPLACE if this one verifies and
     * the node's (taken unchecked) does not, UNKNOWN if the node cannot tell the leader yet; INVALID
     * otherwise.
     */
    sidechain::OtherExtension CheckOtherExtension(const CBlockHeader& header, const CBlockIndex& index) const override EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_sets_mutex);
    //! A block of stake whose signature fails: its header, taken unchecked, may have come with a forged one.
    bool ExtensionFailure(const std::string& reject_reason) const override { return reject_reason == "bad-pos-sig"; }
    /**
     * 10 minutes while the network has blocks of stake (genesis validators, or the set the newest
     * anchor of the active chain made is active), Bitcoin's two hours otherwise: from the state of the
     * tip, so that a node that restarts, or reorgs, agrees with the others.
     */
    int64_t MaxFutureBlockTime() const override EXCLUSIVE_LOCKS_REQUIRED(!m_sets_mutex, !m_staking_mutex);
    /** FastBlocks' reasons a block fails whatever the record of the mainchain says. */
    bool RecordIndependent(const std::string& reject_reason) const override;
    /**
     * The height of the final block of the active chain (final_anchors anchors cover it, net of the
     * commitments it skipped: see Finality); -1 if none. Cached by tip and record. Called with cs_main held.
     */
    int FinalHeight() const EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_final_mutex);

    bool CustomChainOrder() const override { return true; }
    arith_uint256 ChainWork(const CBlockIndex* prev, const CBlockHeader& header) const override;

    std::optional<uint256> CommitmentOf(const CBlockIndex& index) const override EXCLUSIVE_LOCKS_REQUIRED(!m_acks_mutex);
    std::vector<uint256> BlocksCommittedBy(const uint256& commitment, std::optional<int> main_height) const override EXCLUSIVE_LOCKS_REQUIRED(!m_acks_mutex);
    /**
     * For a BMM block, the height on record of its own commitment. For a block of stake, the height on record of
     * the commitment of the first anchor that covers it (anchors it, or a block above it): until then, stake alone
     * could build a branch without it. Nothing if that commitment is not on record at the height the anchor
     * names. Defined in finality.cpp.
     */
    std::optional<int> FixedFrom(const CBlockIndex& block, const CChain& chain, const sidechain::Mainchain& record) const override EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_acks_mutex);

    bool CheckExtension(const sidechain::State& state, const CBlock& block, std::string& reject_reason) const override;
    bool GetPegStep(const CBlock& block, const sidechain::Mainchain& mainchain, std::optional<sidechain::PegStep>& step, std::string& reject_reason) const override;
    bool RefundsOnlyInPegSteps() const override { return true; }
    bool BeforePegStep(sidechain::State& state, const CBlock& block, int height, const sidechain::PegStep& step, const Consensus::SidechainParams& params,
                       std::vector<CTxOut>& owed_tx, std::string& reject_reason) const override EXCLUSIVE_LOCKS_REQUIRED(!m_sets_mutex);
    bool ApplyTx(sidechain::State& state, const CTransaction& tx, int height, bool peg_step, std::vector<CTxOut>& payouts, std::string& reject_reason) const override;
    //! Bonds and unbonds.
    bool HasTxRules(const CTransaction& tx) const override;
    //! The records of the validators they name (ValidatorRuleKeys).
    std::optional<std::vector<std::vector<unsigned char>>> TxRuleKeys(const CTransaction& tx) const override;
    bool AfterBlock(sidechain::State& state, const CBlock& block, int height, const std::optional<sidechain::PegStep>& step, CAmount minted,
                    const Consensus::SidechainParams& params, std::string& reject_reason) const override EXCLUSIVE_LOCKS_REQUIRED(!m_sets_mutex, !m_acks_mutex);

    /**
     * The fee split (from fee_split_height): the validators' share of the ordinary fees in the pool
     * output, and for a block of stake the bidder's share and the fees of peg transactions in the pot
     * (bad-pos-fee-split; S from the record: MedianSpacingAt at FeeSplitMainHeight).
     */
    bool CheckFees(const sidechain::State& state, const CBlock& block, int height, const std::optional<sidechain::PegStep>& step, const std::vector<CAmount>& fees,
                   std::string& reject_reason) const override;
    /** The validators' share of a merged mined block's ordinary fees, to the pool output. */
    CAmount MergeMinedFeeOutputs(const CBlock& block, int height, int main_height, const std::vector<CAmount>& fees, std::vector<CTxOut>& outputs, CAmount extra = 0) const override;
    /** Whether the block at `height` takes the fee split (PosParams::fee_split_height). */
    bool FeeSplitActive(int height) const { return height >= m_params.fee_split_height; }
    /** The curve of the bidder's share the block at `height` takes (the second from PosParams::fee_curve2_height). */
    FeeCurve FeeCurveAt(int height) const { return height >= m_params.fee_curve2_height ? FeeCurve::LINEAR : FeeCurve::FIRST; }
    /** Whether the block at `height` takes the liveness fix (PosParams::jail_fix_height). */
    bool JailFixActive(int height) const { return height >= m_params.jail_fix_height; }
    /**
     * The mainchain height whose S a block's fee split takes: for a block of stake of `slot`, the block
     * that seeds its epoch (SeedHeight; none if not on record yet), for a merged mined block the
     * block before its commitment (`bmm_main_height`, PegStep::main_height).
     */
    std::optional<int> FeeSplitMainHeight(bool pos_block, int64_t slot, int bmm_main_height) const;
    /** S at a mainchain height, from the record (MedianSpacingAt from main_activation_height), in units of PosParams::fee_target_spacing (60 s × S / target). */
    int64_t FeeSplitSpacing(int main_height) const;

    /** The proofs this node knows that the chain has not punished, in a BMM block's coinbase (MAX_MINED_EVIDENCE, one per validator). */
    std::vector<CTxOut> MergeMinedOutputs(const sidechain::StoreView& view, const CBlockIndex& prev, int64_t time) const override EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_sets_mutex);
    bool FollowerRound(sidechain::Follower& follower) const override EXCLUSIVE_LOCKS_REQUIRED(!m_tickets_mutex, !m_anchor_mutex, !m_sets_mutex);
    void StartServices(node::NodeContext& node) const override EXCLUSIVE_LOCKS_REQUIRED(!m_anchor_mutex, !m_acks_mutex);
    void StopServices(node::NodeContext& node) const override EXCLUSIVE_LOCKS_REQUIRED(!m_anchor_mutex);
    bool ProcessMessage(int64_t peer, const std::string& msg_type, DataStream& recv, std::string& misbehaving) const override EXCLUSIVE_LOCKS_REQUIRED(!m_tickets_mutex, !m_sets_mutex);

    //
    // For the producer, the anchorer and RPC.
    //

    /**
     * The header rule of acknowledgements (bad-pos-ack-order): a header that acknowledges a commitment
     * (nNonce) names one after the last anchor of the chain below it, as its chain key says.
     */
    static bool AckInOrder(const CBlockIndex& prev, const CBlockHeader& header);
    /** The slot of the last PoS block at or below `index`, if any. */
    static std::optional<int64_t> LastPosSlot(const CBlockIndex* index, const Consensus::PosParams& params);
    /** Note a ticket (`own`: this node's merged miner made it). @return whether it was new */
    bool AddTicket(const AnchorTicket& ticket, bool own = false) const EXCLUSIVE_LOCKS_REQUIRED(!m_tickets_mutex);
    std::optional<AnchorTicket> GetTicket(const uint256& hash) const EXCLUSIVE_LOCKS_REQUIRED(!m_tickets_mutex);
    /**
     * Whether a ticket the mainchain has not committed to may be taken from `peer` (and count it), per
     * mainchain block: MAX_PEER_TICKETS per network group of peers, MAX_NONROUTABLE_TICKETS from all
     * peers whose addresses are not routable, MAX_UNKNOWN_PEER_TICKETS from peers it cannot find, and
     * MAX_UNCOMMITTED_TICKETS in all; past that, the first of a group (each of those three a group)
     * that has none yet, up to MAX_UNCOMMITTED_TICKETS_SHARED.
     */
    bool TicketAllowed(int64_t peer) const EXCLUSIVE_LOCKS_REQUIRED(!m_tickets_mutex);
    std::vector<AnchorTicket> Tickets() const EXCLUSIVE_LOCKS_REQUIRED(!m_tickets_mutex);
    /** Relay a ticket to the peers (but `except`). */
    void RelayTicket(const AnchorTicket& ticket, std::optional<int64_t> except = std::nullopt) const;
    /**
     * Ask the peers for the tickets of the commitments on record after the last acknowledged one
     * that this node has no ticket nor block of (the newest anchors, for a node that just started or
     * synced): once per mainchain block. @return the commitments asked for
     */
    std::vector<uint256> AskMissingTickets() const EXCLUSIVE_LOCKS_REQUIRED(!m_tickets_mutex, !m_anchor_mutex);

    /** The proofs of leaders that signed twice this node knows, and the headers it saw signed. */
    EvidencePool& Evidence() const { return m_evidence; }
    /**
     * Check a proof that a leader signed two blocks for one slot, keep it and relay it (but to `except`).
     * Not taken: one of a slot after now, the headers' margin and a slot (pos-evidence-future), of a leader
     * the active chain banned (pos-evidence-banned); one of an offender the pool keeps a proof of is a proof
     * (the offence returned), not kept (one per offender).
     * @return the offence; nothing, with `reason` set, if the proof is not one
     */
    std::optional<Offence> AddEvidence(const DoubleProposal& proof, std::string& reason, std::optional<int64_t> except = std::nullopt) const EXCLUSIVE_LOCKS_REQUIRED(!m_sets_mutex);
    /** Whether the active chain banned a validator (tombstoned: punished once, its further proofs of no use). */
    static bool Banned(const ChainstateManager& chainman, const uint256& pubkey) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    /** Relay the proofs found since the last call (by watching headers). */
    void RelayNewEvidence() const;
    /**
     * The acknowledgement a PoS block of `slot` on top of `tip` can carry: of the anchor committed first
     * among those it may acknowledge (Acknowledgeable), if any, but the tickets in `skip` (by hash).
     * Called with cs_main held.
     */
    std::optional<AnchorAck> NextAck(const CBlockIndex& tip, const FastState& fast, int64_t slot, const std::set<uint256>* skip = nullptr) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_tickets_mutex, !m_acks_mutex);
    /**
     * Whether a PoS block of `slot` on top of `tip` may acknowledge a ticket, as far as the chain tells
     * (not the record): a ticket of version 1 with a payee, for a block on the chain at or after the
     * block the last anchor anchored, and not so old that the blocks of stake above it would be
     * uncovered for max_unanchored_slots. Called with cs_main held.
     */
    bool Acknowledgeable(const AnchorTicket& ticket, const CBlockIndex& tip, const FastState& fast, int64_t slot) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    /**
     * The block a ticket made at `slot` would anchor: the block before the tip (ticket depth 1), or the
     * tip if that one is not after the last block anchored (`after_anchor_point`: the last anchor
     * point, so that its anchor may build a bundle), or if a ticket for it could not be acknowledged
     * in the next slot (blocks of stake stopped for want of an anchor: nothing is built on the tip
     * until one comes). Called with cs_main held.
     */
    const CBlockIndex* TicketTarget(const CBlockIndex& tip, const FastState& fast, int64_t slot, bool after_anchor_point = true) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    /**
     * Where a block on `prev` that acknowledges `ack`, which anchors `anchored`, takes the peg actions:
     * as if it followed the mainchain up to the block before the commitment, with a bundle of the
     * withdrawals at or below the block anchored, its hash as the nonce, its height as the lock time
     * (design 4.3.4) -- and no bundle at all if the block anchored is below the block that took the
     * last peg actions (a ticket made before the last commitment was acknowledged): a bundle depends
     * on the block anchored alone only if no peg actions came between it and the acknowledgement.
     * Called with cs_main held.
     */
    sidechain::PegStep AckPegStep(const AnchorAck& ack, const CBlockIndex& anchored, const CBlockIndex& prev) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    /** A validator's own anchorer: where the pot it wins goes, and how it bids (pos/anchorbid.h). */
    struct ValidatorBid {
        //! The producer's payout: the pot each anchor pays.
        CScript payee;
        //! The bid policy in force: the options' (-validatoranchorbid*), with what setanchorbidpolicy changed.
        AnchorBidPolicy policy;
        //! The producer's validator keys: the node takes its turn when any of them is designated (DesignatedBidder).
        std::vector<uint256> keys;
    };
    /** A validator's own anchorer: none if this node is not a validator (its policy may be off). */
    std::optional<ValidatorBid> ValidatorAnchoring() const EXCLUSIVE_LOCKS_REQUIRED(!m_anchor_mutex);
    /** A bid of the anchorer, and what became of it. */
    struct AnchorBidRecord {
        //! (As PastBid::Status: NO_BMM, the block carried no BMM commitment of any sidechain; TOO_SOON, it was found less
        //! than MIN_BID_LEAD_SECONDS after the bid. Neither is a miss of ours.)
        enum class Status { PENDING, LANDED, MISSED, OUTBID, NO_BMM, TOO_SOON };
        //! The mainchain tip the request followed: it is for the block after it.
        int main_height{0};
        uint256 main_tip;
        uint256 ticket;
        CAmount offer{0};
        std::string kind;
        std::string reason;
        double pressure{0};
        Status status{Status::PENDING};
        int64_t time{0};
        //! What set it off: "anchor" (the validator's anchorer), "mine" (setbmm), "anchor+mine", or "hand" (requestbmmblock).
        std::string trigger;
        //! A bid for work waiting without fees (BidDecision::work): the throttle counts these.
        bool work{false};
        //! The pot the anchor pays its payee (the fees behind the bid), and what the bid kept back of the fee share for
        //! the anchor reserve (BidDecision::holdback), credited when it lands.
        CAmount pot{0};
        CAmount holdback{0};
        //! What it was for before the limits named it (BidDecision::basis; empty in the files of 8.3.6: then `kind`).
        std::string basis;
        //! No fees behind it (BidDecision::unfunded): debited from the anchor reserve when it lands.
        bool unfunded{false};
    };
    /** What the landed anchors of a window collected and paid. */
    struct AnchoringTotals {
        //! The pots of the landed anchors, and their offers.
        CAmount collected{0};
        CAmount paid{0};
        //! Landed anchors, by what they were bid for (AnchorBidRecord::basis).
        std::map<std::string, int> anchors;
        //! Bids that did not land: real misses, blocks without any BMM commitment, blocks found too soon after the bid.
        int missed{0};
        int no_bmm{0};
        int too_soon{0};
    };
    /** What the anchorer bids now, and why; what it bid; what its wallet covers. */
    struct AnchorBidReport {
        bool validator{false};
        std::optional<ValidatorBid> bidder;
        //! The node's bid policy in force, for both triggers (the validator's, or the merged miner's of any node).
        AnchorBidPolicy policy;
        //! The triggers on (the merged miner's as the caller says it, `mining`).
        bool anchor_trigger{false};
        bool mine_trigger{false};
        //! The stale bid a new one must pay more than (pos::StaleOffer), 0 if none.
        CAmount stale{0};
        //! The policy of the options alone, and the fields setanchorbidpolicy changed (the node's settings).
        AnchorBidPolicy defaults;
        UniValue overrides{UniValue::VOBJ};
        BidInputs inputs;
        BidDecision decision;
        //! Newest first.
        std::vector<AnchorBidRecord> history;
        int missed{0};
        CAmount spent{0};
        std::optional<CAmount> balance;
        std::string balance_error;
        std::string fee_source;
        std::vector<std::string> warnings;
        //! The anchor reserve (AnchorBidPolicy::reserve_pct), and what landed bids without fees spent past it since it
        //! was last full.
        CAmount reserve{0};
        CAmount deficit{0};
        //! Taking turns: whether this node is designated for the next mainchain block, `turn_main_height`.
        bool designated{false};
        int turn_main_height{0};
        //! The time from which the heartbeat bids, if it will (an unanchored run, a heartbeat).
        std::optional<int64_t> next_heartbeat;
        //! The last 24 hours, and the last 7 days.
        AnchoringTotals day;
        AnchoringTotals week;
    };
    /**
     * The report as of `slot`, the run without an anchor starting at `first_unanchored_slot` (-1: none); `mining`, the
     * merged miner's settings (setbmm), if the caller knows them: its trigger, in the decision shown.
     */
    AnchorBidReport GetAnchorBidReport(int64_t slot, int64_t first_unanchored_slot, const sidechain::Follower::Mining* mining = nullptr) const EXCLUSIVE_LOCKS_REQUIRED(!m_anchor_mutex);
    /** The node's bid policy in force (both triggers, and the most a bid by hand may offer). */
    AnchorBidPolicy BidPolicy() const EXCLUSIVE_LOCKS_REQUIRED(!m_anchor_mutex);
    /**
     * Note a bid this node made otherwise (requestbmmblock, by hand), for the mainchain block after `main_tip` at
     * `main_height`, to `commitment`: settled and counted in the period's spending as the others.
     */
    void NoteBid(int main_height, const uint256& main_tip, const uint256& commitment, CAmount offer, const std::string& trigger) const EXCLUSIVE_LOCKS_REQUIRED(!m_anchor_mutex);
    /** Put `policy` in force (checked first: CheckPolicy), `overrides` being the fields set on the options' policy. */
    bool SetAnchorBidPolicy(const AnchorBidPolicy& policy, const UniValue& overrides, std::string& error) const EXCLUSIVE_LOCKS_REQUIRED(!m_anchor_mutex);
    /**
     * Whether a BMM block built now would be in stall mode (and may take transactions): no active
     * validator set (`pos_active` false), or no block of stake for stall_seconds by `main_time`.
     */
    bool Stalled(const FastState& fast, int64_t main_time, bool pos_active) const;
    const Producer* GetProducer() const { return m_producer.get(); }
    Producer* GetProducer() { return m_producer.get(); }
    /**
     * The block an anchor on the chain anchors (P): for an acknowledgement, the block its ticket
     * names; for a BMM block, its parent. Nothing for a block that is no anchor. Called with cs_main held.
     */
    const CBlockIndex* AnchoredBy(const CBlockIndex& index) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_acks_mutex);
    /**
     * The margins (finality.h, NetAnchoredHeights) of the forks below the oldest of `anchors`
     * (the newest anchors of the chain that ends at `tip`, newest first): down to genesis, every
     * commitment on record since the chain's genesis could be known that the chain skipped counts
     * against every such fork (audit 7, finding 2: forks below the window were not looked at, and a
     * bidder that won more commitments in a row than the window holds took "final" blocks back).
     * Nothing if there are no anchors. Incremental: what is below the window is kept between calls.
     * Defined in finality.cpp. Called with cs_main held.
     */
    std::optional<DeepForks> DeepForkMargin(const CBlockIndex& tip, const std::vector<ChainAnchor>& anchors) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !m_deep_mutex, !m_acks_mutex);
    /** Forget what DeepForkMargin keeps (tests: the same as computed at once). */
    void ForgetDeepFinality() const EXCLUSIVE_LOCKS_REQUIRED(!m_deep_mutex)
    {
        LOCK(m_deep_mutex);
        m_deep = DeepFinality{};
    }
    /**
     * Blocks of stake on the active chain whose leader the record no longer agrees with (a mainchain
     * reorg moved the seed of their epoch): taken back, as blocks that lost their commitment are; and
     * those taken back so whose leader it agrees with again, given back; and the blocks of stake from the
     * start of the oldest epoch whose seed, as the chain's state says its blocks read it, the record
     * moved, connected again. Newest first, down to the first epoch whose seed has not moved (doc/fastblocks.md,
     * "Seed"). Once per mainchain tip, in the follower's round.
     */
    void CheckSeeds() const EXCLUSIVE_LOCKS_REQUIRED(!m_sets_mutex);
    /** Block index entries the walks for validator sets (SetFor) passed, since the hooks were made (the work done: tests). */
    uint64_t IndexWalks() const { return m_index_walks.load(); }
    /** Transactions the staking rules were applied to (ApplyTx), since the hooks were made (tests). */
    uint64_t TxRuleChecks() const { return m_tx_rule_checks.load(); }
    /** Why the block producer the options ask for could not start; empty if it did, or none was asked for. */
    std::string ProducerError() const { return m_producer_error; }

private:
    /**
     * Whether a PoS block of `slot` on `tip` that acknowledges an anchor of `target` is in time: the
     * blocks of stake above `target` not uncovered for max_unanchored_slots by then.
     */
    bool InTime(const CBlockIndex& tip, const CBlockIndex& target, int64_t slot) const;
    /** The acknowledgement in a PoS block, read from its sections, noted by commitment. */
    void NoteAck(const uint256& block_hash, const AnchorAck& ack) const EXCLUSIVE_LOCKS_REQUIRED(!m_acks_mutex);
    /** The acknowledgement a PoS block carries, from memory or else from disk. */
    std::optional<AnchorAck> ReadAck(const CBlockIndex& index) const EXCLUSIVE_LOCKS_REQUIRED(!m_acks_mutex);

    /**
     * The set an anchor made, from the state `view` (noted in memory then); or, unless `from_state`
     * (the rules of a block: its chain's state alone), from memory first.
     */
    std::shared_ptr<const ValidatorSet> AnchorSetOf(const uint256& anchor, const sidechain::StoreView& view, uint64_t* set_anchors, bool from_state) const
        EXCLUSIVE_LOCKS_REQUIRED(!m_sets_mutex);

    const CChainParams& m_chain_params;
    const Consensus::PosParams& m_params;
    const uint256 m_genesis;
    const ValidatorSet m_genesis_set;
    const Staking m_staking;
    const uint32_t m_pos_bits;
    std::atomic<ChainstateManager*> m_chainman{nullptr};
    mutable std::atomic<uint64_t> m_index_walks{0};
    mutable std::atomic<uint64_t> m_tx_rule_checks{0};
    //! Whether the set the newest anchor of the active chain made is active, as of the tip m_staking_for.
    mutable Mutex m_staking_mutex;
    mutable bool m_staking_seen GUARDED_BY(m_staking_mutex){false};
    mutable uint256 m_staking_for GUARDED_BY(m_staking_mutex);
    //! What DeepForkMargin keeps between calls (finality.cpp).
    struct DeepFinality {
        //! The record's height at and below which commitments are not counted (before the genesis could be known).
        int floor{-2};
        //! Commitments on record above `floor`, up to each height from floor + 1 on.
        std::vector<uint32_t> counted;
        //! The last heights counted and their hashes, to find where the record changed.
        std::deque<std::pair<int, uint256>> recent;
        //! The largest G (finality.cpp) of the forks at or below the block `through`, on a chain with
        //! the block `guard` (the oldest anchor walked then), and the record with `main_hash` at `main_height`.
        bool valid{false};
        int through_height{-1};
        uint256 through_hash;
        int guard_height{-1};
        uint256 guard_hash;
        int main_height{-1};
        uint256 main_hash;
        int64_t max_g{0};
    };
    mutable Mutex m_deep_mutex;
    mutable DeepFinality m_deep GUARDED_BY(m_deep_mutex);
    //! The final height of the active chain, as of the tip and the record's tip m_final_for.
    mutable Mutex m_final_mutex;
    mutable int m_final_height GUARDED_BY(m_final_mutex){-1};
    mutable std::pair<uint256, uint256> m_final_for GUARDED_BY(m_final_mutex);

    //! A header of a block of stake in the block index for good, and its signer (nothing: not known yet,
    //! loaded at startup say; found by its signature when HeaderSpare asks of a leader).
    struct SlotHeader {
        uint256 hash;
        std::optional<uint256> signer;
    };
    //! The headers of blocks of stake in the block index for good, by slot (HeaderSpare).
    mutable std::unordered_map<int64_t, std::vector<SlotHeader>> m_slot_headers GUARDED_BY(::cs_main);
    //! The last header (or block) whose signature by the leader of its slot was checked, and that leader
    //! (HeaderKept notes it so, right after).
    mutable std::pair<uint256, uint256> m_checked_signer GUARDED_BY(::cs_main);
    //! The header HeaderSpare last found spare: CheckHeader, right after, checks no signature of it (it
    //! is refused, or taken unchecked: its block's signature is checked when connected), and clears it.
    mutable uint256 m_spare_header GUARDED_BY(::cs_main);

    mutable Mutex m_sets_mutex;
    struct KnownSet {
        std::shared_ptr<const ValidatorSet> set;
        uint64_t anchors{0};
    };
    //! The sets anchors made, by anchor, as this node read them from a state: of branches no longer active too.
    mutable std::map<uint256, KnownSet> m_known_sets GUARDED_BY(m_sets_mutex);
    mutable std::deque<uint256> m_known_order GUARDED_BY(m_sets_mutex);
    //! Blocks taken back because the record moved the seed of their epoch.
    mutable std::set<uint256> m_seed_failed GUARDED_BY(m_sets_mutex);
    //! The mainchain tip for which the seeds of the active chain were last checked.
    mutable uint256 m_seeds_checked_for GUARDED_BY(m_sets_mutex);

    mutable Mutex m_acks_mutex;
    //! Commitment (h*) of each acknowledging block seen, and the blocks of each commitment.
    mutable std::map<uint256, uint256> m_ack_of GUARDED_BY(m_acks_mutex);
    //! The block each acknowledging block seen anchors (P).
    mutable std::map<uint256, uint256> m_anchored_by GUARDED_BY(m_acks_mutex);
    mutable std::map<uint256, std::set<uint256>> m_acks_by_commitment GUARDED_BY(m_acks_mutex);
    mutable std::deque<std::pair<uint256, uint256>> m_ack_order GUARDED_BY(m_acks_mutex);

    mutable EvidencePool m_evidence;
    //! The last proof whose signatures ChainEvidence checked (round robin over those not checked).
    mutable std::optional<Offence> m_evidence_cursor GUARDED_BY(::cs_main);
    //! Proofs relayed by each peer whose signatures this node checks (the rest are dropped).
    mutable EvidenceRateLimiter m_evidence_rate;

    mutable Mutex m_tickets_mutex;
    mutable std::map<uint256, AnchorTicket> m_tickets GUARDED_BY(m_tickets_mutex);
    mutable std::deque<uint256> m_ticket_order GUARDED_BY(m_tickets_mutex);
    mutable std::set<uint256> m_own_tickets GUARDED_BY(m_tickets_mutex);
    //! Tickets taken that the mainchain had not committed to, since the mainchain block m_ticket_counts_for:
    //! by the group of the peers they came from (TICKETS_BY_*, then the network group of a routable one,
    //! or the id of a peer the node could not find) ...
    static constexpr int TICKETS_BY_GROUP{0}, TICKETS_BY_NONROUTABLE{1}, TICKETS_BY_UNKNOWN_PEER{2};
    mutable std::map<std::pair<int, uint64_t>, size_t> m_ticket_counts GUARDED_BY(m_tickets_mutex);
    //! ... from peers the node could not find (by id, TICKETS_BY_UNKNOWN_PEER), and in all.
    mutable size_t m_unknown_ticket_count GUARDED_BY(m_tickets_mutex){0};
    mutable size_t m_ticket_count GUARDED_BY(m_tickets_mutex){0};
    mutable uint256 m_ticket_counts_for GUARDED_BY(m_tickets_mutex);

    mutable node::NodeContext* m_node{nullptr};
    mutable std::unique_ptr<Producer> m_producer;
    mutable std::string m_producer_error;
    //! The mainchain block for which a ticket was last asked for.
    mutable Mutex m_anchor_mutex;
    mutable uint256 m_ticket_requested_for GUARDED_BY(m_anchor_mutex);
    //! The mainchain block for which this node's committed ticket was last relayed again.
    mutable uint256 m_ticket_relayed_for GUARDED_BY(m_anchor_mutex);
    //! The mainchain block for which missing tickets were last asked for.
    mutable uint256 m_tickets_asked_for GUARDED_BY(m_anchor_mutex);
    //! The bid of this node's anchorer for the next mainchain block, the one after `main_tip`.
    struct Bid {
        uint256 main_tip;
        CAmount offer{0};
        //! Whether its ticket's block is at or after the anchor point its acknowledgement will follow (its anchor may build a bundle).
        bool after_anchor_point{false};
        //! Whether it was made again (once) for a block after the anchor point.
        bool again{false};
        //! Whether a ticket was made at all (none while there was no block to anchor after the waiting commitment's).
        bool made{false};
    };
    mutable Bid m_bid GUARDED_BY(m_anchor_mutex);
    //! A validator's own anchorer, if this node is a validator.
    mutable std::optional<ValidatorBid> m_validator GUARDED_BY(m_anchor_mutex);
    //! The bid policy of a node that is no validator (its merged miner's); a validator's is m_validator's.
    mutable AnchorBidPolicy m_node_policy GUARDED_BY(m_anchor_mutex);
    const AnchorBidPolicy& PolicyLocked() const EXCLUSIVE_LOCKS_REQUIRED(m_anchor_mutex) { return m_validator ? m_validator->policy : m_node_policy; }
    //! What the bids that landed cost, and when they were made (the period's spending; a month at most).
    mutable std::deque<std::pair<int64_t, CAmount>> m_bid_landed GUARDED_BY(m_anchor_mutex);
    /** A landed bid, for what anchoring collected and paid (the last ANCHORS_LANDED_SECONDS). */
    struct LandedAnchor {
        int64_t time{0};
        CAmount offer{0};
        CAmount pot{0};
        std::string basis;
    };
    mutable std::deque<LandedAnchor> m_anchors_landed GUARDED_BY(m_anchor_mutex);
    //! The bids settled MISSED, NO_BMM or TOO_SOON, and when they were made (the last ANCHORS_LANDED_SECONDS).
    mutable std::deque<std::pair<int64_t, AnchorBidRecord::Status>> m_bids_unlanded GUARDED_BY(m_anchor_mutex);
    /** A mainchain block as PollBidSignals saw it: whether it carried any BMM commitment, and its time. */
    struct MainSeen {
        uint256 hash;
        bool bmm{true};
        int64_t time{0};
    };
    //! By height, the last MAIN_SEEN blocks (one a mainchain tip polled; a block that was never the tip is not known).
    mutable std::map<int, MainSeen> m_main_seen GUARDED_BY(m_anchor_mutex);
    /** What PollBidSignals saw of the mainchain block at `height` with `hash`, if it saw it. */
    std::optional<MainSeen> MainSeenLocked(int height, const std::optional<uint256>& hash) const EXCLUSIVE_LOCKS_REQUIRED(m_anchor_mutex);
    //! The anchor reserve: what the holdbacks of landed fee bids put in, less the landed bids without fees; and what those
    //! spent past an empty reserve since it was last at its target (reported, never a reason not to bid).
    mutable CAmount m_anchor_reserve GUARDED_BY(m_anchor_mutex){0};
    mutable CAmount m_anchor_deficit GUARDED_BY(m_anchor_mutex){0};
    //! Taking turns: whether this node was designated for the mainchain block after the tip m_turn_for (the last round),
    //! for the report; and the mainchain height from which it holds a bid while another validator has the turn (-1: none).
    mutable bool m_turn_designated GUARDED_BY(m_anchor_mutex){false};
    mutable int m_turn_main_height GUARDED_BY(m_anchor_mutex){0};
    mutable int m_turn_held_since GUARDED_BY(m_anchor_mutex){-1};
    mutable int64_t m_turn_held_time GUARDED_BY(m_anchor_mutex){0};
    //! What the transactions of each block of stake no anchor covers move (the sum of their outputs), by block: read once
    //! a block (ValueAtRisk), the blocks of the run only.
    mutable Mutex m_risk_mutex;
    mutable std::map<uint256, CAmount> m_risk_moved GUARDED_BY(m_risk_mutex);
    //! The run's total as of the tip m_risk_for, the last anchored height m_risk_anchored.
    mutable uint256 m_risk_for GUARDED_BY(m_risk_mutex);
    mutable int m_risk_anchored GUARDED_BY(m_risk_mutex){-1};
    mutable CAmount m_risk_value GUARDED_BY(m_risk_mutex){0};
    /** What the transactions of the active chain's blocks above `anchored_height`, to `tip`, move: their outputs summed. */
    CAmount ValueAtRisk(const CBlockIndex* tip, int anchored_height) const EXCLUSIVE_LOCKS_REQUIRED(!m_risk_mutex, !::cs_main);
    /** The stale bid a new one must pay more than (pos::StaleOffer over the history), at `now`. */
    CAmount StaleLocked(int64_t now) const EXCLUSIVE_LOCKS_REQUIRED(m_anchor_mutex);
    //! The policy of the options, and the fields the settings changed.
    mutable AnchorBidPolicy m_bid_defaults GUARDED_BY(m_anchor_mutex);
    mutable UniValue m_bid_overrides GUARDED_BY(m_anchor_mutex){UniValue::VOBJ};
    //! The bids made (newest last, BID_HISTORY at most), the run of missed blocks, what the last round saw.
    mutable std::deque<AnchorBidRecord> m_bid_history GUARDED_BY(m_anchor_mutex);
    mutable int m_bid_missed GUARDED_BY(m_anchor_mutex){0};
    mutable int m_bid_missed_warned GUARDED_BY(m_anchor_mutex){0};
    mutable BidInputs m_bid_inputs GUARDED_BY(m_anchor_mutex);
    //! The well-formed BMM requests the last 12 mainchain blocks took (the anchor bid's fallback base).
    mutable std::deque<std::vector<BmmRequestSeen>> m_bid_seen GUARDED_BY(m_anchor_mutex);
    mutable std::optional<CAmount> m_bid_balance GUARDED_BY(m_anchor_mutex);
    mutable std::string m_bid_balance_error GUARDED_BY(m_anchor_mutex);
    mutable std::string m_bid_fee_source GUARDED_BY(m_anchor_mutex);
    mutable uint256 m_bid_polled_for GUARDED_BY(m_anchor_mutex);
    mutable bool m_bid_low_warned GUARDED_BY(m_anchor_mutex){false};
    mutable bool m_bid_off_warned GUARDED_BY(m_anchor_mutex){false};
    //! Where the bids made and what landed cost are kept across restarts (BID_FILE in the network's data directory; empty: not kept).
    mutable fs::path m_bid_file GUARDED_BY(m_anchor_mutex);
    /** Write the bids made, what landed cost (pruned to the longest period) and the run of missed blocks to m_bid_file. */
    void SaveBidsLocked() const EXCLUSIVE_LOCKS_REQUIRED(m_anchor_mutex);
    /** Read them back from `path` (nothing if it is missing or not a file of bids), and keep them there from now on. */
    void LoadBids(const fs::path& path) const EXCLUSIVE_LOCKS_REQUIRED(!m_anchor_mutex);
    /** Settle the pending bids with the commitments of the mainchain record; what landed costs in the period. */
    void SettleBids(const sidechain::Mainchain& record) const EXCLUSIVE_LOCKS_REQUIRED(m_anchor_mutex);
    CAmount SpentInPeriod(const AnchorBidPolicy& policy, int64_t now) const EXCLUSIVE_LOCKS_REQUIRED(m_anchor_mutex);
    /** The mainchain's next-block fee estimate and the bid wallet's balance, once per mainchain block. */
    void PollBidSignals(const sidechain::Follower& follower, const uint256& main_tip, const AnchorBidPolicy& policy) const EXCLUSIVE_LOCKS_REQUIRED(!m_anchor_mutex);
};

/** The bids an anchorer remembers. */
inline constexpr size_t BID_HISTORY{50};
/** While the last bid for work waiting without fees did not land, the next such bid waits this many mainchain blocks. */
inline constexpr int WORK_BID_SPACING{3};
/** The file, in the network's data directory beside settings.json, that keeps the node's bids across restarts: the history
 * (the stale rule, the pages) and what the landed ones cost (the period's cap, "spent" and "left"). */
inline constexpr const char* BID_FILE{"anchorbids.json"};
//! The longest period of the cap (periodhours at most 744): what landed is kept that long.
inline constexpr int64_t BID_LANDED_SECONDS{int64_t{24 * 31} * 3600};
//! What anchoring collected and paid is reported for 7 days at most: the landed anchors are kept that long.
inline constexpr int64_t ANCHORS_LANDED_SECONDS{int64_t{7 * 24} * 3600};
//! The mainchain blocks PollBidSignals remembers (whether they carried BMM, their time).
inline constexpr size_t MAIN_SEEN{200};
/** The node's setting (settings.json) that keeps the fields setanchorbidpolicy changed. */
inline constexpr const char* ANCHOR_BID_SETTING{"anchorbidpolicy"};
/**
 * The anchor bid policy of the options: -validatoranchorbid (the floor; off), -validatoranchorbidmode (the old
 * modes: standard is feespreventstall, fees is fees at 100 %), -validatoranchorbidmax and the others
 * (-validatoranchorbidstrategy, ...). Off on regtest unless one is given. Errors logged, the defaults kept.
 */
AnchorBidPolicy AnchorBidPolicyFromArgs(const ArgsManager& args, ChainType chain);
/**
 * The fields to read of an 8.3.5 settings.json (IsLegacySettings), which saved only the fields changed: without a
 * strategy, the options' if it depends on the floor (8.3.5's default, feespreventstall, unless the options say another),
 * so that the floor of the settings makes it Fees + minimum as it did. Read with StrategyContext::LEGACY_SETTINGS.
 */
UniValue LegacyBidSettings(const UniValue& saved, const AnchorBidPolicy& defaults, const ArgsManager& args, ChainType chain);
/** Those fields in the words of 8.3.6, with preventstall (what setanchorbidpolicy saves), for `policy` read from them. */
UniValue NormalizedLegacySettings(const UniValue& fields, const AnchorBidPolicy& policy);
/** The policy in a line, for the log. */
std::string AnchorBidPolicySummary(const AnchorBidPolicy& policy);

/** The FastBlocks hooks installed, if they are. */
FastBlocksHooks* InstalledHooks();
/** Install the FastBlocks hooks for a chain that is a sidechain (none otherwise). */
void InstallHooks(const CChainParams& params);
/** Put the template's hooks back. */
void UninstallHooks();

} // namespace pos

#endif // BITCOIN_POS_HOOKS_H
