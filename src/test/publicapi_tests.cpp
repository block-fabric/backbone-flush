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
#include <util/strencodings.h>

#include <nghttp2/nghttp2.h>

#include <boost/test/unit_test.hpp>

#include <map>
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

BOOST_AUTO_TEST_SUITE_END()

#endif // ENABLE_PUBLICAPI
