// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitassets/launch.h>

#include <addresstype.h>
#include <bitassets/curves.h>
#include <bitassets/dex.h>
#include <bitassets/logo.h>
#include <bitassets/rpcutil.h>
#include <key_io.h>
#include <policy/feerate.h>
#include <rpc/protocol.h>
#include <rpc/request.h>
#include <tinyformat.h>
#include <util/moneystr.h>
#include <util/strencodings.h>

#include <cmath>

namespace bitassets {

namespace {
//! NameMimicsNumber: refused by the wallet, not relayed by the mempool.
constexpr const char* NAME_MIMICS_NUMBER{"The name holds '#', '(', ')' or four digits, a dash and four digits: shown with its number, it would read as the number of another asset"};

void NeedLogos(const LaunchRules& rules)
{
    if (!rules.logos) throw JSONRPCError(RPC_WALLET_ERROR, "The logos of assets are not in the rules of the chain yet (logo_height in getbitassetsinfo)");
}

void NeedTickers(const LaunchRules& rules)
{
    if (!rules.tickers) throw JSONRPCError(RPC_WALLET_ERROR, "Display names of assets are not in the rules of the chain yet (ticker_height in getbitassetsinfo)");
}
} // namespace

LaunchRules LaunchRulesAt(const Consensus::SidechainParams& rules, int height)
{
    LaunchRules r;
    r.height = height;
    r.tickers = rules.TickerActive(height);
    r.logos = rules.LogoActive(height);
    r.curves = rules.DexCurvesActive(height);
    r.params = rules.LaunchParamsActive(height);
    r.fee_flow = rules.FeeFlowActive(height);
    r.fee_split = rules.FeeSplitActive(height);
    r.max_allocation = MaxLaunchAllocation(rules.Allocation10Active(height));
    r.min_chn = MinChnPayout(height);
    r.reveal_depth = rules.bitassets_reveal_depth;
    r.network_fee = rules.launch_fee;
    r.graduation = rules.LaunchGraduation(height);
    return r;
}

std::string NewAssetName(const std::string& given, bool tickers)
{
    if (!tickers) return given;
    const std::string name{ToUpper(given)};
    if (!IsTicker(name)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The name of a new asset is a ticker (from ticker_height in getbitassetsinfo): %u to %u characters, "
                                                            "letters A to Z and digits 0 to 9 only (\"%s\" is not one). A longer name goes in the display name",
                                                            MIN_TICKER_SIZE, MAX_TICKER_SIZE, name));
    }
    return name;
}

void CheckReserveName(const std::string& name)
{
    if (!IsAssetName(name)) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("A name is 1 to %u printable characters, without a space at either end", MAX_NAME_TEXT_SIZE));
    if (ReadsAsAnotherAsset(name)) throw JSONRPCError(RPC_INVALID_PARAMETER, "The name reads as another asset (" + CURRENCY_UNIT + ", a number such as 1739-0029, or \"0x\" and a hash)");
    if (NameMimicsNumber(name)) throw JSONRPCError(RPC_INVALID_PARAMETER, NAME_MIMICS_NUMBER);
}

std::vector<unsigned char> ParseLogo(const UniValue& value, bool allow_empty)
{
    const std::string& text{value.get_str()};
    // Base64 of the largest logo, and no more: nothing larger is decoded.
    if (text.size() > (MAX_LOGO_BYTES + 2) / 3 * 4) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The rules refuse this logo (bad-ba-logo-oversize): a logo is at most %u bytes", MAX_LOGO_BYTES));
    const auto bytes{DecodeBase64(text)};
    if (!bytes) throw JSONRPCError(RPC_INVALID_PARAMETER, "The logo is not base64");
    if (bytes->empty()) {
        if (!allow_empty) throw JSONRPCError(RPC_INVALID_PARAMETER, "The logo is empty");
        return {};
    }
    std::string reason;
    if (!CheckLogo(*bytes, reason)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The rules refuse this logo (%s): a PNG or a JPEG of exactly %ux%u pixels, at most %u bytes, "
                                                            "without metadata or chunks other than the image's (see doc/bitassets.md, Logos)",
                                                            reason, LOGO_SIZE, LOGO_SIZE, MAX_LOGO_BYTES));
    }
    return *bytes;
}

std::string ParseDisplayName(const UniValue& value, bool allow_empty)
{
    const std::string& text{value.get_str()};
    if (text.empty()) {
        if (!allow_empty) throw JSONRPCError(RPC_INVALID_PARAMETER, "The display name is empty");
        return text;
    }
    if (const auto why{WhyNotDisplayName(text)}) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The rules refuse this display name (bad-ba-display-name): %s. A display name is 1 to %u bytes of UTF-8 in NFC, "
                                                            "printable, without a space at either end", *why, MAX_DISPLAY_NAME_SIZE));
    }
    return text;
}

