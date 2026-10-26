// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Inspired by BitAssets by LayerTwo Labs (plain-bitassets).

#include <arith_uint256.h>
#include <bitassets/clmath.h>
#include <bitassets/curves.h>
#include <bitassets/dex.h>
#include <bitassets/index.h>
#include <bitassets/logo.h>
#include <bitassets/market.h>
#include <bitassets/rpcutil.h>
#include <bitassets/state.h>
#include <coins.h>
#include <core_io.h>
#include <key_io.h>
#include <node/context.h>
#include <node/transaction.h>
#include <policy/feerate.h>
#include <rpc/bitassets.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <sidechain/state.h>
#include <sync.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <validation.h>

#include <univalue.h>

#include <cmath>
#include <limits>
#include <locale>
#include <sstream>
#include <memory>

using bitassets::AssetId;
using bitassets::AssetLabel;
using bitassets::AmountToJSON;
using node::NodeContext;

namespace {

bitassets::State StateOf(ChainstateManager& chainman) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    return chainman.ActiveChainstate().SideState().BitAssets();
}

/**
 * The assets' state as of the tip, and the height of the next block, for a read of the DEX (a quote, a pool, a launch):
 * from a snapshot shared by the readers of the same tip (sidechain::StoreOverlay::SharedSnapshot), taken under cs_main
 * and read without it, so that what a quote searches (routes, curves, ticks) does not hold block validation back; under
 * cs_main if no snapshot can be made. The app's pages and the public API (-publicapi) read through these same methods.
 */
template <typename F>
UniValue WithDexState(ChainstateManager& chainman, F&& fn) EXCLUSIVE_LOCKS_REQUIRED(!::cs_main)
{
    std::shared_ptr<const sidechain::StoreView> view;
    int next_height;
    {
        LOCK(::cs_main);
        Chainstate& chainstate{chainman.ActiveChainstate()};
        next_height = chainstate.m_chain.Height() + 1;
        view = chainstate.SideCache().SharedSnapshot();
        if (!view) return fn(StateOf(chainman), next_height);
    }
    return fn(sidechain::State{*view}.BitAssets(), next_height);
}

AssetId ParseAsset(const UniValue& value, const bitassets::State& state)
{
    return bitassets::ParseAssetArg(value, [&](uint32_t seq) { return state.AssetOfSeq(seq); });
}

std::optional<bitassets::AssetRecord> RecordOf(const bitassets::State& state, const AssetId& asset)
{
    return state.GetAsset(asset);
}

std::string LabelOf(const bitassets::State& state, const AssetId& asset)
{
    const auto record{RecordOf(state, asset)};
    return bitassets::AssetLabel(asset, record ? &*record : nullptr);
}

uint8_t DecimalsOf(const bitassets::State& state, const AssetId& asset)
{
    if (asset.IsNull()) return bitassets::CHN_DECIMALS;
    const auto record{RecordOf(state, asset)};
    return record ? record->decimals : 0;
}

/** A registered asset, else an error. */
bitassets::AssetRecord Registered(const bitassets::State& state, const AssetId& asset)
{
    const auto record{RecordOf(state, asset)};
    if (!record) throw JSONRPCError(RPC_INVALID_PARAMETER, "No such asset is registered");
    return *record;
}

/** Where a token is: the output that carries it, and its address. */
UniValue OutputToJSON(Chainstate& chainstate, const COutPoint& outpoint) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("txid", outpoint.hash.GetHex());
    obj.pushKV("vout", outpoint.n);
    if (const auto coin{chainstate.CoinsTip().GetCoin(outpoint)}) {
        CTxDestination dest;
        if (ExtractDestination(coin->out.scriptPubKey, dest)) obj.pushKV("address", EncodeDestination(dest));
    }
    return obj;
}

std::vector<RPCResult> LogoResults()
{
    return {
        {RPCResult::Type::STR_HEX, "hash", "SHA-256 of its bytes"},
        {RPCResult::Type::STR, "mime", "image/png or image/jpeg"},
        {RPCResult::Type::NUM, "size", "Its size in bytes"},
        {RPCResult::Type::NUM, "height", "The height of the block that set it"},
        {RPCResult::Type::STR_HEX, "txid", "The transaction that set it"},
    };
}

UniValue LogoToJSON(const bitassets::LogoRecord& logo)
{
    UniValue obj(UniValue::VOBJ);
    // The hash as sha256sum prints it, of the bytes in order.
    obj.pushKV("hash", HexStr(logo.hash));
    obj.pushKV("mime", bitassets::LogoMime(static_cast<bitassets::LogoFormat>(logo.format)));
    obj.pushKV("size", logo.size);
    obj.pushKV("height", logo.height);
    obj.pushKV("txid", logo.txid.GetHex());
    return obj;
}

std::vector<RPCResult> AssetResults()
{
    return {
        {RPCResult::Type::STR_HEX, "asset", "The hash of its name, by which the chain knows it"},
        {RPCResult::Type::STR, "name", /*optional=*/true, "Its name, if its registration published it: its ticker (from ticker_height a ticker; the names of before as they are)"},
        {RPCResult::Type::STR, "display_name", /*optional=*/true, "Its display name, if it has one (not unique: shown with the ticker, never instead of it)"},
        {RPCResult::Type::STR, "label", "How it is shown: its name, or \"0x\" and its hash"},
        {RPCResult::Type::STR, "seq", "Its number, in the order of registration"},
        {RPCResult::Type::NUM, "decimals", "The decimals it is shown with"},
        {RPCResult::Type::NUM, "supply", "How much there is: all minted, less all burned"},
        {RPCResult::Type::NUM, "minted", "All ever minted, the initial supply included"},
        {RPCResult::Type::NUM, "burned", "All ever burned"},
        {RPCResult::Type::BOOL, "fixed", "Whether the control coin was burned: nothing more can be minted, the data cannot change"},
        {RPCResult::Type::NUM, "registered", "The height of the block that registered it"},
        {RPCResult::Type::STR_HEX, "registration", "The transaction that registered it"},
        {RPCResult::Type::OBJ, "data", /*optional=*/true, "Its data; left out at a height before its registration", bitassets::DataResults()},
        {RPCResult::Type::BOOL, "releasable", "Whether it is dead and can be retired (releaseasset): fixed supply, every unit in pools nobody provides for"},
        {RPCResult::Type::NUM, "release_fee", /*optional=*/true, "If releasable: the " + CURRENCY_UNIT + " its pools hold, which retiring it pays to mainchain miners"},
        {RPCResult::Type::OBJ, "control", /*optional=*/true, "The output that carries its control coin, unless burned",
        {
            {RPCResult::Type::STR_HEX, "txid", ""},
            {RPCResult::Type::NUM, "vout", ""},
            {RPCResult::Type::STR, "address", /*optional=*/true, "Who controls it"},
        }},
        {RPCResult::Type::OBJ, "logo", /*optional=*/true, "Its logo now, if it has one (its bytes: getassetlogo)", LogoResults()},
    };
}

UniValue AssetToJSON(Chainstate& chainstate, const bitassets::State& state, const AssetId& asset, const bitassets::AssetRecord& record, std::optional<int> height) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("asset", asset.GetHex());
    if (!record.text.empty()) obj.pushKV("name", record.text);
    if (const auto display{state.GetDisplayName(asset)}) obj.pushKV("display_name", display->text);
    obj.pushKV("label", AssetLabel(asset, &record));
    obj.pushKV("seq", bitassets::FormatSeq(record.seq));
    obj.pushKV("decimals", record.decimals);
    obj.pushKV("supply", AmountToJSON(record.supply, record.decimals));
    obj.pushKV("minted", AmountToJSON(record.minted, record.decimals));
    obj.pushKV("burned", AmountToJSON(record.burned, record.decimals));
    obj.pushKV("fixed", record.fixed);
    obj.pushKV("registered", record.height);
    obj.pushKV("registration", record.registration.GetHex());
    const auto data{height ? state.DataAt(asset, *height) : std::optional{record.Current()}};
    if (data) obj.pushKV("data", bitassets::DataToJSON(*data));
    const bool releasable{state.Releasable(asset)};
    obj.pushKV("releasable", releasable);
    if (releasable) {
        CAmount fee{0};
        for (const auto& [id, pool] : state.PoolsOf(asset)) {
            if (pool.asset0.IsNull() && pool.asset1 == asset) fee += static_cast<CAmount>(pool.reserve0);
        }
        obj.pushKV("release_fee", ValueFromAmount(fee));
    }
    if (!record.fixed) {
        if (const auto outpoint{state.FirstTokenOf(asset, bitassets::Token::Kind::CONTROL)}) obj.pushKV("control", OutputToJSON(chainstate, *outpoint));
    }
    if (const auto logo{state.GetLogo(asset)}) obj.pushKV("logo", LogoToJSON(*logo));
    return obj;
}

/** A price: how much of `quote` one whole `base` is worth, from amounts in units. */
UniValue PriceToJSON(uint64_t base_units, uint8_t base_decimals, uint64_t quote_units, uint8_t quote_decimals)
{
    if (base_units == 0) return UniValue{};
    // quote per base, in whole assets, with 12 significant decimals.
    const double price{(static_cast<double>(quote_units) / std::pow(10.0, quote_decimals)) / (static_cast<double>(base_units) / std::pow(10.0, base_decimals))};
    UniValue value;
    // Plain decimals, with 12 significant digits however small (no exponent).
    const int decimals{price >= 1 ? 8 : std::min(30, 12 - static_cast<int>(std::floor(std::log10(price))))};
    std::string text{strprintf("%.*f", decimals, price)};
    if (text.find('.') != std::string::npos) {
        text.erase(text.find_last_not_of('0') + 1);
        if (text.back() == '.') text.pop_back();
    }
    value.setNumStr(text);
    return value;
}

UniValue DecimalToJSON(double price);

/** The pool kinds' fields of a pool (bitassets/curves.h), its asset `a` first. */
std::vector<RPCResult> KindResults()
{
    return {
        {RPCResult::Type::STR, "curve", "\"constant_product\", \"stable\" or \"weighted\""},
        {RPCResult::Type::NUM, "fee", "Its fee, in percent of what goes in (0.01, 0.05, 0.3 or 1), all to the liquidity providers"},
        {RPCResult::Type::NUM, "fee_millionths", "The same in millionths"},
        {RPCResult::Type::NUM, "amp", /*optional=*/true, "A stable pool's amplification A"},
        {RPCResult::Type::NUM, "weight_a", /*optional=*/true, "A weighted pool's weight of the first asset, in percent"},
        {RPCResult::Type::NUM, "weight_b", /*optional=*/true, "And of the second"},
        {RPCResult::Type::BOOL, "default_kind", "Whether it is the kind AddLiquidity makes (constant product, 0.3 %): the pool of the two assets that swapasset, swaproute's paths and addliquidity name"},
        {RPCResult::Type::BOOL, "dynamic_fee", "Whether its fee rises with its volatility"},
        {RPCResult::Type::NUM, "fee_now", /*optional=*/true, "A dynamic fee's value for the next block, in percent"},
        {RPCResult::Type::OBJ, "concentrated", /*optional=*/true, "A concentrated pool's state",
        {
            {RPCResult::Type::STR_HEX, "sqrt_price_x96", "Its square root price, Q64.96 (asset1 per asset0, in units)"},
            {RPCResult::Type::NUM, "tick", "Its tick"},
            {RPCResult::Type::STR, "liquidity", "The liquidity in range (decimal)"},
            {RPCResult::Type::NUM, "tick_spacing", "Positions' ticks are multiples of it"},
        }},
    };
}

std::string U128Text(unsigned __int128 value)
{
    if (value == 0) return "0";
    std::string text;
    while (value > 0) {
        text.insert(text.begin(), static_cast<char>('0' + static_cast<int>(value % 10)));
        value /= 10;
    }
    return text;
}

void PushKind(UniValue& obj, const bitassets::PoolParams& params, bool flip)
{
    obj.pushKV("curve", bitassets::curves::CurveName(params.GetCurve()));
    obj.pushKV("fee", DecimalToJSON(params.fee / 10000.0));
    obj.pushKV("fee_millionths", static_cast<uint64_t>(params.fee));
    if (params.GetCurve() == bitassets::Curve::STABLE) obj.pushKV("amp", static_cast<uint64_t>(params.amp));
    if (params.GetCurve() == bitassets::Curve::WEIGHTED) {
        const int w0{params.weight0};
        obj.pushKV("weight_a", flip ? 100 - w0 : w0);
        obj.pushKV("weight_b", flip ? w0 : 100 - w0);
    }
    obj.pushKV("default_kind", params.Legacy());
    obj.pushKV("dynamic_fee", (params.flags & bitassets::POOL_FLAG_DYNAMIC_FEE) != 0);
}

/** What one whole `a` is worth in `b` at a pool's point (its curve's price; the reserves' ratio for constant product; a concentrated pool's, its state's). */
double WholeSpotPrice(const bitassets::PoolParams& params, const bitassets::Pool& pool, bool a_is_0, uint8_t da, uint8_t db, const bitassets::ClState* cl = nullptr)
{
    const double raw0{cl ? UintToArith256(bitassets::cl::PriceQ64(UintToArith256(cl->sqrt_price))).getdouble() / 18446744073709551616.0
                         : bitassets::curves::SpotPrice(params, pool.reserve0, pool.reserve1)};
    if (!(raw0 > 0)) return 0;
    const double raw{a_is_0 ? raw0 : 1 / raw0};
    return raw * std::pow(10.0, int{da} - int{db});
}

std::vector<RPCResult> PoolResults()
{
    return {
        {RPCResult::Type::STR_HEX, "pool", "The id of the pool, which its shares carry"},
        {RPCResult::Type::STR, "asset_a", "The first asset, as asked for (else the lower)"},
        {RPCResult::Type::STR, "asset_b", "The second asset"},
        {RPCResult::Type::STR_HEX, "asset_a_id", "Its hash (null for " + CURRENCY_UNIT + ")"},
        {RPCResult::Type::STR_HEX, "asset_b_id", "Its hash"},
        {RPCResult::Type::NUM, "reserve_a", "What the pool holds of the first asset"},
        {RPCResult::Type::NUM, "reserve_b", "What it holds of the second"},
        {RPCResult::Type::NUM, "price", /*optional=*/true, "What one of the first asset is worth in the second: the pool's price (the reserves' ratio for constant product, its curve's slope for the others)"},
        {RPCResult::Type::NUM, "shares", "The shares there are, in units (a share has no decimals)"},
        {RPCResult::Type::NUM, "volume_a", "All of the first asset ever traded in"},
        {RPCResult::Type::NUM, "volume_b", "All of the second asset ever traded in"},
        {RPCResult::Type::NUM, "swaps", "How many trades it has made"},
        {RPCResult::Type::NUM, "created", "The height of the block that made it"},
        {RPCResult::Type::BOOL, "abandoned", "Whether nobody provides liquidity any more: it holds only the minimum it keeps for good, and trading in it gets almost nothing"},
    };
}

std::vector<RPCResult> PoolResultsWithKind()
{
    std::vector<RPCResult> results{PoolResults()};
    for (RPCResult& r : KindResults()) results.push_back(std::move(r));
    return results;
}

UniValue PoolToJSON(const bitassets::State& state, const uint256& id, const bitassets::Pool& pool, const AssetId& first)
{
    const bool flip{pool.asset0 != first};
    const AssetId& a{flip ? pool.asset1 : pool.asset0};
    const AssetId& b{flip ? pool.asset0 : pool.asset1};
    const uint64_t reserve_a{flip ? pool.reserve1 : pool.reserve0}, reserve_b{flip ? pool.reserve0 : pool.reserve1};
    const uint64_t volume_a{flip ? pool.volume1 : pool.volume0}, volume_b{flip ? pool.volume0 : pool.volume1};
    const uint8_t da{DecimalsOf(state, a)}, db{DecimalsOf(state, b)};
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("pool", id.GetHex());
    obj.pushKV("asset_a", LabelOf(state, a));
    obj.pushKV("asset_b", LabelOf(state, b));
    obj.pushKV("asset_a_id", a.GetHex());
    obj.pushKV("asset_b_id", b.GetHex());
    obj.pushKV("reserve_a", AmountToJSON(reserve_a, da));
    obj.pushKV("reserve_b", AmountToJSON(reserve_b, db));
    const bitassets::PoolParams params{state.GetPoolParams(id)};
    if (params.GetCurve() == bitassets::Curve::LAUNCH) {
        // A launch's curve: what it holds (its reserves less the virtual), and its price.
        if (const auto launch{state.GetLaunch(pool.asset1)}) {
            const uint64_t coin{pool.reserve0 - std::min(pool.reserve0, launch->virtual_coin)}, tokens{pool.reserve1 - std::min(pool.reserve1, launch->CurveEnd())};
            obj.pushKV("reserve_a", AmountToJSON(flip ? tokens : coin, da));
            obj.pushKV("reserve_b", AmountToJSON(flip ? coin : tokens, db));
        }
        if (UniValue price{PriceToJSON(reserve_a, da, reserve_b, db)}; !price.isNull()) obj.pushKV("price", std::move(price));
    } else if (params.GetCurve() == bitassets::Curve::CONSTANT_PRODUCT) {
        if (UniValue price{PriceToJSON(reserve_a, da, reserve_b, db)}; !price.isNull()) obj.pushKV("price", std::move(price));
    } else if (params.GetCurve() == bitassets::Curve::CONCENTRATED) {
        if (const auto cl{state.GetClState(id)}) obj.pushKV("price", DecimalToJSON(WholeSpotPrice(params, pool, !flip, da, db, &*cl)));
    } else if (reserve_a > 0 && reserve_b > 0) {
        obj.pushKV("price", DecimalToJSON(WholeSpotPrice(params, pool, !flip, da, db)));
    }
    obj.pushKV("shares", pool.shares);
    obj.pushKV("volume_a", AmountToJSON(volume_a, da));
    obj.pushKV("volume_b", AmountToJSON(volume_b, db));
    obj.pushKV("swaps", pool.swaps);
    obj.pushKV("created", pool.height);
    obj.pushKV("abandoned", bitassets::amm::Abandoned(pool));
    PushKind(obj, params, flip);
    if (params.flags & bitassets::POOL_FLAG_DYNAMIC_FEE) obj.pushKV("fee_now", DecimalToJSON(state.QuoteFee(id, params) / 10000.0));
    if (const auto cl{params.GetCurve() == bitassets::Curve::CONCENTRATED ? state.GetClState(id) : std::nullopt}) {
        UniValue c(UniValue::VOBJ);
        c.pushKV("sqrt_price_x96", UintToArith256(cl->sqrt_price).GetHex());
        c.pushKV("tick", cl->tick);
        c.pushKV("liquidity", U128Text(cl->Liquidity()));
        c.pushKV("tick_spacing", bitassets::cl::TickSpacing(params.fee));
        obj.pushKV("concentrated", std::move(c));
    }
    return obj;
}

std::vector<RPCResult> AuctionResults()
{
    return {
        {RPCResult::Type::STR_HEX, "auction", "The transaction that made it, which names it"},
        {RPCResult::Type::STR, "base", "What it sells"},
        {RPCResult::Type::STR, "quote", "What it sells for"},
        {RPCResult::Type::STR_HEX, "base_id", "The hash of what it sells (null for " + CURRENCY_UNIT + ")"},
        {RPCResult::Type::STR_HEX, "quote_id", "The hash of what it sells for"},
        {RPCResult::Type::NUM, "amount", "How much it sells, in all"},
        {RPCResult::Type::NUM, "remaining", "How much is left"},
        {RPCResult::Type::NUM, "proceeds", "What bids paid in, not collected yet"},
        {RPCResult::Type::NUM, "start_price", "The price of all of it at the start"},
        {RPCResult::Type::NUM, "end_price", "The price of all of it at the end"},
        {RPCResult::Type::NUM, "unit_price", "What one whole unit of it costs in the next block"},
        {RPCResult::Type::NUM, "price", "What all of it costs in the next block"},
        {RPCResult::Type::NUM, "cost_of_remaining", "What a bid for all that is left pays in the next block (see quotebid)"},
        {RPCResult::Type::NUM, "start_height", "The first block that takes bids"},
        {RPCResult::Type::NUM, "end_height", "The last block that takes bids"},
        {RPCResult::Type::NUM, "bids", "How many bids it took"},
        {RPCResult::Type::STR, "status", "\"upcoming\", \"open\", \"sold out\", \"ended\" or \"closed\""},
        {RPCResult::Type::OBJ, "receipt", /*optional=*/true, "The output that carries its receipt, until it is collected",
        {
            {RPCResult::Type::STR_HEX, "txid", ""},
            {RPCResult::Type::NUM, "vout", ""},
            {RPCResult::Type::STR, "address", /*optional=*/true, "Who collects"},
        }},
    };
}

std::string AuctionStatus(const bitassets::Auction& auction, int height)
{
    if (auction.closed) return "closed";
    if (auction.remaining == 0) return "sold out";
    if (height < auction.start_height) return "upcoming";
    if (height > auction.EndHeight()) return "ended";
    return "open";
}

UniValue AuctionToJSON(Chainstate& chainstate, const bitassets::State& state, const Txid& id, const bitassets::Auction& auction, int height) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    const uint8_t db{DecimalsOf(state, auction.base)}, dq{DecimalsOf(state, auction.quote)};
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("auction", id.GetHex());
    obj.pushKV("base", LabelOf(state, auction.base));
    obj.pushKV("quote", LabelOf(state, auction.quote));
    obj.pushKV("base_id", auction.base.GetHex());
    obj.pushKV("quote_id", auction.quote.GetHex());
    obj.pushKV("amount", AmountToJSON(auction.base_amount, db));
    obj.pushKV("remaining", AmountToJSON(auction.remaining, db));
    obj.pushKV("proceeds", AmountToJSON(auction.proceeds, dq));
    obj.pushKV("start_price", AmountToJSON(auction.start_price, dq));
    obj.pushKV("end_price", AmountToJSON(auction.end_price, dq));
    const uint64_t price{auction.PriceAt(std::max(height, auction.start_height))};
    obj.pushKV("unit_price", PriceToJSON(auction.base_amount, db, price, dq));
    obj.pushKV("price", AmountToJSON(price, dq));
    obj.pushKV("cost_of_remaining", AmountToJSON(auction.CostOfRemaining(std::max(height, auction.start_height)), dq));
    obj.pushKV("start_height", auction.start_height);
    obj.pushKV("end_height", auction.EndHeight());
    obj.pushKV("bids", auction.bids);
    obj.pushKV("status", AuctionStatus(auction, height));
    if (!auction.closed) {
        if (const auto outpoint{state.FirstTokenOf(id.ToUint256(), bitassets::Token::Kind::RECEIPT)}) obj.pushKV("receipt", OutputToJSON(chainstate, *outpoint));
    }
    return obj;
}

/**
 * What the lists of pools and auctions are paged over, and the counts getbitassetsinfo gives: made
 * once per tip, from a snapshot of the state read without cs_main (a poll of every open page, every
 * few seconds, does not walk tables anyone can grow while block validation waits). Pages read the
 * entries they show from the state as it is then.
 */
struct ListIndex {
    uint256 tip;
    struct AuctionEntry {
        int created;
        Txid id;
        bool closed;
        AssetId base, quote;
    };
    //! Newest first.
    std::vector<AuctionEntry> auctions;
    //! In the order of their ids.
    std::vector<uint256> pools;
    size_t assets{0}, reservations{0}, open_auctions{0};
};

Mutex g_list_index_mutex;
std::shared_ptr<const ListIndex> g_list_index GUARDED_BY(g_list_index_mutex);

std::shared_ptr<const ListIndex> MakeListIndex(const bitassets::State& state, const uint256& tip)
{
    auto index{std::make_shared<ListIndex>()};
    index->tip = tip;
    state.ForEachAsset([&](const AssetId&, const bitassets::AssetRecord&) { ++index->assets; return true; });
    state.ForEachReservation([&](const Txid&, const uint256&) { ++index->reservations; return true; });
    state.ForEachAuction([&](const Txid& id, const bitassets::Auction& a) {
        if (!a.closed) ++index->open_auctions;
        index->auctions.push_back({a.created, id, a.closed, a.base, a.quote});
        return true;
    });
    std::stable_sort(index->auctions.begin(), index->auctions.end(), [](const auto& x, const auto& y) { return x.created > y.created; });
    state.ForEachPool([&](const uint256& id, const bitassets::Pool&) { index->pools.push_back(id); return true; });
    return index;
}

std::shared_ptr<const ListIndex> GetListIndex(ChainstateManager& chainman) EXCLUSIVE_LOCKS_REQUIRED(!::cs_main, !g_list_index_mutex)
{
    LOCK(g_list_index_mutex);
    uint256 tip;
    std::shared_ptr<const sidechain::StoreView> view;
    {
        LOCK(::cs_main);
        Chainstate& chainstate{chainman.ActiveChainstate()};
        tip = chainstate.m_chain.Tip()->GetBlockHash();
        if (g_list_index && g_list_index->tip == tip) return g_list_index;
        view = chainstate.SideCache().SharedSnapshot();
        if (!view) {
            g_list_index = MakeListIndex(StateOf(chainman), tip);
            return g_list_index;
        }
    }
    g_list_index = MakeListIndex(sidechain::State{*view}.BitAssets(), tip);
    return g_list_index;
}

