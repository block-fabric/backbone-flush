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
 * `rate` a second; a call takes its cost (a quote more than a page). At most `max_clients` are kept: past them, the
 * buckets that are full again (nobody spent from them for a while) are dropped first, then the oldest.
 */
class RateLimiter
{
public:
    RateLimiter(double rate, double burst, size_t max_clients = 50'000) : m_rate{rate}, m_burst{burst}, m_max_clients{max_clients} {}
    /** Take `cost` from the client's bucket; if it has not that many, nothing is taken and the seconds to wait are given. */
    std::optional<double> Take(const std::string& client, double cost, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now())
        EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    size_t Clients() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    double Rate() const { return m_rate; }
    double Burst() const { return m_burst; }

private:
    struct Bucket {
        double tokens;
        std::chrono::steady_clock::time_point at;
    };
    const double m_rate, m_burst;
    const size_t m_max_clients;
    mutable Mutex m_mutex;
    std::unordered_map<std::string, Bucket> m_buckets GUARDED_BY(m_mutex);
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
