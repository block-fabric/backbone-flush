// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/opener.h>

#include <boost/test/unit_test.hpp>

#include <set>
#include <utility>
#include <string>
#include <vector>

using webui::IsWsl;
using webui::LoggableUri;
using webui::OpenSteps;
using webui::OpenWithFallbacks;

namespace {
//! A connect callback: & and # that no shell may read.
const std::string URI{"https://flush.markets/connect#address=bflx1qexample&nonce=abc-123_x"};

/** The ways tried, in order; `works` the programs that start (and "gio" if GIO opens it). */
struct Recorder {
    std::vector<std::vector<std::string>> tried;
    std::set<std::string> works;
    OpenSteps Steps(bool wsl)
    {
        OpenSteps steps;
        steps.wsl = wsl;
        steps.gio = [this](const std::string& uri) {
            tried.push_back({"gio", uri});
            return works.contains("gio");
        };
        steps.spawn = [this](const std::vector<std::string>& argv) {
            tried.push_back(argv);
            return works.contains(argv.at(0));
        };
        return steps;
    }
};
} // namespace

BOOST_AUTO_TEST_SUITE(webui_opener_tests)

BOOST_AUTO_TEST_CASE(gio_first)
{
    Recorder r;
    r.works = {"gio", "wslview", "xdg-open"};
    BOOST_CHECK_EQUAL(OpenWithFallbacks(URI, r.Steps(true)), "gio");
    BOOST_CHECK_EQUAL(r.tried.size(), 1U);
}

BOOST_AUTO_TEST_CASE(wsl_order_wslview_rundll32_xdg_open)
{
    // GIO has no https handler (WSL): wslview, then Windows' own handler, then xdg-open.
    Recorder r;
    r.works = {"xdg-open"};
    BOOST_CHECK_EQUAL(OpenWithFallbacks(URI, r.Steps(true)), "xdg-open");
    BOOST_REQUIRE_EQUAL(r.tried.size(), 4U);
    BOOST_CHECK(r.tried[0] == (std::vector<std::string>{"gio", URI}));
    BOOST_CHECK(r.tried[1] == (std::vector<std::string>{"wslview", URI}));
    // The address one argument, as it is: no shell, no quoting.
    BOOST_CHECK(r.tried[2] == (std::vector<std::string>{"/mnt/c/Windows/System32/rundll32.exe", "url.dll,FileProtocolHandler", URI}));
    BOOST_CHECK(r.tried[3] == (std::vector<std::string>{"xdg-open", URI}));

    Recorder w;
    w.works = {"/mnt/c/Windows/System32/rundll32.exe", "xdg-open"};
    BOOST_CHECK_EQUAL(OpenWithFallbacks(URI, w.Steps(true)), "rundll32");
    BOOST_CHECK_EQUAL(w.tried.size(), 3U);

    Recorder v;
    v.works = {"wslview"};
    BOOST_CHECK_EQUAL(OpenWithFallbacks(URI, v.Steps(true)), "wslview");
    BOOST_CHECK_EQUAL(v.tried.size(), 2U);
}

BOOST_AUTO_TEST_CASE(not_wsl_skips_windows)
{
    Recorder r;
    BOOST_CHECK_EQUAL(OpenWithFallbacks(URI, r.Steps(false)), "");
    BOOST_REQUIRE_EQUAL(r.tried.size(), 3U);
    BOOST_CHECK_EQUAL(r.tried[1][0], "wslview");
    BOOST_CHECK_EQUAL(r.tried[2][0], "xdg-open");
}

BOOST_AUTO_TEST_CASE(nothing_opens)
{
    Recorder r;
    BOOST_CHECK_EQUAL(OpenWithFallbacks(URI, r.Steps(true)), "");
    BOOST_CHECK_EQUAL(r.tried.size(), 4U);
    BOOST_CHECK_EQUAL(OpenWithFallbacks(URI, OpenSteps{}), "");
}

BOOST_AUTO_TEST_CASE(detects_wsl)
{
    BOOST_CHECK(IsWsl("Ubuntu", ""));
    BOOST_CHECK(IsWsl(nullptr, "6.18.54.1-microsoft-standard-WSL2\n"));
    BOOST_CHECK(IsWsl(nullptr, "4.4.0-19041-Microsoft"));
    BOOST_CHECK(!IsWsl(nullptr, "6.8.0-45-generic"));
    BOOST_CHECK(!IsWsl("", "6.8.0-45-generic"));
}

BOOST_AUTO_TEST_CASE(logs_the_origin_only)
{
    // Whatever ends the host, no secret after it is logged: ? # \ ; a path, a user part, a port; an opaque link's verb alone.
    const std::vector<std::pair<std::string, std::string>> cases{
        {URI, "https://flush.markets"},
        {"https://flush.markets?nonce=n1&address=bflx1qsecret", "https://flush.markets"},
        {"https://flush.markets#address=bflx1qsecret&nonce=n1", "https://flush.markets"},
        {"https://flush.markets\\connect#address=bflx1qsecret", "https://flush.markets"},
        {"https://flush.markets;address=bflx1qsecret", "https://flush.markets"},
        {"https://user:pw@flush.markets:8443/connect?nonce=n1", "https://flush.markets"},
        {"https://[::1]:8443/x#address=bflx1qsecret", "https://[::1]"},
        {"flush:connect?address=bflx1qsecret&nonce=n1", "flush:connect"},
        {"flush:connect#address=bflx1qsecret", "flush:connect"},
        {"flush:launch;nonce=n1", "flush:launch"},
        {"file:///home/me/.flush/debug.log", "file://"},
        {"nothing", "(an address)"},
        {":nonce=n1", "(an address)"},
    };
    for (const auto& [uri, logged] : cases) {
        BOOST_CHECK_EQUAL(LoggableUri(uri), logged);
        for (const char* secret : {"#", "?", ";", "\\", "nonce", "bflx1q", "pw"}) BOOST_CHECK_EQUAL(LoggableUri(uri).find(secret), std::string::npos);
    }
}

BOOST_AUTO_TEST_SUITE_END()
