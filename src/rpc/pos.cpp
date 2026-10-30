// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <arith_uint256.h>
#include <chain.h>
#include <common/args.h>
#include <consensus/amount.h>
#include <core_io.h>
#include <kernel/chainparams.h>
#include <kernel/coinstats.h>
#include <key_io.h>
#include <node/context.h>
#include <policy/feerate.h>
#include <pos/evidence.h>
#include <pos/feesplit.h>
#include <pos/finality.h>
#include <pos/forkchoice.h>
#include <pos/hooks.h>
#include <pos/producer.h>
#include <pos/staking.h>
#include <rpc/pos.h>
#include <streams.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <sidechain/follower.h>
#include <sidechain/mainclient.h>
#include <sidechain/state.h>
#include <txdb.h>
#include <univalue.h>

#include <set>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>

using node::NodeContext;

namespace {

pos::FastBlocksHooks& EnsureHooks()
{
    pos::FastBlocksHooks* hooks{pos::InstalledHooks()};
    if (!hooks) throw JSONRPCError(RPC_MISC_ERROR, "This chain is not a sidechain: it has no blocks of stake");
    return *hooks;
}

int64_t NowSlot(const Consensus::PosParams& params)
{
    return pos::SlotOf(TicksSinceEpoch<std::chrono::seconds>(NodeClock::now()), params);
}

std::vector<RPCResult> PolicyFieldsDoc()
{
    return {
        {RPCResult::Type::STR, "strategy", "fees (the fee share, no minimum), feesminimum (the fee share, at least the minimum), minimum (every mainchain block), fixed or off (no bids at all)"},
        {RPCResult::Type::BOOL, "preventstall", "Prevent stalling: near the unanchored limit, a bid that lands an anchor whatever the strategy (not with off)"},
        {RPCResult::Type::BOOL, "anchor", "Anchor my validator: a validator's own anchorer bids (the other trigger is setbmm)"},
        {RPCResult::Type::STR_AMOUNT, "floor", "The user's minimum, in coins of the mainchain (the minimum is this, or the relay minimum of a request if higher)"},
        {RPCResult::Type::STR_AMOUNT, "max", "The most it bids, always"},
        {RPCResult::Type::STR_AMOUNT, "fixed", "The bid of the fixed strategy"},
        {RPCResult::Type::NUM, "feeshare", "The percentage of the pot the fee strategies bid"},
        {RPCResult::Type::NUM, "escalationstart", "The stall pressure from which stall prevention bids"},
        {RPCResult::Type::BOOL, "flatboost", "Use the flat boost instead of the legacy curve and rebids"},
        {RPCResult::Type::NUM, "boost", "Flat boost multiplier of the base"},
        {RPCResult::Type::ARR, "curve", "The escalation curve", {{RPCResult::Type::OBJ, "", "", {{RPCResult::Type::NUM, "pressure", "Stall pressure"}, {RPCResult::Type::NUM, "multiplier", "Multiplier of the base (fee estimate x requestvsize, else the floor)"}}}}},
        {RPCResult::Type::BOOL, "interpolate", "Linear between the points of the curve (else steps)"},
        {RPCResult::Type::BOOL, "usefeeestimate", "Stall prevention's base follows the mainchain's next-block fee estimate"},
        {RPCResult::Type::NUM, "requestvsize", "The size of a BMM request, in vbytes"},
        {RPCResult::Type::NUM, "rebidafter", "Missed mainchain blocks in a row before the minimum is raised (the legacy curve; with the flat boost, x2 for every two missed, at most boost); 0: no raises after missed blocks, in either"},
        {RPCResult::Type::NUM, "rebidstep", "The legacy curve: the multiplier of each such raise"},
        {RPCResult::Type::NUM, "rebidmax", "The legacy curve: the most those raises multiply the bid"},
        {RPCResult::Type::NUM, "warnmissed", "Missed mainchain blocks in a row before the \"bid too low\" warning (0: never)"},
        {RPCResult::Type::STR_AMOUNT, "periodcap", "The most the landed bids cost in periodhours (0: no cap)"},
        {RPCResult::Type::NUM, "periodhours", "The period of the cap, in hours"},
        {RPCResult::Type::NUM, "stallreserve", "The percentage of the cap kept for stall prevention's bids (the others spend the cap less it)"},
        {RPCResult::Type::NUM, "lowbalanceblocks", "A warning when the bid wallet covers fewer mainchain blocks (0: never)"},
    };
}

std::vector<RPCResult> PolicyResultDoc()
{
    return {
        {RPCResult::Type::BOOL, "validator", "Whether this node is a validator (only validators bid)"},
        {RPCResult::Type::BOOL, "bidding", "Whether the node bids: its anchorer (a validator with anchor on) or its merged miner (setbmm on), the strategy not off"},
        {RPCResult::Type::STR, "explanation", /*optional=*/true, "Why it does not bid, if it does not"},
        {RPCResult::Type::BOOL, "anchortrigger", "\"Anchor my validator\": a validator with anchor on, the strategy not off"},
        {RPCResult::Type::BOOL, "minetrigger", "\"Mine blocks when fees pay\": setbmm on"},
        {RPCResult::Type::OBJ, "policy", "The policy in force", PolicyFieldsDoc()},
        {RPCResult::Type::OBJ, "defaults", "The policy of the options (-validatoranchorbid*), before setanchorbidpolicy", PolicyFieldsDoc()},
        {RPCResult::Type::OBJ, "overrides", "The fields setanchorbidpolicy changed, kept in the node's settings", {}, {.skip_type_check = true}},
        {RPCResult::Type::ARR, "strategies", "The strategies", {{RPCResult::Type::STR, "", "A strategy"}}},
    };
}

std::string NotBiddingExplanation(const pos::FastBlocksHooks::AnchorBidReport& report)
{
    if (!report.validator) return report.mine_trigger ? "" : report.policy.Bids() ? "This node is not a validator: it bids only as a merged miner (setbmm), under this policy; merged mining is off." : "The strategy is off: this node bids for nothing.";
    if (!report.policy.Bids()) return "The strategy is off: this node bids for nothing, and the chain relies on other bidders; blocks of stake halt after maxunanchoredslots without an anchor.";
    if (!report.anchor_trigger && !report.mine_trigger) return "\"Anchor my validator\" is off and merged mining is off: this validator bids for no anchors, and the chain relies on other bidders.";
    return "";
}

UniValue PolicyResult(const pos::FastBlocksHooks::AnchorBidReport& report)
{
    UniValue out(UniValue::VOBJ);
    out.pushKV("validator", report.validator);
    out.pushKV("bidding", report.anchor_trigger || report.mine_trigger);
    if (const std::string why{NotBiddingExplanation(report)}; !why.empty()) out.pushKV("explanation", why);
    out.pushKV("anchortrigger", report.anchor_trigger);
    out.pushKV("minetrigger", report.mine_trigger);
    out.pushKV("policy", pos::PolicyToUniValue(report.policy));
    out.pushKV("defaults", pos::PolicyToUniValue(report.defaults));
    out.pushKV("overrides", report.overrides);
    UniValue strategies(UniValue::VARR);
    for (const char* name : {"fees", "feesminimum", "minimum", "fixed", "off"}) strategies.push_back(name);
    out.pushKV("strategies", std::move(strategies));
    return out;
}

pos::FastBlocksHooks::AnchorBidReport ReportNow(ChainstateManager& chainman, const pos::FastBlocksHooks& hooks, const node::NodeContext* node = nullptr)
{
    // The merged miner's settings, for its trigger.
    std::optional<sidechain::Follower::Mining> mining;
    if (node && node->follower) mining = node->follower->GetMining();
    int64_t first{-1};
    {
        LOCK(::cs_main);
        first = pos::GetFastState(chainman.ActiveChainstate().SideState().View()).first_unanchored_slot;
    }
    return hooks.GetAnchorBidReport(NowSlot(hooks.Params()), first, mining ? &*mining : nullptr);
}

const char* BidStatusName(pos::FastBlocksHooks::AnchorBidRecord::Status status)
{
    using enum pos::FastBlocksHooks::AnchorBidRecord::Status;
    switch (status) {
    case PENDING: return "pending";
    case LANDED: return "landed";
    case MISSED: return "missed";
    case OUTBID: return "outbid";
    }
    return "";
}

UniValue BidResult(const pos::FastBlocksHooks::AnchorBidReport& report, int count)
{
    UniValue out(UniValue::VOBJ);
    out.pushKV("validator", report.validator);
    const bool bidding{report.anchor_trigger || report.mine_trigger};
    out.pushKV("bidding", bidding);
    if (const std::string why{NotBiddingExplanation(report)}; !why.empty()) out.pushKV("explanation", why);
    out.pushKV("strategy", pos::StrategyName(report.policy.strategy));
    out.pushKV("anchortrigger", report.anchor_trigger);
    out.pushKV("minetrigger", report.mine_trigger);
    out.pushKV("trigger", report.decision.trigger);
    out.pushKV("effective", ValueFromAmount(report.decision.bid ? report.decision.amount : 0));
    out.pushKV("normal", ValueFromAmount(report.decision.normal));
    out.pushKV("kind", report.decision.kind);
    out.pushKV("reason", report.decision.reason);
    out.pushKV("pressure", report.decision.pressure);
    out.pushKV("multiplier", report.decision.multiplier);
    out.pushKV("escalationstart", report.policy.escalation_start);
    const auto& in{report.inputs};
    out.pushKV("firstunanchoredslot", in.first_unanchored_slot);
    out.pushKV("unanchoredslots", in.first_unanchored_slot >= 0 && in.slot > in.first_unanchored_slot ? in.slot - in.first_unanchored_slot : 0);
    out.pushKV("maxunanchoredslots", in.max_unanchored_slots);
    out.pushKV("pot", ValueFromAmount(in.pot));
    out.pushKV("pendingactions", in.pending_actions);
    out.pushKV("relayfeerate", in.relay_rate_per_kvb / 1000.0);
    if (in.fee_rate_per_kvb) out.pushKV("feerate", *in.fee_rate_per_kvb / 1000.0);
    out.pushKV("feesource", report.fee_source);
    out.pushKV("missed", report.missed);
    if (report.balance) {
        out.pushKV("balance", ValueFromAmount(*report.balance));
        const CAmount rate{report.decision.bid ? report.decision.amount : 0};
        if (rate > 0) out.pushKV("coverblocks", *report.balance / rate);
    }
    if (!report.balance_error.empty()) out.pushKV("balanceerror", report.balance_error);
    out.pushKV("spentperiod", ValueFromAmount(report.spent));
    out.pushKV("periodcap", ValueFromAmount(report.policy.period_cap));
    out.pushKV("periodhours", report.policy.period_hours);
    out.pushKV("max", ValueFromAmount(report.policy.max));
    if (report.stale > 0) out.pushKV("stalebid", ValueFromAmount(report.stale));
    UniValue history(UniValue::VARR);
    for (const auto& bid : report.history) {
        if (static_cast<int>(history.size()) >= count) break;
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("mainheight", bid.main_height + 1);
        entry.pushKV("ticket", bid.ticket.GetHex());
        entry.pushKV("offer", ValueFromAmount(bid.offer));
        entry.pushKV("kind", bid.kind);
        entry.pushKV("reason", bid.reason);
        entry.pushKV("pressure", bid.pressure);
        entry.pushKV("status", BidStatusName(bid.status));
        entry.pushKV("trigger", bid.trigger);
        entry.pushKV("cost", ValueFromAmount(bid.status == pos::FastBlocksHooks::AnchorBidRecord::Status::LANDED ? bid.offer : 0));
        entry.pushKV("time", bid.time);
        history.push_back(std::move(entry));
    }
    out.pushKV("history", std::move(history));
    UniValue warnings(UniValue::VARR);
    for (const std::string& warning : report.warnings) warnings.push_back(warning);
    out.pushKV("warnings", std::move(warnings));
    return out;
}

std::vector<RPCResult> BidResultDoc()
{
    return {
        {RPCResult::Type::BOOL, "validator", "Whether this node is a validator (only validators bid)"},
        {RPCResult::Type::BOOL, "bidding", "Whether the node bids (either trigger)"},
        {RPCResult::Type::STR, "explanation", /*optional=*/true, "Why it does not bid, if it does not"},
        {RPCResult::Type::STR, "strategy", "The strategy in force"},
        {RPCResult::Type::BOOL, "anchortrigger", "\"Anchor my validator\" on"},
        {RPCResult::Type::BOOL, "minetrigger", "\"Mine blocks when fees pay\" on (setbmm)"},
        {RPCResult::Type::STR, "trigger", "What sets off the bid now: anchor, mine, anchor+mine, or empty"},
        {RPCResult::Type::STR_AMOUNT, "effective", "What it bids now for the next mainchain block"},
        {RPCResult::Type::STR_AMOUNT, "normal", "The strategy's bid before stall prevention"},
        {RPCResult::Type::STR, "kind", "fees, minimum, fixed, rebid (the minimum raised after missed blocks), pending (work waiting for an anchor: deposits, withdrawals, and bonds, unbonds and jails), stall (the stall boost), escalation (the legacy curve), max, periodcap, replace (above a stale bid), always (setbmm's always), idle (Fees without fees that pay), throttled (work waiting, but its last bid did not land: one in 3 mainchain blocks) or off"},
        {RPCResult::Type::STR, "reason", "Why, in words"},
        {RPCResult::Type::NUM, "pressure", "Stall pressure p: unanchoredslots / maxunanchoredslots (the chain halts at 1)"},
        {RPCResult::Type::NUM, "multiplier", "The escalation (or rebid) multiplier applied"},
        {RPCResult::Type::NUM, "escalationstart", "The pressure from which the bid escalates"},
        {RPCResult::Type::NUM, "firstunanchoredslot", "Slot of the first block of stake no anchor covers, -1 if none"},
        {RPCResult::Type::NUM, "unanchoredslots", "Slots since then"},
        {RPCResult::Type::NUM, "maxunanchoredslots", "The halt: blocks of stake stop after this many slots without an anchor"},
        {RPCResult::Type::STR_AMOUNT, "pot", "The pot of the last anchor bid for"},
        {RPCResult::Type::BOOL, "pendingactions", "Work waits for an anchor: deposits, withdrawals, and bonds, unbonds and jails (not the staking accounting)"},
        {RPCResult::Type::NUM, "relayfeerate", "Mainchain relay minimum, in sat/vB"},
        {RPCResult::Type::NUM, "feerate", /*optional=*/true, "The mainchain's next-block fee estimate, in sat/vB"},
        {RPCResult::Type::STR, "feesource", "Where the estimate came from"},
        {RPCResult::Type::NUM, "missed", "Mainchain blocks in a row that came without this validator's commitment"},
        {RPCResult::Type::STR_AMOUNT, "balance", /*optional=*/true, "The bid wallet's balance (the mainchain wallet that pays the requests)"},
        {RPCResult::Type::NUM, "coverblocks", /*optional=*/true, "How many mainchain blocks it covers at the current bid"},
        {RPCResult::Type::STR, "balanceerror", /*optional=*/true, "Why the balance is not known"},
        {RPCResult::Type::STR_AMOUNT, "spentperiod", "What the landed bids cost in the period"},
        {RPCResult::Type::STR_AMOUNT, "periodcap", "The cap of the period (0: none): every landed bid of the node counts, whatever set it off"},
        {RPCResult::Type::NUM, "periodhours", "The period, in hours"},
        {RPCResult::Type::STR_AMOUNT, "max", "The most any one bid is"},
        {RPCResult::Type::STR_AMOUNT, "stalebid", /*optional=*/true, "A bid that did not land, which other nodes may still hold: the next one is raised above it (within max and the cap)"},
        {RPCResult::Type::ARR, "history", "The last bids, newest first",
        {
            {RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::NUM, "mainheight", "The mainchain block it was for"},
                {RPCResult::Type::STR_HEX, "ticket", "Its ticket"},
                {RPCResult::Type::STR_AMOUNT, "offer", "The bid"},
                {RPCResult::Type::STR, "kind", "As kind above"},
                {RPCResult::Type::STR, "reason", "Why"},
                {RPCResult::Type::NUM, "pressure", "The stall pressure then"},
                {RPCResult::Type::STR, "trigger", "anchor, mine, anchor+mine or hand"},
                {RPCResult::Type::STR, "status", "pending, landed, missed (the block had no commitment of this chain) or outbid (another commitment of this chain, or the mainchain went another way)"},
                {RPCResult::Type::STR_AMOUNT, "cost", "What it cost (landed bids only)"},
                {RPCResult::Type::NUM_TIME, "time", "When it was made"},
            }},
        }},
        {RPCResult::Type::ARR, "warnings", "Warnings: the bid too low, the wallet low, the cap reached, the anchorer off", {{RPCResult::Type::STR, "", "A warning"}}},
    };
}

