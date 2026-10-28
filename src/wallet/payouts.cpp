// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/payouts.h>

#include <addresstype.h>
#include <bitassets/state.h>
#include <interfaces/chain.h>
#include <pos/bond.h>
#include <sidechain/state.h>
#include <wallet/wallet.h>

#include <variant>

namespace wallet {
namespace {
/** The chain's coin an operation is paid out in, by the coinbase: its chn_to (empty for those that have none). */
std::vector<CScript> PayeesOf(const bitassets::Operation& operation)
{
    return std::visit([](const auto& op) -> std::vector<CScript> {
        using T = std::decay_t<decltype(op)>;
        if constexpr (requires { op.chn_to; }) {
            std::vector<CScript> scripts{op.chn_to};
            if constexpr (std::is_same_v<T, bitassets::LaunchToken>) {
                // The creator's part of its fees (claimed), and the addresses of its split (paid without a claim).
                scripts.push_back(op.creator);
                for (const bitassets::FeeShare& share : op.split) {
                    if (share.kind == bitassets::FeeShare::ADDRESS) scripts.push_back(share.script);
                }
            } else if constexpr (std::is_same_v<T, bitassets::CurveAsset>) {
                scripts.push_back(op.creator);
            }
            return scripts;
        } else {
            return {};
        }
    }, operation);
}
} // namespace

CoinbasePayouts::CoinbasePayouts(const CWallet& wallet) : m_wallet{&wallet}
{
    LOCK(wallet.cs_wallet);
    std::map<uint256, std::pair<int, Txid>> unbonds; // validator key -> an unbond of the wallet (its height, txid)
    std::vector<std::pair<uint256, std::pair<int, Txid>>> unbond_list;
    std::map<uint256, CScript> owner_of;
    for (const auto& [txid, wtx] : wallet.mapWallet) {
        const auto* confirmed{wtx.state<TxStateConfirmed>()};
        if (!confirmed || wtx.IsCoinBase()) continue;
        const int height{confirmed->confirmed_block_height};
        const CTransaction& tx{*wtx.GetTx()};
        for (const CTxOut& out : tx.vout) {
            if (sidechain::IsWithdrawalScript(out.scriptPubKey)) {
                if (const auto withdrawal{sidechain::ParseWithdrawalOutput(out)}) {
                    m_refunds[GetScriptForDestination(WitnessV0KeyHash{withdrawal->refund_keyhash})].emplace_back(height, txid);
                }
            } else if (pos::IsBondScript(out.scriptPubKey)) {
                if (const auto bond{pos::ParseBond(out)}) owner_of[bond->pubkey] = GetScriptForDestination(WitnessV0KeyHash{bond->owner});
            } else if (pos::IsUnbondScript(out.scriptPubKey)) {
                if (const auto unbond{pos::ParseUnbond(out)}) unbond_list.emplace_back(unbond->pubkey, std::make_pair(height, txid));
            }
        }
        const auto marker{bitassets::GetMarker(tx)};
        if (!marker || !marker->operation) continue;
        for (const CScript& script : PayeesOf(*marker->operation)) {
            if (!script.empty()) m_payees[script].emplace_back(height, txid);
        }
        // A claim pays the creator of its launch.
        if (const auto* claim{std::get_if<bitassets::ClaimLaunch>(&*marker->operation)}) {
            if (const auto launch{wallet.chain().getBitAssetsLaunch(claim->token)}) m_payees[launch->creator].emplace_back(height, txid);
        }
    }
    for (const auto& [pubkey, at] : unbond_list) {
        if (const auto owner{owner_of.find(pubkey)}; owner != owner_of.end()) m_owners[owner->second].push_back(at);
    }
    for (const auto& [pubkey, owner] : owner_of) m_owners.try_emplace(owner);
    // The addresses getdepositaddress gave out.
    for (const auto& [dest, data] : wallet.m_address_book) {
        if (!data.IsChange() && data.GetLabel() == "deposit") m_deposits.insert(GetScriptForDestination(dest));
    }
}

namespace {
/** The transaction of a payout at `height` among those that named its script: the only one of its block, else the only one before. */
std::optional<Txid> Pick(const std::vector<std::pair<int, Txid>>& named, int height)
{
    std::optional<Txid> same, before;
    size_t n_same{0}, n_before{0};
    for (const auto& [h, txid] : named) {
        if (h == height) {
            ++n_same;
            same = txid;
        }
        if (h <= height) {
            ++n_before;
            before = txid;
        }
    }
    if (n_same == 1) return same;
    if (n_same == 0 && n_before == 1) return before;
    return std::nullopt;
}
} // namespace

CoinbasePayout CoinbasePayouts::Of(const CScript& script, int height) const
{
    if (const auto it{m_refunds.find(script)}; it != m_refunds.end()) return {"refund", Pick(it->second, height)};
    if (const auto it{m_owners.find(script)}; it != m_owners.end()) return {"unbond", Pick(it->second, height)};
    if (const auto it{m_payees.find(script)}; it != m_payees.end()) return {"payout", Pick(it->second, height)};
    if (m_deposits.contains(script)) return {"deposit", std::nullopt};
    return {"chain", std::nullopt};
}

std::vector<CoinbasePayout> CoinbasePayouts::OfCoinbase(const CTransaction& coinbase, int height, const std::vector<std::string>& chain_roles) const
{
    std::vector<CoinbasePayout> out(coinbase.vout.size());
    for (size_t n{0}; n < coinbase.vout.size(); ++n) {
        if (n < chain_roles.size() && !chain_roles[n].empty()) {
            out[n].role = chain_roles[n];
        } else if (n > 0 && coinbase.vout[n].nValue > 0) {
            // Past the first output (what the block's maker is paid), a payout of the chain.
            LOCK(m_wallet->cs_wallet);
            if (m_wallet->IsMine(coinbase.vout[n])) out[n] = Of(coinbase.vout[n].scriptPubKey, height);
        }
    }
    return out;
}

} // namespace wallet
