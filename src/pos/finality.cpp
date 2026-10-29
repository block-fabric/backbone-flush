// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/finality.h>

#include <chain.h>
#include <pos/forkchoice.h>
#include <pos/hooks.h>
#include <sidechain/mainchain.h>
#include <validation.h>

#include <algorithm>
#include <deque>
#include <limits>
#include <queue>
#include <set>

namespace pos {

std::vector<ChainAnchor> RecentAnchors(const FastBlocksHooks& hooks, const CBlockIndex& tip, size_t count, int floor)
{
    AssertLockHeld(::cs_main);
    std::vector<ChainAnchor> anchors;
    // Between two anchors there are at most max_unanchored_slots blocks of stake (and BMM blocks are
    // anchors), so the walk is short.
    for (const CBlockIndex* index{&tip}; index && index->nHeight > floor && anchors.size() < count; index = index->pprev) {
        const CBlockIndex* anchored{hooks.AnchoredBy(*index)};
        if (!anchored) continue;
        anchors.push_back({.block = index, .anchored = anchored, .main_height = ChainKey::FromWork(index->nChainWork).last_anchor_height});
    }
    return anchors;
}

std::optional<int> FastBlocksHooks::FixedFrom(const CBlockIndex& block, const CChain& chain, const sidechain::Mainchain& record) const
{
    AssertLockHeld(::cs_main);
    // A BMM block: the mainchain committed to it, to its hash alone; only a reorg of the mainchain drops it.
    if (!block.m_ext) {
        const auto committed{record.CommittedHeight(block.GetBlockHash())};
        return committed && *committed > 0 ? committed : std::nullopt;
    }
    // A block of stake. Up from the block: the anchors are in the order of their commitments, so the first one found that covers
    // it has the earliest. (Short: an anchor comes at least every max_unanchored_slots blocks of stake.)
    for (int height{block.nHeight + 1}; height <= chain.Height(); ++height) {
        const CBlockIndex* index{chain[height]};
        const CBlockIndex* anchored{AnchoredBy(*index)};
        if (!anchored || anchored->nHeight < block.nHeight) continue;
        // The height of its commitment as the record has it now, never as the block index noted it: a BMM
        // block's commitment to itself; an acknowledgement's to its ticket, at the height it names (nNonce).
        // A commitment not on record (any more), or elsewhere: nothing fixes the block yet.
        if (!index->m_ext) {
            const auto committed{record.CommittedHeight(index->GetBlockHash())};
            return committed && *committed > 0 ? committed : std::nullopt;
        }
        const auto commitment{CommitmentOf(*index)};
        const auto committed{commitment ? record.CommittedHeight(*commitment) : std::nullopt};
        if (!committed || *committed <= 0 || static_cast<int64_t>(index->nNonce) != *committed) return std::nullopt;
        return committed;
    }
    return std::nullopt;
}

std::vector<int> NetAnchoredHeights(const std::vector<ChainAnchor>& anchors, const std::vector<SkippedCommitment>& skipped, int count, const std::optional<DeepForks>& deep)
{
    std::vector<int> heights;
    if (anchors.empty() || count <= 0) return heights;
    // The anchors a chain that leaves out the blocks above F can have, against those of this chain:
    // it shares the anchors acknowledged at or below F, and may acknowledge again those that anchor a
    // block at or below F (both have them: they cancel), and every commitment this chain skipped
    // after the last anchor it shares. What this chain has more: the anchors of blocks above F, less
    // the commitments skipped since. (Forks below the oldest acknowledgement walked: `deep`.)
    const int oldest{anchors.back().block->nHeight};
    const auto margin{[&](int fork) -> std::optional<int> {
        if (fork < oldest) return std::nullopt;
        int above{0};
        uint32_t shared_height{0};
        for (const ChainAnchor& anchor : anchors) {
            if (anchor.anchored->nHeight > fork) ++above;
            if (anchor.block->nHeight <= fork) shared_height = std::max(shared_height, anchor.main_height);
        }
        const int after{static_cast<int>(std::count_if(skipped.begin(), skipped.end(), [&](const SkippedCommitment& c) { return c.main_height > shared_height && c.UsableAt(fork); }))};
        return above - after;
    }};
    // The margin of a block at `height`: the least over the forks below it. It changes only where an
    // anchored block or an acknowledgement is, so those are the forks to try.
    const auto net{[&](int height) {
        int least{std::numeric_limits<int>::max()};
        const auto take{[&](int fork) { if (const auto m{margin(fork)}) least = std::min(least, *m); }};
        take(height - 1);
        for (const ChainAnchor& anchor : anchors) {
            if (anchor.block->nHeight - 1 < height - 1) take(anchor.block->nHeight - 1);
        }
        for (const SkippedCommitment& commitment : skipped) {
            if (commitment.exact_fork && *commitment.exact_fork < height - 1) take(*commitment.exact_fork);
        }
        // Deeper than every acknowledgement walked: DeepForkMargin (audit 7, finding 2), else as many
        // anchors as were walked.
        if (deep) {
            // (The forks kept as one bound are below every height but the lowest ones: taken for each
            // height above genesis, which is fewer final blocks, never more.)
            if (deep->kept_height >= 0 && height >= 1) least = std::min(least, deep->kept_margin);
            for (const auto& [fork, m] : deep->forks) {
                if (fork < height) least = std::min(least, m);
            }
        }
        return least == std::numeric_limits<int>::max() ? static_cast<int>(anchors.size()) : least;
    }};
    std::set<int> candidates;
    for (const ChainAnchor& anchor : anchors) {
        candidates.insert(anchor.anchored->nHeight);
        candidates.insert(anchor.block->nHeight - 1);
    }
    // (A fork at a height adds a candidate just above it.)
    for (const SkippedCommitment& commitment : skipped) {
        if (commitment.exact_fork) candidates.insert(*commitment.exact_fork);
        if (commitment.min_fork) candidates.insert(*commitment.min_fork);
    }
    std::vector<std::pair<int, int>> margins; // height, margin: the margin does not grow with the height
    for (const int height : candidates) margins.emplace_back(height, net(height));
    for (int k{1}; k <= count; ++k) {
        std::optional<int> highest;
        for (const auto& [height, m] : margins) {
            if (m >= k && (!highest || height > *highest)) highest = height;
        }
        if (!highest) break;
        heights.push_back(*highest);
    }
    return heights;
}

std::vector<SkippedCommitment> SkippedCommitments(const FastBlocksHooks& hooks, const CBlockIndex& tip, const std::vector<ChainAnchor>& anchors)
{
    AssertLockHeld(::cs_main);
    std::vector<SkippedCommitment> skipped;
    const ChainstateManager* chainman{hooks.Chainman()};
    if (anchors.empty() || !chainman || !chainman->m_mainchain) return skipped;
    std::set<uint32_t> used;
    for (const ChainAnchor& anchor : anchors) used.insert(anchor.main_height);
    const sidechain::Mainchain& record{*chainman->m_mainchain};
    for (int height{static_cast<int>(anchors.back().main_height) + 1}; height <= record.Height(); ++height) {
        const auto bmm{record.BmmAt(height)};
        if (!bmm || used.contains(static_cast<uint32_t>(height))) continue;
        SkippedCommitment commitment{.main_height = static_cast<uint32_t>(height)};
        // Where a chain that has it leaves this one: at the branch of the block it rests on, if the
        // node knows it (a BMM block, or the block a ticket names); anywhere otherwise.
        const CBlockIndex* rests_on{chainman->m_blockman.LookupBlockIndex(*bmm)};
        if (!rests_on) {
            if (const auto ticket{hooks.GetTicket(*bmm)}) rests_on = chainman->m_blockman.LookupBlockIndex(ticket->anchored);
        }
        if (rests_on && tip.GetAncestor(rests_on->nHeight) == rests_on) {
            commitment.min_fork = rests_on->nHeight;
        } else if (rests_on) {
            commitment.exact_fork = LastCommonAncestor(rests_on, &tip)->nHeight;
        }
        skipped.push_back(commitment);
    }
    return skipped;
}

std::vector<int> AnchoredHeights(const FastBlocksHooks& hooks, const CBlockIndex& tip, int final_anchors)
{
    AssertLockHeld(::cs_main);
    if (final_anchors <= 0) return {};
    const std::vector<ChainAnchor> anchors{RecentAnchors(hooks, tip, AnchorsWalked(final_anchors))};
    return NetAnchoredHeights(anchors, SkippedCommitments(hooks, tip, anchors), final_anchors, hooks.DeepForkMargin(tip, anchors));
}

std::optional<DeepForks> FastBlocksHooks::DeepForkMargin(const CBlockIndex& tip, const std::vector<ChainAnchor>& anchors) const
{
    // A chain that leaves this one above F (a block of this chain at height F, below the oldest
    // acknowledgement walked, O) can have every commitment this chain skipped since its last anchor
    // at or below F (the shared one, at mainchain height sh(F)), and the acknowledgements this chain
    // made above F of blocks at or below F (both chains may have them: `pending`). This chain has more
    // than it, at least:
    //
    //   margin(F) = (acks above F - pending(F)) - (commitments on record after sh(F) - acks above F)
    //             = 2 A(tip) - C(record tip) - G(F),  G(F) = 2 A(F) + pending(F) - C(sh(F))
    //
    // with A(X) the anchors of the chain up to X (its fork-choice key: ChainKey) and C(m) the
    // commitments on record up to mainchain height m (from the floor on: none can be of this chain
    // before its genesis was known). Every skipped commitment counts for every such fork, known or
    // not (unlike the window, which places those it knows): fewer final blocks, never more. G(F) does
    // not depend on the tip once a block above F anchors a block above F (P only grows along a chain),
    // so the largest G of the forks below the block before the one O anchors is kept between calls,
    // and each call walks only the blocks the window left since: down to genesis once, then a few.
    AssertLockHeld(::cs_main);
    ChainstateManager* chainman{Chainman()};
    if (anchors.empty() || !chainman || !chainman->m_mainchain) return std::nullopt;
    const sidechain::Mainchain& record{*chainman->m_mainchain};
    const CBlockIndex* oldest{anchors.back().block};
    const int p_oldest{anchors.back().anchored->nHeight};
    if (oldest->nHeight <= 0 || oldest->GetAncestor(0) != tip.GetAncestor(0)) return std::nullopt;

    LOCK(m_deep_mutex);
    DeepFinality& cache{m_deep};
    // The commitments on record, counted from the floor on: kept, and counted again from where the
    // record changed (a mainchain reorg) only.
    const int floor{record.TimeHeight(m_params.genesis_time - FINALITY_RECORD_MARGIN).value_or(-1)};
    const auto reset{[&] {
        cache = DeepFinality{};
        cache.floor = floor;
    }};
    if (cache.floor != floor) reset();
    while (!cache.recent.empty() && record.HashAt(cache.recent.back().first) != cache.recent.back().second) {
        cache.counted.resize(cache.recent.back().first - floor - 1);
        cache.recent.pop_back();
        if (cache.recent.empty()) reset();
    }
    const int record_height{record.Height()};
    for (int height{floor + 1 + static_cast<int>(cache.counted.size())}; height <= record_height; ++height) {
        const uint32_t before{cache.counted.empty() ? 0 : cache.counted.back()};
        cache.counted.push_back(before + (record.BmmAt(height) ? 1 : 0));
        if (const auto hash{record.HashAt(height)}) cache.recent.emplace_back(height, *hash);
        while (cache.recent.size() > 256) cache.recent.pop_front();
    }
    const auto counted_up_to{[&](int64_t main_height) -> int64_t {
        if (main_height <= floor || cache.counted.empty()) return 0;
        return cache.counted[std::min<int64_t>(main_height - floor - 1, cache.counted.size() - 1)];
    }};

    // The block G was kept for, and the chain it was kept on: still this one, with the same record.
    const auto on_chain{[&](int height, const uint256& hash) {
        const CBlockIndex* at{tip.GetAncestor(height)};
        return at && at->GetBlockHash() == hash;
    }};
    if (cache.valid && (!on_chain(cache.through_height, cache.through_hash) || !on_chain(cache.guard_height, cache.guard_hash) ||
                        (cache.main_height >= 0 && record.HashAt(cache.main_height) != cache.main_hash))) {
        cache.valid = false;
    }
    const int stop{cache.valid ? cache.through_height : -1};

    // The acknowledgements above F of blocks at or below F: the anchored heights (largest first) of
    // those known; and, for those whose ticket the node no longer has in memory deeper than the blocks
    // it keeps undo data for (not read from disk: a pruned node may not have them), their heights,
    // taken to anchor as low as a block can (an acknowledgement anchors a block at most
    // max_unanchored_slots blocks of stake and a BMM block below it).
    std::priority_queue<int> known;
    std::deque<int> unknown;
    const int reach{static_cast<int>(m_params.MaxUnanchoredSlotsEver()) + 2};
    for (const ChainAnchor& anchor : anchors) {
        if (anchor.anchored->nHeight <= oldest->nHeight - 1) known.push(anchor.anchored->nHeight);
    }
    const int64_t base{2 * static_cast<int64_t>(ChainKey::FromWork(tip.nChainWork).anchors)};
    const auto margin_of{[&](int64_t g) {
        return static_cast<int>(std::clamp<int64_t>(base - counted_up_to(record_height) - g, std::numeric_limits<int>::min() / 2, std::numeric_limits<int>::max() / 2));
    }};
    DeepForks deep;
    int64_t kept_max{std::numeric_limits<int64_t>::min()};
    const CBlockIndex* above{nullptr};
    const CBlockIndex* through{nullptr};
    for (const CBlockIndex* index{oldest->pprev}; index && index->nHeight > stop; above = index, index = index->pprev) {
        const int height{index->nHeight};
        const ChainKey key{ChainKey::FromWork(index->nChainWork)};
        if (above && ChainKey::FromWork(above->nChainWork).anchors > key.anchors) {
            // `above` is an anchor (it adds one): the block it anchors.
            if (!above->m_ext) {
                if (above->nHeight - 1 <= height) known.push(above->nHeight - 1);
            } else {
                const CBlockIndex* anchored{nullptr};
                if (above->nHeight >= tip.nHeight - DRIVECHAIN_UNDO_DEPTH) {
                    anchored = AnchoredBy(*above);
                } else {
                    std::optional<uint256> hash;
                    {
                        LOCK(m_acks_mutex);
                        if (const auto found{m_anchored_by.find(above->GetBlockHash())}; found != m_anchored_by.end()) hash = found->second;
                    }
                    if (hash) anchored = chainman->m_blockman.LookupBlockIndex(*hash);
                }
                if (anchored && anchored->nHeight < above->nHeight) {
                    if (anchored->nHeight <= height) known.push(anchored->nHeight);
                } else {
                    unknown.push_back(above->nHeight);
                }
            }
        }
        while (!known.empty() && known.top() > height) known.pop();
        while (!unknown.empty() && unknown.front() > height + reach) unknown.pop_front();
        const int64_t pending{static_cast<int64_t>(known.size() + unknown.size())};
        const int64_t g{2 * static_cast<int64_t>(key.anchors) + pending - counted_up_to(key.last_anchor_height)};
        if (height <= p_oldest - 1) {
            kept_max = std::max(kept_max, g);
            if (!through) through = index;
        } else {
            deep.forks.emplace_back(height, margin_of(g));
        }
    }
    // Kept: the forks at or below the block before the one the oldest acknowledgement anchors.
    if (through) {
        const ChainKey key{ChainKey::FromWork(through->nChainWork)};
        cache.max_g = cache.valid ? std::max(cache.max_g, kept_max) : kept_max;
        cache.valid = true;
        cache.through_height = through->nHeight;
        cache.through_hash = through->GetBlockHash();
        cache.guard_height = oldest->nHeight;
        cache.guard_hash = oldest->GetBlockHash();
        cache.main_height = key.last_anchor_height > 0 ? static_cast<int>(key.last_anchor_height) : -1;
        if (cache.main_height >= 0) cache.main_hash = record.HashAt(cache.main_height).value_or(uint256{});
    }
    if (cache.valid) {
        deep.kept_height = cache.through_height;
        deep.kept_margin = margin_of(cache.max_g);
    }
    return deep;
}

Finality FinalityOf(const FastBlocksHooks& hooks, const CBlockIndex& block, const CBlockIndex& tip, int final_anchors)
{
    AssertLockHeld(::cs_main);
    return FinalityAt(block.nHeight, tip.nHeight - block.nHeight + 1, AnchoredHeights(hooks, tip, final_anchors), final_anchors);
}

} // namespace pos
