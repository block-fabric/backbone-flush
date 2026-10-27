// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// The public DEX API (publicapi/, doc/public-api.md) in pieces: the writer of builds (URIs, canonical payloads, their
// hash), the rate limiter, the cache of replies, the REST routes, creators' links, the codec's REST printing, and the
// server's public face over HTTP/1.1 and HTTP/2 (CORS, preflights, REST, ETag, size caps, guards, the client behind a
// proxy, streams charged per message, slow requests).

#include <bitcoin-build-config.h> // IWYU pragma: keep

#ifdef ENABLE_PUBLICAPI

#include <grpc/descriptors.h>
#include <grpc/protobuf.h>
#include <grpc/server.h>
#include <grpc/standard.h>
#include <netbase.h>
#include <publicapi/build.h>
#include <publicapi/limits.h>
#include <publicapi/rest.h>
#include <publicapi/service.h>
#include <test/util/setup_common.h>
#include <util/sock.h>
#include <util/strencodings.h>
#include <util/time.h>

#include <nghttp2/nghttp2.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <map>
#include <set>
#include <thread>

using pb::Message;
using grpcsrv::Code;

namespace {

const pb::Pool& Pool()
{
    static const pb::Pool pool{[] {
        pb::Pool p;
        std::string error;
        if (!p.Load(grpcsrv::DescriptorSet(), error)) throw std::runtime_error(error);
        return p;
    }()};
    return pool;
}

Message New(const std::string& name) { return Message{*Assert(Pool().FindMessage("flush.dex.v1." + name))}; }

/** A gRPC client over HTTP/2 (nghttp2's client), wired to a Connection in memory. */
struct H2Client {
    grpcsrv::Connection connection;
    nghttp2_session* session{nullptr};
    struct Response {
        std::map<std::string, std::string> headers;
        std::string body;
        bool closed{false};
        uint32_t error_code{0};
    };
    std::map<int32_t, Response> responses;
    std::map<int32_t, std::string> bodies;
    std::map<int32_t, size_t> sent;

    explicit H2Client(grpcsrv::Server& server, CService peer = {}) : connection{server, peer}
    {
        nghttp2_session_callbacks* cb;
        nghttp2_session_callbacks_new(&cb);
        nghttp2_session_callbacks_set_on_header_callback(cb, [](nghttp2_session*, const nghttp2_frame* frame, const uint8_t* name, size_t namelen, const uint8_t* value, size_t valuelen, uint8_t, void* user) {
            static_cast<H2Client*>(user)->responses[frame->hd.stream_id].headers[std::string(reinterpret_cast<const char*>(name), namelen)] = std::string(reinterpret_cast<const char*>(value), valuelen);
            return 0;
        });
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cb, [](nghttp2_session*, uint8_t, int32_t id, const uint8_t* data, size_t len, void* user) {
            static_cast<H2Client*>(user)->responses[id].body.append(reinterpret_cast<const char*>(data), len);
            return 0;
        });
        nghttp2_session_callbacks_set_on_stream_close_callback(cb, [](nghttp2_session*, int32_t id, uint32_t error_code, void* user) {
            auto& r{static_cast<H2Client*>(user)->responses[id]};
            r.closed = true;
            r.error_code = error_code;
            return 0;
        });
        nghttp2_session_client_new(&session, cb, this);
        nghttp2_session_callbacks_del(cb);
        nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, nullptr, 0);
    }
    ~H2Client() { nghttp2_session_del(session); }

    /** Exchange bytes until neither side has anything more to say. */
    void Pump(int rounds = 50)
    {
        for (int i{0}; i < rounds; ++i) {
            const uint8_t* data;
            std::string out;
            for (ssize_t n; (n = nghttp2_session_mem_send(session, &data)) > 0;) out.append(reinterpret_cast<const char*>(data), n);
            if (!out.empty()) connection.Receive(MakeUCharSpan(out));
            connection.Poll();
            const std::string in{connection.TakeOutput()};
            if (!in.empty()) nghttp2_session_mem_recv(session, reinterpret_cast<const uint8_t*>(in.data()), in.size());
            if (out.empty() && in.empty()) {
                // Handlers run on the server's threads: give them a moment.
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
        }
    }

    int32_t Call(const std::string& path, const std::string& body, const std::string& content_type = "application/grpc")
    {
        const std::vector<std::pair<std::string, std::string>> headers{
            {":method", "POST"}, {":scheme", "http"}, {":authority", "127.0.0.1:50051"}, {":path", path}, {"content-type", content_type}, {"te", "trailers"}};
        std::vector<nghttp2_nv> nvs;
        for (const auto& [n, v] : headers) {
            nvs.push_back({reinterpret_cast<uint8_t*>(const_cast<char*>(n.data())), reinterpret_cast<uint8_t*>(const_cast<char*>(v.data())), n.size(), v.size(), NGHTTP2_NV_FLAG_NONE});
        }
        nghttp2_data_provider provider;
        provider.source.ptr = this;
        provider.read_callback = [](nghttp2_session*, int32_t id, uint8_t* buf, size_t length, uint32_t* flags, nghttp2_data_source* source, void*) -> ssize_t {
            auto& self{*static_cast<H2Client*>(source->ptr)};
            const std::string& body{self.bodies[id]};
            size_t& done{self.sent[id]};
            const size_t n{std::min(length, body.size() - done)};
            std::memcpy(buf, body.data() + done, n);
            done += n;
            if (done == body.size()) *flags |= NGHTTP2_DATA_FLAG_EOF;
            return static_cast<ssize_t>(n);
        };
        // The id is the next one the session gives: the body is set before the provider reads it.
        const int32_t id{static_cast<int32_t>(nghttp2_session_get_next_stream_id(session))};
        bodies[id] = body;
        BOOST_REQUIRE_EQUAL(nghttp2_submit_request(session, nullptr, nvs.data(), nvs.size(), &provider, nullptr), id);
        return id;
    }
};



