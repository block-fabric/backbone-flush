// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_GRPC_STANDARD_H
#define BITCOIN_GRPC_STANDARD_H

#include <grpc/protobuf.h>
#include <grpc/server.h>

#include <string>
#include <vector>

/**
 * The standard services every gRPC server of ours serves beside its own: grpc.health.v1 (Check: SERVING for "" and
 * each service served, NOT_FOUND otherwise; Watch: the status, then the stream ends) and grpc.reflection.v1 /
 * v1alpha (the services served, the files of the descriptor pool with what they import). Ported from Backbone's
 * enforcer interface (src/grpc/services.cpp there).
 */
namespace grpcsrv {

void RegisterHealth(Server& server, const pb::Pool& pool, const std::vector<std::string>& services);
void RegisterReflection(Server& server, const pb::Pool& pool, const std::vector<std::string>& services);

} // namespace grpcsrv

#endif // BITCOIN_GRPC_STANDARD_H
