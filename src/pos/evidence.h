// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POS_EVIDENCE_H
#define BITCOIN_POS_EVIDENCE_H

#include <pos/params.h>
#include <primitives/block.h>
#include <script/script.h>
#include <serialize.h>
#include <sync.h>
#include <uint256.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pos {

//! The P2P message that relays proofs that a leader signed two blocks for one slot.
inline constexpr const char* MSG_EVIDENCE{"fbevidence"};
//! Proofs a node keeps, at most: those of the oldest slots go first.
inline constexpr size_t MAX_EVIDENCE{1000};
//! Signed headers a node remembers, one per slot and signer, to find a second one: those of the oldest slots go first.
inline constexpr size_t MAX_SIGNED_HEADERS{4000};
//! Results of signature checks of proofs a node remembers, by proof and leader: the oldest go first.
inline constexpr size_t MAX_CHECKED_SIGNATURES{4 * MAX_EVIDENCE};
//! Proofs whose signatures a block this node makes (of stake, or merged mined) checks, at most, beyond those checked before.
inline constexpr size_t MAX_EVIDENCE_SIG_CHECKS{32};
//! Blocks this node makes within which each proof of a full pool whose signatures it has not checked is checked (round robin).
inline constexpr size_t EVIDENCE_CHECK_ROUNDS{(MAX_EVIDENCE + MAX_EVIDENCE_SIG_CHECKS - 1) / MAX_EVIDENCE_SIG_CHECKS};
//! Proofs relayed by a peer whose signatures the node checks: at most this many at once...
inline constexpr double EVIDENCE_CHECKS_BURST{64};
//! ... and this many a second after.
inline constexpr double EVIDENCE_CHECKS_PER_SECOND{2};
//! Peers whose relay of proofs is counted, at most.
inline constexpr size_t MAX_EVIDENCE_RATE_PEERS{1024};

/**
 * Two headers the leader of a slot signed for that slot: the proof that it signed twice, which
 * slashing punishes (doc/fastblocks.md, "Evidence"). The same proof always reads the same: the
 * header with the lower hash first (Normalize).
 */
struct DoubleProposal {
    CBlockHeader a;
    CBlockHeader b;

    SERIALIZE_METHODS(DoubleProposal, obj) { READWRITE(obj.a, obj.b); }
    friend bool operator==(const DoubleProposal& x, const DoubleProposal& y)
    {
        return x.a.GetHash() == y.a.GetHash() && x.a.ext == y.a.ext && x.b.GetHash() == y.b.GetHash() && x.b.ext == y.b.ext;
    }

    void Normalize();
    /** The slot of the first header. */
    int64_t Slot(const Consensus::PosParams& params) const;
};

//! Coinbase output of a BMM block that carries a proof: OP_RETURN with EVIDENCE_TAG | the proof (serialized).
inline constexpr unsigned char EVIDENCE_TAG[4]{0xDD, 'E', 'V', 'D'};
//! Proofs a BMM block's coinbase may carry, at most (as many as the sections of a block of stake).
inline constexpr size_t MAX_BMM_EVIDENCE{16};
//! Proofs this node's merged miner puts in a BMM block, at most (the coinbase's weight is set aside for little).
inline constexpr size_t MAX_MINED_EVIDENCE{4};

/** The coinbase output of a BMM block that carries `proof`. */
CScript EvidenceScript(const DoubleProposal& proof);
/** Whether a script claims to carry a proof (EVIDENCE_TAG), well formed or not. */
bool IsEvidenceScript(const CScript& script);
/** The proof an output carries; nothing if it carries none or it does not read. */
std::optional<DoubleProposal> ParseEvidenceScript(const CScript& script);

/** What a valid proof says: who signed twice, and at which slot. */
struct Offence {
    uint256 pubkey;
    int64_t slot{0};
    friend auto operator<=>(const Offence&, const Offence&) = default;
};

/**
 * Check the form of a proof: two different headers of blocks of stake, of one slot, at its start.
 * Reasons: bad-pos-evidence-kind (not two headers of blocks of stake), bad-pos-evidence-slot (not
 * the start of one slot, both), bad-pos-evidence-same (one header twice).
 */
bool CheckEvidenceForm(const DoubleProposal& proof, const Consensus::PosParams& params, std::string& reason);

/**
 * Check a proof against the leader of its slot, `pubkey` (which the caller finds from the chain: the
 * validator set and seed of the slot's epoch). Reasons: those of CheckEvidenceForm, and
 * bad-pos-evidence-sig (not both signed by `pubkey`, on the chain of `genesis`).
 * @return the offence; nothing, with `reason` set, if the proof is not one
 */
std::optional<Offence> CheckEvidence(const DoubleProposal& proof, const uint256& pubkey, const uint256& genesis, const Consensus::PosParams& params, std::string& reason);

/**
 * The proofs a node knows, one per offence, and the headers it saw signed, to make proofs of its
 * own. Bounded. Thread safe.
 */
