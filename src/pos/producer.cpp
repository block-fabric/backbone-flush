// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/producer.h>

#include <pos/feesplit.h>

#include <addresstype.h>
#include <chain.h>
#include <common/args.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <interfaces/wallet.h>
#include <kernel/chainparams.h>
#include <key_io.h>
#include <logging.h>
#include <node/context.h>
#include <node/miner.h>
#include <outputtype.h>
#include <pos/hooks.h>
#include <pos/staking.h>
#include <pubkey.h>
#include <random.h>
#include <tinyformat.h>
#include <txmempool.h>
#include <util/fs.h>
#include <util/result.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/thread.h>
#include <util/time.h>
#include <validation.h>

#include <cstdlib>
#include <fstream>

#ifndef WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pos {

namespace {
//! Tickets (and offences) the producer leaves out of its blocks, at most.
constexpr size_t MAX_REFUSED{1000};
//! A slot that failed before the signature is tried again after this long (Producer::RetrySeconds).
constexpr auto RETRY_INTERVAL{std::chrono::milliseconds{500}};

uint256 XOnly(const CKey& key)
{
    const XOnlyPubKey xonly{key.GetPubKey()};
    return uint256{std::span<const unsigned char>{xonly.begin(), xonly.end()}};
}
} // namespace

int64_t Producer::ProducerShare(int64_t fees, int64_t bps)
{
    // fees * bps would overflow for large fees: by parts.
    return fees / 10000 * bps + fees % 10000 * bps / 10000;
}

bool Producer::Behind(ChainstateManager& chainman)
{
    AssertLockHeld(::cs_main);
    // (Not initial block download as such: a chain stalled for a day, restarted, would never start again.)
    const Chainstate& chainstate{chainman.ActiveChainstate()};
    const CBlockIndex* tip{chainstate.m_chain.Tip()};
    if (!tip) return true;
    // A block of more work that the node has, with all below it (a candidate for the tip): it is
    // connecting it. Not merely a header of more work.
    for (auto it{chainstate.setBlockIndexCandidates.rbegin()}; it != chainstate.setBlockIndexCandidates.rend(); ++it) {
        const CBlockIndex* candidate{*it};
        if (candidate->nChainWork <= tip->nChainWork) break;
        if (!(candidate->nStatus & BLOCK_FAILED_VALID)) return true;
    }
    return false;
}

namespace {
//! The first wallet the node has loaded, if any.
std::unique_ptr<interfaces::Wallet> FirstWallet(node::NodeContext& node)
{
    if (!node.wallet_loader) return nullptr;
    auto wallets{node.wallet_loader->getWallets()};
    if (wallets.empty()) return nullptr;
    return std::move(wallets.front());
}
} // namespace

