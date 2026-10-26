// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/opener.h>

#include <boost/test/unit_test.hpp>

#include <set>
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
    // A connect callback: no address, nonce or network in the log; an action link: its verb alone.
    for (const std::string& uri : {URI, std::string{"https://user:pw@flush.markets/connect?x=1#address=bflx1q&nonce=n"}, std::string{"flush:connect?v=1&nonce=abc#x"}}) {
        const std::string logged{LoggableUri(uri)};
        BOOST_CHECK_EQUAL(logged.find('#'), std::string::npos);
        BOOST_CHECK_EQUAL(logged.find('?'), std::string::npos);
        BOOST_CHECK_EQUAL(logged.find("nonce"), std::string::npos);
        BOOST_CHECK_EQUAL(logged.find("bflx1q"), std::string::npos);
    }
    BOOST_CHECK_EQUAL(LoggableUri(URI), "https://flush.markets");
    BOOST_CHECK_EQUAL(LoggableUri("https://user:pw@flush.markets/connect#a"), "https://flush.markets");
    BOOST_CHECK_EQUAL(LoggableUri("flush:connect?v=1&nonce=abc"), "flush:connect");
    BOOST_CHECK_EQUAL(LoggableUri("file:///home/me/.flush/debug.log"), "file://");
    BOOST_CHECK_EQUAL(LoggableUri("nothing"), "(an address)");
}

BOOST_AUTO_TEST_SUITE_END()
