// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitassets/dex.h>

#include <bitassets/curves.h>

#include <arith_uint256.h>
#include <hash.h>
#include <pubkey.h>

#include <algorithm>
#include <functional>

namespace bitassets {

std::optional<CScript> OracleContract::Winner(const std::optional<Attestation>& attestation, int height) const
{
    // An attestation counts if a block up to the expiry has it: after that, the refund is due.
    if (attestation && attestation->height <= expiry) {
        for (const OracleOutcome& outcome : outcomes) {
            if (attestation->value >= outcome.lo && attestation->value <= outcome.hi) return outcome.script;
        }
        return refund;
    }
    if (height > expiry) return refund;
    return std::nullopt;
}

namespace dex {

std::optional<Clearing> ClearPool(uint64_t reserve0, uint64_t reserve1, const std::vector<HopIn>& orders)
{
    if (reserve0 == 0 || reserve1 == 0 || reserve0 > MAX_AMOUNT || reserve1 > MAX_AMOUNT) return std::nullopt;
    uint64_t in0{0}, in1{0};
    for (const HopIn& order : orders) {
        if (order.amount == 0 || order.amount > MAX_AMOUNT) return std::nullopt;
        uint64_t& sum{order.zero_in ? in0 : in1};
        if (order.amount > MAX_AMOUNT - sum) return std::nullopt;
        sum += order.amount;
    }
    // What goes in stays in the pool, fee and all: the reserves after hold it.
    if (in0 > MAX_AMOUNT - reserve0 || in1 > MAX_AMOUNT - reserve1) return std::nullopt;
    // In thousandths: 1000·(R1 + B') and 1000·(R0 + A'), the price of asset0 in asset1 their ratio. In
    // 256 bits: an amount (63 bits) times 997 (10 bits) times one of them (74 bits) needs 147.
    const arith_uint256 keep{1000 - SWAP_FEE_PER_MILLE};
    const arith_uint256 num{arith_uint256{reserve1} * 1000 + arith_uint256{in1} * keep};
    const arith_uint256 den{arith_uint256{reserve0} * 1000 + arith_uint256{in0} * keep};
    Clearing clearing;
    uint64_t out0{0}, out1{0};
    for (const HopIn& order : orders) {
        const arith_uint256 paid{arith_uint256{order.amount} * keep};
        // Down: the buyer never gets more than the price gives.
        const arith_uint256 out{order.zero_in ? paid * num / (den * 1000) : paid * den / (num * 1000)};
        // Less than a reserve after (doc above), so it fits.
        const uint64_t got{out.GetLow64()};
        clearing.outs.push_back(got);
        uint64_t& sum{order.zero_in ? out1 : out0};
        if (got > MAX_AMOUNT - sum) return std::nullopt;
        sum += got;
    }
    // Never more out than the pool holds: the exact amounts leave both reserves positive, the rounding more.
    if (out0 >= reserve0 + in0 || out1 >= reserve1 + in1) return std::nullopt;
    clearing.reserve0 = reserve0 + in0 - out0;
    clearing.reserve1 = reserve1 + in1 - out1;
    return clearing;
}

uint256 PriceQ64(uint64_t reserve_num, uint64_t reserve_den)
{
    if (reserve_den == 0) return uint256{};
    return ArithToUint256((arith_uint256{reserve_num} << 64) / arith_uint256{reserve_den});
}

uint256 Accumulate(const uint256& cumulative, const uint256& price, uint32_t seconds)
{
    // arith_uint256 is arithmetic modulo 2^256: the sum wraps, as Uniswap's does.
    return ArithToUint256(UintToArith256(cumulative) + UintToArith256(price) * seconds);
}

uint256 CumulativeDelta(const uint256& later, const uint256& earlier)
{
    return ArithToUint256(UintToArith256(later) - UintToArith256(earlier));
}

std::pair<uint256, uint256> PricesQ64(const PoolParams& params, uint64_t reserve0, uint64_t reserve1)
{
    // Constant product: the reserves' ratios, as they always were; the others the slope of the curve and its inverse, each the floor of an exact quotient.
    return {curves::SpotPriceQ64(params, reserve0, reserve1, /*inverse=*/false), curves::SpotPriceQ64(params, reserve0, reserve1, /*inverse=*/true)};
}

std::pair<uint256, uint256> CumulativeAt(const Observation& obs, uint32_t time, const PoolParams& params)
{
    if (time <= obs.time) return {obs.price0_cumulative, obs.price1_cumulative};
    const uint32_t elapsed{time - obs.time};
    const auto [p0, p1]{PricesQ64(params, obs.reserve0, obs.reserve1)};
    return {Accumulate(obs.price0_cumulative, p0, elapsed), Accumulate(obs.price1_cumulative, p1, elapsed)};
}

uint32_t PoolFee(const PoolParams& params) { return params.fee; }

std::string VerificationEvent(const AssetId& asset, int round) { return "Layers verification " + asset.GetHex() + " #" + std::to_string(round); }

double AveragePrice(const uint256& delta, uint32_t seconds)
{
    if (seconds == 0) return 0;
    return UintToArith256(delta).getdouble() / seconds / 18446744073709551616.0;
}

uint256 OracleId(const uint256& pubkey)
{
    HashWriter writer{};
    writer << std::string{"Layers oracle"} << pubkey;
    return writer.GetSHA256();
}

uint256 RegistrationHash(const std::string& name, const uint256& pubkey, const uint256& genesis)
{
    HashWriter writer{};
    writer << std::string{"Layers oracle registration"} << genesis << name << pubkey;
    return writer.GetSHA256();
}

bool CheckRegistration(const std::string& name, const uint256& pubkey, const uint256& genesis, const std::array<unsigned char, 64>& proof)
{
    const XOnlyPubKey key{pubkey};
    return key.IsFullyValid() && key.VerifySchnorr(RegistrationHash(name, pubkey, genesis), proof);
}

uint256 EventId(const std::string& event)
{
    HashWriter writer{};
    writer << std::string{"Layers oracle event"} << event;
    return writer.GetSHA256();
}

uint256 AttestationHash(const uint256& oracle, const uint256& event, int64_t value, int32_t height)
{
    HashWriter writer{};
    writer << std::string{"Layers oracle attestation"} << oracle << event << value << height;
    return writer.GetSHA256();
}

bool CheckAttestation(const uint256& pubkey, const uint256& oracle, const uint256& event, int64_t value, int32_t height, const std::array<unsigned char, 64>& signature)
{
    const XOnlyPubKey key{pubkey};
    if (!key.IsFullyValid()) return false;
    return key.VerifySchnorr(AttestationHash(oracle, event, value, height), signature);
}

bool IsOracleName(const std::string& name) { return IsAssetName(name); }

namespace {
/** What `amount` of `in` gives alone in a pool of a kind; 0 if nothing (or the pool is closed). A launch curve's from its record (a buy paying in directly). */
uint64_t HopAlone(const State& state, const uint256& id, const Pool& pool, const PoolParams& params, const AssetId& in, uint64_t amount)
{
    if (amm::Abandoned(pool)) return 0;
    if (params.GetCurve() == Curve::BOOK) {
        std::map<uint256, BookOrder> resting;
        // The book's index as it is: the end of each block sweeps the orders that expired out of it (SweepBooks), so it
        // holds live orders only, unless more expired at once than a block's end sweeps (a quote then; min_out guards).
        const auto cleared{state.ClearBook(id, pool, resting, {HopIn{pool.asset0 == in, amount}}, 0)};
        return cleared ? cleared->outs[0] : 0;
    }
    // From fee_flow_height a pool of the chain's coin takes its fee on the coin's side, as its rounds do (State::ClearCoinSide).
    const uint32_t fee{state.QuoteFee(id, params)};
    if (state.CoinSideFees() && pool.asset0.IsNull() && fee > 0 && params.GetCurve() != Curve::LAUNCH) {
        std::optional<ClState> cl;
        std::map<int32_t, ClTick> ticks;
        if (params.GetCurve() == Curve::CONCENTRATED) {
            cl = state.GetClState(id);
            if (!cl) return 0;
        }
        uint64_t security{0};
        const auto cleared{state.ClearCoinSide(id, params, fee, pool, cl ? &*cl : nullptr, &ticks, {HopIn{pool.asset0 == in, amount}}, /*security_share=*/0, security)};
        return cleared ? cleared->outs[0] : 0;
    }
    if (params.GetCurve() == Curve::CONCENTRATED) {
        auto cl{state.GetClState(id)};
        if (!cl) return 0;
        std::map<int32_t, ClTick> ticks;
        const auto cleared{state.ClearCl(id, state.QuoteFee(id, params), pool, *cl, ticks, {HopIn{pool.asset0 == in, amount}})};
        return cleared ? cleared->outs[0] : 0;
    }
    if (params.GetCurve() == Curve::LAUNCH) {
        const auto launch{state.GetLaunch(pool.asset1)};
        if (!launch) return 0;
        const auto cleared{curves::ClearLaunch(curves::LaunchCurve{launch->fee, launch->CurveEnd(), launch->max_buy, launch->min_buy, launch->StrictEnd(), launch->StartBound()}, pool.reserve0, pool.reserve1, {HopIn{pool.asset0 == in, amount}}, {in.IsNull()})};
        return cleared ? cleared->outs[0] : 0;
    }
    // ClearPool's own result for the pools AddLiquidity makes (curves::ClearPoolOf at 0.30 % is it).
    const auto cleared{curves::ClearPoolOf(params, state.QuoteFee(id, params), pool.reserve0, pool.reserve1, {HopIn{pool.asset0 == in, amount}})};
    return cleared ? cleared->outs[0] : 0;
}
} // namespace

std::optional<std::vector<uint64_t>> QuoteRoute(const State& state, const std::vector<AssetId>& path, uint64_t amount_in)
{
    if (path.size() < 2) return std::nullopt;
    std::vector<uint64_t> outs;
    uint64_t amount{amount_in};
    for (size_t i{0}; i + 1 < path.size(); ++i) {
        const auto pool{state.FindPool(path[i], path[i + 1])};
        if (!pool) return std::nullopt;
        amount = HopAlone(state, PoolId(path[i], path[i + 1]), *pool, PoolParams{}, path[i], amount);
        if (amount == 0) return std::nullopt;
        outs.push_back(amount);
    }
    return outs;
}

std::optional<std::vector<AssetId>> PathOfPools(const State& state, const AssetId& asset_in, const std::vector<uint256>& pools)
{
    if (pools.empty() || pools.size() + 1 > MAX_ROUTE_ASSETS) return std::nullopt;
    std::vector<AssetId> path{asset_in};
    for (const uint256& id : pools) {
        const auto pool{state.GetPool(id)};
        if (!pool) return std::nullopt;
        const AssetId& at{path.back()};
        if (pool->asset0 != at && pool->asset1 != at) return std::nullopt;
        const AssetId next{pool->asset0 == at ? pool->asset1 : pool->asset0};
        if (std::find(path.begin(), path.end(), next) != path.end()) return std::nullopt;
        path.push_back(next);
    }
    return path;
}

std::optional<std::vector<uint64_t>> QuoteRoutePools(const State& state, const AssetId& asset_in, const std::vector<uint256>& pools, uint64_t amount_in, std::vector<AssetId>* path_out)
{
    const auto path{PathOfPools(state, asset_in, pools)};
    if (!path) return std::nullopt;
    std::vector<uint64_t> outs;
    uint64_t amount{amount_in};
    for (size_t i{0}; i < pools.size(); ++i) {
        const Pool pool{*Assert(state.GetPool(pools[i]))};
        amount = HopAlone(state, pools[i], pool, state.GetPoolParams(pools[i]), (*path)[i], amount);
        if (amount == 0) return std::nullopt;
        outs.push_back(amount);
    }
    if (path_out) *path_out = *path;
    return outs;
}

std::vector<FoundRoute> FindRoutes(const State& state, const AssetId& from, const AssetId& to, uint64_t amount_in, size_t max_hops, size_t max_results, size_t max_visits,
                                   size_t max_pools_per_asset, bool curves)
{
    std::vector<FoundRoute> found;
    if (from == to || amount_in == 0) return found;
    max_hops = std::min(max_hops, MAX_ROUTE_ASSETS - 1);
    size_t visits{0};
    std::vector<AssetId> path{from};
    std::vector<uint64_t> outs;
    std::vector<uint256> ids;
    std::function<void(uint64_t)> extend{[&](uint64_t amount) {
        if (path.size() - 1 >= max_hops) return;
        const AssetId at{path.back()};
        std::vector<std::pair<uint256, Pool>> pools{state.PoolsOf(at)};
        // Without the pool kinds, only the pool of each two assets AddLiquidity makes.
        if (!curves) std::erase_if(pools, [&](const auto& p) { return p.first != PoolId(p.second.asset0, p.second.asset1); });
        // The deepest pools of the asset first (most of it), then by id.
        const auto depth{[&](const Pool& pool) { return pool.asset0 == at ? pool.reserve0 : pool.reserve1; }};
        std::sort(pools.begin(), pools.end(), [&](const auto& a, const auto& b) {
            if (depth(a.second) != depth(b.second)) return depth(a.second) > depth(b.second);
            return a.first < b.first;
        });
        if (pools.size() > max_pools_per_asset) pools.resize(max_pools_per_asset);
        for (const auto& [id, pool] : pools) {
            if (++visits > max_visits) return;
            const AssetId other{pool.asset0 == at ? pool.asset1 : pool.asset0};
            if (std::find(path.begin(), path.end(), other) != path.end()) continue;
            const uint64_t out{HopAlone(state, id, pool, state.GetPoolParams(id), at, amount)};
            if (out == 0) continue;
            path.push_back(other);
            outs.push_back(out);
            ids.push_back(id);
            if (other == to) {
                FoundRoute route{path, outs, ids, true};
                for (size_t i{0}; i < ids.size(); ++i) route.legacy = route.legacy && ids[i] == PoolId(path[i], path[i + 1]);
                found.push_back(std::move(route));
            } else {
                extend(out);
            }
            path.pop_back();
            outs.pop_back();
            ids.pop_back();
        }
    }};
    extend(amount_in);
    std::sort(found.begin(), found.end(), [](const FoundRoute& a, const FoundRoute& b) {
        if (a.outs.back() != b.outs.back()) return a.outs.back() > b.outs.back();
        if (a.path.size() != b.path.size()) return a.path.size() < b.path.size();
        if (a.path != b.path) return a.path < b.path;
        return a.pools < b.pools;
    });
    if (found.size() > max_results) found.resize(max_results);
    return found;
}

} // namespace dex
} // namespace bitassets
