// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/linkhost.h>

#include <bitassets/curves.h>
#include <bitassets/logo.h>
#include <bitassets/rpcutil.h>
#include <bitassets/state.h>
#include <chainparams.h>
#include <consensus/amount.h>
#include <crypto/sha256.h>
#include <interfaces/chain.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <logging.h>
#include <node/context.h>
#include <rpc/request.h>
#include <tinyformat.h>
#include <util/fs.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <webui/bridge.h>
#include <webui/confirm.h>
#include <webui/local.h>

#include <algorithm>
#include <fstream>

namespace webui::links {

namespace {
//! The label of the wallet's flush.markets address, where what a link's action brings in goes (one per wallet).
constexpr const char* LINK_LABEL{"flush.markets"};

std::string Short(const uint256& id)
{
    const std::string hex{id.GetHex()};
    return hex.substr(0, 8) + "…" + hex.substr(56);
}

/** An asset as the preface names it: its ticker (or the coin), its display name and its id, short (the describer's row adds its number). */
std::string Named(const Reader::Asset& asset)
{
    std::string out{asset.ticker.empty() ? std::string{"(no published name)"} : asset.ticker};
    if (!asset.display_name.empty()) out += " — " + asset.display_name;
    return out + strprintf(" (0x%s)", Short(asset.id));
}

/** The slippage, in hundredths of a percent, that makes the RPC's floor (quote less it, rounded down) exactly at least `floor`. */
UniValue SlippageFor(uint64_t quote, uint64_t floor)
{
    const auto bps{static_cast<uint64_t>((static_cast<unsigned __int128>(quote - floor) * 10000) / quote)};
    UniValue out;
    out.setNumStr(strprintf("%d.%02d", bps / 100, bps % 100));
    return out;
}

std::string Bps(int bps) { return strprintf("%d.%02d %%", bps / 100, bps % 100); }

std::vector<dialog::Row> Head(const Request& request, Source source, const std::string& summary, int64_t now)
{
    std::vector<dialog::Row> rows;
    rows.push_back(confirm::Heading(source == Source::FILE ? "From a launch file" : "From a link", SourceText(source)));
    rows.push_back(confirm::Text("It asks", summary));
    if (request.expires > 0) {
        const int64_t left{std::max<int64_t>(0, request.expires - now)};
        rows.push_back(confirm::Text("Link good until", strprintf("%s (in %d min)", FormatISO8601DateTime(request.expires), (left + 59) / 60)));
    }
    return rows;
}

dialog::Row Warning()
{
    return confirm::Note("Anyone can make a link like this, and any page can open one: it says nothing true of who made it. Accept only if you asked for "
                         "this on flush.markets yourself just now, and the asset, the pool and the amounts below are the ones you meant.");
}

std::string Message(Source source)
{
    return strprintf("%s (%s) asks the app for this. The app read it itself, resolved every id and quoted it again from its own node: "
                     "below is exactly what would happen. Nothing is done unless you accept.",
                     source == Source::FILE ? "A launch file" : "A link", SourceText(source));
}

std::string Coin(uint64_t units) { return Decimal(units, bitassets::CHN_DECIMALS) + " " + std::string{COIN_TICKER}; }
std::string Of(uint64_t units, const Reader::Asset& asset) { return Decimal(units, asset.decimals) + " " + (asset.ticker.empty() ? "units" : asset.ticker); }

uint64_t Exact(const std::string& text, int decimals, const char* what)
{
    const auto units{Units(text, decimals, Rounding::EXACT)};
    if (!units || *units == 0) throw LinkError(strprintf("%s: %s has at most %d places here", what, text, decimals));
    return *units;
}

uint64_t Floor(const std::string& text, int decimals)
{
    const auto units{Units(text, decimals, Rounding::UP)};
    if (!units || *units == 0) throw LinkError(strprintf("The least it may give (%s) is not an amount", text));
    return *units;
}

Reader::Asset Token(Reader& reader, const uint256& id, const std::string& ticker)
{
    const auto asset{reader.AssetById(id)};
    if (!asset) throw LinkError(strprintf("No asset has the id %s on this chain: nothing was done", id.GetHex()));
    // Ticker and id together: a ticker retired and taken again names another token, and must not redirect a trade.
    if (asset->ticker != ticker) {
        throw LinkError(strprintf("The link names %s, and the asset of id %s is %s: refused (the ticker and the id must be of one asset)", ticker, Short(id),
                                  asset->ticker.empty() ? std::string{"without a public name"} : asset->ticker));
    }
    return *asset;
}

/** The coin as an asset of the preface. */
Reader::Asset CoinAsset()
{
    Reader::Asset coin;
    coin.ticker = std::string{COIN_TICKER};
    coin.decimals = bitassets::CHN_DECIMALS;
    return coin;
}

std::string Ref(const uint256& id) { return id.IsNull() ? bitassets::AssetLabel(bitassets::CHN, nullptr) : "0x" + id.GetHex(); }

/** A side of a link (pool, auction, order): the coin, or the asset of the id, whose ticker must be the link's. */
Reader::Asset Side(Reader& reader, const uint256& id, const std::string& ticker)
{
    if (id.IsNull()) return CoinAsset();
    return Token(reader, id, ticker);
}

std::string SideName(const Reader::Asset& asset) { return asset.id.IsNull() ? std::string{COIN_TICKER} + " (the chain's coin)" : Named(asset); }

/** A fee in basis points as the RPCs take it: a percent. */
UniValue Percent(int bps)
{
    UniValue out;
    out.setNumStr(strprintf("%d.%02d", bps / 100, bps % 100));
    return out;
}

/** The info of an asset from a link: its description, then its links, a line each. */
std::string InfoOf(const Request& r)
{
    std::string info{r.description};
    for (const std::string& link : r.links) info += (info.empty() ? "" : "\n") + link;
    return info;
}

/** The row of a link's logo: where its bytes came from (the file chosen, or downloaded from the link's logo_url), and that its hash is the link's. */
dialog::Row LogoRow(const Request& r, const local::Logo& logo)
{
    if (r.logo_downloaded) return confirm::Text("Logo", "downloaded from " + UrlHost(r.logo_url) + "; its SHA-256 is the one the link names (" + logo.hash.substr(0, 16) + "…)");
    return confirm::Text("Logo", "the file's SHA-256 is the one the request names (" + logo.hash.substr(0, 16) + "…)");
}

/** A link's logo, checked against the hash it names (the host asked for the file); nullopt if none. */
std::optional<local::Logo> CheckedLogo(const Request& r)
{
    if (!r.logo_sha256) return std::nullopt;
    if (r.logo.empty()) throw LinkError("The logo the link names was not given");
    unsigned char hash[CSHA256::OUTPUT_SIZE];
    CSHA256().Write(r.logo.data(), r.logo.size()).Finalize(hash);
    if (HexStr(hash) != *r.logo_sha256) throw LinkError("The logo given is not the one the link names (its SHA-256 differs): nothing was done");
    std::string reason;
    auto logo{local::ReadLogo(r.logo, reason)};
    if (!logo) throw LinkError(strprintf("The rules refuse this logo (%s): a PNG or JPEG of exactly %ux%u pixels, at most %u bytes, without metadata", reason, bitassets::LOGO_SIZE, bitassets::LOGO_SIZE, bitassets::MAX_LOGO_BYTES));
    return logo;
}

/** The units of a supply of whole units at `decimals`, refused past what an asset can have. */
uint64_t SupplyUnits(const std::string& whole, int decimals)
{
    const auto units{Units(whole, decimals, Rounding::EXACT, bitassets::MAX_AMOUNT)};
    if (!units || *units == 0) throw LinkError(strprintf("supply: %s at %d decimals is more than an asset can have", whole, decimals));
    return *units;
}

void QuoteRows(std::vector<dialog::Row>& rows, const std::string& quote, const std::string& floor, const std::optional<int>& slippage)
{
    rows.push_back(confirm::Text("Quote now", quote + ", alone in its block (this app's node, now)"));
    rows.push_back(confirm::Text("Least it gives", floor + ": the link's floor — refused rather than less"));
    if (slippage) rows.push_back(confirm::Text("Slippage the site showed", Bps(*slippage) + " (shown only: the floor binds)"));
}

/** A pool's two assets, by id: the coin, or the asset of the id. */
std::pair<Reader::Asset, Reader::Asset> PoolSides(Reader& reader, const uint256& id, Reader::Pool& pool_out)
{
    const auto pool{reader.PoolById(id)};
    if (!pool) throw LinkError(strprintf("No pool has the id %s on this chain: nothing was done", id.GetHex()));
    pool_out = *pool;
    const auto side{[&](const uint256& asset_id) {
        if (asset_id.IsNull()) return CoinAsset();
        const auto asset{reader.AssetById(asset_id)};
        if (!asset) throw LinkError("The pool holds an asset this node does not know");
        return *asset;
    }};
    return {side(pool->asset0), side(pool->asset1)};
}

/** A link's two tickers of a pool, checked against its two assets in the pool's order (asset_a its first). */
void CheckPoolTickers(const Request& r, const Reader::Asset& a, const Reader::Asset& b)
{
    if (r.ticker_a != a.ticker || r.ticker_b != b.ticker) {
        throw LinkError(strprintf("The pool %s is of %s and %s, in that order: the link says %s and %s. Refused (the pool's id binds its assets).", Short(r.pool),
                                  a.ticker.empty() ? "?" : a.ticker, b.ticker.empty() ? "?" : b.ticker, r.ticker_a, r.ticker_b));
    }
}

/** A link's two tickers of a pool in either order: the pool's two assets as the link names them (asset_a, asset_b). */
std::pair<Reader::Asset, Reader::Asset> Oriented(const Request& r, const Reader::Asset& a, const Reader::Asset& b)
{
    if (r.ticker_a == a.ticker && r.ticker_b == b.ticker) return {a, b};
    if (r.ticker_a == b.ticker && r.ticker_b == a.ticker) return {b, a};
    throw LinkError(strprintf("The pool %s is of %s and %s: the link says %s and %s. Refused (the pool's id binds its assets).", Short(r.pool),
                              a.ticker.empty() ? "?" : a.ticker, b.ticker.empty() ? "?" : b.ticker, r.ticker_a, r.ticker_b));
}

/** A number of a node's reply (exact: its text), in units of `decimals` places; 0 if none. */
uint64_t UnitsOf(const UniValue& v, int decimals)
{
    if (!v.isNum() && !v.isStr()) return 0;
    return Units(v.getValStr(), decimals, Rounding::EXACT).value_or(0);
}

/** The value of an object by the asset's label as the node writes it (its ticker, the coin's), or by its ref. */
const UniValue& ByAsset(const UniValue& obj, const Reader::Asset& asset)
{
    if (!obj.isObject()) return NullUniValue;
    for (const std::string& key : {asset.id.IsNull() ? std::string{COIN_TICKER} : asset.ticker, Ref(asset.id), asset.id.IsNull() ? std::string{} : asset.id.GetHex()}) {
        if (key.empty()) continue;
        const UniValue& v{obj.find_value(key)};
        if (!v.isNull()) return v;
    }
    return NullUniValue;
}

/** The least of two slippages that each make their RPC floor at least the link's (a side with no floor asks none). */
UniValue SlippageForBoth(uint64_t quote_a, uint64_t floor_a, uint64_t quote_b, uint64_t floor_b)
{
    const auto bps{[](uint64_t q, uint64_t f) -> uint64_t { return f == 0 || q == 0 ? 10000 : static_cast<uint64_t>((static_cast<unsigned __int128>(q - f) * 10000) / q); }};
    const uint64_t least{std::min(bps(quote_a, floor_a), bps(quote_b, floor_b))};
    UniValue out;
    out.setNumStr(strprintf("%d.%02d", least / 100, least % 100));
    return out;
}
} // namespace

std::string SourceText(Source source)
{
    switch (source) {
    case Source::BROWSER: return "opened in your browser — check it";
    case Source::PASTED: return "pasted or dropped into the app — check it";
    case Source::FILE: return "you opened it — check it";
    }
    return "";
}

Plan Resolve(const Request& r, Source source, Reader& reader, int64_t now)
{
    Plan plan;
    plan.request = r;
    plan.message = Message(source);
    std::vector<dialog::Row> rows;
    switch (r.verb) {
    case Verb::BUY:
    case Verb::SELL: {
        const bool buy{r.verb == Verb::BUY};
        const Reader::Asset token{Token(reader, r.asset, r.ticker)};
        const auto launch{reader.LaunchOf(r.asset)};
        if (!launch) throw LinkError(strprintf("%s is not on a curve: it has none to trade with", r.ticker));
        if (launch->graduated) throw LinkError(strprintf("%s graduated: it trades in its pool now (ask the site for a swap)", r.ticker));
        const uint64_t amount{buy ? Exact(r.amount, bitassets::CHN_DECIMALS, "ecx") : Exact(r.amount, token.decimals, "tokens")};
        if (buy && amount < reader.MinChnPayout()) throw LinkError(strprintf("A buy pays %s at least", Coin(reader.MinChnPayout())));
        const uint64_t floor{Floor(r.floor, buy ? token.decimals : bitassets::CHN_DECIMALS)};
        const uint64_t quote{reader.QuoteLaunch(r.asset, buy, amount)};
        const std::string quoted{buy ? Of(quote, token) : Coin(quote)};
        const std::string floored{buy ? Of(floor, token) : Coin(floor)};
        if (quote == 0 || (!buy && quote < reader.MinChnPayout())) throw LinkError(strprintf("The curve of %s takes no such %s now", r.ticker, buy ? "buy" : "sale"));
        if (quote < floor) {
            throw LinkError(strprintf("The curve moved: %s now gives %s, less than the %s the link asks at least. Nothing was done: quote it again on the site.",
                                      buy ? "the buy" : "the sale", quoted, floored));
        }
        plan.summary = buy ? strprintf("Buy %s with %s", r.ticker, Coin(amount)) : strprintf("Sell %s", Of(amount, token));
        rows = Head(r, source, plan.summary, now);
        rows.push_back(confirm::Text("Asset", Named(token)));
        QuoteRows(rows, quoted, floored, r.slippage_bps);
        plan.method = buy ? "buyfromlaunch" : "selltolaunch";
        plan.params.push_back(Ref(r.asset));
        plan.params.push_back(buy ? Decimal(amount, bitassets::CHN_DECIMALS) : Decimal(amount, token.decimals));
        plan.params.push_back(SlippageFor(quote, floor));
        plan.params.push_back(buy ? Decimal(floor, token.decimals) : Decimal(floor, bitassets::CHN_DECIMALS));
        break;
    }
    case Verb::SWAP: {
        const auto pool{reader.PoolById(r.pool)};
        if (!pool) throw LinkError(strprintf("No pool has the id %s on this chain: nothing was done", r.pool.GetHex()));
        if (pool->abandoned) throw LinkError("Nobody provides liquidity to this pool any more: a swap would get almost nothing");
        const auto side{[&](const uint256& id) {
            if (id.IsNull()) return CoinAsset();
            const auto asset{reader.AssetById(id)};
            if (!asset) throw LinkError("The pool holds an asset this node does not know");
            return *asset;
        }};
        const Reader::Asset a{side(pool->asset0)}, b{side(pool->asset1)};
        // From and to: the pool's two assets, by their tickers (the pool's id binds them; a ticker that is not one of its two is refused).
        const auto pick{[&](const std::string& ticker, const char* what) {
            if (a.ticker == ticker && b.ticker != ticker) return a;
            if (b.ticker == ticker && a.ticker != ticker) return b;
            throw LinkError(strprintf("The pool %s is of %s and %s: the link's %s %s is not one of them", Short(r.pool), a.ticker.empty() ? "?" : a.ticker, b.ticker.empty() ? "?" : b.ticker, what, ticker));
        }};
        const Reader::Asset from{pick(r.from, "from")}, to{pick(r.to, "to")};
        const uint64_t amount{Exact(r.amount, from.decimals, "amount")};
        if (from.id.IsNull() && amount < reader.MinChnPayout()) throw LinkError(strprintf("A swap pays in %s at least", Coin(reader.MinChnPayout())));
        const uint64_t floor{Floor(r.floor, to.decimals)};
        const uint64_t quote{reader.QuotePool(from.id, r.pool, amount)};
        if (quote == 0 || (to.id.IsNull() && quote < reader.MinChnPayout())) throw LinkError("The pool takes no such swap now");
        if (quote < floor) {
            throw LinkError(strprintf("The pool moved: the swap now gives %s, less than the %s the link asks at least. Nothing was done: quote it again on the site.", Of(quote, to), Of(floor, to)));
        }
        plan.summary = strprintf("Swap %s for %s", Of(amount, from), to.ticker);
        rows = Head(r, source, plan.summary, now);
        rows.push_back(confirm::Text("Pays in", from.id.IsNull() ? std::string{COIN_TICKER} + " (the chain's coin)" : Named(from)));
        rows.push_back(confirm::Text("Gets", to.id.IsNull() ? std::string{COIN_TICKER} + " (the chain's coin)" : Named(to)));
        QuoteRows(rows, Of(quote, to), Of(floor, to), r.slippage_bps);
        plan.method = "swaproute";
        UniValue path{UniValue::VARR};
        path.push_back(Ref(from.id));
        path.push_back(Ref(to.id));
        UniValue pools{UniValue::VARR};
        pools.push_back(r.pool.GetHex());
        plan.params.push_back(std::move(path));
        plan.params.push_back(Decimal(amount, from.decimals));
        plan.params.push_back(SlippageFor(quote, floor));
        plan.params.push_back(Decimal(floor, to.decimals));
        plan.params.push_back(std::move(pools));
        break;
    }
    case Verb::LAUNCH: {
        if (!reader.LaunchesActive()) throw LinkError("This chain takes no launches yet");
        if (reader.NameTaken(r.ticker)) throw LinkError(strprintf("%s is taken: an asset of this ticker is registered. Nothing was done.", r.ticker));
        UniValue options{UniValue::VOBJ};
        options.pushKV("display_name", r.name);
        const std::string info{InfoOf(r)};
        if (!info.empty()) options.pushKV("info", info);
        const std::optional<local::Logo> logo{CheckedLogo(r)};
        if (logo) options.pushKV("logo", EncodeBase64(r.logo));
        // Its parameters (a launch with parameters only where they apply): the supply and decimals, the fee, the allocation.
        const int decimals{r.decimals.value_or(bitassets::LAUNCH_DECIMALS)};
        uint64_t supply{0};
        const bool with_params{!r.supply.empty() || r.fee_bps};
        if (with_params && !reader.LaunchParamsActive()) throw LinkError("This chain takes no launches with a supply or a fee of their own yet: nothing was done");
        if (!r.supply.empty()) {
            supply = SupplyUnits(r.supply, decimals);
            if (!bitassets::IsLaunchSupply(supply)) {
                throw LinkError(strprintf("A curve's supply is a multiple of 8 units, %s to %s at %d decimals: %s is not", Decimal(bitassets::MIN_LAUNCH_SUPPLY, decimals),
                                          Decimal(bitassets::MAX_LAUNCH_SUPPLY, decimals), decimals, r.supply));
            }
            options.pushKV("supply", r.supply);
            options.pushKV("decimals", decimals);
        }
        const uint32_t fee{r.fee_bps ? static_cast<uint32_t>(*r.fee_bps) * 100 : 0};
        if (r.fee_bps) options.pushKV("fee", Percent(*r.fee_bps));
        if (r.allocation_bps && *r.allocation_bps > 0) options.pushKV("allocation", Percent(*r.allocation_bps));
        if (r.keep_control) {
            if (!reader.FeeFlowActive()) throw LinkError("This chain takes no launch that keeps its control coin yet: nothing was done");
            options.pushKV("keep_control", true);
        }
        // Upgrade v7: the link's split of the tunable part of the fees (a launchpad's share): every recipient shown in full,
        // accepted or declined with the whole launch (the app does not edit it).
        std::vector<dialog::Row> split_rows;
        if (!r.split.empty()) {
            if (!reader.FeeSplitActive()) throw LinkError("This chain takes no fee split yet (upgrade v7): nothing was done");
            std::vector<bitassets::FeeShare> shares;
            UniValue split(UniValue::VARR);
            const uint32_t tunable{(bitassets::FEE_DENOMINATOR - reader.LaunchSecurityShare()) / 10'000};
            const uint32_t fee_bps{r.fee_bps ? static_cast<uint32_t>(*r.fee_bps) : reader.LaunchFee() / 100};
            for (const auto& [to, bps] : r.split) {
                bitassets::FeeShare share;
                share.bps = static_cast<uint16_t>(bps);
                std::string label;
                if (to == "creator") {
                    share.kind = bitassets::FeeShare::CREATOR;
                    label = "Creator (this wallet, claimed with claimlaunch)";
                } else if (to == "burn") {
                    share.kind = bitassets::FeeShare::BURN;
                    label = "Buyback and burn";
                } else {
                    const CTxDestination dest{DecodeDestination(to)};
                    if (!IsValidDestination(dest)) throw LinkError("split: " + to.substr(0, 128) + " is not an address of this chain");
                    share.kind = bitassets::FeeShare::ADDRESS;
                    share.script = GetScriptForDestination(dest);
                    label = "Paid to " + EncodeDestination(dest) + " (not this wallet's: the link's or launch file's, e.g. the launchpad's fee)";
                }
                shares.push_back(share);
                UniValue entry(UniValue::VOBJ);
                entry.pushKV("to", to);
                entry.pushKV("bps", bps);
                split.push_back(std::move(entry));
                // Its share of the tunable part, and of every trade's fee on the curve (fee_bps x tunable % x share).
                const double of_trade{fee_bps / 100.0 * tunable / 100.0 * bps / 10'000.0};
                split_rows.push_back(confirm::Text("Fee share", strprintf("%s: %s of the creator's %u %% of the fees (%.4f %% of each trade)", label, Bps(bps), tunable, of_trade)));
            }
            if (!bitassets::CheckFeeSplit(shares)) throw LinkError("split: the shares sum to 10000 basis points, creator and burn at most once each, up to 3 different addresses");
            options.pushKV("fee_split", std::move(split));
        }
        std::string quoted, floored;
        if (!r.amount.empty()) {
            const uint64_t ecx{Exact(r.amount, bitassets::CHN_DECIMALS, "ecx")};
            if (ecx < reader.MinChnPayout()) throw LinkError(strprintf("The first buy pays %s at least", Coin(reader.MinChnPayout())));
            const uint64_t floor{Floor(r.floor, decimals)};
            const uint64_t quote{reader.QuoteFirstBuy(ecx, supply, fee)};
            quoted = Decimal(quote, decimals) + " " + r.ticker;
            floored = Decimal(floor, decimals) + " " + r.ticker;
            if (quote < floor) throw LinkError(strprintf("The first buy of %s would get %s, less than the %s the link asks at least: refused (the site's quote is not the chain's)", Coin(ecx), quoted, floored));
            options.pushKV("first_buy", Decimal(ecx, bitassets::CHN_DECIMALS));
            options.pushKV("first_min_out", Decimal(floor, decimals));
            plan.summary = strprintf("Launch %s (%s) and buy with %s", r.ticker, r.name, Coin(ecx));
        } else {
            plan.summary = strprintf("Launch %s (%s)", r.ticker, r.name);
        }
        rows = Head(r, source, plan.summary, now);
        if (logo) rows.push_back(LogoRow(r, *logo));
        if (!r.supply.empty()) rows.push_back(confirm::Text("Supply", strprintf("%s %s (%d decimals): 3/4 sold on the curve, 1/4 into the locked pool at graduation", r.supply, r.ticker, decimals)));
        if (r.fee_bps) rows.push_back(confirm::Text("Curve's fee", Bps(*r.fee_bps) + " of each trade (creator and security parts as the network's)"));
        rows.insert(rows.end(), split_rows.begin(), split_rows.end());
        // 7.0.1: the link asks to keep the control coin: shown as loud as the split, accepted or declined with the whole launch.
        if (r.keep_control) {
            rows.push_back(confirm::Heading("Unlocked Supply: you keep the control coin and can mint more"));
            rows.push_back(confirm::Text("Control coin", "Kept by this wallet: it can create more units at any time. Its pages tag this launch \"Unlocked Supply\". Decline if you did not choose this on the site."));
        }
        if (!quoted.empty()) QuoteRows(rows, quoted, floored, r.slippage_bps);
        plan.method = "reserveasset";
        plan.params.push_back(r.ticker);
        plan.then_method = "launchtoken";
        plan.then_params.push_back(r.ticker);
        plan.then_params.push_back(std::move(options));
        break;
    }
    case Verb::REGISTER: {
        if (reader.NameTaken(r.ticker)) throw LinkError(strprintf("%s is taken: an asset of this ticker is registered. Nothing was done.", r.ticker));
        const int decimals{*r.decimals};
        SupplyUnits(r.supply, decimals);
        const std::optional<local::Logo> logo{CheckedLogo(r)};
        plan.summary = strprintf("Register %s%s: %s of %d decimals to this wallet, no market", r.ticker, r.name.empty() ? "" : " (" + r.name + ")", r.supply, decimals);
        rows = Head(r, source, plan.summary, now);
        if (logo) rows.push_back(LogoRow(r, *logo));
        rows.push_back(confirm::Text("Control", "this wallet gets the control coin: it mints more, changes the asset's data, opens markets"));
        plan.method = "reserveasset";
        plan.params.push_back(r.ticker);
        plan.then_method = "registerasset";
        plan.then_params.push_back(r.ticker);
        plan.then_params.push_back(r.supply);
        plan.then_params.push_back(decimals);
        UniValue data{UniValue::VOBJ};
        const std::string info{InfoOf(r)};
        if (!info.empty()) data.pushKV("info", info);
        plan.then_params.push_back(std::move(data));
        plan.then_params.push_back(true);
        plan.then_params.push_back(logo ? UniValue{EncodeBase64(r.logo)} : UniValue{});
        plan.then_params.push_back(r.name.empty() ? UniValue{} : UniValue{r.name});
        break;
    }
    case Verb::CURVE: {
        if (!reader.LaunchParamsActive()) throw LinkError("This chain does not put existing assets on curves yet: nothing was done");
        const Reader::Asset asset{Token(reader, r.asset, r.ticker)};
        if (reader.LaunchOf(r.asset)) throw LinkError(strprintf("%s was launched or put on a curve already", r.ticker));
        if (reader.PoolById(bitassets::PoolId(bitassets::CHN, r.asset))) throw LinkError(strprintf("%s has a pool with %s already: a curve would graduate into it", r.ticker, std::string{COIN_TICKER}));
        const uint64_t amount{Exact(r.amount, asset.decimals, "amount")};
        if (!bitassets::IsLaunchSupply(amount)) {
            throw LinkError(strprintf("A curve takes a multiple of 8 units, %s to %s %s", Decimal(bitassets::MIN_LAUNCH_SUPPLY, asset.decimals), Decimal(bitassets::MAX_LAUNCH_SUPPLY, asset.decimals), r.ticker));
        }
        UniValue options{UniValue::VOBJ};
        options.pushKV("fee", Percent(*r.fee_bps));
        if (!r.min_buy.empty()) options.pushKV("min_buy", Decimal(Exact(r.min_buy, bitassets::CHN_DECIMALS, "min_buy"), bitassets::CHN_DECIMALS));
        if (!r.max_buy.empty()) options.pushKV("max_buy", Decimal(Exact(r.max_buy, bitassets::CHN_DECIMALS, "max_buy"), bitassets::CHN_DECIMALS));
        plan.summary = strprintf("Put %s on a bonding curve at %s", Of(amount, asset), Bps(*r.fee_bps));
        rows = Head(r, source, plan.summary, now);
        rows.push_back(confirm::Text("Asset", Named(asset)));
        rows.push_back(confirm::Text("On the curve", strprintf("%s sold for %s, %s into the locked pool at graduation", Of(amount / 8 * 6, asset), std::string{COIN_TICKER}, Of(amount / 8 * 2, asset))));
        plan.method = "curveasset";
        plan.params.push_back(Ref(r.asset));
        plan.params.push_back(Decimal(amount, asset.decimals));
        plan.params.push_back(std::move(options));
        break;
    }
    case Verb::POOL: {
        const Reader::Asset a{Side(reader, r.asset_a, r.ticker_a)}, b{Side(reader, r.asset_b, r.ticker_b)};
        const uint64_t amount_a{Exact(r.amount_a, a.decimals, "amount_a")}, amount_b{Exact(r.amount_b, b.decimals, "amount_b")};
        UniValue kind{UniValue::VOBJ};
        kind.pushKV("curve", r.kind == "constant" ? "constant_product" : r.kind);
        UniValue fee;
        fee.setNumStr(strprintf("%d.%04d", r.fee_ppm / 10000, r.fee_ppm % 10000));
        kind.pushKV("fee", fee);
        if (r.amp) kind.pushKV("amp", *r.amp);
        if (r.weight_a) kind.pushKV("weight_a", *r.weight_a);
        plan.summary = r.kind == "concentrated" ? strprintf("Create a concentrated pool of %s and %s at the price of %s for %s (nothing put in: a position next)", a.ticker, b.ticker, Of(amount_b, b), Of(amount_a, a))
                                                : strprintf("Create a %s pool of %s and %s", r.kind == "constant" ? "constant-product" : r.kind, Of(amount_a, a), Of(amount_b, b));
        rows = Head(r, source, plan.summary, now);
        rows.push_back(confirm::Text("First asset", SideName(a)));
        rows.push_back(confirm::Text("Second asset", SideName(b)));
        plan.method = "createpool";
        plan.params.push_back(Ref(r.asset_a));
        plan.params.push_back(Decimal(amount_a, a.decimals));
        plan.params.push_back(Ref(r.asset_b));
        plan.params.push_back(Decimal(amount_b, b.decimals));
        plan.params.push_back(std::move(kind));
        break;
    }
    case Verb::AUCTION: {
        const Reader::Asset base{Side(reader, r.asset_a, r.ticker_a)}, quote{Side(reader, r.asset_b, r.ticker_b)};
        const uint64_t amount{Exact(r.amount, base.decimals, "amount")};
        const uint64_t start{Exact(r.start_price, quote.decimals, "start_price")}, end{Exact(r.end_price, quote.decimals, "end_price")};
        if (end > start) throw LinkError("end_price is above start_price: a Dutch auction's price falls");
        plan.summary = strprintf("Auction %s for %s falling to %s over %d blocks", Of(amount, base), Of(start, quote), Of(end, quote), r.blocks);
        rows = Head(r, source, plan.summary, now);
        rows.push_back(confirm::Text("Sells", SideName(base)));
        rows.push_back(confirm::Text("For", SideName(quote)));
        plan.method = "createauction";
        plan.params.push_back(Ref(r.asset_a));
        plan.params.push_back(Decimal(amount, base.decimals));
        plan.params.push_back(Ref(r.asset_b));
        plan.params.push_back(Decimal(start, quote.decimals));
        plan.params.push_back(Decimal(end, quote.decimals));
        plan.params.push_back(r.blocks);
        plan.params.push_back(r.delay.value_or(1));
        break;
    }
    case Verb::ORDER: {
        const Reader::Asset sell{Side(reader, r.asset_a, r.ticker_a)}, buy{Side(reader, r.asset_b, r.ticker_b)};
        const uint64_t amount{Exact(r.amount, sell.decimals, "amount")};
        Exact(r.price, std::max(buy.decimals, 8), "price");
        plan.summary = strprintf("Sell %s for %s at %s %s each at least, for %d blocks", Of(amount, sell), buy.ticker, r.price, buy.ticker, r.blocks);
        rows = Head(r, source, plan.summary, now);
        rows.push_back(confirm::Text("Sells", SideName(sell)));
        rows.push_back(confirm::Text("Buys", SideName(buy)));
        plan.method = "placeorder";
        plan.params.push_back(Ref(r.asset_a));
        plan.params.push_back(Decimal(amount, sell.decimals));
        plan.params.push_back(Ref(r.asset_b));
        plan.params.push_back(r.price);
        plan.params.push_back(r.blocks);
        break;
    }
    case Verb::ROUTE: {
        // The path: from the link's `from` through each pool to the other side, to its `to`.
        std::vector<Reader::Asset> path;
        Reader::Asset at;
        bool first{true};
        for (const uint256& id : r.pools) {
            Reader::Pool pool;
            const auto [a, b]{PoolSides(reader, id, pool)};
            if (pool.abandoned) throw LinkError(strprintf("Nobody provides liquidity to the pool %s any more: the route would get almost nothing", Short(id)));
            if (first) {
                if (a.ticker == r.from && b.ticker != r.from) at = a;
                else if (b.ticker == r.from && a.ticker != r.from) at = b;
                else throw LinkError(strprintf("The route's first pool %s is of %s and %s: the link's from %s is not one of them", Short(id), a.ticker, b.ticker, r.from));
                path.push_back(at);
                first = false;
            }
            if (at.id == a.id) at = b;
            else if (at.id == b.id) at = a;
            else throw LinkError(strprintf("The pool %s does not take %s: the route's pools do not follow one another", Short(id), at.ticker.empty() ? "?" : at.ticker));
            path.push_back(at);
        }
        if (path.back().ticker != r.to) throw LinkError(strprintf("The route ends in %s, and the link's to is %s: refused", path.back().ticker.empty() ? "?" : path.back().ticker, r.to));
        const Reader::Asset& from{path.front()};
        const Reader::Asset& to{path.back()};
        const uint64_t amount{Exact(r.amount, from.decimals, "amount")};
        if (from.id.IsNull() && amount < reader.MinChnPayout()) throw LinkError(strprintf("A swap pays in %s at least", Coin(reader.MinChnPayout())));
        const uint64_t floor{Floor(r.floor, to.decimals)};
        // The quote of now, hop by hop, each pool alone.
        uint64_t quote{amount};
        for (size_t i{0}; i < r.pools.size() && quote > 0; ++i) quote = reader.QuotePool(path[i].id, r.pools[i], quote);
        if (quote == 0 || (to.id.IsNull() && quote < reader.MinChnPayout())) throw LinkError("The route takes no such swap now");
        if (quote < floor) {
            throw LinkError(strprintf("The pools moved: the route now gives %s, less than the %s the link asks at least. Nothing was done: quote it again on the site.", Of(quote, to), Of(floor, to)));
        }
        plan.summary = strprintf("Swap %s for %s through %u pools", Of(amount, from), to.ticker, r.pools.size());
        rows = Head(r, source, plan.summary, now);
        std::string through;
        for (size_t i{0}; i < path.size(); ++i) through += (i ? " → " : "") + (path[i].ticker.empty() ? std::string{"?"} : path[i].ticker);
        rows.push_back(confirm::Text("Route", through));
        rows.push_back(confirm::Text("Pays in", SideName(from)));
        rows.push_back(confirm::Text("Gets", SideName(to)));
        QuoteRows(rows, Of(quote, to), Of(floor, to), r.slippage_bps);
        plan.method = "swaproute";
        UniValue assets{UniValue::VARR};
        for (const Reader::Asset& asset : path) assets.push_back(Ref(asset.id));
        UniValue pools{UniValue::VARR};
        for (const uint256& id : r.pools) pools.push_back(id.GetHex());
        plan.params.push_back(std::move(assets));
        plan.params.push_back(Decimal(amount, from.decimals));
        plan.params.push_back(SlippageFor(quote, floor));
        plan.params.push_back(Decimal(floor, to.decimals));
        plan.params.push_back(std::move(pools));
        break;
    }
    case Verb::DEPOSIT:
    case Verb::WITHDRAW: {
        Reader::Pool pool;
        const auto [p0, p1]{PoolSides(reader, r.pool, pool)};
        // asset_a and asset_b as the link names them (either order); amounts, floors and `only` follow its names.
        const auto [a, b]{Oriented(r, p0, p1)};
        const bool in{r.verb == Verb::DEPOSIT};
        UniValue args{UniValue::VARR};
        args.push_back(r.pool.GetHex());
        uint64_t amount_a{0}, amount_b{0};
        if (in) {
            amount_a = Exact(r.amount_a, a.decimals, "amount_a");
            amount_b = Exact(r.amount_b, b.decimals, "amount_b");
            UniValue amounts{UniValue::VOBJ};
            amounts.pushKV(Ref(a.id), Decimal(amount_a, a.decimals));
            amounts.pushKV(Ref(b.id), Decimal(amount_b, b.decimals));
            args.push_back(std::move(amounts));
        } else {
            args.push_back(UniValue{UniValue::VOBJ});
            UniValue shares;
            shares.setNumStr(r.shares);
            args.push_back(std::move(shares));
            if (!r.only.empty()) args.push_back(Ref(r.only == "a" ? a.id : b.id));
        }
        const UniValue q{reader.Read("quotepoolliquidity", args)};
        const uint64_t shares_link{*ToIntegral<uint64_t>(r.shares)};
        if (in) {
            const uint64_t shares{UnitsOf(q.find_value("shares"), 0)};
            if (shares < shares_link) {
                throw LinkError(strprintf("The pool moved: these amounts now get %u shares, less than the %s the link asks at least. Nothing was done: quote it again on the site.", shares, r.shares));
            }
            plan.summary = strprintf("Add %s and %s to the pool", Of(amount_a, a), Of(amount_b, b));
            rows = Head(r, source, plan.summary, now);
            rows.push_back(confirm::Text("Pool", strprintf("%s and %s (0x%s)", a.ticker, b.ticker, Short(r.pool))));
            QuoteRows(rows, strprintf("%u shares", shares), strprintf("%s shares", r.shares), r.slippage_bps);
            plan.method = "addpoolliquidity";
            plan.params.push_back(r.pool.GetHex());
            plan.params.push_back(args[1]);
            plan.params.push_back(SlippageFor(shares, shares_link));
            // The floor itself too: the wallet refuses below it at signing, and the transaction's own floor is never under it.
            UniValue floor;
            floor.setNumStr(r.shares);
            plan.params.push_back(std::move(floor));
        } else {
            const uint64_t out_a{UnitsOf(ByAsset(q.find_value("amounts"), a), a.decimals)}, out_b{UnitsOf(ByAsset(q.find_value("amounts"), b), b.decimals)};
            const uint64_t min_a{r.min_a == "0" ? 0 : Floor(r.min_a, a.decimals)}, min_b{r.min_b == "0" ? 0 : Floor(r.min_b, b.decimals)};
            if (out_a < min_a || out_b < min_b) {
                throw LinkError(strprintf("The pool moved: these shares now take out %s and %s, less than the %s and %s the link asks at least. Nothing was done.", Of(out_a, a), Of(out_b, b), Of(min_a, a), Of(min_b, b)));
            }
            plan.summary = strprintf("Take %s shares out of the pool of %s and %s", r.shares, a.ticker, b.ticker);
            rows = Head(r, source, plan.summary, now);
            rows.push_back(confirm::Text("Pool", strprintf("%s and %s (0x%s)", a.ticker, b.ticker, Short(r.pool))));
            if (!r.only.empty()) rows.push_back(confirm::Text("Comes out as", (r.only == "a" ? a.ticker : b.ticker) + " alone (the rest traded in the pool, its fee paid)"));
            QuoteRows(rows, Of(out_a, a) + " and " + Of(out_b, b), Of(min_a, a) + " and " + Of(min_b, b), r.slippage_bps);
            plan.method = "removepoolliquidity";
            plan.params.push_back(r.pool.GetHex());
            plan.params.push_back(100);
            plan.params.push_back(r.only.empty() ? UniValue{} : UniValue{Ref(r.only == "a" ? a.id : b.id)});
            plan.params.push_back(SlippageForBoth(out_a, min_a, out_b, min_b));
            UniValue shares;
            shares.setNumStr(r.shares);
            plan.params.push_back(std::move(shares));
            // The floors themselves too (as for a deposit): refused below them at signing, never under them on chain.
            UniValue floors{UniValue::VOBJ};
            floors.pushKV(Ref(a.id), Decimal(min_a, a.decimals));
            floors.pushKV(Ref(b.id), Decimal(min_b, b.decimals));
            plan.params.push_back(std::move(floors));
        }
        break;
    }
    case Verb::POSITION: {
        Reader::Pool pool;
        const auto [a, b]{PoolSides(reader, r.pool, pool)};
        CheckPoolTickers(r, a, b);
        if (r.price_lower != "0") Exact(r.price_lower, 8, "price_lower");
        if (r.price_upper != "0") Exact(r.price_upper, 8, "price_upper");
        const bool full{r.price_lower == "0" && r.price_upper == "0"};
        UniValue amounts{UniValue::VOBJ};
        if (r.amount_a != "0") amounts.pushKV(Ref(a.id), Decimal(Exact(r.amount_a, a.decimals, "amount_a"), a.decimals));
        if (r.amount_b != "0") amounts.pushKV(Ref(b.id), Decimal(Exact(r.amount_b, b.decimals, "amount_b"), b.decimals));
        UniValue args{UniValue::VARR};
        args.push_back(r.pool.GetHex());
        args.push_back(r.price_lower);
        args.push_back(r.price_upper);
        args.push_back(amounts);
        const UniValue preview{reader.Read("previewposition", args)};
        plan.summary = full ? strprintf("Open a full-range position in the pool of %s and %s", a.ticker, b.ticker)
                            : strprintf("Open a position in the pool of %s and %s, from %s to %s", a.ticker, b.ticker, r.price_lower == "0" ? std::string{"no bound"} : r.price_lower, r.price_upper == "0" ? std::string{"no bound"} : r.price_upper);
        rows = Head(r, source, plan.summary, now);
        rows.push_back(confirm::Text("Pool", strprintf("%s and %s (0x%s), concentrated", a.ticker, b.ticker, Short(r.pool))));
        rows.push_back(confirm::Text("Range", full ? std::string{"the full range: every price (as a constant-product pool)"} : strprintf("%s to %s %s per %s", r.price_lower == "0" ? std::string{"no bound"} : r.price_lower, r.price_upper == "0" ? std::string{"no bound"} : r.price_upper, b.ticker, a.ticker)));
        const UniValue& in_range{preview.find_value("in_range")};
        if (in_range.isBool()) rows.push_back(confirm::Text("Now", in_range.get_bool() ? "the pool's price is in this range: it earns fees at once" : "the pool's price is outside this range: it earns nothing until the price comes in"));
        rows.push_back(confirm::Text("Slippage", Bps(*r.slippage_bps) + " at most if the pool moves first (binds)"));
        if (!r.price.empty()) {
            Exact(r.price, 8, "price");
            rows.push_back(confirm::Text("At the price", r.price + " " + b.ticker + " per " + a.ticker + ", as the site showed it: refused if the pool's price is further from it than the slippage"));
        }
        plan.method = "openposition";
        plan.params.push_back(r.pool.GetHex());
        plan.params.push_back(r.price_lower);
        plan.params.push_back(r.price_upper);
        plan.params.push_back(std::move(amounts));
        plan.params.push_back(Percent(*r.slippage_bps));
        if (!r.price.empty()) plan.params.push_back(r.price);
        break;
    }
    case Verb::REPOSITION: {
        UniValue args{UniValue::VARR};
        args.push_back(r.target.GetHex());
        const UniValue pos{reader.Read("getposition", args)};
        const UniValue& pool{pos.find_value("pool")};
        plan.summary = r.action == "collect" ? "Collect the fees of a position" : r.action == "close" ? "Close a position (all of it out, with its fees)" : strprintf("Take %d %% of a position out, with its fees", r.percent);
        rows = Head(r, source, plan.summary, now);
        rows.push_back(confirm::Text("Position", "0x" + Short(r.target) + (pool.isStr() ? " in the pool 0x" + pool.get_str().substr(0, 8) + "…" : std::string{})));
        const UniValue& fees0{pos.find_value("fees0")};
        const UniValue& fees1{pos.find_value("fees1")};
        if (!fees0.isNull() || !fees1.isNull()) rows.push_back(confirm::Text("Fees owed now", fees0.getValStr() + " " + pos.find_value("asset0").getValStr() + " and " + fees1.getValStr() + " " + pos.find_value("asset1").getValStr()));
        if (r.action != "collect") rows.push_back(confirm::Text("Slippage", Bps(*r.slippage_bps) + " at most if the pool moves first (binds)"));
        UniValue change{UniValue::VOBJ};
        if (r.action == "collect") change.pushKV("collect", true);
        else if (r.action == "close") change.pushKV("close", true);
        else {
            change.pushKV("remove", r.percent);
            change.pushKV("collect", true);
        }
        if (r.action != "collect") change.pushKV("slippage", Percent(*r.slippage_bps));
        plan.method = "changeposition";
        plan.params.push_back(r.target.GetHex());
        plan.params.push_back(std::move(change));
        break;
    }
    case Verb::SETTLE: {
        UniValue args{UniValue::VARR};
        args.push_back(r.target.GetHex());
        const UniValue order{reader.Read("getorder", args)};
        plan.summary = r.cancel ? "Cancel an order: what it got and what is left come back" : "Collect what an order got (it stays open)";
        rows = Head(r, source, plan.summary, now);
        rows.push_back(confirm::Text("Order", "0x" + Short(r.target)));
        for (const char* key : {"sells", "buys", "remaining", "to_collect", "status"}) {
            const UniValue& v{order.find_value(key)};
            if (!v.isNull() && !v.isObject() && !v.isArray()) rows.push_back(confirm::Text(key, v.getValStr()));
        }
        plan.method = "settleorder";
        plan.params.push_back(r.target.GetHex());
        plan.params.push_back(r.cancel);
        break;
    }
    case Verb::BID:
    case Verb::COLLECT: {
        UniValue args{UniValue::VARR};
        args.push_back(r.target.GetHex());
        const UniValue auction{reader.Read("getauction", args)};
        const bool bid{r.verb == Verb::BID};
        plan.summary = bid ? strprintf("Bid %s%s on an auction", r.amount, r.buy_all ? " at most, for all that is left," : "") : "Collect an auction: what it brought in and what is left of what it sold";
        rows = Head(r, source, plan.summary, now);
        rows.push_back(confirm::Text("Auction", "0x" + Short(r.target)));
        for (const char* key : {"base", "quote", "price", "remaining", "status"}) {
            const UniValue& v{auction.find_value(key)};
            if (!v.isNull() && !v.isObject() && !v.isArray()) rows.push_back(confirm::Text(key, v.getValStr()));
        }
        plan.method = bid ? "bidauction" : "collectauction";
        plan.params.push_back(r.target.GetHex());
        if (bid) {
            plan.params.push_back(r.amount);
            plan.params.push_back(r.buy_all);
        }
        break;
    }
    case Verb::CLAIM: {
        const Reader::Asset token{Token(reader, r.asset, r.ticker)};
        if (!reader.LaunchOf(r.asset)) throw LinkError(strprintf("%s was not launched: it has no creator to pay", r.ticker));
        UniValue args{UniValue::VARR};
        args.push_back(Ref(r.asset));
        const UniValue launch{reader.Read("getlaunch", args)};
        plan.summary = strprintf("Pay the creator of %s what it is owed (its fees, its allocation once graduated)", r.ticker);
        rows = Head(r, source, plan.summary, now);
        rows.push_back(confirm::Text("Launch", Named(token)));
        rows.push_back(confirm::Text("Fees owed", launch.find_value("creator_fees_owed").getValStr() + " " + std::string{COIN_TICKER}));
        const UniValue& allocation{launch.find_value("allocation")};
        if (allocation.isNum() && allocation.getValStr() != "0") {
            const bool claimed{launch.find_value("allocation_claimed").isTrue()};
            rows.push_back(confirm::Text("Allocation", allocation.getValStr() + "% of the supply" + (claimed ? ", minted already" : launch.find_value("status").getValStr() == "graduated" ? ", minted now" : ", at graduation (not yet)")));
        }
        rows.push_back(confirm::Note("Anyone may send this: what is owed goes to the launch's creator address only, never elsewhere."));
        plan.method = "claimlaunch";
        plan.params.push_back(Ref(r.asset));
        break;
    }
    case Verb::PEGIN: {
        const uint64_t amount{Exact(r.amount, bitassets::CHN_DECIMALS, "amount")};
        plan.summary = strprintf("Deposit %s from eCash into this wallet", Coin(amount));
        rows = Head(r, source, plan.summary, now);
        rows.push_back(confirm::Text("Paid by", "the wallet of the eCash node this app is linked to (its own question comes next)"));
        rows.push_back(confirm::Text("Credited to", "this wallet (its flush.markets address if it has one), once the deposit is in an eCash block and a block of this chain takes it"));
        rows.push_back(confirm::Note("A deposit moves coins from eCash into this sidechain's escrow: coins come back only by a withdrawal, which takes long."));
        plan.amount_units = amount;
        break;
    }
    case Verb::PEGOUT: {
        const uint64_t amount{Exact(r.amount, bitassets::CHN_DECIMALS, "amount")};
        const uint64_t fee{Exact(r.fee, bitassets::CHN_DECIMALS, "fee")};
        plan.summary = strprintf("Withdraw %s to eCash, to %s", Coin(amount), r.address);
        rows = Head(r, source, plan.summary, now);
        rows.push_back(confirm::Destination("eCash address paid", r.address));
        rows.push_back(confirm::Text("Fee to eCash's miners", Coin(fee) + ", on top of the amount"));
        rows.push_back(confirm::Note("A withdrawal leaves this wallet at once and is paid on eCash only once a bundle of withdrawals is voted through by eCash's miners: "
                                     "that takes long (weeks). Until it is in a bundle it can be taken back (refundwithdrawal). Check the address: a payment on eCash cannot be undone."));
        plan.method = "createwithdrawal";
        plan.params.push_back(r.address);
        plan.params.push_back(Decimal(amount, bitassets::CHN_DECIMALS));
        plan.params.push_back(Decimal(fee, bitassets::CHN_DECIMALS));
        break;
    }
    }
    rows.push_back(Warning());
    plan.preface = std::move(rows);
    return plan;
}

namespace {
class NodeChainReader : public Reader
{
public:
    NodeChainReader(interfaces::Node& node, interfaces::Chain& chain) : m_node{node}, m_chain{chain} {}
    UniValue Read(const std::string& method, const UniValue& params) override
    {
        // The node's own reads only (none of a wallet's): the methods a link's question shows.
        static const std::set<std::string> READS{"getorder", "getauction", "getposition", "getlaunch", "quotepoolliquidity", "previewposition"};
        if (!READS.contains(method)) throw LinkError("Not a read of a link");
        try {
            return m_node.executeRpc(method, params, "/");
        } catch (const UniValue& error) {
            const UniValue& message{error.find_value("message")};
            throw LinkError(message.isStr() ? message.get_str() : std::string{"The node refused it"});
        }
    }
    std::optional<Asset> AssetById(const uint256& id) override
    {
        const auto record{m_chain.getBitAsset(id)};
        if (!record) return std::nullopt;
        Asset asset;
        asset.id = id;
        asset.ticker = record->text;
        asset.decimals = record->decimals;
        asset.seq = record->seq;
        if (const auto display{m_chain.getBitAssetDisplayName(id)}) asset.display_name = display->text;
        return asset;
    }
    bool NameTaken(const std::string& name) override { return m_chain.getBitAsset(bitassets::HashName(name)).has_value(); }
    std::optional<Launch> LaunchOf(const uint256& token) override
    {
        const auto launch{m_chain.getBitAssetsLaunch(token)};
        if (!launch) return std::nullopt;
        return Launch{launch->pool, launch->Graduated()};
    }
    uint64_t QuoteLaunch(const uint256& token, bool buy, uint64_t amount) override
    {
        const auto launch{m_chain.getBitAssetsLaunch(token)};
        if (!launch || launch->Graduated()) return 0;
        const auto curve{m_chain.getBitAssetsPoolOfKind(launch->pool)};
        if (!curve) return 0;
        const auto cleared{bitassets::curves::ClearLaunch(bitassets::curves::LaunchCurve{launch->fee, launch->CurveEnd(), launch->max_buy, launch->min_buy, launch->StrictEnd(), launch->StartBound()},
                                                          curve->first.reserve0, curve->first.reserve1, {bitassets::dex::HopIn{buy, amount}}, {true})};
        return cleared ? cleared->outs[0] : 0;
    }
    uint64_t QuoteFirstBuy(uint64_t ecx, uint64_t supply, uint32_t fee) override
    {
        const Consensus::SidechainParams& rules{Params().GetConsensus().sidechain};
        const uint64_t graduation{static_cast<uint64_t>(rules.LaunchGraduation(m_chain.getBitAssetsHeight()))};
        const uint64_t s{supply == 0 ? bitassets::LAUNCH_SUPPLY : supply};
        const auto cleared{bitassets::curves::ClearLaunch(bitassets::curves::LaunchCurve{fee == 0 ? rules.launch_fee : fee, s / 8 * 3, 0, 0}, graduation / 2,
                                                          s / 8 * 9, {bitassets::dex::HopIn{true, ecx}}, {true})};
        return cleared ? cleared->outs[0] : 0;
    }
    bool LaunchParamsActive() override { return m_chain.getBitAssetsLaunchParamsActive(); }
    bool FeeSplitActive() override { return Params().GetConsensus().sidechain.FeeSplitActive(m_chain.getBitAssetsHeight()); }
    bool FeeFlowActive() override { return Params().GetConsensus().sidechain.FeeFlowActive(m_chain.getBitAssetsHeight()); }
    uint32_t LaunchSecurityShare() override { return Params().GetConsensus().sidechain.LaunchSecurityShare(m_chain.getBitAssetsHeight()); }
    uint32_t LaunchFee() override { return Params().GetConsensus().sidechain.launch_fee; }
    std::optional<Pool> PoolById(const uint256& id) override
    {
        const auto found{m_chain.getBitAssetsPoolOfKind(id)};
        if (!found) return std::nullopt;
        return Pool{found->first.asset0, found->first.asset1, bitassets::amm::Abandoned(found->first)};
    }
    uint64_t QuotePool(const uint256& asset_in, const uint256& pool, uint64_t amount) override
    {
        const auto outs{m_chain.quoteBitAssetsPools(asset_in, {pool}, amount)};
        return outs && !outs->empty() ? outs->back() : 0;
    }
    bool LaunchesActive() override { return m_chain.getBitAssetsDexCurvesActive(); }
    uint64_t MinChnPayout() override { return m_chain.getBitAssetsMinChnPayout(); }
    std::optional<int> RevealWait(const uint256& txid) override
    {
        const auto made{m_chain.getBitAssetsReservationHeight(Txid::FromUint256(txid))};
        if (!made) return std::nullopt;
        return std::max(0, *made + m_chain.getBitAssetsRevealDepth() - m_chain.getBitAssetsHeight());
    }

private:
    interfaces::Node& m_node;
    interfaces::Chain& m_chain;
};
} // namespace

std::unique_ptr<Reader> NodeReader(interfaces::Node& node)
{
    node::NodeContext* context{node.context()};
    if (!context || !context->chain) throw HostError(BRIDGE_NOT_READY, "The node is still starting");
    return std::make_unique<NodeChainReader>(node, *context->chain);
}

LinkHost::LinkHost(Deps deps) : m_deps{std::move(deps)}
{
    m_thread = std::thread{[this] { Worker(); }};
}

LinkHost::~LinkHost() { Stop(); }

void LinkHost::Stop()
{
    m_stop = true;
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    std::vector<std::thread> launches;
    {
        std::lock_guard lock{m_mutex};
        launches.swap(m_launches);
    }
    for (std::thread& t : launches) {
        if (t.joinable()) t.join();
    }
}

bool LinkHost::Enqueue(std::string uri, Source source)
{
    {
        std::lock_guard lock{m_mutex};
        if (m_stop || m_queue.size() >= MAX_QUEUED) {
            LogWarning("webui: a link was refused: %u links wait already", m_queue.size());
            return false;
        }
        m_queue.emplace_back(std::move(uri), source);
    }
    m_cv.notify_all();
    return true;
}

void LinkHost::Worker()
{
    while (true) {
        std::pair<std::string, Source> next;
        {
            std::unique_lock lock{m_mutex};
            m_cv.wait(lock, [&] { return m_stop || !m_queue.empty(); });
            if (m_stop) return;
            next = std::move(m_queue.front());
            m_queue.pop_front();
        }
        try {
            Handle(next.first, next.second);
        } catch (const LinkError& e) {
            LogInfo("webui: a link was refused: %s", e.what());
            if (m_deps.inform) m_deps.inform("Link refused", e.what(), true);
        } catch (const HostError& e) {
            if (e.code == BRIDGE_CANCELLED) {
                LogInfo("webui: a link was declined");
            } else {
                LogInfo("webui: a link failed: %s", e.what());
                if (m_deps.inform) m_deps.inform("The link was not done", e.what(), true);
            }
        } catch (const std::exception& e) {
            LogWarning("webui: a link failed: %s", e.what());
            if (m_deps.inform) m_deps.inform("The link was not done", e.what(), true);
        }
    }
}

std::string LinkHost::Wallet(interfaces::Node& node)
{
    UniValue list;
    try {
        list = node.executeRpc("listwallets", UniValue{UniValue::VARR}, "/");
    } catch (...) {
        throw LinkError("Wallets are disabled in this node: a link acts from a wallet");
    }
    std::vector<std::string> names;
    if (list.isArray()) {
        for (const UniValue& v : list.getValues()) {
            if (v.isStr()) names.push_back(v.get_str());
        }
    }
    const std::string chosen{m_deps.wallet ? m_deps.wallet() : std::string{}};
    if (!chosen.empty() && std::ranges::find(names, chosen) != names.end()) return chosen;
    if (names.size() == 1) return names.front();
    if (names.empty()) throw LinkError("No wallet is open: open or create one in the app, then open the link again");
    throw LinkError("Several wallets are open: choose the one to use in the app, then open the link again");
}

UniValue LinkHost::Handle(const std::string& uri, Source source)
{
    const int64_t now{m_deps.now()};
    Request request{Parse(uri, m_deps.network(), now)};
    // The same link once in a session (a link that ran): a link replayed does nothing.
    uint256 hash;
    CSHA256().Write(reinterpret_cast<const unsigned char*>(uri.data()), uri.size()).Finalize(hash.data());
    {
        std::lock_guard lock{m_mutex};
        if (m_used.contains(hash.GetHex())) throw LinkError("This link was used already: ask the site for a fresh one (a link does its work once)");
    }
    UniValue result{Run(std::move(request), source, uri)};
    std::lock_guard lock{m_mutex};
    m_used.insert(hash.GetHex());
    return result;
}

UniValue LinkHost::ImportLaunch(const std::string& json)
{
    Request request{ParseLaunchFile(json, m_deps.network())};
    return Run(std::move(request), Source::FILE, {});
}

namespace {
/** The wallet's address of the label flush.markets, if it has one. */
std::string LinkAddress(interfaces::Node& node, const std::string& wallet)
{
    try {
        UniValue args{UniValue::VARR};
        args.push_back(LINK_LABEL);
        const UniValue found{node.executeRpc("getaddressesbylabel", args, WalletUri(wallet))};
        if (found.isObject()) {
            for (const std::string& key : found.getKeys()) {
                const UniValue& purpose{found.find_value(key).find_value("purpose")};
                if (purpose.isStr() && purpose.get_str() == "receive") return key;
            }
        }
    } catch (...) {
    }
    return {};
}

/**
 * For the link's one call, on this thread alone (ScopedLinkDestination, read by the wallet when the call runs, after the
 * user accepted it): what it brings in goes to the wallet's flush.markets address. None if the wallet has no such
 * address (its new addresses, as always).
 */
class LinkDestination
{
public:
    LinkDestination(interfaces::Node& node, const std::string& wallet) : m_scope{LinkAddress(node, wallet)} {}

private:
    ScopedLinkDestination m_scope;
};
} // namespace

UniValue LinkHost::Run(Request request, Source source, const std::string& uri)
{
    interfaces::Node& node{m_deps.node()};
    const std::string wallet{Wallet(node)};
    LogInfo("webui: a link (%s): %s", SourceText(source), VerbName(request.verb));
    // A logo the link names by its hash (it cannot carry 32 KB): downloaded from where the link says the site hosts it
    // (8.1.0, logo_url) and kept only if it is that one; else, or if that fails, chosen by the user, refused if it is not.
    std::string download_failed;
    if ((request.verb == Verb::LAUNCH || request.verb == Verb::REGISTER) && request.logo_sha256 && request.logo.empty() && !request.logo_url.empty()) {
        std::string error;
        std::optional<std::vector<unsigned char>> bytes;
        if (m_deps.fetch) {
            bytes = m_deps.fetch(request.logo_url, bitassets::MAX_LOGO_BYTES, error);
        } else {
            error = "downloads are not supported here";
        }
        if (bytes && bytes->size() > bitassets::MAX_LOGO_BYTES) {
            bytes.reset();
            error = strprintf("larger than the %u bytes the chain takes", bitassets::MAX_LOGO_BYTES);
        }
        if (bytes) {
            unsigned char hash[CSHA256::OUTPUT_SIZE];
            CSHA256().Write(bytes->data(), bytes->size()).Finalize(hash);
            if (HexStr(hash) != *request.logo_sha256) {
                bytes.reset();
                error = "its SHA-256 is not the one the link names";
            }
        }
        if (bytes) {
            LogInfo("webui: the logo of the link was downloaded from %s (%u bytes, its SHA-256 the link's)", UrlHost(request.logo_url), bytes->size());
            request.logo = std::move(*bytes);
            request.logo_downloaded = true;
        } else {
            if (error.empty()) error = "it could not be downloaded";
            LogInfo("webui: the logo of the link could not be downloaded from %s: %s; the file is asked for", UrlHost(request.logo_url), error);
            download_failed = error;
        }
    }
    if ((request.verb == Verb::LAUNCH || request.verb == Verb::REGISTER) && request.logo_sha256 && request.logo.empty()) {
        Window::FileDialog chooser;
        chooser.title = download_failed.empty() ? strprintf("The logo of %s (SHA-256 %s…)", request.ticker, request.logo_sha256->substr(0, 12)) :
                                                  strprintf("The logo of %s could not be downloaded from %s (%s): choose its file (SHA-256 %s…)", request.ticker,
                                                            UrlHost(request.logo_url), download_failed.substr(0, 160), request.logo_sha256->substr(0, 12));
        chooser.filters = {{"Logo (PNG, JPEG)", {"*.png", "*.jpg", "*.jpeg"}}};
        const auto path{m_deps.choose ? m_deps.choose(chooser) : std::nullopt};
        if (!path) throw HostError(BRIDGE_CANCELLED, "No logo chosen: nothing was done.");
        std::ifstream file{fs::PathFromString(*path).std_path(), std::ios::binary};
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (!file.good() && !file.eof()) throw LinkError("The logo cannot be read");
        if (bytes.size() > bitassets::MAX_LOGO_BYTES) throw LinkError(strprintf("The logo is larger than the %u bytes the chain takes", bitassets::MAX_LOGO_BYTES));
        unsigned char hash[CSHA256::OUTPUT_SIZE];
        CSHA256().Write(bytes.data(), bytes.size()).Finalize(hash);
        if (HexStr(hash) != *request.logo_sha256) throw LinkError("This file is not the logo the link names (its SHA-256 differs): nothing was done");
        request.logo = std::move(bytes);
    }
    const auto reader{m_deps.reader(node)};
    Plan plan{Resolve(request, source, *reader, m_deps.now())};
    if (request.verb == Verb::PEGIN) return Deposit(plan, wallet);
    UniValue call{UniValue::VOBJ};
    call.pushKV("method", plan.method);
    call.pushKV("params", plan.params);
    call.pushKV("wallet", wallet);
    if (!plan.then_method.empty()) {
        UniValue then{UniValue::VOBJ};
        then.pushKV("method", plan.then_method);
        then.pushKV("params", plan.then_params);
        call.pushKV("then", std::move(then));
    }
    UniValue result;
    {
        confirm::Preface preface{plan.preface, plan.message, "link"};
        const LinkDestination destination{node, wallet};
        result = m_deps.confirmed(call);
    }
    LogInfo("webui: a link ran as accepted: %s", plan.summary);
    if (!plan.then_method.empty()) {
        const UniValue& txid{result.isObject() ? result.find_value("txid") : NullUniValue};
        if (txid.isStr() && result.find_value("authorized").isNum()) {
            std::lock_guard lock{m_mutex};
            m_launches.emplace_back([this, plan, wallet, txid = txid.get_str()] { WaitAndLaunch(plan, wallet, txid); });
        }
    }
    return result;
}

UniValue LinkHost::Deposit(const Plan& plan, const std::string& wallet)
{
    interfaces::Node& node{m_deps.node()};
    if (!m_deps.deposit) throw LinkError("This app makes no deposits from eCash: deposit with an eCash wallet to the address the site shows");
    // Credited to the wallet's flush.markets address if it has one; else a new address of the wallet (getdepositaddress).
    std::string address{LinkAddress(node, wallet)};
    UniValue args{UniValue::VARR};
    if (!address.empty()) args.push_back(address);
    UniValue made;
    try {
        made = node.executeRpc("getdepositaddress", args, WalletUri(wallet));
    } catch (const UniValue& error) {
        const UniValue& message{error.find_value("message")};
        throw LinkError(message.isStr() ? message.get_str() : std::string{"No deposit address"});
    }
    const UniValue& deposit_address{made.find_value("depositaddress")};
    const UniValue& credited{made.find_value("address")};
    if (!deposit_address.isStr() || !credited.isStr()) throw LinkError("No deposit address");
    // The link's question first (where it comes from, what it asks), then the deposit's own (the amount, the eCash wallet that pays).
    Window::ConfirmDialog question;
    question.title = "Deposit from eCash";
    question.accept = "Continue";
    question.level = dialog::Level::WARNING;
    question.kind = "link:pegin";
    question.message = plan.message;
    question.rows = plan.preface;
    question.rows.insert(question.rows.end() - 1, confirm::Destination("Credited to", credited.get_str() + " (this wallet: " + (wallet.empty() ? std::string{"the default wallet"} : wallet) + ")"));
    question.rows.insert(question.rows.end() - 1, confirm::Destination("Deposit address", deposit_address.get_str()));
    if (!m_deps.ask || !m_deps.ask(question).accepted) throw HostError(BRIDGE_CANCELLED, "Not deposited: it was declined.");
    LogInfo("webui: a link's deposit goes on to its own question: %s", plan.summary);
    return m_deps.deposit(deposit_address.get_str(), Decimal(plan.amount_units, bitassets::CHN_DECIMALS));
}

void LinkHost::WaitAndLaunch(Plan plan, std::string wallet, std::string reserve_txid)
{
    const auto txid{uint256::FromHex(reserve_txid)};
    if (!txid) return;
    const auto deadline{std::chrono::steady_clock::now() + confirm::Authorizations::EXPIRY};
    while (!m_stop) {
        try {
            const auto reader{m_deps.reader(m_deps.node())};
            const auto wait{reader->RevealWait(*txid)};
            if (wait && *wait == 0) break;
        } catch (...) {
        }
        if (std::chrono::steady_clock::now() > deadline) {
            if (m_deps.inform) m_deps.inform(plan.then_method == "launchtoken" ? "Launch not sent" : "Registration not sent", strprintf("The reservation of %s was not deep enough within the hour: finish it from the Assets page.", plan.request.ticker), true);
            return;
        }
        std::unique_lock lock{m_mutex};
        m_cv.wait_for(lock, m_deps.poll, [&] { return m_stop.load(); });
    }
    if (m_stop) return;
    UniValue call{UniValue::VOBJ};
    call.pushKV("method", plan.then_method);
    call.pushKV("params", plan.then_params);
    call.pushKV("wallet", wallet);
    try {
        confirm::Preface preface{plan.preface, plan.message, "link"};
        const LinkDestination destination{m_deps.node(), wallet};
        const UniValue result{m_deps.confirmed(call)};
        const bool launch{plan.then_method == "launchtoken"};
        LogInfo("webui: the %s of %s was sent: %s", launch ? "launch" : "registration", plan.request.ticker, result.write());
        if (m_deps.inform) m_deps.inform(launch ? "Launched" : "Registered", plan.request.ticker + (launch ? std::string{" is launched: on its curve with the next block."} : std::string{" is registered: in this wallet with the next block."}), false);
    } catch (const std::exception& e) {
        LogInfo("webui: the second step for %s failed: %s", plan.request.ticker, e.what());
        if (m_deps.inform) m_deps.inform("Not sent", strprintf("%s was reserved, and its %s failed: %s", plan.request.ticker, plan.then_method == "launchtoken" ? "launch" : "registration", e.what()), true);
    }
}

void LinkHost::Register(Bridge& bridge)
{
    // A link the page hands over as it is (pasted, dropped): read here, never in the page.
    bridge.RegisterHost("links.open", [this](const UniValue& params) {
        const UniValue& uri{params.isObject() ? params.find_value("uri") : NullUniValue};
        if (!uri.isStr() || uri.get_str().size() > MAX_URI) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("uri: a link of at most %u bytes", MAX_URI));
        try {
            return Handle(uri.get_str(), Source::PASTED);
        } catch (const LinkError& e) {
            LogInfo("webui: a link was refused: %s", e.what());
            throw HostError(BRIDGE_INVALID_REQUEST, e.what());
        }
    }, Bridge::Thread::LONG);
    // A launch file: its text (dropped on the window), or chosen here (File > Import launch).
    bridge.RegisterHost("links.importLaunch", [this](const UniValue& params) {
        const UniValue& text{params.isObject() ? params.find_value("text") : NullUniValue};
        std::string json;
        if (text.isStr()) {
            json = text.get_str();
        } else {
            Window::FileDialog chooser;
            chooser.title = "Import a launch file";
            chooser.filters = {{"Launch file", {"*.flushlaunch.json", "*.json"}}};
            const auto path{m_deps.choose ? m_deps.choose(chooser) : std::nullopt};
            if (!path) return UniValue{};
            std::ifstream file{fs::PathFromString(*path).std_path(), std::ios::binary};
            std::string data;
            data.resize(MAX_LAUNCH_FILE + 1);
            file.read(data.data(), data.size());
            data.resize(file.gcount());
            json = std::move(data);
        }
        try {
            return ImportLaunch(json);
        } catch (const LinkError& e) {
            LogInfo("webui: a launch file was refused: %s", e.what());
            throw HostError(BRIDGE_INVALID_REQUEST, e.what());
        }
    }, Bridge::Thread::LONG);
}

} // namespace webui::links