/** count and skip of a paged list. */
std::pair<size_t, size_t> PageArgs(const UniValue& count_arg, const UniValue& skip_arg)
{
    const int count{count_arg.isNull() ? 100 : count_arg.getInt<int>()};
    const int skip{skip_arg.isNull() ? 0 : skip_arg.getInt<int>()};
    if (count < 0 || skip < 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative count or skip");
    if (count > 1000) throw JSONRPCError(RPC_INVALID_PARAMETER, "At most 1000 at a time (count)");
    return {static_cast<size_t>(count), static_cast<size_t>(skip)};
}

RPCMethod getasset()
{
    return RPCMethod{
        "getasset",
        "A registered asset: its supply, its data, and who controls it.",
        {
            {"asset", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::AssetArgHelp()},
            {"height", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Its data as it was at the end of the block at this height"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", AssetResults()},
        RPCExamples{HelpExampleCli("getasset", "\"GOLD\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    const AssetId asset{ParseAsset(request.params[0], state)};
    if (asset.IsNull()) throw JSONRPCError(RPC_INVALID_PARAMETER, CURRENCY_UNIT + " is the coin of the chain, not a registered asset");
    const auto& record{Registered(state, asset)};
    const std::optional<int> height{request.params[1].isNull() ? std::nullopt : std::optional<int>{request.params[1].getInt<int>()}};
    return AssetToJSON(chainman.ActiveChainstate(), state, asset, record, height);
},
    };
}

RPCMethod listassets()
{
    return RPCMethod{
        "listassets",
        "The registered assets, in the order of registration.",
        {
            {"count", RPCArg::Type::NUM, RPCArg::Default{100}, "How many"},
            {"skip", RPCArg::Type::NUM, RPCArg::Default{0}, "How many to skip"},
            {"match", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Only those whose public name contains this text (any case)"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::OBJ, "", "", AssetResults()}}},
        RPCExamples{HelpExampleCli("listassets", "10 0")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const int count{request.params[0].isNull() ? 100 : request.params[0].getInt<int>()};
    const int skip{request.params[1].isNull() ? 0 : request.params[1].getInt<int>()};
    if (count < 0 || skip < 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative count or skip");
    const std::optional<std::string> match{request.params[2].isNull() ? std::nullopt : std::optional{ToLower(request.params[2].get_str())}};
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    UniValue result(UniValue::VARR);
    int skipped{0};
    for (uint32_t seq{0}; seq < state.NextSeq() && result.size() < static_cast<size_t>(count); ++seq) {
        const auto asset{state.AssetOfSeq(seq)};
        if (!asset) continue;
        // A retired asset keeps its number, and its name may be registered again under another.
        const auto found{state.GetAsset(*asset)};
        if (!found || found->seq != seq) continue;
        const auto& record{*found};
        if (match && ToLower(record.text).find(*match) == std::string::npos) continue;
        if (skipped++ < skip) continue;
        result.push_back(AssetToJSON(chainman.ActiveChainstate(), state, *asset, record, std::nullopt));
    }
    return result;
},
    };
}

RPCMethod getassethistory()
{
    return RPCMethod{
        "getassethistory",
        "Every value each field of the data of an asset has had, with the transaction and height that set it.",
        {
            {"asset", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::AssetArgHelp()},
        },
        RPCResult{RPCResult::Type::OBJ_DYN, "", "Per field, oldest first",
        {
            {RPCResult::Type::ARR, "field", "", {{RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::ANY, "value", "The value; null where it was deleted"},
                {RPCResult::Type::STR_HEX, "txid", ""},
                {RPCResult::Type::NUM, "height", ""},
            }}}},
        }},
        RPCExamples{HelpExampleCli("getassethistory", "\"GOLD\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    const AssetId asset{ParseAsset(request.params[0], state)};
    Registered(state, asset);
    const bitassets::AssetHistory record{state.GetHistory(asset)};
    UniValue result(UniValue::VOBJ);
    const auto add{[&](const std::string& field, const auto& history, const auto& format) {
        UniValue entries(UniValue::VARR);
        for (const auto& entry : history) {
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("value", entry.value ? format(*entry.value) : UniValue{});
            obj.pushKV("txid", entry.txid.GetHex());
            obj.pushKV("height", entry.height);
            entries.push_back(std::move(obj));
        }
        result.pushKV(field, std::move(entries));
    }};
    const auto data_field{[](const char* key) {
        return [key](const auto& v) {
            bitassets::AssetData data;
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) data.info = v;
            if constexpr (std::is_same_v<T, uint256>) data.commitment = v;
            if constexpr (std::is_same_v<T, bitassets::SocketV4>) data.ipv4 = v;
            if constexpr (std::is_same_v<T, bitassets::SocketV6>) data.ipv6 = v;
            if constexpr (std::is_same_v<T, bitassets::EncryptionKey>) data.encryption_key = v;
            return bitassets::DataToJSON(data)[key];
        };
    }};
    add("info", record.info, data_field("info"));
    add("commitment", record.commitment, data_field("commitment"));
    add("ipv4", record.ipv4, data_field("ipv4"));
    add("ipv6", record.ipv6, data_field("ipv6"));
    add("encryptionkey", record.encryption_key, data_field("encryptionkey"));
    add("signingkey", record.signing_key, [](const bitassets::SigningKey& v) { return UniValue{HexStr(v)}; });
    return result;
},
    };
}

RPCMethod getassetlogo()
{
    return RPCMethod{
        "getassetlogo",
        strprintf("The logo of an asset: a PNG or JPEG of %ux%u pixels, at most %u bytes, as the chain has it.\n"
                  "Show it only as an image of its MIME type (a data: URL in an <img>, say), never as a document.", bitassets::LOGO_SIZE, bitassets::LOGO_SIZE, bitassets::MAX_LOGO_BYTES),
        {
            {"asset", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::AssetArgHelp()},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", [] {
            std::vector<RPCResult> results{{RPCResult::Type::STR_HEX, "asset", "The asset"}};
            for (auto& r : LogoResults()) results.push_back(std::move(r));
            results.push_back({RPCResult::Type::STR, "base64", "Its bytes, in base64"});
            return results;
        }()},
        RPCExamples{HelpExampleCli("getassetlogo", "\"GOLD\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    const AssetId asset{ParseAsset(request.params[0], state)};
    Registered(state, asset);
    const auto logo{state.GetLogo(asset)};
    const auto bytes{state.GetLogoBytes(asset)};
    if (!logo || !bytes) throw JSONRPCError(RPC_INVALID_PARAMETER, "This asset has no logo");
    // Bounded: the rules let no logo be larger.
    if (bytes->size() > bitassets::MAX_LOGO_BYTES) throw JSONRPCError(RPC_INTERNAL_ERROR, "The logo is larger than a logo can be");
    UniValue result{LogoToJSON(*logo)};
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("asset", asset.GetHex());
    for (const auto& key : result.getKeys()) obj.pushKV(key, result[key]);
    obj.pushKV("base64", EncodeBase64(*bytes));
    return obj;
},
    };
}

RPCMethod getbitassetsinfo()
{
    return RPCMethod{
        "getbitassetsinfo",
        "How many assets, reservations, pools and auctions there are (counted once per block).",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "assets", "Registered assets"},
            {RPCResult::Type::NUM, "reservations", "Reservations not registered yet"},
            {RPCResult::Type::NUM, "pools", "Pools"},
            {RPCResult::Type::NUM, "auctions", "Auctions not closed"},
            {RPCResult::Type::NUM, "auctions_all", "Auctions, those closed included"},
            {RPCResult::Type::NUM, "height", "The height of the next block, which the prices are for"},
            {RPCResult::Type::NUM, "reveal_depth", "How many blocks after the block that made a reservation a block may take its registration"},
            {RPCResult::Type::NUM, "logo_height", "The first block of the logos of assets (-1: never on this network)"},
            {RPCResult::Type::NUM, "max_logo_bytes", "The most bytes a logo may have"},
            {RPCResult::Type::NUM, "ticker_height", "The first block of tickers and display names (-1: never on this network): from it the name of a new asset is a ticker"},
            {RPCResult::Type::STR_AMOUNT, "min_chn_payout", "The least " + CURRENCY_UNIT + " a pool, a book, a curve or an auction pays out in the next block"},
            {RPCResult::Type::STR_AMOUNT, "min_book_chn", "The least an order of a pair with " + CURRENCY_UNIT + " is worth on its " + CURRENCY_UNIT + " side, in the next block"},
            {RPCResult::Type::NUM, "small_payout_height", "The first block of payouts of 0.00005 at least (-1: never on this network)"},
            {RPCResult::Type::BOOL, "tickers_active", "Whether the next block has the rules of tickers and display names"},
            {RPCResult::Type::NUM, "min_ticker_length", "The fewest characters of a ticker"},
            {RPCResult::Type::NUM, "max_ticker_length", "The most characters of a ticker (A-Z, 0-9)"},
            {RPCResult::Type::NUM, "max_display_name_bytes", "The most bytes (UTF-8) of a display name"},
        }},
        RPCExamples{HelpExampleCli("getbitassetsinfo", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    // Counted once per tip (ListIndex), not at every call.
    const auto index{GetListIndex(chainman)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("assets", index->assets);
    result.pushKV("reservations", index->reservations);
    result.pushKV("pools", index->pools.size());
    result.pushKV("auctions", index->open_auctions);
    result.pushKV("auctions_all", index->auctions.size());
    LOCK(::cs_main);
    result.pushKV("height", chainman.ActiveChain().Height() + 1);
    result.pushKV("reveal_depth", chainman.GetConsensus().sidechain.bitassets_reveal_depth);
    const int logo_height{chainman.GetConsensus().sidechain.logo_height};
    result.pushKV("logo_height", logo_height == std::numeric_limits<int>::max() ? -1 : logo_height);
    result.pushKV("max_logo_bytes", static_cast<uint64_t>(bitassets::MAX_LOGO_BYTES));
    const int ticker_height{chainman.GetConsensus().sidechain.ticker_height};
    result.pushKV("ticker_height", ticker_height == std::numeric_limits<int>::max() ? -1 : ticker_height);
    result.pushKV("tickers_active", chainman.GetConsensus().sidechain.TickerActive(chainman.ActiveChain().Height() + 1));
    result.pushKV("min_ticker_length", static_cast<uint64_t>(bitassets::MIN_TICKER_SIZE));
    result.pushKV("max_ticker_length", static_cast<uint64_t>(bitassets::MAX_TICKER_SIZE));
    result.pushKV("max_display_name_bytes", static_cast<uint64_t>(bitassets::MAX_DISPLAY_NAME_SIZE));
    // The least ECX paid out in the next block (0.00005 from small_payout_height, 0.0005 before).
    result.pushKV("min_chn_payout", ValueFromAmount(static_cast<CAmount>(bitassets::MinChnPayout(chainman.ActiveChain().Height() + 1))));
    result.pushKV("min_book_chn", ValueFromAmount(static_cast<CAmount>(bitassets::MinBookChn(chainman.ActiveChain().Height() + 1))));
    const int small_height{chainman.GetConsensus().sidechain.small_payout_height};
    result.pushKV("small_payout_height", small_height == std::numeric_limits<int>::max() ? -1 : small_height);
    return result;
},
    };
}

RPCMethod listpools()
{
    return RPCMethod{
        "listpools",
        "The pools, with what they hold and their prices, a page at a time (pools in getbitassetsinfo says how many).",
        {
            {"asset", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Only the pools of this asset, which then comes first"},
            {"count", RPCArg::Type::NUM, RPCArg::Default{100}, "How many, at most 1000"},
            {"skip", RPCArg::Type::NUM, RPCArg::Default{0}, "How many to skip"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::OBJ, "", "", PoolResultsWithKind()}}},
        RPCExamples{HelpExampleCli("listpools", "") + HelpExampleCli("listpools", "\"GOLD\"") + HelpExampleCli("listpools", "null 100 100")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const auto [count, skip]{PageArgs(request.params[1], request.params[2])};
    const auto index{request.params[0].isNull() ? GetListIndex(chainman) : nullptr};
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    const std::optional<AssetId> only{request.params[0].isNull() ? std::nullopt : std::optional{ParseAsset(request.params[0], state)}};
    UniValue result(UniValue::VARR);
    std::vector<std::pair<uint256, bitassets::Pool>> pools;
    if (only) {
        // By the index of pools by asset.
        pools = state.PoolsOf(*only);
        pools.erase(pools.begin(), pools.begin() + std::min(skip, pools.size()));
        if (pools.size() > count) pools.resize(count);
    } else {
        for (size_t n{skip}; n < index->pools.size() && pools.size() < count; ++n) {
            // Gone since the index was made (the tip moved): left out.
            if (const auto pool{state.GetPool(index->pools[n])}) pools.emplace_back(index->pools[n], *pool);
        }
    }
    for (const auto& [id, pool] : pools) {
        // A pool with CHN shows its price in CHN.
        AssetId first{pool.asset0.IsNull() ? pool.asset1 : pool.asset0};
        if (only) first = *only;
        result.push_back(PoolToJSON(state, id, pool, first));
    }
    return result;
},
    };
}

RPCMethod getpool()
{
    return RPCMethod{
        "getpool",
        "A pool: of two assets, the kind addliquidity makes (constant product, 0.3 %); or any pool by its id (a pool of another kind: createpool).",
        {
            {"asset_a", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::AssetArgHelp() + "; or the id of a pool (64 hex digits), asset_b left out"},
            {"asset_b", RPCArg::Type::STR, RPCArg::Optional::OMITTED, bitassets::AssetArgHelp()},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", PoolResultsWithKind()},
        RPCExamples{HelpExampleCli("getpool", "\"GOLD\" \"" + CURRENCY_UNIT + "\"") + HelpExampleCli("getpool", "\"<pool id>\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    return WithDexState(chainman, [&](const bitassets::State& state, [[maybe_unused]] int next_height) -> UniValue {
        if (request.params[1].isNull()) {
            const uint256 id{ParseHashV(request.params[0], "pool")};
            const auto pool{state.GetPool(id)};
            if (!pool) throw JSONRPCError(RPC_INVALID_PARAMETER, "No such pool");
            return PoolToJSON(state, id, *pool, pool->asset0.IsNull() ? pool->asset1 : pool->asset0);
        }
        const AssetId a{ParseAsset(request.params[0], state)}, b{ParseAsset(request.params[1], state)};
        const auto pool{state.FindPool(a, b)};
        if (!pool) throw JSONRPCError(RPC_INVALID_PARAMETER, "There is no pool of these assets (addliquidity makes one)");
        return PoolToJSON(state, bitassets::PoolId(a, b), *pool, a);
    });
},
    };
}

RPCMethod quoteswap()
{
    return RPCMethod{
        "quoteswap",
        "What a trade in a pool gives now: for an amount in, what comes out; or for an amount out, what has to go in.",
        {
            {"asset_in", RPCArg::Type::STR, RPCArg::Optional::NO, "What is paid in. " + bitassets::AssetArgHelp()},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "How much goes in (or comes out, with exact_out)"},
            {"asset_out", RPCArg::Type::STR, RPCArg::Optional::NO, "What comes out"},
            {"exact_out", RPCArg::Type::BOOL, RPCArg::Default{false}, "The amount is what comes out"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "amount_in", "What goes in"},
            {RPCResult::Type::NUM, "amount_out", "What comes out"},
            {RPCResult::Type::NUM, "price", "What one of what comes out costs, in what goes in, for this trade"},
            {RPCResult::Type::NUM, "spot_price", "The same before the trade"},
            {RPCResult::Type::NUM, "price_impact", "How much worse the trade's price is than the spot price, in percent (the fee included)"},
            {RPCResult::Type::NUM, "fee", "What the pool keeps, in what goes in"},
            {RPCResult::Type::BOOL, "abandoned", "Whether nobody provides liquidity to the pool any more (see listpools)"},
        }},
        RPCExamples{HelpExampleCli("quoteswap", "\"" + CURRENCY_UNIT + "\" 1 \"GOLD\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    return WithDexState(chainman, [&](const bitassets::State& state, [[maybe_unused]] int next_height) -> UniValue {
        const AssetId in{ParseAsset(request.params[0], state)}, out{ParseAsset(request.params[2], state)};
        if (in == out) throw JSONRPCError(RPC_INVALID_PARAMETER, "The same asset in and out");
        const auto pool{state.FindPool(in, out)};
        if (!pool) throw JSONRPCError(RPC_INVALID_PARAMETER, "There is no pool of these assets");
        const bool zero_in{pool->asset0 == in};
        const uint64_t reserve_in{zero_in ? pool->reserve0 : pool->reserve1}, reserve_out{zero_in ? pool->reserve1 : pool->reserve0};
        const uint8_t di{DecimalsOf(state, in)}, dout{DecimalsOf(state, out)};
        const bool exact_out{!request.params[3].isNull() && request.params[3].get_bool()};
        uint64_t amount_in, amount_out;
        if (exact_out) {
            amount_out = bitassets::ParseUnits(request.params[1], dout);
            const auto needed{bitassets::amm::SwapIn(reserve_in, reserve_out, amount_out)};
            if (!needed) throw JSONRPCError(RPC_INVALID_PARAMETER, "The pool does not hold that much");
            amount_in = *needed;
            amount_out = bitassets::amm::SwapOut(reserve_in, reserve_out, amount_in);
        } else {
            amount_in = bitassets::ParseUnits(request.params[1], di);
            amount_out = bitassets::amm::SwapOut(reserve_in, reserve_out, amount_in);
            if (amount_out == 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "Too little: it buys nothing");
        }
        UniValue result(UniValue::VOBJ);
        result.pushKV("amount_in", AmountToJSON(amount_in, di));
        result.pushKV("amount_out", AmountToJSON(amount_out, dout));
        result.pushKV("price", PriceToJSON(amount_out, dout, amount_in, di));
        result.pushKV("spot_price", PriceToJSON(reserve_out, dout, reserve_in, di));
        UniValue impact_value;
        impact_value.setNumStr(strprintf("%.4f", bitassets::amm::PriceImpact(reserve_in, reserve_out, amount_in, amount_out)));
        result.pushKV("price_impact", impact_value);
        // In 128 bits: amount_in times 3 overflows 64 above about 6e18.
        result.pushKV("fee", AmountToJSON(static_cast<uint64_t>(static_cast<unsigned __int128>(amount_in) * bitassets::SWAP_FEE_PER_MILLE / 1000), di));
        result.pushKV("abandoned", bitassets::amm::Abandoned(*pool));
        return result;
    });
},
    };
}

RPCMethod listauctions()
{
    return RPCMethod{
        "listauctions",
        "The auctions, newest first, a page at a time (auctions and auctions_all in getbitassetsinfo say how many).",
        {
            {"include_closed", RPCArg::Type::BOOL, RPCArg::Default{false}, "Also those collected already"},
            {"count", RPCArg::Type::NUM, RPCArg::Default{100}, "How many, at most 1000"},
            {"skip", RPCArg::Type::NUM, RPCArg::Default{0}, "How many to skip"},
            {"asset", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Only those that sell this asset or sell for it"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::OBJ, "", "", AuctionResults()}}},
        RPCExamples{HelpExampleCli("listauctions", "") + HelpExampleCli("listauctions", "false 100 100") + HelpExampleCli("listauctions", "true 10 0 \"GOLD\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const bool closed{!request.params[0].isNull() && request.params[0].get_bool()};
    const auto [count, skip]{PageArgs(request.params[1], request.params[2])};
    const auto index{GetListIndex(chainman)};
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    const std::optional<AssetId> only{request.params[3].isNull() ? std::nullopt : std::optional{ParseAsset(request.params[3], state)}};
    const int height{chainman.ActiveChain().Height() + 1};
    UniValue result(UniValue::VARR);
    size_t skipped{0};
    for (const auto& entry : index->auctions) {
        if (result.size() >= count) break;
        if (entry.closed && !closed) continue;
        if (only && entry.base != *only && entry.quote != *only) continue;
        if (skipped < skip) {
            ++skipped;
            continue;
        }
        // As it is now (the tip may have moved since the index was made).
        const auto auction{state.GetAuction(entry.id)};
        if (!auction || (auction->closed && !closed)) continue;
        result.push_back(AuctionToJSON(chainman.ActiveChainstate(), state, entry.id, *auction, height));
    }
    return result;
},
    };
}

RPCMethod getauction()
{
    return RPCMethod{
        "getauction",
        "An auction.",
        {
            {"auction", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction that made it"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", AuctionResults()},
        RPCExamples{HelpExampleCli("getauction", "\"txid\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const Txid id{Txid::FromUint256(ParseHashV(request.params[0], "auction"))};
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    const auto found{state.GetAuction(id)};
    if (!found) throw JSONRPCError(RPC_INVALID_PARAMETER, "No such auction");
    return AuctionToJSON(chainman.ActiveChainstate(), state, id, *found, chainman.ActiveChain().Height() + 1);
},
    };
}

RPCMethod quotebid()
{
    return RPCMethod{
        "quotebid",
        "What a bid on an auction buys in the next block.",
        {
            {"auction", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction that made it"},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "What the bid pays, in what the auction sells for"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "buys", "What it buys"},
            {RPCResult::Type::NUM, "remaining", "What is left to buy"},
            {RPCResult::Type::NUM, "cost_of_remaining", "What a bid for all that is left pays in the next block"},
            {RPCResult::Type::BOOL, "open", "Whether the next block takes bids"},
        }},
        RPCExamples{HelpExampleCli("quotebid", "\"txid\" 10")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const Txid id{Txid::FromUint256(ParseHashV(request.params[0], "auction"))};
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    const auto found{state.GetAuction(id)};
    if (!found) throw JSONRPCError(RPC_INVALID_PARAMETER, "No such auction");
    const bitassets::Auction auction{*found};
    const int height{chainman.ActiveChain().Height() + 1};
    const uint8_t db{DecimalsOf(state, auction.base)}, dq{DecimalsOf(state, auction.quote)};
    const uint64_t amount{bitassets::ParseUnits(request.params[1], dq)};
    const int at{std::max(height, auction.start_height)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("buys", AmountToJSON(std::min(auction.BuysAt(at, amount), auction.remaining), db));
    result.pushKV("remaining", AmountToJSON(auction.remaining, db));
    result.pushKV("cost_of_remaining", AmountToJSON(auction.CostOfRemaining(at), dq));
    result.pushKV("open", auction.OpenAt(height));
    return result;
},
    };
}


//
// Layers' DEX (bitassets/dex.h).
//

/** A price as a plain decimal, 12 significant digits (PriceToJSON's format). */
UniValue DecimalToJSON(double price)
{
    if (!(price > 0) || !std::isfinite(price)) {
        UniValue zero;
        zero.setNumStr("0");
        return zero;
    }
    const int decimals{price >= 1 ? 8 : std::min(30, 12 - static_cast<int>(std::floor(std::log10(price))))};
    std::string text{strprintf("%.*f", decimals, price)};
    if (text.find('.') != std::string::npos) {
        text.erase(text.find_last_not_of('0') + 1);
        if (text.back() == '.') text.pop_back();
    }
    UniValue value;
    value.setNumStr(text);
    return value;
}

int NextHeight(ChainstateManager& chainman) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) { return chainman.ActiveChain().Height() + 1; }



/** An oracle an argument names: its id, or its key (64 hex digits each; a name is a label others may share). */
uint256 ParseOracle(const UniValue& value, const bitassets::State& state)
{
    const std::string text{value.get_str()};
    if (text.size() == 64 && IsHex(text)) {
        const auto id{uint256::FromHex(text)};
        if (id && state.GetOracle(*id)) return *id;
        const std::vector<unsigned char> bytes{ParseHex(text)};
        uint256 key;
        std::copy(bytes.begin(), bytes.end(), key.begin());
        if (const uint256 by_key{bitassets::dex::OracleId(key)}; state.GetOracle(by_key)) return by_key;
    }
    throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("No oracle %s is registered (name it by its id or its key)", text));
}

/** An event an argument names: its id (64 hex digits), or the text it is the id of. */
uint256 ParseEvent(const UniValue& value)
{
    const std::string text{value.get_str()};
    if (text.size() == 64 && IsHex(text)) return *uint256::FromHex(text);
    return bitassets::dex::EventId(text);
}

std::string AddressOf(const CScript& script)
{
    CTxDestination dest;
    return ExtractDestination(script, dest) ? EncodeDestination(dest) : HexStr(script);
}

UniValue OracleToJSON(const uint256& id, const bitassets::OracleRecord& oracle)
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("oracle", id.GetHex());
    obj.pushKV("name", oracle.name);
    obj.pushKV("pubkey", HexStr(oracle.pubkey));
    obj.pushKV("registration", oracle.registration.GetHex());
    obj.pushKV("height", oracle.height);
    obj.pushKV("attestations", oracle.attestations);
    return obj;
}

const std::vector<RPCResult> ORACLE_RESULTS{
    {RPCResult::Type::STR_HEX, "oracle", "Its id (the tagged hash of its key)"},
    {RPCResult::Type::STR, "name", "Its name: a label, which others may share"},
    {RPCResult::Type::STR_HEX, "pubkey", "The x-only key it signs with (BIP340): its identity"},
    {RPCResult::Type::STR_HEX, "registration", "The transaction that registered it"},
    {RPCResult::Type::NUM, "height", "The block that registered it"},
    {RPCResult::Type::NUM, "attestations", "How many events it attested"},
};

RPCMethod getdexinfo()
{
    return RPCMethod{
        "getdexinfo",
        "Flush's DEX: whether its rules apply to the next block, and its bounds (doc/layers.md, \"The DEX\").",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "dex_height", "The first block of the DEX's rules (-1: never on this network)"},
            {RPCResult::Type::BOOL, "active", "Whether the next block has them"},
            {RPCResult::Type::NUM, "max_route_hops", "Most pools one order goes through"},
            {RPCResult::Type::NUM, "max_block_orders", "Most orders a block clears"},
            {RPCResult::Type::NUM, "clearing_passes", "Passes of the batch clearing before orders execute one after another"},
            {RPCResult::Type::NUM, "twap_observations", "Observations each pool keeps"},
            {RPCResult::Type::STR_AMOUNT, "oracle_registration_fee", "What registering an oracle costs (to mainchain miners)"},
            {RPCResult::Type::NUM, "max_contract_outcomes", "Most outcomes an oracle contract names"},
            {RPCResult::Type::NUM, "max_contract_duration", "Latest an oracle contract expires, in blocks"},
            {RPCResult::Type::NUM, "curves_height", "The first block of the pool kinds (fee tiers, stable and weighted pools; -1: never on this network)"},
            {RPCResult::Type::BOOL, "curves_active", "Whether the next block has them"},
            {RPCResult::Type::ARR, "fee_tiers", "The fees a pool is made with, in percent", {{RPCResult::Type::NUM, "", ""}}},
            {RPCResult::Type::ARR, "curves", "The curves", {{RPCResult::Type::STR, "", ""}}},
            {RPCResult::Type::NUM, "min_amp", "A stable pool's least amplification"},
            {RPCResult::Type::NUM, "max_amp", "And most"},
            {RPCResult::Type::ARR, "weights", "A weighted pool's weights of an asset, in percent (the other has the rest)", {{RPCResult::Type::NUM, "", ""}}},
            {RPCResult::Type::STR_AMOUNT, "security_paid", "The security parts of the fees paid with the blocks (to the BMM bidders and validators), in all"},
            {RPCResult::Type::OBJ, "swap_fee_parts", "The parts of every swap fee, in percent of it (consensus parameters of this network)",
            {
                {RPCResult::Type::NUM, "liquidity_providers", "What stays in the pool (the remainders of the roundings too)"},
                {RPCResult::Type::NUM, "security", "To the block's BMM bidder and validators, by the curve of the network fees (in a pool of the chain's coin from the fee-flow release, all in the coin; a fee in an asset otherwise stays in its pool)"},
            }},
        }},
        RPCExamples{HelpExampleCli("getdexinfo", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const Consensus::SidechainParams& params{chainman.GetConsensus().sidechain};
    LOCK(::cs_main);
    UniValue result(UniValue::VOBJ);
    result.pushKV("dex_height", params.dex_height == std::numeric_limits<int>::max() ? -1 : params.dex_height);
    result.pushKV("active", params.DexActive(NextHeight(chainman)));
    result.pushKV("max_route_hops", static_cast<int>(bitassets::MAX_ROUTE_ASSETS - 1));
    result.pushKV("max_block_orders", static_cast<int>(bitassets::MAX_BLOCK_ORDERS));
    result.pushKV("clearing_passes", bitassets::MAX_CLEARING_PASSES);
    result.pushKV("twap_observations", static_cast<int>(bitassets::TWAP_OBSERVATIONS));
    result.pushKV("oracle_registration_fee", ValueFromAmount(bitassets::ORACLE_REGISTRATION_FEE));
    result.pushKV("max_contract_outcomes", static_cast<int>(bitassets::MAX_CONTRACT_OUTCOMES));
    result.pushKV("max_contract_duration", bitassets::MAX_CONTRACT_DURATION);
    result.pushKV("curves_height", params.dex_curves_height == std::numeric_limits<int>::max() ? -1 : std::max(params.dex_curves_height, params.dex_height));
    result.pushKV("curves_active", params.DexCurvesActive(NextHeight(chainman)));
    UniValue tiers(UniValue::VARR);
    for (uint32_t fee : bitassets::FEE_TIERS) tiers.push_back(DecimalToJSON(fee / 10000.0));
    result.pushKV("fee_tiers", std::move(tiers));
    UniValue curves(UniValue::VARR);
    for (uint8_t c{0}; c <= bitassets::MAX_CURVE; ++c) curves.push_back(bitassets::curves::CurveName(static_cast<bitassets::Curve>(c)));
    result.pushKV("curves", std::move(curves));
    result.pushKV("min_amp", static_cast<uint64_t>(bitassets::MIN_AMP));
    result.pushKV("max_amp", static_cast<uint64_t>(bitassets::MAX_AMP));
    UniValue weights(UniValue::VARR);
    for (uint8_t w : bitassets::WEIGHTS) weights.push_back(w);
    result.pushKV("weights", std::move(weights));
    UniValue parts(UniValue::VOBJ);
    {
        const int next{chainman.ActiveChain().Height() + 1};
        // In force in the next block.
        parts.pushKV("liquidity_providers", DecimalToJSON((bitassets::FEE_DENOMINATOR - params.SwapSecurityShare(next) - params.SwapProtocolShare(next)) / 10000.0));
        parts.pushKV("security", DecimalToJSON(params.SwapSecurityShare(next) / 10000.0));
    }
    result.pushKV("swap_fee_parts", std::move(parts));
    result.pushKV("security_paid", ValueFromAmount(static_cast<CAmount>(WITH_LOCK(::cs_main, return chainman.ActiveChainstate().SideState().BitAssets().GetSecurityFees().total))));
    return result;
},
    };
}

RPCMethod quoteroute()
{
    return RPCMethod{
        "quoteroute",
        "The best routes through the pools from one asset to another, and what each gives alone (as the only order of a block). "
        "The search is bounded: of each asset its 16 deepest pools, at most 20000 partial paths.",
        {
            {"asset_in", RPCArg::Type::STR, RPCArg::Optional::NO, "What is paid in. " + bitassets::AssetArgHelp()},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "How much"},
            {"asset_out", RPCArg::Type::STR, RPCArg::Optional::NO, "What comes out"},
            {"max_hops", RPCArg::Type::NUM, RPCArg::Default{4}, "Most pools on a route (1 to 4)"},
            {"count", RPCArg::Type::NUM, RPCArg::Default{5}, "Most routes returned"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "amount_in", "What goes in"},
            {RPCResult::Type::NUM, "amount_out", "What the best route gives (0 if there is none)"},
            {RPCResult::Type::ARR, "path", "The best route's assets, first to last (for swaproute); empty if none", {{RPCResult::Type::STR, "", "An asset's label"}}},
            {RPCResult::Type::ARR, "pools", "The best route's pools (for swaproute's pools); empty if none", {{RPCResult::Type::STR_HEX, "", "A pool"}}},
            {RPCResult::Type::ARR, "routes", "The routes found, best first",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::ARR, "path", "Its assets", {{RPCResult::Type::STR, "", "An asset's label"}}},
                    {RPCResult::Type::ARR, "path_ids", "The same, by hash (null for " + CURRENCY_UNIT + ")", {{RPCResult::Type::STR_HEX, "", "An asset"}}},
                    {RPCResult::Type::NUM, "amount_out", "What it gives"},
                    {RPCResult::Type::ARR, "pools", "Its pools, first to last (for swaproute's pools, or swappools)", {{RPCResult::Type::STR_HEX, "", "A pool"}}},
                    {RPCResult::Type::BOOL, "default_pools", "Whether every pool is of the kind addliquidity makes: swaproute's path alone names them"},
                    {RPCResult::Type::ARR, "hops", "Each pool's part",
                    {
                        {RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::STR_HEX, "pool", "The pool"},
                            {RPCResult::Type::STR, "curve", "Its curve"},
                            {RPCResult::Type::NUM, "fee", "Its fee, in percent"},
                            {RPCResult::Type::NUM, "amount_out", "What comes out of it"},
                        }},
                    }},
                }},
            }},
        }},
        RPCExamples{HelpExampleCli("quoteroute", "\"GOLD\" 10 \"SILVER\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const int max_hops{request.params[3].isNull() ? 4 : request.params[3].getInt<int>()};
    if (max_hops < 1 || max_hops > static_cast<int>(bitassets::MAX_ROUTE_ASSETS - 1)) throw JSONRPCError(RPC_INVALID_PARAMETER, "max_hops is 1 to 4");
    const int count{request.params[4].isNull() ? 5 : request.params[4].getInt<int>()};
    if (count < 1 || count > 50) throw JSONRPCError(RPC_INVALID_PARAMETER, "count is 1 to 50");
    return WithDexState(chainman, [&](const bitassets::State& state, [[maybe_unused]] int next_height) -> UniValue {
        const AssetId in{ParseAsset(request.params[0], state)}, out{ParseAsset(request.params[2], state)};
        if (in == out) throw JSONRPCError(RPC_INVALID_PARAMETER, "The same asset in and out");
        const uint8_t di{DecimalsOf(state, in)}, dout{DecimalsOf(state, out)};
        const uint64_t amount_in{bitassets::ParseUnits(request.params[1], di)};
        const auto routes{bitassets::dex::FindRoutes(state, in, out, amount_in, max_hops, count, 20000, 16, chainman.GetConsensus().sidechain.DexCurvesActive(next_height))};
        const auto pool_ids{[](const std::vector<uint256>& pools) {
            UniValue arr(UniValue::VARR);
            for (const uint256& id : pools) arr.push_back(id.GetHex());
            return arr;
        }};
        const auto labels{[&](const std::vector<AssetId>& path) {
            UniValue arr(UniValue::VARR);
            for (const AssetId& asset : path) arr.push_back(LabelOf(state, asset));
            return arr;
        }};
        UniValue list(UniValue::VARR);
        for (const auto& route : routes) {
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("path", labels(route.path));
            UniValue ids(UniValue::VARR);
            for (const AssetId& asset : route.path) ids.push_back(asset.GetHex());
            obj.pushKV("path_ids", std::move(ids));
            obj.pushKV("amount_out", AmountToJSON(route.outs.back(), dout));
            obj.pushKV("pools", pool_ids(route.pools));
            obj.pushKV("default_pools", route.legacy);
            UniValue hops(UniValue::VARR);
            for (size_t i{0}; i < route.outs.size(); ++i) {
                UniValue hop(UniValue::VOBJ);
                const bitassets::PoolParams kind{state.GetPoolParams(route.pools[i])};
                hop.pushKV("pool", route.pools[i].GetHex());
                hop.pushKV("curve", bitassets::curves::CurveName(kind.GetCurve()));
                hop.pushKV("fee", DecimalToJSON(kind.fee / 10000.0));
                hop.pushKV("amount_out", AmountToJSON(route.outs[i], DecimalsOf(state, route.path[i + 1])));
                hops.push_back(std::move(hop));
            }
            obj.pushKV("hops", std::move(hops));
            list.push_back(std::move(obj));
        }
        UniValue result(UniValue::VOBJ);
        result.pushKV("amount_in", AmountToJSON(amount_in, di));
        result.pushKV("amount_out", AmountToJSON(routes.empty() ? 0 : routes.front().outs.back(), dout));
        result.pushKV("path", routes.empty() ? UniValue{UniValue::VARR} : labels(routes.front().path));
        result.pushKV("pools", routes.empty() ? UniValue{UniValue::VARR} : pool_ids(routes.front().pools));
        result.pushKV("routes", std::move(list));
        return result;
    });
},
    };
}

