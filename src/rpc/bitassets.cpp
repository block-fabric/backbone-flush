// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Inspired by BitAssets by LayerTwo Labs (plain-bitassets).

#include <bitassets/dex.h>
#include <bitassets/index.h>
#include <bitassets/logo.h>
#include <bitassets/rpcutil.h>
#include <bitassets/state.h>
#include <coins.h>
#include <common/url.h>
#include <core_io.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <node/context.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <sidechain/state.h>
#include <sync.h>
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
        {RPCResult::Type::STR, "name", /*optional=*/true, "Its name, if its registration published it"},
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
        {RPCResult::Type::NUM, "release_fee", /*optional=*/true, "If releasable: the CHN its pools hold, which retiring it pays to mainchain miners"},
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

std::vector<RPCResult> PoolResults()
{
    return {
        {RPCResult::Type::STR_HEX, "pool", "The id of the pool, which its shares carry"},
        {RPCResult::Type::STR, "asset_a", "The first asset, as asked for (else the lower)"},
        {RPCResult::Type::STR, "asset_b", "The second asset"},
        {RPCResult::Type::STR_HEX, "asset_a_id", "Its hash (null for CHN)"},
        {RPCResult::Type::STR_HEX, "asset_b_id", "Its hash"},
        {RPCResult::Type::NUM, "reserve_a", "What the pool holds of the first asset"},
        {RPCResult::Type::NUM, "reserve_b", "What it holds of the second"},
        {RPCResult::Type::NUM, "price", /*optional=*/true, "What one of the first asset is worth in the second"},
        {RPCResult::Type::NUM, "shares", "The shares there are, in units (a share has no decimals)"},
        {RPCResult::Type::NUM, "volume_a", "All of the first asset ever traded in"},
        {RPCResult::Type::NUM, "volume_b", "All of the second asset ever traded in"},
        {RPCResult::Type::NUM, "swaps", "How many trades it has made"},
        {RPCResult::Type::NUM, "created", "The height of the block that made it"},
        {RPCResult::Type::BOOL, "abandoned", "Whether nobody provides liquidity any more: it holds only the minimum it keeps for good, and trading in it gets almost nothing"},
    };
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
    if (UniValue price{PriceToJSON(reserve_a, da, reserve_b, db)}; !price.isNull()) obj.pushKV("price", std::move(price));
    obj.pushKV("shares", pool.shares);
    obj.pushKV("volume_a", AmountToJSON(volume_a, da));
    obj.pushKV("volume_b", AmountToJSON(volume_b, db));
    obj.pushKV("swaps", pool.swaps);
    obj.pushKV("created", pool.height);
    obj.pushKV("abandoned", bitassets::amm::Abandoned(pool));
    return obj;
}

std::vector<RPCResult> AuctionResults()
{
    return {
        {RPCResult::Type::STR_HEX, "auction", "The transaction that made it, which names it"},
        {RPCResult::Type::STR, "base", "What it sells"},
        {RPCResult::Type::STR, "quote", "What it sells for"},
        {RPCResult::Type::STR_HEX, "base_id", "The hash of what it sells (null for CHN)"},
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
            {"asset", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::ASSET_ARG_HELP},
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
    if (asset.IsNull()) throw JSONRPCError(RPC_INVALID_PARAMETER, "CHN is the coin of the chain, not a registered asset");
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
            {"asset", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::ASSET_ARG_HELP},
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
            {"asset", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::ASSET_ARG_HELP},
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
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::OBJ, "", "", PoolResults()}}},
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
        "The pool of two assets.",
        {
            {"asset_a", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::ASSET_ARG_HELP},
            {"asset_b", RPCArg::Type::STR, RPCArg::Optional::NO, bitassets::ASSET_ARG_HELP},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", PoolResults()},
        RPCExamples{HelpExampleCli("getpool", "\"GOLD\" \"CHN\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    const AssetId a{ParseAsset(request.params[0], state)}, b{ParseAsset(request.params[1], state)};
    const auto pool{state.FindPool(a, b)};
    if (!pool) throw JSONRPCError(RPC_INVALID_PARAMETER, "There is no pool of these assets (addliquidity makes one)");
    return PoolToJSON(state, bitassets::PoolId(a, b), *pool, a);
},
    };
}

