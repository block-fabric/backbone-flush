// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_CHAINTYPE_H
#define BITCOIN_UTIL_CHAINTYPE_H

#include <optional>
#include <string>
#include <string_view>

enum class ChainType {
    //! The networks of a sidechain of the Chains (CHN) mainchain: legacy, during the move to Backbone.
    MAIN,
    TESTNET,
    SIGNET,
    REGTEST,
    //! The networks of a sidechain of Backbone, on eCash (ECX): of its main network, of its betanet and of its
    //! test network (the mainchain's -chain=ecash, ecashbeta and ecashtest).
    ECASH,
    ECASH_BETANET,
    ECASH_TESTNET,
};

std::string ChainTypeToString(ChainType chain);

std::optional<ChainType> ChainTypeFromString(std::string_view chain);

/** Whether `chain` is a network of a sidechain of Backbone on eCash, rather than of the Chains mainchain. */
constexpr bool IsEcashChain(ChainType chain)
{
    return chain == ChainType::ECASH || chain == ChainType::ECASH_BETANET || chain == ChainType::ECASH_TESTNET;
}

#endif // BITCOIN_UTIL_CHAINTYPE_H