RPCMethod getpooltwap()
{
    return RPCMethod{
        "getpooltwap",
        "The time-weighted average price of a pool over a window that ends at the tip's time: from the time of a block, or over the last seconds. "
        "From the pool's price accumulators (Uniswap v2: price times seconds, at the end of every block that changes the pool, the block's time); "
        "each pool keeps its last 1024 observations, and a window must start at or after the oldest.",
        {
            {"pool", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The pool (see listpools)"},
            {"from_height", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The window starts at the time of this block of the active chain"},
            {"seconds", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Or: the window is the last this many seconds"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "pool", "The pool"},
            {RPCResult::Type::STR, "asset_a", "Its first asset (by hash)"},
            {RPCResult::Type::STR, "asset_b", "Its second"},
            {RPCResult::Type::NUM, "from_time", "The window's start"},
            {RPCResult::Type::NUM, "to_time", "Its end: the tip's time, or the pool's last change if later"},
            {RPCResult::Type::NUM, "seconds", "Its length"},
            {RPCResult::Type::NUM, "twap_a", "The average price of a whole unit of asset_a, in asset_b"},
            {RPCResult::Type::NUM, "twap_b", "The average price of a whole unit of asset_b, in asset_a"},
            {RPCResult::Type::NUM, "spot_a", "The price of asset_a now, in asset_b"},
            {RPCResult::Type::STR_HEX, "cumulative_a", "The accumulator of asset_a's price at the end (UQ64.64 seconds, modulo 2^256)"},
            {RPCResult::Type::STR_HEX, "cumulative_b", "The same, of asset_b's"},
            {RPCResult::Type::NUM, "observations", "Observations kept"},
            {RPCResult::Type::NUM, "oldest_time", "The time of the oldest kept"},
        }},
        RPCExamples{HelpExampleCli("getpooltwap", "\"pool\" 1000") + HelpExampleCli("getpooltwap", "\"pool\" null 3600")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const uint256 id{ParseHashV(request.params[0], "pool")};
    if (request.params[1].isNull() == request.params[2].isNull()) throw JSONRPCError(RPC_INVALID_PARAMETER, "Name the window's start: from_height, or seconds");
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    const auto pool{state.GetPool(id)};
    if (!pool) throw JSONRPCError(RPC_INVALID_PARAMETER, "No such pool");
    const auto acc{state.GetAccumulator(id)};
    if (!acc || acc->creation != pool->creation || acc->count == 0) throw JSONRPCError(RPC_MISC_ERROR, "The pool has no observations yet: a block has to change it under the DEX's rules first");
    const uint64_t kept{std::min<uint64_t>(acc->count, bitassets::TWAP_OBSERVATIONS)};
    const uint64_t first{acc->count - kept};
    const auto at{[&](uint64_t n) { return *Assert(state.GetObservation(id, static_cast<uint32_t>(n % bitassets::TWAP_OBSERVATIONS))); }};
    const bitassets::Observation last{at(acc->count - 1)}, oldest{at(first)};
    const CChain& chain{chainman.ActiveChain()};
    const uint32_t to_time{std::max(chain.Tip()->nTime, acc->last_time)};
    uint32_t from_time;
    if (!request.params[1].isNull()) {
        const int height{request.params[1].getInt<int>()};
        if (height < 0 || height > chain.Height()) throw JSONRPCError(RPC_INVALID_PARAMETER, "from_height is not a block of the active chain");
        from_time = chain[height]->nTime;
    } else {
        const int64_t seconds{request.params[2].getInt<int64_t>()};
        if (seconds <= 0 || seconds > to_time) throw JSONRPCError(RPC_INVALID_PARAMETER, "seconds is positive");
        from_time = static_cast<uint32_t>(to_time - seconds);
    }
    if (from_time >= to_time) throw JSONRPCError(RPC_INVALID_PARAMETER, "The window is empty");
    if (from_time < oldest.time) {
        throw JSONRPCError(RPC_MISC_ERROR, strprintf("The window starts before the oldest observation of the pool kept (time %u)", oldest.time));
    }
    // The last observation at or before the start, by halving.
    uint64_t lo{first}, hi{acc->count - 1};
    while (lo < hi) {
        const uint64_t mid{lo + (hi - lo + 1) / 2};
        if (at(mid).time <= from_time) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    const bitassets::PoolParams kind{state.GetPoolParams(id)};
    const auto [start0, start1]{bitassets::dex::CumulativeAt(at(lo), from_time, kind)};
    const auto [end0, end1]{bitassets::dex::CumulativeAt(last, to_time, kind)};
    const uint32_t seconds{to_time - from_time};
    const uint8_t d0{DecimalsOf(state, pool->asset0)}, d1{DecimalsOf(state, pool->asset1)};
    // Units of the other asset per unit, as whole assets.
    const double scale0{std::pow(10.0, int{d0} - int{d1})};
    UniValue result(UniValue::VOBJ);
    result.pushKV("pool", id.GetHex());
    result.pushKV("asset_a", LabelOf(state, pool->asset0));
    result.pushKV("asset_b", LabelOf(state, pool->asset1));
    result.pushKV("from_time", from_time);
    result.pushKV("to_time", to_time);
    result.pushKV("seconds", seconds);
    result.pushKV("twap_a", DecimalToJSON(bitassets::dex::AveragePrice(bitassets::dex::CumulativeDelta(end0, start0), seconds) * scale0));
    result.pushKV("twap_b", DecimalToJSON(bitassets::dex::AveragePrice(bitassets::dex::CumulativeDelta(end1, start1), seconds) / scale0));
    const auto cl_now{state.GetClState(id)};
    result.pushKV("spot_a", DecimalToJSON(WholeSpotPrice(kind, *pool, true, d0, d1, cl_now ? &*cl_now : nullptr)));
    result.pushKV("cumulative_a", end0.GetHex());
    result.pushKV("cumulative_b", end1.GetHex());
    result.pushKV("observations", kept);
    result.pushKV("oldest_time", oldest.time);
    return result;
},
    };
}

RPCMethod listoracles()
{
    return RPCMethod{
        "listoracles",
        "The oracles registered, by id.",
        {
            {"start", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "List from this id on (the next page: the last id of the previous one, which is left out)"},
            {"count", RPCArg::Type::NUM, RPCArg::Default{100}, "Most oracles listed (1 to 1000)"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::OBJ, "", "", ORACLE_RESULTS}}},
        RPCExamples{HelpExampleCli("listoracles", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const std::optional<uint256> start{request.params[0].isNull() ? std::nullopt : std::optional{ParseHashV(request.params[0], "start")}};
    const int count{request.params[1].isNull() ? 100 : request.params[1].getInt<int>()};
    if (count < 1 || count > 1000) throw JSONRPCError(RPC_INVALID_PARAMETER, "count is 1 to 1000");
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    UniValue list(UniValue::VARR);
    state.ForEachOracle([&](const uint256& id, const bitassets::OracleRecord& oracle) {
        if (start && id == *start) return true;
        list.push_back(OracleToJSON(id, oracle));
        return list.size() < static_cast<size_t>(count);
    }, start);
    return list;
},
    };
}

RPCMethod getoraclevalue()
{
    return RPCMethod{
        "getoraclevalue",
        "What an oracle attested for an event, if it did.",
        {
            {"oracle", RPCArg::Type::STR, RPCArg::Optional::NO, "The oracle: its id, or its key"},
            {"event", RPCArg::Type::STR, RPCArg::Optional::NO, "The event: its description (any text, whose tagged hash is its id), or its id (64 hex digits)"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "oracle", "The oracle's id"},
            {RPCResult::Type::STR_HEX, "event", "The event's id"},
            {RPCResult::Type::BOOL, "attested", "Whether the oracle attested it"},
            {RPCResult::Type::NUM, "value", /*optional=*/true, "The value it attested"},
            {RPCResult::Type::NUM, "signed_height", /*optional=*/true, "The height it signed for"},
            {RPCResult::Type::NUM, "height", /*optional=*/true, "The block that has the attestation"},
            {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Its transaction"},
        }},
        RPCExamples{HelpExampleCli("getoraclevalue", "\"prices\" \"BTCUSD 2026-12-31\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    const uint256 oracle{ParseOracle(request.params[0], state)};
    const uint256 event{ParseEvent(request.params[1])};
    UniValue result(UniValue::VOBJ);
    result.pushKV("oracle", oracle.GetHex());
    result.pushKV("event", event.GetHex());
    const auto attestation{state.GetAttestation(oracle, event)};
    result.pushKV("attested", attestation.has_value());
    if (attestation) {
        result.pushKV("value", attestation->value);
        result.pushKV("signed_height", attestation->signed_height);
        result.pushKV("height", attestation->height);
        result.pushKV("txid", attestation->txid.GetHex());
    }
    return result;
},
    };
}

RPCMethod getverifications()
{
    return RPCMethod{
        "getverifications",
        "Verified issuers (not a rule: what wallets show): what curators say of an asset. A curator is an oracle that attests "
        "\"Layers verification <asset id> #<round>\" with 1 (verified) or 0 (taken back); its word is that of its last round. "
        "Only the curators named count (the ones the user trusts: anyone can register an oracle, so no list means no verification).",
        {
            {"asset", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::AssetArgHelp()},
            {"curators", RPCArg::Type::ARR, RPCArg::Optional::NO, "The curators the user trusts (ids or keys of oracles; 1 to 200)", {{"curator", RPCArg::Type::STR, RPCArg::Optional::OMITTED, ""}}},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "asset", "The asset's id"},
            {RPCResult::Type::BOOL, "verified", "Whether one of the curators named verifies it now"},
            {RPCResult::Type::ARR, "curators", "", {{RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "curator", "The oracle's id"},
                {RPCResult::Type::STR, "name", "Its name (a label)"},
                {RPCResult::Type::BOOL, "verified", "Its word now"},
                {RPCResult::Type::NUM, "round", "Its last round attested (0: none)"},
                {RPCResult::Type::NUM, "height", /*optional=*/true, "The block of that attestation"},
            }}}},
        }},
        RPCExamples{HelpExampleCli("getverifications", "\"GOLD\" '[\"<curator>\"]'")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    return WithDexState(chainman, [&](const bitassets::State& state, [[maybe_unused]] int next_height) -> UniValue {
        const AssetId asset{ParseAsset(request.params[0], state)};
        std::vector<uint256> curators;
        const UniValue& named{request.params[1]};
        if (named.empty() || named.size() > 200) throw JSONRPCError(RPC_INVALID_PARAMETER, "Name 1 to 200 curators (the ones you trust)");
        for (const UniValue& c : named.getValues()) curators.push_back(ParseOracle(c, state));
        UniValue list(UniValue::VARR);
        bool any{false};
        for (const uint256& curator : curators) {
            int round{0};
            std::optional<bitassets::Attestation> last;
            for (int r{1}; r <= bitassets::dex::MAX_VERIFICATION_ROUNDS; ++r) {
                const auto a{state.GetAttestation(curator, bitassets::dex::EventId(bitassets::dex::VerificationEvent(asset, r)))};
                if (!a) break;
                round = r;
                last = a;
            }
            const bool verified{last && last->value > 0};
            any = any || verified;
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("curator", curator.GetHex());
            const auto record{state.GetOracle(curator)};
            obj.pushKV("name", record ? record->name : "");
            obj.pushKV("verified", verified);
            obj.pushKV("round", round);
            if (last) obj.pushKV("height", last->height);
            list.push_back(std::move(obj));
        }
        UniValue result(UniValue::VOBJ);
        result.pushKV("asset", asset.GetHex());
        result.pushKV("verified", any);
        result.pushKV("curators", std::move(list));
        return result;
    });
},
    };
}

RPCMethod getoraclecontract()
{
    return RPCMethod{
        "getoraclecontract",
        "An oracle contract: what it holds, its outcomes, and who gets it.",
        {
            {"contract", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction that made it"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "contract", "Its id"},
            {RPCResult::Type::STR_HEX, "oracle", "The oracle"},
            {RPCResult::Type::STR_HEX, "event", "The event"},
            {RPCResult::Type::STR, "asset", "What it holds"},
            {RPCResult::Type::NUM, "amount", "How much"},
            {RPCResult::Type::ARR, "outcomes", "Its outcomes, in order: the first whose range holds the value wins",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::NUM, "min", "The least value"},
                    {RPCResult::Type::NUM, "max", "The most"},
                    {RPCResult::Type::STR, "address", "Who gets it"},
                }},
            }},
            {RPCResult::Type::STR, "refund", "Who gets it if no outcome holds the value, or nothing is attested by the expiry"},
            {RPCResult::Type::NUM, "expiry", "The last block an attestation counts in"},
            {RPCResult::Type::NUM, "created", "The block that made it"},
            {RPCResult::Type::STR, "status", "\"pending\", \"settleable\" (the next block can pay it out), or \"settled\""},
            {RPCResult::Type::STR, "winner", /*optional=*/true, "Who gets it (settleable or settled)"},
            {RPCResult::Type::NUM, "settled_height", /*optional=*/true, "The block that paid it out"},
        }},
        RPCExamples{HelpExampleCli("getoraclecontract", "\"txid\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const Txid id{Txid::FromUint256(ParseHashV(request.params[0], "contract"))};
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    const auto contract{state.GetContract(id)};
    if (!contract) throw JSONRPCError(RPC_INVALID_PARAMETER, "No such oracle contract");
    const uint8_t decimals{DecimalsOf(state, contract->asset)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("contract", id.GetHex());
    result.pushKV("oracle", contract->oracle.GetHex());
    result.pushKV("event", contract->event.GetHex());
    result.pushKV("asset", LabelOf(state, contract->asset));
    result.pushKV("amount", AmountToJSON(contract->amount, decimals));
    UniValue outcomes(UniValue::VARR);
    for (const bitassets::OracleOutcome& outcome : contract->outcomes) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("min", outcome.lo);
        obj.pushKV("max", outcome.hi);
        obj.pushKV("address", AddressOf(outcome.script));
        outcomes.push_back(std::move(obj));
    }
    result.pushKV("outcomes", std::move(outcomes));
    result.pushKV("refund", AddressOf(contract->refund));
    result.pushKV("expiry", contract->expiry);
    result.pushKV("created", contract->created);
    if (contract->settled) {
        result.pushKV("status", "settled");
        result.pushKV("winner", AddressOf(contract->paid_to));
        result.pushKV("settled_height", contract->settled_height);
    } else if (const auto winner{contract->Winner(state.GetAttestation(contract->oracle, contract->event), NextHeight(chainman))}) {
        result.pushKV("status", "settleable");
        result.pushKV("winner", AddressOf(*winner));
    } else {
        result.pushKV("status", "pending");
    }
    return result;
},
    };
}

//
// Search: the search index of the assets (bitassets/index.h), a page at a time.
//

/** Run `fn` on the state at the tip and its height: a snapshot read without cs_main if one can be made. */
template <typename F>
UniValue WithIndexView(ChainstateManager& chainman, F&& fn) EXCLUSIVE_LOCKS_REQUIRED(!::cs_main)
{
    std::shared_ptr<const sidechain::StoreView> view;
    int height;
    {
        LOCK(::cs_main);
        Chainstate& chainstate{chainman.ActiveChainstate()};
        height = chainstate.m_chain.Height();
        view = chainstate.SideCache().SharedSnapshot();
        if (!view) return fn(static_cast<const sidechain::StoreView&>(chainstate.SideCache()), height);
    }
    return fn(*view, height);
}

std::vector<unsigned char> ParseCursor(const UniValue& value)
{
    if (value.isNull() || value.get_str().empty()) return {};
    const auto bytes{TryParseHex<unsigned char>(value.get_str())};
    if (!bytes) throw JSONRPCError(RPC_INVALID_PARAMETER, "The cursor is not one a search returned");
    return *bytes;
}

UniValue CursorToJSON(const std::vector<unsigned char>& next)
{
    return next.empty() ? UniValue{} : UniValue{HexStr(next)};
}

size_t ParseLimit(const UniValue& value)
{
    if (value.isNull()) return 50;
    const int limit{value.getInt<int>()};
    if (limit < 1 || static_cast<size_t>(limit) > bitassets::index::MAX_PAGE) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("limit is from 1 to %u", bitassets::index::MAX_PAGE));
    }
    return static_cast<size_t>(limit);
}

/** An asset named exactly (its number, "#" and its number, its hash): the numbers of what it names (none if nothing). */
std::optional<std::vector<uint32_t>> ExactQuery(const std::string& query, const bitassets::State& state)
{
    std::string text{util::TrimString(query)};
    if (text.starts_with('#')) text = text.substr(1);
    if (bitassets::ShapedAsNumber(text)) {
        const auto seq{bitassets::ParseSeq(text)};
        if (!seq) return std::vector<uint32_t>{};
        return std::vector<uint32_t>{*seq};
    }
    std::string hex{text};
    if (hex.starts_with("0x") || hex.starts_with("0X")) hex = hex.substr(2);
    if (hex.size() == 64 && IsHex(hex)) {
        const auto id{uint256::FromHex(hex)};
        const auto record{id ? state.GetAsset(*id) : std::nullopt};
        if (!record) return std::vector<uint32_t>{};
        return std::vector<uint32_t>{record->seq};
    }
    if (text != util::TrimString(query) || text.starts_with("0x")) return std::vector<uint32_t>{};
    return std::nullopt;
}

UniValue HitToJSON(const bitassets::index::Hit& hit)
{
    using namespace bitassets::index;
    const AssetRow& row{hit.row};
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("asset", row.id.GetHex());
    if (row.flags & NAMED) obj.pushKV("name", row.text);
    if (!row.display.empty()) obj.pushKV("display_name", row.display);
    obj.pushKV("label", row.flags & NAMED ? row.text : "0x" + row.id.GetHex());
    obj.pushKV("seq", bitassets::FormatSeq(row.seq));
    obj.pushKV("decimals", row.decimals);
    obj.pushKV("supply", AmountToJSON(row.supply, row.decimals));
    obj.pushKV("minted", AmountToJSON(row.minted, row.decimals));
    obj.pushKV("burned", AmountToJSON(row.burned, row.decimals));
    obj.pushKV("fixed", (row.flags & bitassets::index::FIXED) != 0);
    obj.pushKV("retired", (row.flags & RETIRED) != 0);
    obj.pushKV("has_logo", (row.flags & HAS_LOGO) != 0);
    obj.pushKV("has_pool", (row.flags & HAS_POOL) != 0);
    obj.pushKV("pools", row.pools);
    obj.pushKV("pool_depth", ValueFromAmount(static_cast<CAmount>(std::min<uint64_t>(row.depth, MAX_MONEY))));
    obj.pushKV("volume", ValueFromAmount(static_cast<CAmount>(std::min<uint64_t>(row.volume, MAX_MONEY))));
    obj.pushKV("created", row.created);
    obj.pushKV("registered", row.created);
    obj.pushKV("last_activity", row.last);
    static constexpr const char* MATCHES[]{"exact", "prefix", "word", "display_name", "display_word"};
    if (hit.match >= 0 && hit.match < 5) obj.pushKV("match", MATCHES[hit.match]);
    return obj;
}

