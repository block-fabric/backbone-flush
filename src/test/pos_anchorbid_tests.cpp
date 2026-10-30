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
    return in;
}

AnchorBidPolicy With(BidStrategy strategy)
{
    AnchorBidPolicy policy;
    policy.strategy = strategy;
    // The curve's escalation (the tests below it); the default's flat boost is flat_boost's own test.
    policy.flat_boost = false;
    policy.request_vsize = 190; // Legacy curve coverage.
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
    BOOST_CHECK_EQUAL(policy.floor, 0);
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
    AnchorBidPolicy policy;
    policy.fee_share_bps = 10000;
    const auto bid{[&](double p, CAmount pot, int missed = 0) {
        BidInputs in{At(p, 1410, pot)};
        in.missed = missed;
        return DecideBid(policy, in);
    }};
    // ceil(1.41 sat/vB) x 189 vB = 378 sat. Pending work requires a bid even without fees.
    BOOST_CHECK_EQUAL(bid(0, 0).amount, 378);
    BOOST_CHECK_EQUAL(bid(0, 300).amount, 378);
    BOOST_CHECK_EQUAL(bid(0, 900).amount, 900);
    BOOST_CHECK_EQUAL(bid(0.6, 0).amount, 1512);
    BOOST_CHECK_EQUAL(bid(0.95, 0).amount, 1512);
    BOOST_CHECK_EQUAL(bid(0.6, 900).amount, 1512);
    BOOST_CHECK_EQUAL(bid(0.6, 5000).amount, 5000);
    BOOST_CHECK_EQUAL(bid(0.6, 5000).kind, "fees");
    BOOST_CHECK_EQUAL(bid(0.6, 0).kind, "boost");
    BOOST_CHECK_EQUAL(bid(0.4, 0, 9).amount, 4 * 378); // missed blocks raise the base (x4 at most)
    BOOST_CHECK_EQUAL(bid(0.6, 0, 9).amount, 1512);  // the boost: 4 x the base before, not more
    policy.max = 1000;
    BOOST_CHECK_EQUAL(bid(0.6, 0).amount, 1000);
    BOOST_CHECK_EQUAL(bid(0, 5000).amount, 1000);
    policy.max = 100000;
    policy.floor = 20000;
    BOOST_CHECK_EQUAL(bid(0.95, 0).amount, 80000);
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

BOOST_AUTO_TEST_CASE(idle_and_relay_minimum)
{
    AnchorBidPolicy policy;
    BidInputs in{At(0.1, 1410)};
    in.pending_actions = false;
    const auto quiet{DecideBid(policy, in)};
    BOOST_CHECK(!quiet.bid);
    BOOST_CHECK_EQUAL(quiet.kind, "idle");
    BOOST_CHECK_EQUAL(quiet.reason, "skipped: nothing to anchor");
    in.pending_actions = true; // deposits, withdrawals, refunds, staking payouts or epoch maintenance
    BOOST_CHECK(DecideBid(policy, in).bid);
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 378);
    in.pending_actions = false;
    in.pot = 100;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 378);
    in.pot = 5000;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 4950);
    in.pot = 0;
    in.slot = 5500; // threshold: boost even without pending actions
    BOOST_CHECK(DecideBid(policy, in).bid);
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 1512);
    in.relay_rate_per_kvb = 5000;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 5000); // the relay floor for a request of 250 vB
    in.fee_rate_per_kvb.reset();
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 5000);
    in.relay_rate_per_kvb = 1001; // relay amount rounds upward, not to the nearest satoshi
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 1004);
    in.fee_rate_per_kvb = 2000; // exact whole sat/vB is not rounded again
    in.relay_rate_per_kvb = 1000;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 1512);
    policy.use_fee_estimate = false;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 1000);
    policy.strategy = BidStrategy::FIXED;
    policy.fixed = 123;
    in.slot = 5100;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 123);
    policy.strategy = BidStrategy::OFF;
    BOOST_CHECK(!DecideBid(policy, in).bid);
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

