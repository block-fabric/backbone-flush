// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sidechain/follower.h>

#include <chain.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <interfaces/mining.h>
#include <logging.h>
#include <node/context.h>
#include <node/warnings.h>
#include <pow.h>
#include <rpc/util.h>
#include <sidechain/mainchain.h>
#include <util/threadnames.h>
#include <util/translation.h>
#include <validation.h>

namespace sidechain {
namespace {
//! How often the mainchain node is asked for news.
constexpr auto POLL_INTERVAL{1s};
//! Number of mainchain blocks asked for at a time.
constexpr int BATCH{2000};

UniValue Params(std::initializer_list<UniValue> values)
{
    UniValue params(UniValue::VARR);
    for (const auto& value : values) params.push_back(value);
    return params;
}

uint256 Hash256(const UniValue& value)
{
    const auto hash{uint256::FromHex(value.get_str())};
    if (!hash) throw std::runtime_error("the mainchain node sent something that is not a hash");
    return *hash;
}

MainBlock ParseMainBlock(const UniValue& obj)
{
    MainBlock block;
    block.hash = Hash256(obj["hash"]);
    if (obj.exists("previousblockhash")) block.prev_hash = Hash256(obj["previousblockhash"]);
    block.time = obj["time"].getInt<int64_t>();
    if (obj.exists("bmm")) block.bmm = Hash256(obj["bmm"]);
    for (const UniValue& entry : obj["deposits"].getValues()) {
        MainDeposit deposit;
        deposit.destination = entry["destination"].get_str();
        deposit.amount = AmountFromValue(entry["amount"]);
        deposit.txid = Hash256(entry["txid"]);
        deposit.burn_index = entry["burnindex"].getInt<uint32_t>();
        block.deposits.push_back(std::move(deposit));
    }
    for (const UniValue& entry : obj["bundles"].getValues()) {
        block.bundles.push_back({Hash256(entry["hash"]), entry["paid"].get_bool()});
    }
    return block;
}
} // namespace

Follower::Follower(node::NodeContext& node, MainClient::Options options) : m_node{node}, m_client{std::move(options)} {}

Follower::~Follower() { Stop(); }

void Follower::Start()
{
    LOCK(m_mutex);
    m_stop = false;
    m_thread = std::thread{[this] {
        util::ThreadRename("mainchain");
        Run();
    }};
}

void Follower::Stop()
{
    std::thread thread;
    {
        LOCK(m_mutex);
        m_stop = true;
        thread.swap(m_thread);
    }
    m_wake.notify_all();
    if (thread.joinable()) thread.join();
}

Follower::Status Follower::GetStatus() const
{
    LOCK(m_mutex);
    return m_status;
}

bool Follower::UpdateRecord()
{
    Mainchain& record{*Assert(m_node.chainman->m_mainchain)};
    const uint32_t slot{m_node.chainman->GetConsensus().sidechain.slot};
    const auto fetch{[&](int height, int count) {
        return m_client.Call("getsidechainevents", Params({static_cast<uint64_t>(slot), height, count}));
    }};

    bool changed{false};
    while (!m_stop) {
        const int height{record.Height()};
        // The block on record is asked for again, to learn whether the mainchain still has it.
        const UniValue events{fetch(std::max(height, 0), BATCH)};
        size_t first_new{0};
        if (height >= 0) {
            if (events.empty() || Hash256(events[0]["hash"]) != record.TipHash()) {
                // The mainchain dropped blocks on record: go back to the last one it still has.
                int common{height - 1};
                for (; common >= 0; --common) {
                    const UniValue event{fetch(common, 1)};
                    if (!event.empty() && Hash256(event[0]["hash"]) == record.GetBlock(common)->hash) break;
                }
                const std::vector<MainBlock> removed{record.Truncate(common)};
                LogInfo("The mainchain dropped %d blocks above height %d", removed.size(), common);
                LOCK(m_mutex);
                for (const MainBlock& block : removed) {
                    if (block.bmm) m_uncommitted.push_back(*block.bmm);
                }
                changed = true;
                continue;
            }
            first_new = 1;
        }
        if (events.size() <= first_new) break;
        for (size_t i{first_new}; i < events.size(); ++i) {
            const MainBlock block{ParseMainBlock(events[i])};
            // Not the block that follows: the mainchain changed between two calls. Start over.
            if (!record.Append(block)) break;
            changed = true;
            if (block.bmm) {
                LOCK(m_mutex);
                m_committed.push_back(*block.bmm);
            }
        }
    }
    return changed;
}

void Follower::Poll()
{
    TRY_LOCK(m_sync_mutex, locked);
    if (!locked) return;
    try {
        if (UpdateRecord()) {
            LOCK(m_mutex);
            m_woken = true;
            m_wake.notify_all();
        }
    } catch (const std::exception&) {
        // The thread of the follower will run into the same error and report it.
    }
}

bool Follower::Sync(std::string& error)
{
    AssertLockNotHeld(::cs_main);
    try {
        {
            LOCK(m_sync_mutex);
            UpdateRecord();
        }
        Act();
        SendBundle();
    } catch (const std::exception& e) {
        error = e.what();
        LOCK(m_mutex);
        if (m_status.connected || m_status.error != error) {
            LogWarning("%s", error);
            // Shown where the node shows its warnings, such as the status bar of the GUI.
            if (m_node.warnings) {
                m_node.warnings->Unset(node::Warning::MAINCHAIN_UNREACHABLE);
                m_node.warnings->Set(node::Warning::MAINCHAIN_UNREACHABLE, strprintf(_("This node cannot follow the mainchain, without which it cannot tell which new blocks are valid. %s"), error));
            }
        }
        m_status = {false, error};
        return false;
    }
    LOCK(m_mutex);
    if (!m_status.connected) {
        LogInfo("Following the mainchain node at %s:%u", m_client.GetOptions().host, m_client.GetOptions().port);
        if (m_node.warnings) m_node.warnings->Unset(node::Warning::MAINCHAIN_UNREACHABLE);
    }
    m_status = {true, {}};
    return true;
}

void Follower::Act()
{
    ChainstateManager& chainman{*m_node.chainman};
    std::vector<uint256> uncommitted, committed;
    std::vector<std::shared_ptr<const CBlock>> to_submit;
    {
        LOCK(m_mutex);
        uncommitted.swap(m_uncommitted);
        committed.swap(m_committed);
        const Mainchain& record{*chainman.m_mainchain};
        for (const uint256& hash : committed) {
            const auto it{m_candidates.find(hash)};
            if (it == m_candidates.end()) continue;
            to_submit.push_back(it->second.block);
            if (m_mining.enabled) ++m_mining.blocks;
        }
        // A candidate can only be committed to by the block after the one it was built on.
        std::erase_if(m_candidates, [&](const auto& entry) { return entry.second.main_prev_height < record.Height(); });
    }
    if (uncommitted.empty() && committed.empty()) return;

    // A block that lost its commitment is no longer valid, and neither is what was built on it.
    const Mainchain& record{*chainman.m_mainchain};
    for (const uint256& hash : uncommitted) {
        if (record.BmmHeight(hash)) continue; // It has a commitment again.
        CBlockIndex* pindex{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(hash))};
        if (!pindex) continue;
        LogInfo("Block %s lost its commitment on the mainchain", hash.ToString());
        BlockValidationState state;
        chainman.ActiveChainstate().InvalidateBlock(state, pindex);
    }
    {
        LOCK(::cs_main);
        for (const uint256& hash : committed) {
            CBlockIndex* pindex{chainman.m_blockman.LookupBlockIndex(hash)};
            if (pindex && (pindex->nStatus & BLOCK_FAILED_VALID)) {
                LogInfo("Block %s has a commitment on the mainchain again", hash.ToString());
                chainman.ActiveChainstate().ResetBlockFailureFlags(pindex);
                chainman.RecalculateBestHeader();
            }
        }
    }
    // Headers that came before the commitment to them was on record. With the
    // header accepted, the block is fetched from the peers that announced it.
    for (const uint256& hash : committed) {
        std::optional<CBlockHeader> header;
        {
            LOCK(::cs_main);
            if (const auto it{chainman.m_bmm_waiting.find(hash)}; it != chainman.m_bmm_waiting.end()) {
                header = it->second;
                chainman.m_bmm_waiting.erase(it);
            }
        }
        if (!header) continue;
        BlockValidationState state;
        chainman.ProcessNewBlockHeaders({{*header}}, /*min_pow_checked=*/true, state);
    }
    for (const auto& block : to_submit) {
        bool new_block{false};
        if (!chainman.ProcessNewBlock(block, /*force_processing=*/true, /*min_pow_checked=*/true, &new_block)) {
            LogWarning("The block %s that the mainchain committed to was not accepted", block->GetHash().ToString());
        }
    }
    BlockValidationState state;
    chainman.ActiveChainstate().ActivateBestChain(state);
}