/** The filters common to the three searches, read by none of the pools and auctions (an app sends the same object to all). */
bool AssetOnlyFilter(const std::string& key)
{
    static const std::set<std::string> keys{"min_supply", "has_pool", "has_logo", "active_within_blocks", "exclude_dead", "created_after",
                                            "exclude_burned", "exclude_retired", "assets", "mine"};
    return keys.contains(key);
}

std::vector<std::string> ParseKeywords(const UniValue& v)
{
    std::vector<std::string> out;
    for (const UniValue& word : v.get_array().getValues()) {
        for (std::string w : bitassets::index::Words(bitassets::index::Normalize(word.get_str()))) out.push_back(std::move(w));
    }
    if (out.size() > 100) throw JSONRPCError(RPC_INVALID_PARAMETER, "At most 100 keywords");
    return out;
}

/** Whether a label has any of the words (whole words, any case). */
bool HasKeyword(const std::string& label, const std::vector<std::string>& words)
{
    for (const std::string& w : bitassets::index::Words(bitassets::index::Normalize(label))) {
        if (std::find(words.begin(), words.end(), w) != words.end()) return true;
    }
    return false;
}

UniValue SearchAssetsImpl(ChainstateManager& chainman, const UniValue& params, const std::vector<AssetId>* only) EXCLUSIVE_LOCKS_REQUIRED(!::cs_main)
{
    using namespace bitassets::index;
    const auto param{[&](size_t i) { return i < params.size() ? params[i] : UniValue{}; }};
    Request search;
    const std::string query{param(0).isNull() ? "" : param(0).get_str()};
    const std::string sort_name{param(2).isNull() ? "relevance" : param(2).get_str()};
    const auto sort{ParseSort(sort_name)};
    if (!sort) throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown sort: " + sort_name);
    search.sort = *sort;
    search.cursor = ParseCursor(param(3));
    search.limit = ParseLimit(param(4));
    const UniValue filters{param(1)};
    std::optional<int> within;
    bool dead_default{false};
    std::vector<std::string> named;
    if (!filters.isNull()) {
        for (const std::string& key : filters.getKeys()) {
            const UniValue& v{filters[key]};
            if (v.isNull()) continue;
            if (key == "min_supply") {
                double min{0};
                if (v.isNum()) {
                    min = v.get_real();
                } else {
                    std::istringstream in{v.get_str()};
                    in.imbue(std::locale::classic());
                    if (!(in >> min) || !in.eof()) throw JSONRPCError(RPC_INVALID_PARAMETER, "min_supply is a number of whole units");
                }
                search.filters.min_supply = min;
            } else if (key == "has_pool") {
                search.filters.has_pool = v.get_bool();
            } else if (key == "has_logo") {
                search.filters.has_logo = v.get_bool();
            } else if (key == "exclude_dead" && v.isBool()) {
                dead_default = v.get_bool();
            } else if (key == "active_within_blocks" || key == "exclude_dead") {
                const int n{v.getInt<int>()};
                if (n < 1) throw JSONRPCError(RPC_INVALID_PARAMETER, key + " is a number of blocks, at least 1");
                within = within ? std::min(*within, n) : n;
            } else if (key == "created_after") {
                search.filters.created_after = v.getInt<int>();
            } else if (key == "exclude_burned") {
                search.filters.exclude_burned = v.get_bool();
            } else if (key == "exclude_retired") {
                search.filters.exclude_retired = v.get_bool();
            } else if (key == "exclude_keywords") {
                search.filters.exclude_keywords = ParseKeywords(v);
            } else if (key == "assets") {
                if (v.size() > CANDIDATE_CAP) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("At most %u assets", CANDIDATE_CAP));
                for (const UniValue& a : v.getValues()) named.push_back(a.get_str());
            } else if (key == "mine") {
                if (v.get_bool() && !only) throw JSONRPCError(RPC_INVALID_PARAMETER, "mine is the wallet's: searchmyassets (a wallet command, with the same arguments)");
            } else {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown filter: " + key);
            }
        }
    }
    if (dead_default && !within) {
        // As dead: nothing for about a day (the volume window).
        const int window{chainman.GetConsensus().sidechain.asset_volume_window};
        within = window > 0 ? window : 1440;
    }
    std::vector<Hit> hits;
    UniValue result(UniValue::VOBJ);
    int height{0};
    WithIndexView(chainman, [&](const sidechain::StoreView& view, int tip) {
        height = tip;
        const bitassets::State state{view};
        if (within) search.filters.min_last = tip - *within + 1;
        if (only || !named.empty()) {
            std::set<uint32_t> seqs;
            const auto add{[&](const AssetId& id) {
                if (const auto record{state.GetAsset(id)}) seqs.insert(record->seq);
            }};
            if (only) for (const AssetId& id : *only) add(id);
            std::set<uint32_t> listed;
            for (const std::string& name : named) {
                const AssetId id{ParseAsset(UniValue{name}, state)};
                if (const auto record{state.GetAsset(id)}) listed.insert(record->seq);
            }
            if (!named.empty()) {
                if (only) {
                    std::set<uint32_t> both;
                    std::set_intersection(seqs.begin(), seqs.end(), listed.begin(), listed.end(), std::inserter(both, both.end()));
                    seqs = std::move(both);
                } else {
                    seqs = std::move(listed);
                }
            }
            search.filters.only = std::move(seqs);
        }
        if (auto exact{ExactQuery(query, state)}) {
            search.exact = std::move(*exact);
        } else {
            search.text = query;
        }
        Page page;
        try {
            page = Search(view, search);
        } catch (const std::invalid_argument& e) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, e.what());
        }
        hits = std::move(page.hits);
        result.pushKV("next_cursor", CursorToJSON(page.next));
        result.pushKV("partial", page.partial);
        result.pushKV("scanned", page.scanned);
        return UniValue{};
    });
    // Each with what getasset shows of it now (its logo, data, controller), unless retired.
    UniValue assets(UniValue::VARR);
    {
        LOCK(::cs_main);
        const bitassets::State state{StateOf(chainman)};
        for (const Hit& hit : hits) {
            UniValue obj{HitToJSON(hit)};
            if (const auto record{state.GetAsset(hit.row.id)}; record && record->seq == hit.row.seq) {
                const UniValue more{AssetToJSON(chainman.ActiveChainstate(), state, hit.row.id, *record, std::nullopt)};
                for (const std::string& key : more.getKeys()) {
                    if (!obj.exists(key)) obj.pushKV(key, more[key]);
                }
            }
            assets.push_back(std::move(obj));
        }
    }
    UniValue out(UniValue::VOBJ);
    out.pushKV("assets", std::move(assets));
    for (const std::string& key : result.getKeys()) out.pushKV(key, result[key]);
    out.pushKV("height", height);
    return out;
}

RPCMethod searchassets()
{
    return RPCMethod{
        "searchassets",
        "Search, filter and sort the assets, a page at a time, by the node's search index (getassetindexinfo): a page\n"
        "never reads the assets whole, however many there are. Go on with the cursor \"next_cursor\" a page returns.\n"
        "A text matches the whole name, its start, or the start of any of its words, in any case; \"#\" and a number,\n"
        "a number (\"1739-0029\"), or a hash (\"0x\" and 64 hex digits), that asset only.\n"
        "A search stops at a budget of index entries it looks at; a page cut short by it says \"partial\": go on with\n"
        "\"next_cursor\" (filters that few assets pass, along an order that is not theirs, take several calls).\n"
        "What the wallet holds: searchmyassets (a wallet command), with the same arguments.",
        bitassets::SearchAssetsArgs(),
        bitassets::SearchAssetsResult(),
        RPCExamples{HelpExampleCli("searchassets", "\"gold\"") + HelpExampleCli("-named searchassets", "query=gold filters='{\"has_pool\":true}' sort=volume limit=20") +
                    HelpExampleCli("searchassets", "\"\" '{\"exclude_dead\":5760,\"exclude_burned\":true}' newest") + HelpExampleRpc("searchassets", "\"gold\", {}, \"supply\", null, 100")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    UniValue params(UniValue::VARR);
    for (size_t i{0}; i < 5; ++i) params.push_back(i < request.params.size() ? request.params[i] : UniValue{});
    return SearchAssetsImpl(chainman, params, nullptr);
},
    };
}

/** The assets a text names in a search of pools or auctions, bounded (bitassets::index::MatchingAssets). */
std::optional<std::vector<AssetId>> AssetsOfText(const sidechain::StoreView& view, const std::string& query, bool& partial)
{
    using namespace bitassets::index;
    if (util::TrimString(query).empty()) return std::nullopt;
    const bitassets::State state{view};
    Request search;
    if (auto exact{ExactQuery(query, state)}) {
        search.exact = std::move(*exact);
    } else {
        search.text = query;
    }
    return MatchingAssets(view, search, 1000, partial);
}

const RPCResult NEXT_CURSOR{RPCResult::Type::ANY, "next_cursor", "The cursor of the next page (a string); null when there is no more (a full page always has one: the next may be empty)"};

/** The sorts of the pools, the app's names of the sorts of the assets mapped onto them. */
bitassets::index::PoolSort PoolSortOf(const std::string& name)
{
    using bitassets::index::PoolSort;
    if (const auto sort{bitassets::index::ParsePoolSort(name)}) return *sort;
    if (name == "volume" || name == "activity") return PoolSort::VOLUME;
    if (name == "newest") return PoolSort::NEWEST;
    if (name == "relevance" || name == "pool_depth" || name == "name" || name == "supply" || name == "oldest") return PoolSort::DEPTH;
    throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown sort: " + name);
}

RPCMethod searchpools()
{
    return RPCMethod{
        "searchpools",
        "Search and sort the pools, a page at a time, by the node's search index. A text matches the names of their assets as\n"
        "searchassets matches them (the pools of the first 1000 assets it matches, found within the search's budget).\n"
        "No text and no filters (the default): every pool of every kind (constant product, stable, weighted, concentrated,\n"
        "launch curve, book) that someone provides liquidity to, the pools with " + CURRENCY_UNIT + " by depth first, then the others, newest\n"
        "first. A first argument that is a JSON object (searchpools '{...}') is taken for the filters, with no text.",
        {
            {"query", RPCArg::Type::STR, RPCArg::Default{""}, "Text, a number or a hash, matched against either asset"},
            {"filters", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "(the filters of searchassets that are not these are taken and left aside)",
            {
                {"exclude_abandoned", RPCArg::Type::BOOL, RPCArg::Default{true}, "Leave out those nobody provides liquidity to"},
                {bitassets::CoinOnlyFilter(), RPCArg::Type::BOOL, RPCArg::Default{false}, "Only pools with " + CURRENCY_UNIT},
                {"min_depth", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "At least this much " + CURRENCY_UNIT + " in the pool"},
                {"exclude_keywords", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "Leave out those with an asset with any of these words in its name",
                    {{"keyword", RPCArg::Type::STR, RPCArg::Optional::OMITTED, ""}}},
            }},
            {"sort", RPCArg::Type::STR, RPCArg::Default{"depth"}, "depth (" + CURRENCY_UNIT + " held, most first; then the pools without " + CURRENCY_UNIT + ", newest first), volume (" + CURRENCY_UNIT + " traded in the window; those traded only), "
                "price_change (of the second asset in the first, " + CURRENCY_UNIT + " for a pool with " + CURRENCY_UNIT + ", over the window; largest rise first), newest. "
                "The sorts of searchassets map onto these: relevance, pool_depth, name, supply, oldest: depth; activity: volume"},
            {"cursor", RPCArg::Type::STR_HEX, RPCArg::Default{""}, "\"next_cursor\" of the page before"},
            {"limit", RPCArg::Type::NUM, RPCArg::Default{50}, strprintf("From 1 to %u", bitassets::index::MAX_PAGE)},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::ARR, "pools", "", {{RPCResult::Type::OBJ, "", "", [] {
                std::vector<RPCResult> results{PoolResultsWithKind()};
                results.push_back({RPCResult::Type::NUM, "depth", "The " + CURRENCY_UNIT + " it holds (0 for a pool without " + CURRENCY_UNIT + ")"});
                results.push_back({RPCResult::Type::NUM, "volume_window", CURRENCY_UNIT + " traded over the volume window"});
                results.push_back({RPCResult::Type::NUM, "price_change", "The change of the price of asset_b in asset_a over the window, in percent"});
                results.push_back({RPCResult::Type::NUM, "last_activity", "The height of the last block that touched it"});
                return results;
            }()}}},
            NEXT_CURSOR,
            {RPCResult::Type::BOOL, "partial", "The search stopped at a budget: go on with next_cursor (or narrow the text)"},
            {RPCResult::Type::NUM, "scanned", "Entries looked at"},
            {RPCResult::Type::NUM, "height", "The height of the tip"},
        }},
        RPCExamples{HelpExampleCli("searchpools", "") + HelpExampleCli("searchpools", "'{\"exclude_abandoned\": false}'") + HelpExampleCli("searchpools", "\"gold\" '{}' volume")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    using namespace bitassets::index;
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    PoolRequest search;
    std::string query{request.params[0].isNull() ? "" : request.params[0].get_str()};
    UniValue filters{request.params[1]};
    // `searchpools {}` (the filters given first, the text left out): no text, those filters, not the text "{}".
    if (filters.isNull() && !query.empty() && query.front() == '{') {
        if (UniValue parsed; parsed.read(query) && parsed.isObject()) {
            filters = std::move(parsed);
            query.clear();
        }
    }
    search.sort = PoolSortOf(request.params[2].isNull() ? "depth" : request.params[2].get_str());
    search.cursor = ParseCursor(request.params[3]);
    search.limit = ParseLimit(request.params[4]);
    std::vector<std::string> keywords;
    if (!filters.isNull()) {
        if (!filters.isObject()) throw JSONRPCError(RPC_TYPE_ERROR, "filters: an object");
        for (const std::string& key : filters.getKeys()) {
            const UniValue& v{filters[key]};
            if (v.isNull() || AssetOnlyFilter(key)) continue;
            if (key == "exclude_abandoned") {
                search.exclude_abandoned = v.get_bool();
            } else if (key == bitassets::CoinOnlyFilter() || key == "ecx_only" || key == "chn_only") {
                search.chn_only = v.get_bool();
            } else if (key == "min_depth") {
                search.min_depth = static_cast<uint64_t>(AmountFromValue(v));
            } else if (key == "exclude_keywords") {
                keywords = ParseKeywords(v);
            } else {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown filter: " + key);
            }
        }
    }
    return WithIndexView(chainman, [&](const sidechain::StoreView& view, int height) {
        const bitassets::State state{view};
        bool partial{false};
        search.assets = AssetsOfText(view, query, partial);
        PoolPage page;
        try {
            page = SearchPools(view, search);
        } catch (const std::invalid_argument& e) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, e.what());
        }
        UniValue pools(UniValue::VARR);
        for (const PoolRow& row : page.pools) {
            const auto pool{state.GetPool(row.id)};
            if (!pool) continue;
            // A pool with CHN shows its price in CHN.
            UniValue obj{PoolToJSON(state, row.id, *pool, pool->asset0.IsNull() ? pool->asset1 : pool->asset0)};
            if (!keywords.empty() && (HasKeyword(obj["asset_a"].get_str(), keywords) || HasKeyword(obj["asset_b"].get_str(), keywords))) continue;
            obj.pushKV("depth", ValueFromAmount(static_cast<CAmount>(std::min<uint64_t>(row.Depth(), MAX_MONEY))));
            obj.pushKV("volume_window", ValueFromAmount(static_cast<CAmount>(std::min<uint64_t>(row.volume, MAX_MONEY))));
            UniValue change;
            change.setNumStr(strprintf("%.4f", row.ChangePpm() / 1e4));
            obj.pushKV("price_change", change);
            obj.pushKV("last_activity", row.last);
            pools.push_back(std::move(obj));
        }
        UniValue result(UniValue::VOBJ);
        result.pushKV("pools", std::move(pools));
        result.pushKV("next_cursor", CursorToJSON(page.next));
        result.pushKV("partial", page.partial || partial);
        result.pushKV("scanned", page.scanned);
        result.pushKV("height", height);
        return result;
    });
},
    };
}

RPCMethod searchauctions()
{
    return RPCMethod{
        "searchauctions",
        "Search and sort the auctions, a page at a time, by the node's search index. A text matches the names of what they sell or\n"
        "sell for, as searchassets matches them (auctions not closed only).",
        {
            {"query", RPCArg::Type::STR, RPCArg::Default{""}, "Text, a number or a hash, matched against either asset"},
            {"filters", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "(the filters of searchassets that are not these are taken and left aside)",
            {
                {"status", RPCArg::Type::STR, RPCArg::Default{"active"}, "open, upcoming, active (open or upcoming), ended (not collected yet) or any (not closed)"},
                {"include_closed", RPCArg::Type::BOOL, RPCArg::Default{false}, "Also those collected (sort newest, no text)"},
                {"exclude_keywords", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "Leave out those of an asset with any of these words in its name",
                    {{"keyword", RPCArg::Type::STR, RPCArg::Optional::OMITTED, ""}}},
            }},
            {"sort", RPCArg::Type::STR, RPCArg::Default{"ending_soon"}, "ending_soon (by last block) or newest (any other sort of searchassets: ending_soon, but newest)"},
            {"cursor", RPCArg::Type::STR_HEX, RPCArg::Default{""}, "\"next_cursor\" of the page before"},
            {"limit", RPCArg::Type::NUM, RPCArg::Default{50}, strprintf("From 1 to %u", bitassets::index::MAX_PAGE)},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::ARR, "auctions", "", {{RPCResult::Type::OBJ, "", "", AuctionResults()}}},
            NEXT_CURSOR,
            {RPCResult::Type::BOOL, "partial", "The search stopped at a budget: go on with next_cursor (or narrow the text)"},
            {RPCResult::Type::NUM, "scanned", "Entries looked at"},
            {RPCResult::Type::NUM, "height", "The height of the next block, which the status and prices are for"},
        }},
        RPCExamples{HelpExampleCli("searchauctions", "") + HelpExampleCli("searchauctions", "\"gold\" '{\"status\":\"open\"}' newest")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    using namespace bitassets::index;
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    AuctionRequest search;
    const std::string query{request.params[0].isNull() ? "" : request.params[0].get_str()};
    const std::string sort_name{request.params[2].isNull() ? "ending_soon" : request.params[2].get_str()};
    search.sort = sort_name == "newest" ? AuctionSort::NEWEST : AuctionSort::ENDING_SOON;
    if (!ParseAuctionSort(sort_name) && !ParseSort(sort_name)) throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown sort: " + sort_name);
    search.cursor = ParseCursor(request.params[3]);
    search.limit = ParseLimit(request.params[4]);
    std::vector<std::string> keywords;
    if (const UniValue& filters{request.params[1]}; !filters.isNull()) {
        for (const std::string& key : filters.getKeys()) {
            const UniValue& v{filters[key]};
            if (v.isNull() || AssetOnlyFilter(key)) continue;
            if (key == "status") {
                search.status = v.get_str();
                if (search.status != "open" && search.status != "upcoming" && search.status != "active" && search.status != "ended" && search.status != "any") {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown status: " + search.status);
                }
            } else if (key == "include_closed") {
                search.include_closed = v.get_bool();
            } else if (key == "exclude_keywords") {
                keywords = ParseKeywords(v);
            } else {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown filter: " + key);
            }
        }
    }
    std::vector<std::pair<Txid, bitassets::Auction>> found;
    UniValue result(UniValue::VOBJ);
    WithIndexView(chainman, [&](const sidechain::StoreView& view, int height) {
        bool partial{false};
        search.assets = AssetsOfText(view, query, partial);
        search.height = height + 1;
        AuctionPage page;
        try {
            page = SearchAuctions(view, search);
        } catch (const std::invalid_argument& e) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, e.what());
        }
        found = std::move(page.auctions);
        result.pushKV("next_cursor", CursorToJSON(page.next));
        result.pushKV("partial", page.partial || partial);
        result.pushKV("scanned", page.scanned);
        return UniValue{};
    });
    // Shown as they are now, with who holds their receipts.
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    const int height{chainman.ActiveChain().Height() + 1};
    UniValue auctions(UniValue::VARR);
    for (const auto& [id, auction] : found) {
        const auto now{state.GetAuction(id)};
        if (!now) continue;
        UniValue obj{AuctionToJSON(chainman.ActiveChainstate(), state, id, *now, height)};
        if (!keywords.empty() && (HasKeyword(obj["base"].get_str(), keywords) || HasKeyword(obj["quote"].get_str(), keywords))) continue;
        auctions.push_back(std::move(obj));
    }
    UniValue out(UniValue::VOBJ);
    out.pushKV("auctions", std::move(auctions));
    for (const std::string& key : result.getKeys()) out.pushKV(key, result[key]);
    out.pushKV("height", height);
    return out;
},
    };
}

RPCMethod getassetindexinfo()
{
    return RPCMethod{
        "getassetindexinfo",
        "The search index of the assets (searchassets, searchpools, searchauctions). It is derived data kept with the state of the\n"
        "chain, block by block: always at the tip, undone with the blocks by a reorganisation, nothing to enable or sync apart.",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::BOOL, "synced", "Whether it is at the tip (always: it is updated with each block)"},
            {RPCResult::Type::NUM, "height", "The height of the tip it is of"},
            {RPCResult::Type::STR_HEX, "bestblock", "The tip"},
            {RPCResult::Type::NUM, "count", "Assets indexed, the retired ones included"},
            {RPCResult::Type::NUM, "retired", "Of which retired"},
            {RPCResult::Type::NUM, "pools", "Pools indexed"},
            {RPCResult::Type::NUM, "volume_window", "The blocks volumes and price changes are over"},
            {RPCResult::Type::NUM, "max_page", "The most a page holds"},
        }},
        RPCExamples{HelpExampleCli("getassetindexinfo", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    LOCK(::cs_main);
    Chainstate& chainstate{chainman.ActiveChainstate()};
    const bitassets::index::Info info{bitassets::index::GetInfo(chainstate.SideCache())};
    UniValue result(UniValue::VOBJ);
    result.pushKV("synced", true);
    result.pushKV("height", chainstate.m_chain.Height());
    result.pushKV("bestblock", chainstate.m_chain.Tip()->GetBlockHash().GetHex());
    result.pushKV("count", info.assets);
    result.pushKV("retired", info.retired);
    result.pushKV("pools", info.pools);
    result.pushKV("volume_window", chainman.GetConsensus().sidechain.asset_volume_window);
    result.pushKV("max_page", static_cast<uint64_t>(bitassets::index::MAX_PAGE));
    return result;
},
    };
}

//
// The DEX's pool kinds (bitassets/curves.h): quotes over every pool of a pair, a simulated batch, a preview of
// a pool before it is made, and the depth of a pool.
//

/** A round of a pool now (any kind, at the fee it charges next), and the pool after it; `preview`: a pool not made yet, of that kind. */
struct RoundResult {
    bitassets::dex::Clearing clearing;
    bitassets::Pool after;
    std::optional<bitassets::ClState> cl_before, cl_after;
};
std::optional<RoundResult> RoundOf(const bitassets::State& state, const uint256& id, const bitassets::Pool& pool, const std::vector<bitassets::dex::HopIn>& orders,
                                   const std::optional<bitassets::PoolParams>& preview = std::nullopt)
{
    const bitassets::PoolParams kind{preview ? *preview : state.GetPoolParams(id)};
    const uint32_t fee{preview ? kind.fee : state.QuoteFee(id, kind)};
    RoundResult r;
    r.after = pool;
    std::optional<bitassets::dex::Clearing> cleared;
    if (kind.GetCurve() == bitassets::Curve::CONCENTRATED) {
        r.cl_before = state.GetClState(id);
        if (!r.cl_before) return std::nullopt;
        r.cl_after = r.cl_before;
        std::map<int32_t, bitassets::ClTick> ticks;
        cleared = state.ClearCl(id, fee, pool, *r.cl_after, ticks, orders);
    } else if (kind.GetCurve() == bitassets::Curve::BOOK) {
        std::map<uint256, bitassets::BookOrder> resting;
        cleared = state.ClearBook(id, pool, resting, orders, 0);
    } else if (kind.GetCurve() == bitassets::Curve::LAUNCH) {
        const auto launch{state.GetLaunch(pool.asset1)};
        if (!launch) return std::nullopt;
        cleared = bitassets::curves::ClearLaunch(bitassets::curves::LaunchCurve{launch->fee, launch->CurveEnd(), launch->max_buy, launch->min_buy, launch->StrictEnd(), launch->StartBound()}, pool.reserve0, pool.reserve1, orders,
                                                 std::vector<bool>(orders.size(), true));
    } else {
        cleared = bitassets::curves::ClearPoolOf(kind, fee, pool.reserve0, pool.reserve1, orders);
    }
    if (!cleared) return std::nullopt;
    r.clearing = *cleared;
    r.after.reserve0 = cleared->reserve0;
    r.after.reserve1 = cleared->reserve1;
    return r;
}

