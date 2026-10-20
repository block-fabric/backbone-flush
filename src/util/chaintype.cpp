// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/chaintype.h>

#include <util/check.h>

#include <optional>
#include <string>

std::string ChainTypeToString(ChainType chain)
{
    switch (chain) {
    case ChainType::MAIN:
        return "main";
    case ChainType::TESTNET:
        return "test";
    case ChainType::SIGNET:
        return "signet";
    case ChainType::REGTEST:
        return "regtest";
    case ChainType::ECASH:
        return "ecash";
    case ChainType::ECASH_BETANET:
        return "ecashbeta";
    case ChainType::ECASH_TESTNET:
        return "ecashtest";
    }
    assert(false);
}

std::optional<ChainType> ChainTypeFromString(std::string_view chain)
{
    if (chain == "main") {
        return ChainType::MAIN;
    } else if (chain == "test") {
        return ChainType::TESTNET;
    } else if (chain == "signet") {
        return ChainType::SIGNET;
    } else if (chain == "regtest") {
        return ChainType::REGTEST;
    } else if (chain == "ecash") {
        return ChainType::ECASH;
    } else if (chain == "ecashbeta") {
        return ChainType::ECASH_BETANET;
    } else if (chain == "ecashtest") {
        return ChainType::ECASH_TESTNET;
    } else {
        return std::nullopt;
    }
}
