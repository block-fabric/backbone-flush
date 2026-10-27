// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitassets/market.h>

#include <arith_uint256.h>
#include <bitassets/clmath.h>
#include <bitassets/curves.h>
#include <bitassets/index.h>
#include <crypto/sha256.h>
#include <hash.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <set>

namespace bitassets::market {

namespace {

using Bytes = sidechain::StoreBytes;

std::atomic<bool> g_enabled{false};

//! The tables of the state the index reads the journal of (bitassets/state.cpp).
constexpr uint8_t ST_TOKENS{0x30};
constexpr uint8_t ST_POOLS{0x34};
constexpr uint8_t ST_BOOK_ORDERS{0x98};

//! After DERIVED_TABLE: the market index's letter, then its own.
constexpr unsigned char MARKET{'X'};
constexpr unsigned char K_LAYOUT{'L'};
constexpr unsigned char K_FILL{'f'};
constexpr unsigned char K_OUT{'o'};
constexpr unsigned char K_BALANCE{'b'};
constexpr unsigned char K_RANK{'r'};
constexpr unsigned char K_COUNT{'c'};
constexpr unsigned char K_WALLET{'w'};
constexpr unsigned char K_TRADE_POOL{'t'};
constexpr unsigned char K_TRADE_LAUNCH{'T'};
constexpr unsigned char K_CANDLE_POOL{'k'};
constexpr unsigned char K_CANDLE_LAUNCH{'K'};
constexpr unsigned char K_MARKET_POOL{'p'};
constexpr unsigned char K_MARKET_LAUNCH{'P'};
constexpr unsigned char K_PLACED{'g'};
constexpr unsigned char K_SETTLED{'h'};
constexpr unsigned char K_SWAP{'x'};
constexpr unsigned char K_ORDER{'l'};
constexpr unsigned char K_EVENT{'e'};
constexpr unsigned char K_SETTLE{'y'};
constexpr unsigned char K_BY_SCRIPT{'a'};
constexpr unsigned char K_SCRIPT_COUNT{'n'};

Bytes K(unsigned char sub) { return Bytes{sidechain::DERIVED_TABLE, MARKET, sub}; }
void Put32(Bytes& out, uint32_t v) { sidechain::KeyCodec<uint32_t>::Encode(out, v); }
void Put64(Bytes& out, uint64_t v) { sidechain::KeyCodec<uint64_t>::Encode(out, v); }
void PutHash(Bytes& out, const uint256& v) { out.insert(out.end(), v.begin(), v.end()); }

/** A script in a key: its length and bytes (64 bytes at most), else 0xFF and its hash (the index keeps the script beside). */
Bytes ScriptKey(const CScript& script)
{
    Bytes out;
    if (script.size() <= 64) {
        out.push_back(static_cast<unsigned char>(script.size()));
        out.insert(out.end(), script.begin(), script.end());
    } else {
        uint256 hash;
        CSHA256().Write(script.data(), script.size()).Finalize(hash.begin());
        out.push_back(0xFF);
        PutHash(out, hash);
    }
    return out;
}

/** What an output that carries a token is, kept until it is spent (its script is not in the state). */
struct OutRow {
    CScript script;
    Token token;
    SERIALIZE_METHODS(OutRow, obj) { READWRITE(obj.script, obj.token); }
};

Bytes OutKey(const COutPoint& out)
{
    Bytes k{K(K_OUT)};
    PutHash(k, out.hash.ToUint256());
    Put32(k, out.n);
    return k;
}
Bytes BalanceKey(const uint256& id, uint8_t kind, const Bytes& script)
{
    Bytes k{K(K_BALANCE)};
    PutHash(k, id);
    k.push_back(kind);
    k.insert(k.end(), script.begin(), script.end());
    return k;
}
Bytes RankKey(const uint256& id, uint8_t kind, uint64_t amount, const Bytes& script)
{
    Bytes k{K(K_RANK)};
    PutHash(k, id);
    k.push_back(kind);
    Put64(k, ~amount);
    k.insert(k.end(), script.begin(), script.end());
    return k;
}
Bytes CountKey(const uint256& id, uint8_t kind)
{
    Bytes k{K(K_COUNT)};
    PutHash(k, id);
    k.push_back(kind);
    return k;
}
Bytes WalletKey(const Bytes& script, const uint256& id, uint8_t kind)
{
    Bytes k{K(K_WALLET)};
    k.insert(k.end(), script.begin(), script.end());
    PutHash(k, id);
    k.push_back(kind);
    return k;
}
Bytes TradePrefix(const Market& m)
{
    Bytes k{K(m.launch ? K_TRADE_LAUNCH : K_TRADE_POOL)};
    PutHash(k, m.id);
    return k;
}
Bytes TradeKey(const Market& m, int32_t height, uint32_t seq)
{
    Bytes k{TradePrefix(m)};
    Put32(k, ~static_cast<uint32_t>(height));
    Put32(k, ~seq);
    return k;
}
Bytes CandlePrefix(const Market& m, Interval interval)
{
    Bytes k{K(m.launch ? K_CANDLE_LAUNCH : K_CANDLE_POOL)};
    PutHash(k, m.id);
    k.push_back(static_cast<unsigned char>(interval));
    return k;
}
Bytes CandleKey(const Market& m, Interval interval, uint32_t t)
{
    Bytes k{CandlePrefix(m, interval)};
    Put32(k, t);
    return k;
}
Bytes MarketKey(const Market& m)
{
    Bytes k{K(m.launch ? K_MARKET_LAUNCH : K_MARKET_POOL)};
    PutHash(k, m.id);
    return k;
}

Bytes TxKey(unsigned char sub, const Txid& txid)
{
    Bytes k{K(sub)};
    PutHash(k, txid.ToUint256());
    return k;
}
Bytes EventKey(const Txid& order, int32_t height, uint32_t seq)
{
    Bytes k{TxKey(K_EVENT, order)};
    Put32(k, static_cast<uint32_t>(height));
    Put32(k, seq);
    return k;
}
Bytes ByScriptPrefix(const CScript& script)
{
    Bytes k{K(K_BY_SCRIPT)};
    const Bytes skey{ScriptKey(script)};
    k.insert(k.end(), skey.begin(), skey.end());
    return k;
}
Bytes ScriptCountKey(const CScript& script)
{
    Bytes k{K(K_SCRIPT_COUNT)};
    const Bytes skey{ScriptKey(script)};
    k.insert(k.end(), skey.begin(), skey.end());
    return k;
}

template <typename V>
std::optional<V> GetValue(const sidechain::StoreView& view, const Bytes& key)
{
    const auto bytes{view.Get(key)};
    if (!bytes) return std::nullopt;
    return sidechain::DecodeValue<V>(*bytes);
}

uint64_t GetU64(const sidechain::StoreView& view, const Bytes& key) { return GetValue<uint64_t>(view, key).value_or(0); }

uint8_t DecimalsOf(const State& state, const AssetId& asset)
{
    if (asset.IsNull()) return 8;
    const auto record{state.GetAsset(asset)};
    return record ? record->decimals : 0;
}

/** Add `delta` (signed) to what `script` holds of a token, keeping the orders by token and by script and the count. */
void AddBalance(sidechain::StoreOverlay& store, const CScript& script, const Token& token, bool add)
{
    const Bytes skey{ScriptKey(script)};
    const uint8_t kind{static_cast<uint8_t>(token.kind)};
    const Bytes bkey{BalanceKey(token.id, kind, skey)};
    const uint64_t before{GetU64(store, bkey)};
    uint64_t after;
    if (add) {
        after = before + std::min(token.amount, MAX_AMOUNT - std::min(before, MAX_AMOUNT));
    } else {
        after = before - std::min(before, token.amount);
    }
    if (after == before) return;
    if (before > 0) store.Erase(RankKey(token.id, kind, before, skey));
    if (after > 0) {
        store.Put(bkey, sidechain::EncodeValue(after));
        store.Put(RankKey(token.id, kind, after, skey), sidechain::EncodeValue(script.size() > 64 ? script : CScript{}));
        store.Put(WalletKey(skey, token.id, kind), sidechain::EncodeValue(after));
    } else {
        store.Erase(bkey);
        store.Erase(WalletKey(skey, token.id, kind));
    }
    if ((before == 0) != (after == 0)) {
        const Bytes ckey{CountKey(token.id, kind)};
        const uint64_t count{GetU64(store, ckey)};
        const uint64_t now{after > 0 ? count + 1 : count - std::min<uint64_t>(count, 1)};
        if (now > 0) {
            store.Put(ckey, sidechain::EncodeValue(now));
        } else {
            store.Erase(ckey);
        }
    }
}

/** The launch a pool trades for, if any: its curve, or the pool it graduated into (the default pool of the token and the coin). */
std::optional<AssetId> LaunchOfPool(const State& state, const uint256& id, const Pool& pool)
{
    if (!pool.asset0.IsNull()) return std::nullopt;
    const auto launch{state.GetLaunch(pool.asset1)};
    if (!launch) return std::nullopt;
    if (launch->pool == id || id == PoolId(CHN, pool.asset1) || state.GetPoolParams(id).GetCurve() == Curve::LAUNCH) return pool.asset1;
    return std::nullopt;
}

/** An order's outcome under a script: newest first (its block, then its id), and the script's count. */
void AddByScript(sidechain::StoreOverlay& store, const CScript& script, int32_t height, const Txid& txid)
{
    if (script.empty()) return;
    Bytes key{ByScriptPrefix(script)};
    Put32(key, ~static_cast<uint32_t>(height));
    PutHash(key, txid.ToUint256());
    if (store.Contains(key)) return;
    store.Put(key, {});
    const Bytes ckey{ScriptCountKey(script)};
    store.Put(ckey, sidechain::EncodeValue(GetU64(store, ckey) + 1));
}

/** Drain the entries noted under `sub` this block, in key order. */
template <typename V>
std::vector<V> TakeNotes(sidechain::StoreOverlay& store, unsigned char sub)
{
    std::vector<Bytes> keys;
    std::vector<V> notes;
    store.ForEach(K(sub), [&](const Bytes& key, const Bytes& value) {
        keys.push_back(key);
        notes.push_back(sidechain::DecodeValue<V>(value));
        return true;
    });
    for (const Bytes& key : keys) store.Erase(key);
    return notes;
}

std::vector<OrderEvent> EventsOf(const sidechain::StoreView& view, const Txid& order, size_t limit)
{
    std::vector<OrderEvent> events;
    view.ForEach(TxKey(K_EVENT, order), [&](const Bytes&, const Bytes& value) {
        if (events.size() >= limit) return false;
        events.push_back(sidechain::DecodeValue<OrderEvent>(value));
        return true;
    });
    return events;
}

} // namespace

void SetEnabled(bool enabled) { g_enabled = enabled; }
bool Enabled() { return g_enabled.load(); }

std::optional<Interval> ParseInterval(const std::string& text)
{
    if (text == "5m") return Interval::M5;
    if (text == "1h") return Interval::H1;
    if (text == "4h") return Interval::H4;
    if (text == "1d") return Interval::D1;
    return std::nullopt;
}

bool Present(const sidechain::StoreView& view)
{
    const Bytes prefix{sidechain::DERIVED_TABLE, MARKET};
    return view.Next(prefix, prefix).has_value();
}

bool LayoutCurrent(const sidechain::StoreView& view)
{
    if (!Enabled()) return true;
    if (const auto layout{GetValue<uint32_t>(view, K(K_LAYOUT))}) return *layout == MARKET_LAYOUT;
    // Nothing indexed yet: nothing missed, if there is nothing to index (no asset ever registered).
    return index::GetInfo(view).assets == 0;
}

size_t Wipe(sidechain::StoreOverlay& store)
{
    const Bytes prefix{sidechain::DERIVED_TABLE, MARKET};
    std::vector<Bytes> keys;
    store.ForEach(prefix, [&](const Bytes& key, const Bytes&) {
        keys.push_back(key);
        return true;
    });
    for (const Bytes& key : keys) store.Erase(key);
    return keys.size();
}

void NoteFill(sidechain::StoreOverlay& store, uint32_t index, const Fill& fill)
{
    if (!Enabled()) return;
    Bytes key{K(K_FILL)};
    Put32(key, index);
    store.Put(key, sidechain::EncodeValue(fill));
}

void NoteOrder(sidechain::StoreOverlay& store, const Placed& placed)
{
    if (!Enabled()) return;
    store.Put(TxKey(K_PLACED, placed.order.txid), sidechain::EncodeValue(placed));
}

void NoteSettle(sidechain::StoreOverlay& store, const Settled& settled)
{
    if (!Enabled()) return;
    store.Put(TxKey(K_SETTLED, settled.txid), sidechain::EncodeValue(settled));
}

std::optional<double> PoolPrice(const State& state, const uint256& id, const Pool& pool)
{
    const PoolParams params{state.GetPoolParams(id)};
    const Curve curve{params.GetCurve()};
    // asset0 per asset1, in units.
    double raw{0};
    if (curve == Curve::BOOK) return std::nullopt;
    if (curve == Curve::CONCENTRATED) {
        const auto cl{state.GetClState(id)};
        if (!cl) return std::nullopt;
        const double p01{UintToArith256(cl::PriceQ64(UintToArith256(cl->sqrt_price))).getdouble() / 18446744073709551616.0};
        if (!(p01 > 0)) return std::nullopt;
        raw = 1 / p01;
    } else if (curve == Curve::CONSTANT_PRODUCT || curve == Curve::LAUNCH) {
        if (pool.reserve0 == 0 || pool.reserve1 == 0) return std::nullopt;
        raw = static_cast<double>(pool.reserve0) / static_cast<double>(pool.reserve1);
    } else {
        if (pool.reserve0 == 0 || pool.reserve1 == 0) return std::nullopt;
        const double p01{curves::SpotPrice(params, pool.reserve0, pool.reserve1)};
        if (!(p01 > 0)) return std::nullopt;
        raw = 1 / p01;
    }
    const double whole{raw * std::pow(10.0, int{DecimalsOf(state, pool.asset1)} - int{DecimalsOf(state, pool.asset0)})};
    if (!std::isfinite(whole) || !(whole > 0)) return std::nullopt;
    return whole;
}

void ConnectBlock(sidechain::StoreOverlay& store, const CBlock& block, int height)
{
    if (!Enabled() || !store.Journaling()) return;
    if (!store.Contains(K(K_LAYOUT))) store.Put(K(K_LAYOUT), sidechain::EncodeValue(MARKET_LAYOUT));
    const State state{static_cast<const sidechain::StoreView&>(store)};

    // The block's time for its candles, never before the last block's (a market's own last time keeps them in order).
    const uint32_t time{block.nTime};
    std::map<Txid, const CTransaction*> txs;
    for (const auto& tx : block.vtx) txs.emplace(tx->GetHash(), tx.get());
    const auto script_of{[&](const COutPoint& out) -> CScript {
        const auto it{txs.find(out.hash)};
        if (it == txs.end() || out.n >= it->second->vout.size()) return CScript{};
        return it->second->vout[out.n].scriptPubKey;
    }};

    // Holders: the outputs that carry tokens, created and spent by the block (the journal's tokens, before and now).
    // What the journal says is read whole first: writing to the overlay notes more in it.
    std::map<uint256, std::optional<Pool>> old_pools;
    std::vector<std::pair<COutPoint, std::optional<Token>>> token_changes;
    std::vector<uint256> orders_changed;
    for (const auto& [key, old] : store.Journal().entries) {
        if (key.empty()) continue;
        if (key[0] == ST_BOOK_ORDERS && key.size() == 33) {
            orders_changed.emplace_back(std::span{key}.subspan(1, 32));
            continue;
        }
        if (key[0] == ST_POOLS && key.size() >= 33) {
            const uint256 id{std::span{key}.subspan(1, 32)};
            old_pools.emplace(id, old ? std::optional{sidechain::DecodeValue<Pool>(*old)} : std::nullopt);
            continue;
        }
        if (key[0] != ST_TOKENS || key.size() != 1 + 36) continue;
        std::span<const unsigned char> rest{std::span{key}.subspan(1)};
        token_changes.emplace_back(sidechain::KeyCodec<COutPoint>::Decode(rest), old ? std::optional{sidechain::DecodeValue<Token>(*old)} : std::nullopt);
    }
    for (const auto& [outpoint, before] : token_changes) {
        const std::optional<Token> now{state.GetToken(outpoint)};
        if (before == now) continue;
        const Bytes okey{OutKey(outpoint)};
        if (before) {
            if (const auto row{GetValue<OutRow>(store, okey)}) {
                AddBalance(store, row->script, row->token, /*add=*/false);
            }
            store.Erase(okey);
        }
        if (now) {
            const CScript script{script_of(outpoint)};
            AddBalance(store, script, *now, /*add=*/true);
            store.Put(okey, sidechain::EncodeValue(OutRow{script, *now}));
        }
    }

    // Trades: the batch's orders that filled, a row per pool they went through.
    std::vector<Fill> fills;
    {
        std::vector<Bytes> keys;
        store.ForEach(K(K_FILL), [&](const Bytes& key, const Bytes& value) {
            keys.push_back(key);
            fills.push_back(sidechain::DecodeValue<Fill>(value));
            return true;
        });
        for (const Bytes& key : keys) store.Erase(key);
    }
    // Outcomes: each order of the batch, filled or missed, by its transaction and its scripts (chn_to, its result output's).
    for (const Fill& fill : fills) {
        if (fill.path.empty()) continue;
        const SwapRow row{fill, height, time, script_of(COutPoint{fill.txid, fill.result_n})};
        store.Put(TxKey(K_SWAP, fill.txid), sidechain::EncodeValue(row));
        AddByScript(store, fill.chn_to, height, fill.txid);
        if (row.result != fill.chn_to) AddByScript(store, row.result, height, fill.txid);
    }
    // Limit orders: placed (their owner the output their token went to), settled (collected, or closed), then filled or
    // evicted (what the journal of the orders' table says changed, against each one's row: the batch is the block's end).
    uint32_t event_seq{0};
    for (const Placed& placed : TakeNotes<Placed>(store, K_PLACED)) {
        OrderRow row;
        row.placed = placed;
        row.time = time;
        row.owner = script_of(COutPoint{placed.order.txid, placed.token_n});
        row.remaining = placed.order.remaining;
        row.got = placed.order.got;
        store.Put(TxKey(K_ORDER, placed.order.txid), sidechain::EncodeValue(row));
        AddByScript(store, row.owner, height, placed.order.txid);
    }
    for (const Settled& settled : TakeNotes<Settled>(store, K_SETTLED)) {
        const Bytes okey{TxKey(K_ORDER, settled.order)};
        auto row{GetValue<OrderRow>(store, okey)};
        if (!row) continue;
        store.Put(TxKey(K_SETTLE, settled.txid), sidechain::EncodeValue(settled.order));
        if (settled.closes) {
            row->closed = height;
            row->closed_time = time;
            // A cancel of an order that still traded (not filled, expired or evicted).
            row->cancelled = settled.cancel && settled.remaining > 0 && row->placed.order.expiry >= height && row->evicted == 0;
            row->settle = settled;
        } else {
            OrderEvent event;
            event.kind = OrderEvent::COLLECT;
            event.height = height;
            event.time = time;
            event.got = settled.proceeds;
            event.settle = settled;
            store.Put(EventKey(settled.order, height, event_seq++), sidechain::EncodeValue(event));
            ++row->collects;
        }
        store.Put(okey, sidechain::EncodeValue(*row));
    }
    for (const uint256& id : orders_changed) {
        const auto now{state.GetOrder(id)};
        if (!now) continue;
        const Bytes okey{TxKey(K_ORDER, now->txid)};
        auto row{GetValue<OrderRow>(store, okey)};
        if (!row) continue;
        bool changed{false};
        if (now->remaining < row->remaining || now->got > row->got) {
            OrderEvent event;
            event.height = height;
            event.time = time;
            event.sold = row->remaining - std::min(row->remaining, now->remaining);
            event.got = now->got - std::min(now->got, row->got);
            store.Put(EventKey(now->txid, height, event_seq++), sidechain::EncodeValue(event));
            ++row->fills;
            row->remaining = now->remaining;
            row->got = now->got;
            changed = true;
        }
        if (now->evicted != 0 && row->evicted == 0) {
            row->evicted = now->evicted;
            changed = true;
        }
        if (changed) store.Put(okey, sidechain::EncodeValue(*row));
    }

    std::map<uint256, uint32_t> pool_trades;
    //! What the trades moved of each pool's first asset (in its units): the candles' volume.
    std::map<uint256, uint64_t> pool_volume0;
    //! The same for the launches: the coin their trades moved, and how many.
    std::map<AssetId, std::pair<uint64_t, uint32_t>> launch_volume;
    uint32_t seq{0};
    for (const Fill& fill : fills) {
        if (!fill.filled || fill.amounts.size() != fill.path.size() || fill.pools.size() + 1 != fill.path.size()) continue;
        const CScript to{fill.path.back().IsNull() ? fill.chn_to : script_of(COutPoint{fill.txid, fill.result_n})};
        for (size_t hop{0}; hop < fill.pools.size(); ++hop) {
            TradeRow row;
            row.txid = fill.txid;
            row.height = height;
            row.time = time;
            row.pool = fill.pools[hop];
            row.asset_in = fill.path[hop];
            row.asset_out = fill.path[hop + 1];
            row.amount_in = fill.amounts[hop];
            row.amount_out = fill.amounts[hop + 1];
            row.back = hop == 0 ? fill.back : 0;
            row.to = to;
            row.curve = hop < fill.curves.size() ? fill.curves[hop] : state.GetPoolParams(row.pool).curve;
            row.hops = static_cast<uint8_t>(fill.pools.size());
            const uint32_t s{seq++};
            store.Put(TradeKey(Market{false, row.pool}, height, s), sidechain::EncodeValue(row));
            ++pool_trades[row.pool];
            // The first asset's side of the trade: what went in of it (less what came back), or what came out of it.
            const bool in_is_0{row.asset_in < row.asset_out};
            const uint64_t side0{in_is_0 ? row.amount_in - std::min(row.amount_in, row.back) : row.amount_out};
            pool_volume0[row.pool] = std::min(MAX_AMOUNT, pool_volume0[row.pool] + side0);
            std::optional<AssetId> token;
            if (hop < fill.launches.size() && !fill.launches[hop].IsNull()) {
                token = fill.launches[hop];
            } else if (const auto pool{state.GetPool(row.pool)}) {
                token = LaunchOfPool(state, row.pool, *pool);
            }
            if (token) {
                store.Put(TradeKey(Market{true, *token}, height, s), sidechain::EncodeValue(row));
                // A launch's market is priced in the chain's coin: its side of the trade.
                auto& [coin, count]{launch_volume[*token]};
                coin = std::min(MAX_AMOUNT, coin + side0);
                ++count;
            }
        }
    }

    // Candles: each pool the block changed, at its price after the block; each launch whose curve or pool it changed,
    // at the price of its pool now (the curve, or the pool it graduated into: one market across its graduation).
    const auto close{[&](const Market& m, double price, double volume, uint32_t trades) {
        MarketRow mrow{GetValue<MarketRow>(store, MarketKey(m)).value_or(MarketRow{})};
        const double open{mrow.price > 0 ? mrow.price : price};
        const uint32_t t{std::max(time, mrow.time)};
        for (uint8_t i{0}; i < INTERVAL_SECONDS.size(); ++i) {
            const Interval interval{static_cast<Interval>(i)};
            const uint32_t bucket{t - t % INTERVAL_SECONDS[i]};
            const Bytes ckey{CandleKey(m, interval, bucket)};
            CandleRow c{GetValue<CandleRow>(store, ckey).value_or(CandleRow{bucket, open, std::max(open, price), std::min(open, price), price, 0, 0})};
            c.h = std::max(c.h, price);
            c.l = std::min(c.l, price);
            c.c = price;
            c.v += volume;
            c.n += trades;
            store.Put(ckey, sidechain::EncodeValue(c));
        }
        mrow.price = price;
        mrow.time = t;
        mrow.height = height;
        mrow.trades += trades;
        store.Put(MarketKey(m), sidechain::EncodeValue(mrow));
    }};
    std::set<uint256> pools;
    for (const auto& [id, _] : old_pools) pools.insert(id);
    for (const auto& [id, _] : pool_trades) pools.insert(id);
    std::set<AssetId> launches;
    for (const auto& [token, _] : launch_volume) launches.insert(token);
    for (const uint256& id : pools) {
        const auto pool{state.GetPool(id)};
        if (!pool) continue;
        if (const auto token{LaunchOfPool(state, id, *pool)}) launches.insert(*token);
        const auto price{PoolPrice(state, id, *pool)};
        if (!price) continue;
        // The volume, in asset0: what the block's trades moved of it; for a pool that traded otherwise (a book's
        // resting orders crossing), what its own counts say was traded in of each side, at the price after the block.
        const double d0{std::pow(10.0, DecimalsOf(state, pool->asset0))}, d1{std::pow(10.0, DecimalsOf(state, pool->asset1))};
        double volume{0};
        if (const auto v{pool_volume0.find(id)}; v != pool_volume0.end()) {
            volume = v->second / d0;
        } else {
            const auto it{old_pools.find(id)};
            const std::optional<Pool> before{it != old_pools.end() ? it->second : pool};
            const bool same{!before || (before->asset0 == pool->asset0 && before->asset1 == pool->asset1)};
            const uint64_t was0{before ? before->volume0 : 0}, was1{before ? before->volume1 : 0};
            const uint64_t in0{same && pool->volume0 >= was0 ? pool->volume0 - was0 : 0};
            const uint64_t in1{same && pool->volume1 >= was1 ? pool->volume1 - was1 : 0};
            volume = in0 / d0 + (in1 / d1) * *price;
        }
        close(Market{false, id}, *price, volume, pool_trades.contains(id) ? pool_trades.at(id) : 0);
    }
    for (const AssetId& token : launches) {
        const auto launch{state.GetLaunch(token)};
        const auto pool{launch ? state.GetPool(launch->pool) : std::nullopt};
        if (!pool) continue;
        const auto price{PoolPrice(state, launch->pool, *pool)};
        if (!price) continue;
        const auto it{launch_volume.find(token)};
        close(Market{true, token}, *price, it != launch_volume.end() ? it->second.first / 1e8 : 0, it != launch_volume.end() ? it->second.second : 0);
    }
}

std::vector<TradeRow> Trades(const sidechain::StoreView& view, const Market& market, int32_t before, const std::vector<unsigned char>& cursor, size_t limit,
                             std::vector<unsigned char>& next)
{
    const std::vector<unsigned char> cursor_in{cursor}; // `next` may be the cursor given
    next.clear();
    const Bytes prefix{TradePrefix(market)};
    Bytes from{prefix};
    if (!cursor_in.empty()) {
        if (cursor_in.size() != prefix.size() + 8 || !std::equal(prefix.begin(), prefix.end(), cursor_in.begin())) throw std::invalid_argument("not a cursor of these trades");
        from = Bytes(cursor_in.begin(), cursor_in.end());
    } else if (before > 0) {
        // Trades of blocks below `before`: keys from ~(before - 1) on.
        Put32(from, ~static_cast<uint32_t>(before - 1));
    }
    std::vector<TradeRow> rows;
    while (auto entry{view.Next(from, prefix)}) {
        if (rows.size() >= limit) {
            next = entry->first;
            break;
        }
        rows.push_back(sidechain::DecodeValue<TradeRow>(entry->second));
        from = entry->first;
        from.push_back(0);
    }
    return rows;
}

std::optional<Outcome> GetOutcome(const sidechain::StoreView& view, const Txid& txid, size_t max_events)
{
    Outcome out;
    if ((out.swap = GetValue<SwapRow>(view, TxKey(K_SWAP, txid)))) return out;
    Txid order{txid};
    if (const auto settled{GetValue<Txid>(view, TxKey(K_SETTLE, txid))}) {
        order = *settled;
        out.by_settle = true;
    }
    out.order = GetValue<OrderRow>(view, TxKey(K_ORDER, order));
    if (!out.order) return std::nullopt;
    out.events = EventsOf(view, order, max_events);
    return out;
}

std::vector<Outcome> OutcomesOf(const sidechain::StoreView& view, const CScript& script, const std::vector<unsigned char>& cursor, size_t limit, size_t max_events,
                                std::vector<unsigned char>& next)
{
    const std::vector<unsigned char> cursor_in{cursor}; // `next` may be the cursor given
    next.clear();
    const Bytes prefix{ByScriptPrefix(script)};
    Bytes from{prefix};
    if (!cursor_in.empty()) {
        if (cursor_in.size() != prefix.size() + 36 || !std::equal(prefix.begin(), prefix.end(), cursor_in.begin())) throw std::invalid_argument("not a cursor of these trades");
        from = Bytes(cursor_in.begin(), cursor_in.end());
    }
    std::vector<Outcome> rows;
    while (auto entry{view.Next(from, prefix)}) {
        if (rows.size() >= limit) {
            next = entry->first;
            break;
        }
        if (entry->first.size() == prefix.size() + 36) {
            const Txid txid{Txid::FromUint256(uint256{std::span{entry->first}.subspan(prefix.size() + 4, 32)})};
            if (auto outcome{GetOutcome(view, txid, max_events)}) rows.push_back(std::move(*outcome));
        }
        from = entry->first;
        from.push_back(0);
    }
    return rows;
}

uint64_t OutcomeCount(const sidechain::StoreView& view, const CScript& script) { return GetU64(view, ScriptCountKey(script)); }

std::vector<CandleRow> Candles(const sidechain::StoreView& view, const Market& market, Interval interval, uint32_t from, uint32_t to, size_t limit)
{
    const uint32_t step{INTERVAL_SECONDS[static_cast<size_t>(interval)]};
    if (to < from) return {};
    // The last `limit` buckets up to `to`, not before `from`.
    const uint64_t span{static_cast<uint64_t>(step) * limit};
    const uint32_t start{static_cast<uint32_t>(std::max<uint64_t>(from - from % step, to >= span ? (to - to % step) - (span - step) : 0))};
    const Bytes prefix{CandlePrefix(market, interval)};
    Bytes at{CandleKey(market, interval, start)};
    std::vector<CandleRow> rows;
    while (auto entry{view.Next(at, prefix)}) {
        const CandleRow c{sidechain::DecodeValue<CandleRow>(entry->second)};
        if (c.t > to || rows.size() >= limit) break;
        rows.push_back(c);
        at = entry->first;
        at.push_back(0);
    }
    return rows;
}

std::optional<MarketRow> GetMarket(const sidechain::StoreView& view, const Market& market) { return GetValue<MarketRow>(view, MarketKey(market)); }

std::vector<Holding> Holders(const sidechain::StoreView& view, const uint256& id, Token::Kind kind, const std::vector<unsigned char>& cursor, size_t limit,
                             std::vector<unsigned char>& next)
{
    const std::vector<unsigned char> cursor_in{cursor}; // `next` may be the cursor given
    next.clear();
    Bytes prefix{K(K_RANK)};
    PutHash(prefix, id);
    prefix.push_back(static_cast<uint8_t>(kind));
    Bytes from{prefix};
    if (!cursor_in.empty()) {
        if (cursor_in.size() <= prefix.size() + 8 || !std::equal(prefix.begin(), prefix.end(), cursor_in.begin())) throw std::invalid_argument("not a cursor of these holders");
        from = Bytes(cursor_in.begin(), cursor_in.end());
    }
    std::vector<Holding> rows;
    while (auto entry{view.Next(from, prefix)}) {
        if (rows.size() >= limit) {
            next = entry->first;
            break;
        }
        std::span<const unsigned char> rest{std::span{entry->first}.subspan(prefix.size())};
        Holding h;
        h.id = id;
        h.kind = kind;
        h.amount = ~sidechain::KeyCodec<uint64_t>::Decode(rest);
        if (!rest.empty() && rest[0] != 0xFF && rest.size() == size_t{1} + rest[0]) {
            h.script = CScript(rest.begin() + 1, rest.end());
        } else {
            h.script = sidechain::DecodeValue<CScript>(entry->second);
        }
        rows.push_back(std::move(h));
        from = entry->first;
        from.push_back(0);
    }
    return rows;
}

uint64_t HolderCount(const sidechain::StoreView& view, const uint256& id, Token::Kind kind) { return GetU64(view, CountKey(id, static_cast<uint8_t>(kind))); }

std::vector<Holding> HoldingsOf(const sidechain::StoreView& view, const CScript& script, size_t limit)
{
    Bytes prefix{K(K_WALLET)};
    const Bytes skey{ScriptKey(script)};
    prefix.insert(prefix.end(), skey.begin(), skey.end());
    std::vector<Holding> rows;
    view.ForEach(prefix, [&](const Bytes& key, const Bytes& value) {
        if (rows.size() >= limit || key.size() != prefix.size() + 33) return false;
        Holding h;
        std::copy(key.begin() + prefix.size(), key.begin() + prefix.size() + 32, h.id.begin());
        h.kind = static_cast<Token::Kind>(key.back());
        h.amount = sidechain::DecodeValue<uint64_t>(value);
        h.script = script;
        rows.push_back(std::move(h));
        return true;
    });
    return rows;
}

} // namespace bitassets::market
