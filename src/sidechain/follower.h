// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SIDECHAIN_FOLLOWER_H
#define BITCOIN_SIDECHAIN_FOLLOWER_H

#include <consensus/amount.h>
#include <primitives/block.h>
#include <script/script.h>
#include <sidechain/mainclient.h>
#include <sync.h>
#include <uint256.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace node {
struct NodeContext;
} // namespace node

namespace sidechain {
struct MainBlock;

/**
 * Keeps the record of the mainchain up to date, and does what follows from a
 * change of the mainchain:
 *
 *  - when the mainchain drops a block that committed to a block of this chain,
 *    that block is no longer valid, and when the commitment comes back, it is
 *    valid again;
 *  - when the mainchain commits to a block this node built, the block is
 *    connected;
 *  - when this chain has a withdrawal bundle to be voted on, the mainchain
 *    node is given its transaction.
 *
 * It also builds the blocks of this chain and asks the mainchain to commit to
 * them (blind merged mining), on request or continuously.
 */
class Follower
{
public:
    Follower(node::NodeContext& node, MainClient::Options options);
    ~Follower();

    void Start() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void Stop() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /**
     * Bring the record of the mainchain up to date, and act on the changes.
     * Must not be called with cs_main held.
     * @return false, with `error` set, if the mainchain node could not be asked.
     */
    bool Sync(std::string& error) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex, !m_sync_mutex);
    /**
     * Bring the record up to date if no other thread is doing so, and leave
     * acting on the changes to the thread of the follower. For callers that
     * hold cs_main.
     */
    void Poll() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex, !m_sync_mutex);

    struct Status {
        bool connected{false};
        std::string error;
    };
    Status GetStatus() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** A block of this chain waiting for the mainchain to commit to it. */
    struct Candidate {
        std::shared_ptr<const CBlock> block;
        //! The mainchain block the candidate was built on; only the next one can commit to it.
        uint256 main_prev_hash;
        int main_prev_height{0};
    };
    /**
     * Build a block that pays its fees to `coinbase_script`, and keep it until
     * the mainchain commits to it or moves on.
     */
    std::optional<Candidate> CreateCandidate(const CScript& coinbase_script, std::string& error) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex, !m_sync_mutex);
    /** Ask the wallet of the mainchain node to pay `amount` for a commitment to `candidate`. @return the id of its transaction */
    std::optional<std::string> RequestCommitment(const Candidate& candidate, CAmount amount, std::string& error) const;

    struct Mining {
        bool enabled{false};
        CScript coinbase_script;
        //! What to offer mainchain miners for each commitment.
        CAmount amount{0};
        uint64_t requests{0};
        uint64_t blocks{0};
        std::string error;
    };
    /** Turn continuous merged mining on or off. */
    void SetMining(bool enabled, const CScript& coinbase_script, CAmount amount) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Mining GetMining() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    const MainClient& Client() const { return m_client; }

private:
    void Run() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex, !m_sync_mutex);
    /** Update the record. @return whether it changed */
    bool UpdateRecord() EXCLUSIVE_LOCKS_REQUIRED(m_sync_mutex, !m_mutex);
    /** Do what the changes of the record call for. */
    void Act() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void Mine() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex, !m_sync_mutex);
    void SendBundle() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    node::NodeContext& m_node;
    const MainClient m_client;

    //! Held while the record is updated.
    Mutex m_sync_mutex;

    mutable Mutex m_mutex;
    std::condition_variable_any m_wake;
    std::thread m_thread GUARDED_BY(m_mutex);
    std::atomic<bool> m_stop{false};
    //! Set when the thread has something to do before its next regular round.
    bool m_woken GUARDED_BY(m_mutex){false};
    Status m_status GUARDED_BY(m_mutex);
    //! Sidechain blocks that lost, respectively gained, their commitment, and are still to be acted on.
    std::vector<uint256> m_uncommitted GUARDED_BY(m_mutex);
    std::vector<uint256> m_committed GUARDED_BY(m_mutex);
    std::map<uint256, Candidate> m_candidates GUARDED_BY(m_mutex);
    Mining m_mining GUARDED_BY(m_mutex);
    //! The mainchain block for which a commitment was last asked.
    uint256 m_mining_requested_for GUARDED_BY(m_mutex);
    //! The bundle the mainchain node was last given.
    uint256 m_bundle_sent GUARDED_BY(m_mutex);
};

} // namespace sidechain

#endif // BITCOIN_SIDECHAIN_FOLLOWER_H
