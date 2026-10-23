// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_STAKING_H
#define BITCOIN_POS_STAKING_H

#include <consensus/amount.h>
#include <pos/bond.h>
#include <pos/evidence.h>
#include <pos/params.h>
#include <pos/schedule.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>
#include <sidechain/store.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

/**
 * Staking (doc/fastblocks.md, "Staking"): validators bond pegged coins with a BOND output, which
 * takes them out of circulation into the state, and take them back with an UNBOND output, paid
 * unbonding_anchors anchors later through the payouts of the peg actions. Validator sets are fixed
 * at anchors; a validator that misses its slots is jailed for a while, one that signs two blocks for
 * a slot loses slash_bps of its stake and is banned for good.
 *
 * The state is in the sidechain store, so the store's journal is its undo. Table bytes (0x50 to 0x5f
 * are FastBlocks'; the template has 'Q' 0x51 and 'R' 0x52, and FastBlocks 0x55 and 0x56 already):
 * 0x50 STAKE_STATE, 0x53 OFFENCES, 0x54 REWARDS, 0x57 VALIDATORS, 0x58 UNBOND_QUEUE, 0x59 ELIGIBLE,
 * 0x5a SETS, 0x5b ANCHOR_SETS, 0x5c ANCHOR_ORDER, 0x5d LIVENESS (and hooks.cpp's 0x5e EPOCH_SEEDS and 0x5f POOL, the validator pool of the fee split).
 */
namespace pos {

/** Coins on their way back to a validator's owner: paid at the anchor release_anchor. */
struct UnbondingEntry {
    uint64_t release_anchor{0};
    uint32_t sequence{0};
    CAmount amount{0};
    SERIALIZE_METHODS(UnbondingEntry, obj) { READWRITE(obj.release_anchor, obj.sequence, obj.amount); }
    friend bool operator==(const UnbondingEntry&, const UnbondingEntry&) = default;
};

/** A validator, in the state. */
struct ValidatorRecord {
    //! Fixed by the first bond; null for a genesis validator that never bonded.
    uint160 owner;
    CAmount bonded{0};
    //! The stake of a genesis validator (PosParams::genesis_validators): weight only, no coins.
    CAmount genesis_stake{0};
    std::vector<UnbondingEntry> unbonding;
    uint32_t unbond_seq{0};
    //! Left out of the validator sets made in epochs before this one.
    int64_t jailed_until_epoch{0};
    //! Signed two blocks for a slot: never in a set again, bonds refused.
    bool tombstoned{false};
    int32_t first_bond_height{-1};
    //! The first coin the transaction of its first bond spent (what the owner's signature of it names):
    //! unbonds name it (UnbondMessage), so that they serve on this chain only. Null before a first bond.
    COutPoint first_bond;
    //! The newest anchor block of this chain (FastState::anchor_point) when its first bond was applied:
    //! a block of this chain instance, which unbonds name too (UnbondMessage). The coin alone could be
    //! the same on two chains of one genesis (deposits credited again after a restart give the same
    //! transactions); an anchor block, committed on the mainchain, is of one chain only.
    uint256 first_bond_anchor;

    CAmount Unbonding() const;
    CAmount Weight() const { return bonded + genesis_stake; }
    bool Eligible(const Consensus::PosParams& params) const { return !tombstoned && Weight() > 0 && (genesis_stake > 0 || bonded >= params.min_stake); }

