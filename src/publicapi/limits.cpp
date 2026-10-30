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
    Bucket* bucket{nullptr};
    if (const auto it{m_buckets.find(client)}; it != m_buckets.end()) {
        m_order.splice(m_order.begin(), m_order, it->second);
        bucket = &*it->second;
    } else {
        // Past the bound, room is made only by a bucket full again (its client went quiet), among those used longest ago:
        // a drained one is never dropped, or a client could start afresh with another key.
        bool room{m_buckets.size() < m_max_clients};
        if (!room) {
            size_t looked{0};
            for (auto b{m_order.end()}; b != m_order.begin() && looked < EVICTION_CANDIDATES; ++looked) {
                --b;
                if (Level(*b, now) >= m_burst) {
                    m_buckets.erase(b->client);
                    m_order.erase(b);
                    room = true;
                    break;
                }
            }
        }
        if (room) {
            m_order.push_front(Bucket{client, m_burst, now});
            m_buckets.emplace(client, m_order.begin());
            bucket = &m_order.front();
        } else {
            bucket = &m_overflow;
        }
    }
    bucket->tokens = Level(*bucket, now);
    if (now > bucket->at) bucket->at = now;
    if (bucket->tokens >= cost) {
        bucket->tokens -= cost;
        return std::nullopt;
    }
    return m_rate > 0 ? (cost - bucket->tokens) / m_rate : 3600.0;
}

bool RateLimiter::Has(const std::string& client, double cost, std::chrono::steady_clock::time_point now) const
{
    LOCK(m_mutex);
    const auto it{m_buckets.find(client)};
    if (it != m_buckets.end()) return Level(*it->second, now) >= cost;
    return m_buckets.size() < m_max_clients ? m_burst >= cost : true;
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

void LogoStore::Drop(std::list<Entry>::iterator it)
{
    if (!it->owner.empty()) {
        if (const auto owned{m_owned.find(it->owner)}; owned != m_owned.end() && --owned->second == 0) m_owned.erase(owned);
    }
    m_index.erase(it->sha256);
    m_order.erase(it);
}

void LogoStore::Expire(Clock::time_point now)
{
    while (!m_order.empty() && now - m_order.back().at >= m_ttl) Drop(std::prev(m_order.end()));
}

void LogoStore::Put(const std::string& sha256, Logo logo, Clock::time_point now, const std::string& owner)
{
    LOCK(m_mutex);
    Expire(now);
    if (const auto it{m_index.find(sha256)}; it != m_index.end()) {
        // Uploaded again: kept from now on, as the newest (still its first uploader's).
        it->second->at = now;
        m_order.splice(m_order.begin(), m_order, it->second);
        return;
    }
    // An owner at its bound: its own oldest makes room (the oldest last in the order).
    if (m_max_per_owner > 0 && !owner.empty()) {
        if (const auto owned{m_owned.find(owner)}; owned != m_owned.end() && owned->second >= m_max_per_owner) {
            for (auto it{m_order.end()}; it != m_order.begin();) {
                --it;
                if (it->owner == owner) {
                    Drop(it);
                    break;
                }
            }
        }
    }
    m_order.push_front(Entry{sha256, std::make_shared<const Logo>(std::move(logo)), now, owner});
    m_index[sha256] = m_order.begin();
    if (!owner.empty()) ++m_owned[owner];
    while (m_order.size() > m_max_entries) Drop(std::prev(m_order.end()));
}

std::shared_ptr<const LogoStore::Logo> LogoStore::Get(const std::string& sha256, Clock::time_point now)
{
    LOCK(m_mutex);
    Expire(now);
    const auto it{m_index.find(sha256)};
    if (it == m_index.end()) return nullptr;
    return it->second->logo;
}

size_t LogoStore::Entries() const
{
    LOCK(m_mutex);
    return m_order.size();
}

} // namespace publicapi
