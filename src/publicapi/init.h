// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PUBLICAPI_INIT_H
#define BITCOIN_PUBLICAPI_INIT_H

class ArgsManager;
namespace node {
struct NodeContext;
} // namespace node

/** The options of the public DEX API (-publicapi...) and of the market index it reads (-dexindex). */
void AddPublicApiOptions(ArgsManager& args);
/** Whether -publicapi is set (and so -dexindex on by default). */
bool PublicApiRequested(const ArgsManager& args);
/**
 * Serve the public DEX API (doc/public-api.md) if -publicapi is set: REST under /api/v1, gRPC, gRPC-Web and Connect on
 * the same port. @return false (an init error given) if it is asked for and cannot be served, or the build has none.
 */
bool StartPublicApi(node::NodeContext& node, const ArgsManager& args);
void StopPublicApi();

#endif // BITCOIN_PUBLICAPI_INIT_H
