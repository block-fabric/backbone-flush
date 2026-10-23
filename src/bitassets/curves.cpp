// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitassets/curves.h>

#include <arith_uint256.h>
#include <hash.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace bitassets {

uint256 PoolIdOf(const AssetId& a, const AssetId& b, const PoolParams& params)
{
    if (params.Legacy()) return PoolId(a, b);
    HashWriter writer{};
    writer << std::string{"Layers pool"} << std::min(a, b) << std::max(a, b) << params.curve << params.fee << params.amp << params.weight0 << params.flags;
    return writer.GetSHA256();
}

std::optional<std::string> WhyNotPoolKind(uint8_t curve, uint32_t fee, uint32_t amp, uint8_t weight_a, uint8_t flags)
{
    if (!CreatableCurve(curve)) return "an unknown curve";
    if (std::find(FEE_TIERS.begin(), FEE_TIERS.end(), fee) == FEE_TIERS.end()) return "a fee that is not a tier (0.01, 0.05, 0.3 or 1 %)";
    if ((flags & ~POOL_FLAG_DYNAMIC_FEE) != 0) return "unknown flags";
    switch (static_cast<Curve>(curve)) {
    case Curve::CONCENTRATED:
        if (amp != 0 || weight_a != 0) return "an amplification or a weight for a concentrated pool";
        break;
    case Curve::LAUNCH:
        return "a launch's curve (launchtoken makes those)";
    case Curve::BOOK:
        return "an order book (placeorder makes those)";
    case Curve::CONSTANT_PRODUCT:
        if (amp != 0 || weight_a != 0) return "an amplification or a weight for a constant-product pool";
        break;
    case Curve::STABLE:
        if (amp < MIN_AMP || amp > MAX_AMP) return "an amplification out of range (1 to 10000)";
        if (weight_a != 0) return "a weight for a stable pool";
        break;
    case Curve::WEIGHTED:
        if (std::find(WEIGHTS.begin(), WEIGHTS.end(), weight_a) == WEIGHTS.end()) return "a weight that is not one of 2, 5, 10, 20, 25, 40, 60, 75, 80, 90, 95, 98 % (50/50 is a constant-product pool)";
        if (amp != 0) return "an amplification for a weighted pool";
        break;
    }
    return std::nullopt;
}

PoolParams MakePoolParams(const CreatePool& create, uint8_t decimals0, uint8_t decimals1)
{
    PoolParams params;
    params.curve = create.curve;
    params.fee = create.fee;
    params.amp = create.amp;
    params.flags = create.flags;
    const bool a_first{create.asset_a < create.asset_b};
    if (create.curve == static_cast<uint8_t>(Curve::WEIGHTED)) params.weight0 = a_first ? create.weight_a : static_cast<uint8_t>(100 - create.weight_a);
    if (create.curve == static_cast<uint8_t>(Curve::STABLE)) {
        const uint8_t most{std::max(decimals0, decimals1)};
        params.scale0 = most - decimals0;
        params.scale1 = most - decimals1;
    }
    return params;
}

