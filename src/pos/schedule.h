// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_SCHEDULE_H
#define BITCOIN_POS_SCHEDULE_H

#include <pos/params.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace sidechain {
class Mainchain;
} // namespace sidechain

namespace pos {

/**
 * The validators of an epoch, in the order leaders are picked from: by key (bytes in order). At most
 * max_validators, the highest stakes first (ties to the lower key). Blocks of stake can be made only
 * with at least min_active_validators of them (Active).
 */
class ValidatorSet
{
public:
    ValidatorSet() = default;
    /** The validators the chain starts with (PosParams::genesis_validators). */
    explicit ValidatorSet(const Consensus::PosParams& params);
    /** These validators: one entry per key (the first given), stakes above 0, at most max_validators. */
    ValidatorSet(std::vector<Consensus::PosValidator> validators, const Consensus::PosParams& params);

    const std::vector<Consensus::PosValidator>& Validators() const { return m_validators; }
    int64_t TotalStake() const { return m_total; }
    /** Whether PoS blocks can be made: enough validators. */
    bool Active() const { return m_active; }
    /** The position of a key in the set, if it is in it. */
    std::optional<size_t> Find(const uint256& pubkey) const;
    friend bool operator==(const ValidatorSet& a, const ValidatorSet& b) { return a.m_validators == b.m_validators && a.m_active == b.m_active; }

    /** The seed of an epoch: from the epoch and the hash of a mainchain block (SeedHeight). */
    static uint256 Seed(int64_t epoch, const uint256& main_hash);
    /** The leader of a slot: picked by stake, with its epoch's seed. Nothing if the set is not active. */
    std::optional<size_t> Leader(int64_t slot, const uint256& seed) const;

private:
    std::vector<Consensus::PosValidator> m_validators;
    int64_t m_total{0};
    bool m_active{false};
};

/** The time the seed of an epoch is taken at: the start of the epoch, less seed_lookahead_seconds. */
int64_t SeedTime(int64_t epoch, const Consensus::PosParams& params);
/**
 * The slot the anchor that fixes the validator set of `epoch` comes before: the first slot of the epoch before it,
 * or, with set_lead_seconds, the slot of the time that long before the epoch starts, if earlier; after a bootstrap
 * (bootstrap_end_epoch), never below the boundary of its last epoch. Never lower for a later epoch (the sets of the
 * anchors below the boundary of the current epoch are those blocks to come can need).
 */
int64_t SetBoundary(int64_t epoch, const Consensus::PosParams& params);
/**
 * The mainchain block whose hash seeds an epoch: the highest whose median time is at or before
 * SeedTime. Known once the record has a block with a later median time (median times never go down):
 * nothing before. -1 if every block on record is later (its hash is then taken as null).
 */
std::optional<int> SeedHeight(int64_t epoch, const Consensus::PosParams& params, const sidechain::Mainchain& record);
/** The seed of an epoch, from the record; nothing if it is not known yet (SeedHeight). */
std::optional<uint256> EpochSeed(int64_t epoch, const Consensus::PosParams& params, const sidechain::Mainchain& record);

} // namespace pos

#endif // BITCOIN_POS_SCHEDULE_H
