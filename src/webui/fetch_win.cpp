// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The download of a link's logo on Windows (webui/fetch.h): WinHTTP, synchronous, on the calling thread (the links'
// thread, never the UI's). https only (WINHTTP_FLAG_SECURE, the system's certificate checks), redirects never followed
// (WINHTTP_OPTION_REDIRECT_POLICY_NEVER: a 3xx is a failure), a timeout of each step and a deadline for the whole (the
// request's handle closed by a watchdog, which ends a call that waits on it, as WinHTTP documents).

#include <webui/fetch.h>

#include <tinyformat.h>

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace webui {

namespace {

/** An address LogoUrl took is printable ASCII: each byte one wide character. */
std::wstring Wide(const std::string& text) { return std::wstring(text.begin(), text.end()); }

std::string Why(const char* step, DWORD code, bool timed_out)
{
    if (timed_out || code == ERROR_WINHTTP_TIMEOUT) return strprintf("no answer within %d seconds", static_cast<int>(FETCH_TIMEOUT.count()));
    if (code == ERROR_WINHTTP_SECURE_FAILURE) return "its TLS certificate is not valid";
    if (code == ERROR_WINHTTP_NAME_NOT_RESOLVED) return "its host was not found";
    if (code == ERROR_WINHTTP_CANNOT_CONNECT) return "its host could not be reached";
    return strprintf("%s failed (WinHTTP error %u)", step, static_cast<unsigned>(code));
}

} // namespace

std::optional<std::vector<unsigned char>> FetchUrl(const std::string& url, size_t max_bytes, std::string& error)
{
    if (!url.starts_with("https://")) {
        error = "https addresses only";
        return std::nullopt;
    }
    const std::wstring wide{Wide(url)};
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS || !parts.lpszHostName) {
        error = "https addresses only";
        return std::nullopt;
    }
    const std::wstring host{parts.lpszHostName, parts.dwHostNameLength};
    std::wstring path{parts.lpszUrlPath ? std::wstring{parts.lpszUrlPath, parts.dwUrlPathLength} : std::wstring{}};
    if (parts.lpszExtraInfo) path += std::wstring{parts.lpszExtraInfo, parts.dwExtraInfoLength};
    if (path.empty()) path = L"/";

    const int ms{static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(FETCH_TIMEOUT).count())};
    HINTERNET session{WinHttpOpen(L"Flush-app", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session) session = WinHttpOpen(L"Flush-app", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = Why("WinHttpOpen", GetLastError(), false);
        return std::nullopt;
    }
    WinHttpSetTimeouts(session, ms, ms, ms, ms);
    HINTERNET connection{WinHttpConnect(session, host.c_str(), parts.nPort, 0)};
    if (!connection) {
        error = Why("WinHttpConnect", GetLastError(), false);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }
    LPCWSTR accept[]{L"image/png", L"image/jpeg", nullptr};
    HINTERNET request{WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, accept, WINHTTP_FLAG_SECURE)};
    if (!request) {
        error = Why("WinHttpOpenRequest", GetLastError(), false);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }
    DWORD policy{WINHTTP_OPTION_REDIRECT_POLICY_NEVER};
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    DWORD no_cookies{WINHTTP_DISABLE_COOKIES};
    WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &no_cookies, sizeof(no_cookies));

    // The deadline of the whole download: the request's handle closed at FETCH_TIMEOUT (a call waiting on it ends).
    std::mutex mutex;
    std::condition_variable cv;
    bool done{false};
    bool timed_out{false};
    std::thread watchdog{[&] {
        std::unique_lock lock{mutex};
        if (!cv.wait_for(lock, FETCH_TIMEOUT, [&] { return done; })) {
            timed_out = true;
            WinHttpCloseHandle(request);
        }
    }};
    const auto finish{[&] {
        {
            std::lock_guard lock{mutex};
            done = true;
        }
        cv.notify_all();
        watchdog.join();
        if (!timed_out) WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
    }};

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request, nullptr)) {
        const DWORD code{GetLastError()};
        finish();
        error = Why("the request", code, timed_out);
        return std::nullopt;
    }
    DWORD status{0}, size{sizeof(status)};
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        const DWORD code{GetLastError()};
        finish();
        error = Why("the reply", code, timed_out);
        return std::nullopt;
    }
    if (status != 200) {
        finish();
        error = status >= 300 && status < 400 ? strprintf("HTTP %u, a redirect (not followed)", static_cast<unsigned>(status)) : strprintf("HTTP %u (200 only)", static_cast<unsigned>(status));
        return std::nullopt;
    }
    // At most max_bytes + 1 bytes read: one more than allowed says it is too large.
    std::vector<unsigned char> bytes;
    bytes.reserve(max_bytes + 1);
    unsigned char buffer[8192];
    while (bytes.size() <= max_bytes) {
        DWORD got{0};
        const DWORD want{static_cast<DWORD>(std::min(sizeof(buffer), max_bytes + 1 - bytes.size()))};
        if (!WinHttpReadData(request, buffer, want, &got)) {
            const DWORD code{GetLastError()};
            finish();
            error = Why("reading the reply", code, timed_out);
            return std::nullopt;
        }
        if (got == 0) break;
        bytes.insert(bytes.end(), buffer, buffer + got);
    }
    finish();
    if (timed_out) {
        error = Why("", 0, true);
        return std::nullopt;
    }
    if (bytes.size() > max_bytes) {
        error = strprintf("larger than %u bytes", max_bytes);
        return std::nullopt;
    }
    return bytes;
}

} // namespace webui
