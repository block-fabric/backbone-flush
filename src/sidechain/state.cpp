// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sidechain/state.h>

#include <addresstype.h>
#include <common/signmessage.h>
#include <crypto/common.h>
#include <drivechain/sidechain.h>
#include <key_io.h>
#include <pubkey.h>
#include <sidechain/mainchain.h>
#include <util/strencodings.h>

#include <algorithm>

namespace sidechain {
namespace {

/** If `script` is OP_RETURN followed by a single push that starts with `tag`, return what follows the tag. */
std::optional<std::vector<unsigned char>> ParseTagged(const CScript& script, std::span<const unsigned char> tag)
{
    CScript::const_iterator pc{script.begin()};
    opcodetype opcode;
    std::vector<unsigned char> data;
    if (!script.GetOp(pc, opcode) || opcode != OP_RETURN) return std::nullopt;
    if (!script.GetOp(pc, opcode, data) || opcode > OP_PUSHDATA4 || pc != script.end()) return std::nullopt;
    if (data.size() < tag.size() || !std::equal(tag.begin(), tag.end(), data.begin())) return std::nullopt;
    data.erase(data.begin(), data.begin() + tag.size());
    return data;
}

CScript Tagged(std::span<const unsigned char> tag, std::span<const unsigned char> payload)
{
    std::vector<unsigned char> data{tag.begin(), tag.end()};
    data.insert(data.end(), payload.begin(), payload.end());
    return CScript() << OP_RETURN << data;
}

/**
 * The transaction that pays `withdrawals` on the mainchain, in the blind form the mainchain votes on.
 *
 * The mainchain never votes twice on the same transaction. A bundle that pays
 * the withdrawals of one that failed has to differ from it, which the height
 * of the block that commits to it, put in the lock time, sees to. The lock
 * time has no other effect, as the input of the transaction is final.
 */
CMutableTransaction BuildBundle(const std::vector<const Withdrawal*>& withdrawals, int height)
{
    CMutableTransaction tx;
    tx.version = 2;
    tx.nLockTime = static_cast<uint32_t>(height);
    tx.vin.resize(1);
    tx.vin[0].prevout.SetNull();
    tx.vin[0].scriptSig = CScript() << OP_0;
    CAmount fee{0};
    for (const Withdrawal* withdrawal : withdrawals) fee += withdrawal->main_fee;
    tx.vout.emplace_back(0, drivechain::DestinationScript(drivechain::WITHDRAWAL_RETURN_DEST));
    tx.vout.emplace_back(0, drivechain::WithdrawalFeeScript(fee));
    for (const Withdrawal* withdrawal : withdrawals) tx.vout.emplace_back(withdrawal->amount, withdrawal->main_script);
    return tx;
}

} // namespace

CScript WithdrawalScript(CAmount main_fee, const uint160& refund_keyhash, const CScript& main_script)
{
    std::vector<unsigned char> payload(8);
    WriteLE64(payload.data(), static_cast<uint64_t>(main_fee));
    payload.insert(payload.end(), refund_keyhash.begin(), refund_keyhash.end());
    payload.insert(payload.end(), main_script.begin(), main_script.end());
    return Tagged(WITHDRAWAL_TAG, payload);
}

bool IsWithdrawalScript(const CScript& script)
{
    return ParseTagged(script, WITHDRAWAL_TAG).has_value();
}

std::optional<Withdrawal> ParseWithdrawalOutput(const CTxOut& out)
{
    const auto payload{ParseTagged(out.scriptPubKey, WITHDRAWAL_TAG)};
    if (!payload || payload->size() <= 28 || payload->size() > 28 + MAX_MAIN_SCRIPT_SIZE) return std::nullopt;
    Withdrawal withdrawal;
    const uint64_t fee{ReadLE64(payload->data())};
    if (fee > static_cast<uint64_t>(MAX_MONEY) || !MoneyRange(out.nValue) || static_cast<CAmount>(fee) >= out.nValue) return std::nullopt;
    withdrawal.main_fee = static_cast<CAmount>(fee);
    withdrawal.amount = out.nValue - withdrawal.main_fee;
    std::copy(payload->begin() + 8, payload->begin() + 28, withdrawal.refund_keyhash.begin());
    withdrawal.main_script = CScript{payload->begin() + 28, payload->end()};
    // Paying an output that cannot be spent would only take the coins out of the escrow for nobody.
    if (withdrawal.main_script.IsUnspendable()) return std::nullopt;
    return withdrawal;
}

CScript RefundScript(const RefundRequest& request)
{
    const uint256& txid{request.withdrawal.hash.ToUint256()};
    std::vector<unsigned char> payload{txid.begin(), txid.end()};
    payload.resize(36);
    WriteLE32(payload.data() + 32, request.withdrawal.n);
    payload.insert(payload.end(), request.signature.begin(), request.signature.end());
    return Tagged(REFUND_TAG, payload);
}

std::optional<RefundRequest> ParseRefundScript(const CScript& script)
{
    const auto payload{ParseTagged(script, REFUND_TAG)};
    if (!payload || payload->size() != 36 + CPubKey::COMPACT_SIGNATURE_SIZE) return std::nullopt;
    RefundRequest request;
    request.withdrawal = COutPoint{Txid::FromUint256(uint256{std::span{*payload}.first(32)}), ReadLE32(payload->data() + 32)};
    request.signature.assign(payload->begin() + 36, payload->end());
    return request;
}

std::string RefundMessage(const COutPoint& withdrawal)
{
    return strprintf("Refund withdrawal %s:%u", withdrawal.hash.GetHex(), withdrawal.n);
}

CScript BundleCommitScript(const uint256& hash)
{
    return Tagged(BUNDLE_COMMIT_TAG, hash);
}

std::optional<uint256> ParseBundleCommitScript(const CScript& script)
{
    const auto payload{ParseTagged(script, BUNDLE_COMMIT_TAG)};
    if (!payload || payload->size() != 32) return std::nullopt;
    return uint256{std::span{*payload}};
}

CScript DepositScript(const std::string& destination)
{
    const CTxDestination dest{DecodeDestination(destination)};
    if (!IsValidDestination(dest)) return CScript() << OP_RETURN;
    return GetScriptForDestination(dest);
}

void State::Remove(const COutPoint& outpoint, StateUndo& undo)
{
    const auto it{m_withdrawals.find(outpoint)};
    if (it == m_withdrawals.end()) return;
    undo.removed.push_back(it->second);
    m_withdrawals.erase(it);
}

bool State::InBundle(const COutPoint& withdrawal) const
{
    return m_bundle && std::find(m_bundle->withdrawals.begin(), m_bundle->withdrawals.end(), withdrawal) != m_bundle->withdrawals.end();
}

bool State::ApplyMainEvents(int main_height, const Mainchain& mainchain, int height, StateUndo& undo,
                            std::vector<CTxOut>& payouts, std::string& reject_reason)
{
    if (main_height < m_main_height) {
        reject_reason = "bad-sc-main-height";
        return false;
    }
    for (int h{m_main_height + 1}; h <= main_height; ++h) {
        const auto main_block{mainchain.GetBlock(h)};
        if (!main_block) {
            reject_reason = "bad-sc-main-unknown";
            return false;
        }
        for (const MainBundleEvent& event : main_block->bundles) {
            if (!m_bundle || event.hash != m_bundle->hash) continue;
            if (event.paid) {
                for (const COutPoint& outpoint : m_bundle->withdrawals) Remove(outpoint, undo);
            } else {
                // The withdrawals wait for the next bundle, or for their owners to take them back.
                m_last_failure_height = height;
            }
            m_bundle.reset();
        }
        for (const MainDeposit& deposit : main_block->deposits) {
            // The change of a withdrawal is an escrow change too, but no deposit.
            if (deposit.amount <= 0 || deposit.destination == drivechain::WITHDRAWAL_RETURN_DEST) continue;
            if (!MoneyRange(deposit.amount)) {
                reject_reason = "bad-sc-deposit-amount";
                return false;
            }
            payouts.emplace_back(deposit.amount, DepositScript(deposit.destination));
        }
    }
    m_main_height = main_height;
    return true;
}

std::optional<CMutableTransaction> State::NextBundle(int height, const Consensus::SidechainParams& params, std::vector<COutPoint>* withdrawals) const
{
    if (m_bundle || m_withdrawals.empty()) return std::nullopt;
    if (m_last_failure_height >= 0 && height - m_last_failure_height < params.bundle_retry_delay) return std::nullopt;

    // Those that offer mainchain miners the most go first; the rest wait for the next bundle.
    std::vector<const Withdrawal*> chosen;
    chosen.reserve(m_withdrawals.size());
    for (const auto& [outpoint, withdrawal] : m_withdrawals) chosen.push_back(&withdrawal);
    std::sort(chosen.begin(), chosen.end(), [](const Withdrawal* a, const Withdrawal* b) {
        if (a->main_fee != b->main_fee) return a->main_fee > b->main_fee;
        return a->outpoint < b->outpoint;
    });
    if (chosen.size() > params.max_bundle_withdrawals) chosen.resize(params.max_bundle_withdrawals);
    if (withdrawals) {
        withdrawals->clear();
        for (const Withdrawal* withdrawal : chosen) withdrawals->push_back(withdrawal->outpoint);
    }
    return BuildBundle(chosen, height);
}

std::optional<CMutableTransaction> State::BundleTx() const
{
    if (!m_bundle) return std::nullopt;
    std::vector<const Withdrawal*> withdrawals;
    for (const COutPoint& outpoint : m_bundle->withdrawals) {
        const auto it{m_withdrawals.find(outpoint)};
        if (it == m_withdrawals.end()) return std::nullopt;
        withdrawals.push_back(&it->second);
    }
    return BuildBundle(withdrawals, m_bundle->height);
}

bool State::StartBundle(const uint256& hash, int height, const Consensus::SidechainParams& params, StateUndo& undo, std::string& reject_reason)
{
    std::vector<COutPoint> withdrawals;
    const auto bundle{NextBundle(height, params, &withdrawals)};
    if (!bundle) {
        reject_reason = "bad-sc-bundle-not-allowed";
        return false;
    }
    if (bundle->GetHash().ToUint256() != hash) {
        reject_reason = "bad-sc-bundle-hash";
        return false;
    }
    m_bundle = PendingBundle{hash, std::move(withdrawals), height};
    return true;
}

bool State::CheckRefund(const RefundRequest& request, std::string& reject_reason) const
{
    const auto it{m_withdrawals.find(request.withdrawal)};
    if (it == m_withdrawals.end()) {
        reject_reason = "bad-sc-refund-unknown";
        return false;
    }
    if (InBundle(request.withdrawal)) {
        reject_reason = "bad-sc-refund-in-bundle";
        return false;
    }
    CPubKey pubkey;
    if (!pubkey.RecoverCompact(MessageHash(RefundMessage(request.withdrawal)), request.signature) || !pubkey.IsCompressed() ||
        pubkey.GetID() != CKeyID{it->second.refund_keyhash}) {
        reject_reason = "bad-sc-refund-signature";
        return false;
    }
    return true;
}

bool State::ApplyTx(const CTransaction& tx, int height, const Consensus::SidechainParams& params, StateUndo& undo,
                    std::vector<CTxOut>& payouts, std::string& reject_reason)
{
    for (uint32_t n{0}; n < tx.vout.size(); ++n) {
        const CTxOut& out{tx.vout[n]};
        if (IsWithdrawalScript(out.scriptPubKey)) {
            auto withdrawal{ParseWithdrawalOutput(out)};
            if (!withdrawal) {
                reject_reason = "bad-sc-withdrawal";
                return false;
            }
            if (withdrawal->amount < params.min_withdrawal) {
                reject_reason = "bad-sc-withdrawal-amount";
                return false;
            }
            withdrawal->outpoint = COutPoint{tx.GetHash(), n};
            withdrawal->height = height;
            undo.added.push_back(withdrawal->outpoint);
            m_withdrawals.emplace(withdrawal->outpoint, std::move(*withdrawal));
        } else if (const auto refund{ParseRefundScript(out.scriptPubKey)}) {
            if (!CheckRefund(*refund, reject_reason)) return false;
            const Withdrawal& withdrawal{m_withdrawals.at(refund->withdrawal)};
            payouts.emplace_back(withdrawal.Burned(), GetScriptForDestination(WitnessV0KeyHash{withdrawal.refund_keyhash}));
            Remove(refund->withdrawal, undo);
        }
    }
    return true;
}

bool State::ConnectBlock(const CBlock& block, int height, const Consensus::SidechainParams& params, const Mainchain& mainchain,
                         StateUndo& undo, CAmount& minted, std::string& reject_reason)
{
    undo = StateUndo{};
    undo.main_height = m_main_height;
    undo.bundle = m_bundle;
    undo.last_failure_height = m_last_failure_height;

    // The block follows the mainchain up to the block before the one that committed to it.
    const auto bmm_height{mainchain.BmmHeight(block.GetHash())};
    if (!bmm_height) {
        reject_reason = "bmm-unknown";
        return false;
    }
    std::vector<CTxOut> payouts;
    if (!ApplyMainEvents(*bmm_height - 1, mainchain, height, undo, payouts, reject_reason)) return false;

    // A new bundle takes the withdrawals that were waiting before this block.
    std::optional<uint256> commitment;
    for (const CTxOut& out : block.vtx[0]->vout) {
        const auto hash{ParseBundleCommitScript(out.scriptPubKey)};
        if (!hash) continue;
        if (commitment) {
            reject_reason = "bad-sc-bundle-multiple";
            return false;
        }
        commitment = hash;
    }
    if (commitment && !StartBundle(*commitment, height, params, undo, reject_reason)) return false;

    for (size_t i{1}; i < block.vtx.size(); ++i) {
        if (!ApplyTx(*block.vtx[i], height, params, undo, payouts, reject_reason)) return false;
    }

    // The coinbase pays the deposits and the refunds, right after its first output.
    const std::vector<CTxOut>& coinbase{block.vtx[0]->vout};
    if (coinbase.size() < 1 + payouts.size()) {
        reject_reason = "bad-sc-payouts-missing";
        return false;
    }
    minted = 0;
    for (size_t i{0}; i < payouts.size(); ++i) {
        if (coinbase[1 + i] != payouts[i]) {
            reject_reason = "bad-sc-payout";
            return false;
        }
        minted += payouts[i].nValue;
        if (!MoneyRange(minted)) {
            reject_reason = "bad-sc-payout-amount";
            return false;
        }
    }
    return true;
}

void State::DisconnectBlock(const StateUndo& undo)
{
    // A withdrawal that the block both added and removed ends up removed.
    for (const Withdrawal& withdrawal : undo.removed) m_withdrawals.emplace(withdrawal.outpoint, withdrawal);
    for (const COutPoint& outpoint : undo.added) m_withdrawals.erase(outpoint);
    m_main_height = undo.main_height;
    m_bundle = undo.bundle;
    m_last_failure_height = undo.last_failure_height;
}

} // namespace sidechain