std::unique_ptr<Producer> Producer::FromArgs(node::NodeContext& node, const FastBlocksHooks& hooks, std::string& error)
{
    const ArgsManager& args{*node.args};
    if (!args.IsArgSet("-validatorkeyfile")) return nullptr;
    const fs::path key_path{args.GetPathArg("-validatorkeyfile")};
#ifndef WIN32
    // Hot keys: as -mainchainrpcpasswordfile, the file must be this user's and readable by no one else. (On Windows its
    // access list is not checked.)
    {
        struct stat st;
        const std::string name{fs::PathToString(key_path)};
        if (::stat(key_path.c_str(), &st) == 0) {
            if (st.st_uid != geteuid()) {
                error = strprintf("the validator key file %s is not owned by this user: make it this user's, readable by its owner only (chown, then chmod 600 %s)", name, name);
                return nullptr;
            }
            if (st.st_mode & (S_IRWXG | S_IRWXO)) {
                error = strprintf("the validator key file %s can be read or written by other users: make it readable by its owner only (chmod 600 %s)", name, name);
                return nullptr;
            }
        }
    }
#endif
    std::ifstream file{key_path.std_path()};
    if (!file.is_open()) {
        error = strprintf("cannot read the validator key file %s", fs::PathToString(key_path));
        return nullptr;
    }
    std::vector<CKey> keys;
    std::string line;
    while (std::getline(file, line)) {
        line = util::TrimString(line);
        if (line.empty() || line[0] == '#') continue;
        const auto secret{TryParseHex<uint8_t>(line)};
        CKey key;
        if (secret && secret->size() == 32) key.Set(secret->begin(), secret->end(), /*fCompressedIn=*/true);
        if (!key.IsValid()) {
            error = strprintf("the validator key file %s has a line that is not a 32-byte secret key in hex", fs::PathToString(key_path));
            return nullptr;
        }
        keys.push_back(key);
    }
    if (keys.empty()) {
        error = strprintf("the validator key file %s has no key", fs::PathToString(key_path));
        return nullptr;
    }
    // (A key leads slots once it is in the validator set of an epoch: from the start, or bonded.)
    for (const CKey& key : keys) LogInfo("FastBlocks: validator key %s", HexStr(XOnly(key)));
    // The producer's share of fees goes to -validatorpayout; else to a new address of the node's
    // wallet, which sees the income and keeps it whatever becomes of the (hot) key file; else, with no
    // wallet loaded, to the first key (P2WPKH).
    CScript payout{GetScriptForDestination(WitnessV0KeyHash{keys[0].GetPubKey()})};
    if (args.IsArgSet("-validatorpayout")) {
        const CTxDestination dest{DecodeDestination(args.GetArg("-validatorpayout", ""))};
        if (!IsValidDestination(dest)) {
            error = "-validatorpayout is not an address of this chain";
            return nullptr;
        }
        payout = GetScriptForDestination(dest);
    } else if (auto wallet{FirstWallet(node)}) {
        const auto dest{wallet->getNewDestination(OutputType::BECH32, "validator payout")};
        if (dest) {
            payout = GetScriptForDestination(*dest);
            LogInfo("FastBlocks: the producer's share of fees goes to %s, of the wallet \"%s\" (-validatorpayout to choose)", EncodeDestination(*dest), wallet->getWalletName());
        } else {
            LogWarning("FastBlocks: the wallet \"%s\" gives no address for the producer's share of fees (%s): it goes to the validator key's address %s",
                       wallet->getWalletName(), util::ErrorString(dest).original, EncodeDestination(WitnessV0KeyHash{keys[0].GetPubKey()}));
        }
    } else {
        LogWarning("FastBlocks: no wallet and no -validatorpayout: the producer's share of fees goes to the validator key's address %s, "
                   "which no wallet sees (doc/fastblocks.md, \"Producer and anchorer\", says how to import it)", EncodeDestination(WitnessV0KeyHash{keys[0].GetPubKey()}));
    }
    // The slashing protection lives next to the key file (named after it), so that wiping the data
    // directory to sync again keeps it; the file of older versions, in the data directory, counts too.
    const std::string key_name{fs::PathToString(key_path.filename())};
    const fs::path key_dir{key_path.parent_path()};
    auto protection{SlashingProtection::Open(key_dir / fs::PathFromString(key_name + ".protection"), key_dir,
                                             fs::PathFromString(key_name + ".lock"), error, std::vector<fs::path>{args.GetDataDirNet() / fs::u8path("validator_protection.dat")},
                                             node.chainman->GetConsensus().hashGenesisBlock)};
    if (!protection) return nullptr;
    LogInfo("FastBlocks: block producer with %u validator key(s)", keys.size());
    auto producer{std::make_unique<Producer>(node, hooks, std::move(keys), std::move(payout), std::move(protection))};
    producer->m_fault_after_sign = args.GetBoolArg("-posfaultaftersign", false) && node.chainman->GetParams().GetChainType() == ChainType::REGTEST;
    return producer;
}

Producer::Producer(node::NodeContext& node, const FastBlocksHooks& hooks, std::vector<CKey> keys, CScript payout, std::unique_ptr<SlashingProtection> protection)
    : m_node{node}, m_hooks{hooks}, m_keys{std::move(keys)}, m_payout{std::move(payout)}, m_protection{std::move(protection)}
{
}

Producer::~Producer() { Stop(); }

void Producer::SetExtraCheckForTesting(std::function<std::optional<std::string>(const CBlock&)> check)
{
    LOCK(m_mutex);
    m_extra_check = std::move(check);
}