BOOST_AUTO_TEST_CASE(competitive_base)
{
    // 2026-10-26: through the enforcer's interface no estimate reached the node and it bid the relay floor (189 sat), which
    // waited while other slots' requests of 2-3 sat/vB got in. The base: the estimate, else what blocks took for a BMM
    // request, rounded up, never below the network's least (2 sat/vB on eCash); missed blocks raise the base itself.
    AnchorBidPolicy policy;
    BidInputs in{At(0)};
    in.pending_actions = true;
    in.min_rate_per_kvb = 2000;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 2 * 189);
    in.fee_rate_per_kvb = 1410; // 1.41 sat/vB: rounded up to 2
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 2 * 189);
    in.fee_rate_per_kvb = 2500;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 3 * 189);
    in.fee_rate_per_kvb.reset();
    in.included_rate_per_kvb = 2900; // what the last block took
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 3 * 189);
    // Missed: x2 at 2, x4 at 4 and more (the boost's factor at most).
    in.missed = 2;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 2 * 3 * 189);
    in.missed = 7;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 4 * 3 * 189);
    in.slot = in.first_unanchored_slot + 900; // under pressure: the boost of the base before, the same
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 4 * 3 * 189);
    in.slot = in.first_unanchored_slot;
    // Nothing pending, no fees, no pressure: no bid; a fee share or the pressure: a bid.
    in.missed = 0;
    in.pending_actions = false;
    BOOST_CHECK(!DecideBid(policy, in).bid);
    in.pot = 1000;
    BOOST_CHECK(DecideBid(policy, in).bid);
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
    // In the bid, the real bounds: clamped to [the network's least, 10 x it], never above the max.
    AnchorBidPolicy policy;
    BidInputs in{At(0)};
    in.pending_actions = true;
    in.min_rate_per_kvb = 2000;
    in.included_rate_per_kvb = 900'000'000;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 20 * 189);
    in.included_rate_per_kvb = 1;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 2 * 189);
    policy.max = 1000;
    in.included_rate_per_kvb = 900'000'000;
    BOOST_CHECK_EQUAL(DecideBid(policy, in).amount, 1000);
}

