// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BITASSETS_MARKET_H
#define BITCOIN_BITASSETS_MARKET_H

#include <bitassets/state.h>
#include <primitives/block.h>
#include <script/script.h>
#include <serialize.h>
#include <sidechain/store.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/**
 * The DEX's market index (doc/public-api.md): what a market data API serves and the state does not keep, derived
 * block by block, for the nodes that keep it (-dexindex; on with -publicapi).
 *
 *  - **Trades**: every order of a block's batch that filled (a swap, a route, a launch buy or sale), each pool it went
 *    through with what went in and came out of it, the block, its time and who got what came out; by pool, and by
 *    launch (its curve and the pool it graduated into), newest first.
 *  - **Candles** (OHLCV) of each pool's price, and of each launch's (its curve's, then its pool's), at 5 minutes, an
 *    hour, 4 hours and a day: each block that changes a pool closes the candle of its time at the pool's price after
 *    the block, the volume (what the pool's own volumes counted) in its first asset.
 *  - **Holders**: what each script holds of each token (asset coins, liquidity shares, positions, orders), by token
 *    (largest first, and how many hold it) and by script (an address's holdings), from the outputs that carry tokens.
 *
 * Like the search index of the assets (bitassets/index.h) it is derived data kept with the state, under the byte
 * sidechain::DERIVED_TABLE (then 'X', the index's other letters being its own), written at the end of each block in the
 * block's overlay: flushed with the state, undone with it by a reorganisation (the block's undo data has its keys),
 * left out of the state's hash, read by no rule. The batch's per-order outcomes, which the state does not keep, are
 * noted while the block clears (NoteFill, from State::FinishBlock) and taken by ConnectBlock in the same block.
 *
 * A node that turns it on derives the sidechain state again from the blocks once (LayoutCurrent); one that turns it
 * off erases it (Wipe).
 */
namespace bitassets::market {

/** The layout of the market index (1: the first). */
inline constexpr uint32_t MARKET_LAYOUT{1};

/** Whether this node keeps the index (-dexindex). Set before the chainstate is loaded. */
void SetEnabled(bool enabled);
bool Enabled();

/** Whether the index in `view` is complete for its blocks: kept and of this layout, or nothing to index yet. True if not kept. */
bool LayoutCurrent(const sidechain::StoreView& view);
/** Whether a store holds any of it. */
bool Present(const sidechain::StoreView& view);
/** Erase all of it (a node that no longer keeps it). @return the entries erased. */
size_t Wipe(sidechain::StoreOverlay& store);

/** An order of the batch, as it cleared: noted by FinishBlock, taken by ConnectBlock. */
struct Fill {
    Txid txid;
    uint32_t result_n{0};
    CScript chn_to;
    std::vector<AssetId> path;
    //! The pool of each hop.
    std::vector<uint256> pools;
    //! What went into the first hop, then what came out of each hop (one more than the hops if it filled).
    std::vector<uint64_t> amounts;
    bool filled{false};
    //! The chain's coin given back (a launch buy past the curve's end).
    uint64_t back{0};
    //! Each hop's pool's curve, and the token whose launch curve it is (null for another pool): as the batch found them
    //! (a curve that graduates in the block is gone after it).
    std::vector<uint8_t> curves;
    std::vector<AssetId> launches;
    SERIALIZE_METHODS(Fill, obj) { READWRITE(obj.txid, obj.result_n, obj.chn_to, obj.path, obj.pools, obj.amounts, obj.filled, obj.back, obj.curves, obj.launches); }
};
void NoteFill(sidechain::StoreOverlay& store, uint32_t index, const Fill& fill);

/** Bring the index up to date with a block connected at `height` (after State::FinishBlock and the search index). */
void ConnectBlock(sidechain::StoreOverlay& store, const CBlock& block, int height);

/** A trade: an order's passage through one pool. */
struct TradeRow {
    Txid txid;
    int32_t height{0};
    uint32_t time{0};
    uint256 pool;
    AssetId asset_in;
    AssetId asset_out;
    uint64_t amount_in{0};
    uint64_t amount_out{0};
    //! What the order got back of what it paid in (a launch buy at the curve's end).
    uint64_t back{0};
    //! Who got what the order brought out.
    CScript to;
    //! The pool's curve (Curve), and the order's hops.
    uint8_t curve{0};
    uint8_t hops{1};
    SERIALIZE_METHODS(TradeRow, obj) { READWRITE(obj.txid, obj.height, obj.time, obj.pool, obj.asset_in, obj.asset_out, obj.amount_in, obj.amount_out, obj.back, obj.to, obj.curve, obj.hops); }
};

/** Candle intervals. */
enum class Interval : uint8_t { M5 = 0, H1 = 1, H4 = 2, D1 = 3 };
inline constexpr std::array<uint32_t, 4> INTERVAL_SECONDS{300, 3600, 14400, 86400};
std::optional<Interval> ParseInterval(const std::string& text);

struct CandleRow {
    uint32_t t{0};
    double o{0}, h{0}, l{0}, c{0};
    //! Volume in the pool's first asset (whole units).
    double v{0};
    uint32_t n{0};
    template <typename Stream>
    void Serialize(Stream& s) const { s << t << std::bit_cast<uint64_t>(o) << std::bit_cast<uint64_t>(h) << std::bit_cast<uint64_t>(l) << std::bit_cast<uint64_t>(c) << std::bit_cast<uint64_t>(v) << n; }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        uint64_t a, b, c_, d, e;
        s >> t >> a >> b >> c_ >> d >> e >> n;
        o = std::bit_cast<double>(a); h = std::bit_cast<double>(b); l = std::bit_cast<double>(c_); c = std::bit_cast<double>(d); v = std::bit_cast<double>(e);
    }
};

/** The last price of a market (a pool, or a launch across its curve and pool), and its counts. */
struct MarketRow {
    double price{0};
    uint32_t time{0};
    int32_t height{0};
    uint64_t trades{0};
    template <typename Stream>
    void Serialize(Stream& s) const { s << std::bit_cast<uint64_t>(price) << time << height << trades; }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        uint64_t p;
        s >> p >> time >> height >> trades;
        price = std::bit_cast<double>(p);
    }
};

/** A market: a pool's, or a launch's (its token; the curve's trades and candles, then its graduated pool's). */
struct Market {
    bool launch{false};
    uint256 id;
};

/**
 * A page of a market's trades, newest first: below height `before` (0: any), after `cursor` (from a page before).
 * @param[out] next  the cursor of the next page (empty at the end)
 */
std::vector<TradeRow> Trades(const sidechain::StoreView& view, const Market& market, int32_t before, const std::vector<unsigned char>& cursor, size_t limit,
                             std::vector<unsigned char>& next);
/** The candles of a market from `from` to `to` (times), oldest first, at most `limit`: the last ones if more. */
std::vector<CandleRow> Candles(const sidechain::StoreView& view, const Market& market, Interval interval, uint32_t from, uint32_t to, size_t limit);
std::optional<MarketRow> GetMarket(const sidechain::StoreView& view, const Market& market);

/** What a script holds of a token, the script as the index keeps it. */
struct Holding {
    uint256 id;
    Token::Kind kind{Token::Kind::ASSET};
    uint64_t amount{0};
    CScript script;
};
/** The holders of a token, largest first, a page at a time; and how many there are. */
std::vector<Holding> Holders(const sidechain::StoreView& view, const uint256& id, Token::Kind kind, const std::vector<unsigned char>& cursor, size_t limit,
                             std::vector<unsigned char>& next);
uint64_t HolderCount(const sidechain::StoreView& view, const uint256& id, Token::Kind kind);
/** What a script holds, by token (at most `limit`). */
std::vector<Holding> HoldingsOf(const sidechain::StoreView& view, const CScript& script, size_t limit);

/** The price of a pool now, in whole units of asset0 per whole asset1; nullopt for a book or an empty pool. */
std::optional<double> PoolPrice(const State& state, const uint256& id, const Pool& pool);

} // namespace bitassets::market

#endif // BITCOIN_BITASSETS_MARKET_H
