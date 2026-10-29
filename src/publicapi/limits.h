// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PUBLICAPI_LIMITS_H
#define BITCOIN_PUBLICAPI_LIMITS_H

#include <sync.h>
#include <uint256.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace pb {
class Message;
} // namespace pb

/** What keeps the public API (publicapi/service.h) from being run into the ground: rate limits and a cache. */
namespace publicapi {

/**
 * Token buckets by client (its address, or what the proxy in front says): each holds at most `burst` tokens and gains
 * `rate` a second; a call takes its cost (a quote more than a page). At most `max_clients` are kept: past them, a bucket
 * full again (its client went quiet) among the few used longest ago is dropped for the new client (in constant time: they
 * are kept in the order of their last use); if none of them is full, the new client's calls are taken from one bucket that
 * all such clients share, so that new keys never bring fresh limits while the others are spent.
 */
class RateLimiter
{
public:
    RateLimiter(double rate, double burst, size_t max_clients = 50'000) : m_rate{rate}, m_burst{burst}, m_max_clients{max_clients}, m_overflow{{}, burst, {}} {}
    /** Take `cost` from the client's bucket; if it has not that many, nothing is taken and the seconds to wait are given. */
    std::optional<double> Take(const std::string& client, double cost, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now())
        EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** Whether Take would take `cost` from the client's bucket now (nothing taken, no bucket made). */
    bool Has(const std::string& client, double cost, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const
        EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    size_t Clients() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! How many of the buckets used longest ago are looked at for one full again.
    static constexpr size_t EVICTION_CANDIDATES{8};
    double Rate() const { return m_rate; }
    double Burst() const { return m_burst; }

private:
    struct Bucket {
        std::string client;
        double tokens;
        std::chrono::steady_clock::time_point at;
    };
    const double m_rate, m_burst;
    const size_t m_max_clients;
    mutable Mutex m_mutex;
    //! Used most recently first.
    std::list<Bucket> m_order GUARDED_BY(m_mutex);
    std::unordered_map<std::string, std::list<Bucket>::iterator> m_buckets GUARDED_BY(m_mutex);
    //! The bucket of the clients that found no room (none of the oldest buckets full again).
    Bucket m_overflow GUARDED_BY(m_mutex);
    /** What a bucket holds at `now`. */
    double Level(const Bucket& bucket, std::chrono::steady_clock::time_point now) const
    {
        if (now <= bucket.at) return bucket.tokens;
        return std::min(m_burst, bucket.tokens + std::chrono::duration<double>(now - bucket.at).count() * m_rate);
    }
};

/**
 * Replies by request, for the tip they were made at: a call that asks what another asked at the same tip gets the same
 * reply without its work being done again. The key is the method and the request's canonical bytes (nothing else of the
 * request, so that no header can make an entry another client gets: no cache poisoning). Bounded in entries and bytes,
 * least recently used out first; entries of an older tip are dropped as the tip moves.
 */
class ReplyCache
{
public:
    ReplyCache(size_t max_entries, size_t max_bytes) : m_max_entries{max_entries}, m_max_bytes{max_bytes} {}
    std::shared_ptr<const pb::Message> Get(const std::string& key, const uint256& tip) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void Put(const std::string& key, const uint256& tip, std::shared_ptr<const pb::Message> reply, size_t bytes) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    size_t Entries() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    size_t Bytes() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    uint64_t Hits() const { return m_hits.load(); }

private:
    struct Entry {
        std::string key;
        uint256 tip;
        std::shared_ptr<const pb::Message> reply;
        size_t bytes;
    };
    const size_t m_max_entries, m_max_bytes;
    mutable Mutex m_mutex;
    std::list<Entry> m_lru GUARDED_BY(m_mutex);
    std::unordered_map<std::string, std::list<Entry>::iterator> m_index GUARDED_BY(m_mutex);
    size_t m_bytes GUARDED_BY(m_mutex){0};
    uint256 m_tip GUARDED_BY(m_mutex);
    std::atomic<uint64_t> m_hits{0};
};

/**
 * Logos uploaded by sites (8.1.0, POST /api/v1/logos), kept by their SHA-256 so that a link can name where one is
 * (logo_url): each for `ttl` from its last upload (uploaded again, it is kept longer), at most `max_entries`, the one
 * uploaded longest ago dropped first. What is put here is checked by the caller (the chain's rules of logos).
 */
class LogoStore
{
public:
    using Clock = std::chrono::steady_clock;
    struct Logo {
        std::string bytes;
        std::string content_type;
    };
    LogoStore(size_t max_entries, Clock::duration ttl) : m_max_entries{max_entries}, m_ttl{ttl} {}
    /** Keep a logo under `sha256` (hex) from `now`, or keep it longer if it is here. */
    void Put(const std::string& sha256, Logo logo, Clock::time_point now = Clock::now()) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** The logo of `sha256` if it is here and not expired at `now`. */
    std::shared_ptr<const Logo> Get(const std::string& sha256, Clock::time_point now = Clock::now()) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    size_t Entries() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    struct Entry {
        std::string sha256;
        std::shared_ptr<const Logo> logo;
        Clock::time_point at;
    };
    void Expire(Clock::time_point now) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    const size_t m_max_entries;
    const Clock::duration m_ttl;
    mutable Mutex m_mutex;
    //! Oldest upload last.
    std::list<Entry> m_order GUARDED_BY(m_mutex);
    std::unordered_map<std::string, std::list<Entry>::iterator> m_index GUARDED_BY(m_mutex);
};

/** A bound on how many of a kind of call run at once (the expensive ones: quotes, builds); one over it is refused at once. */
class Gate
{
public:
    explicit Gate(int max) : m_max{max} {}
    class Pass
    {
    public:
        explicit Pass(Gate* gate) : m_gate{gate} {}
        Pass(Pass&& other) noexcept : m_gate{std::exchange(other.m_gate, nullptr)} {}
        Pass(const Pass&) = delete;
        ~Pass()
        {
            if (m_gate) --m_gate->m_running;
        }
        explicit operator bool() const { return m_gate != nullptr; }

    private:
        Gate* m_gate;
    };
    Pass Enter()
    {
        if (++m_running > m_max) {
            --m_running;
            return Pass{nullptr};
        }
        return Pass{this};
    }

private:
    const int m_max;
    std::atomic<int> m_running{0};
};

} // namespace publicapi

#endif // BITCOIN_PUBLICAPI_LIMITS_H