// The node's one bid policy: the merged miner (setbmm) and a validator's anchorer under the same max, period cap
// and stale rule (doc/fastblocks-anchor-bids.md, "Triggers").
BOOST_AUTO_TEST_CASE(node_bid_mine_trigger)
{
    AnchorBidPolicy policy; // feespreventstall, max 0.001, cap 0.01 a day
    BidInputs in{At(0, std::nullopt, 100'000)};
    in.relay_rate_per_kvb = 1000;
    // The fee share, 99 % of the fees.
    BidDecision d{DecideNodeBid(policy, in, BidTriggers{.mine = true})};
    BOOST_CHECK(d.bid);
    BOOST_CHECK_EQUAL(d.amount, 99'000);
    BOOST_CHECK_EQUAL(d.trigger, "mine");
    // At most max per bid: min(99 % of the fees, max).
    in.pot = 10'000'000;
    d = DecideNodeBid(policy, in, BidTriggers{.mine = true});
    BOOST_CHECK(d.bid);
    BOOST_CHECK_EQUAL(d.amount, policy.max);
    BOOST_CHECK_EQUAL(d.kind, "max");
    // Fees that do not pay for a request: no bid (mining waits), whatever the strategy.
    in.pot = 400;
    d = DecideNodeBid(policy, in, BidTriggers{.mine = true});
    BOOST_CHECK(!d.bid);
    BOOST_CHECK_EQUAL(d.kind, "idle");
    // A strategy off stops the anchorer, not setbmm: the fee share (regtest's default, setbmm as before).
    policy.strategy = BidStrategy::OFF;
    in.pot = 100'000;
    d = DecideNodeBid(policy, in, BidTriggers{.mine = true});
    BOOST_CHECK(d.bid);
    BOOST_CHECK_EQUAL(d.amount, 99'000);
    // ... and exactly Follower::Offer, as setbmm offered before.
    BOOST_CHECK_EQUAL(FeeShare(123'457, 9900), sidechain::Follower::Offer(123'457));
    // A fixed offer when the fees pay for a block.
    policy.strategy = BidStrategy::FIXED;
    policy.fixed = 20'000;
    d = DecideNodeBid(policy, in, BidTriggers{.mine = true});
    BOOST_CHECK_EQUAL(d.amount, 20'000);
    in.pot = 0;
    BOOST_CHECK(!DecideNodeBid(policy, in, BidTriggers{.mine = true}).bid);
    // setbmm's always (for tests): its amount every block, fees or not, but at most max.
    d = DecideNodeBid(policy, in, BidTriggers{.mine = true, .mine_always = true, .mine_amount = 10'000});
    BOOST_CHECK(d.bid);
    BOOST_CHECK_EQUAL(d.amount, 10'000);
    BOOST_CHECK_EQUAL(d.kind, "always");
    d = DecideNodeBid(policy, in, BidTriggers{.mine = true, .mine_always = true, .mine_amount = 5'000'000});
    BOOST_CHECK_EQUAL(d.amount, policy.max);
    // No trigger: no bid.
    BOOST_CHECK(!DecideNodeBid(policy, in, BidTriggers{}).bid);
}

BOOST_AUTO_TEST_CASE(node_bid_period_cap)
{
    AnchorBidPolicy policy;
    policy.max = 50'000;
    policy.period_cap = 120'000;
    BidInputs in{At(0, std::nullopt, 100'000)};
    // Spent 100'000 of 120'000: cut to what is left.
    in.spent_in_period = 100'000;
    BidDecision d{DecideNodeBid(policy, in, BidTriggers{.mine = true})};
    BOOST_CHECK(d.bid);
    BOOST_CHECK_EQUAL(d.amount, 20'000);
    BOOST_CHECK_EQUAL(d.kind, "periodcap");
    // Spent: nothing, for either trigger, and for "always" too.
    in.spent_in_period = 120'000;
    BOOST_CHECK(!DecideNodeBid(policy, in, BidTriggers{.mine = true}).bid);
    BOOST_CHECK(!DecideNodeBid(policy, in, BidTriggers{.anchor = true}).bid);
    BOOST_CHECK(!DecideNodeBid(policy, in, BidTriggers{.mine = true, .mine_always = true, .mine_amount = 1000}).bid);
    // A cap of 0: none.
    policy.period_cap = 0;
    BOOST_CHECK(DecideNodeBid(policy, in, BidTriggers{.mine = true}).bid);
}

BOOST_AUTO_TEST_CASE(node_bid_both_triggers)
{
    AnchorBidPolicy policy{With(BidStrategy::MINIMAL)}; // floor 500
    policy.max = 100'000;
    BidInputs in{At(0, std::nullopt, 100'000)};
    // One request serves both: the higher of the two bids.
    BidDecision d{DecideNodeBid(policy, in, BidTriggers{.anchor = true, .mine = true})};
    BOOST_CHECK(d.bid);
    BOOST_CHECK_EQUAL(d.trigger, "anchor+mine");
    BOOST_CHECK_EQUAL(d.amount, DecideBid(policy, in).amount);
    // The anchorer alone is DecideBid, unchanged (existing configs keep their meaning).
    for (const BidStrategy strategy : {BidStrategy::FIXED, BidStrategy::MINIMAL, BidStrategy::FEES, BidStrategy::FEES_PREVENT_STALL, BidStrategy::OFF}) {
        AnchorBidPolicy p{policy};
        p.strategy = strategy;
        p.fixed = 10'000;
        const BidDecision a{DecideNodeBid(p, in, BidTriggers{.anchor = true})};
        const BidDecision b{DecideBid(p, in)};
        BOOST_CHECK_EQUAL(a.bid, b.bid);
        BOOST_CHECK_EQUAL(a.amount, b.amount);
    }
}

BOOST_AUTO_TEST_CASE(node_bid_stale_rule)
{
    AnchorBidPolicy policy{With(BidStrategy::FIXED)};
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

// The options keep their meaning on the one policy: the validators' minimal/0.0001/0.001, and regtest's defaults.
BOOST_AUTO_TEST_CASE(node_bid_options_compat)
{
    ArgsManager args;
    args.AddArg("-validatoranchorbid", "", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    args.AddArg("-validatoranchorbidstrategy", "", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    args.AddArg("-validatoranchorbidmax", "", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    args.AddArg("-validatoranchorbidperiodcap", "", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    {
        const char* argv[]{"x", "-validatoranchorbidstrategy=minimal", "-validatoranchorbid=0.0001", "-validatoranchorbidmax=0.001"};
        std::string error;
        BOOST_REQUIRE(args.ParseParameters(4, argv, error));
        const AnchorBidPolicy p{AnchorBidPolicyFromArgs(args, ChainType::MAIN)};
        BOOST_CHECK(p.strategy == BidStrategy::MINIMAL);
        BOOST_CHECK_EQUAL(p.floor, 10'000);
        BOOST_CHECK_EQUAL(p.max, 100'000);
        BOOST_CHECK_EQUAL(p.period_cap, DEFAULT_ANCHOR_BID_PERIOD_CAP);
        // The anchorer bids as before; the merged miner of the same node under the same floor and max.
        BidInputs in{At(0, std::nullopt, 10'000'000)};
        BOOST_CHECK_EQUAL(DecideNodeBid(p, in, BidTriggers{.anchor = true}).amount, DecideBid(p, in).amount);
        BOOST_CHECK_EQUAL(DecideNodeBid(p, in, BidTriggers{.mine = true}).amount, 10'000);
    }
    {
        const char* argv[]{"x"};
        std::string error;
        BOOST_REQUIRE(args.ParseParameters(1, argv, error));
        // Mainnet: the defaults cap the merged miner too.
        const AnchorBidPolicy main{AnchorBidPolicyFromArgs(args, ChainType::MAIN)};
        BOOST_CHECK_EQUAL(main.max, DEFAULT_ANCHOR_BID_MAX);
        BOOST_CHECK_EQUAL(main.period_cap, DEFAULT_ANCHOR_BID_PERIOD_CAP);
        // Regtest: the anchorer off, no period cap (setbmm in tests as before).
        const AnchorBidPolicy reg{AnchorBidPolicyFromArgs(args, ChainType::REGTEST)};
        BOOST_CHECK(reg.strategy == BidStrategy::OFF);
        BOOST_CHECK_EQUAL(reg.period_cap, 0);
        BidInputs in{At(0, std::nullopt, 100'000)};
        BOOST_CHECK_EQUAL(DecideNodeBid(reg, in, BidTriggers{.mine = true}).amount, sidechain::Follower::Offer(100'000));
        BOOST_CHECK(!DecideNodeBid(reg, in, BidTriggers{.anchor = true}).bid);
    }
}

BOOST_AUTO_TEST_SUITE_END()
