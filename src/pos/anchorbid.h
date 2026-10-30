// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_ANCHORBID_H
#define BITCOIN_POS_ANCHORBID_H

// The bid policy of a validator's own anchorer: what it offers the mainchain's miners for a BMM commitment to
// its ticket in every mainchain block. Consensus-neutral: what a node bids is its own business; the chain only
// sees whether a commitment landed. See doc/fastblocks-anchor-bids.md.

#include <consensus/amount.h>

#include <uint256.h>

#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

class UniValue;

namespace pos {

/**
 * What the node bids (since 8.3.6: one choice of five, and "Prevent stalling" beside it, AnchorBidPolicy::prevent_stall).
 * See doc/fastblocks-anchor-bids.md, "Strategies".
 */
enum class BidStrategy {
    //! The fee share of what the anchor (or the BMM block) pays out, no minimum: no fees, no bid. The default.
    FEES,
    //! The fee share, at least the minimum (the floor, or the relay minimum of a request if higher): every mainchain block.
    FEES_MINIMUM,
    //! The minimum for every mainchain block.
    MINIMUM,
    //! `fixed` for every mainchain block.
    FIXED,
    //! No bids at all (neither trigger; one by hand still goes).
    OFF,
};

std::string StrategyName(BidStrategy strategy);
std::optional<BidStrategy> ParseStrategy(const std::string& name);

/**
 * Where a strategy word comes from, for the words that are both a name before 8.3.6 and a new name (fees, fixed, off):
 * doc/fastblocks-anchor-bids.md, "The names before 8.3.6".
 */
enum class StrategyContext {
    //! setanchorbidpolicy, the app, and the settings 8.3.6 writes (always with preventstall): the new names.
    CURRENT,
    //! -validatoranchorbidstrategy: the new names, but off, which is 8.3.5's (the anchorer off alone).
    OPTIONS,
    //! The settings 8.3.5 wrote (an anchorbidpolicy without preventstall nor anchor): 8.3.5's meaning of every word.
    LEGACY_SETTINGS,
};

/** A strategy as it was said: new names, and the names before 8.3.6, which also said whether to prevent stalling. */
struct StrategyWord {
    BidStrategy strategy{BidStrategy::FEES};
    //! What the word says of "Prevent stalling", if anything (the old names: minimal and feespreventstall yes, and in
    //! 8.3.5's settings fees and fixed no; the new names, fees and fixed among them, and off say nothing).
    std::optional<bool> prevent_stall;
    //! The old feespreventstall (or -validatoranchorbidmode=standard), and fees in 8.3.5's settings: Fees + minimum when
    //! the policy has a floor above 0 (it bid at least the floor), Fees without one.
    bool fees_or_floor{false};
    //! 8.3.5's off (in its settings, or -validatoranchorbidstrategy=off): the validator's anchorer off ("Anchor my
    //! validator", anchor false), the strategy as it was; the merged miner (setbmm) still bids, as in 8.3.5.
    bool anchor_off{false};
};
std::optional<StrategyWord> ParseStrategyWord(const std::string& name, StrategyContext context = StrategyContext::CURRENT);
/** Whether a saved anchorbidpolicy is one 8.3.5 wrote: neither preventstall nor anchor (8.3.6 always saves preventstall). */
bool IsLegacySettings(const UniValue& saved);

//! Optional operator floor. Automatic bids also cover the estimated mainchain transaction fee.
inline constexpr CAmount DEFAULT_VALIDATOR_ANCHOR_BID{0};
//! The most it bids by default, whatever the fees, the pressure or the mempool: 0.001 coins (about 526 sat/vB).
inline constexpr CAmount DEFAULT_ANCHOR_BID_MAX{100'000};
//! What it spends at most by default in a period (landed bids): 0.01 coins a day.
inline constexpr CAmount DEFAULT_ANCHOR_BID_PERIOD_CAP{1'000'000};
//! The size of a BMM request on the mainchain, for a fee rate made a bid.
inline constexpr int DEFAULT_ANCHOR_REQUEST_VSIZE{189};

/** A point of the escalation curve: at stall pressure `pressure`, the escalated bid is `multiplier` times its base. */
struct EscalationPoint {
    double pressure{0};
    double multiplier{1};
    bool operator==(const EscalationPoint&) const = default;
};

struct AnchorBidPolicy {
    BidStrategy strategy{BidStrategy::FEES};
    //! "Prevent stalling": as the chain nears the unanchored limit, a bid that lands an anchor whatever the strategy (the stall
    //! boost or the curve); when work waits for an anchor (deposits, withdrawals, and bonds, unbonds and jails), the competitive base;
    //! after missed blocks, the minimum raised for the strategies that bid every block. Not with OFF.
    bool prevent_stall{true};
    //! "Anchor my validator": a validator's own anchorer bids (the other trigger is the merged miner, setbmm).
    bool anchor{true};
    //! The user's floor: the minimum is this, or the relay minimum of a request if higher (MinimumBid).
    CAmount floor{DEFAULT_VALIDATOR_ANCHOR_BID};
    //! The most it bids, always.
    CAmount max{DEFAULT_ANCHOR_BID_MAX};
    //! The bid of the fixed strategy.
    CAmount fixed{DEFAULT_VALIDATOR_ANCHOR_BID};
    //! The share of the pot the fee strategies bid, in basis points (9900: all but the operator's 1 %).
    int fee_share_bps{9900};
    //! Stall pressure from which the bid escalates.
    double escalation_start{0.5};
    //! The escalation curve: ascending in pressure.
    std::vector<EscalationPoint> curve{{0.5, 1}, {0.75, 2}, {0.9, 4}};
    //! Between the points of the curve: interpolated linearly (true) or in steps (false).
    bool interpolate{true};
    /** Stall prevention's bid: the flat boost, max(the strategy's bid, boost x the competitive base) from escalation_start
     * (the base: the minimum, the network's least competitive rate and, with use_fee_estimate, the next-block estimate
     * rounded up to whole sat/vB, else what recent blocks took). Off: the curve of the legacy policy, and its rebids.
     */
    bool flat_boost{true};
    double boost{4};
    //! Use the next-block fee estimate for stall prevention's base.
    bool use_fee_estimate{true};
    int request_vsize{DEFAULT_ANCHOR_REQUEST_VSIZE};
    //! After this many mainchain blocks in a row without its commitment, the bid is multiplied by rebid_step for each
    //! such run (rebid_step^(missed / rebid_after)), at most rebid_max times (the legacy curve); with the flat boost, x2 for
    //! every two missed, at most the boost. 0: no raises after missed blocks in either.
    int rebid_after{3};
    double rebid_step{2};
    double rebid_max{8};
    //! A warning after this many missed mainchain blocks in a row ("the bid is too low"). 0: none.
    int warn_missed{3};
    //! The most the landed bids may cost in period_hours (0: no cap).
    CAmount period_cap{DEFAULT_ANCHOR_BID_PERIOD_CAP};
    int period_hours{24};
    //! The share of the period's cap kept for stall prevention's bids, in percent: every other bid (fees, the minimum,
    //! fixed, work waiting, setbmm's always) spends at most the cap less it, a stall bid the whole cap. Peg work and set
    //! changes anyone can make cheaply cannot spend what keeps the chain from halting.
    int stall_reserve_pct{50};
    //! A warning when the bid wallet covers fewer mainchain blocks than this at the current bid (0: none).
    int low_balance_blocks{1008};
    //! Since 8.3.7, demand-driven anchoring (doc/fastblocks-anchor-bids.md, "When the anchorer bids"):
    //! Fees bids only once the fee share is `batch` times the landing price (MinimumBid) at least, so that one anchor
    //! carries the fees of many blocks: the pot keeps growing across the blocks no anchor covers, so waiting loses no
    //! fees, and a request of ~378 sat on eCash is not paid for every few sat of fees. 0: as before 8.3.7 (a share that
    //! pays for its own request).
    double batch{10};
    //! The heartbeat: after this many hours without an anchor (the age of the unanchored run, in slots), a bid of the
    //! competitive base whatever the fees, so that a quiet chain is anchored every few hours, not only at the stall
    //! bid's p = 0.5 (8 h on eCash mainnet). 0: none.
    double heartbeat_hours{6};
    //! Value at risk: when the transactions of the blocks of stake no anchor covers move this much at least (the sum of
    //! their outputs), a bid like work waiting (the competitive base, the work throttle). 0: none.
    CAmount value_at_risk{COIN};
    //! The anchor reserve, a ledger of this node funded by its fee bids: while it holds less than reserve_target, a fee
    //! bid keeps back reserve_pct of the fee share (at most what the reserve lacks), credited when the bid lands; a
    //! landed bid without fees behind it (the heartbeat, work, value at risk, a stall) is debited from it.
    int reserve_pct{20};
    CAmount reserve_target{1'000'000};
    //! Taking turns: of the active validators, one is designated for each mainchain block (DesignatedBidder) and bids at
    //! once; the others hold their bid until the need has gone turn_backup mainchain blocks without an anchor. Fees only;
    //! a stall bid never waits.
    bool turns{true};
    int turn_backup{2};

    bool Bids() const { return strategy != BidStrategy::OFF; }
    //! A bid for every mainchain block (the minimum strategies and fixed), not only when fees pay.
    bool BidsEveryBlock() const { return strategy == BidStrategy::FEES_MINIMUM || strategy == BidStrategy::MINIMUM || strategy == BidStrategy::FIXED; }
    //! Stall prevention in force (OFF means off: the checkbox is kept, greyed out).
    bool PreventsStall() const { return Bids() && prevent_stall; }
    bool operator==(const AnchorBidPolicy&) const = default;
};

/** What is wrong with a policy, empty if nothing. */
std::string CheckPolicy(const AnchorBidPolicy& policy);
/** The policy's fields, as the RPCs say them. */
UniValue PolicyToUniValue(const AnchorBidPolicy& policy);
/**
 * Set the fields given in `fields` (an object: numbers, booleans, or text as in the options) on `policy`.
 * @return false, and why in `error`, if a field is unknown or not a value of its kind (the policy is then not checked).
 */
bool ApplyPolicyFields(AnchorBidPolicy& policy, const UniValue& fields, std::string& error, StrategyContext context = StrategyContext::CURRENT);
/**
 * setanchorbidpolicy `fields` without a strategy, on a node whose saved fields (`overrides`) keep an old strategy word
 * that depends on the floor (feespreventstall): that strategy again for the floor now in force, as a restart reads the
 * settings, so that a floor set later bids the same before and after a restart.
 */
void ReapplySavedStrategy(AnchorBidPolicy& policy, const UniValue& overrides, const UniValue& fields);
/** The names of the fields of a policy. */
const std::vector<std::string>& PolicyFieldNames();

/** What the bid is made from. */
struct BidInputs {
    //! What the anchor's pot will pay its payee.
    CAmount pot{0};
    int64_t slot{0};
    //! The first slot of the run of blocks of stake without an anchor, -1 if none.
    int64_t first_unanchored_slot{-1};
    int64_t max_unanchored_slots{1};
    //! The mainchain's fee estimate for its next block, in satoshis per 1000 vbytes, if known.
    std::optional<CAmount> fee_rate_per_kvb;
    //! Mainchain relay minimum in satoshis per 1000 vbytes; conservative fallback when unavailable.
    CAmount relay_rate_per_kvb{1000};
    //! Work waits for an anchor: a deposit to credit, withdrawals or a bundle, or a bond, an unbond or a jail (NeedsAnchor;
    //! stall prevention bids for it). Not the staking accounting (the pot's reserve, the fee pool, rewards owed, the unbond
    //! release countdown). Unknown: bid.
    bool pending_actions{true};
    //! The least feerate of a BMM request recent mainchain blocks took (any slot), per 1000 vbytes, if seen.
    std::optional<CAmount> included_rate_per_kvb;
    //! The network's least competitive rate for a bid, per 1000 vbytes (PosParams::min_bid_rate_per_kvb).
    CAmount min_rate_per_kvb{0};
    //! Mainchain blocks in a row that came without this anchorer's commitment while it bid.
    int missed{0};
    //! What landed bids cost in the current period.
    CAmount spent_in_period{0};
    //! What the transactions of the blocks of stake no anchor covers move: the sum of their outputs (change too).
    CAmount value_at_risk{0};
    //! What the node's anchor reserve holds (AnchorBidPolicy::reserve_pct).
    CAmount reserve{0};
    //! The length of a slot, for the heartbeat's hours.
    int64_t slot_seconds{15};
    //! A commitment of this chain in the last mainchain block waits for its acknowledgement: the unanchored run ends
    //! with it, so no heartbeat for it (a stall bid still goes).
    bool anchor_waiting{false};
};

/** A BMM request a mainchain block took: its slot and its feerate (per 1000 vbytes). */
struct BmmRequestSeen {
    uint8_t slot{0};
    CAmount rate_per_kvb{0};
};
/**
 * The slot of a well-formed BMM request output (OP_RETURN, a push of 68 bytes: 00 bf 00, the slot, the commitment, the
 * mainchain block it follows), for a request of a block that follows `prev`; nothing for anything else.
 */
std::optional<uint8_t> BmmRequestSlot(std::span<const unsigned char> script, const uint256& prev);
/**
 * What recent mainchain blocks took for a BMM request (the fallback base when no estimate comes). In each block, one value
 * an active sidechain's slot other than `own`: the lowest rate of that slot's requests (a real bidder needs one landed
 * request); the block's value is the median of those, given `min_slots` such slots at least; the result is the median of
 * the blocks' values over the window. So one party moves a block's value only through the slots whose requests it
 * controls (at most the active slots, about 8 on betanet), not by the number of its requests, and must do so in most of
 * the window's blocks to move the result. No Sybil resistance is claimed beyond that: the real bounds are the caller's
 * clamp (the network's least rate, ten times it) and the policy's max.
 */
std::optional<CAmount> RobustIncludedRate(const std::vector<std::vector<BmmRequestSeen>>& blocks, const std::set<uint8_t>& active, std::optional<uint8_t> own,
                                          size_t min_slots = 3);

/** (slot - first_unanchored_slot) / max_unanchored_slots: 0 without a run, 1 at the halt. */
double StallPressure(int64_t slot, int64_t first_unanchored_slot, int64_t max_unanchored_slots);
/** The multiplier of the escalation curve at pressure `p` (1 if the curve is empty). */
double EscalationMultiplier(const AnchorBidPolicy& policy, double p);

struct BidDecision {
    //! Whether to bid at all (no: off, or the period's cap spent).
    bool bid{false};
    CAmount amount{0};
    //! The strategy's bid before stall prevention.
    CAmount normal{0};
    //! "fees", "minimum", "fixed", "rebid", "pending" (peg work), "valueatrisk" (the value the unanchored blocks move),
    //! "heartbeat" (hours without an anchor), "stall" (the stall boost), "escalation" (the curve), "max", "periodcap",
    //! "replace", "always", "idle" (Fees without fees that pay, or below a batch) or "off"; the node adds "throttled"
    //! and "turn" (another validator's turn).
    std::string kind;
    //! In words, for the log and the pages.
    std::string reason;
    double pressure{0};
    double multiplier{1};
    //! What set it off (DecideNodeBid): "anchor", "mine", or "anchor+mine" (one request serves both); "hand" for one by hand.
    std::string trigger;
    //! A stall prevention bid (the stall boost or the curve): it may spend the whole cap, the stall reserve too.
    bool stall{false};
    //! A bid for work waiting without fees (kind "pending" or "valueatrisk" before the limits): throttled by the node
    //! (FollowerRound).
    bool work{false};
    //! A heartbeat bid: never throttled, and, as liveness, it may spend the stall reserve of the cap.
    bool heartbeat{false};
    //! What the bid is for, before the limits (max, periodcap, replace) name it: "fees", "minimum", "fixed", "rebid",
    //! "pending", "valueatrisk", "heartbeat", "stall", "escalation", "always" (or "hand", a bid by hand).
    std::string basis;
    //! What a fee bid keeps back of the fee share for the anchor reserve, credited to it when the bid lands.
    CAmount holdback{0};
    //! A bid with no fees behind it (work, value at risk, the heartbeat, a stall): debited from the reserve when it lands.
    bool unfunded{false};
    //! A bid that takes turns (AnchorBidPolicy::turns: Fees, not a stall bid): a validator not designated holds it.
    bool turns{false};
    //! Whether it may spend the stall reserve of the cap (CapLeft's `stall`): a stall bid or a heartbeat.
    bool SpendsReserve() const { return stall || heartbeat; }
};

/**
 * What is left of the period's cap for a bid (MAX_MONEY without a cap): for a stall bid the whole cap less what was
 * spent; for any other, the cap less the stall reserve (stall_reserve_pct) less what was spent.
 */
CAmount CapLeft(const AnchorBidPolicy& policy, CAmount spent, bool stall);

/** Replacement requests need a higher offer; refuse one that would exceed the configured limits. */
std::optional<CAmount> ReplacementBid(const AnchorBidPolicy& policy, CAmount amount, CAmount previous, CAmount spent, bool stall = false);

/** The bid of `policy` for these inputs. */
BidDecision DecideBid(const AnchorBidPolicy& policy, const BidInputs& inputs);

/**
 * The share of `pot` a fee strategy bids: all but the operator's margin (pot - pot / 100 at 99 %, as
 * sidechain::Follower::Offer).
 */
CAmount FeeShare(CAmount pot, int fee_share_bps);

//! What a bid has to pay above another one that spends the same coin, for nodes to take it as a replacement:
//! the relay fee of the largest request a wallet makes (250 vB at 1 sat/vB) with a margin (as ReplacementBid).
inline constexpr CAmount REPLACEMENT_BUMP{500};
//! The least fee share the Fees strategy bids (either trigger): below it, a request does not pay for its own place in a
//! mainchain block (sidechain::Follower::MIN_OFFER). It skips a bid, it never raises one: Fees has no minimum.
inline constexpr CAmount MINE_MIN_OFFER{500};

/**
 * What makes this node bid for the next mainchain block (doc/fastblocks-anchor-bids.md, "Triggers"). Both pay
 * from the same mainchain wallet under the same policy; only what sets them off differs.
 */
struct BidTriggers {
    //! "Anchor my validator": a validator's own anchorer (a validator whose policy has `anchor`).
    bool anchor{false};
    //! "Merged-mine as a backup producer": the merged miner (setbmm on), which bids when the fee share pays for a request.
    bool mine{false};
    //! setbmm's "always" (for tests): a bid for every mainchain block, `mine_amount` (at most the policy's max).
    bool mine_always{false};
    CAmount mine_amount{0};
    //! The most a bid of this node that did not land offered, since one last landed: other nodes may still hold
    //! it in their mempools, and a new bid spending the same coin must pay more to replace it (0: none).
    CAmount stale{0};
};

/**
 * The minimum of the minimum strategies: the user's floor, or the relay minimum of a request (the relay rate times the
 * larger of requestvsize and 250 vB, rounded up) if higher.
 */
CAmount MinimumBid(const AnchorBidPolicy& policy, const BidInputs& inputs);

/**
 * The relay minimum of a request: the relay rate times the larger of requestvsize and 250 vB, rounded up (the
 * largest request a wallet makes; a request of more inputs than the usual 189 vB was refused under it).
 */
CAmount RelayMinimum(const AnchorBidPolicy& policy, const BidInputs& inputs);

/**
 * The fee share from which Fees bids: `batch` times the landing price (MinimumBid), rounded up, and at least what pays
 * for a request (MINE_MIN_OFFER, the relay minimum).
 */
CAmount BatchNeeded(const AnchorBidPolicy& policy, const BidInputs& inputs);

/** The heartbeat's age of the unanchored run in slots of `slot_seconds` (heartbeat_hours, rounded up); 0: none. */
int64_t HeartbeatSlots(const AnchorBidPolicy& policy, int64_t slot_seconds);

/** What a fee bid of `share` keeps back for a reserve holding `reserve`: reserve_pct of it, at most what the reserve lacks of its target. */
CAmount Holdback(const AnchorBidPolicy& policy, CAmount share, CAmount reserve);

/**
 * The validator designated to bid for mainchain block `main_height` (taking turns): of `validators` sorted by key, the
 * one at the first 8 bytes of SHA256(seed, main_height), little-endian, mod their number. The same on every node that
 * knows the set and the seed (the epoch's, else a value of the chain both know). Nothing for an empty set.
 */
std::optional<uint256> DesignatedBidder(std::vector<uint256> validators, const uint256& seed, int main_height);

//! A bid that misses this many times the relay minimum or more is not too low: the mainchain's pool may not be
//! including BMM commitments at all (the enforcer, or the pool's side), and the warning says so.
inline constexpr int POOL_WARNING_RELAY_MULTIPLE{10};

/**
 * The bid of the node for these inputs and triggers: the policy's (DecideBid), the same for both triggers; setbmm's
 * fixed offer with "always" (the higher of the two with the anchorer). Nothing with the strategy off, nor without a
 * trigger. Then above a stale bid (StaleRaise), and always within max and the period's cap.
 */
BidDecision DecideNodeBid(const AnchorBidPolicy& policy, const BidInputs& inputs, const BidTriggers& triggers);

/**
 * A bid made more than `stale` (a bid of this node that did not land, which other nodes may still hold), by
 * REPLACEMENT_BUMP, if that stays within max and what is left of the period's cap; else the bid as it is (if the
 * wallet spends the same coin, nodes holding the stale bid refuse it, which costs nothing; reason says so).
 */
void StaleRaise(const AnchorBidPolicy& policy, CAmount stale, CAmount spent, BidDecision& decision);

/** A bid of this node, as far as the stale rule needs it. */
struct PastBid {
    //! As FastBlocksHooks::AnchorBidRecord::Status: MISSED, a mainchain block that carried BMM commitments (of any
    //! sidechain) and came MIN_BID_LEAD_SECONDS or more after the bid, without this chain's; NO_BMM, a block that carried
    //! none at all (a pool that does not do BMM: no miss of ours); TOO_SOON, a block found less than MIN_BID_LEAD_SECONDS
    //! after the bid (its template predates the request).
    enum class Status { PENDING, LANDED, MISSED, OUTBID, NO_BMM, TOO_SOON };
    Status status{Status::PENDING};
    CAmount offer{0};
    int64_t time{0};
};
//! How long other nodes keep a bid that cannot be mined in their mempools at most (the default -mempoolexpiry).
inline constexpr int64_t STALE_BID_SECONDS{336 * 3600};
//! A mainchain block found less than this after a bid could not have taken it: its template predates the request (on
//! eCash mainnet, 2026-10-30: 22 of 22 bids with 4-10 s of lead landed; 114 misses came 3 s or less after the bid).
inline constexpr int64_t MIN_BID_LEAD_SECONDS{4};
/**
 * The stale bid a new one must pay more than: the highest offer of the bids, oldest first, that did not land
 * (missed, or outbid) after the last one that did, and younger than STALE_BID_SECONDS at `now`; 0 if none. Not the
 * bids that could not have landed (NO_BMM, TOO_SOON): raised above each of them, a bid that could not land compounded
 * into the next one (378, 878, 1378 sat ... on eCash mainnet, 2026-10-30, the day's cap spent by 05:13).
 */
CAmount StaleOffer(std::span<const PastBid> bids, int64_t now);

} // namespace pos

#endif // BITCOIN_POS_ANCHORBID_H
