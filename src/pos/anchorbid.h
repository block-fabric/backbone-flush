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

/** How the anchorer bids. */
enum class BidStrategy {
    //! `fixed` for every mainchain block.
    FIXED,
    //! The floor, raised as the chain nears a halt (stall pressure) and after missed blocks: keep the chain moving.
    MINIMAL,
    //! The fee share the anchor owes (the pot, less the operator's margin), at least the floor; no escalation.
    FEES,
    //! FEES, and a stall boost only when needed (stall pressure, missed blocks). The default.
    FEES_PREVENT_STALL,
    //! No bids: the chain relies on other bidders.
    OFF,
};

std::string StrategyName(BidStrategy strategy);
std::optional<BidStrategy> ParseStrategy(const std::string& name);

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
    BidStrategy strategy{BidStrategy::FEES_PREVENT_STALL};
    //! Operator floor; the automatic strategy additionally covers the relay minimum and estimate.
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
    /** Flat boost: max(fee share, boost x the estimated base), only under stall pressure.
     * With use_fee_estimate, the base covers the relay minimum and the next-block estimate rounded up to
     * whole sat/vB, as well as the operator floor. Idle default-strategy bids are skipped below the threshold.
     * Off: the legacy curve and rebid strategy.
     */
    bool flat_boost{true};
    double boost{4};
    //! Use the next-block fee estimate for automatic bids (or under pressure with the legacy curve).
    bool use_fee_estimate{true};
    int request_vsize{DEFAULT_ANCHOR_REQUEST_VSIZE};
    //! After this many mainchain blocks in a row without its commitment, the bid is multiplied by rebid_step for each
    //! such run (rebid_step^(missed / rebid_after)), at most rebid_max times. 0: never.
    int rebid_after{3};
    double rebid_step{2};
    double rebid_max{8};
    //! A warning after this many missed mainchain blocks in a row ("the bid is too low"). 0: none.
    int warn_missed{3};
    //! The most the landed bids may cost in period_hours (0: no cap).
    CAmount period_cap{DEFAULT_ANCHOR_BID_PERIOD_CAP};
    int period_hours{24};
    //! A warning when the bid wallet covers fewer mainchain blocks than this at the current bid (0: none).
    int low_balance_blocks{1008};

    bool PassesFees() const { return strategy == BidStrategy::FEES || strategy == BidStrategy::FEES_PREVENT_STALL; }
    bool Escalates() const { return strategy == BidStrategy::MINIMAL || strategy == BidStrategy::FEES_PREVENT_STALL; }
    bool Bids() const { return strategy != BidStrategy::OFF; }
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
bool ApplyPolicyFields(AnchorBidPolicy& policy, const UniValue& fields, std::string& error);
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
    //! Peg work or staking/validator-set maintenance needs an anchor. Unknown means bid conservatively.
    bool pending_actions{true};
    //! The least feerate of a BMM request recent mainchain blocks took (any slot), per 1000 vbytes, if seen.
    std::optional<CAmount> included_rate_per_kvb;
    //! The network's least competitive rate for a bid, per 1000 vbytes (PosParams::min_bid_rate_per_kvb).
    CAmount min_rate_per_kvb{0};
    //! Mainchain blocks in a row that came without this anchorer's commitment while it bid.
    int missed{0};
    //! What landed bids cost in the current period.
    CAmount spent_in_period{0};
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
    //! The strategy's bid before escalation.
    CAmount normal{0};
    //! "fixed", "floor", "fees", "rebid", "escalation", "max", "periodcap", "base", "idle", "boost" or "off".
    std::string kind;
    //! In words, for the log and the pages.
    std::string reason;
    double pressure{0};
    double multiplier{1};
};

/** Replacement requests need a higher offer; refuse one that would exceed the configured limits. */
std::optional<CAmount> ReplacementBid(const AnchorBidPolicy& policy, CAmount amount, CAmount previous, CAmount spent);

/** The bid of `policy` for these inputs. */
BidDecision DecideBid(const AnchorBidPolicy& policy, const BidInputs& inputs);

} // namespace pos

#endif // BITCOIN_POS_ANCHORBID_H
