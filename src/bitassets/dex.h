// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BITASSETS_DEX_H
#define BITCOIN_BITASSETS_DEX_H

#include <bitassets/state.h>
#include <consensus/amount.h>
#include <serialize.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/**
 * Layers' DEX: rules of the assets from Consensus::SidechainParams::dex_height (doc/layers.md, "The DEX").
 *
 *  - **Routes.** An order trades through up to MAX_ROUTE_ASSETS - 1 pools in one transaction (Route; a
 *    Swap is a route of one hop), with one least amount out for the whole path; each hop pays its pool's
 *    0.3 %.
 *  - **Batch clearing.** Orders do not trade when their transaction is applied: they wait for the end of
 *    the block (State::FinishBlock), where every order of the block clears together, a hop at a time
 *    (round k clears the k-th hop of every order still in), each pool at one uniform price per round
 *    (ClearPool). An order whose result misses its min_out does not fill: what it paid in comes back to
 *    it, and its transaction, valid, pays its fee. Orders that miss are taken out and the rest cleared
 *    again from the start, MAX_CLEARING_PASSES times at most; past that, the block's orders execute one
 *    after another in block order, each alone, each that misses refunded (the rules before the DEX).
 *  - **TWAP.** Each pool keeps price accumulators as Uniswap v2 does: at the end of every block that
 *    changes it, the price the pool had since its last change, times the seconds since then, is added
 *    (UQ64.64, modulo 2^256). The time is the block's (nTime: the slot's time for a block of stake, the
 *    header's for a BMM block), never less than the last. Each such block leaves an observation, the
 *    last TWAP_OBSERVATIONS of each pool kept (a ring).
 *  - **Oracles.** An oracle is its key, registered with a proof of possession and a name that labels
 *    it (the fee, ORACLE_REGISTRATION_FEE, goes to mainchain miners); it attests the value of an event once, signed (BIP340) over
 *    AttestationHash. Oracle contracts lock an amount and pay it to the outcome the attestation names.
 */
