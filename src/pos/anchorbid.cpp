// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/anchorbid.h>

#include <algorithm>
#include <map>

#include <core_io.h>
#include <tinyformat.h>
#include <univalue.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <algorithm>
#include <charconv>
#include <cmath>

namespace pos {

namespace {
constexpr std::pair<BidStrategy, const char*> STRATEGIES[]{
    {BidStrategy::FEES, "fees"},
    {BidStrategy::FEES_MINIMUM, "feesminimum"},
    {BidStrategy::MINIMUM, "minimum"},
    {BidStrategy::FIXED, "fixed"},
    {BidStrategy::OFF, "off"},
};

//! Without a fee estimate, the base of an escalated bid: the floor, but this at least (a floor of 0 escalates too).
constexpr CAmount ESCALATION_FALLBACK_BASE{564};

std::optional<double> Number(const UniValue& value)
{
    if (value.isNum()) return value.get_real();
    if (!value.isStr()) return std::nullopt;
    const std::string text{util::TrimString(value.get_str())};
    double out{0};
    const auto [end, ec]{std::from_chars(text.data(), text.data() + text.size(), out)};
    if (ec != std::errc{} || end != text.data() + text.size() || !std::isfinite(out)) return std::nullopt;
    return out;
}

std::optional<int64_t> Integer(const UniValue& value)
{
    const auto number{Number(value)};
    if (!number || *number != std::floor(*number) || std::abs(*number) > 1e15) return std::nullopt;
    return static_cast<int64_t>(*number);
}

std::optional<bool> Boolean(const UniValue& value)
{
    if (value.isBool()) return value.get_bool();
    if (value.isNum()) return value.getValStr() != "0";
    if (!value.isStr()) return std::nullopt;
    const std::string text{ToLower(util::TrimString(value.get_str()))};
    if (text == "1" || text == "true" || text == "on" || text == "yes") return true;
    if (text == "0" || text == "false" || text == "off" || text == "no") return false;
    return std::nullopt;
}

std::optional<CAmount> Money(const UniValue& value)
{
    if (!value.isNum() && !value.isStr()) return std::nullopt;
    if (const auto parsed{ParseMoney(util::TrimString(value.getValStr()))}) return parsed;
    // A number the JSON wrote in another form (1e-05).
    const auto number{Number(value)};
    if (!number || *number < 0 || *number > static_cast<double>(MAX_MONEY) / COIN) return std::nullopt;
    return static_cast<CAmount>(std::llround(*number * COIN));
}

/** "0.5:1,0.75:2,0.9:4", or an array of {pressure, multiplier} (or of [pressure, multiplier]). */
std::optional<std::vector<EscalationPoint>> Curve(const UniValue& value)
{
    std::vector<EscalationPoint> points;
    if (value.isStr()) {
        for (const std::string& part : util::SplitString(value.get_str(), ',')) {
            const auto pair{util::SplitString(part, ':')};
            if (pair.size() != 2) return std::nullopt;
            const auto p{Number(UniValue{std::string{pair[0]}})};
            const auto m{Number(UniValue{std::string{pair[1]}})};
            if (!p || !m) return std::nullopt;
            points.push_back(EscalationPoint{*p, *m});
        }
        return points;
    }
    if (!value.isArray()) return std::nullopt;
    for (const UniValue& point : value.getValues()) {
        std::optional<double> p, m;
        if (point.isObject()) {
            p = Number(point.find_value("pressure"));
            m = Number(point.find_value("multiplier"));
        } else if (point.isArray() && point.size() == 2) {
            p = Number(point[0]);
            m = Number(point[1]);
        }
        if (!p || !m) return std::nullopt;
        points.push_back(EscalationPoint{*p, *m});
    }
    return points;
}

CAmount Clamp(double amount)
{
    if (!(amount > 0)) return 0;
    if (amount >= static_cast<double>(MAX_MONEY)) return MAX_MONEY;
    return static_cast<CAmount>(std::llround(amount));
}
} // namespace

std::string StrategyName(BidStrategy strategy)
{
    for (const auto& [s, name] : STRATEGIES) {
        if (s == strategy) return name;
    }
    return "";
}

std::optional<StrategyWord> ParseStrategyWord(const std::string& name, StrategyContext context)
{
    std::string word{ToLower(util::TrimString(name))};
    std::erase_if(word, [](char c) { return c == ' ' || c == '_' || c == '-'; });
    const bool legacy{context == StrategyContext::LEGACY_SETTINGS};
    // The names before 8.3.6 alone (doc/fastblocks-anchor-bids.md, "The names before 8.3.6"): each also said whether to
    // prevent stalling, wherever it is said.
    if (word == "feespreventstall" || word == "fees+preventstalling" || word == "fees+preventstall") {
        return StrategyWord{.strategy = BidStrategy::FEES, .prevent_stall = true, .fees_or_floor = true};
    }
    if (word == "minimal" || word == "keepmoving") return StrategyWord{.strategy = BidStrategy::MINIMUM, .prevent_stall = true};
    // Old and new names: 8.3.5's meaning only in 8.3.5's settings (fees bid at least its floor, fees and fixed without
    // stall prevention); a new name says nothing of stall prevention ("preventstall": false is what turns it off).
    if (word == "fees") {
        if (legacy) return StrategyWord{.strategy = BidStrategy::FEES, .prevent_stall = false, .fees_or_floor = true};
        return StrategyWord{.strategy = BidStrategy::FEES};
    }
    if (word == "fixed") {
        if (legacy) return StrategyWord{.strategy = BidStrategy::FIXED, .prevent_stall = false};
        return StrategyWord{.strategy = BidStrategy::FIXED};
    }
    // 8.3.5's off turned the anchorer off alone (setbmm still bid): so in its settings and in -validatoranchorbidstrategy,
    // which it read; the new Off (setanchorbidpolicy, the app) is no bids at all.
    if (word == "off") {
        if (context == StrategyContext::CURRENT) return StrategyWord{.strategy = BidStrategy::OFF};
        return StrategyWord{.anchor_off = true};
    }
    // The new names, and the words of the pages.
    if (word == "feesminimum" || word == "fees+minimum") return StrategyWord{.strategy = BidStrategy::FEES_MINIMUM};
    if (word == "minimum" || word == "minimumeveryblock") return StrategyWord{.strategy = BidStrategy::MINIMUM};
    return std::nullopt;
}

bool IsLegacySettings(const UniValue& saved)
{
    return saved.isObject() && saved.find_value("preventstall").isNull() && saved.find_value("anchor").isNull();
}

std::optional<BidStrategy> ParseStrategy(const std::string& name)
{
    const auto word{ParseStrategyWord(name)};
    return word ? std::optional{word->strategy} : std::nullopt;
}

const std::vector<std::string>& PolicyFieldNames()
{
    static const std::vector<std::string> names{"strategy", "preventstall", "anchor", "floor", "max", "fixed", "feeshare", "escalationstart", "curve", "interpolate",
                                                "usefeeestimate", "requestvsize", "rebidafter", "rebidstep", "rebidmax", "warnmissed",
                                                "periodcap", "periodhours", "lowbalanceblocks", "flatboost", "boost", "stallreserve"};
    return names;
}

std::string CheckPolicy(const AnchorBidPolicy& policy)
{
    if (StrategyName(policy.strategy).empty()) return "unknown strategy";
    if (!MoneyRange(policy.floor)) return "floor: an amount from 0 to all the coins";
    if (!MoneyRange(policy.max)) return "max: an amount from 0 to all the coins";
    if (!MoneyRange(policy.fixed)) return "fixed: an amount from 0 to all the coins";
    if (!MoneyRange(policy.period_cap)) return "periodcap: an amount from 0 to all the coins (0: no cap)";
    if (policy.floor > policy.max) return strprintf("floor (%s) above max (%s)", FormatMoney(policy.floor), FormatMoney(policy.max));
    if (policy.fixed > policy.max) return strprintf("fixed (%s) above max (%s)", FormatMoney(policy.fixed), FormatMoney(policy.max));
    if (policy.fee_share_bps < 0 || policy.fee_share_bps > 10000) return "feeshare: a percentage from 0 to 100";
    if (!(policy.escalation_start > 0) || policy.escalation_start > 1) return "escalationstart: a stall pressure above 0, at most 1";
    if (policy.curve.empty() || policy.curve.size() > 8) return "curve: 1 to 8 points";
    for (size_t i{0}; i < policy.curve.size(); ++i) {
        const EscalationPoint& point{policy.curve[i]};
        if (!(point.pressure >= 0) || point.pressure > 1) return "curve: pressures from 0 to 1";
        if (!(point.multiplier >= 1) || point.multiplier > 1000) return "curve: multipliers from 1 to 1000";
        if (i > 0 && !(point.pressure > policy.curve[i - 1].pressure)) return "curve: pressures in ascending order";
        if (i > 0 && point.multiplier < policy.curve[i - 1].multiplier) return "curve: multipliers that do not fall";
    }
    if (policy.request_vsize < 50 || policy.request_vsize > 10000) return "requestvsize: from 50 to 10000 vbytes";
    if (policy.rebid_after < 0 || policy.rebid_after > 1000) return "rebidafter: from 0 (never) to 1000 mainchain blocks";
    if (!(policy.rebid_step >= 1) || policy.rebid_step > 100) return "rebidstep: a multiplier from 1 to 100";
    if (!(policy.rebid_max >= 1) || policy.rebid_max > 1000) return "rebidmax: a multiplier from 1 to 1000";
    if (policy.warn_missed < 0 || policy.warn_missed > 1000) return "warnmissed: from 0 (never) to 1000 mainchain blocks";
    if (policy.period_hours < 1 || policy.period_hours > 24 * 31) return "periodhours: from 1 to 744";
    if (policy.low_balance_blocks < 0 || policy.low_balance_blocks > 1'000'000) return "lowbalanceblocks: from 0 (no warning) to 1000000";
    if (!(policy.boost >= 1) || policy.boost > 1000) return "boost: a multiplier of the floor from 1 to 1000";
    if (policy.stall_reserve_pct < 0 || policy.stall_reserve_pct > 100) return "stallreserve: a percentage of the cap from 0 to 100";
    return "";
}

UniValue PolicyToUniValue(const AnchorBidPolicy& policy)
{
    UniValue out(UniValue::VOBJ);
    out.pushKV("strategy", StrategyName(policy.strategy));
    out.pushKV("preventstall", policy.prevent_stall);
    out.pushKV("anchor", policy.anchor);
    out.pushKV("floor", ValueFromAmount(policy.floor));
    out.pushKV("max", ValueFromAmount(policy.max));
    out.pushKV("fixed", ValueFromAmount(policy.fixed));
    out.pushKV("feeshare", policy.fee_share_bps / 100.0);
    out.pushKV("escalationstart", policy.escalation_start);
    out.pushKV("flatboost", policy.flat_boost);
    out.pushKV("boost", policy.boost);
    UniValue curve(UniValue::VARR);
    for (const EscalationPoint& point : policy.curve) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("pressure", point.pressure);
        entry.pushKV("multiplier", point.multiplier);
        curve.push_back(std::move(entry));
    }
    out.pushKV("curve", std::move(curve));
    out.pushKV("interpolate", policy.interpolate);
    out.pushKV("usefeeestimate", policy.use_fee_estimate);
    out.pushKV("requestvsize", policy.request_vsize);
    out.pushKV("rebidafter", policy.rebid_after);
    out.pushKV("rebidstep", policy.rebid_step);
    out.pushKV("rebidmax", policy.rebid_max);
    out.pushKV("warnmissed", policy.warn_missed);
    out.pushKV("periodcap", ValueFromAmount(policy.period_cap));
    out.pushKV("periodhours", policy.period_hours);
    out.pushKV("stallreserve", policy.stall_reserve_pct);
    out.pushKV("lowbalanceblocks", policy.low_balance_blocks);
    return out;
}

