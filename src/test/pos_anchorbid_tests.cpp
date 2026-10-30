// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/anchorbid.h>
#include <addresstype.h>
#include <script/solver.h>
#include <pos/hooks.h>
#include <sidechain/mainchain.h>
#include <sidechain/state.h>
#include <consensus/params.h>

#include <sidechain/follower.h>
#include <common/args.h>
#include <univalue.h>
#include <common/signmessage.h>
#include <key.h>
#include <pos/staking.h>
#include <primitives/transaction.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

using namespace pos;

namespace {
BidInputs At(double p, std::optional<CAmount> rate = std::nullopt, CAmount pot = 0)
{
    BidInputs in;
    in.max_unanchored_slots = 1000;
    in.first_unanchored_slot = 5000;
    in.slot = 5000 + static_cast<int64_t>(p * 1000);
    in.fee_rate_per_kvb = rate;
    in.pot = pot;
    // A quiet chain: no peg work waiting (pending_peg_work below).
    in.pending_actions = false;
    return in;
}

AnchorBidPolicy With(BidStrategy strategy, bool prevent_stall = true)
{
    AnchorBidPolicy policy;
    policy.strategy = strategy;
    policy.prevent_stall = prevent_stall;
    return policy;
}

bool Close(double a, double b) { return std::abs(a - b) < 1e-9; }

AnchorBidPolicy Applied(const std::string& json, AnchorBidPolicy policy = {})
{
    UniValue fields;
    BOOST_REQUIRE(fields.read(json));
    std::string error;
    BOOST_REQUIRE_MESSAGE(ApplyPolicyFields(policy, fields, error), error);
    BOOST_REQUIRE_EQUAL(CheckPolicy(policy), "");
    return policy;
}
} // namespace

BOOST_AUTO_TEST_SUITE(pos_anchorbid_tests)

