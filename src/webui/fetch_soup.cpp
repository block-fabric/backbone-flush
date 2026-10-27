// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The download of a link's logo on Linux (webui/fetch.h): libsoup 3, the HTTP library WebKitGTK links already. A
// session of its own for each download, made and used on the calling thread (the links' thread, never the UI's), with
// its synchronous API: no redirect, no content decoder (the bytes hashed are the bytes served), the system's TLS
// certificates, a timeout of each step and a deadline for the whole (a cancellable cancelled by a watchdog).

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
    SoupSession* session{soup_session_new_with_options("timeout", seconds, "idle-timeout", seconds, "user-agent", "Flush-app", nullptr)};
    // The bytes as served (their SHA-256 is checked): no Accept-Encoding, nothing decoded.
    soup_session_remove_feature_by_type(session, SOUP_TYPE_CONTENT_DECODER);

    // The deadline of the whole download: cancelled at FETCH_TIMEOUT, whatever step it is at (a server that sends a
    // byte now and then keeps each step under its timeout).
    GCancellable* cancellable{g_cancellable_new()};
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
        error = Why(gerror, timed_out);
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
            if (failed) {
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
    g_object_unref(cancellable);
    g_object_unref(session);
    g_object_unref(msg);
    return result;
}

} // namespace webui
