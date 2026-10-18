// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_INCOME_H
#define BITCOIN_POS_INCOME_H

#include <script/script.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <vector>

class CBlock;

namespace pos {

/**
 * What an output of a block's coinbase pays (doc/fastblocks.md, "Staking income"). Not consensus:
 * what wallets show.
 *  - PRODUCER: the first output of a block of stake, the producer's share of its fees;
 *  - ANCHOR: the pot (and a share of the reserve of penalties) paid to an anchor's payee, in the block
 *    that takes the peg actions (an acknowledgement, or a merged mined block);
 *  - REPORTER: the reward for a proof that a leader signed two blocks for a slot, paid at the peg step
 *    after the block that carried the proof, to that block's first output's script;
 *  - VALIDATOR: a validator's share of the validator pool (fee split), paid at the first anchor after
 *    its epoch to its bond's owner: the payouts the zero-value marker (PoolSharesMarker) counts;
 *  - OTHER: the rest (a merged mined block's first output, deposits, refunds, unbonds, data).
 */
enum class CoinbaseRole : uint8_t { OTHER, PRODUCER, ANCHOR, REPORTER, VALIDATOR };

/** Whether a block takes the peg actions as an anchor: a merged mined block, or a block of stake with an acknowledgement. */
bool IsAnchorBlock(const CBlock& block);

/** Whom an anchor block pays the pot to: the ticket's payee, or a merged mined block's first output's script. */
std::optional<CScript> AnchorPayee(const CBlock& block);

/**
 * The scripts a block's proofs earn a reward for (one per proof, all to its first output's script).
 * @param proposer  for a block of stake, its leader if known: its proofs against itself earn nothing
 * @param genesis   the chain's genesis block hash (which the leaders' signatures commit to)
 */
std::vector<CScript> ReporterScripts(const CBlock& block, const std::optional<uint256>& proposer, const uint256& genesis);

/**
 * The role of each output of a block's coinbase.
 * @param reporters  for an anchor block, the scripts owed rewards for the proofs of the blocks since the
 *                   anchor before it, that one included (ReporterScripts of each): the peg step pays them
 *
 * The peg step pays, after the first output, what the mainchain gave rise to (deposits, bundle outcomes),
 * then the pot to the payee, the unbonds due, the rewards, the validators' shares of the pool (after their
 * marker) and the refunds, in this order. So the last outputs to a reporter's script before the marker
 * are its rewards, and the last other one to the payee before them is the pot. (Not
 * exact when the payouts of a block passed the cap of a block and some waited for the next, nor for a
 * refund to the same script in the same block.)
 */
std::vector<CoinbaseRole> CoinbaseRoles(const CBlock& block, const std::vector<CScript>& reporters);

} // namespace pos

#endif // BITCOIN_POS_INCOME_H
