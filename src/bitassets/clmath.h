// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BITASSETS_CLMATH_H
#define BITCOIN_BITASSETS_CLMATH_H

#include <arith_uint256.h>
#include <bitassets/curves.h>

#include <cstdint>
#include <optional>
#include <utility>

/**
 * Concentrated liquidity (doc/layers.md, "Concentrated liquidity"): the arithmetic of Uniswap v3, in exact
 * integers. Prices are square roots in Q64.96 (sqrtPriceX96 = sqrt(asset1 per asset0, in units)·2^96), ticks are
 * the powers of 1.0001 (the price at tick t is 1.0001^t), liquidity is a 128-bit L.
 *
 *  - SqrtRatioAtTick is Uniswap v3's TickMath.getSqrtRatioAtTick, bit for bit (its constants: 2^128·1.0001^(−2^(k−1))
 *    rounded, each within one unit of the exact value, checked in contrib/dex-curves/reference.py); TickAtSqrtRatio
 *    is the largest tick whose ratio is at most the price (what Uniswap's getTickAtSqrtRatio returns).
 *  - The amounts between two prices and the next price after an amount are SqrtPriceMath's, rounded as it rounds
 *    (toward the pool: what goes in up, what comes out down), with every product exact (BigUint).
 */
namespace bitassets::cl {

inline constexpr int32_t MIN_TICK{-887272};
inline constexpr int32_t MAX_TICK{887272};
/** The ratios at MIN_TICK and MAX_TICK: no price is out of [MIN_SQRT_RATIO, MAX_SQRT_RATIO). */
const arith_uint256& MinSqrtRatio();
const arith_uint256& MaxSqrtRatio();

/** sqrt(1.0001^tick)·2^96, Uniswap v3's getSqrtRatioAtTick (|tick| ≤ MAX_TICK). */
arith_uint256 SqrtRatioAtTick(int32_t tick);
/** The largest tick whose SqrtRatioAtTick is at most `sqrt_price` (MIN_SQRT_RATIO ≤ sqrt_price < MAX_SQRT_RATIO). */
int32_t TickAtSqrtRatio(const arith_uint256& sqrt_price);

/** The asset0 between two prices for a liquidity: L·2^96·(b − a)/(b·a), rounded up or down. */
curves::BigUint Amount0Delta(const arith_uint256& sqrt_a, const arith_uint256& sqrt_b, unsigned __int128 liquidity, bool round_up);
/** The asset1 between two prices for a liquidity: L·(b − a)/2^96, rounded up or down. */
curves::BigUint Amount1Delta(const arith_uint256& sqrt_a, const arith_uint256& sqrt_b, unsigned __int128 liquidity, bool round_up);
/**
 * The price after `amount` of an asset goes in (zero_in: asset0, the price falls), as SqrtPriceMath's
 * getNextSqrtPriceFromInput: rounded so that the pool gets at least the amount's worth. nullopt if out of range.
 */
std::optional<arith_uint256> NextSqrtPriceFromInput(const arith_uint256& sqrt_price, unsigned __int128 liquidity, uint64_t amount, bool zero_in);

/** One step of an exact-input swap toward `target` (SwapMath.computeSwapStep, with no fee: the batch takes the fee before). */
struct Step {
    arith_uint256 sqrt_next;
    uint64_t amount_in{0};
    uint64_t amount_out{0};
};
Step SwapStep(const arith_uint256& sqrt_current, const arith_uint256& sqrt_target, unsigned __int128 liquidity, uint64_t amount_remaining, bool zero_in);

/** The most liquidity `amount0` and `amount1` give between two ticks at the price (LiquidityAmounts.getLiquidityForAmounts, rounded down). */
unsigned __int128 LiquidityForAmounts(const arith_uint256& sqrt_price, const arith_uint256& sqrt_a, const arith_uint256& sqrt_b, uint64_t amount0, uint64_t amount1);
/** What a liquidity between two ticks holds at the price, each side rounded up (going in) or down (coming out); nullopt above MAX_AMOUNT. */
std::optional<std::pair<uint64_t, uint64_t>> AmountsForLiquidity(const arith_uint256& sqrt_price, const arith_uint256& sqrt_a, const arith_uint256& sqrt_b, unsigned __int128 liquidity, bool round_up);

/** The tick spacing of a fee tier: 1, 10, 60, 200 (0.01, 0.05, 0.30, 1.00 %). */
int32_t TickSpacing(uint32_t fee);

/** The price of asset0 in asset1 at a square root price, UQ64.64 (floor of sqrt²·2^64/2^192); the inverse. */
uint256 PriceQ64(const arith_uint256& sqrt_price, bool inverse = false);

} // namespace bitassets::cl

#endif // BITCOIN_BITASSETS_CLMATH_H