void Follower::SendBundle()
{
    ChainstateManager& chainman{*m_node.chainman};
    std::optional<CMutableTransaction> tx;
    uint256 hash;
    {
        LOCK(::cs_main);
        const State& side{chainman.ActiveChainstate().m_scdb.m_side};
        if (!side.Bundle()) return;
        hash = side.Bundle()->hash;
        tx = side.BundleTx();
    }
    if (!tx || WITH_LOCK(m_mutex, return m_bundle_sent == hash)) return;
    // The mainchain node proposes the bundle in the blocks it mines; others learn its hash from those.
    m_client.Call("receivewithdrawalbundle", Params({static_cast<uint64_t>(chainman.GetConsensus().sidechain.slot), EncodeHexTx(CTransaction{*tx})}));
    LogInfo("Gave the mainchain node the withdrawal bundle %s", hash.ToString());
    LOCK(m_mutex);
    m_bundle_sent = hash;
}

std::optional<Follower::Candidate> Follower::CreateCandidate(const CScript& coinbase_script, std::string& error)
{
    ChainstateManager& chainman{*m_node.chainman};
    const Mainchain& record{*Assert(chainman.m_mainchain)};
    if (!Sync(error)) return std::nullopt;
    for (int attempt{0}; attempt < 10 && !m_stop; ++attempt) {
        Candidate candidate;
        candidate.main_prev_height = record.Height();
        candidate.main_prev_hash = record.TipHash();
        if (candidate.main_prev_height < 0) {
            error = "The mainchain has no blocks on record yet";
            return std::nullopt;
        }
        std::unique_ptr<interfaces::BlockTemplate> block_template;
        try {
            block_template = Assert(m_node.mining)->createNewBlock({.coinbase_output_script = coinbase_script}, /*cooldown=*/false);
        } catch (const std::exception& e) {
            error = e.what();
            return std::nullopt;
        }
        if (!block_template) {
            error = "The node is shutting down";
            return std::nullopt;
        }
        // The block was built for the mainchain as it was on record; if that moved, build another.
        if (record.TipHash() != candidate.main_prev_hash) continue;

        CBlock block{block_template->getBlock()};
        block.hashMerkleRoot = BlockMerkleRoot(block);
        // The chain is secured by the work of the mainchain; its own proof of work is a formality.
        while (!CheckProofOfWork(block.GetHash(), block.nBits, chainman.GetConsensus())) ++block.nNonce;
        candidate.block = std::make_shared<const CBlock>(std::move(block));
        LOCK(m_mutex);
        m_candidates[candidate.block->GetHash()] = candidate;
        return candidate;
    }
    error = "The mainchain keeps moving";
    return std::nullopt;
}

