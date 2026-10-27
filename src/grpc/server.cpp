// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <grpc/server.h>

#include <logging.h>
#include <netbase.h>
#include <tinyformat.h>
#include <util/sock.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/thread.h>
#include <util/threadpool.h>
#include <crypto/sha256.h>
#include <util/time.h>

#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <set>

namespace grpcsrv {

static constexpr int SOCKET_OPTION_TRUE{1};

std::string ConnectCodeName(Code code)
{
    switch (code) {
    case Code::OK: return "ok";
    case Code::CANCELLED: return "canceled";
    case Code::UNKNOWN: return "unknown";
    case Code::INVALID_ARGUMENT: return "invalid_argument";
    case Code::DEADLINE_EXCEEDED: return "deadline_exceeded";
    case Code::NOT_FOUND: return "not_found";
    case Code::ALREADY_EXISTS: return "already_exists";
    case Code::PERMISSION_DENIED: return "permission_denied";
    case Code::RESOURCE_EXHAUSTED: return "resource_exhausted";
    case Code::FAILED_PRECONDITION: return "failed_precondition";
    case Code::ABORTED: return "aborted";
    case Code::OUT_OF_RANGE: return "out_of_range";
    case Code::UNIMPLEMENTED: return "unimplemented";
    case Code::INTERNAL: return "internal";
    case Code::UNAVAILABLE: return "unavailable";
    case Code::DATA_LOSS: return "data_loss";
    case Code::UNAUTHENTICATED: return "unauthenticated";
    }
    return "unknown";
}

int ConnectHttpStatus(Code code)
{
    // The Connect protocol's mapping (connectrpc.com/docs/protocol, "Error codes").
    switch (code) {
    case Code::OK: return 200;
    case Code::CANCELLED: return 499;
    case Code::INVALID_ARGUMENT: case Code::FAILED_PRECONDITION: case Code::OUT_OF_RANGE: return 400;
    case Code::DEADLINE_EXCEEDED: return 504;
    case Code::NOT_FOUND: return 404;
    case Code::ALREADY_EXISTS: case Code::ABORTED: return 409;
    case Code::PERMISSION_DENIED: return 403;
    case Code::RESOURCE_EXHAUSTED: return 429;
    case Code::UNIMPLEMENTED: return 501;
    case Code::UNAVAILABLE: return 503;
    case Code::UNAUTHENTICATED: return 401;
    default: return 500;
    }
}

std::string PercentEncode(std::string_view message)
{
    std::string out;
    for (const unsigned char c : message) {
        if (c >= 0x20 && c <= 0x7e && c != '%') {
            out.push_back(static_cast<char>(c));
        } else {
            out += strprintf("%%%02X", c);
        }
    }
    return out;
}

namespace {

enum class Protocol { GRPC, GRPC_WEB, CONNECT_UNARY, CONNECT_STREAM, REST };

/** Whether a protocol answers once the call is over, with an HTTP status of its own. */
bool Unary(Protocol p) { return p == Protocol::CONNECT_UNARY || p == Protocol::REST; }

std::string RestErrorJson(const Status& status)
{
    UniValue err(UniValue::VOBJ);
    err.pushKV("code", ConnectCodeName(status.code));
    err.pushKV("message", status.message);
    UniValue body(UniValue::VOBJ);
    body.pushKV("error", std::move(err));
    return body.write();
}

std::string ConnectErrorJson(const Status& status)
{
    UniValue err(UniValue::VOBJ);
    err.pushKV("code", ConnectCodeName(status.code));
    if (!status.message.empty()) err.pushKV("message", status.message);
    return err.write();
}

void AppendEnvelope(std::string& out, uint8_t flags, std::string_view payload)
{
    out.push_back(static_cast<char>(flags));
    const uint32_t size{static_cast<uint32_t>(payload.size())};
    for (int shift{24}; shift >= 0; shift -= 8) out.push_back(static_cast<char>((size >> shift) & 0xff));
    out.append(payload);
}

/** A call in progress: the Writer its handler has. */
class Call : public Writer
{
public:
    Call(Protocol protocol, bool json, const pb::MethodDesc* method, size_t max_response = 0) : m_protocol{protocol}, m_json{json}, m_method{method}, m_max_response{max_response} {}

    bool Write(const pb::Message& reply) override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        std::string payload;
        std::optional<std::string> raw_type;
        if (m_protocol == Protocol::REST) {
            std::optional<std::pair<std::string, std::string>> raw;
            if (m_raw) raw = m_raw(reply);
            if (raw) {
                raw_type = std::move(raw->first);
                payload = std::move(raw->second);
            } else {
                payload = reply.ToJson(pb::Message::JsonOptions{.proto_names = true, .emit_unpopulated = true}).write();
            }
        } else {
            payload = m_json ? reply.ToJson().write() : reply.Serialize();
        }
        std::vector<std::function<void()>> callbacks;
        {
            LOCK(m_mutex);
            if (m_finished || m_cancelled) return false;
            if (m_max_response > 0 && m_out.size() + payload.size() > m_max_response) {
                // Too big to send: the call ends with an error instead (nothing of it is sent).
                m_out.clear();
                m_finished = true;
                m_status = Status{Code::RESOURCE_EXHAUSTED, strprintf("reply larger than %u bytes: ask for less (a smaller page)", m_max_response)};
                if (m_protocol == Protocol::REST) m_out = RestErrorJson(m_status);
                if (m_protocol == Protocol::CONNECT_UNARY) m_out = ConnectErrorJson(m_status);
                callbacks.swap(m_on_close);
            } else {
                if (raw_type) m_raw_type = *raw_type;
                if (Unary(m_protocol)) {
                    m_out = payload;
                } else {
                    AppendEnvelope(m_out, 0, payload);
                }
                ++m_writes;
            }
        }
        for (auto& callback : callbacks) callback();
        return callbacks.empty();
    }

    void SetHeader(const std::string& name, const std::string& value) override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        m_headers.emplace_back(name, value);
    }
    std::vector<std::pair<std::string, std::string>> Headers() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        return m_headers;
    }
    void SetRaw(std::function<std::optional<std::pair<std::string, std::string>>(const pb::Message&)> raw) { m_raw = std::move(raw); }
    /** The whole body of a unary reply, once the call is over (taken: nothing is left to send after). */
    std::string TakeAll() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        std::string out;
        out.swap(m_out);
        return out;
    }

    void Finish(const Status& status) override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        std::vector<std::function<void()>> callbacks;
        {
            LOCK(m_mutex);
            if (m_finished || m_cancelled) return;
            m_finished = true;
            m_status = status;
            switch (m_protocol) {
            case Protocol::GRPC:
                break; // In the trailers.
            case Protocol::GRPC_WEB: {
                std::string trailers{strprintf("grpc-status: %d\r\n", static_cast<int>(status.code))};
                if (!status.message.empty()) trailers += "grpc-message: " + PercentEncode(status.message) + "\r\n";
                AppendEnvelope(m_out, 0x80, trailers);
                break;
            }
            case Protocol::CONNECT_STREAM: {
                UniValue end(UniValue::VOBJ);
                if (!status.ok()) {
                    UniValue err(UniValue::VOBJ);
                    err.pushKV("code", ConnectCodeName(status.code));
                    if (!status.message.empty()) err.pushKV("message", status.message);
                    end.pushKV("error", std::move(err));
                }
                AppendEnvelope(m_out, 0x02, end.write());
                break;
            }
            case Protocol::CONNECT_UNARY:
                if (!status.ok()) m_out = ConnectErrorJson(status);
                break;
            case Protocol::REST:
                if (!status.ok()) m_out = RestErrorJson(status);
                break;
            }
            callbacks.swap(m_on_close);
        }
        for (auto& callback : callbacks) callback();
    }

    bool Closed() const override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        return m_finished || m_cancelled;
    }

    size_t Pending() const override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        return m_out.size();
    }

    void OnClose(std::function<void()> callback) override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        {
            LOCK(m_mutex);
            if (!m_finished && !m_cancelled) {
                m_on_close.push_back(std::move(callback));
                return;
            }
        }
        callback();
    }

    /** The client went away. */
    void Cancel() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        std::vector<std::function<void()>> callbacks;
        {
            LOCK(m_mutex);
            if (m_cancelled) return;
            m_cancelled = true;
            callbacks.swap(m_on_close);
        }
        for (auto& callback : callbacks) callback();
    }

    /** Take up to `max` bytes of the body written so far. */
    std::string Take(size_t max) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        const size_t n{std::min(max, m_out.size())};
        std::string chunk{m_out.substr(0, n)};
        m_out.erase(0, n);
        return chunk;
    }

    struct State {
        bool has_data;
        bool finished;
        Status status;
        uint64_t writes;
    };
    State GetState() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        return {!m_out.empty(), m_finished, m_status, m_writes};
    }

    Protocol GetProtocol() const { return m_protocol; }
    bool Json() const { return m_json; }
    const pb::MethodDesc* Method() const { return m_method; }

    std::string ContentType() const
    {
        switch (m_protocol) {
        case Protocol::GRPC: return m_json ? "application/grpc+json" : "application/grpc";
        case Protocol::GRPC_WEB: return m_json ? "application/grpc-web+json" : "application/grpc-web+proto";
        case Protocol::CONNECT_STREAM: return m_json ? "application/connect+json" : "application/connect+proto";
        case Protocol::CONNECT_UNARY: return m_json ? "application/json" : "application/proto";
        case Protocol::REST: {
            LOCK(m_mutex);
            return m_raw_type.empty() ? "application/json; charset=utf-8" : m_raw_type;
        }
        }
        return {};
    }