bool ApplyPolicyFields(AnchorBidPolicy& policy, const UniValue& fields, std::string& error, StrategyContext context)
{
    if (!fields.isObject()) {
        error = "the policy is an object of fields";
        return false;
    }
    // A strategy of the old names says whether to prevent stalling, unless "preventstall" is given too (in any order).
    const bool stall_given{!fields.find_value("preventstall").isNull()};
    const bool anchor_given{!fields.find_value("anchor").isNull()};
    bool fees_or_floor{false};
    for (const std::string& key : fields.getKeys()) {
        const UniValue& value{fields.find_value(key)};
        const auto bad{[&](const std::string& what) {
            error = strprintf("%s: %s", key, what);
            return false;
        }};
        const auto money{[&](CAmount& out) {
            const auto amount{Money(value)};
            if (!amount || !MoneyRange(*amount)) return bad("an amount of coins");
            out = *amount;
            return true;
        }};
        const auto integer{[&](int& out) {
            const auto number{Integer(value)};
            if (!number || *number < 0 || *number > 1'000'000'000) return bad("a whole number");
            out = static_cast<int>(*number);
            return true;
        }};
        const auto real{[&](double& out) {
            const auto number{Number(value)};
            if (!number) return bad("a number");
            out = *number;
            return true;
        }};
        const auto boolean{[&](bool& out) {
            const auto b{Boolean(value)};
            if (!b) return bad("true or false");
            out = *b;
            return true;
        }};
        bool ok{true};
        if (key == "strategy") {
            const auto word{value.isStr() ? ParseStrategyWord(value.get_str(), context) : std::nullopt};
            if (!word) return bad("fees, feesminimum, minimum, fixed or off (or the older minimal, feespreventstall)");
            if (word->anchor_off) {
                // 8.3.5's off: the anchorer off, the strategy as it was.
                if (!anchor_given) policy.anchor = false;
            } else {
                policy.strategy = word->strategy;
            }
            if (word->prevent_stall && !stall_given) policy.prevent_stall = *word->prevent_stall;
            fees_or_floor = word->fees_or_floor;
        } else if (key == "preventstall") {
            ok = boolean(policy.prevent_stall);
        } else if (key == "anchor") {
            ok = boolean(policy.anchor);
        } else if (key == "floor") {
            ok = money(policy.floor);
        } else if (key == "max") {
            ok = money(policy.max);
        } else if (key == "fixed") {
            ok = money(policy.fixed);
        } else if (key == "periodcap") {
            ok = money(policy.period_cap);
        } else if (key == "feeshare") {
            const auto percent{Number(value)};
            if (!percent || *percent < 0 || *percent > 100) return bad("a percentage from 0 to 100");
            policy.fee_share_bps = static_cast<int>(std::lround(*percent * 100));
        } else if (key == "escalationstart") {
            ok = real(policy.escalation_start);
        } else if (key == "curve") {
            const auto curve{Curve(value)};
            if (!curve) return bad("points of pressure and multiplier: \"0.5:1,0.75:2,0.9:4\", or [{\"pressure\":0.5,\"multiplier\":1}, ...]");
            policy.curve = *curve;
        } else if (key == "interpolate") {
            ok = boolean(policy.interpolate);
        } else if (key == "usefeeestimate") {
            ok = boolean(policy.use_fee_estimate);
        } else if (key == "requestvsize") {
            ok = integer(policy.request_vsize);
        } else if (key == "rebidafter") {
            ok = integer(policy.rebid_after);
        } else if (key == "rebidstep") {
            ok = real(policy.rebid_step);
        } else if (key == "rebidmax") {
            ok = real(policy.rebid_max);
        } else if (key == "warnmissed") {
            ok = integer(policy.warn_missed);
        } else if (key == "periodhours") {
            ok = integer(policy.period_hours);
        } else if (key == "lowbalanceblocks") {
            ok = integer(policy.low_balance_blocks);
        } else if (key == "flatboost") {
            ok = boolean(policy.flat_boost);
        } else if (key == "boost") {
            ok = real(policy.boost);
        } else if (key == "stallreserve") {
            const auto percent{Integer(value)};
            if (!percent || *percent < 0 || *percent > 100) return bad("a whole percentage from 0 to 100");
            policy.stall_reserve_pct = static_cast<int>(*percent);
        } else {
            error = strprintf("%s: not a field of the policy (%s)", key, util::Join(PolicyFieldNames(), ", "));
            return false;
        }
        if (!ok) return false;
    }
    // The old feespreventstall bid at least its floor: with one, that is Fees + minimum now (with a floor of 0, Fees).
    if (fees_or_floor && policy.floor > 0) policy.strategy = BidStrategy::FEES_MINIMUM;
    return true;
}

void ReapplySavedStrategy(AnchorBidPolicy& policy, const UniValue& overrides, const UniValue& fields)
{
    if (!fields.isObject() || !fields.find_value("strategy").isNull() || !overrides.isObject()) return;
    const UniValue& saved{overrides.find_value("strategy")};
    if (!saved.isStr()) return;
    const auto word{ParseStrategyWord(saved.get_str(), IsLegacySettings(overrides) ? StrategyContext::LEGACY_SETTINGS : StrategyContext::CURRENT)};
    if (!word || !word->fees_or_floor) return;
    if (policy.strategy != BidStrategy::FEES && policy.strategy != BidStrategy::FEES_MINIMUM) return;
    policy.strategy = policy.floor > 0 ? BidStrategy::FEES_MINIMUM : BidStrategy::FEES;
}

std::optional<uint8_t> BmmRequestSlot(std::span<const unsigned char> script, const uint256& prev)
{
    if (script.size() != 70 || script[0] != 0x6a || script[1] != 0x44 || script[2] != 0x00 || script[3] != 0xbf || script[4] != 0x00) return std::nullopt;
    if (!std::equal(prev.begin(), prev.end(), script.begin() + 38)) return std::nullopt;
    return script[5];
}

std::optional<CAmount> RobustIncludedRate(const std::vector<std::vector<BmmRequestSeen>>& blocks, const std::set<uint8_t>& active, std::optional<uint8_t> own,
                                          size_t min_slots)
{
    const auto median{[](std::vector<CAmount> v) { std::sort(v.begin(), v.end()); return v[(v.size() - 1) / 2]; }};
    std::vector<CAmount> per_block;
    for (const auto& block : blocks) {
        // One value an active slot: the lowest rate of its requests in the block (a real bidder needs one landed request;
        // stuffing a block with many requests of a few slots adds nothing but those slots' lowest).
        std::map<uint8_t, CAmount> by_slot;
        for (const BmmRequestSeen& r : block) {
            if (r.rate_per_kvb <= 0 || !active.contains(r.slot) || (own && r.slot == *own)) continue;
            auto [it, fresh]{by_slot.emplace(r.slot, r.rate_per_kvb)};
            if (!fresh) it->second = std::min(it->second, r.rate_per_kvb);
        }
        if (by_slot.size() < min_slots) continue;
        std::vector<CAmount> rates;
        for (const auto& [slot, rate] : by_slot) rates.push_back(rate);
        per_block.push_back(median(std::move(rates)));
    }
    if (per_block.empty()) return std::nullopt;
    return median(std::move(per_block));
}

double StallPressure(int64_t slot, int64_t first_unanchored_slot, int64_t max_unanchored_slots)
{
    if (first_unanchored_slot < 0 || max_unanchored_slots <= 0 || slot <= first_unanchored_slot) return 0;
    return static_cast<double>(slot - first_unanchored_slot) / static_cast<double>(max_unanchored_slots);
}

double EscalationMultiplier(const AnchorBidPolicy& policy, double p)
{
    const auto& curve{policy.curve};
    if (curve.empty()) return 1;
    if (p <= curve.front().pressure) return curve.front().multiplier;
    for (size_t i{0}; i + 1 < curve.size(); ++i) {
        const EscalationPoint& a{curve[i]};
        const EscalationPoint& b{curve[i + 1]};
        if (p < b.pressure) {
            if (!policy.interpolate) return a.multiplier;
            return a.multiplier + (b.multiplier - a.multiplier) * (p - a.pressure) / (b.pressure - a.pressure);
        }
    }
    return curve.back().multiplier;
}

CAmount CapLeft(const AnchorBidPolicy& policy, CAmount spent, bool stall)
{
    if (policy.period_cap <= 0) return MAX_MONEY;
    const CAmount reserve{stall ? 0 : policy.period_cap / 100 * policy.stall_reserve_pct + policy.period_cap % 100 * policy.stall_reserve_pct / 100};
    return std::max<CAmount>(policy.period_cap - reserve - spent, 0);
}

std::optional<CAmount> ReplacementBid(const AnchorBidPolicy& policy, CAmount amount, CAmount previous, CAmount spent, bool stall)
{
    const CAmount replacement{std::max(amount, previous + 500)};
    if (replacement > policy.max) return std::nullopt;
    if (replacement > CapLeft(policy, spent, stall)) return std::nullopt;
    return replacement;
}

namespace {
/** The relay floor for the largest request a wallet makes (250 vB: a request of more inputs than the usual 189 vB was
 * refused under it, "min relay fee not met, 19 < 21"). */
CAmount RelayFloor(const AnchorBidPolicy& policy, const BidInputs& inputs)
{
    return Clamp(std::ceil(static_cast<double>(std::max<CAmount>(inputs.relay_rate_per_kvb, 0)) * std::max(policy.request_vsize, 250) / 1000.0));
}
} // namespace

CAmount MinimumBid(const AnchorBidPolicy& policy, const BidInputs& inputs)
{
    return std::max(policy.floor, RelayFloor(policy, inputs));
}

namespace {
/**
 * What a bid must be to land when it must (stall prevention's base): the minimum; the next-block estimate (with
 * use_fee_estimate), else what recent mainchain blocks took for a BMM request, rounded up to whole sat/vB; never below the
 * network's least competitive rate (min_rate_per_kvb: 2 sat/vB on eCash, where a bid of the relay floor stayed in the
 * mempool while other slots' requests of 2-3 sat/vB got in, 2026-10-26).
 */
CAmount CompetitiveBase(const AnchorBidPolicy& policy, const BidInputs& inputs)
{
    CAmount base{MinimumBid(policy, inputs)};
    std::optional<CAmount> rate;
    if (policy.use_fee_estimate && inputs.fee_rate_per_kvb && *inputs.fee_rate_per_kvb > 0) rate = inputs.fee_rate_per_kvb;
    // What blocks took (RobustIncludedRate), clamped to 10 x the least of the relay floor and the network's rate.
    if (!rate && inputs.included_rate_per_kvb && *inputs.included_rate_per_kvb > 0) {
        rate = std::min<CAmount>(*inputs.included_rate_per_kvb, 10 * std::max<CAmount>({inputs.relay_rate_per_kvb, inputs.min_rate_per_kvb, 1000}));
    }
    const CAmount per_kvb{std::max(rate.value_or(0), inputs.min_rate_per_kvb)};
    if (per_kvb > 0) base = std::max(base, Clamp(std::ceil(static_cast<double>(per_kvb) / 1000.0) * policy.request_vsize));
    return base;
}
} // namespace

BidDecision DecideBid(const AnchorBidPolicy& policy, const BidInputs& inputs)
{
    BidDecision d;
    d.pressure = StallPressure(inputs.slot, inputs.first_unanchored_slot, inputs.max_unanchored_slots);
    const std::string p_text{strprintf("p=%.2f", d.pressure)};
    if (!policy.Bids()) {
        d.kind = "off";
        d.reason = "off: no bids (the chain relies on other bidders)";
        return d;
    }
    const CAmount minimum{MinimumBid(policy, inputs)};
    const CAmount pot{std::max<CAmount>(inputs.pot, 0)};
    // All but the operator's margin, as Follower::Offer: pot - pot / 100 at 99 %.
    const CAmount share{FeeShare(pot, policy.fee_share_bps)};
    const std::string share_text{strprintf("fee share %s (%.2f %% of %s)", FormatMoney(share), policy.fee_share_bps / 100.0, FormatMoney(pot))};
    // The strategy's own bid; `bids` false: none (Fees without fees that pay for a request).
    bool bids{true};
    switch (policy.strategy) {
    case BidStrategy::FEES:
        // No minimum, no floor: the fee share once it pays for a request's own place in a block, else nothing.
        if (share >= std::max(MINE_MIN_OFFER, RelayFloor(policy, inputs))) {
            d.normal = share;
            d.kind = "fees";
            d.reason = share_text;
        } else {
            bids = false;
            d.kind = "idle";
            d.reason = share > 0 ? strprintf("no bid: the %s does not pay for a request", share_text) : "no bid: no fees";
            if (policy.PreventsStall()) d.reason += ", nothing waiting to anchor";
        }
        break;
    case BidStrategy::FEES_MINIMUM:
        if (share > minimum) {
            d.normal = share;
            d.kind = "fees";
            d.reason = share_text;
        } else {
            d.normal = minimum;
            d.kind = "minimum";
            d.reason = strprintf("minimum %s (%s)", FormatMoney(minimum), share_text);
        }
        break;
    case BidStrategy::MINIMUM:
        d.normal = minimum;
        d.kind = "minimum";
        d.reason = strprintf("minimum %s", FormatMoney(minimum));
        break;
    case BidStrategy::FIXED:
        d.normal = policy.fixed;
        d.kind = "fixed";
        d.reason = strprintf("fixed %s", FormatMoney(policy.fixed));
        break;
    case BidStrategy::OFF:
        break;
    }
    d.amount = bids ? d.normal : 0;
    if (policy.PreventsStall()) {
        // Missed mainchain blocks raise what must land: flat, twice for every two missed, at most the boost; legacy,
        // rebidstep for every rebidafter missed, at most rebidmax. rebidafter 0: no raises after missed blocks, in either
        // (the stall bid and the raise above a stale bid are not such raises).
        double factor{1};
        if (policy.flat_boost && policy.rebid_after > 0 && inputs.missed >= 2) {
            factor = std::min(policy.boost, std::pow(2.0, inputs.missed / 2));
        } else if (!policy.flat_boost && policy.rebid_after > 0 && inputs.missed >= policy.rebid_after) {
            factor = std::min(policy.rebid_max, std::pow(policy.rebid_step, inputs.missed / policy.rebid_after));
        }
        // The strategies that bid every block: their minimum again, higher (a fee share or a fixed amount is what it is).
        if (factor > 1 && (policy.strategy == BidStrategy::FEES_MINIMUM || policy.strategy == BidStrategy::MINIMUM)) {
            const CAmount raised{Clamp(static_cast<double>(minimum) * factor)};
            if (raised > d.amount) {
                d.amount = raised;
                d.multiplier = factor;
                d.kind = "rebid";
                d.reason = strprintf("rebid after %d missed mainchain blocks: x%.2f the minimum %s", inputs.missed, factor, FormatMoney(minimum));
            }
        }
        // Work waiting for an anchor (a deposit to credit, withdrawals or a bundle; a bond, an unbond or a jail): it
        // would wait without one, so a bid that lands (the competitive base), fees or not, as 8.3.5's
        // feespreventstall did (with the flat boost: the legacy policy is its curve and rebids alone).
        if (policy.flat_boost && inputs.pending_actions) {
            const CAmount base{CompetitiveBase(policy, inputs)};
            const CAmount pending{Clamp(static_cast<double>(base) * factor)};
            if (!bids || pending > d.amount) {
                d.reason = strprintf("peg work waiting for an anchor: the competitive base %s%s%s", FormatMoney(base),
                                     factor > 1 ? strprintf(" x%.2f after %d missed", factor, inputs.missed) : "",
                                     bids ? strprintf(" (the strategy: %s)", d.reason) : "");
                d.amount = pending;
                d.multiplier = factor;
                d.kind = "pending";
                d.work = true;
                bids = true;
            }
        }
        // Near the unanchored limit: a bid that lands, whatever the strategy said (Fees without fees too). Never a
        // large fee share multiplied: the higher of the two.
        if (d.pressure >= policy.escalation_start) {
            CAmount stall{0};
            double multiplier{1};
            std::string why;
            if (policy.flat_boost) {
                const CAmount base{CompetitiveBase(policy, inputs)};
                stall = Clamp(static_cast<double>(base) * policy.boost);
                multiplier = policy.boost;
                why = strprintf("preventing a stall at %s: x%.2f the competitive base %s", p_text, policy.boost, FormatMoney(base));
            } else {
                multiplier = EscalationMultiplier(policy, d.pressure);
                const bool estimated{policy.use_fee_estimate && inputs.fee_rate_per_kvb && *inputs.fee_rate_per_kvb > 0};
                const double base{estimated ? static_cast<double>(*inputs.fee_rate_per_kvb) * policy.request_vsize / 1000.0
                                            : static_cast<double>(std::max(policy.floor, ESCALATION_FALLBACK_BASE))};
                stall = Clamp(base * multiplier);
                why = estimated ? strprintf("escalation at %s: x%.2f the fee estimate (%.2f sat/vB for %d vB)", p_text, multiplier, *inputs.fee_rate_per_kvb / 1000.0, policy.request_vsize)
                                : strprintf("escalation at %s: x%.2f the floor (no fee estimate)", p_text, multiplier);
            }
            if (!bids || stall > d.amount) {
                d.reason = bids ? strprintf("%s (the strategy: %s)", why, d.reason) : strprintf("%s (%s)", why, d.reason);
                d.amount = stall;
                d.multiplier = multiplier;
                d.kind = policy.flat_boost ? "stall" : "escalation";
                d.stall = true;
                d.work = false;
                bids = true;
            }
        }
    }
    if (!bids) return d;
    d.bid = true;
    if (d.amount > policy.max) {
        d.amount = policy.max;
        d.kind = "max";
        d.reason += strprintf(", capped at max %s", FormatMoney(policy.max));
    }
    if (policy.period_cap > 0) {
        const CAmount left{CapLeft(policy, inputs.spent_in_period, d.stall)};
        const std::string of{d.stall || policy.stall_reserve_pct == 0 ? std::string{} : strprintf(" (%d %% kept for stall prevention)", policy.stall_reserve_pct)};
        if (left == 0 && d.amount > 0) {
            d.bid = false;
            d.kind = "periodcap";
            d.reason = strprintf("no bid: the cap of %s in %d h is spent%s", FormatMoney(policy.period_cap), policy.period_hours, of);
            d.amount = 0;
            return d;
        }
        if (d.amount > left) {
            d.amount = left;
            d.kind = "periodcap";
            d.reason += strprintf(", cut to what is left of the cap of %s in %d h%s", FormatMoney(policy.period_cap), policy.period_hours, of);
        }
    }
    return d;
}

CAmount FeeShare(CAmount pot, int fee_share_bps)
{
    pot = std::max<CAmount>(pot, 0);
    return pot - (pot / 10000) * (10000 - fee_share_bps) - (pot % 10000) * (10000 - fee_share_bps) / 10000;
}

namespace {
/** max, then the period's cap, on a decision that bids (as the end of DecideBid). */
void Limit(const AnchorBidPolicy& policy, CAmount spent, BidDecision& d)
{
    if (d.amount > policy.max) {
        d.amount = policy.max;
        d.kind = "max";
        d.reason += strprintf(", capped at max %s", FormatMoney(policy.max));
    }
    if (policy.period_cap > 0) {
        const CAmount left{CapLeft(policy, spent, d.stall)};
        if (left == 0 && d.amount > 0) {
            d.kind = "periodcap";
            d.reason = strprintf("no bid: the cap of %s in %d h is spent (less the stall reserve)", FormatMoney(policy.period_cap), policy.period_hours);
            d.amount = 0;
            d.bid = false;
            return;
        }
        if (d.amount > left) {
            d.amount = left;
            d.kind = "periodcap";
            d.reason += strprintf(", cut to what is left of the cap of %s in %d h", FormatMoney(policy.period_cap), policy.period_hours);
        }
    }
}
} // namespace

void StaleRaise(const AnchorBidPolicy& policy, CAmount stale, CAmount spent, BidDecision& d)
{
    if (!d.bid || stale <= 0 || d.amount > stale + REPLACEMENT_BUMP - 1) return;
    const CAmount raised{stale + REPLACEMENT_BUMP};
    const bool fits{raised <= policy.max && raised <= CapLeft(policy, spent, d.stall)};
    if (fits) {
        d.amount = raised;
        d.kind = "replace";
        d.reason += strprintf(", raised to %s: above a bid of %s that did not land, which other nodes may still hold on the same coin", FormatMoney(raised), FormatMoney(stale));
    } else {
        d.reason += strprintf(" (not above a bid of %s that did not land: within max and the cap it cannot be, so nodes that still hold that bid refuse this one if it spends the same coin)", FormatMoney(stale));
    }
}

BidDecision DecideNodeBid(const AnchorBidPolicy& policy, const BidInputs& inputs, const BidTriggers& triggers)
{
    BidDecision d;
    if (!policy.Bids()) {
        // Off means off: neither trigger bids, and no stall prevention (a bid by hand still goes).
        d.pressure = StallPressure(inputs.slot, inputs.first_unanchored_slot, inputs.max_unanchored_slots);
        d.kind = "off";
        d.reason = "off: no bids (the chain relies on other bidders)";
        return d;
    }
    if (!triggers.anchor && !triggers.mine) {
        d.kind = "off";
        d.reason = "no trigger: merged mining is off and this node anchors no validator";
        return d;
    }
    // One policy for both triggers: the same bid; setbmm's "always" (for tests) offers its own amount, the higher one with
    // the anchorer (one request serves both).
    if (triggers.mine && triggers.mine_always) {
        BidDecision always;
        always.pressure = StallPressure(inputs.slot, inputs.first_unanchored_slot, inputs.max_unanchored_slots);
        always.normal = always.amount = triggers.mine_amount;
        always.kind = "always";
        always.reason = strprintf("merged mining, always: %s for every mainchain block", FormatMoney(triggers.mine_amount));
        always.bid = always.amount > 0;
        if (always.bid) Limit(policy, inputs.spent_in_period, always);
        d = always;
        if (triggers.anchor) {
            const BidDecision anchor{DecideBid(policy, inputs)};
            if (anchor.bid && (!always.bid || anchor.amount > always.amount)) d = anchor;
        }
    } else {
        d = DecideBid(policy, inputs);
    }
    d.trigger = triggers.anchor && triggers.mine ? "anchor+mine" : triggers.anchor ? "anchor" : "mine";
    StaleRaise(policy, triggers.stale, inputs.spent_in_period, d);
    return d;
}

CAmount StaleOffer(std::span<const PastBid> bids, int64_t now)
{
    CAmount stale{0};
    for (const PastBid& bid : bids) {
        if (bid.status == PastBid::Status::LANDED) {
            stale = 0;
        } else if ((bid.status == PastBid::Status::MISSED || bid.status == PastBid::Status::OUTBID) && bid.time > now - STALE_BID_SECONDS) {
            stale = std::max(stale, bid.offer);
        }
    }
    return stale;
}

} // namespace pos
