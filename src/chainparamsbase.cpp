// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparamsbase.h>

#include <common/args.h>
#include <tinyformat.h>
#include <util/chaintype.h>

#include <cassert>

void SetupChainParamsBaseOptions(ArgsManager& argsman)
{
    argsman.AddArg("-chain=<chain>", "Use the chain <chain> (default: main). Allowed values: " LIST_CHAIN_NAMES ". "
                   "main, test, signet and regtest are sidechains of the Chains (CHN) mainchain (chainsd), in the data directory of this program; "
                   "ecash, ecashbeta and ecashtest sidechains of Backbone (backboned) on eCash's main network, its betanet and its test network, in the Backbone data directory of this program (~/.backbone-<name>).", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-regtest", "Enter regression test mode, which uses a special chain in which blocks can be solved instantly. "
                 "This is intended for regression testing tools and app development. Equivalent to -chain=regtest.", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-posvalidators=<key[:stake],...>", "FastBlocks: the fixed validator set of a regtest sidechain, as 32-byte x-only public keys in hex, each with an optional stake (1 by default). Without it the chain has no blocks of stake. (regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-posfinalanchors=<n>", "FastBlocks: wallets and RPCs call a block final once <n> anchors cover it (not consensus; default: 1; regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-posslotseconds=<n>", "FastBlocks: slots of <n> seconds, and stalls of 30 slots (default: 1; regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-posuncheckedinbound=<n>", "FastBlocks: an inbound peer may hold <n> headers the node cannot check yet (not consensus; default: 2000, as an outbound peer; regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-posuncheckedheaders=<n>", "FastBlocks: an outbound peer may hold <n> headers the node cannot check yet (not consensus; default: 2000; regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-posuncheckedbytes=<n>", "FastBlocks: the blocks of the headers the node cannot check yet that a peer holds may weigh <n> bytes (not consensus; default: 33554432; regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-posseedlookahead=<n>", "FastBlocks: the leader seed of an epoch is the hash of the highest mainchain block whose median time is at least <n> seconds before the epoch starts (default: 3600; regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-sidechainslot=<n>", "Make the regtest chain a sidechain in slot <n> of the mainchain, with the rules of one: blocks are blind merged mined and coins come from deposits. (regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-testmainenforcer", "With -sidechainslot: the mainchain follows the drivechain rules of the bip300301_enforcer, as on the eCash networks (backboned -testecash): no single payout, and the parameters tied to its blocks scaled as there (see SidechainParams). (regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-sidechainactivationheight=<n>", "With -sidechainslot: the height of the mainchain block that activated the sidechain in its slot (SidechainParams::main_activation_height; default: 0). (regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-testactivationheight=name@height.", "Set the activation height of 'name' (segwit, bip34, dersig, cltv, csv; FastBlocks: feesplit, feecurve2; BitAssets: logo, ticker; Layers: dex; regtest only). (test-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-testdrivechainparam=name@value", "Set the drivechain parameter 'name' (a field of DrivechainParams, such as withdrawal_period) on regtest. (test-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-testnet", "Use the test chain. Equivalent to -chain=test.", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-vbparams=deployment:start:end[:min_activation_height]", "Use given start/end times and min_activation_height for specified version bits deployment (test-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-signet", "Use the signet chain. Equivalent to -chain=signet. Note that the network is defined by the -signetchallenge parameter", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-signetchallenge", "Blocks must satisfy the given script to be considered valid (only for signet networks; defaults to the global default signet test network challenge)", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-signetseednode", "Specify a seed node for the signet network, in the hostname[:port] format, e.g. sig.net:1234 (may be used multiple times to specify multiple seed nodes; defaults to the global default signet test network seed node(s))", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::CHAINPARAMS);
}

static std::unique_ptr<CBaseChainParams> globalChainBaseParams;

const CBaseChainParams& BaseParams()
{
    assert(globalChainBaseParams);
    return *globalChainBaseParams;
}

/**
 * Port numbers for incoming Tor connections (8334, 18334, 38334, 48334, 18445) have
 * been chosen arbitrarily to keep ranges of used ports tight.
 */
std::unique_ptr<CBaseChainParams> CreateBaseChainParams(const ChainType chain)
{
    switch (chain) {
    case ChainType::MAIN:
        return std::make_unique<CBaseChainParams>("", 9254);
    case ChainType::TESTNET:
        return std::make_unique<CBaseChainParams>("testnet", 19254);
    case ChainType::SIGNET:
        return std::make_unique<CBaseChainParams>("signet", 39254);
    case ChainType::REGTEST:
        return std::make_unique<CBaseChainParams>("regtest", 29254);
    // A sidechain of Backbone on eCash: the RPC port of its Chains network less 1000 (8x54), its betanet's
    // and its test network's in 38x54 and 48x54 (as Backbone's own test network, 48532). The main network
    // at the root of the Backbone data directory of this program, betanet and the test network in the
    // folders Backbone has them in.
    case ChainType::ECASH:
        return std::make_unique<CBaseChainParams>("", 8254);
    case ChainType::ECASH_BETANET:
        return std::make_unique<CBaseChainParams>("betanet", 38254);
    case ChainType::ECASH_TESTNET:
        return std::make_unique<CBaseChainParams>("testnet4", 48254);
    }
    assert(false);
}

void SelectBaseParams(const ChainType chain)
{
    globalChainBaseParams = CreateBaseChainParams(chain);
    gArgs.SelectConfigNetwork(ChainTypeToString(chain));
}