uint256 ParsePubkey(const UniValue& value)
{
    const std::vector<unsigned char> bytes{ParseHexV(value, "pubkey")};
    if (bytes.size() != 32) throw JSONRPCError(RPC_INVALID_PARAMETER, "pubkey must be an x-only key: 32 bytes in hex");
    return uint256{std::span<const unsigned char>{bytes}};
}

/** The validator set and the seed of an epoch, as the active chain and the record say now. */
struct EpochSchedule {
    std::shared_ptr<const pos::ValidatorSet> set;
    std::optional<uint256> seed;
    //! Validators of the set the active chain banned: they lead no slot any more (their slots stay empty).
    std::set<uint256> banned;

    /** The leader of a slot of the epoch, if the set is known and active and the seed known. */
    std::optional<size_t> Leader(int64_t slot) const
    {
        if (!set || !seed) return std::nullopt;
        return set->Leader(slot, *seed);
    }
    /** The key of the leader the set and the seed name, banned or not. */
    std::optional<uint256> ScheduledKey(int64_t slot) const
    {
        const auto index{Leader(slot)};
        if (!index) return std::nullopt;
        return set->Validators()[*index].pubkey;
    }
    /** The key of the leader of a slot: none if it is banned. */
    std::optional<uint256> LeaderKey(int64_t slot) const
    {
        const auto key{ScheduledKey(slot)};
        if (!key || banned.contains(*key)) return std::nullopt;
        return key;
    }
};

EpochSchedule ScheduleOf(const pos::FastBlocksHooks& hooks, ChainstateManager& chainman, int64_t epoch)
{
    EpochSchedule schedule;
    LOCK(::cs_main);
    const CBlockIndex* tip{chainman.ActiveChain().Tip()};
    if (tip) schedule.set = hooks.SetFor(*tip, epoch);
    if (chainman.m_mainchain) schedule.seed = pos::EpochSeed(epoch, hooks.Params(), *chainman.m_mainchain);
    if (schedule.set) {
        const sidechain::StoreView& view{chainman.ActiveChainstate().SideState().View()};
        for (const Consensus::PosValidator& validator : schedule.set->Validators()) {
            if (const auto record{pos::GetValidator(view, validator.pubkey)}; record && record->tombstoned) schedule.banned.insert(validator.pubkey);
        }
    }
    return schedule;
}

/** The slots of `epoch` each validator of its set leads. */
std::vector<int64_t> SlotsLed(const pos::FastBlocksHooks& hooks, const EpochSchedule& schedule, int64_t epoch)
{
    const Consensus::PosParams& params{hooks.Params()};
    std::vector<int64_t> counts(schedule.set ? schedule.set->Validators().size() : 0, 0);
    for (int64_t slot{epoch * params.slots_per_epoch}; slot < (epoch + 1) * params.slots_per_epoch; ++slot) {
        if (const auto leader{schedule.Leader(slot)}; leader && schedule.LeaderKey(slot)) ++counts[*leader];
    }
    return counts;
}

/** The first slot from `from` on, within two epochs, that one of `keys` leads (as far as the schedules are known). */
std::optional<int64_t> NextSlotLed(const pos::FastBlocksHooks& hooks, ChainstateManager& chainman, const std::vector<uint256>& keys, int64_t from)
{
    const Consensus::PosParams& params{hooks.Params()};
    std::map<int64_t, EpochSchedule> schedules;
    for (int64_t slot{from}; slot < from + 2 * params.slots_per_epoch; ++slot) {
        const int64_t epoch{pos::EpochOf(slot, params)};
        auto found{schedules.find(epoch)};
        if (found == schedules.end()) found = schedules.emplace(epoch, ScheduleOf(hooks, chainman, epoch)).first;
        const auto leader{found->second.LeaderKey(slot)};
        if (leader && std::find(keys.begin(), keys.end(), *leader) != keys.end()) return slot;
    }
    return std::nullopt;
}

/** The fee split now (pos/feesplit.h), for getposinfo and getstakinginfo. */
std::vector<RPCResult> FeeSplitFields()
{
    return {
        {RPCResult::Type::OBJ, "feesplit", "The fee split (from fee_split_height): the BMM bidder's share of ordinary fees, the validators' (the pool)", {
            {RPCResult::Type::BOOL, "active", "Whether the next block takes it"},
            {RPCResult::Type::NUM, "activationheight", "fee_split_height: its first block"},
            {RPCResult::Type::NUM, "curve", "The curve of the bidder's share the next block takes: 1, or 2 from fee_curve2_height"},
            {RPCResult::Type::NUM, "curve2height", "fee_curve2_height: the first block of curve 2"},
            {RPCResult::Type::NUM, "mainheight", "The mainchain block whose spacing blocks of stake of the current epoch take (the epoch's seed block), -1 if not on record yet"},
            {RPCResult::Type::NUM, "spacing", "S: the median spacing of the last 11 mainchain blocks there, in seconds, as the curves take it: scaled by 60 / targetspacing"},
            {RPCResult::Type::NUM, "targetspacing", "The spacing of mainchain blocks the curves are drawn for (fee_target_spacing): 60 on the Chains networks, 600 on the eCash networks"},
            {RPCResult::Type::NUM, "bidderbps", "The bidder's share of ordinary fees, in basis points (rounded down): curve 1 max(10 %, 50 % x min(1, 60 / S)); curve 2 clamp(50 % - 5 % x (S - 60) / 60, 20 %, 50 %)"},
            {RPCResult::Type::NUM, "validatorsbps", "The validators' share (10000 less it)"},
            {RPCResult::Type::NUM, "bmmmainheight", "For a BMM block made now: the mainchain tip it follows"},
            {RPCResult::Type::NUM, "bmmspacing", "S there"},
            {RPCResult::Type::NUM, "bmmbidderbps", "The bidder's share there"},
            {RPCResult::Type::STR_AMOUNT, "pool", "The validator pool: shares of fees not paid yet"},
            {RPCResult::Type::ARR, "poolepochs", "By the epoch whose validators share it (paid at the first anchor after the epoch ends)",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::NUM, "epoch", "The epoch"},
                    {RPCResult::Type::STR_AMOUNT, "amount", "Its pool"},
                }},
            }},
        }},
    };
}

UniValue FeeSplitToJSON(const pos::FastBlocksHooks& hooks, ChainstateManager& chainman, int64_t slot)
{
    const Consensus::PosParams& params{hooks.Params()};
    int next_height;
    std::vector<std::pair<int64_t, int64_t>> pool;
    std::optional<int> main_height;
    int main_tip;
    {
        LOCK(::cs_main);
        next_height = chainman.ActiveChain().Height() + 1;
        pool = pos::PoolEntries(chainman.ActiveChainstate().SideState().View());
        main_height = hooks.FeeSplitMainHeight(/*pos_block=*/true, slot, -1);
        main_tip = chainman.m_mainchain ? chainman.m_mainchain->Height() : -1;
    }
    const int64_t spacing{main_height ? hooks.FeeSplitSpacing(*main_height) : pos::FEE_SPLIT_TARGET_SPACING};
    const int64_t bmm_spacing{hooks.FeeSplitSpacing(main_tip)};
    UniValue split(UniValue::VOBJ);
    split.pushKV("active", hooks.FeeSplitActive(next_height));
    split.pushKV("activationheight", params.fee_split_height);
    const pos::FeeCurve curve{hooks.FeeCurveAt(next_height)};
    split.pushKV("curve", static_cast<int>(curve));
    split.pushKV("curve2height", params.fee_curve2_height);
    split.pushKV("mainheight", main_height.value_or(-1));
    split.pushKV("spacing", spacing);
    split.pushKV("targetspacing", params.fee_target_spacing);
    split.pushKV("bidderbps", pos::BidderShareBps(spacing, curve));
    split.pushKV("validatorsbps", 10000 - pos::BidderShareBps(spacing, curve));
    split.pushKV("bmmmainheight", main_tip);
    split.pushKV("bmmspacing", bmm_spacing);
    split.pushKV("bmmbidderbps", pos::BidderShareBps(bmm_spacing, curve));
    CAmount total{0};
    UniValue epochs(UniValue::VARR);
    for (const auto& [epoch, amount] : pool) {
        total += amount;
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("epoch", epoch);
        entry.pushKV("amount", ValueFromAmount(amount));
        epochs.push_back(std::move(entry));
    }
    split.pushKV("pool", ValueFromAmount(total));
    split.pushKV("poolepochs", std::move(epochs));
    return split;
}

