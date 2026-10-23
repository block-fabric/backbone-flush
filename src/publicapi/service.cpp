// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <publicapi/service.h>

#include <addresstype.h>
#include <bitassets/curves.h>
#include <bitassets/dex.h>
#include <bitassets/index.h>
#include <bitassets/logo.h>
#include <bitassets/market.h>
#include <bitassets/rpcutil.h>
#include <bitassets/state.h>
#include <chain.h>
#include <chainparams.h>
#include <common/messages.h>
#include <consensus/consensus.h>
#include <node/types.h>
#include <policy/feerate.h>
#include <consensus/amount.h>
#include <core_io.h>
#include <grpc/standard.h>
#include <key_io.h>
#include <logging.h>
#include <node/context.h>
#include <node/transaction.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <publicapi/build.h>
#include <publicapi/limits.h>
#include <rpc/protocol.h>
#include <rpc/request.h>
#include <rpc/server.h>
#include <sidechain/mainchain.h>
#include <sidechain/store.h>
#include <sync.h>
#include <tinyformat.h>
#include <txmempool.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/time.h>
#include <validation.h>

#include <univalue.h>

#include <algorithm>
#include <cmath>
#include <set>

using bitassets::AssetId;
using grpcsrv::Code;
using grpcsrv::Status;
using pb::Message;

namespace publicapi {

namespace {

/** A refusal or failure of a call, as the client sees it. */
struct ApiError : std::runtime_error {
    Status status;
    ApiError(Code code, const std::string& message) : std::runtime_error{message}, status{code, message} {}
};

[[noreturn]] void Fail(Code code, const std::string& message) { throw ApiError{code, message}; }

//! The largest transaction Broadcast takes, in bytes (its hex: twice that).
constexpr size_t MAX_BROADCAST_BYTES{100'000};

/** The node's own RPC methods the API may call: the DEX's reads and quotes, never a wallet's. */
const std::set<std::string> NODE_METHODS{
    "getpool", "searchpools", "getswapquote", "quoteroute", "quotelaunch", "quotepoolliquidity", "previewposition",
    "getbook", "getposition", "getpoolticks", "getverifications", "getlaunchstats", "getlaunchlog", "getpeginfo", "searchassets",
};

/** An error of an RPC method, as a status. */
Status FromRpcError(const UniValue& error)
{
    const int code{error.exists("code") && error["code"].isNum() ? error["code"].getInt<int>() : RPC_MISC_ERROR};
    std::string message{error.exists("message") && error["message"].isStr() ? error["message"].get_str() : "error"};
    switch (code) {
    case RPC_INVALID_PARAMETER:
    case RPC_TYPE_ERROR:
    case RPC_INVALID_ADDRESS_OR_KEY:
    case RPC_DESERIALIZATION_ERROR:
        if (message.starts_with("No such") || message.starts_with("Not an asset on a curve") || message.starts_with("There is no pool")) return {Code::NOT_FOUND, message};
        return {Code::INVALID_ARGUMENT, message};
    case RPC_IN_WARMUP: return {Code::UNAVAILABLE, "the node is starting: " + message};
    case RPC_MISC_ERROR: return {Code::FAILED_PRECONDITION, message};
    default: return {Code::INTERNAL, message};
    }
}

/** What a call reads: the tip, and the state as of it (a snapshot, read without cs_main). */
struct Tip {
    uint256 hash;
    int height{-1};
    uint32_t time{0};
    std::shared_ptr<const sidechain::StoreView> view;
    bitassets::State State() const { return bitassets::State{*view}; }
};

double Whole(uint64_t units, int decimals) { return static_cast<double>(units) / std::pow(10.0, decimals); }

std::string Hex(const uint256& id) { return id.GetHex(); }

std::string AddressOf(const CScript& script)
{
    CTxDestination dest;
    if (ExtractDestination(script, dest)) return EncodeDestination(dest);
    return script.empty() ? "" : "script:" + HexStr(script).substr(0, 128);
}

/** An exact amount from a decimal the node's methods wrote (AmountToJSON: plain decimals). */
uint64_t UnitsOf(const UniValue& value, int decimals)
{
    int64_t units{0};
    if (!ParseFixedPoint(value.getValStr(), decimals, &units) || units < 0) Fail(Code::INTERNAL, "unexpected amount " + value.getValStr());
    return static_cast<uint64_t>(units);
}

/** An amount a client gave, in units of `decimals`: a plain decimal, no sign, no exponent, not 0. */
uint64_t ParseAmount(const std::string& text, int decimals, const char* what)
{
    int64_t units{0};
    if (text.empty() || text.size() > 40 || text.find_first_not_of("0123456789.") != std::string::npos || !ParseFixedPoint(text, decimals, &units) || units <= 0) {
        Fail(Code::INVALID_ARGUMENT, strprintf("%s: a positive decimal of at most %d places", what, decimals));
    }
    return static_cast<uint64_t>(units);
}

uint32_t Slippage(uint32_t given)
{
    if (given == 0) return 100;
    if (given > 5000) Fail(Code::INVALID_ARGUMENT, "slippage_bps: at most 5000");
    return given;
}

/** What is asked at least: the amount less the slippage, rounded down, then to what a link's decimal can say. */
uint64_t Floor(uint64_t amount, uint32_t slippage_bps, int decimals)
{
    const uint64_t floor{static_cast<uint64_t>(static_cast<unsigned __int128>(amount) * (10'000 - slippage_bps) / 10'000)};
    return FloorUnits(floor, decimals);
}

std::string LinkLabel(const std::string& url)
{
    std::string host{url.substr(8)};
    host = host.substr(0, host.find_first_of("/?#"));
    host = ToLower(host);
    if (host.starts_with("www.")) host.erase(0, 4);
    if (host == "x.com" || host == "twitter.com") return "X";
    if (host == "t.me" || host == "telegram.me") return "Telegram";
    if (host == "discord.gg" || host == "discord.com") return "Discord";
    if (host == "github.com") return "GitHub";
    return "Website";
}

const std::string& Coin()
{
    return CURRENCY_UNIT;
}

} // namespace

bool SafeLink(const std::string& url)
{
    if (url.size() > 512 || !url.starts_with("https://")) return false;
    for (const unsigned char c : url) {
        if (c <= 0x20 || c == 0x7f || c == '"' || c == '<' || c == '>' || c == '\\' || c == '`' || c >= 0x80) return false;
    }
    const std::string rest{url.substr(8)};
    const std::string authority{rest.substr(0, rest.find_first_of("/?#"))};
    if (authority.empty() || authority.find('@') != std::string::npos) return false;
    const std::string host{authority.substr(0, authority.rfind(':') == std::string::npos || authority.starts_with('[') ? authority.size() : authority.rfind(':'))};
    if (host.empty() || host.front() == '.' || host.back() == '.') return false;
    return std::all_of(host.begin(), host.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '[' || c == ']' || c == ':'; });
}

CreatorText SplitInfo(const std::string& info)
{
    CreatorText out;
    std::vector<std::string> lines;
    for (const std::string& raw : util::SplitString(info, '\n')) {
        // Plain text: control characters out (a tab or a carriage return would be one).
        std::string line;
        for (const unsigned char c : raw) {
            if (c >= 0x20 && c != 0x7f) line.push_back(static_cast<char>(c));
        }
        line = util::TrimString(line);
        if (line.empty()) continue;
        if (line.find(' ') == std::string::npos && SafeLink(line) && out.links.size() < 4) {
            out.links.emplace_back(LinkLabel(line), line);
        } else {
            lines.push_back(line);
        }
    }
    out.description = util::Join(lines, "\n");
    return out;
}

double CostOf(const std::string& method_path)
{
    const std::string method{method_path.substr(method_path.rfind('/') + 1)};
    if (method.starts_with("Quote") || method == "BuildAction") return 4;
    if (method == "ListLaunches" || method == "ListPools" || method == "ListAssets" || method == "GetAsset" || method == "GetVerifications" || method == "GetHoldings" || method == "GetLaunchLog") return 2;
    if (method == "ServerReflectionInfo") return 2;
    return 1;
}

namespace {

class ServiceImpl final : public Service
{
public:
    ServiceImpl(grpcsrv::Server& server, const pb::Pool& pool, node::NodeContext& node, const Options& options)
        : m_pool{pool}, m_node{node}, m_options{options}, m_limits{options.rate, options.burst},
          m_broadcast_limits{options.broadcast_per_minute / 60.0, options.broadcast_burst},
          m_broadcast_global{options.broadcast_global_per_minute / 60.0, std::max(1.0, options.broadcast_global_per_minute / 6.0), 1},
          m_cache{options.cache_entries, options.cache_bytes}, m_gate{options.max_expensive}
    {
        RegisterAll(server);
        grpcsrv::RegisterHealth(server, pool, {SERVICE});
        if (options.reflection) grpcsrv::RegisterReflection(server, pool, {SERVICE});
    }

    std::optional<Status> Guard(const grpcsrv::CallInfo& call) override
    {
        const double cost{CostOf(call.path)};
        if (const auto wait{m_limits.Take(call.client, cost)}) {
            const int seconds{std::max(1, static_cast<int>(std::ceil(*wait)))};
            return Status{Code::RESOURCE_EXHAUSTED, strprintf("rate limit: %g calls a second at most (try again in %d s)", m_options.rate, seconds), {{"retry-after", util::ToString(seconds)}}};
        }
        if (call.path == std::string{"/"} + SERVICE + "/Broadcast") {
            if (!m_options.broadcast) return Status{Code::PERMISSION_DENIED, "this node does not broadcast for others (-publicapibroadcast=0)"};
            if (const auto wait{m_broadcast_limits.Take(call.client, 1)}) {
                const int seconds{std::max(1, static_cast<int>(std::ceil(*wait)))};
                return Status{Code::RESOURCE_EXHAUSTED, strprintf("broadcast limit: %g a minute (try again in %d s)", m_options.broadcast_per_minute, seconds), {{"retry-after", util::ToString(seconds)}}};
            }
            if (const auto wait{m_broadcast_global.Take("*", 1)}) {
                const int seconds{std::max(1, static_cast<int>(std::ceil(*wait)))};
                return Status{Code::RESOURCE_EXHAUSTED, "this node broadcasts no more for now: try again shortly", {{"retry-after", util::ToString(seconds)}}};
            }
        }
        return std::nullopt;
    }

private:
    // -----------------------------------------------------------------------------------------------------
    // Plumbing

    struct Policy {
        //! Replies cached per tip (the same request at the same tip: the same reply).
        bool cache{true};
        //! Taken through the gate of expensive calls.
        bool expensive{false};
        std::string cache_control{"public, max-age=5"};
    };

    using Fn = std::function<Message(const Message& req, const Tip& tip)>;

    Message New(const std::string& name) const
    {
        const pb::MessageDesc* desc{m_pool.FindMessage("flush.dex.v1." + name)};
        if (!desc) throw std::logic_error("no message " + name);
        return Message{*desc};
    }

    Tip TakeTip() const
    {
        ChainstateManager& chainman{*Assert(m_node.chainman)};
        LOCK(::cs_main);
        Chainstate& chainstate{chainman.ActiveChainstate()};
        const CBlockIndex* tip{chainstate.m_chain.Tip()};
        if (!tip) Fail(Code::UNAVAILABLE, "the node is starting");
        Tip t;
        t.hash = tip->GetBlockHash();
        t.height = tip->nHeight;
        t.time = static_cast<uint32_t>(tip->GetBlockTime());
        t.view = chainstate.SideCache().SharedSnapshot();
        if (!t.view) {
            std::unique_ptr<sidechain::StoreView> copy{chainstate.SideCache().Snapshot()};
            t.view = std::move(copy);
        }
        if (!t.view) Fail(Code::UNAVAILABLE, "the state cannot be read now");
        return t;
    }

    UniValue Rpc(const std::string& method, UniValue params) const
    {
        if (!NODE_METHODS.contains(method)) throw std::logic_error("not a method the public API calls: " + method);
        JSONRPCRequest request;
        request.context = &m_node;
        request.strMethod = method;
        request.params = std::move(params);
        request.URI = "/publicapi";
        try {
            return tableRPC.execute(request);
        } catch (const UniValue& error) {
            const Status status{FromRpcError(error)};
            throw ApiError{status.code, status.message};
        }
    }

    void Serve(grpcsrv::Server& server, const std::string& method, Policy policy, Fn fn)
    {
        if (!server.Register(SERVICE, method, [this, method, policy, fn = std::move(fn)](const Message& req, const std::shared_ptr<grpcsrv::Writer>& w) {
                try {
                    const Tip tip{TakeTip()};
                    const std::string key{method + '\0' + req.Serialize()};
                    std::shared_ptr<const Message> reply;
                    if (policy.cache) reply = m_cache.Get(key, tip.hash);
                    if (!reply) {
                        std::optional<Gate::Pass> pass;
                        if (policy.expensive) {
                            pass.emplace(m_gate.Enter());
                            if (!*pass) return w->Finish(Status{Code::RESOURCE_EXHAUSTED, "busy: try again in a moment", {{"retry-after", "1"}}});
                        }
                        auto made{std::make_shared<const Message>(fn(req, tip))};
                        if (policy.cache) m_cache.Put(key, tip.hash, made, made->Serialize().size());
                        reply = std::move(made);
                    }
                    w->SetHeader("cache-control", policy.cache_control);
                    w->Write(*reply);
                    w->Finish({});
                } catch (const ApiError& e) {
                    w->Finish(e.status);
                } catch (const UniValue& e) {
                    w->Finish(FromRpcError(e));
                } catch (const std::invalid_argument& e) {
                    w->Finish({Code::INVALID_ARGUMENT, e.what()});
                } catch (const std::exception& e) {
                    LogWarning("public API: %s: %s", method, e.what());
                    w->Finish({Code::INTERNAL, "internal error"});
                }
            })) {
            throw std::logic_error("no method " + method);
        }
    }

    // -----------------------------------------------------------------------------------------------------
    // Assets

    static uint8_t DecimalsOf(const bitassets::State& state, const AssetId& asset)
    {
        if (asset.IsNull()) return bitassets::CHN_DECIMALS;
        const auto record{state.GetAsset(asset)};
        return record ? record->decimals : 0;
    }

    static std::string Label(const bitassets::State& state, const AssetId& asset)
    {
        if (asset.IsNull()) return Coin();
        const auto record{state.GetAsset(asset)};
        return bitassets::AssetLabel(asset, record ? &*record : nullptr);
    }

    /** An asset a client names: ECX, a ticker or name (any case for a ticker), 64 hex digits, or its number. */
    static AssetId Resolve(const bitassets::State& state, const std::string& text)
    {
        if (text.empty() || text.size() > 128) Fail(Code::INVALID_ARGUMENT, "name an asset: " + CURRENCY_UNIT + ", a ticker, or its id");
        if (text == "ECX" || text == "ecx" || text == "CHN") return AssetId{};
        if (text.size() == 64 && IsHex(text)) return AssetId{*uint256::FromHex(ToLower(text))};
        if (bitassets::IsTicker(ToUpper(text))) {
            const AssetId id{bitassets::HashName(ToUpper(text))};
            if (state.GetAsset(id)) return id;
        }
        try {
            return bitassets::ParseAssetArg(UniValue{text}, [&](uint32_t seq) { return state.AssetOfSeq(seq); });
        } catch (const UniValue&) {
            Fail(Code::INVALID_ARGUMENT, "no such asset: " + text.substr(0, 64));
        }
    }

    static AssetId ResolveRegistered(const bitassets::State& state, const std::string& text)
    {
        const AssetId id{Resolve(state, text)};
        if (!id.IsNull() && !state.GetAsset(id)) Fail(Code::NOT_FOUND, "no such asset: " + text.substr(0, 64));
        return id;
    }

    /** A launch a client names: its ticker (any case) or its token's id. */
    static std::pair<AssetId, bitassets::LaunchRecord> ResolveLaunch(const bitassets::State& state, const std::string& text)
    {
        const AssetId id{Resolve(state, text)};
        const auto launch{id.IsNull() ? std::nullopt : state.GetLaunch(id)};
        if (!launch) Fail(Code::NOT_FOUND, "no such launch: " + text.substr(0, 64));
        return {id, *launch};
    }

    static std::string ArgOf(const AssetId& asset) { return asset.IsNull() ? CURRENCY_UNIT : "0x" + asset.GetHex(); }

    static std::optional<std::string> LogoUrl(const bitassets::State& state, const AssetId& asset, bool launch)
    {
        if (asset.IsNull() || !state.GetLogo(asset)) return std::nullopt;
        return strprintf("/api/v1/%s/%s/logo", launch ? "launches" : "assets", asset.GetHex());
    }

    // -----------------------------------------------------------------------------------------------------
    // Markets: 24 hours of a market, from its hourly candles

    struct Window {
        double volume{0};
        uint32_t trades{0};
        std::optional<double> change;
        std::optional<double> price;
    };