std::optional<std::string> Follower::RequestCommitment(const Candidate& candidate, CAmount amount, std::string& error) const
{
    try {
        const UniValue result{m_client.Call("createbmmrequest", Params({static_cast<uint64_t>(m_node.chainman->GetConsensus().sidechain.slot), candidate.block->GetHash().GetHex(), ValueFromAmount(amount)}), /*wallet=*/true)};
        // The request names the mainchain block it has to follow; it has to be the one the candidate was built on.
        if (Hash256(result["prevblockhash"]) != candidate.main_prev_hash) {
            error = "The mainchain moved while the request was made";
            return std::nullopt;
        }
        return result["txid"].get_str();
    } catch (const std::exception& e) {
        error = e.what();
        return std::nullopt;
    }
}

void Follower::SetMining(bool enabled, const CScript& coinbase_script, CAmount amount)
{
    LOCK(m_mutex);
    m_mining.enabled = enabled;
    m_mining.coinbase_script = coinbase_script;
    m_mining.amount = amount;
    m_mining.error.clear();
    m_mining_requested_for.SetNull();
    m_woken = true;
    m_wake.notify_all();
}

Follower::Mining Follower::GetMining() const
{
    LOCK(m_mutex);
    return m_mining;
}

void Follower::Mine()
{
    Mining mining;
    {
        LOCK(m_mutex);
        mining = m_mining;
        if (!mining.enabled) return;
        // One request per mainchain block.
        if (m_mining_requested_for == m_node.chainman->m_mainchain->TipHash() && !m_mining_requested_for.IsNull()) return;
    }
    std::string error;
    const auto candidate{CreateCandidate(mining.coinbase_script, error)};
    if (candidate && RequestCommitment(*candidate, mining.amount, error)) {
        LOCK(m_mutex);
        m_mining_requested_for = candidate->main_prev_hash;
        ++m_mining.requests;
        m_mining.error.clear();
        return;
    }
    LOCK(m_mutex);
    if (m_mining.error != error) LogWarning("Merged mining: %s", error);
    m_mining.error = error;
}

void Follower::Run()
{
    while (!m_stop) {
        std::string error;
        if (Sync(error)) Mine();
        WAIT_LOCK(m_mutex, lock);
        m_wake.wait_for(lock, POLL_INTERVAL, [this]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_stop.load() || m_woken; });
        m_woken = false;
    }
}

} // namespace sidechain