/** A validator's share, by stake, of the pool of an epoch so far (what it is paid if the epoch ended now, unless jailed or banned). */
CAmount PoolShareOf(ChainstateManager& chainman, const EpochSchedule& schedule, int64_t epoch, int64_t stake)
{
    if (!schedule.set || schedule.set->TotalStake() <= 0 || stake <= 0) return 0;
    CAmount pool{0};
    for (const auto& [at, amount] : WITH_LOCK(::cs_main, return pos::PoolEntries(chainman.ActiveChainstate().SideState().View()))) {
        if (at == epoch) pool = amount;
    }
    // (DistributePool's rounding: down.)
    arith_uint256 product{static_cast<uint64_t>(pool)};
    product *= arith_uint256{static_cast<uint64_t>(stake)};
    product /= arith_uint256{static_cast<uint64_t>(schedule.set->TotalStake())};
    return static_cast<CAmount>(product.GetLow64());
}

std::vector<uint256> LocalKeys(const pos::FastBlocksHooks& hooks)
{
    const pos::Producer* producer{hooks.GetProducer()};
    return producer ? producer->Keys() : std::vector<uint256>{};
}

UniValue ProofToJSON(const pos::Offence& offence, const pos::DoubleProposal& proof)
{
    UniValue entry(UniValue::VOBJ);
    entry.pushKV("pubkey", HexStr(offence.pubkey));
    entry.pushKV("slot", offence.slot);
    UniValue hashes(UniValue::VARR);
    hashes.push_back(proof.a.GetHash().GetHex());
    hashes.push_back(proof.b.GetHash().GetHex());
    entry.pushKV("hashes", std::move(hashes));
    DataStream s{};
    s << proof;
    entry.pushKV("hex", HexStr(s));
    return entry;
}

/** The fields of BondToJSON. */
std::vector<RPCResult> BondFields()
{
    return {
        {RPCResult::Type::STR, "owner", "The address that may unbond, and is paid what is unbonded (empty for a genesis validator that never bonded)"},
        {RPCResult::Type::STR_AMOUNT, "bonded", "Coins bonded"},
        {RPCResult::Type::STR_AMOUNT, "genesisstake", "The stake it starts with as a genesis validator (weight only)"},
        {RPCResult::Type::STR_AMOUNT, "unbonding", "Coins unbonded, not paid yet"},
        {RPCResult::Type::ARR, "unbonds", "Them, with the anchor they are paid at",
        {
            {RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_AMOUNT, "amount", "The coins"},
                {RPCResult::Type::NUM, "releaseanchor", "Paid at the anchor with this count"},
            }},
        }},
        {RPCResult::Type::NUM, "unbondsequence", "The sequence its next unbond has"},
        {RPCResult::Type::STR, "firstbond", "The first coin its first bond spent (txid:n), which its unbonds name; empty before a bond"},
        {RPCResult::Type::STR_HEX, "firstbondanchor", "The newest anchor block of the chain when its first bond was applied, which its unbonds name too"},
        {RPCResult::Type::BOOL, "eligible", "Whether its stake lets it take part (not banned, enough bonded)"},
        {RPCResult::Type::NUM, "jaileduntilepoch", "Left out of the sets made before this epoch (0: never jailed)"},
        {RPCResult::Type::BOOL, "banned", "Signed two blocks for a slot: never in a set again"},
    };
}

/**
 * The blocks of stake this node's validator keys signed on the active chain (their times), counted as
 * the chain grows: the blocks are looked at once, and again only above the fork of a reorg.
 */
struct MadeBlocks {
    Mutex mutex;
    std::vector<uint256> keys GUARDED_BY(mutex);
    //! The block the count is as of: a block of the active chain when it was made.
    const CBlockIndex* scanned GUARDED_BY(mutex){nullptr};
    //! The heights and times of the blocks made, in order.
    std::vector<std::pair<int, int64_t>> made GUARDED_BY(mutex);
};
MadeBlocks g_made_blocks;

std::vector<std::pair<int, int64_t>> BlocksMade(ChainstateManager& chainman, const std::vector<uint256>& keys)
{
    MadeBlocks& m{g_made_blocks};
    LOCK2(::cs_main, m.mutex);
    const CChain& chain{chainman.ActiveChain()};
    if (m.keys != keys) {
        m.keys = keys;
        m.scanned = nullptr;
        m.made.clear();
    }
    if (m.scanned && !chain.Contains(*m.scanned)) {
        // A reorg: what is above the fork is looked at again.
        m.scanned = chain.FindFork(*m.scanned);
        const int fork{m.scanned ? m.scanned->nHeight : -1};
        std::erase_if(m.made, [&](const auto& block) { return block.first > fork; });
    }
    if (keys.empty()) return {};
    for (int height{m.scanned ? m.scanned->nHeight + 1 : 0}; height <= chain.Height(); ++height) {
        const CBlockIndex& index{*chain[height]};
        if (!index.m_ext) continue;
        const CBlockHeader header{index.GetBlockHeader()};
        if (std::any_of(keys.begin(), keys.end(), [&](const uint256& key) { return pos::CheckBlockSig(header, key, chainman.GetConsensus().hashGenesisBlock); })) m.made.emplace_back(height, index.GetBlockTime());
    }
    m.scanned = chain.Tip();
    return m.made;
}

UniValue BondToJSON(const pos::ValidatorRecord& record, const Consensus::PosParams& params)
{
    UniValue bond(UniValue::VOBJ);
    bond.pushKV("owner", record.owner.IsNull() ? std::string{} : EncodeDestination(WitnessV0KeyHash{record.owner}));
    bond.pushKV("bonded", ValueFromAmount(record.bonded));
    bond.pushKV("genesisstake", ValueFromAmount(record.genesis_stake));
    bond.pushKV("unbonding", ValueFromAmount(record.Unbonding()));
    UniValue unbonds(UniValue::VARR);
    for (const pos::UnbondingEntry& entry : record.unbonding) {
        UniValue item(UniValue::VOBJ);
        item.pushKV("amount", ValueFromAmount(entry.amount));
        item.pushKV("releaseanchor", entry.release_anchor);
        unbonds.push_back(std::move(item));
    }
    bond.pushKV("unbonds", std::move(unbonds));
    bond.pushKV("unbondsequence", static_cast<uint64_t>(record.unbond_seq));
    bond.pushKV("firstbondanchor", record.first_bond_anchor.GetHex());
    bond.pushKV("firstbond", record.first_bond.IsNull() ? std::string{} : strprintf("%s:%u", record.first_bond.hash.GetHex(), record.first_bond.n));
    bond.pushKV("eligible", record.Eligible(params));
    bond.pushKV("jaileduntilepoch", record.jailed_until_epoch);
    bond.pushKV("banned", record.tombstoned);
    return bond;
}

