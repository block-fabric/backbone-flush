// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <publicapi/limits.h>

#include <grpc/protobuf.h>

#include <algorithm>

namespace publicapi {

std::optional<double> RateLimiter::Take(const std::string& client, double cost, std::chrono::steady_clock::time_point now)
{
    LOCK(m_mutex);
    auto it{m_buckets.find(client)};
    if (it == m_buckets.end()) {
        if (m_buckets.size() >= m_max_clients) {
            // Drop the buckets that are full again (their clients went quiet); if none, the oldest.
            for (auto b{m_buckets.begin()}; b != m_buckets.end();) {
                const double refill{std::chrono::duration<double>(now - b->second.at).count() * m_rate};
                if (b->second.tokens + refill >= m_burst) {
                    b = m_buckets.erase(b);
                } else {
                    ++b;
                }
            }
            if (m_buckets.size() >= m_max_clients) {
                m_buckets.erase(std::min_element(m_buckets.begin(), m_buckets.end(), [](const auto& a, const auto& b) { return a.second.at < b.second.at; }));
            }
        }
        it = m_buckets.emplace(client, Bucket{m_burst, now}).first;
    }
    Bucket& bucket{it->second};
    if (now > bucket.at) {
        bucket.tokens = std::min(m_burst, bucket.tokens + std::chrono::duration<double>(now - bucket.at).count() * m_rate);
        bucket.at = now;
    }
    if (bucket.tokens >= cost) {
        bucket.tokens -= cost;
        return std::nullopt;
    }
    return m_rate > 0 ? (cost - bucket.tokens) / m_rate : 3600.0;
}

size_t RateLimiter::Clients() const
{
    LOCK(m_mutex);
    return m_buckets.size();
}

std::shared_ptr<const pb::Message> ReplyCache::Get(const std::string& key, const uint256& tip)
{
    LOCK(m_mutex);
    const auto it{m_index.find(key)};
    if (it == m_index.end() || it->second->tip != tip) return nullptr;
    m_lru.splice(m_lru.begin(), m_lru, it->second);
    ++m_hits;
    return it->second->reply;
}

void ReplyCache::Put(const std::string& key, const uint256& tip, std::shared_ptr<const pb::Message> reply, size_t bytes)
{
    LOCK(m_mutex);
    if (bytes > m_max_bytes / 4) return; // one entry never takes much of it
    if (tip != m_tip) {
        // A new tip: what was cached is of another state.
        m_lru.clear();
        m_index.clear();
        m_bytes = 0;
        m_tip = tip;
    }
    if (const auto it{m_index.find(key)}; it != m_index.end()) {
        m_bytes -= it->second->bytes;
        m_lru.erase(it->second);
        m_index.erase(it);
    }
    m_lru.push_front(Entry{key, tip, std::move(reply), bytes});
    m_index[key] = m_lru.begin();
    m_bytes += bytes + key.size();
    m_lru.front().bytes = bytes + key.size();
    while (!m_lru.empty() && (m_lru.size() > m_max_entries || m_bytes > m_max_bytes)) {
        m_bytes -= m_lru.back().bytes;
        m_index.erase(m_lru.back().key);
        m_lru.pop_back();
    }
}

size_t ReplyCache::Entries() const
{
    LOCK(m_mutex);
    return m_lru.size();
}

size_t ReplyCache::Bytes() const
{
    LOCK(m_mutex);
    return m_bytes;
}

} // namespace publicapi