    SERIALIZE_METHODS(ValidatorRecord, obj)
    {
        READWRITE(obj.owner, obj.bonded, obj.genesis_stake, obj.unbonding, obj.unbond_seq, obj.jailed_until_epoch, obj.tombstoned, obj.first_bond_height, obj.first_bond, obj.first_bond_anchor);
    }
    friend bool operator==(const ValidatorRecord&, const ValidatorRecord&) = default;
};

/** The totals of staking, one entry of the store. */
struct StakeState {
    //! The genesis validators are in the validator table (done at the first peg step).
    bool genesis_loaded{false};
    CAmount bonded{0};
    CAmount unbonding{0};
    //! Penalties not paid out yet: each anchor pays 1/pot_reserve_anchors of it with the pot.
    CAmount pot_reserve{0};
    //! Rewards for proofs, owed to the blocks that carried them, paid at the next peg step.
    CAmount rewards_owed{0};
    uint64_t next_reward{0};
    //! The version of the last validator set made (SETS).
    uint64_t set_version{0};
    //! Penalties since the chain began.
    CAmount slashed{0};

    SERIALIZE_METHODS(StakeState, obj)
    {
        READWRITE(obj.genesis_loaded, obj.bonded, obj.unbonding, obj.pot_reserve, obj.rewards_owed, obj.next_reward, obj.set_version, obj.slashed);
    }
    friend bool operator==(const StakeState&, const StakeState&) = default;
};

/** The validator set an anchor made: its version, and the anchor's count and slot. */
struct AnchorSet {
    uint64_t version{0};
    uint64_t anchors{0};
    int64_t slot{0};
    SERIALIZE_METHODS(AnchorSet, obj) { READWRITE(obj.version, obj.anchors, obj.slot); }
    friend bool operator==(const AnchorSet&, const AnchorSet&) = default;
};

/** Slots a validator led in an epoch, and blocks it made in them. */
struct Liveness {
    uint32_t assigned{0};
    uint32_t produced{0};
    SERIALIZE_METHODS(Liveness, obj) { READWRITE(obj.assigned, obj.produced); }
    friend bool operator==(const Liveness&, const Liveness&) = default;
};

StakeState GetStakeState(const sidechain::StoreView& view);
std::optional<ValidatorRecord> GetValidator(const sidechain::StoreView& view, const uint256& pubkey);
/**
 * The keys of the state whose values alone decide whether the staking rules take a transaction
 * (Staking::ApplyTx): the records of the validators its bonds and unbonds name.
 */
std::vector<sidechain::StoreBytes> ValidatorRuleKeys(const CTransaction& tx);
/** Every validator in the state, by key, until `fn` returns false. */
void ForEachValidator(const sidechain::StoreView& view, const std::function<bool(const uint256&, const ValidatorRecord&)>& fn);
/** The validator set an anchor (the block that took the peg actions) made, if the state has it. */
std::optional<AnchorSet> GetAnchorSet(const sidechain::StoreView& view, const uint256& anchor);
std::optional<std::vector<Consensus::PosValidator>> GetSetVersion(const sidechain::StoreView& view, uint64_t version);
bool OffencePunished(const sidechain::StoreView& view, const uint256& pubkey, int64_t slot);
std::optional<Liveness> GetLiveness(const sidechain::StoreView& view, int64_t epoch, const uint256& pubkey);
/** The payouts owed to the owners of unbonded coins, by release anchor (for RPC). */
void ForEachUnbonding(const sidechain::StoreView& view, const std::function<bool(uint64_t release_anchor, const uint256& pubkey, CAmount amount)>& fn);

/**
 * The staking rules on the state of a chain. Each call writes to `out` (an overlay of the state the
 * view `view` is, or `view` itself): a failure leaves a partial update, which the caller drops.
 */
class Staking
{
public:
    Staking(const Consensus::PosParams& params, const uint256& genesis_hash) : m_params{params}, m_genesis{genesis_hash} {}