BOOST_AUTO_TEST_CASE(defaults)
{
    // A fresh node (8.3.6): Fees, Prevent stalling on, anchoring its validator; max and the cap as 8.3.5.
    const AnchorBidPolicy policy;
    BOOST_CHECK(policy.strategy == BidStrategy::FEES);
    BOOST_CHECK(policy.prevent_stall);
    BOOST_CHECK(policy.anchor);
    BOOST_CHECK_EQUAL(policy.floor, 0);
    BOOST_CHECK(policy.flat_boost);
    BOOST_CHECK(Close(policy.boost, 4));
    BOOST_CHECK_EQUAL(policy.max, 100'000);
    BOOST_CHECK_EQUAL(policy.period_cap, 1'000'000);
    BOOST_CHECK_EQUAL(policy.fee_share_bps, 9900);
    BOOST_CHECK(Close(policy.escalation_start, 0.5));
    BOOST_CHECK_EQUAL(CheckPolicy(policy), "");
    for (const auto s : {BidStrategy::FEES, BidStrategy::FEES_MINIMUM, BidStrategy::MINIMUM, BidStrategy::FIXED, BidStrategy::OFF}) {
        BOOST_CHECK(ParseStrategy(StrategyName(s)) == s);
    }
    BOOST_CHECK_EQUAL(StrategyName(BidStrategy::FEES_MINIMUM), "feesminimum");
    BOOST_CHECK(!ParseStrategy("standard"));
    const UniValue said{PolicyToUniValue(policy)};
    BOOST_CHECK_EQUAL(said.find_value("strategy").get_str(), "fees");
    BOOST_CHECK(said.find_value("preventstall").get_bool());
    BOOST_CHECK(said.find_value("anchor").get_bool());
}

// Every strategy, with and without stall prevention: no fees and fees, far from a stall and near one. The inputs: eCash's
// relay minimum (1 sat/vB), an estimate of 1.41 sat/vB, 189 vB; the minimum is the relay floor of 250 vB (250 sat), the
// competitive base 2 x 189 = 378 sat, the stall bid 4 x 378 = 1512 sat.
BOOST_AUTO_TEST_CASE(strategy_by_stall)
{
    struct Case {
        BidStrategy strategy;
        bool stall;
        double p;
        CAmount pot;
        std::optional<CAmount> amount; // none: no bid
        std::string kind;
    };
    const std::vector<Case> cases{
        // Fees: the fee share, no minimum; no fees, no bid; near a stall, the stall bid only with Prevent stalling.
        {BidStrategy::FEES, false, 0, 0, std::nullopt, "idle"},
        {BidStrategy::FEES, true, 0, 0, std::nullopt, "idle"},
        {BidStrategy::FEES, false, 0, 100'000, 99'000, "fees"},
        {BidStrategy::FEES, true, 0, 100'000, 99'000, "fees"},
        {BidStrategy::FEES, false, 0.6, 0, std::nullopt, "idle"},
        {BidStrategy::FEES, true, 0.6, 0, 1512, "stall"},
        {BidStrategy::FEES, true, 0.6, 1000, 1512, "stall"},
        {BidStrategy::FEES, true, 0.6, 100'000, 99'000, "fees"},
        // Fees + minimum: max(fee share, minimum), every block.
        {BidStrategy::FEES_MINIMUM, false, 0, 0, 250, "minimum"},
        {BidStrategy::FEES_MINIMUM, true, 0, 0, 250, "minimum"},
        {BidStrategy::FEES_MINIMUM, false, 0, 100'000, 99'000, "fees"},
        {BidStrategy::FEES_MINIMUM, false, 0.6, 0, 250, "minimum"},
        {BidStrategy::FEES_MINIMUM, true, 0.6, 0, 1512, "stall"},
        {BidStrategy::FEES_MINIMUM, true, 0.6, 100'000, 99'000, "fees"},
        // Minimum every block: the minimum, whatever the fees.
        {BidStrategy::MINIMUM, false, 0, 0, 250, "minimum"},
        {BidStrategy::MINIMUM, false, 0, 100'000, 250, "minimum"},
        {BidStrategy::MINIMUM, true, 0, 100'000, 250, "minimum"},
        {BidStrategy::MINIMUM, false, 0.6, 0, 250, "minimum"},
        {BidStrategy::MINIMUM, true, 0.6, 0, 1512, "stall"},
        // Fixed: the fixed amount (1234), raised near a stall only with Prevent stalling.
        {BidStrategy::FIXED, false, 0, 0, 1234, "fixed"},
        {BidStrategy::FIXED, true, 0, 100'000, 1234, "fixed"},
        {BidStrategy::FIXED, false, 0.95, 0, 1234, "fixed"},
        {BidStrategy::FIXED, true, 0.95, 0, 1512, "stall"},
        // Off: nothing, Prevent stalling or not.
        {BidStrategy::OFF, false, 0, 100'000, std::nullopt, "off"},
        {BidStrategy::OFF, true, 0, 100'000, std::nullopt, "off"},
        {BidStrategy::OFF, true, 0.95, 0, std::nullopt, "off"},
    };
    for (const Case& c : cases) {
        AnchorBidPolicy policy{With(c.strategy, c.stall)};
        policy.fixed = 1234;
        BidInputs in{At(c.p, 1410, c.pot)};
        const std::string what{strprintf("%s, stall %d, p %.2f, pot %d", StrategyName(c.strategy), c.stall, c.p, c.pot)};
        for (const BidDecision& d : {DecideBid(policy, in), DecideNodeBid(policy, in, BidTriggers{.anchor = true}), DecideNodeBid(policy, in, BidTriggers{.mine = true})}) {
            BOOST_CHECK_MESSAGE(d.bid == c.amount.has_value(), what + ": " + d.reason);
            if (c.amount) BOOST_CHECK_MESSAGE(d.amount == *c.amount, what + strprintf(": %d, ", d.amount) + d.reason);
            BOOST_CHECK_MESSAGE(d.kind == c.kind, what + ": " + d.kind);
        }
    }
}

BOOST_AUTO_TEST_CASE(fees_has_no_minimum)
{
    // The user's floor does not count for Fees; a fee share that does not pay for a request is no bid (it is not raised).
    AnchorBidPolicy policy{With(BidStrategy::FEES, /*prevent_stall=*/false)};
    policy.floor = 20'000;
    BOOST_CHECK_EQUAL(DecideBid(policy, At(0, std::nullopt, 1000)).amount, 990);
    BOOST_CHECK(!DecideBid(policy, At(0, std::nullopt, 400)).bid); // 396 < 500
    BOOST_CHECK_EQUAL(DecideBid(policy, At(0, std::nullopt, 400)).kind, "idle");
    BOOST_CHECK_EQUAL(DecideBid(policy, At(0, std::nullopt, 506)).amount, 501);
    BidInputs high{At(0, std::nullopt, 1000)};
    high.relay_rate_per_kvb = 5000; // the relay floor of a request: 1250 sat, above the share
    BOOST_CHECK(!DecideBid(policy, high).bid);
    // Pending peg work without fees: no bid either (only fees, or the stall bid).
    BidInputs pending{At(0)};
    pending.pending_actions = true;
    BOOST_CHECK(!DecideBid(policy, pending).bid);
    // 99 %: exactly what Follower::Offer bids; 100 %: the pot whole.
    for (CAmount pot : {CAmount{506}, CAmount{12'345}, CAmount{99'999}, CAmount{100'000}}) {
        BOOST_CHECK_EQUAL(DecideBid(policy, At(0, std::nullopt, pot)).amount, sidechain::Follower::Offer(pot));
    }
    policy.fee_share_bps = 10000;
    BOOST_CHECK_EQUAL(DecideBid(policy, At(0, std::nullopt, 12'345)).amount, 12'345);
}

BOOST_AUTO_TEST_CASE(the_minimum)
{
    // The floor, or the relay minimum of a request (rounded up) if higher; never the fee estimate.
    AnchorBidPolicy policy{With(BidStrategy::MINIMUM, /*prevent_stall=*/false)};
    BidInputs in{At(0, 50'000)};
    BOOST_CHECK_EQUAL(MinimumBid(policy, in), 250);
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 250);
    in.relay_rate_per_kvb = 1001;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 251);
    policy.request_vsize = 400;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 401);
    policy.floor = 10'000;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 10'000);
    in.relay_rate_per_kvb = 0; // a mainchain that relays for nothing (tests): the floor, 0 too
    policy.floor = 0;
    const auto zero{DecideBid(policy, in)};
    BOOST_CHECK(zero.bid);
    BOOST_CHECK_EQUAL(zero.amount, 0);
    // Fees + minimum: the higher of the share and the minimum.
    AnchorBidPolicy both{With(BidStrategy::FEES_MINIMUM, /*prevent_stall=*/false)};
    both.floor = 5'000;
    BOOST_CHECK_EQUAL(DecideBid(both, At(0, std::nullopt, 4'000)).amount, 5'000);
    BOOST_CHECK_EQUAL(DecideBid(both, At(0, std::nullopt, 6'000)).amount, 5'940);
    BOOST_CHECK_EQUAL(DecideBid(both, At(0, std::nullopt, 6'000)).kind, "fees");
}

BOOST_AUTO_TEST_CASE(stall_prevention)
{
    AnchorBidPolicy policy; // Fees, Prevent stalling
    const auto bid{[&](double p, CAmount pot, int missed = 0, CAmount spent = 0) {
        BidInputs in{At(p, 1410, pot)};
        in.missed = missed;
        in.spent_in_period = spent;
        return DecideBid(policy, in);
    }};
    // Below escalationstart nothing, from it the stall bid: 4 x the competitive base (ceil(1.41) x 189).
    BOOST_CHECK(!bid(0.49, 0).bid);
    BOOST_CHECK_EQUAL(bid(0.5, 0).amount, 1512);
    BOOST_CHECK_EQUAL(bid(0.95, 0).amount, 1512);
    BOOST_CHECK(bid(0.6, 0).reason.find("preventing a stall") != std::string::npos);
    // The competitive base: never below the network's least rate; without an estimate, what blocks took.
    BidInputs in{At(0.6)};
    in.min_rate_per_kvb = 2000;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 4 * 2 * 189);
    in.included_rate_per_kvb = 2900;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 4 * 3 * 189);
    in.included_rate_per_kvb = 900'000'000; // clamped to 10 x the network's least
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 4 * 20 * 189);
    policy.use_fee_estimate = false; // the estimate left out: the relay floor and the network's rate
    BOOST_CHECK_EQUAL(bid(0.6, 0).amount, 4 * 250);
    policy.use_fee_estimate = true;
    // Within max and the period's cap (the cap is the user's word: a stall bid does not go past it).
    policy.max = 1000;
    BOOST_CHECK_EQUAL(bid(0.6, 0).amount, 1000);
    BOOST_CHECK_EQUAL(bid(0.6, 0).kind, "max");
    policy.max = 100'000;
    BOOST_CHECK_EQUAL(bid(0.6, 0, 0, policy.period_cap - 700).amount, 700);
    BOOST_CHECK_EQUAL(bid(0.6, 0, 0, policy.period_cap - 700).kind, "periodcap");
    BOOST_CHECK(!bid(0.6, 0, 0, policy.period_cap).bid);
    // A floor raises the stall bid's base too.
    policy.floor = 20'000;
    BOOST_CHECK_EQUAL(bid(0.95, 0).amount, 80'000);
    // Not with the checkbox off, nor with the strategy off.
    policy.prevent_stall = false;
    BOOST_CHECK(!bid(0.95, 0).bid);
    policy.prevent_stall = true;
    policy.strategy = BidStrategy::OFF;
    BOOST_CHECK(!bid(0.95, 0).bid);
    BOOST_CHECK(!policy.PreventsStall());
}

BOOST_AUTO_TEST_CASE(pending_peg_work)
{
    // Peg work waiting for an anchor (a deposit, withdrawals, a set change): with Prevent stalling, a bid that lands, the
    // competitive base (2 x 189 at 1.41 sat/vB), fees or not; raised after missed blocks; not without the checkbox.
    AnchorBidPolicy policy;
    BidInputs in{At(0, 1410)};
    in.pending_actions = true;
    BidDecision d{DecideBid(policy, in)};
    BOOST_CHECK(d.bid);
    BOOST_CHECK_EQUAL(d.amount, 378);
    BOOST_CHECK_EQUAL(d.kind, "pending");
    in.pot = 300; // a share that does not pay: the base
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 378);
    in.pot = 5000; // a share above it: the share
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 4950);
    BOOST_CHECK_EQUAL(DecideBid(policy, in).kind, "fees");
    in.pot = 0;
    in.missed = 3;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 2 * 378);
    in.missed = 9;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 4 * 378);
    in.missed = 0;
    in.slot += 600; // near a stall: the boost
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 1512);
    BOOST_CHECK_EQUAL(DecideBid(policy, in).kind, "stall");
    in.slot -= 600;
    // A floor raises the base.
    policy.floor = 10'000;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 10'000);
    policy.floor = 0;
    // Not without Prevent stalling, nor with Off; Minimum bids the base when it is above its minimum.
    policy.prevent_stall = false;
    BOOST_CHECK(!DecideBid(policy, in).bid);
    policy.prevent_stall = true;
    policy.strategy = BidStrategy::OFF;
    BOOST_CHECK(!DecideBid(policy, in).bid);
    policy.strategy = BidStrategy::MINIMUM;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 378);
    policy.prevent_stall = false;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 250);
    // A quiet chain, the default: no bid, and why.
    policy = AnchorBidPolicy{};
    in.pending_actions = false;
    d = DecideBid(policy, in);
    BOOST_CHECK(!d.bid);
    BOOST_CHECK_EQUAL(d.kind, "idle");
    BOOST_CHECK_EQUAL(d.reason, "no bid: no fees, nothing waiting to anchor");
}

BOOST_AUTO_TEST_CASE(rebids)
{
    // With Prevent stalling, missed blocks raise the minimum of the strategies that bid every block: x2 for every two
    // missed, the boost (4) at most; a fee share or a fixed amount is what it is.
    AnchorBidPolicy policy{With(BidStrategy::MINIMUM)};
    policy.floor = 1000;
    BidInputs in{At(0)};
    in.missed = 1;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 1000);
    in.missed = 2;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 2000);
    BOOST_CHECK_EQUAL(DecideBid(policy, in).kind, "rebid");
    in.missed = 9;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 4000);
    policy.strategy = BidStrategy::FEES_MINIMUM;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 4000);
    policy.prevent_stall = false;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 1000);
    policy.prevent_stall = true;
    policy.strategy = BidStrategy::FIXED;
    policy.fixed = 1000;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 1000);
    policy.strategy = BidStrategy::FEES;
    in.pot = 10'000;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 9'900);
    // The legacy policy (flatboost off): rebidstep for every rebidafter missed, at most rebidmax.
    policy = With(BidStrategy::MINIMUM);
    policy.flat_boost = false;
    policy.floor = 500;
    in = At(0);
    in.missed = 2;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 500);
    in.missed = 3;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 1000);
    in.missed = 6;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 2000);
    in.missed = 300;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 4000);
}

