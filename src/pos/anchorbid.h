// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_ANCHORBID_H
#define BITCOIN_POS_ANCHORBID_H

// The bid policy of a validator's own anchorer: what it offers the mainchain's miners for a BMM commitment to
// its ticket in every mainchain block. Consensus-neutral: what a node bids is its own business; the chain only
// sees whether a commitment landed. See doc/fastblocks-anchor-bids.md.

#include <consensus/amount.h>

#include <cstdint>
#include <optional>
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

//! The least a validator's anchorer bids by default on the eCash networks: 0.000005 coins (500 sat, about
//! 2.6 sat/vB for a ~190 vB request). (Off on regtest unless given.)
inline constexpr CAmount DEFAULT_VALIDATOR_ANCHOR_BID{500};
//! The most it bids by default, whatever the fees, the pressure or the mempool: 0.001 coins (about 526 sat/vB).
inline constexpr CAmount DEFAULT_ANCHOR_BID_MAX{100'000};
//! What it spends at most by default in a period (landed bids): 0.01 coins a day.
inline constexpr CAmount DEFAULT_ANCHOR_BID_PERIOD_CAP{1'000'000};
//! The size of a BMM request on the mainchain, for a fee rate made a bid.
inline constexpr int DEFAULT_ANCHOR_REQUEST_VSIZE{190};

/** A point of the escalation curve: at stall pressure `pressure`, the escalated bid is `multiplier` times its base. */
struct EscalationPoint {
    double pressure{0};
    double multiplier{1};
    bool operator==(const EscalationPoint&) const = default;
};

struct AnchorBidPolicy {
    BidStrategy strategy{BidStrategy::FEES_PREVENT_STALL};
    //! The least it bids (0 allowed: a request that pays no fee, for a miner that takes it).
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
    //! Under pressure, the base of the escalated bid is the mainchain's next-block fee estimate times the request's
    //! size (if known); else (or without an estimate) the floor (500 sat at least).
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
    //! Mainchain blocks in a row that came without this anchorer's commitment while it bid.
    int missed{0};
    //! What landed bids cost in the current period.
    CAmount spent_in_period{0};
};

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
    //! "fixed", "floor", "fees", "rebid", "escalation", "max", "periodcap" or "off".
    std::string kind;
    //! In words, for the log and the pages.
    std::string reason;
    double pressure{0};
    double multiplier{1};
};

/** The bid of `policy` for these inputs. */
BidDecision DecideBid(const AnchorBidPolicy& policy, const BidInputs& inputs);

} // namespace pos

#endif // BITCOIN_POS_ANCHORBID_H
