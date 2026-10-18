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
 * Slot 7 is proposed on the Chains testnet (18/Oct/2026); until it activates this is 0, which the
 * node accepts on a test network with a warning in its log (CheckSidechainActivation) and which
 * leaves nothing of the mainchain out. Once the slot is active, set it to the "activationheight" that
 * `chains-cli -testnet getsidechain 7` shows, rebuild, and start the nodes of the test network
 * only then: a node started before keeps the record of the mainchain it began (from height 0, not from
 * the activation; harmless, but start new datadirs with the height set).
 */
inline constexpr int TESTNET_MAIN_ACTIVATION_HEIGHT{0};

/**
 * The fee split (Consensus::PosParams::fee_split_height, pos/feesplit.h) on the test network and signet.
 * Layers: from its first block (a new chain, every rule from genesis: no height to set later), as on the
 * main network. (FastBlocks has a placeholder here, set when its test network upgrades.)
 */
inline constexpr int TESTNET_FEE_SPLIT_HEIGHT{0};
inline constexpr int SIGNET_FEE_SPLIT_HEIGHT{0};

#endif // BITCOIN_KERNEL_TESTNET_ACTIVATION_H
