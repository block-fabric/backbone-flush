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
 *
 * Layers' test network restarted on 19/Oct/2026 from a new genesis, every rule from its first block (the
 * DEX, the fee split, the second fee curve, logos, tickers: no height of the test network to set later);
 * slot 7 stays the one activated at Chains testnet block 4997. Only this height is the test network's own.
 */
inline constexpr int TESTNET_MAIN_ACTIVATION_HEIGHT{4997};

/**
 * The DEX's pool kinds (fee tiers, stable and weighted pools; Consensus::SidechainParams::dex_curves_height)
 * on the running test network: a change of its rules, so a height every node runs a release with before
 * it is reached. Never until a deployment names one (a node of this release then follows the test network
 * exactly as one of before). The next restart of the test network from a new genesis takes them from its
 * first block (0), as every other rule, and this goes.
 */
inline constexpr int TESTNET_DEX_CURVES_HEIGHT{std::numeric_limits<int>::max()};

#endif // BITCOIN_KERNEL_TESTNET_ACTIVATION_H