private:
    const Protocol m_protocol;
    const bool m_json;
    const pb::MethodDesc* const m_method;
    const size_t m_max_response;
    std::function<std::optional<std::pair<std::string, std::string>>(const pb::Message&)> m_raw;
    mutable Mutex m_mutex;
    std::vector<std::pair<std::string, std::string>> m_headers GUARDED_BY(m_mutex);
    std::string m_raw_type GUARDED_BY(m_mutex);
    std::string m_out GUARDED_BY(m_mutex);
    bool m_finished GUARDED_BY(m_mutex){false};
    bool m_cancelled GUARDED_BY(m_mutex){false};
    Status m_status GUARDED_BY(m_mutex);
    uint64_t m_writes GUARDED_BY(m_mutex){0};
    std::vector<std::function<void()>> m_on_close GUARDED_BY(m_mutex);
};

std::string ToLowerAscii(std::string s)
{
    for (char& c : s) c = ToLower(c);
    return s;
}

/** A request, as both transports collect it. */
struct Request {
    std::string method;
    std::string path;
    std::string query;
    std::map<std::string, std::string> headers;
    std::string body;
    //! Envelopes of the body already handed to a client streaming handler.
    size_t consumed{0};
    std::shared_ptr<Call> call;
    const Server::MethodEntry* entry{nullptr};
    //! Not a content type of these protocols: HTTP 415.
    bool bad_type{false};
    //! Who sent it, and the client it is for (a trusted proxy's word).
    CService peer;
    std::string client;
    //! The Origin, if it is allowed and the server answers web pages (Options::cors).
    std::string cors_origin;
    //! A CORS preflight (OPTIONS), answered at once.
    bool preflight{false};
    //! A request of the REST face, routed once its body is in; and how its reply is written raw.
    bool rest{false};
    //! Messages a client stream sent so far.
    size_t messages{0};
    //! All of it is in (END_STREAM): its call runs, or ran.
    bool ended{false};
    //! When its first bytes came (HTTP/2: its HEADERS frame).
    NodeClock::time_point begun{};
};

std::string Header(const Request& r, const std::string& name)
{
    const auto it{r.headers.find(name)};
    return it == r.headers.end() ? std::string{} : it->second;
}

