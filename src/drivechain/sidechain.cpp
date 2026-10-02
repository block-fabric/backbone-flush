// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <drivechain/sidechain.h>

#include <crypto/common.h>
#include <hash.h>
#include <script/script.h>
#include <streams.h>

#include <algorithm>
#include <span>

namespace drivechain {

namespace {

/** Script consisting of OP_RETURN and a single push of `tag` followed by `payload`. */
CScript MessageScript(std::span<const unsigned char> tag, std::span<const unsigned char> payload)
{
    std::vector<unsigned char> data(tag.begin(), tag.end());
    data.insert(data.end(), payload.begin(), payload.end());
    return CScript() << OP_RETURN << data;
}

/** If `script` is OP_RETURN and a single data push and nothing else, return the pushed data. */
std::optional<std::vector<unsigned char>> ParsePushScript(const CScript& script)
{
    if (script.empty() || script[0] != OP_RETURN) return std::nullopt;
    CScript::const_iterator pc{script.begin() + 1};
    opcodetype opcode;
    std::vector<unsigned char> data;
    if (!script.GetOp(pc, opcode, data)) return std::nullopt;
    if (opcode > OP_PUSHDATA4) return std::nullopt;
    if (pc != script.end()) return std::nullopt;
    return data;
}

/** The payload of a message script carrying `tag`. */
std::optional<std::vector<unsigned char>> ParseMessageScript(const CScript& script, std::span<const unsigned char> tag)
{
    auto data{ParsePushScript(script)};
    if (!data || data->size() < tag.size()) return std::nullopt;
    if (!std::equal(tag.begin(), tag.end(), data->begin())) return std::nullopt;
    data->erase(data->begin(), data->begin() + tag.size());
    return data;
}

std::vector<unsigned char> SlotAndHash(SidechainId slot, const uint256& hash)
{
    std::vector<unsigned char> payload(4);
    WriteLE32(payload.data(), slot);
    payload.insert(payload.end(), hash.begin(), hash.end());
    return payload;
}

std::optional<std::pair<SidechainId, uint256>> ParseSlotAndHash(const std::vector<unsigned char>& payload)
{
    if (payload.size() != 4 + 32) return std::nullopt;
    return std::make_pair(ReadLE32(payload.data()), uint256{std::span<const unsigned char>{payload}.subspan(4)});
}

} // namespace

uint256 Sidechain::GetHash() const
{
    return (HashWriter{} << *this).GetHash();
}

bool Sidechain::IsValid(uint32_t max_sidechains) const
{
    return slot < max_sidechains &&
           version >= 0 && version <= SIDECHAIN_VERSION_MAX &&
           !title.empty() && title.size() <= MAX_TITLE_SIZE &&
           description.size() <= MAX_DESCRIPTION_SIZE;
}

CScript EscrowScript(SidechainId slot)
{
    std::vector<unsigned char> number(4);
    WriteLE32(number.data(), slot);
    return CScript() << OP_DRIVECHAIN << number << OP_DROP << OP_TRUE;
}

std::optional<SidechainId> ParseEscrowScript(const CScript& script)
{
    // OP_DRIVECHAIN, push of four bytes, OP_DROP, OP_TRUE
    if (script.size() != 8) return std::nullopt;
    if (script[0] != OP_DRIVECHAIN || script[1] != 4 || script[6] != OP_DROP || script[7] != OP_TRUE) return std::nullopt;
    return ReadLE32(&script[2]);
}

CScript ProposalScript(const Sidechain& sidechain)
{
    DataStream stream{};
    stream << sidechain;
    return MessageScript(PROPOSAL_TAG, MakeUCharSpan(stream));
}

std::optional<Sidechain> ParseProposalScript(const CScript& script)
{
    const auto payload{ParseMessageScript(script, PROPOSAL_TAG)};
    if (!payload) return std::nullopt;
    Sidechain sidechain;
    try {
        DataStream stream{*payload};
        stream >> sidechain;
        if (!stream.empty()) return std::nullopt;
    } catch (const std::ios_base::failure&) {
        return std::nullopt;
    }
    return sidechain;
}

CScript AckScript(const uint256& proposal_hash)
{
    return MessageScript(ACK_TAG, proposal_hash);
}

std::optional<uint256> ParseAckScript(const CScript& script)
{
    const auto payload{ParseMessageScript(script, ACK_TAG)};
    if (!payload || payload->size() != 32) return std::nullopt;
    return uint256{*payload};
}

CScript BundleScript(SidechainId slot, const uint256& bundle_hash)
{
    return MessageScript(BUNDLE_TAG, SlotAndHash(slot, bundle_hash));
}

std::optional<std::pair<SidechainId, uint256>> ParseBundleScript(const CScript& script)
{
    const auto payload{ParseMessageScript(script, BUNDLE_TAG)};
    if (!payload) return std::nullopt;
    return ParseSlotAndHash(*payload);
}

CScript VoteScript(const std::vector<uint16_t>& votes)
{
    std::vector<unsigned char> payload{VOTE_VERSION};
    for (const uint16_t vote : votes) {
        payload.push_back(vote & 0xff);
        payload.push_back(vote >> 8);
    }
    return MessageScript(VOTE_TAG, payload);
}

std::optional<std::vector<uint16_t>> ParseVoteScript(const CScript& script)
{
    const auto payload{ParseMessageScript(script, VOTE_TAG)};
    if (!payload || payload->empty() || (*payload)[0] != VOTE_VERSION) return std::nullopt;
    if ((payload->size() - 1) % 2 != 0) return std::nullopt;
    std::vector<uint16_t> votes;
    for (size_t i{1}; i < payload->size(); i += 2) {
        votes.push_back(ReadLE16(payload->data() + i));
    }
    return votes;
}

CScript BmmAcceptScript(SidechainId slot, const uint256& side_block_hash)
{
    return MessageScript(BMM_ACCEPT_TAG, SlotAndHash(slot, side_block_hash));
}

std::optional<std::pair<SidechainId, uint256>> ParseBmmAcceptScript(const CScript& script)
{
    const auto payload{ParseMessageScript(script, BMM_ACCEPT_TAG)};
    if (!payload) return std::nullopt;
    return ParseSlotAndHash(*payload);
}

CScript BmmRequestScript(const BmmRequest& request)
{
    std::vector<unsigned char> payload{SlotAndHash(request.slot, request.side_block_hash)};
    payload.insert(payload.end(), request.prev_main_block_hash.begin(), request.prev_main_block_hash.end());
    return MessageScript(BMM_REQUEST_TAG, payload);
}

std::optional<BmmRequest> ParseBmmRequestScript(const CScript& script)
{
    const auto payload{ParseMessageScript(script, BMM_REQUEST_TAG)};
    if (!payload || payload->size() != 4 + 32 + 32) return std::nullopt;
    const std::span<const unsigned char> bytes{*payload};
    BmmRequest request;
    request.slot = ReadLE32(bytes.data());
    request.side_block_hash = uint256{bytes.subspan(4, 32)};
    request.prev_main_block_hash = uint256{bytes.subspan(36, 32)};
    return request;
}

CScript DestinationScript(const std::string& destination)
{
    return CScript() << OP_RETURN << std::vector<unsigned char>(destination.begin(), destination.end());
}

std::optional<std::string> ParseDestinationScript(const CScript& script)
{
    const auto data{ParsePushScript(script)};
    if (!data) return std::nullopt;
    return std::string{data->begin(), data->end()};
}

CScript WithdrawalFeeScript(CAmount fee)
{
    std::vector<unsigned char> amount(8);
    WriteLE64(amount.data(), fee);
    return CScript() << OP_RETURN << amount;
}

std::optional<CAmount> ParseWithdrawalFeeScript(const CScript& script)
{
    const auto data{ParsePushScript(script)};
    if (!data || data->size() != 8) return std::nullopt;
    const CAmount fee{static_cast<CAmount>(ReadLE64(data->data()))};
    if (!MoneyRange(fee)) return std::nullopt;
    return fee;
}

std::optional<CMutableTransaction> BlindWithdrawalTx(const CTransaction& tx)
{
    if (tx.vin.empty() || tx.vout.empty()) return std::nullopt;
    CMutableTransaction blind{tx};
    // Replace the input with the placeholder the sidechain used when it built
    // the bundle: a null outpoint with OP_0 as scriptSig. The sequence number
    // is the sidechain's choice and stays.
    CTxIn placeholder;
    placeholder.scriptSig = CScript() << OP_0;
    placeholder.nSequence = tx.vin[0].nSequence;
    blind.vin.assign(1, placeholder);
    // Drop the change returned to the escrow.
    blind.vout.pop_back();
    return blind;
}

std::optional<uint256> BlindWithdrawalHash(const CTransaction& tx)
{
    const auto blind{BlindWithdrawalTx(tx)};
    if (!blind) return std::nullopt;
    return blind->GetHash().ToUint256();
}

std::vector<BmmRequest> GetBmmRequests(const CTransaction& tx)
{
    std::vector<BmmRequest> requests;
    for (const CTxOut& out : tx.vout) {
        if (const auto request{ParseBmmRequestScript(out.scriptPubKey)}) requests.push_back(*request);
    }
    return requests;
}

} // namespace drivechain
