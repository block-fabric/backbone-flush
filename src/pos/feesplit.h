// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_FEESPLIT_H
#define BITCOIN_POS_FEESPLIT_H

#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sidechain {
class Mainchain;
} // namespace sidechain

/**
 * The fee split of FastBlocks from PosParams::fee_split_height on (doc/fastblocks.md, "Fees, the pot and
 * the validator pool"). Consensus, integers only.
 *
 *  - The fees of a peg transaction (one with a withdrawal output or a refund request: IsPegTx) go to the
 *    BMM bidder whole: into the pot for a block of stake (paid to the ticket's payee), kept by the
 *    merged miner of a BMM block.
 *  - The ordinary fees F are split: the bidder takes B = max(floor(F / 10), min(floor(F / 2), floor(30 F / S)))
 *    -- b = max(10 %, 50 % · min(1, 60 s / S)) rounded down to the satoshi -- and the validators the rest,
 *    F - B (the odd satoshi of a 50/50 split is theirs), into the validator pool.
 *    That is the first curve (FeeCurve::FIRST). From PosParams::fee_curve2_height on, the second one
 *    (FeeCurve::LINEAR, "the fee curve of 2026-10-18 (2)"): the bidder's share falls linearly with S,
 *    5 points a minute from 50 % at 60 s to the floor of 20 % at 420 s:
 *        bps = 5000 for S <= 60, 2000 for S >= 420, else 5000 - ceil(500 (S - 60) / 60),
 *    i.e. the exact share in basis points rounded down to an integer, and B = floor(F bps / 10000).
 *  - S is the median spacing of the last 11 mainchain blocks (MedianSpacingAt) at a mainchain height the
 *    block cannot choose: the epoch's seed block for a block of stake (SeedHeight), the block before
 *    the commitment for a BMM block (PegStep::main_height). Neither depends on acknowledgements.
 */
