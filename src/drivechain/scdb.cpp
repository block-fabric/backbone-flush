// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <drivechain/scdb.h>

#include <consensus/amount.h>
#include <hash.h>
#include <primitives/transaction.h>
#include <script/script.h>

#include <algorithm>
#include <limits>
#include <optional>

namespace drivechain {

namespace {

/** Age of something proposed at `proposed_height`, counting the proposing block as one. */
int Age(int proposed_height, int height) { return height - proposed_height + 1; }

} // namespace

const Slot* SidechainDB::GetSlot(SidechainId id) const
{
    const auto it{m_slots.find(id)};
    return it == m_slots.end() ? nullptr : &it->second;
}

const Proposal* SidechainDB::GetProposal(const uint256& hash) const
{
    for (const Proposal& proposal : m_proposals) {
        if (proposal.sidechain.GetHash() == hash) return &proposal;
    }
    return nullptr;
}

SidechainDB::PendingBundles SidechainDB::GetPendingBundles() const
{
    PendingBundles pending;
    for (const auto& [id, slot] : m_slots) {
        if (!slot.bundles.empty()) pending.emplace_back(id, slot.bundles);
    }
    return pending;
}

int SidechainDB::BlocksUntilActivation(const Proposal& proposal, bool slot_in_use, int height, const Consensus::DrivechainParams& params)
{
    const int period{slot_in_use ? params.replacement_period : params.activation_period};
    return period - Age(proposal.height, height);
}

int SidechainDB::Failures(const Proposal& proposal, int height)
{
    // The proposing block counts as an ack.
    return std::max(0, Age(proposal.height, height) - 1 - static_cast<int>(proposal.acks));
}

int SidechainDB::BlocksLeft(const Bundle& bundle, int height, const Consensus::DrivechainParams& params)
{
    return params.withdrawal_period - Age(bundle.height, height);
}

uint256 SidechainDB::GetHash() const
{
    return (HashWriter{} << *this).GetHash();
}

void SidechainDB::SaveSlot(SidechainId id, BlockUndo& undo) const
{
    for (const BlockUndo::SlotUndo& saved : undo.slots) {
        if (saved.id == id) return;
    }
    BlockUndo::SlotUndo saved;
    saved.id = id;
    if (const Slot* slot{GetSlot(id)}) {
        saved.existed = true;
        saved.slot = *slot;
    }
    undo.slots.push_back(std::move(saved));
}

void SidechainDB::CloseBundle(SidechainId id, const uint256& hash, bool paid, BlockUndo& undo)
{
    if (m_closed.emplace(std::pair{id, hash}, paid).second) undo.closed.emplace_back(id, hash);
}

void SidechainDB::RemoveProposal(size_t index, BlockUndo& undo)
{
    undo.removed.emplace_back(static_cast<uint32_t>(index), m_proposals[index]);
    m_proposals.erase(m_proposals.begin() + index);
}

SidechainDB::EscrowOutputs SidechainDB::GetEscrowOutputs() const
{
    EscrowOutputs escrow_outputs;
    for (const auto& [id, slot] : m_slots) {
        if (slot.has_ctip) escrow_outputs.emplace(slot.ctip.outpoint, id);
    }
    return escrow_outputs;
}

bool SidechainDB::ConnectTx(const CTransaction& tx, const Consensus::DrivechainParams& params, EscrowOutputs& escrow_outputs,
                            bool allow_withdrawal, BlockUndo& undo, std::optional<Deposit>* deposit, std::string& reject_reason)
{
    const auto invalid = [&](const char* reason) {
        reject_reason = reason;
        return false;
    };

    // The escrow output this transaction creates, if any.
    std::optional<SidechainId> out_slot;
    uint32_t burn_index{0};
    for (uint32_t i{0}; i < tx.vout.size(); ++i) {
        const auto slot{ParseEscrowScript(tx.vout[i].scriptPubKey)};
        if (!slot || *slot >= params.max_sidechains) continue;
        if (out_slot) return invalid("bad-dc-multiple-escrow-outputs");
        out_slot = *slot;
        burn_index = i;
    }

    // The escrow output this transaction spends, if any.
    std::optional<SidechainId> in_slot;
    for (const CTxIn& in : tx.vin) {
        const auto spent{escrow_outputs.find(in.prevout)};
        if (spent == escrow_outputs.end()) continue;
        if (in_slot) return invalid("bad-dc-multiple-escrow-inputs");
        in_slot = spent->second;
    }

    if (!out_slot && !in_slot) return true;
    // Coins leave the escrow only through a withdrawal, which returns the remainder to the escrow.
    if (!out_slot) return invalid("bad-dc-escrow-spend");

    const SidechainId id{*out_slot};
    const auto it{m_slots.find(id)};
    if (it == m_slots.end()) return invalid("bad-dc-inactive-sidechain");
    // A sidechain has a single escrow output: the new one has to replace the current one.
    if (in_slot && *in_slot != id) return invalid("bad-dc-escrow-mismatch");
    if (it->second.has_ctip && !in_slot) return invalid("bad-dc-escrow-unspent");

    SaveSlot(id, undo);
    Slot& slot{it->second};

    const CAmount old_amount{slot.has_ctip ? slot.ctip.amount : 0};
    const CAmount new_amount{tx.vout[burn_index].nValue};
    if (!MoneyRange(new_amount)) return invalid("bad-dc-escrow-amount");

    std::string destination;
    if (new_amount > old_amount) {
        // Deposit. The first OP_RETURN output names the destination on the sidechain.
        std::optional<std::string> dest;
        for (const CTxOut& out : tx.vout) {
            if (out.scriptPubKey.empty() || out.scriptPubKey[0] != OP_RETURN) continue;
            dest = ParseDestinationScript(out.scriptPubKey);
            break;
        }
        if (!dest || dest->empty() || dest->size() > MAX_DEPOSIT_DESTINATION_SIZE || *dest == WITHDRAWAL_RETURN_DEST) {
            return invalid("bad-dc-deposit-destination");
        }
        destination = *dest;
    } else if (new_amount < old_amount) {
        // Withdrawal: outputs are the return destination marker, the fee, the
        // payouts and finally the change returned to the escrow.
        if (!allow_withdrawal) return invalid("bad-dc-withdrawal-loose");
        if (tx.vin.size() != 1) return invalid("bad-dc-withdrawal-inputs");
        if (tx.vout.size() < 3 || burn_index != tx.vout.size() - 1) return invalid("bad-dc-withdrawal-outputs");
        if (ParseDestinationScript(tx.vout[0].scriptPubKey) != WITHDRAWAL_RETURN_DEST) return invalid("bad-dc-withdrawal-marker");
        const auto fee{ParseWithdrawalFeeScript(tx.vout[1].scriptPubKey)};
        if (!fee) return invalid("bad-dc-withdrawal-fee");

        const auto blind_hash{BlindWithdrawalHash(tx)};
        if (!blind_hash) return invalid("bad-dc-withdrawal-format");
        const auto bundle{std::find_if(slot.bundles.begin(), slot.bundles.end(), [&](const Bundle& b) { return b.hash == *blind_hash; })};
        if (bundle == slot.bundles.end()) return invalid("bad-dc-withdrawal-unknown");
        if (bundle->score < static_cast<uint32_t>(params.withdrawal_min_score)) return invalid("bad-dc-withdrawal-score");

        CAmount paid_out{0};
        for (size_t i{0}; i + 1 < tx.vout.size(); ++i) {
            if (!MoneyRange(tx.vout[i].nValue)) return invalid("bad-dc-withdrawal-amount");
            paid_out += tx.vout[i].nValue;
            if (!MoneyRange(paid_out)) return invalid("bad-dc-withdrawal-amount");
        }
        // The change is what is left of the escrow after the payouts and the fee.
        if (paid_out + *fee > old_amount || new_amount != old_amount - paid_out - *fee) return invalid("bad-dc-withdrawal-change");

        slot.bundles.erase(bundle);
        CloseBundle(id, *blind_hash, /*paid=*/true, undo);
        destination = WITHDRAWAL_RETURN_DEST;
    } else {
        return invalid("bad-dc-escrow-amount");
    }

    if (slot.has_ctip) escrow_outputs.erase(slot.ctip.outpoint);
    slot.has_ctip = true;
    slot.ctip.outpoint = COutPoint{tx.GetHash(), burn_index};
    slot.ctip.amount = new_amount;
    escrow_outputs.emplace(slot.ctip.outpoint, id);

    if (deposit) {
        deposit->emplace();
        (*deposit)->slot = id;
        (*deposit)->destination = destination;
        (*deposit)->burn_index = burn_index;
        (*deposit)->amount = new_amount > old_amount ? new_amount - old_amount : 0;
        (*deposit)->total = new_amount;
    }
    return true;
}

bool SidechainDB::ConnectBlock(const CBlock& block, int height, const Consensus::DrivechainParams& params,
                               BlockUndo& undo, std::vector<Deposit>* deposits, std::string& reject_reason,
                               const SideContext* side)
{
    const auto invalid = [&](const char* reason) {
        reject_reason = reason;
        return false;
    };

    undo = BlockUndo{};
    undo.prev_block_hash = m_block_hash;

    if (side) {
        if (block.vtx.empty() || !block.vtx[0]->IsCoinBase()) {
            reject_reason = "bad-dc-no-coinbase";
            return false;
        }
        if (!m_side.ConnectBlock(block, height, side->params, side->mainchain, undo.side, side->minted, reject_reason)) return false;
    }

    if (block.vtx.empty() || !block.vtx[0]->IsCoinBase()) return invalid("bad-dc-no-coinbase");
    const uint256 block_hash{block.GetHash()};

    //
    // Read the messages in the coinbase. Messages for slots this software does
    // not know about, and messages that do not parse, are ignored so that later
    // versions can give them a meaning.
    //
    std::optional<Sidechain> new_proposal;
    std::vector<uint256> acks;
    std::map<SidechainId, uint256> new_bundles;
    std::optional<std::vector<uint16_t>> votes;
    std::map<SidechainId, uint256> bmm_accepts;

    for (const CTxOut& out : block.vtx[0]->vout) {
        const CScript& script{out.scriptPubKey};

        if (const auto slot{ParseEscrowScript(script)}; slot && *slot < params.max_sidechains) {
            // A coinbase cannot be a deposit: it cannot spend the previous escrow output.
            return invalid("bad-dc-coinbase-escrow");
        }
        if (script.empty() || script[0] != OP_RETURN) continue;

        if (const auto proposal{ParseProposalScript(script)}) {
            if (!proposal->IsValid(params.max_sidechains)) continue;
            if (new_proposal) return invalid("bad-dc-multiple-proposals");
            new_proposal = *proposal;
        } else if (const auto ack{ParseAckScript(script)}) {
            acks.push_back(*ack);
        } else if (const auto bundle{ParseBundleScript(script)}) {
            if (bundle->first >= params.max_sidechains) continue;
            if (!new_bundles.emplace(bundle->first, bundle->second).second) return invalid("bad-dc-multiple-bundles");
        } else if (const auto parsed_votes{ParseVoteScript(script)}) {
            if (votes) return invalid("bad-dc-multiple-votes");
            votes = *parsed_votes;
        } else if (const auto accept{ParseBmmAcceptScript(script)}) {
            if (accept->first >= params.max_sidechains) continue;
            if (!IsActive(accept->first)) return invalid("bad-dc-bmm-inactive-sidechain");
            if (!bmm_accepts.emplace(accept->first, accept->second).second) return invalid("bad-dc-multiple-bmm-accepts");
        }
    }

    // Votes refer to the bundles as they were before this block.
    const PendingBundles votable{GetPendingBundles()};

    //
    // Transactions: deposits (M5), withdrawals (M6) and BMM requests (M8).
    //
    EscrowOutputs escrow_outputs{GetEscrowOutputs()};
    std::set<SidechainId> bmm_requested;

    for (size_t tx_index{1}; tx_index < block.vtx.size(); ++tx_index) {
        const CTransaction& tx{*block.vtx[tx_index]};

        for (const BmmRequest& request : GetBmmRequests(tx)) {
            if (request.slot >= params.max_sidechains) continue;
            // A request is only good for one block: the one mined on top of the block it names.
            if (request.prev_main_block_hash != block.hashPrevBlock) return invalid("bad-dc-bmm-prev-block");
            const auto accept{bmm_accepts.find(request.slot)};
            if (accept == bmm_accepts.end() || accept->second != request.side_block_hash) return invalid("bad-dc-bmm-not-accepted");
            if (!bmm_requested.insert(request.slot).second) return invalid("bad-dc-multiple-bmm-requests");
        }

        std::optional<Deposit> deposit;
        if (!ConnectTx(tx, params, escrow_outputs, /*allow_withdrawal=*/true, undo, &deposit, reject_reason)) return false;
        if (deposit && deposits) {
            deposit->tx = block.vtx[tx_index];
            deposit->tx_index = static_cast<uint32_t>(tx_index);
            deposit->block_hash = block_hash;
            deposits->push_back(std::move(*deposit));
        }
    }

    //
    // Sidechain proposals (M1) and acks (M2).
    //
    if (new_proposal) {
        if (GetProposal(new_proposal->GetHash())) return invalid("bad-dc-duplicate-proposal");
        Proposal proposal;
        proposal.sidechain = *new_proposal;
        proposal.height = height;
        m_proposals.push_back(std::move(proposal));
        undo.proposal_added = true;
    }

    std::set<SidechainId> acked_slots;
    for (const uint256& hash : acks) {
        const auto proposal{std::find_if(m_proposals.begin(), m_proposals.end(), [&](const Proposal& p) { return p.sidechain.GetHash() == hash; })};
        if (proposal == m_proposals.end()) return invalid("bad-dc-ack-unknown-proposal");
        // A block can support one proposal per slot.
        if (!acked_slots.insert(proposal->sidechain.slot).second) return invalid("bad-dc-multiple-acks");
        // The block that proposes a sidechain already counts as an ack of it.
        if (proposal->height == height) continue;
        ++proposal->acks;
        undo.acked.push_back(hash);
    }

    // Proposals that ran out of time.
    for (size_t i{0}; i < m_proposals.size();) {
        if (BlocksUntilActivation(m_proposals[i], IsActive(m_proposals[i].sidechain.slot), height, params) < 0) {
            RemoveProposal(i, undo);
        } else {
            ++i;
        }
    }
    // Proposals that too many blocks did not ack.
    for (size_t i{0}; i < m_proposals.size();) {
        if (Failures(m_proposals[i], height) >= params.activation_max_failures) {
            RemoveProposal(i, undo);
        } else {
            ++i;
        }
    }
    // Proposals that made it.
    for (size_t i{0}; i < m_proposals.size();) {
        const SidechainId id{m_proposals[i].sidechain.slot};
        if (BlocksUntilActivation(m_proposals[i], IsActive(id), height, params) != 0) {
            ++i;
            continue;
        }
        SaveSlot(id, undo);
        Slot& slot{m_slots[id]};
        // A sidechain that replaces another one takes over its escrow, but not its pending withdrawals.
        slot.sidechain = m_proposals[i].sidechain;
        slot.activation_height = height;
        slot.bundles.clear();
        RemoveProposal(i, undo);
    }

    //
    // Withdrawal bundles: expiry, votes (M4) and new bundles (M3).
    //
    for (auto& [id, slot] : m_slots) {
        for (size_t i{0}; i < slot.bundles.size();) {
            const Bundle& bundle{slot.bundles[i]};
            // Blocks left before this one was connected.
            const int blocks_left{BlocksLeft(bundle, height, params) + 1};
            const bool expired{blocks_left <= 0 ||
                               params.withdrawal_min_score - static_cast<int64_t>(bundle.score) > blocks_left};
            if (!expired) {
                ++i;
                continue;
            }
            SaveSlot(id, undo);
            const uint256 hash{bundle.hash};
            slot.bundles.erase(slot.bundles.begin() + i);
            CloseBundle(id, hash, /*paid=*/false, undo);
        }
    }

    if (votes) {
        if (votes->size() > votable.size()) return invalid("bad-dc-votes-size");
        for (size_t i{0}; i < votes->size(); ++i) {
            const uint16_t vote{(*votes)[i]};
            const auto& [id, bundles]{votable[i]};
            if (vote == VOTE_ABSTAIN) continue;
            if (vote != VOTE_DOWNVOTE && vote >= bundles.size()) return invalid("bad-dc-votes-index");
            // The scores of a sidechain do not move in a block that proposes a bundle for it.
            if (new_bundles.contains(id)) continue;

            const auto it{m_slots.find(id)};
            if (it == m_slots.end() || it->second.bundles.empty()) continue;
            SaveSlot(id, undo);
            for (Bundle& bundle : it->second.bundles) {
                if (vote != VOTE_DOWNVOTE && bundle.hash == bundles[vote].hash) {
                    if (bundle.score < std::numeric_limits<uint32_t>::max()) ++bundle.score;
                } else if (bundle.score > 0) {
                    --bundle.score;
                }
            }
        }
    }

    for (const auto& [id, hash] : new_bundles) {
        const auto it{m_slots.find(id)};
        if (it == m_slots.end()) continue;
        Slot& slot{it->second};
        if (std::any_of(slot.bundles.begin(), slot.bundles.end(), [&](const Bundle& b) { return b.hash == hash; })) return invalid("bad-dc-bundle-known");
        if (IsClosed(id, hash)) return invalid("bad-dc-bundle-closed");
        if (slot.bundles.size() >= params.max_pending_bundles) return invalid("bad-dc-too-many-bundles");
        SaveSlot(id, undo);
        Bundle bundle;
        bundle.hash = hash;
        bundle.height = height;
        bundle.score = 1;
        slot.bundles.push_back(bundle);
    }

    m_block_hash = block_hash;
    return true;
}

void SidechainDB::DisconnectBlock(const BlockUndo& undo)
{
    m_side.DisconnectBlock(undo.side);
    for (const BlockUndo::SlotUndo& saved : undo.slots) {
        if (saved.existed) {
            m_slots[saved.id] = saved.slot;
        } else {
            m_slots.erase(saved.id);
        }
    }
    for (const auto& closed : undo.closed) {
        m_closed.erase(closed);
    }
    for (auto it{undo.removed.rbegin()}; it != undo.removed.rend(); ++it) {
        m_proposals.insert(m_proposals.begin() + it->first, it->second);
    }
    for (const uint256& hash : undo.acked) {
        for (Proposal& proposal : m_proposals) {
            if (proposal.sidechain.GetHash() == hash) {
                --proposal.acks;
                break;
            }
        }
    }
    if (undo.proposal_added) m_proposals.pop_back();
    m_block_hash = undo.prev_block_hash;
}

} // namespace drivechain