std::set<uint256> Producer::RefusedTickets() const
{
    LOCK(m_mutex);
    return m_refused_tickets;
}

std::set<Offence> Producer::RefusedOffences() const
{
    LOCK(m_mutex);
    return m_refused_offences;
}

void Producer::Start()
{
    LOCK(m_mutex);
    if (m_thread.joinable()) return;
    m_stop = false;
    m_thread = std::thread{[this] { util::TraceThread("fbproducer", [this] { Run(); }); }};
}

void Producer::Stop()
{
    std::thread thread;
    {
        LOCK(m_mutex);
        m_stop = true;
        thread = std::move(m_thread);
    }
    m_wake.notify_all();
    if (thread.joinable()) thread.join();
}

Producer::Status Producer::GetStatus() const
{
    LOCK(m_mutex);
    return m_status;
}

int64_t Producer::LastSignedSlot() const
{
    int64_t last{-1};
    for (const CKey& key : m_keys) {
        if (const auto signed_slot{m_protection->LastSigned(XOnly(key))}) last = std::max(last, *signed_slot);
    }
    return last;
}

std::vector<uint256> Producer::Keys() const
{
    std::vector<uint256> keys;
    for (const CKey& key : m_keys) keys.push_back(XOnly(key));
    return keys;
}

std::optional<std::array<unsigned char, 64>> Producer::SignBondPop(const uint256& pubkey, const uint160& owner) const
{
    for (const CKey& key : m_keys) {
        if (XOnly(key) != pubkey) continue;
        std::array<unsigned char, 64> pop;
        if (!key.SignSchnorr(BondPopHash(owner, m_node.chainman->GetConsensus().hashGenesisBlock), pop, /*merkle_root=*/nullptr, GetRandHash())) return std::nullopt;
        return pop;
    }
    return std::nullopt;
}

void Producer::Run()
{
    const Consensus::PosParams& params{m_hooks.Params()};
    //! The last slot done with: made, given up, or not this node's.
    int64_t last_done{-1};
    //! A slot whose last try failed before the signature, and when to try it again.
    int64_t retrying{-1};
    Retry retrying_kind{Retry::NO};
    SteadyClock::time_point next_try{};
    while (!m_stop) {
        const int64_t now{TicksSinceEpoch<std::chrono::seconds>(NodeClock::now())};
        const int64_t slot{SlotOf(now, params)};
        // A slot that was being tried again and is over (or given up as the producer was turned off).
        if (retrying >= 0 && (slot != retrying || !m_enabled)) {
            if (retrying_kind == Retry::COUNTED) NoteFailedSlot();
            last_done = std::max(last_done, retrying);
            retrying = -1;
        }
        if (m_enabled && slot > last_done && now >= SlotTime(slot, params) && now > params.genesis_time &&
            (slot != retrying || SteadyClock::now() >= next_try)) {
            std::string error;
            Retry retry{Retry::NO};
            const auto made{Produce(slot, error, &retry)};
            WITH_LOCK(m_mutex, ++m_status.tries);
            // A failure before the slashing protection noted the slot (nothing signed): the slot is
            // tried again, for a while. A block or a late block arriving as the slot starts (the node
            // still taking it in, the tip moving while the block was built) would otherwise cost the
            // whole slot.
            if (!made && retry != Retry::NO && now < SlotTime(slot, params) + RetrySeconds(params)) {
                if (slot != retrying) LogDebug(BCLog::VALIDATION, "FastBlocks: no block for slot %d yet, trying again: %s\n", slot, error);
                retrying = slot;
                retrying_kind = retry;
                next_try = SteadyClock::now() + RETRY_INTERVAL;
            } else {
                last_done = slot;
                retrying = -1;
                if (!made && retry == Retry::COUNTED) NoteFailedSlot();
                if (!made && !error.empty()) LogDebug(BCLog::VALIDATION, "FastBlocks: no block for slot %d: %s\n", slot, error);
            }
        }
        WAIT_LOCK(m_mutex, lock);
        m_wake.wait_for(lock, std::chrono::milliseconds{100}, [this] { return m_stop.load(); });
    }
}

