// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/evidence.h>

#include <pos/header.h>
#include <pos/schedule.h>

#include <hash.h>
#include <streams.h>

#include <algorithm>
#include <ios>
#include <limits>

namespace pos {

namespace {
/** If `script` is OP_RETURN and one push that starts with EVIDENCE_TAG, what follows the tag. */
std::optional<std::vector<unsigned char>> EvidencePayload(const CScript& script)
{
    CScript::const_iterator pc{script.begin()};
    opcodetype opcode;
    std::vector<unsigned char> data;
    if (!script.GetOp(pc, opcode) || opcode != OP_RETURN) return std::nullopt;
    if (!script.GetOp(pc, opcode, data) || opcode > OP_PUSHDATA4 || pc != script.end()) return std::nullopt;
    if (data.size() < sizeof(EVIDENCE_TAG) || !std::equal(std::begin(EVIDENCE_TAG), std::end(EVIDENCE_TAG), data.begin())) return std::nullopt;
    data.erase(data.begin(), data.begin() + sizeof(EVIDENCE_TAG));
    return data;
}
} // namespace

CScript EvidenceScript(const DoubleProposal& proof)
{
    DataStream s{};
    s << proof;
    std::vector<unsigned char> data{std::begin(EVIDENCE_TAG), std::end(EVIDENCE_TAG)};
    data.insert(data.end(), UCharCast(s.data()), UCharCast(s.data()) + s.size());
    return CScript() << OP_RETURN << data;
}

bool IsEvidenceScript(const CScript& script) { return EvidencePayload(script).has_value(); }

std::optional<DoubleProposal> ParseEvidenceScript(const CScript& script)
{
    const auto payload{EvidencePayload(script)};
    if (!payload) return std::nullopt;
    try {
        SpanReader reader{*payload};
        DoubleProposal proof;
        reader >> proof;
        if (!reader.empty()) return std::nullopt;
        return proof;
    } catch (const std::ios_base::failure&) {
        return std::nullopt;
    }
}

void DoubleProposal::Normalize()
{
    if (b.GetHash() < a.GetHash()) std::swap(a, b);
}

int64_t DoubleProposal::Slot(const Consensus::PosParams& params) const { return SlotOf(a.nTime, params); }

bool CheckEvidenceForm(const DoubleProposal& proof, const Consensus::PosParams& params, std::string& reason)
{
    const auto fail{[&](const char* why) {
        reason = why;
        return false;
    }};
    if (!IsPosHeader(proof.a) || !IsPosHeader(proof.b)) return fail("bad-pos-evidence-kind");
    const int64_t time{proof.a.nTime};
    if (proof.b.nTime != proof.a.nTime || time <= params.genesis_time || (time - params.genesis_time) % params.slot_seconds != 0) {
        return fail("bad-pos-evidence-slot");
    }
    if (proof.a.GetHash() == proof.b.GetHash()) return fail("bad-pos-evidence-same");
    return true;
}

std::optional<Offence> CheckEvidence(const DoubleProposal& proof, const uint256& pubkey, const uint256& genesis, const Consensus::PosParams& params, std::string& reason)
{
    if (!CheckEvidenceForm(proof, params, reason)) return std::nullopt;
    if (!CheckBlockSig(proof.a, pubkey, genesis) || !CheckBlockSig(proof.b, pubkey, genesis)) {
        reason = "bad-pos-evidence-sig";
        return std::nullopt;
    }
    return Offence{.pubkey = pubkey, .slot = SlotOf(proof.a.nTime, params)};
}

uint256 EvidencePool::ProofId(const DoubleProposal& proof)
{
    DoubleProposal normal{proof};
    normal.Normalize();
    HashWriter hasher{};
    hasher << normal;
    return hasher.GetSHA256();
}

bool EvidencePool::Add(const Offence& offence, DoubleProposal proof)
{
    proof.Normalize();
    const uint256 id{ProofId(proof)};
    LOCK(m_mutex);
    // Full: a proof of a slot not after the oldest kept would be the one pushed out at once. It is
    // not taken (nor relayed): else a proof pushed out is new again each time it comes back, and
    // nodes in a cycle relay it round for ever. Proofs are only ever pushed out, never removed, so
    // this floor only rises: a node takes each proof once at most.
    if (m_proofs.size() >= MAX_EVIDENCE) {
        const auto oldest{std::min_element(m_proofs.begin(), m_proofs.end(), [](const auto& x, const auto& y) { return x.first.slot < y.first.slot; })};
        if (offence.slot <= oldest->first.slot) return false;
    }
    if (!m_proofs.emplace(offence, std::move(proof)).second) return false;
    m_known.emplace(id, offence);
    m_unrelayed.push_back(offence);
    // Kept as checked: its signatures by the offender are good, and stay known while it is kept.
    PinSigs(id, offence.pubkey);
    // Bounded: the proofs of the oldest slots go first (and their good results with them).
    while (m_proofs.size() > MAX_EVIDENCE) {
        const auto oldest{std::min_element(m_proofs.begin(), m_proofs.end(), [](const auto& x, const auto& y) { return x.first.slot < y.first.slot; })};
        std::erase(m_unrelayed, oldest->first);
        const uint256 old_id{ProofId(oldest->second)};
        m_known.erase(old_id);
        m_sigs.erase({old_id, oldest->first.pubkey});
        m_proofs.erase(oldest);
    }
    return true;
}

bool EvidencePool::HasOffender(const uint256& pubkey) const
{
    LOCK(m_mutex);
    // By key, then slot: the first entry at or after the key's lowest slot is of it, if any is.
    const auto kept{m_proofs.lower_bound(Offence{.pubkey = pubkey, .slot = std::numeric_limits<int64_t>::min()})};
    return kept != m_proofs.end() && kept->first.pubkey == pubkey;
}

std::optional<Offence> EvidencePool::Known(const DoubleProposal& proof) const
{
    const uint256 id{ProofId(proof)};
    LOCK(m_mutex);
    const auto found{m_known.find(id)};
    if (found == m_known.end()) return std::nullopt;
    return found->second;
}

void EvidencePool::PinSigs(const uint256& id, const uint256& pubkey)
{
    AssertLockHeld(m_mutex);
    const auto key{std::make_pair(id, pubkey)};
    // (Noted before, as any result: out of the order, so that nothing pushes it out.)
    if (const auto [it, inserted]{m_sigs.insert_or_assign(key, true)}; !inserted) {
        if (const auto ordered{std::find(m_sigs_order.begin(), m_sigs_order.end(), key)}; ordered != m_sigs_order.end()) m_sigs_order.erase(ordered);
    }
}

void EvidencePool::NoteSigs(const uint256& id, const uint256& pubkey, bool valid)
{
    AssertLockHeld(m_mutex);
    // The good result of a proof the pool keeps, against its offender: kept as long as the proof.
    if (valid) {
        if (const auto known{m_known.find(id)}; known != m_known.end() && known->second.pubkey == pubkey) {
            PinSigs(id, pubkey);
            return;
        }
    }
    if (!m_sigs.emplace(std::make_pair(id, pubkey), valid).second) return;
    m_sigs_order.emplace_back(id, pubkey);
    while (m_sigs_order.size() > MAX_CHECKED_SIGNATURES) {
        m_sigs.erase(m_sigs_order.front());
        m_sigs_order.pop_front();
    }
}

bool EvidencePool::CheckSigs(const DoubleProposal& proof, const uint256& pubkey, const uint256& genesis, bool* checked)
{
    const uint256 id{ProofId(proof)};
    if (checked) *checked = false;
    {
        LOCK(m_mutex);
        if (const auto found{m_sigs.find({id, pubkey})}; found != m_sigs.end()) return found->second;
    }
    // (Not under the lock: the two checks are what costs.)
    ++m_sig_checks;
    const bool valid{CheckBlockSig(proof.a, pubkey, genesis) && (++m_sig_checks, CheckBlockSig(proof.b, pubkey, genesis))};
    if (checked) *checked = true;
    LOCK(m_mutex);
    NoteSigs(id, pubkey, valid);
    return valid;
}

bool EvidencePool::SigsKnown(const DoubleProposal& proof, const uint256& pubkey) const
{
    const uint256 id{ProofId(proof)};
    LOCK(m_mutex);
    return m_sigs.contains({id, pubkey});
}

void EvidenceRateLimiter::Refill(Bucket& bucket, std::chrono::milliseconds now)
{
    if (now <= bucket.updated) return;
    const double seconds{std::chrono::duration<double>(now - bucket.updated).count()};
    bucket.tokens = std::min(EVIDENCE_CHECKS_BURST, bucket.tokens + seconds * EVIDENCE_CHECKS_PER_SECOND);
    bucket.updated = now;
}

bool EvidenceRateLimiter::Allow(int64_t peer, std::chrono::milliseconds now)
{
    LOCK(m_mutex);
    auto it{m_buckets.find(peer)};
    if (it == m_buckets.end()) {
        if (m_buckets.size() >= MAX_EVIDENCE_RATE_PEERS) {
            // A full bucket is one that was never used: those go first, then the oldest peers (the
            // lowest ids: a node numbers its peers in the order they connect).
            std::erase_if(m_buckets, [&](auto& entry) {
                Refill(entry.second, now);
                return entry.second.tokens >= EVIDENCE_CHECKS_BURST;
            });
            while (m_buckets.size() >= MAX_EVIDENCE_RATE_PEERS) m_buckets.erase(m_buckets.begin());
        }
        it = m_buckets.emplace(peer, Bucket{.tokens = EVIDENCE_CHECKS_BURST, .updated = now}).first;
    }
    Bucket& bucket{it->second};
    Refill(bucket, now);
    if (bucket.tokens < 1) return false;
    bucket.tokens -= 1;
    return true;
}

size_t EvidenceRateLimiter::Peers() const
{
    LOCK(m_mutex);
    return m_buckets.size();
}

std::optional<DoubleProposal> EvidencePool::NoteSigned(const CBlockHeader& header, int64_t slot, const uint256& signer)
{
    LOCK(m_mutex);
    const auto [it, inserted]{m_signed.emplace(std::make_pair(slot, signer), header)};
    if (inserted) {
        // Bounded: the oldest slots go first (a proof of so old a slot is old news).
        while (m_signed.size() > MAX_SIGNED_HEADERS) m_signed.erase(m_signed.begin());
        return std::nullopt;
    }
    if (it->second.GetHash() == header.GetHash()) return std::nullopt;
    DoubleProposal proof{.a = it->second, .b = header};
    proof.Normalize();
    return proof;
}

std::vector<std::pair<Offence, DoubleProposal>> EvidencePool::All() const
{
    LOCK(m_mutex);
    return {m_proofs.begin(), m_proofs.end()};
}

std::optional<DoubleProposal> EvidencePool::Get(const Offence& offence) const
{
    LOCK(m_mutex);
    const auto found{m_proofs.find(offence)};
    if (found == m_proofs.end()) return std::nullopt;
    return found->second;
}

size_t EvidencePool::Size() const
{
    LOCK(m_mutex);
    return m_proofs.size();
}

std::vector<DoubleProposal> EvidencePool::TakeUnrelayed()
{
    LOCK(m_mutex);
    std::vector<DoubleProposal> proofs;
    for (const Offence& offence : m_unrelayed) {
        if (const auto found{m_proofs.find(offence)}; found != m_proofs.end()) proofs.push_back(found->second);
    }
    m_unrelayed.clear();
    return proofs;
}

void EvidencePool::MarkRelayed(const Offence& offence)
{
    LOCK(m_mutex);
    std::erase(m_unrelayed, offence);
}

} // namespace pos