RPCMethod getposinfo()
{
    return RPCMethod{
        "getposinfo",
        "Returns the state of FastBlocks' blocks of stake: the validators, the slot and its leader, anchors, the fee pot, and this node's block producer.",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::BOOL, "active", "Whether blocks of stake can be made in the current epoch (enough validators in its set)"},
            {RPCResult::Type::NUM, "slotseconds", "Length of a slot"},
            {RPCResult::Type::NUM, "slot", "The current slot, by this node's clock"},
            {RPCResult::Type::NUM, "epoch", "The current epoch"},
            {RPCResult::Type::STR_HEX, "leader", /*optional=*/true, "The x-only key of the leader of the current slot"},
            {RPCResult::Type::ARR, "validators", "The validator set of the current epoch",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR_HEX, "pubkey", "x-only key"},
                    {RPCResult::Type::NUM, "stake", "Weight in leader selection"},
                    {RPCResult::Type::STR_AMOUNT, "poolshare", "Its share by stake of the epoch's validator pool so far (fee split): what it is paid at the end of the epoch if the pool stopped growing, unless jailed or banned"},
                }},
            }},
            {RPCResult::Type::NUM, "anchors", "Anchors on the active chain (acknowledged tickets and BMM blocks)"},
            {RPCResult::Type::NUM, "lastanchormainheight", "Mainchain height of the commitment of the last anchor, -1 if none"},
            {RPCResult::Type::STR_HEX, "lastanchored", /*optional=*/true, "The block the last anchor anchors"},
            {RPCResult::Type::NUM, "lastanchoredheight", "Its height, -1 if none"},
            {RPCResult::Type::STR_HEX, "anchorpoint", /*optional=*/true, "The last block that took the peg actions: the next ticket anchors it or a block after it"},
            {RPCResult::Type::NUM, "lastposslot", "Slot of the last block of stake on the active chain, -1 if none"},
            {RPCResult::Type::NUM, "firstunanchoredslot", "Slot of the first block of stake no anchor covers, -1 if none"},
            {RPCResult::Type::NUM, "maxunanchoredslots", "Blocks of stake go on for this many slots after the first one no anchor covers (max_unanchored_slots, or the long-gap rule's from gapepoch): how long the chain runs without a mainchain block, now"},
            {RPCResult::Type::NUM, "setleadseconds", "The validator set of the current epoch is fixed by an anchor at least an epoch, and at least this long, before it starts (set_lead_seconds, or the long-gap rule's)"},
            {RPCResult::Type::NUM, "seedlookaheadseconds", "The seed of the current epoch is the mainchain block whose median time is this long before it starts (seed_lookahead_seconds, or the long-gap rule's)"},
            {RPCResult::Type::NUM, "gapepoch", "The first epoch of the long-gap rule (blocks of stake go on for 24 hours without BMM, and no seed stops them first), -1 if never"},
            {RPCResult::Type::OBJ, "gap", /*optional=*/true, "How long blocks of stake can go on if no mainchain block comes from now (with a mainchain record)",
            {
                {RPCResult::Type::NUM_TIME, "mainmediantime", "The median time of the last mainchain block on record"},
                {RPCResult::Type::NUM_TIME, "seedsuntil", "The end of the last epoch whose seed that block makes known: no leader after it without a later mainchain block"},
                {RPCResult::Type::NUM_TIME, "unanchoreduntil", /*optional=*/true, "When the unanchored limit stops blocks of stake without an anchor (a run of blocks no anchor covers)"},
                {RPCResult::Type::NUM_TIME, "blocksuntil", "The earlier of the two: when blocks of stake stop if no mainchain block comes"},
            }},
            {RPCResult::Type::NUM, "jailfixheight", "jail_fix_height: the first block of the liveness fix (no slot charged before the first block of stake, no jails that would leave too few validators)"},
            {RPCResult::Type::BOOL, "stalled", "Whether a BMM block built now may carry transactions (blocks of stake stalled)"},
            {RPCResult::Type::OBJ, "pot", "The fee pot",
            {
                {RPCResult::Type::STR_AMOUNT, "accrued", "Put in the pot since the chain began"},
                {RPCResult::Type::STR_AMOUNT, "paid", "Paid out of it"},
                {RPCResult::Type::STR_AMOUNT, "reserve", "Penalties not paid out yet: each anchor pays a share of them with the pot"},
            }},
            {RPCResult::Type::OBJ, "staking", "Staking on the active chain",
            {
                {RPCResult::Type::STR_AMOUNT, "bonded", "Coins bonded"},
                {RPCResult::Type::STR_AMOUNT, "unbonding", "Coins unbonded, not paid yet"},
                {RPCResult::Type::STR_AMOUNT, "rewardsowed", "Rewards for proofs that leaders signed twice, owed to the blocks that carried them"},
                {RPCResult::Type::STR_AMOUNT, "slashed", "Penalties since the chain began"},
                {RPCResult::Type::STR_AMOUNT, "minstake", "The least a validator must bond to take part"},
                {RPCResult::Type::NUM, "unbondinganchors", "Anchors after an unbond that it is paid"},
            }},
            FeeSplitFields()[0],
            {RPCResult::Type::STR_HEX, "chainkey", "The order of the active chain (nChainWork): anchors, last commitment height, height"},
            {RPCResult::Type::NUM, "tickets", "Anchor tickets this node knows"},
            {RPCResult::Type::NUM, "evidence", "Proofs this node knows that a leader signed two blocks for one slot"},
            {RPCResult::Type::OBJ, "producer", /*optional=*/true, "This node's block producer, if it has validator keys",
            {
                {RPCResult::Type::ARR, "keys", "x-only keys", {{RPCResult::Type::STR_HEX, "", ""}}},
                {RPCResult::Type::NUM, "produced", "Blocks made"},
                {RPCResult::Type::NUM, "acknowledgements", "Of which acknowledgements"},
                {RPCResult::Type::NUM, "failed", "Slots this node led and made no block for"},
                {RPCResult::Type::NUM, "lastslot", "Slot of the last block made, -1 if none"},
                {RPCResult::Type::STR, "error", "Why the last block could not be made, if it could not"},
            }},
        }},
        RPCExamples{HelpExampleCli("getposinfo", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node{EnsureAnyNodeContext(request.context)};
    ChainstateManager& chainman{EnsureChainman(node)};
    const pos::FastBlocksHooks* hooks{pos::InstalledHooks()};
    if (!hooks) throw JSONRPCError(RPC_MISC_ERROR, "This chain is not a sidechain: it has no blocks of stake");
    const Consensus::PosParams& params{hooks->Params()};
    const int64_t now{TicksSinceEpoch<std::chrono::seconds>(NodeClock::now())};
    const int64_t slot{pos::SlotOf(now, params)};
    const EpochSchedule schedule{ScheduleOf(*hooks, chainman, pos::EpochOf(slot, params))};
    const bool active{schedule.set && schedule.set->Active(pos::EpochOf(slot, params))};
    UniValue result(UniValue::VOBJ);
    result.pushKV("active", active);
    result.pushKV("slotseconds", params.slot_seconds);
    result.pushKV("slot", slot);
    result.pushKV("epoch", pos::EpochOf(slot, params));
    if (const auto leader{schedule.LeaderKey(slot)}) result.pushKV("leader", HexStr(*leader));
    UniValue validators(UniValue::VARR);
    if (schedule.set) {
        for (const auto& validator : schedule.set->Validators()) {
            UniValue entry(UniValue::VOBJ);
            entry.pushKV("pubkey", HexStr(validator.pubkey));
            entry.pushKV("stake", validator.stake);
            entry.pushKV("poolshare", ValueFromAmount(PoolShareOf(chainman, schedule, pos::EpochOf(slot, params), validator.stake)));
            validators.push_back(std::move(entry));
        }
    }
    result.pushKV("validators", std::move(validators));
    pos::FastState fast;
    pos::StakeState stake;
    arith_uint256 work;
    {
        LOCK(::cs_main);
        fast = pos::GetFastState(chainman.ActiveChainstate().SideState().View());
        stake = pos::GetStakeState(chainman.ActiveChainstate().SideState().View());
        work = chainman.ActiveChain().Tip()->nChainWork;
    }
    result.pushKV("anchors", fast.anchors);
    result.pushKV("lastanchormainheight", fast.last_main_height);
    if (fast.last_anchored_height >= 0) result.pushKV("lastanchored", fast.last_anchored.GetHex());
    result.pushKV("lastanchoredheight", fast.last_anchored_height);
    if (fast.anchor_point_height >= 0) result.pushKV("anchorpoint", fast.anchor_point.GetHex());
    result.pushKV("lastposslot", fast.last_pos_slot);
    result.pushKV("firstunanchoredslot", fast.first_unanchored_slot);
    const int64_t epoch_now{pos::EpochOf(slot, params)};
    result.pushKV("maxunanchoredslots", params.MaxUnanchoredSlots(slot));
    result.pushKV("setleadseconds", params.SetLead(epoch_now));
    result.pushKV("seedlookaheadseconds", params.SeedLookahead(epoch_now));
    result.pushKV("gapepoch", params.gap_epoch == std::numeric_limits<int64_t>::max() ? int64_t{-1} : params.gap_epoch);
    const auto main_tip{chainman.m_mainchain ? chainman.m_mainchain->GetBlock(chainman.m_mainchain->Height()) : std::nullopt};
    if (main_tip && params.slots_per_epoch > 0) {
        // The last epoch whose seed is known (pos::SeedHeight: its seed time before the last median time), from now on.
        int64_t epoch{std::max<int64_t>(epoch_now, 0)};
        for (int i{0}; i < 1000 && pos::SeedTime(epoch + 1, params) < main_tip->time; ++i) ++epoch;
        const bool seeded{pos::SeedTime(epoch, params) < main_tip->time};
        const int64_t seeds_until{pos::SlotTime((seeded ? epoch + 1 : epoch) * params.slots_per_epoch, params)};
        UniValue gap(UniValue::VOBJ);
        gap.pushKV("mainmediantime", main_tip->time);
        gap.pushKV("seedsuntil", seeds_until);
        int64_t until{seeds_until};
        if (fast.first_unanchored_slot >= 0) {
            const int64_t unanchored{pos::SlotTime(fast.first_unanchored_slot + params.MaxUnanchoredSlots(slot), params)};
            gap.pushKV("unanchoreduntil", unanchored);
            until = std::min(until, unanchored);
        }
        gap.pushKV("blocksuntil", until);
        result.pushKV("gap", std::move(gap));
    }
    result.pushKV("jailfixheight", params.jail_fix_height);
    result.pushKV("stalled", hooks->Stalled(fast, main_tip ? main_tip->time : now, active));
    UniValue pot(UniValue::VOBJ);
    pot.pushKV("accrued", ValueFromAmount(fast.pot_accrued));
    pot.pushKV("paid", ValueFromAmount(fast.pot_paid));
    pot.pushKV("reserve", ValueFromAmount(stake.pot_reserve));
    result.pushKV("pot", std::move(pot));
    UniValue staking(UniValue::VOBJ);
    staking.pushKV("bonded", ValueFromAmount(stake.bonded));
    staking.pushKV("unbonding", ValueFromAmount(stake.unbonding));
    staking.pushKV("rewardsowed", ValueFromAmount(stake.rewards_owed));
    staking.pushKV("slashed", ValueFromAmount(stake.slashed));
    staking.pushKV("minstake", ValueFromAmount(params.min_stake));
    staking.pushKV("unbondinganchors", params.unbonding_anchors);
    result.pushKV("staking", std::move(staking));
    result.pushKV("feesplit", FeeSplitToJSON(*hooks, chainman, slot));
    result.pushKV("chainkey", work.GetHex());
    result.pushKV("tickets", hooks->Tickets().size());
    result.pushKV("evidence", hooks->Evidence().Size());
    if (const pos::Producer* producer{hooks->GetProducer()}) {
        const auto status{producer->GetStatus()};
        UniValue entry(UniValue::VOBJ);
        UniValue keys(UniValue::VARR);
        for (const uint256& key : producer->Keys()) keys.push_back(HexStr(key));
        entry.pushKV("keys", std::move(keys));
        entry.pushKV("produced", status.produced);
        entry.pushKV("acknowledgements", status.acknowledgements);
        entry.pushKV("failed", status.failed);
        entry.pushKV("lastslot", status.last_slot);
        entry.pushKV("error", status.error);
        result.pushKV("producer", std::move(entry));
    }
    return result;
},
    };
}


RPCMethod getvalidators()
{
    return RPCMethod{
        "getvalidators",
        "Returns the validator set of an epoch of FastBlocks' blocks of stake, as the active chain fixes it, and how many slots each validator leads in it.",
        {
            {"epoch", RPCArg::Type::NUM, RPCArg::DefaultHint{"the current epoch"}, "The epoch"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "epoch", "The epoch"},
            {RPCResult::Type::NUM, "firstslot", "Its first slot"},
            {RPCResult::Type::BOOL, "known", "Whether the active chain fixes the set of this epoch yet (by an anchor before the epoch before it)"},
            {RPCResult::Type::BOOL, "seedknown", "Whether the record of the mainchain has the block that seeds the epoch"},
            {RPCResult::Type::BOOL, "active", "Whether blocks of stake can be made (enough validators)"},
            {RPCResult::Type::NUM, "totalstake", "The stake of the set"},
            {RPCResult::Type::ARR, "validators", "In the order leaders are picked from (by key)",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR_HEX, "pubkey", "x-only key"},
                    {RPCResult::Type::NUM, "stake", "Weight in leader selection"},
                    {RPCResult::Type::NUM, "slots", "Slots of the epoch it leads"},
                    {RPCResult::Type::BOOL, "local", "Whether this node holds its key"},
                    {RPCResult::Type::BOOL, "banned", "Whether the active chain banned it since (it leads no more)"},
                    {RPCResult::Type::NUM, "jaileduntilepoch", "Left out of the sets made before this epoch, in the state of the active chain (0: never jailed)"},
                }},
            }},
        }},
        RPCExamples{HelpExampleCli("getvalidators", "") + HelpExampleCli("getvalidators", "100")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const pos::FastBlocksHooks& hooks{EnsureHooks()};
    const Consensus::PosParams& params{hooks.Params()};
    const int64_t epoch{request.params[0].isNull() ? pos::EpochOf(NowSlot(params), params) : request.params[0].getInt<int64_t>()};
    if (epoch < 0 || epoch > std::numeric_limits<uint32_t>::max()) throw JSONRPCError(RPC_INVALID_PARAMETER, "epoch out of range");
    const EpochSchedule schedule{ScheduleOf(hooks, chainman, epoch)};
    const auto slots{SlotsLed(hooks, schedule, epoch)};
    const auto local{LocalKeys(hooks)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("epoch", epoch);
    result.pushKV("firstslot", epoch * params.slots_per_epoch);
    result.pushKV("known", schedule.set != nullptr);
    result.pushKV("seedknown", schedule.seed.has_value());
    result.pushKV("active", schedule.set && schedule.set->Active(epoch));
    result.pushKV("totalstake", schedule.set ? schedule.set->TotalStake() : 0);
    UniValue validators(UniValue::VARR);
    for (size_t i{0}; schedule.set && i < schedule.set->Validators().size(); ++i) {
        const auto& validator{schedule.set->Validators()[i]};
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("pubkey", HexStr(validator.pubkey));
        entry.pushKV("stake", validator.stake);
        entry.pushKV("slots", slots[i]);
        entry.pushKV("local", std::find(local.begin(), local.end(), validator.pubkey) != local.end());
        entry.pushKV("banned", schedule.banned.contains(validator.pubkey));
        const auto record{WITH_LOCK(::cs_main, return pos::GetValidator(chainman.ActiveChainstate().SideState().View(), validator.pubkey))};
        entry.pushKV("jaileduntilepoch", record ? record->jailed_until_epoch : 0);
        validators.push_back(std::move(entry));
    }
    result.pushKV("validators", std::move(validators));
    return result;
},
    };
}