void Producer::NoteFailedSlot()
{
    LOCK(m_mutex);
    ++m_status.failed;
}

std::optional<CBlock> Producer::Build(int64_t slot, const std::optional<AnchorAck>& ack, std::string& error, const std::vector<DoubleProposal>& evidence) const
{
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::PosParams& params{m_hooks.Params()};
    node::BlockCreateOptions options;
    options.coinbase_output_script = m_payout;
    options.test_block_validity = false;
    node::SidechainAssembly assembly;
    PosSections sections;
    sections.evidence = evidence;
    if (ack) {
        // The bundle is P's (design 4.3.4): its withdrawals, its hash as the nonce, its height as the
        // lock time; none if P is below the last anchor point. (P looked up and read with cs_main held: an
        // unchecked header may leave the block index as soon as it is released, EraseUnchecked.)
        std::optional<sidechain::PegStep> step;
        {
            LOCK(::cs_main);
            if (const CBlockIndex* anchored{chainman.m_blockman.LookupBlockIndex(ack->ticket.anchored)}) {
                step = m_hooks.AckPegStep(*ack, *anchored, *Assert(chainman.ActiveChain().Tip()));
            }
        }
        if (!step) {
            error = "the block the ticket anchors is unknown";
            return std::nullopt;
        }
        sections.ack = ack;
        assembly.peg = node::SidechainAssembly::Peg::STEP;
        assembly.step = std::move(*step);
        assembly.sections = sections.Encode();
    } else {
        assembly.peg = node::SidechainAssembly::Peg::NONE;
    }
    std::unique_ptr<node::CBlockTemplate> block_template;
    try {
        block_template = node::BlockAssembler{chainman.ActiveChainstate(), m_node.mempool.get(), options, assembly}.CreateNewBlock();
    } catch (const std::exception& e) {
        error = e.what();
        return std::nullopt;
    }
    CBlock block{block_template->block};

    CMutableTransaction coinbase{*block.vtx.at(0)};
    const CAmount fees{coinbase.vout.at(0).nValue};
    const int height{WITH_LOCK(::cs_main, const CBlockIndex* prev{chainman.m_blockman.LookupBlockIndex(block.hashPrevBlock)}; return prev ? prev->nHeight + 1 : -1)};
    if (height < 0) {
        error = "the block's parent is unknown";
        return std::nullopt;
    }
    if (m_hooks.FeeSplitActive(height)) {
        // The fee split (pos/feesplit.h): the producer keeps nothing; the bidder's share and the fees of
        // peg transactions go to the pot, the validators' share to the validator pool.
        const auto main_height{WITH_LOCK(::cs_main, return m_hooks.FeeSplitMainHeight(/*pos_block=*/true, slot, -1))};
        if (!main_height) {
            error = "the mainchain block that seeds the epoch is not on record yet";
            return std::nullopt;
        }
        const FeeSplit split{SplitFees(block.vtx, block_template->vTxFees, /*fees_without_coinbase=*/true, m_hooks.FeeSplitSpacing(*main_height), m_hooks.FeeCurveAt(height), block_template->side_security_fees)};
        coinbase.vout[0].nValue = 0;
        if (split.Bidder() > 0) coinbase.vout.emplace_back(split.Bidder(), PotScript());
        if (split.validators > 0) coinbase.vout.emplace_back(split.validators, PoolScript());
    } else {
        // Before it: the producer keeps its share, the rest goes to the pot.
        const CAmount share{ProducerShare(fees, params.producer_share_bps)};
        coinbase.vout[0].nValue = share;
        if (fees - share > 0) coinbase.vout.emplace_back(fees - share, PotScript());
    }
    block.vtx[0] = MakeTransactionRef(std::move(coinbase));
    node::RegenerateCommitments(block, chainman);

    block.nVersion |= POS_BLOCK_BIT;
    block.nTime = static_cast<uint32_t>(SlotTime(slot, params));
    block.nBits = PosBits(chainman.GetConsensus().powLimit);
    block.nNonce = ack ? static_cast<uint32_t>(ack->main_height) : 0;
    block.vSections = sections.Encode();
    block.ext.hashSections = block.vSections.empty() ? uint256{} : SectionsHash(block.vSections);
    block.ext.sig = {};
    return block;
}

