// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_PAYOUTS_H
#define BITCOIN_WALLET_PAYOUTS_H

#include <primitives/transaction.h>
#include <script/script.h>
#include <sync.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace wallet {
class CWallet;

/**
 * What an output of a coinbase pays the wallet, where the chain names no role for it (pos::CoinbaseRole::OTHER, past
 * the first output): not consensus, what the wallet shows instead of "mined". From what the wallet knows of its own
 * transactions and addresses:
 *  - "refund": a withdrawal of the wallet given back (its refund script);
 *  - "unbond": what a bond of the wallet unbonded (paid to its owner);
 *  - "payout": what an order or a launch of the wallet is paid in the chain's coin (its chn_to, the creator of a launch,
 *    an address of a launch's fee split): a sale, a refund of an order, a withdrawal from a pool, a creator's fees;
 *  - "deposit": a deposit from the mainchain (an address of getdepositaddress);
 *  - "chain": anything else the chain pays (a deposit to another address, an order of another wallet).
 * `source`: the transaction that gave rise to it, when the wallet can tell it alone: the one of the payout's own block
 * (from upgrade v7 the assets' payouts are paid in the block of their transaction), else the only one that named the
 * script before; none if several did.
 */
struct CoinbasePayout {
    std::string role;
    std::optional<Txid> source;
};

/** The wallet's knowledge of what its coinbase outputs pay, read once from its transactions (a list's worth of queries). */
class CoinbasePayouts
{
public:
    explicit CoinbasePayouts(const CWallet& wallet);
    /** An output past the first of a coinbase of the block at `height`, to `script`, that the chain names no role. */
    CoinbasePayout Of(const CScript& script, int height) const;
    /** The role of the outputs of a confirmed coinbase of the wallet: those the chain names (`chain_roles`, "" for OTHER) kept, the others past the first worked out. */
    std::vector<CoinbasePayout> OfCoinbase(const CTransaction& coinbase, int height, const std::vector<std::string>& chain_roles) const;

private:
    const CWallet* m_wallet;
    //! The scripts the wallet's orders and launches are paid to, with the transactions (and their heights) that named them.
    std::map<CScript, std::vector<std::pair<int, Txid>>> m_payees;
    std::map<CScript, std::vector<std::pair<int, Txid>>> m_refunds;
    std::map<CScript, std::vector<std::pair<int, Txid>>> m_owners;
    std::set<CScript> m_deposits;
};

} // namespace wallet

#endif // BITCOIN_WALLET_PAYOUTS_H