/** What `amount_in` of `in` gives alone in a pool, and the measures of the trade, as an object. */
UniValue TradeToJSON(const bitassets::State& state, const uint256& id, const bitassets::Pool& pool, const AssetId& in, uint64_t amount_in, uint64_t amount_out)
{
    const bitassets::PoolParams kind{state.GetPoolParams(id)};
    const AssetId out{pool.asset0 == in ? pool.asset1 : pool.asset0};
    const uint8_t di{DecimalsOf(state, in)}, dout{DecimalsOf(state, out)};
    const bool zero_in{pool.asset0 == in};
    const auto round{RoundOf(state, id, pool, {{zero_in, amount_in}})};
    const bitassets::ClState* cl_before{round && round->cl_before ? &*round->cl_before : nullptr};
    const double spot{WholeSpotPrice(kind, pool, zero_in, di, dout, cl_before)};
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("pool", id.GetHex());
    PushKind(obj, kind, !zero_in);
    obj.pushKV("amount_in", AmountToJSON(amount_in, di));
    obj.pushKV("amount_out", AmountToJSON(amount_out, dout));
    const double whole_in{amount_in / std::pow(10.0, di)}, whole_out{amount_out / std::pow(10.0, dout)};
    obj.pushKV("price", DecimalToJSON(whole_out > 0 ? whole_in / whole_out : 0));
    obj.pushKV("spot_price", DecimalToJSON(spot > 0 ? 1 / spot : 0));
    // The price of what comes out, worse than the spot's by: the fee included.
    const double impact{spot > 0 && whole_in > 0 ? std::clamp(100.0 * (1.0 - (whole_out / whole_in) / spot), 0.0, 100.0) : 100.0};
    UniValue impact_value;
    impact_value.setNumStr(strprintf("%.4f", impact));
    obj.pushKV("price_impact", impact_value);
    obj.pushKV("fee_paid", AmountToJSON(static_cast<uint64_t>(static_cast<unsigned __int128>(amount_in) * state.QuoteFee(id, kind) / bitassets::FEE_DENOMINATOR), di));
    if (round) {
        const double next{WholeSpotPrice(kind, round->after, zero_in, di, dout, round->cl_after ? &*round->cl_after : nullptr)};
        obj.pushKV("spot_price_after", DecimalToJSON(next > 0 ? 1 / next : 0));
    }
    obj.pushKV("abandoned", bitassets::amm::Abandoned(pool));
    return obj;
}

std::vector<RPCResult> TradeResults()
{
    std::vector<RPCResult> results{
        {RPCResult::Type::STR_HEX, "pool", "The pool"},
    };
    for (RPCResult& r : KindResults()) results.push_back(std::move(r));
    for (RPCResult r : std::vector<RPCResult>{
             {RPCResult::Type::NUM, "amount_in", "What goes in"},
             {RPCResult::Type::NUM, "amount_out", "What comes out, alone in its block"},
             {RPCResult::Type::NUM, "price", "What one of what comes out costs, in what goes in"},
             {RPCResult::Type::NUM, "spot_price", "The same at the pool's price before"},
             {RPCResult::Type::NUM, "price_impact", "How much worse the trade's price is than the spot price, in percent (the fee included)"},
             {RPCResult::Type::NUM, "fee_paid", "What the pool keeps of what goes in"},
             {RPCResult::Type::NUM, "spot_price_after", /*optional=*/true, "The pool's price after the trade"},
             {RPCResult::Type::BOOL, "abandoned", "Whether nobody provides liquidity to it"},
         }) {
        results.push_back(std::move(r));
    }
    return results;
}

RPCMethod getswapquote()
{
    return RPCMethod{
        "getswapquote",
        "What a trade gives now in every pool of a pair, of every kind, and through the best route over all pools: for an amount in, what "
        "comes out; or (exact_out) the least that has to go in for an amount out. Each as the only order of its block would get it.",
        {
            {"asset_in", RPCArg::Type::STR, RPCArg::Optional::NO, "What is paid in. " + bitassets::AssetArgHelp()},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "How much goes in (or comes out, with exact_out)"},
            {"asset_out", RPCArg::Type::STR, RPCArg::Optional::NO, "What comes out"},
            {"options", RPCArg::Type::OBJ_NAMED_PARAMS, RPCArg::Optional::OMITTED, "",
            {
                {"exact_out", RPCArg::Type::BOOL, RPCArg::Default{false}, "The amount is what comes out"},
                {"max_hops", RPCArg::Type::NUM, RPCArg::Default{4}, "Most pools on a route (1 to 4); 0: no route, the pair's pools only"},
            }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::ARR, "pools", "The pair's pools that trade it, best first", {{RPCResult::Type::OBJ, "", "", TradeResults()}}},
            {RPCResult::Type::STR_HEX, "best_pool", /*optional=*/true, "The pair's pool that gives most (or takes least)"},
            {RPCResult::Type::OBJ, "route", /*optional=*/true, "The best route over all pools (exact_out: none)",
            {
                {RPCResult::Type::ARR, "path", "Its assets", {{RPCResult::Type::STR, "", "An asset's label"}}},
                {RPCResult::Type::ARR, "pools", "Its pools", {{RPCResult::Type::STR_HEX, "", "A pool"}}},
                {RPCResult::Type::NUM, "amount_out", "What it gives"},
                {RPCResult::Type::BOOL, "default_pools", "Whether swaproute's path alone names its pools"},
            }},
        }},
        RPCExamples{HelpExampleCli("getswapquote", "\"" + CURRENCY_UNIT + "\" 1 \"GOLD\"") + HelpExampleCli("-named getswapquote", "asset_in=GOLD amount=5 asset_out=" + CURRENCY_UNIT + " exact_out=true")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const UniValue& options{request.params[3]};
    const bool exact_out{options.exists("exact_out") && options["exact_out"].get_bool()};
    const int max_hops{options.exists("max_hops") ? options["max_hops"].getInt<int>() : 4};
    if (max_hops < 0 || max_hops > static_cast<int>(bitassets::MAX_ROUTE_ASSETS - 1)) throw JSONRPCError(RPC_INVALID_PARAMETER, "max_hops is 0 to 4");
    return WithDexState(chainman, [&](const bitassets::State& state, [[maybe_unused]] int next_height) -> UniValue {
        const AssetId in{ParseAsset(request.params[0], state)}, out{ParseAsset(request.params[2], state)};
        if (in == out) throw JSONRPCError(RPC_INVALID_PARAMETER, "The same asset in and out");
        const uint8_t di{DecimalsOf(state, in)}, dout{DecimalsOf(state, out)};
        const uint64_t amount{bitassets::ParseUnits(request.params[1], exact_out ? dout : di)};
        struct Quote {
            uint256 id;
            bitassets::Pool pool;
            uint64_t in, out;
        };
        std::vector<Quote> quotes;
        for (const auto& [id, pool] : state.PoolsOf(in)) {
            if ((pool.asset0 == in ? pool.asset1 : pool.asset0) != out || bitassets::amm::Abandoned(pool)) continue;
            const bool zero_in{pool.asset0 == in};
            const auto alone{[&](uint64_t x) -> uint64_t {
                const auto r{RoundOf(state, id, pool, {{zero_in, x}})};
                return r ? r->clearing.outs[0] : 0;
            }};
            if (exact_out) {
                // The least in that gives the amount out (what comes out never falls as more goes in).
                const uint64_t reserve_in{zero_in ? pool.reserve0 : pool.reserve1};
                if (reserve_in >= bitassets::MAX_AMOUNT) continue;
                const uint64_t most{bitassets::MAX_AMOUNT - reserve_in};
                if (alone(most) < amount) continue;
                uint64_t lo{1}, hi{most};
                while (lo < hi) {
                    const uint64_t mid{lo + (hi - lo) / 2};
                    if (alone(mid) >= amount) {
                        hi = mid;
                    } else {
                        lo = mid + 1;
                    }
                }
                quotes.push_back({id, pool, lo, alone(lo)});
            } else {
                const uint64_t got{alone(amount)};
                if (got == 0) continue;
                quotes.push_back({id, pool, amount, got});
            }
        }
        std::sort(quotes.begin(), quotes.end(), [&](const Quote& a, const Quote& b) {
            if (exact_out ? a.in != b.in : a.out != b.out) return exact_out ? a.in < b.in : a.out > b.out;
            return a.id < b.id;
        });
        UniValue result(UniValue::VOBJ);
        UniValue list(UniValue::VARR);
        for (const Quote& q : quotes) list.push_back(TradeToJSON(state, q.id, q.pool, in, q.in, q.out));
        result.pushKV("pools", std::move(list));
        if (!quotes.empty()) result.pushKV("best_pool", quotes.front().id.GetHex());
        if (!exact_out && max_hops > 0) {
            const auto routes{bitassets::dex::FindRoutes(state, in, out, amount, max_hops, 1, 20000, 16, chainman.GetConsensus().sidechain.DexCurvesActive(next_height))};
            if (!routes.empty()) {
                UniValue route(UniValue::VOBJ);
                UniValue path(UniValue::VARR), pools(UniValue::VARR);
                for (const AssetId& a : routes[0].path) path.push_back(LabelOf(state, a));
                for (const uint256& id : routes[0].pools) pools.push_back(id.GetHex());
                route.pushKV("path", std::move(path));
                route.pushKV("pools", std::move(pools));
                route.pushKV("amount_out", AmountToJSON(routes[0].outs.back(), dout));
                route.pushKV("default_pools", routes[0].legacy);
                result.pushKV("route", std::move(route));
            }
        }
        return result;
    });
},
    };
}

RPCMethod simulateclearing()
{
    return RPCMethod{
        "simulateclearing",
        "What a round of a pool's batch clearing would give these orders together now (doc/layers.md, \"Pool kinds\"): one price for all, "
        "the pool's net trade on its curve. Nothing is sent.",
        {
            {"pool", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The pool"},
            {"orders", RPCArg::Type::ARR, RPCArg::Optional::NO, "The orders",
            {
                {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                {
                    {"asset", RPCArg::Type::STR, RPCArg::Optional::NO, "What it pays in: one of the pool's assets"},
                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "How much"},
                }},
            }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR, "asset_a", "The pool's first asset"},
            {RPCResult::Type::STR, "asset_b", "Its second"},
            {RPCResult::Type::ARR, "outs", "What each order gets, in order", {{RPCResult::Type::NUM, "", ""}}},
            {RPCResult::Type::NUM, "price", /*optional=*/true, "The uniform price of a whole asset_a in asset_b (from the orders that pay asset_a in, else those that pay asset_b)"},
            {RPCResult::Type::NUM, "reserve_a", "What the pool holds after"},
            {RPCResult::Type::NUM, "reserve_b", ""},
            {RPCResult::Type::NUM, "spot_before", "The pool's price before, of a whole asset_a in asset_b"},
            {RPCResult::Type::NUM, "spot_after", "And after"},
        }},
        RPCExamples{HelpExampleCli("simulateclearing", "\"<pool>\" '[{\"asset\":\"GOLD\",\"amount\":10},{\"asset\":\"" + CURRENCY_UNIT + "\",\"amount\":1}]'")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const uint256 id{ParseHashV(request.params[0], "pool")};
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    const auto pool{state.GetPool(id)};
    if (!pool) throw JSONRPCError(RPC_INVALID_PARAMETER, "No such pool");
    const bitassets::PoolParams kind{state.GetPoolParams(id)};
    const uint8_t d0{DecimalsOf(state, pool->asset0)}, d1{DecimalsOf(state, pool->asset1)};
    std::vector<bitassets::dex::HopIn> orders;
    for (const UniValue& order : request.params[1].getValues()) {
        const AssetId asset{ParseAsset(order["asset"], state)};
        if (asset != pool->asset0 && asset != pool->asset1) throw JSONRPCError(RPC_INVALID_PARAMETER, "An order pays in an asset the pool does not hold");
        orders.push_back({asset == pool->asset0, bitassets::ParseUnits(order["amount"], asset == pool->asset0 ? d0 : d1)});
    }
    if (orders.empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, "No orders");
    if (bitassets::amm::Abandoned(*pool)) throw JSONRPCError(RPC_INVALID_PARAMETER, "Nobody provides liquidity to this pool: orders through it are refunded");
    const auto round{RoundOf(state, id, *pool, orders)};
    if (!round) throw JSONRPCError(RPC_INVALID_PARAMETER, "The pool cannot clear these orders (amounts beyond its bounds)");
    const bitassets::dex::Clearing* cleared{&round->clearing};
    UniValue result(UniValue::VOBJ);
    result.pushKV("asset_a", LabelOf(state, pool->asset0));
    result.pushKV("asset_b", LabelOf(state, pool->asset1));
    UniValue outs(UniValue::VARR);
    std::optional<double> price;
    for (size_t i{0}; i < orders.size(); ++i) {
        outs.push_back(AmountToJSON(cleared->outs[i], orders[i].zero_in ? d1 : d0));
        if (cleared->outs[i] == 0) continue;
        const double in_whole{orders[i].amount / std::pow(10.0, orders[i].zero_in ? d0 : d1)}, out_whole{cleared->outs[i] / std::pow(10.0, orders[i].zero_in ? d1 : d0)};
        // After the fee: the uniform price is what every order of a side gets for what it pays, less the fee.
        const double keep{1.0 - kind.fee / double(bitassets::FEE_DENOMINATOR)};
        if (orders[i].zero_in && !price) price = out_whole / (in_whole * keep);
        if (!orders[i].zero_in && !price) price = (in_whole * keep) / out_whole;
    }
    result.pushKV("outs", std::move(outs));
    if (price) result.pushKV("price", DecimalToJSON(*price));
    result.pushKV("reserve_a", AmountToJSON(cleared->reserve0, d0));
    result.pushKV("reserve_b", AmountToJSON(cleared->reserve1, d1));
    result.pushKV("spot_before", DecimalToJSON(WholeSpotPrice(kind, *pool, true, d0, d1, round->cl_before ? &*round->cl_before : nullptr)));
    result.pushKV("spot_after", DecimalToJSON(WholeSpotPrice(kind, round->after, true, d0, d1, round->cl_after ? &*round->cl_after : nullptr)));
    return result;
},
    };
}

/** The pool kind an options object names (createpool, previewpool): curve, fee, amp, weight_a. */
bitassets::CreatePool ParseKindOptions(const UniValue& options)
{
    const UniValue& p_options{options};
    bitassets::CreatePool create;
    const std::string curve_name{options.exists("curve") ? options["curve"].get_str() : "constant_product"};
    const auto curve{bitassets::curves::ParseCurve(curve_name)};
    if (!curve) throw JSONRPCError(RPC_INVALID_PARAMETER, "curve is constant_product, stable or weighted");
    create.curve = static_cast<uint8_t>(*curve);
    // The fee in percent: 0.01, 0.05, 0.3 or 1 (default: 0.05 for stable pools, 0.3 for the others).
    const double fee_percent{options.exists("fee") ? options["fee"].get_real() : (*curve == bitassets::Curve::STABLE ? 0.05 : 0.3)};
    create.fee = static_cast<uint32_t>(std::llround(fee_percent * 10000));
    if (*curve == bitassets::Curve::STABLE) create.amp = options.exists("amp") ? options["amp"].getInt<uint32_t>() : 100;
    if (*curve == bitassets::Curve::WEIGHTED) {
        if (!options.exists("weight_a")) throw JSONRPCError(RPC_INVALID_PARAMETER, "A weighted pool needs weight_a, the first asset's weight in percent");
        create.weight_a = static_cast<uint8_t>(options["weight_a"].getInt<int>());
    }
    // The fee that rises with the pool's volatility (dynamic_fee): any curve.
    const UniValue& dyn{p_options.exists("dynamic_fee") ? p_options["dynamic_fee"] : NullUniValue};
    if (!dyn.isNull() && dyn.get_bool()) create.flags |= bitassets::POOL_FLAG_DYNAMIC_FEE;
    if (const auto why{bitassets::WhyNotPoolKind(create.curve, create.fee, create.amp, create.weight_a, create.flags)}) throw JSONRPCError(RPC_INVALID_PARAMETER, "Not a pool kind: " + *why);
    return create;
}

std::vector<RPCArg> KindOptionArgs()
{
    return {
        {"curve", RPCArg::Type::STR, RPCArg::Default{"constant_product"}, "constant_product, stable or weighted"},
        {"fee", RPCArg::Type::NUM, RPCArg::DefaultHint{"0.05 for stable, else 0.3"}, "The fee tier, in percent: 0.01, 0.05, 0.3 or 1"},
        {"amp", RPCArg::Type::NUM, RPCArg::DefaultHint{"100 for stable"}, "A stable pool's amplification, 1 to 10000: higher, flatter around the balance"},
        {"weight_a", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "A weighted pool's weight of asset_a, in percent: 2, 5, 10, 20, 25, 40, 60, 75, 80, 90, 95 or 98"},
        {"dynamic_fee", RPCArg::Type::BOOL, RPCArg::Default{false}, "The fee rises with the pool's volatility (above its tier, by half the recent average change of the price per block, at most 2 % more)"},
    };
}

/** The trades of a depth chart or a preview: for each size, what goes in and comes out each way. */
UniValue DepthToJSON(const bitassets::State& state, const uint256& id, const bitassets::PoolParams& kind, const bitassets::Pool& pool, uint8_t d0, uint8_t d1,
                     const std::vector<double>& fractions, bool flip = false, bool preview = false)
{
    UniValue points(UniValue::VARR);
    const auto cl_now{preview ? std::nullopt : state.GetClState(id)};
    for (const double fraction : fractions) {
        for (const bool zero_in : {true, false}) {
            // A concentrated pool holds liquidity around its price, not its reserves' worth at every price: sizes of what it holds.
            const uint64_t reserve_in{zero_in ? pool.reserve0 : pool.reserve1};
            const uint64_t amount{static_cast<uint64_t>(std::max(1.0, std::floor(reserve_in * fraction)))};
            if (amount > bitassets::MAX_AMOUNT - reserve_in) continue;
            const auto round{RoundOf(state, id, pool, {{zero_in, amount}}, preview ? std::optional{kind} : std::nullopt)};
            if (!round || round->clearing.outs[0] == 0) continue;
            const uint64_t got{round->clearing.outs[0]};
            const double in_whole{amount / std::pow(10.0, zero_in ? d0 : d1)}, out_whole{got / std::pow(10.0, zero_in ? d1 : d0)};
            // Prices of a whole asset_a in asset_b, asset_a the pool's first (its second, flipped).
            const bool a_in{zero_in != flip};
            const double spot{WholeSpotPrice(kind, pool, !flip, flip ? d1 : d0, flip ? d0 : d1, cl_now ? &*cl_now : nullptr)};
            const double avg{a_in ? out_whole / in_whole : in_whole / out_whole};
            UniValue point(UniValue::VOBJ);
            point.pushKV("side", a_in ? "sell_a" : "sell_b");
            point.pushKV("size_percent", DecimalToJSON(fraction * 100));
            point.pushKV("amount_in", AmountToJSON(amount, zero_in ? d0 : d1));
            point.pushKV("amount_out", AmountToJSON(got, zero_in ? d1 : d0));
            point.pushKV("avg_price", DecimalToJSON(avg));
            point.pushKV("price_after", DecimalToJSON(WholeSpotPrice(kind, round->after, !flip, flip ? d1 : d0, flip ? d0 : d1, round->cl_after ? &*round->cl_after : nullptr)));
            UniValue impact;
            impact.setNumStr(strprintf("%.4f", spot > 0 ? std::clamp(100.0 * (a_in ? 1.0 - avg / spot : 1.0 - spot / avg), 0.0, 100.0) : 100.0));
            point.pushKV("price_impact", impact);
            points.push_back(std::move(point));
        }
    }
    return points;
}

std::vector<RPCResult> WithKind(std::vector<RPCResult> results)
{
    for (RPCResult& r : KindResults()) results.push_back(std::move(r));
    return results;
}

const RPCResult DEPTH_RESULT{RPCResult::Type::ARR, "depth", "Trades of growing size each way, alone in their block (a depth chart)",
{
    {RPCResult::Type::OBJ, "", "",
    {
        {RPCResult::Type::STR, "side", "sell_a (asset_a in) or sell_b"},
        {RPCResult::Type::NUM, "size_percent", "What goes in, in percent of the pool's reserve of it"},
        {RPCResult::Type::NUM, "amount_in", ""},
        {RPCResult::Type::NUM, "amount_out", ""},
        {RPCResult::Type::NUM, "avg_price", "The trade's price of a whole asset_a in asset_b"},
        {RPCResult::Type::NUM, "price_after", "The pool's price after it"},
        {RPCResult::Type::NUM, "price_impact", "How much worse than the spot price, in percent (the fee included)"},
    }},
}};

RPCMethod previewpool()
{
    return RPCMethod{
        "previewpool",
        "What createpool would make, without making it: the pool's id, whether it exists, the shares the deposit gets, its price, and what trades "
        "of growing size would do (slippage).",
        {
            {"asset_a", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::AssetArgHelp()},
            {"amount_a", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "How much of it goes in"},
            {"asset_b", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::AssetArgHelp()},
            {"amount_b", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "How much of it"},
            {"kind", RPCArg::Type::OBJ_NAMED_PARAMS, RPCArg::Optional::OMITTED, "The pool's kind", KindOptionArgs()},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", WithKind({
            {RPCResult::Type::STR_HEX, "pool", "Its id"},
            {RPCResult::Type::BOOL, "exists", "Whether a pool of this kind of the pair exists"},
            {RPCResult::Type::BOOL, "open", "Whether someone provides liquidity to it (then createpool is refused: addpoolliquidity)"},
            {RPCResult::Type::NUM, "shares", "The shares the deposit gets (0 if too little)"},
            {RPCResult::Type::STR, "too_small", /*optional=*/true, "Why the deposit is too little to open a pool"},
            {RPCResult::Type::NUM, "price", "What a whole asset_a would be worth in asset_b"},
            DEPTH_RESULT,
        })},
        RPCExamples{HelpExampleCli("previewpool", "\"USDX\" 1000 \"USDY\" 1000 '{\"curve\":\"stable\",\"fee\":0.05,\"amp\":200}'")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    bitassets::CreatePool create{ParseKindOptions(request.params[4].isNull() ? UniValue{UniValue::VOBJ} : request.params[4])};
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    create.asset_a = ParseAsset(request.params[0], state);
    create.asset_b = ParseAsset(request.params[2], state);
    if (create.asset_a == create.asset_b) throw JSONRPCError(RPC_INVALID_PARAMETER, "Two different assets make a pool");
    const uint8_t da{DecimalsOf(state, create.asset_a)}, db{DecimalsOf(state, create.asset_b)};
    create.amount_a = bitassets::ParseUnits(request.params[1], da);
    create.amount_b = bitassets::ParseUnits(request.params[3], db);
    const bool a_first{create.asset_a < create.asset_b};
    const bitassets::PoolParams kind{bitassets::MakePoolParams(create, a_first ? da : db, a_first ? db : da)};
    const uint256 id{bitassets::PoolIdOf(create.asset_a, create.asset_b, kind)};
    const auto existing{state.GetPool(id)};
    if (kind.GetCurve() == bitassets::Curve::CONCENTRATED) {
        // Made at the price the amounts name, nothing in: positions bring its liquidity (previewposition).
        UniValue result(UniValue::VOBJ);
        result.pushKV("pool", id.GetHex());
        result.pushKV("exists", existing.has_value());
        result.pushKV("open", existing.has_value());
        result.pushKV("shares", 0);
        result.pushKV("price", DecimalToJSON((create.amount_b / std::pow(10.0, db)) / (create.amount_a / std::pow(10.0, da))));
        PushKind(result, kind, !a_first);
        result.pushKV("depth", UniValue{UniValue::VARR});
        return result;
    }
    bitassets::Pool pool;
    if (existing) pool = *existing;
    pool.asset0 = std::min(create.asset_a, create.asset_b);
    pool.asset1 = std::max(create.asset_a, create.asset_b);
    pool.reserve0 += a_first ? create.amount_a : create.amount_b;
    pool.reserve1 += a_first ? create.amount_b : create.amount_a;
    const auto total{bitassets::curves::OpeningShares(kind, pool.reserve0, pool.reserve1)};
    pool.shares = total.value_or(0);
    const uint64_t shares{total && *total > bitassets::MIN_LIQUIDITY ? *total - bitassets::MIN_LIQUIDITY : 0};
    UniValue result(UniValue::VOBJ);
    result.pushKV("pool", id.GetHex());
    result.pushKV("exists", existing.has_value());
    result.pushKV("open", existing && !bitassets::amm::Abandoned(*existing));
    result.pushKV("shares", shares);
    const uint64_t chn{pool.asset0.IsNull() ? pool.reserve0 : pool.asset1.IsNull() ? pool.reserve1 : bitassets::MIN_OPEN_CHN};
    if (shares < bitassets::MIN_OPEN_SHARES) {
        result.pushKV("too_small", strprintf("it makes %u shares; a pool opens with %u at least", shares, bitassets::MIN_OPEN_SHARES));
    } else if (chn < bitassets::MIN_OPEN_CHN) {
        result.pushKV("too_small", strprintf("a pool with %s opens with %s %s at least", CURRENCY_UNIT, FormatMoney(bitassets::MIN_OPEN_CHN), CURRENCY_UNIT));
    }
    result.pushKV("price", DecimalToJSON(WholeSpotPrice(kind, pool, a_first, da, db)));
    PushKind(result, kind, !a_first);
    // In the order the caller named the assets: sell_a is asset_a in.
    result.pushKV("depth", DepthToJSON(state, id, kind, pool, a_first ? da : db, a_first ? db : da, {0.001, 0.01, 0.05, 0.1, 0.25, 0.5}, !a_first, /*preview=*/true));
    return result;
},
    };
}

RPCMethod getpoolcurve()
{
    return RPCMethod{
        "getpoolcurve",
        "A pool's depth: what trades of growing size, each way, would get alone now, and the price after each (for a depth chart).",
        {
            {"pool", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The pool"},
            {"points", RPCArg::Type::NUM, RPCArg::Default{12}, "How many sizes, from 0.1 % to 90 % of the reserve put in (2 to 40)"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", WithKind({
            {RPCResult::Type::STR_HEX, "pool", "The pool"},
            {RPCResult::Type::STR, "asset_a", "Its first asset"},
            {RPCResult::Type::STR, "asset_b", "Its second"},
            {RPCResult::Type::STR_HEX, "asset_a_id", "The first asset's id (the lower; null for " + CURRENCY_UNIT + ")"},
            {RPCResult::Type::STR_HEX, "asset_b_id", "The second's"},
            {RPCResult::Type::NUM, "price", "A whole asset_a in asset_b now"},
            DEPTH_RESULT,
        })},
        RPCExamples{HelpExampleCli("getpoolcurve", "\"<pool>\" 20")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const uint256 id{ParseHashV(request.params[0], "pool")};
    const int count{request.params[1].isNull() ? 12 : request.params[1].getInt<int>()};
    if (count < 2 || count > 40) throw JSONRPCError(RPC_INVALID_PARAMETER, "points is 2 to 40");
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    const auto pool{state.GetPool(id)};
    if (!pool) throw JSONRPCError(RPC_INVALID_PARAMETER, "No such pool");
    const bitassets::PoolParams kind{state.GetPoolParams(id)};
    const uint8_t d0{DecimalsOf(state, pool->asset0)}, d1{DecimalsOf(state, pool->asset1)};
    // Geometric from 0.1 % to 90 %.
    std::vector<double> fractions;
    for (int i{0}; i < count; ++i) fractions.push_back(0.001 * std::pow(900.0, i / double(count - 1)));
    UniValue result(UniValue::VOBJ);
    result.pushKV("pool", id.GetHex());
    result.pushKV("asset_a", LabelOf(state, pool->asset0));
    result.pushKV("asset_b", LabelOf(state, pool->asset1));
    result.pushKV("asset_a_id", pool->asset0.GetHex());
    result.pushKV("asset_b_id", pool->asset1.GetHex());
    const auto cl_now{state.GetClState(id)};
    result.pushKV("price", DecimalToJSON(WholeSpotPrice(kind, *pool, true, d0, d1, cl_now ? &*cl_now : nullptr)));
    PushKind(result, kind, false);
    result.pushKV("depth", DepthToJSON(state, id, kind, *pool, d0, d1, fractions));
    return result;
},
    };
}