class EvidencePool
{
public:
    /**
     * Keep a proof that was checked. Full (MAX_EVIDENCE), one of a slot not after the oldest kept is
     * not taken: a proof pushed out never comes back as new (nor is relayed again).
     * @return whether it was new and kept
     */
    bool Add(const Offence& offence, DoubleProposal proof) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /**
     * Note a header whose signature by `signer`, the leader of its slot, was checked. If `signer`
     * signed another one of the same slot noted before, the proof that it signed both. (By slot and
     * signer: two branches, or two records, may give a slot two leaders; the header of one does not
     * hide the other's two.)
     */
    std::optional<DoubleProposal> NoteSigned(const CBlockHeader& header, int64_t slot, const uint256& signer) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** Whether the pool keeps a proof of an offence of `pubkey` (the node keeps one per offender: FastBlocksHooks::AddEvidence). */
    bool HasOffender(const uint256& pubkey) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** The offence of a proof this node keeps already (in either order), if it does: no signature to check again. */
    std::optional<Offence> Known(const DoubleProposal& proof) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /**
     * Whether both headers of a proof are signed by `pubkey` (on the chain of `genesis`), checked once:
     * the result is remembered, by proof and leader (MAX_CHECKED_SIGNATURES, the oldest go first), so
     * that a proof the node keeps costs no signature check at each block it makes. A result that says a
     * proof the pool keeps is good (against its offender) is not pushed out while the proof is kept:
     * else a flood of checks (relayed proofs, by many peers) would push it out between its check and
     * the block that carries it, and the proof would wait for a check again (audit 6 review, item 11).
     * `checked`: set to whether the signatures were checked now (not remembered).
     */
    bool CheckSigs(const DoubleProposal& proof, const uint256& pubkey, const uint256& genesis, bool* checked = nullptr) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** Whether the result of CheckSigs for this proof and leader is remembered. */
    bool SigsKnown(const DoubleProposal& proof, const uint256& pubkey) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    std::vector<std::pair<Offence, DoubleProposal>> All() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    std::optional<DoubleProposal> Get(const Offence& offence) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    size_t Size() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** Proofs added since the last call, to relay. */
    std::vector<DoubleProposal> TakeUnrelayed() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** Have the next TakeUnrelayed leave out a proof (it was relayed already). */
    void MarkRelayed(const Offence& offence) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** Signatures of headers checked by CheckSigs since the pool was made (the work it did: tests). */
    uint64_t SigChecks() const { return m_sig_checks.load(); }

private:
    mutable Mutex m_mutex;
    std::map<Offence, DoubleProposal> m_proofs GUARDED_BY(m_mutex);
    //! The offence of each proof kept, by the hashes of its two headers (lower first) and their signatures.
    std::map<uint256, Offence> m_known GUARDED_BY(m_mutex);
    std::vector<Offence> m_unrelayed GUARDED_BY(m_mutex);
    //! The first header each signer signed for each slot seen, by slot and signer.
    std::map<std::pair<int64_t, uint256>, CBlockHeader> m_signed GUARDED_BY(m_mutex);
    //! Whether both headers of a proof are signed by a key, by the proof's id and the key; oldest first in m_sigs_order.
    //! (Those not in m_sigs_order are kept while their proof is: the good result of a proof of the pool.)
    std::map<std::pair<uint256, uint256>, bool> m_sigs GUARDED_BY(m_mutex);
    std::deque<std::pair<uint256, uint256>> m_sigs_order GUARDED_BY(m_mutex);
    std::atomic<uint64_t> m_sig_checks{0};
    void NoteSigs(const uint256& id, const uint256& pubkey, bool valid) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    /** Keep the good result of a proof of the pool while it is kept (out of m_sigs_order). */
    void PinSigs(const uint256& id, const uint256& pubkey) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

public:
    /** What identifies a proof, in either order. */
    static uint256 ProofId(const DoubleProposal& proof);
};

/**
 * How many proofs relayed by each peer the node checks the signatures of: a token bucket per peer
 * (EVIDENCE_CHECKS_BURST at once, EVIDENCE_CHECKS_PER_SECOND after). A peer is not blamed for a proof
 * whose signatures do not check (its chain may name another leader), so it could send such proofs
 * without end; past its rate they are dropped unchecked, and it is not blamed for that either.
 * Bounded (MAX_EVIDENCE_RATE_PEERS). Thread safe.
 */
class EvidenceRateLimiter
{
public:
    /** Whether `peer` may have the signatures of one more proof checked at `now` (it takes a token if so). */
    bool Allow(int64_t peer, std::chrono::milliseconds now) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    size_t Peers() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    struct Bucket {
        double tokens{EVIDENCE_CHECKS_BURST};
        std::chrono::milliseconds updated{0};
    };
    static void Refill(Bucket& bucket, std::chrono::milliseconds now);
    mutable Mutex m_mutex;
    std::map<int64_t, Bucket> m_buckets GUARDED_BY(m_mutex);
};

} // namespace pos

#endif // BITCOIN_POS_EVIDENCE_H