std::optional<std::string> UrlDecode(std::string_view s)
{
    std::string out;
    for (size_t i{0}; i < s.size(); ++i) {
        if (s[i] == '+') {
            out.push_back(' ');
        } else if (s[i] == '%') {
            if (i + 2 >= s.size()) return std::nullopt;
            const auto v{TryParseHex<uint8_t>(s.substr(i + 1, 2))};
            if (!v || v->size() != 1) return std::nullopt;
            out.push_back(static_cast<char>((*v)[0]));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

/** What a request asks for, from its content type and method. */
struct Kind {
    Protocol protocol;
    bool json;
};

std::optional<Kind> KindOf(const Request& r, bool http2)
{
    std::string ct{ToLowerAscii(Header(r, "content-type"))};
    if (const auto semi{ct.find(';')}; semi != std::string::npos) ct.resize(semi);
    ct = std::string{util::TrimStringView(ct)};
    if (r.method == "GET") return Kind{Protocol::CONNECT_UNARY, true}; // the encoding is in the query
    if (r.method != "POST") return std::nullopt;
    if (ct == "application/grpc" || ct == "application/grpc+proto") return http2 ? std::optional<Kind>{Kind{Protocol::GRPC, false}} : std::nullopt;
    if (ct == "application/grpc+json") return http2 ? std::optional<Kind>{Kind{Protocol::GRPC, true}} : std::nullopt;
    if (ct == "application/grpc-web" || ct == "application/grpc-web+proto") return Kind{Protocol::GRPC_WEB, false};
    if (ct == "application/grpc-web+json") return Kind{Protocol::GRPC_WEB, true};
    if (ct == "application/connect+proto") return Kind{Protocol::CONNECT_STREAM, false};
    if (ct == "application/connect+json") return Kind{Protocol::CONNECT_STREAM, true};
    if (ct == "application/proto") return Kind{Protocol::CONNECT_UNARY, false};
    if (ct == "application/json") return Kind{Protocol::CONNECT_UNARY, true};
    return std::nullopt;
}

/**
 * Read the envelopes of `r.body` from `r.consumed` on: each complete one is passed to `each`
 * (false: stop). @return the status to end the call with, if one is malformed.
 */
std::optional<Status> TakeEnvelopes(Request& r, size_t max_bytes, const std::function<bool(std::string_view)>& each)
{
    for (;;) {
        const std::string_view rest{std::string_view{r.body}.substr(r.consumed)};
        if (rest.size() < 5) return std::nullopt;
        const uint8_t flags{static_cast<uint8_t>(rest[0])};
        const uint32_t size{(uint32_t{static_cast<uint8_t>(rest[1])} << 24) | (uint32_t{static_cast<uint8_t>(rest[2])} << 16) |
                            (uint32_t{static_cast<uint8_t>(rest[3])} << 8) | uint32_t{static_cast<uint8_t>(rest[4])}};
        if (size > max_bytes) return Status{Code::RESOURCE_EXHAUSTED, strprintf("request message larger than %u bytes", max_bytes)};
        if (rest.size() < 5 + size) return std::nullopt;
        if (flags & 0x01) return Status{Code::UNIMPLEMENTED, "compressed messages are not supported (only identity)"};
        r.consumed += 5 + size;
        // The end-of-stream envelope of a Connect client stream carries no message.
        if (flags & 0x02) continue;
        if (!each(rest.substr(5, size))) return std::nullopt;
    }
}

bool DecodeMessage(bool json, std::string_view bytes, pb::Message& out, std::string& error)
{
    if (json) {
        UniValue value;
        if (bytes.empty()) bytes = "{}";
        if (!value.read(bytes)) {
            error = "invalid JSON";
            return false;
        }
        return out.FromJson(value, error);
    }
    return out.Parse(MakeUCharSpan(bytes), error);
}

/** Begin a call for a request whose headers are in. It may end at once (an unknown method, say). */
/** A Host or :authority a browser could not have been tricked into (DNS rebinding): an IP literal, localhost, or one allowed. */
bool HostAllowed(const std::string& host_port, const std::vector<std::string>& allowed)
{
    if (host_port.empty()) return true;
    std::string host{ToLowerAscii(host_port)};
    if (host.starts_with('[')) return host.find(']') != std::string::npos; // an IPv6 literal
    if (const auto colon{host.rfind(':')}; colon != std::string::npos) host.resize(colon);
    if (host == "localhost" || std::find(allowed.begin(), allowed.end(), host) != allowed.end()) return true;
    return !host.empty() && std::all_of(host.begin(), host.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; });
}

/** The client a request is for: the peer, or what a proxy it trusts says (X-Real-IP, else the last of X-Forwarded-For).
 *  Keyed as its connections are counted (Server::AddressKey): an IPv6 client by its prefix, so that rotating addresses
 *  in its /64 does not give it fresh rate limits. */
std::string ClientOf(const Server& server, const Request& r)
{
    const Options& options{server.GetOptions()};
    const std::string peer{r.peer.IsValid() ? Server::AddressKey(r.peer, options.ipv6_prefix) : "local"};
    if (options.proxy_header.empty()) return peer;
    const bool trusted{r.peer.IsValid() && std::any_of(options.trusted_proxies.begin(), options.trusted_proxies.end(), [&](const CSubNet& s) { return s.Match(r.peer); })};
    if (!trusted) return peer;
    std::string forwarded{Header(r, options.proxy_header)};
    // X-Forwarded-For: the right-most entry, the one the trusted proxy appended (those before it are the client's word).
    if (const auto comma{forwarded.rfind(',')}; comma != std::string::npos) forwarded = forwarded.substr(comma + 1);
    forwarded = util::TrimString(forwarded);
    if (const auto addr{LookupHost(forwarded, /*fAllowLookup=*/false)}) return Server::AddressKey(*addr, options.ipv6_prefix);
    return peer;
}

void BeginCall(Server& server, Request& r, bool http2)
{
    const Options& options{server.GetOptions()};
    const size_t max_response{options.max_response_bytes};
    r.client = ClientOf(server, r);
    const bool rest{!options.rest_prefix.empty() && options.rest && r.path.starts_with(options.rest_prefix)};
    const auto kind{KindOf(r, http2)};
    const Server::MethodEntry* entry{rest ? nullptr : server.Find(r.path)};
    const pb::MethodDesc* method{entry ? entry->desc : nullptr};
    const auto refuse{[&](Status status) {
        r.call = std::make_shared<Call>(rest ? Protocol::REST : kind ? kind->protocol : Protocol::CONNECT_UNARY, kind ? kind->json : true, method, max_response);
        r.call->Finish(status);
    }};
    // Requests a web page could make (the server has no authentication, as the enforcer's): one from a page
    // (an Origin header) is refused but from the origins allowed, as is a Host that is not this machine's.
    const std::string origin{Header(r, "origin")};
    if (!origin.empty() && std::find(options.allowed_origins.begin(), options.allowed_origins.end(), origin) == options.allowed_origins.end()) {
        return refuse({Code::PERMISSION_DENIED, "requests from web pages (Origin " + origin + ") are refused but from the origins allowed"});
    }
    if (!HostAllowed(Header(r, "host"), options.allowed_hosts)) {
        return refuse({Code::PERMISSION_DENIED, "host " + Header(r, "host") + " refused (an IP address or localhost, or one of the hosts allowed)"});
    }
    if (options.cors && !origin.empty()) r.cors_origin = origin;
    if (r.method == "OPTIONS") {
        // A CORS preflight: answered for the origins allowed (above), with what they may send.
        if (!options.cors || origin.empty()) return refuse({Code::UNIMPLEMENTED, "OPTIONS is for CORS preflights"});
        r.preflight = true;
        r.call = std::make_shared<Call>(Protocol::REST, true, nullptr, max_response);
        r.call->Finish({});
        return;
    }
    if (rest) {
        // Routed once the body is in (EndRequest): GET for the reads, POST for what has a body.
        if (r.method != "GET" && r.method != "POST") return refuse({Code::UNIMPLEMENTED, "the REST face takes GET (and POST for broadcast)"});
        r.rest = true;
        r.call = std::make_shared<Call>(Protocol::REST, true, nullptr, max_response);
        return;
    }
    if (!kind) {
        // Not a request of these protocols: answered with a Connect error (HTTP 415).
        r.call = std::make_shared<Call>(Protocol::CONNECT_UNARY, true, method, max_response);
        r.bad_type = true;
        r.call->Finish({Code::UNKNOWN, "unsupported content type " + Header(r, "content-type")});
        return;
    }
    r.call = std::make_shared<Call>(kind->protocol, kind->json, method, max_response);
    if (!entry) {
        r.call->Finish({Code::UNIMPLEMENTED, "unknown procedure " + r.path});
        return;
    }
    // Connect's GET form only for reads (idempotency_level NO_SIDE_EFFECTS), as the protocol says.
    if (r.method == "GET" && !method->no_side_effects) {
        r.call->Finish({Code::INVALID_ARGUMENT, "GET is for methods without side effects; POST " + r.path});
        return;
    }
    if (const Guard& guard{server.GetGuard()}) {
        if (const auto refused{guard(CallInfo{r.path, r.headers, r.client, false})}) {
            r.call->Finish(*refused);
            return;
        }
    }
    const bool streaming{method->client_streaming || method->server_streaming};
    if (kind->protocol == Protocol::CONNECT_UNARY && streaming) {
        r.call->Finish({Code::UNIMPLEMENTED, "a streaming procedure needs the application/connect+proto or +json content type"});
        return;
    }
    if (kind->protocol == Protocol::CONNECT_STREAM && !streaming) {
        r.call->Finish({Code::UNIMPLEMENTED, "a unary procedure takes the application/proto or application/json content type"});
        return;
    }
    if (method->client_streaming && kind->protocol != Protocol::GRPC) {
        r.call->Finish({Code::UNIMPLEMENTED, "client streaming is served over gRPC (HTTP/2) only"});
        return;
    }
    const std::string encoding{ToLowerAscii(Header(r, kind->protocol == Protocol::CONNECT_UNARY ? "content-encoding" : (kind->protocol == Protocol::CONNECT_STREAM ? "connect-content-encoding" : "grpc-encoding")))};
    if (!encoding.empty() && encoding != "identity") {
        r.call->Finish({Code::UNIMPLEMENTED, "compression " + encoding + " is not supported (only identity)"});
        return;
    }
    r.entry = entry;
}

/** A REST request whose body is in: mapped onto its method (Options::rest), checked, and run. */
void RunRest(Server& server, Request& r)
{
    const Options& options{server.GetOptions()};
    if (r.body.size() > options.max_request_bytes) return r.call->Finish({Code::RESOURCE_EXHAUSTED, strprintf("request larger than %u bytes", options.max_request_bytes)});
    std::variant<RestMatch, Status> routed;
    try {
        routed = options.rest(r.method, r.path.substr(options.rest_prefix.size()), r.query, r.body);
    } catch (const std::exception& e) {
        return r.call->Finish({Code::INVALID_ARGUMENT, e.what()});
    }
    if (auto* status{std::get_if<Status>(&routed)}) return r.call->Finish(*status);
    RestMatch& match{std::get<RestMatch>(routed)};
    const Server::MethodEntry* entry{server.Find(match.method_path)};
    if (!entry || !match.request) return r.call->Finish({Code::NOT_FOUND, "no such resource"});
    // GET only for reads; what has side effects is a POST. A read may be a POST where its route says so (the routes
    // decide: addresses/portfolio with a long list in a body).
    if (r.method == "GET" && !entry->desc->no_side_effects) return r.call->Finish({Code::UNIMPLEMENTED, "POST this resource"});
    if (const Guard& guard{server.GetGuard()}) {
        if (const auto refused{guard(CallInfo{match.method_path, r.headers, r.client, true, match.request.get()})}) return r.call->Finish(*refused);
    }
    r.call->SetRaw(std::move(match.raw));
    std::shared_ptr<Call> call{r.call};
    Handler handler{entry->handler};
    server.Run([handler, call, request = std::move(*match.request)]() {
        if (call->Closed()) return;
        try {
            handler(request, call);
        } catch (const std::exception& e) {
            call->Finish({Code::INTERNAL, e.what()});
        }
    });
}

/** More of the body of a call came in (a client streaming one is given its requests as they come). */
void ContinueCall(Server& server, Request& r)
{
    if (!r.entry || r.call->Closed() || !r.entry->desc->client_streaming) return;
    const auto status{TakeEnvelopes(r, server.GetOptions().max_request_bytes, [&](std::string_view bytes) {
        // Each message of the stream is a call of its own for the limits: the guard again, and a bound on how many.
        if (++r.messages > server.GetOptions().max_stream_messages) {
            r.call->Finish({Code::RESOURCE_EXHAUSTED, strprintf("at most %u messages in a stream", server.GetOptions().max_stream_messages)});
            return false;
        }
        if (r.messages > 1) {
            if (const Guard& guard{server.GetGuard()}) {
                if (const auto refused{guard(CallInfo{r.path, r.headers, r.client, false})}) {
                    r.call->Finish(*refused);
                    return false;
                }
            }
        }
        pb::Message request{*r.entry->desc->input};
        std::string error;
        if (!DecodeMessage(r.call->Json(), bytes, request, error)) {
            r.call->Finish({Code::INVALID_ARGUMENT, "failed to decode the request: " + error});
            return false;
        }
        r.entry->handler(request, r.call);
        return !r.call->Closed();
    })};
    if (status) r.call->Finish(*status);
    // Only what is left unread is kept.
    r.body.erase(0, r.consumed);
    r.consumed = 0;
}

/** The whole request is in: run it. */
void EndRequest(Server& server, Request& r)
{
    if (r.rest && r.call && !r.call->Closed()) return RunRest(server, r);
    if (!r.entry || r.call->Closed()) return;
    const pb::MethodDesc& method{*r.entry->desc};
    if (method.client_streaming) {
        ContinueCall(server, r);
        if (!r.call->Closed() && r.consumed != r.body.size()) {
            r.call->Finish({Code::INTERNAL, "incomplete request message"});
        }
        r.call->Finish({});
        return;
    }
    if (r.body.size() > server.GetOptions().max_request_bytes + 5) {
        r.call->Finish({Code::RESOURCE_EXHAUSTED, strprintf("request larger than %u bytes", server.GetOptions().max_request_bytes)});
        return;
    }
    std::optional<std::string> payload;
    if (r.call->GetProtocol() == Protocol::CONNECT_UNARY) {
        if (r.method == "GET") {
            // Connect's GET form: ?message=...&encoding=json|proto[&base64=1]
            std::map<std::string, std::string> params;
            for (const std::string& part : util::SplitString(r.query, '&')) {
                const auto eq{part.find('=')};
                const auto key{UrlDecode(part.substr(0, eq))};
                const auto value{UrlDecode(eq == std::string::npos ? "" : part.substr(eq + 1))};
                if (key && value) params[*key] = *value;
            }
            const bool json{params["encoding"] != "proto"};
            r.call = std::make_shared<Call>(Protocol::CONNECT_UNARY, json, &method, server.GetOptions().max_response_bytes);
            std::string message{params["message"]};
            if (params["base64"] == "1") {
                const auto decoded{pb::Base64Decode(message)};
                if (!decoded) {
                    r.call->Finish({Code::INVALID_ARGUMENT, "invalid base64 message"});
                    return;
                }
                message = *decoded;
            }
            payload = message;
        } else {
            payload = r.body;
        }
    } else {
        std::vector<std::string> messages;
        const auto status{TakeEnvelopes(r, server.GetOptions().max_request_bytes, [&](std::string_view bytes) {
            messages.emplace_back(bytes);
            return true;
        })};
        if (status) {
            r.call->Finish(*status);
            return;
        }
        if (messages.size() != 1 || r.consumed != r.body.size()) {
            r.call->Finish({Code::INTERNAL, messages.empty() ? "missing request message" : "a unary request takes one message"});
            return;
        }
        payload = std::move(messages[0]);
    }
    pb::Message request{*method.input};
    std::string error;
    if (!DecodeMessage(r.call->Json(), *payload, request, error)) {
        r.call->Finish({Code::INVALID_ARGUMENT, "failed to decode the request: " + error});
        return;
    }
    // Decoded: the guard again, for what the request itself costs beyond its method's base (a portfolio of many addresses).
    if (const Guard& guard{server.GetGuard()}) {
        if (const auto refused{guard(CallInfo{r.path, r.headers, r.client, false, &request, true})}) {
            r.call->Finish(*refused);
            return;
        }
    }
    std::shared_ptr<Call> call{r.call};
    Handler handler{r.entry->handler};
    server.Run([handler, call, request = std::move(request)]() {
        if (call->Closed()) return;
        try {
            handler(request, call);
        } catch (const std::exception& e) {
            call->Finish({Code::INTERNAL, e.what()});
        }
    });
}

/** What a unary reply (Connect unary, REST, a preflight) is on the wire: its status, headers and body. */
struct UnaryReply {
    int status{200};
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

/** The CORS headers of a reply to a request from an origin allowed (Options::cors): never credentials. */
void AddCors(const Request& r, std::vector<std::pair<std::string, std::string>>& headers)
{
    if (r.cors_origin.empty()) return;
    headers.emplace_back("access-control-allow-origin", r.cors_origin);
    headers.emplace_back("vary", "Origin");
    headers.emplace_back("access-control-expose-headers", "grpc-status, grpc-message, etag, retry-after, content-length");
}

UnaryReply MakeUnaryReply(const Server& server, const Request& r, Call& call, const Call::State& state)
{
    UnaryReply out;
    const Options& options{server.GetOptions()};
    if (r.preflight) {
        out.status = 204;
        AddCors(r, out.headers);
        out.headers.emplace_back("access-control-allow-methods", "GET, POST, OPTIONS");
        out.headers.emplace_back("access-control-allow-headers", "content-type, connect-protocol-version, connect-timeout-ms, x-grpc-web, x-user-agent, grpc-timeout, if-none-match");
        out.headers.emplace_back("access-control-max-age", "600");
        for (const auto& h : options.response_headers) out.headers.push_back(h);
        return out;
    }
    out.body = call.TakeAll();
    if (state.status.ok()) {
        out.status = 200;
        out.headers.emplace_back("content-type", call.ContentType());
        for (auto& h : call.Headers()) out.headers.push_back(std::move(h));
        if (r.method == "GET") {
            // A strong validator of the body (what is cached per block changes with the blocks).
            uint8_t hash[CSHA256::OUTPUT_SIZE];
            CSHA256().Write(MakeUCharSpan(out.body).data(), out.body.size()).Finalize(hash);
            const std::string etag{"\"" + HexStr(std::span{hash}.first(16)) + "\""};
            out.headers.emplace_back("etag", etag);
            if (Header(r, "if-none-match") == etag) {
                out.status = 304;
                out.body.clear();
                std::erase_if(out.headers, [](const auto& h) { return h.first == "content-type"; });
            }
        }
    } else {
        out.status = r.bad_type ? 415 : ConnectHttpStatus(state.status.code);
        out.headers.emplace_back("content-type", "application/json");
        if (options.cors || options.rest) out.headers.emplace_back("cache-control", "no-store");
        for (const auto& h : state.status.headers) out.headers.push_back(h);
    }
    AddCors(r, out.headers);
    for (const auto& h : options.response_headers) out.headers.push_back(h);
    return out;
}

std::string ReasonPhrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 415: return "Unsupported Media Type";
    case 429: return "Too Many Requests";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return "Error";
    }
}

} // namespace