    /**
     * The bonds and unbonds of a transaction (sidechain::ChainHooks::ApplyTx). `anchors`: the anchors
     * of the chain so far (an unbond is paid unbonding_anchors after). `anchor_point`: the newest anchor
     * block of the chain so far (FastState::anchor_point), which a first bond records (first_bond_anchor);
     * a first bond before the chain's first anchor is refused (bad-pos-bond-unanchored).
     * Reasons: bad-pos-bond (does not read), bad-pos-bond-amount, bad-pos-bond-pop, bad-pos-bond-tombstoned, bad-pos-bond-unanchored,
     * bad-pos-bond-owner; bad-pos-unbond (does not read), bad-pos-unbond-value (not 0), bad-pos-unbond-unknown,
     * bad-pos-unbond-sequence, bad-pos-unbond-amount, bad-pos-unbond-remainder, bad-pos-unbond-sig.
     */
    bool ApplyTx(sidechain::StoreOverlay& out, const CTransaction& tx, int height, uint64_t anchors, std::string& reject_reason, const uint256& anchor_point) const;

    struct PegStepResult {
        //! The share of the reserve released, for the pot's payee.
        CAmount release{0};
        //! Whether the set made can make blocks of stake.
        bool set_active{false};
    };
    /**
     * The staking peg actions of an anchor, the `anchors`-th, in a block of slot `slot` (the slot of
     * the epochs whose set it makes: SetFor), in epoch `epoch` at the earliest (a BMM block's time may
     * be ahead of the clock): unbonds due are paid, rewards for proofs paid, a share of the reserve
     * released, validators that made too few of their blocks in the epochs before `epoch` jailed
     * (`epoch` is for liveness and pruning only: those epochs are surely over), and the validator set
     * made, noted under `anchor` (the block's hash). The set is made as of `set_epoch` (at least
     * `epoch`; no later than the clock, and which whoever makes the block cannot move ahead): jails
     * over by then are over for it, and jails given now run jail_epochs after it. With `keep_active`
     * (PosParams::jail_fix_height), the liveness jails are given only if the set made with them is still
     * active (min_active_validators): none of them otherwise.
     */
    PegStepResult PegStep(sidechain::StoreOverlay& out, const uint256& anchor, uint64_t anchors, int64_t slot, int64_t epoch, int64_t set_epoch, std::vector<CTxOut>& owed_tx,
                          bool keep_active = false) const;

    /** Coins the chain could not pay to the reserve (a BMM block's pot whose payee is no address: IsRewardPayee). */
    void ToReserve(sidechain::StoreOverlay& out, CAmount amount) const;

    /** A block of stake at `slot` made by `producer`; the leaders of the slots it skipped (`missed`) are charged. */
    void NoteLiveness(sidechain::StoreOverlay& out, int64_t slot, const uint256& producer, const std::vector<std::pair<int64_t, uint256>>& missed) const;

    /**
     * Punish an offence a block carried proof of (checked by the caller: the slot's leader signed two
     * headers): slash_bps of its stake, a ban, the rest unbonded; 1/reporter_share_divisor of the
     * penalty to `reporter`, at the next peg step, the rest to the reserve. No reporter (the offender
     * carried the proof itself), or one that is not an address (IsRewardPayee): all of it to the
     * reserve. Once: a banned validator loses nothing more.
     * Reasons: bad-pos-evidence-known (punished before), bad-pos-evidence-banned (the validator is banned).
     */
    bool Punish(sidechain::StoreOverlay& out, const Offence& offence, const std::optional<CScript>& reporter, uint64_t anchors, std::string& reject_reason) const;

    /** The validator set the state makes now, for epochs `epoch` on (jailed validators left out). */
    ValidatorSet MakeSet(const sidechain::StoreView& view, int64_t epoch) const;
    /** Put the genesis validators in the table, once. */
    void LoadGenesis(sidechain::StoreOverlay& out) const;

    /** The penalty of stake `stake`: slash_bps of it, in integers. */
    CAmount Penalty(CAmount stake) const;

private:
    void PutValidator(sidechain::StoreOverlay& out, const uint256& pubkey, const std::optional<ValidatorRecord>& before, const ValidatorRecord& after) const;

    const Consensus::PosParams& m_params;
    const uint256 m_genesis;
};

} // namespace pos

#endif // BITCOIN_POS_STAKING_H