uint32_t LaunchFeeArg(const UniValue& value)
{
    const double fee{value.isNum() ? value.get_real() : std::stod(value.get_str())};
    const long long ppm{std::llround(fee * 10000)};
    if (!(fee > 0) || ppm < MIN_LAUNCH_FEE || ppm > MAX_LAUNCH_FEE) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The curve's fee is %g to %g percent", MIN_LAUNCH_FEE / 10000.0, MAX_LAUNCH_FEE / 10000.0));
    }
    return static_cast<uint32_t>(ppm);
}

void CheckLaunchSupply(uint64_t supply, uint8_t decimals)
{
    if (supply % LAUNCH_SUPPLY_MULTIPLE != 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("A curve's supply is a multiple of %u units (%s of %u decimals)", LAUNCH_SUPPLY_MULTIPLE,
                                                            FormatUnits(LAUNCH_SUPPLY_MULTIPLE, decimals), decimals));
    }
    if (supply < MIN_LAUNCH_SUPPLY || supply > MAX_LAUNCH_SUPPLY) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("A curve's supply is %s to %s (at %u decimals)", FormatUnits(MIN_LAUNCH_SUPPLY, decimals),
                                                            FormatUnits(MAX_LAUNCH_SUPPLY, decimals), decimals));
    }
}

void CheckFirstBuy(int64_t graduation, uint64_t supply, uint32_t fee, uint64_t first_buy, uint64_t first_min_out, uint64_t min_buy, uint64_t max_buy, uint8_t decimals)
{
    if (first_min_out == 0) return;
    if (first_buy == 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "first_min_out goes with a first buy");
    const auto quote{curves::ClearLaunch(curves::LaunchCurve{fee, supply / 8 * 3, max_buy, min_buy}, static_cast<uint64_t>(graduation) / 2, supply / 8 * 9,
                                         {dex::HopIn{true, first_buy}}, {true})};
    if (!quote || quote->outs[0] < first_min_out) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The first buy gets %s alone in its block, less than first_min_out", FormatUnits(quote ? quote->outs[0] : 0, decimals)));
    }
}

std::vector<FeeShare> ParseFeeSplit(const UniValue& value)
{
    if (!value.isArray()) throw JSONRPCError(RPC_INVALID_PARAMETER, "fee_split: an array of {\"to\", \"bps\"}");
    if (value.size() == 0 || value.size() > MAX_FEE_SHARES) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("fee_split: 1 to %u entries", MAX_FEE_SHARES));
    std::vector<FeeShare> split;
    for (const UniValue& entry : value.getValues()) {
        if (!entry.isObject() || !entry["to"].isStr() || !entry["bps"].isNum()) throw JSONRPCError(RPC_INVALID_PARAMETER, "fee_split: each entry is {\"to\": \"creator\" | \"burn\" | address, \"bps\": 1 to 10000}");
        const int64_t bps{entry["bps"].getInt<int64_t>()};
        if (bps < 1 || bps > FEE_SPLIT_BPS) throw JSONRPCError(RPC_INVALID_PARAMETER, "fee_split: a share is 1 to 10000 basis points");
        FeeShare share;
        share.bps = static_cast<uint16_t>(bps);
        const std::string& to{entry["to"].get_str()};
        if (to == "creator") {
            share.kind = FeeShare::CREATOR;
        } else if (to == "burn") {
            share.kind = FeeShare::BURN;
        } else {
            const CTxDestination dest{DecodeDestination(to)};
            if (!IsValidDestination(dest)) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "fee_split: not an address of this chain: " + to);
            share.kind = FeeShare::ADDRESS;
            share.script = GetScriptForDestination(dest);
        }
        split.push_back(std::move(share));
    }
    if (!CheckFeeSplit(split)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "fee_split: the shares sum to 10000 basis points, with the creator and the burn at most once each and up to 3 different addresses");
    }
    return split;
}

