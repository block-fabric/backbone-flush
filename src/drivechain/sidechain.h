// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DRIVECHAIN_SIDECHAIN_H
#define BITCOIN_DRIVECHAIN_SIDECHAIN_H

#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/**
 * Drivechain data structures and script formats.
 *
 * Miners coordinate sidechains through messages placed in OP_RETURN outputs of
 * the coinbase transaction. Every message is a single data push that starts
 * with a four byte tag:
 *
 *   M1  propose sidechain    PROPOSAL_TAG | Sidechain
 *   M2  ack proposal         ACK_TAG      | proposal hash
 *   M3  propose bundle       BUNDLE_TAG   | slot | bundle hash
 *   M4  vote on bundles      VOTE_TAG     | version | 2 bytes per sidechain with pending bundles
 *   M7  accept BMM request   BMM_ACCEPT_TAG | slot | sidechain block hash
 *
 * Two more messages are ordinary transactions rather than coinbase outputs:
 *
 *   M5  deposit      adds coins to the escrow output of a sidechain
 *   M6  withdrawal   pays out a withdrawal bundle from the escrow output
 *   M8  BMM request  OP_RETURN output: BMM_REQUEST_TAG | slot | sidechain block hash | previous mainchain block hash
 */
namespace drivechain {

/** Sidechain slot number. */
using SidechainId = uint32_t;

/** Highest sidechain description version this software understands. */
inline constexpr int32_t SIDECHAIN_VERSION_MAX{0};
/** Largest title of a sidechain proposal, in bytes. */
inline constexpr size_t MAX_TITLE_SIZE{255};
/** Largest description of a sidechain proposal, in bytes. */
inline constexpr size_t MAX_DESCRIPTION_SIZE{1024};
/** Largest deposit destination, in bytes. */
inline constexpr size_t MAX_DEPOSIT_DESTINATION_SIZE{70};
/** Deposit destination that marks the change output of a withdrawal. */
inline const std::string WITHDRAWAL_RETURN_DEST{"D"};
/** Version of the vote message. */
inline constexpr uint8_t VOTE_VERSION{0};
/** Vote value: abstain from voting on the bundles of a sidechain. */
inline constexpr uint16_t VOTE_ABSTAIN{0xFFFF};
/** Vote value: downvote every bundle of a sidechain. */
inline constexpr uint16_t VOTE_DOWNVOTE{0xFFFE};

inline constexpr unsigned char PROPOSAL_TAG[4]{0xD5, 0xE0, 0xC4, 0xAF};
inline constexpr unsigned char ACK_TAG[4]{0xD6, 0xE1, 0xC5, 0xBF};
inline constexpr unsigned char BUNDLE_TAG[4]{0xD4, 0x5A, 0xA9, 0x43};
inline constexpr unsigned char VOTE_TAG[4]{0xD7, 0x7D, 0x17, 0x76};
inline constexpr unsigned char BMM_ACCEPT_TAG[4]{0xD1, 0x61, 0x73, 0x68};
inline constexpr unsigned char BMM_REQUEST_TAG[3]{0x00, 0xBF, 0x00};

/** Description of a sidechain, as proposed by a miner. */
struct Sidechain {
    SidechainId slot{0};
    int32_t version{0};
    std::string title;
    std::string description;
    //! Hash of the release tarball of the sidechain software.
    uint256 hash_id1;
    //! Hash of the commit the sidechain software was built from.
    uint160 hash_id2;

    SERIALIZE_METHODS(Sidechain, obj) { READWRITE(obj.slot, obj.version, obj.title, obj.description, obj.hash_id1, obj.hash_id2); }

    /** Hash identifying this proposal; miners ack a proposal by committing to it. */
    uint256 GetHash() const;
    /** Whether the fields are within the limits a proposal has to respect. */
    bool IsValid(uint32_t max_sidechains) const;

    friend bool operator==(const Sidechain&, const Sidechain&) = default;
};

/** A sidechain proposal that is collecting acks. */
struct Proposal {
    Sidechain sidechain;
    //! Height of the block that contained the proposal.
    int32_t height{0};
    //! Number of blocks after the proposal block that acked the proposal.
    uint32_t acks{0};

    SERIALIZE_METHODS(Proposal, obj) { READWRITE(obj.sidechain, obj.height, obj.acks); }

    friend bool operator==(const Proposal&, const Proposal&) = default;
};

/** The escrow output of a sidechain ("critical txid-index pair"). */
struct Ctip {
    COutPoint outpoint;
    CAmount amount{0};

    SERIALIZE_METHODS(Ctip, obj) { READWRITE(obj.outpoint, obj.amount); }

    friend bool operator==(const Ctip&, const Ctip&) = default;
};

/** A withdrawal bundle that miners are voting on. */
struct Bundle {
    //! Blind hash of the withdrawal transaction.
    uint256 hash;
    //! Height of the block that proposed the bundle.
    int32_t height{0};
    uint32_t score{0};

    SERIALIZE_METHODS(Bundle, obj) { READWRITE(obj.hash, obj.height, obj.score); }

