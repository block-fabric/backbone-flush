// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitassets/clmath.h>

#include <uint256.h>

#include <algorithm>
#include <array>
#include <string>

namespace bitassets::cl {

using curves::BigUint;

namespace {
arith_uint256 FromHex(const std::string& hex)
{
    return UintToArith256(*Assert(uint256::FromHex(std::string(64 - hex.size(), '0') + hex)));
}

BigUint Big(const arith_uint256& value) { return BigUint::FromArith(value); }
BigUint Big128(unsigned __int128 value) { return BigUint::U128(value); }

BigUint CeilDiv(const BigUint& a, const BigUint& b)
{
    const BigUint q{a / b};
    return q * b == a ? q : q + BigUint{1};
}

/** A BigUint that fits in 256 bits, as an arith_uint256 (the caller knows it fits). */
arith_uint256 Arith(const BigUint& value) { return UintToArith256(value.ToUint256()); }

std::optional<uint64_t> Fits64(const BigUint& value)
{
    const auto v{value.ToU128()};
    if (!v || *v > MAX_AMOUNT) return std::nullopt;
    return static_cast<uint64_t>(*v);
}

// TickMath constants: 2^128·1.0001^(−2^(k−1)) rounded, for k = 0..19 (the first is 1.0001^(−1/2)).
const std::array<arith_uint256, 20>& Constants()
{
    static const std::array<arith_uint256, 20> c{
        FromHex("fffcb933bd6fad37aa2d162d1a594001"), FromHex("fff97272373d413259a46990580e213a"), FromHex("fff2e50f5f656932ef12357cf3c7fdcc"),
        FromHex("ffe5caca7e10e4e61c3624eaa0941cd0"), FromHex("ffcb9843d60f6159c9db58835c926644"), FromHex("ff973b41fa98c081472e6896dfb254c0"),
        FromHex("ff2ea16466c96a3843ec78b326b52861"), FromHex("fe5dee046a99a2a811c461f1969c3053"), FromHex("fcbe86c7900a88aedcffc83b479aa3a4"),
        FromHex("f987a7253ac413176f2b074cf7815e54"), FromHex("f3392b0822b70005940c7a398e4b70f3"), FromHex("e7159475a2c29b7443b29c7fa6e889d9"),
        FromHex("d097f3bdfd2022b8845ad8f792aa5825"), FromHex("a9f746462d870fdf8a65dc1f90e061e5"), FromHex("70d869a156d2a1b890bb3df62baf32f7"),
        FromHex("31be135f97d08fd981231505542fcfa6"), FromHex("9aa508b5b7a84e1c677de54f3e99bc9"), FromHex("5d6af8dedb81196699c329225ee604"),
        FromHex("2216e584f5fa1ea926041bedfe98"), FromHex("48a170391f7dc42444e8fa2"),
    };
    return c;
}
} // namespace

const arith_uint256& MinSqrtRatio()
{
    static const arith_uint256 v{4295128739};
    return v;
}
const arith_uint256& MaxSqrtRatio()
{
    static const arith_uint256 v{FromHex("fffd8963efd1fc6a506488495d951d5263988d26")};
    return v;
}

arith_uint256 SqrtRatioAtTick(int32_t tick)
{
    const uint32_t abs_tick{static_cast<uint32_t>(tick < 0 ? -static_cast<int64_t>(tick) : tick)};
    const auto& c{Constants()};
    arith_uint256 ratio{(abs_tick & 1) ? c[0] : arith_uint256{arith_uint256{1} << 128}};
    for (int k{1}; k < 20; ++k) {
        if (abs_tick & (uint32_t{1} << k)) ratio = arith_uint256{(ratio * c[k]) >> 128};
    }
    if (tick > 0) ratio = (~arith_uint256{}) / ratio;
    // Down to Q64.96, rounded up: the price at a tick is never below the exact one by the rounding.
    const arith_uint256 low{ratio - ((ratio >> 32) << 32)};
    return arith_uint256{(ratio >> 32) + arith_uint256{low == 0 ? 0U : 1U}};
}

int32_t TickAtSqrtRatio(const arith_uint256& sqrt_price)
{
    // The largest tick t with SqrtRatioAtTick(t) ≤ sqrt_price: monotone, by bisection.
    int32_t lo{MIN_TICK}, hi{MAX_TICK};
    while (lo < hi) {
        const int32_t mid{static_cast<int32_t>(lo + (static_cast<int64_t>(hi) - lo + 1) / 2)};
        if (SqrtRatioAtTick(mid) <= sqrt_price) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

BigUint Amount0Delta(const arith_uint256& sqrt_a, const arith_uint256& sqrt_b, unsigned __int128 liquidity, bool round_up)
{
    const arith_uint256 a{std::min(sqrt_a, sqrt_b)}, b{std::max(sqrt_a, sqrt_b)};
    if (a == 0 || a == b || liquidity == 0) return {};
    const BigUint num{(Big128(liquidity) << 96) * Big(b - a)};
    // Uniswap's divRoundingUp(mulDivRoundingUp(L << 96, b − a, b), a), or both down.
    return round_up ? CeilDiv(CeilDiv(num, Big(b)), Big(a)) : (num / Big(b)) / Big(a);
}

BigUint Amount1Delta(const arith_uint256& sqrt_a, const arith_uint256& sqrt_b, unsigned __int128 liquidity, bool round_up)
{
    const arith_uint256 a{std::min(sqrt_a, sqrt_b)}, b{std::max(sqrt_a, sqrt_b)};
    if (liquidity == 0 || a == b) return {};
    const BigUint num{Big128(liquidity) * Big(b - a)};
    const BigUint q96{BigUint{1} << 96};
    return round_up ? CeilDiv(num, q96) : num / q96;
}

std::optional<arith_uint256> NextSqrtPriceFromInput(const arith_uint256& sqrt_price, unsigned __int128 liquidity, uint64_t amount, bool zero_in)
{
    if (sqrt_price == 0 || liquidity == 0) return std::nullopt;
    if (amount == 0) return sqrt_price;
    if (zero_in) {
        // Asset0 in, the price down, rounded up: ⌈L·2^96·p / (L·2^96 + amount·p)⌉.
        const BigUint n1{Big128(liquidity) << 96};
        const BigUint p{Big(sqrt_price)};
        return Arith(CeilDiv(n1 * p, n1 + BigUint{amount} * p));
    }
    // Asset1 in, the price up, rounded down: p + ⌊amount·2^96 / L⌋.
    const BigUint next{Big(sqrt_price) + (BigUint{amount} << 96) / Big128(liquidity)};
    if (next.Bits() > 160) return std::nullopt;
    return Arith(next);
}

Step SwapStep(const arith_uint256& sqrt_current, const arith_uint256& sqrt_target, unsigned __int128 liquidity, uint64_t amount_remaining, bool zero_in)
{
    Step step;
    step.sqrt_next = sqrt_current;
    if (liquidity == 0 || amount_remaining == 0) {
        // Nothing provides here: the price moves to the target, nothing traded.
        step.sqrt_next = liquidity == 0 ? sqrt_target : sqrt_current;
        return step;
    }
    // What reaching the target takes.
    const BigUint to_target{zero_in ? Amount0Delta(sqrt_target, sqrt_current, liquidity, true) : Amount1Delta(sqrt_current, sqrt_target, liquidity, true)};
    const bool reaches{to_target <= BigUint{amount_remaining}};
    if (reaches) {
        step.sqrt_next = sqrt_target;
    } else {
        step.sqrt_next = NextSqrtPriceFromInput(sqrt_current, liquidity, amount_remaining, zero_in).value_or(sqrt_target);
        // Never past the target (a rounding at the edge).
        if (zero_in ? step.sqrt_next < sqrt_target : step.sqrt_next > sqrt_target) step.sqrt_next = sqrt_target;
    }
    const BigUint in{reaches ? to_target : (zero_in ? Amount0Delta(step.sqrt_next, sqrt_current, liquidity, true) : Amount1Delta(sqrt_current, step.sqrt_next, liquidity, true))};
    const BigUint out{zero_in ? Amount1Delta(step.sqrt_next, sqrt_current, liquidity, false) : Amount0Delta(sqrt_current, step.sqrt_next, liquidity, false)};
    step.amount_in = std::min<uint64_t>(Fits64(in).value_or(amount_remaining), amount_remaining);
    step.amount_out = Fits64(out).value_or(MAX_AMOUNT);
    return step;
}

unsigned __int128 LiquidityForAmounts(const arith_uint256& sqrt_price, const arith_uint256& sqrt_a_in, const arith_uint256& sqrt_b_in, uint64_t amount0, uint64_t amount1)
{
    const arith_uint256 a{std::min(sqrt_a_in, sqrt_b_in)}, b{std::max(sqrt_a_in, sqrt_b_in)};
    if (a == b) return 0;
    const BigUint q96{BigUint{1} << 96};
    const auto for0{[&](const arith_uint256& lo, const arith_uint256& hi) {
        const BigUint intermediate{(Big(lo) * Big(hi)) / q96};
        return (BigUint{amount0} * intermediate) / Big(hi - lo);
    }};
    const auto for1{[&](const arith_uint256& lo, const arith_uint256& hi) { return (BigUint{amount1} * q96) / Big(hi - lo); }};
    BigUint l;
    if (sqrt_price <= a) {
        l = for0(a, b);
    } else if (sqrt_price < b) {
        l = std::min(for0(sqrt_price, b), for1(a, sqrt_price));
    } else {
        l = for1(a, b);
    }
    const auto v{l.ToU128()};
    return v ? *v : ~static_cast<unsigned __int128>(0);
}

std::optional<std::pair<uint64_t, uint64_t>> AmountsForLiquidity(const arith_uint256& sqrt_price, const arith_uint256& sqrt_a_in, const arith_uint256& sqrt_b_in, unsigned __int128 liquidity, bool round_up)
{
    const arith_uint256 a{std::min(sqrt_a_in, sqrt_b_in)}, b{std::max(sqrt_a_in, sqrt_b_in)};
    BigUint x, y;
    if (sqrt_price <= a) {
        x = Amount0Delta(a, b, liquidity, round_up);
    } else if (sqrt_price < b) {
        x = Amount0Delta(sqrt_price, b, liquidity, round_up);
        y = Amount1Delta(a, sqrt_price, liquidity, round_up);
    } else {
        y = Amount1Delta(a, b, liquidity, round_up);
    }
    const auto x64{Fits64(x)}, y64{Fits64(y)};
    if (!x64 || !y64) return std::nullopt;
    return std::make_pair(*x64, *y64);
}

int32_t TickSpacing(uint32_t fee)
{
    switch (fee) {
    case 100: return 1;
    case 500: return 10;
    case 3'000: return 60;
    case 10'000: return 200;
    }
    return 0;
}

uint256 PriceQ64(const arith_uint256& sqrt_price, bool inverse)
{
    if (sqrt_price == 0) return uint256{};
    const BigUint sq{Big(sqrt_price) * Big(sqrt_price)};
    if (!inverse) return (sq >> 128).ToUint256();
    // 2^256 / sqrt²: of asset1 in asset0, UQ64.64.
    const BigUint q{(BigUint{1} << 256) / sq};
    if (q.Bits() > 256) return ArithToUint256(~arith_uint256{});
    return q.ToUint256();
}

} // namespace bitassets::cl
