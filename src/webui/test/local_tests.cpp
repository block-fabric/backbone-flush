// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The journal of BitAssets (local.cpp): the assets being created and the prices seen, in a file of the
// data directory, with no cap but a sanity one (in the preferences of the app, capped at 256 kB, it could
// fill up).

#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <util/fs.h>
#include <webui/bridge.h>
#include <webui/dialog.h>
#include <webui/guard.h>
#include <webui/local.h>

#include <boost/test/unit_test.hpp>

#include <fstream>
#include <string>

BOOST_FIXTURE_TEST_SUITE(webui_local_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(journal_of_bitassets)
{
    const fs::path path{m_args.GetDataDirNet() / webui::local::JOURNAL_FILE};
    BOOST_CHECK(webui::local::ReadStore(path).isNull());

    // Larger than the preferences ever held: written whole, read back.
    UniValue value{UniValue::VOBJ};
    UniValue pending{UniValue::VOBJ};
    for (int i{0}; i < 4000; ++i) {
        UniValue rec{UniValue::VOBJ};
        rec.pushKV("supply", "1000");
        rec.pushKV("name", std::string(64, 'a' + i % 6));
        pending.pushKV(strprintf("w|%d", i), rec);
    }
    value.pushKV("pending", pending);
    BOOST_CHECK_GT(value.write().size(), 256U << 10);
    webui::local::WriteStore(path, value);
    const UniValue back{webui::local::ReadStore(path)};
    BOOST_CHECK_EQUAL(back.write(), value.write());
    BOOST_CHECK(!fs::exists(fs::PathFromString(fs::PathToString(path) + ".new")));

    // What is not an object, or too large, is refused, and the file stays as it was.
    BOOST_CHECK_THROW(webui::local::WriteStore(path, UniValue{"text"}), webui::HostError);
    UniValue huge{UniValue::VOBJ};
    huge.pushKV("x", std::string(webui::local::MAX_STORE_BYTES, 'x'));
    BOOST_CHECK_THROW(webui::local::WriteStore(path, huge), webui::HostError);
    BOOST_CHECK_EQUAL(webui::local::ReadStore(path).write(), value.write());

    // A file that cannot be read is kept aside, not lost: the names being created, with their supply and data.
    {
        std::ofstream broken{path.std_path(), std::ios::trunc};
        broken << "{\"pending\": {";
    }
    BOOST_CHECK(webui::local::ReadStore(path).isNull());
    BOOST_CHECK(fs::exists(fs::PathFromString(fs::PathToString(path) + ".unreadable")));
}

BOOST_AUTO_TEST_CASE(confirmation_of_an_amount_past_2_53_units)
{
    // What the host shows of a send of 2^60 + 1 units of an asset with 8 decimals (audit 7 E-BA6): the page sends the
    // amount as text, and the dialog of the host says it digit for digit, as the node will take it.
    UniValue params{UniValue::VARR};
    params.push_back("rbast1qexample");
    params.push_back("0x" + std::string(64, 'a'));
    params.push_back("11529215046.06846977");
    const auto rows{webui::guard::ParamRows("sendasset", params, {"address", "asset", "amount"}, [](const std::string& name) {
        return name == "address" ? webui::guard::ParamRole::DESTINATION : name == "amount" ? webui::guard::ParamRole::AMOUNT : webui::guard::ParamRole::OTHER;
    })};
    bool shown{false};
    for (const auto& row : rows) {
        if (row.label == "amount") {
            BOOST_CHECK_EQUAL(row.value, "11529215046.06846977");
            shown = true;
        }
    }
    BOOST_CHECK(shown);
}

BOOST_AUTO_TEST_SUITE_END()