RPCMethod getvalidator()
{
    return RPCMethod{
        "getvalidator",
        "Returns what this node knows of a validator of FastBlocks: its bond, whether it is in the set of the current epoch, its stake, and the next slot it leads.",
        {
            {"pubkey", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The validator's x-only key"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "pubkey", "x-only key"},
            {RPCResult::Type::BOOL, "inset", "Whether it is in the validator set of the current epoch"},
            {RPCResult::Type::NUM, "stake", /*optional=*/true, "Its weight in leader selection"},
            {RPCResult::Type::NUM, "index", /*optional=*/true, "Its position in the set"},
            {RPCResult::Type::NUM, "slots", /*optional=*/true, "Slots of the current epoch it leads"},
            {RPCResult::Type::NUM, "nextslot", /*optional=*/true, "The next slot it leads (within two epochs)"},
            {RPCResult::Type::BOOL, "local", "Whether this node holds its key"},
            {RPCResult::Type::NUM, "offences", "Proofs this node knows that it signed two blocks for one slot"},
            {RPCResult::Type::OBJ, "bond", /*optional=*/true, "Its bond, in the state of the active chain", BondFields()},
        }},
        RPCExamples{HelpExampleCli("getvalidator", "\"<pubkey>\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const pos::FastBlocksHooks& hooks{EnsureHooks()};
    const Consensus::PosParams& params{hooks.Params()};
    const uint256 pubkey{ParsePubkey(request.params[0])};
    const int64_t slot{NowSlot(params)};
    const auto local{LocalKeys(hooks)};
    const EpochSchedule schedule{ScheduleOf(hooks, chainman, pos::EpochOf(slot, params))};
    UniValue result(UniValue::VOBJ);
    result.pushKV("pubkey", HexStr(pubkey));
    const auto index{schedule.set ? schedule.set->Find(pubkey) : std::nullopt};
    result.pushKV("inset", index.has_value());
    if (index) {
        result.pushKV("stake", schedule.set->Validators()[*index].stake);
        result.pushKV("index", static_cast<uint64_t>(*index));
        result.pushKV("slots", SlotsLed(hooks, schedule, pos::EpochOf(slot, params))[*index]);
        if (const auto next{NextSlotLed(hooks, chainman, {pubkey}, slot + 1)}) result.pushKV("nextslot", *next);
    }
    result.pushKV("local", std::find(local.begin(), local.end(), pubkey) != local.end());
    int64_t offences{0};
    for (const auto& [offence, _] : hooks.Evidence().All()) offences += offence.pubkey == pubkey;
    result.pushKV("offences", offences);
    const auto record{WITH_LOCK(::cs_main, return pos::GetValidator(chainman.ActiveChainstate().SideState().View(), pubkey))};
    if (record) result.pushKV("bond", BondToJSON(*record, params));
    return result;
},
    };
}

RPCMethod getleaderschedule()
{
    return RPCMethod{
        "getleaderschedule",
        "Returns the leader of each slot of an epoch of FastBlocks' blocks of stake.",
        {
            {"epoch", RPCArg::Type::NUM, RPCArg::DefaultHint{"the current epoch"}, "The epoch"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "epoch", "The epoch"},
            {RPCResult::Type::ARR, "slots", "Its slots, in order",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::NUM, "slot", "The slot"},
                    {RPCResult::Type::NUM_TIME, "time", "When it starts, " + UNIX_EPOCH_TIME},
                    {RPCResult::Type::STR_HEX, "leader", /*optional=*/true, "x-only key of its leader (none if blocks of stake cannot be made, or the set or the seed of the epoch is not known yet, or the leader is banned)"},
                    {RPCResult::Type::STR_HEX, "banned", /*optional=*/true, "x-only key of the leader the set names, banned since: nobody leads the slot"},
                }},
            }},
        }},
        RPCExamples{HelpExampleCli("getleaderschedule", "") + HelpExampleCli("getleaderschedule", "100")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const pos::FastBlocksHooks& hooks{EnsureHooks()};
    const Consensus::PosParams& params{hooks.Params()};
    const int64_t epoch{request.params[0].isNull() ? pos::EpochOf(NowSlot(params), params) : request.params[0].getInt<int64_t>()};
    if (epoch < 0 || epoch > std::numeric_limits<uint32_t>::max()) throw JSONRPCError(RPC_INVALID_PARAMETER, "epoch out of range");
    const EpochSchedule schedule{ScheduleOf(hooks, chainman, epoch)};
    UniValue slots(UniValue::VARR);
    for (int64_t slot{epoch * params.slots_per_epoch}; slot < (epoch + 1) * params.slots_per_epoch; ++slot) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("slot", slot);
        entry.pushKV("time", pos::SlotTime(slot, params));
        if (const auto leader{schedule.LeaderKey(slot)}) {
            entry.pushKV("leader", HexStr(*leader));
        } else if (const auto scheduled{schedule.ScheduledKey(slot)}) {
            entry.pushKV("banned", HexStr(*scheduled));
        }
        slots.push_back(std::move(entry));
    }
    UniValue result(UniValue::VOBJ);
    result.pushKV("epoch", epoch);
    result.pushKV("slots", std::move(slots));
    return result;
},
    };
}

RPCMethod getanchorinfo()
{
    return RPCMethod{
        "getanchorinfo",
        "Returns FastBlocks' anchors: the newest ones on the active chain, the blocks they make anchored and final, the fee pot, and the tickets this node knows.",
        {
            {"count", RPCArg::Type::NUM, RPCArg::Default{10}, "How many of the newest anchors and tickets to list (at most 100)"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "anchors", "Anchors on the active chain"},
            {RPCResult::Type::NUM, "finalanchors", "Anchors that make a block final"},
            {RPCResult::Type::STR_HEX, "anchoredtip", /*optional=*/true, "The highest block an anchor covers"},
            {RPCResult::Type::NUM, "anchoredheight", "Its height, -1 if none"},
            {RPCResult::Type::STR_HEX, "finaltip", /*optional=*/true, "The highest final block"},
            {RPCResult::Type::NUM, "finalheight", "Its height, -1 if none"},
            {RPCResult::Type::ARR, "recent", "The newest anchors, newest first",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR_HEX, "block", "The acknowledgement or the BMM block"},
                    {RPCResult::Type::NUM, "height", "Its height"},
                    {RPCResult::Type::STR, "kind", "\"ticket\" (an acknowledgement) or \"bmm\""},
                    {RPCResult::Type::STR_HEX, "anchored", "The block it anchors"},
                    {RPCResult::Type::NUM, "anchoredheight", "Its height"},
                    {RPCResult::Type::NUM, "mainheight", "Mainchain height of the commitment"},
                }},
            }},
            {RPCResult::Type::ARR, "skipped", "Commitments on record after the oldest anchor finality looks at that the active chain does not use: each may be the anchor of a chain that leaves some of its blocks out",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::NUM, "mainheight", "Mainchain height of the commitment"},
                    {RPCResult::Type::STR, "fork", "Where a chain with it as an anchor leaves this one: \"any\" (the node does not know its block), \"from\" (at forkheight or above) or \"at\" (at forkheight)"},
                    {RPCResult::Type::NUM, "forkheight", /*optional=*/true, "The height of that fork"},
                }},
            }},
            {RPCResult::Type::OBJ, "pot", "The fee pot",
            {
                {RPCResult::Type::STR_AMOUNT, "accrued", "Put in the pot since the chain began"},
                {RPCResult::Type::STR_AMOUNT, "paid", "Paid out of it"},
                {RPCResult::Type::STR_AMOUNT, "due", "What an anchor of the tip would pay"},
            }},
            {RPCResult::Type::ARR, "tickets", "The newest tickets this node knows, newest first",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR_HEX, "hash", "h*, what the mainchain commits to"},
                    {RPCResult::Type::STR_HEX, "anchored", "The block it anchors"},
                    {RPCResult::Type::STR_HEX, "payee", "The script the pot is paid to"},
                    {RPCResult::Type::NUM, "committedheight", /*optional=*/true, "Mainchain height of the commitment to it, if any"},
                }},
            }},
            {RPCResult::Type::OBJ, "bid", "What this node's anchorer bids, and why (see getanchorbidpolicy)", BidResultDoc()},
        }},
        RPCExamples{HelpExampleCli("getanchorinfo", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const pos::FastBlocksHooks& hooks{EnsureHooks()};
    const int count{request.params[0].isNull() ? 10 : request.params[0].getInt<int>()};
    if (count < 0 || count > 100) throw JSONRPCError(RPC_INVALID_PARAMETER, "count must be from 0 to 100");
    const int final_anchors{hooks.Params().final_anchors};
    UniValue result(UniValue::VOBJ);
    UniValue recent(UniValue::VARR);
    UniValue pot(UniValue::VOBJ);
    {
        LOCK(::cs_main);
        const CBlockIndex& tip{*chainman.ActiveChain().Tip()};
        const pos::FastState fast{pos::GetFastState(chainman.ActiveChainstate().SideState().View())};
        result.pushKV("anchors", fast.anchors);
        result.pushKV("finalanchors", final_anchors);
        const auto anchors{pos::RecentAnchors(hooks, tip, std::max<size_t>(count, final_anchors))};
        const auto heights{pos::AnchoredHeights(hooks, tip, final_anchors)};
        const CBlockIndex* anchored{heights.empty() ? nullptr : tip.GetAncestor(heights.front())};
        const CBlockIndex* final_block{static_cast<int>(heights.size()) >= final_anchors && final_anchors > 0 ? tip.GetAncestor(heights[final_anchors - 1]) : nullptr};
        if (anchored) result.pushKV("anchoredtip", anchored->GetBlockHash().GetHex());
        result.pushKV("anchoredheight", anchored ? anchored->nHeight : -1);
        if (final_block) result.pushKV("finaltip", final_block->GetBlockHash().GetHex());
        result.pushKV("finalheight", final_block ? final_block->nHeight : -1);
        for (size_t i{0}; i < anchors.size() && i < static_cast<size_t>(count); ++i) {
            const pos::ChainAnchor& anchor{anchors[i]};
            UniValue entry(UniValue::VOBJ);
            entry.pushKV("block", anchor.block->GetBlockHash().GetHex());
            entry.pushKV("height", anchor.block->nHeight);
            entry.pushKV("kind", anchor.block->m_ext ? "ticket" : "bmm");
            entry.pushKV("anchored", anchor.anchored->GetBlockHash().GetHex());
            entry.pushKV("anchoredheight", anchor.anchored->nHeight);
            entry.pushKV("mainheight", static_cast<uint64_t>(anchor.main_height));
            recent.push_back(std::move(entry));
        }
        UniValue skipped(UniValue::VARR);
        for (const pos::SkippedCommitment& commitment : pos::SkippedCommitments(hooks, tip, pos::RecentAnchors(hooks, tip, pos::AnchorsWalked(final_anchors)))) {
            UniValue entry(UniValue::VOBJ);
            entry.pushKV("mainheight", static_cast<uint64_t>(commitment.main_height));
            entry.pushKV("fork", commitment.exact_fork ? "at" : commitment.min_fork ? "from" : "any");
            if (commitment.exact_fork || commitment.min_fork) entry.pushKV("forkheight", commitment.exact_fork ? *commitment.exact_fork : *commitment.min_fork);
            skipped.push_back(std::move(entry));
        }
        result.pushKV("skipped", std::move(skipped));
        pot.pushKV("accrued", ValueFromAmount(fast.pot_accrued));
        pot.pushKV("paid", ValueFromAmount(fast.pot_paid));
        pot.pushKV("due", ValueFromAmount(pos::PotAccruedAt(chainman.ActiveChainstate().SideState().View(), tip.nHeight) - fast.pot_paid));
    }
    result.pushKV("recent", std::move(recent));
    result.pushKV("pot", std::move(pot));
    UniValue tickets(UniValue::VARR);
    const auto known{hooks.Tickets()};
    for (auto it{known.rbegin()}; it != known.rend() && static_cast<int>(tickets.size()) < count; ++it) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("hash", it->Hash().GetHex());
        entry.pushKV("anchored", it->anchored.GetHex());
        entry.pushKV("payee", HexStr(it->payee));
        if (chainman.m_mainchain) {
            if (const auto committed{chainman.m_mainchain->CommittedHeight(it->Hash())}) entry.pushKV("committedheight", *committed);
        }
        tickets.push_back(std::move(entry));
    }
    result.pushKV("tickets", std::move(tickets));
    result.pushKV("bid", BidResult(ReportNow(chainman, hooks, &EnsureAnyNodeContext(request.context)), count));
    return result;
},
    };
}

