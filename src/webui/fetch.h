// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_FETCH_H
#define BITCOIN_WEBUI_FETCH_H

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace webui {

//! How long a download of a link's logo may take in all (connecting, TLS, the reply), and each of its steps.
inline constexpr std::chrono::seconds FETCH_TIMEOUT{10};

/**
 * Download what an https address serves (8.1.0: the logo a launch or register link names by logo_url, linkhost.h), for
 * LinkHost::Deps::fetch: a GET, https only (the system's certificates checked), no redirect followed (a 3xx is a
 * failure), no content encoding asked for, FETCH_TIMEOUT for the whole of it, at most `max_bytes + 1` bytes read (more
 * than `max_bytes` is a failure), and HTTP 200 only. The bytes, or nullopt with why in `error` (said to the user).
 * It blocks: never called on the UI thread.
 *
 * Linux: libsoup 3 (fetch_soup.cpp, the one of WebKitGTK); Windows: WinHTTP (fetch_win.cpp); elsewhere none
 * (fetch_generic.cpp: "not supported here", and the app asks for the file).
 */
std::optional<std::vector<unsigned char>> FetchUrl(const std::string& url, size_t max_bytes, std::string& error);

} // namespace webui

#endif // BITCOIN_WEBUI_FETCH_H
