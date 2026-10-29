// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <publicapi/init.h>

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <bitassets/market.h>
#include <chainparams.h>
#include <common/args.h>
#include <logging.h>
#include <node/context.h>
#include <node/interface_ui.h>
#include <tinyformat.h>
#include <util/chaintype.h>
#include <util/strencodings.h>
#include <util/translation.h>

#ifdef ENABLE_PUBLICAPI
#include <grpc/descriptors.h>
#include <grpc/protobuf.h>
#include <grpc/server.h>
#include <netbase.h>
#include <publicapi/rest.h>
#include <publicapi/service.h>
#endif

#include <memory>

namespace {
#ifdef ENABLE_PUBLICAPI
std::unique_ptr<pb::Pool> g_pool;
std::unique_ptr<grpcsrv::Server> g_server;
std::unique_ptr<publicapi::Service> g_service;
#endif

/** The network as the app's links name it. */
std::string NetworkName()
{
    switch (Params().GetChainType()) {
    case ChainType::ECASH: return "ecash";
    case ChainType::ECASH_BETANET: return "ecashbeta";
    case ChainType::ECASH_TESTNET: return "ecashtest";
    default: return Params().GetChainTypeString();
    }
}
} // namespace

void AddPublicApiOptions(ArgsManager& args)
{
    args.AddArg("-publicapi=<[addr:]port>", "Serve the public, read-only DEX API (doc/public-api.md): REST/JSON under /api/v1, and on the same port gRPC, gRPC-Web and the "
                                            "Connect protocol (flush.dex.v1.DexService). A port alone binds 127.0.0.1 and ::1 (put it behind a reverse proxy, doc/public-api.md). "
                                            "Never any wallet method or data. Off by default. Can be specified multiple times",
                 ArgsManager::ALLOW_ANY | ArgsManager::NETWORK_ONLY, OptionsCategory::RPC);
    args.AddArg("-publicapiorigin=<origin>", "Let web pages of this origin (e.g. https://flush.markets) call the public API (CORS, without credentials); requests from other pages are refused. "
                                             "Default: none. Can be specified multiple times",
                 ArgsManager::ALLOW_ANY, OptionsCategory::RPC);
    args.AddArg("-publicapihost=<name>", "Accept public API requests whose Host is this name, beside IP addresses and localhost (a proxy that passes the Host on). Can be specified multiple times",
                 ArgsManager::ALLOW_ANY, OptionsCategory::RPC);
    args.AddArg("-publicapiallowip=<ip>", "Accept public API connections from this source, beside localhost (needed to bind beyond localhost): an IP, a network/netmask or a network/CIDR. Can be specified multiple times",
                 ArgsManager::ALLOW_ANY, OptionsCategory::RPC);
    args.AddArg("-publicapiproxy=<ip>", "A proxy in front of the public API (e.g. 127.0.0.1 for nginx on this machine): its -publicapiproxyheader names the client, for the rate limits, "
                                        "and its connections are not bounded by address. Default: none (the client is the connection's peer, forwarding headers ignored). Can be specified multiple times",
                 ArgsManager::ALLOW_ANY, OptionsCategory::RPC);
    args.AddArg("-publicapiproxyheader=<name>", "The header the proxies of -publicapiproxy set with the client's address (e.g. X-Real-IP; X-Forwarded-For: its right-most entry). The proxy must set it itself, overwriting what the client sent",
                 ArgsManager::ALLOW_ANY, OptionsCategory::RPC);
    args.AddArg("-publicapiipv6prefix=<bits>", "IPv6 clients of the public API are counted by this prefix, for the 16 connections an address may hold and for its rate limits (one client usually has a /64 to rotate addresses in; 16 to 128, default: 64)",
                 ArgsManager::ALLOW_ANY, OptionsCategory::RPC);
    args.AddArg("-publicapiwideprefix=<bits>", "IPv6 clients of the public API in one network of this prefix are also limited together, at -publicapiwidemultiplier times one client's rate (0: not; 16 to 128, default: 48)",
                 ArgsManager::ALLOW_ANY, OptionsCategory::RPC);
    args.AddArg("-publicapiwidemultiplier=<n>", "How many clients' rate the IPv6 clients of one -publicapiwideprefix network share (at least 1, default: 64)", ArgsManager::ALLOW_ANY, OptionsCategory::RPC);
    args.AddArg("-publicapireflection", "Serve gRPC server reflection on the public API (default: 0)", ArgsManager::ALLOW_ANY, OptionsCategory::RPC);
    args.AddArg("-publicapirate=<n>", "Calls a second each client of the public API may make on average (a quote counts 4, a list 2; default: 10)", ArgsManager::ALLOW_ANY, OptionsCategory::RPC);
    args.AddArg("-publicapiburst=<n>", "And at once (default: 40)", ArgsManager::ALLOW_ANY, OptionsCategory::RPC);
    args.AddArg("-publicapibroadcast", "Broadcast transactions signed elsewhere (POST /api/v1/broadcast; 6 a minute per client, 120 in all; default: 1)", ArgsManager::ALLOW_ANY, OptionsCategory::RPC);
    args.AddArg("-publicapithreads=<n>", "Threads that serve the public API (default: 4)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::RPC);
    args.AddArg("-dexindex", "Keep the DEX's market index: trades, candles and holders, derived block by block with the state (default: 1 with -publicapi, else 0). "
                             "Turning it on derives the sidechain state again from the blocks once",
                 ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
}

bool PublicApiRequested(const ArgsManager& args) { return args.IsArgSet("-publicapi") && !args.IsArgNegated("-publicapi"); }

bool StartPublicApi(node::NodeContext& node, const ArgsManager& args)
{
    if (!PublicApiRequested(args)) return true;
#ifndef ENABLE_PUBLICAPI
    (void)node;
    return InitError(Untranslated("-publicapi: this build has no public API (built without libnghttp2, WITH_PUBLICAPI=OFF)"));
#else
    if (!bitassets::market::Enabled()) LogWarning("public API: -dexindex is off: trades, candles and holders are not served");
    g_pool = std::make_unique<pb::Pool>();
    std::string error;
    if (!g_pool->Load(grpcsrv::DescriptorSet(), error)) return InitError(Untranslated("public API descriptors: " + error));

    std::vector<CSubNet> allow;
    for (const std::string& ip : args.GetArgs("-publicapiallowip")) {
        const CSubNet subnet{LookupSubNet(ip)};
        if (!subnet.IsValid()) return InitError(Untranslated(strprintf("Invalid -publicapiallowip subnet: %s", ip)));
        allow.push_back(subnet);
    }
    std::vector<CSubNet> proxies;
    for (const std::string& ip : args.GetArgs("-publicapiproxy")) {
        const CSubNet subnet{LookupSubNet(ip)};
        if (!subnet.IsValid()) return InitError(Untranslated(strprintf("Invalid -publicapiproxy subnet: %s", ip)));
        proxies.push_back(subnet);
    }
    std::vector<CService> binds;
    for (const std::string& spec : args.GetArgs("-publicapi")) {
        uint16_t port{0};
        std::string host;
        if (spec.find_first_not_of("0123456789") == std::string::npos) {
            const auto p{ToIntegral<uint16_t>(spec)};
            if (!p || *p == 0) return InitError(Untranslated("-publicapi=" + spec + ": a port, or an address and a port"));
            for (const char* loopback : {"127.0.0.1", "::1"}) binds.push_back(*Lookup(loopback, *p, false));
            continue;
        }
        if (!SplitHostPort(spec, port, host) || port == 0) return InitError(Untranslated("-publicapi=" + spec + ": [addr:]port"));
        const std::optional<CService> addr{Lookup(host, port, false)};
        if (!addr) return InitError(Untranslated("-publicapi: cannot resolve " + host));
        if ((!addr->IsLocal() || addr->IsBindAny()) && allow.empty()) {
            return InitError(Untranslated(strprintf("-publicapi=%s is not a loopback address: that needs -publicapiallowip (or put it behind a proxy on localhost)", addr->ToStringAddrPort())));
        }
        binds.push_back(*addr);
    }
    std::vector<std::string> origins;
    for (const std::string& origin : args.GetArgs("-publicapiorigin")) {
        // An origin as a browser sends it: scheme, host, port; no path, no wildcard.
        if (!(origin.starts_with("https://") || origin.starts_with("http://")) || origin.find('/', 8) != std::string::npos || origin.find('*') != std::string::npos || origin.size() > 255) {
            return InitError(Untranslated("-publicapiorigin=" + origin + ": an origin, https://host[:port]"));
        }
        origins.push_back(origin);
    }

    grpcsrv::Options options;
    options.threads = std::clamp<int>(args.GetIntArg("-publicapithreads", 4), 1, 64);
    options.allowed_origins = origins;
    for (const std::string& h : args.GetArgs("-publicapihost")) options.allowed_hosts.push_back(ToLower(h));
    options.cors = true;
    options.max_request_bytes = publicapi::MAX_BODY_BYTES;
    options.max_connection_buffer = 1 << 20;
    options.max_total_buffer = 64 << 20;
    options.max_response_bytes = 4 << 20;
    options.max_connections = 256;
    options.max_streams = 32;
    options.idle_timeout_seconds = 30;
    options.request_timeout_seconds = 15;
    options.max_connections_per_ip = 16;
    const int64_t ipv6_prefix{args.GetIntArg("-publicapiipv6prefix", 64)};
    if (ipv6_prefix < 16 || ipv6_prefix > 128) return InitError(Untranslated("-publicapiipv6prefix: a prefix length from 16 to 128"));
    options.ipv6_prefix = static_cast<int>(ipv6_prefix);
    // Slow reads (also behind HTTP/2 flow control), Rapid Reset and MadeYouReset, requests per connection.
    options.write_timeout_seconds = 30;
    // A slow reader: at least 4 KiB a second taken of its waiting replies (or all of them) over each 30 s; what a
    // connection holds of replies at most (a call's 4 MiB, what waits for the socket, room for a few streams).
    options.min_read_bytes_per_second = 4 << 10;
    options.max_reply_buffer = 32 << 20;
    options.max_requests_per_connection = 10000;
    options.max_abandoned_requests = 100;
    options.max_stream_messages = 32;
    options.rest_prefix = publicapi::REST_PREFIX;
    options.rest = publicapi::MakeRestRouter(*g_pool);
    // The client's address from a proxy only when the operator names both the proxy and its header.
    options.trusted_proxies = proxies;
    options.proxy_header = ToLower(args.GetArg("-publicapiproxyheader", ""));
    if (!options.proxy_header.empty() && proxies.empty()) return InitError(Untranslated("-publicapiproxyheader needs -publicapiproxy (the addresses of the proxies that set it)"));
    if (options.proxy_header.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos) return InitError(Untranslated("-publicapiproxyheader: a header name"));
    options.response_headers = {
        {"x-content-type-options", "nosniff"},
        {"content-security-policy", "default-src 'none'; frame-ancestors 'none'"},
        {"referrer-policy", "no-referrer"},
    };
    g_server = std::make_unique<grpcsrv::Server>(*g_pool, options);
    g_server->SetName("public API");

    publicapi::Options service_options;
    service_options.network = NetworkName();
    const auto number{[&](const char* name, double fallback) {
        const std::string text{args.GetArg(name, "")};
        if (text.empty()) return fallback;
        int64_t milli{0};
        if (!ParseFixedPoint(text, 3, &milli) || milli <= 0) return -1.0;
        return milli / 1000.0;
    }};
    service_options.rate = number("-publicapirate", 10);
    service_options.burst = number("-publicapiburst", 40);
    if (service_options.rate <= 0 || service_options.burst <= 0) return InitError(Untranslated("-publicapirate and -publicapiburst: numbers above 0"));
    service_options.broadcast = args.GetBoolArg("-publicapibroadcast", true);
    const int64_t wide_prefix{args.GetIntArg("-publicapiwideprefix", 48)};
    if (wide_prefix != 0 && (wide_prefix < 16 || wide_prefix > 128)) return InitError(Untranslated("-publicapiwideprefix: 0, or a prefix length from 16 to 128"));
    service_options.wide_prefix = static_cast<int>(wide_prefix);
    service_options.wide_multiplier = number("-publicapiwidemultiplier", 64);
    if (service_options.wide_multiplier < 1) return InitError(Untranslated("-publicapiwidemultiplier: a number of at least 1"));
    service_options.reflection = args.GetBoolArg("-publicapireflection", false);
    g_service = publicapi::RegisterService(*g_server, *g_pool, node, service_options);
    publicapi::Service* service{g_service.get()};
    g_server->SetGuard([service](const grpcsrv::CallInfo& call) { return service->Guard(call); });
    if (!g_server->Start(binds, allow, error)) {
        g_service.reset();
        g_server.reset();
        return InitError(Untranslated("public API: " + error));
    }
    for (const CService& bound : g_server->Bound()) LogInfo("public API: serving on %s (REST %s, gRPC, gRPC-Web, Connect)", bound.ToStringAddrPort(), publicapi::REST_PREFIX);
    LogInfo("public API: %u origin(s) allowed (CORS), %g calls a second a client, broadcast %s, market index %s", origins.size(), service_options.rate,
            service_options.broadcast ? "on" : "off", bitassets::market::Enabled() ? "on" : "off");
    return true;
#endif
}

void StopPublicApi()
{
#ifdef ENABLE_PUBLICAPI
    if (g_server) {
        LogInfo("public API: stopping");
        g_server->Stop();
    }
    g_service.reset();
    g_server.reset();
    g_pool.reset();
#endif
}
