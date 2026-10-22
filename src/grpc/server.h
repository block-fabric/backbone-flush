// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_GRPC_SERVER_H
#define BITCOIN_GRPC_SERVER_H

#include <grpc/protobuf.h>
#include <netaddress.h>
#include <sync.h>
#include <util/threadinterrupt.h>
#include <util/time.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

class Sock;
class ThreadPool;

/**
 * A server of RPCs described by protocol buffers (pb::Pool), as the bip300301_enforcer serves them
 * (connectrpc): gRPC over HTTP/2 without TLS (h2c, prior knowledge), which its clients use, and on
 * the same port gRPC-Web and the Connect protocol, over HTTP/2 or HTTP/1.1. HTTP/2 is nghttp2's;
 * there is no TLS, no authentication and no compression (identity only), as the enforcer.
 */
namespace grpcsrv {

/** gRPC status codes. */
enum class Code : int {
    OK = 0, CANCELLED = 1, UNKNOWN = 2, INVALID_ARGUMENT = 3, DEADLINE_EXCEEDED = 4, NOT_FOUND = 5,
    ALREADY_EXISTS = 6, PERMISSION_DENIED = 7, RESOURCE_EXHAUSTED = 8, FAILED_PRECONDITION = 9, ABORTED = 10,
    OUT_OF_RANGE = 11, UNIMPLEMENTED = 12, INTERNAL = 13, UNAVAILABLE = 14, DATA_LOSS = 15, UNAUTHENTICATED = 16,
};

struct Status {
    Code code{Code::OK};
    std::string message;
    //! Headers to send with the reply (Retry-After with RESOURCE_EXHAUSTED, say).
    std::vector<std::pair<std::string, std::string>> headers{};
    bool ok() const { return code == Code::OK; }
};

/** The Connect name of a code ("invalid_argument") and the HTTP status the Connect protocol gives it. */
std::string ConnectCodeName(Code code);
int ConnectHttpStatus(Code code);
/** grpc-message is percent-encoded. */
std::string PercentEncode(std::string_view message);

/** Where a call puts its replies. Thread safe; handlers may keep it and write from any thread. */
class Writer
{
public:
    virtual ~Writer() = default;
    /** Queue a reply. @return false once the call is over (finished, or the client went away). */
    virtual bool Write(const pb::Message& reply) = 0;
    /** End the call (a unary call: after its one reply, or with an error instead). Later calls do nothing. */
    virtual void Finish(const Status& status) = 0;
    /** Whether the call is over. */
    virtual bool Closed() const = 0;
    /** Bytes written and not sent yet (a client that does not read lets them pile up). */
    virtual size_t Pending() const = 0;
    /** Called once when the call ends (the client cancelled, the connection closed, or Finish). */
    virtual void OnClose(std::function<void()> callback) = 0;
    /** A header to send with a successful reply (before the first Write). Names in lower case. */
    virtual void SetHeader(const std::string& name, const std::string& value) {}
};

/**
 * A method: the handler gets the request and the writer, and finishes the call when it is done
 * (now or later: a server stream stays open for as long as it does not finish it). It runs on a
 * worker thread. A method with client streaming (server reflection) gets each request as it comes
 * instead, on the connection's thread, and the call ends with OK when the client is done sending.
 */
using Handler = std::function<void(const pb::Message& request, const std::shared_ptr<Writer>& writer)>;

/** What a guard knows of a call. */
struct CallInfo {
    //! The method's path (/package.Service/Method), also for a REST request (the method it maps onto).
    std::string path;
    //! The request's headers, names in lower case.
    const std::map<std::string, std::string>& headers;
    //! The client's key: the peer's, or what a trusted proxy in front says (Options::trusted_proxies), IPv6 by its prefix (Server::AddressKey).
    std::string client;
    //! A REST request (Options::rest).
    bool rest{false};
};

/** A check of each call before it runs; a status refuses it. */
using Guard = std::function<std::optional<Status>(const CallInfo& call)>;

/**
 * The REST face (Options::rest_prefix): a request whose path starts with the prefix is mapped onto a method and
 * its request message. The reply is that method's, printed as JSON with the fields' own names and every field
 * present (pb::Message::JsonOptions), or as raw bytes (`raw`: content type and body from the reply).
 */
struct RestMatch {
    std::string method_path;
    std::unique_ptr<pb::Message> request;
    std::function<std::optional<std::pair<std::string, std::string>>(const pb::Message& reply)> raw;
};
//! (HTTP method, path after the prefix, query, body) -> the call, or a status (NOT_FOUND for an unknown path).
using RestRouter = std::function<std::variant<RestMatch, Status>(const std::string& method, const std::string& path, const std::string& query, const std::string& body)>;

//! Reply bytes waiting for the socket above which a connection takes nothing more from its calls (backpressure).
inline constexpr size_t REPLY_HIGH_WATER{1 << 20};

struct Options {
    //! Largest request message, in bytes.
    size_t max_request_bytes{16 << 20};
    //! Request bytes a connection may have buffered at once (all its streams), and all connections together.
    size_t max_connection_buffer{64 << 20};
    size_t max_total_buffer{256 << 20};
    //! Origin header values let in. A request with another Origin (from a web page) is refused (CSRF).
    std::vector<std::string> allowed_origins;
    //! Host (or :authority) names let in besides IP literals and localhost; others are refused (DNS rebinding).
    std::vector<std::string> allowed_hosts;
    //! Largest number of connections at a time.
    int max_connections{128};
    //! Worker threads for the handlers.
    int threads{4};
    //! HTTP/2 streams a connection may have open at once.
    uint32_t max_streams{256};

