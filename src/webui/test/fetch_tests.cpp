// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The addresses a download of a link's logo may connect to (webui/fetch.h, IsPublicAddress): every range refused,
// as IPv4 and inside IPv6 (mapped, compatible, NAT64, 6to4), the edges of each range, and public addresses taken.

#include <webui/fetch.h>

#include <boost/test/unit_test.hpp>

#include <string>

using webui::IsPublicAddress;

namespace {
bool Public(const std::string& ip)
{
    std::string why;
    const bool ok{IsPublicAddress(ip, why)};
    BOOST_CHECK_MESSAGE(ok == why.empty(), ip << ": " << why);
    return ok;
}
std::string Why(const std::string& ip)
{
    std::string why;
    IsPublicAddress(ip, why);
    return why;
}
} // namespace

BOOST_AUTO_TEST_SUITE(webui_fetch_tests)

BOOST_AUTO_TEST_CASE(refused_ranges)
{
    for (const char* ip : {
             // 0.0.0.0/8, loopback, private, link-local, CGNAT, multicast, reserved and broadcast.
             "0.0.0.0", "0.1.2.3", "127.0.0.1", "127.255.255.254", "10.0.0.0", "10.255.255.255", "172.16.0.0", "172.31.255.255",
             "192.168.0.1", "192.168.255.255", "169.254.0.1", "169.254.169.254", "100.64.0.0", "100.127.255.255", "224.0.0.1",
             "239.255.255.250", "240.0.0.1", "255.255.255.255",
             // IPv6: unspecified, loopback, unique local, link-local, site-local, multicast.
             "::", "::1", "fc00::1", "fd12:3456::1", "fe80::1", "febf::1", "fec0::1", "ff02::1", "FF05::2",
             // IPv4 inside IPv6: mapped, compatible, NAT64, 6to4.
             "::ffff:127.0.0.1", "::ffff:7f00:1", "::FFFF:10.1.2.3", "::ffff:192.168.1.1", "::ffff:169.254.169.254", "::ffff:100.64.1.1",
             "::ffff:0.0.0.0", "::ffff:255.255.255.255", "::ffff:224.0.0.1", "::127.0.0.1", "::10.0.0.1", "64:ff9b::127.0.0.1",
             "64:ff9b::a00:1", "2002:7f00:1::1", "2002:c0a8:101::",
             // Allowlist: only global unicast IPv6 (2000::/3) and public IPv4 inside it.
             "64:ff9b::808:808", "64:ff9b:1::808:808", "::ffff:0:808:808", "fbff::1", "fe7f::1", "2001::1", "2001:db8::1",
             "192.0.0.8", "198.18.0.1", "198.19.255.255", "192.0.2.1", "198.51.100.7", "203.0.113.9",
             // As a connection may write them: in brackets, with a zone.
             "[::1]", "[fe80::1%eth0]", "fe80::1%12",
         }) {
        BOOST_CHECK_MESSAGE(!Public(ip), ip);
    }
    BOOST_CHECK_NE(Why("127.0.0.1").find("loopback"), std::string::npos);
    BOOST_CHECK_NE(Why("::ffff:10.0.0.1").find("private"), std::string::npos);
    BOOST_CHECK_NE(Why("169.254.169.254").find("link-local"), std::string::npos);
    BOOST_CHECK_NE(Why("100.64.0.1").find("CGNAT"), std::string::npos);
    BOOST_CHECK_NE(Why("fd00::1").find("fc00::/7"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(public_addresses_and_edges)
{
    for (const char* ip : {
             "1.1.1.1", "8.8.8.8", "93.184.216.34", "9.255.255.255", "11.0.0.0", "126.255.255.255", "128.0.0.0", "172.15.255.255",
             "172.32.0.0", "192.167.255.255", "192.169.0.0", "169.253.255.255", "169.255.0.0", "100.63.255.255", "100.128.0.0",
             "223.255.255.255", "2606:4700:4700::1111", "2001:4860:4860::8888", "::ffff:8.8.8.8", "2002:808:808::1",
             "192.0.1.1", "198.17.255.255", "198.20.0.0", "[2606:4700::1]",
         }) {
        BOOST_CHECK_MESSAGE(Public(ip), ip);
    }
}

BOOST_AUTO_TEST_CASE(what_is_not_an_address)
{
    for (const char* ip : {"", "localhost", "example.com", "127.1", "0x7f.0.0.1", "1.2.3.4.5", "256.1.1.1", "1.2.3", "::ffff:1.2.3.4.5", "[]", "1.2.3.4 "}) {
        BOOST_CHECK_MESSAGE(!Public(ip), ip);
        BOOST_CHECK_EQUAL(Why(ip), "not an address");
    }
}

BOOST_AUTO_TEST_SUITE_END()