// ---------------------------------------------------------------------------
// The connection

struct Connection::Impl {
    explicit Impl(Server& server, CService peer) : m_server{server}, m_peer{std::move(peer)} {}
    virtual ~Impl() = default;
    virtual bool Receive(std::span<const uint8_t> data) = 0;
    virtual bool Poll() = 0;
    virtual bool WantsWrite() const = 0;
    virtual void CloseAll() = 0;
    //! A call is in progress (a connection idle otherwise may be closed: Options::idle_timeout_seconds).
    virtual bool Busy() const = 0;
    //! A request is being read (Options::request_timeout_seconds).
    virtual bool Receiving() const = 0;
    //! Before closing: HTTP/2 says GOAWAY.
    virtual void Goodbye() {}
    //! HTTP/2: when the oldest request still being read began (each stream is timed on its own).
    virtual std::optional<NodeClock::time_point> OldestRequest() const { return std::nullopt; }
    //! Reply bytes the calls wrote that are not handed to the connection yet (HTTP/2: held back by flow control).
    virtual size_t Undelivered() const { return 0; }
    //! No new request (HTTP/2: a GOAWAY naming the last stream taken); the running calls go on.
    virtual void Drain() {}
    //! Requests begun, all in, and streams reset before their call was over (by the client, or by the server
    //! because of it), each counted once.
    uint64_t m_begun{0}, m_completed{0}, m_abandoned{0};
    std::string m_out;
    //! Backpressure: nothing more is taken from the calls (Connection::Hold).
    bool m_hold{false};
    //! HTTP/2: bytes put in m_out, and of them the replies' DATA payload; the rest (PING and SETTINGS ACKs, frame
    //! headers, resets) a client can ask for at will, which is no progress on its replies (Connection::ControlBytes).
    uint64_t m_serialized{0}, m_payload{0};
    Server& m_server;
    CService m_peer;
};

namespace {

#if NGHTTP2_VERSION_NUM >= 0x013d00
using ngssize = nghttp2_ssize;
#else
using ngssize = ssize_t;
#endif

nghttp2_nv MakeNv(const std::string& name, const std::string& value)
{
    return {reinterpret_cast<uint8_t*>(const_cast<char*>(name.data())), reinterpret_cast<uint8_t*>(const_cast<char*>(value.data())),
            name.size(), value.size(), NGHTTP2_NV_FLAG_NONE};
}

constexpr std::string_view H2_PREFACE{"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"};

class Http2 : public Connection::Impl
{
public:
    explicit Http2(Server& server, CService peer) : Impl{server, std::move(peer)}
    {
        nghttp2_session_callbacks* callbacks;
        nghttp2_session_callbacks_new(&callbacks);
        nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, OnBeginHeaders);
        nghttp2_session_callbacks_set_on_header_callback(callbacks, OnHeader);
        nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, OnFrameRecv);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, OnDataChunk);
        nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, OnStreamClose);
        nghttp2_option* option;
        nghttp2_option_new(&option);
        nghttp2_option_set_max_deflate_dynamic_table_size(option, 4096);
        // Flow control by hand: the window opens again only for the bytes taken in (see OnDataChunk).
        nghttp2_option_set_no_auto_window_update(option, 1);
#if NGHTTP2_VERSION_NUM >= 0x013900
        // nghttp2's own Rapid Reset bound (CVE-2023-44487; 1.57 on), stated: 1000 at once, 33 a second; ours beside it.
        nghttp2_option_set_stream_reset_rate_limit(option, 1000, 33);
#endif
        nghttp2_session_server_new2(&m_session, callbacks, this, option);
        nghttp2_option_del(option);
        nghttp2_session_callbacks_del(callbacks);
        const nghttp2_settings_entry settings[]{
            {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, server.GetOptions().max_streams},
            {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 1 << 20},
            {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, 64 << 10},
        };
        nghttp2_submit_settings(m_session, NGHTTP2_FLAG_NONE, settings, std::size(settings));
        nghttp2_session_set_local_window_size(m_session, NGHTTP2_FLAG_NONE, 0, 8 << 20);
    }
    ~Http2() override
    {
        CloseAll();
        nghttp2_session_del(m_session);
    }

    bool Receive(std::span<const uint8_t> data) override
    {
#if NGHTTP2_VERSION_NUM >= 0x013d00
        const ngssize n{nghttp2_session_mem_recv2(m_session, data.data(), data.size())};
#else
        const ngssize n{nghttp2_session_mem_recv(m_session, data.data(), data.size())};
#endif
        if (n < 0) {
            LogDebug(BCLog::HTTP, "gRPC: HTTP/2 error %s\n", nghttp2_strerror(static_cast<int>(n)));
            return false;
        }
        return Flush();
    }

    bool Poll() override
    {
        for (auto& [id, stream] : m_streams) {
            if (!stream.request.call) continue;
            Call& call{*stream.request.call};
            const Call::State state{call.GetState()};
            // A Connect unary (or REST) reply's HTTP status is its status: sent once the call is over.
            if (!stream.responded && Unary(call.GetProtocol()) && !state.finished) continue;
            if (!stream.responded && (state.has_data || state.finished)) {
                Respond(id, stream, state);
            } else if (stream.responded && stream.deferred && (state.has_data || state.finished)) {
                stream.deferred = false;
                nghttp2_session_resume_data(m_session, id);
            }
            // Replies held back by a window the client does not open (slow reads by flow control): the stream is
            // reset once they waited longer than a client may take no byte (Options::write_timeout_seconds).
            const int timeout{m_server.GetOptions().write_timeout_seconds};
            const bool waiting{state.has_data || (stream.unary && stream.unary_sent < stream.unary_body.size())};
            const bool blocked{stream.responded && waiting &&
                               (nghttp2_session_get_stream_remote_window_size(m_session, id) <= 0 || nghttp2_session_get_remote_window_size(m_session) <= 0)};
            if (!blocked) {
                stream.blocked_since.reset();
            } else if (!stream.blocked_since) {
                stream.blocked_since = NodeClock::now();
            } else if (timeout > 0 && NodeClock::now() - *stream.blocked_since > std::chrono::seconds{timeout}) {
                LogDebug(BCLog::HTTP, "%s: stream %d: its window stays closed: reset\n", m_server.Name(), id);
                call.Cancel();
                stream.blocked_since.reset();
                nghttp2_submit_rst_stream(m_session, NGHTTP2_FLAG_NONE, id, NGHTTP2_CANCEL);
            }
        }
        return Flush();
    }

    bool WantsWrite() const override { return !m_out.empty() || nghttp2_session_want_write(m_session); }
    bool Busy() const override
    {
        // Busy only while a call runs; a stream still being read is not (Receiving).
        return std::any_of(m_streams.begin(), m_streams.end(), [](const auto& s) { return s.second.request.ended && s.second.request.call && !s.second.request.call->Closed(); });
    }
    bool Receiving() const override
    {
        return std::any_of(m_streams.begin(), m_streams.end(), [](const auto& s) { return !s.second.request.ended; });
    }
    void Goodbye() override
    {
        nghttp2_session_terminate_session(m_session, NGHTTP2_NO_ERROR);
        Flush(/*force=*/true);
    }
    std::optional<NodeClock::time_point> OldestRequest() const override
    {
        std::optional<NodeClock::time_point> oldest;
        for (const auto& [id, stream] : m_streams) {
            if (!stream.request.ended && (!oldest || stream.request.begun < *oldest)) oldest = stream.request.begun;
        }
        return oldest;
    }
    size_t Undelivered() const override
    {
        size_t n{0};
        for (const auto& [id, stream] : m_streams) {
            if (stream.request.call) n += stream.request.call->Pending();
            if (stream.unary) n += stream.unary_body.size() - std::min(stream.unary_sent, stream.unary_body.size());
        }
        return n;
    }
    void Drain() override
    {
        if (m_draining) return;
        m_draining = true;
        nghttp2_submit_goaway(m_session, NGHTTP2_FLAG_NONE, nghttp2_session_get_last_proc_stream_id(m_session), NGHTTP2_NO_ERROR, nullptr, 0);
        Flush();
    }

    void CloseAll() override
    {
        for (auto& [id, stream] : m_streams) {
            if (stream.request.call) stream.request.call->Cancel();
            m_server.Release(stream.buffered);
        }
        m_buffered = 0;
        m_streams.clear();
    }