LaunchPlan ParseLaunchOptions(const std::string& name, const UniValue& options_in, const LaunchRules& rules)
{
    if (!rules.curves) throw JSONRPCError(RPC_MISC_ERROR, "The DEX's pool kinds do not apply yet (getdexinfo's curves_active)");
    if (ReadsAsAnotherAsset(name)) throw JSONRPCError(RPC_INVALID_PARAMETER, "The name reads as another asset (" + CURRENCY_UNIT + ", a number such as 1739-0029, or \"0x\" and a hash)");
    if (NameMimicsNumber(name)) throw JSONRPCError(RPC_INVALID_PARAMETER, NAME_MIMICS_NUMBER);
    const UniValue none{UniValue::VOBJ};
    const UniValue& options{options_in.isNull() ? none : options_in};
    // Its parameters: the network's unless chosen (a launch with parameters only where they apply).
    int decimals{LAUNCH_DECIMALS};
    if (options.exists("decimals") && !options["decimals"].isNull()) {
        decimals = options["decimals"].getInt<int>();
        if (decimals < 0 || decimals > MAX_DECIMALS) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Decimals are 0 to %u", MAX_DECIMALS));
    }
    uint64_t supply{LAUNCH_SUPPLY};
    if (options.exists("supply") && !options["supply"].isNull()) supply = ParseUnits(options["supply"], static_cast<uint8_t>(decimals));
    else if (decimals != LAUNCH_DECIMALS) throw JSONRPCError(RPC_INVALID_PARAMETER, "With decimals of its own, a launch names its supply");
    const uint32_t fee{options.exists("fee") && !options["fee"].isNull() ? LaunchFeeArg(options["fee"]) : rules.network_fee};
    const bool with_params{supply != LAUNCH_SUPPLY || decimals != LAUNCH_DECIMALS || fee != rules.network_fee};
    if (with_params) {
        if (!rules.params) {
            throw JSONRPCError(RPC_MISC_ERROR, "Launches with parameters (supply, decimals, fee; an existing asset on a curve) do not apply yet on this network (getlaunchstats' rules.params_active)");
        }
        CheckLaunchSupply(supply, static_cast<uint8_t>(decimals));
    }
    LaunchPlan plan;
    plan.supply = supply;
    plan.decimals = static_cast<uint8_t>(decimals);
    plan.fee = fee;
    LaunchToken& launch{plan.launch};
    launch.reg.name = HashName(name);
    launch.reg.decimals = static_cast<uint8_t>(decimals);
    launch.reg.text = name;
    if (with_params) {
        launch.reg.supply = supply;
        launch.fee = fee;
    }
    if (options.exists("display_name") && !options["display_name"].get_str().empty()) launch.reg.display_name = ParseDisplayName(options["display_name"], /*allow_empty=*/false);
    if (options.exists("logo") && !options["logo"].get_str().empty()) {
        NeedLogos(rules);
        launch.reg.logo = ParseLogo(options["logo"], /*allow_empty=*/false);
    }
    if (options.exists("info")) {
        const std::string info{options["info"].get_str()};
        if (info.size() > MAX_INFO_SIZE) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The description is at most %u bytes", MAX_INFO_SIZE));
        if (!info.empty()) launch.reg.data.info = info;
    }
    const double allocation{options.exists("allocation") ? options["allocation"].get_real() : 0.0};
    if (!(allocation >= 0 && allocation <= rules.max_allocation / 100.0)) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The allocation is 0 to %d percent", rules.max_allocation / 100));
    launch.allocation = static_cast<uint16_t>(std::llround(allocation * 100));
    // The supply and the allocation minted on top of it are within an amount (IsLaunchAllocation).
    if (!IsLaunchAllocation(supply, launch.allocation)) throw JSONRPCError(RPC_INVALID_PARAMETER, "The supply with its allocation is more than an asset can have: a smaller supply or allocation");
    const auto coin{[&](const char* key) { return options.exists(key) ? ParseUnits(options[key], CHN_DECIMALS, /*allow_zero=*/true) : uint64_t{0}; }};
    launch.min_buy = coin("min_buy");
    launch.max_buy = coin("max_buy");
    if (launch.max_buy != 0 && launch.max_buy < launch.min_buy) throw JSONRPCError(RPC_INVALID_PARAMETER, "max_buy is less than min_buy");
    launch.first_buy = coin("first_buy");
    if (options.exists("first_min_out")) launch.first_min_out = ParseUnits(options["first_min_out"], static_cast<uint8_t>(decimals), /*allow_zero=*/true);
    CheckFirstBuy(rules.graduation, supply, fee, launch.first_buy, launch.first_min_out, launch.min_buy, launch.max_buy, static_cast<uint8_t>(decimals));
    // The fee-flow release's options (the node refuses them before its height: bad-ba-marker).
    if (options.exists("fee_destination")) {
        const std::string destination{options["fee_destination"].get_str()};
        if (destination == "creator") {
            launch.destination = static_cast<uint8_t>(FeeDestination::CREATOR);
        } else if (destination == "burn") {
            launch.destination = static_cast<uint8_t>(FeeDestination::BURN);
        } else if (destination == "split") {
            launch.destination = static_cast<uint8_t>(FeeDestination::SPLIT);
        } else {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "fee_destination: creator, burn or split");
        }
    }
    if (launch.destination == static_cast<uint8_t>(FeeDestination::SPLIT)) {
        const double percent{options.exists("burn_percent") ? options["burn_percent"].get_real() : 0.0};
        const long bps{std::lround(percent * 100)};
        if (bps <= 0 || bps >= 10000) throw JSONRPCError(RPC_INVALID_PARAMETER, "burn_percent: above 0 and below 100 with a split");
        launch.burn_bps = static_cast<uint16_t>(bps);
    } else if (options.exists("burn_percent") && options["burn_percent"].get_real() != 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "burn_percent goes with fee_destination \"split\"");
    }
    // Upgrade v7's split (the node refuses it before its height: bad-ba-marker).
    if (options.exists("fee_split") && !options["fee_split"].isNull()) {
        if (options.exists("fee_destination") || options.exists("burn_percent")) throw JSONRPCError(RPC_INVALID_PARAMETER, "fee_split replaces fee_destination and burn_percent: give one or the other");
        launch.split = ParseFeeSplit(options["fee_split"]);
        // All to the creator is the default: no split in the marker.
        if (launch.split.size() == 1 && launch.split[0].kind == FeeShare::CREATOR) launch.split.clear();
    }
    launch.keep_control = options.exists("keep_control") && options["keep_control"].get_bool();
    if (launch.first_buy > 0 && launch.first_buy < rules.min_chn) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The first buy pays %s %s at least", FormatMoney(rules.min_chn), CURRENCY_UNIT));
    return plan;
}

