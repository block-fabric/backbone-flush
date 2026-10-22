// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// The DEX's pool kinds (bitassets/curves.h): the exact arithmetic of the curves, property by property, at
// the extremes, and against vectors other implementations can check (data/dex_curves.json; the reference
// in Python, contrib/dex-curves/reference.py, computes the same).

#include <arith_uint256.h>
#include <bitassets/curves.h>
#include <bitassets/dex.h>
#include <bitassets/state.h>
#include <test/data/dex_curves.json.h>
#include <test/util/json.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <util/fs.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace bitassets;
using namespace bitassets::curves;

namespace {

PoolParams Stable(uint32_t amp, uint32_t fee = 500, uint8_t scale0 = 0, uint8_t scale1 = 0)
{
    PoolParams p;
    p.curve = static_cast<uint8_t>(Curve::STABLE);
    p.fee = fee;
    p.amp = amp;
    p.scale0 = scale0;
    p.scale1 = scale1;
    return p;
}
PoolParams Weighted(uint8_t weight0, uint32_t fee = 3000)
{
    PoolParams p;
    p.curve = static_cast<uint8_t>(Curve::WEIGHTED);
    p.fee = fee;
    p.weight0 = weight0;
    return p;
}
PoolParams Product(uint32_t fee)
{
    PoolParams p;
    p.fee = fee;
    return p;
}

/** Every kind the tests go through. */
std::vector<PoolParams> Kinds()
{
    std::vector<PoolParams> kinds;
    for (uint32_t fee : FEE_TIERS) kinds.push_back(Product(fee));
    for (uint32_t amp : {1U, 10U, 100U, 2000U, 10000U}) kinds.push_back(Stable(amp, 500));
    kinds.push_back(Stable(100, 100, 6, 0));
    kinds.push_back(Stable(100, 100, 0, 12));
    for (uint8_t w : WEIGHTS) kinds.push_back(Weighted(w, w % 2 ? 10000 : 3000));
    return kinds;
}

std::string Describe(const PoolParams& p)
{
    return strprintf("curve=%d fee=%u amp=%u w0=%u scales=%u/%u", p.curve, p.fee, p.amp, p.weight0, p.scale0, p.scale1);
}

uint64_t RandAmount(FastRandomContext& rng, uint64_t most)
{
    // Spread over magnitudes: from a few units to `most`.
    const int bits{1 + static_cast<int>(rng.randrange(63))};
    const uint64_t cap{bits >= 63 ? most : std::min<uint64_t>(most, (uint64_t{1} << bits))};
    return 1 + rng.randrange(cap);
}

/** The invariant of the pool at its reserves holds at the reserves after: it never falls. */
void CheckNeverFalls(const PoolParams& p, uint64_t r0, uint64_t r1, uint64_t a0, uint64_t a1)
{
    if (p.GetCurve() == Curve::STABLE) {
        // D is irrational in general: compare floor(2^k·D) of both points (D is homogeneous), 2^-k of a unit apart.
        unsigned __int128 s0{1}, s1{1};
        for (int i{0}; i < p.scale0; ++i) s0 *= 10;
        for (int i{0}; i < p.scale1; ++i) s1 *= 10;
        const int k{p.scale0 + p.scale1 > 0 ? 8 : 20};
        const auto before{StableD(p.amp, (r0 * s0) << k, (r1 * s1) << k).first}, after{StableD(p.amp, (a0 * s0) << k, (a1 * s1) << k).first};
        BOOST_CHECK_MESSAGE(after >= before, Describe(p) << strprintf(" from %u/%u to %u/%u", r0, r1, a0, a1));
        return;
    }
    const Level level{p, r0, r1};
    BOOST_CHECK_MESSAGE(level.Holds(Rat{a0}, Rat{a1}), Describe(p) << strprintf(" from %u/%u to %u/%u", r0, r1, a0, a1));
}

/** Reserves of at most about 2^bits of the smallest unit (the scales of a stable pool counted). */
std::pair<uint64_t, uint64_t> RandReserves(FastRandomContext& rng, const PoolParams& p, int bits)
{
    uint64_t d0{1}, d1{1};
    for (int i{0}; i < p.scale0; ++i) d0 *= 10;
    for (int i{0}; i < p.scale1; ++i) d1 *= 10;
    const uint64_t most{(uint64_t{1} << bits)};
    return {std::max<uint64_t>(2, RandAmount(rng, most) / d0), std::max<uint64_t>(2, RandAmount(rng, most) / d1)};
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(dex_curves_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(biguint_arithmetic)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    for (int i{0}; i < 2000; ++i) {
        const uint64_t a{rng.rand64()}, b{rng.rand64()};
        const unsigned __int128 product{static_cast<unsigned __int128>(a) * b};
        BOOST_CHECK(*(BigUint{a} * BigUint{b}).ToU128() == product);
        BOOST_CHECK(*(BigUint::U128(product) / BigUint{b == 0 ? 1 : b}).ToU128() == product / (b == 0 ? 1 : b));
        const unsigned __int128 sum{static_cast<unsigned __int128>(a) + b};
        BOOST_CHECK(*(BigUint{a} + BigUint{b}).ToU128() == sum);
        BOOST_CHECK(*(BigUint::U128(sum) - BigUint{b}).ToU128() == a);
        BOOST_CHECK((BigUint{a} <=> BigUint{b}) == (a <=> b));
        // Wide: (a·b)^3 / (a·b) = (a·b)^2, (x << k) >> as a division.
        if (a > 0 && b > 0) {
            const BigUint ab{BigUint{a} * BigUint{b}};
            BOOST_CHECK(ab.Pow(3) / ab == ab * ab);
            BOOST_CHECK((ab << 77) / (BigUint{1} << 77) == ab);
        }
    }
    BOOST_CHECK(BigUint{2}.Pow(255).ToUint256() == ArithToUint256(arith_uint256{1} << 255));
    BOOST_CHECK(BigUint::FromArith(arith_uint256{1} << 200) == BigUint{1} << 200);
}

BOOST_AUTO_TEST_CASE(kinds_and_ids)
{
    // The default kind is the pool AddLiquidity makes, by the id it always had.
    const AssetId a{uint256::ONE}, b{uint256{2}};
    BOOST_CHECK(PoolIdOf(a, b, PoolParams{}) == PoolId(a, b));
    BOOST_CHECK(PoolIdOf(b, a, Product(500)) == PoolIdOf(a, b, Product(500)));
    BOOST_CHECK(PoolIdOf(a, b, Product(500)) != PoolId(a, b));
    BOOST_CHECK(PoolIdOf(a, b, Stable(100)) != PoolIdOf(a, b, Stable(200)));
    BOOST_CHECK(PoolIdOf(a, b, Weighted(80)) != PoolIdOf(a, b, Weighted(20)));
    // Scales follow the assets: not part of the id.
    BOOST_CHECK(PoolIdOf(a, b, Stable(100, 500, 0, 6)) == PoolIdOf(a, b, Stable(100, 500, 0, 0)));
    BOOST_CHECK(!WhyNotPoolKind(0, 100, 0, 0, 0));
    BOOST_CHECK(WhyNotPoolKind(0, 200, 0, 0, 0));
    BOOST_CHECK(WhyNotPoolKind(1, 500, 0, 0, 0));
    BOOST_CHECK(WhyNotPoolKind(1, 500, 10001, 0, 0));
    BOOST_CHECK(!WhyNotPoolKind(1, 500, 10000, 0, 0));
    BOOST_CHECK(WhyNotPoolKind(2, 3000, 0, 50, 0));
    BOOST_CHECK(!WhyNotPoolKind(2, 3000, 0, 98, 0));
    BOOST_CHECK(!WhyNotPoolKind(2, 3000, 0, 80, POOL_FLAG_DYNAMIC_FEE));
    BOOST_CHECK(WhyNotPoolKind(2, 3000, 0, 80, 2));
    BOOST_CHECK(!WhyNotPoolKind(4, 500, 0, 0, 0));
    BOOST_CHECK(WhyNotPoolKind(4, 500, 5, 0, 0));
    BOOST_CHECK(WhyNotPoolKind(3, 3000, 0, 0, 0));
    BOOST_CHECK(WhyNotPoolKind(5, 3000, 0, 0, 0));
    // The weight named for asset_a becomes asset0's.
    CreatePool create;
    create.asset_a = b;
    create.asset_b = a;
    create.curve = static_cast<uint8_t>(Curve::WEIGHTED);
    create.fee = 3000;
    create.weight_a = 80;
    BOOST_CHECK_EQUAL(MakePoolParams(create, 2, 8).weight0, 20);
    create.curve = static_cast<uint8_t>(Curve::STABLE);
    create.weight_a = 0;
    create.amp = 50;
    const PoolParams stable{MakePoolParams(create, 2, 8)};
    BOOST_CHECK_EQUAL(stable.scale0, 6);
    BOOST_CHECK_EQUAL(stable.scale1, 0);
}

BOOST_AUTO_TEST_CASE(constant_product_is_clearpool)
{
    // The default kind clears exactly as the DEX always did; every fee tier by the same formula.
    FastRandomContext rng{/*fDeterministic=*/true};
    for (int i{0}; i < 3000; ++i) {
        const uint64_t r0{RandAmount(rng, MAX_AMOUNT / 2)}, r1{RandAmount(rng, MAX_AMOUNT / 2)};
        std::vector<dex::HopIn> orders;
        for (int k{0}, n{1 + static_cast<int>(rng.randrange(5))}; k < n; ++k) orders.push_back({rng.randbool(), RandAmount(rng, MAX_AMOUNT / 16)});
        const auto legacy{dex::ClearPool(r0, r1, orders)};
        const auto now{ClearPoolOf(PoolParams{}, LEGACY_FEE, r0, r1, orders)};
        BOOST_REQUIRE(legacy.has_value() == now.has_value());
        if (legacy) {
            BOOST_CHECK(legacy->outs == now->outs);
            BOOST_CHECK_EQUAL(legacy->reserve0, now->reserve0);
            BOOST_CHECK_EQUAL(legacy->reserve1, now->reserve1);
        }
        // One order alone is amm::SwapOut's.
        const uint64_t a{RandAmount(rng, MAX_AMOUNT / 4)};
        BOOST_CHECK_EQUAL(SwapOutOf(PoolParams{}, LEGACY_FEE, r0, r1, true, a), amm::SwapOut(r0, r1, a));
    }
}

BOOST_AUTO_TEST_CASE(stable_d)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    for (uint32_t amp : {1U, 2U, 50U, 1000U, 10000U}) {
        // Balanced: D = X + Y exactly.
        for (uint64_t x : {uint64_t{1}, uint64_t{1000}, uint64_t{123456789}, MAX_AMOUNT}) {
            const auto [lo, hi]{StableD(amp, x, x)};
            BOOST_CHECK(lo == static_cast<unsigned __int128>(x) * 2);
            BOOST_CHECK(hi == lo);
        }
        for (int i{0}; i < 200; ++i) {
            const unsigned __int128 x{RandAmount(rng, MAX_AMOUNT)}, y{RandAmount(rng, MAX_AMOUNT)};
            const auto [lo, hi]{StableD(amp, x, y)};
            // Between the constant product's 2·sqrt(xy) and the constant sum's x + y.
            BOOST_CHECK(lo <= x + y);
            const BigUint d{BigUint::U128(lo + 1)};
            BOOST_CHECK(d * d >= BigUint{4} * BigUint::U128(x) * BigUint::U128(y));
            BOOST_CHECK(hi == lo || hi == lo + 1);
            // The floor is on or under the curve, the next above (Level::Holds says the same with D = ceil).
        }
    }
}

BOOST_AUTO_TEST_CASE(clearing_properties)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    int checked{0};
    for (const PoolParams& p : Kinds()) {
        for (int i{0}; i < 60; ++i) {
            const auto [r0, r1]{RandReserves(rng, p, 61)};
            std::vector<dex::HopIn> orders;
            const int n{1 + static_cast<int>(rng.randrange(6))};
            for (int k{0}; k < n; ++k) orders.push_back({rng.randbool(), RandAmount(rng, std::max<uint64_t>(1, (rng.randbool() ? r0 : r1) / 3))});
            const auto cleared{ClearPoolOf(p, p.fee, r0, r1, orders)};
            if (!cleared) continue;
            ++checked;
            // Conservation: what went in, less what came out, is what the pool holds.
            unsigned __int128 in0{0}, in1{0}, out0{0}, out1{0};
            for (size_t k{0}; k < orders.size(); ++k) {
                (orders[k].zero_in ? in0 : in1) += orders[k].amount;
                (orders[k].zero_in ? out1 : out0) += cleared->outs[k];
            }
            BOOST_CHECK(cleared->reserve0 == r0 + in0 - out0);
            BOOST_CHECK(cleared->reserve1 == r1 + in1 - out1);
            BOOST_CHECK(cleared->reserve0 > 0 && cleared->reserve1 > 0);
            BOOST_CHECK_EQUAL(cleared->taken0 + cleared->taken1, 0U);
            // The invariant never falls (the fees and the rounding raise it).
            CheckNeverFalls(p, r0, r1, cleared->reserve0, cleared->reserve1);
            // One price: of two orders of a side, the larger never gets less, and out/in is the same up to rounding.
            for (size_t j{0}; j < orders.size(); ++j) {
                for (size_t k{0}; k < orders.size(); ++k) {
                    if (orders[j].zero_in != orders[k].zero_in || orders[j].amount > orders[k].amount) continue;
                    BOOST_CHECK(cleared->outs[j] <= cleared->outs[k]);
                    // out_j/in_j and out_k/in_k: floors of the same price times the amounts.
                    const BigUint lhs{BigUint{cleared->outs[j]} * BigUint{orders[k].amount}}, rhs{BigUint{cleared->outs[k] + 1} * BigUint{orders[j].amount}};
                    BOOST_CHECK(lhs < rhs);
                }
            }
            // Deterministic: the same again.
            const auto again{ClearPoolOf(p, p.fee, r0, r1, orders)};
            BOOST_CHECK(again->outs == cleared->outs);
        }
    }
    BOOST_CHECK_GT(checked, 500);
}