RPCMethod getanchorticket()
{
    return RPCMethod{
        "getanchorticket",
        "Returns what a mainchain BMM commitment of this chain is: on FastBlocks a commitment is to an anchor ticket (h*), not to a block, "
        "so the block it anchors is found here. A block hash committed to (a merged mined block) is getblockheader's.",
        {
            {"hash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hash the mainchain committed to"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "hash", "The hash given"},
            {RPCResult::Type::BOOL, "known", "Whether it is a ticket this node knows"},
            {RPCResult::Type::STR_HEX, "anchored", /*optional=*/true, "The block it anchors"},
            {RPCResult::Type::NUM, "anchoredheight", /*optional=*/true, "Its height, if this node has its header"},
            {RPCResult::Type::BOOL, "inactivechain", /*optional=*/true, "Whether that block is in the active chain"},
            {RPCResult::Type::STR_HEX, "payee", /*optional=*/true, "The script the pot is paid to"},
            {RPCResult::Type::NUM, "committedheight", /*optional=*/true, "Mainchain height of the commitment to it, if on record"},
        }},
        RPCExamples{HelpExampleCli("getanchorticket", "\"hash\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    const pos::FastBlocksHooks& hooks{EnsureHooks()};
    const uint256 hash{ParseHashV(request.params[0], "hash")};
    UniValue result(UniValue::VOBJ);
    result.pushKV("hash", hash.GetHex());
    const auto ticket{hooks.GetTicket(hash)};
    result.pushKV("known", ticket.has_value());
    if (!ticket) return result;
    result.pushKV("anchored", ticket->anchored.GetHex());
    {
        LOCK(::cs_main);
        if (const CBlockIndex* index{chainman.m_blockman.LookupBlockIndex(ticket->anchored)}) {
            result.pushKV("anchoredheight", index->nHeight);
            result.pushKV("inactivechain", chainman.ActiveChain().Contains(*index));
        }
    }
    result.pushKV("payee", HexStr(ticket->payee));
    if (chainman.m_mainchain) {
        if (const auto committed{chainman.m_mainchain->CommittedHeight(hash)}) result.pushKV("committedheight", *committed);
    }
    return result;
},
    };
}

RPCMethod getanchorbidpolicy()
{
    return RPCMethod{
        "getanchorbidpolicy",
        "Returns the node's bid policy, for both triggers (a validator's anchorer and the merged miner, setbmm): how it bids for a BMM commitment "
        "(see doc/fastblocks-anchor-bids.md). What it bids now: getanchorinfo's bid.",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "", PolicyResultDoc()},
        RPCExamples{HelpExampleCli("getanchorbidpolicy", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    return PolicyResult(ReportNow(chainman, EnsureHooks(), &EnsureAnyNodeContext(request.context)));
},
    };
}