private:
    struct Stream {
        Request request;
        //! Request bytes counted against the budgets.
        size_t buffered{0};
        bool responded{false};
        bool deferred{false};
        bool trailers_sent{false};
        //! A unary reply, whole (MakeUnaryReply), and how much of it was sent.
        bool unary{false};
        std::string unary_body;
        size_t unary_sent{0};
        //! Since when its replies wait for a flow-control window the client does not open.
        std::optional<NodeClock::time_point> blocked_since;
        //! Counted among the streams reset (Connection::Impl::m_abandoned), once.
        bool counted{false};
    };

    /** What nghttp2 has to send, into m_out: nothing while held (that would take from the calls, ReadData), and up to
     * REPLY_HIGH_WATER at a time, unless `force` (the GOAWAY of a goodbye). */
    bool Flush(bool force = false)
    {
        while (force || (!m_hold && m_out.size() < REPLY_HIGH_WATER)) {
            const uint8_t* data;
#if NGHTTP2_VERSION_NUM >= 0x013d00
            const ngssize n{nghttp2_session_mem_send2(m_session, &data)};
#else
            const ngssize n{nghttp2_session_mem_send(m_session, &data)};
#endif
            if (n < 0) return false;
            if (n == 0) break;
            m_out.append(reinterpret_cast<const char*>(data), static_cast<size_t>(n));
            m_serialized += static_cast<uint64_t>(n);
        }
        return nghttp2_session_want_read(m_session) || nghttp2_session_want_write(m_session) || !m_out.empty();
    }

    void Respond(int32_t id, Stream& stream, const Call::State& state)
    {
        Call& call{*stream.request.call};
        stream.responded = true;
        std::vector<std::pair<std::string, std::string>> headers;
        const Protocol protocol{call.GetProtocol()};
        if (Unary(protocol)) {
            UnaryReply reply{MakeUnaryReply(m_server, stream.request, call, state)};
            headers.emplace_back(":status", util::ToString(reply.status));
            for (auto& h : reply.headers) headers.push_back(std::move(h));
            stream.unary = true;
            stream.unary_body = std::move(reply.body);
            std::vector<nghttp2_nv> nvs;
            for (const auto& [n, v] : headers) nvs.push_back(MakeNv(n, v));
#if NGHTTP2_VERSION_NUM >= 0x013d00
            nghttp2_data_provider2 provider;
#else
            nghttp2_data_provider provider;
#endif
            provider.source.ptr = &stream;
            provider.read_callback = ReadData;
#if NGHTTP2_VERSION_NUM >= 0x013d00
            nghttp2_submit_response2(m_session, id, nvs.data(), nvs.size(), stream.unary_body.empty() ? nullptr : &provider);
#else
            nghttp2_submit_response(m_session, id, nvs.data(), nvs.size(), stream.unary_body.empty() ? nullptr : &provider);
#endif
            return;
        }
        const bool trailers_only{protocol == Protocol::GRPC && state.finished && !state.has_data};
        if (protocol == Protocol::CONNECT_UNARY) {
            const int status{state.status.ok() ? 200 : stream.request.bad_type ? 415 : ConnectHttpStatus(state.status.code)};
            headers.emplace_back(":status", util::ToString(status));
            headers.emplace_back("content-type", state.status.ok() ? call.ContentType() : "application/json");
        } else {
            headers.emplace_back(":status", "200");
            headers.emplace_back("content-type", call.ContentType());
        }
        if (protocol == Protocol::GRPC) headers.emplace_back("grpc-accept-encoding", "identity");
        AddCors(stream.request, headers);
        for (const auto& h : m_server.GetOptions().response_headers) headers.push_back(h);
        if (trailers_only) {
            headers.emplace_back("grpc-status", util::ToString(static_cast<int>(state.status.code)));
            if (!state.status.message.empty()) headers.emplace_back("grpc-message", PercentEncode(state.status.message));
        }
        std::vector<nghttp2_nv> nvs;
        for (const auto& [n, v] : headers) nvs.push_back(MakeNv(n, v));
        if (trailers_only) {
            stream.trailers_sent = true;
#if NGHTTP2_VERSION_NUM >= 0x013d00
            nghttp2_submit_response2(m_session, id, nvs.data(), nvs.size(), nullptr);
#else
            nghttp2_submit_response(m_session, id, nvs.data(), nvs.size(), nullptr);
#endif
            return;
        }
#if NGHTTP2_VERSION_NUM >= 0x013d00
        nghttp2_data_provider2 provider;
#else
        nghttp2_data_provider provider;
#endif
        provider.source.ptr = &stream;
        provider.read_callback = ReadData;
#if NGHTTP2_VERSION_NUM >= 0x013d00
        nghttp2_submit_response2(m_session, id, nvs.data(), nvs.size(), &provider);
#else
        nghttp2_submit_response(m_session, id, nvs.data(), nvs.size(), &provider);
#endif
    }

    static ngssize ReadData(nghttp2_session* session, int32_t id, uint8_t* buf, size_t length, uint32_t* flags, nghttp2_data_source* source, void* user)
    {
        auto& self{*static_cast<Http2*>(user)};
        auto& stream{*static_cast<Stream*>(source->ptr)};
        if (stream.unary) {
            const size_t n{std::min(length, stream.unary_body.size() - stream.unary_sent)};
            std::memcpy(buf, stream.unary_body.data() + stream.unary_sent, n);
            stream.unary_sent += n;
            if (stream.unary_sent == stream.unary_body.size()) *flags |= NGHTTP2_DATA_FLAG_EOF;
            return static_cast<ngssize>(n);
        }
        Call& call{*stream.request.call};
        const std::string chunk{call.Take(length)};
        if (!chunk.empty()) {
            self.m_payload += chunk.size();
            std::memcpy(buf, chunk.data(), chunk.size());
            return static_cast<ngssize>(chunk.size());
        }
        const Call::State state{call.GetState()};
        if (!state.finished) {
            stream.deferred = true;
            return NGHTTP2_ERR_DEFERRED;
        }
        if (state.has_data) return NGHTTP2_ERR_DEFERRED; // not possible: taken above
        *flags |= NGHTTP2_DATA_FLAG_EOF;
        if (call.GetProtocol() == Protocol::GRPC && !stream.trailers_sent) {
            stream.trailers_sent = true;
            *flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
            const std::string code{util::ToString(static_cast<int>(state.status.code))};
            const std::string message{PercentEncode(state.status.message)};
            const std::string k_status{"grpc-status"}, k_message{"grpc-message"};
            std::vector<nghttp2_nv> nvs{MakeNv(k_status, code)};
            if (!message.empty()) nvs.push_back(MakeNv(k_message, message));
            nghttp2_submit_trailer(session, id, nvs.data(), nvs.size());
        }
        (void)self;
        return 0;
    }

    static int OnBeginHeaders(nghttp2_session*, const nghttp2_frame* frame, void* user)
    {
        auto& self{*static_cast<Http2*>(user)};
        if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
            Request& r{self.m_streams[frame->hd.stream_id].request};
            r.peer = self.m_peer;
            r.begun = NodeClock::now();
            ++self.m_begun;
        }
        return 0;
    }

    static int OnHeader(nghttp2_session*, const nghttp2_frame* frame, const uint8_t* name, size_t namelen, const uint8_t* value, size_t valuelen, uint8_t, void* user)
    {
        auto& self{*static_cast<Http2*>(user)};
        const auto it{self.m_streams.find(frame->hd.stream_id)};
        if (it == self.m_streams.end()) return 0;
        Request& r{it->second.request};
        const std::string n(reinterpret_cast<const char*>(name), namelen);
        std::string v(reinterpret_cast<const char*>(value), valuelen);
        if (n == ":method") {
            r.method = v;
        } else if (n == ":path") {
            const auto q{v.find('?')};
            r.path = v.substr(0, q);
            if (q != std::string::npos) r.query = v.substr(q + 1);
        } else if (n == ":authority") {
            r.headers["host"] = std::move(v);
        } else if (!n.starts_with(':')) {
            r.headers[n] = std::move(v);
        }
        return 0;
    }

    static int OnFrameRecv(nghttp2_session* session, const nghttp2_frame* frame, void* user)
    {
        auto& self{*static_cast<Http2*>(user)};
        const auto it{self.m_streams.find(frame->hd.stream_id)};
        if (it == self.m_streams.end()) return 0;
        // Every stream the client resets counts (Rapid Reset: a call cancelled as soon as asked is work for nothing).
        if (frame->hd.type == NGHTTP2_RST_STREAM) self.CountReset(it->second);
        Request& r{it->second.request};
        if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
            BeginCall(self.m_server, r, /*http2=*/true);
        }
        if ((frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA) && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) && r.call) {
            r.ended = true;
            ++self.m_completed;
            EndRequest(self.m_server, r);
            // The request is decoded: its bytes are no longer buffered.
            Stream& stream{it->second};
            std::string{}.swap(r.body);
            self.m_buffered -= stream.buffered;
            self.m_server.Release(stream.buffered);
            stream.buffered = 0;
        }
        (void)session;
        return 0;
    }

    static int OnDataChunk(nghttp2_session* session, uint8_t, int32_t id, const uint8_t* data, size_t len, void* user)
    {
        auto& self{*static_cast<Http2*>(user)};
        const Options& options{self.m_server.GetOptions()};
        const auto it{self.m_streams.find(id)};
        // Bytes not taken in still count against the connection's window: given back to it alone.
        if (it == self.m_streams.end()) {
            nghttp2_session_consume_connection(session, len);
            return 0;
        }
        Stream& stream{it->second};
        Request& r{stream.request};
        if (!r.call || r.call->Closed()) {
            nghttp2_session_consume_connection(session, len);
            return 0;
        }
        if (r.body.size() + len > options.max_request_bytes + 16) {
            r.call->Finish({Code::RESOURCE_EXHAUSTED, strprintf("request larger than %u bytes", options.max_request_bytes)});
            nghttp2_session_consume_connection(session, len);
            return 0;
        }
        // The budgets of the connection and of the server: a stream over them is reset.
        if (self.m_buffered + len > options.max_connection_buffer || !self.m_server.Reserve(len)) {
            r.call->Cancel();
            nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, id, NGHTTP2_ENHANCE_YOUR_CALM);
            nghttp2_session_consume_connection(session, len);
            return 0;
        }
        self.m_buffered += len;
        stream.buffered += len;
        r.body.append(reinterpret_cast<const char*>(data), len);
        nghttp2_session_consume(session, id, len);
        const size_t before{r.body.size()};
        ContinueCall(self.m_server, r);
        // A client stream's requests taken out of the body free their bytes.
        const size_t freed{std::min(stream.buffered, before - r.body.size())};
        stream.buffered -= freed;
        self.m_buffered -= freed;
        self.m_server.Release(freed);
        return 0;
    }

    void CountReset(Stream& stream)
    {
        if (stream.counted) return;
        stream.counted = true;
        ++m_abandoned;
    }

    static int OnStreamClose(nghttp2_session*, int32_t id, uint32_t error_code, void* user)
    {
        auto& self{*static_cast<Http2*>(user)};
        const auto it{self.m_streams.find(id)};
        if (it == self.m_streams.end()) return 0;
        // A stream closed before its call was over counts as reset, whoever reset it: a client can make the server
        // reset streams (malformed or oversized frames, flow-control errors: MadeYouReset) as well as reset them.
        const Request& r{it->second.request};
        if (!r.ended || error_code != NGHTTP2_NO_ERROR || (r.call && !r.call->GetState().finished)) self.CountReset(it->second);
        if (it->second.request.call) it->second.request.call->Cancel();
        self.m_buffered -= it->second.buffered;
        self.m_server.Release(it->second.buffered);
        self.m_streams.erase(it);
        return 0;
    }

    nghttp2_session* m_session{nullptr};
    std::map<int32_t, Stream> m_streams;
    bool m_draining{false};
    //! Request bytes of all the streams.
    size_t m_buffered{0};
};