Register ParseRegistration(const std::string& name, const UniValue& supply_arg, const UniValue& decimals_arg, const UniValue& data, const UniValue& is_public,
                           const UniValue& logo, const UniValue& display_name, const LaunchRules& rules)
{
    if (rules.tickers && !is_public.isNull() && !is_public.get_bool()) throw JSONRPCError(RPC_INVALID_PARAMETER, "From ticker_height the name of a new asset (its ticker) is published: public cannot be false");
    if (ReadsAsAnotherAsset(name)) throw JSONRPCError(RPC_INVALID_PARAMETER, "The name reads as another asset (" + CURRENCY_UNIT + ", a number such as 1739-0029, or \"0x\" and a hash)");
    const int decimals{decimals_arg.isNull() ? 0 : decimals_arg.getInt<int>()};
    if (decimals < 0 || decimals > MAX_DECIMALS) throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The decimals are from 0 to %u", MAX_DECIMALS));
    const uint64_t supply{ParseUnits(supply_arg, decimals, /*allow_zero=*/true)};
    Register reg;
    reg.name = HashName(name);
    reg.supply = supply;
    reg.decimals = decimals;
    if (!data.isNull()) reg.data = ParseData(data);
    if (!logo.isNull() && !logo.get_str().empty()) {
        NeedLogos(rules);
        reg.logo = ParseLogo(logo, /*allow_empty=*/false);
    }
    if (!display_name.isNull() && !display_name.get_str().empty()) {
        NeedTickers(rules);
        reg.display_name = ParseDisplayName(display_name, /*allow_empty=*/false);
    }
    if (is_public.isNull() || is_public.get_bool()) {
        // Published, it would read as another asset's number where names are shown with theirs.
        if (NameMimicsNumber(name)) throw JSONRPCError(RPC_INVALID_PARAMETER, std::string{NAME_MIMICS_NUMBER} + " (register it unpublished: public false)");
        reg.text = name;
    }
    return reg;
}

std::vector<OutToken> LaunchOutputs(const LaunchToken& launch)
{
    std::vector<OutToken> outs;
    // The first buy's result output, then the control coin kept (the first token output the marker names).
    if (launch.first_buy > 0) outs.emplace_back(std::nullopt);
    if (launch.keep_control) outs.emplace_back(Token{Token::Kind::CONTROL, launch.reg.name, 1});
    return outs;
}

std::vector<OutToken> RegisterOutputs(const Register& reg)
{
    std::vector<OutToken> outs{Token{Token::Kind::CONTROL, reg.name, 1}};
    if (reg.supply > 0) outs.emplace_back(Token{Token::Kind::ASSET, reg.name, reg.supply});
    return outs;
}

std::vector<OutToken> ReserveOutputs()
{
    // A null id: the transaction's own reservation.
    return {Token{Token::Kind::RESERVATION, uint256{}, 1}};
}

Marker MarkerOf(std::optional<Operation> operation, const std::vector<OutToken>& outputs)
{
    Marker marker;
    marker.operation = std::move(operation);
    for (size_t i{0}; i < outputs.size(); ++i) marker.outputs.push_back({static_cast<uint32_t>(i), outputs[i]});
    return marker;
}

} // namespace bitassets