    // The public face (flushd -publicapi): requests from web pages are expected.
    //! CORS: a request from an origin in allowed_origins gets Access-Control-Allow-Origin (never credentials), and
    //! preflights (OPTIONS) are answered for them; any other Origin is still refused.
    bool cors{false};
    //! Largest reply body; a bigger one ends the call with RESOURCE_EXHAUSTED (0: no bound).
    size_t max_response_bytes{0};
    //! Close a connection that has no call running and began or completed no request for this long (0: never; bytes
    //! alone, PINGs, are not activity); over HTTP/2 with a GOAWAY first.
    int idle_timeout_seconds{0};
    //! Close a connection whose request (its headers and body) is not all in this long after its first byte, however
    //! slowly it keeps coming (slow clients holding connections); 0: never. Calls running on a worker are not counted.
    int request_timeout_seconds{0};
    //! Connections a client address may hold at once (0: no bound); the trusted proxies' own addresses are not bound.
    int max_connections_per_ip{0};
    //! IPv6 clients are counted by this prefix (a client usually has a /64 to rotate addresses in); IPv4 by address.
    int ipv6_prefix{64};
    //! Close a connection whose replies the peer took no byte of for this long (slow reads: in the socket, the calls or
    //! behind HTTP/2 flow control; a stream whose window stays closed so long is reset); 0: never.
    int write_timeout_seconds{0};
    //! With write_timeout_seconds: over each such window with replies waiting, the client must take at least this many
    //! bytes a second of them (or all that was waiting when the window began, if less); a byte now and then does not
    //! keep it open. 0: one byte a window is enough.
    size_t min_read_bytes_per_second{0};
    //! Reply bytes one connection may hold at once (0: no bound): those not sent to the socket yet, those of the
    //! protocol layer and those its calls wrote and did not hand over; past it the connection is closed. Above
    //! REPLY_HIGH_WATER bytes waiting for the socket, nothing more is taken from the calls (backpressure: their own
    //! bound, max_response_bytes, then ends them).
    size_t max_reply_buffer{0};
    //! Past this many requests (0: no bound), a connection takes no new one (HTTP/2: GOAWAY) and is closed once its
    //! calls are over; past this many streams reset before their call was over (by the client, or by the server
    //! because of it: Rapid Reset, MadeYouReset), it is closed at once.
    uint64_t max_requests_per_connection{0};
    uint64_t max_abandoned_requests{0};
    //! Messages a client-streaming call (server reflection) may send; each is checked by the guard again.
    size_t max_stream_messages{1024};
    //! The REST face: the prefix of its paths ("/api/v1/"), and how they map onto methods (empty: none).
    std::string rest_prefix;
    RestRouter rest;
    //! A proxy in front: the header it sets with the client's address (lower case: "x-real-ip", or "x-forwarded-for",
    //! whose right-most entry, the one that proxy appended, is taken), believed only from these addresses. Nothing set:
    //! the client is the socket's peer, and forwarding headers are ignored.
    std::string proxy_header;
    std::vector<CSubNet> trusted_proxies;
    //! Headers every reply carries.
    std::vector<std::pair<std::string, std::string>> response_headers;
};

class Connection;

class Server
{
public:
    explicit Server(const pb::Pool& pool, Options options = {});
    ~Server();

    /** Register the method `/service/method` of the pool. @return false if the pool has no such method. */
    bool Register(const std::string& service, const std::string& method, Handler handler);
    void SetGuard(Guard guard) { m_guard = std::move(guard); }
    const Guard& GetGuard() const { return m_guard; }
    /** Account for request bytes buffered (all connections); false if over the budget. */
    bool Reserve(size_t bytes);
    void Release(size_t bytes);
    /** The full names of the services with a method registered. */
    std::vector<std::string> Services() const;

