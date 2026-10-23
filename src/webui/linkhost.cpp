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
#include <logging.h>
#include <node/context.h>
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
//! The label of the address a connect shares (one per wallet, the same each time).
constexpr const char* CONNECT_LABEL{"flush.markets"};

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
    case Verb::CONNECT:
        throw LinkError("A connect is not a call");
    case Verb::BUY:
    case Verb::SELL: {
        const bool buy{r.verb == Verb::BUY};
        const Reader::Asset token{Token(reader, r.asset, r.ticker)};
        const auto launch{reader.LaunchOf(r.asset)};
        if (!launch) throw LinkError(strprintf("%s is not on a curve: it has none to trade with", r.ticker));
        if (launch->graduated) throw LinkError(strprintf("%s graduated: it trades in its pool now (ask the site for a swap)", r.ticker));
        const uint64_t amount{buy ? Exact(r.amount, bitassets::CHN_DECIMALS, "ecx") : Exact(r.amount, token.decimals, "tokens")};
        if (buy && amount < bitassets::MIN_CHN_PAYOUT) throw LinkError(strprintf("A buy pays %s at least", Coin(bitassets::MIN_CHN_PAYOUT)));
        const uint64_t floor{Floor(r.floor, buy ? token.decimals : bitassets::CHN_DECIMALS)};
        const uint64_t quote{reader.QuoteLaunch(r.asset, buy, amount)};
        const std::string quoted{buy ? Of(quote, token) : Coin(quote)};
        const std::string floored{buy ? Of(floor, token) : Coin(floor)};
        if (quote == 0 || (!buy && quote < bitassets::MIN_CHN_PAYOUT)) throw LinkError(strprintf("The curve of %s takes no such %s now", r.ticker, buy ? "buy" : "sale"));
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
        if (from.id.IsNull() && amount < bitassets::MIN_CHN_PAYOUT) throw LinkError(strprintf("A swap pays in %s at least", Coin(bitassets::MIN_CHN_PAYOUT)));
        const uint64_t floor{Floor(r.floor, to.decimals)};
        const uint64_t quote{reader.QuotePool(from.id, r.pool, amount)};
        if (quote == 0 || (to.id.IsNull() && quote < bitassets::MIN_CHN_PAYOUT)) throw LinkError("The pool takes no such swap now");
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
        std::string quoted, floored;
        if (!r.amount.empty()) {
            const uint64_t ecx{Exact(r.amount, bitassets::CHN_DECIMALS, "ecx")};
            if (ecx < bitassets::MIN_CHN_PAYOUT) throw LinkError(strprintf("The first buy pays %s at least", Coin(bitassets::MIN_CHN_PAYOUT)));
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
        if (logo) rows.push_back(confirm::Text("Logo", "the file's SHA-256 is the one the request names (" + logo->hash.substr(0, 16) + "…)"));
        if (!r.supply.empty()) rows.push_back(confirm::Text("Supply", strprintf("%s %s (%d decimals): 3/4 sold on the curve, 1/4 into the locked pool at graduation", r.supply, r.ticker, decimals)));
        if (r.fee_bps) rows.push_back(confirm::Text("Curve's fee", Bps(*r.fee_bps) + " of each trade (creator, treasury and security parts as the network's)"));
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
        if (logo) rows.push_back(confirm::Text("Logo", "the file's SHA-256 is the one the request names (" + logo->hash.substr(0, 16) + "…)"));
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
        plan.summary = strprintf("Create a %s pool of %s and %s", r.kind == "constant" ? "constant-product" : r.kind, Of(amount_a, a), Of(amount_b, b));
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
    }
    rows.push_back(Warning());
    plan.preface = std::move(rows);
    return plan;
}

Window::ConfirmDialog ConnectQuestion(const Request& r, Source source, const std::string& wallet, const std::string& address, int64_t now)
{
    Window::ConfirmDialog dialog;
    dialog.title = "Share an address with flush.markets";
    dialog.accept = "Share";
    dialog.level = dialog::Level::WARNING;
    dialog.kind = "link:connect";
    dialog.message = strprintf("A link (%s) asks for an address of this wallet, for %s to show what it holds. On Share, the app opens your browser at %s "
                               "with this address and the site's code: nothing else is sent, nothing is signed or paid.\n\nShare only if you asked for this on %s yourself just now.",
                               SourceText(source), r.origin, r.callback, r.origin);
    dialog.rows = Head(r, source, "Share an address with " + r.origin, now);
    dialog.rows.push_back(confirm::Text("Site", r.origin));
    dialog.rows.push_back(confirm::Destination("Address shared", address));
    dialog.rows.push_back(confirm::Text("Wallet", wallet.empty() ? std::string{"(the default wallet)"} : wallet));
    dialog.rows.push_back(confirm::Text("Network", confirm::NetworkName()));
    dialog.rows.push_back(confirm::Text("The answer goes to", r.callback + " (in your browser)"));
    dialog.rows.push_back(confirm::Note("The site can then show anyone who has this address what it holds and receives: an address of its own (label flush.markets) is shared, the same each time."));
    return dialog;
}