BOOST_AUTO_TEST_CASE(one_order_is_the_curve)
{
    // One order alone gets the most the curve gives for it: one unit more and the pool would lose.
    FastRandomContext rng{/*fDeterministic=*/true};
    for (const PoolParams& p : Kinds()) {
        if (p.GetCurve() == Curve::CONSTANT_PRODUCT) continue;
        for (int i{0}; i < 30; ++i) {
            const auto [r0, r1]{RandReserves(rng, p, 61)};
            const bool zero_in{rng.randbool()};
            const uint64_t a{RandAmount(rng, std::max<uint64_t>(1, (zero_in ? r0 : r1) / 2))};
            const uint64_t out{SwapOutOf(p, p.fee, r0, r1, zero_in, a)};
            const Level level{p, r0, r1};
            // The fee-free input as a rational: a·(F − fee)/F.
            const BigUint f{FEE_DENOMINATOR};
            Rat in_side{BigUint{zero_in ? r0 : r1} * f + BigUint{a} * BigUint{FEE_DENOMINATOR - p.fee}, f};
            const uint64_t reserve_out{zero_in ? r1 : r0};
            if (out + 1 < reserve_out) {
                const Rat out_side{reserve_out - out - 1};
                BOOST_CHECK_MESSAGE(!(zero_in ? level.Holds(in_side, out_side) : level.Holds(out_side, in_side)), Describe(p));
            }
            // (Holds compares with the ceiling of a stable pool's D: the point a trade of nothing leaves may be under it.)
            if (out > 0) {
                const Rat at_out{reserve_out - out};
                BOOST_CHECK(zero_in ? level.Holds(in_side, at_out) : level.Holds(at_out, in_side));
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(monotone)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    for (const PoolParams& p : Kinds()) {
        const uint64_t r0{1 + rng.randrange(uint64_t{1} << 50)}, r1{1 + rng.randrange(uint64_t{1} << 50)};
        uint64_t last{0};
        for (uint64_t a{1}; a < (uint64_t{1} << 52); a = a * 3 + rng.randrange(7)) {
            const uint64_t out{SwapOutOf(p, p.fee, r0, r1, true, a)};
            BOOST_CHECK_MESSAGE(out >= last, Describe(p));
            last = out;
        }
        // A higher fee never gives more.
        const uint64_t a{1 + rng.randrange(r0)};
        uint64_t prev{MAX_AMOUNT};
        for (uint32_t fee : FEE_TIERS) {
            const uint64_t out{SwapOutOf(p, fee, r0, r1, false, a)};
            BOOST_CHECK(out <= prev);
            prev = out;
        }
    }
    // More amplification: closer to the constant sum at balance (more out for the same in).
    uint64_t prev{0};
    for (uint32_t amp : {1U, 10U, 100U, 1000U, 10000U}) {
        const uint64_t out{SwapOutOf(Stable(amp, 100), 100, 1'000'000'000, 1'000'000'000, true, 100'000'000)};
        BOOST_CHECK(out >= prev);
        prev = out;
    }
    BOOST_CHECK_GT(prev, 99'000'000U);
}

BOOST_AUTO_TEST_CASE(extremes)
{
    // The largest reserves and orders: no overflow, the invariant holds, nothing out of the pool's reach.
    for (const PoolParams& p : Kinds()) {
        for (const auto& [r0, r1] : std::vector<std::pair<uint64_t, uint64_t>>{{MAX_AMOUNT / 2, MAX_AMOUNT / 2}, {1, MAX_AMOUNT / 2}, {MAX_AMOUNT / 2, 1}, {1, 1}, {2, 3}}) {
            for (const uint64_t a : {uint64_t{1}, uint64_t{1000}, MAX_AMOUNT / 2}) {
                for (const bool zero_in : {true, false}) {
                    const auto cleared{ClearPoolOf(p, p.fee, r0, r1, {{zero_in, a}, {!zero_in, a / 3 + 1}})};
                    if (!cleared) continue;
                    BOOST_CHECK(cleared->reserve0 > 0 && cleared->reserve1 > 0);
                    CheckNeverFalls(p, r0, r1, cleared->reserve0, cleared->reserve1);
                }
            }
        }
        // Over MAX_AMOUNT: refused, as ClearPool refuses.
        BOOST_CHECK(!ClearPoolOf(p, p.fee, MAX_AMOUNT, 10, {{true, 1}}));
        BOOST_CHECK(!ClearPoolOf(p, p.fee, 0, 10, {{true, 1}}));
    }
}

BOOST_AUTO_TEST_CASE(liquidity)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    for (const PoolParams& p : Kinds()) {
        auto [r0, r1]{RandReserves(rng, p, 40)};
        r0 += 1'000'000;
        r1 += 1'000'000;
        const auto opened{OpeningShares(p, r0, r1)};
        BOOST_REQUIRE(opened);
        if (p.GetCurve() == Curve::CONSTANT_PRODUCT) {
            // isqrt(x·y), as AddLiquidity opens a pool.
            const BigUint s{*opened}, s1{*opened + 1}, k{BigUint{r0} * BigUint{r1}};
            BOOST_CHECK(s * s <= k && s1 * s1 > k);
        }
        Pool pool;
        pool.reserve0 = r0;
        pool.reserve1 = r1;
        pool.shares = *opened;
        // A deposit at the pool's proportions gets what the shares are worth, as AddLiquidity's.
        const uint64_t t{1 + rng.randrange(1000)};
        const uint64_t d0{r0 / 1000 * t}, d1{r1 / 1000 * t};
        if (d0 > 0 && d1 > 0) {
            const auto shares{DepositShares(p, p.fee, pool, d0, d1)};
            BOOST_REQUIRE(shares);
            // At least what the side in proportion gives (less a unit of D's ceiling), at most what the larger side would.
            const uint64_t s0{static_cast<uint64_t>(static_cast<unsigned __int128>(d0) * pool.shares / r0)}, s1{static_cast<uint64_t>(static_cast<unsigned __int128>(d1) * pool.shares / r1)};
            BOOST_CHECK_MESSAGE(*shares <= std::max(s0, s1) && *shares + 2 + std::min(s0, s1) / 1'000'000 >= std::min(s0, s1), Describe(p) << strprintf(" %u vs %u..%u", *shares, s0, s1));
        }
        // One side alone: fewer shares than half the value at the spot price would give; a round trip
        // (deposit one side, withdraw that side) never gives back more than went in.
        for (const uint8_t side : {uint8_t{1}, uint8_t{2}}) {
            const uint64_t amount{(side == 1 ? r0 : r1) / 20 + 1};
            const auto shares{DepositShares(p, p.fee, pool, side == 1 ? amount : 0, side == 2 ? amount : 0)};
            BOOST_REQUIRE_MESSAGE(shares, Describe(p));
            Pool after{pool};
            after.reserve0 += side == 1 ? amount : 0;
            after.reserve1 += side == 2 ? amount : 0;
            after.shares += *shares;
            const auto [b0, b1]{WithdrawOf(p, p.fee, after, *shares, side)};
            BOOST_CHECK_MESSAGE((side == 1 ? b0 : b1) <= amount, Describe(p) << strprintf(" side %u: in %u back %u", side, amount, side == 1 ? b0 : b1));
            BOOST_CHECK_EQUAL(side == 1 ? b1 : b0, 0U);
            // The LPs who stay never lose: the pool after the round trip holds its level per share.
            const uint64_t left0{after.reserve0 - b0}, left1{after.reserve1 - b1};
            const Level level{p, r0, r1};
            BOOST_CHECK(level.Holds(Rat{left0}, Rat{left1}));
        }
        // Withdrawing both sides: the shares' part of each reserve, down.
        const auto [w0, w1]{WithdrawOf(p, p.fee, pool, pool.shares / 3, 0)};
        BOOST_CHECK(w0 == amm::Withdraw(pool, pool.shares / 3).first && w1 == amm::Withdraw(pool, pool.shares / 3).second);
    }
}

BOOST_AUTO_TEST_CASE(spot_prices)
{
    // Constant product: the reserves' ratio, as the accumulators always added.
    BOOST_CHECK(SpotPriceQ64(PoolParams{}, 1000, 3000) == dex::PriceQ64(3000, 1000));
    BOOST_CHECK(SpotPriceQ64(PoolParams{}, 1000, 3000, true) == dex::PriceQ64(1000, 3000));
    // Weighted 80/20 at 1000/1000: an asset0 weighs four times: 4 of asset1 for one.
    BOOST_CHECK(SpotPriceQ64(Weighted(80), 1000, 1000) == ArithToUint256(arith_uint256{4} << 64));
    BOOST_CHECK(SpotPriceQ64(Weighted(80), 1000, 1000, true) == ArithToUint256(arith_uint256{1} << 62));
    // Stable at balance: exactly 1; scaled by decimals (6 more decimals for asset1: 10^6 units of it for one of asset0).
    BOOST_CHECK(SpotPriceQ64(Stable(100), 5000, 5000) == ArithToUint256(arith_uint256{1} << 64));
    BOOST_CHECK(SpotPriceQ64(Stable(100, 500, 6, 0), 5000, 5'000'000'000) == ArithToUint256(arith_uint256{1'000'000} << 64));
    // Off balance, a stable pool's price stays near 1 (amplified), and the swap's average price is between the prices before and after.
    // More of asset1 than of asset0: asset0 is dearer, a little.
    const double p{SpotPrice(Stable(1000), 1'000'000, 1'500'000)};
    BOOST_CHECK(p > 1.0 && p < 1.01);
    for (const PoolParams& kind : Kinds()) {
        const uint64_t r0{1'000'000'000}, r1{3'000'000'000};
        const uint64_t a{10'000'000};
        const uint64_t out{SwapOutOf(kind, 0 + kind.fee, r0, r1, true, a)};
        const auto after{ClearPoolOf(kind, kind.fee, r0, r1, {{true, a}})};
        const double before_p{SpotPrice(kind, r0, r1)}, after_p{SpotPrice(kind, after->reserve0, after->reserve1)};
        const double avg{static_cast<double>(out) / a};
        BOOST_CHECK_MESSAGE(avg <= before_p * 1.000001 && after_p <= before_p, Describe(kind));
        // The two prices are each other's inverse, up to the floors.
        const double inv{UintToArith256(SpotPriceQ64(kind, r0, r1, true)).getdouble() / 18446744073709551616.0};
        BOOST_CHECK_CLOSE(before_p * inv, 1.0, 1e-6);
    }
}

BOOST_AUTO_TEST_CASE(speed)
{
    // The work of a round of a pool: the bound a block's clearing has to stay within (doc/layers.md).
    const auto start{std::chrono::steady_clock::now()};
    int rounds{0};
    for (const PoolParams& p : {Stable(2000, 500, 0, 6), Weighted(98), Weighted(60)}) {
        for (int i{0}; i < 20; ++i) {
            ClearPoolOf(p, p.fee, MAX_AMOUNT / 7, MAX_AMOUNT / 9, {{true, MAX_AMOUNT / 100 + i}, {false, MAX_AMOUNT / 90}, {true, 12345}});
            ++rounds;
        }
    }
    const auto micros{std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()};
    BOOST_TEST_MESSAGE("two-sided rounds: " << micros / rounds << " us each");
    BOOST_CHECK_LT(micros / rounds, 20'000);
}

BOOST_AUTO_TEST_CASE(worst_case_round)
{
    // The slowest round of a pool whose curve is searched, over adversarial kinds, reserves and orders: the
    // bound of a block's clearing is (MAX_CLEARING_PASSES + 1) · MAX_BLOCK_CURVE_WORK of these (doc/layers.md).
    std::vector<PoolParams> kinds{Stable(1, 100), Stable(10000, 100), Stable(5000, 10000, 12, 0), Stable(1, 10000, 0, 12), Weighted(98, 100), Weighted(2, 10000), Weighted(60, 100), Weighted(40, 10000)};
    double worst{0};
    for (const PoolParams& p : kinds) {
        uint64_t s0{1}, s1{1};
        for (int i{0}; i < p.scale0; ++i) s0 *= 10;
        for (int i{0}; i < p.scale1; ++i) s1 *= 10;
        for (const auto& [r0, r1] : std::vector<std::pair<uint64_t, uint64_t>>{{MAX_AMOUNT / 3 / s0, MAX_AMOUNT / 3 / s1}, {MAX_AMOUNT / 3 / s0, 7}, {11, MAX_AMOUNT / 3 / s1}, {123456789, 987654321}}) {
            for (const auto& orders : std::vector<std::vector<dex::HopIn>>{{{true, MAX_AMOUNT / 4}, {false, MAX_AMOUNT / 5}}, {{true, 1}, {false, 1}}, {{true, r0 / 2 + 1}, {false, r1 / 3 + 1}, {true, 17}},
                                                                          {{false, MAX_AMOUNT / 3}}}) {
                const auto start{std::chrono::steady_clock::now()};
                ClearPoolOf(p, p.fee, r0, r1, orders);
                worst = std::max(worst, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
            }
        }
    }
    const double block_ms{worst * (MAX_CLEARING_PASSES + 1) * MAX_BLOCK_CURVE_WORK / 1000};
    BOOST_TEST_MESSAGE("worst round: " << worst << " us; a block's clearing at most about " << block_ms << " ms");
    // Well under a slot (15 s), on this machine at its slowest (nice 19, sanitizers off).
    BOOST_CHECK_LT(block_ms, 5000);
}

/** The vectors: each case's inputs and what the rules give, which other implementations check. */
UniValue Case(const PoolParams& p, uint64_t r0, uint64_t r1, const std::vector<dex::HopIn>& orders)
{
    UniValue c(UniValue::VOBJ);
    c.pushKV("curve", p.curve);
    c.pushKV("fee", p.fee);
    c.pushKV("amp", p.amp);
    c.pushKV("weight0", p.weight0);
    c.pushKV("scale0", p.scale0);
    c.pushKV("scale1", p.scale1);
    c.pushKV("reserve0", strprintf("%u", r0));
    c.pushKV("reserve1", strprintf("%u", r1));
    UniValue ords(UniValue::VARR);
    for (const auto& o : orders) {
        UniValue e(UniValue::VARR);
        e.push_back(o.zero_in ? 0 : 1);
        e.push_back(strprintf("%u", o.amount));
        ords.push_back(e);
    }
    c.pushKV("orders", ords);
    const auto cleared{ClearPoolOf(p, p.fee, r0, r1, orders)};
    UniValue result(UniValue::VOBJ);
    if (cleared) {
        UniValue outs(UniValue::VARR);
        for (uint64_t o : cleared->outs) outs.push_back(strprintf("%u", o));
        result.pushKV("outs", outs);
        result.pushKV("reserve0", strprintf("%u", cleared->reserve0));
        result.pushKV("reserve1", strprintf("%u", cleared->reserve1));
    }
    c.pushKV("clearing", result);
    const auto opened{OpeningShares(p, r0, r1)};
    c.pushKV("opening_shares", opened ? strprintf("%u", *opened) : "");
    c.pushKV("spot_price_q64", UintToArith256(SpotPriceQ64(p, r0, r1)).GetHex());
    c.pushKV("spot_price_inverse_q64", UintToArith256(SpotPriceQ64(p, r0, r1, true)).GetHex());
    if (opened) {
        Pool pool;
        pool.reserve0 = r0;
        pool.reserve1 = r1;
        pool.shares = *opened;
        const uint64_t d0{orders[0].amount}, d1{orders.size() > 1 ? orders[1].amount / 2 : 0};
        const auto shares{DepositShares(p, p.fee, pool, d0, d1)};
        c.pushKV("deposit", strprintf("%u %u", d0, d1));
        c.pushKV("deposit_shares", shares ? strprintf("%u", *shares) : "");
        const auto [w0, w1]{WithdrawOf(p, p.fee, pool, *opened / 5, 2)};
        c.pushKV("withdraw_only1", strprintf("%u %u", w0, w1));
    }
    return c;
}

std::vector<std::tuple<PoolParams, uint64_t, uint64_t, std::vector<dex::HopIn>>> VectorInputs()
{
    FastRandomContext rng{/*fDeterministic=*/true};
    std::vector<std::tuple<PoolParams, uint64_t, uint64_t, std::vector<dex::HopIn>>> inputs;
    for (const PoolParams& p : Kinds()) {
        for (int i{0}; i < 6; ++i) {
            const auto [r0, r1]{RandReserves(rng, p, 60)};
            std::vector<dex::HopIn> orders;
            for (int k{0}, n{1 + static_cast<int>(rng.randrange(4))}; k < n; ++k) orders.push_back({rng.randbool(), RandAmount(rng, std::max<uint64_t>(2, std::min(r0, r1) / 4))});
            inputs.emplace_back(p, r0, r1, orders);
        }
    }
    return inputs;
}

BOOST_AUTO_TEST_CASE(vectors)
{
    UniValue cases(UniValue::VARR);
    for (const auto& [p, r0, r1, orders] : VectorInputs()) cases.push_back(Case(p, r0, r1, orders));
    // DEX_CURVES_VECTORS=<file>: write them (contrib/dex-curves/README.md), to commit as test/data/dex_curves.json.
    if (const char* out{std::getenv("DEX_CURVES_VECTORS")}) {
        std::ofstream file{out};
        file << cases.write(1) << "\n";
    }
    const UniValue committed{read_json(json_tests::dex_curves)};
    BOOST_REQUIRE_EQUAL(committed.size(), cases.size());
    for (size_t i{0}; i < cases.size(); ++i) BOOST_CHECK_MESSAGE(committed[i].write() == cases[i].write(), "vector " << i << ": " << cases[i].write());
}

BOOST_AUTO_TEST_SUITE_END()