/** One HTTP/1.1 exchange on a fresh connection: the raw reply. */
std::string Http1(grpcsrv::Server& server, const std::string& request, CService peer = {})
{
    grpcsrv::Connection c{server, peer};
    c.Receive(MakeUCharSpan(request));
    std::string out;
    for (int i{0}; i < 100; ++i) {
        c.Poll();
        out += c.TakeOutput();
        if (out.find("\r\n\r\n") != std::string::npos && !c.Busy()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    return out;
}

std::string HeaderOf(const std::string& reply, const std::string& name)
{
    const std::string lower{ToLower(reply.substr(0, reply.find("\r\n\r\n")))};
    const auto at{lower.find("\r\n" + name + ": ")};
    if (at == std::string::npos) return "";
    const size_t start{at + 4 + name.size()};
    return reply.substr(start, lower.find("\r\n", start) - start);
}

int StatusOf(const std::string& reply) { return reply.size() > 12 ? std::stoi(reply.substr(9, 3)) : 0; }
std::string BodyOf(const std::string& reply) { return reply.substr(reply.find("\r\n\r\n") + 4); }

/** A server of the public face, not listening (its calls run inline), with GetStatus, GetLaunch and a guard. */
struct PublicServer {
    grpcsrv::Options options;
    std::unique_ptr<grpcsrv::Server> server;
    std::vector<std::string> clients;
    int refuse_after{1000};
    PublicServer(size_t max_response = 0)
    {
        options.cors = true;
        options.allowed_origins = {"https://flush.markets"};
        options.rest_prefix = publicapi::REST_PREFIX;
        options.rest = publicapi::MakeRestRouter(Pool());
        options.max_response_bytes = max_response;
        options.max_stream_messages = 3;
        options.response_headers = {{"x-content-type-options", "nosniff"}};
        options.trusted_proxies = {LookupSubNet("127.0.0.1")};
        options.proxy_header = "x-real-ip";
        server = std::make_unique<grpcsrv::Server>(Pool(), options);
        server->SetGuard([this](const grpcsrv::CallInfo& call) -> std::optional<grpcsrv::Status> {
            clients.push_back(call.client);
            if (static_cast<int>(clients.size()) > refuse_after) return grpcsrv::Status{Code::RESOURCE_EXHAUSTED, "slow down", {{"retry-after", "7"}}};
            return std::nullopt;
        });
        BOOST_REQUIRE(server->Register(publicapi::SERVICE, "GetStatus", [](const Message&, const std::shared_ptr<grpcsrv::Writer>& w) {
            Message s{New("ChainStatus")};
            s.SetString("network", "regtest");
            s.Mutable("flush").SetUInt("height", 7);
            w->SetHeader("cache-control", "public, max-age=5");
            w->Write(s);
            w->Finish({});
        }));
        BOOST_REQUIRE(server->Register(publicapi::SERVICE, "GetLaunch", [](const Message& req, const std::shared_ptr<grpcsrv::Writer>& w) {
            if (req.GetString("id") != "MOON") return w->Finish({Code::NOT_FOUND, "no such launch"});
            Message d{New("LaunchDetail")};
            d.SetString("ticker", "MOON");
            d.SetString("description", std::string(5000, 'x'));
            w->Write(d);
            w->Finish({});
        }));
        BOOST_REQUIRE(server->Register("grpc.health.v1.Health", "Check", [](const Message&, const std::shared_ptr<grpcsrv::Writer>& w) { w->Finish({}); }));
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(publicapi_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(builds)
{
    using namespace publicapi;
    BOOST_CHECK_EQUAL(PercentEncode("a b/é&=~-._"), "a%20b%2F%C3%A9%26%3D~-._");
    BOOST_CHECK_EQUAL(Decimal(150, 2), "1.5");
    BOOST_CHECK_EQUAL(Decimal(300, 2), "3");
    BOOST_CHECK_EQUAL(Decimal(1, 8), "0.00000001");
    BOOST_CHECK_EQUAL(Decimal(0, 6), "0");
    // An asset of 12 decimals: what a link (8 places) can say, rounded down.
    BOOST_CHECK_EQUAL(FloorUnits(123'456'789'012, 12), 123'456'780'000U);
    BOOST_CHECK_EQUAL(Decimal(123'456'789'012, 12), "0.12345678");
    BOOST_CHECK_EQUAL(ActionUri("buy", "ecashbeta", 1800000000, {{"launch", "TIDE"}, {"ecx", "5"}}), "flush:buy?v=1&network=ecashbeta&exp=1800000000&launch=TIDE&ecx=5");
    // Canonical JSON: keys sorted, no spaces, whatever the order given.
    const std::string payload{CanonicalJson({{"b", UniValue{2}}, {"a", UniValue{"1"}}})};
    BOOST_CHECK_EQUAL(payload, R"({"a":"1","b":2})");
    BOOST_CHECK_EQUAL(ActionHash(payload), "65d0919afa70fed28a8edfb70c7dd1f27fc5cf55172a8c16ac06cae8ef34be82");
}

BOOST_AUTO_TEST_CASE(rate_limits_and_cache)
{
    using namespace std::chrono;
    publicapi::RateLimiter limits{1, 2, 3};
    const auto t0{steady_clock::time_point{} + hours{1}};
    BOOST_CHECK(!limits.Take("a", 1, t0));
    BOOST_CHECK(!limits.Take("a", 1, t0));
    const auto wait{limits.Take("a", 1, t0)};
    BOOST_REQUIRE(wait);
    BOOST_CHECK_CLOSE(*wait, 1.0, 1e-6);
    BOOST_CHECK(!limits.Take("a", 1, t0 + seconds{1})); // refilled at the rate
    BOOST_CHECK(limits.Take("a", 4, t0 + seconds{100})); // never more than the burst
    BOOST_CHECK(!limits.Take("b", 2, t0));                // another client, its own bucket
    // At most 3 clients kept: a fourth drops the full ones (a and b are full again later).
    BOOST_CHECK(!limits.Take("c", 1, t0));
    BOOST_CHECK(!limits.Take("d", 1, t0 + seconds{100}));
    BOOST_CHECK_LE(limits.Clients(), 3U);

    publicapi::ReplyCache cache{2, 1 << 20};
    const uint256 tip1{1}, tip2{2};
    auto reply{std::make_shared<const Message>(New("ChainStatus"))};
    cache.Put("x", tip1, reply, 10);
    BOOST_CHECK(cache.Get("x", tip1) == reply);
    BOOST_CHECK(!cache.Get("x", tip2)); // another tip: not this reply
    cache.Put("y", tip1, reply, 10);
    cache.Put("z", tip1, reply, 10);
    BOOST_CHECK_EQUAL(cache.Entries(), 2U); // least recently used out
    BOOST_CHECK(!cache.Get("x", tip1));
    cache.Put("w", tip2, reply, 10); // a new tip drops the rest
    BOOST_CHECK_EQUAL(cache.Entries(), 1U);

    publicapi::Gate gate{1};
    {
        auto a{gate.Enter()};
        BOOST_CHECK(a);
        BOOST_CHECK(!gate.Enter());
    }
    BOOST_CHECK(gate.Enter());
}

BOOST_AUTO_TEST_CASE(creators_links)
{
    using publicapi::SafeLink;
    BOOST_CHECK(SafeLink("https://example.org/a?b=c#d"));
    BOOST_CHECK(SafeLink("https://example.org:8443/"));
    BOOST_CHECK(!SafeLink("http://example.org"));
    BOOST_CHECK(!SafeLink("javascript:alert(1)"));
    BOOST_CHECK(!SafeLink("https://user:pw@example.org"));
    BOOST_CHECK(!SafeLink("https://exa mple.org"));
    BOOST_CHECK(!SafeLink("https://example.org/\"onmouseover=x"));
    BOOST_CHECK(!SafeLink("https://"));
    BOOST_CHECK(!SafeLink("https://" + std::string(600, 'a')));
    BOOST_CHECK(!SafeLink("https://ex\x01" "ample.org"));
    const auto text{publicapi::SplitInfo("Moon coin\nhttps://x.com/moon\njavascript:alert(1)\n\thttps://t.me/moon \nhttp://a.b")};
    BOOST_CHECK_EQUAL(text.description, "Moon coin\njavascript:alert(1)\nhttp://a.b");
    BOOST_REQUIRE_EQUAL(text.links.size(), 2U);
    BOOST_CHECK_EQUAL(text.links[0].first, "X");
    BOOST_CHECK_EQUAL(text.links[1].first, "Telegram");
    BOOST_CHECK_EQUAL(text.links[1].second, "https://t.me/moon");
}

BOOST_AUTO_TEST_CASE(rest_routes)
{
    const auto route{[](const std::string& method, const std::string& path, const std::string& query = "", const std::string& body = "") {
        return publicapi::Route(Pool(), method, path, query, body);
    }};
    const auto match{[&](const std::string& path, const std::string& query = "") {
        auto r{route("GET", path, query)};
        BOOST_REQUIRE_MESSAGE(std::holds_alternative<grpcsrv::RestMatch>(r), path + ": " + (std::holds_alternative<grpcsrv::Status>(r) ? std::get<grpcsrv::Status>(r).message : std::string{}));
        return std::move(std::get<grpcsrv::RestMatch>(r));
    }};
    const auto refused{[&](const std::string& method, const std::string& path, const std::string& query = "") {
        auto r{route(method, path, query)};
        BOOST_REQUIRE(std::holds_alternative<grpcsrv::Status>(r));
        return std::get<grpcsrv::Status>(r).code;
    }};
    BOOST_CHECK_EQUAL(match("status").method_path, "/flush.dex.v1.DexService/GetStatus");
    BOOST_CHECK_EQUAL(match("launches/stats").method_path, "/flush.dex.v1.DexService/GetLaunchStats");
    auto launch{match("launches/MOON")};
    BOOST_CHECK_EQUAL(launch.method_path, "/flush.dex.v1.DexService/GetLaunch");
    BOOST_CHECK_EQUAL(launch.request->GetString("id"), "MOON");
    auto list{match("launches", "filter=near&sort=progress&limit=5&q=moon%20coin")};
    BOOST_CHECK_EQUAL(list.request->GetUInt("limit"), 5U);
    BOOST_CHECK_EQUAL(list.request->GetString("q"), "moon coin");
    BOOST_CHECK(match("launches/MOON/logo").raw);
    BOOST_CHECK(!match("launches/MOON").raw);
    const std::string id(64, 'a');
    BOOST_CHECK_EQUAL(match("pools/" + id).request->GetString("pool"), id);
    BOOST_CHECK_EQUAL(match("pools/" + id + "/trades").method_path, "/flush.dex.v1.DexService/ListPoolTrades");
    auto pair{match("pools/MOON/ECX")};
    BOOST_CHECK_EQUAL(pair.request->GetString("asset_a"), "MOON");
    BOOST_CHECK_EQUAL(match("quote/deposit", "pool=" + id + "&amount_a=1").request->GetString("action"), "deposit");
    auto verify{match("verifications", "assets=A,B&curators=" + id)};
    BOOST_CHECK_EQUAL(verify.request->Count("assets"), 2U);
    auto build{match("build/launch", "ticker=X1&links=https%3A%2F%2Fa.example%2F%3Fa%2Cb&links=https%3A%2F%2Fb.example")};
    BOOST_CHECK_EQUAL(build.request->Count("links"), 2U); // a link keeps its comma
    BOOST_CHECK_EQUAL(build.request->StringAt("links", 0), "https://a.example/?a,b");
    BOOST_CHECK(refused("GET", "nope") == Code::NOT_FOUND);
    BOOST_CHECK(refused("GET", "pools/short") == Code::NOT_FOUND);
    BOOST_CHECK(refused("GET", "launches", "bogus=1") == Code::INVALID_ARGUMENT);
    BOOST_CHECK(refused("GET", "launches", "limit=-1") == Code::INVALID_ARGUMENT);
    BOOST_CHECK(refused("GET", "launches", "limit=99999999999") == Code::INVALID_ARGUMENT);
    BOOST_CHECK(refused("GET", "launches", "limit=1&limit=2") == Code::INVALID_ARGUMENT);
    BOOST_CHECK(refused("GET", "launches/MOON", "id=SUN") == Code::INVALID_ARGUMENT); // the path's own field
    BOOST_CHECK(refused("GET", "launches/MO%00ON") == Code::INVALID_ARGUMENT);        // no control character
    BOOST_CHECK(refused("GET", "launches/MO%2FON") == Code::INVALID_ARGUMENT);        // no slash in a segment
    BOOST_CHECK(refused("POST", "status") == Code::UNIMPLEMENTED);
    BOOST_CHECK(refused("GET", "broadcast") == Code::UNIMPLEMENTED);
    BOOST_CHECK(refused("GET", "launches", std::string(5000, 'a')) == Code::INVALID_ARGUMENT);
    auto post{route("POST", "broadcast", "", R"({"hex":"00"})")};
    BOOST_REQUIRE(std::holds_alternative<grpcsrv::RestMatch>(post));
    BOOST_CHECK_EQUAL(std::get<grpcsrv::RestMatch>(post).request->GetString("hex"), "00");
    BOOST_CHECK(std::holds_alternative<grpcsrv::Status>(route("POST", "broadcast", "", "[1]")));
    // 7.0.1: a portfolio of several addresses, GET with a list in the query, POST with one in a body.
    BOOST_CHECK_EQUAL(match("addresses/portfolio", "addresses=a1,a2").request->Count("addresses"), 2U);
    auto many{route("POST", "addresses/portfolio", "", R"({"addresses":["a1","a2","a3"]})")};
    const std::string why{std::holds_alternative<grpcsrv::Status>(many) ? std::get<grpcsrv::Status>(many).message : std::string{}};
    BOOST_REQUIRE_MESSAGE(std::holds_alternative<grpcsrv::RestMatch>(many), why);
    BOOST_CHECK_EQUAL(std::get<grpcsrv::RestMatch>(many).request->Count("addresses"), 3U);
    BOOST_CHECK_EQUAL(match("addresses/abc/portfolio").request->GetString("address"), "abc");
}

BOOST_AUTO_TEST_CASE(rest_json_printing)
{
    Message d{New("LaunchDetail")};
    d.SetString("ticker", "MOON");
    d.SetUInt("created_height", 7);
    const UniValue rest{d.ToJson(Message::JsonOptions{.proto_names = true, .emit_unpopulated = true})};
    BOOST_CHECK_EQUAL(rest["ticker"].get_str(), "MOON");
    BOOST_CHECK_EQUAL(rest["created_height"].getInt<int>(), 7);
    BOOST_CHECK(rest["logo"].isNull());         // optional, not set: null
    BOOST_CHECK(rest["pool"].isNull());         // a message not set: null
    BOOST_CHECK(rest["links"].isArray() && rest["links"].empty());
    BOOST_CHECK_EQUAL(rest["raised_ecx"].get_real(), 0); // a scalar: its default
    const UniValue connect{d.ToJson()};
    BOOST_CHECK(connect.exists("createdHeight") && !connect.exists("created_height") && !connect.exists("logo"));
}

BOOST_AUTO_TEST_CASE(public_face_http1)
{
    PublicServer p;
    // REST: the reply in proto names, cache headers, ETag; CORS for the origin allowed, never credentials.
    std::string r{Http1(*p.server, "GET /api/v1/status HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: https://flush.markets\r\n\r\n")};
    BOOST_CHECK_EQUAL(StatusOf(r), 200);
    BOOST_CHECK_EQUAL(HeaderOf(r, "access-control-allow-origin"), "https://flush.markets");
    BOOST_CHECK_EQUAL(HeaderOf(r, "access-control-allow-credentials"), "");
    BOOST_CHECK_EQUAL(HeaderOf(r, "vary"), "Origin");
    BOOST_CHECK_EQUAL(HeaderOf(r, "cache-control"), "public, max-age=5");
    BOOST_CHECK_EQUAL(HeaderOf(r, "x-content-type-options"), "nosniff");
    BOOST_CHECK_EQUAL(HeaderOf(r, "content-type"), "application/json; charset=utf-8");
    UniValue body;
    BOOST_REQUIRE(body.read(BodyOf(r)));
    BOOST_CHECK_EQUAL(body["flush"]["height"].getInt<int>(), 7);
    BOOST_CHECK(body["ecash"].isNull());
    const std::string etag{HeaderOf(r, "etag")};
    BOOST_CHECK(!etag.empty());
    r = Http1(*p.server, "GET /api/v1/status HTTP/1.1\r\nIf-None-Match: " + etag + "\r\n\r\n");
    BOOST_CHECK_EQUAL(StatusOf(r), 304);
    BOOST_CHECK(BodyOf(r).empty());
    // Another origin: refused, without CORS headers; a preflight of the allowed one answered, of another refused.
    r = Http1(*p.server, "GET /api/v1/status HTTP/1.1\r\nOrigin: https://evil.example\r\n\r\n");
    BOOST_CHECK_EQUAL(StatusOf(r), 403);
    BOOST_CHECK_EQUAL(HeaderOf(r, "access-control-allow-origin"), "");
    r = Http1(*p.server, "OPTIONS /api/v1/status HTTP/1.1\r\nOrigin: https://flush.markets\r\nAccess-Control-Request-Method: GET\r\n\r\n");
    BOOST_CHECK_EQUAL(StatusOf(r), 204);
    BOOST_CHECK(HeaderOf(r, "access-control-allow-methods").find("GET") != std::string::npos);
    BOOST_CHECK_EQUAL(StatusOf(Http1(*p.server, "OPTIONS /api/v1/status HTTP/1.1\r\nOrigin: https://evil.example\r\n\r\n")), 403);
    // Errors: the REST error object, with the Connect HTTP status, not cached.
    r = Http1(*p.server, "GET /api/v1/launches/SUN HTTP/1.1\r\n\r\n");
    BOOST_CHECK_EQUAL(StatusOf(r), 404);
    BOOST_CHECK_EQUAL(HeaderOf(r, "cache-control"), "no-store");
    BOOST_REQUIRE(body.read(BodyOf(r)));
    BOOST_CHECK_EQUAL(body["error"]["code"].get_str(), "not_found");
    BOOST_CHECK_EQUAL(StatusOf(Http1(*p.server, "GET /api/v1/nothing HTTP/1.1\r\n\r\n")), 404);
    // Connect: the same method, JSON names, and its GET form.
    r = Http1(*p.server, "POST /flush.dex.v1.DexService/GetLaunch HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 13\r\n\r\n{\"id\":\"MOON\"}");
    BOOST_CHECK_EQUAL(StatusOf(r), 200);
    BOOST_REQUIRE(body.read(BodyOf(r)));
    BOOST_CHECK_EQUAL(body["ticker"].get_str(), "MOON");
    BOOST_CHECK(!body.exists("logo"));
    // The guard: a client refused with its Retry-After; the client of a request from the trusted proxy is its header's.
    p.clients.clear();
    Http1(*p.server, "GET /api/v1/status HTTP/1.1\r\nX-Real-IP: 203.0.113.9\r\n\r\n", *Lookup("127.0.0.1", 1, false));
    Http1(*p.server, "GET /api/v1/status HTTP/1.1\r\nX-Real-IP: 203.0.113.9\r\n\r\n", *Lookup("10.1.1.1", 1, false));
    Http1(*p.server, "GET /api/v1/status HTTP/1.1\r\nX-Real-IP: 203.0.113.9, 198.51.100.4\r\n\r\n", *Lookup("127.0.0.1", 1, false));
    BOOST_REQUIRE_EQUAL(p.clients.size(), 3U);
    BOOST_CHECK_EQUAL(p.clients[0], "203.0.113.9");
    BOOST_CHECK_EQUAL(p.clients[1], "10.1.1.1"); // not a proxy: its header is not believed
    BOOST_CHECK_EQUAL(p.clients[2], "198.51.100.4"); // the right-most entry, the proxy's own
    // IPv6 clients are known by their /64, peers and forwarded addresses alike: rotating addresses gets no fresh limits.
    p.clients.clear();
    for (int i{1}; i <= 20; ++i) {
        Http1(*p.server, "GET /api/v1/status HTTP/1.1\r\n\r\n", *Lookup(strprintf("2a02:6b8:1:2:%x:%x::%x", i, i * 7, i * 13), 1, false));
        Http1(*p.server, strprintf("GET /api/v1/status HTTP/1.1\r\nX-Real-IP: 2a02:6b8:5:6:%x::%x\r\n\r\n", i, i * 3), *Lookup("127.0.0.1", 1, false));
    }
    Http1(*p.server, "GET /api/v1/status HTTP/1.1\r\nX-Real-IP: 2a02:6b8:5:7::1\r\n\r\n", *Lookup("127.0.0.1", 1, false));
    BOOST_REQUIRE_EQUAL(p.clients.size(), 41U);
    for (int i{0}; i < 40; i += 2) {
        BOOST_CHECK_EQUAL(p.clients[i], "2a02:6b8:1:2::/64");
        BOOST_CHECK_EQUAL(p.clients[i + 1], "2a02:6b8:5:6::/64");
    }
    BOOST_CHECK_EQUAL(p.clients[40], "2a02:6b8:5:7::/64"); // another /64, another client
    // So they share one bucket of the rate limiter: a burst of 3, then refused, whatever address in the /64 is next.
    publicapi::RateLimiter limits{0.001, 3};
    int taken{0};
    for (const auto& client : std::vector<std::string>{p.clients.begin(), p.clients.begin() + 40}) taken += !limits.Take(client, 1);
    BOOST_CHECK_EQUAL(taken, 6); // 3 for each /64
    BOOST_CHECK(!limits.Take(p.clients[40], 1));
    p.refuse_after = 0;
    r = Http1(*p.server, "GET /api/v1/status HTTP/1.1\r\n\r\n");
    BOOST_CHECK_EQUAL(StatusOf(r), 429);
    BOOST_CHECK_EQUAL(HeaderOf(r, "retry-after"), "7");
}

BOOST_AUTO_TEST_CASE(public_face_limits)
{
    // A reply bigger than the cap: an error instead (nothing of it sent).
    PublicServer p{1000};
    std::string r{Http1(*p.server, "GET /api/v1/launches/MOON HTTP/1.1\r\n\r\n")};
    BOOST_CHECK_EQUAL(StatusOf(r), 429);
    UniValue body;
    BOOST_REQUIRE(body.read(BodyOf(r)));
    BOOST_CHECK_EQUAL(body["error"]["code"].get_str(), "resource_exhausted");
    // A request still coming in counts as being received, not as a running call (the connection's deadline applies).
    grpcsrv::Connection c{*p.server};
    c.Receive(MakeUCharSpan(std::string{"GET /api/v1/status HTTP/1.1\r\nHost: x"}));
    BOOST_CHECK(c.Receiving());
    BOOST_CHECK(!c.Busy());
}

BOOST_AUTO_TEST_CASE(public_face_http2)
{
    PublicServer p;
    H2Client client{*p.server};
    // REST over HTTP/2 and gRPC on the same connection.
    Message req{New("GetLaunchRequest")};
    req.SetString("id", "MOON");
    const std::string payload{req.Serialize()};
    std::string grpc_body(1, '\0');
    for (int s{24}; s >= 0; s -= 8) grpc_body.push_back(static_cast<char>((payload.size() >> s) & 0xff));
    grpc_body += payload;
    const int32_t g{client.Call("/flush.dex.v1.DexService/GetLaunch", grpc_body)};
    client.Pump();
    BOOST_CHECK_EQUAL(client.responses[g].headers["grpc-status"], "0");
    Message reply{New("LaunchDetail")};
    std::string error;
    BOOST_REQUIRE(reply.Parse(MakeUCharSpan(client.responses[g].body.substr(5)), error));
    BOOST_CHECK_EQUAL(reply.GetString("ticker"), "MOON");
    // A client stream (health is unary; a stream of messages to a unary method is refused as one request).
    std::string two{grpc_body + grpc_body};
    const int32_t t{client.Call("/flush.dex.v1.DexService/GetLaunch", two)};
    client.Pump();
    BOOST_CHECK_NE(client.responses[t].headers["grpc-status"], "0");

    // A client stream (server reflection): each message charged by the guard, at most max_stream_messages of them.
    grpcsrv::RegisterReflection(*p.server, Pool(), {publicapi::SERVICE});
    Message list{*Assert(Pool().FindMessage("grpc.reflection.v1.ServerReflectionRequest"))};
    list.SetString("list_services", "");
    const std::string m{list.Serialize()};
    std::string envelope(1, '\0');
    for (int s{24}; s >= 0; s -= 8) envelope.push_back(static_cast<char>((m.size() >> s) & 0xff));
    envelope += m;
    p.clients.clear();
    const int32_t two_messages{client.Call("/grpc.reflection.v1.ServerReflection/ServerReflectionInfo", envelope + envelope)};
    client.Pump();
    BOOST_CHECK_EQUAL(client.responses[two_messages].headers["grpc-status"], "0");
    BOOST_CHECK_EQUAL(p.clients.size(), 2U);
    const int32_t too_many{client.Call("/grpc.reflection.v1.ServerReflection/ServerReflectionInfo", envelope + envelope + envelope + envelope)};
    client.Pump();
    BOOST_CHECK_EQUAL(client.responses[too_many].headers["grpc-status"], "8");
}

namespace {
using namespace std::chrono_literals;

/** A real client socket of the server, read and written by the test. */
struct RawClient {
    std::unique_ptr<Sock> sock;
    std::string in;
    bool closed{false};

    explicit RawClient(const CService& to) : sock{ConnectDirectly(to, /*manual_connection=*/true)} { BOOST_REQUIRE(sock); }
    bool Send(std::string_view data)
    {
        while (!data.empty()) {
            const ssize_t n{sock->Send(data.data(), data.size(), MSG_NOSIGNAL)};
            if (n <= 0) return false;
            data.remove_prefix(static_cast<size_t>(n));
        }
        return true;
    }
    /** Read what comes for up to `wait`; @return true once the server closed the connection. */
    bool ReadFor(std::chrono::milliseconds wait)
    {
        const auto end{NodeClock::now() + wait};
        while (!closed && NodeClock::now() < end) {
            Sock::Event occurred{0};
            if (!sock->Wait(10ms, Sock::RecvEvent, &occurred)) return closed = true;
            if (!(occurred & Sock::RecvEvent)) continue;
            char buf[4096];
            const ssize_t n{sock->Recv(buf, sizeof(buf), 0)};
            if (n <= 0) return closed = true;
            in.append(buf, static_cast<size_t>(n));
        }
        return closed;
    }
};

double Seconds(NodeClock::duration d) { return std::chrono::duration<double>(d).count(); }

std::vector<uint8_t> FrameTypes(std::string_view in)
{
    std::vector<uint8_t> types;
    while (in.size() >= 9) {
        const size_t len{(size_t{uint8_t(in[0])} << 16) | (size_t{uint8_t(in[1])} << 8) | uint8_t(in[2])};
        types.push_back(uint8_t(in[3]));
        if (in.size() < 9 + len) break;
        in.remove_prefix(9 + len);
    }
    return types;
}
bool Has(const std::vector<uint8_t>& types, uint8_t type) { return std::find(types.begin(), types.end(), type) != types.end(); }

std::string GrpcEnvelope(const std::string& payload)
{
    std::string out(1, '\0');
    for (int s{24}; s >= 0; s -= 8) out.push_back(static_cast<char>((payload.size() >> s) & 0xff));
    return out + payload;
}

/** An HTTP/2 client's bytes (nghttp2's client session; what the server sends is read raw by the test). */
struct H2Bytes {
    nghttp2_session* session{nullptr};
    std::map<int32_t, std::string> bodies;
    std::map<int32_t, size_t> sent;
    explicit H2Bytes(const std::vector<nghttp2_settings_entry>& settings = {})
    {
        nghttp2_session_callbacks* cb;
        nghttp2_session_callbacks_new(&cb);
        nghttp2_session_client_new(&session, cb, this);
        nghttp2_session_callbacks_del(cb);
        nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, settings.data(), settings.size());
    }
    ~H2Bytes() { nghttp2_session_del(session); }
    std::string Out()
    {
        std::string out;
        const uint8_t* data;
        for (ssize_t n; (n = nghttp2_session_mem_send(session, &data)) > 0;) out.append(reinterpret_cast<const char*>(data), n);
        return out;
    }
    std::vector<nghttp2_nv> Headers(const std::string& path)
    {
        m_headers = {{":method", "POST"}, {":scheme", "http"}, {":authority", "127.0.0.1"}, {":path", path}, {"content-type", "application/grpc"}, {"te", "trailers"}};
        std::vector<nghttp2_nv> nvs;
        for (const auto& [n, v] : m_headers) nvs.push_back({reinterpret_cast<uint8_t*>(const_cast<char*>(n.data())), reinterpret_cast<uint8_t*>(const_cast<char*>(v.data())), n.size(), v.size(), NGHTTP2_NV_FLAG_NONE});
        return nvs;
    }
    /** A whole gRPC request (its body, END_STREAM). */
    int32_t Request(const std::string& path, const std::string& body)
    {
        const std::vector<nghttp2_nv> nvs{Headers(path)};
        nghttp2_data_provider provider;
        provider.source.ptr = this;
        provider.read_callback = [](nghttp2_session*, int32_t id, uint8_t* buf, size_t length, uint32_t* flags, nghttp2_data_source* source, void*) -> ssize_t {
            auto& self{*static_cast<H2Bytes*>(source->ptr)};
            const std::string& b{self.bodies[id]};
            size_t& done{self.sent[id]};
            const size_t n{std::min(length, b.size() - done)};
            std::memcpy(buf, b.data() + done, n);
            done += n;
            if (done == b.size()) *flags |= NGHTTP2_DATA_FLAG_EOF;
            return static_cast<ssize_t>(n);
        };
        const int32_t id{static_cast<int32_t>(nghttp2_session_get_next_stream_id(session))};
        bodies[id] = body;
        BOOST_REQUIRE_EQUAL(nghttp2_submit_request(session, nullptr, nvs.data(), nvs.size(), &provider, nullptr), id);
        return id;
    }
    std::vector<std::pair<std::string, std::string>> m_headers;
};

pb::Message Big(size_t bytes)
{
    pb::Message m{*Assert(Pool().FindMessage("grpc.health.v1.HealthCheckRequest"))};
    m.SetString("service", std::string(bytes, 'a'));
    return m;
}
} // namespace

BOOST_AUTO_TEST_CASE(server_address_keys)
{
    // Connections are counted by address; IPv6 by its /64 (-publicapiipv6prefix), where one client rotates addresses.
    const auto key{[](const std::string& a, int prefix = 64) { return grpcsrv::Server::AddressKey(*Assert(LookupHost(a, false)), prefix); }};
    BOOST_CHECK_EQUAL(key("2001:db8:1:2::1"), "2001:db8:1:2::/64");
    BOOST_CHECK(key("2001:db8:1:2::1") != key("2001:db8:1:3::1"));
    BOOST_CHECK_EQUAL(key("2001:db8:1:2::1", 48), key("2001:db8:1:3::1", 48));
    BOOST_CHECK(key("2001:db8:1:2::1", 128) != key("2001:db8:1:2::2", 128));
    BOOST_CHECK_EQUAL(key("1.2.3.4"), "1.2.3.4");
    std::set<std::string> keys;
    for (int i{1}; i < 200; ++i) keys.insert(key(strprintf("2001:db8:aa:bb:%x:%x::%x", i, i * 7, i * 13)));
    BOOST_CHECK_EQUAL(keys.size(), 1U);
}

BOOST_AUTO_TEST_CASE(server_slow_clients)
{
    grpcsrv::Options options;
    options.write_timeout_seconds = 1;
    options.idle_timeout_seconds = 2;
    options.request_timeout_seconds = 1;
    options.max_requests_per_connection = 8;
    options.max_abandoned_requests = 5;
    options.max_response_bytes = 1 << 20;
    options.threads = 2;
    grpcsrv::Server server{Pool(), options};
    constexpr size_t MESSAGES{512}, MESSAGE_BYTES{64 << 10};
    std::atomic<size_t> written{0};
    std::atomic<bool> refused{false};
    // A server stream that would write 32 MiB; the call's buffer is bounded at 1 MiB.
    BOOST_REQUIRE(server.Register("grpc.health.v1.Health", "Watch", [&](const pb::Message&, const std::shared_ptr<grpcsrv::Writer>& w) {
        const pb::Message big{Big(MESSAGE_BYTES)};
        for (size_t i{0}; i < MESSAGES; ++i) {
            if (!w->Write(big)) {
                refused = true;
                break;
            }
            ++written;
        }
        w->Finish({});
    }));
    BOOST_REQUIRE(server.Register("grpc.health.v1.Health", "Check", [](const pb::Message&, const std::shared_ptr<grpcsrv::Writer>& w) {
        w->Write(pb::Message{*Assert(Pool().FindMessage("grpc.health.v1.HealthCheckResponse"))});
        w->Finish({});
    }));
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start({*Assert(Lookup("127.0.0.1", 0, /*fAllowLookup=*/false))}, {}, error), error);
    const CService to{server.Bound().at(0)};

    {
        // HTTP/2 PINGs alone are not activity: closed with a GOAWAY 2 s after it began, though it keeps pinging.
        RawClient c{to};
        const auto start{NodeClock::now()};
        BOOST_REQUIRE(c.Send(std::string{"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"} + std::string{"\0\0\0\4\0\0\0\0\0", 9}));
        const std::string ping{std::string{"\0\0\x08\x06\0\0\0\0\0", 9} + std::string(8, 'p')};
        while (!c.closed && NodeClock::now() - start < 6s) {
            if (!c.Send(ping)) break;
            c.ReadFor(300ms);
        }
        BOOST_CHECK(c.closed || c.ReadFor(1s));
        const double took{Seconds(NodeClock::now() - start)};
        BOOST_CHECK_MESSAGE(took >= 1.9 && took < 4, "pinging connection closed after " << took << " s");
        BOOST_CHECK(Has(FrameTypes(c.in), NGHTTP2_GOAWAY));
    }
    {
        // A client with a zero window (never a WINDOW_UPDATE): the call's unsent replies bounded, its stream reset or
        // the connection closed once nothing moved for 1 s.
        written = 0;
        refused = false;
        RawClient c{to};
        H2Bytes client{{{NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 0}}};
        client.Request("/grpc.health.v1.Health/Watch", GrpcEnvelope(""));
        BOOST_REQUIRE(c.Send(client.Out()));
        const auto start{NodeClock::now()};
        bool reset{false};
        while (!c.closed && !reset && NodeClock::now() - start < 5s) {
            c.ReadFor(100ms);
            reset = Has(FrameTypes(c.in), NGHTTP2_RST_STREAM);
        }
        BOOST_CHECK(reset || c.closed);
        const double took{Seconds(NodeClock::now() - start)};
        BOOST_CHECK_MESSAGE(took >= 0.9 && took < 4, "zero-window stream ended after " << took << " s");
        BOOST_CHECK(refused.load());
        BOOST_CHECK_LE(written.load() * MESSAGE_BYTES, size_t{1} << 20);
    }
    {
        // Rapid Reset: calls asked for whole and reset at once: closed past 5, with a GOAWAY.
        RawClient c{to};
        H2Bytes client;
        int rounds{0};
        for (; rounds < 20 && !c.closed; ++rounds) {
            const int32_t id{client.Request("/grpc.health.v1.Health/Watch", GrpcEnvelope(""))};
            std::string out{client.Out()};
            nghttp2_submit_rst_stream(client.session, NGHTTP2_FLAG_NONE, id, NGHTTP2_CANCEL);
            out += client.Out();
            if (!c.Send(out)) break;
            c.ReadFor(100ms);
        }
        BOOST_CHECK(c.closed || c.ReadFor(1s));
        BOOST_CHECK_MESSAGE(rounds >= 6 && rounds < 9, rounds);
        BOOST_CHECK(Has(FrameTypes(c.in), NGHTTP2_GOAWAY));
    }
    {
        // Past 8 requests on one connection: closed once its call is over.
        RawClient c{to};
        const std::string request{"POST /grpc.health.v1.Health/Check HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}"};
        for (int i{0}; i < 8; ++i) {
            c.in.clear();
            BOOST_REQUIRE(c.Send(request));
            while (c.in.find("\r\n\r\n") == std::string::npos && !c.ReadFor(100ms)) {}
            BOOST_CHECK_MESSAGE(c.in.starts_with("HTTP/1.1 200"), c.in);
            BOOST_CHECK(!c.ReadFor(300ms));
        }
        BOOST_REQUIRE(c.Send(request));
        BOOST_CHECK(c.ReadFor(1s));
    }
    server.Stop();

    // MadeYouReset: a client that makes the server reset its streams (requests over the connection's buffer, reset
    // with ENHANCE_YOUR_CALM) without resetting any itself is closed past the bound as well.
    grpcsrv::Options tight{options};
    tight.max_request_bytes = 5000;
    tight.max_connection_buffer = 2000;
    grpcsrv::Server strict{Pool(), tight};
    BOOST_REQUIRE(strict.Register("grpc.health.v1.Health", "Check", [](const pb::Message&, const std::shared_ptr<grpcsrv::Writer>& w) { w->Finish({}); }));
    BOOST_REQUIRE_MESSAGE(strict.Start({*Assert(Lookup("127.0.0.1", 0, /*fAllowLookup=*/false))}, {}, error), error);
    {
        RawClient c{strict.Bound().at(0)};
        H2Bytes client;
        int rounds{0};
        for (; rounds < 20 && !c.closed; ++rounds) {
            client.Request("/grpc.health.v1.Health/Check", GrpcEnvelope(std::string(3000, 'y')));
            if (!c.Send(client.Out())) break;
            c.ReadFor(100ms);
        }
        BOOST_CHECK(c.closed || c.ReadFor(1s));
        BOOST_CHECK_MESSAGE(rounds >= 6 && rounds < 9, rounds);
        BOOST_CHECK(Has(FrameTypes(c.in), NGHTTP2_RST_STREAM));
        BOOST_CHECK(Has(FrameTypes(c.in), NGHTTP2_GOAWAY));
    }
    strict.Stop();
}

BOOST_AUTO_TEST_CASE(server_slow_reader_bounded)
{
    // A client that takes a byte now and then (slow reads) used to keep its connection open with the server moving
    // every reply into its unsent bytes, without bound. Now: backpressure (no more taken from the call above
    // REPLY_HIGH_WATER waiting), the call ends at its own bound, and the client is closed for reading too little.
    grpcsrv::Options options;
    options.write_timeout_seconds = 1;
    options.min_read_bytes_per_second = 64 << 10;
    // A call's bound above what the loopback sockets hold, so that replies keep waiting on the server's side.
    options.max_response_bytes = 16 << 20;
    options.max_reply_buffer = 32 << 20;
    options.idle_timeout_seconds = 30;
    options.threads = 2;
    grpcsrv::Server server{Pool(), options};
    constexpr size_t MESSAGES{4096}, MESSAGE_BYTES{64 << 10};
    std::atomic<bool> small{false};
    std::atomic<size_t> written{0};
    std::atomic<bool> refused{false};
    // Either a stream that would write 256 MiB as fast as it can, or 100 small events, a few a second.
    BOOST_REQUIRE(server.Register("grpc.health.v1.Health", "Watch", [&](const pb::Message&, const std::shared_ptr<grpcsrv::Writer>& w) {
        const pb::Message msg{Big(small ? 40 : MESSAGE_BYTES)};
        for (size_t i{0}; i < (small ? 100 : MESSAGES); ++i) {
            if (!w->Write(msg)) {
                refused = true;
                break;
            }
            ++written;
            if (small) std::this_thread::sleep_for(25ms);
        }
        w->Finish({});
    }));
    std::string error;
    BOOST_REQUIRE_MESSAGE(server.Start({*Assert(Lookup("127.0.0.1", 0, /*fAllowLookup=*/false))}, {}, error), error);
    const CService to{server.Bound().at(0)};
    const std::string body{GrpcEnvelope("")};
    const std::string ask{strprintf("POST /grpc.health.v1.Health/Watch HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/connect+proto\r\nContent-Length: %u\r\n\r\n", body.size()) + body};
    {
        RawClient c{to};
        BOOST_REQUIRE(c.Send(ask));
        const auto start{NodeClock::now()};
        // One byte every 50 ms for 4 s: 20 bytes a second, far under 64 KiB.
        while (!c.closed && NodeClock::now() - start < 4s) {
            Sock::Event occurred{0};
            if (!c.sock->Wait(50ms, Sock::RecvEvent, &occurred)) break;
            if (occurred & Sock::RecvEvent) {
                char b;
                const ssize_t n{c.sock->Recv(&b, 1, 0)};
                if (n <= 0) {
                    c.closed = true;
                    break;
                }
                c.in.push_back(b);
            }
            std::this_thread::sleep_for(50ms);
        }
        // What the server took from the call stayed bounded: the call ended at its own bound (max_response_bytes drops
        // what it held), with what waits for the socket and what the sockets hold; not the 256 MiB the call would
        // write. (What is left fits in the sockets: then nothing waits on the server's side, and the connection may
        // stay open; with more waiting, min_read_bytes_per_second closes it: grpc_tests in Backbone.)
        BOOST_CHECK(refused.load());
        BOOST_CHECK_MESSAGE(written.load() * MESSAGE_BYTES < size_t{48} << 20, written.load() * MESSAGE_BYTES);
    }
    {
        // A client that reads as the events come is not cut: all of them, then the end of the stream.
        small = true;
        written = 0;
        refused = false;
        RawClient c{to};
        BOOST_REQUIRE(c.Send(ask));
        const auto start{NodeClock::now()};
        while (!c.closed && NodeClock::now() - start < 10s && c.in.find("0\r\n\r\n") == std::string::npos) c.ReadFor(100ms);
        BOOST_CHECK(c.in.starts_with("HTTP/1.1 200"));
        BOOST_CHECK(c.in.find("0\r\n\r\n") != std::string::npos);
        BOOST_CHECK_EQUAL(written.load(), 100U);
        BOOST_CHECK(!refused.load());
    }
    {
        // HTTP/2: a client that opens no window and pings all the time, reading every PING ACK: the ACKs are no progress
        // on its replies: the stream is reset, or the connection closed, within the windows of 1 s. (The small events:
        // a stream that stays under its bound, so that its replies keep waiting.)
        small = true;
        RawClient c{to};
        H2Bytes client{{{NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 0}}};
        client.Request("/grpc.health.v1.Health/Watch", GrpcEnvelope(""));
        BOOST_REQUIRE(c.Send(client.Out()));
        const std::string ping{std::string{"\0\0\x08\x06\0\0\0\0\0", 9} + std::string(8, 'p')};
        const auto start{NodeClock::now()};
        bool reset{false};
        while (!c.closed && !reset && NodeClock::now() - start < 6s) {
            if (!c.Send(ping)) break;
            c.ReadFor(20ms);
            reset = Has(FrameTypes(c.in), NGHTTP2_RST_STREAM);
        }
        BOOST_CHECK(reset || c.closed);
        const double took{Seconds(NodeClock::now() - start)};
        BOOST_CHECK_MESSAGE(took < 4, "pinging zero-window client ended after " << took << " s");
        BOOST_CHECK(Has(FrameTypes(c.in), NGHTTP2_PING));
    }
    server.Stop();
}

BOOST_AUTO_TEST_SUITE_END()

#endif // ENABLE_PUBLICAPI
