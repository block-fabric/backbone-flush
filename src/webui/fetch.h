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
 * than `max_bytes` is a failure), HTTP 200 only, no proxy, and the connection's own remote address public
 * (IsPublicAddress: checked as it connects, before TLS or HTTP; a refused one is closed at once). The bytes, or nullopt with why in `error` (said to the user).
 * It blocks: never called on the UI thread.
 *
 * Linux: libsoup 3 (fetch_soup.cpp, the one of WebKitGTK); Windows: WinHTTP (fetch_win.cpp); elsewhere none
 * (fetch_generic.cpp: "not supported here", and the app asks for the file).
 */
std::optional<std::vector<unsigned char>> FetchUrl(const std::string& url, size_t max_bytes, std::string& error);

/**
 * Whether a download may talk to this address (8.1.0, security review: a DNS name rebound to the user's own network).
 * `ip` is an address as text (IPv4 dotted, or IPv6, with or without [] and a %zone), the remote address of the very
 * connection the download made (never a lookup of its own: no time-of-check race). Refused, with why in `why`:
 * not an address; the unspecified address and 0.0.0.0/8; loopback (127.0.0.0/8, ::1); private (10/8, 172.16/12,
 * 192.168/16, fc00::/7, and the old site-local fec0::/10); link-local (169.254/16, fe80::/10); CGNAT (100.64/10);
 * multicast (224/4, ff00::/8), the reserved 240/4 and the broadcast 255.255.255.255; and every IPv4 address that one
 * of these refuses written inside IPv6: IPv4-mapped (::ffff:a.b.c.d), IPv4-compatible (::a.b.c.d), NAT64 (64:ff9b::/96)
 * and 6to4 (2002:AABB:CCDD::/48).
 */
bool IsPublicAddress(const std::string& ip, std::string& why);

} // namespace webui

#endif // BITCOIN_WEBUI_FETCH_H
