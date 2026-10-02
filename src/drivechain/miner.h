// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DRIVECHAIN_MINER_H
#define BITCOIN_DRIVECHAIN_MINER_H

#include <consensus/amount.h>
#include <consensus/params.h>
#include <drivechain/scdb.h>
#include <drivechain/sidechain.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <sync.h>
#include <uint256.h>
#include <util/fs.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace drivechain {

/** What the drivechain rules make a miner add to a block. */
struct BlockAdditions {
    //! Messages for the coinbase, as zero value outputs.
    std::vector<CTxOut> coinbase_outputs;
    //! Withdrawal bundles that can be paid out, to be placed after all other transactions.
    std::vector<CTransactionRef> withdrawals;
    //! Fee of each of the withdrawals.
    std::vector<CAmount> withdrawal_fees;
};

/**
 * What this node does with the drivechain decisions that are left to miners:
 * the sidechains it proposes and acks, the withdrawal bundles it has been
 * handed by sidechains, and how it votes on them. None of this is consensus;
 * it only shapes the blocks this node builds.
 */
class MinerState
{
public:
    /**
     * Keep the state in this file: load what it holds, and write it back
     * whenever something changes.
     * @return false if the file exists but cannot be read
     */
    bool Load(const fs::path& path) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Propose a sidechain in the next block this node builds. */
    void AddProposal(const Sidechain& sidechain) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    bool RemoveProposal(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    std::vector<Sidechain> GetProposals() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Whether to ack the proposal with this hash in the blocks this node builds. */
    void SetAck(const uint256& proposal_hash, bool ack) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    bool IsAcked(const uint256& proposal_hash) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    std::set<uint256> GetAcks() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /**
     * Remember a withdrawal bundle received from a sidechain, in blind form.
     * It is proposed in the next block and paid out once it has the work score.
     * @return the hash of the bundle, or nullopt with `error` set
     */
    std::optional<uint256> AddBundle(SidechainId slot, const CMutableTransaction& blind_tx, std::string& error) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    std::optional<CMutableTransaction> GetBundle(SidechainId slot, const uint256& hash) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    std::vector<std::pair<SidechainId, uint256>> GetBundles() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** How to vote on the bundles of a sidechain. Sidechains without a vote get the default. */
    void SetVote(SidechainId slot, const Vote& vote) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void ClearVote(SidechainId slot) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Vote GetVote(SidechainId slot) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    std::map<SidechainId, Vote> GetVotes() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void SetDefaultVote(Vote::Type type) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Vote::Type GetDefaultVote() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Drop what the chain has made obsolete: bundles that were paid out or failed. */
    void Prune(const SidechainDB& scdb) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /**
     * What to add to a block built on top of the block `scdb` belongs to.
     *
     * @param[in] txs  the transactions selected for the block, without the coinbase
     */
    BlockAdditions CreateBlockAdditions(const SidechainDB& scdb, const Consensus::DrivechainParams& params,
                                        const uint256& prev_block_hash, const std::vector<CTransactionRef>& txs) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    /** Write the state to m_path, if there is one. */
    void Save() const EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

    mutable Mutex m_mutex;
    fs::path m_path GUARDED_BY(m_mutex);
    std::vector<Sidechain> m_proposals GUARDED_BY(m_mutex);
    std::set<uint256> m_acks GUARDED_BY(m_mutex);
    std::map<std::pair<SidechainId, uint256>, CMutableTransaction> m_bundles GUARDED_BY(m_mutex);
    std::map<SidechainId, Vote> m_votes GUARDED_BY(m_mutex);
    Vote::Type m_default_vote GUARDED_BY(m_mutex){Vote::Type::ABSTAIN};
};

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_MINER_H
