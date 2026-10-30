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
    {BidStrategy::FIXED, "fixed"},
    {BidStrategy::MINIMAL, "minimal"},
    {BidStrategy::FEES, "fees"},
    {BidStrategy::FEES_PREVENT_STALL, "feespreventstall"},
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

std::optional<BidStrategy> ParseStrategy(const std::string& name)
{
    const std::string lower{ToLower(util::TrimString(name))};
    for (const auto& [s, n] : STRATEGIES) {
        if (lower == n) return s;
    }
    // The words of the pages and of the documentation too.
    if (lower == "fees+preventstalling" || lower == "fees+prevent stalling" || lower == "fees_prevent_stall") return BidStrategy::FEES_PREVENT_STALL;
    if (lower == "keepmoving") return BidStrategy::MINIMAL;
    return std::nullopt;
}

const std::vector<std::string>& PolicyFieldNames()
{
    static const std::vector<std::string> names{"strategy", "floor", "max", "fixed", "feeshare", "escalationstart", "curve", "interpolate",
                                                "usefeeestimate", "requestvsize", "rebidafter", "rebidstep", "rebidmax", "warnmissed",
                                                "periodcap", "periodhours", "lowbalanceblocks", "flatboost", "boost"};
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
    return "";
}

UniValue PolicyToUniValue(const AnchorBidPolicy& policy)
{
    UniValue out(UniValue::VOBJ);
    out.pushKV("strategy", StrategyName(policy.strategy));
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
    out.pushKV("lowbalanceblocks", policy.low_balance_blocks);
    return out;
}

bool ApplyPolicyFields(AnchorBidPolicy& policy, const UniValue& fields, std::string& error)
{
    if (!fields.isObject()) {
        error = "the policy is an object of fields";
        return false;
    }
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
            const auto strategy{value.isStr() ? ParseStrategy(value.get_str()) : std::nullopt};
            if (!strategy) return bad("fixed, minimal, fees, feespreventstall or off");
            policy.strategy = *strategy;
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
        } else {
            error = strprintf("%s: not a field of the policy (%s)", key, util::Join(PolicyFieldNames(), ", "));
            return false;
        }
        if (!ok) return false;
    }
    return true;
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