/** HTTP/1.1: one request at a time (gRPC-Web and Connect; gRPC itself needs HTTP/2). */
class Http1 : public Connection::Impl
{
public:
    using Impl::Impl;
    ~Http1() override { CloseAll(); }
    bool Busy() const override { return m_current != nullptr; }
    bool Receiving() const override { return m_current == nullptr && (m_request != nullptr || !m_in.empty()); }
    size_t Undelivered() const override { return m_current && m_current->call ? m_current->call->Pending() : 0; }
    void Drain() override { m_draining = true; }

    bool Receive(std::span<const uint8_t> data) override
    {
        // While a request is served, the next can only be small (pipelining); never more than a request.
        const size_t cap{m_current ? size_t{64 << 10} : m_server.GetOptions().max_request_bytes + (64 << 10)};
        if (m_in.size() + data.size() > cap) return false;
        m_in.append(reinterpret_cast<const char*>(data.data()), data.size());
        return Parse();
    }

    bool Poll() override
    {
        if (!m_current) return m_open;
        // Backpressure: the socket has not taken what is waiting; the call's writes wait in it (max_response_bytes).
        if (m_hold) return true;
        Call& call{*m_current->call};
        const Call::State state{call.GetState()};
        if (Unary(call.GetProtocol())) {
            if (!state.finished) return true;
            const UnaryReply reply{MakeUnaryReply(m_server, *m_current, call, state)};
            m_out += strprintf("HTTP/1.1 %d %s\r\n", reply.status, ReasonPhrase(reply.status));
            for (const auto& [n, v] : reply.headers) m_out += n + ": " + v + "\r\n";
            if (reply.status != 204 && reply.status != 304) m_out += strprintf("Content-Length: %u\r\n", reply.body.size());
            m_out += m_keep_alive ? "\r\n" : "Connection: close\r\n\r\n";
            m_out += reply.body;
            m_current.reset();
            if (!m_keep_alive) m_open = false;
            return m_open && Parse();
        }
        if (!m_started && (state.has_data || state.finished)) {
            m_started = true;
            std::vector<std::pair<std::string, std::string>> headers;
            AddCors(*m_current, headers);
            for (const auto& h : m_server.GetOptions().response_headers) headers.push_back(h);
            std::string extra;
            for (const auto& [n, v] : headers) extra += n + ": " + v + "\r\n";
            m_out += strprintf("HTTP/1.1 200 OK\r\nContent-Type: %s\r\nTransfer-Encoding: chunked\r\n%s%s\r\n", call.ContentType(), extra, m_keep_alive ? "" : "Connection: close\r\n");
        }
        if (m_started) {
            const std::string chunk{call.Take(SIZE_MAX)};
            if (!chunk.empty()) m_out += strprintf("%x\r\n", chunk.size()) + chunk + "\r\n";
            if (state.finished && !call.GetState().has_data) {
                m_out += "0\r\n\r\n";
                m_current.reset();
                m_started = false;
                if (!m_keep_alive) m_open = false;
                return m_open && Parse();
            }
        }
        return true;
    }

    bool WantsWrite() const override { return !m_out.empty() || m_current; }

    void CloseAll() override
    {
        if (m_current && m_current->call) m_current->call->Cancel();
        m_current.reset();
    }

private:
    bool Parse()
    {
        while (m_open && !m_current) {
            // Draining: no next request; closed once the reply before is out.
            if (m_draining) return m_open = false;
            if (!m_request) {
                const auto end{m_in.find("\r\n\r\n")};
                if (end == std::string::npos) return m_in.size() <= (64 << 10);
                m_request = std::make_unique<Request>();
                ++m_begun;
                m_request->peer = m_peer;
                const std::vector<std::string> lines{util::SplitString(m_in.substr(0, end), '\n')};
                m_in.erase(0, end + 4);
                const std::vector<std::string> start{util::SplitString(util::TrimString(lines[0]), ' ')};
                if (start.size() != 3 || !start[2].starts_with("HTTP/1.")) return false;
                m_request->method = start[0];
                const auto q{start[1].find('?')};
                m_request->path = start[1].substr(0, q);
                if (q != std::string::npos) m_request->query = start[1].substr(q + 1);
                for (size_t i{1}; i < lines.size(); ++i) {
                    const auto colon{lines[i].find(':')};
                    if (colon == std::string::npos) continue;
                    m_request->headers[ToLowerAscii(util::TrimString(lines[i].substr(0, colon)))] = util::TrimString(lines[i].substr(colon + 1));
                }
                const std::string connection{ToLowerAscii(Header(*m_request, "connection"))};
                m_keep_alive = start[2] == "HTTP/1.1" ? connection != "close" : connection == "keep-alive";
                m_chunked = ToLowerAscii(Header(*m_request, "transfer-encoding")).find("chunked") != std::string::npos;
                m_length = 0;
                if (!m_chunked) {
                    const std::string cl{Header(*m_request, "content-length")};
                    if (!cl.empty()) {
                        const auto n{ToIntegral<uint64_t>(cl)};
                        if (!n) return false;
                        m_length = *n;
                    }
                }
                if (m_length > m_server.GetOptions().max_request_bytes + 16) {
                    BeginCall(m_server, *m_request, /*http2=*/false);
                    m_request->call->Finish({Code::RESOURCE_EXHAUSTED, "request too large"});
                    m_keep_alive = false;
                    m_current = std::move(m_request);
                    ++m_completed;
                    return true;
                }
            }
            // The body.
            if (m_chunked) {
                for (;;) {
                    const auto eol{m_in.find("\r\n")};
                    if (eol == std::string::npos) return true;
                    const auto size{TryParseHexInt(m_in.substr(0, eol))};
                    if (!size || *size > m_server.GetOptions().max_request_bytes) return false;
                    if (*size == 0) {
                        const auto end{m_in.find("\r\n\r\n", eol)};
                        const auto end2{m_in.compare(eol, 4, "\r\n\r\n") == 0 ? eol : end};
                        if (end2 == std::string::npos) return true;
                        m_in.erase(0, end2 + 4);
                        break;
                    }
                    if (m_in.size() < eol + 2 + *size + 2) return true;
                    m_request->body.append(m_in, eol + 2, *size);
                    if (m_request->body.size() > m_server.GetOptions().max_request_bytes + 16) return false;
                    m_in.erase(0, eol + 2 + *size + 2);
                }
            } else {
                if (m_in.size() < m_length) return true;
                m_request->body = m_in.substr(0, m_length);
                m_in.erase(0, m_length);
            }
            BeginCall(m_server, *m_request, /*http2=*/false);
            EndRequest(m_server, *m_request);
            m_current = std::move(m_request);
            ++m_completed;
        }
        return m_open;
    }