bool Producer::Sign(CBlock& block, const CKey& key, const uint256& genesis)
{
    // Deterministic: BIP340 with the auxiliary randomness all zero, so that the nonce depends on the
    // key and the message alone (BIP340's own derivation). A block signed twice gets the same
    // signature, and no peer holds a copy with another one. (The auxiliary randomness only guards the
    // nonce against side channels on the signing machine; a validator key signs nothing else.)
    return key.SignSchnorr(BlockSigHash(block, genesis), block.ext.sig, /*merkle_root=*/nullptr, uint256::ZERO);
}

std::optional<uint256> Producer::Produce(int64_t slot, std::string& error, Retry* retry)
{
    LOCK(m_produce_mutex);
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::PosParams& params{m_hooks.Params()};
    if (retry) *retry = Retry::NO;
    // Before the slashing protection notes the slot: nothing is signed, the slot may be tried again
    // (the caller counts it as failed once, when it gives it up).
    bool before_signing{true};
    const auto fail{[&](std::string why) -> std::optional<uint256> {
        error = std::move(why);
        LOCK(m_mutex);
        if (before_signing && retry) {
            *retry = Retry::COUNTED;
        } else {
            ++m_status.failed;
        }
        m_status.error = error;
        return std::nullopt;
    }};
    // Not known to lead the slot (yet): not counted as a failure, but worth trying again.
    const auto unknown{[&](std::string why) -> std::optional<uint256> {
        error = std::move(why);
        if (retry) *retry = Retry::UNCOUNTED;
        return std::nullopt;
    }};

    std::optional<AnchorAck> ack;
    std::vector<DoubleProposal> evidence;
    std::vector<Offence> offences;
    uint256 tip_hash;
    uint256 leader_key;
    const CKey* key{nullptr};
    {
        LOCK(::cs_main);
        const CBlockIndex* tip{chainman.ActiveChain().Tip()};
        // The leader of the slot on the tip: the set of its epoch, which an anchor made, and its seed.
        const FastBlocksHooks::SlotLeader leader{m_hooks.LeaderOf(*tip, slot)};
        switch (leader.status) {
        case FastBlocksHooks::LeaderStatus::INACTIVE: error = "no blocks of stake in this epoch: too few validators"; return std::nullopt;
        case FastBlocksHooks::LeaderStatus::SEED_UNKNOWN: return unknown("the mainchain block that seeds the epoch is not on record yet");
        case FastBlocksHooks::LeaderStatus::SET_UNKNOWN: return unknown("the validator set of the epoch is not known");
        case FastBlocksHooks::LeaderStatus::OK: break;
        }
        leader_key = leader.pubkey;
        for (const CKey& candidate : m_keys) {
            if (XOnly(candidate) == leader_key) key = &candidate;
        }
        if (!key) return std::nullopt;
        // Not while this node is behind: its block would fork the chain. Behind means a better block
        // it has and has yet to connect, not a better header: a leader can sign a header that claims
        // an acknowledgement (more anchors than any chain) and never send its block.
        if (Behind(chainman)) return fail("the node is still taking in the best chain");
        if (const auto last{FastBlocksHooks::LastPosSlot(tip, params)}; last && *last >= slot) return fail("the chain has a block of this slot or a later one");
        if (SlotTime(slot, params) <= tip->GetMedianTimePast()) return fail("the slot is not after the median time of the chain");
        const sidechain::StoreView& view{chainman.ActiveChainstate().SideCache()};
        // A banned leader leads no more (its block would be invalid: bad-pos-leader-banned).
        if (const auto record{GetValidator(view, leader_key)}; record && record->tombstoned) return fail("this validator is banned: it signed two blocks for a slot");
        const FastState fast{GetFastState(view)};
        const auto [refused_tickets, refused_offences]{WITH_LOCK(m_mutex, return std::make_pair(m_refused_tickets, m_refused_offences))};
        ack = m_hooks.NextAck(*tip, fast, slot, &refused_tickets);
        if (!ack && fast.first_unanchored_slot >= 0 && slot - fast.first_unanchored_slot >= params.MaxUnanchoredSlots(slot)) {
            return fail("no anchor for too long: blocks of stake wait for one");
        }
        // The proofs this node knows that the chain has not punished yet, one per validator (a
        // validator is slashed once: a second proof against it in the block would be invalid).
        // (Cheap checks first, and a bounded number of signatures: ChainEvidence.)
        const uint64_t anchors{fast.anchors + (ack ? 1 : 0)};
        for (auto& [offence, proof] : m_hooks.ChainEvidence(*tip, slot, view, anchors, MAX_SECTION_EVIDENCE, refused_offences)) {
            evidence.push_back(std::move(proof));
            offences.push_back(offence);
        }
        tip_hash = tip->GetBlockHash();
    }
    std::string build_error;
    auto block{Build(slot, ack, build_error, evidence)};
    // An acknowledgement the block cannot carry: the block without it.
    if (!block && ack) {
        LogInfo("FastBlocks: slot %d without the acknowledgement of %s: %s", slot, ack->ticket.Hash().ToString(), build_error);
        ack.reset();
        block = Build(slot, ack, build_error, evidence);
    }
    if (!block) return fail(build_error);
    if (block->hashPrevBlock != tip_hash) return fail("the tip moved while the block was built");

    // The slot is noted on disk before the signature exists: from here on it is not tried again.
    before_signing = false;
    std::string protection_error;
    if (!m_protection->MaySign(leader_key, slot, protection_error)) return fail("slashing protection: " + protection_error);
    if (!Sign(*block, *key, chainman.GetConsensus().hashGenesisBlock)) return fail("signing failed");
    if (m_fault_after_sign) {
        // (Tests: a node killed between the signature and the block's relay.)
        LogInfo("FastBlocks: -posfaultaftersign: the block of slot %d is signed; the node stops at once", slot);
        std::_Exit(EXIT_FAILURE);
    }

    // The block is not made again without what made it invalid: the node checked the signed header
    // (and noted it, to find leaders that sign twice), and a second block of the slot would be a
    // second signature, which this node would take for proof against its own key. The slot is lost;
    // the acknowledgement and the proofs it carried are left out of the blocks to come.
    std::optional<std::string> invalid;
    {
        LOCK(::cs_main);
        if (chainman.ActiveChain().Tip()->GetBlockHash() != tip_hash) return fail("the tip moved while the block was built");
        const BlockValidationState state{TestBlockValidity(chainman.ActiveChainstate(), *block, /*check_pow=*/false, /*check_merkle_root=*/true)};
        if (!state.IsValid()) invalid = state.ToString();
    }
    if (const auto extra{WITH_LOCK(m_mutex, return m_extra_check)}; !invalid && extra) invalid = extra(*block);
    if (invalid) {
        if (ack || !evidence.empty()) {
            LogInfo("FastBlocks: slot %d lost; its acknowledgement and proofs are left out of the next blocks: %s", slot, *invalid);
            LOCK(m_mutex);
            if (m_refused_tickets.size() >= MAX_REFUSED) m_refused_tickets.clear();
            if (m_refused_offences.size() >= MAX_REFUSED) m_refused_offences.clear();
            if (ack) m_refused_tickets.insert(ack->ticket.Hash());
            m_refused_offences.insert(offences.begin(), offences.end());
        }
        return fail("the block is not valid: " + *invalid);
    }
    const auto shared{std::make_shared<const CBlock>(*block)};
    bool new_block{false};
    if (!chainman.ProcessNewBlock(shared, /*force_processing=*/true, /*min_pow_checked=*/true, &new_block)) return fail("the node refused the block");
    LogInfo("FastBlocks: block %s of slot %d%s", shared->GetHash().ToString(), slot, ack ? strprintf(", acknowledging the anchor committed at %d", ack->main_height) : "");
    LOCK(m_mutex);
    ++m_status.produced;
    if (ack) ++m_status.acknowledgements;
    m_status.last_slot = slot;
    m_status.error.clear();
    return shared->GetHash();
}

} // namespace pos