    friend bool operator==(const Bundle&, const Bundle&) = default;
};

/** Everything consensus tracks about an active sidechain. */
struct Slot {
    Sidechain sidechain;
    //! Height at which the sidechain was activated.
    int32_t activation_height{0};
    bool has_ctip{false};
    Ctip ctip;
    //! Pending withdrawal bundles, oldest first.
    std::vector<Bundle> bundles;

    SERIALIZE_METHODS(Slot, obj) { READWRITE(obj.sidechain, obj.activation_height, obj.has_ctip, obj.ctip, obj.bundles); }

    friend bool operator==(const Slot&, const Slot&) = default;
};

/** A change of the coins held in escrow by a sidechain, reported to sidechain software. */
struct Deposit {
    SidechainId slot{0};
    //! Destination on the sidechain, or WITHDRAWAL_RETURN_DEST for the change of a withdrawal.
    std::string destination;
    //! The transaction, with its escrow output at index `burn_index`.
    CTransactionRef tx;
    uint32_t burn_index{0};
    //! Position of the transaction in its block.
    uint32_t tx_index{0};
    //! Coins added to the escrow by this transaction; zero for a withdrawal.
    CAmount amount{0};
    //! Coins held in escrow after this transaction.
    CAmount total{0};
    uint256 block_hash;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << slot << destination << TX_WITH_WITNESS(tx) << burn_index << tx_index << amount << total << block_hash;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> slot >> destination >> TX_WITH_WITNESS(tx) >> burn_index >> tx_index >> amount >> total >> block_hash;
    }
};

/** A request for blind merged mining found in a transaction output. */
struct BmmRequest {
    SidechainId slot{0};
    //! Hash of the sidechain block to commit to.
    uint256 side_block_hash;
    //! Mainchain block the request must be mined on top of.
    uint256 prev_main_block_hash;
};

/** A vote of a miner on the withdrawal bundles of one sidechain. */
struct Vote {
    enum class Type : uint8_t { ABSTAIN, DOWNVOTE, UPVOTE };
    Type type{Type::ABSTAIN};
    //! Bundle to upvote.
    uint256 bundle;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << static_cast<uint8_t>(type) << bundle;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        uint8_t value;
        s >> value >> bundle;
        type = value <= static_cast<uint8_t>(Type::UPVOTE) ? static_cast<Type>(value) : Type::ABSTAIN;
    }
};

//
// Script formats.
//

/**
 * Script of the escrow output of the sidechain in `slot`:
 * OP_DRIVECHAIN <slot> OP_DROP OP_TRUE. At the script level anyone can spend
 * it, with an empty scriptSig; the drivechain rules restrict how.
 */
CScript EscrowScript(SidechainId slot);
/** If `script` is an escrow script, return its slot. */
std::optional<SidechainId> ParseEscrowScript(const CScript& script);

CScript ProposalScript(const Sidechain& sidechain);
std::optional<Sidechain> ParseProposalScript(const CScript& script);

CScript AckScript(const uint256& proposal_hash);
std::optional<uint256> ParseAckScript(const CScript& script);

CScript BundleScript(SidechainId slot, const uint256& bundle_hash);
std::optional<std::pair<SidechainId, uint256>> ParseBundleScript(const CScript& script);

/** `votes` holds one entry per sidechain that has pending bundles, in slot order: a bundle index, VOTE_ABSTAIN or VOTE_DOWNVOTE. */
CScript VoteScript(const std::vector<uint16_t>& votes);
std::optional<std::vector<uint16_t>> ParseVoteScript(const CScript& script);

CScript BmmAcceptScript(SidechainId slot, const uint256& side_block_hash);
std::optional<std::pair<SidechainId, uint256>> ParseBmmAcceptScript(const CScript& script);

CScript BmmRequestScript(const BmmRequest& request);
std::optional<BmmRequest> ParseBmmRequestScript(const CScript& script);

/** Script of an OP_RETURN output carrying a deposit destination. */
CScript DestinationScript(const std::string& destination);
/** If `script` is an OP_RETURN followed by a data push, return the pushed data as a string. */
std::optional<std::string> ParseDestinationScript(const CScript& script);

/** Script of the output of a withdrawal transaction that states the fee it pays to mainchain miners. */
CScript WithdrawalFeeScript(CAmount fee);
std::optional<CAmount> ParseWithdrawalFeeScript(const CScript& script);

//
// Withdrawal transactions.
//

/**
 * A sidechain builds a withdrawal bundle without knowing which escrow output it
 * will spend or how much change will be left. The blind form of a withdrawal
 * transaction replaces its input with a null input that has OP_0 as scriptSig
 * and drops the last output, the change returned to the escrow; miners vote
 * on its hash.
 */
std::optional<CMutableTransaction> BlindWithdrawalTx(const CTransaction& tx);
/** Hash of the blind form of a withdrawal transaction. */
std::optional<uint256> BlindWithdrawalHash(const CTransaction& tx);

/** The BMM requests among the outputs of a transaction. */
std::vector<BmmRequest> GetBmmRequests(const CTransaction& tx);

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_SIDECHAIN_H