std::string ScriptAddress(const CScript& script)
{
    CTxDestination dest;
    return ExtractDestination(script, dest) ? EncodeDestination(dest) : HexStr(script);
}

/** An operation as an explorer shows it: its kind, and its fields (assets by id, amounts in units). */
UniValue OperationToJSON(const bitassets::Operation& operation)
{
    UniValue obj(UniValue::VOBJ);
    const auto asset{[](const AssetId& id) { return id.GetHex(); }};
    std::visit([&](const auto& op) {
        using T = std::decay_t<decltype(op)>;
        obj.pushKV("kind", static_cast<int>(T::KIND));
        if constexpr (std::is_same_v<T, bitassets::Reserve>) {
            obj.pushKV("name", "reserve");
        } else if constexpr (std::is_same_v<T, bitassets::Register>) {
            obj.pushKV("name", "register");
            obj.pushKV("asset", asset(op.name));
            obj.pushKV("supply", op.supply);
            obj.pushKV("decimals", op.decimals);
            if (op.text) obj.pushKV("ticker", *op.text);
            if (!op.display_name.empty()) obj.pushKV("display_name", op.display_name);
            obj.pushKV("logo", !op.logo.empty());
        } else if constexpr (std::is_same_v<T, bitassets::Mint>) {
            obj.pushKV("name", "mint");
            obj.pushKV("asset", asset(op.asset));
            obj.pushKV("amount", op.amount);
        } else if constexpr (std::is_same_v<T, bitassets::Swap>) {
            obj.pushKV("name", "swap");
            obj.pushKV("asset_in", asset(op.asset_in));
            obj.pushKV("amount_in", op.amount_in);
            obj.pushKV("asset_out", asset(op.asset_out));
            obj.pushKV("min_out", op.min_out);
        } else if constexpr (std::is_same_v<T, bitassets::AddLiquidity>) {
            obj.pushKV("name", "add_liquidity");
            obj.pushKV("asset_a", asset(op.asset_a));
            obj.pushKV("amount_a", op.amount_a);
            obj.pushKV("asset_b", asset(op.asset_b));
            obj.pushKV("amount_b", op.amount_b);
            obj.pushKV("pool", bitassets::PoolId(op.asset_a, op.asset_b).GetHex());
        } else if constexpr (std::is_same_v<T, bitassets::RemoveLiquidity>) {
            obj.pushKV("name", "remove_liquidity");
            obj.pushKV("pool", bitassets::PoolId(op.asset_a, op.asset_b).GetHex());
            obj.pushKV("shares", op.shares);
        } else if constexpr (std::is_same_v<T, bitassets::Route>) {
            obj.pushKV("name", "route");
            UniValue path(UniValue::VARR);
            for (const AssetId& a : op.path) path.push_back(asset(a));
            obj.pushKV("path", std::move(path));
            UniValue pools(UniValue::VARR);
            for (size_t i{0}; i + 1 < op.path.size(); ++i) pools.push_back(bitassets::PoolId(op.path[i], op.path[i + 1]).GetHex());
            obj.pushKV("pools", std::move(pools));
            obj.pushKV("amount_in", op.amount_in);
            obj.pushKV("min_out", op.min_out);
        } else if constexpr (std::is_same_v<T, bitassets::CreatePool>) {
            obj.pushKV("name", "create_pool");
            obj.pushKV("asset_a", asset(op.asset_a));
            obj.pushKV("amount_a", op.amount_a);
            obj.pushKV("asset_b", asset(op.asset_b));
            obj.pushKV("amount_b", op.amount_b);
            obj.pushKV("curve", bitassets::curves::CurveName(static_cast<bitassets::Curve>(op.curve)));
            obj.pushKV("fee_millionths", static_cast<uint64_t>(op.fee));
            if (op.amp) obj.pushKV("amp", static_cast<uint64_t>(op.amp));
            if (op.weight_a) obj.pushKV("weight_a", op.weight_a);
            obj.pushKV("min_shares", op.min_shares);
            bitassets::PoolParams params;
            params.curve = op.curve;
            params.fee = op.fee;
            params.amp = op.amp;
            params.flags = op.flags;
            if (op.curve == static_cast<uint8_t>(bitassets::Curve::WEIGHTED)) params.weight0 = op.asset_a < op.asset_b ? op.weight_a : static_cast<uint8_t>(100 - op.weight_a);
            obj.pushKV("pool", bitassets::PoolIdOf(op.asset_a, op.asset_b, params).GetHex());
        } else if constexpr (std::is_same_v<T, bitassets::PoolDeposit>) {
            obj.pushKV("name", "pool_deposit");
            obj.pushKV("pool", op.pool.GetHex());
            obj.pushKV("amount0", op.amount0);
            obj.pushKV("amount1", op.amount1);
            obj.pushKV("min_shares", op.min_shares);
        } else if constexpr (std::is_same_v<T, bitassets::PoolWithdraw>) {
            obj.pushKV("name", "pool_withdraw");
            obj.pushKV("pool", op.pool.GetHex());
            obj.pushKV("shares", op.shares);
            obj.pushKV("only", op.only == 0 ? "both" : op.only == 1 ? "asset0" : "asset1");
            obj.pushKV("min0", op.min0);
            obj.pushKV("min1", op.min1);
        } else if constexpr (std::is_same_v<T, bitassets::SwapPools>) {
            obj.pushKV("name", "swap_pools");
            obj.pushKV("asset_in", asset(op.asset_in));
            UniValue pools(UniValue::VARR);
            for (const uint256& id : op.pools) pools.push_back(id.GetHex());
            obj.pushKV("pools", std::move(pools));
            obj.pushKV("amount_in", op.amount_in);
            obj.pushKV("min_out", op.min_out);
        } else if constexpr (std::is_same_v<T, bitassets::LaunchToken>) {
            obj.pushKV("name", "launch_token");
            obj.pushKV("token", asset(op.reg.name));
            if (op.reg.text) obj.pushKV("ticker", *op.reg.text);
            if (!op.reg.display_name.empty()) obj.pushKV("display_name", op.reg.display_name);
            obj.pushKV("logo", !op.reg.logo.empty());
            obj.pushKV("creator", ScriptAddress(op.creator));
            obj.pushKV("allocation_bps", op.allocation);
            obj.pushKV("min_buy", op.min_buy);
            obj.pushKV("max_buy", op.max_buy);
            obj.pushKV("first_buy", op.first_buy);
            if (op.WithParams()) {
                obj.pushKV("supply", op.reg.supply);
                obj.pushKV("decimals", op.reg.decimals);
                obj.pushKV("fee_ppm", op.fee);
            }
        } else if constexpr (std::is_same_v<T, bitassets::CurveAsset>) {
            obj.pushKV("name", "curve_asset");
            obj.pushKV("asset", asset(op.asset));
            obj.pushKV("amount", op.amount);
            obj.pushKV("fee_ppm", op.fee);
            obj.pushKV("creator", ScriptAddress(op.creator));
            obj.pushKV("min_buy", op.min_buy);
            obj.pushKV("max_buy", op.max_buy);
            obj.pushKV("first_buy", op.first_buy);
        } else if constexpr (std::is_same_v<T, bitassets::ClaimLaunch>) {
            obj.pushKV("name", "claim_launch");
            obj.pushKV("token", asset(op.token));
        } else {
            static const std::map<int, std::string> names{{5, "burn"}, {4, "update"}, {9, "create_auction"}, {10, "bid"}, {11, "collect"}, {12, "release_asset"}, {14, "register_oracle"},
                                                          {15, "attest"}, {16, "create_oracle_contract"}, {17, "settle_oracle_contract"}, {32, "set_logo"}, {34, "set_display_name"}};
            const auto it{names.find(T::KIND)};
            obj.pushKV("name", it == names.end() ? "operation" : it->second);
        }
    }, operation);
    return obj;
}

RPCMethod decodeassettx()
{
    return RPCMethod{
        "decodeassettx",
        "What a transaction does to the assets, as its marker says (for explorers): the operation and its fields, every kind included "
        "(the DEX's routes and pool kinds), and the outputs the marker lists. What it gave (an order's result) is in the chain's state, not here.",
        {
            {"tx", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "A transaction: its id (in the mempool, or a block with -txindex), or its hex"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The transaction"},
            {RPCResult::Type::BOOL, "marker", "Whether it has a marker (an asset transaction)"},
            {RPCResult::Type::STR, "error", /*optional=*/true, "Why its marker is not well formed"},
            {RPCResult::Type::OBJ_DYN, "operation", /*optional=*/true, "The operation: kind (its marker byte), name, and its fields", {{RPCResult::Type::ANY, "", ""}}},
            {RPCResult::Type::ARR, "outputs", /*optional=*/true, "The outputs the marker lists",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::NUM, "n", "The output"},
                    {RPCResult::Type::STR, "carries", "asset, control, reservation, lp, receipt, or result (what the operation gives)"},
                    {RPCResult::Type::STR_HEX, "id", /*optional=*/true, "What the token is of"},
                    {RPCResult::Type::NUM, "amount", /*optional=*/true, "In units"},
                }},
            }},
        }},
        RPCExamples{HelpExampleCli("decodeassettx", "\"<txid>\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node{EnsureAnyNodeContext(request.context)};
    ChainstateManager& chainman{EnsureChainman(node)};
    const std::string text{request.params[0].get_str()};
    CTransactionRef tx;
    if (text.size() == 64) {
        const auto txid{Txid::FromHex(text)};
        if (!txid) throw JSONRPCError(RPC_INVALID_PARAMETER, "Not a transaction id");
        uint256 block;
        tx = node::GetTransaction(nullptr, node.mempool.get(), *txid, chainman.m_blockman, block);
        if (!tx) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No such transaction in the mempool or the transaction index (-txindex); give its hex");
    } else {
        CMutableTransaction mtx;
        if (!DecodeHexTx(mtx, text)) throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Not a transaction");
        tx = MakeTransactionRef(std::move(mtx));
    }
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    std::string error;
    const auto marker{bitassets::GetMarker(*tx, &error)};
    result.pushKV("marker", marker.has_value() || !error.empty());
    if (!error.empty()) result.pushKV("error", error);
    if (!marker) return result;
    if (marker->operation) result.pushKV("operation", OperationToJSON(*marker->operation));
    UniValue outs(UniValue::VARR);
    static const char* kinds[]{"asset", "control", "reservation", "lp", "receipt"};
    for (const bitassets::MarkerOutput& out : marker->outputs) {
        UniValue o(UniValue::VOBJ);
        o.pushKV("n", out.n);
        if (out.token) {
            o.pushKV("carries", kinds[static_cast<int>(out.token->kind)]);
            o.pushKV("id", out.token->id.GetHex());
            o.pushKV("amount", out.token->amount);
        } else {
            o.pushKV("carries", "result");
        }
        outs.push_back(std::move(o));
    }
    result.pushKV("outputs", std::move(outs));
    return result;
},
    };
}

//
// Launches (LaunchToken, doc/layers.md "Launches"): what the website and the app read.
//

std::vector<RPCResult> LaunchResults()
{
    return {
        {RPCResult::Type::STR_HEX, "token", "The token's id"},
        {RPCResult::Type::STR, "ticker", "Its ticker"},
        {RPCResult::Type::STR, "display_name", /*optional=*/true, "Its display name"},
        {RPCResult::Type::STR, "info", /*optional=*/true, "Its description and links"},
        {RPCResult::Type::BOOL, "has_logo", "Whether it has a logo (getassetlogo)"},
        {RPCResult::Type::STR, "creator", "The creator's address (its fees and allocation go there)"},
        {RPCResult::Type::STR_HEX, "txid", "The launch's transaction"},
        {RPCResult::Type::NUM, "height", "Its block"},
        {RPCResult::Type::STR, "status", "\"curve\" (trading on its bonding curve) or \"graduated\" (its pool with " + CURRENCY_UNIT + ", locked)"},
        {RPCResult::Type::STR_HEX, "pool", "Its curve's pool, or, graduated, its pool with " + CURRENCY_UNIT},
        {RPCResult::Type::STR_AMOUNT, "raised", CURRENCY_UNIT + " the curve raised (graduated: what it had raised)"},
        {RPCResult::Type::STR_AMOUNT, "graduation", "What it graduates at"},
        {RPCResult::Type::NUM, "progress", "Percent of that raised (100 once graduated)"},
        {RPCResult::Type::NUM, "price", CURRENCY_UNIT + " for a whole unit now (the curve's, or the pool's)"},
        {RPCResult::Type::STR_AMOUNT, "market_cap", "The price times the supply"},
        {RPCResult::Type::NUM, "supply", "Whole units of the asset there are"},
        {RPCResult::Type::NUM, "decimals", "The asset's decimals"},
        {RPCResult::Type::NUM, "curve_supply", "Whole units the curve sells in all (3/4 of the launch's supply)"},
        {RPCResult::Type::NUM, "pool_supply", "Whole units that go into the pool at graduation (1/4)"},
        {RPCResult::Type::BOOL, "from_asset", "Whether an existing asset was put on this curve (curveasset), rather than launched"},
        {RPCResult::Type::NUM, "curve_tokens_left", "Whole units the curve still sells (0 once graduated)"},
        {RPCResult::Type::NUM, "fee", "The curve's fee, in percent"},
        {RPCResult::Type::NUM, "creator_share", "The creator's part of that fee, in percent of it"},
        {RPCResult::Type::NUM, "security_share", "The chain's security part (the BMM bidder and the validators), in percent of it; the rest goes into the pool at graduation"},
        {RPCResult::Type::STR_AMOUNT, "security_fees", "Paid to the BMM bidders and validators so far"},
        {RPCResult::Type::STR_AMOUNT, "creator_fees_owed", "The creator's fees not paid yet (claimlaunch)"},
        {RPCResult::Type::STR_AMOUNT, "creator_fees_paid", "Paid so far"},
        {RPCResult::Type::NUM, "allocation", "The creator's allocation, in percent of the supply, minted at graduation (0: none)"},
        {RPCResult::Type::BOOL, "allocation_claimed", "Whether it was minted"},
        {RPCResult::Type::STR_AMOUNT, "min_buy", "The least a buy pays (0: any)"},
        {RPCResult::Type::STR_AMOUNT, "max_buy", "The most (0: any)"},
        {RPCResult::Type::STR_AMOUNT, "volume", CURRENCY_UNIT + " traded on the curve, in and out"},
        {RPCResult::Type::NUM, "trades", "Blocks that traded on it"},
        {RPCResult::Type::NUM, "graduated_height", /*optional=*/true, "The block it graduated in"},
        {RPCResult::Type::BOOL, "fee_flow", "Whether it was made under the fee-flow release (its graduation into a locked position, its fee destination, its control coin)"},
        {RPCResult::Type::STR, "fee_destination", "Where the creator's part of its fees goes: creator, burn (bought back and burned each block) or split"},
        {RPCResult::Type::NUM, "burn_percent", "With split: the part burned, in percent"},
        {RPCResult::Type::ARR, "fee_split", "The split of the tunable part of its fees (v5's destination mapped onto one; empty before v5): the curve's fee but the security part, and the creator's part of its locked position's fees",
            {
                {RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "to", "\"creator\", \"burn\" or the address paid"},
                        {RPCResult::Type::STR, "kind", "creator (claimlaunch), burn (bought back and burned each block) or address (paid by the chain once 0.001 is owed)"},
                        {RPCResult::Type::NUM, "bps", "Its share, in basis points of the tunable part"},
                        {RPCResult::Type::STR_AMOUNT, "paid", "What it was paid: the creator claimed, the burn bought back, the address got"},
                        {RPCResult::Type::STR_AMOUNT, "owed", "What it is owed and not paid yet"},
                    }},
            }},
        {RPCResult::Type::BOOL, "mintable", "Whether its creator kept the control coin: the creator can create more units (a risk to buyers)"},
        {RPCResult::Type::NUM, "burned", "Whole units bought back and burned"},
        {RPCResult::Type::STR_AMOUNT, "buyback_pending", CURRENCY_UNIT + " waiting to buy it back"},
        {RPCResult::Type::NUM, "creator_tokens_owed", "The creator's part of its locked position's fees in its own units, not claimed yet (claimlaunch)"},
        {RPCResult::Type::STR, "pool_kind", "The kind of pool it graduates (or graduated) into: concentrated (a locked full-range position at 1 %) or constant_product (locked shares)"},
        {RPCResult::Type::STR_HEX, "position", /*optional=*/true, "Its locked position, once graduated into one"},
    };
}

/** The decimals of a launch's asset (LAUNCH_DECIMALS for one of before, or not found). */
uint8_t LaunchDecimals(const bitassets::State& state, const AssetId& token)
{
    const auto record{state.GetAsset(token)};
    return record ? record->decimals : bitassets::LAUNCH_DECIMALS;
}

UniValue LaunchToJSON(const bitassets::State& state, const AssetId& token, const bitassets::LaunchRecord& launch, int next_height)
{
    const auto record{state.GetAsset(token)};
    const uint8_t decimals{record ? record->decimals : bitassets::LAUNCH_DECIMALS};
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("token", token.GetHex());
    obj.pushKV("ticker", record && !record->text.empty() ? record->text : "0x" + token.GetHex());
    if (const auto display{state.GetDisplayName(token)}) obj.pushKV("display_name", display->text);
    if (record && record->data.info) obj.pushKV("info", *record->data.info);
    obj.pushKV("has_logo", state.GetLogo(token).has_value());
    obj.pushKV("creator", ScriptAddress(launch.creator));
    obj.pushKV("txid", launch.txid.GetHex());
    obj.pushKV("height", launch.height);
    obj.pushKV("status", launch.Graduated() ? "graduated" : "curve");
    obj.pushKV("pool", launch.pool.GetHex());
    const auto pool{state.GetPool(launch.pool)};
    uint64_t raised{launch.raised_at_graduation}, left{0};
    double price{0};
    const double whole{std::pow(10.0, decimals)};
    if (pool && !launch.Graduated()) {
        raised = pool->reserve0 - std::min(pool->reserve0, launch.virtual_coin);
        left = pool->reserve1 - std::min(pool->reserve1, launch.CurveEnd());
    }
    // Marginal price, in CHN for a whole token: the reserves' ratio (virtual on the curve).
    if (pool && pool->reserve1 > 0) price = (static_cast<double>(pool->reserve0) / COIN) / (static_cast<double>(pool->reserve1) / whole);
    obj.pushKV("raised", ValueFromAmount(static_cast<CAmount>(raised)));
    obj.pushKV("graduation", ValueFromAmount(static_cast<CAmount>(launch.graduation)));
    obj.pushKV("progress", DecimalToJSON(launch.Graduated() ? 100.0 : std::min(100.0, 100.0 * raised / launch.graduation)));
    obj.pushKV("price", DecimalToJSON(price));
    const uint64_t supply{record ? record->supply : launch.Supply()};
    obj.pushKV("market_cap", ValueFromAmount(static_cast<CAmount>(std::llround(price * (supply / whole) * COIN))));
    obj.pushKV("supply", AmountToJSON(supply, decimals));
    obj.pushKV("decimals", decimals);
    obj.pushKV("curve_supply", AmountToJSON(launch.CurveSupply(), decimals));
    obj.pushKV("pool_supply", AmountToJSON(launch.PoolSupply(), decimals));
    obj.pushKV("from_asset", launch.from_asset);
    obj.pushKV("curve_tokens_left", AmountToJSON(left, decimals));
    obj.pushKV("fee", DecimalToJSON(launch.fee / 10000.0));
    // The creator's part in force (a part of before for the protocol is the creator's: slot-99 history only).
    const bool merged{Params().GetConsensus().sidechain.NoTreasury(next_height)};
    obj.pushKV("creator_share", DecimalToJSON((merged ? launch.creator_share + launch.protocol_share : launch.creator_share) / 10000.0));
    obj.pushKV("security_share", DecimalToJSON(launch.security_share / 10000.0));
    obj.pushKV("security_fees", ValueFromAmount(static_cast<CAmount>(launch.security_fees)));
    obj.pushKV("creator_fees_owed", ValueFromAmount(static_cast<CAmount>(launch.creator_fees)));
    obj.pushKV("creator_fees_paid", ValueFromAmount(static_cast<CAmount>(launch.creator_paid)));
    obj.pushKV("allocation", DecimalToJSON(launch.allocation / 100.0));
    obj.pushKV("allocation_claimed", launch.allocation_claimed);
    obj.pushKV("min_buy", ValueFromAmount(static_cast<CAmount>(launch.min_buy)));
    obj.pushKV("max_buy", ValueFromAmount(static_cast<CAmount>(launch.max_buy)));
    obj.pushKV("volume", ValueFromAmount(static_cast<CAmount>(launch.volume)));
    obj.pushKV("trades", launch.trades);
    if (launch.Graduated()) obj.pushKV("graduated_height", launch.graduated);
    obj.pushKV("fee_flow", launch.v5);
    obj.pushKV("fee_destination", launch.destination == static_cast<uint8_t>(bitassets::FeeDestination::BURN) ? "burn" : launch.destination == static_cast<uint8_t>(bitassets::FeeDestination::SPLIT) ? "split" : "creator");
    obj.pushKV("burn_percent", DecimalToJSON(launch.burn_bps / 100.0));
    UniValue split(UniValue::VARR);
    for (const bitassets::FeeShareView& share : bitassets::FeeSplitView(launch)) {
        UniValue entry(UniValue::VOBJ);
        CTxDestination dest;
        entry.pushKV("to", std::string_view{share.kind} == "address" ? (ExtractDestination(share.script, dest) ? EncodeDestination(dest) : HexStr(share.script)) : std::string{share.kind});
        entry.pushKV("kind", share.kind);
        entry.pushKV("bps", share.bps);
        entry.pushKV("paid", ValueFromAmount(static_cast<CAmount>(share.paid)));
        entry.pushKV("owed", ValueFromAmount(static_cast<CAmount>(share.owed)));
        split.push_back(std::move(entry));
    }
    obj.pushKV("fee_split", std::move(split));
    obj.pushKV("mintable", launch.keep_control);
    obj.pushKV("burned", AmountToJSON(launch.burned, decimals));
    obj.pushKV("buyback_pending", ValueFromAmount(static_cast<CAmount>(launch.buyback)));
    obj.pushKV("creator_tokens_owed", AmountToJSON(launch.creator_tokens, decimals));
    obj.pushKV("pool_kind", launch.v5 ? "concentrated" : "constant_product");
    if (!launch.position.IsNull()) obj.pushKV("position", launch.position.GetHex());
    return obj;
}

AssetId ParseToken(const UniValue& value, const bitassets::State& state, bitassets::LaunchRecord& launch)
{
    const AssetId token{ParseAsset(value, state)};
    const auto found{state.GetLaunch(token)};
    if (!found) throw JSONRPCError(RPC_INVALID_PARAMETER, "Not an asset on a curve (launched or put on one)");
    launch = *found;
    return token;
}

RPCMethod getlaunch()
{
    return RPCMethod{
        "getlaunch",
        "A launch (launchtoken): its asset, curve, progress to graduation, price, fees and creator.",
        {{"token", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::AssetArgHelp()}},
        RPCResult{RPCResult::Type::OBJ, "", "", LaunchResults()},
        RPCExamples{HelpExampleCli("getlaunch", "\"MOON\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    return WithDexState(chainman, [&](const bitassets::State& state, int next_height) -> UniValue {
        bitassets::LaunchRecord launch;
        const AssetId token{ParseToken(request.params[0], state, launch)};
        return LaunchToJSON(state, token, launch, next_height);
    });
},
    };
}

RPCMethod listlaunches()
{
    return RPCMethod{
        "listlaunches",
        "The launches, filtered and sorted, a page at a time (next_cursor gives the next page of the same query).",
        {
            {"options", RPCArg::Type::OBJ_NAMED_PARAMS, RPCArg::Optional::OMITTED, "",
            {
                {"status", RPCArg::Type::STR, RPCArg::Default{"all"}, "all, curve (not graduated), graduated, or new (made in the last 5760 blocks, not graduated)"},
                {"min_progress", RPCArg::Type::NUM, RPCArg::Default{0}, "Only those at least this far to graduation, in percent (near graduation: 80)"},
                {"sort", RPCArg::Type::STR, RPCArg::Default{"newest"}, "newest, oldest, volume, progress, market_cap"},
                {"cursor", RPCArg::Type::STR, RPCArg::Default{""}, "next_cursor of the page before"},
                {"limit", RPCArg::Type::NUM, RPCArg::Default{50}, "1 to 200"},
            }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::ARR, "launches", "", {{RPCResult::Type::OBJ, "", "", LaunchResults()}}},
            {RPCResult::Type::NUM, "total", "How many match"},
            {RPCResult::Type::STR, "next_cursor", /*optional=*/true, "For the next page; none at the end"},
        }},
        RPCExamples{HelpExampleCli("-named listlaunches", "status=curve min_progress=80 sort=progress") + HelpExampleCli("listlaunches", "'{\"status\":\"graduated\",\"sort\":\"volume\"}'")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const UniValue none{UniValue::VOBJ};
    const UniValue& o{request.params[0].isNull() ? none : request.params[0]};
    const std::string status{o.exists("status") ? o["status"].get_str() : "all"};
    if (status != "all" && status != "curve" && status != "graduated" && status != "new") throw JSONRPCError(RPC_INVALID_PARAMETER, "status is all, curve, graduated or new");
    const double min_progress{o.exists("min_progress") ? o["min_progress"].get_real() : 0.0};
    const std::string sort{o.exists("sort") ? o["sort"].get_str() : "newest"};
    if (sort != "newest" && sort != "oldest" && sort != "volume" && sort != "progress" && sort != "market_cap") throw JSONRPCError(RPC_INVALID_PARAMETER, "sort is newest, oldest, volume, progress or market_cap");
    const int limit{o.exists("limit") ? o["limit"].getInt<int>() : 50};
    if (limit < 1 || limit > 200) throw JSONRPCError(RPC_INVALID_PARAMETER, "limit is 1 to 200");
    // The cursor: the position in this query's order (a page of a list that moves may skip or repeat one at its edge).
    size_t skip{0};
    if (o.exists("cursor") && !o["cursor"].get_str().empty()) {
        const auto parsed{ToIntegral<uint64_t>(o["cursor"].get_str())};
        if (!parsed) throw JSONRPCError(RPC_INVALID_PARAMETER, "Not a cursor of listlaunches");
        skip = *parsed;
    }
    return WithDexState(chainman, [&](const bitassets::State& state, [[maybe_unused]] int next_height) -> UniValue {
        const int tip{(next_height - 1)};
        struct Row {
            AssetId token;
            bitassets::LaunchRecord launch;
            UniValue json;
            double progress, volume, cap;
        };
        std::vector<Row> rows;
        state.ForEachLaunch([&](int32_t height, const AssetId& token) {
            const auto launch{state.GetLaunch(token)};
            if (!launch) return true;
            if (status == "curve" && launch->Graduated()) return true;
            if (status == "graduated" && !launch->Graduated()) return true;
            if (status == "new" && (launch->Graduated() || tip - height > 5760)) return true;
            UniValue json{LaunchToJSON(state, token, *launch, next_height)};
            const double progress{json["progress"].get_real()};
            if (progress < min_progress) return true;
            const double cap{json["market_cap"].get_real()};
            rows.push_back({token, *launch, std::move(json), progress, static_cast<double>(launch->volume), cap});
            return true;
        });
        std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
            if (sort == "oldest") return a.launch.height < b.launch.height;
            if (sort == "volume" && a.volume != b.volume) return a.volume > b.volume;
            if (sort == "progress" && a.progress != b.progress) return a.progress > b.progress;
            if (sort == "market_cap" && a.cap != b.cap) return a.cap > b.cap;
            return a.launch.height > b.launch.height;
        });
        UniValue list(UniValue::VARR);
        for (size_t i{skip}; i < rows.size() && list.size() < static_cast<size_t>(limit); ++i) list.push_back(rows[i].json);
        UniValue result(UniValue::VOBJ);
        result.pushKV("launches", std::move(list));
        result.pushKV("total", static_cast<uint64_t>(rows.size()));
        if (skip + limit < rows.size()) result.pushKV("next_cursor", strprintf("%u", skip + limit));
        return result;
    });
},
    };
}

