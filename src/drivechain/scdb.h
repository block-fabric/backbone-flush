// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DRIVECHAIN_SCDB_H
#define BITCOIN_DRIVECHAIN_SCDB_H

#include <consensus/params.h>
#include <drivechain/sidechain.h>
#include <primitives/block.h>
#include <sidechain/state.h>
#include <serialize.h>
#include <uint256.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace drivechain {

/** What a block changed in the sidechain database, in the form needed to revert it. */
struct BlockUndo {
    struct SlotUndo {
        SidechainId id{0};
        //! Whether the slot held a sidechain before the block.
        bool existed{false};
        Slot slot;

        SERIALIZE_METHODS(SlotUndo, obj) { READWRITE(obj.id, obj.existed, obj.slot); }
    };

    uint256 prev_block_hash;
    //! The value each changed slot had before the block.
    std::vector<SlotUndo> slots;
    //! Whether the block added a proposal, at the end of the list.
    bool proposal_added{false};
    //! Hashes of the proposals whose ack count the block incremented.
    std::vector<uint256> acked;
    //! Proposals the block removed, with the position each had when it was removed, in order of removal.
    std::vector<std::pair<uint32_t, Proposal>> removed;
    //! Bundles the block closed, by being paid out or by failing.
    std::vector<std::pair<SidechainId, uint256>> closed;

    //! What the block changed in the state of this chain as a sidechain.
    sidechain::StateUndo side;

    SERIALIZE_METHODS(BlockUndo, obj) { READWRITE(obj.prev_block_hash, obj.slots, obj.proposal_added, obj.acked, obj.removed, obj.closed, obj.side); }
};

/** What the rules of a chain that is itself a sidechain need to check a block. */
struct SideContext {
    const Consensus::SidechainParams& params;
    const sidechain::Mainchain& mainchain;
    //! Set to the coins the coinbase has to create on top of the fees.
    CAmount& minted;
};

/**
 * The sidechain database: the drivechain state implied by a chain of blocks.
 *
 * The state after a block depends on the state before it and on the block
 * alone. In particular the escrow output of every sidechain is tracked here,
 * so that no lookup in the UTXO set is needed. That works because the rules
 * below guarantee that a sidechain never has more than one unspent escrow
 * output.
 */
class SidechainDB
{
public:
    /** Vote of a block on the bundles of the sidechain in `first`. */
    using PendingBundles = std::vector<std::pair<SidechainId, std::vector<Bundle>>>;

    /**
     * Check `block`, to be connected at `height` on top of the block this
     * state belongs to, against the drivechain rules, and update the state.
     *
     * @param[out] undo           what is needed to revert the update
     * @param[out] deposits       escrow changes, for the benefit of sidechain software; may be null
     * @param[out] reject_reason  set when the block is invalid
     * @return false if the block is invalid, in which case the state is left
     *         partially updated and must be discarded.
     */
    [[nodiscard]] bool ConnectBlock(const CBlock& block, int height, const Consensus::DrivechainParams& params,
                                    BlockUndo& undo, std::vector<Deposit>* deposits, std::string& reject_reason,
                                    const SideContext* side = nullptr);

    /** Outpoints of the escrow outputs of the sidechains. */
    using EscrowOutputs = std::map<COutPoint, SidechainId>;
    EscrowOutputs GetEscrowOutputs() const;

    /**
     * Apply the escrow rules to a transaction that is not a coinbase: a
     * transaction that creates or spends an escrow output has to be a valid
     * deposit or withdrawal. ConnectBlock does this for every transaction of a
     * block; the mempool uses it to keep out transactions that could not be mined.
     *
     * @param[in,out] escrow_outputs   as returned by GetEscrowOutputs(), kept up to date across calls
     * @param[in]     allow_withdrawal whether a withdrawal is acceptable; false for loose transactions
     * @param[out]    deposit          the escrow change, if the transaction made one; may be null.
     *                                 The caller fills in the transaction and block fields.
     * @return false if the transaction is invalid; see ConnectBlock.
     */
    [[nodiscard]] bool ConnectTx(const CTransaction& tx, const Consensus::DrivechainParams& params, EscrowOutputs& escrow_outputs,
                                 bool allow_withdrawal, BlockUndo& undo, std::optional<Deposit>* deposit, std::string& reject_reason);

    /** Revert the update of the block that produced `undo`. */
    void DisconnectBlock(const BlockUndo& undo);

    /** Hash of the block this state belongs to; null before any block was connected. */
    const uint256& GetBlockHash() const { return m_block_hash; }
    void SetBlockHash(const uint256& hash) { m_block_hash = hash; }

    const std::map<SidechainId, Slot>& GetSlots() const { return m_slots; }
    const Slot* GetSlot(SidechainId id) const;
    bool IsActive(SidechainId id) const { return m_slots.contains(id); }
    const std::vector<Proposal>& GetProposals() const { return m_proposals; }
    const Proposal* GetProposal(const uint256& hash) const;
    /** Whether a bundle was paid out or failed. */
    bool IsClosed(SidechainId id, const uint256& bundle_hash) const { return m_closed.contains({id, bundle_hash}); }
    /** Whether a bundle was paid out (true) or failed (false), if it was closed. Sidechains refund the withdrawals of a failed bundle. */
    std::optional<bool> WasPaid(SidechainId id, const uint256& bundle_hash) const
    {
        const auto it{m_closed.find({id, bundle_hash})};
        return it == m_closed.end() ? std::nullopt : std::optional<bool>{it->second};
    }

    /**
     * The sidechains that have pending bundles, in slot order. The vote
     * message of the next block has one entry per element.
     */
    PendingBundles GetPendingBundles() const;

    /** Number of blocks a proposal has left before it activates, if it keeps collecting acks. */
    static int BlocksUntilActivation(const Proposal& proposal, bool slot_in_use, int height, const Consensus::DrivechainParams& params);
    /** Number of blocks without an ack a proposal had as of `height`. */
    static int Failures(const Proposal& proposal, int height);
    /** Number of blocks a bundle has left to reach the minimum work score, as of `height`. */
    static int BlocksLeft(const Bundle& bundle, int height, const Consensus::DrivechainParams& params);

    /** Hash committing to the entire state. */
    uint256 GetHash() const;

    /**
     * The state of this chain as a sidechain of another. It lives here so that
     * it is stored, copied and reverted together with the rest.
     */
    sidechain::State m_side;

    SERIALIZE_METHODS(SidechainDB, obj) { READWRITE(obj.m_block_hash, obj.m_slots, obj.m_proposals, obj.m_closed, obj.m_side); }

    friend bool operator==(const SidechainDB&, const SidechainDB&) = default;

private:
    /** Remember the value of a slot before its first change in a block. */
    void SaveSlot(SidechainId id, BlockUndo& undo) const;
    void CloseBundle(SidechainId id, const uint256& hash, bool paid, BlockUndo& undo);
    void RemoveProposal(size_t index, BlockUndo& undo);

    uint256 m_block_hash;
    //! The active sidechains.
    std::map<SidechainId, Slot> m_slots;
    //! Proposals collecting acks, oldest first.
    std::vector<Proposal> m_proposals;
    //! Bundles that were paid out (true) or failed (false); they cannot be proposed again.
    std::map<std::pair<SidechainId, uint256>, bool> m_closed;
};

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_SCDB_H
