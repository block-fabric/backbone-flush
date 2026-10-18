// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <chain.h>
#include <consensus/amount.h>
#include <core_io.h>
#include <kernel/chainparams.h>
#include <kernel/coinstats.h>
#include <key_io.h>
#include <node/context.h>
#include <pos/evidence.h>
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
                }},
            }},
            {RPCResult::Type::NUM, "anchors", "Anchors on the active chain (acknowledged tickets and BMM blocks)"},
            {RPCResult::Type::NUM, "lastanchormainheight", "Mainchain height of the commitment of the last anchor, -1 if none"},
            {RPCResult::Type::STR_HEX, "lastanchored", /*optional=*/true, "The block the last anchor anchors"},
            {RPCResult::Type::NUM, "lastanchoredheight", "Its height, -1 if none"},
            {RPCResult::Type::STR_HEX, "anchorpoint", /*optional=*/true, "The last block that took the peg actions: the next ticket anchors it or a block after it"},
            {RPCResult::Type::NUM, "lastposslot", "Slot of the last block of stake on the active chain, -1 if none"},
            {RPCResult::Type::NUM, "firstunanchoredslot", "Slot of the first block of stake no anchor covers, -1 if none"},
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
    const bool active{schedule.set && schedule.set->Active()};
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
    const auto main_tip{chainman.m_mainchain ? chainman.m_mainchain->GetBlock(chainman.m_mainchain->Height()) : std::nullopt};
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
    result.pushKV("active", schedule.set && schedule.set->Active());
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
    return result;
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
            {RPCResult::Type::STR, "payout", /*optional=*/true, "Where the producer's share of fees goes: -validatorpayout, else an address of the node's wallet, else the first validator key's (P2WPKH)"},
            {RPCResult::Type::ARR, "keys", "Its validator keys",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR_HEX, "pubkey", "x-only key"},
                    {RPCResult::Type::BOOL, "inset", "Whether it is in the validator set"},
                    {RPCResult::Type::NUM, "stake", "Its stake (0 if not in the set)"},
                    {RPCResult::Type::OBJ, "bond", /*optional=*/true, "Its bond, in the state of the active chain", BondFields()},
                }},
            }},
            {RPCResult::Type::NUM, "stake", "The stake of its keys in the set"},
            {RPCResult::Type::NUM, "totalstake", "The stake of the whole set"},
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
    for (const uint256& key : keys) {
        const auto index{schedule.set ? schedule.set->Find(key) : std::nullopt};
        const int64_t key_stake{index ? schedule.set->Validators()[*index].stake : 0};
        stake += key_stake;
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("pubkey", HexStr(key));
        entry.pushKV("inset", index.has_value());
        entry.pushKV("stake", key_stake);
        const auto record{WITH_LOCK(::cs_main, return pos::GetValidator(chainman.ActiveChainstate().SideState().View(), key))};
        if (record) entry.pushKV("bond", BondToJSON(*record, params));
        entries.push_back(std::move(entry));
    }
    result.pushKV("keys", std::move(entries));
    result.pushKV("stake", stake);
    result.pushKV("totalstake", schedule.set ? schedule.set->TotalStake() : 0);
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
            {RPCResult::Type::STR_AMOUNT, "bonded", "Coins bonded by validators"},
            {RPCResult::Type::STR_AMOUNT, "unbonding", "Coins unbonded, not paid yet"},
            {RPCResult::Type::STR_AMOUNT, "reserve", "Penalties not paid out yet (with the pot, at anchors)"},
            {RPCResult::Type::STR_AMOUNT, "rewards", "Rewards for proofs, owed to the blocks that carried them"},
            {RPCResult::Type::STR_AMOUNT, "payouts", "Payouts owed and not paid yet (deposits, refunds, the pot, unbonds, rewards, what pools and auctions pay)"},
            {RPCResult::Type::STR_AMOUNT, "assets", "Layers: CHN the pools and the auctions not collected hold"},
            {RPCResult::Type::STR_AMOUNT, "total", "Their sum: the escrow after the block mainheight, less fees left unclaimed and coins burned"},
        }},
    };
}

void PushPegBalance(ChainstateManager& chainman, UniValue& result)
{
    if (!pos::InstalledHooks()) return;
    CAmount utxos{0}, withdrawals{0}, pot{0}, payouts{0}, assets{0};
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
    balance.pushKV("bonded", ValueFromAmount(stake.bonded));
    balance.pushKV("unbonding", ValueFromAmount(stake.unbonding));
    balance.pushKV("reserve", ValueFromAmount(stake.pot_reserve));
    balance.pushKV("rewards", ValueFromAmount(stake.rewards_owed));
    balance.pushKV("payouts", ValueFromAmount(payouts));
    balance.pushKV("assets", ValueFromAmount(assets));
    balance.pushKV("total", ValueFromAmount(utxos + withdrawals + pot + stake.bonded + stake.unbonding + stake.pot_reserve + stake.rewards_owed + payouts + assets));
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
    try {
        for (int attempt{0}; attempt < 3 && !escrow; ++attempt) {
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