namespace pos {

//! Coinbase output that puts coins in the validator pool: OP_RETURN with this 4-byte push ("\xDE" "VPL").
inline constexpr unsigned char POOL_TAG[4]{0xDE, 0x56, 0x50, 0x4C};
//! Zero-value payout that comes before the validators' shares of the pool a peg step pays: OP_RETURN
//! with this tag and the number of shares after it (4 bytes, little-endian). Not consensus to read, only
//! to match: wallets read it to tell the shares (CoinbaseRoles).
inline constexpr unsigned char POOL_SHARES_TAG[4]{0xDE, 0x56, 0x50, 0x53};

//! The spacing of mainchain blocks at and below which the split is 50/50.
inline constexpr int64_t FEE_SPLIT_TARGET_SPACING{60};
//! The spacings S is the median of: of this many blocks at most (the 12 blocks they lie between).
inline constexpr int FEE_SPLIT_SPACINGS{11};
//! The bidder's share of ordinary fees: at most 50 % (5000 bps), at least 10 % (1000 bps).
inline constexpr int64_t FEE_SPLIT_MAX_BIDDER_BPS{5000};
inline constexpr int64_t FEE_SPLIT_MIN_BIDDER_BPS{1000};
//! The second curve: the bidder's share falls this many basis points for each FEE_SPLIT_TARGET_SPACING
//! of S above FEE_SPLIT_TARGET_SPACING, down to FEE_CURVE2_MIN_BIDDER_BPS (reached at S = 420 s).
inline constexpr int64_t FEE_CURVE2_STEP_BPS{500};
inline constexpr int64_t FEE_CURVE2_MIN_BIDDER_BPS{2000};
inline constexpr int64_t FEE_CURVE2_FLOOR_SPACING{FEE_SPLIT_TARGET_SPACING + (FEE_SPLIT_MAX_BIDDER_BPS - FEE_CURVE2_MIN_BIDDER_BPS) * FEE_SPLIT_TARGET_SPACING / FEE_CURVE2_STEP_BPS};
static_assert(FEE_CURVE2_FLOOR_SPACING == 420);

/** Which curve gives the bidder's share of ordinary fees (PosParams::fee_curve2_height). */
enum class FeeCurve : int {
    //! b = max(10 %, 50 % · min(1, 60 s / S)).
    FIRST = 1,
    //! b = clamp(50 % - 5 % · (S - 60 s) / 60 s, 20 %, 50 %) (from fee_curve2_height).
    LINEAR = 2,
};

CScript PoolScript();
bool IsPoolScript(const CScript& script);
CScript PoolSharesMarker(uint32_t count);
std::optional<uint32_t> ParsePoolSharesMarker(const CScript& script);

/** Whether the fees of a transaction are the bidder's whole: it makes a withdrawal or asks for a refund. */
bool IsPegTx(const CTransaction& tx);

/**
 * The median of up to FEE_SPLIT_SPACINGS spacings (seconds; any order): the middle one once sorted, the
 * upper of the two middle ones for an even count. FEE_SPLIT_TARGET_SPACING if there are none.
 */
int64_t MedianSpacing(std::vector<int64_t> spacings);
/**
 * S at mainchain height `height` from the record: the median of the spacings of the median times
 * (MainBlock::time, what the record keeps) of the blocks from max(height - 11, `base`) to `height`;
 * `base` is the block that activated the slot (PosParams::main_activation_height), as the record may
 * start there or lower (one kept from an earlier release starts at 0): no block below it counts.
 * FEE_SPLIT_TARGET_SPACING if fewer than two blocks count (or `height` is below 0).
 */
int64_t MedianSpacingAt(const sidechain::Mainchain& record, int height, int base);

/** B for ordinary fees F at spacing S by a curve (see above). */
CAmount BidderShare(CAmount fees, int64_t spacing, FeeCurve curve = FeeCurve::FIRST);
/**
 * The bidder's share of ordinary fees at spacing S in basis points, rounded down: what RPCs show for
 * the first curve; for the second, what B is computed from.
 */
int64_t BidderShareBps(int64_t spacing, FeeCurve curve = FeeCurve::FIRST);

struct FeeSplit {
    //! Fees of peg transactions (IsPegTx): the bidder's.
    CAmount peg{0};
    //! Other fees, split.
    CAmount ordinary{0};
    //! The bidder's part of them (BidderShare).
    CAmount bidder_ordinary{0};
    //! The validators' part: ordinary - bidder_ordinary, into the pool.
    CAmount validators{0};
    //! All that is the bidder's: peg + bidder_ordinary.
    CAmount Bidder() const { return peg + bidder_ordinary; }
};
/**
 * The split of a block's fees. `fees[i]` is the fee of `vtx[i]` (entry 0, the coinbase's, is not read;
 * `fees` may be one shorter than `vtx`, without it: then `fees[i - 1]` is that of `vtx[i]`). `extra_ordinary`: ordinary
 * fees no transaction paid (the security parts of the DEX's fees of the block, in the chain's coin), split with the others.
 */
FeeSplit SplitFees(std::span<const CTransactionRef> vtx, std::span<const CAmount> fees, bool fees_without_coinbase, int64_t spacing, FeeCurve curve = FeeCurve::FIRST, CAmount extra_ordinary = 0);

/** A validator of the set of an epoch whose pool is paid. */
struct PoolMember {
    uint256 pubkey;
    int64_t weight{0};
    //! Jailed or banned when the pool is paid: no share.
    bool excluded{false};
    //! Where its share goes: its bond owner's P2WPKH; none for a genesis validator that never bonded.
    std::optional<CScript> payee;
};
struct PoolDistribution {
    //! The shares paid, in the order of the members (none of 0).
    std::vector<CTxOut> shares;
    //! Shares of members without a payee: to the reserve.
    CAmount to_reserve{0};
    //! What rounding left (or everything, without a member with weight): carried to the next epoch's pool (the one not over yet).
    CAmount remainder{0};
};
/**
 * The pool of an epoch shared by weight among the members not excluded: each floor(pool · weight / W),
 * W the weight of those members; the rest is the remainder.
 */
PoolDistribution DistributePool(CAmount pool, const std::vector<PoolMember>& members);

} // namespace pos

#endif // BITCOIN_POS_FEESPLIT_H
