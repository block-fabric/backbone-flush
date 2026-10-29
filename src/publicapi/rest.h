// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PUBLICAPI_REST_H
#define BITCOIN_PUBLICAPI_REST_H

#include <grpc/protobuf.h>
#include <grpc/server.h>

#include <string>
#include <variant>

/**
 * The REST face of the public API (doc/public-api.md; flush.markets docs/api.md): GET /api/v1/... (POST for broadcast, logos and a long portfolio),
 * each path mapped onto a method of flush.dex.v1.DexService and its request message, from the path's segments and the
 * query's parameters (each a field of the request, by its own name; an unknown one is refused). The reply is the
 * method's, printed with the fields' own names and every field present; a logo is served as its image.
 */
namespace publicapi {

inline constexpr const char* REST_PREFIX{"/api/v1/"};
//! The largest body of a request (a POST's JSON, a gRPC message): a PSBT built here fits in it signed (BuildPsbt).
inline constexpr size_t MAX_BODY_BYTES{256 << 10};

/** The REST face's routes, for grpcsrv::Options::rest. */
grpcsrv::RestRouter MakeRestRouter(const pb::Pool& pool);

/** One request's route (for tests): the method path and its request message, or the status that refuses it. */
std::variant<grpcsrv::RestMatch, grpcsrv::Status> Route(const pb::Pool& pool, const std::string& method, const std::string& path, const std::string& query, const std::string& body);

} // namespace publicapi

#endif // BITCOIN_PUBLICAPI_REST_H