BOOST_AUTO_TEST_CASE(legacy_curve)
{
    // flatboost off: the stall bid follows the curve, x the estimate for the request (else the floor, 564 sat at least).
    AnchorBidPolicy policy{With(BidStrategy::MINIMUM)};
    policy.flat_boost = false;
    policy.request_vsize = 190;
    policy.floor = 500;
    for (double p : {0.0, 0.2, 0.49}) BOOST_CHECK_EQUAL(DecideBid(policy, At(p, 50'000)).amount, 500);
    BOOST_CHECK_EQUAL(DecideBid(policy, At(0.5, 50'000)).amount, 9500);
    BOOST_CHECK_EQUAL(DecideBid(policy, At(0.75, 50'000)).amount, 19'000);
    const auto high{DecideBid(policy, At(0.9, 50'000))};
    BOOST_CHECK_EQUAL(high.amount, 38'000);
    BOOST_CHECK_EQUAL(high.kind, "escalation");
    BOOST_CHECK(Close(high.multiplier, 4));
    BOOST_CHECK_EQUAL(DecideBid(policy, At(0.95, 1'000'000)).amount, policy.max);
    BOOST_CHECK_EQUAL(DecideBid(policy, At(0.92)).amount, 2256);
    // Fees without fees: the curve's bid all the same; peg work waiting: nothing more (the flat policy's rule).
    policy.strategy = BidStrategy::FEES;
    policy.floor = 0;
    BOOST_CHECK(!DecideBid(policy, At(0.2, 1000)).bid);
    BidInputs pending{At(0.2, 1000)};
    pending.pending_actions = true;
    BOOST_CHECK(!DecideBid(policy, pending).bid);
    BOOST_CHECK_EQUAL(DecideBid(policy, At(0.92, 1000)).amount, 760);
    BOOST_CHECK_EQUAL(DecideBid(policy, At(0.92)).amount, 2256);
}

BOOST_FIXTURE_TEST_CASE(pending_peg_actions, BasicTestingSetup)
{
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty};
    sidechain::State side{store};
    sidechain::Mainchain record;
    sidechain::MainBlock block;
    block.hash = uint256{1};
    BOOST_REQUIRE(record.Append(block));
    BOOST_CHECK(!HasPendingAnchorActions(side, record));
    block.prev_hash = block.hash;
    block.hash = uint256{2};
    block.deposits.push_back(sidechain::MainDeposit{});
    BOOST_REQUIRE(record.Append(block));
    BOOST_CHECK(HasPendingAnchorActions(side, record));
    // A reorg removes that uncredited deposit: no stale activity flag.
    record.Truncate(0);
    BOOST_CHECK(!HasPendingAnchorActions(side, record));
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{3}), 0});
    tx.vout.emplace_back(COIN, sidechain::WithdrawalScript(1000, uint160{}, GetScriptForDestination(PKHash{uint160{}})));
    std::vector<CTxOut> payouts, refunds;
    std::string error;
    BOOST_REQUIRE_MESSAGE(side.ApplyTx(CTransaction{tx}, 1, Consensus::SidechainParams{}, payouts, refunds, error), error);
    BOOST_CHECK(HasPendingAnchorActions(side, record));
}

// The staking accounting is no peg work: a fee pool waiting (the incident: 0.0001479 ECX, a bid every mainchain block)
// sets off no bid without fees.
BOOST_FIXTURE_TEST_CASE(staking_accounting_is_no_peg_work, BasicTestingSetup)
{
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty};
    // The validator pool (pos/hooks.cpp's POOL, table 0x5f).
    const sidechain::Table<uint64_t, int64_t> pool{0x5f};
    pool.Put(store, 72, 14'790);
    sidechain::State side{store};
    BOOST_CHECK_EQUAL(PoolTotal(side.View()), 14'790);
    sidechain::Mainchain record;
    sidechain::MainBlock block;
    block.hash = uint256{1};
    BOOST_REQUIRE(record.Append(block));
    BOOST_CHECK(!HasPendingAnchorActions(side, record));
    // Fees with Prevent stalling, no fees, far from the limit: no bid.
    BidInputs in{At(0.1)};
    in.pending_actions = HasPendingAnchorActions(side, record);
    const BidDecision d{DecideNodeBid(AnchorBidPolicy{}, in, BidTriggers{.anchor = true, .mine = true})};
    BOOST_CHECK(!d.bid);
    BOOST_CHECK_EQUAL(d.kind, "idle");
    // Near the limit, Prevent stalling bids all the same.
    in.slot = in.first_unanchored_slot + in.max_unanchored_slots * 6 / 10;
    BOOST_CHECK_EQUAL(DecideNodeBid(AnchorBidPolicy{}, in, BidTriggers{.anchor = true}).kind, "stall");
}

// A bond with no fees waits for an anchor: one bid (the strategy's for waiting work: the competitive base, at least the
// relay minimum), none once an anchor has made the new set. The unbond release countdown is no work.
BOOST_FIXTURE_TEST_CASE(set_change_needs_an_anchor, BasicTestingSetup)
{
    CKey validator, owner;
    validator.MakeNewKey(/*fCompressed=*/true);
    owner.MakeNewKey(/*fCompressed=*/true);
    Consensus::PosParams params;
    params.min_stake = 100;
    params.unbonding_anchors = 10;
    const uint256 genesis{1};
    const Staking staking{params, genesis};
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty};
    sidechain::Mainchain record;
    sidechain::MainBlock block;
    block.hash = uint256{1};
    BOOST_REQUIRE(record.Append(block));
    std::vector<CTxOut> owed;
    // The first set, made by an anchor: nothing waits.
    staking.PegStep(store, uint256{5}, 1, 0, 0, 0, owed);
    const auto needs{[&] { sidechain::State side{store}; return NeedsAnchor(side, record, staking, 0); }};
    BOOST_CHECK(!needs());
    const auto bid{[&] {
        BidInputs in{At(0.1)};
        in.pending_actions = needs();
        return DecideNodeBid(AnchorBidPolicy{}, in, BidTriggers{.anchor = true});
    }};
    BOOST_CHECK(!bid().bid);
    // A bond (no fees): one bid, the competitive base (2 sat/vB at least on eCash: here the relay minimum, 250 sat).
    const uint160 owner_hash{owner.GetPubKey().GetID()};
    const uint256 pubkey{XOnlyPubKey{validator.GetPubKey()}};
    std::array<unsigned char, 64> pop;
    BOOST_REQUIRE(validator.SignSchnorr(BondPopHash(owner_hash, genesis), pop, nullptr, uint256{}));
    CMutableTransaction bond;
    bond.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{1}), 0});
    std::vector<unsigned char> owner_sig;
    BOOST_REQUIRE(owner.SignCompact(MessageHash(BondMessage(pubkey, 10000, bond.vin[0].prevout, genesis)), owner_sig));
    bond.vout.emplace_back(10000, BondScript(pubkey, owner_hash, pop, owner_sig));
    std::string reason;
    BOOST_REQUIRE_MESSAGE(staking.ApplyTx(store, CTransaction{bond}, 1, 1, reason, uint256{9}), reason);
    BOOST_CHECK(needs());
    const BidDecision once{bid()};
    BOOST_CHECK(once.bid);
    BOOST_CHECK_EQUAL(once.kind, "pending");
    BOOST_CHECK_GE(once.amount, 250);
    // The anchor makes the new set: nothing waits, no bid.
    staking.PegStep(store, uint256{6}, 2, 0, 0, 0, owed);
    BOOST_CHECK(!needs());
    BOOST_CHECK(!bid().bid);
    // An unbond: a change of the set, one bid; then its release counting down over the anchors (unbonding > 0): none.
    Unbond u;
    u.pubkey = pubkey;
    u.amount = 10000;
    u.sequence = 0;
    BOOST_REQUIRE(owner.SignCompact(MessageHash(UnbondMessage(pubkey, 10000, 0, bond.vin[0].prevout, uint256{9}, genesis)), u.signature));
    CMutableTransaction unbond;
    unbond.vout.emplace_back(0, UnbondScript(u));
    BOOST_REQUIRE_MESSAGE(staking.ApplyTx(store, CTransaction{unbond}, 2, 2, reason, uint256{9}), reason);
    BOOST_CHECK(needs());
    staking.PegStep(store, uint256{7}, 3, 0, 0, 0, owed);
    BOOST_CHECK_GT(GetStakeState(store).unbonding, 0);
    BOOST_CHECK(!needs());
    // Off still bids nothing.
    BidInputs in{At(0.1)};
    in.pending_actions = true;
    BOOST_CHECK(!DecideNodeBid(With(BidStrategy::OFF), in, BidTriggers{.anchor = true}).bid);
}

// rebidafter 0: no raises after missed blocks, with the flat boost too; unset (3), the flat raises as before.
BOOST_AUTO_TEST_CASE(rebid_zero_no_raises)
{
    AnchorBidPolicy policy{With(BidStrategy::MINIMUM)};
    policy.floor = 1000;
    BidInputs in{At(0.1)};
    in.missed = 6;
    const BidDecision raised{DecideBid(policy, in)};
    BOOST_CHECK_EQUAL(raised.kind, "rebid");
    BOOST_CHECK_EQUAL(raised.amount, 4000); // x2 per two missed: x8, at most the boost (4)
    policy.rebid_after = 0;
    const BidDecision flat{DecideBid(policy, in)};
    BOOST_CHECK_EQUAL(flat.kind, "minimum");
    BOOST_CHECK_EQUAL(flat.amount, 1000);
    policy.flat_boost = false;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 1000);
    // The stall bid as the pressure rises stays.
    policy.flat_boost = true;
    BOOST_CHECK_EQUAL(DecideBid(policy, At(0.6)).kind, "stall");
    // And the raise above a stale bid on the same coin.
    BOOST_CHECK_EQUAL(DecideNodeBid(policy, in, BidTriggers{.anchor = true, .stale = 1000}).kind, "replace");
}

BOOST_AUTO_TEST_CASE(pressure_and_curve)
{
    BOOST_CHECK(Close(StallPressure(100, -1, 1920), 0));
    BOOST_CHECK(Close(StallPressure(100, 100, 1920), 0));
    BOOST_CHECK(Close(StallPressure(1060, 100, 1920), 0.5));
    BOOST_CHECK(Close(StallPressure(2020, 100, 1920), 1));
    AnchorBidPolicy policy;
    BOOST_CHECK(Close(EscalationMultiplier(policy, 0.2), 1));
    BOOST_CHECK(Close(EscalationMultiplier(policy, 0.5), 1));
    BOOST_CHECK(Close(EscalationMultiplier(policy, 0.625), 1.5));
    BOOST_CHECK(Close(EscalationMultiplier(policy, 0.75), 2));
    BOOST_CHECK(Close(EscalationMultiplier(policy, 0.825), 3));
    BOOST_CHECK(Close(EscalationMultiplier(policy, 0.9), 4));
    BOOST_CHECK(Close(EscalationMultiplier(policy, 1.2), 4));
    policy.interpolate = false;
    BOOST_CHECK(Close(EscalationMultiplier(policy, 0.6), 1));
    BOOST_CHECK(Close(EscalationMultiplier(policy, 0.8), 2));
    BOOST_CHECK(Close(EscalationMultiplier(policy, 0.95), 4));
}

BOOST_AUTO_TEST_CASE(replacement_limits)
{
    AnchorBidPolicy policy;
    policy.max = 1600;
    // A fee increase and a stall boost can replace the request within the operator's limits.
    BOOST_REQUIRE(ReplacementBid(policy, 1512, 378, 0));
    BOOST_CHECK_EQUAL(*ReplacementBid(policy, 1512, 378, 0), 1512);
    BOOST_CHECK_EQUAL(*ReplacementBid(policy, 500, 378, 0), 878);
    // The replacement increment must never bypass either spending limit.
    BOOST_CHECK(!ReplacementBid(policy, 1500, 1200, 0));
    policy.period_cap = 1000;
    BOOST_CHECK(!ReplacementBid(policy, 500, 378, 200));
    BOOST_CHECK(!ReplacementBid(policy, 500, 378, 1000));
    policy.period_cap = 0;
    BOOST_CHECK(ReplacementBid(policy, 500, 378, 1000));
}

// The names before 8.3.6, and the new form: existing configs and settings keep their meaning.
BOOST_AUTO_TEST_CASE(old_names)
{
    struct Case {
        const char* json;
        BidStrategy strategy;
        bool stall;
    };
    // From a policy with Prevent stalling on and one with it off: an old name says it, whatever it was.
    for (const bool before : {true, false}) {
        AnchorBidPolicy start;
        start.prevent_stall = before;
        for (const Case& c : std::vector<Case>{
                 {R"({"strategy":"feespreventstall"})", BidStrategy::FEES, true},
                 {R"({"strategy":"minimal"})", BidStrategy::MINIMUM, true},
                 {R"({"strategy":"Fees + prevent stalling"})", BidStrategy::FEES, true},
                 // The new names say nothing of it: kept (fees and fixed are new names here: "preventstall": false alone turns it off).
                 {R"({"strategy":"fees"})", BidStrategy::FEES, before},
                 {R"({"strategy":"fixed"})", BidStrategy::FIXED, before},
                 {R"({"strategy":"minimum"})", BidStrategy::MINIMUM, before},
                 {R"({"strategy":"feesminimum"})", BidStrategy::FEES_MINIMUM, before},
                 {R"({"strategy":"Fees + minimum"})", BidStrategy::FEES_MINIMUM, before},
                 // The new form, in either order.
                 {R"({"strategy":"fees","preventstall":true})", BidStrategy::FEES, true},
                 {R"({"preventstall":true,"strategy":"fees"})", BidStrategy::FEES, true},
                 {R"({"strategy":"minimal","preventstall":false})", BidStrategy::MINIMUM, false},
                 {R"({"strategy":"fixed","preventstall":"1"})", BidStrategy::FIXED, true},
             }) {
            const AnchorBidPolicy p{Applied(c.json, start)};
            BOOST_CHECK_MESSAGE(p.strategy == c.strategy, c.json);
            BOOST_CHECK_MESSAGE(p.prevent_stall == c.stall, c.json);
        }
        const AnchorBidPolicy off{Applied(R"({"strategy":"off"})", start)};
        BOOST_CHECK(off.strategy == BidStrategy::OFF);
        BOOST_CHECK(!off.Bids());
        BOOST_CHECK(!off.PreventsStall());
        BOOST_CHECK_EQUAL(off.prevent_stall, before); // kept, greyed out
    }
    // feespreventstall bid at least its floor: with one above 0, Fees + minimum (in the same fields or already set).
    BOOST_CHECK(Applied(R"({"strategy":"feespreventstall","floor":0.00001})").strategy == BidStrategy::FEES_MINIMUM);
    BOOST_CHECK(Applied(R"({"floor":0.00001,"strategy":"feespreventstall"})").strategy == BidStrategy::FEES_MINIMUM);
    BOOST_CHECK(Applied(R"({"strategy":"feespreventstall"})", Applied(R"({"floor":0.00001})")).strategy == BidStrategy::FEES_MINIMUM);
    BOOST_CHECK(Applied(R"({"strategy":"feespreventstall","floor":0})").strategy == BidStrategy::FEES);
    // "fees" is a new name too: Fees, the floor kept but unused; Prevent stalling on (the default) unless said off.
    BOOST_CHECK(Applied(R"({"strategy":"fees","floor":0.00001})").strategy == BidStrategy::FEES);
    BOOST_CHECK(Applied(R"({"strategy":"fees"})").PreventsStall());
    BOOST_CHECK(!Applied(R"({"strategy":"fees","preventstall":false})").PreventsStall());
    // Louis's app (8.3.5 settings.json): Fees, Prevent stalling on, no minimum.
    const AnchorBidPolicy ui{Applied(R"({"strategy":"feespreventstall","floor":0,"usefeeestimate":false,"feeshare":99,"max":0.0001,"periodcap":0.001})")};
    BOOST_CHECK(ui.strategy == BidStrategy::FEES);
    BOOST_CHECK(ui.prevent_stall);
    BOOST_CHECK(ui.anchor);
    BOOST_CHECK_EQUAL(ui.floor, 0);
    BOOST_CHECK(!ui.use_fee_estimate);
    BOOST_CHECK_EQUAL(ui.fee_share_bps, 9900);
    BOOST_CHECK_EQUAL(ui.max, 10'000);
    BOOST_CHECK_EQUAL(ui.period_cap, 100'000);
    BOOST_CHECK(!DecideBid(ui, At(0.1)).bid);                                  // no fees: no bid
    BOOST_CHECK_EQUAL(DecideBid(ui, At(0.1, std::nullopt, 5'000)).amount, 4'950); // the fee share
    BOOST_CHECK_EQUAL(DecideBid(ui, At(0.6)).amount, 1'000);                   // near a stall: 4 x the relay floor
    // The live validators' minimal: Minimum every block, Prevent stalling on.
    const AnchorBidPolicy live{Applied(R"({"strategy":"minimal","floor":0.0001,"max":0.001})")};
    BOOST_CHECK(live.strategy == BidStrategy::MINIMUM);
    BOOST_CHECK(live.prevent_stall);
    BOOST_CHECK_EQUAL(DecideBid(live, At(0)).amount, 10'000);
}

// The words both old and new (fees, fixed, off), by where they are said (doc/fastblocks-anchor-bids.md, "The names before 8.3.6").
BOOST_AUTO_TEST_CASE(old_and_new_words_by_context)
{
    const auto applied{[](const std::string& json, StrategyContext context, AnchorBidPolicy policy = {}) {
        UniValue fields;
        BOOST_REQUIRE(fields.read(json));
        std::string error;
        BOOST_REQUIRE_MESSAGE(ApplyPolicyFields(policy, fields, error, context), error);
        BOOST_REQUIRE_EQUAL(CheckPolicy(policy), "");
        return policy;
    }};
    for (const StrategyContext context : {StrategyContext::CURRENT, StrategyContext::OPTIONS}) {
        // New names: fees and fixed keep Prevent stalling (on by default).
        BOOST_CHECK(applied(R"({"strategy":"fees"})", context).prevent_stall);
        BOOST_CHECK(applied(R"({"strategy":"fixed","fixed":0.00001})", context).prevent_stall);
        BOOST_CHECK(applied(R"({"strategy":"fees","floor":0.00001})", context).strategy == BidStrategy::FEES);
    }
    // off: the new Off in setanchorbidpolicy and the app; 8.3.5's off (the anchorer alone) in the options.
    const AnchorBidPolicy off{applied(R"({"strategy":"off"})", StrategyContext::CURRENT)};
    BOOST_CHECK(!off.Bids());
    const AnchorBidPolicy anchor_off{applied(R"({"strategy":"off"})", StrategyContext::OPTIONS, With(BidStrategy::MINIMUM))};
    BOOST_CHECK(anchor_off.Bids());
    BOOST_CHECK(anchor_off.strategy == BidStrategy::MINIMUM);
    BOOST_CHECK(!anchor_off.anchor);
    BOOST_CHECK(anchor_off.prevent_stall);
    // With it, the merged miner still bids (as setbmm did in 8.3.5); the anchorer does not (no anchor trigger: FollowerRound).
    BOOST_CHECK(DecideNodeBid(anchor_off, At(0, std::nullopt, 10'000), BidTriggers{.mine = true}).bid);
    // 8.3.5's settings: every word as 8.3.5 meant it.
    const auto legacy{StrategyContext::LEGACY_SETTINGS};
    AnchorBidPolicy p{applied(R"({"strategy":"fees"})", legacy)};
    BOOST_CHECK(p.strategy == BidStrategy::FEES);
    BOOST_CHECK(!p.prevent_stall);
    p = applied(R"({"strategy":"fees","floor":0.00001})", legacy);
    BOOST_CHECK(p.strategy == BidStrategy::FEES_MINIMUM); // 8.3.5's fees bid at least its floor
    BOOST_CHECK(!p.prevent_stall);
    p = applied(R"({"strategy":"fixed","fixed":0.00001})", legacy);
    BOOST_CHECK(p.strategy == BidStrategy::FIXED);
    BOOST_CHECK(!p.prevent_stall);
    p = applied(R"({"strategy":"off","floor":0.00001})", legacy);
    BOOST_CHECK(p.Bids());
    BOOST_CHECK(!p.anchor);
    BOOST_CHECK_EQUAL(p.floor, 1000);
    BOOST_CHECK(applied(R"({"strategy":"off","anchor":true})", legacy).anchor); // (anchor said too: as said)
    BOOST_CHECK(applied(R"({"strategy":"minimal"})", legacy).strategy == BidStrategy::MINIMUM);
    // Which settings are 8.3.5's: neither preventstall nor anchor (8.3.6 saves preventstall with every change).
    UniValue saved;
    BOOST_REQUIRE(saved.read(R"({"floor":0.00001})"));
    BOOST_CHECK(IsLegacySettings(saved));
    BOOST_REQUIRE(saved.read(R"({"floor":0.00001,"preventstall":true})"));
    BOOST_CHECK(!IsLegacySettings(saved));
    BOOST_REQUIRE(saved.read(R"({"anchor":false})"));
    BOOST_CHECK(!IsLegacySettings(saved));
}

// An 8.3.5 settings.json with a floor and no strategy (8.3.5 saved only the fields changed; its default was
// feespreventstall): Fees + minimum with a floor above 0; and a floor set later the same at once as after a restart.
BOOST_AUTO_TEST_CASE(legacy_settings_upgrade)
{
    const auto args_of{[](std::vector<const char*> argv, ArgsManager& args) {
        for (const char* name : {"-validatoranchorbid", "-validatoranchorbidstrategy", "-validatoranchorbidpreventstall", "-validatoranchorbidmode"}) {
            args.AddArg(name, "", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
        }
        argv.insert(argv.begin(), "x");
        std::string error;
        BOOST_REQUIRE(args.ParseParameters(argv.size(), argv.data(), error));
    }};
    // As StartServices reads the settings.
    const auto start{[](const ArgsManager& args, const std::string& json, UniValue& overrides) {
        const AnchorBidPolicy defaults{AnchorBidPolicyFromArgs(args, ChainType::MAIN)};
        UniValue saved;
        BOOST_REQUIRE(saved.read(json));
        const bool legacy{IsLegacySettings(saved)};
        const UniValue fields{legacy ? LegacyBidSettings(saved, defaults, args, ChainType::MAIN) : saved};
        AnchorBidPolicy policy{defaults};
        std::string error;
        BOOST_REQUIRE_MESSAGE(ApplyPolicyFields(policy, fields, error, legacy ? StrategyContext::LEGACY_SETTINGS : StrategyContext::CURRENT), error);
        overrides = legacy ? NormalizedLegacySettings(fields, policy) : saved;
        return policy;
    }};
    // As setanchorbidpolicy changes the policy (without reset): the overrides it saves are the fields, normalized.
    const auto set{[](AnchorBidPolicy policy, UniValue& overrides, const std::string& json) {
        UniValue fields;
        BOOST_REQUIRE(fields.read(json));
        std::string error;
        BOOST_REQUIRE(ApplyPolicyFields(policy, fields, error));
        ReapplySavedStrategy(policy, overrides, fields);
        const UniValue said{PolicyToUniValue(policy)};
        UniValue kept{UniValue::VOBJ};
        for (const std::string& key : overrides.getKeys()) {
            if (fields.find_value(key).isNull() && key != "preventstall") kept.pushKV(key, overrides.find_value(key));
        }
        for (const std::string& key : fields.getKeys()) kept.pushKV(key, said.find_value(key));
        if (fields.find_value("preventstall").isNull()) kept.pushKV("preventstall", said.find_value("preventstall"));
        overrides = kept;
        return policy;
    }};
    ArgsManager none;
    args_of({}, none);
    UniValue overrides;
    // The floor alone: Fees + minimum, Prevent stalling on (as 8.3.5 bid at least its floor).
    AnchorBidPolicy p{start(none, R"({"floor":0.00001})", overrides)};
    BOOST_CHECK(p.strategy == BidStrategy::FEES_MINIMUM);
    BOOST_CHECK(p.prevent_stall);
    BOOST_CHECK(!IsLegacySettings(overrides));
    BOOST_CHECK_EQUAL(overrides.find_value("strategy").get_str(), "feespreventstall");
    // ... read back the same from what 8.3.6 keeps.
    UniValue again;
    BOOST_CHECK(start(none, overrides.write(), again) == p);
    // A floor of 0 in them: Fees.
    BOOST_CHECK(start(none, R"({"floor":0,"max":0.0001})", overrides).strategy == BidStrategy::FEES);
    // The options' strategy when they say one: minimal stays Minimum.
    ArgsManager minimal;
    args_of({"-validatoranchorbidstrategy=minimal"}, minimal);
    BOOST_CHECK(start(minimal, R"({"floor":0.00001})", overrides).strategy == BidStrategy::MINIMUM);
    // -validatoranchorbidmode=fees (8.3.5's fees): at least the floor, no stall prevention.
    ArgsManager mode;
    args_of({"-validatoranchorbidmode=fees"}, mode);
    p = start(mode, R"({"floor":0.00001})", overrides);
    BOOST_CHECK(p.strategy == BidStrategy::FEES_MINIMUM);
    BOOST_CHECK(!p.prevent_stall);
    // 8.3.5's off in its settings: the anchorer off, the merged miner still bidding.
    p = start(none, R"({"strategy":"off"})", overrides);
    BOOST_CHECK(p.Bids());
    BOOST_CHECK(!p.anchor);
    BOOST_CHECK(start(none, overrides.write(), again) == p);

    // The saved strategy is the old word, floor 0 (the app of 8.3.5): Fees. A floor set later: Fees + minimum at once, and after a restart.
    p = start(none, R"({"strategy":"feespreventstall","floor":0})", overrides);
    BOOST_CHECK(p.strategy == BidStrategy::FEES);
    p = set(p, overrides, R"({"floor":0.00001})");
    BOOST_CHECK(p.strategy == BidStrategy::FEES_MINIMUM);
    BOOST_CHECK(start(none, overrides.write(), again) == p);
    // And back to 0: Fees, both ways.
    p = set(p, overrides, R"({"floor":0})");
    BOOST_CHECK(p.strategy == BidStrategy::FEES);
    BOOST_CHECK(start(none, overrides.write(), again) == p);
    // A strategy said in the new words replaces it: no floor makes it anything else then.
    p = set(p, overrides, R"({"strategy":"fees"})");
    p = set(p, overrides, R"({"floor":0.00001})");
    BOOST_CHECK(p.strategy == BidStrategy::FEES);
    BOOST_CHECK(start(none, overrides.write(), again) == p);
    // 8.3.5's fees with a floor of 0, then a floor: Fees + minimum, without stall prevention, both ways.
    p = start(none, R"({"strategy":"fees","floor":0})", overrides);
    BOOST_CHECK(p.strategy == BidStrategy::FEES);
    BOOST_CHECK(!p.prevent_stall);
    p = set(p, overrides, R"({"floor":0.00001})");
    BOOST_CHECK(p.strategy == BidStrategy::FEES_MINIMUM);
    BOOST_CHECK(!p.prevent_stall);
    BOOST_CHECK(start(none, overrides.write(), again) == p);
}

BOOST_AUTO_TEST_CASE(fields_and_checks)
{
    AnchorBidPolicy policy;
    std::string error;
    UniValue conf{UniValue::VOBJ};
    conf.pushKV("strategy", "minimum");
    conf.pushKV("preventstall", false);
    conf.pushKV("anchor", "0");
    conf.pushKV("floor", "0.00001");
    conf.pushKV("curve", "0.6:1,0.8:3");
    conf.pushKV("interpolate", "0");
    conf.pushKV("feeshare", "100");
    conf.pushKV("periodcap", "0");
    BOOST_CHECK(ApplyPolicyFields(policy, conf, error));
    BOOST_CHECK(policy.strategy == BidStrategy::MINIMUM);
    BOOST_CHECK(!policy.prevent_stall);
    BOOST_CHECK(!policy.anchor);
    BOOST_CHECK_EQUAL(policy.floor, 1000);
    BOOST_CHECK(policy.curve == (std::vector<EscalationPoint>{{0.6, 1}, {0.8, 3}}));
    BOOST_CHECK(!policy.interpolate);
    BOOST_CHECK_EQUAL(policy.fee_share_bps, 10000);
    BOOST_CHECK_EQUAL(CheckPolicy(policy), "");
    // Said and read back: the same policy, whatever the policy before.
    for (const bool stall : {true, false}) {
        AnchorBidPolicy again{With(BidStrategy::OFF, stall)};
        BOOST_CHECK(ApplyPolicyFields(again, PolicyToUniValue(policy), error));
        BOOST_CHECK(again == policy);
    }
    AnchorBidPolicy fresh;
    fresh.floor = 5000;
    AnchorBidPolicy again;
    BOOST_CHECK(ApplyPolicyFields(again, PolicyToUniValue(fresh), error));
    BOOST_CHECK(again == fresh);

    const auto refused{[](const std::string& json) {
        AnchorBidPolicy p;
        UniValue fields;
        BOOST_REQUIRE(fields.read(json));
        std::string why;
        return !ApplyPolicyFields(p, fields, why) || !CheckPolicy(p).empty();
    }};
    BOOST_CHECK(!refused(R"({"floor":0,"max":0,"fixed":0})"));
    BOOST_CHECK(!refused(R"({"curve":[{"pressure":0.5,"multiplier":1},{"pressure":0.7,"multiplier":5}]})"));
    BOOST_CHECK(refused(R"({"floor":0.01,"max":0.001})"));
    BOOST_CHECK(refused(R"({"fixed":0.01})"));
    BOOST_CHECK(refused(R"({"max":21000001})"));
    BOOST_CHECK(refused(R"({"floor":-0.001})"));
    BOOST_CHECK(refused(R"({"escalationstart":0})"));
    BOOST_CHECK(refused(R"({"escalationstart":1.5})"));
    BOOST_CHECK(refused(R"({"curve":"0.8:2,0.6:3"})"));
    BOOST_CHECK(refused(R"({"curve":"0.5:3,0.6:2"})"));
    BOOST_CHECK(refused(R"({"curve":"0.5:0.5"})"));
    BOOST_CHECK(refused(R"({"curve":""})"));
    BOOST_CHECK(refused(R"({"feeshare":101})"));
    BOOST_CHECK(refused(R"({"strategy":"standard"})"));
    BOOST_CHECK(refused(R"({"preventstall":"maybe"})"));
    BOOST_CHECK(refused(R"({"periodhours":0})"));
    BOOST_CHECK(refused(R"({"bid":1})"));
}

BOOST_AUTO_TEST_CASE(included_rate_robust)
{
    // Only well-formed requests: OP_RETURN, a push of 68 bytes (00 bf 00, the slot, the commitment, the block before).
    const uint256 prev{uint256::FromHex("11" + std::string(62, '2')).value()};
    std::vector<unsigned char> script{0x6a, 0x44, 0x00, 0xbf, 0x00, 7};
    script.resize(38, 0x33);
    script.insert(script.end(), prev.begin(), prev.end());
    BOOST_CHECK(BmmRequestSlot(script, prev) == std::optional<uint8_t>{7});
    BOOST_CHECK(!BmmRequestSlot(script, uint256{}));
    auto shorter{script};
    shorter.pop_back();
    BOOST_CHECK(!BmmRequestSlot(shorter, prev));
    auto other{script};
    other[3] = 0xbe;
    BOOST_CHECK(!BmmRequestSlot(other, prev));
    // Active slots other than ours only, one value a slot (its lowest); a block counts with 3 such slots at least, and
    // gives one value (their median); the median of those over the window.
    const std::set<uint8_t> active{1, 2, 3, 4, 9};
    std::vector<std::vector<BmmRequestSeen>> blocks;
    for (int i{0}; i < 8; ++i) blocks.push_back({{1, 2000}, {2, 3000}, {3, 2500}});
    // Ours and inactive slots: nothing; fewer than 3 active slots: nothing.
    blocks.push_back({{7, 1}, {100, 1}, {101, 1}, {1, 1}});
    BOOST_CHECK_EQUAL(*RobustIncludedRate(blocks, active, uint8_t{7}), 2500);
    // One party fills a minority of the window (4 of 12 blocks) with requests at extreme rates: the median stays.
    for (int i{0}; i < 4; ++i) {
        std::vector<BmmRequestSeen> fake;
        for (int slot : {1, 2, 3, 4, 9}) for (int k{0}; k < 50; ++k) fake.push_back({static_cast<uint8_t>(slot), 900'000'000});
        blocks.push_back(fake);
    }
    BOOST_CHECK_EQUAL(*RobustIncludedRate(blocks, active, uint8_t{7}), 2500);
    std::vector<std::vector<BmmRequestSeen>> low{blocks.begin(), blocks.begin() + 8};
    for (int i{0}; i < 4; ++i) low.push_back({{1, 1}, {2, 1}, {3, 1}});
    BOOST_CHECK_EQUAL(*RobustIncludedRate(low, active, uint8_t{7}), 2500);
    BOOST_CHECK(!RobustIncludedRate({{{1, 1000}, {2, 1000}}}, active, std::nullopt));
    // Stuffing: 100 requests of 2 active slots at an extreme rate among 3 real slots: the block's value is the median of
    // its 5 slots' lowest, the real ones' median when the stuffed slots' lowest are high (or low).
    std::vector<BmmRequestSeen> stuffed{{1, 2000}, {2, 3000}, {3, 2500}};
    for (int k{0}; k < 100; ++k) stuffed.push_back({static_cast<uint8_t>(k % 2 ? 4 : 9), 900'000'000});
    BOOST_CHECK_EQUAL(*RobustIncludedRate({stuffed}, active, uint8_t{7}), 3000);
    std::vector<BmmRequestSeen> stuffed_low{{1, 2000}, {2, 3000}, {3, 2500}};
    for (int k{0}; k < 100; ++k) stuffed_low.push_back({static_cast<uint8_t>(k % 2 ? 4 : 9), 1});
    BOOST_CHECK_EQUAL(*RobustIncludedRate({stuffed_low}, active, uint8_t{7}), 2000);
}

// The node's one bid policy: the merged miner (setbmm) and a validator's anchorer bid the same, under the same max,
// period cap and stale rule (doc/fastblocks-anchor-bids.md, "One policy, two triggers").
BOOST_AUTO_TEST_CASE(node_bid_triggers)
{
    AnchorBidPolicy policy; // Fees, Prevent stalling, max 0.001, cap 0.01 a day
    BidInputs in{At(0, std::nullopt, 100'000)};
    BidDecision d{DecideNodeBid(policy, in, BidTriggers{.mine = true})};
    BOOST_CHECK(d.bid);
    BOOST_CHECK_EQUAL(d.amount, 99'000);
    BOOST_CHECK_EQUAL(d.trigger, "mine");
    BOOST_CHECK_EQUAL(DecideNodeBid(policy, in, BidTriggers{.anchor = true}).trigger, "anchor");
    BOOST_CHECK_EQUAL(DecideNodeBid(policy, in, BidTriggers{.anchor = true, .mine = true}).trigger, "anchor+mine");
    in.pot = 10'000'000;
    d = DecideNodeBid(policy, in, BidTriggers{.mine = true});
    BOOST_CHECK_EQUAL(d.amount, policy.max);
    BOOST_CHECK_EQUAL(d.kind, "max");
    // Fees that do not pay for a request: no bid (mining waits).
    in.pot = 400;
    BOOST_CHECK(!DecideNodeBid(policy, in, BidTriggers{.mine = true}).bid);
    BOOST_CHECK_EQUAL(FeeShare(123'457, 9900), sidechain::Follower::Offer(123'457));
    // Either trigger alone, or both: the policy's bid, for every strategy and stall setting.
    in.pot = 100'000;
    for (const BidStrategy strategy : {BidStrategy::FEES, BidStrategy::FEES_MINIMUM, BidStrategy::MINIMUM, BidStrategy::FIXED, BidStrategy::OFF}) {
        for (const bool stall : {false, true}) {
            AnchorBidPolicy p{With(strategy, stall)};
            p.fixed = 10'000;
            const BidDecision b{DecideBid(p, in)};
            for (const BidTriggers t : {BidTriggers{.anchor = true}, BidTriggers{.mine = true}, BidTriggers{.anchor = true, .mine = true}}) {
                const BidDecision a{DecideNodeBid(p, in, t)};
                BOOST_CHECK_EQUAL(a.bid, b.bid);
                BOOST_CHECK_EQUAL(a.amount, b.amount);
            }
        }
    }
    // setbmm's always (for tests): its amount every block, fees or not, at most max; the higher one with the anchorer.
    in.pot = 0;
    d = DecideNodeBid(policy, in, BidTriggers{.mine = true, .mine_always = true, .mine_amount = 10'000});
    BOOST_CHECK(d.bid);
    BOOST_CHECK_EQUAL(d.amount, 10'000);
    BOOST_CHECK_EQUAL(d.kind, "always");
    BOOST_CHECK_EQUAL(DecideNodeBid(policy, in, BidTriggers{.mine = true, .mine_always = true, .mine_amount = 5'000'000}).amount, policy.max);
    AnchorBidPolicy fixed{With(BidStrategy::FIXED)};
    fixed.fixed = 20'000;
    BOOST_CHECK_EQUAL(DecideNodeBid(fixed, in, BidTriggers{.anchor = true, .mine = true, .mine_always = true, .mine_amount = 10'000}).amount, 20'000);
    // No trigger: no bid. Off: no bid for either trigger, nor setbmm's always.
    BOOST_CHECK(!DecideNodeBid(policy, in, BidTriggers{}).bid);
    policy.strategy = BidStrategy::OFF;
    in.pot = 100'000;
    for (const BidTriggers t : {BidTriggers{.anchor = true}, BidTriggers{.mine = true}, BidTriggers{.mine = true, .mine_always = true, .mine_amount = 1000}}) {
        d = DecideNodeBid(policy, in, t);
        BOOST_CHECK(!d.bid);
        BOOST_CHECK_EQUAL(d.kind, "off");
    }
}

BOOST_AUTO_TEST_CASE(node_bid_period_cap)
{
    AnchorBidPolicy policy;
    policy.max = 50'000;
    policy.period_cap = 120'000;
    BidInputs in{At(0, std::nullopt, 100'000)};
    in.spent_in_period = 100'000;
    BidDecision d{DecideNodeBid(policy, in, BidTriggers{.mine = true})};
    BOOST_CHECK(d.bid);
    BOOST_CHECK_EQUAL(d.amount, 20'000);
    BOOST_CHECK_EQUAL(d.kind, "periodcap");
    in.spent_in_period = 120'000;
    BOOST_CHECK(!DecideNodeBid(policy, in, BidTriggers{.mine = true}).bid);
    BOOST_CHECK(!DecideNodeBid(policy, in, BidTriggers{.anchor = true}).bid);
    BOOST_CHECK(!DecideNodeBid(policy, in, BidTriggers{.mine = true, .mine_always = true, .mine_amount = 1000}).bid);
    policy.period_cap = 0;
    BOOST_CHECK(DecideNodeBid(policy, in, BidTriggers{.mine = true}).bid);
}

BOOST_AUTO_TEST_CASE(node_bid_stale_rule)
{
    AnchorBidPolicy policy{With(BidStrategy::FIXED, /*prevent_stall=*/false)};
    policy.fixed = 10'000;
    policy.max = 50'000;
    policy.period_cap = 0;
    BidInputs in{At(0)};
    // A bid of 10'000 did not land, and may still be held: the next one, on the same coin, must pay more.
    BidDecision d{DecideNodeBid(policy, in, BidTriggers{.anchor = true, .stale = 10'000})};
    BOOST_CHECK(d.bid);
    BOOST_CHECK_EQUAL(d.amount, 10'000 + REPLACEMENT_BUMP);
    BOOST_CHECK_EQUAL(d.kind, "replace");
    // A bid above it already: unchanged.
    d = DecideNodeBid(policy, in, BidTriggers{.anchor = true, .stale = 2000});
    BOOST_CHECK_EQUAL(d.amount, 10'000);
    // Never above max: the bid as it is, with the reason why.
    d = DecideNodeBid(policy, in, BidTriggers{.anchor = true, .stale = 49'800});
    BOOST_CHECK(d.bid);
    BOOST_CHECK_EQUAL(d.amount, 10'000);
    BOOST_CHECK(d.reason.find("did not land") != std::string::npos);
    // Nor above the cap's rest.
    policy.period_cap = 100'000;
    in.spent_in_period = 89'800;
    d = DecideNodeBid(policy, in, BidTriggers{.anchor = true, .stale = 10'000});
    BOOST_CHECK_EQUAL(d.amount, 10'000);

    // The stale bid: the highest that did not land since one last did, younger than the mempool expiry.
    const int64_t now{1'000'000'000};
    using S = PastBid::Status;
    std::vector<PastBid> bids{{S::MISSED, 7000, now - 100}, {S::LANDED, 9000, now - 90}, {S::MISSED, 3000, now - 80}, {S::OUTBID, 4000, now - 70}, {S::PENDING, 99'000, now - 1}};
    BOOST_CHECK_EQUAL(StaleOffer(bids, now), 4000);
    bids.push_back({S::LANDED, 1000, now});
    BOOST_CHECK_EQUAL(StaleOffer(bids, now), 0);
    // Expired: no longer held anywhere.
    BOOST_CHECK_EQUAL(StaleOffer(std::vector<PastBid>{{S::MISSED, 7000, now - STALE_BID_SECONDS - 1}}, now), 0);
    BOOST_CHECK_EQUAL(StaleOffer(std::vector<PastBid>{}, now), 0);
}

// The options: the old names and the new, -validatoranchorbidpreventstall, and regtest's defaults.
BOOST_AUTO_TEST_CASE(node_bid_options)
{
    const auto from{[](std::vector<const char*> argv, ChainType chain = ChainType::MAIN) {
        ArgsManager args;
        for (const char* name : {"-validatoranchorbid", "-validatoranchorbidstrategy", "-validatoranchorbidpreventstall", "-validatoranchorbidmode", "-validatoranchorbidmax",
                                 "-validatoranchorbidfixed", "-validatoranchorbidperiodcap", "-validatoranchorbidshare"}) {
            args.AddArg(name, "", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
        }
        argv.insert(argv.begin(), "x");
        std::string error;
        BOOST_REQUIRE(args.ParseParameters(argv.size(), argv.data(), error));
        return AnchorBidPolicyFromArgs(args, chain);
    }};
    // A fresh node: Fees, Prevent stalling, the 8.3.5 limits.
    AnchorBidPolicy p{from({})};
    BOOST_CHECK(p == AnchorBidPolicy{});
    // The live validators: minimal, a floor, a max.
    p = from({"-validatoranchorbidstrategy=minimal", "-validatoranchorbid=0.0001", "-validatoranchorbidmax=0.001"});
    BOOST_CHECK(p.strategy == BidStrategy::MINIMUM);
    BOOST_CHECK(p.prevent_stall);
    BOOST_CHECK_EQUAL(p.floor, 10'000);
    BOOST_CHECK_EQUAL(p.max, 100'000);
    BidInputs in{At(0, std::nullopt, 10'000'000)};
    BOOST_CHECK_EQUAL(DecideNodeBid(p, in, BidTriggers{.anchor = true}).amount, 10'000);
    BOOST_CHECK_EQUAL(DecideNodeBid(p, in, BidTriggers{.mine = true}).amount, 10'000);
    // The old names, the new names and the checkbox.
    // fees is the new Fees: Prevent stalling on unless -novalidatoranchorbidpreventstall.
    BOOST_CHECK(from({"-validatoranchorbidstrategy=fees"}).strategy == BidStrategy::FEES);
    BOOST_CHECK(from({"-validatoranchorbidstrategy=fees"}).prevent_stall);
    BOOST_CHECK(!from({"-validatoranchorbidstrategy=fees", "-novalidatoranchorbidpreventstall"}).prevent_stall);
    BOOST_CHECK(from({"-validatoranchorbidstrategy=feespreventstall"}).prevent_stall);
    BOOST_CHECK(from({"-validatoranchorbidstrategy=feespreventstall"}).strategy == BidStrategy::FEES);
    BOOST_CHECK(from({"-validatoranchorbidstrategy=fixed", "-validatoranchorbid=0.00001"}).prevent_stall);
    BOOST_CHECK(from({"-validatoranchorbidstrategy=minimum"}).prevent_stall);
    BOOST_CHECK(!from({"-validatoranchorbidstrategy=minimum", "-validatoranchorbidpreventstall=0"}).prevent_stall);
    BOOST_CHECK(!from({"-validatoranchorbidstrategy=minimum", "-novalidatoranchorbidpreventstall"}).prevent_stall);
    BOOST_CHECK(from({"-validatoranchorbidstrategy=fees", "-validatoranchorbidpreventstall"}).prevent_stall);
    BOOST_CHECK(from({"-validatoranchorbidstrategy=feesminimum"}).strategy == BidStrategy::FEES_MINIMUM);
    // off in the options is 8.3.5's: the anchorer off alone, the merged miner still bids.
    p = from({"-validatoranchorbidstrategy=off"});
    BOOST_CHECK(p.Bids());
    BOOST_CHECK(!p.anchor);
    BOOST_CHECK(p.strategy == BidStrategy::FEES);
    // A floor alone, the old default feespreventstall: at least the floor, Fees + minimum; a floor of 0: Fees.
    p = from({"-validatoranchorbid=0.00001"});
    BOOST_CHECK(p.strategy == BidStrategy::FEES_MINIMUM);
    BOOST_CHECK(p.prevent_stall);
    BOOST_CHECK(from({"-validatoranchorbid=0"}).strategy == BidStrategy::FEES);
    BOOST_CHECK(from({"-validatoranchorbidmode=standard"}).strategy == BidStrategy::FEES);
    // 8.3.5's fees mode: the pot whole, at least the floor, no stall prevention.
    p = from({"-validatoranchorbidmode=fees", "-validatoranchorbid=0.00001"});
    BOOST_CHECK(p.strategy == BidStrategy::FEES_MINIMUM);
    BOOST_CHECK(!p.prevent_stall);
    BOOST_CHECK_EQUAL(p.fee_share_bps, 10000);
    // The anchorer off: "Anchor my validator" off; the merged miner bids all the same.
    for (const char* off : {"-novalidatoranchorbid", "-validatoranchorbid=off"}) {
        p = from({off});
        BOOST_CHECK(!p.anchor);
        BOOST_CHECK(p.Bids());
    }
    // Regtest without options: no anchorer, no stall prevention, no cap; setbmm bids the fee share as before.
    const AnchorBidPolicy reg{from({}, ChainType::REGTEST)};
    BOOST_CHECK(reg.strategy == BidStrategy::FEES);
    BOOST_CHECK(!reg.anchor);
    BOOST_CHECK(!reg.prevent_stall);
    BOOST_CHECK_EQUAL(reg.period_cap, 0);
    BidInputs fees{At(0, std::nullopt, 100'000)};
    BOOST_CHECK_EQUAL(DecideNodeBid(reg, fees, BidTriggers{.mine = true}).amount, sidechain::Follower::Offer(100'000));
    BOOST_CHECK(from({"-validatoranchorbid=0"}, ChainType::REGTEST).anchor);
}

BOOST_AUTO_TEST_SUITE_END()