    static std::optional<uint64_t> TryParseHexInt(const std::string& s)
    {
        std::string t{util::TrimString(s.substr(0, s.find(';')))};
        if (t.empty() || t.size() > 15) return std::nullopt;
        uint64_t v{0};
        for (char c : t) {
            const int d{HexDigit(c)};
            if (d < 0) return std::nullopt;
            v = v * 16 + static_cast<uint64_t>(d);
        }
        return v;
    }

    std::string m_in;
    std::unique_ptr<Request> m_request;
    std::unique_ptr<Request> m_current;
    bool m_started{false};
    bool m_keep_alive{true};
    bool m_chunked{false};
    uint64_t m_length{0};
    bool m_open{true};
    bool m_draining{false};
};

/** Until the first bytes tell which protocol it is. */
class Sniff : public Connection::Impl
{
public:
    using Impl::Impl;
    bool Busy() const override { return false; }
    bool Receiving() const override { return !m_in.empty(); }
    bool Receive(std::span<const uint8_t> data) override
    {
        m_in.append(reinterpret_cast<const char*>(data.data()), data.size());
        return true;
    }
    bool Poll() override { return true; }
    bool WantsWrite() const override { return false; }
    void CloseAll() override {}
    std::string m_in;
};

} // namespace

Connection::Connection(Server& server, CService peer) : m_impl{std::make_unique<Sniff>(server, std::move(peer))} {}
Connection::~Connection() = default;

bool Connection::Receive(std::span<const uint8_t> data)
{
    if (auto* sniff{dynamic_cast<Sniff*>(m_impl.get())}) {
        sniff->Receive(data);
        const std::string& in{sniff->m_in};
        const size_t n{std::min(in.size(), H2_PREFACE.size())};
        if (in.compare(0, n, H2_PREFACE.substr(0, n)) == 0 && n < H2_PREFACE.size()) return true; // could still be HTTP/2
        const std::string buffered{in};
        Server& server{m_impl->m_server};
        CService peer{m_impl->m_peer};
        if (buffered.starts_with(H2_PREFACE)) {
            m_impl = std::make_unique<Http2>(server, peer);
        } else {
            m_impl = std::make_unique<Http1>(server, peer);
        }
        return m_impl->Receive(MakeUCharSpan(buffered));
    }
    return m_impl->Receive(data);
}

bool Connection::Poll() { return m_impl->Poll(); }

std::string Connection::TakeOutput()
{
    std::string out;
    out.swap(m_impl->m_out);
    return out;
}

bool Connection::WantsWrite() const { return m_impl->WantsWrite(); }
bool Connection::Busy() const { return m_impl->Busy(); }
bool Connection::Receiving() const { return m_impl->Receiving(); }
void Connection::Goodbye() { m_impl->Goodbye(); }
std::optional<NodeClock::time_point> Connection::OldestRequest() const { return m_impl->OldestRequest(); }
size_t Connection::Undelivered() const { return m_impl->Undelivered(); }
size_t Connection::Buffered() const { return m_impl->m_out.size() + m_impl->Undelivered(); }
void Connection::Hold(bool hold) { m_impl->m_hold = hold; }
uint64_t Connection::ControlBytes() const { return m_impl->m_serialized - m_impl->m_payload; }
void Connection::Drain() { m_impl->Drain(); }
Connection::Counts Connection::GetCounts() const { return {m_impl->m_begun, m_impl->m_completed, m_impl->m_abandoned}; }
void Connection::CloseAll() { m_impl->CloseAll(); }

// ---------------------------------------------------------------------------
// The server

Server::Server(const pb::Pool& pool, Options options) : m_pool{pool}, m_options{options} {}

Server::~Server() { Stop(); }

bool Server::Register(const std::string& service, const std::string& method, Handler handler)
{
    const pb::ServiceDesc* s{m_pool.FindService(service)};
    if (!s) return false;
    for (const pb::MethodDesc& m : s->methods) {
        if (m.name == method) {
            m_methods["/" + service + "/" + method] = MethodEntry{&m, std::move(handler)};
            return true;
        }
    }
    return false;
}

bool Server::Reserve(size_t bytes)
{
    size_t now{m_buffered.load()};
    do {
        if (now + bytes > m_options.max_total_buffer) return false;
    } while (!m_buffered.compare_exchange_weak(now, now + bytes));
    return true;
}

void Server::Release(size_t bytes) { m_buffered -= std::min(bytes, m_buffered.load()); }

std::vector<std::string> Server::Services() const
{
    std::set<std::string> names;
    for (const auto& [path, entry] : m_methods) names.insert(path.substr(1, path.rfind('/') - 1));
    return {names.begin(), names.end()};
}

const Server::MethodEntry* Server::Find(std::string_view path) const
{
    const auto it{m_methods.find(path)};
    return it == m_methods.end() ? nullptr : &it->second;
}

void Server::Run(std::function<void()> task)
{
    if (m_workers) {
        (void)m_workers->Submit(std::move(task));
    } else {
        task();
    }
}

std::vector<CService> Server::Bound() const { return m_bound; }

std::string Server::AddressKey(const CNetAddr& addr, int ipv6_prefix)
{
    // An IPv6 client usually has a whole prefix (a /64 at least) to rotate its addresses in: bounded as one.
    if (addr.IsIPv6() && ipv6_prefix > 0 && ipv6_prefix < 128) return CSubNet{addr, static_cast<uint8_t>(ipv6_prefix)}.ToString();
    return addr.ToStringAddr();
}

