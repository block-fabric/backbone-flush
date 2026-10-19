// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_KERNEL_TESTNET_ACTIVATION_H
#define BITCOIN_KERNEL_TESTNET_ACTIVATION_H

#include <limits>

/**
 * Layers on the Chains test network: the height of the Chains testnet block that activated Layers in
 * slot 7 (Consensus::SidechainParams::main_activation_height of the test network, and the PoS rules'
 * pos.main_activation_height with it).
 *
 * Slot 7 activated at Chains testnet block 4997 (18/Oct/2026): the "activationheight" that
 * `chains-cli -testnet getsidechain 7` shows. (Until it was set, 0, which a test network accepts with a
 * warning in the log, CheckSidechainActivation, and which leaves nothing of the mainchain out.) Start the
 * nodes of the test network with a release that has it: a node started before keeps the record of the
 * mainchain it began (from height 0, not from the activation; harmless, but start new datadirs).
 */
inline constexpr int TESTNET_MAIN_ACTIVATION_HEIGHT{4997};

/**
 * The fee split (Consensus::PosParams::fee_split_height, pos/feesplit.h) on the test network and signet.
 * Layers: from its first block (a new chain, every rule from genesis: no height to set later), as on the
 * main network. (FastBlocks has a placeholder here, set when its test network upgrades.)
 */
inline constexpr int TESTNET_FEE_SPLIT_HEIGHT{0};
inline constexpr int SIGNET_FEE_SPLIT_HEIGHT{0};

/**
 * Layers' DEX (Consensus::SidechainParams::dex_height, bitassets/dex.h: routes, batch clearing, price
 * accumulators, oracles) on the test network, which runs since 2026-10-18 under the rules before it: the
 * first block of Layers' test network with the DEX's rules. Never (INT_MAX) until the test network's
 * nodes upgrade; set it a little ahead of the tip then (every node upgraded before the tip gets there).
 * The main network and signet have them from their first block (0).
 */
inline constexpr int TESTNET_DEX_HEIGHT{1374};

/**
 * The second fee curve (Consensus::PosParams::fee_curve2_height, pos/feesplit.h FeeCurve::LINEAR: "the
 * fee curve of 2026-10-18 (2)") on the test network: Layers' test network runs since 2026-10-18 with the
 * first curve from genesis, so the first block of the second is a placeholder (INT_MAX: never) until the
 * coordinator sets it with TESTNET_DEX_HEIGHT (a little ahead of the tip, every node the same, before the
 * upgraded nodes start). Signet, not launched, has it from its first block, as the main network.
 */
inline constexpr int TESTNET_FEE_CURVE2_HEIGHT{1374};
inline constexpr int SIGNET_FEE_CURVE2_HEIGHT{0};

/**
 * The logos of assets on the test network (Consensus::SidechainParams::logo_height; bitassets/logo.h,
 * doc/bitassets.md "Logos"): a marker that sets a logo is valid from this BitAssets block on. A
 * placeholder (never) until the deploy: set it a few blocks above the tip then, rebuild, and upgrade
 * every node of the test network before the chain reaches it (a node of before would refuse the blocks).
 */
inline constexpr int TESTNET_LOGO_HEIGHT{4373};

/**
 * Tickers and display names on the test network (Consensus::SidechainParams::ticker_height;
 * bitassets::CheckTickerActivation, doc/bitassets.md "Tickers and display names"): from this Layers
 * block on, the name of a new asset is a ticker and a marker may set a display name. A placeholder
 * (never) until the deploy: set it a few blocks above the tip then, rebuild, and upgrade every node of
 * the test network before the chain reaches it (a node of before would refuse the blocks that set a
 * display name, and take registrations that are not tickers).
 */
inline constexpr int TESTNET_TICKER_HEIGHT{std::numeric_limits<int>::max()};

#endif // BITCOIN_KERNEL_TESTNET_ACTIVATION_H