RPCMethod quotelaunch()
{
    return RPCMethod{
        "quotelaunch",
        "What a buy (" + CURRENCY_UNIT + " in) or a sale (units of the asset in) on a launch's curve gives now, alone in its block: what comes out, what comes back if it would "
        "take the curve past its end, the fee, the price, and whether the launch would graduate.",
        {
            {"token", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::AssetArgHelp()},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, CURRENCY_UNIT + " to pay in (buy), or units of the asset to sell"},
            {"side", RPCArg::Type::STR, RPCArg::Default{"buy"}, "buy or sell"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "amount_in", "What goes in"},
            {RPCResult::Type::NUM, "amount_out", "What comes out (units of the asset for a buy, " + CURRENCY_UNIT + " for a sale, the fee taken)"},
            {RPCResult::Type::STR_AMOUNT, "back", CURRENCY_UNIT + " back (a buy past the curve's end)"},
            {RPCResult::Type::STR_AMOUNT, "fee", "The fee, in " + CURRENCY_UNIT},
            {RPCResult::Type::NUM, "price", CURRENCY_UNIT + " for a whole unit, this trade"},
            {RPCResult::Type::NUM, "spot_price", "The curve's price before"},
            {RPCResult::Type::NUM, "price_after", "And after"},
            {RPCResult::Type::NUM, "price_impact", "How much worse than the spot price, in percent (the fee included)"},
            {RPCResult::Type::NUM, "progress_after", "Percent to graduation after"},
            {RPCResult::Type::BOOL, "graduates", "Whether it would take the launch to graduation"},
        }},
        RPCExamples{HelpExampleCli("quotelaunch", "\"MOON\" 5") + HelpExampleCli("quotelaunch", "\"MOON\" 1000000 sell")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const std::string side{request.params[2].isNull() ? "buy" : request.params[2].get_str()};
    if (side != "buy" && side != "sell") throw JSONRPCError(RPC_INVALID_PARAMETER, "side is buy or sell");
    const bool buy{side == "buy"};
    return WithDexState(chainman, [&](const bitassets::State& state, [[maybe_unused]] int next_height) -> UniValue {
        bitassets::LaunchRecord launch;
        const AssetId launch_token{ParseToken(request.params[0], state, launch)};
        if (launch.Graduated()) throw JSONRPCError(RPC_INVALID_PARAMETER, "The launch graduated: quote its pool (getswapquote)");
        const auto pool{state.GetPool(launch.pool)};
        if (!pool) throw JSONRPCError(RPC_MISC_ERROR, "The launch has no curve");
        const uint8_t decimals{LaunchDecimals(state, launch_token)};
        const uint64_t amount{bitassets::ParseUnits(request.params[1], buy ? bitassets::CHN_DECIMALS : decimals)};
        const auto cleared{bitassets::curves::ClearLaunch(bitassets::curves::LaunchCurve{launch.fee, launch.CurveEnd(), launch.max_buy, launch.min_buy, launch.StrictEnd(), launch.StartBound()}, pool->reserve0, pool->reserve1,
                                                          {bitassets::dex::HopIn{buy, amount}}, {true})};
        if (!cleared) throw JSONRPCError(RPC_INVALID_PARAMETER, "The curve cannot take this trade");
        const double whole{std::pow(10.0, decimals)};
        const auto spot{[&](uint64_t x, uint64_t y) { return (static_cast<double>(x) / COIN) / (static_cast<double>(y) / whole); }};
        const uint64_t out{cleared->outs[0]}, back{cleared->refunds[0]};
        const double coin{(buy ? static_cast<double>(amount - back) : static_cast<double>(out)) / COIN};
        const double tokens{(buy ? static_cast<double>(out) : static_cast<double>(amount)) / whole};
        const double price{tokens > 0 ? coin / tokens : 0}, before{spot(pool->reserve0, pool->reserve1)};
        UniValue result(UniValue::VOBJ);
        result.pushKV("amount_in", AmountToJSON(amount, buy ? bitassets::CHN_DECIMALS : decimals));
        result.pushKV("amount_out", AmountToJSON(out, buy ? decimals : bitassets::CHN_DECIMALS));
        result.pushKV("back", ValueFromAmount(static_cast<CAmount>(back)));
        result.pushKV("fee", ValueFromAmount(static_cast<CAmount>(cleared->fees)));
        result.pushKV("price", DecimalToJSON(price));
        result.pushKV("spot_price", DecimalToJSON(before));
        result.pushKV("price_after", DecimalToJSON(spot(cleared->reserve0, cleared->reserve1)));
        UniValue impact;
        impact.setNumStr(strprintf("%.4f", before > 0 && price > 0 ? std::clamp(100.0 * (buy ? price / before - 1.0 : 1.0 - price / before), 0.0, 100.0) : 100.0));
        result.pushKV("price_impact", impact);
        const uint64_t raised{cleared->reserve0 - std::min(cleared->reserve0, launch.virtual_coin)};
        result.pushKV("progress_after", DecimalToJSON(std::min(100.0, 100.0 * raised / launch.graduation)));
        result.pushKV("graduates", cleared->reached_end || raised >= launch.graduation);
        return result;
    });
},
    };
}

RPCMethod getlaunchstats()
{
    return RPCMethod{
        "getlaunchstats",
        "Launches in all: how many, on their curves and graduated, the " + CURRENCY_UNIT + " traded and raised, and the rules of new launches.",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "launches", "How many"},
            {RPCResult::Type::NUM, "on_curve", "Not graduated"},
            {RPCResult::Type::NUM, "graduated", "Graduated"},
            {RPCResult::Type::STR_AMOUNT, "volume", CURRENCY_UNIT + " traded on curves, in and out"},
            {RPCResult::Type::STR_AMOUNT, "raised", CURRENCY_UNIT + " the curves hold now, and raised by those graduated"},
            {RPCResult::Type::OBJ, "rules", "What a new launch is made under (consensus parameters of this network)",
            {
                {RPCResult::Type::STR_AMOUNT, "graduation", "What a curve raises before it graduates"},
                {RPCResult::Type::NUM, "fee", "A trade's fee on a curve, in percent"},
                {RPCResult::Type::NUM, "creator_share", "The creator's part of it, in percent of the fee"},
                {RPCResult::Type::NUM, "security_share", "The chain's security (the BMM bidder and the validators), in percent of the fee"},
                {RPCResult::Type::NUM, "supply", "Whole units of a launch of the network's defaults (supply_default)"},
                {RPCResult::Type::NUM, "curve_supply", "Of which the curve sells (3/4)"},
                {RPCResult::Type::NUM, "pool_supply", "And go into the pool at graduation (1/4)"},
                {RPCResult::Type::BOOL, "params_active", "Whether the next block takes launches with parameters (supply, decimals, fee; an existing asset on a curve: curveasset)"},
                {RPCResult::Type::NUM, "params_height", "Their first block (-1: never on this network)"},
                {RPCResult::Type::BOOL, "fee_flow_active", "Whether the next block is under the fee-flow release: launches made then graduate into a locked full-range position of a concentrated pool, take a fee destination and may keep their control coin"},
                {RPCResult::Type::BOOL, "fee_split_active", "Whether the next block is under upgrade v7: a launch's fee 25 % security, 75 % split as it says (launchtoken's fee_split)"},
                {RPCResult::Type::BOOL, "instant_payouts_active", "Whether the next block is under upgrade v7: the DEX's payouts in the chain's coin come in the block itself, not at the next anchor"},
                {RPCResult::Type::NUM, "v7_height", "The block upgrade v7 applies from (-1: never on this network)"},
                {RPCResult::Type::NUM, "fee_flow_height", "Its first block (-1: never)"},
                {RPCResult::Type::NUM, "graduation_fee", "The fee of the pool such a launch graduates into, in percent (1)"},
                {RPCResult::Type::NUM, "graduated_creator_share", "Its creator's part of its locked position's fees, in percent (60; the rest to the chain's security)"},
                {RPCResult::Type::NUM, "fee_min", "The least fee a launch with parameters sets, in percent"},
                {RPCResult::Type::NUM, "fee_max", "The most, in percent"},
                {RPCResult::Type::NUM, "fee_default", "The network's (a launch without parameters)"},
                {RPCResult::Type::STR, "supply_default", "Whole units of a launch without parameters"},
                {RPCResult::Type::NUM, "decimals_default", "Its decimals"},
                {RPCResult::Type::NUM, "decimals_max", "The most decimals an asset has"},
                {RPCResult::Type::STR, "supply_min_units", "The least supply of a launch with parameters, in units (the smallest)"},
                {RPCResult::Type::STR, "supply_max_units", "The most, in units"},
                {RPCResult::Type::NUM, "supply_multiple_units", "Its supply in units is a multiple of this"},
                {RPCResult::Type::NUM, "max_allocation", "The most a creator's allocation is in the next block, in percent (10 from allocation10_height, 5 before)"},
                {RPCResult::Type::NUM, "allocation10_height", "The first block of allocations of up to 10 % (-1: never on this network)"},
                {RPCResult::Type::NUM, "log_blocks", "The blocks getlaunchlog keeps"},
                {RPCResult::Type::BOOL, "active", "Whether the next block takes launches"},
            }},
        }},
        RPCExamples{HelpExampleCli("getlaunchstats", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    return WithDexState(chainman, [&](const bitassets::State& state, [[maybe_unused]] int next_height) -> UniValue {
        const Consensus::SidechainParams& params{chainman.GetConsensus().sidechain};
        uint64_t count{0}, graduated{0}, volume{0}, raised{0};
        state.ForEachLaunch([&](int32_t, const AssetId& token) {
            const auto launch{state.GetLaunch(token)};
            if (!launch) return true;
            ++count;
            volume += launch->volume;
            if (launch->Graduated()) {
                ++graduated;
                raised += launch->raised_at_graduation;
            } else if (const auto pool{state.GetPool(launch->pool)}) {
                raised += pool->reserve0 - std::min(pool->reserve0, launch->virtual_coin);
            }
            return true;
        });
        UniValue result(UniValue::VOBJ);
        result.pushKV("launches", count);
        result.pushKV("on_curve", count - graduated);
        result.pushKV("graduated", graduated);
        result.pushKV("volume", ValueFromAmount(static_cast<CAmount>(volume)));
        result.pushKV("raised", ValueFromAmount(static_cast<CAmount>(raised)));
        UniValue rules(UniValue::VOBJ);
        rules.pushKV("graduation", ValueFromAmount(params.LaunchGraduation(next_height)));
        rules.pushKV("fee", DecimalToJSON(params.launch_fee / 10000.0));
        rules.pushKV("creator_share", DecimalToJSON(params.LaunchCreatorShare(next_height) / 10000.0));
        rules.pushKV("security_share", DecimalToJSON(params.LaunchSecurityShare(next_height) / 10000.0));
        rules.pushKV("supply", AmountToJSON(bitassets::LAUNCH_SUPPLY, bitassets::LAUNCH_DECIMALS));
        rules.pushKV("curve_supply", AmountToJSON(bitassets::LAUNCH_CURVE_SUPPLY, bitassets::LAUNCH_DECIMALS));
        rules.pushKV("pool_supply", AmountToJSON(bitassets::LAUNCH_POOL_SUPPLY, bitassets::LAUNCH_DECIMALS));
        rules.pushKV("params_active", params.LaunchParamsActive(next_height));
        rules.pushKV("fee_flow_active", params.FeeFlowActive(next_height));
        rules.pushKV("fee_split_active", params.FeeSplitActive(next_height));
        rules.pushKV("instant_payouts_active", params.InstantPayoutsActive(next_height));
        rules.pushKV("v7_height", params.v7_height == std::numeric_limits<int>::max() ? -1 : params.v7_height);
        rules.pushKV("fee_flow_height", params.fee_flow_height == std::numeric_limits<int>::max() ? -1 : params.fee_flow_height);
        rules.pushKV("graduation_fee", DecimalToJSON(bitassets::GRADUATION_FEE / 10000.0));
        rules.pushKV("graduated_creator_share", DecimalToJSON(bitassets::GRADUATED_CREATOR_SHARE / 10000.0));
        rules.pushKV("params_height", params.launch_params_height == std::numeric_limits<int>::max() ? -1 : params.launch_params_height);
        rules.pushKV("fee_min", DecimalToJSON(bitassets::MIN_LAUNCH_FEE / 10000.0));
        rules.pushKV("fee_max", DecimalToJSON(bitassets::MAX_LAUNCH_FEE / 10000.0));
        rules.pushKV("fee_default", DecimalToJSON(params.launch_fee / 10000.0));
        rules.pushKV("supply_default", bitassets::FormatUnits(bitassets::LAUNCH_SUPPLY, bitassets::LAUNCH_DECIMALS));
        rules.pushKV("decimals_default", bitassets::LAUNCH_DECIMALS);
        rules.pushKV("decimals_max", bitassets::MAX_DECIMALS);
        rules.pushKV("supply_min_units", strprintf("%u", bitassets::MIN_LAUNCH_SUPPLY));
        rules.pushKV("supply_max_units", strprintf("%u", bitassets::MAX_LAUNCH_SUPPLY));
        rules.pushKV("supply_multiple_units", bitassets::LAUNCH_SUPPLY_MULTIPLE);
        rules.pushKV("max_allocation", DecimalToJSON(bitassets::MaxLaunchAllocation(params.Allocation10Active(next_height)) / 100.0));
        rules.pushKV("allocation10_height", params.allocation10_height == std::numeric_limits<int>::max() ? -1 : params.allocation10_height);
        rules.pushKV("log_blocks", bitassets::LAUNCH_LOG_BLOCKS);
        rules.pushKV("active", params.DexCurvesActive(next_height));
        result.pushKV("rules", std::move(rules));
        return result;
    });
},
    };
}

RPCMethod getlaunchlog()
{
    return RPCMethod{
        "getlaunchlog",
        "For indexers: what blocks did on launch curves, a row per block and launch: made, " + CURRENCY_UNIT + " and units of the asset in and out, fees, the curve after, "
        "graduated. The last 10080 blocks are kept (getlaunchstats' log_blocks); a reorganisation takes rows back with their blocks.",
        {
            {"from_height", RPCArg::Type::NUM, RPCArg::Optional::NO, "The first block"},
            {"to_height", RPCArg::Type::NUM, RPCArg::DefaultHint{"from_height"}, "The last block (at most 1000 blocks)"},
        },
        RPCResult{RPCResult::Type::ARR, "", "",
        {
            {RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::NUM, "height", "The block"},
                {RPCResult::Type::STR_HEX, "token", "The launch"},
                {RPCResult::Type::BOOL, "created", "Made in this block"},
                {RPCResult::Type::BOOL, "graduated", "Graduated in this block"},
                {RPCResult::Type::NUM, "orders", "Orders it filled"},
                {RPCResult::Type::STR_AMOUNT, "coin_in", CURRENCY_UNIT + " buyers paid in (what came back not counted)"},
                {RPCResult::Type::STR_AMOUNT, "coin_out", CURRENCY_UNIT + " sellers got"},
                {RPCResult::Type::NUM, "tokens_in", "Tokens sold to it"},
                {RPCResult::Type::NUM, "tokens_out", "Tokens bought from it"},
                {RPCResult::Type::STR_AMOUNT, "fees", "Its fees"},
                {RPCResult::Type::STR_AMOUNT, "reserve_coin", "The curve's " + CURRENCY_UNIT + " after (virtual), or the pool's at graduation"},
                {RPCResult::Type::NUM, "reserve_tokens", "And its tokens"},
            }},
        }},
        RPCExamples{HelpExampleCli("getlaunchlog", "1000 1100")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const int from{request.params[0].getInt<int>()};
    const int to{request.params[1].isNull() ? from : request.params[1].getInt<int>()};
    if (from < 0 || to < from || to - from >= 1000) throw JSONRPCError(RPC_INVALID_PARAMETER, "from_height to to_height: at most 1000 blocks");
    return WithDexState(chainman, [&](const bitassets::State& state, [[maybe_unused]] int next_height) -> UniValue {
        UniValue rows(UniValue::VARR);
        for (int h{from}; h <= to; ++h) {
            for (const auto& [token, e] : state.GetLaunchLog(h)) {
                UniValue row(UniValue::VOBJ);
                row.pushKV("height", h);
                row.pushKV("token", token.GetHex());
                row.pushKV("created", e.created);
                row.pushKV("graduated", e.graduated);
                row.pushKV("orders", e.orders);
                row.pushKV("coin_in", ValueFromAmount(static_cast<CAmount>(e.coin_in)));
                row.pushKV("coin_out", ValueFromAmount(static_cast<CAmount>(e.coin_out)));
                const uint8_t decimals{LaunchDecimals(state, token)};
                row.pushKV("tokens_in", AmountToJSON(e.tokens_in, decimals));
                row.pushKV("tokens_out", AmountToJSON(e.tokens_out, decimals));
                row.pushKV("fees", ValueFromAmount(static_cast<CAmount>(e.fees)));
                row.pushKV("reserve_coin", ValueFromAmount(static_cast<CAmount>(e.reserve_coin)));
                row.pushKV("reserve_tokens", AmountToJSON(e.reserve_tokens, decimals));
                rows.push_back(std::move(row));
            }
        }
        return rows;
    });
},
    };
}

RPCMethod quotepoolliquidity()
{
    return RPCMethod{
        "quotepoolliquidity",
        "What putting liquidity into a pool (by id, any kind) or taking it out would give now, as the rules work it out: the shares for "
        "amounts (one side or both), or the amounts for shares (both sides, or one alone).",
        {
            {"pool", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The pool"},
            {"amounts", RPCArg::Type::OBJ_USER_KEYS, RPCArg::Optional::OMITTED, "To put in: amounts by asset (an asset's id, 0x and 64 hex digits, or " + CURRENCY_UNIT + ")",
            {
                {"asset", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "How much"},
            }},
            {"shares", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Or, to take out: shares"},
            {"only", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "With shares: the one asset to take out alone"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "shares", "The shares (given, or that the amounts get)"},
            {RPCResult::Type::BOOL, "opens", "Whether the deposit opens the pool (nobody provides for it)"},
            {RPCResult::Type::OBJ_DYN, "amounts", "By asset: what goes in, or what comes out", {{RPCResult::Type::NUM, "asset", ""}}},
            {RPCResult::Type::NUM, "share_of_pool", "In percent, after"},
        }},
        RPCExamples{HelpExampleCli("quotepoolliquidity", "\"<pool>\" '{\"" + CURRENCY_UNIT + "\":1}'") + HelpExampleCli("-named quotepoolliquidity", "pool=<pool> shares=1000 only=" + CURRENCY_UNIT)},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const uint256 id{ParseHashV(request.params[0], "pool")};
    return WithDexState(chainman, [&](const bitassets::State& state, [[maybe_unused]] int next_height) -> UniValue {
        const auto found{state.GetPool(id)};
        if (!found) throw JSONRPCError(RPC_INVALID_PARAMETER, "No such pool");
        const bitassets::Pool& pool{*found};
        const bitassets::PoolParams kind{state.GetPoolParams(id)};
        if (kind.GetCurve() == bitassets::Curve::LAUNCH) throw JSONRPCError(RPC_INVALID_PARAMETER, "A launch's curve takes no liquidity");
        const uint8_t d0{DecimalsOf(state, pool.asset0)}, d1{DecimalsOf(state, pool.asset1)};
        const uint32_t fee{state.QuoteFee(id, kind)};
        if (kind.GetCurve() == bitassets::Curve::CONCENTRATED) throw JSONRPCError(RPC_INVALID_PARAMETER, "A concentrated pool takes liquidity by positions (previewposition)");
        UniValue result(UniValue::VOBJ);
        UniValue amounts(UniValue::VOBJ);
        if (!request.params[1].isNull()) {
            uint64_t a0{0}, a1{0};
            for (const std::string& key : request.params[1].getKeys()) {
                const AssetId asset{ParseAsset(UniValue{key}, state)};
                if (asset != pool.asset0 && asset != pool.asset1) throw JSONRPCError(RPC_INVALID_PARAMETER, key + " is not an asset of this pool");
                (asset == pool.asset0 ? a0 : a1) = bitassets::ParseUnits(request.params[1][key], asset == pool.asset0 ? d0 : d1, /*allow_zero=*/true);
            }
            uint64_t shares{0};
            const bool opens{bitassets::amm::Abandoned(pool)};
            if (opens) {
                const auto total{a0 && a1 ? bitassets::curves::OpeningShares(kind, pool.reserve0 + a0, pool.reserve1 + a1) : std::nullopt};
                shares = total && *total > bitassets::MIN_LIQUIDITY ? *total - bitassets::MIN_LIQUIDITY : 0;
            } else {
                shares = bitassets::curves::DepositShares(kind, fee, pool, a0, a1).value_or(0);
            }
            amounts.pushKV(LabelOf(state, pool.asset0), AmountToJSON(a0, d0));
            amounts.pushKV(LabelOf(state, pool.asset1), AmountToJSON(a1, d1));
            result.pushKV("shares", shares);
            result.pushKV("opens", opens);
            result.pushKV("amounts", std::move(amounts));
            const uint64_t total{opens ? shares + bitassets::MIN_LIQUIDITY : pool.shares + shares};
            result.pushKV("share_of_pool", DecimalToJSON(total ? 100.0 * shares / total : 0));
            return result;
        }
        if (request.params[2].isNull()) throw JSONRPCError(RPC_INVALID_PARAMETER, "Name amounts to put in, or shares to take out");
        const uint64_t shares{request.params[2].getInt<uint64_t>()};
        if (shares == 0 || shares > pool.shares - std::min(pool.shares, bitassets::MIN_LIQUIDITY)) throw JSONRPCError(RPC_INVALID_PARAMETER, "More shares than the pool can give back");
        uint8_t only{0};
        if (!request.params[3].isNull()) {
            const AssetId asset{ParseAsset(request.params[3], state)};
            if (asset != pool.asset0 && asset != pool.asset1) throw JSONRPCError(RPC_INVALID_PARAMETER, "That is not an asset of this pool");
            only = asset == pool.asset0 ? 1 : 2;
        }
        auto [out0, out1]{bitassets::curves::WithdrawOf(kind, fee, pool, shares, only)};
        if (pool.asset0.IsNull() && out0 < bitassets::MinChnPayout(next_height)) out0 = 0;
        if (pool.asset1.IsNull() && out1 < bitassets::MinChnPayout(next_height)) out1 = 0;
        amounts.pushKV(LabelOf(state, pool.asset0), AmountToJSON(out0, d0));
        amounts.pushKV(LabelOf(state, pool.asset1), AmountToJSON(out1, d1));
        result.pushKV("shares", shares);
        result.pushKV("opens", false);
        result.pushKV("amounts", std::move(amounts));
        result.pushKV("share_of_pool", DecimalToJSON(100.0 * shares / pool.shares));
        return result;
    });
},
    };
}

//
// Concentrated liquidity: positions and ticks.
//

/** A price given as a number or as text. */
double PriceArg(const UniValue& value)
{
    if (value.isNum()) return value.get_real();
    std::istringstream in{value.getValStr()};
    in.imbue(std::locale::classic());
    double price{0};
    if (!(in >> price) || !in.eof()) throw JSONRPCError(RPC_INVALID_PARAMETER, "A price is a number");
    return price;
}

/** A tick's price of asset0 in asset1, whole units. */
double TickPrice(int32_t tick, uint8_t d0, uint8_t d1) { return std::pow(1.0001, tick) * std::pow(10.0, int{d0} - int{d1}); }

