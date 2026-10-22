// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/feesplit.h>

#include <arith_uint256.h>
#include <crypto/common.h>
#include <sidechain/mainchain.h>
#include <sidechain/state.h>

#include <algorithm>

namespace pos {

CScript PoolScript()
{
    return CScript() << OP_RETURN << std::vector<unsigned char>(std::begin(POOL_TAG), std::end(POOL_TAG));
}

bool IsPoolScript(const CScript& script) { return script == PoolScript(); }

CScript PoolSharesMarker(uint32_t count)
{
    std::vector<unsigned char> data(std::begin(POOL_SHARES_TAG), std::end(POOL_SHARES_TAG));
    data.resize(8);
    WriteLE32(data.data() + 4, count);
    return CScript() << OP_RETURN << data;
}

std::optional<uint32_t> ParsePoolSharesMarker(const CScript& script)
{
    // OP_RETURN, a push of 8, the tag and the count.
    if (script.size() != 10 || script[0] != OP_RETURN || script[1] != 8) return std::nullopt;
    if (!std::equal(std::begin(POOL_SHARES_TAG), std::end(POOL_SHARES_TAG), script.begin() + 2)) return std::nullopt;
    return ReadLE32(&script[6]);
}

bool IsPegTx(const CTransaction& tx)
{
    return std::any_of(tx.vout.begin(), tx.vout.end(), [](const CTxOut& out) {
        return sidechain::IsWithdrawalScript(out.scriptPubKey) || sidechain::ParseRefundScript(out.scriptPubKey).has_value();
    });
}

int64_t MedianSpacing(std::vector<int64_t> spacings)
{
    if (spacings.empty()) return FEE_SPLIT_TARGET_SPACING;
    std::sort(spacings.begin(), spacings.end());
    return spacings[spacings.size() / 2];
}

int64_t MedianSpacingAt(const sidechain::Mainchain& record, int height, int base)
{
    if (height < 0) return FEE_SPLIT_TARGET_SPACING;
    const int from{std::max({height - FEE_SPLIT_SPACINGS, base, 0})};
    std::vector<int64_t> spacings;
    std::optional<int64_t> last;
    for (int h{from}; h <= height; ++h) {
        const auto block{record.GetBlock(h)};
        // (Not reached for a height the rules use, which is on record with the blocks from the base.)
        if (!block) {
            last.reset();
            continue;
        }
        if (last) spacings.push_back(block->time - *last);
        last = block->time;
    }
    return MedianSpacing(std::move(spacings));
}

int64_t BidderShareBps(int64_t spacing, FeeCurve curve)
{
    if (spacing <= FEE_SPLIT_TARGET_SPACING) return FEE_SPLIT_MAX_BIDDER_BPS;
    if (curve == FeeCurve::LINEAR) {
        if (spacing >= FEE_CURVE2_FLOOR_SPACING) return FEE_CURVE2_MIN_BIDDER_BPS;
        // 5000 - 500 (S - 60) / 60 rounded down: less the drop rounded up. (60 < S < 420: no overflow.)
        const int64_t drop{FEE_CURVE2_STEP_BPS * (spacing - FEE_SPLIT_TARGET_SPACING)};
        return FEE_SPLIT_MAX_BIDDER_BPS - (drop + FEE_SPLIT_TARGET_SPACING - 1) / FEE_SPLIT_TARGET_SPACING;
    }
    return std::max(FEE_SPLIT_MIN_BIDDER_BPS, FEE_SPLIT_MAX_BIDDER_BPS * FEE_SPLIT_TARGET_SPACING / spacing);
}

CAmount BidderShare(CAmount fees, int64_t spacing, FeeCurve curve)
{
    if (fees <= 0) return 0;
    const CAmount half{fees / 2};
    if (spacing <= FEE_SPLIT_TARGET_SPACING) return half;
    if (curve == FeeCurve::LINEAR) {
        // floor(F · bps / 10000) without overflow (F · 5000 may not fit in 64 bits): F = q · 10000 + r.
        const int64_t bps{BidderShareBps(spacing, curve)};
        return fees / 10000 * bps + fees % 10000 * bps / 10000;
    }
    const CAmount floor_share{fees * FEE_SPLIT_MIN_BIDDER_BPS / 10000};
    // 50 % · 60 / S = 30 / S. (fees ≤ MAX_MONEY: 30 · fees fits.)
    const CAmount scaled{fees * (FEE_SPLIT_TARGET_SPACING * FEE_SPLIT_MAX_BIDDER_BPS / 10000) / spacing};
    return std::max(floor_share, std::min(half, scaled));
}

FeeSplit SplitFees(std::span<const CTransactionRef> vtx, std::span<const CAmount> fees, bool fees_without_coinbase, int64_t spacing, FeeCurve curve, CAmount extra_ordinary)
{
    FeeSplit split;
    split.ordinary = extra_ordinary;
    for (size_t i{1}; i < vtx.size(); ++i) {
        const size_t at{fees_without_coinbase ? i - 1 : i};
        if (at >= fees.size()) break;
        (IsPegTx(*vtx[i]) ? split.peg : split.ordinary) += fees[at];
    }
    split.bidder_ordinary = BidderShare(split.ordinary, spacing, curve);
    split.validators = split.ordinary - split.bidder_ordinary;
    return split;
}

PoolDistribution DistributePool(CAmount pool, const std::vector<PoolMember>& members)
{
    PoolDistribution result;
    if (pool <= 0) return result;
    arith_uint256 total{0};
    for (const PoolMember& member : members) {
        if (!member.excluded && member.weight > 0) total += arith_uint256{static_cast<uint64_t>(member.weight)};
    }
    if (total == 0) {
        result.remainder = pool;
        return result;
    }
    CAmount paid{0};
    for (const PoolMember& member : members) {
        if (member.excluded || member.weight <= 0) continue;
        arith_uint256 share{static_cast<uint64_t>(pool)};
        share *= arith_uint256{static_cast<uint64_t>(member.weight)};
        share /= total;
        const CAmount amount{static_cast<CAmount>(share.GetLow64())};
        if (amount <= 0) continue;
        paid += amount;
        if (member.payee) {
            result.shares.emplace_back(amount, *member.payee);
        } else {
            result.to_reserve += amount;
        }
    }
    result.remainder = pool - paid;
    return result;
}

} // namespace pos
