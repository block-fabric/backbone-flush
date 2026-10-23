// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BITASSETS_CURVES_H
#define BITCOIN_BITASSETS_CURVES_H

#include <arith_uint256.h>
#include <bitassets/dex.h>
#include <bitassets/state.h>
#include <uint256.h>

#include <compare>
#include <functional>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/**
 * The DEX's pool kinds (doc/layers.md, "Pool kinds"): the curves of the pools, in exact integer arithmetic.
 *
 * Every number the rules take from a curve is defined as the floor or ceiling of an exact quantity (the
 * largest amount out for which the pool's invariant does not fall, the largest number of shares whose
 * part of the invariant the deposit covers): it does not depend on how it is found. The search here is a
 * bisection over integers, each step an exact comparison of the invariant at a point with its level
 * (Level::Holds: products of integers of any size, BigUint). So every implementation that finds the same
 * floors agrees bit for bit, whatever its iteration; there is no fixed-point approximation, and no
 * rounding but at the end, toward the pool.
 *
 *  - Constant product: x·y = k (the pools AddLiquidity makes, at any fee tier).
 *  - StableSwap (Curve, two assets): 4A(X+Y) + D = 4AD + D³/(4XY), on X = x·10^scale0 and Y = y·10^scale1
 *    (the assets' decimals told apart). The level of a point is D, its exact root; a point holds a level D
 *    if Φ(X, Y, D) = 16A·XY(X+Y) + 4XY·D − 16A·D·XY − D³ ≥ 0. A pool compares points to the ceiling of its
 *    D (so it never takes less than the exact curve gives).
 *  - Weighted (Balancer, two assets): x^wa · y^wb = k, wa:wb the weights in lowest terms (80/20 is 4:1).
 *    A point holds the level of (x0, y0) if x^wa · y^wb ≥ x0^wa · y0^wb, exactly.
 */
namespace bitassets::curves {

/** An unsigned integer of any size: what the exact comparisons of the invariants need (no rounding anywhere). */
class BigUint
{
public:
    BigUint() = default;
    BigUint(uint64_t value)
    {
        if (value != 0) m_limbs.push_back(value);
    }
    static BigUint U128(unsigned __int128 value);
    static BigUint FromArith(const arith_uint256& value);
    bool IsZero() const { return m_limbs.empty(); }
    size_t Bits() const;
    /** The value if it fits in 128 bits. */
    std::optional<unsigned __int128> ToU128() const;
    /** The low 256 bits. */
    uint256 ToUint256() const;

    friend BigUint operator+(const BigUint& a, const BigUint& b);
    /** a - b; a must be at least b. */
    friend BigUint operator-(const BigUint& a, const BigUint& b);
    friend BigUint operator*(const BigUint& a, const BigUint& b);
    /** Floor of a / b; b must not be zero. */
    friend BigUint operator/(const BigUint& a, const BigUint& b);
    BigUint operator<<(unsigned shift) const;
    BigUint operator>>(unsigned shift) const;
    BigUint Pow(uint32_t exponent) const;
    friend std::strong_ordering operator<=>(const BigUint& a, const BigUint& b);
    friend bool operator==(const BigUint& a, const BigUint& b) = default;

private:
    //! Little-endian, no zero limb at the top.
    std::vector<uint64_t> m_limbs;
    void Trim();
};

/** A non-negative rational number. */
struct Rat {
    BigUint num;
    BigUint den{1};
};

/** The largest v in [lo, hi] for which ok(v), ok(lo) holding and ok true then false: the bisection every search of the curves is. */
uint64_t SearchMaxU64(uint64_t lo, uint64_t hi, const std::function<bool(uint64_t)>& ok);

/** The weights of a weighted pool in lowest terms (wa for asset0, wb for asset1); 1:1 for a constant-product pool. */
std::pair<uint32_t, uint32_t> ReducedWeights(const PoolParams& params);

/**
 * The invariant of a pool at a point (its reserves), which other points are compared to. For a
 * constant-product pool and a weighted one, x^wa·y^wb; for a stable pool, D (its ceiling, Dc).
 */
class Level
{
public:
    Level(const PoolParams& params, uint64_t reserve0, uint64_t reserve1);
    /**
     * Whether the point (x, y) is on or above this level times num/den in the units of the invariant's
     * first degree (D for a stable pool, the weighted geometric mean for the others): the pool would not
     * lose by going there. num/den 1 for a trade; (shares + s)/shares for a deposit of s shares.
     */
    bool Holds(const Rat& x, const Rat& y, uint64_t num = 1, uint64_t den = 1) const;
    /** A stable pool's Dc, the ceiling of its D (in the scaled units); 0 for the others. */
    unsigned __int128 D() const { return m_d; }

private:
    PoolParams m_params;
    uint64_t m_x0, m_y0;
    //! Weighted and constant product: x0^wa·y0^wb.
    BigUint m_k;
    uint32_t m_wa{1}, m_wb{1};
    //! Stable: the scales (10^scale) and Dc.
    uint64_t m_s0{1}, m_s1{1};
    unsigned __int128 m_d{0};
};

/** Stable: the exact root D of the invariant at (X, Y) (scaled), its floor and ceiling. */
std::pair<unsigned __int128, unsigned __int128> StableD(uint32_t amp, unsigned __int128 x, unsigned __int128 y);

/**
 * A round of a pool of any kind (dex::ClearPool's generalisation), at a fee of `fee` millionths. A
 * constant-product pool clears by ClearPool's formula (at 0.30 %, ClearPool's result exactly): the price
 * P = (R1 + B')/(R0 + A'). Every other curve by the same rule, the pool's net trade on (or above) its
 * curve at one uniform price:
 *
 * With A' and B' the inputs of asset0 and of asset1 less the fee, if asset0 comes in on balance the pool
 * pays out dy of asset1 net, and every order gets the price P = (dy + B')/A' of asset0 in asset1: one
 * paying a of asset0 gets floor(a·(1−fee)·P), one paying b of asset1 floor(b·(1−fee)/P). Then the pool
 * takes at least A'·dy/(dy + B') of asset0 net for at most dy of asset1: dy is the largest for which that
 * point, (x + A'·dy/(dy + B'), y − dy), holds the pool's level (Level::Holds). As dy grows that point
 * moves along a hyperbola that crosses the convex level curve once, so the largest dy is found by
 * bisection, exactly. If no dy > 0 holds, asset1 comes in on balance (the same, sides swapped); if
 * neither, the orders of each side are crossed with one another at P = B'/A', the pool trading nothing.
 * One order alone (B' = 0) gets dy, the most out the curve gives for its input. Rounding is down,
 * toward the pool; the fee stays in the pool. nullopt as ClearPool's.
 */
std::optional<dex::Clearing> ClearPoolOf(const PoolParams& params, uint32_t fee, uint64_t reserve0, uint64_t reserve1, const std::vector<dex::HopIn>& orders);

/**
 * A launch curve (bonding-curve token launches, doc/layers.md "Launches"): the constant product of virtual
 * reserves, x the chain's coin (asset0: CHN is the null id) and y the token, x·y = k, from (V, Tv) to its end
 * (V + G, yE), where it graduates. `fee` (millionths) is taken in the chain's coin out of every trade, on the
 * way in for a buy and on the way out for a sale; it leaves the curve (Clearing::fees).
 *
 * A round clears as a constant-product round of the fee-free amounts (ClearPool's formula: the price
 * (x + A')/(y + B)), unless the token's end would be passed: then the round goes exactly to the end of the
 * curve at the price of its last stretch, P = Δx/Δy with Δy = y − yE and Δx = ceil(x·y/yE) − x (a chord of the
 * convex curve: the curve never takes less than it would). Sellers get ⌊b·P⌋ less the fee; buyers that pay the
 * coin in directly (`direct`) share the Δy + B tokens there are by what they paid, rounded down, the units
 * left one each to the first that can pay for one more, each paying ⌈t·P⌉ (and the fee on it) and getting the
 * rest back (Clearing::refunds); an order that reaches the curve through another pool first misses there
 * (it gets 0: refunded whole by the batch). `max_buy` (0: none): a buy paying more misses.
 */
struct LaunchCurve {
    uint32_t fee{0};
    uint64_t end{0};
    uint64_t max_buy{0};
    //! The least a buy pays (0: any); one paying less misses.
    uint64_t min_buy{0};
    //! Whether a round past the end ends the curve only when it is at its end exactly (launches from
    //! SidechainParams::launch_params_height): without it, a round whose buys past the end all miss (routed in) or cannot
    //! pay for the last units still ends it, and it graduates with less raised.
    bool strict_end{false};
    //! The curve's virtual units at its start (Tv; 0: no bound, the launches of before launch_params_height, whose units all
    //! came off their curve). With it the curve never takes back more units than it sold: a round that would leave it
    //! above Tv (its coin below the virtual V: coin it never took in) clears without its sales, which miss (0 out: the
    //! batch gives their units back). An existing asset's units held outside the curve cannot drain it.
    uint64_t start{0};
};
std::optional<dex::Clearing> ClearLaunch(const LaunchCurve& curve, uint64_t x, uint64_t y, const std::vector<dex::HopIn>& orders, const std::vector<bool>& direct);

/** What `amount` of one side gives alone in a pool (a round of one order); 0 if nothing. */
uint64_t SwapOutOf(const PoolParams& params, uint32_t fee, uint64_t reserve0, uint64_t reserve1, bool zero_in, uint64_t amount);
/** The least that has to go in alone to get `amount_out` out; nullopt if the pool cannot give that much. */
std::optional<uint64_t> SwapInOf(const PoolParams& params, uint32_t fee, uint64_t reserve0, uint64_t reserve1, bool zero_in, uint64_t amount_out);

/**
 * The shares a pool opens with, as AddLiquidity's: the floor of the invariant's first degree at the
 * reserves after (the geometric mean, isqrt(x·y) for constant product; D for a stable pool, in the units
 * of the asset with more decimals). nullopt if it is above MAX_AMOUNT.
 */
std::optional<uint64_t> OpeningShares(const PoolParams& params, uint64_t reserve0, uint64_t reserve1);

/**
 * The shares a deposit of `amount0` and `amount1` (one may be zero) into a pool that has providers gives
 * (PoolDeposit): the most s for which the pool after, with the deposit's effective amounts, holds its
 * level times (shares + s)/shares. The effective amounts: the deposit at the pool's proportions counts
 * whole; of the side out of proportion, the excess e pays fee·φ·e, φ the part of it a swap to the pool's
 * proportions would trade (the other asset's weight; one half for constant product and stable), rounded
 * toward the pool. nullopt if no share.
 */
std::optional<uint64_t> DepositShares(const PoolParams& params, uint32_t fee, const Pool& pool, uint64_t amount0, uint64_t amount1);

/**
 * What withdrawing `shares` gives: both sides (`only` 0: amm::Withdraw), or one (1: asset0, 2: asset1):
 * its part plus what the other side's part buys of it alone in the pool after the shares' parts left
 * (SwapOutOf, at the pool's fee). The pool's reserves after are `reserve0`, `reserve1` less the result.
 */
std::pair<uint64_t, uint64_t> WithdrawOf(const PoolParams& params, uint32_t fee, const Pool& pool, uint64_t shares, uint8_t only);

/**
 * The price of asset0 in asset1 at the pool's point, UQ64.64 (floor): the reserves' ratio for constant
 * product (dex::PriceQ64, as before); y·wa/(x·wb) for weighted; for stable the slope of the curve at the
 * point, (s0·Y·(4A(2X+Y) + Dc − 4A·Dc)) / (s1·X·(4A(X+2Y) + Dc − 4A·Dc)). What the price accumulators
 * (TWAP) add up. `inverse`: of asset1 in asset0, the floor of the inverse quotient (not of the floor's inverse).
 */
uint256 SpotPriceQ64(const PoolParams& params, uint64_t reserve0, uint64_t reserve1, bool inverse = false);
/** The same, as a decimal number of raw units (for those who look). */
double SpotPrice(const PoolParams& params, uint64_t reserve0, uint64_t reserve1);

/** A curve's name ("constant_product", "stable", "weighted"), and back. */
std::string CurveName(Curve curve);
std::optional<Curve> ParseCurve(const std::string& name);

} // namespace bitassets::curves

#endif // BITCOIN_BITASSETS_CURVES_H