RPCMethod getposition()
{
    return RPCMethod{
        "getposition",
        "A position of concentrated liquidity: its pool, range, liquidity, what it holds now, the fees it earned, whether it is in range.",
        {{"position", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Its id"}},
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "position", "Its id"},
            {RPCResult::Type::STR_HEX, "pool", "Its pool"},
            {RPCResult::Type::STR, "asset0", "The pool's first asset (the lower id)"},
            {RPCResult::Type::STR, "asset1", "Its second"},
            {RPCResult::Type::NUM, "tick_lower", ""},
            {RPCResult::Type::NUM, "tick_upper", ""},
            {RPCResult::Type::NUM, "price_lower", "Its range: asset0 in asset1, whole units"},
            {RPCResult::Type::NUM, "price_upper", ""},
            {RPCResult::Type::STR, "liquidity", ""},
            {RPCResult::Type::BOOL, "in_range", "Whether the pool's price is in its range"},
            {RPCResult::Type::NUM, "amount0", "What it holds now (taken out now, rounded down)"},
            {RPCResult::Type::NUM, "amount1", ""},
            {RPCResult::Type::NUM, "fees0", "Fees owed"},
            {RPCResult::Type::NUM, "fees1", ""},
            {RPCResult::Type::STR_HEX, "txid", "The transaction that opened it"},
            {RPCResult::Type::NUM, "height", "Its block"},
        }},
        RPCExamples{HelpExampleCli("getposition", "\"<position>\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const uint256 id{ParseHashV(request.params[0], "position")};
    return WithDexState(chainman, [&](const bitassets::State& state, [[maybe_unused]] int next_height) -> UniValue {
        const auto position{state.GetPosition(id)};
        if (!position) throw JSONRPCError(RPC_INVALID_PARAMETER, "No such position");
        const auto pool{state.GetPool(position->pool)};
        const auto cl{state.GetClState(position->pool)};
        if (!pool || !cl) throw JSONRPCError(RPC_MISC_ERROR, "Its pool is gone");
        const uint8_t d0{DecimalsOf(state, pool->asset0)}, d1{DecimalsOf(state, pool->asset1)};
        const auto amounts{bitassets::cl::AmountsForLiquidity(UintToArith256(cl->sqrt_price), bitassets::cl::SqrtRatioAtTick(position->tick_lower),
                                                              bitassets::cl::SqrtRatioAtTick(position->tick_upper), position->Liquidity(), false)};
        const auto [fee0, fee1]{state.PositionFees(*position)};
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("position", id.GetHex());
        obj.pushKV("pool", position->pool.GetHex());
        obj.pushKV("asset0", LabelOf(state, pool->asset0));
        obj.pushKV("asset1", LabelOf(state, pool->asset1));
        obj.pushKV("tick_lower", position->tick_lower);
        obj.pushKV("tick_upper", position->tick_upper);
        obj.pushKV("price_lower", DecimalToJSON(TickPrice(position->tick_lower, d0, d1)));
        obj.pushKV("price_upper", DecimalToJSON(TickPrice(position->tick_upper, d0, d1)));
        obj.pushKV("liquidity", U128Text(position->Liquidity()));
        obj.pushKV("in_range", position->tick_lower <= cl->tick && cl->tick < position->tick_upper);
        obj.pushKV("amount0", AmountToJSON(amounts ? amounts->first : 0, d0));
        obj.pushKV("amount1", AmountToJSON(amounts ? amounts->second : 0, d1));
        obj.pushKV("fees0", AmountToJSON(fee0, d0));
        obj.pushKV("fees1", AmountToJSON(fee1, d1));
        obj.pushKV("txid", position->txid.GetHex());
        obj.pushKV("height", position->height);
        return obj;
    });
},
    };
}

RPCMethod getpoolticks()
{
    return RPCMethod{
        "getpoolticks",
        "A concentrated pool's liquidity by price: each tick where liquidity starts or ends, what crossing it changes, and the liquidity in force "
        "from it up to the next (a liquidity distribution chart).",
        {
            {"pool", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The pool"},
            {"count", RPCArg::Type::NUM, RPCArg::Default{500}, "Most ticks, 1 to 5000"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "tick", "The pool's tick"},
            {RPCResult::Type::NUM, "price", "Its price, asset0 in asset1, whole units"},
            {RPCResult::Type::STR, "liquidity", "In range now"},
            {RPCResult::Type::NUM, "tick_spacing", ""},
            {RPCResult::Type::ARR, "ticks", "", {{RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::NUM, "tick", ""},
                {RPCResult::Type::NUM, "price", "asset0 in asset1, whole units"},
                {RPCResult::Type::STR, "liquidity_net", "What crossing it upward adds (negative: takes)"},
                {RPCResult::Type::STR, "liquidity_gross", "All liquidity that starts or ends there"},
                {RPCResult::Type::STR, "liquidity_above", "The liquidity in force from it to the next tick"},
            }}}},
        }},
        RPCExamples{HelpExampleCli("getpoolticks", "\"<pool>\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const uint256 id{ParseHashV(request.params[0], "pool")};
    const int count{request.params[1].isNull() ? 500 : request.params[1].getInt<int>()};
    if (count < 1 || count > 5000) throw JSONRPCError(RPC_INVALID_PARAMETER, "count is 1 to 5000");
    return WithDexState(chainman, [&](const bitassets::State& state, [[maybe_unused]] int next_height) -> UniValue {
        const auto pool{state.GetPool(id)};
        const auto cl{state.GetClState(id)};
        if (!pool || !cl) throw JSONRPCError(RPC_INVALID_PARAMETER, "Not a concentrated pool");
        const uint8_t d0{DecimalsOf(state, pool->asset0)}, d1{DecimalsOf(state, pool->asset1)};
        UniValue result(UniValue::VOBJ);
        result.pushKV("tick", cl->tick);
        result.pushKV("price", DecimalToJSON(WholeSpotPrice(state.GetPoolParams(id), *pool, true, d0, d1, &*cl)));
        result.pushKV("liquidity", U128Text(cl->Liquidity()));
        result.pushKV("tick_spacing", bitassets::cl::TickSpacing(state.GetPoolParams(id).fee));
        UniValue ticks(UniValue::VARR);
        __int128 running{0};
        for (const auto& [tick, info] : state.ClTicksOf(id, count)) {
            const __int128 net{info.net_negative ? -static_cast<__int128>(info.NetAbs()) : static_cast<__int128>(info.NetAbs())};
            running += net;
            UniValue t(UniValue::VOBJ);
            t.pushKV("tick", tick);
            t.pushKV("price", DecimalToJSON(TickPrice(tick, d0, d1)));
            t.pushKV("liquidity_net", (net < 0 ? "-" : "") + U128Text(static_cast<unsigned __int128>(net < 0 ? -net : net)));
            t.pushKV("liquidity_gross", U128Text(info.Gross()));
            t.pushKV("liquidity_above", U128Text(running < 0 ? 0 : static_cast<unsigned __int128>(running)));
            ticks.push_back(std::move(t));
        }
        result.pushKV("ticks", std::move(ticks));
        return result;
    });
},
    };
}

RPCMethod previewposition()
{
    return RPCMethod{
        "previewposition",
        "What openposition would make in a concentrated pool: the range in ticks (to the pool's spacing), the liquidity the amounts give, and what goes in of each.",
        {
            {"pool", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The pool"},
            {"price_lower", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "asset0 in asset1, whole units (0 with price_upper 0: the full range)"},
            {"price_upper", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, ""},
            {"amounts", RPCArg::Type::OBJ_USER_KEYS, RPCArg::Optional::NO, "The most of each, by asset (its id or " + CURRENCY_UNIT + ")", {{"asset", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, ""}}},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "tick_lower", ""},
            {RPCResult::Type::NUM, "tick_upper", ""},
            {RPCResult::Type::NUM, "price_lower", "The range as the ticks make it"},
            {RPCResult::Type::NUM, "price_upper", ""},
            {RPCResult::Type::STR, "liquidity", ""},
            {RPCResult::Type::NUM, "amount0", "What goes in"},
            {RPCResult::Type::NUM, "amount1", ""},
            {RPCResult::Type::BOOL, "in_range", "Whether the price is in the range now (else one asset alone goes in)"},
        }},
        RPCExamples{HelpExampleCli("previewposition", "\"<pool>\" 0.9 1.1 '{\"" + CURRENCY_UNIT + "\":10,\"0x<id>\":10}'")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const uint256 id{ParseHashV(request.params[0], "pool")};
    return WithDexState(chainman, [&](const bitassets::State& state, [[maybe_unused]] int next_height) -> UniValue {
        const auto pool{state.GetPool(id)};
        const auto cl{state.GetClState(id)};
        if (!pool || !cl) throw JSONRPCError(RPC_INVALID_PARAMETER, "Not a concentrated pool");
        const uint8_t d0{DecimalsOf(state, pool->asset0)}, d1{DecimalsOf(state, pool->asset1)};
        const int32_t spacing{bitassets::cl::TickSpacing(state.GetPoolParams(id).fee)};
        const auto tick_of{[&](double price, bool up) {
            // 0: no bound on that side (full range: the lowest or the highest tick of the spacing).
            if (price == 0) return up ? bitassets::cl::MAX_TICK / spacing * spacing : bitassets::cl::MIN_TICK / spacing * spacing;
            if (!(price > 0)) throw JSONRPCError(RPC_INVALID_PARAMETER, "A price is above 0 (0: no bound, the full range)");
            const double exact{std::log(price * std::pow(10.0, int{d1} - int{d0})) / std::log(1.0001)};
            int32_t tick{static_cast<int32_t>(std::clamp(std::floor(exact), double(bitassets::cl::MIN_TICK), double(bitassets::cl::MAX_TICK)))};
            int32_t aligned{tick / spacing * spacing};
            if (aligned > tick) aligned -= spacing;
            if (up && aligned < tick) aligned += spacing;
            return std::clamp(aligned, bitassets::cl::MIN_TICK / spacing * spacing, bitassets::cl::MAX_TICK / spacing * spacing);
        }};
        const int32_t lower{tick_of(PriceArg(request.params[1]), false)}, upper{tick_of(PriceArg(request.params[2]), true)};
        if (lower >= upper) throw JSONRPCError(RPC_INVALID_PARAMETER, "The range is empty");
        uint64_t a0{0}, a1{0};
        for (const std::string& key : request.params[3].getKeys()) {
            const AssetId asset{ParseAsset(UniValue{key}, state)};
            if (asset != pool->asset0 && asset != pool->asset1) throw JSONRPCError(RPC_INVALID_PARAMETER, key + " is not an asset of this pool");
            (asset == pool->asset0 ? a0 : a1) = bitassets::ParseUnits(request.params[3][key], asset == pool->asset0 ? d0 : d1, /*allow_zero=*/true);
        }
        const arith_uint256 price{UintToArith256(cl->sqrt_price)}, sa{bitassets::cl::SqrtRatioAtTick(lower)}, sb{bitassets::cl::SqrtRatioAtTick(upper)};
        const unsigned __int128 liquidity{bitassets::cl::LiquidityForAmounts(price, sa, sb, a0, a1)};
        const auto needed{bitassets::cl::AmountsForLiquidity(price, sa, sb, liquidity, true)};
        UniValue result(UniValue::VOBJ);
        result.pushKV("tick_lower", lower);
        result.pushKV("tick_upper", upper);
        result.pushKV("price_lower", DecimalToJSON(TickPrice(lower, d0, d1)));
        result.pushKV("price_upper", DecimalToJSON(TickPrice(upper, d0, d1)));
        result.pushKV("liquidity", U128Text(liquidity));
        result.pushKV("amount0", AmountToJSON(needed ? needed->first : 0, d0));
        result.pushKV("amount1", AmountToJSON(needed ? needed->second : 0, d1));
        result.pushKV("in_range", lower <= cl->tick && cl->tick < upper);
        return result;
    });
},
    };
}

//
// Order books.
//

UniValue OrderToJSON(const bitassets::State& state, const uint256& id, const bitassets::BookOrder& o)
{
    const auto book{state.GetPool(o.book)};
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("order", id.GetHex());
    obj.pushKV("book", o.book.GetHex());
    if (!book) return obj;
    const AssetId sell{o.sells0 ? book->asset0 : book->asset1}, buy{o.sells0 ? book->asset1 : book->asset0};
    const uint8_t ds{DecimalsOf(state, sell)}, db{DecimalsOf(state, buy)};
    obj.pushKV("sells", LabelOf(state, sell));
    obj.pushKV("sells_id", sell.GetHex());
    obj.pushKV("buys", LabelOf(state, buy));
    obj.pushKV("buys_id", buy.GetHex());
    obj.pushKV("amount", AmountToJSON(o.amount, ds));
    obj.pushKV("remaining", AmountToJSON(o.remaining, ds));
    // Its limit, in whole units of what it buys per whole unit of what it sells.
    obj.pushKV("price", DecimalToJSON(static_cast<double>(o.price_num) / o.price_den * std::pow(10.0, int{ds} - int{db})));
    obj.pushKV("got", AmountToJSON(o.got, db));
    obj.pushKV("to_collect", AmountToJSON(o.proceeds, db));
    obj.pushKV("expiry", o.expiry);
    obj.pushKV("txid", o.txid.GetHex());
    obj.pushKV("height", o.height);
    if (o.evicted != 0) obj.pushKV("evicted_height", o.evicted);
    return obj;
}

const std::vector<RPCResult> ORDER_RESULTS{
    {RPCResult::Type::STR_HEX, "order", "Its id (its token)"},
    {RPCResult::Type::STR_HEX, "book", "Its book"},
    {RPCResult::Type::STR, "sells", "What it sells"},
    {RPCResult::Type::STR_HEX, "sells_id", ""},
    {RPCResult::Type::STR, "buys", "What it buys"},
    {RPCResult::Type::STR_HEX, "buys_id", ""},
    {RPCResult::Type::NUM, "amount", "What it sold at first"},
    {RPCResult::Type::NUM, "remaining", "What is left to sell"},
    {RPCResult::Type::NUM, "price", "Its limit: whole units bought per whole unit sold, at least"},
    {RPCResult::Type::NUM, "got", "What it bought in all"},
    {RPCResult::Type::NUM, "to_collect", "What it bought and is not collected yet"},
    {RPCResult::Type::NUM, "expiry", "The last block it trades in"},
    {RPCResult::Type::STR_HEX, "txid", "The transaction that placed it"},
    {RPCResult::Type::NUM, "height", "Its block"},
    {RPCResult::Type::NUM, "evicted_height", /*optional=*/true, "The block in which a better and no smaller order took its place in a full book (it trades no more; settle it to take all it holds back)"},
};

RPCMethod getorder()
{
    return RPCMethod{
        "getorder",
        "A limit order of a book (placeorder): what it sells and buys, its limit, what is left, what it got.",
        {{"order", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Its id"}},
        RPCResult{RPCResult::Type::OBJ, "", "", ORDER_RESULTS},
        RPCExamples{HelpExampleCli("getorder", "\"<order>\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const uint256 id{ParseHashV(request.params[0], "order")};
    return WithDexState(chainman, [&](const bitassets::State& state, [[maybe_unused]] int next_height) -> UniValue {
        const auto order{state.GetOrder(id)};
        if (!order) throw JSONRPCError(RPC_INVALID_PARAMETER, "No such order");
        return OrderToJSON(state, id, *order);
    });
},
    };
}

RPCMethod getpoolcandles()
{
    return RPCMethod{
        "getpoolcandles",
        "The candles (OHLCV) of a pool, a book or a launch's curve, from the market index (-dexindex): the price of asset_b in asset_a in whole "
        "units (" + CURRENCY_UNIT + " per unit for one with " + CURRENCY_UNIT + "), the volume in asset_a, oldest first, at most 500.",
        {
            {"id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The pool's id (a book's: getbook's book; a launch's curve: getlaunch's pool)"},
            {"interval", RPCArg::Type::STR, RPCArg::Default{"1h"}, "5m, 1h, 4h or 1d"},
            {"from", RPCArg::Type::NUM, RPCArg::Default{0}, "The first time (Unix seconds)"},
            {"to", RPCArg::Type::NUM, RPCArg::DefaultHint{"now"}, "The last time"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::NUM, "t", "Its start"}, {RPCResult::Type::NUM, "o", ""}, {RPCResult::Type::NUM, "h", ""}, {RPCResult::Type::NUM, "l", ""},
            {RPCResult::Type::NUM, "c", ""}, {RPCResult::Type::NUM, "v", "Volume"}, {RPCResult::Type::NUM, "n", "Trades"}}}}},
        RPCExamples{HelpExampleCli("getpoolcandles", "\"<pool id>\" 1h")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    if (!bitassets::market::Enabled()) throw JSONRPCError(RPC_MISC_ERROR, "This node keeps no market index (-dexindex)");
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const uint256 id{ParseHashV(request.params[0], "id")};
    const std::string text{request.params[1].isNull() ? "1h" : request.params[1].get_str()};
    const auto interval{bitassets::market::ParseInterval(text)};
    if (!interval) throw JSONRPCError(RPC_INVALID_PARAMETER, "interval: 5m, 1h, 4h or 1d");
    const uint32_t from{request.params[2].isNull() ? 0 : request.params[2].getInt<uint32_t>()};
    const uint32_t to{request.params[3].isNull() ? static_cast<uint32_t>(GetTime()) : request.params[3].getInt<uint32_t>()};
    std::shared_ptr<const sidechain::StoreView> view;
    {
        LOCK(::cs_main);
        view = chainman.ActiveChainstate().SideCache().SharedSnapshot();
        if (!view) throw JSONRPCError(RPC_IN_WARMUP, "No snapshot of the state yet: try again");
    }
    UniValue out(UniValue::VARR);
    for (const auto& c : bitassets::market::Candles(*view, bitassets::market::Market{false, id}, *interval, from, to, 500)) {
        UniValue row(UniValue::VOBJ);
        row.pushKV("t", c.t);
        row.pushKV("o", c.o);
        row.pushKV("h", c.h);
        row.pushKV("l", c.l);
        row.pushKV("c", c.c);
        row.pushKV("v", c.v);
        row.pushKV("n", c.n);
        out.push_back(std::move(row));
    }
    return out;
},
    };
}

/** 10^n as a 256-bit integer. */
arith_uint256 Pow10(int n)
{
    arith_uint256 out{1};
    for (int i{0}; i < n; ++i) out *= 10;
    return out;
}

double RatioToDouble(const arith_uint256& n, const arith_uint256& d) { return n.getdouble() / d.getdouble(); }

/** n/d as a decimal of `significant` digits (rounded down; trailing zeros dropped): exact text of a rational price. */
std::string ExactDecimal(arith_uint256 n, const arith_uint256& d, int significant)
{
    if (d == 0) return "0";
    const arith_uint256 whole{n / d};
    std::string out{whole == 0 ? std::string{"0"} : [&] {
        std::string digits;
        arith_uint256 w{whole};
        while (w > 0) {
            digits.insert(digits.begin(), static_cast<char>('0' + (w - (w / 10) * 10).GetLow64()));
            w /= 10;
        }
        return digits;
    }()};
    int used{whole == 0 ? 0 : static_cast<int>(out.size())};
    arith_uint256 rest{n - whole * d};
    std::string frac;
    while (rest > 0 && used < significant && frac.size() < 60) {
        rest *= 10;
        const arith_uint256 digit{rest / d};
        rest -= digit * d;
        frac.push_back(static_cast<char>('0' + digit.GetLow64()));
        if (used > 0 || digit > 0) ++used;
    }
    while (!frac.empty() && frac.back() == '0') frac.pop_back();
    return frac.empty() ? out : out + "." + frac;
}

RPCMethod getbook()
{
    return RPCMethod{
        "getbook",
        "The order book of two assets: its open orders on each side by price (a ladder), the best of each, and the spread. Prices of a whole "
        "asset_a in asset_b.",
        {
            {"asset_a", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::AssetArgHelp()},
            {"asset_b", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::AssetArgHelp()},
            {"levels", RPCArg::Type::NUM, RPCArg::Default{50}, "Most prices on each side (1 to 500)"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "book", "Its id (a venue of routes: swappools)"},
            {RPCResult::Type::STR, "asset_a", ""},
            {RPCResult::Type::STR, "asset_b", ""},
            {RPCResult::Type::NUM, "open_orders", "Open orders, both sides"},
            {RPCResult::Type::NUM, "best_ask", /*optional=*/true, "The lowest price asset_a is sold at"},
            {RPCResult::Type::NUM, "best_bid", /*optional=*/true, "The highest price asset_a is bought at"},
            {RPCResult::Type::STR, "best_ask_exact", /*optional=*/true, "The same, exact (18 significant digits)"},
            {RPCResult::Type::STR, "best_bid_exact", /*optional=*/true, "The same, exact (18 significant digits)"},
            {RPCResult::Type::ARR, "asks", "Sellers of asset_a, best first", {{RPCResult::Type::OBJ, "", "", {{RPCResult::Type::NUM, "price", ""}, {RPCResult::Type::NUM, "amount", "asset_a for sale at it"}, {RPCResult::Type::NUM, "orders", ""},
                {RPCResult::Type::STR, "price_exact", "The price, exactly the orders' limit (a decimal of 18 significant digits, rounded down)"}, {RPCResult::Type::STR, "amount_units", "The amount in units of asset_a"}}}}},
            {RPCResult::Type::ARR, "bids", "Buyers of asset_a, best first", {{RPCResult::Type::OBJ, "", "", {{RPCResult::Type::NUM, "price", ""}, {RPCResult::Type::NUM, "amount", "asset_a wanted at it (what their asset_b buys)"}, {RPCResult::Type::NUM, "orders", ""},
                {RPCResult::Type::STR, "price_exact", "The price, exactly"}, {RPCResult::Type::STR, "amount_units", "The amount in units of asset_a (rounded down)"}}}}},
        }},
        RPCExamples{HelpExampleCli("getbook", "\"GOLD\" \"" + CURRENCY_UNIT + "\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const int levels{request.params[2].isNull() ? 50 : request.params[2].getInt<int>()};
    if (levels < 1 || levels > 500) throw JSONRPCError(RPC_INVALID_PARAMETER, "levels is 1 to 500");
    return WithDexState(chainman, [&](const bitassets::State& state, [[maybe_unused]] int next_height) -> UniValue {
        const AssetId a{ParseAsset(request.params[0], state)}, b{ParseAsset(request.params[1], state)};
        if (a == b) throw JSONRPCError(RPC_INVALID_PARAMETER, "Two different assets make a book");
        const uint256 id{bitassets::BookId(a, b)};
        const auto book{state.GetPool(id)};
        const uint8_t da{DecimalsOf(state, a)}, db{DecimalsOf(state, b)};
        const bool a_is_0{a < b};
        UniValue result(UniValue::VOBJ);
        result.pushKV("book", id.GetHex());
        result.pushKV("asset_a", LabelOf(state, a));
        result.pushKV("asset_b", LabelOf(state, b));
        result.pushKV("open_orders", book ? book->shares - std::min<uint64_t>(book->shares, bitassets::MIN_LIQUIDITY) : 0);
        // The side that sells a: the asks of a. Each level at the exact price of its orders' limits (a rational of a whole a in
        // b), its amount exact in units of a (a bid's what its b buys at its limit, rounded down); price_exact a decimal of 18
        // significant digits of it, amount_units the units.
        const arith_uint256 scale_a{Pow10(da > db ? da - db : 0)}, scale_b{Pow10(db > da ? db - da : 0)};
        struct Rat {
            arith_uint256 n, d;
        };
        const auto less{[](const Rat& x, const Rat& y) { return x.n * y.d < y.n * x.d; }};
        const auto ladder{[&](bool sells_a) {
            UniValue arr(UniValue::VARR);
            std::map<Rat, std::pair<uint64_t, int>, std::function<bool(const Rat&, const Rat&)>> levels_map{sells_a ? std::function<bool(const Rat&, const Rat&)>{less}
                                                                                                               : std::function<bool(const Rat&, const Rat&)>{[&](const Rat& x, const Rat& y) { return less(y, x); }}};
            for (const auto& [oid, o] : state.BookSide(id, sells_a == a_is_0, bitassets::MAX_BOOK_SIDE, next_height)) {
                if (o.remaining == 0) continue;
                // Its limit: b units per a unit (an ask), a units per b unit (a bid); as a price of a whole a in b.
                const Rat price{sells_a ? Rat{arith_uint256{o.price_num} * scale_a, arith_uint256{o.price_den} * scale_b}
                                        : Rat{arith_uint256{o.price_den} * scale_a, arith_uint256{o.price_num} * scale_b}};
                const uint64_t units_a{sells_a ? o.remaining : (arith_uint256{o.remaining} * o.price_num / o.price_den).GetLow64()};
                auto& level{levels_map[price]};
                level.first = units_a > std::numeric_limits<uint64_t>::max() - level.first ? std::numeric_limits<uint64_t>::max() : level.first + units_a;
                ++level.second;
            }
            for (const auto& [price, level] : levels_map) {
                if (arr.size() >= static_cast<size_t>(levels)) break;
                UniValue l(UniValue::VOBJ);
                l.pushKV("price", DecimalToJSON(RatioToDouble(price.n, price.d)));
                l.pushKV("amount", DecimalToJSON(level.first / std::pow(10.0, da)));
                l.pushKV("orders", level.second);
                l.pushKV("price_exact", ExactDecimal(price.n, price.d, 18));
                l.pushKV("amount_units", strprintf("%u", level.first));
                arr.push_back(std::move(l));
            }
            return arr;
        }};
        UniValue asks{ladder(true)}, bids{ladder(false)};
        if (!asks.empty()) result.pushKV("best_ask", asks[0]["price"]);
        if (!bids.empty()) result.pushKV("best_bid", bids[0]["price"]);
        if (!asks.empty()) result.pushKV("best_ask_exact", asks[0]["price_exact"]);
        if (!bids.empty()) result.pushKV("best_bid_exact", bids[0]["price_exact"]);
        result.pushKV("asks", std::move(asks));
        result.pushKV("bids", std::move(bids));
        return result;
    });
},
    };
}

} // namespace

UniValue SearchBitAssetsJSON(ChainstateManager& chainman, const UniValue& params, const std::vector<bitassets::AssetId>& only)
{
    return SearchAssetsImpl(chainman, params, &only);
}

void RegisterBitAssetsRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"bitassets", &getasset},
        {"bitassets", &getpoolcandles},
        {"bitassets", &listassets},
        {"bitassets", &getassethistory},
        {"bitassets", &getassetlogo},
        {"bitassets", &getbitassetsinfo},
        {"bitassets", &listpools},
        {"bitassets", &getpool},
        {"bitassets", &quoteswap},
        {"bitassets", &listauctions},
        {"bitassets", &getauction},
        {"bitassets", &quotebid},
        {"bitassets", &getdexinfo},
        {"bitassets", &quoteroute},
        {"bitassets", &getpooltwap},
        {"bitassets", &listoracles},
        {"bitassets", &getoraclevalue},
        {"bitassets", &getoraclecontract},
        {"bitassets", &searchassets},
        {"bitassets", &searchpools},
        {"bitassets", &searchauctions},
        {"bitassets", &getassetindexinfo},
        {"bitassets", &getswapquote},
        {"bitassets", &simulateclearing},
        {"bitassets", &previewpool},
        {"bitassets", &getpoolcurve},
        {"bitassets", &decodeassettx},
        {"bitassets", &getlaunch},
        {"bitassets", &listlaunches},
        {"bitassets", &quotelaunch},
        {"bitassets", &getlaunchstats},
        {"bitassets", &getlaunchlog},
        {"bitassets", &quotepoolliquidity},
        {"bitassets", &getposition},
        {"bitassets", &getpoolticks},
        {"bitassets", &previewposition},
        {"bitassets", &getorder},
        {"bitassets", &getbook},
        {"bitassets", &getverifications},
    };
    for (const auto& c : commands) t.appendCommand(c.name, &c);
}