    static Window Day(const sidechain::StoreView& view, const bitassets::market::Market& market, uint32_t now)
    {
        using namespace bitassets::market;
        Window w;
        if (!Enabled()) return w;
        const uint32_t day_ago{now > 86400 ? now - 86400 : 0};
        const auto candles{Candles(view, market, Interval::H1, day_ago > 3600 ? day_ago - 3600 : 0, now, 26)};
        std::optional<double> before;
        for (const CandleRow& c : candles) {
            if (c.t + 3600 <= day_ago) {
                before = c.c;
                continue;
            }
            if (c.t >= day_ago - day_ago % 3600) {
                w.volume += c.v;
                w.trades += c.n;
            }
            if (!before && c.t <= day_ago) before = c.o;
        }
        if (const auto row{GetMarket(view, market)}) w.price = row->price;
        if (before && w.price && *before > 0) w.change = 100.0 * (*w.price / *before - 1.0);
        return w;
    }

    // -----------------------------------------------------------------------------------------------------
    // The launches, read once per tip

    struct LaunchInfo {
        AssetId token;
        bitassets::LaunchRecord rec;
        std::string ticker, name, info;
        bool logo{false};
        uint32_t created_time{0};
        double raised{0}, sold{0}, price{0}, mcap{0}, progress{0}, curve_left{0}, volume_total{0};
        int decimals{bitassets::LAUNCH_DECIMALS};
        Window day;
        uint64_t holders{0};
        uint256 pool;
    };
    struct LaunchTable {
        uint256 tip;
        std::vector<LaunchInfo> launches; // newest first
        std::map<AssetId, size_t> by_token;
    };