namespace curves {

//
// BigUint
//

BigUint BigUint::U128(unsigned __int128 value)
{
    BigUint result;
    result.m_limbs = {static_cast<uint64_t>(value), static_cast<uint64_t>(value >> 64)};
    result.Trim();
    return result;
}

BigUint BigUint::FromArith(const arith_uint256& value)
{
    BigUint result;
    for (int i{0}; i < 4; ++i) result.m_limbs.push_back((value >> (64 * i)).GetLow64());
    result.Trim();
    return result;
}

void BigUint::Trim()
{
    while (!m_limbs.empty() && m_limbs.back() == 0) m_limbs.pop_back();
}

size_t BigUint::Bits() const
{
    if (m_limbs.empty()) return 0;
    return 64 * (m_limbs.size() - 1) + (64 - std::countl_zero(m_limbs.back()));
}

std::optional<unsigned __int128> BigUint::ToU128() const
{
    if (m_limbs.size() > 2) return std::nullopt;
    unsigned __int128 value{0};
    for (size_t i{m_limbs.size()}; i-- > 0;) value = (value << 64) | m_limbs[i];
    return value;
}

uint256 BigUint::ToUint256() const
{
    uint256 result;
    for (size_t i{0}; i < 4 && i < m_limbs.size(); ++i) {
        for (int byte{0}; byte < 8; ++byte) result.data()[8 * i + byte] = static_cast<unsigned char>(m_limbs[i] >> (8 * byte));
    }
    return result;
}

BigUint operator+(const BigUint& a, const BigUint& b)
{
    BigUint result;
    const size_t n{std::max(a.m_limbs.size(), b.m_limbs.size())};
    result.m_limbs.resize(n + 1);
    unsigned __int128 carry{0};
    for (size_t i{0}; i < n; ++i) {
        const unsigned __int128 sum{carry + (i < a.m_limbs.size() ? a.m_limbs[i] : 0) + (i < b.m_limbs.size() ? b.m_limbs[i] : 0)};
        result.m_limbs[i] = static_cast<uint64_t>(sum);
        carry = sum >> 64;
    }
    result.m_limbs[n] = static_cast<uint64_t>(carry);
    result.Trim();
    return result;
}

BigUint operator-(const BigUint& a, const BigUint& b)
{
    BigUint result;
    result.m_limbs.resize(a.m_limbs.size());
    uint64_t borrow{0};
    for (size_t i{0}; i < a.m_limbs.size(); ++i) {
        const uint64_t sub{i < b.m_limbs.size() ? b.m_limbs[i] : 0};
        const unsigned __int128 take{static_cast<unsigned __int128>(sub) + borrow};
        if (static_cast<unsigned __int128>(a.m_limbs[i]) >= take) {
            result.m_limbs[i] = static_cast<uint64_t>(a.m_limbs[i] - take);
            borrow = 0;
        } else {
            result.m_limbs[i] = static_cast<uint64_t>((static_cast<unsigned __int128>(1) << 64) + a.m_limbs[i] - take);
            borrow = 1;
        }
    }
    result.Trim();
    return result;
}

BigUint operator*(const BigUint& a, const BigUint& b)
{
    BigUint result;
    if (a.IsZero() || b.IsZero()) return result;
    result.m_limbs.assign(a.m_limbs.size() + b.m_limbs.size(), 0);
    for (size_t i{0}; i < a.m_limbs.size(); ++i) {
        unsigned __int128 carry{0};
        for (size_t j{0}; j < b.m_limbs.size(); ++j) {
            // At most (2^64 - 1)^2 + 2·(2^64 - 1) = 2^128 - 1: it fits.
            const unsigned __int128 t{static_cast<unsigned __int128>(a.m_limbs[i]) * b.m_limbs[j] + result.m_limbs[i + j] + carry};
            result.m_limbs[i + j] = static_cast<uint64_t>(t);
            carry = t >> 64;
        }
        result.m_limbs[i + b.m_limbs.size()] = static_cast<uint64_t>(carry);
    }
    result.Trim();
    return result;
}

BigUint BigUint::operator<<(unsigned shift) const
{
    if (IsZero()) return {};
    BigUint result;
    const size_t limbs{shift / 64}, bits{shift % 64};
    result.m_limbs.assign(m_limbs.size() + limbs + 1, 0);
    for (size_t i{0}; i < m_limbs.size(); ++i) {
        result.m_limbs[i + limbs] |= m_limbs[i] << bits;
        if (bits != 0) result.m_limbs[i + limbs + 1] |= m_limbs[i] >> (64 - bits);
    }
    result.Trim();
    return result;
}

BigUint BigUint::operator>>(unsigned shift) const
{
    BigUint result;
    const size_t limbs{shift / 64}, bits{shift % 64};
    if (limbs >= m_limbs.size()) return result;
    result.m_limbs.assign(m_limbs.size() - limbs, 0);
    for (size_t i{limbs}; i < m_limbs.size(); ++i) {
        result.m_limbs[i - limbs] = m_limbs[i] >> bits;
        if (bits != 0 && i + 1 < m_limbs.size()) result.m_limbs[i - limbs] |= m_limbs[i + 1] << (64 - bits);
    }
    result.Trim();
    return result;
}

BigUint operator/(const BigUint& a, const BigUint& b)
{
    BigUint quotient;
    if (b.IsZero() || a < b) return quotient;
    if (b.m_limbs.size() == 1) {
        quotient.m_limbs.resize(a.m_limbs.size());
        unsigned __int128 rem{0};
        for (size_t i{a.m_limbs.size()}; i-- > 0;) {
            const unsigned __int128 cur{(rem << 64) | a.m_limbs[i]};
            quotient.m_limbs[i] = static_cast<uint64_t>(cur / b.m_limbs[0]);
            rem = cur % b.m_limbs[0];
        }
        quotient.Trim();
        return quotient;
    }
    // Bit by bit, from the top (only the prices use it, once a pool and a block).
    BigUint rem;
    const size_t bits{a.Bits()};
    quotient.m_limbs.assign(a.m_limbs.size(), 0);
    for (size_t i{bits}; i-- > 0;) {
        rem = rem << 1;
        if ((a.m_limbs[i / 64] >> (i % 64)) & 1) rem = rem + BigUint{1};
        if (rem >= b) {
            rem = rem - b;
            quotient.m_limbs[i / 64] |= uint64_t{1} << (i % 64);
        }
    }
    quotient.Trim();
    return quotient;
}

BigUint BigUint::Pow(uint32_t exponent) const
{
    BigUint result{1}, base{*this};
    while (exponent > 0) {
        if (exponent & 1) result = result * base;
        exponent >>= 1;
        if (exponent > 0) base = base * base;
    }
    return result;
}

std::strong_ordering operator<=>(const BigUint& a, const BigUint& b)
{
    if (a.m_limbs.size() != b.m_limbs.size()) return a.m_limbs.size() <=> b.m_limbs.size();
    for (size_t i{a.m_limbs.size()}; i-- > 0;) {
        if (a.m_limbs[i] != b.m_limbs[i]) return a.m_limbs[i] <=> b.m_limbs[i];
    }
    return std::strong_ordering::equal;
}

namespace {

/** The largest v in [lo, hi] for which ok(v); ok(lo) holds, and ok is true then false (monotone). */
template <typename T, typename Pred>
T SearchMax(T lo, T hi, Pred&& ok)
{
    while (lo < hi) {
        const T mid{static_cast<T>(lo + (hi - lo) / 2 + 1)};
        if (ok(mid)) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

/** The smallest v in [lo, hi] for which ok(v); ok(hi) holds, and ok is false then true. */
template <typename T, typename Pred>
T SearchMin(T lo, T hi, Pred&& ok)
{
    while (lo < hi) {
        const T mid{static_cast<T>(lo + (hi - lo) / 2)};
        if (ok(mid)) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return lo;
}

uint64_t Pow10(uint8_t exponent)
{
    uint64_t value{1};
    for (uint8_t i{0}; i < exponent; ++i) value *= 10;
    return value;
}

/** Stable: how 16A·XY(X+Y)·m³ + 4XY·pq·n·m² compares with 16A·XY·pq·n·m² + n³·(pq)², X = a/p, Y = b/q, D = n/m: Φ's sign. */
std::strong_ordering StablePhi(uint32_t amp, const BigUint& a, const BigUint& p, const BigUint& b, const BigUint& q, const BigUint& n, const BigUint& m)
{
    const BigUint a16{BigUint{16} * BigUint{amp}};
    const BigUint ab{a * b}, pq{p * q}, m2{m * m};
    const BigUint left{a16 * ab * (a * q + b * p) * m2 * m + BigUint{4} * ab * pq * n * m2};
    const BigUint right{a16 * ab * pq * n * m2 + n * n * n * pq * pq};
    return left <=> right;
}

/** The first-degree units of a level: D (stable) or the geometric mean (the others), as the ratio num/den of a level. */
} // namespace

uint64_t SearchMaxU64(uint64_t lo, uint64_t hi, const std::function<bool(uint64_t)>& ok) { return SearchMax<uint64_t>(lo, hi, ok); }

std::pair<uint32_t, uint32_t> ReducedWeights(const PoolParams& params)
{
    if (params.GetCurve() != Curve::WEIGHTED) return {1, 1};
    const uint32_t w0{params.weight0}, w1{100U - params.weight0};
    const uint32_t g{std::gcd(w0, w1)};
    return {w0 / g, w1 / g};
}

std::pair<unsigned __int128, unsigned __int128> StableD(uint32_t amp, unsigned __int128 x, unsigned __int128 y)
{
    if (x == 0 || y == 0) return {0, 0};
    const BigUint bx{BigUint::U128(x)}, by{BigUint::U128(y)}, one{1};
    // Φ(D) falls as D grows: positive at 0, at most 0 at X + Y (−(X+Y)(X−Y)²).
    const auto at_least{[&](unsigned __int128 d) { return StablePhi(amp, bx, one, by, one, BigUint::U128(d), one) >= 0; }};
    const unsigned __int128 floor{SearchMax<unsigned __int128>(0, x + y, at_least)};
    const bool exact{StablePhi(amp, bx, one, by, one, BigUint::U128(floor), one) == 0};
    return {floor, exact ? floor : floor + 1};
}

Level::Level(const PoolParams& params, uint64_t reserve0, uint64_t reserve1) : m_params{params}, m_x0{reserve0}, m_y0{reserve1}
{
    if (params.GetCurve() == Curve::STABLE) {
        m_s0 = Pow10(params.scale0);
        m_s1 = Pow10(params.scale1);
        m_d = StableD(params.amp, static_cast<unsigned __int128>(reserve0) * m_s0, static_cast<unsigned __int128>(reserve1) * m_s1).second;
    } else {
        std::tie(m_wa, m_wb) = ReducedWeights(params);
        m_k = BigUint{reserve0}.Pow(m_wa) * BigUint{reserve1}.Pow(m_wb);
    }
}

bool Level::Holds(const Rat& x, const Rat& y, uint64_t num, uint64_t den) const
{
    if (m_params.GetCurve() == Curve::STABLE) {
        return StablePhi(m_params.amp, x.num * BigUint{m_s0}, x.den, y.num * BigUint{m_s1}, y.den, BigUint::U128(m_d) * BigUint{num}, BigUint{den}) >= 0;
    }
    // x^wa·y^wb ≥ k·(num/den)^(wa+wb), multiplied out.
    const uint32_t degree{m_wa + m_wb};
    const BigUint left{x.num.Pow(m_wa) * y.num.Pow(m_wb) * BigUint{den}.Pow(degree)};
    const BigUint right{m_k * BigUint{num}.Pow(degree) * x.den.Pow(m_wa) * y.den.Pow(m_wb)};
    return left >= right;
}

namespace {

/** ClearPool's formula at any fee: the price (R1·F + B̂)/(R0·F + Â) of asset0 in asset1, Â, B̂ the inputs times F − fee. */
std::optional<dex::Clearing> ClearConstantProduct(uint32_t fee, uint64_t reserve0, uint64_t reserve1, const std::vector<dex::HopIn>& orders)
{
    // At 0.30 % it is ClearPool's, exactly (the same fractions, scaled by 1000).
    if (fee == LEGACY_FEE) return dex::ClearPool(reserve0, reserve1, orders);
    if (reserve0 == 0 || reserve1 == 0 || reserve0 > MAX_AMOUNT || reserve1 > MAX_AMOUNT) return std::nullopt;
    uint64_t in0{0}, in1{0};
    for (const dex::HopIn& order : orders) {
        if (order.amount == 0 || order.amount > MAX_AMOUNT) return std::nullopt;
        uint64_t& sum{order.zero_in ? in0 : in1};
        if (order.amount > MAX_AMOUNT - sum) return std::nullopt;
        sum += order.amount;
    }
    if (in0 > MAX_AMOUNT - reserve0 || in1 > MAX_AMOUNT - reserve1) return std::nullopt;
    // An amount (63 bits) times F − fee (20) times one of these (84): 167 bits.
    const arith_uint256 keep{FEE_DENOMINATOR - fee}, f{FEE_DENOMINATOR};
    const arith_uint256 num{arith_uint256{reserve1} * f + arith_uint256{in1} * keep};
    const arith_uint256 den{arith_uint256{reserve0} * f + arith_uint256{in0} * keep};
    dex::Clearing clearing;
    uint64_t out0{0}, out1{0};
    for (const dex::HopIn& order : orders) {
        const arith_uint256 paid{arith_uint256{order.amount} * keep};
        const arith_uint256 out{order.zero_in ? paid * num / (den * f) : paid * den / (num * f)};
        const uint64_t got{out.GetLow64()};
        clearing.outs.push_back(got);
        uint64_t& sum{order.zero_in ? out1 : out0};
        if (got > MAX_AMOUNT - sum) return std::nullopt;
        sum += got;
    }
    if (out0 >= reserve0 + in0 || out1 >= reserve1 + in1) return std::nullopt;
    clearing.reserve0 = reserve0 + in0 - out0;
    clearing.reserve1 = reserve1 + in1 - out1;
    return clearing;
}

/** Stable and weighted pools: the uniform price where the pool's net trade meets its curve (ClearPoolOf). */
std::optional<dex::Clearing> ClearInvariant(const PoolParams& params, uint32_t fee, uint64_t reserve0, uint64_t reserve1, const std::vector<dex::HopIn>& orders)
{
    if (reserve0 == 0 || reserve1 == 0 || reserve0 > MAX_AMOUNT || reserve1 > MAX_AMOUNT) return std::nullopt;
    uint64_t in0{0}, in1{0};
    for (const dex::HopIn& order : orders) {
        if (order.amount == 0 || order.amount > MAX_AMOUNT) return std::nullopt;
        uint64_t& sum{order.zero_in ? in0 : in1};
        if (order.amount > MAX_AMOUNT - sum) return std::nullopt;
        sum += order.amount;
    }
    if (in0 > MAX_AMOUNT - reserve0 || in1 > MAX_AMOUNT - reserve1) return std::nullopt;
    const uint64_t keep{FEE_DENOMINATOR - fee};
    // Â and B̂: the inputs less the fee, times F (63 + 20 bits).
    const unsigned __int128 a_hat{static_cast<unsigned __int128>(in0) * keep}, b_hat{static_cast<unsigned __int128>(in1) * keep};
    const unsigned __int128 f{FEE_DENOMINATOR};
    const Level level{params, reserve0, reserve1};
    // Asset0 in on balance, dy of asset1 out: the pool's worst point (x0 + Â·dy/(F·dy + B̂), y0 − dy).
    const auto holds0{[&](uint64_t dy) {
        Rat x;
        if (b_hat == 0) {
            x.num = BigUint{reserve0} * BigUint::U128(f) + BigUint::U128(a_hat);
            x.den = BigUint::U128(f);
        } else {
            const unsigned __int128 m{f * dy + b_hat};
            x.num = BigUint{reserve0} * BigUint::U128(m) + BigUint::U128(a_hat) * BigUint{dy};
            x.den = BigUint::U128(m);
        }
        return level.Holds(x, Rat{reserve1 - dy});
    }};
    const auto holds1{[&](uint64_t dx) {
        Rat y;
        if (a_hat == 0) {
            y.num = BigUint{reserve1} * BigUint::U128(f) + BigUint::U128(b_hat);
            y.den = BigUint::U128(f);
        } else {
            const unsigned __int128 m{f * dx + a_hat};
            y.num = BigUint{reserve1} * BigUint::U128(m) + BigUint::U128(b_hat) * BigUint{dx};
            y.den = BigUint::U128(m);
        }
        return level.Holds(Rat{reserve0 - dx}, y);
    }};
    // The price: of asset0 in asset1 as num/den (zero_side_price), or of asset1 in asset0.
    bool asset0_priced{true};
    unsigned __int128 num{0}, den{0};
    uint64_t dy{0};
    if (a_hat > 0) dy = SearchMax<uint64_t>(0, reserve1 - 1, holds0);
    if (a_hat > 0 && (dy > 0 || b_hat == 0)) {
        num = f * dy + b_hat;
        den = a_hat;
    } else {
        uint64_t dx{0};
        if (b_hat > 0) dx = SearchMax<uint64_t>(0, reserve0 - 1, holds1);
        if (b_hat > 0 && (dx > 0 || a_hat == 0)) {
            asset0_priced = false;
            num = f * dx + a_hat;
            den = b_hat;
        } else {
            // Neither side moves the pool: the orders cross at B'/A'.
            num = b_hat;
            den = a_hat;
        }
    }
    dex::Clearing clearing;
    uint64_t out0{0}, out1{0};
    const arith_uint256 pnum{UintToArith256(BigUint::U128(num).ToUint256())}, pden{UintToArith256(BigUint::U128(den).ToUint256())}, f256{FEE_DENOMINATOR};
    for (const dex::HopIn& order : orders) {
        // An amount times keep (83 bits) times num or den (84): 167 bits.
        const arith_uint256 paid{arith_uint256{order.amount} * keep};
        // The side the price is of gets paid·P, the other paid/P; down, toward the pool.
        const bool priced_side{order.zero_in == asset0_priced};
        arith_uint256 out;
        if (priced_side) {
            out = pden == 0 ? arith_uint256{} : paid * pnum / (pden * f256);
        } else {
            out = pnum == 0 ? arith_uint256{} : paid * pden / (pnum * f256);
        }
        const uint64_t got{out > arith_uint256{MAX_AMOUNT} ? MAX_AMOUNT : out.GetLow64()};
        clearing.outs.push_back(got);
        uint64_t& sum{order.zero_in ? out1 : out0};
        if (got > MAX_AMOUNT - sum) return std::nullopt;
        sum += got;
    }
    if (out0 >= reserve0 + in0 || out1 >= reserve1 + in1) return std::nullopt;
    clearing.reserve0 = reserve0 + in0 - out0;
    clearing.reserve1 = reserve1 + in1 - out1;
    return clearing;
}
} // namespace

std::optional<dex::Clearing> ClearLaunch(const LaunchCurve& curve, uint64_t x, uint64_t y, const std::vector<dex::HopIn>& orders, const std::vector<bool>& direct)
{
    if (curve.fee >= FEE_DENOMINATOR || y <= curve.end || x == 0 || x > MAX_AMOUNT || y > MAX_AMOUNT || direct.size() != orders.size()) return std::nullopt;
    const uint64_t f{FEE_DENOMINATOR};
    const auto fee_of{[&](uint64_t amount) { return static_cast<uint64_t>((static_cast<unsigned __int128>(amount) * curve.fee + f - 1) / f); }};
    // The fee-free amounts: a buy's coin less its fee (rounded up), a sale's tokens whole. A buy over the bound
    // misses (it goes through as nothing: 0 out).
    std::vector<dex::HopIn> net;
    std::vector<bool> in_play;
    for (const dex::HopIn& order : orders) {
        if (order.amount == 0 || order.amount > MAX_AMOUNT) return std::nullopt;
        const bool over{order.zero_in && ((curve.max_buy != 0 && order.amount > curve.max_buy) || order.amount < curve.min_buy)};
        const uint64_t paid{order.zero_in ? order.amount - fee_of(order.amount) : order.amount};
        in_play.push_back(!over && paid > 0);
        if (!over && paid > 0) net.push_back({order.zero_in, paid});
    }
    dex::Clearing clearing;
    clearing.outs.assign(orders.size(), 0);
    clearing.refunds.assign(orders.size(), 0);
    clearing.reserve0 = x;
    clearing.reserve1 = y;
    if (net.empty()) return clearing;
    if (curve.start != 0 && y > curve.start) return std::nullopt;
    auto cleared{ClearConstantProduct(/*fee=*/0, x, y, net)};
    if (!cleared) return std::nullopt;
    if (curve.start != 0 && cleared->reserve1 > curve.start) {
        // More units back than it sold: the sales miss (their units back), the buys clear alone.
        net.clear();
        for (size_t i{0}; i < orders.size(); ++i) {
            if (!in_play[i]) continue;
            if (!orders[i].zero_in) {
                in_play[i] = false;
                continue;
            }
            net.push_back({true, orders[i].amount - fee_of(orders[i].amount)});
        }
        if (net.empty()) return clearing;
        cleared = ClearConstantProduct(/*fee=*/0, x, y, net);
        if (!cleared) return std::nullopt;
    }
    uint64_t fees{0};
    if (cleared->reserve1 >= curve.end) {
        // Not past the end: a constant-product round of the fee-free amounts; the sellers' fee out of what they get.
        size_t k{0};
        for (size_t i{0}; i < orders.size(); ++i) {
            if (!in_play[i]) continue;
            const uint64_t out{cleared->outs[k++]};
            if (orders[i].zero_in) {
                clearing.outs[i] = out;
                fees += fee_of(orders[i].amount);
            } else {
                const uint64_t fee{fee_of(out)};
                clearing.outs[i] = out - std::min(out, fee);
                fees += std::min(out, fee);
            }
        }
        clearing.reserve0 = cleared->reserve0;
        clearing.reserve1 = cleared->reserve1;
        clearing.fees = fees;
        clearing.reached_end = cleared->reserve1 == curve.end;
        return clearing;
    }
    // Past the end: to the end exactly, at the price of the last stretch, P = Δx/Δy.
    const unsigned __int128 k_now{static_cast<unsigned __int128>(x) * y};
    const unsigned __int128 x_end{(k_now + curve.end - 1) / curve.end};
    if (x_end > MAX_AMOUNT || x_end <= x) return std::nullopt;
    const uint64_t dx{static_cast<uint64_t>(x_end) - x}, dy{y - curve.end};
    uint64_t sold_in{0}, coin_out{0};
    // The sellers: ⌊b·Δx/Δy⌋, less the fee.
    for (size_t i{0}; i < orders.size(); ++i) {
        if (!in_play[i] || orders[i].zero_in) continue;
        const uint64_t gross{static_cast<uint64_t>(static_cast<unsigned __int128>(orders[i].amount) * dx / dy)};
        const uint64_t fee{std::min(gross, fee_of(gross))};
        clearing.outs[i] = gross - fee;
        fees += fee;
        coin_out += gross;
        sold_in += orders[i].amount;
    }
    // The buyers that pay in directly share what there is by what they pay (less the fee), rounded down; the units left,
    // one each in order to those that can pay for one more.
    unsigned __int128 offered{0};
    for (size_t i{0}; i < orders.size(); ++i) {
        if (in_play[i] && orders[i].zero_in && direct[i]) offered += orders[i].amount - fee_of(orders[i].amount);
    }
    const uint64_t tokens{dy + sold_in};
    std::vector<uint64_t> got(orders.size(), 0);
    uint64_t given{0};
    const auto cost{[&](uint64_t t) { return static_cast<uint64_t>((static_cast<unsigned __int128>(t) * dx + dy - 1) / dy); }};
    for (size_t i{0}; i < orders.size(); ++i) {
        if (!in_play[i] || !orders[i].zero_in || !direct[i] || offered == 0) continue;
        const uint64_t budget{orders[i].amount - fee_of(orders[i].amount)};
        // Its share, and never more than it can pay for at P: cost(t) ≤ budget ⇔ t ≤ ⌊budget·Δy/Δx⌋.
        const uint64_t affordable{static_cast<uint64_t>(static_cast<unsigned __int128>(budget) * dy / dx)};
        const uint64_t t{std::min(affordable, static_cast<uint64_t>(static_cast<unsigned __int128>(tokens) * budget / offered))};
        got[i] = t;
        given += t;
    }
    for (size_t i{0}; i < orders.size() && given < tokens; ++i) {
        if (!in_play[i] || !orders[i].zero_in || !direct[i]) continue;
        const uint64_t budget{orders[i].amount - fee_of(orders[i].amount)};
        if (cost(got[i] + 1) <= budget) {
            ++got[i];
            ++given;
        }
    }
    uint64_t coin_in{0};
    for (size_t i{0}; i < orders.size(); ++i) {
        if (!in_play[i] || !orders[i].zero_in || !direct[i] || got[i] == 0) continue;
        const uint64_t pays{cost(got[i])};
        // The fee on what it pays, rounded up, never more than it brought.
        const uint64_t fee{std::min(orders[i].amount - pays, static_cast<uint64_t>((static_cast<unsigned __int128>(pays) * curve.fee + (f - curve.fee) - 1) / (f - curve.fee)))};
        clearing.outs[i] = got[i];
        clearing.refunds[i] = orders[i].amount - pays - fee;
        fees += fee;
        coin_in += pays;
    }
    if (coin_in < coin_out || given > tokens) return std::nullopt;
    if (curve.start != 0 && y + sold_in - given > curve.start) return std::nullopt;
    clearing.reserve0 = x + coin_in - coin_out;
    clearing.reserve1 = y + sold_in - given;
    clearing.fees = fees;
    clearing.reached_end = !curve.strict_end || clearing.reserve1 == curve.end;
    return clearing;
}

std::optional<dex::Clearing> ClearPoolOf(const PoolParams& params, uint32_t fee, uint64_t reserve0, uint64_t reserve1, const std::vector<dex::HopIn>& orders)
{
    if (fee >= FEE_DENOMINATOR) return std::nullopt;
    if (params.GetCurve() == Curve::CONSTANT_PRODUCT) return ClearConstantProduct(fee, reserve0, reserve1, orders);
    // A launch curve clears with its record (ClearLaunch), a concentrated pool with its state (State::ClearCl), never here.
    if (params.GetCurve() == Curve::LAUNCH || params.GetCurve() == Curve::CONCENTRATED || params.GetCurve() == Curve::BOOK) return std::nullopt;
    return ClearInvariant(params, fee, reserve0, reserve1, orders);
}

uint64_t SwapOutOf(const PoolParams& params, uint32_t fee, uint64_t reserve0, uint64_t reserve1, bool zero_in, uint64_t amount)
{
    if (amount == 0) return 0;
    const auto cleared{ClearPoolOf(params, fee, reserve0, reserve1, {dex::HopIn{zero_in, amount}})};
    return cleared ? cleared->outs[0] : 0;
}

std::optional<uint64_t> SwapInOf(const PoolParams& params, uint32_t fee, uint64_t reserve0, uint64_t reserve1, bool zero_in, uint64_t amount_out)
{
    const uint64_t reserve_in{zero_in ? reserve0 : reserve1};
    if (amount_out == 0 || reserve_in >= MAX_AMOUNT) return std::nullopt;
    const uint64_t most{MAX_AMOUNT - reserve_in};
    const auto gives{[&](uint64_t in) { return SwapOutOf(params, fee, reserve0, reserve1, zero_in, in) >= amount_out; }};
    if (!gives(most)) return std::nullopt;
    return SearchMin<uint64_t>(1, most, gives);
}

std::optional<uint64_t> OpeningShares(const PoolParams& params, uint64_t reserve0, uint64_t reserve1)
{
    if (reserve0 == 0 || reserve1 == 0) return std::nullopt;
    if (params.GetCurve() == Curve::STABLE) {
        const unsigned __int128 d{StableD(params.amp, static_cast<unsigned __int128>(reserve0) * Pow10(params.scale0), static_cast<unsigned __int128>(reserve1) * Pow10(params.scale1)).first};
        if (d > MAX_AMOUNT) return std::nullopt;
        return static_cast<uint64_t>(d);
    }
    // The floor of the weighted geometric mean: the largest v with v^(wa+wb) ≤ x^wa·y^wb (isqrt(x·y) for constant product).
    const auto [wa, wb]{ReducedWeights(params)};
    const BigUint k{BigUint{reserve0}.Pow(wa) * BigUint{reserve1}.Pow(wb)};
    return SearchMax<uint64_t>(0, std::max(reserve0, reserve1), [&](uint64_t v) { return BigUint{v}.Pow(wa + wb) <= k; });
}

std::optional<uint64_t> DepositShares(const PoolParams& params, uint32_t fee, const Pool& pool, uint64_t amount0, uint64_t amount1)
{
    const uint64_t x{pool.reserve0}, y{pool.reserve1}, total{pool.shares};
    if (x == 0 || y == 0 || total == 0 || total > MAX_AMOUNT || (amount0 == 0 && amount1 == 0)) return std::nullopt;
    if (amount0 > MAX_AMOUNT - x || amount1 > MAX_AMOUNT - y) return std::nullopt;
    // The part of a side's excess a swap to the pool's proportions trades: the other side's weight.
    const auto [wa, wb]{ReducedWeights(params)};
    // The excess: a0 − a1·x/y of asset0 (or a1 − a0·y/x of asset1); the fee on its part φ, rounded up.
    uint64_t eff0{amount0}, eff1{amount1};
    const unsigned __int128 lhs{static_cast<unsigned __int128>(amount0) * y}, rhs{static_cast<unsigned __int128>(amount1) * x};
    const bool zero_excess{lhs >= rhs};
    const unsigned __int128 excess{zero_excess ? lhs - rhs : rhs - lhs};
    const uint64_t phi_num{zero_excess ? wb : wa}, phi_den{wa + wb};
    // excess (126 bits) · φ (6) · fee (14): 146 bits.
    const arith_uint256 owed_num{UintToArith256(BigUint::U128(excess).ToUint256()) * phi_num * fee};
    const arith_uint256 owed_den{arith_uint256{zero_excess ? y : x} * phi_den * FEE_DENOMINATOR};
    const arith_uint256 owed{(owed_num + owed_den - 1) / owed_den};
    uint64_t& charged{zero_excess ? eff0 : eff1};
    if (owed >= arith_uint256{charged}) return std::nullopt;
    charged -= owed.GetLow64();
    const Level level{params, x, y};
    const Rat to_x{x + eff0}, to_y{y + eff1};
    const uint64_t shares{SearchMax<uint64_t>(0, MAX_AMOUNT - total, [&](uint64_t s) { return level.Holds(to_x, to_y, total + s, total); })};
    if (shares == 0) return std::nullopt;
    return shares;
}

std::pair<uint64_t, uint64_t> WithdrawOf(const PoolParams& params, uint32_t fee, const Pool& pool, uint64_t shares, uint8_t only)
{
    auto [out0, out1]{amm::Withdraw(pool, shares)};
    if (only == 0) return {out0, out1};
    const uint64_t left0{pool.reserve0 - out0}, left1{pool.reserve1 - out1};
    if (only == 1) {
        // The asset1 part goes back in and buys asset0 alone.
        const uint64_t bought{out1 > 0 ? SwapOutOf(params, fee, left0, left1, /*zero_in=*/false, out1) : 0};
        return {out0 + bought, 0};
    }
    const uint64_t bought{out0 > 0 ? SwapOutOf(params, fee, left0, left1, /*zero_in=*/true, out0) : 0};
    return {0, out1 + bought};
}

uint256 SpotPriceQ64(const PoolParams& params, uint64_t reserve0, uint64_t reserve1, bool inverse)
{
    if (reserve0 == 0 || reserve1 == 0) return uint256{};
    // The price of asset0 in asset1 as num/den; UQ64.64 the floor of num·2^64/den (den·2^64/num for the inverse).
    BigUint num, den;
    switch (params.GetCurve()) {
    case Curve::CONSTANT_PRODUCT:
    case Curve::LAUNCH:
    case Curve::CONCENTRATED:
        // A launch curve is a constant product of its (virtual) reserves; a concentrated pool's price is its state's
        // (State::GetClState), not of its reserves: this is what it holds, for those who ask without the state.
        return inverse ? dex::PriceQ64(reserve0, reserve1) : dex::PriceQ64(reserve1, reserve0);
    case Curve::WEIGHTED: {
        const auto [wa, wb]{ReducedWeights(params)};
        num = BigUint{reserve1} * BigUint{wa};
        den = BigUint{reserve0} * BigUint{wb};
        break;
    }
    case Curve::STABLE: {
        const BigUint s0{Pow10(params.scale0)}, s1{Pow10(params.scale1)};
        const BigUint x{BigUint{reserve0} * s0}, y{BigUint{reserve1} * s1};
        const BigUint d{BigUint::U128(StableD(params.amp, static_cast<unsigned __int128>(reserve0) * Pow10(params.scale0), static_cast<unsigned __int128>(reserve1) * Pow10(params.scale1)).second)};
        const BigUint a4{BigUint{4} * BigUint{params.amp}};
        const BigUint ad{a4 * d};
        // 4A(2X+Y) + D − 4AD and 4A(X+2Y) + D − 4AD: positive on the curve (D³/(4XY) plus 4A times a reserve).
        const BigUint p0{a4 * (x + x + y) + d}, p1{a4 * (x + y + y) + d};
        if (p0 <= ad || p1 <= ad) return inverse ? dex::PriceQ64(reserve0, reserve1) : dex::PriceQ64(reserve1, reserve0);
        num = s0 * y * (p0 - ad);
        den = s1 * x * (p1 - ad);
        break;
    }
    }
    if (inverse) std::swap(num, den);
    if (den.IsZero()) return uint256{};
    const BigUint q{(num << 64) / den};
    if (q.Bits() > 256) return ArithToUint256(~arith_uint256{});
    return q.ToUint256();
}

double SpotPrice(const PoolParams& params, uint64_t reserve0, uint64_t reserve1)
{
    return UintToArith256(SpotPriceQ64(params, reserve0, reserve1)).getdouble() / 18446744073709551616.0;
}

std::string CurveName(Curve curve)
{
    switch (curve) {
    case Curve::CONSTANT_PRODUCT: return "constant_product";
    case Curve::STABLE: return "stable";
    case Curve::WEIGHTED: return "weighted";
    case Curve::LAUNCH: return "launch";
    case Curve::CONCENTRATED: return "concentrated";
    case Curve::BOOK: return "book";
    }
    return "unknown";
}

std::optional<Curve> ParseCurve(const std::string& name)
{
    if (name == "constant_product" || name == "cp" || name == "xyk") return Curve::CONSTANT_PRODUCT;
    if (name == "stable" || name == "stableswap") return Curve::STABLE;
    if (name == "weighted") return Curve::WEIGHTED;
    if (name == "concentrated" || name == "cl") return Curve::CONCENTRATED;
    return std::nullopt;
}

} // namespace curves
} // namespace bitassets
