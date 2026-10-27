// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The download of a link's logo on Linux (webui/fetch.h): libsoup 3, the HTTP library WebKitGTK links already. A
// session of its own for each download, made and used on the calling thread (the links' thread, never the UI's), with
// its synchronous API: no redirect, no content decoder (the bytes hashed are the bytes served), the system's TLS
// certificates, a timeout of each step and a deadline for the whole (a cancellable cancelled by a watchdog), no proxy,
// and the connection's own remote address checked as it connects, before TLS or HTTP (IsPublicAddress, at
// G_SOCKET_CLIENT_CONNECTED: the address the socket reached, not a lookup of its own, so no DNS rebinding race).

#include <webui/fetch.h>

#include <tinyformat.h>

#include <libsoup/soup.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace webui {

namespace {

/** Why a GError failed a download, as the user reads it. */
std::string Why(GError* error, bool timed_out)
{
    if (timed_out) return strprintf("no answer within %d seconds", static_cast<int>(FETCH_TIMEOUT.count()));
    std::string why{error && error->message ? error->message : "the download failed"};
    if (why.size() > 160) why = why.substr(0, 160) + "…";
    return why;
}

/** The check of the connection's own address (IsPublicAddress), made as it connects (SoupMessage::network-event). */
struct AddressGuard {
    GCancellable* cancellable{nullptr};
    //! Whether a connection's address was checked, and why it was refused (empty: not refused).
    bool checked{false};
    std::string refused;
};

void OnNetworkEvent(SoupMessage*, GSocketClientEvent event, GIOStream* connection, gpointer data)
{
    auto& guard{*static_cast<AddressGuard*>(data)};
    // CONNECTED: the TCP connection is up, before TLS or HTTP: nothing of the request has been sent yet.
    if (event != G_SOCKET_CLIENT_CONNECTED || !G_IS_SOCKET_CONNECTION(connection)) return;
    GSocketConnection* socket{G_SOCKET_CONNECTION(connection)};
    std::string why{"its address could not be read"};
    bool ok{false};
    if (GSocketAddress* remote{g_socket_connection_get_remote_address(socket, nullptr)}) {
        if (G_IS_INET_SOCKET_ADDRESS(remote)) {
            gchar* text{g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(remote)))};
            ok = IsPublicAddress(text ? text : "", why);
            g_free(text);
        }
        g_object_unref(remote);
    }
    if (ok) {
        guard.checked = true;
        return;
    }
    if (guard.refused.empty()) guard.refused = why;
    // The download cancelled here, in the event: its next step (the TLS handshake) does not start, so nothing is sent
    // to that address (tested: a listener there receives 0 bytes). The socket is not closed by hand: libsoup owns it.
    g_cancellable_cancel(guard.cancellable);
}

} // namespace

std::optional<std::vector<unsigned char>> FetchUrl(const std::string& url, size_t max_bytes, std::string& error)
{
    if (!url.starts_with("https://")) {
        error = "https addresses only";
        return std::nullopt;
    }
    SoupMessage* msg{soup_message_new("GET", url.c_str())};
    if (!msg) {
        error = "not an address the app can read";
        return std::nullopt;
    }
    GUri* uri{soup_message_get_uri(msg)};
    if (!uri || !g_uri_get_scheme(uri) || std::strcmp(g_uri_get_scheme(uri), "https") != 0) {
        g_object_unref(msg);
        error = "https addresses only";
        return std::nullopt;
    }
    // A redirect is a failure (its 3xx is not 200): the address is the link's, never one a server sends on to.
    soup_message_add_flags(msg, SOUP_MESSAGE_NO_REDIRECT);
    soup_message_headers_replace(soup_message_get_request_headers(msg), "Accept", "image/png, image/jpeg");

    const guint seconds{static_cast<guint>(FETCH_TIMEOUT.count())};
    // No proxy (not the system's either): the address checked is the destination's own, not a proxy's.
    SoupSession* session{soup_session_new_with_options("timeout", seconds, "idle-timeout", seconds, "user-agent", "Flush-app", "proxy-resolver", nullptr, nullptr)};
    // The bytes as served (their SHA-256 is checked): no Accept-Encoding, nothing decoded.
    soup_session_remove_feature_by_type(session, SOUP_TYPE_CONTENT_DECODER);

    // The deadline of the whole download: cancelled at FETCH_TIMEOUT, whatever step it is at (a server that sends a
    // byte now and then keeps each step under its timeout).
    GCancellable* cancellable{g_cancellable_new()};
    // The address of the connection made, checked as it connects (a DNS name rebound to the user's own network).
    AddressGuard guard;
    guard.cancellable = cancellable;
    g_signal_connect(msg, "network-event", G_CALLBACK(OnNetworkEvent), &guard);
    std::mutex mutex;
    std::condition_variable cv;
    bool done{false};
    bool timed_out{false};
    std::thread watchdog{[&] {
        std::unique_lock lock{mutex};
        if (!cv.wait_for(lock, FETCH_TIMEOUT, [&] { return done; })) {
            timed_out = true;
            g_cancellable_cancel(cancellable);
        }
    }};
    const auto finish{[&] {
        {
            std::lock_guard lock{mutex};
            done = true;
        }
        cv.notify_all();
        watchdog.join();
    }};

    std::optional<std::vector<unsigned char>> result;
    GError* gerror{nullptr};
    GInputStream* in{soup_session_send(session, msg, cancellable, &gerror)};
    if (!in) {
        finish();
        error = guard.refused.empty() ? Why(gerror, timed_out) : guard.refused;
    } else if (!guard.refused.empty() || !guard.checked) {
        finish();
        error = guard.refused.empty() ? std::string{"the address of its connection could not be checked"} : guard.refused;
    } else {
        const guint status{soup_message_get_status(msg)};
        SoupMessageHeaders* headers{soup_message_get_response_headers(msg)};
        if (status != SOUP_STATUS_OK) {
            finish();
            error = SOUP_STATUS_IS_REDIRECTION(status) ? strprintf("HTTP %u, a redirect (not followed)", status) : strprintf("HTTP %u (200 only)", status);
        } else if (soup_message_headers_get_encoding(headers) == SOUP_ENCODING_CONTENT_LENGTH && soup_message_headers_get_content_length(headers) > static_cast<goffset>(max_bytes)) {
            finish();
            error = strprintf("larger than %u bytes", max_bytes);
        } else {
            // At most max_bytes + 1 bytes read: one more than allowed says it is too large.
            std::vector<unsigned char> bytes;
            bytes.reserve(max_bytes + 1);
            unsigned char buffer[8192];
            bool failed{false};
            while (bytes.size() <= max_bytes) {
                const size_t want{std::min(sizeof(buffer), max_bytes + 1 - bytes.size())};
                const gssize got{g_input_stream_read(in, buffer, want, cancellable, &gerror)};
                if (got < 0) {
                    failed = true;
                    break;
                }
                if (got == 0) break;
                bytes.insert(bytes.end(), buffer, buffer + got);
            }
            finish();
            if (!guard.refused.empty()) {
                error = guard.refused;
            } else if (failed) {
                error = Why(gerror, timed_out);
            } else if (bytes.size() > max_bytes) {
                error = strprintf("larger than %u bytes", max_bytes);
            } else {
                result = std::move(bytes);
            }
        }
        g_input_stream_close(in, nullptr, nullptr);
        g_object_unref(in);
    }
    if (gerror) g_error_free(gerror);
    g_signal_handlers_disconnect_by_data(msg, &guard);
    g_object_unref(cancellable);
    g_object_unref(session);
    g_object_unref(msg);
    return result;
}

} // namespace webui