bool Server::Start(const std::vector<CService>& binds, const std::vector<CSubNet>& allow, std::string& error)
{
    m_allow = allow;
    std::vector<std::string> errors;
    for (const CService& to : binds) {
        sockaddr_storage storage;
        socklen_t len{sizeof(storage)};
        if (!to.GetSockAddr(reinterpret_cast<sockaddr*>(&storage), &len)) {
            errors.push_back(strprintf("cannot bind to %s", to.ToStringAddrPort()));
            continue;
        }
        std::unique_ptr<Sock> sock{CreateSock(to.GetSAFamily(), SOCK_STREAM, IPPROTO_TCP)};
        if (!sock) {
            errors.push_back(strprintf("cannot create a socket for %s: %s", to.ToStringAddrPort(), NetworkErrorString(WSAGetLastError())));
            continue;
        }
#ifdef WIN32
        sock->SetSockOpt(SOL_SOCKET, SO_EXCLUSIVEADDRUSE, &SOCKET_OPTION_TRUE, sizeof(SOCKET_OPTION_TRUE));
#else
        sock->SetSockOpt(SOL_SOCKET, SO_REUSEADDR, &SOCKET_OPTION_TRUE, sizeof(SOCKET_OPTION_TRUE));
#endif
#ifdef IPV6_V6ONLY
        if (to.IsIPv6()) sock->SetSockOpt(IPPROTO_IPV6, IPV6_V6ONLY, &SOCKET_OPTION_TRUE, sizeof(SOCKET_OPTION_TRUE));
#endif
        if (sock->Bind(reinterpret_cast<sockaddr*>(&storage), len) == SOCKET_ERROR || sock->Listen(SOMAXCONN) == SOCKET_ERROR) {
            errors.push_back(strprintf("cannot listen on %s: %s", to.ToStringAddrPort(), NetworkErrorString(WSAGetLastError())));
            continue;
        }
        // The port, if the system picked it (port 0).
        sockaddr_storage bound;
        socklen_t bound_len{sizeof(bound)};
        CService service{to};
        if (sock->GetSockName(reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) service.SetSockAddr(reinterpret_cast<sockaddr*>(&bound), bound_len);
        m_bound.push_back(service);
        m_listen.push_back(std::move(sock));
    }
    if (m_listen.empty()) {
        error = util::Join(errors, "; ");
        return false;
    }
    for (const std::string& e : errors) LogWarning("%s: %s", m_name, e);
    // nghttp2 fixed MadeYouReset (CVE-2025-8671) in 1.66; the limit on streams reset here stands either way.
    if (const nghttp2_info* info{nghttp2_version(0)}; info && info->version_num < 0x014200) {
        LogWarning("%s: nghttp2 %s is older than 1.66 (without its MadeYouReset fix, CVE-2025-8671); the node bounds streams reset itself, an update of libnghttp2 is advised", m_name, info->version_str);
    }
    m_workers = std::make_unique<ThreadPool>("grpc");
    m_workers->Start(std::max(1, m_options.threads));
    m_accept_thread = std::thread(&util::TraceThread, "grpcaccept", [this] { AcceptLoop(); });
    return true;
}

void Server::AcceptLoop()
{
    while (!m_interrupt) {
        Sock::EventsPerSock events;
        for (const auto& sock : m_listen) events.emplace(sock, Sock::Events{Sock::RecvEvent});
        if (m_listen.empty() || !m_listen[0]->WaitMany(std::chrono::milliseconds{200}, events)) continue;
        for (auto& [sock, ev] : events) {
            if (!(ev.occurred & Sock::RecvEvent)) continue;
            sockaddr_storage storage;
            socklen_t len{sizeof(storage)};
            std::unique_ptr<Sock> client{sock->Accept(reinterpret_cast<sockaddr*>(&storage), &len)};
            if (!client) continue;
            CService peer;
            peer.SetSockAddr(reinterpret_cast<sockaddr*>(&storage), len);
            const bool allowed{peer.IsLocal() || std::any_of(m_allow.begin(), m_allow.end(), [&](const CSubNet& s) { return s.Match(peer); })};
            if (!allowed) {
                LogWarning("%s: connection from %s refused (not in the addresses allowed)", m_name, peer.ToStringAddrPort());
                continue;
            }
            if (m_connections.load() >= m_options.max_connections) {
                LogWarning("%s: connection from %s refused: %d connections already", m_name, peer.ToStringAddrPort(), m_connections.load());
                continue;
            }
            if (!client->SetNonBlocking()) continue;
            // Connections by address: bounded, but for the proxies in front (which carry everyone's).
            const bool proxy{std::any_of(m_options.trusted_proxies.begin(), m_options.trusted_proxies.end(), [&](const CSubNet& s) { return s.Match(peer); })};
            const std::string ip{proxy ? std::string{} : AddressKey(peer, m_options.ipv6_prefix)};
            LOCK(m_mutex);
            if (!ip.empty() && m_options.max_connections_per_ip > 0 && m_per_ip[ip] >= m_options.max_connections_per_ip) {
                LogDebug(BCLog::HTTP, "%s: connection from %s refused: %d connections from it already\n", m_name, peer.ToStringAddrPort(), m_per_ip[ip]);
                continue;
            }
            if (!ip.empty()) ++m_per_ip[ip];
            ++m_connections;
            std::shared_ptr<Sock> shared{std::move(client)};
            auto done{std::make_shared<std::atomic<bool>>(false)};
            // The threads of connections that ended are joined here, so that they do not pile up.
            for (auto it{m_connection_threads.begin()}; it != m_connection_threads.end();) {
                if (it->second->load()) {
                    it->first.join();
                    it = m_connection_threads.erase(it);
                } else {
                    ++it;
                }
            }
            m_connection_threads.emplace_back(std::thread(&util::TraceThread, "grpcconn", [this, shared, peer, done, ip] {
                ServeConnection(shared, peer);
                if (!ip.empty()) {
                    LOCK(m_mutex);
                    if (--m_per_ip[ip] <= 0) m_per_ip.erase(ip);
                }
                --m_connections;
                *done = true;
            }), done);
        }
    }
}

void Server::ServeConnection(std::shared_ptr<Sock> sock, CService peer)
{
    LogDebug(BCLog::HTTP, "%s: connection from %s\n", m_name, peer.ToStringAddrPort());
    Connection connection{*this, peer};
    std::string pending;
    bool open{true};
    uint8_t buf[16384];
    auto last_activity{NodeClock::now()};
    // Slow reads: from when replies wait for the client, what it took of them, timed over write_timeout_seconds.
    auto window_start{last_activity};
    size_t window_owed{0}, window_sent{0};
    uint64_t window_control{0};
    Connection::Counts counts;
    std::optional<NodeClock::time_point> receiving_since;
    // Close, over HTTP/2 with a GOAWAY first (what it says sent if the client takes it at once).
    const auto goodbye{[&](const char* why) {
        LogDebug(BCLog::HTTP, "%s: %s %s: closed\n", m_name, peer.ToStringAddrPort(), why);
        connection.Goodbye();
        pending += connection.TakeOutput();
        for (int i{0}; i < 50 && !pending.empty(); ++i) {
            Sock::Event ev{0};
            if (!sock->Wait(std::chrono::milliseconds{20}, Sock::SendEvent, &ev)) break;
            const ssize_t n{sock->Send(pending.data(), pending.size(), MSG_NOSIGNAL)};
            if (n <= 0) break;
            pending.erase(0, static_cast<size_t>(n));
        }
    }};
    while (open && !m_interrupt) {
        const auto now{NodeClock::now()};
        // A client that does not read what it asked for (slow reads): replies pile up in the socket, in the calls or
        // behind HTTP/2 flow control. Every reply byte the connection holds counts against one bound
        // (Options::max_reply_buffer); and over each window of write_timeout_seconds with replies waiting, the client
        // must take at least min_read_bytes_per_second of them (or all that waited when the window began), else it is
        // closed: a byte now and then does not keep it open.
        const size_t owed{pending.size() + connection.Buffered()};
        if (m_options.max_reply_buffer > 0 && owed > m_options.max_reply_buffer) {
            LogDebug(BCLog::HTTP, "%s: %s leaves %u bytes of replies unread: closed\n", m_name, peer.ToStringAddrPort(), owed);
            break;
        }
        if (owed == 0) {
            window_owed = 0;
        } else if (window_owed == 0) {
            window_start = now;
            window_owed = owed;
            window_sent = 0;
            window_control = connection.ControlBytes();
        } else if (m_options.write_timeout_seconds > 0 && now - window_start > std::chrono::seconds{m_options.write_timeout_seconds}) {
            const size_t rate{m_options.min_read_bytes_per_second * static_cast<size_t>(m_options.write_timeout_seconds)};
            // Progress on the replies: what the socket took, less the control frames made meanwhile (a client asking for
            // PING ACKs and reading only those makes none).
            const uint64_t control{connection.ControlBytes() - window_control};
            const size_t progress{window_sent > control ? static_cast<size_t>(window_sent - control) : 0};
            if (progress < std::max<size_t>(1, std::min(window_owed, rate))) {
                LogDebug(BCLog::HTTP, "%s: %s reads too little of its replies (%u bytes in %d s): closed\n", m_name, peer.ToStringAddrPort(), progress, m_options.write_timeout_seconds);
                break;
            }
            window_start = now;
            window_owed = owed;
            window_sent = 0;
            window_control = connection.ControlBytes();
        }
        // A request that takes too long to come in, however slowly it keeps coming, ends the connection (HTTP/2: each
        // stream from its own first bytes). Calls running on a worker are never cut for it.
        if (connection.Receiving()) {
            if (!receiving_since) receiving_since = now;
            const NodeClock::time_point since{connection.OldestRequest().value_or(*receiving_since)};
            if (m_options.request_timeout_seconds > 0 && now - since > std::chrono::seconds{m_options.request_timeout_seconds}) {
                LogDebug(BCLog::HTTP, "%s: %s too slow to send its request: closed\n", m_name, peer.ToStringAddrPort());
                break;
            }
        } else {
            receiving_since.reset();
        }
        // Idle: no call running, and no request begun or all in, for a while (bytes alone, PINGs or SETTINGS, are not
        // activity). Counted from the end of the last call.
        const Connection::Counts now_counts{connection.GetCounts()};
        if (connection.Busy() || now_counts.begun != counts.begun || now_counts.completed != counts.completed) last_activity = now;
        counts = now_counts;
        if (m_options.idle_timeout_seconds > 0 && pending.empty() && !connection.Busy() && now - last_activity > std::chrono::seconds{m_options.idle_timeout_seconds}) {
            goodbye("idle");
            break;
        }
        // Streams reset before their call was over, by the client or because of it (Rapid Reset, MadeYouReset; each
        // would restart the idle time): closed. Too many requests on one connection: no new one (GOAWAY), closed once
        // the running calls are over (the client opens another).
        if (m_options.max_abandoned_requests > 0 && counts.abandoned > m_options.max_abandoned_requests) {
            goodbye("resets too many streams");
            break;
        }
        if (m_options.max_requests_per_connection > 0 && counts.begun > m_options.max_requests_per_connection) {
            connection.Drain();
            if (!connection.Busy() && pending.empty()) {
                goodbye("past its requests");
                break;
            }
        }
        Sock::Event occurred{0};
        const Sock::Event wanted{static_cast<Sock::Event>(Sock::RecvEvent | (pending.empty() ? 0 : Sock::SendEvent))};
        // Woken by the socket, or every 20 ms to send what the calls wrote meanwhile.
        if (!sock->Wait(std::chrono::milliseconds{20}, wanted, &occurred)) break;
        if (occurred & Sock::RecvEvent) {
            const ssize_t n{sock->Recv(buf, sizeof(buf), 0)};
            if (n > 0) {
                connection.Hold(pending.size() >= REPLY_HIGH_WATER);
                open = connection.Receive({buf, static_cast<size_t>(n)});
            } else if (n == 0) {
                break;
            } else {
                const int err{WSAGetLastError()};
                if (err != WSAEWOULDBLOCK && err != WSAEINTR && err != WSAEINPROGRESS) break;
            }
        }
        // Backpressure: while the socket has not taken what waits, nothing more is taken from the calls.
        connection.Hold(pending.size() >= REPLY_HIGH_WATER);
        if (open) open = connection.Poll();
        if (pending.size() < REPLY_HIGH_WATER) pending += connection.TakeOutput();
        while (!pending.empty()) {
            const ssize_t n{sock->Send(pending.data(), pending.size(), MSG_NOSIGNAL)};
            if (n > 0) {
                pending.erase(0, static_cast<size_t>(n));
                window_sent += static_cast<size_t>(n);
            } else {
                const int err{WSAGetLastError()};
                if (n < 0 && (err == WSAEWOULDBLOCK || err == WSAEINTR || err == WSAEINPROGRESS)) break;
                open = false;
                pending.clear();
                break;
            }
        }
        if (!open && !pending.empty()) {
            // Send what is left of the last reply before closing.
            for (int i{0}; i < 100 && !pending.empty(); ++i) {
                Sock::Event ev{0};
                if (!sock->Wait(std::chrono::milliseconds{20}, Sock::SendEvent, &ev)) break;
                const ssize_t n{sock->Send(pending.data(), pending.size(), MSG_NOSIGNAL)};
                if (n <= 0) break;
                pending.erase(0, static_cast<size_t>(n));
            }
        }
    }
    connection.CloseAll();
    LogDebug(BCLog::HTTP, "%s: connection from %s closed\n", m_name, peer.ToStringAddrPort());
}

void Server::Stop()
{
    m_interrupt();
    if (m_accept_thread.joinable()) m_accept_thread.join();
    std::vector<std::pair<std::thread, std::shared_ptr<std::atomic<bool>>>> threads;
    {
        LOCK(m_mutex);
        threads.swap(m_connection_threads);
    }
    for (auto& [t, done] : threads) {
        if (t.joinable()) t.join();
    }
    if (m_workers) {
        m_workers->Stop();
        m_workers.reset();
    }
    m_listen.clear();
}

} // namespace grpcsrv
