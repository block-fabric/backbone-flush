// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/anchorbid.h>

#include <sidechain/follower.h>
#include <univalue.h>

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
    return in;
}

AnchorBidPolicy With(BidStrategy strategy)
{
    AnchorBidPolicy policy;
    policy.strategy = strategy;
    // The curve's escalation (the tests below it); the default's flat boost is flat_boost's own test.
    policy.flat_boost = false;
    policy.floor = 500;
    policy.fixed = 500;
    return policy;
}

/** The policy of before the flat boost (a floor of 500, the curve), which the tests of the curve were written for. */
AnchorBidPolicy Old() { return With(BidStrategy::FEES_PREVENT_STALL); }

bool Close(double a, double b) { return std::abs(a - b) < 1e-9; }
} // namespace

BOOST_AUTO_TEST_SUITE(pos_anchorbid_tests)

BOOST_AUTO_TEST_CASE(defaults)
{
    const AnchorBidPolicy policy;
    BOOST_CHECK(policy.strategy == BidStrategy::FEES_PREVENT_STALL);
    BOOST_CHECK_EQUAL(policy.floor, 564);
    BOOST_CHECK(policy.flat_boost);
    BOOST_CHECK(Close(policy.boost, 4));
    BOOST_CHECK_EQUAL(policy.max, 100'000);
    BOOST_CHECK_EQUAL(policy.fee_share_bps, 9900);
    BOOST_CHECK(Close(policy.escalation_start, 0.5));
    BOOST_CHECK_EQUAL(CheckPolicy(policy), "");
    for (const auto s : {BidStrategy::FIXED, BidStrategy::MINIMAL, BidStrategy::FEES, BidStrategy::FEES_PREVENT_STALL, BidStrategy::OFF}) {
        BOOST_CHECK(ParseStrategy(StrategyName(s)) == s);
    }
    BOOST_CHECK(!ParseStrategy("standard"));
}

BOOST_AUTO_TEST_CASE(flat_boost)
{
    // "Fees + prevent stalling", the default (user decision 2026-10-24): max(564, fee share); under pressure max(4 x 564,
    // fee share); never a multiple of the fee share, of the estimate or of a rebid; the max still caps.
    AnchorBidPolicy policy;
    policy.fee_share_bps = 10000; // the fee share is the pot
    const auto bid{[&](double p, CAmount pot, int missed = 0) {
        BidInputs in{At(p, 50'000, pot)};
        in.missed = missed;
        return DecideBid(policy, in);
    }};
    // No fees: exactly 564.
    BOOST_CHECK_EQUAL(bid(0, 0).amount, 564);
    // Fees below 564: still 564 (the fees count toward it).
    BOOST_CHECK_EQUAL(bid(0, 300).amount, 564);
    // Fees above: the fee share.
    BOOST_CHECK_EQUAL(bid(0, 900).amount, 900);
    BOOST_CHECK_EQUAL(bid(0, 50'000).amount, 50'000);
    // The boost: 4 x 564 = 2256, flat, with no fees or fees below it; the fee share when more (not 4 x it).
    BOOST_CHECK_EQUAL(bid(0.6, 0).amount, 2256);
    BOOST_CHECK_EQUAL(bid(0.95, 0).amount, 2256);
    BOOST_CHECK_EQUAL(bid(0.6, 900).amount, 2256);
    BOOST_CHECK_EQUAL(bid(0.6, 5'000).amount, 5'000);
    BOOST_CHECK_EQUAL(bid(0.6, 5'000).kind, "fees");
    BOOST_CHECK_EQUAL(bid(0.6, 0).kind, "boost");
    // Below the pressure threshold, no boost; missed blocks do not multiply it either.
    BOOST_CHECK_EQUAL(bid(0.4, 0).amount, 564);
    BOOST_CHECK_EQUAL(bid(0.4, 0, 9).amount, 564);
    BOOST_CHECK_EQUAL(bid(0.6, 0, 9).amount, 2256);
    // The max caps it.
    policy.max = 1'000;
    BOOST_CHECK_EQUAL(bid(0.6, 0).amount, 1'000);
    BOOST_CHECK_EQUAL(bid(0, 5'000).amount, 1'000);
    // The PC's case (2026-10-24): a floor of 0.0002 boosted by the curve to 0.0008 is now 4 x the floor at most, never more.
    policy.max = 100'000;
    policy.floor = 20'000;
    BOOST_CHECK_EQUAL(bid(0.95, 0).amount, 80'000);
    policy.floor = 564;
    BOOST_CHECK_EQUAL(bid(0.95, 0).amount, 2256);
    // The policy's fields: flatboost and boost, set and printed.
    AnchorBidPolicy set;
    std::string error;
    UniValue fields(UniValue::VOBJ);
    fields.pushKV("flatboost", false);
    fields.pushKV("boost", 3);
    BOOST_CHECK(ApplyPolicyFields(set, fields, error));
    BOOST_CHECK(!set.flat_boost);
    BOOST_CHECK(Close(set.boost, 3));
    BOOST_CHECK_EQUAL(PolicyToUniValue(set).find_value("boost").get_real(), 3);
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

BOOST_AUTO_TEST_CASE(fixed_and_off)
{
    AnchorBidPolicy policy{With(BidStrategy::FIXED)};
    policy.fixed = 1234;
    for (double p : {0.0, 0.6, 0.95}) {
        const auto d{DecideBid(policy, At(p, 50'000))};
        BOOST_CHECK(d.bid);
        BOOST_CHECK_EQUAL(d.amount, 1234);
        BOOST_CHECK_EQUAL(d.kind, "fixed");
    }
    const auto off{DecideBid(With(BidStrategy::OFF), At(0.9, 50'000, 1'000'000))};
    BOOST_CHECK(!off.bid);
    BOOST_CHECK_EQUAL(off.kind, "off");
}

BOOST_AUTO_TEST_CASE(below_the_threshold_the_mempool_does_not_count)
{
    // A congested mainchain (50 sat/vB): the floor all the same until p reaches 0.5.
    const AnchorBidPolicy policy{With(BidStrategy::MINIMAL)};
    for (double p : {0.0, 0.2, 0.49}) {
        const auto d{DecideBid(policy, At(p, 50'000))};
        BOOST_CHECK_EQUAL(d.amount, 500);
        BOOST_CHECK_EQUAL(d.kind, "floor");
    }
    // At 0.5, x1 the estimate for 190 vB: 9500; at 0.75, x2; at 0.9, x4 (19000, 38000).
    BOOST_CHECK_EQUAL(DecideBid(policy, At(0.5, 50'000)).amount, 9500);
    BOOST_CHECK_EQUAL(DecideBid(policy, At(0.75, 50'000)).amount, 19'000);
    const auto high{DecideBid(policy, At(0.9, 50'000))};
    BOOST_CHECK_EQUAL(high.amount, 38'000);
    BOOST_CHECK_EQUAL(high.kind, "escalation");
    BOOST_CHECK(Close(high.multiplier, 4));
    // Never above max.
    BOOST_CHECK_EQUAL(DecideBid(policy, At(0.95, 1'000'000)).amount, policy.max);
    BOOST_CHECK_EQUAL(DecideBid(policy, At(0.95, 1'000'000)).kind, "max");
    // An anchor lands: the run is over, the floor again.
    BidInputs landed{At(0.95, 1'000'000)};
    landed.first_unanchored_slot = -1;
    BOOST_CHECK_EQUAL(DecideBid(policy, landed).amount, 500);
}

BOOST_AUTO_TEST_CASE(the_incident)
{
    // eCash at ~1 sat/vB, a run at p = 0.92: the floor (500) is over 190 vB x 1 sat/vB x 4, the bid 760.
    const AnchorBidPolicy policy{Old()};
    BOOST_CHECK_EQUAL(DecideBid(policy, At(0.3, 1000)).amount, 500);
    BOOST_CHECK_EQUAL(DecideBid(policy, At(0.92, 1000)).amount, 760);
    // Without an estimate: the floor times the curve.
    BOOST_CHECK_EQUAL(DecideBid(policy, At(0.92)).amount, 2256);
    // A floor of 0 escalates from 500 sat.
    AnchorBidPolicy zero{Old()};
    zero.floor = 0;
    const auto quiet{DecideBid(zero, At(0.1))};
    BOOST_CHECK(quiet.bid);
    BOOST_CHECK_EQUAL(quiet.amount, 0);
    BOOST_CHECK_EQUAL(DecideBid(zero, At(0.92)).amount, 2256);
    // Not with the estimate off.
    zero.use_fee_estimate = false;
    BOOST_CHECK_EQUAL(DecideBid(zero, At(0.92, 50'000)).amount, 2256);
}

BOOST_AUTO_TEST_CASE(fee_share)
{
    // 99 %: what Follower::Offer bids, the operator's hundredth kept.
    const AnchorBidPolicy fees{With(BidStrategy::FEES)};
    for (CAmount pot : {CAmount{0}, CAmount{99}, CAmount{501}, CAmount{12'345}, CAmount{99'999}, CAmount{100'000}}) {
        const auto d{DecideBid(fees, At(0, std::nullopt, pot))};
        BOOST_CHECK_EQUAL(d.normal, std::max<CAmount>(sidechain::Follower::Offer(pot), 500));
    }
    const auto share{DecideBid(fees, At(0.95, 1'000'000, 50'000))};
    BOOST_CHECK_EQUAL(share.amount, 49'500);
    BOOST_CHECK_EQUAL(share.kind, "fees");
    // Fees alone never escalates; fees + prevent stalling does, only when needed.
    BOOST_CHECK_EQUAL(DecideBid(fees, At(0.95, 1'000'000)).amount, 500);
    const AnchorBidPolicy both{Old()};
    BOOST_CHECK_EQUAL(DecideBid(both, At(0.2, 1'000'000, 50'000)).amount, 49'500);
    BOOST_CHECK_EQUAL(DecideBid(both, At(0.95, 100'000, 50'000)).amount, 76'000);
    AnchorBidPolicy whole{fees};
    whole.fee_share_bps = 10000;
    BOOST_CHECK_EQUAL(DecideBid(whole, At(0, std::nullopt, 12'345)).amount, 12'345);
}

BOOST_AUTO_TEST_CASE(rebid_and_caps)
{
    const AnchorBidPolicy policy{With(BidStrategy::MINIMAL)};
    BidInputs in{At(0)};
    in.missed = 2;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 500);
    in.missed = 3;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 1000);
    BOOST_CHECK_EQUAL(DecideBid(policy, in).kind, "rebid");
    in.missed = 6;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 2000);
    in.missed = 300;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 4000);
    // The period's cap: cut to what is left, none once spent.
    in.missed = 0;
    in.spent_in_period = policy.period_cap - 200;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 200);
    BOOST_CHECK_EQUAL(DecideBid(policy, in).kind, "periodcap");
    in.spent_in_period = policy.period_cap;
    BOOST_CHECK(!DecideBid(policy, in).bid);
}

BOOST_AUTO_TEST_CASE(fields_and_checks)
{
    AnchorBidPolicy policy;
    std::string error;
    UniValue conf{UniValue::VOBJ};
    conf.pushKV("strategy", "minimal");
    conf.pushKV("floor", "0.00001");
    conf.pushKV("curve", "0.6:1,0.8:3");
    conf.pushKV("interpolate", "0");
    conf.pushKV("feeshare", "100");
    conf.pushKV("periodcap", "0");
    BOOST_CHECK(ApplyPolicyFields(policy, conf, error));
    BOOST_CHECK(policy.strategy == BidStrategy::MINIMAL);
    BOOST_CHECK_EQUAL(policy.floor, 1000);
    BOOST_CHECK(policy.curve == (std::vector<EscalationPoint>{{0.6, 1}, {0.8, 3}}));
    BOOST_CHECK(!policy.interpolate);
    BOOST_CHECK_EQUAL(policy.fee_share_bps, 10000);
    BOOST_CHECK_EQUAL(CheckPolicy(policy), "");
    // Said and read back: the same policy.
    AnchorBidPolicy again{With(BidStrategy::OFF)};
    BOOST_CHECK(ApplyPolicyFields(again, PolicyToUniValue(policy), error));
    BOOST_CHECK(again == policy);

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
    BOOST_CHECK(refused(R"({"periodhours":0})"));
    BOOST_CHECK(refused(R"({"bid":1})"));
}

BOOST_AUTO_TEST_SUITE_END()