RPCMethod quoteswap()
{
    return RPCMethod{
        "quoteswap",
        "What a trade in a pool gives now: for an amount in, what comes out; or for an amount out, what has to go in.",
        {
            {"asset_in", RPCArg::Type::STR, RPCArg::Optional::NO, "What is paid in. " + std::string{bitassets::ASSET_ARG_HELP}},
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
        RPCExamples{HelpExampleCli("quoteswap", "\"CHN\" 1 \"GOLD\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
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
        "Layers' DEX: whether its rules apply to the next block, and its bounds (doc/layers.md, \"The DEX\").",
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
            {"asset_in", RPCArg::Type::STR, RPCArg::Optional::NO, "What is paid in. " + std::string{bitassets::ASSET_ARG_HELP}},
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
            {RPCResult::Type::ARR, "routes", "The routes found, best first",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::ARR, "path", "Its assets", {{RPCResult::Type::STR, "", "An asset's label"}}},
                    {RPCResult::Type::ARR, "path_ids", "The same, by hash (null for CHN)", {{RPCResult::Type::STR_HEX, "", "An asset"}}},
                    {RPCResult::Type::NUM, "amount_out", "What it gives"},
                    {RPCResult::Type::ARR, "hops", "Each pool's part",
                    {
                        {RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::STR_HEX, "pool", "The pool"},
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
    LOCK(::cs_main);
    const bitassets::State state{StateOf(chainman)};
    const AssetId in{ParseAsset(request.params[0], state)}, out{ParseAsset(request.params[2], state)};
    if (in == out) throw JSONRPCError(RPC_INVALID_PARAMETER, "The same asset in and out");
    const uint8_t di{DecimalsOf(state, in)}, dout{DecimalsOf(state, out)};
    const uint64_t amount_in{bitassets::ParseUnits(request.params[1], di)};
    const auto routes{bitassets::dex::FindRoutes(state, in, out, amount_in, max_hops, count)};
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
        UniValue hops(UniValue::VARR);
        for (size_t i{0}; i < route.outs.size(); ++i) {
            UniValue hop(UniValue::VOBJ);
            hop.pushKV("pool", bitassets::PoolId(route.path[i], route.path[i + 1]).GetHex());
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
    result.pushKV("routes", std::move(list));
    return result;
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
    const auto [start0, start1]{bitassets::dex::CumulativeAt(at(lo), from_time)};
    const auto [end0, end1]{bitassets::dex::CumulativeAt(last, to_time)};
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
    result.pushKV("spot_a", PriceToJSON(pool->reserve0, d0, pool->reserve1, d1));
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

size_t ParseLimit(const UniValue& value)
{
    if (value.isNull()) return 50;
    const int limit{value.getInt<int>()};
    if (limit < 1 || static_cast<size_t>(limit) > bitassets::index::MAX_PAGE) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("limit is from 1 to %u", bitassets::index::MAX_PAGE));
    }
    return static_cast<size_t>(limit);
}

/** The outputs of no value of the wallet the request is for (its tokens, if the state has them), for the filter mine. */
std::vector<COutPoint> WalletTokenOutputs(const JSONRPCRequest& request)
{
    const NodeContext& node{EnsureAnyNodeContext(request.context)};
    if (!node.wallet_loader) throw JSONRPCError(RPC_METHOD_NOT_FOUND, "mine needs the wallet (this node has none)");
    std::vector<std::unique_ptr<interfaces::Wallet>> wallets{node.wallet_loader->getWallets()};
    static constexpr std::string_view WALLET_PATH{"/wallet/"};
    std::unique_ptr<interfaces::Wallet> wallet;
    if (request.URI.starts_with(WALLET_PATH)) {
        const std::string name{UrlDecode(std::string_view{request.URI}.substr(WALLET_PATH.size()))};
        for (auto& w : wallets) {
            if (w->getWalletName() == name) wallet = std::move(w);
        }
        if (!wallet) throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Requested wallet does not exist or is not loaded");
    } else if (wallets.size() == 1) {
        wallet = std::move(wallets[0]);
    } else {
        throw JSONRPCError(RPC_WALLET_NOT_SPECIFIED, wallets.empty() ? "mine needs a wallet: none is loaded" :
            "Multiple wallets are loaded. Please select which wallet to use by requesting the RPC through the /wallet/<walletname> URI path.");
    }
    // Bounded by the wallet: its own transactions, once each.
    std::vector<COutPoint> outputs;
    for (const interfaces::WalletTx& wtx : wallet->getWalletTxs()) {
        for (uint32_t n{0}; n < wtx.tx->vout.size(); ++n) {
            if (wtx.tx->vout[n].nValue == 0 && n < wtx.txout_is_mine.size() && wtx.txout_is_mine[n]) outputs.emplace_back(wtx.tx->GetHash(), n);
        }
    }
    return outputs;
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

std::vector<RPCResult> SearchAssetResults()
{
    return {
        {RPCResult::Type::STR_HEX, "asset", "The hash of its name"},
        {RPCResult::Type::STR, "name", /*optional=*/true, "Its name, if public"},
        {RPCResult::Type::STR, "label", "Its name, or \"0x\" and its hash"},
        {RPCResult::Type::STR, "seq", "Its number"},
        {RPCResult::Type::NUM, "decimals", ""},
        {RPCResult::Type::NUM, "supply", "How much there is"},
        {RPCResult::Type::NUM, "minted", "All ever minted"},
        {RPCResult::Type::NUM, "burned", "All ever burned"},
        {RPCResult::Type::BOOL, "fixed", "Whether its supply is fixed for good"},
        {RPCResult::Type::BOOL, "retired", "Whether it was retired (its name may be another asset's now)"},
        {RPCResult::Type::BOOL, "has_logo", ""},
        {RPCResult::Type::BOOL, "has_pool", "Whether it has a pool someone provides liquidity to"},
        {RPCResult::Type::NUM, "pools", "How many"},
        {RPCResult::Type::NUM, "pool_depth", "The CHN in its pool with CHN (0 if none)"},
        {RPCResult::Type::NUM, "volume", "CHN traded in its pools with CHN and its auctions for CHN over the volume window"},
        {RPCResult::Type::NUM, "created", "The height of the block that registered it"},
        {RPCResult::Type::NUM, "last_activity", "The height of the last block that touched it"},
        {RPCResult::Type::STR, "match", /*optional=*/true, "How the text matched: \"exact\", \"prefix\" or \"word\""},
    };
}

UniValue HitToJSON(const bitassets::index::Hit& hit)
{
    using namespace bitassets::index;
    const AssetRow& row{hit.row};
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("asset", row.id.GetHex());
    if (row.flags & NAMED) obj.pushKV("name", row.text);
    obj.pushKV("label", row.flags & NAMED ? row.text : "0x" + row.id.GetHex());
    obj.pushKV("seq", bitassets::FormatSeq(row.seq));
    obj.pushKV("decimals", row.decimals);
    obj.pushKV("supply", AmountToJSON(row.supply, row.decimals));
    obj.pushKV("minted", AmountToJSON(row.minted, row.decimals));
    obj.pushKV("burned", AmountToJSON(row.burned, row.decimals));
    obj.pushKV("fixed", (row.flags & FIXED) != 0);
    obj.pushKV("retired", (row.flags & RETIRED) != 0);
    obj.pushKV("has_logo", (row.flags & HAS_LOGO) != 0);
    obj.pushKV("has_pool", (row.flags & HAS_POOL) != 0);
    obj.pushKV("pools", row.pools);
    obj.pushKV("pool_depth", ValueFromAmount(static_cast<CAmount>(std::min<uint64_t>(row.depth, MAX_MONEY))));
    obj.pushKV("volume", ValueFromAmount(static_cast<CAmount>(std::min<uint64_t>(row.volume, MAX_MONEY))));
    obj.pushKV("created", row.created);
    obj.pushKV("last_activity", row.last);
    if (hit.match >= 0) obj.pushKV("match", hit.match == 0 ? "exact" : hit.match == 1 ? "prefix" : "word");
    return obj;
}

const RPCResult PAGING_RESULTS_NEXT{RPCResult::Type::STR_HEX, "next", /*optional=*/true, "The cursor of the next page; left out when there is no more (a full page always has one: the next may be empty)"};

RPCMethod searchassets()
{
    return RPCMethod{
        "searchassets",
        "Search, filter and sort the assets, a page at a time, by the node's search index (getassetindexinfo): a page\n"
        "never reads the assets whole, however many there are. Go on with the cursor \"next\" a page returns.\n"
        "A text matches the whole name, its start, or the start of any of its words, in any case; \"#\" and a number,\n"
        "a number (\"1739-0029\"), or a hash (\"0x\" and 64 hex digits), that asset only.\n"
        "A search stops at a budget of index entries it looks at; a page cut short by it says \"partial\": go on with\n"
        "\"next\" (filters that few assets pass, along an order that is not theirs, take several calls).",
        {
            {"query", RPCArg::Type::STR, RPCArg::Default{""}, "Text, \"#\" and a number, a number, or a hash"},
            {"filters", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
            {
                {"min_supply", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "At least this supply, in whole units"},
                {"has_pool", RPCArg::Type::BOOL, RPCArg::Default{false}, "Only those with a pool someone provides liquidity to"},
                {"has_logo", RPCArg::Type::BOOL, RPCArg::Default{false}, "Only those with a logo"},
                {"active_within_blocks", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Only those touched in the last this many blocks"},
                {"exclude_dead", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Leave out those not touched in the last this many blocks (the same as active_within_blocks)"},
                {"created_after", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Only those registered after this height"},
                {"exclude_burned", RPCArg::Type::BOOL, RPCArg::Default{false}, "Leave out those with no supply"},
                {"exclude_retired", RPCArg::Type::BOOL, RPCArg::Default{true}, "Leave out the retired ones"},
                {"exclude_keywords", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "Leave out those with any of these words in their names (whole words, any case)",
                    {{"keyword", RPCArg::Type::STR, RPCArg::Optional::OMITTED, ""}}},
                {"mine", RPCArg::Type::BOOL, RPCArg::Default{false}, "Only those the wallet holds or controls, in blocks (the wallet of the request's /wallet/<name>, or the only one loaded)"},
            }},
            {"sort", RPCArg::Type::STR, RPCArg::Default{"relevance"}, "relevance (a text: the whole name, then its start, then a word's start; no text: as activity), "
                "name, newest, oldest, supply, activity (last touched first), volume (traded in the window; those not traded are left out), pool_depth (those with a pool with CHN only)"},
            {"cursor", RPCArg::Type::STR_HEX, RPCArg::Default{""}, "\"next\" of the page before, with the same query, filters and sort"},
            {"limit", RPCArg::Type::NUM, RPCArg::Default{50}, strprintf("How many at most, from 1 to %u", bitassets::index::MAX_PAGE)},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::ARR, "assets", "", {{RPCResult::Type::OBJ, "", "", SearchAssetResults()}}},
            PAGING_RESULTS_NEXT,
            {RPCResult::Type::BOOL, "partial", "The search stopped at its budget before the page was full"},
            {RPCResult::Type::NUM, "scanned", "Index entries looked at"},
            {RPCResult::Type::NUM, "height", "The height of the tip the page is of"},
        }},
        RPCExamples{HelpExampleCli("searchassets", "\"gold\"") + HelpExampleCli("-named searchassets", "query=gold filters='{\"has_pool\":true}' sort=volume limit=20") +
                    HelpExampleCli("searchassets", "\"\" '{\"exclude_dead\":5760,\"exclude_burned\":true}' newest") + HelpExampleRpc("searchassets", "\"gold\", {}, \"supply\", \"\", 100")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    using namespace bitassets::index;
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    Request search;
    const std::string query{request.params[0].isNull() ? "" : request.params[0].get_str()};
    const std::string sort_name{request.params[2].isNull() ? "relevance" : request.params[2].get_str()};
    const auto sort{ParseSort(sort_name)};
    if (!sort) throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown sort: " + sort_name);
    search.sort = *sort;
    search.cursor = ParseCursor(request.params[3]);
    search.limit = ParseLimit(request.params[4]);
    const UniValue& filters{request.params[1]};
    std::optional<int> within;
    bool mine{false};
    if (!filters.isNull()) {
        for (const std::string& key : filters.getKeys()) {
            const UniValue& v{filters[key]};
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
                for (const UniValue& word : v.get_array().getValues()) {
                    for (std::string w : Words(Normalize(word.get_str()))) search.filters.exclude_keywords.push_back(std::move(w));
                }
                if (search.filters.exclude_keywords.size() > 100) throw JSONRPCError(RPC_INVALID_PARAMETER, "At most 100 keywords");
            } else if (key == "mine") {
                mine = v.get_bool();
            } else {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown filter: " + key);
            }
        }
    }
    const std::vector<COutPoint> wallet_outputs{mine ? WalletTokenOutputs(request) : std::vector<COutPoint>{}};
    return WithIndexView(chainman, [&](const sidechain::StoreView& view, int height) {
        const bitassets::State state{view};
        if (within) search.filters.min_last = height - *within + 1;
        if (mine) {
            std::set<uint32_t> seqs;
            for (const COutPoint& outpoint : wallet_outputs) {
                const auto token{state.GetToken(outpoint)};
                if (!token || (token->kind != bitassets::Token::Kind::ASSET && token->kind != bitassets::Token::Kind::CONTROL)) continue;
                if (const auto record{state.GetAsset(token->id)}) seqs.insert(record->seq);
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
        UniValue result(UniValue::VOBJ);
        UniValue assets(UniValue::VARR);
        for (const Hit& hit : page.hits) assets.push_back(HitToJSON(hit));
        result.pushKV("assets", std::move(assets));
        if (!page.next.empty()) result.pushKV("next", HexStr(page.next));
        result.pushKV("partial", page.partial);
        result.pushKV("scanned", page.scanned);
        result.pushKV("height", height);
        return result;
    });
},
    };
}

/** The assets a text names in a search of pools or auctions: those it matches by relevance, at most CANDIDATE_CAP. */
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
    std::vector<AssetId> assets;
    // At most 1000: their pools or auctions are sorted in memory.
    while (assets.size() < 1000) {
        search.limit = MAX_PAGE;
        const Page page{Search(view, search)};
        for (const Hit& hit : page.hits) assets.push_back(hit.row.id);
        if (page.next.empty()) return assets;
        search.cursor = page.next;
    }
    partial = true;
    return assets;
}

RPCMethod searchpools()
{
    return RPCMethod{
        "searchpools",
        "Search and sort the pools, a page at a time, by the node's search index. A text matches the names of their assets as\n"
        "searchassets matches them (the pools of the first 1000 assets it matches).",
        {
            {"query", RPCArg::Type::STR, RPCArg::Default{""}, "Text, a number or a hash, matched against either asset"},
            {"filters", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
            {
                {"exclude_abandoned", RPCArg::Type::BOOL, RPCArg::Default{true}, "Leave out those nobody provides liquidity to"},
                {"chn_only", RPCArg::Type::BOOL, RPCArg::Default{false}, "Only pools with CHN"},
                {"min_depth", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "At least this much CHN in the pool"},
            }},
            {"sort", RPCArg::Type::STR, RPCArg::Default{"depth"}, "depth (CHN held; pools with CHN only), volume (CHN traded in the window; those traded only), "
                "price_change (of the second asset in the first, CHN for a pool with CHN, over the window; largest rise first), newest"},
            {"cursor", RPCArg::Type::STR_HEX, RPCArg::Default{""}, "\"next\" of the page before"},
            {"limit", RPCArg::Type::NUM, RPCArg::Default{50}, strprintf("From 1 to %u", bitassets::index::MAX_PAGE)},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::ARR, "pools", "", {{RPCResult::Type::OBJ, "", "", [] {
                std::vector<RPCResult> results{PoolResults()};
                results.push_back({RPCResult::Type::NUM, "depth", "The CHN it holds (0 for a pool without CHN)"});
                results.push_back({RPCResult::Type::NUM, "volume_window", "CHN traded over the volume window"});
                results.push_back({RPCResult::Type::NUM, "price_change", "The change of the price of asset_b in asset_a over the window, in percent"});
                results.push_back({RPCResult::Type::NUM, "last_activity", "The height of the last block that touched it"});
                return results;
            }()}}},
            PAGING_RESULTS_NEXT,
            {RPCResult::Type::BOOL, "partial", "The search stopped at a budget: go on with next"},
            {RPCResult::Type::NUM, "scanned", "Entries looked at"},
            {RPCResult::Type::NUM, "height", "The height of the tip"},
        }},
        RPCExamples{HelpExampleCli("searchpools", "") + HelpExampleCli("searchpools", "\"gold\" '{}' volume")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    using namespace bitassets::index;
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    PoolRequest search;
    const std::string query{request.params[0].isNull() ? "" : request.params[0].get_str()};
    const std::string sort_name{request.params[2].isNull() ? "depth" : request.params[2].get_str()};
    const auto sort{ParsePoolSort(sort_name)};
    if (!sort) throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown sort: " + sort_name);
    search.sort = *sort;
    search.cursor = ParseCursor(request.params[3]);
    search.limit = ParseLimit(request.params[4]);
    if (const UniValue& filters{request.params[1]}; !filters.isNull()) {
        for (const std::string& key : filters.getKeys()) {
            const UniValue& v{filters[key]};
            if (key == "exclude_abandoned") {
                search.exclude_abandoned = v.get_bool();
            } else if (key == "chn_only") {
                search.chn_only = v.get_bool();
            } else if (key == "min_depth") {
                search.min_depth = static_cast<uint64_t>(AmountFromValue(v));
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
        if (!page.next.empty()) result.pushKV("next", HexStr(page.next));
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
            {"filters", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
            {
                {"status", RPCArg::Type::STR, RPCArg::Default{"active"}, "open, upcoming, active (open or upcoming), ended (not collected yet) or any (not closed)"},
                {"include_closed", RPCArg::Type::BOOL, RPCArg::Default{false}, "Also those collected (sort newest, no text)"},
            }},
            {"sort", RPCArg::Type::STR, RPCArg::Default{"ending_soon"}, "ending_soon (by last block) or newest"},
            {"cursor", RPCArg::Type::STR_HEX, RPCArg::Default{""}, "\"next\" of the page before"},
            {"limit", RPCArg::Type::NUM, RPCArg::Default{50}, strprintf("From 1 to %u", bitassets::index::MAX_PAGE)},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::ARR, "auctions", "", {{RPCResult::Type::OBJ, "", "", AuctionResults()}}},
            PAGING_RESULTS_NEXT,
            {RPCResult::Type::BOOL, "partial", "The search stopped at a budget: go on with next"},
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
    const auto sort{ParseAuctionSort(sort_name)};
    if (!sort) throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown sort: " + sort_name);
    search.sort = *sort;
    search.cursor = ParseCursor(request.params[3]);
    search.limit = ParseLimit(request.params[4]);
    if (const UniValue& filters{request.params[1]}; !filters.isNull()) {
        for (const std::string& key : filters.getKeys()) {
            const UniValue& v{filters[key]};
            if (key == "status") {
                search.status = v.get_str();
                if (search.status != "open" && search.status != "upcoming" && search.status != "active" && search.status != "ended" && search.status != "any") {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown status: " + search.status);
                }
            } else if (key == "include_closed") {
                search.include_closed = v.get_bool();
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
        if (!page.next.empty()) result.pushKV("next", HexStr(page.next));
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
        if (now) auctions.push_back(AuctionToJSON(chainman.ActiveChainstate(), state, id, *now, height));
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

} // namespace

void RegisterBitAssetsRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"bitassets", &getasset},
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
    };
    for (const auto& c : commands) t.appendCommand(c.name, &c);
}