std::optional<CAmount> ReplacementBid(const AnchorBidPolicy& policy, CAmount amount, CAmount previous, CAmount spent)
{
    const CAmount replacement{std::max(amount, previous + 500)};
    if (replacement > policy.max) return std::nullopt;
    if (policy.period_cap > 0 && replacement > std::max<CAmount>(policy.period_cap - spent, 0)) return std::nullopt;
    return replacement;
}

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
    // Flat policies follow the next-block estimate even before stall pressure builds. Keep the operator's
    // floor as an optional override; a disabled estimate still respects the mainchain relay minimum.
    CAmount base{policy.floor};
    CAmount plain_base{policy.floor};
    if (policy.flat_boost && policy.strategy == BidStrategy::FEES_PREVENT_STALL) {
        // The relay floor for the largest request a wallet makes (250 vB: a request of more inputs than the usual 189 vB
        // was refused under it, "min relay fee not met, 19 < 21").
        const CAmount relay{Clamp(std::ceil(static_cast<double>(std::max<CAmount>(inputs.relay_rate_per_kvb, 0)) * std::max(policy.request_vsize, 250) / 1000.0))};
        base = std::max(base, relay);
        // The next-block estimate, else what recent mainchain blocks took for a BMM request, rounded up to whole
        // sat/vB, and never below the network's least competitive rate (min_rate_per_kvb: 2 sat/vB on eCash, where a
        // bid of the relay floor stayed in the mempool while other slots' requests of 2-3 sat/vB got in, 2026-10-26).
        std::optional<CAmount> rate;
        if (policy.use_fee_estimate && inputs.fee_rate_per_kvb && *inputs.fee_rate_per_kvb > 0) rate = inputs.fee_rate_per_kvb;
        // What blocks took (RobustIncludedRate), clamped to 10 x the least of the relay floor and the network's rate.
        if (!rate && inputs.included_rate_per_kvb && *inputs.included_rate_per_kvb > 0) {
            rate = std::min<CAmount>(*inputs.included_rate_per_kvb, 10 * std::max<CAmount>({inputs.relay_rate_per_kvb, inputs.min_rate_per_kvb, 1000}));
        }
        const CAmount per_kvb{std::max(rate.value_or(0), inputs.min_rate_per_kvb)};
        if (per_kvb > 0) base = std::max(base, Clamp(std::ceil(static_cast<double>(per_kvb) / 1000.0) * policy.request_vsize));
        // Missed mainchain blocks: the base itself goes up, twice for every two missed (at most the boost's factor); the
        // boost stays boost x the base before it (so never more than boost x the competitive base).
        plain_base = base;
        if (inputs.missed >= 2) base = Clamp(static_cast<double>(base) * std::min(policy.boost, std::pow(2.0, inputs.missed / 2)));
    }
    if (policy.strategy == BidStrategy::FEES_PREVENT_STALL && policy.flat_boost && inputs.pot <= 0 &&
        !inputs.pending_actions && d.pressure < policy.escalation_start) {
        d.kind = "idle";
        d.reason = "skipped: nothing to anchor";
        return d;
    }
    // The strategy's own bid.
    switch (policy.strategy) {
    case BidStrategy::FIXED:
        d.normal = policy.fixed;
        d.kind = "fixed";
        d.reason = strprintf("fixed %s", FormatMoney(policy.fixed));
        break;
    case BidStrategy::MINIMAL:
        d.normal = base;
        d.kind = base > policy.floor ? "base" : "floor";
        d.reason = strprintf("base %s", FormatMoney(base));
        break;
    case BidStrategy::FEES:
    case BidStrategy::FEES_PREVENT_STALL: {
        const CAmount pot{std::max<CAmount>(inputs.pot, 0)};
        // All but the operator's margin, as Follower::Offer: pot - pot / 100 at 99 %.
        const CAmount share{FeeShare(pot, policy.fee_share_bps)};
        if (share > base) {
            d.normal = share;
            d.kind = "fees";
            d.reason = strprintf("fee share %s (%.2f %% of a pot of %s)", FormatMoney(share), policy.fee_share_bps / 100.0, FormatMoney(pot));
        } else {
            d.normal = base;
            d.kind = base > policy.floor ? "base" : "floor";
            d.reason = strprintf("base %s (fee share %s)", FormatMoney(base), FormatMoney(share));
        }
        break;
    }
    case BidStrategy::OFF:
        break;
    }
    d.amount = d.normal;
    if (policy.Escalates() && policy.flat_boost) {
        // Boost only the base, never a large fee share.
        if (d.pressure >= policy.escalation_start) {
            const CAmount boosted{Clamp(static_cast<double>(plain_base) * policy.boost)};
            if (boosted > d.amount) {
                d.amount = boosted;
                d.multiplier = policy.boost;
                d.kind = "boost";
                d.reason = strprintf("boost at %s: x%.2f the base %s", p_text, policy.boost, FormatMoney(base));
            }
        }
    } else if (policy.Escalates()) {
        // Missed blocks: the bid again, higher, whatever the pressure (capped).
        if (policy.rebid_after > 0 && inputs.missed >= policy.rebid_after) {
            const double factor{std::min(policy.rebid_max, std::pow(policy.rebid_step, inputs.missed / policy.rebid_after))};
            const CAmount raised{Clamp(static_cast<double>(std::max(d.normal, policy.floor)) * factor)};
            if (raised > d.amount) {
                d.amount = raised;
                d.multiplier = factor;
                d.kind = "rebid";
                d.reason = strprintf("rebid after %d missed mainchain blocks (x%.2f)", inputs.missed, factor);
            }
        }
        // Stall pressure: toward the mainchain's next-block fee for the request, times the curve. Never below the
        // threshold, whatever the mempool says.
        if (d.pressure >= policy.escalation_start) {
            const double multiplier{EscalationMultiplier(policy, d.pressure)};
            const bool estimated{policy.use_fee_estimate && inputs.fee_rate_per_kvb && *inputs.fee_rate_per_kvb > 0};
            const double base{estimated ? static_cast<double>(*inputs.fee_rate_per_kvb) * policy.request_vsize / 1000.0
                                        : static_cast<double>(std::max(policy.floor, ESCALATION_FALLBACK_BASE))};
            const CAmount escalated{Clamp(base * multiplier)};
            if (escalated > d.amount) {
                d.amount = escalated;
                d.multiplier = multiplier;
                d.kind = "escalation";
                d.reason = estimated ? strprintf("escalation at %s: x%.2f the fee estimate (%.2f sat/vB for %d vB)", p_text, multiplier, *inputs.fee_rate_per_kvb / 1000.0, policy.request_vsize)
                                     : strprintf("escalation at %s: x%.2f the floor (no fee estimate)", p_text, multiplier);
            }
        }
    }
    if (d.amount > policy.max) {
        d.amount = policy.max;
        d.kind = "max";
        d.reason += strprintf(", capped at max %s", FormatMoney(policy.max));
    }
    if (policy.period_cap > 0) {
        const CAmount left{std::max<CAmount>(policy.period_cap - inputs.spent_in_period, 0)};
        if (left == 0 && d.amount > 0) {
            d.kind = "periodcap";
            d.reason = strprintf("no bid: the cap of %s in %d h is spent", FormatMoney(policy.period_cap), policy.period_hours);
            d.amount = 0;
            return d;
        }
        if (d.amount > left) {
            d.amount = left;
            d.kind = "periodcap";
            d.reason += strprintf(", cut to what is left of the cap of %s in %d h", FormatMoney(policy.period_cap), policy.period_hours);
        }
    }
    d.bid = true;
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
        const CAmount left{std::max<CAmount>(policy.period_cap - spent, 0)};
        if (left == 0 && d.amount > 0) {
            d.kind = "periodcap";
            d.reason = strprintf("no bid: the cap of %s in %d h is spent", FormatMoney(policy.period_cap), policy.period_hours);
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
    const bool fits{raised <= policy.max && (policy.period_cap == 0 || raised <= policy.period_cap - spent)};
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
    std::optional<BidDecision> anchor;
    if (triggers.anchor && policy.Bids()) anchor = DecideBid(policy, inputs);
    std::optional<BidDecision> mine;
    if (triggers.mine) {
        BidDecision d;
        d.pressure = StallPressure(inputs.slot, inputs.first_unanchored_slot, inputs.max_unanchored_slots);
        if (triggers.mine_always) {
            d.normal = d.amount = triggers.mine_amount;
            d.kind = "always";
            d.reason = strprintf("merged mining, always: %s for every mainchain block", FormatMoney(triggers.mine_amount));
            d.bid = d.amount > 0;
            if (d.bid) Limit(policy, inputs.spent_in_period, d);
        } else {
            // A strategy off stops the anchorer, not the merged miner: it then bids the fee share.
            AnchorBidPolicy own{policy};
            if (!own.Bids()) own.strategy = BidStrategy::FEES;
            const CAmount share{FeeShare(inputs.pot, own.fee_share_bps)};
            if (share < MINE_MIN_OFFER) {
                d.kind = "idle";
                d.reason = strprintf("merged mining waits for fees that pay for a block (a fee share of %s)", FormatMoney(share));
            } else {
                BidInputs in{inputs};
                // Fees to pass on: never "nothing to anchor".
                in.pending_actions = true;
                d = DecideBid(own, in);
            }
        }
        mine = d;
    }
    BidDecision d;
    if (anchor && mine) {
        // One request serves both: the higher bid, or the one that bids.
        if (anchor->bid && mine->bid) {
            d = anchor->amount >= mine->amount ? *anchor : *mine;
        } else {
            d = anchor->bid ? *anchor : *mine;
        }
        d.trigger = "anchor+mine";
    } else if (anchor) {
        d = *anchor;
        d.trigger = "anchor";
    } else if (mine) {
        d = *mine;
        d.trigger = "mine";
    } else {
        d.kind = "off";
        d.reason = triggers.anchor ? "off: no bids (the chain relies on other bidders)" : "no trigger: merged mining is off and this node anchors no validator";
        return d;
    }
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