namespace bitassets {

/** Most assets on a route: four hops. */
inline constexpr size_t MAX_ROUTE_ASSETS{5};
/** Most orders (routes and swaps) a block's transactions may make; one beyond waits for the next block (the miner leaves it out). */
inline constexpr size_t MAX_BLOCK_ORDERS{500};
/** Passes of the batch clearing before the block's orders execute one after another. */
inline constexpr int MAX_CLEARING_PASSES{8};
/**
 * The work of the DEX's pool kinds a block may hold (their curves are searched, not a closed form): each
 * hop of an order through a stable or weighted pool, and each CreatePool, PoolDeposit and PoolWithdraw,
 * counts one. A transaction beyond is not valid in that block (bad-ba-block-curve-work-full), and the
 * miner leaves it for the next, as for MAX_BLOCK_ORDERS. With the clearing's passes this bounds a block's
 * clearing to (MAX_CLEARING_PASSES + 1) · MAX_BLOCK_CURVE_WORK rounds of a curve pool, each a few hundred
 * exact comparisons (doc/layers.md, "Pool kinds": the measured worst case).
 */
inline constexpr uint32_t MAX_BLOCK_CURVE_WORK{200};
/** The observations of the price accumulators each pool keeps: the last ones. */
inline constexpr uint32_t TWAP_OBSERVATIONS{1024};
/** What registering an oracle costs, paid to mainchain miners (as the CHN freed by retiring an asset): 0.1 CHN. */
inline constexpr CAmount ORACLE_REGISTRATION_FEE{10'000'000};
/** Most outcomes an oracle contract names. */
inline constexpr size_t MAX_CONTRACT_OUTCOMES{8};
/** Latest an oracle contract may expire, in blocks after the one that makes it: about a year of 15-second blocks. */
inline constexpr int32_t MAX_CONTRACT_DURATION{2'102'400};

/** An order of the block's batch, until the block's end (State::FinishBlock). */
struct Order {
    Txid txid;
    std::vector<AssetId> path;
    uint64_t amount_in{0};
    uint64_t min_out{0};
    CScript chn_to;
    //! The result output.
    uint32_t result_n{0};
    //! Where in the payouts of the block's transactions its CHN (out or back) goes; -1 if neither end is CHN.
    int32_t payout{-1};
    //! The pools of its hops (SwapPools); empty: the pool of each two assets of the path (PoolId), as Route and Swap.
    //! Orders live in the store during their block only (State::FinishBlock erases them), never in undo data.
    std::vector<uint256> pools;
    SERIALIZE_METHODS(Order, obj) { READWRITE(obj.txid, obj.path, obj.amount_in, obj.min_out, obj.chn_to, obj.result_n, obj.payout, obj.pools); }
    /** The pool of hop `hop`. */
    uint256 PoolOfHop(size_t hop) const { return pools.empty() ? PoolId(path[hop], path[hop + 1]) : pools[hop]; }
    friend bool operator==(const Order&, const Order&) = default;
};

/** A pool's reserves at the start of a block that changes it, until the block's end. */
struct PoolSnapshot {
    uint64_t reserve0{0};
    uint64_t reserve1{0};
    SERIALIZE_METHODS(PoolSnapshot, obj) { READWRITE(obj.reserve0, obj.reserve1); }
    friend bool operator==(const PoolSnapshot&, const PoolSnapshot&) = default;
};

/** The price accumulators of a pool (Uniswap v2): sums of price (UQ64.64) times seconds, modulo 2^256. */
struct PoolAccumulator {
    //! The pool they are of (Pool::creation): a pool made again under the same id starts afresh.
    Txid creation;
    uint32_t last_time{0};
    //! The price of asset0 in asset1 (reserve1 / reserve0), and of asset1 in asset0, times seconds.
    uint256 price0_cumulative;
    uint256 price1_cumulative;
    //! Observations ever written: the next goes to count % TWAP_OBSERVATIONS.
    uint64_t count{0};
    SERIALIZE_METHODS(PoolAccumulator, obj) { READWRITE(obj.creation, obj.last_time, obj.price0_cumulative, obj.price1_cumulative, obj.count); }
    friend bool operator==(const PoolAccumulator&, const PoolAccumulator&) = default;
};

/** The accumulators at the time of a block that changed the pool, and the reserves the block left. */
struct Observation {
    int32_t height{0};
    uint32_t time{0};
    uint256 price0_cumulative;
    uint256 price1_cumulative;
    uint64_t reserve0{0};
    uint64_t reserve1{0};
    SERIALIZE_METHODS(Observation, obj) { READWRITE(obj.height, obj.time, obj.price0_cumulative, obj.price1_cumulative, obj.reserve0, obj.reserve1); }
    friend bool operator==(const Observation&, const Observation&) = default;
};

struct OracleRecord {
    std::string name;
    uint256 pubkey;
    Txid registration;
    int32_t height{0};
    uint32_t attestations{0};
    SERIALIZE_METHODS(OracleRecord, obj) { READWRITE(obj.name, obj.pubkey, obj.registration, obj.height, obj.attestations); }
    friend bool operator==(const OracleRecord&, const OracleRecord&) = default;
};

struct Attestation {
    int64_t value{0};
    //! The height it was signed for, and that of the block that has it.
    int32_t signed_height{0};
    int32_t height{0};
    Txid txid;
    SERIALIZE_METHODS(Attestation, obj) { READWRITE(obj.value, obj.signed_height, obj.height, obj.txid); }
    friend bool operator==(const Attestation&, const Attestation&) = default;
};

struct OracleContract {
    uint256 oracle;
    uint256 event;
    AssetId asset;
    uint64_t amount{0};
    std::vector<OracleOutcome> outcomes;
    CScript refund;
    int32_t expiry{0};
    int32_t created{0};
    //! Settled: the script it paid, and the block.
    bool settled{false};
    CScript paid_to;
    int32_t settled_height{0};
    SERIALIZE_METHODS(OracleContract, obj) { READWRITE(obj.oracle, obj.event, obj.asset, obj.amount, obj.outcomes, obj.refund, obj.expiry, obj.created, obj.settled, obj.paid_to, obj.settled_height); }
    friend bool operator==(const OracleContract&, const OracleContract&) = default;
    /** Who gets the collateral given the attestation (if any) in a block at `height`; nullopt while it is pending. */
    std::optional<CScript> Winner(const std::optional<Attestation>& attestation, int height) const;
};

namespace dex {

/** An order's hop in a round of the clearing: which side it pays in, and how much. */
struct HopIn {
    bool zero_in{false};
    uint64_t amount{0};
};
/** A round of a pool: what each order gets, in the order given, and the reserves after. */
struct Clearing {
    std::vector<uint64_t> outs;
    uint64_t reserve0{0};
    uint64_t reserve1{0};
    //! The protocol's part of the fees (PROTOCOL_FEE_SHARE): always 0, in the reserves above; a hook.
    uint64_t protocol0{0};
    uint64_t protocol1{0};
};
/**
 * The uniform price of a round of a pool. With A' and B' the inputs of asset0 and asset1 less the fee
 * (997/1000 of them), every order gets the price P = (R1 + B') / (R0 + A') of asset0 in asset1: one paying
 * a of asset0 gets floor(a·997/1000 · P) of asset1, one paying b of asset1 floor(b·997/1000 / P) of
 * asset0. The pool's net trade is then on its curve (the product of the reserves, the fees aside, is
 * that before, exactly), every order gets the same price, and one order alone gets what amm::SwapOut
 * gives. Rounding is down: the product of the reserves never falls. nullopt if a reserve is zero or a
 * sum overflows (MAX_AMOUNT).
 */
std::optional<Clearing> ClearPool(uint64_t reserve0, uint64_t reserve1, const std::vector<HopIn>& orders);

/** reserve_num / reserve_den as UQ64.64 (zero if the denominator is). */
uint256 PriceQ64(uint64_t reserve_num, uint64_t reserve_den);
/** cumulative + price · seconds, modulo 2^256. */
uint256 Accumulate(const uint256& cumulative, const uint256& price, uint32_t seconds);
/** later - earlier, modulo 2^256: what was added between two readings, whatever wrapped. */
uint256 CumulativeDelta(const uint256& later, const uint256& earlier);
/** The cumulative price at `time`, from an observation at or before it: the reserves it left held since (the prices of a pool of that kind, curves::SpotPriceQ64). */
std::pair<uint256, uint256> CumulativeAt(const Observation& obs, uint32_t time, const PoolParams& params = {});
/** The prices of a pool at its reserves: of asset0 in asset1, and of asset1 in asset0 (UQ64.64; the reserves' ratios for constant product). */
std::pair<uint256, uint256> PricesQ64(const PoolParams& params, uint64_t reserve0, uint64_t reserve1);
/** The average of a price between two cumulative readings `seconds` apart, as a decimal number of the other asset per unit. */
double AveragePrice(const uint256& delta, uint32_t seconds);

/** The id of an oracle: the tagged hash of its key (its name is a label, which others may share). */
uint256 OracleId(const uint256& pubkey);
/** What an oracle's key signs (BIP340) to register: the tag, the chain's first block, the name, the key. */
uint256 RegistrationHash(const std::string& name, const uint256& pubkey, const uint256& genesis);
/** Whether `proof` is the key's signature over RegistrationHash. */
bool CheckRegistration(const std::string& name, const uint256& pubkey, const uint256& genesis, const std::array<unsigned char, 64>& proof);
/** The id of an event, by its description (any text). */
uint256 EventId(const std::string& event);
/** What an oracle signs (BIP340) to attest `value` for `event` from `height` on. */
uint256 AttestationHash(const uint256& oracle, const uint256& event, int64_t value, int32_t height);
/** Whether `signature` is the oracle's key's over AttestationHash. */
bool CheckAttestation(const uint256& pubkey, const uint256& oracle, const uint256& event, int64_t value, int32_t height, const std::array<unsigned char, 64>& signature);
/** Whether a name can name an oracle: as an asset's (IsAssetName). */
bool IsOracleName(const std::string& name);

/** What a route gives, alone, against these pools: each hop's output (the last is what comes out); nullopt if a pool is missing, closed, or a hop gives nothing. */
std::optional<std::vector<uint64_t>> QuoteRoute(const State& state, const std::vector<AssetId>& path, uint64_t amount_in);
/**
 * The same through named pools (SwapPools), from `asset_in`: `path` gets the assets of the route; nullopt
 * if a pool is missing, closed, does not hold the asset the hop before gave, or a hop gives nothing.
 */
std::optional<std::vector<uint64_t>> QuoteRoutePools(const State& state, const AssetId& asset_in, const std::vector<uint256>& pools, uint64_t amount_in,
                                                      std::vector<AssetId>* path = nullptr);
/** The assets a route through named pools goes through, from `asset_in`; nullopt if the pools do not chain (or repeat an asset). */
std::optional<std::vector<AssetId>> PathOfPools(const State& state, const AssetId& asset_in, const std::vector<uint256>& pools);

/** A route the search found, and what it gives alone. */
struct FoundRoute {
    std::vector<AssetId> path;
    std::vector<uint64_t> outs;
    //! The pool of each hop (of any kind).
    std::vector<uint256> pools;
    /** Whether every hop is through the pool of the kind AddLiquidity makes (a Route can name it). */
    bool legacy{true};
};
/**
 * The routes from `from` to `to` of at most `max_hops` pools, over the pools there are (of every kind, when
 * `curves`: the DEX's pool kinds apply), best (most out) first, ties to fewer hops; at most `max_results`. Bounded: at most `max_visits` partial paths are
 * looked at, and of each asset its `max_pools_per_asset` deepest pools (by its reserve). For the RPC.
 */
std::vector<FoundRoute> FindRoutes(const State& state, const AssetId& from, const AssetId& to, uint64_t amount_in, size_t max_hops, size_t max_results = 5,
                                   size_t max_visits = 20000, size_t max_pools_per_asset = 16, bool curves = true);

/** The fee a pool charges in the block being made (of FEE_DENOMINATOR): its kind's. */
uint32_t PoolFee(const PoolParams& params);

} // namespace dex
} // namespace bitassets

#endif // BITCOIN_BITASSETS_DEX_H