    std::shared_ptr<const LaunchTable> Launches(const Tip& tip) const
    {
        LOCK(m_table_mutex);
        if (m_table && m_table->tip == tip.hash) return m_table;
        auto table{std::make_shared<LaunchTable>()};
        table->tip = tip.hash;
        const bitassets::State state{tip.State()};
        std::vector<std::pair<int32_t, AssetId>> tokens;
        state.ForEachLaunch([&](int32_t height, const AssetId& token) {
            tokens.emplace_back(height, token);
            return true;
        });
        std::stable_sort(tokens.begin(), tokens.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::map<int32_t, uint32_t> times;
        {
            ChainstateManager& chainman{*Assert(m_node.chainman)};
            LOCK(::cs_main);
            const CChain& chain{chainman.ActiveChain()};
            for (const auto& [height, _] : tokens) {
                if (const CBlockIndex* block{chain[height]}) times[height] = static_cast<uint32_t>(block->GetBlockTime());
            }
        }
        for (const auto& [height, token] : tokens) {
            const auto rec{state.GetLaunch(token)};
            if (!rec) continue;
            LaunchInfo l;
            l.token = token;
            l.rec = *rec;
            const auto record{state.GetAsset(token)};
            l.decimals = record ? record->decimals : bitassets::LAUNCH_DECIMALS;
            const double whole{std::pow(10.0, l.decimals)};
            l.ticker = record && !record->text.empty() ? record->text : "0x" + token.GetHex();
            const auto display{state.GetDisplayName(token)};
            l.name = display ? display->text : l.ticker;
            if (record && record->data.info) l.info = *record->data.info;
            l.logo = state.GetLogo(token).has_value();
            l.created_time = times.contains(rec->height) ? times.at(rec->height) : 0;
            l.pool = rec->pool;
            const auto pool{state.GetPool(rec->pool)};
            uint64_t raised{rec->raised_at_graduation}, left{0};
            if (pool && !rec->Graduated()) {
                raised = pool->reserve0 - std::min(pool->reserve0, rec->virtual_coin);
                left = pool->reserve1 - std::min(pool->reserve1, rec->CurveEnd());
            }
            if (pool && pool->reserve1 > 0) l.price = (static_cast<double>(pool->reserve0) / COIN) / (static_cast<double>(pool->reserve1) / whole);
            l.raised = static_cast<double>(raised) / COIN;
            l.curve_left = static_cast<double>(left) / whole;
            const uint64_t curve_supply{rec->CurveSupply()};
            l.sold = rec->Graduated() ? static_cast<double>(curve_supply) / whole : static_cast<double>(curve_supply - std::min(left, curve_supply)) / whole;
            const uint64_t supply{record ? record->supply : rec->Supply()};
            l.mcap = l.price * (static_cast<double>(supply) / whole);
            l.progress = rec->Graduated() ? 100.0 : std::min(100.0, 100.0 * static_cast<double>(raised) / static_cast<double>(std::max<uint64_t>(rec->graduation, 1)));
            l.volume_total = static_cast<double>(rec->volume) / COIN;
            l.day = Day(*tip.view, bitassets::market::Market{true, token}, tip.time);
            l.holders = bitassets::market::Enabled() ? bitassets::market::HolderCount(*tip.view, token, bitassets::Token::Kind::ASSET) : 0;
            table->by_token[token] = table->launches.size();
            table->launches.push_back(std::move(l));
        }
        m_table = table;
        return table;
    }

    void FillLaunch(Message& m, const LaunchInfo& l, const bitassets::State& state) const
    {
        m.SetString("id", Hex(l.token));
        m.SetString("ticker", l.ticker);
        m.SetString("name", l.name);
        if (l.logo) m.SetString("logo", strprintf("/api/v1/launches/%s/logo", l.token.GetHex()));
        m.SetString("creator", AddressOf(l.rec.creator));
        m.SetUInt("created_height", static_cast<uint32_t>(l.rec.height));
        m.SetUInt("created_time", l.created_time);
        m.SetString("state", l.rec.Graduated() ? "graduated" : "curve");
        m.SetDouble("raised_ecx", l.raised);
        m.SetDouble("sold", l.sold);
        m.SetDouble("price_ecx", l.price);
        m.SetDouble("mcap_ecx", l.mcap);
        m.SetDouble("volume_24h", l.day.volume);
        m.SetUInt("trades_24h", l.day.trades);
        m.SetUInt("holders", l.holders);
        if (l.day.change) m.SetDouble("change_24h", *l.day.change);
        if (l.rec.Graduated()) {
            m.SetUInt("graduated_height", static_cast<uint32_t>(l.rec.graduated));
            Message& ref{m.Mutable("pool")};
            ref.SetString("pool", Hex(l.rec.pool));
            ref.SetString("asset_a", l.ticker);
            ref.SetString("asset_b", Coin());
        }
        m.SetDouble("progress", l.progress);
        m.SetString("pool_id", Hex(l.rec.pool));
        m.SetDouble("volume_total", l.volume_total);
        m.SetUInt("trades_total", l.rec.trades);
        m.SetUInt("decimals", static_cast<uint32_t>(l.decimals));
        m.SetDouble("curve_supply", Whole(l.rec.CurveSupply(), l.decimals));
        m.SetDouble("pool_supply", Whole(l.rec.PoolSupply(), l.decimals));
        m.SetBool("from_asset", l.rec.from_asset);
        m.SetDouble("fee_bps", l.rec.fee / 100.0);
        (void)state;
    }

    /** A launch's own curve: its supply and decimals, its virtual reserves. */
    Message CurveOfLaunch(const bitassets::LaunchRecord& rec, int decimals) const
    {
        Message c{New("CurveParams")};
        c.SetDouble("supply", Whole(rec.Supply(), decimals));
        c.SetDouble("sale_supply", Whole(rec.CurveSupply(), decimals));
        c.SetDouble("pool_supply", Whole(rec.PoolSupply(), decimals));
        c.SetDouble("virtual_ecx", static_cast<double>(rec.virtual_coin) / COIN);
        c.SetDouble("virtual_tokens", Whole(rec.VirtualUnits(), decimals));
        c.SetDouble("graduation_ecx", static_cast<double>(rec.graduation) / COIN);
        c.SetUInt("decimals", static_cast<uint32_t>(decimals));
        c.SetDouble("max_allocation_percent", bitassets::MAX_LAUNCH_ALLOCATION / 100.0);
        return c;
    }

    Message CurveOf(uint64_t graduation, uint32_t fee) const
    {
        Message c{New("CurveParams")};
        const double whole{std::pow(10.0, bitassets::LAUNCH_DECIMALS)};
        c.SetDouble("supply", bitassets::LAUNCH_SUPPLY / whole);
        c.SetDouble("sale_supply", bitassets::LAUNCH_CURVE_SUPPLY / whole);
        c.SetDouble("pool_supply", bitassets::LAUNCH_POOL_SUPPLY / whole);
        c.SetDouble("virtual_ecx", static_cast<double>(graduation / 2) / COIN);
        c.SetDouble("virtual_tokens", bitassets::LAUNCH_VIRTUAL_TOKENS / whole);
        c.SetDouble("graduation_ecx", static_cast<double>(graduation) / COIN);
        c.SetUInt("decimals", bitassets::LAUNCH_DECIMALS);
        c.SetDouble("max_allocation_percent", bitassets::MAX_LAUNCH_ALLOCATION / 100.0);
        (void)fee;
        return c;
    }

    /** A launch's fee and its parts (millionths of the fee): the creator's, the treasury's, the chain's security, the rest to the pool. */
    Message FeesOf(uint32_t launch_fee, uint32_t creator_share, uint32_t protocol_share, uint32_t security_share) const
    {
        Message f{New("Fees")};
        f.SetDouble("trade_bps", launch_fee / 100.0);
        const auto part{[&](const char* to, uint32_t share) {
            if (share == 0) return;
            Message& m{f.Add("split")};
            m.SetString("to", to);
            m.SetDouble("bps", share / 100.0);
        }};
        part("creator", creator_share);
        part("treasury", protocol_share);
        part("security", security_share);
        const uint32_t used{creator_share + protocol_share + security_share};
        part("pool", used < bitassets::FEE_DENOMINATOR ? bitassets::FEE_DENOMINATOR - used : 0);
        f.SetDouble("pool_fee_bps", bitassets::LEGACY_FEE / 100.0);
        f.SetDouble("launch_fee_ecx", 0);
        return f;
    }
    Message RulesFees() const
    {
        const Consensus::SidechainParams& p{Params().GetConsensus().sidechain};
        return FeesOf(p.launch_fee, p.launch_creator_share, p.launch_protocol_share, p.launch_security_share);
    }

    // -----------------------------------------------------------------------------------------------------
    // Pools

    /** A pool as getpool / searchpools write it, with the market index's measures. */
    Message PoolFromJson(const UniValue& j, const Tip& tip) const
    {
        const bitassets::State state{tip.State()};
        Message p{New("Pool")};
        const uint256 id{*uint256::FromHex(j["pool"].get_str())};
        p.SetString("pool", j["pool"].get_str());
        const auto id_of{[](const UniValue& v) -> AssetId { return v.isStr() ? AssetId{*uint256::FromHex(v.get_str())} : AssetId{}; }};
        const AssetId a{id_of(j["asset_a_id"])}, b{id_of(j["asset_b_id"])};
        const uint8_t da{DecimalsOf(state, a)}, db{DecimalsOf(state, b)};
        p.SetString("asset_a", a.IsNull() ? Coin() : j["asset_a"].get_str());
        p.SetString("asset_b", b.IsNull() ? Coin() : j["asset_b"].get_str());
        if (!a.IsNull()) p.SetString("asset_a_id", Hex(a));
        if (!b.IsNull()) p.SetString("asset_b_id", Hex(b));
        const uint64_t ra{UnitsOf(j["reserve_a"], da)}, rb{UnitsOf(j["reserve_b"], db)};
        p.SetDouble("reserve_a", Whole(ra, da));
        p.SetDouble("reserve_b", Whole(rb, db));
        p.SetString("reserve_a_units", util::ToString(ra));
        p.SetString("reserve_b_units", util::ToString(rb));
        if (j.exists("price") && j["price"].isNum()) p.SetDouble("price", j["price"].get_real());
        const uint64_t shares{j["shares"].getInt<uint64_t>()};
        p.SetDouble("shares", static_cast<double>(shares));
        p.SetString("shares_units", util::ToString(shares));
        p.SetDouble("volume_a", j["volume_a"].get_real());
        p.SetDouble("volume_b", j["volume_b"].get_real());
        p.SetUInt("swaps", j["swaps"].getInt<uint64_t>());
        p.SetUInt("created", j["created"].getInt<uint64_t>());
        p.SetBool("abandoned", j["abandoned"].get_bool());
        const AssetId named{a.IsNull() ? b : a};
        const auto display{named.IsNull() ? std::nullopt : state.GetDisplayName(named)};
        p.SetString("name", display ? display->text : Label(state, named));
        const auto launch{named.IsNull() ? std::nullopt : state.GetLaunch(named)};
        if (const auto logo{LogoUrl(state, named, false)}) p.SetString("logo", *logo);
        double liquidity{0};
        if (a.IsNull()) liquidity = 2 * Whole(ra, da);
        if (b.IsNull()) liquidity = 2 * Whole(rb, db);
        p.SetDouble("liquidity_ecx", liquidity);
        const Window day{Day(*tip.view, bitassets::market::Market{false, id}, tip.time)};
        p.SetDouble("volume_24h", day.volume);
        if (day.change) p.SetDouble("change_24h", *day.change);
        const bool launch_pool{launch && (launch->pool == id)};
        if (launch_pool) p.SetString("launch", Label(state, named));
        p.SetBool("locked", launch_pool && launch->Graduated());
        p.SetString("curve", j["curve"].get_str());
        const uint32_t fee{static_cast<uint32_t>(j["fee_millionths"].getInt<uint64_t>())};
        p.SetUInt("fee_ppm", fee);
        p.SetDouble("fee_bps", fee / 100.0);
        if (j.exists("amp")) p.SetUInt("amp", j["amp"].getInt<uint64_t>());
        if (j.exists("weight_a")) p.SetUInt("weight_a", j["weight_a"].getInt<uint64_t>());
        if (j.exists("weight_b")) p.SetUInt("weight_b", j["weight_b"].getInt<uint64_t>());
        p.SetBool("default_kind", j["default_kind"].get_bool());
        p.SetBool("dynamic_fee", j["dynamic_fee"].get_bool());
        if (j.exists("fee_now")) p.SetDouble("fee_now_bps", j["fee_now"].get_real() * 100);
        if (j.exists("concentrated")) {
            const UniValue& c{j["concentrated"]};
            Message& m{p.Mutable("concentrated")};
            m.SetString("sqrt_price_x96", c["sqrt_price_x96"].get_str());
            m.SetInt("tick", c["tick"].getInt<int64_t>());
            m.SetString("liquidity", c["liquidity"].get_str());
            m.SetInt("tick_spacing", c["tick_spacing"].getInt<int64_t>());
        }
        if (j.exists("depth")) p.SetDouble("depth_ecx", j["depth"].get_real());
        if (j.exists("volume_window")) p.SetDouble("volume_window_ecx", j["volume_window"].get_real());
        if (j.exists("price_change")) p.SetDouble("price_change_window", j["price_change"].get_real());
        if (j.exists("last_activity")) p.SetUInt("last_activity", j["last_activity"].getInt<uint64_t>());
        return p;
    }

    // -----------------------------------------------------------------------------------------------------
    // Trades and candles

    void FillTrade(Message& t, const bitassets::market::TradeRow& row, const bitassets::State& state) const
    {
        t.SetString("txid", row.txid.GetHex());
        t.SetUInt("height", static_cast<uint32_t>(row.height));
        t.SetUInt("time", row.time);
        t.SetString("pool", Hex(row.pool));
        t.SetString("asset_in", Label(state, row.asset_in));
        t.SetString("asset_out", Label(state, row.asset_out));
        t.SetString("amount_in_units", util::ToString(row.amount_in));
        t.SetString("amount_out_units", util::ToString(row.amount_out));
        t.SetString("address", AddressOf(row.to));
        t.SetUInt("hops", row.hops);
        const auto curve{static_cast<bitassets::Curve>(row.curve)};
        t.SetString("venue", curve == bitassets::Curve::LAUNCH ? "curve" : curve == bitassets::Curve::BOOK ? "book" : "pool");
        // Against ECX: the side of the other asset; else of the second asset of the pair (asset_b: the larger id).
        const bool coin_in{row.asset_in.IsNull()}, coin_out{row.asset_out.IsNull()};
        bool buy;
        if (coin_in || coin_out) {
            buy = coin_in;
        } else {
            buy = row.asset_out > row.asset_in;
        }
        t.SetString("side", buy ? "buy" : "sell");
        const AssetId token{buy ? row.asset_out : row.asset_in};
        const uint64_t token_units{buy ? row.amount_out : row.amount_in};
        const uint64_t quote_units{buy ? row.amount_in - std::min(row.amount_in, row.back) : row.amount_out};
        const AssetId quote{buy ? row.asset_in : row.asset_out};
        const double tokens{Whole(token_units, DecimalsOf(state, token))}, quoted{Whole(quote_units, DecimalsOf(state, quote))};
        t.SetDouble("tokens", tokens);
        t.SetDouble("ecx", quoted);
        t.SetDouble("price_ecx", tokens > 0 ? quoted / tokens : 0);
    }

    Message TradesOf(const bitassets::market::Market& market, const Message& req, const Tip& tip) const
    {
        if (!bitassets::market::Enabled()) Fail(Code::FAILED_PRECONDITION, "this node keeps no market index (-dexindex)");
        const uint32_t limit{req.GetUInt("limit") == 0 ? 50 : static_cast<uint32_t>(req.GetUInt("limit"))};
        if (limit > 100) Fail(Code::INVALID_ARGUMENT, "limit: 1 to 100");
        std::vector<unsigned char> cursor;
        if (!req.GetString("cursor").empty()) {
            const auto bytes{TryParseHex<unsigned char>(req.GetString("cursor"))};
            if (!bytes || bytes->size() > 128) Fail(Code::INVALID_ARGUMENT, "not a cursor of these trades");
            cursor = *bytes;
        }
        std::vector<unsigned char> next;
        const auto rows{bitassets::market::Trades(*tip.view, market, static_cast<int32_t>(req.GetUInt("before")), cursor, limit, next)};
        const bitassets::State state{tip.State()};
        Message page{New("TradePage")};
        for (const auto& row : rows) FillTrade(page.Add("items"), row, state);
        if (!next.empty()) page.SetString("next_cursor", HexStr(next));
        const auto m{bitassets::market::GetMarket(*tip.view, market)};
        page.SetUInt("total", m ? std::min<uint64_t>(m->trades, std::numeric_limits<uint32_t>::max()) : 0);
        return page;
    }

    Message CandlesOf(const bitassets::market::Market& market, const Message& req, const Tip& tip) const
    {
        if (!bitassets::market::Enabled()) Fail(Code::FAILED_PRECONDITION, "this node keeps no market index (-dexindex)");
        const std::string text{req.GetString("interval").empty() ? "1h" : req.GetString("interval")};
        const auto interval{bitassets::market::ParseInterval(text)};
        if (!interval) Fail(Code::INVALID_ARGUMENT, "interval: 5m, 1h, 4h or 1d");
        const uint32_t to{req.GetUInt("to") == 0 ? std::max<uint32_t>(tip.time, static_cast<uint32_t>(GetTime())) : static_cast<uint32_t>(req.GetUInt("to"))};
        const uint32_t from{static_cast<uint32_t>(req.GetUInt("from"))};
        Message list{New("CandleList")};
        list.SetString("interval", text);
        for (const auto& c : bitassets::market::Candles(*tip.view, market, *interval, from, to, 500)) {
            Message& m{list.Add("items")};
            m.SetUInt("t", c.t);
            m.SetDouble("o", c.o);
            m.SetDouble("h", c.h);
            m.SetDouble("l", c.l);
            m.SetDouble("c", c.c);
            m.SetDouble("v", c.v);
            m.SetUInt("n", c.n);
        }
        return list;
    }

    // -----------------------------------------------------------------------------------------------------
    // Quotes (the node's own methods) and builds

    Message LaunchQuote(const std::string& launch_text, const std::string& side_text, const std::string& amount_text, uint32_t slippage_in, const Tip& tip) const
    {
        const bitassets::State state{tip.State()};
        const auto [token, launch]{ResolveLaunch(state, launch_text)};
        const std::string side{side_text.empty() ? "buy" : side_text};
        if (side != "buy" && side != "sell") Fail(Code::INVALID_ARGUMENT, "side: buy or sell");
        const bool buy{side == "buy"};
        const int dt{DecimalsOf(state, token)};
        const int din{buy ? bitassets::CHN_DECIMALS : dt}, dout{buy ? dt : bitassets::CHN_DECIMALS};
        const uint64_t amount{FloorUnits(ParseAmount(amount_text, din, "amount"), din)};
        if (amount == 0) Fail(Code::INVALID_ARGUMENT, "amount: too small");
        const uint32_t slippage{Slippage(slippage_in)};
        UniValue params(UniValue::VARR);
        params.push_back(ArgOf(token));
        params.push_back(Decimal(amount, din));
        params.push_back(side);
        const UniValue q{Rpc("quotelaunch", params)};
        const uint64_t out{UnitsOf(q["amount_out"], dout)}, back{UnitsOf(q["back"], 8)}, fee{UnitsOf(q["fee"], 8)};
        Message m{New("LaunchQuote")};
        m.SetString("id", Hex(token));
        m.SetString("ticker", Label(state, token));
        m.SetString("side", side);
        m.SetDouble("ecx", buy ? Whole(amount - std::min(amount, back), 8) : Whole(out, 8));
        m.SetDouble("tokens", buy ? Whole(out, dout) : Whole(amount, din));
        m.SetDouble("fee_ecx", Whole(fee, 8));
        m.SetDouble("avg_price", q["price"].get_real());
        m.SetDouble("price_after", q["price_after"].get_real());
        m.SetDouble("spot_price", q["spot_price"].get_real());
        m.SetDouble("impact", q["price_impact"].get_real());
        const double progress{q["progress_after"].get_real()};
        m.SetDouble("progress_after", progress);
        m.SetDouble("raised_after", progress / 100.0 * static_cast<double>(launch.graduation) / COIN);
        m.SetDouble("refund_ecx", Whole(back, 8));
        m.SetString("amount_in_units", util::ToString(amount));
        m.SetString("amount_out_units", util::ToString(out));
        const uint64_t floor{Floor(out, slippage, dout)};
        m.SetString("min_out_units", util::ToString(floor));
        m.SetDouble("min_out", Whole(floor, dout));
        m.SetUInt("slippage_bps", slippage);
        m.SetBool("graduates", q["graduates"].get_bool());
        m.SetUInt("height", static_cast<uint32_t>(tip.height));
        return m;
    }

    Message SwapQuote(const std::string& from_text, const std::string& to_text, const std::string& amount_text, uint32_t slippage_in, uint32_t max_hops_in, const Tip& tip) const
    {
        const bitassets::State state{tip.State()};
        const AssetId from{ResolveRegistered(state, from_text)}, to{ResolveRegistered(state, to_text)};
        if (from == to) Fail(Code::INVALID_ARGUMENT, "from and to are the same asset");
        const uint32_t max_hops{max_hops_in == 0 ? 3 : max_hops_in};
        if (max_hops > bitassets::MAX_ROUTE_ASSETS - 1) Fail(Code::INVALID_ARGUMENT, "max_hops: 1 to 4");
        const uint8_t din{DecimalsOf(state, from)}, dout{DecimalsOf(state, to)};
        const uint64_t amount{FloorUnits(ParseAmount(amount_text, din, "amount"), din)};
        if (amount == 0) Fail(Code::INVALID_ARGUMENT, "amount: too small");
        const uint32_t slippage{Slippage(slippage_in)};
        Message quote{New("SwapQuote")};
        quote.SetString("from", Label(state, from));
        quote.SetString("to", Label(state, to));
        quote.SetString("from_id", from.IsNull() ? "" : Hex(from));
        quote.SetString("to_id", to.IsNull() ? "" : Hex(to));
        quote.SetDouble("amount_in", Whole(amount, din));
        quote.SetString("amount_in_units", util::ToString(amount));
        quote.SetUInt("slippage_bps", slippage);
        quote.SetUInt("height", static_cast<uint32_t>(tip.height));
        struct Option {
            Message m;
            uint64_t out;
        };
        std::vector<Option> options;
        const auto label_path{[&](Message& o, const std::vector<AssetId>& path) {
            for (const AssetId& a : path) {
                o.AddString("path_ids", a.IsNull() ? "" : Hex(a));
                o.AddString("path", Label(state, a));
            }
        }};
        // Every pool of the pair, of every kind (the book of the pair among them): getswapquote.
        {
            UniValue params(UniValue::VARR);
            params.push_back(ArgOf(from));
            params.push_back(Decimal(amount, din, din));
            params.push_back(ArgOf(to));
            UniValue opts(UniValue::VOBJ);
            opts.pushKV("max_hops", 0);
            params.push_back(opts);
            const UniValue q{Rpc("getswapquote", params)};
            for (const UniValue& p : q["pools"].getValues()) {
                Message o{New("SwapOption")};
                const uint64_t out{UnitsOf(p["amount_out"], dout)};
                o.SetString("venue", p["curve"].get_str() == "book" ? "book" : "pool");
                o.AddString("pools", p["pool"].get_str());
                label_path(o, {from, to});
                o.SetDouble("amount_out", Whole(out, dout));
                o.SetString("amount_out_units", util::ToString(out));
                o.SetDouble("price", p["price"].get_real());
                o.SetDouble("price_impact", p["price_impact"].get_real());
                o.SetBool("default_pools", p["default_kind"].get_bool());
                Message& hop{o.Add("hops")};
                hop.SetString("pool", p["pool"].get_str());
                hop.SetString("curve", p["curve"].get_str());
                hop.SetDouble("fee_bps", p["fee"].get_real() * 100);
                hop.SetString("asset_in", Label(state, from));
                hop.SetString("asset_out", Label(state, to));
                hop.SetDouble("amount_out", Whole(out, dout));
                options.push_back({std::move(o), out});
            }
        }
        // Routes through several pools: quoteroute (the same search the app's swap panel shows).
        if (max_hops >= 2) {
            UniValue params(UniValue::VARR);
            params.push_back(ArgOf(from));
            params.push_back(Decimal(amount, din, din));
            params.push_back(ArgOf(to));
            params.push_back(static_cast<int>(max_hops));
            params.push_back(3);
            const UniValue q{Rpc("quoteroute", params)};
            for (const UniValue& r : q["routes"].getValues()) {
                if (r["pools"].size() < 2) continue;
                Message o{New("SwapOption")};
                const uint64_t out{UnitsOf(r["amount_out"], dout)};
                o.SetString("venue", "route");
                std::vector<AssetId> path;
                for (const UniValue& id : r["path_ids"].getValues()) path.push_back(id.isStr() ? AssetId{*uint256::FromHex(id.get_str())} : AssetId{});
                label_path(o, path);
                for (const UniValue& id : r["pools"].getValues()) o.AddString("pools", id.get_str());
                o.SetDouble("amount_out", Whole(out, dout));
                o.SetString("amount_out_units", util::ToString(out));
                o.SetDouble("price", out > 0 ? Whole(amount, din) / Whole(out, dout) : 0);
                o.SetBool("default_pools", r["default_pools"].get_bool());
                for (size_t i{0}; i < r["hops"].size(); ++i) {
                    const UniValue& h{r["hops"][i]};
                    Message& hop{o.Add("hops")};
                    hop.SetString("pool", h["pool"].get_str());
                    hop.SetString("curve", h["curve"].get_str());
                    hop.SetDouble("fee_bps", h["fee"].get_real() * 100);
                    if (i + 1 < path.size()) {
                        hop.SetString("asset_in", Label(state, path[i]));
                        hop.SetString("asset_out", Label(state, path[i + 1]));
                    }
                    hop.SetDouble("amount_out", h["amount_out"].get_real());
                }
                options.push_back({std::move(o), out});
            }
        }
        if (options.empty()) Fail(Code::NOT_FOUND, "no pool or route trades this pair for this amount");
        std::stable_sort(options.begin(), options.end(), [](const Option& a, const Option& b) { return a.out > b.out; });
        const Option& best{options.front()};
        quote.Mutable("best") = best.m;
        for (size_t i{0}; i < options.size() && i < 8; ++i) quote.Add("options") = options[i].m;
        quote.SetDouble("amount_out", Whole(best.out, dout));
        quote.SetString("amount_out_units", util::ToString(best.out));
        const uint64_t floor{Floor(best.out, slippage, dout)};
        quote.SetString("min_out_units", util::ToString(floor));
        quote.SetDouble("min_out", Whole(floor, dout));
        return quote;
    }

    Message LiquidityQuote(const Message& req, const Tip& tip) const
    {
        const bitassets::State state{tip.State()};
        const auto id{uint256::FromHex(ToLower(req.GetString("pool")))};
        if (!id) Fail(Code::INVALID_ARGUMENT, "pool: its id, 64 hex digits");
        const auto pool{state.GetPool(*id)};
        if (!pool) Fail(Code::NOT_FOUND, "no such pool");
        // a and b as the pool shows them (Pool): a pool with ECX shows its other asset first.
        const bool flip{pool->asset0.IsNull()};
        const AssetId A{flip ? pool->asset1 : pool->asset0}, B{flip ? pool->asset0 : pool->asset1};
        const uint8_t d0{DecimalsOf(state, A)}, d1{DecimalsOf(state, B)};
        const uint32_t slippage{Slippage(static_cast<uint32_t>(req.GetUInt("slippage_bps")))};
        const std::string action{req.GetString("action")};
        UniValue params(UniValue::VARR);
        params.push_back(Hex(*id));
        uint64_t a0{0}, a1{0};
        if (action == "deposit") {
            if (!req.GetString("amount_a").empty()) a0 = FloorUnits(ParseAmount(req.GetString("amount_a"), d0, "amount_a"), d0);
            if (!req.GetString("amount_b").empty()) a1 = FloorUnits(ParseAmount(req.GetString("amount_b"), d1, "amount_b"), d1);
            if (a0 == 0 && a1 == 0) Fail(Code::INVALID_ARGUMENT, "amount_a and/or amount_b: what goes in");
            UniValue amounts(UniValue::VOBJ);
            if (a0) amounts.pushKV(ArgOf(A), Decimal(a0, d0, d0));
            if (a1) amounts.pushKV(ArgOf(B), Decimal(a1, d1, d1));
            params.push_back(amounts);
        } else if (action == "withdraw") {
            const auto shares{ToIntegral<uint64_t>(req.GetString("shares"))};
            if (!shares || *shares == 0) Fail(Code::INVALID_ARGUMENT, "shares: a whole number of shares");
            params.push_back(UniValue{});
            params.push_back(*shares);
            const std::string only{req.GetString("only")};
            if (only == "a") params.push_back(ArgOf(A));
            else if (only == "b") params.push_back(ArgOf(B));
            else if (!only.empty()) Fail(Code::INVALID_ARGUMENT, "only: a or b");
        } else {
            Fail(Code::INVALID_ARGUMENT, "action: deposit or withdraw");
        }
        const UniValue q{Rpc("quotepoolliquidity", params)};
        Message m{New("LiquidityQuote")};
        m.SetString("pool", Hex(*id));
        m.SetString("action", action);
        m.SetString("asset_a", Label(state, A));
        m.SetString("asset_b", Label(state, B));
        const UniValue& amounts{q["amounts"]};
        const auto amount_of{[&](const AssetId& asset) {
            const std::string label{Label(state, asset)};
            return amounts.exists(label) ? amounts[label] : amounts[std::string{CURRENCY_UNIT}];
        }};
        const uint64_t u0{UnitsOf(amount_of(A), d0)};
        const uint64_t u1{UnitsOf(amount_of(B), d1)};
        m.SetDouble("amount_a", Whole(u0, d0));
        m.SetDouble("amount_b", Whole(u1, d1));
        m.SetString("amount_a_units", util::ToString(u0));
        m.SetString("amount_b_units", util::ToString(u1));
        const uint64_t shares{q["shares"].getInt<uint64_t>()};
        m.SetDouble("shares", static_cast<double>(shares));
        m.SetString("shares_units", util::ToString(shares));
        if (action == "deposit") {
            m.SetString("min_shares_units", util::ToString(static_cast<uint64_t>(static_cast<unsigned __int128>(shares) * (10'000 - slippage) / 10'000)));
        } else {
            m.SetString("min_a_units", util::ToString(Floor(u0, slippage, d0)));
            m.SetString("min_b_units", util::ToString(Floor(u1, slippage, d1)));
        }
        m.SetDouble("share_of_pool", q["share_of_pool"].get_real());
        m.SetUInt("slippage_bps", slippage);
        m.SetUInt("height", static_cast<uint32_t>(tip.height));
        return m;
    }

    /** The first buy of a launch not made yet: on a fresh curve of the rules of the next block (the chain's own curve code). */
    std::pair<uint64_t, uint64_t> FirstBuy(uint64_t ecx, uint64_t supply = 0, uint32_t fee = 0) const
    {
        const Consensus::SidechainParams& params{Params().GetConsensus().sidechain};
        const uint64_t graduation{static_cast<uint64_t>(params.launch_graduation)};
        const uint64_t units{supply == 0 ? bitassets::LAUNCH_SUPPLY : supply};
        const auto cleared{bitassets::curves::ClearLaunch(bitassets::curves::LaunchCurve{fee == 0 ? params.launch_fee : fee, units / 8 * 3, 0, 0, true}, graduation / 2,
                                                          units / 8 * 9, {bitassets::dex::HopIn{true, ecx}}, {true})};
        if (!cleared) Fail(Code::INVALID_ARGUMENT, "the curve cannot take this first buy");
        return {cleared->outs[0], cleared->refunds[0]};
    }

    // -----------------------------------------------------------------------------------------------------
    // The asset registry

    /** An asset of the registry from its row of searchassets and the state: its markets of every kind. */
    void FillAsset(Message& m, const UniValue& row, const bitassets::State& state, const Tip& tip, bool whole_info) const
    {
        const AssetId id{*uint256::FromHex(row["asset"].get_str())};
        const auto record{state.GetAsset(id)};
        const int d{record ? record->decimals : row["decimals"].getInt<int>()};
        m.SetString("id", Hex(id));
        m.SetString("ticker", row.exists("name") ? row["name"].get_str() : row["label"].get_str());
        if (row.exists("display_name")) m.SetString("display_name", row["display_name"].get_str());
        const std::string info{record && record->data.info ? *record->data.info : std::string{}};
        const CreatorText text{SplitInfo(info)};
        m.SetString("info", whole_info ? text.description : text.description.substr(0, 280));
        m.SetUInt("decimals", static_cast<uint32_t>(d));
        const uint64_t supply{record ? record->supply : 0};
        m.SetDouble("supply", Whole(supply, d));
        m.SetString("supply_units", util::ToString(supply));
        m.SetUInt("holders", bitassets::market::Enabled() ? bitassets::market::HolderCount(*tip.view, id, bitassets::Token::Kind::ASSET) : 0);
        if (const auto url{LogoUrl(state, id, false)}) m.SetString("logo", *url);
        const auto control{state.FirstTokenOf(id, bitassets::Token::Kind::CONTROL)};
        m.SetBool("fixed_supply", !control);
        if (row.exists("control") && row["control"].exists("address")) m.SetString("controller", row["control"]["address"].get_str());
        if (record) {
            m.SetUInt("registered_height", static_cast<uint32_t>(record->height));
            m.SetString("registration_txid", record->registration.GetHex());
            m.SetUInt("seq", record->seq);
        }
        const auto launch{state.GetLaunch(id)};
        m.SetBool("launched", launch.has_value());
        // Its markets: a curve not graduated, then pools, books, auctions.
        Message& market{m.Mutable("market")};
        uint32_t pools{0}, books{0};
        for (const auto& [pid, pool] : state.PoolsOf(id)) {
            const auto curve{state.GetPoolParams(pid).GetCurve()};
            if (curve == bitassets::Curve::LAUNCH) continue;
            if (curve == bitassets::Curve::BOOK) {
                if (pool.shares > bitassets::MIN_LIQUIDITY) ++books;
            } else if (!bitassets::amm::Abandoned(pool)) {
                ++pools;
            }
        }
        const uint32_t auctions{static_cast<uint32_t>(state.AuctionsOf(id, 20).size())};
        const bool on_curve{launch && !launch->Graduated()};
        std::vector<std::string> kinds;
        if (on_curve) kinds.emplace_back("curve");
        if (pools) kinds.emplace_back("pool");
        if (books) kinds.emplace_back("book");
        if (auctions) kinds.emplace_back("auction");
        market.SetString("status", kinds.empty() ? "none" : kinds.front());
        for (const std::string& k : kinds) market.AddString("kinds", k);
        market.SetUInt("pools", pools);
        market.SetUInt("books", books);
        market.SetUInt("auctions", auctions);
        std::optional<double> price;
        if (launch) {
            if (const auto pool{state.GetPool(launch->pool)}) {
                if (on_curve) {
                    const uint64_t raised{pool->reserve0 - std::min(pool->reserve0, launch->virtual_coin)};
                    market.SetDouble("progress", std::min(1.0, static_cast<double>(raised) / static_cast<double>(std::max<uint64_t>(launch->graduation, 1))));
                }
                if (pool->reserve1 > 0) price = (static_cast<double>(pool->reserve0) / COIN) / Whole(pool->reserve1, d);
            }
        } else if (const auto pool{state.GetPool(bitassets::PoolId(bitassets::CHN, id))}; pool && pool->reserve1 > 0 && !bitassets::amm::Abandoned(*pool)) {
            price = (static_cast<double>(pool->reserve0) / COIN) / Whole(pool->reserve1, d);
        }
        if (price) market.SetDouble("price_ecx", *price);
        market.SetDouble("volume_24h_ecx", row.exists("volume") ? row["volume"].get_real() : 0.0);
    }

    /** The rows of searchassets for a request (its sort mapped), a page. */
    UniValue SearchAssets(const std::string& q, const std::string& sort, const std::string& cursor, uint32_t limit, const UniValue& filters) const
    {
        UniValue params(UniValue::VARR);
        params.push_back(q);
        params.push_back(filters);
        params.push_back(sort);
        params.push_back(cursor);
        params.push_back(static_cast<int>(limit));
        return Rpc("searchassets", params);
    }

    /** Whole units of a supply (digits only) at `decimals`, in units; refused past what an asset can have. */
    static uint64_t WholeSupply(const std::string& text, int decimals)
    {
        if (text.empty() || text.size() > 19 || text.find_first_not_of("0123456789") != std::string::npos || text[0] == '0') Fail(Code::INVALID_ARGUMENT, "supply: whole units, digits only");
        unsigned __int128 units{0};
        for (const char c : text) units = units * 10 + static_cast<unsigned>(c - '0');
        for (int i{0}; i < decimals; ++i) units *= 10;
        if (units > bitassets::MAX_AMOUNT) Fail(Code::INVALID_ARGUMENT, "supply: more than an asset can have at these decimals");
        return static_cast<uint64_t>(units);
    }

    struct Creator {
        std::string name, desc;
        UniValue links;
        std::string logo;
    };
    /** The creator's fields of a launch or a registration: the display name, the description and https links (the chain's 512 bytes), the logo's hash. */
    static Creator CreatorFields(const Message& req)
    {
        Creator c;
        c.name = req.GetString("name");
        if (c.name.empty() || c.name.size() > bitassets::MAX_DISPLAY_NAME_SIZE) Fail(Code::INVALID_ARGUMENT, "name: 1 to 64 bytes");
        if (const auto why{bitassets::WhyNotDisplayName(c.name)}) Fail(Code::INVALID_ARGUMENT, "name: " + *why);
        c.desc = req.GetString("description");
        for (const unsigned char ch : c.desc) {
            if ((ch < 0x20 && ch != '\n') || ch == 0x7f) Fail(Code::INVALID_ARGUMENT, "description: plain text (line breaks allowed, no other control character)");
        }
        size_t info{c.desc.size()};
        c.links = UniValue{UniValue::VARR};
        for (size_t i{0}; i < req.Count("links"); ++i) {
            const std::string& link{req.StringAt("links", i)};
            if (!SafeLink(link)) Fail(Code::INVALID_ARGUMENT, "links: https only, no user, no space, 512 bytes at most");
            c.links.push_back(link);
            info += link.size() + 1;
        }
        if (c.links.size() > 4) Fail(Code::INVALID_ARGUMENT, "links: at most 4");
        if (info > bitassets::MAX_INFO_SIZE) Fail(Code::INVALID_ARGUMENT, strprintf("the description and the links take %u bytes: the chain keeps %u", info, bitassets::MAX_INFO_SIZE));
        c.logo = ToLower(req.GetString("logo_sha256"));
        if (!c.logo.empty() && (c.logo.size() != 64 || !IsHex(c.logo))) Fail(Code::INVALID_ARGUMENT, "logo_sha256: 64 hex digits");
        return c;
    }

    /** Why the controller of an asset cannot put part of it on a curve now (nullopt: it can). */
    static std::optional<std::string> WhyNoCurve(const bitassets::State& state, const AssetId& asset)
    {
        const auto record{state.GetAsset(asset)};
        if (!record) return "no such asset";
        if (state.GetLaunch(asset)) return "it was launched or put on a curve already";
        if (!state.FirstTokenOf(asset, bitassets::Token::Kind::CONTROL)) return "it has no control coin (its supply is fixed): nobody can put it on a curve";
        if (state.GetPool(bitassets::PoolId(bitassets::CHN, asset))) return "it has a pool with " + Coin() + " already: a curve would graduate into it";
        return std::nullopt;
    }

    Message Build(const Message& req, const Tip& tip) const
    {
        const bitassets::State state{tip.State()};
        const std::string verb{req.GetString("verb")};
        // Ten minutes for a trade, an hour for a launch (its reservation has to be deep enough first), as the site writes them.
        const uint32_t expires_in{req.GetUInt("expires_in") == 0 ? (verb == "launch" ? 3600 : 600) : static_cast<uint32_t>(req.GetUInt("expires_in"))};
        if (expires_in < 60 || expires_in > 3600) Fail(Code::INVALID_ARGUMENT, "expires_in: 60 to 3600 seconds");
        const int64_t expires{GetTime() + expires_in};
        const uint32_t slippage{Slippage(static_cast<uint32_t>(req.GetUInt("slippage_bps")))};
        Message built{New("BuiltAction")};
        built.SetString("verb", verb);
        built.SetString("network", m_options.network);
        built.SetUInt("expires", static_cast<uint32_t>(expires));
        built.SetUInt("height", static_cast<uint32_t>(tip.height));
        UriParams uri;
        std::map<std::string, UniValue> payload{
            {"format", UniValue{"flush-action"}}, {"v", UniValue{1}}, {"network", UniValue{m_options.network}}, {"verb", UniValue{verb}},
            {"expires", UniValue{expires}}, {"quote_height", UniValue{tip.height}}, {"slippage_bps", UniValue{static_cast<int>(slippage)}},
        };
        const auto ticker_of{[&](const AssetId& asset) {
            const std::string label{Label(state, asset)};
            if (!asset.IsNull() && !bitassets::IsTicker(label)) Fail(Code::FAILED_PRECONDITION, label.substr(0, 20) + " has no ticker: the app's links name assets by ticker");
            return label;
        }};
        std::string summary;
        if (verb == "buy" || verb == "sell") {
            const bool buy{verb == "buy"};
            Message q{LaunchQuote(req.GetString("launch"), verb, req.GetString("amount"), slippage, tip)};
            const AssetId token{*uint256::FromHex(q.GetString("id"))};
            const std::string ticker{ticker_of(token)};
            const uint64_t in{*ToIntegral<uint64_t>(q.GetString("amount_in_units"))}, floor{*ToIntegral<uint64_t>(q.GetString("min_out_units"))};
            if (floor == 0) Fail(Code::FAILED_PRECONDITION, "the trade gives too little");
            const int dt{DecimalsOf(state, token)};
            const int din{buy ? 8 : dt}, dout{buy ? dt : 8};
            uri = {{"launch", ticker}, {"asset", Hex(token)}, {buy ? "ecx" : "tokens", Decimal(in, din)}, {buy ? "min_tokens" : "min_ecx", Decimal(floor, dout)}, {"slippage_bps", util::ToString(slippage)}};
            payload["launch"] = UniValue{ticker};
            payload["asset"] = UniValue{Hex(token)};
            payload[buy ? "ecx" : "tokens"] = UniValue{Decimal(in, din)};
            payload[buy ? "min_tokens" : "min_ecx"] = UniValue{Decimal(floor, dout)};
            summary = buy ? strprintf("Buy at least %s %s for %s %s on its curve", Decimal(floor, dout), ticker, Decimal(in, din), CURRENCY_UNIT)
                          : strprintf("Sell %s %s to its curve for at least %s %s", Decimal(in, din), ticker, Decimal(floor, dout), CURRENCY_UNIT);
            built.Mutable("launch_quote") = q;
            built.SetBool("app_ready", true);
        } else if (verb == "swap") {
            const uint32_t max_hops{req.GetUInt("max_hops") == 0 ? 1 : static_cast<uint32_t>(req.GetUInt("max_hops"))};
            Message q{SwapQuote(req.GetString("from"), req.GetString("to"), req.GetString("amount"), slippage, max_hops, tip)};
            const Message* best{q.Sub("best")};
            const AssetId from{q.GetString("from_id").empty() ? AssetId{} : AssetId{*uint256::FromHex(q.GetString("from_id"))}};
            const AssetId to{q.GetString("to_id").empty() ? AssetId{} : AssetId{*uint256::FromHex(q.GetString("to_id"))}};
            const uint8_t din{DecimalsOf(state, from)}, dout{DecimalsOf(state, to)};
            const uint64_t in{*ToIntegral<uint64_t>(q.GetString("amount_in_units"))}, floor{*ToIntegral<uint64_t>(q.GetString("min_out_units"))};
            if (floor == 0) Fail(Code::FAILED_PRECONDITION, "the trade gives too little");
            const std::string from_t{ticker_of(from)}, to_t{ticker_of(to)};
            const bool single{best->Count("pools") == 1};
            if (single) {
                uri = {{"pool", best->StringAt("pools", 0)}, {"from", from_t}, {"to", to_t}, {"amount", Decimal(in, din)}, {"min_out", Decimal(floor, dout)}, {"slippage_bps", util::ToString(slippage)}};
                payload["pool"] = UniValue{best->StringAt("pools", 0)};
            } else {
                // A route through several pools: the verb "route" (not in the app's links yet: built for the extension and later apps).
                built.SetString("verb", "route");
                payload["verb"] = UniValue{"route"};
                std::string pools;
                UniValue arr(UniValue::VARR);
                for (size_t i{0}; i < best->Count("pools"); ++i) {
                    pools += (i ? "," : "") + best->StringAt("pools", i);
                    arr.push_back(best->StringAt("pools", i));
                }
                uri = {{"pools", pools}, {"from", from_t}, {"to", to_t}, {"amount", Decimal(in, din)}, {"min_out", Decimal(floor, dout)}, {"slippage_bps", util::ToString(slippage)}};
                payload["pools"] = arr;
            }
            payload["from"] = UniValue{from_t};
            payload["to"] = UniValue{to_t};
            payload["from_id"] = UniValue{from.IsNull() ? "" : Hex(from)};
            payload["to_id"] = UniValue{to.IsNull() ? "" : Hex(to)};
            payload["amount"] = UniValue{Decimal(in, din)};
            payload["min_out"] = UniValue{Decimal(floor, dout)};
            summary = strprintf("Swap %s %s for at least %s %s%s", Decimal(in, din), from_t, Decimal(floor, dout), to_t, single ? "" : strprintf(" through %u pools", best->Count("pools")));
            built.Mutable("swap_quote") = q;
            built.SetBool("app_ready", single);
        } else if (verb == "deposit" || verb == "withdraw") {
            Message lreq{New("QuoteLiquidityRequest")};
            lreq.SetString("pool", req.GetString("pool"));
            lreq.SetString("action", verb);
            lreq.SetString("amount_a", req.GetString("amount_a"));
            lreq.SetString("amount_b", req.GetString("amount_b"));
            lreq.SetString("shares", req.GetString("shares"));
            lreq.SetString("only", req.GetString("only"));
            lreq.SetUInt("slippage_bps", slippage);
            Message q{LiquidityQuote(lreq, tip)};
            const uint256 id{*uint256::FromHex(q.GetString("pool"))};
            const auto pool{*state.GetPool(id)};
            const AssetId A{pool.asset0.IsNull() ? pool.asset1 : pool.asset0}, B{pool.asset0.IsNull() ? pool.asset0 : pool.asset1};
            const uint8_t d0{DecimalsOf(state, A)}, d1{DecimalsOf(state, B)};
            const std::string ta{ticker_of(A)}, tb{ticker_of(B)};
            uri = {{"pool", Hex(id)}, {"asset_a", ta}, {"asset_b", tb}};
            payload["pool"] = UniValue{Hex(id)};
            payload["asset_a"] = UniValue{ta};
            payload["asset_b"] = UniValue{tb};
            if (verb == "deposit") {
                const uint64_t u0{*ToIntegral<uint64_t>(q.GetString("amount_a_units"))}, u1{*ToIntegral<uint64_t>(q.GetString("amount_b_units"))};
                uri.emplace_back("amount_a", Decimal(u0, d0));
                uri.emplace_back("amount_b", Decimal(u1, d1));
                uri.emplace_back("min_shares", q.GetString("min_shares_units"));
                payload["amount_a"] = UniValue{Decimal(u0, d0)};
                payload["amount_b"] = UniValue{Decimal(u1, d1)};
                payload["min_shares"] = UniValue{q.GetString("min_shares_units")};
                summary = strprintf("Add %s %s and %s %s to the pool for at least %s shares", Decimal(u0, d0), ta, Decimal(u1, d1), tb, q.GetString("min_shares_units"));
            } else {
                const uint64_t m0{*ToIntegral<uint64_t>(q.GetString("min_a_units"))}, m1{*ToIntegral<uint64_t>(q.GetString("min_b_units"))};
                uri.emplace_back("shares", q.GetString("shares_units"));
                uri.emplace_back("min_a", Decimal(m0, d0));
                uri.emplace_back("min_b", Decimal(m1, d1));
                payload["shares"] = UniValue{q.GetString("shares_units")};
                payload["min_a"] = UniValue{Decimal(m0, d0)};
                payload["min_b"] = UniValue{Decimal(m1, d1)};
                if (!req.GetString("only").empty()) {
                    uri.emplace_back("only", req.GetString("only"));
                    payload["only"] = UniValue{req.GetString("only")};
                }
                summary = strprintf("Take %s shares out of the pool for at least %s %s and %s %s", q.GetString("shares_units"), Decimal(m0, d0), ta, Decimal(m1, d1), tb);
            }
            uri.emplace_back("slippage_bps", util::ToString(slippage));
            built.Mutable("liquidity_quote") = q;
            built.SetBool("app_ready", false);
        } else if (verb == "launch") {
            const std::string ticker{ToUpper(req.GetString("ticker"))};
            if (!bitassets::IsTicker(ticker) || ticker == Coin()) Fail(Code::INVALID_ARGUMENT, "ticker: 2 to 12 capital letters and digits");
            if (state.GetAsset(bitassets::HashName(ticker))) Fail(Code::ALREADY_EXISTS, "the ticker " + ticker + " is taken");
            const std::string name{req.GetString("name")};
            if (name.empty() || name.size() > bitassets::MAX_DISPLAY_NAME_SIZE) Fail(Code::INVALID_ARGUMENT, "name: 1 to 64 bytes");
            if (const auto why{bitassets::WhyNotDisplayName(name)}) Fail(Code::INVALID_ARGUMENT, "name: " + *why);
            const std::string desc{req.GetString("description")};
            for (const unsigned char c : desc) {
                if ((c < 0x20 && c != '\n') || c == 0x7f) Fail(Code::INVALID_ARGUMENT, "description: plain text (line breaks allowed, no other control character)");
            }
            size_t info{desc.size()};
            uri = {{"ticker", ticker}, {"name", name}};
            payload["ticker"] = UniValue{ticker};
            payload["name"] = UniValue{name};
            UniValue links(UniValue::VARR);
            for (size_t i{0}; i < req.Count("links"); ++i) {
                const std::string& link{req.StringAt("links", i)};
                if (!SafeLink(link)) Fail(Code::INVALID_ARGUMENT, "links: https only, no user, no space, 512 bytes at most");
                links.push_back(link);
                info += link.size() + 1;
            }
            if (links.size() > 4) Fail(Code::INVALID_ARGUMENT, "links: at most 4");
            if (info > bitassets::MAX_INFO_SIZE) Fail(Code::INVALID_ARGUMENT, strprintf("the description and the links take %u bytes: the chain keeps %u", info, bitassets::MAX_INFO_SIZE));
            const std::string logo{ToLower(req.GetString("logo_sha256"))};
            if (!logo.empty()) {
                if (logo.size() != 64 || !IsHex(logo)) Fail(Code::INVALID_ARGUMENT, "logo_sha256: 64 hex digits");
                uri.emplace_back("logo", "sha256:" + logo);
                payload["logo_sha256"] = UniValue{logo};
            }
            if (!desc.empty()) {
                uri.emplace_back("desc", desc);
                payload["desc"] = UniValue{desc};
            }
            for (const UniValue& link : links.getValues()) uri.emplace_back("link", link.get_str());
            if (!links.empty()) payload["links"] = links;
            summary = strprintf("Launch %s (%s)", ticker, name);
            // Its parameters (launch_params_height): a supply and decimals, a fee, an allocation of its own.
            int decimals{bitassets::LAUNCH_DECIMALS};
            uint64_t supply{0};
            uint32_t fee{0};
            if (!req.GetString("supply").empty() || req.Has("decimals") || req.GetUInt("fee_bps") != 0) {
                if (!Params().GetConsensus().sidechain.LaunchParamsActive(tip.height + 1)) Fail(Code::FAILED_PRECONDITION, "launches with a supply or a fee of their own do not apply yet on this network");
            }
            if (req.Has("decimals")) {
                decimals = static_cast<int>(req.GetUInt("decimals"));
                if (decimals > bitassets::MAX_DECIMALS) Fail(Code::INVALID_ARGUMENT, "decimals: 0 to 12");
                if (req.GetString("supply").empty()) Fail(Code::INVALID_ARGUMENT, "decimals go with a supply");
            }
            if (!req.GetString("supply").empty()) {
                supply = WholeSupply(req.GetString("supply"), decimals);
                if (!bitassets::IsLaunchSupply(supply)) {
                    Fail(Code::INVALID_ARGUMENT, strprintf("supply: a multiple of 8 units, %s to %s at %d decimals", Decimal(bitassets::MIN_LAUNCH_SUPPLY, decimals, 12),
                                                          Decimal(bitassets::MAX_LAUNCH_SUPPLY, decimals, 12), decimals));
                }
                uri.emplace_back("supply", req.GetString("supply"));
                uri.emplace_back("decimals", util::ToString(decimals));
                payload["supply"] = UniValue{req.GetString("supply")};
                payload["decimals"] = UniValue{decimals};
                summary += strprintf(", %s of %d decimals", req.GetString("supply"), decimals);
            }
            if (req.GetUInt("fee_bps") != 0) {
                const uint32_t bps{static_cast<uint32_t>(req.GetUInt("fee_bps"))};
                if (bps < bitassets::MIN_LAUNCH_FEE / 100 || bps > bitassets::MAX_LAUNCH_FEE / 100) Fail(Code::INVALID_ARGUMENT, "fee_bps: 30 to 500");
                fee = bps * 100;
                uri.emplace_back("fee_bps", util::ToString(bps));
                payload["fee_bps"] = UniValue{static_cast<int>(bps)};
                summary += strprintf(", a fee of %d.%02d %%", bps / 100, bps % 100);
            }
            if (req.GetUInt("allocation_bps") != 0) {
                const uint32_t bps{static_cast<uint32_t>(req.GetUInt("allocation_bps"))};
                if (bps > bitassets::MAX_LAUNCH_ALLOCATION) Fail(Code::INVALID_ARGUMENT, "allocation_bps: 0 to 500");
                uri.emplace_back("allocation_bps", util::ToString(bps));
                payload["allocation_bps"] = UniValue{static_cast<int>(bps)};
            }
            if (!req.GetString("first_buy_ecx").empty()) {
                const uint64_t ecx{FloorUnits(ParseAmount(req.GetString("first_buy_ecx"), 8, "first_buy_ecx"), 8)};
                const auto [tokens, back]{FirstBuy(ecx, supply, fee)};
                const uint64_t floor{Floor(tokens, slippage, decimals)};
                uri.emplace_back("ecx", Decimal(ecx, 8));
                uri.emplace_back("min_tokens", Decimal(floor, decimals));
                payload["ecx"] = UniValue{Decimal(ecx, 8)};
                payload["min_tokens"] = UniValue{Decimal(floor, decimals)};
                summary += strprintf(", buying at least %s for %s %s", Decimal(floor, decimals), Decimal(ecx, 8), CURRENCY_UNIT);
                (void)back;
            }
            uri.emplace_back("slippage_bps", util::ToString(slippage));
            built.SetBool("app_ready", true);
        } else if (verb == "register") {
            const std::string ticker{ToUpper(req.GetString("ticker"))};
            if (!bitassets::IsTicker(ticker) || ticker == Coin()) Fail(Code::INVALID_ARGUMENT, "ticker: 2 to 12 capital letters and digits");
            if (state.GetAsset(bitassets::HashName(ticker))) Fail(Code::ALREADY_EXISTS, "the ticker " + ticker + " is taken");
            const auto [name, desc, links, logo]{CreatorFields(req)};
            if (req.GetString("supply").empty() || !req.Has("decimals")) Fail(Code::INVALID_ARGUMENT, "register: supply and decimals");
            const int decimals{static_cast<int>(req.GetUInt("decimals"))};
            if (decimals > bitassets::MAX_DECIMALS) Fail(Code::INVALID_ARGUMENT, "decimals: 0 to 12");
            WholeSupply(req.GetString("supply"), decimals);
            uri = {{"ticker", ticker}, {"name", name}};
            payload["ticker"] = UniValue{ticker};
            payload["name"] = UniValue{name};
            if (!logo.empty()) {
                uri.emplace_back("logo", "sha256:" + logo);
                payload["logo_sha256"] = UniValue{logo};
            }
            if (!desc.empty()) {
                uri.emplace_back("desc", desc);
                payload["desc"] = UniValue{desc};
            }
            for (const UniValue& link : links.getValues()) uri.emplace_back("link", link.get_str());
            if (!links.empty()) payload["links"] = links;
            uri.emplace_back("supply", req.GetString("supply"));
            uri.emplace_back("decimals", util::ToString(decimals));
            payload["supply"] = UniValue{req.GetString("supply")};
            payload["decimals"] = UniValue{decimals};
            summary = strprintf("Register %s (%s): %s of %d decimals, no market", ticker, name, req.GetString("supply"), decimals);
            built.SetBool("app_ready", true);
        } else if (verb == "curve") {
            if (!Params().GetConsensus().sidechain.LaunchParamsActive(tip.height + 1)) Fail(Code::FAILED_PRECONDITION, "existing assets go on curves from launch_params_height: not yet on this network");
            const AssetId asset{ResolveRegistered(state, req.GetString("asset"))};
            if (asset.IsNull()) Fail(Code::INVALID_ARGUMENT, "asset: not " + Coin());
            if (const auto why{WhyNoCurve(state, asset)}) Fail(Code::FAILED_PRECONDITION, *why);
            const int d{DecimalsOf(state, asset)};
            const uint64_t amount{ParseAmount(req.GetString("amount"), d, "amount")};
            if (!bitassets::IsLaunchSupply(amount)) Fail(Code::INVALID_ARGUMENT, strprintf("amount: a multiple of 8 units, %s to %s", Decimal(bitassets::MIN_LAUNCH_SUPPLY, d, 12), Decimal(bitassets::MAX_LAUNCH_SUPPLY, d, 12)));
            const uint32_t bps{static_cast<uint32_t>(req.GetUInt("fee_bps") == 0 ? Params().GetConsensus().sidechain.launch_fee / 100 : req.GetUInt("fee_bps"))};
            if (bps < bitassets::MIN_LAUNCH_FEE / 100 || bps > bitassets::MAX_LAUNCH_FEE / 100) Fail(Code::INVALID_ARGUMENT, "fee_bps: 30 to 500");
            const std::string ticker{ticker_of(asset)};
            uri = {{"asset", Hex(asset)}, {"ticker", ticker}, {"amount", Decimal(amount, d, 12)}, {"fee_bps", util::ToString(bps)}};
            payload["asset"] = UniValue{Hex(asset)};
            payload["ticker"] = UniValue{ticker};
            payload["amount"] = UniValue{Decimal(amount, d, 12)};
            payload["fee_bps"] = UniValue{static_cast<int>(bps)};
            for (const char* key : {"min_buy", "max_buy"}) {
                if (req.GetString(key).empty()) continue;
                const uint64_t v{FloorUnits(ParseAmount(req.GetString(key), 8, key), 8)};
                uri.emplace_back(key, Decimal(v, 8));
                payload[key] = UniValue{Decimal(v, 8)};
            }
            summary = strprintf("Put %s %s on a bonding curve at %d.%02d %%", Decimal(amount, d, 12), ticker, bps / 100, bps % 100);
            built.SetBool("app_ready", true);
        } else if (verb == "pool") {
            const AssetId a{ResolveRegistered(state, req.GetString("asset_a"))}, b{ResolveRegistered(state, req.GetString("asset_b"))};
            if (a == b) Fail(Code::INVALID_ARGUMENT, "two different assets make a pool");
            if (state.Launching(a) || state.Launching(b)) Fail(Code::FAILED_PRECONDITION, "an asset on its launch curve trades there alone until it graduates");
            const std::string kind{req.GetString("kind").empty() ? "constant" : req.GetString("kind")};
            if (kind != "constant" && kind != "stable" && kind != "weighted") Fail(Code::INVALID_ARGUMENT, "kind: constant, stable or weighted");
            const uint32_t fee_ppm{static_cast<uint32_t>(req.GetUInt("fee_ppm") == 0 ? bitassets::LEGACY_FEE : req.GetUInt("fee_ppm"))};
            if (std::find(bitassets::FEE_TIERS.begin(), bitassets::FEE_TIERS.end(), fee_ppm) == bitassets::FEE_TIERS.end()) Fail(Code::INVALID_ARGUMENT, "fee_ppm: 100, 500, 3000 or 10000");
            if ((kind == "weighted") != (req.GetUInt("weight_a") != 0)) Fail(Code::INVALID_ARGUMENT, "weight_a goes with a weighted pool, which needs it");
            if (req.GetUInt("amp") != 0 && kind != "stable") Fail(Code::INVALID_ARGUMENT, "amp goes with a stable pool");
            const int da{DecimalsOf(state, a)}, db{DecimalsOf(state, b)};
            const uint64_t ua{ParseAmount(req.GetString("amount_a"), da, "amount_a")}, ub{ParseAmount(req.GetString("amount_b"), db, "amount_b")};
            const std::string ta{ticker_of(a)}, tb{ticker_of(b)};
            uri = {{"asset_a", a.IsNull() ? Coin() : Hex(a)}, {"ticker_a", ta}, {"asset_b", b.IsNull() ? Coin() : Hex(b)}, {"ticker_b", tb}, {"kind", kind}, {"fee_ppm", util::ToString(fee_ppm)},
                   {"amount_a", Decimal(ua, da, 12)}, {"amount_b", Decimal(ub, db, 12)}};
            if (req.GetUInt("amp") != 0) uri.emplace_back("amp", util::ToString(req.GetUInt("amp")));
            if (req.GetUInt("weight_a") != 0) uri.emplace_back("weight_a", util::ToString(req.GetUInt("weight_a")));
            for (const auto& [k, v] : uri) payload[k] = UniValue{v};
            summary = strprintf("Create a %s pool of %s %s and %s %s at %s %%", kind == "constant" ? "constant-product" : kind, Decimal(ua, da, 12), ta, Decimal(ub, db, 12), tb, Decimal(fee_ppm, 4));
            built.SetBool("app_ready", true);
        } else if (verb == "auction") {
            const AssetId base{ResolveRegistered(state, req.GetString("base"))}, quote{req.GetString("quote").empty() ? AssetId{} : ResolveRegistered(state, req.GetString("quote"))};
            if (base.IsNull() || base == quote) Fail(Code::INVALID_ARGUMENT, "base: an asset other than the quote");
            const int db{DecimalsOf(state, base)}, dq{DecimalsOf(state, quote)};
            const uint64_t amount{ParseAmount(req.GetString("amount"), db, "amount")};
            const uint64_t start{ParseAmount(req.GetString("start_price"), dq, "start_price")}, end{ParseAmount(req.GetString("end_price"), dq, "end_price")};
            if (end > start) Fail(Code::INVALID_ARGUMENT, "end_price is above start_price: a Dutch auction's price falls");
            const uint32_t duration{static_cast<uint32_t>(req.GetUInt("duration"))};
            if (duration == 0 || duration > static_cast<uint32_t>(bitassets::MAX_AUCTION_DURATION)) Fail(Code::INVALID_ARGUMENT, "duration: 1 to 1000000 blocks");
            const uint32_t delay{static_cast<uint32_t>(req.GetUInt("delay"))};
            if (delay > static_cast<uint32_t>(bitassets::MAX_AUCTION_DELAY)) Fail(Code::INVALID_ARGUMENT, "delay: at most 52560 blocks");
            const std::string tb{ticker_of(base)}, tq{ticker_of(quote)};
            uri = {{"base", Hex(base)}, {"base_ticker", tb}, {"amount", Decimal(amount, db, 12)}, {"quote", quote.IsNull() ? Coin() : Hex(quote)}, {"quote_ticker", tq},
                   {"start_price", Decimal(start, dq, 12)}, {"end_price", Decimal(end, dq, 12)}, {"duration", util::ToString(duration)}};
            if (delay != 0) uri.emplace_back("delay", util::ToString(delay));
            for (const auto& [k, v] : uri) payload[k] = UniValue{v};
            summary = strprintf("Auction %s %s, its price falling from %s to %s %s over %u blocks", Decimal(amount, db, 12), tb, Decimal(start, dq, 12), Decimal(end, dq, 12), tq, duration);
            built.SetBool("app_ready", true);
        } else if (verb == "order") {
            const AssetId sell{ResolveRegistered(state, req.GetString("sell"))}, buy{ResolveRegistered(state, req.GetString("buy"))};
            if (sell == buy) Fail(Code::INVALID_ARGUMENT, "two different assets make a book");
            const int ds{DecimalsOf(state, sell)}, dbuy{DecimalsOf(state, buy)};
            const uint64_t amount{ParseAmount(req.GetString("amount"), ds, "amount")};
            const std::string price{req.GetString("price")};
            ParseAmount(price, std::max(8, dbuy), "price");
            const uint32_t blocks{static_cast<uint32_t>(req.GetUInt("expiry_blocks") == 0 ? 40320 : req.GetUInt("expiry_blocks"))};
            if (blocks > 1'000'000) Fail(Code::INVALID_ARGUMENT, "expiry_blocks: 1 to 1000000");
            const std::string ts{ticker_of(sell)}, tbuy{ticker_of(buy)};
            uri = {{"sell", sell.IsNull() ? Coin() : Hex(sell)}, {"sell_ticker", ts}, {"buy", buy.IsNull() ? Coin() : Hex(buy)}, {"buy_ticker", tbuy}, {"amount", Decimal(amount, ds, 12)},
                   {"price", price}, {"expiry_blocks", util::ToString(blocks)}};
            for (const auto& [k, v] : uri) payload[k] = UniValue{v};
            summary = strprintf("Sell %s %s for %s at %s each at least, for %u blocks", Decimal(amount, ds, 12), ts, tbuy, price, blocks);
            built.SetBool("app_ready", true);
        } else {
            Fail(Code::INVALID_ARGUMENT, "verb: buy, sell, swap, deposit, withdraw, launch, register, curve, pool, auction or order");
        }
        const std::string payload_text{CanonicalJson(payload)};
        const std::string hash{ActionHash(payload_text)};
        built.SetString("uri", ActionUri(built.GetString("verb"), m_options.network, expires, uri));
        built.SetString("payload", payload_text);
        built.SetString("payload_hash", hash);
        built.SetString("check_code", hash.substr(0, 8));
        built.SetString("summary", summary);
        return built;
    }

    // -----------------------------------------------------------------------------------------------------
    // The methods

    void RegisterAll(grpcsrv::Server& server)
    {
        const Policy read{};
        const Policy expensive{.cache = true, .expensive = true, .cache_control = "public, max-age=2"};
        const Policy fresh{.cache = false, .expensive = true, .cache_control = "no-store"};

        Serve(server, "GetStatus", read, [this](const Message&, const Tip& tip) {
            Message s{New("ChainStatus")};
            s.SetString("network", m_options.network);
            Message& f{s.Mutable("flush")};
            f.SetUInt("height", static_cast<uint32_t>(tip.height));
            f.SetUInt("time", tip.time);
            f.SetString("hash", tip.hash.GetHex());
            f.SetUInt("block_target", static_cast<uint32_t>(Params().GetConsensus().pos.slot_seconds));
            ChainstateManager& chainman{*Assert(m_node.chainman)};
            if (chainman.m_mainchain && chainman.m_mainchain->Height() >= 0) {
                Message& e{s.Mutable("ecash")};
                e.SetUInt("height", static_cast<uint32_t>(chainman.m_mainchain->Height()));
                e.SetString("hash", chainman.m_mainchain->TipHash().GetHex());
            }
            s.SetUInt("slot", static_cast<uint32_t>(Params().GetConsensus().sidechain.slot));
            const auto peg{PegInfo()};
            if (peg.isObject()) {
                if (peg.exists("sidechain") && peg["sidechain"].exists("active")) s.SetBool("slot_active", peg["sidechain"]["active"].get_bool());
                if (peg.exists("escrow") && peg["escrow"].exists("amount")) s.SetDouble("escrow_ecx", peg["escrow"]["amount"].get_real());
            }
            const auto table{Launches(tip)};
            uint32_t today{0};
            for (const auto& l : table->launches) {
                if (l.created_time + 86400 >= tip.time) ++today;
            }
            s.SetUInt("launches_today", today);
            s.SetUInt("indexer_lag", 0);
            s.SetUInt("updated", static_cast<uint32_t>(GetTime()));
            s.SetBool("market_index", bitassets::market::Enabled());
            const Consensus::SidechainParams& rules{Params().GetConsensus().sidechain};
            Message& lp{s.Mutable("launch_params")};
            lp.SetBool("active", rules.LaunchParamsActive(tip.height + 1));
            lp.SetInt("height", rules.launch_params_height == std::numeric_limits<int>::max() ? -1 : rules.launch_params_height);
            lp.SetUInt("fee_min_bps", bitassets::MIN_LAUNCH_FEE / 100);
            lp.SetUInt("fee_max_bps", bitassets::MAX_LAUNCH_FEE / 100);
            lp.SetUInt("fee_default_bps", rules.launch_fee / 100);
            lp.SetString("supply_min_units", util::ToString(bitassets::MIN_LAUNCH_SUPPLY));
            lp.SetString("supply_max_units", util::ToString(bitassets::MAX_LAUNCH_SUPPLY));
            lp.SetUInt("supply_multiple_units", bitassets::LAUNCH_SUPPLY_MULTIPLE);
            lp.SetUInt("decimals_max", bitassets::MAX_DECIMALS);
            lp.SetUInt("decimals_default", bitassets::LAUNCH_DECIMALS);
            return s;
        });

        Serve(server, "GetStats", read, [this](const Message&, const Tip& tip) {
            const auto table{Launches(tip)};
            const Consensus::SidechainParams& params{Params().GetConsensus().sidechain};
            Message s{New("Stats")};
            uint32_t graduated{0};
            double volume_24h{0}, volume_total{0}, raised{0}, liquidity{0};
            for (const auto& l : table->launches) {
                if (l.rec.Graduated()) ++graduated;
                volume_24h += l.day.volume;
                volume_total += l.volume_total;
                raised += l.raised;
            }
            const bitassets::State state{tip.State()};
            for (const auto& l : table->launches) {
                if (!l.rec.Graduated()) continue;
                if (const auto pool{state.GetPool(l.rec.pool)}) liquidity += 2.0 * pool->reserve0 / COIN;
            }
            s.SetUInt("launches", table->launches.size());
            s.SetUInt("live", table->launches.size() - graduated);
            s.SetUInt("graduated", graduated);
            s.SetDouble("volume_24h", volume_24h);
            s.SetDouble("volume_total", volume_total);
            s.SetDouble("graduation_ecx", static_cast<double>(params.launch_graduation) / COIN);
            s.Mutable("fees") = RulesFees();
            s.Mutable("curve") = CurveOf(params.launch_graduation, params.launch_fee);
            s.SetDouble("raised_total", raised);
            s.SetUInt("pools", bitassets::index::GetInfo(*tip.view).pools);
            s.SetDouble("liquidity_ecx", liquidity);
            return s;
        });

        Serve(server, "GetFees", read, [this](const Message&, const Tip&) {
            const Consensus::SidechainParams& params{Params().GetConsensus().sidechain};
            Message f{New("FeeSchedule")};
            for (uint32_t tier : bitassets::FEE_TIERS) f.AddUInt("pool_fee_tiers_ppm", tier);
            f.SetUInt("default_pool_fee_ppm", bitassets::LEGACY_FEE);
            for (const char* curve : {"constant_product", "stable", "weighted", "concentrated"}) {
                Message& k{f.Add("kinds")};
                k.SetString("curve", curve);
                for (uint32_t tier : bitassets::FEE_TIERS) k.AddUInt("fee_tiers_ppm", tier);
            }
            f.SetBool("dynamic_fees", true);
            f.SetUInt("launch_fee_ppm", params.launch_fee);
            Message fees{RulesFees()};
            for (size_t i{0}; i < fees.Count("split"); ++i) f.Add("launch_split") = fees.At("split", i);
            f.SetDouble("launch_graduation_ecx", static_cast<double>(params.launch_graduation) / COIN);
            f.SetUInt("protocol_fee_share_ppm", params.dex_fee_protocol_share);
            const auto swap_part{[&](const char* to, uint32_t share) {
                Message& m{f.Add("swap_split")};
                m.SetString("to", to);
                m.SetDouble("bps", share / 100.0);
            }};
            swap_part("liquidity_providers", bitassets::FEE_DENOMINATOR - params.dex_fee_security_share - params.dex_fee_protocol_share);
            swap_part("security", params.dex_fee_security_share);
            if (params.dex_fee_protocol_share) swap_part("treasury", params.dex_fee_protocol_share);
            if (m_node.mempool) f.SetDouble("min_relay_fee_ecx_per_kvb", static_cast<double>(m_node.mempool->m_opts.min_relay_feerate.GetFeePerK()) / COIN);
            return f;
        });

        Serve(server, "GetTreasury", read, [this](const Message&, const Tip& tip) {
            const bitassets::State state{tip.State()};
            Message t{New("Treasury")};
            t.SetBool("available", true);
            t.SetString("reason", "held by the chain itself (no key, no output); no rule spends from it yet");
            state.ForEachTreasury([&](const AssetId& asset, const bitassets::TreasuryEntry& e) {
                const uint8_t d{DecimalsOf(state, asset)};
                Message& b{t.Add("balances")};
                b.SetString("asset", Label(state, asset));
                if (!asset.IsNull()) b.SetString("asset_id", Hex(asset));
                b.SetDouble("balance", Whole(e.balance, d));
                b.SetDouble("from_swaps", Whole(e.from_swaps, d));
                b.SetDouble("from_launches", Whole(e.from_launches, d));
                b.SetDouble("security_held", Whole(e.security_held, d));
                if (asset.IsNull()) t.SetDouble("balance_ecx", Whole(e.balance, 8));
                return t.Count("balances") < 200;
            });
            t.SetDouble("security_paid_ecx", Whole(state.GetSecurityFees().total, 8));
            t.SetBool("spending_rule", false);
            if (bitassets::market::Enabled()) {
                double day{0};
                for (const auto& row : bitassets::market::TreasuryInflows(*tip.view, 100)) {
                    const uint8_t d{DecimalsOf(state, row.asset)};
                    if (row.asset.IsNull() && row.time + 86400 >= tip.time) day += Whole(row.from_swaps + row.from_launches, 8);
                    if (t.Count("inflows") >= 50) continue;
                    Message& in{t.Add("inflows")};
                    in.SetUInt("height", static_cast<uint32_t>(row.height));
                    in.SetUInt("time", row.time);
                    in.SetString("asset", Label(state, row.asset));
                    in.SetDouble("amount_ecx", row.asset.IsNull() ? Whole(row.from_swaps + row.from_launches, 8) : 0);
                    in.SetString("source", row.from_launches > row.from_swaps ? "launches" : "swaps");
                    in.SetDouble("from_swaps", Whole(row.from_swaps, d));
                    in.SetDouble("from_launches", Whole(row.from_launches, d));
                    in.SetDouble("security_held", Whole(row.security_held, d));
                }
                t.SetDouble("inflow_24h_ecx", day);
            }
            return t;
        });

        Serve(server, "ListLaunches", read, [this](const Message& req, const Tip& tip) {
            const auto table{Launches(tip)};
            const std::string filter{req.GetString("filter").empty() ? "all" : req.GetString("filter")};
            if (filter != "all" && filter != "new" && filter != "near" && filter != "graduated" && filter != "curve") Fail(Code::INVALID_ARGUMENT, "filter: all, new, near, graduated or curve");
            std::string sort{req.GetString("sort").empty() ? (filter == "near" ? "progress" : "newest") : req.GetString("sort")};
            if (sort != "newest" && sort != "oldest" && sort != "progress" && sort != "mcap" && sort != "volume" && sort != "trades") Fail(Code::INVALID_ARGUMENT, "sort: newest, oldest, progress, mcap, volume or trades");
            const uint32_t limit{req.GetUInt("limit") == 0 ? 50 : static_cast<uint32_t>(req.GetUInt("limit"))};
            if (limit > 100) Fail(Code::INVALID_ARGUMENT, "limit: 1 to 100");
            size_t skip{0};
            if (!req.GetString("cursor").empty()) {
                const auto n{ToIntegral<uint32_t>(req.GetString("cursor"))};
                if (!n) Fail(Code::INVALID_ARGUMENT, "not a cursor of these launches");
                skip = *n;
            }
            const std::string q{ToLower(util::TrimString(req.GetString("q")))};
            if (q.size() > 64) Fail(Code::INVALID_ARGUMENT, "q: 64 characters at most");
            std::vector<const LaunchInfo*> rows;
            for (const LaunchInfo& l : table->launches) {
                const bool grad{l.rec.Graduated()};
                if (filter == "graduated" && !grad) continue;
                if ((filter == "new" || filter == "near" || filter == "curve") && grad) continue;
                if (filter == "new" && l.created_time + 86400 < tip.time) continue;
                if (filter == "near" && l.progress < 70) continue;
                if (!q.empty() && ToLower(l.ticker).find(q) == std::string::npos && ToLower(l.name).find(q) == std::string::npos && !l.token.GetHex().starts_with(q)) continue;
                rows.push_back(&l);
            }
            std::stable_sort(rows.begin(), rows.end(), [&](const LaunchInfo* a, const LaunchInfo* b) {
                if (sort == "oldest") return a->rec.height < b->rec.height;
                if (sort == "progress" && a->progress != b->progress) return a->progress > b->progress;
                if (sort == "mcap" && a->mcap != b->mcap) return a->mcap > b->mcap;
                if (sort == "volume" && a->day.volume != b->day.volume) return a->day.volume > b->day.volume;
                if (sort == "trades" && a->day.trades != b->day.trades) return a->day.trades > b->day.trades;
                return a->rec.height > b->rec.height;
            });
            const bitassets::State state{tip.State()};
            Message page{New("LaunchPage")};
            for (size_t i{skip}; i < rows.size() && i < skip + limit; ++i) FillLaunch(page.Add("items"), *rows[i], state);
            if (skip + limit < rows.size()) page.SetString("next_cursor", util::ToString(skip + limit));
            page.SetUInt("total", rows.size());
            return page;
        });

        Serve(server, "GetLaunch", read, [this](const Message& req, const Tip& tip) {
            const bitassets::State state{tip.State()};
            const auto [token, launch]{ResolveLaunch(state, req.GetString("id"))};
            const auto table{Launches(tip)};
            const auto it{table->by_token.find(token)};
            if (it == table->by_token.end()) Fail(Code::NOT_FOUND, "no such launch");
            const LaunchInfo& l{table->launches[it->second]};
            Message d{New("LaunchDetail")};
            FillLaunch(d, l, state);
            const CreatorText text{SplitInfo(l.info)};
            d.SetString("description", text.description);
            for (const auto& [label, url] : text.links) {
                Message& link{d.Add("links")};
                link.SetString("label", label);
                link.SetString("url", url);
            }
            d.Mutable("curve") = CurveOfLaunch(l.rec, l.decimals);
            d.SetDouble("trade_fee_bps", l.rec.fee / 100.0);
            d.SetString("create_txid", l.rec.txid.GetHex());
            // The first buy: the launch's own order, the first trade of its curve.
            if (bitassets::market::Enabled()) {
                std::vector<unsigned char> next;
                const auto first{bitassets::market::Trades(*tip.view, bitassets::market::Market{true, token}, l.rec.height + 1, {}, 16, next)};
                for (const auto& row : first) {
                    if (row.txid == l.rec.txid && row.asset_in.IsNull()) d.SetDouble("first_buy_ecx", Whole(row.amount_in - std::min(row.amount_in, row.back), 8));
                }
            }
            d.SetDouble("creator_share_percent", l.rec.creator_share / 10000.0);
            d.SetDouble("protocol_share_percent", l.rec.protocol_share / 10000.0);
            d.SetDouble("security_share_percent", l.rec.security_share / 10000.0);
            d.SetDouble("creator_fees_owed_ecx", static_cast<double>(l.rec.creator_fees) / COIN);
            d.SetDouble("creator_fees_paid_ecx", static_cast<double>(l.rec.creator_paid) / COIN);
            d.SetDouble("allocation_percent", l.rec.allocation / 100.0);
            d.SetBool("allocation_claimed", l.rec.allocation_claimed);
            d.SetDouble("min_buy_ecx", static_cast<double>(l.rec.min_buy) / COIN);
            d.SetDouble("max_buy_ecx", static_cast<double>(l.rec.max_buy) / COIN);
            d.SetDouble("curve_tokens_left", l.curve_left);
            (void)launch;
            return d;
        });

        Serve(server, "ListLaunchTrades", read, [this](const Message& req, const Tip& tip) {
            const auto [token, launch]{ResolveLaunch(tip.State(), req.GetString("id"))};
            return TradesOf(bitassets::market::Market{true, token}, req, tip);
        });
        Serve(server, "GetLaunchCandles", read, [this](const Message& req, const Tip& tip) {
            const auto [token, launch]{ResolveLaunch(tip.State(), req.GetString("id"))};
            return CandlesOf(bitassets::market::Market{true, token}, req, tip);
        });

        Serve(server, "ListLaunchHolders", read, [this](const Message& req, const Tip& tip) {
            if (!bitassets::market::Enabled()) Fail(Code::FAILED_PRECONDITION, "this node keeps no market index (-dexindex)");
            const bitassets::State state{tip.State()};
            const auto [token, launch]{ResolveLaunch(state, req.GetString("id"))};
            const uint32_t limit{req.GetUInt("limit") == 0 ? 20 : static_cast<uint32_t>(req.GetUInt("limit"))};
            if (limit > 100) Fail(Code::INVALID_ARGUMENT, "limit: 1 to 100");
            std::vector<unsigned char> cursor, next;
            if (!req.GetString("cursor").empty()) {
                const auto bytes{TryParseHex<unsigned char>(req.GetString("cursor"))};
                if (!bytes || bytes->size() > 256) Fail(Code::INVALID_ARGUMENT, "not a cursor of these holders");
                cursor = *bytes;
            }
            const auto record{state.GetAsset(token)};
            const double supply{static_cast<double>(record ? record->supply : launch.Supply())};
            const int decimals{record ? record->decimals : bitassets::LAUNCH_DECIMALS};
            Message page{New("HolderPage")};
            // What the curve, or the pool it graduated into, holds: no output carries it.
            if (cursor.empty()) {
                if (const auto pool{state.GetPool(launch.pool)}) {
                    const uint64_t held{launch.Graduated() ? pool->reserve1 : pool->reserve1 - std::min(pool->reserve1, launch.CurveEnd())};
                    if (held > 0) {
                        Message& h{page.Add("items")};
                        h.SetString("address", launch.Graduated() ? "pool:" + Hex(launch.pool) : "curve:" + Hex(launch.pool));
                        h.SetDouble("amount", Whole(held, decimals));
                        h.SetDouble("share", supply > 0 ? held / supply : 0);
                        h.SetString("tag", launch.Graduated() ? "pool" : "curve");
                    }
                }
            }
            for (const auto& holding : bitassets::market::Holders(*tip.view, token, bitassets::Token::Kind::ASSET, cursor, limit, next)) {
                Message& h{page.Add("items")};
                h.SetString("address", AddressOf(holding.script));
                h.SetDouble("amount", Whole(holding.amount, decimals));
                h.SetDouble("share", supply > 0 ? holding.amount / supply : 0);
                if (holding.script == launch.creator) h.SetString("tag", "creator");
            }
            if (!next.empty()) page.SetString("next_cursor", HexStr(next));
            page.SetUInt("total", bitassets::market::HolderCount(*tip.view, token, bitassets::Token::Kind::ASSET));
            return page;
        });

        Serve(server, "QuoteLaunch", expensive, [this](const Message& req, const Tip& tip) {
            return LaunchQuote(req.GetString("id"), req.GetString("side"), req.GetString("amount"), static_cast<uint32_t>(req.GetUInt("slippage_bps")), tip);
        });

        const auto logo{[this](const Message& req, const Tip& tip, bool launch) {
            const bitassets::State state{tip.State()};
            const AssetId asset{launch ? ResolveLaunch(state, req.GetString("id")).first : ResolveRegistered(state, req.GetString("id"))};
            const auto bytes{asset.IsNull() ? std::nullopt : state.GetLogoBytes(asset)};
            const auto record{asset.IsNull() ? std::nullopt : state.GetLogo(asset)};
            if (!bytes || !record) Fail(Code::NOT_FOUND, "no logo");
            // Served as an image only after its magic bytes say which (the chain checked its structure when it was set).
            std::string type;
            if (bytes->size() > 8 && std::equal(bytes->begin(), bytes->begin() + 8, std::begin({0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A}))) type = "image/png";
            if (bytes->size() > 3 && (*bytes)[0] == 0xFF && (*bytes)[1] == 0xD8 && (*bytes)[2] == 0xFF) type = "image/jpeg";
            if (type.empty()) Fail(Code::NOT_FOUND, "no logo of a known type");
            Message m{New("Logo")};
            m.SetString("data", std::string(bytes->begin(), bytes->end()));
            m.SetString("content_type", type);
            m.SetString("sha256", HexStr(record->hash));
            return m;
        }};
        Serve(server, "GetLaunchLogo", Policy{.cache_control = "public, max-age=300"}, [logo](const Message& req, const Tip& tip) { return logo(req, tip, true); });
        Serve(server, "GetAssetLogo", Policy{.cache_control = "public, max-age=300"}, [logo](const Message& req, const Tip& tip) { return logo(req, tip, false); });

        Serve(server, "GetLaunchStats", read, [this](const Message&, const Tip& tip) {
            const UniValue s{Rpc("getlaunchstats", UniValue{UniValue::VARR})};
            const auto table{Launches(tip)};
            const Consensus::SidechainParams& params{Params().GetConsensus().sidechain};
            Message m{New("LaunchStats")};
            m.SetUInt("launches", s["launches"].getInt<uint64_t>());
            m.SetUInt("on_curve", s["on_curve"].getInt<uint64_t>());
            m.SetUInt("graduated", s["graduated"].getInt<uint64_t>());
            m.SetDouble("volume_ecx", s["volume"].get_real());
            m.SetDouble("raised_ecx", s["raised"].get_real());
            const UniValue& rules{s["rules"]};
            m.SetDouble("graduation_ecx", rules["graduation"].get_real());
            m.SetDouble("fee_percent", rules["fee"].get_real());
            m.SetDouble("creator_share_percent", rules["creator_share"].get_real());
            m.Mutable("curve") = CurveOf(params.launch_graduation, params.launch_fee);
            m.SetBool("active", rules["active"].get_bool());
            uint32_t made{0}, graduated{0};
            double volume{0};
            ChainstateManager& chainman{*Assert(m_node.chainman)};
            for (const auto& l : table->launches) {
                if (l.created_time + 86400 >= tip.time) ++made;
                if (l.rec.Graduated()) {
                    const CBlockIndex* block{WITH_LOCK(::cs_main, return chainman.ActiveChain()[l.rec.graduated])};
                    if (block && block->GetBlockTime() + 86400 >= tip.time) ++graduated;
                }
                volume += l.day.volume;
            }
            m.SetUInt("launches_24h", made);
            m.SetUInt("graduated_24h", graduated);
            m.SetDouble("volume_24h", volume);
            return m;
        });

        Serve(server, "GetLaunchLog", read, [this](const Message& req, const Tip& tip) {
            const uint32_t from{static_cast<uint32_t>(req.GetUInt("from_height"))};
            const uint32_t to{req.GetUInt("to_height") == 0 ? from : static_cast<uint32_t>(req.GetUInt("to_height"))};
            if (to < from || to - from >= 1000) Fail(Code::INVALID_ARGUMENT, "from_height to to_height: at most 1000 blocks");
            UniValue params(UniValue::VARR);
            params.push_back(static_cast<int>(from));
            params.push_back(static_cast<int>(to));
            const UniValue rows{Rpc("getlaunchlog", params)};
            const bitassets::State state{tip.State()};
            Message log{New("LaunchLog")};
            for (const UniValue& r : rows.getValues()) {
                Message& m{log.Add("items")};
                m.SetUInt("height", r["height"].getInt<uint64_t>());
                const AssetId token{*uint256::FromHex(r["token"].get_str())};
                m.SetString("id", Hex(token));
                m.SetString("ticker", Label(state, token));
                m.SetBool("created", r["created"].get_bool());
                m.SetBool("graduated", r["graduated"].get_bool());
                m.SetUInt("orders", r["orders"].getInt<uint64_t>());
                for (const char* key : {"coin_in", "coin_out", "tokens_in", "tokens_out", "fees", "reserve_coin", "reserve_tokens"}) m.SetDouble(key, r[key].get_real());
            }
            return log;
        });

        Serve(server, "ListPools", read, [this](const Message& req, const Tip& tip) {
            // By liquidity: the pools with ECX (their depth in ECX); with a kind or a fee asked, newest first (all pools).
            const std::string sort_in{!req.GetString("sort").empty() ? req.GetString("sort") : (req.GetString("kind").empty() && req.GetUInt("fee_ppm") == 0) ? "liquidity" : "newest"};
            const std::map<std::string, std::string> sorts{{"liquidity", "depth"}, {"volume", "volume"}, {"change", "price_change"}, {"newest", "newest"}};
            if (!sorts.contains(sort_in)) Fail(Code::INVALID_ARGUMENT, "sort: liquidity, volume, change or newest");
            const std::string kind{req.GetString("kind")};
            const std::set<std::string> kinds{"", "constant_product", "stable", "weighted", "concentrated", "launch", "book"};
            if (!kinds.contains(kind)) Fail(Code::INVALID_ARGUMENT, "kind: constant_product, stable, weighted, concentrated, launch or book");
            const uint32_t limit{req.GetUInt("limit") == 0 ? 50 : static_cast<uint32_t>(req.GetUInt("limit"))};
            if (limit > 200) Fail(Code::INVALID_ARGUMENT, "limit: 1 to 200");
            const uint32_t fee{static_cast<uint32_t>(req.GetUInt("fee_ppm"))};
            if (req.GetString("q").size() > 64) Fail(Code::INVALID_ARGUMENT, "q: 64 characters at most");
            std::string cursor{req.GetString("cursor")};
            if (!cursor.empty() && (cursor.size() > 512 || !IsHex(cursor))) Fail(Code::INVALID_ARGUMENT, "not a cursor of these pools");
            Message page{New("PoolPage")};
            // The market index (searchpools), filtered by kind and fee: at most 5 of its pages for one of these.
            for (int round{0}; round < 5 && page.Count("items") < limit; ++round) {
                UniValue params(UniValue::VARR);
                params.push_back(req.GetString("q"));
                UniValue filters(UniValue::VOBJ);
                filters.pushKV("exclude_abandoned", !req.GetBool("include_abandoned"));
                filters.pushKV(bitassets::CoinOnlyFilter(), req.GetBool("ecx_only"));
                params.push_back(filters);
                params.push_back(sorts.at(sort_in));
                params.push_back(cursor);
                params.push_back(static_cast<int>(limit));
                const UniValue r{Rpc("searchpools", params)};
                for (const UniValue& p : r["pools"].getValues()) {
                    if (page.Count("items") >= limit) break;
                    const std::string& curve{p["curve"].get_str()};
                    if (kind.empty() ? curve == "launch" : curve != kind) continue;
                    if (fee != 0 && p["fee_millionths"].getInt<uint64_t>() != fee) continue;
                    page.Add("items") = PoolFromJson(p, tip);
                }
                if (r["partial"].get_bool()) page.SetBool("partial", true);
                cursor = r["next_cursor"].isStr() ? r["next_cursor"].get_str() : "";
                if (cursor.empty()) break;
            }
            if (!cursor.empty()) page.SetString("next_cursor", cursor);
            page.SetUInt("total", bitassets::index::GetInfo(*tip.view).pools);
            return page;
        });

        Serve(server, "GetPool", read, [this](const Message& req, const Tip& tip) {
            UniValue params(UniValue::VARR);
            if (!req.GetString("pool").empty()) {
                const std::string id{ToLower(req.GetString("pool"))};
                if (id.size() != 64 || !IsHex(id)) Fail(Code::INVALID_ARGUMENT, "pool: its id, 64 hex digits");
                params.push_back(id);
            } else {
                const bitassets::State state{tip.State()};
                params.push_back(ArgOf(ResolveRegistered(state, req.GetString("asset_a"))));
                params.push_back(ArgOf(ResolveRegistered(state, req.GetString("asset_b"))));
            }
            return PoolFromJson(Rpc("getpool", params), tip);
        });

        const auto pool_market{[](const Message& req) {
            const std::string id{ToLower(req.GetString("id"))};
            if (id.size() != 64 || !IsHex(id)) Fail(Code::INVALID_ARGUMENT, "id: a pool's id, 64 hex digits");
            return bitassets::market::Market{false, *uint256::FromHex(id)};
        }};
        Serve(server, "ListPoolTrades", read, [this, pool_market](const Message& req, const Tip& tip) { return TradesOf(pool_market(req), req, tip); });
        Serve(server, "GetPoolCandles", read, [this, pool_market](const Message& req, const Tip& tip) { return CandlesOf(pool_market(req), req, tip); });

        Serve(server, "ListPoolTicks", read, [this](const Message& req, const Tip& tip) {
            const std::string id{ToLower(req.GetString("pool"))};
            if (id.size() != 64 || !IsHex(id)) Fail(Code::INVALID_ARGUMENT, "pool: its id, 64 hex digits");
            const uint32_t limit{req.GetUInt("limit") == 0 ? 200 : static_cast<uint32_t>(req.GetUInt("limit"))};
            if (limit > 500) Fail(Code::INVALID_ARGUMENT, "limit: 1 to 500");
            UniValue params(UniValue::VARR);
            params.push_back(id);
            params.push_back(static_cast<int>(limit));
            const UniValue r{Rpc("getpoolticks", params)};
            Message m{New("PoolTicks")};
            m.SetString("pool", id);
            m.SetInt("tick", r["tick"].getInt<int64_t>());
            m.SetString("liquidity", r["liquidity"].get_str());
            m.SetInt("tick_spacing", r["tick_spacing"].getInt<int64_t>());
            const auto cl{tip.State().GetClState(*uint256::FromHex(id))};
            if (cl) m.SetString("sqrt_price_x96", UintToArith256(cl->sqrt_price).GetHex());
            for (const UniValue& t : r["ticks"].getValues()) {
                Message& tick{m.Add("items")};
                tick.SetInt("tick", t["tick"].getInt<int64_t>());
                tick.SetDouble("price", t["price"].get_real());
                tick.SetString("liquidity_net", t["liquidity_net"].get_str());
                tick.SetString("liquidity_gross", t["liquidity_gross"].get_str());
            }
            return m;
        });

        Serve(server, "GetPosition", read, [this](const Message& req, const Tip& tip) {
            const std::string id{ToLower(req.GetString("id"))};
            if (id.size() != 64 || !IsHex(id)) Fail(Code::INVALID_ARGUMENT, "id: a position's id, 64 hex digits");
            UniValue params(UniValue::VARR);
            params.push_back(id);
            const UniValue r{Rpc("getposition", params)};
            const bitassets::State state{tip.State()};
            Message m{New("Position")};
            m.SetString("id", id);
            m.SetString("pool", r["pool"].get_str());
            m.SetInt("tick_lower", r["tick_lower"].getInt<int64_t>());
            m.SetInt("tick_upper", r["tick_upper"].getInt<int64_t>());
            m.SetDouble("price_lower", r["price_lower"].get_real());
            m.SetDouble("price_upper", r["price_upper"].get_real());
            m.SetString("liquidity", r["liquidity"].get_str());
            m.SetDouble("amount_a", r["amount0"].get_real());
            m.SetDouble("amount_b", r["amount1"].get_real());
            m.SetDouble("fees_a", r["fees0"].get_real());
            m.SetDouble("fees_b", r["fees1"].get_real());
            m.SetBool("in_range", r["in_range"].get_bool());
            m.SetString("txid", r["txid"].get_str());
            m.SetUInt("height", r["height"].getInt<uint64_t>());
            m.SetString("asset_a", r["asset0"].get_str() == Coin() ? Coin() : r["asset0"].get_str());
            m.SetString("asset_b", r["asset1"].get_str() == Coin() ? Coin() : r["asset1"].get_str());
            (void)state;
            return m;
        });

        Serve(server, "GetBook", read, [this](const Message& req, const Tip& tip) {
            const bitassets::State state{tip.State()};
            const AssetId base{ResolveRegistered(state, req.GetString("base"))};
            const AssetId quote{req.GetString("quote").empty() ? AssetId{} : ResolveRegistered(state, req.GetString("quote"))};
            if (base == quote) Fail(Code::INVALID_ARGUMENT, "two different assets make a book");
            const uint32_t depth{req.GetUInt("depth") == 0 ? 20 : static_cast<uint32_t>(req.GetUInt("depth"))};
            if (depth > 100) Fail(Code::INVALID_ARGUMENT, "depth: 1 to 100");
            UniValue params(UniValue::VARR);
            params.push_back(ArgOf(base));
            params.push_back(ArgOf(quote));
            params.push_back(static_cast<int>(depth));
            const UniValue r{Rpc("getbook", params)};
            Message b{New("Book")};
            b.SetString("book", r["book"].get_str());
            b.SetString("base", Label(state, base));
            b.SetString("quote", Label(state, quote));
            for (const char* side : {"asks", "bids"}) {
                double total{0};
                uint32_t orders{0};
                for (const UniValue& l : r[side].getValues()) {
                    Message& level{b.Add(side)};
                    level.SetDouble("price", l["price"].get_real());
                    level.SetDouble("amount", l["amount"].get_real());
                    level.SetUInt("orders", l["orders"].getInt<uint64_t>());
                    if (l.exists("price_exact")) level.SetString("price_exact", l["price_exact"].get_str());
                    if (l.exists("amount_units")) level.SetString("amount_units", l["amount_units"].get_str());
                    total += l["amount"].get_real();
                    level.SetDouble("total", total);
                    orders += l["orders"].getInt<uint32_t>();
                }
                b.SetUInt(std::string{side} == "asks" ? "ask_orders" : "bid_orders", orders);
            }
            if (r.exists("best_ask")) b.SetDouble("best_ask", r["best_ask"].get_real());
            if (r.exists("best_bid")) b.SetDouble("best_bid", r["best_bid"].get_real());
            if (r.exists("best_ask") && r.exists("best_bid")) b.SetDouble("spread", r["best_ask"].get_real() - r["best_bid"].get_real());
            if (r.exists("best_ask_exact")) b.SetString("best_ask_exact", r["best_ask_exact"].get_str());
            if (r.exists("best_bid_exact")) b.SetString("best_bid_exact", r["best_bid_exact"].get_str());
            return b;
        });

        Serve(server, "ListAssets", read, [this](const Message& req, const Tip& tip) {
            const bitassets::State state{tip.State()};
            const uint32_t limit{req.GetUInt("limit") == 0 ? 50 : static_cast<uint32_t>(req.GetUInt("limit"))};
            if (limit > 100) Fail(Code::INVALID_ARGUMENT, "limit: 1 to 100");
            const std::string sort_in{req.GetString("sort").empty() ? "newest" : req.GetString("sort")};
            static const std::map<std::string, std::string> SORTS{{"newest", "newest"}, {"oldest", "oldest"}, {"name", "name"}, {"supply", "supply"}, {"volume", "volume"}, {"activity", "activity"}, {"holders", "newest"}};
            const auto sort{SORTS.find(sort_in)};
            if (sort == SORTS.end()) Fail(Code::INVALID_ARGUMENT, "sort: newest, oldest, name, supply, volume, holders or activity");
            const std::string market{req.GetString("market").empty() ? "all" : req.GetString("market")};
            if (market != "all" && market != "none" && market != "curve" && market != "pool" && market != "book" && market != "auction") {
                Fail(Code::INVALID_ARGUMENT, "market: all, none, curve, pool, book or auction");
            }
            const std::string cursor{req.GetString("cursor")};
            if (!cursor.empty() && (cursor.size() > 512 || !IsHex(cursor))) Fail(Code::INVALID_ARGUMENT, "not a cursor of this list");
            UniValue filters(UniValue::VOBJ);
            const UniValue found{SearchAssets(req.GetString("q"), sort->second, cursor, limit, filters)};
            Message page{New("AssetPage")};
            std::vector<Message> items;
            for (const UniValue& row : found["assets"].getValues()) {
                if (!row.exists("asset") || (row.exists("retired") && row["retired"].get_bool())) continue;
                Message m{New("AssetSummary")};
                FillAsset(m, row, state, tip, false);
                if (market != "all") {
                    const Message* mk{m.Sub("market")};
                    bool has{false};
                    for (size_t i{0}; mk && i < mk->Count("kinds"); ++i) has |= mk->StringAt("kinds", i) == market;
                    if (market == "none" ? (mk && mk->Count("kinds") > 0) : !has) continue;
                }
                items.push_back(std::move(m));
            }
            if (sort_in == "holders") std::stable_sort(items.begin(), items.end(), [](const Message& a, const Message& b) { return a.GetUInt("holders") > b.GetUInt("holders"); });
            for (Message& m : items) page.Add("items") = std::move(m);
            if (found["next_cursor"].isStr()) page.SetString("next_cursor", found["next_cursor"].get_str());
            page.SetUInt("total", bitassets::index::GetInfo(*tip.view).assets);
            return page;
        });

        Serve(server, "GetAsset", read, [this](const Message& req, const Tip& tip) {
            const bitassets::State state{tip.State()};
            const AssetId id{ResolveRegistered(state, req.GetString("id"))};
            if (id.IsNull()) Fail(Code::INVALID_ARGUMENT, Coin() + " is the chain's coin, not an asset of the registry");
            UniValue filters(UniValue::VOBJ);
            UniValue only(UniValue::VARR);
            only.push_back("0x" + id.GetHex());
            filters.pushKV("assets", only);
            filters.pushKV("exclude_retired", false);
            const UniValue found{SearchAssets("", "newest", "", 1, filters)};
            if (found["assets"].empty()) Fail(Code::NOT_FOUND, "no such asset");
            Message d{New("AssetDetail")};
            FillAsset(d, found["assets"][0], state, tip, true);
            const auto record{state.GetAsset(id)};
            const CreatorText text{SplitInfo(record && record->data.info ? *record->data.info : std::string{})};
            for (const auto& [label, url] : text.links) {
                Message& link{d.Add("links")};
                link.SetString("label", label);
                link.SetString("url", url);
            }
            if (state.GetLaunch(id)) {
                const auto table{Launches(tip)};
                if (const auto it{table->by_token.find(id)}; it != table->by_token.end()) FillLaunch(d.Mutable("launch"), table->launches[it->second], state);
            }
            size_t pools{0}, books{0};
            for (const auto& [pid, pool] : state.PoolsOf(id)) {
                const auto curve{state.GetPoolParams(pid).GetCurve()};
                if (curve == bitassets::Curve::LAUNCH) continue;
                if (curve == bitassets::Curve::BOOK) {
                    if (pool.shares <= bitassets::MIN_LIQUIDITY || books >= 10) continue;
                    ++books;
                    const AssetId other{pool.asset0 == id ? pool.asset1 : pool.asset0};
                    UniValue params(UniValue::VARR);
                    params.push_back(ArgOf(id));
                    params.push_back(ArgOf(other));
                    params.push_back(1);
                    const UniValue r{Rpc("getbook", params)};
                    Message& b{d.Add("books")};
                    b.SetString("id", Hex(pid));
                    b.SetString("base", Hex(id));
                    b.SetString("quote", other.IsNull() ? "" : Hex(other));
                    b.SetString("base_ticker", Label(state, id));
                    b.SetString("quote_ticker", Label(state, other));
                    uint32_t asks{0}, bids{0};
                    for (const auto& [oid, o] : state.BookSide(pid, true, bitassets::MAX_BOOK_SIDE, tip.height + 1)) asks += o.remaining > 0;
                    for (const auto& [oid, o] : state.BookSide(pid, false, bitassets::MAX_BOOK_SIDE, tip.height + 1)) bids += o.remaining > 0;
                    // Asks of the base: the side that sells asset0 when the base is asset0.
                    if (pool.asset0 != id) std::swap(asks, bids);
                    b.SetUInt("asks", asks);
                    b.SetUInt("bids", bids);
                    if (r.exists("best_ask")) b.SetDouble("best_ask", r["best_ask"].get_real());
                    if (r.exists("best_bid")) b.SetDouble("best_bid", r["best_bid"].get_real());
                    continue;
                }
                if (bitassets::amm::Abandoned(pool) || pools >= 50) continue;
                ++pools;
                UniValue params(UniValue::VARR);
                params.push_back(Hex(pid));
                d.Add("pools") = PoolFromJson(Rpc("getpool", params), tip);
            }
            for (const Txid& aid : state.AuctionsOf(id, 20)) {
                const auto auction{state.GetAuction(aid)};
                if (!auction || auction->base != id) continue;
                Message& a{d.Add("auctions")};
                const int dq{DecimalsOf(state, auction->quote)};
                a.SetString("id", aid.GetHex());
                a.SetDouble("base_amount", Whole(auction->base_amount, DecimalsOf(state, id)));
                a.SetString("quote", auction->quote.IsNull() ? "" : Hex(auction->quote));
                a.SetString("quote_ticker", Label(state, auction->quote));
                const int64_t h{tip.height + 1};
                const int64_t end{int64_t{auction->start_height} + auction->duration};
                const int64_t at{std::clamp<int64_t>(h, auction->start_height, end)};
                const double start_p{Whole(auction->start_price, dq)}, end_p{Whole(auction->end_price, dq)};
                a.SetDouble("price", auction->duration > 0 ? start_p - (start_p - end_p) * static_cast<double>(at - auction->start_height) / auction->duration : end_p);
                a.SetUInt("start_height", static_cast<uint32_t>(auction->start_height));
                a.SetUInt("end_height", static_cast<uint32_t>(end));
                a.SetString("status", auction->remaining == 0 ? "sold_out" : h < auction->start_height ? "upcoming" : h > end ? "ended" : "live");
                a.SetDouble("remaining", Whole(auction->remaining, DecimalsOf(state, id)));
            }
            Message& can{d.Mutable("can_curve")};
            const auto why{Params().GetConsensus().sidechain.LaunchParamsActive(tip.height + 1) ? WhyNoCurve(state, id) : std::optional<std::string>{"existing assets go on curves from launch_params_height: not yet on this network"}};
            can.SetBool("allowed", !why);
            if (why) can.SetString("reason", *why);
            return d;
        });

        Serve(server, "GetTicker", read, [this](const Message& req, const Tip& tip) {
            const std::string ticker{ToUpper(util::TrimString(req.GetString("ticker")))};
            Message m{New("TickerAvailability")};
            m.SetString("ticker", ticker.substr(0, 16));
            if (!bitassets::IsTicker(ticker) || ticker == Coin()) {
                m.SetString("reason", "invalid: 2 to 12 capital letters and digits");
                return m;
            }
            const bitassets::State state{tip.State()};
            const bool taken{state.GetAsset(bitassets::HashName(ticker)).has_value()};
            m.SetBool("available", !taken);
            // Reservations are commitments that hide their names: "available" can still lose a race.
            if (taken) m.SetString("reason", "taken");
            return m;
        });

        Serve(server, "GetHoldings", read, [this](const Message& req, const Tip& tip) {
            if (!bitassets::market::Enabled()) Fail(Code::FAILED_PRECONDITION, "this node keeps no market index (-dexindex)");
            const std::string address{util::TrimString(req.GetString("address"))};
            const CTxDestination dest{address.size() <= 128 ? DecodeDestination(address) : CNoDestination{}};
            if (!IsValidDestination(dest)) Fail(Code::INVALID_ARGUMENT, "not an address of this network");
            const bitassets::State state{tip.State()};
            const auto table{Launches(tip)};
            Message m{New("Holdings")};
            m.SetString("address", address);
            for (const auto& h : bitassets::market::HoldingsOf(*tip.view, GetScriptForDestination(dest), 500)) {
                using Kind = bitassets::Token::Kind;
                // Control coins, reservations and receipts are not holdings a page shows.
                if (h.kind != Kind::ASSET && h.kind != Kind::LP && h.kind != Kind::POSITION && h.kind != Kind::ORDER) continue;
                Message& item{m.Add("items")};
                item.SetString("asset_id", Hex(h.id));
                item.SetString("amount_units", util::ToString(h.amount));
                switch (h.kind) {
                case bitassets::Token::Kind::ASSET: {
                    const uint8_t d{DecimalsOf(state, h.id)};
                    item.SetString("kind", "asset");
                    item.SetString("ticker", Label(state, h.id));
                    item.SetDouble("amount", Whole(h.amount, d));
                    // Its worth in ECX: a launch's price, else the default pool's with ECX.
                    double price{0};
                    if (const auto it{table->by_token.find(h.id)}; it != table->by_token.end()) {
                        price = table->launches[it->second].price;
                    } else if (const auto pool{state.GetPool(bitassets::PoolId(bitassets::CHN, h.id))}) {
                        price = bitassets::market::PoolPrice(state, bitassets::PoolId(bitassets::CHN, h.id), *pool).value_or(0);
                    }
                    item.SetDouble("value_ecx", Whole(h.amount, d) * price);
                    break;
                }
                case bitassets::Token::Kind::LP:
                    item.SetString("kind", "lp");
                    item.SetString("ticker", "LP " + Hex(h.id).substr(0, 8));
                    item.SetDouble("amount", static_cast<double>(h.amount));
                    break;
                case bitassets::Token::Kind::POSITION:
                    item.SetString("kind", "position");
                    item.SetString("ticker", "Position " + Hex(h.id).substr(0, 8));
                    item.SetDouble("amount", 1);
                    break;
                case bitassets::Token::Kind::ORDER:
                    item.SetString("kind", "order");
                    item.SetString("ticker", "Order " + Hex(h.id).substr(0, 8));
                    item.SetDouble("amount", 1);
                    break;
                default:
                    break;
                }
            }
            return m;
        });

        Serve(server, "GetVerifications", read, [this](const Message& req, const Tip& tip) {
            const size_t assets{req.Count("assets")}, curators{req.Count("curators")};
            if (assets == 0 || assets > 50) Fail(Code::INVALID_ARGUMENT, "assets: 1 to 50");
            if (curators == 0 || curators > 20) Fail(Code::INVALID_ARGUMENT, "curators: 1 to 20 (the ones the client trusts: this API keeps no list of its own)");
            const bitassets::State state{tip.State()};
            UniValue named(UniValue::VARR);
            for (size_t i{0}; i < curators; ++i) {
                const std::string& c{req.StringAt("curators", i)};
                if (c.size() != 64 || !IsHex(c)) Fail(Code::INVALID_ARGUMENT, "curators: oracle ids or keys, 64 hex digits each");
                named.push_back(ToLower(c));
            }
            Message out{New("Verifications")};
            for (size_t i{0}; i < assets; ++i) {
                const AssetId asset{ResolveRegistered(state, req.StringAt("assets", i))};
                if (asset.IsNull()) Fail(Code::INVALID_ARGUMENT, CURRENCY_UNIT + " is the chain's coin: nothing to verify");
                UniValue params(UniValue::VARR);
                params.push_back(ArgOf(asset));
                params.push_back(named);
                const UniValue r{Rpc("getverifications", params)};
                Message& v{out.Add("items")};
                v.SetString("asset", Hex(asset));
                v.SetBool("verified", r["verified"].get_bool());
                for (const UniValue& c : r["curators"].getValues()) {
                    Message& w{v.Add("curators")};
                    w.SetString("curator", c["curator"].get_str());
                    w.SetString("name", c["name"].get_str());
                    w.SetBool("verified", c["verified"].get_bool());
                    w.SetUInt("round", c["round"].getInt<uint64_t>());
                    if (c.exists("height")) w.SetUInt("height", c["height"].getInt<uint64_t>());
                }
            }
            return out;
        });

        Serve(server, "QuoteSwap", expensive, [this](const Message& req, const Tip& tip) {
            return SwapQuote(req.GetString("from"), req.GetString("to"), req.GetString("amount"), static_cast<uint32_t>(req.GetUInt("slippage_bps")),
                             static_cast<uint32_t>(req.GetUInt("max_hops")), tip);
        });
        Serve(server, "QuoteLiquidity", expensive, [this](const Message& req, const Tip& tip) { return LiquidityQuote(req, tip); });

        Serve(server, "QuotePosition", expensive, [this](const Message& req, const Tip& tip) {
            const bitassets::State state{tip.State()};
            const std::string id{ToLower(req.GetString("pool"))};
            if (id.size() != 64 || !IsHex(id)) Fail(Code::INVALID_ARGUMENT, "pool: its id, 64 hex digits");
            const auto pool{state.GetPool(*uint256::FromHex(id))};
            if (!pool) Fail(Code::NOT_FOUND, "no such pool");
            const double lower{req.GetDouble("price_lower")}, upper{req.GetDouble("price_upper")};
            if (!(lower > 0) || !(upper > lower) || !std::isfinite(upper)) Fail(Code::INVALID_ARGUMENT, "price_lower and price_upper: 0 < lower < upper");
            const uint8_t d0{DecimalsOf(state, pool->asset0)}, d1{DecimalsOf(state, pool->asset1)};
            UniValue amounts(UniValue::VOBJ);
            if (!req.GetString("amount_a").empty()) amounts.pushKV(ArgOf(pool->asset0), Decimal(ParseAmount(req.GetString("amount_a"), d0, "amount_a"), d0, d0));
            if (!req.GetString("amount_b").empty()) amounts.pushKV(ArgOf(pool->asset1), Decimal(ParseAmount(req.GetString("amount_b"), d1, "amount_b"), d1, d1));
            if (amounts.empty()) Fail(Code::INVALID_ARGUMENT, "amount_a and/or amount_b: the most of each");
            UniValue params(UniValue::VARR);
            params.push_back(id);
            params.push_back(strprintf("%.17g", lower));
            params.push_back(strprintf("%.17g", upper));
            params.push_back(amounts);
            const UniValue r{Rpc("previewposition", params)};
            Message m{New("PositionQuote")};
            m.SetString("pool", id);
            m.SetInt("tick_lower", r["tick_lower"].getInt<int64_t>());
            m.SetInt("tick_upper", r["tick_upper"].getInt<int64_t>());
            m.SetDouble("price_lower", r["price_lower"].get_real());
            m.SetDouble("price_upper", r["price_upper"].get_real());
            m.SetString("liquidity", r["liquidity"].get_str());
            const uint64_t u0{UnitsOf(r["amount0"], d0)}, u1{UnitsOf(r["amount1"], d1)};
            m.SetDouble("amount_a", Whole(u0, d0));
            m.SetDouble("amount_b", Whole(u1, d1));
            m.SetString("amount_a_units", util::ToString(u0));
            m.SetString("amount_b_units", util::ToString(u1));
            m.SetBool("in_range", r["in_range"].get_bool());
            m.SetUInt("height", static_cast<uint32_t>(tip.height));
            return m;
        });

        Serve(server, "BuildAction", fresh, [this](const Message& req, const Tip& tip) { return Build(req, tip); });

        Serve(server, "Broadcast", Policy{.cache = false, .expensive = true, .cache_control = "no-store"}, [this](const Message& req, const Tip&) {
            const std::string& hex{req.GetString("hex")};
            if (hex.size() > 2 * MAX_BROADCAST_BYTES) Fail(Code::RESOURCE_EXHAUSTED, strprintf("a transaction of %u bytes at most", MAX_BROADCAST_BYTES));
            CMutableTransaction mtx;
            if (!DecodeHexTx(mtx, hex)) Fail(Code::INVALID_ARGUMENT, "not a transaction (hex)");
            // Coins burned only where the assets' rules take them: the marker of an operation of the DEX.
            for (const CTxOut& out : mtx.vout) {
                if ((out.scriptPubKey.IsUnspendable() || !out.scriptPubKey.HasValidOps()) && out.nValue > 0 && !bitassets::IsMarkerScript(out.scriptPubKey)) {
                    Fail(Code::FAILED_PRECONDITION, "the transaction burns coins outside an asset marker: refused");
                }
            }
            const CTransactionRef tx{MakeTransactionRef(std::move(mtx))};
            if (GetVirtualTransactionSize(*tx) > MAX_STANDARD_TX_WEIGHT / WITNESS_SCALE_FACTOR) Fail(Code::FAILED_PRECONDITION, "the transaction is larger than the mempool takes");
            std::string error;
            const CAmount max_fee{node::DEFAULT_MAX_RAW_TX_FEE_RATE.GetFee(GetVirtualTransactionSize(*tx))};
            const node::TransactionError result{node::BroadcastTransaction(m_node, tx, error, max_fee, node::TxBroadcast::MEMPOOL_AND_BROADCAST_TO_ALL, /*wait_callback=*/true)};
            if (result != node::TransactionError::OK) Fail(Code::FAILED_PRECONDITION, strprintf("refused: %s%s", common::TransactionErrorString(result).original, error.empty() ? "" : " (" + error + ")"));
            Message m{New("BroadcastReply")};
            m.SetString("txid", tx->GetHash().GetHex());
            return m;
        });
    }

    UniValue PegInfo() const
    {
        // The mainchain node is asked at most every 30 seconds, by one call at a time; the others take the last answer.
        LOCK(m_peg_mutex);
        const int64_t now{GetTime()};
        if (now - m_peg_time < 30) return m_peg;
        m_peg_time = now;
        try {
            m_peg = Rpc("getpeginfo", UniValue{UniValue::VARR});
        } catch (const std::exception&) {
            m_peg = UniValue{};
        }
        return m_peg;
    }

    const pb::Pool& m_pool;
    node::NodeContext& m_node;
    const Options m_options;
    RateLimiter m_limits;
    RateLimiter m_broadcast_limits;
    RateLimiter m_broadcast_global;
    mutable ReplyCache m_cache;
    mutable Gate m_gate;
    mutable Mutex m_table_mutex;
    mutable std::shared_ptr<const LaunchTable> m_table GUARDED_BY(m_table_mutex);
    mutable Mutex m_peg_mutex;
    mutable UniValue m_peg GUARDED_BY(m_peg_mutex);
    mutable int64_t m_peg_time GUARDED_BY(m_peg_mutex){0};
};

} // namespace

std::unique_ptr<Service> RegisterService(grpcsrv::Server& server, const pb::Pool& pool, node::NodeContext& node, const Options& options)
{
    return std::make_unique<ServiceImpl>(server, pool, node, options);
}

} // namespace publicapi