namespace {
class NodeChainReader : public Reader
{
public:
    explicit NodeChainReader(interfaces::Chain& chain) : m_chain{chain} {}
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
        const uint64_t graduation{static_cast<uint64_t>(rules.launch_graduation)};
        const uint64_t s{supply == 0 ? bitassets::LAUNCH_SUPPLY : supply};
        const auto cleared{bitassets::curves::ClearLaunch(bitassets::curves::LaunchCurve{fee == 0 ? rules.launch_fee : fee, s / 8 * 3, 0, 0}, graduation / 2,
                                                          s / 8 * 9, {bitassets::dex::HopIn{true, ecx}}, {true})};
        return cleared ? cleared->outs[0] : 0;
    }
    bool LaunchParamsActive() override { return m_chain.getBitAssetsLaunchParamsActive(); }
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
    std::optional<int> RevealWait(const uint256& txid) override
    {
        const auto made{m_chain.getBitAssetsReservationHeight(Txid::FromUint256(txid))};
        if (!made) return std::nullopt;
        return std::max(0, *made + m_chain.getBitAssetsRevealDepth() - m_chain.getBitAssetsHeight());
    }

private:
    interfaces::Chain& m_chain;
};
} // namespace

std::unique_ptr<Reader> NodeReader(interfaces::Node& node)
{
    node::NodeContext* context{node.context()};
    if (!context || !context->chain) throw HostError(BRIDGE_NOT_READY, "The node is still starting");
    return std::make_unique<NodeChainReader>(*context->chain);
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
    // The same link once in a session (a link that ran, or the nonce of a connect answered): a link replayed does nothing.
    uint256 hash;
    CSHA256().Write(reinterpret_cast<const unsigned char*>(uri.data()), uri.size()).Finalize(hash.data());
    {
        std::lock_guard lock{m_mutex};
        if (m_used.contains(hash.GetHex())) throw LinkError("This link was used already: ask the site for a fresh one (a link does its work once)");
        if (request.verb == Verb::CONNECT && m_nonces.contains(request.nonce)) throw LinkError("This connect was answered already: ask the site again for a fresh one");
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

UniValue LinkHost::Connect(const Request& request, Source source, const std::string& wallet)
{
    interfaces::Node& node{m_deps.node()};
    // The wallet's address for the site: the one of its label, else a new one with it.
    std::string address;
    try {
        UniValue args{UniValue::VARR};
        args.push_back(CONNECT_LABEL);
        const UniValue found{node.executeRpc("getaddressesbylabel", args, WalletUri(wallet))};
        if (found.isObject()) {
            for (const std::string& key : found.getKeys()) {
                const UniValue& purpose{found.find_value(key).find_value("purpose")};
                if (purpose.isStr() && purpose.get_str() == "receive") {
                    address = key;
                    break;
                }
            }
        }
    } catch (...) {
        // No address of the label yet.
    }
    if (address.empty()) {
        UniValue args{UniValue::VARR};
        args.push_back(CONNECT_LABEL);
        try {
            const UniValue made{node.executeRpc("getnewaddress", args, WalletUri(wallet))};
            if (made.isStr()) address = made.get_str();
        } catch (const UniValue& error) {
            const UniValue& message{error.find_value("message")};
            throw HostError(message.isStr() ? message.get_str() : std::string{"The wallet gave no address"});
        }
    }
    const std::string answer{ConnectAnswer(request, address)};
    const Window::ConfirmDialog question{ConnectQuestion(request, source, wallet, address, m_deps.now())};
    if (!m_deps.ask || !m_deps.ask(question).accepted) throw HostError(BRIDGE_CANCELLED, "Not shared: it was declined.");
    {
        std::lock_guard lock{m_mutex};
        m_nonces.insert(request.nonce);
    }
    LogInfo("webui: an address was shared with %s, as the user accepted", request.origin);
    if (!m_deps.open || !m_deps.open(answer)) throw HostError("The browser could not be opened");
    UniValue out{UniValue::VOBJ};
    out.pushKV("opened", answer);
    out.pushKV("address", address);
    return out;
}

UniValue LinkHost::Run(Request request, Source source, const std::string& uri)
{
    interfaces::Node& node{m_deps.node()};
    const std::string wallet{Wallet(node)};
    LogInfo("webui: a link (%s): %s", SourceText(source), VerbName(request.verb));
    if (request.verb == Verb::CONNECT) return Connect(request, source, wallet);
    // A logo the link names by its hash (it cannot carry 32 KB): chosen by the user, refused if it is not that one.
    if ((request.verb == Verb::LAUNCH || request.verb == Verb::REGISTER) && request.logo_sha256 && request.logo.empty()) {
        Window::FileDialog chooser;
        chooser.title = strprintf("The logo of %s (SHA-256 %s…)", request.ticker, request.logo_sha256->substr(0, 12));
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
