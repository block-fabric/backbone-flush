// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Which addresses a download of a link's logo may connect to (webui/fetch.h, IsPublicAddress): shared by the
// downloads of every platform (fetch_soup.cpp, fetch_win.cpp), and tested alone.

#include <webui/fetch.h>

#include <compat/compat.h>

#include <array>
#include <cstdint>
#include <cstring>

namespace webui {

namespace {

/** Why an IPv4 address (its 4 bytes) is refused, or nullptr if it is public. */
const char* WhyNotPublic4(const uint8_t* a)
{
    if (a[0] == 0) return "the unspecified network 0.0.0.0/8";
    if (a[0] == 127) return "a loopback address (127.0.0.0/8)";
    if (a[0] == 10) return "a private address (10.0.0.0/8)";
    if (a[0] == 172 && (a[1] & 0xF0) == 16) return "a private address (172.16.0.0/12)";
    if (a[0] == 192 && a[1] == 168) return "a private address (192.168.0.0/16)";
    if (a[0] == 169 && a[1] == 254) return "a link-local address (169.254.0.0/16)";
    if (a[0] == 100 && (a[1] & 0xC0) == 64) return "a shared (CGNAT) address (100.64.0.0/10)";
    if ((a[0] & 0xF0) == 224) return "a multicast address (224.0.0.0/4)";
    if (a[0] == 255 && a[1] == 255 && a[2] == 255 && a[3] == 255) return "the broadcast address";
    if ((a[0] & 0xF0) == 240) return "a reserved address (240.0.0.0/4)";
    if (a[0] == 192 && a[1] == 0 && a[2] == 0) return "a special-purpose address (192.0.0.0/24)";
    if (a[0] == 198 && (a[1] & 0xFE) == 18) return "a benchmarking address (198.18.0.0/15)";
    if ((a[0] == 192 && a[1] == 0 && a[2] == 2) || (a[0] == 198 && a[1] == 51 && a[2] == 100) || (a[0] == 203 && a[1] == 0 && a[2] == 113)) return "a documentation address";
    return nullptr;
}

const char* WhyNotPublic6(const uint8_t* a)
{
    static constexpr uint8_t ZERO[16]{};
    // An IPv4 address as a dual-stack socket reports it (::ffff:0:0/96, mapped): its IPv4's rules.
    if (std::memcmp(a, ZERO, 10) == 0 && a[10] == 0xFF && a[11] == 0xFF) return WhyNotPublic4(a + 12);
    // Otherwise global unicast only (2000::/3): every other range (loopback, unspecified, the IPv4-compatible and SIIT
    // forms, both NAT64 prefixes, unique local, link-local, site-local, multicast, the reserved rest) is refused, so a
    // range left out of a list can't be reached.
    if ((a[0] & 0xE0) != 0x20) {
        if (std::memcmp(a, ZERO, 15) == 0 && a[15] == 1) return "a loopback address (::1)";
        if ((a[0] & 0xFE) == 0xFC) return "a private address (fc00::/7)";
        if (a[0] == 0xFE && (a[1] & 0xC0) == 0x80) return "a link-local address (fe80::/10)";
        if (a[0] == 0xFF) return "a multicast address (ff00::/8)";
        return "not a global unicast IPv6 address (2000::/3)";
    }
    // Inside 2000::/3: 6to4 (2002::/16) carries an IPv4, judged by its rules; Teredo (2001::/32) hides one; and
    // documentation (2001:db8::/32).
    if (a[0] == 0x20 && a[1] == 0x02) return WhyNotPublic4(a + 2);
    if (a[0] == 0x20 && a[1] == 0x01 && a[2] == 0 && a[3] == 0) return "a Teredo address (2001::/32)";
    if (a[0] == 0x20 && a[1] == 0x01 && a[2] == 0x0d && a[3] == 0xb8) return "a documentation address (2001:db8::/32)";
    return nullptr;
}

} // namespace

bool IsPublicAddress(const std::string& ip, std::string& why)
{
    std::string text{ip};
    if (text.size() >= 2 && text.front() == '[' && text.back() == ']') text = text.substr(1, text.size() - 2);
    // A zone (fe80::1%eth0) says the scope only; the address is what is checked.
    if (const size_t zone{text.find('%')}; zone != std::string::npos) text.resize(zone);
    if (text.empty() || text.size() > 45) {
        why = "not an address";
        return false;
    }
    const char* refused{nullptr};
    std::array<uint8_t, 16> bytes{};
    if (text.find(':') == std::string::npos) {
        in_addr v4{};
        if (inet_pton(AF_INET, text.c_str(), &v4) != 1) {
            why = "not an address";
            return false;
        }
        std::memcpy(bytes.data(), &v4, 4);
        refused = WhyNotPublic4(bytes.data());
    } else {
        in6_addr v6{};
        if (inet_pton(AF_INET6, text.c_str(), &v6) != 1) {
            why = "not an address";
            return false;
        }
        std::memcpy(bytes.data(), &v6, 16);
        refused = WhyNotPublic6(bytes.data());
    }
    if (refused) {
        why = std::string{"the host's address "} + text + " is " + refused + ": refused";
        return false;
    }
    return true;
}

} // namespace webui