RPCMethod setanchorbidpolicy()
{
    return RPCMethod{
        "setanchorbidpolicy",
        "Changes the node's bid policy, at once and without a restart: the one policy of both triggers, a validator's anchorer and the merged miner "
        "of any node (setbmm), whose bids the same mainchain wallet pays. The fields given change; the others stay. "
        "Kept in the node's settings (settings.json), over the options (-validatoranchorbid*), which stay the defaults (reset: back to them). "
        "strategy off stops every bid of the node (both triggers; one by hand still goes); anchor=false stops only the anchorer. "
        "The strategy names before 8.3.6 still work and say whether to prevent stalling, unless preventstall is given: feespreventstall "
        "(fees, yes; feesminimum with a floor above 0), minimal (minimum, yes). fees, fixed and off are new names here: they say nothing of stall "
        "prevention (preventstall=false is what turns it off; on by default). Prevent stalling bids without fees only near the unanchored limit, "
        "or when deposits, withdrawals, and bonds, unbonds and jails wait for an anchor. rebidafter 0: no raises after missed blocks.",
        {
            {"policy", RPCArg::Type::OBJ_NAMED_PARAMS, RPCArg::Optional::OMITTED, "The fields to change",
            {
                {"strategy", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "fees, feesminimum, minimum, fixed or off (or the older minimal, feespreventstall)"},
                {"preventstall", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Prevent stalling: near the unanchored limit, bid enough to land an anchor whatever the strategy"},
                {"anchor", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Anchor my validator: a validator's own anchorer bids"},
                {"floor", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "The user's minimum, for feesminimum and minimum (0 allowed: the relay minimum)"},
                {"max", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "The most it bids, always (at least floor and fixed)"},
                {"fixed", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "The bid of the fixed strategy"},
                {"feeshare", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The percentage of the pot the fee strategies bid (0-100)"},
                {"escalationstart", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The stall pressure from which the bid escalates (above 0, at most 1)"},
                {"curve", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The escalation curve, \"p:x,...\" (or an array of {pressure, multiplier}); pressures ascending, multipliers 1-1000 not falling"},
                {"interpolate", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Linear between the points of the curve (else steps)"},
                {"usefeeestimate", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Stall prevention's base follows the mainchain's next-block fee estimate"},
                {"requestvsize", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The size of a BMM request, in vbytes (50-10000)"},
                {"rebidafter", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Missed mainchain blocks in a row before the minimum is raised (the legacy curve; with the flat boost, the default, x2 for every two missed, at most boost); 0: no raises after missed blocks, in either"},
                {"rebidstep", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The legacy curve: the multiplier of each such raise (1-100)"},
                {"rebidmax", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The legacy curve: the most those raises multiply the bid (1-1000)"},
                {"warnmissed", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Missed mainchain blocks in a row before the \"bid too low\" warning (0: never)"},
                {"periodcap", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "The most the landed bids cost in periodhours (0: no cap)"},
                {"periodhours", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The period of the cap, in hours (1-744)"},
                {"stallreserve", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The percentage of the cap kept for stall prevention's bids (0-100; 50 by default): every other bid spends at most the cap less it"},
                {"lowbalanceblocks", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "A warning when the bid wallet covers fewer mainchain blocks (0: never)"},
            }},
            {"reset", RPCArg::Type::BOOL, RPCArg::Default{false}, "Back to the options' policy first (the settings forgotten)"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "The policy, as getanchorbidpolicy", PolicyResultDoc()},
        RPCExamples{HelpExampleCli("setanchorbidpolicy", "'{\"strategy\":\"fees\",\"preventstall\":true}'") +
                    HelpExampleCli("-named setanchorbidpolicy", "floor=0.00001 max=0.0005") +
                    HelpExampleCli("-named setanchorbidpolicy", "reset=true") +
                    HelpExampleRpc("setanchorbidpolicy", "{\"curve\":\"0.5:1,0.75:3,0.9:6\"}")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node{EnsureAnyNodeContext(request.context)};
    ChainstateManager& chainman{EnsureChainman(node)};
    const pos::FastBlocksHooks& hooks{EnsureHooks()};
    const auto report{ReportNow(chainman, hooks, &EnsureAnyNodeContext(request.context))};
    const bool reset{!request.params[1].isNull() && request.params[1].get_bool()};
    const UniValue fields{request.params[0].isNull() ? UniValue{UniValue::VOBJ} : request.params[0]};
    if (!fields.isObject()) throw JSONRPCError(RPC_INVALID_PARAMETER, "policy: an object of fields");
    pos::AnchorBidPolicy policy{reset ? report.defaults : report.policy};
    std::string error;
    if (!pos::ApplyPolicyFields(policy, fields, error)) throw JSONRPCError(RPC_INVALID_PARAMETER, error);
    // The fields changed from the options' policy, as they are said (normalized).
    UniValue overrides{reset ? UniValue{UniValue::VOBJ} : report.overrides};
    // A saved strategy that depends on the floor (8.3.5's feespreventstall): again for the floor now, as after a restart.
    pos::ReapplySavedStrategy(policy, overrides, fields);
    if (error = pos::CheckPolicy(policy); !error.empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, error);
    const UniValue said{pos::PolicyToUniValue(policy)};
    // A strategy is kept with what it says of stall prevention (an old name says it too), so that the settings read back the
    // same; and preventstall is in every policy saved, which tells it from one 8.3.5 saved (pos::IsLegacySettings).
    std::vector<std::string> keys{fields.getKeys()};
    if (!keys.empty() && fields.find_value("preventstall").isNull() && (!fields.find_value("strategy").isNull() || overrides.find_value("preventstall").isNull())) {
        keys.emplace_back("preventstall");
    }
    for (const std::string& key : keys) {
        UniValue kept(UniValue::VOBJ);
        for (const std::string& other : overrides.getKeys()) {
            if (other != key) kept.pushKV(other, overrides.find_value(other));
        }
        kept.pushKV(key, said.find_value(key));
        overrides = std::move(kept);
    }
    if (!hooks.SetAnchorBidPolicy(policy, overrides, error)) throw JSONRPCError(RPC_INVALID_PARAMETER, error);
    if (node.args) {
        node.args->LockSettings([&](common::Settings& settings) {
            if (overrides.getKeys().empty()) {
                settings.rw_settings.erase(pos::ANCHOR_BID_SETTING);
            } else {
                settings.rw_settings[pos::ANCHOR_BID_SETTING] = overrides;
            }
        });
        if (node.args->GetSettingsPath()) node.args->WriteSettingsFile();
    }
    return PolicyResult(ReportNow(chainman, hooks, &EnsureAnyNodeContext(request.context)));
},
    };
}

RPCMethod submitevidence()
{
    return RPCMethod{
        "submitevidence",
        "Gives the node a proof that a leader of FastBlocks signed two blocks for one slot (two extended headers, serialized): it keeps it and relays it.",
        {
            {"hex", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The proof: the two headers, serialized, in hex"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "pubkey", "The validator that signed twice"},
            {RPCResult::Type::NUM, "slot", "The slot"},
            {RPCResult::Type::BOOL, "new", "Whether the node did not know the proof yet"},
        }},
        RPCExamples{HelpExampleCli("submitevidence", "\"<hex>\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const pos::FastBlocksHooks& hooks{EnsureHooks()};
    const std::vector<unsigned char> bytes{ParseHexV(request.params[0], "hex")};
    pos::DoubleProposal proof;
    try {
        SpanReader reader{bytes};
        reader >> proof;
        if (!reader.empty()) throw std::ios_base::failure("extra bytes");
    } catch (const std::ios_base::failure&) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "The proof does not read: two extended headers are expected");
    }
    const size_t before{hooks.Evidence().Size()};
    std::string reason;
    const auto offence{hooks.AddEvidence(proof, reason)};
    if (!offence) throw JSONRPCError(RPC_VERIFY_REJECTED, reason);
    UniValue result(UniValue::VOBJ);
    result.pushKV("pubkey", HexStr(offence->pubkey));
    result.pushKV("slot", offence->slot);
    result.pushKV("new", hooks.Evidence().Size() > before);
    return result;
},
    };
}

RPCMethod listevidence()
{
    return RPCMethod{
        "listevidence",
        "Lists the proofs this node knows that a leader of FastBlocks signed two blocks for one slot (found by watching headers, or received).",
        {},
        RPCResult{RPCResult::Type::ARR, "", "",
        {
            {RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "pubkey", "The validator that signed twice"},
                {RPCResult::Type::NUM, "slot", "The slot"},
                {RPCResult::Type::ARR, "hashes", "The two blocks", {{RPCResult::Type::STR_HEX, "", ""}}},
                {RPCResult::Type::STR_HEX, "hex", "The proof, for submitevidence"},
            }},
        }},
        RPCExamples{HelpExampleCli("listevidence", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const pos::FastBlocksHooks& hooks{EnsureHooks()};
    UniValue result(UniValue::VARR);
    for (const auto& [offence, proof] : hooks.Evidence().All()) result.push_back(ProofToJSON(offence, proof));
    return result;
},
    };
}

RPCMethod setstaking()
{
    return RPCMethod{
        "setstaking",
        "Turns the making of blocks of stake by this node's validator keys (-validatorkeyfile) on or off. It is on when the node starts.",
        {
            {"enabled", RPCArg::Type::BOOL, RPCArg::Optional::NO, "Whether to make blocks at the slots the keys lead"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::BOOL, "enabled", "Whether the node makes blocks of stake now"},
        }},
        RPCExamples{HelpExampleCli("setstaking", "false")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    pos::FastBlocksHooks& hooks{EnsureHooks()};
    pos::Producer* producer{hooks.GetProducer()};
    if (!producer) throw JSONRPCError(RPC_MISC_ERROR, "This node has no validator keys (-validatorkeyfile)");
    producer->SetEnabled(request.params[0].get_bool());
    LogInfo("FastBlocks: making blocks of stake turned %s", producer->Enabled() ? "on" : "off");
    UniValue result(UniValue::VOBJ);
    result.pushKV("enabled", producer->Enabled());
    return result;
},
    };
}

RPCMethod getstakinginfo()
{
    return RPCMethod{
        "getstakinginfo",
        "Returns what this node does as a validator of FastBlocks: its keys, whether it makes blocks of stake, what it made, and its next slot.",
        {
            {"since", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "A time (" + UNIX_EPOCH_TIME + "): count the blocks its keys made from then on as well (blocksmadesince)"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::BOOL, "validator", "Whether the node has validator keys (-validatorkeyfile)"},
            {RPCResult::Type::BOOL, "enabled", "Whether it makes blocks of stake (setstaking)"},
            {RPCResult::Type::STR, "producererror", "Why the block producer could not start, if it could not"},
            {RPCResult::Type::STR, "payout", /*optional=*/true, "Where the producer's share of fees goes (before the fee split: from it on, a block of stake pays its producer nothing, and the validators' shares of the pool go to their bonds' owners): -validatorpayout, else an address of the node's wallet, else the first validator key's (P2WPKH)"},
            {RPCResult::Type::ARR, "keys", "Its validator keys",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR_HEX, "pubkey", "x-only key"},
                    {RPCResult::Type::BOOL, "inset", "Whether it is in the validator set"},
                    {RPCResult::Type::NUM, "stake", "Its stake (0 if not in the set)"},
                    {RPCResult::Type::STR_AMOUNT, "poolshare", "Its share by stake of the current epoch's validator pool so far (fee split; paid to its bond's owner)"},
                    {RPCResult::Type::OBJ, "bond", /*optional=*/true, "Its bond, in the state of the active chain", BondFields()},
                }},
            }},
            {RPCResult::Type::NUM, "stake", "The stake of its keys in the set"},
            {RPCResult::Type::NUM, "totalstake", "The stake of the whole set"},
            {RPCResult::Type::STR_AMOUNT, "poolshare", "The share of its keys of the current epoch's validator pool so far"},
            FeeSplitFields()[0],
            {RPCResult::Type::NUM, "produced", "Blocks made since the node started"},
            {RPCResult::Type::NUM, "blocksmade", "Blocks of stake on the active chain that its keys signed"},
            {RPCResult::Type::NUM, "blocksmadesince", /*optional=*/true, "Of which with a time at or after since"},
            {RPCResult::Type::NUM, "acknowledgements", "Of which acknowledgements"},
            {RPCResult::Type::NUM, "failed", "Slots its keys led and it made no block for"},
            {RPCResult::Type::NUM, "lastslot", "Slot of the last block made, -1 if none"},
            {RPCResult::Type::NUM, "lastsignedslot", "The highest slot its keys signed, by the slashing protection, -1 if none"},
            {RPCResult::Type::STR, "error", "Why the last block could not be made, if it could not"},
            {RPCResult::Type::NUM, "slot", "The current slot"},
            {RPCResult::Type::NUM, "nextslot", /*optional=*/true, "The next slot one of its keys leads (within two epochs)"},
            {RPCResult::Type::NUM_TIME, "nexttime", /*optional=*/true, "When it starts, " + UNIX_EPOCH_TIME},
        }},
        RPCExamples{HelpExampleCli("getstakinginfo", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    pos::FastBlocksHooks& hooks{EnsureHooks()};
    const Consensus::PosParams& params{hooks.Params()};
    const pos::Producer* producer{hooks.GetProducer()};
    const int64_t slot{NowSlot(params)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("validator", producer != nullptr);
    result.pushKV("enabled", producer && producer->Enabled());
    result.pushKV("producererror", hooks.ProducerError());
    if (producer) {
        CTxDestination payout;
        result.pushKV("payout", ExtractDestination(producer->Payout(), payout) ? EncodeDestination(payout) : HexStr(producer->Payout()));
    }
    const auto keys{LocalKeys(hooks)};
    const EpochSchedule schedule{ScheduleOf(hooks, chainman, pos::EpochOf(slot, params))};
    UniValue entries(UniValue::VARR);
    int64_t stake{0};
    CAmount pool_share{0};
    for (const uint256& key : keys) {
        const auto index{schedule.set ? schedule.set->Find(key) : std::nullopt};
        const int64_t key_stake{index ? schedule.set->Validators()[*index].stake : 0};
        stake += key_stake;
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("pubkey", HexStr(key));
        entry.pushKV("inset", index.has_value());
        entry.pushKV("stake", key_stake);
        const CAmount key_share{PoolShareOf(chainman, schedule, pos::EpochOf(slot, params), key_stake)};
        pool_share += key_share;
        entry.pushKV("poolshare", ValueFromAmount(key_share));
        const auto record{WITH_LOCK(::cs_main, return pos::GetValidator(chainman.ActiveChainstate().SideState().View(), key))};
        if (record) entry.pushKV("bond", BondToJSON(*record, params));
        entries.push_back(std::move(entry));
    }
    result.pushKV("keys", std::move(entries));
    result.pushKV("stake", stake);
    result.pushKV("totalstake", schedule.set ? schedule.set->TotalStake() : 0);
    result.pushKV("poolshare", ValueFromAmount(pool_share));
    result.pushKV("feesplit", FeeSplitToJSON(hooks, chainman, slot));
    const pos::Producer::Status status{producer ? producer->GetStatus() : pos::Producer::Status{}};
    result.pushKV("produced", status.produced);
    const auto made{BlocksMade(chainman, keys)};
    result.pushKV("blocksmade", static_cast<uint64_t>(made.size()));
    if (!request.params[0].isNull()) {
        const int64_t since{request.params[0].getInt<int64_t>()};
        result.pushKV("blocksmadesince", static_cast<uint64_t>(std::count_if(made.begin(), made.end(), [&](const auto& block) { return block.second >= since; })));
    }
    result.pushKV("acknowledgements", status.acknowledgements);
    result.pushKV("failed", status.failed);
    result.pushKV("lastslot", status.last_slot);
    result.pushKV("lastsignedslot", producer ? producer->LastSignedSlot() : -1);
    result.pushKV("error", status.error);
    result.pushKV("slot", slot);
    if (const auto next{NextSlotLed(hooks, chainman, keys, slot + 1)}) {
        result.pushKV("nextslot", *next);
        result.pushKV("nexttime", pos::SlotTime(*next, params));
    }
    return result;
},
    };
}

RPCMethod signbondpop()
{
    return RPCMethod{
        "signbondpop",
        "Signs, with a validator key of this node's block producer (-validatorkeyfile), the proof of possession that a bond to it owned by\n"
        "an address needs (createbond on the owner's wallet): no one can bond a key that is not theirs.",
        {
            {"owner", RPCArg::Type::STR, RPCArg::Optional::NO, "The P2WPKH address that owns the bond"},
            {"pubkey", RPCArg::Type::STR_HEX, RPCArg::DefaultHint{"this node's first validator key"}, "The validator's x-only key"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "pubkey", "The validator's x-only key"},
            {RPCResult::Type::STR_HEX, "pop", "The proof of possession, for createbond"},
        }},
        RPCExamples{HelpExampleCli("signbondpop", "\"<address>\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const pos::FastBlocksHooks& hooks{EnsureHooks()};
    const pos::Producer* producer{hooks.GetProducer()};
    if (!producer) throw JSONRPCError(RPC_MISC_ERROR, "This node has no validator keys (-validatorkeyfile)");
    const CTxDestination dest{DecodeDestination(request.params[0].get_str())};
    const WitnessV0KeyHash* owner{std::get_if<WitnessV0KeyHash>(&dest)};
    if (!owner) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "The owner must be a P2WPKH address of this chain");
    const uint256 pubkey{request.params[1].isNull() ? producer->Keys().at(0) : ParsePubkey(request.params[1])};
    const auto pop{producer->SignBondPop(pubkey, uint160{*owner})};
    if (!pop) throw JSONRPCError(RPC_INVALID_PARAMETER, "This node does not hold that validator key");
    UniValue result(UniValue::VOBJ);
    result.pushKV("pubkey", HexStr(pubkey));
    result.pushKV("pop", HexStr(*pop));
    return result;
},
    };
}

} // namespace

std::vector<RPCResult> PosBlockFields()
{
    return {
        {RPCResult::Type::STR, "kind", /*optional=*/true, "FastBlocks: \"pos\" for a block of stake, \"bmm\" for a merged mined block"},
        {RPCResult::Type::NUM, "slot", /*optional=*/true, "FastBlocks: the slot of a block of stake"},
        {RPCResult::Type::STR_HEX, "proposer", /*optional=*/true, "FastBlocks: the x-only key of the leader that signed a block of stake"},
        {RPCResult::Type::NUM, "mainheight", /*optional=*/true, "FastBlocks: the height of the mainchain block that committed to a merged mined block (on record)"},
        {RPCResult::Type::OBJ, "ack", /*optional=*/true, "FastBlocks: the anchor a block of stake acknowledges",
        {
            {RPCResult::Type::STR_HEX, "ticket", "h*, what the mainchain committed to"},
            {RPCResult::Type::STR_HEX, "anchored", "The block anchored"},
            {RPCResult::Type::STR_HEX, "payee", "The script the pot is paid to"},
            {RPCResult::Type::NUM, "mainheight", "Mainchain height of the commitment"},
        }},
        {RPCResult::Type::ARR, "evidence", /*optional=*/true, "FastBlocks: the proofs that leaders signed twice a block of stake carries",
        {
            {RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::NUM, "slot", "The slot signed twice"},
                {RPCResult::Type::ARR, "hashes", "The two blocks", {{RPCResult::Type::STR_HEX, "", ""}}},
            }},
        }},
        {RPCResult::Type::OBJ, "finality", /*optional=*/true, "FastBlocks: how final the block is (on the active chain only)",
        {
            {RPCResult::Type::STR, "status", "\"soft\", \"anchored\" or \"final\""},
            {RPCResult::Type::NUM, "anchors", "Anchors that cover it (counted up to finalanchors)"},
        }},
    };
}

void PushPosBlockInfo(const ChainstateManager& chainman, const CBlock& block, const CBlockIndex& index, UniValue& result)
{
    const pos::FastBlocksHooks* hooks{pos::InstalledHooks()};
    if (!hooks) return;
    const Consensus::PosParams& params{hooks->Params()};
    const bool is_pos{pos::IsPosHeader(block)};
    result.pushKV("kind", is_pos ? "pos" : "bmm");
    if (is_pos) {
        const int64_t slot{pos::SlotOf(block.nTime, params)};
        result.pushKV("slot", slot);
        if (index.pprev) {
            const auto leader{WITH_LOCK(::cs_main, return hooks->LeaderOf(*index.pprev, slot))};
            if (leader.status == pos::FastBlocksHooks::LeaderStatus::OK) result.pushKV("proposer", HexStr(leader.pubkey));
        }
        pos::PosSections evidence;
        if (pos::PosSections::Decode(block.vSections, evidence) && !evidence.evidence.empty()) {
            UniValue proofs(UniValue::VARR);
            for (const pos::DoubleProposal& proof : evidence.evidence) {
                UniValue entry(UniValue::VOBJ);
                entry.pushKV("slot", pos::SlotOf(proof.a.nTime, params));
                UniValue hashes(UniValue::VARR);
                hashes.push_back(proof.a.GetHash().GetHex());
                hashes.push_back(proof.b.GetHash().GetHex());
                entry.pushKV("hashes", std::move(hashes));
                proofs.push_back(std::move(entry));
            }
            result.pushKV("evidence", std::move(proofs));
        }
        pos::PosSections sections;
        if (pos::PosSections::Decode(block.vSections, sections) && sections.ack) {
            UniValue ack(UniValue::VOBJ);
            ack.pushKV("ticket", sections.ack->ticket.Hash().GetHex());
            ack.pushKV("anchored", sections.ack->ticket.anchored.GetHex());
            ack.pushKV("payee", HexStr(sections.ack->ticket.payee));
            ack.pushKV("mainheight", sections.ack->main_height);
            result.pushKV("ack", std::move(ack));
        }
    } else if (chainman.m_mainchain && index.nHeight > 0) {
        if (const auto main_height{chainman.m_mainchain->CommittedHeight(index.GetBlockHash())}) result.pushKV("mainheight", *main_height);
    }
    LOCK(::cs_main);
    const CChain& chain{chainman.ActiveChain()};
    if (!chain.Contains(index)) return;
    const pos::Finality finality{pos::FinalityOf(*hooks, index, *chain.Tip(), params.final_anchors)};
    UniValue entry(UniValue::VOBJ);
    entry.pushKV("status", pos::FinalityName(finality.status));
    entry.pushKV("anchors", finality.anchors);
    result.pushKV("finality", std::move(entry));
}

void PushPosChainInfo(const ChainstateManager& chainman, UniValue& result)
{
    const pos::FastBlocksHooks* hooks{pos::InstalledHooks()};
    if (!hooks) return;
    LOCK(::cs_main);
    const int final_anchors{hooks->Params().final_anchors};
    const CBlockIndex& tip{*chainman.ActiveChain().Tip()};
    const auto heights{pos::AnchoredHeights(*hooks, tip, std::max(final_anchors, 1))};
    if (!heights.empty()) result.pushKV("anchored_tip", tip.GetAncestor(heights.front())->GetBlockHash().GetHex());
    if (final_anchors > 0 && static_cast<int>(heights.size()) >= final_anchors) result.pushKV("final_tip", tip.GetAncestor(heights[final_anchors - 1])->GetBlockHash().GetHex());
}

std::vector<RPCResult> PegBalanceFields()
{
    return {
        {RPCResult::Type::OBJ, "pegbalance", /*optional=*/true, "FastBlocks: the coins the chain holds against the mainchain's escrow, as of the tip", {
            {RPCResult::Type::STR_HEX, "bestblock", "The block it is as of"},
            {RPCResult::Type::NUM, "mainheight", "The mainchain block the chain has acted on, up to"},
            {RPCResult::Type::STR_AMOUNT, "utxos", "Spendable coins (the UTXO set)"},
            {RPCResult::Type::STR_AMOUNT, "withdrawals", "Coins burned by withdrawals waiting or in a bundle (amounts and mainchain fees)"},
            {RPCResult::Type::STR_AMOUNT, "pot", "The fee pot: accrued less paid"},
            {RPCResult::Type::STR_AMOUNT, "pool", "The validator pool: the validators' share of fees, not shared out yet (fee split)"},
            {RPCResult::Type::STR_AMOUNT, "bonded", "Coins bonded by validators"},
            {RPCResult::Type::STR_AMOUNT, "unbonding", "Coins unbonded, not paid yet"},
            {RPCResult::Type::STR_AMOUNT, "reserve", "Penalties not paid out yet (with the pot, at anchors)"},
            {RPCResult::Type::STR_AMOUNT, "rewards", "Rewards for proofs, owed to the blocks that carried them"},
            {RPCResult::Type::STR_AMOUNT, "payouts", "Payouts owed and not paid yet (deposits, refunds, the pot, unbonds, rewards, what pools and auctions pay)"},
            {RPCResult::Type::STR_AMOUNT, "assets", "Flush: " + CURRENCY_UNIT + " the pools and the auctions not collected hold"},
            {RPCResult::Type::STR_AMOUNT, "total", "Their sum: the escrow after the block mainheight, less fees left unclaimed and coins burned"},
        }},
    };
}

void PushPegBalance(ChainstateManager& chainman, UniValue& result)
{
    if (!pos::InstalledHooks()) return;
    CAmount utxos{0}, withdrawals{0}, pot{0}, pool{0}, payouts{0}, assets{0};
    pos::StakeState stake;
    int main_height;
    uint256 tip;
    {
        // The coins and the sidechain state as of one tip.
        LOCK(::cs_main);
        Chainstate& chainstate{chainman.ActiveChainstate()};
        chainstate.ForceFlushStateToDisk(/*wipe_cache=*/false);
        tip = chainstate.m_chain.Tip()->GetBlockHash();
        const auto stats{kernel::ComputeUTXOStats(kernel::CoinStatsHashType::NONE, chainstate.CoinsDB(), chainman.m_blockman)};
        if (!stats || !stats->total_amount) throw JSONRPCError(RPC_INTERNAL_ERROR, "Unable to read the UTXO set");
        utxos = *stats->total_amount;
        const sidechain::State side{chainstate.SideState()};
        side.ForEachWithdrawal([&](const sidechain::Withdrawal& withdrawal) {
            withdrawals += withdrawal.Burned();
            return true;
        });
        for (const CTxOut& out : side.Queue()) payouts += out.nValue;
        for (const CTxOut& out : side.TxQueue()) payouts += out.nValue;
        const pos::FastState fast{pos::GetFastState(chainstate.SideState().View())};
        pot = fast.pot_accrued - fast.pot_paid;
        pool = pos::PoolTotal(chainstate.SideState().View());
        stake = pos::GetStakeState(chainstate.SideState().View());
        main_height = side.MainHeight();
        assets = side.BitAssets().HeldChn();
    }
    UniValue balance(UniValue::VOBJ);
    balance.pushKV("bestblock", tip.GetHex());
    balance.pushKV("mainheight", main_height);
    balance.pushKV("utxos", ValueFromAmount(utxos));
    balance.pushKV("withdrawals", ValueFromAmount(withdrawals));
    balance.pushKV("pot", ValueFromAmount(pot));
    balance.pushKV("pool", ValueFromAmount(pool));
    balance.pushKV("bonded", ValueFromAmount(stake.bonded));
    balance.pushKV("unbonding", ValueFromAmount(stake.unbonding));
    balance.pushKV("reserve", ValueFromAmount(stake.pot_reserve));
    balance.pushKV("rewards", ValueFromAmount(stake.rewards_owed));
    balance.pushKV("payouts", ValueFromAmount(payouts));
    balance.pushKV("assets", ValueFromAmount(assets));
    balance.pushKV("total", ValueFromAmount(utxos + withdrawals + pot + pool + stake.bonded + stake.unbonding + stake.pot_reserve + stake.rewards_owed + payouts + assets));
    result.pushKV("pegbalance", std::move(balance));
}

RPCMethod checkpegbalance()
{
    return RPCMethod{
        "checkpegbalance",
        "FastBlocks: compare the coins the chain holds against the mainchain's escrow (getsidechainstate's pegbalance) with what the\n"
        "mainchain held in escrow for this sidechain's slot after the mainchain block the chain acted on, up to (asked of the mainchain\n"
        "node). The chain holding more than the escrow (\"deficit\") means coins it cannot pay back: a withdrawal bundle of another chain\n"
        "of the slot was paid (an earlier test network, say), or a fault. The escrow holding more is normal (fees left unclaimed, coins\n"
        "burned). Run it after a restart of the chain (audit 7, finding 3).",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR, "status", "\"ok\" (the escrow covers the chain's coins) or \"deficit\""},
            {RPCResult::Type::NUM, "mainheight", "The mainchain block the chain has acted on, up to"},
            {RPCResult::Type::STR_AMOUNT, "pegbalance", "The coins the chain holds against the escrow (getsidechainstate's pegbalance.total)"},
            {RPCResult::Type::STR_AMOUNT, "escrow", "What the mainchain held in escrow for the slot after the block mainheight"},
            {RPCResult::Type::STR_AMOUNT, "difference", "escrow less pegbalance: negative for a deficit"},
        }},
        RPCExamples{HelpExampleCli("checkpegbalance", "") + HelpExampleRpc("checkpegbalance", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    if (!pos::InstalledHooks()) throw JSONRPCError(RPC_MISC_ERROR, "This chain is not FastBlocks");
    NodeContext& node{EnsureAnyNodeContext(request.context)};
    ChainstateManager& chainman{EnsureChainman(node)};
    if (!node.follower) throw JSONRPCError(RPC_MISC_ERROR, "This node does not follow a mainchain node");
    UniValue state(UniValue::VOBJ);
    PushPegBalance(chainman, state);
    const UniValue& balance{state.find_value("pegbalance")};
    const CAmount total{AmountFromValue(balance.find_value("total"))};
    const int main_height{balance.find_value("mainheight").getInt<int>()};
    const uint64_t slot{chainman.GetConsensus().sidechain.slot};
    // The escrow after the block main_height: the total after the last escrow change at or below it
    // (listsidechaindeposits, oldest first, by pages; heights from confirmations, as of one tip).
    std::optional<CAmount> escrow;
    const sidechain::MainClient& client{node.follower->Client()};
    if (client.EnforcerOnly()) {
        // Through the enforcer's interface alone (-mainchaingrpc) the escrow is known at the mainchain tip only
        // (GetCtip: the treasury output's value), not after an earlier block.
        try {
            for (int attempt{0}; attempt < 3 && !escrow; ++attempt) {
                const int tip_before{client.Call("getblockcount", UniValue{UniValue::VARR}).getInt<int>()};
                if (tip_before != main_height) {
                    throw JSONRPCError(RPC_MISC_ERROR, strprintf("Through the enforcer's interface the escrow is known at the mainchain tip only (height %d); "
                                                                 "the chain has acted on the mainchain up to height %d: ask again once it acted on the tip", tip_before, main_height));
                }
                UniValue request(UniValue::VOBJ);
                request.pushKV("sidechainNumber", slot);
                const UniValue reply{client.CallEnforcer("cusf.mainchain.v1.ValidatorService/GetCtip", request)};
                const UniValue& ctip{reply.find_value("ctip")};
                const UniValue& value{ctip.isObject() ? ctip.find_value("value") : NullUniValue};
                CAmount found{0};
                if (value.isStr()) {
                    found = ToIntegral<int64_t>(value.get_str()).value_or(-1);
                } else if (value.isNum()) {
                    found = value.getInt<int64_t>();
                }
                if (found < 0 || !MoneyRange(found)) throw JSONRPCError(RPC_MISC_ERROR, "The enforcer sent a treasury value that is not an amount");
                if (client.Call("getblockcount", UniValue{UniValue::VARR}).getInt<int>() == tip_before) escrow = found;
            }
        } catch (const sidechain::MainClientError& e) {
            throw JSONRPCError(RPC_MISC_ERROR, strprintf("The mainchain node did not answer: %s", e.what()));
        } catch (const std::runtime_error& e) {
            throw JSONRPCError(RPC_MISC_ERROR, strprintf("The mainchain node did not answer: %s", e.what()));
        }
    }
    try {
        for (int attempt{0}; attempt < 3 && !escrow && !client.EnforcerOnly(); ++attempt) {
            const int tip_before{node.follower->Client().Call("getblockcount", UniValue{UniValue::VARR}).getInt<int>()};
            CAmount found{0};
            std::optional<std::string> after;
            while (true) {
                UniValue params(UniValue::VARR);
                params.push_back(slot);
                if (after) params.push_back(*after);
                const UniValue changes{node.follower->Client().Call("listsidechaindeposits", params)};
                for (const UniValue& change : changes.getValues()) {
                    const int height{tip_before - change.find_value("confirmations").getInt<int>() + 1};
                    if (height <= main_height) found = AmountFromValue(change.find_value("total"));
                }
                if (changes.size() < 1000) break;
                after = changes.getValues().back().find_value("txid").get_str();
            }
            const int tip_after{node.follower->Client().Call("getblockcount", UniValue{UniValue::VARR}).getInt<int>()};
            if (tip_after == tip_before) escrow = found;
        }
    } catch (const sidechain::MainClientError& e) {
        throw JSONRPCError(RPC_MISC_ERROR, strprintf("The mainchain node did not answer: %s", e.what()));
    }
    if (!escrow) throw JSONRPCError(RPC_MISC_ERROR, "The mainchain tip kept moving: try again");
    UniValue result(UniValue::VOBJ);
    result.pushKV("status", *escrow >= total ? "ok" : "deficit");
    result.pushKV("mainheight", main_height);
    result.pushKV("pegbalance", ValueFromAmount(total));
    result.pushKV("escrow", ValueFromAmount(*escrow));
    result.pushKV("difference", ValueFromAmount(*escrow - total));
    return result;
},
    };
}

void RegisterPosRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"sidechain", &getposinfo},
        {"sidechain", &getvalidators},
        {"sidechain", &getvalidator},
        {"sidechain", &getleaderschedule},
        {"sidechain", &getanchorinfo},
        {"sidechain", &getanchorticket},
        {"sidechain", &getanchorbidpolicy},
        {"sidechain", &setanchorbidpolicy},
        {"sidechain", &submitevidence},
        {"sidechain", &listevidence},
        {"sidechain", &setstaking},
        {"sidechain", &getstakinginfo},
        {"sidechain", &signbondpop},
        {"sidechain", &checkpegbalance},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
