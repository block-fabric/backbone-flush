// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PUBLICAPI_SERVICE_H
#define BITCOIN_PUBLICAPI_SERVICE_H

#include <grpc/protobuf.h>
#include <grpc/server.h>

#include <memory>
#include <optional>
#include <string>

namespace node {
struct NodeContext;
} // namespace node

/**
 * The public DEX API (doc/public-api.md): flush.dex.v1.DexService, read only but for Broadcast, served from this node's
 * own state, its market index (bitassets/market.h) and the node's own RPC methods of the DEX (quoteswap, getswapquote,
 * quoteroute, quotelaunch, quotepoolliquidity, previewposition, getbook, getpool, searchpools...: the code the app's pages
 * quote with, called in process). Never a wallet: no method of a wallet is called, no wallet is read.
 */
namespace publicapi {

inline constexpr const char* SERVICE{"flush.dex.v1.DexService"};

struct Options {
    //! The network's name, as the app's links say it (ecash, ecashbeta, ecashtest, regtest).
    std::string network;
    //! Each client's calls: `rate` cost units a second, at most `burst` at once (a page costs 1, a quote 4...).
    double rate{10};
    double burst{40};
    //! Broadcasts: each client's, a minute, and the burst; and all clients together, a minute.
    double broadcast_per_minute{6};
    double broadcast_burst{3};
    double broadcast_global_per_minute{120};
    bool broadcast{true};
    //! Expensive calls (quotes, builds, the first page of a tip) running at once, at most.
    int max_expensive{4};
    //! The cache of replies, per tip.
    size_t cache_entries{4096};
    size_t cache_bytes{64 << 20};
};

class Service
{
public:
    virtual ~Service() = default;
    /** The rate limits, checked before a call runs (grpcsrv::Server::SetGuard). */
    virtual std::optional<grpcsrv::Status> Guard(const grpcsrv::CallInfo& call) = 0;
};

/** Register DexService's methods on `server`, with grpc.health.v1 and grpc.reflection. */
std::unique_ptr<Service> RegisterService(grpcsrv::Server& server, const pb::Pool& pool, node::NodeContext& node, const Options& options);

/** What a call of a method costs against its client's rate (RateLimiter). */
double CostOf(const std::string& method_path);

/** Whether a link a creator wrote may be served as a link: https, a host, no user or password, no space or control, 512 bytes at most. */
bool SafeLink(const std::string& url);
/** A creator's text (an asset's info): its description (every line not a link) and its safe links, with their labels. */
struct CreatorText {
    std::string description;
    std::vector<std::pair<std::string, std::string>> links;
};
CreatorText SplitInfo(const std::string& info);

} // namespace publicapi

#endif // BITCOIN_PUBLICAPI_SERVICE_H
