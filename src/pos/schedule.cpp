// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/schedule.h>

#include <arith_uint256.h>
#include <hash.h>
#include <pos/header.h>
#include <sidechain/mainchain.h>

#include <algorithm>

namespace pos {

ValidatorSet::ValidatorSet(const Consensus::PosParams& params) : ValidatorSet{params.genesis_validators, params} {}

ValidatorSet::ValidatorSet(std::vector<Consensus::PosValidator> all, const Consensus::PosParams& params)
{
    // One entry per key: a key given twice counts once, with the stake given first.
    std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.pubkey < b.pubkey; });
    all.erase(std::unique(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.pubkey == b.pubkey; }), all.end());
    std::erase_if(all, [](const auto& v) { return v.stake <= 0; });
    // The largest stakes take part, ties to the lower key.
    std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.stake > b.stake; });
    if (all.size() > params.max_validators) all.resize(params.max_validators);
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.pubkey < b.pubkey; });
    for (const auto& validator : all) {
        m_validators.push_back(validator);
        m_total += validator.stake;
    }
    m_active = !m_validators.empty() && m_validators.size() >= params.min_active_validators;
}

std::optional<size_t> ValidatorSet::Find(const uint256& pubkey) const
{
    for (size_t i{0}; i < m_validators.size(); ++i) {
        if (m_validators[i].pubkey == pubkey) return i;
    }
    return std::nullopt;
}

uint256 ValidatorSet::Seed(int64_t epoch, const uint256& main_hash)
{
    return (TaggedHash("hpos/seed") << epoch << main_hash).GetSHA256();
}

std::optional<size_t> ValidatorSet::Leader(int64_t slot, const uint256& seed) const
{
    if (!m_active || m_total <= 0) return std::nullopt;
    const arith_uint256 draw{UintToArith256((TaggedHash("hpos/leader") << seed << slot).GetSHA256())};
    // draw mod total, in integers.
    const arith_uint256 total{static_cast<uint64_t>(m_total)};
    const uint64_t r{(draw - (draw / total) * total).GetLow64()};
    uint64_t cumulative{0};
    for (size_t i{0}; i < m_validators.size(); ++i) {
        cumulative += static_cast<uint64_t>(m_validators[i].stake);
        if (cumulative > r) return i;
    }
    return m_validators.size() - 1; // not reached: r < total
}

int64_t SeedTime(int64_t epoch, const Consensus::PosParams& params)
{
    return SlotTime(epoch * params.slots_per_epoch, params) - params.SeedLookahead(epoch);
}

int64_t SetBoundary(int64_t epoch, const Consensus::PosParams& params)
{
    const int64_t by_epoch{(epoch - 1) * params.slots_per_epoch};
    const int64_t lead{params.SetLead(epoch)};
    const int64_t boundary{lead <= 0 ? by_epoch : std::min(by_epoch, SlotOf(SlotTime(epoch * params.slots_per_epoch, params) - lead, params))};
    // After the bootstrap, never below its last epoch's boundary (the epoch before bootstrap_end_epoch): the boundary never
    // goes down from one epoch to the next, so no set fixed is taken back, and the epochs from the end of the bootstrap
    // share that set until the long lead passes it.
    if (params.bootstrap_end_epoch > 0 && !params.Bootstrap(epoch)) return std::max(boundary, (params.bootstrap_end_epoch - 2) * params.slots_per_epoch);
    return boundary;
}

std::optional<int> SeedHeight(int64_t epoch, const Consensus::PosParams& params, const sidechain::Mainchain& record)
{
    const int64_t time{SeedTime(epoch, params)};
    const int tip{record.Height()};
    if (tip < 0) return std::nullopt;
    // Known once a block after it has a later median time: then no block to come can be the highest
    // at or before the time (median times never go down along a chain).
    const auto last{record.GetBlock(tip)};
    if (!last || last->time <= time) return std::nullopt;
    // Below the block that activated the chain in its slot, no block: the record may start there
    // (Mainchain::Base), or below it (a record kept from an earlier release), with the same seed.
    const int height{record.TimeHeight(time).value_or(-1)};
    return height < params.main_activation_height ? -1 : height;
}

std::optional<uint256> EpochSeed(int64_t epoch, const Consensus::PosParams& params, const sidechain::Mainchain& record)
{
    const auto height{SeedHeight(epoch, params, record)};
    if (!height) return std::nullopt;
    uint256 main_hash;
    if (*height >= 0) {
        const auto hash{record.HashAt(*height)};
        if (!hash) return std::nullopt;
        main_hash = *hash;
    }
    return ValidatorSet::Seed(epoch, main_hash);
}

} // namespace pos