    /** Listen on these addresses; a client must be in `allow` (or on the loopback interface). */
    bool Start(const std::vector<CService>& binds, const std::vector<CSubNet>& allow, std::string& error);
    /** The name logs and threads use ("gRPC" for the enforcer interface, "public API"). */
    void SetName(std::string name) { m_name = std::move(name); }
    /** Stop accepting connections, end every call (with UNAVAILABLE) and close the connections. */
    void Stop();
    /** The ports bound, for -grpcport=0 in tests. */
    std::vector<CService> Bound() const;

    struct MethodEntry {
        const pb::MethodDesc* desc;
        Handler handler;
    };
    const MethodEntry* Find(std::string_view path) const;
    const pb::Pool& Pool() const { return m_pool; }
    const Options& GetOptions() const { return m_options; }
    const std::string& Name() const { return m_name; }
    /** The key a client is known by, for its connections (Options::max_connections_per_ip) and its calls' limits (CallInfo::client):
     *  an IPv6 address by its prefix (Options::ipv6_prefix), an IPv4 one whole. */
    static std::string AddressKey(const CNetAddr& addr, int ipv6_prefix);
    void Run(std::function<void()> task);

private:
    void AcceptLoop();
    void ServeConnection(std::shared_ptr<Sock> sock, CService peer);

    const pb::Pool& m_pool;
    const Options m_options;
    std::map<std::string, MethodEntry, std::less<>> m_methods;
    std::vector<std::shared_ptr<Sock>> m_listen;
    std::vector<CService> m_bound;
    std::vector<CSubNet> m_allow;
    CThreadInterrupt m_interrupt;
    std::thread m_accept_thread;
    std::unique_ptr<ThreadPool> m_workers;
    Mutex m_mutex;
    std::vector<std::pair<std::thread, std::shared_ptr<std::atomic<bool>>>> m_connection_threads GUARDED_BY(m_mutex);
    std::atomic<int> m_connections{0};
    std::atomic<size_t> m_buffered{0};
    Guard m_guard;
    //! Connections by client address (Options::max_connections_per_ip).
    std::map<std::string, int> m_per_ip GUARDED_BY(m_mutex);
    std::string m_name{"gRPC"};
};

/**
 * One client connection, without its socket (so that tests can drive it): bytes in (Receive),
 * bytes out (TakeOutput), and Poll() to send what calls have written meanwhile. HTTP/2 if the
 * client starts with its connection preface, HTTP/1.1 otherwise. Not thread safe: one thread runs it.
 */
class Connection
{
public:
    explicit Connection(Server& server, CService peer = {});
    ~Connection();
    /** @return false if the connection is to be closed (a protocol error, or HTTP/1.1 without keep-alive). */
    bool Receive(std::span<const uint8_t> data);
    bool Poll();
    std::string TakeOutput();
    /** Whether there is something to send, or to wait for. */
    bool WantsWrite() const;
    /** Whether a request is in progress (being read, or its call not over). */
    bool Busy() const;
    /** Whether a request is being read: begun and not all in (its headers, or its body). */
    bool Receiving() const;
    /** Before closing an idle connection: HTTP/2 says GOAWAY (no error), to be sent from TakeOutput. */
    void Goodbye();
    /** HTTP/2: when the oldest request still being read began (each stream timed on its own); else nothing. */
    std::optional<NodeClock::time_point> OldestRequest() const;
    /** Reply bytes the calls wrote and the connection has not taken yet (HTTP/2: held back by flow control). */
    size_t Undelivered() const;
    /** Every reply byte the connection holds: Undelivered, and what the protocol layer made and TakeOutput did not take. */
    size_t Buffered() const;
    /** Take nothing more from the calls while `hold` (backpressure: the socket does not take what is waiting). */
    void Hold(bool hold);
    /** HTTP/2: bytes of output that are not the replies' DATA payload (control frames, frame headers), so far. */
    uint64_t ControlBytes() const;
    /** Take no new request (HTTP/2: GOAWAY); the running calls go on. */
    void Drain();
    /** Requests begun, all in, and streams reset before their call was over. */
    struct Counts {
        uint64_t begun{0}, completed{0}, abandoned{0};
    };
    Counts GetCounts() const;
    /** End every call of the connection (the client went away, or the server stops). */
    void CloseAll();

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace grpcsrv

#endif // BITCOIN_GRPC_SERVER_H
