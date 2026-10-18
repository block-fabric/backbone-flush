// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The host side of the pages of FastBlocks (local.cpp): the methods the pages may call (read only, and a proof
// given by hand), and the host methods that must not act on the page's word alone: staking turned off and a proof
// of possession signed only after the desktop's yes, the node never asked on a no; bonds and unbonds refuse what is
// not a key, an amount or a proof before anything else.

//
// And the journal of BitAssets (local.cpp): the assets being created and the prices seen, in a file of the
// data directory, with no cap but a sanity one (in the preferences of the app, capped at 256 kB, it could
// fill up).

#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <util/fs.h>
#include <webui/bridge.h>
#include <webui/local.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

using namespace std::chrono_literals;

namespace {
struct Answers {
    std::mutex mutex;
    std::condition_variable cv;
    std::map<std::string, std::pair<int, UniValue>> answers;
    webui::Bridge::ReplyFn Fn()
    {
        return [this](const std::string& id, int status, const std::string& json) {
            UniValue value;
            if (!json.empty()) (void)value.read(json);
            {
                std::lock_guard lock{mutex};
                answers.emplace(id, std::pair{status, value});
            }
            cv.notify_all();
        };
    }
    std::pair<int, UniValue> Wait(const std::string& id)
    {
        std::unique_lock lock{mutex};
        BOOST_REQUIRE_MESSAGE(cv.wait_for(lock, 10s, [&] { return answers.contains(id); }), "no answer for " + id);
        return answers.at(id);
    }
};

std::string Call(const std::string& method, const UniValue& params)
{
    UniValue body{UniValue::VOBJ};
    body.pushKV("kind", "host");
    body.pushKV("method", method);
    body.pushKV("params", params);
    UniValue args{UniValue::VARR};
    args.push_back(body);
    return args.write();
}

/** A bridge with FastBlocks' host methods, a pretend node that only counts the times it is asked, and the desktop's answer. */
struct Host {
    Answers answers;
    webui::Bridge bridge{answers.Fn(), [](std::function<void()> fn) { fn(); }};
    std::atomic<int> node_asked{0};
    std::atomic<int> asked{0};
    std::atomic<bool> yes{false};
    std::string shown;
    int next{0};
    Host()
    {
        webui::local::RegisterHost(
            bridge,
            [this]() -> interfaces::Node& {
                ++node_asked;
                // No node in this test: reaching it is what is checked.
                throw webui::HostError(-4242, "the node was asked");
            },
            [this](const webui::Window::ConfirmDialog& dialog) {
                ++asked;
                shown = dialog.title + "\n" + dialog.message;
                return yes.load();
            });
    }
    ~Host() { bridge.Stop(); }
    /** The error code of a call. */
    int Code(const std::string& method, const UniValue& params)
    {
        const std::string id{std::to_string(++next)};
        bridge.Handle(id, Call(method, params));
        const auto answer{answers.Wait(id)};
        BOOST_REQUIRE_EQUAL(answer.first, 1);
        return answer.second.find_value("code").getInt<int>();
    }
};

UniValue Obj(std::initializer_list<std::pair<std::string, UniValue>> fields)
{
    UniValue o{UniValue::VOBJ};
    for (const auto& [k, v] : fields) o.pushKV(k, v);
    return o;
}

const std::string KEY(64, 'a');
} // namespace

BOOST_FIXTURE_TEST_SUITE(webui_local_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(the_pages_read_and_never_bond_on_their_own)
{
    const auto methods{webui::local::RpcMethods()};
    for (const char* m : {"getposinfo", "getstakinginfo", "getvalidators", "getvalidator", "getleaderschedule", "getanchorinfo", "listevidence", "submitevidence", "listbonds"}) {
        BOOST_CHECK_MESSAGE(methods.contains(m), m);
    }
    // Bonds, unbonds, staking and proofs of possession go through the host, which asks the desktop.
    for (const char* m : {"createbond", "createunbond", "setstaking", "signbondpop"}) BOOST_CHECK_MESSAGE(!methods.contains(m), m);
    // Layers: and the 28 of the assets (journal_of_bitassets below), of which 18 sign with the wallet's keys; no bond among them.
    BOOST_CHECK_EQUAL(methods.size(), 9U + 28U);
    const auto keyed{webui::local::KeyedRpcMethods()};
    BOOST_CHECK_EQUAL(keyed.size(), 18U);
    for (const char* m : {"getposinfo", "getstakinginfo", "listbonds", "createbond", "createunbond", "setstaking", "signbondpop"}) BOOST_CHECK_MESSAGE(!keyed.contains(m), m);
}

BOOST_AUTO_TEST_CASE(staking_off_waits_for_the_desktop)
{
    Host host;
    // Off, declined: cancelled, the node never asked.
    BOOST_CHECK_EQUAL(host.Code("fastblocks.staking", Obj({{"enabled", UniValue{false}}})), webui::BRIDGE_CANCELLED);
    BOOST_CHECK_EQUAL(host.asked.load(), 1);
    BOOST_CHECK_EQUAL(host.node_asked.load(), 0);
    BOOST_CHECK(host.shown.starts_with("Stop staking"));
    BOOST_CHECK(host.shown.find("jailed") != std::string::npos);
    // Off, accepted: the node.
    host.yes = true;
    BOOST_CHECK_EQUAL(host.Code("fastblocks.staking", Obj({{"enabled", UniValue{false}}})), -4242);
    BOOST_CHECK_EQUAL(host.node_asked.load(), 1);
    // On: nothing to ask.
    host.yes = false;
    BOOST_CHECK_EQUAL(host.Code("fastblocks.staking", Obj({{"enabled", UniValue{true}}})), -4242);
    BOOST_CHECK_EQUAL(host.asked.load(), 2);
    // Not a choice: refused.
    BOOST_CHECK_EQUAL(host.Code("fastblocks.staking", Obj({{"enabled", UniValue{"no"}}})), webui::BRIDGE_INVALID_REQUEST);
}

BOOST_AUTO_TEST_CASE(proof_of_possession_waits_for_the_desktop)
{
    Host host;
    BOOST_CHECK_EQUAL(host.Code("fastblocks.pop", Obj({{"owner", UniValue{"rsc1qowner\n\xe2\x80\xae" "evil"}}})), webui::BRIDGE_CANCELLED);
    BOOST_CHECK_EQUAL(host.node_asked.load(), 0);
    // The owner on one line.
    BOOST_CHECK(host.shown.find("rsc1qowner") != std::string::npos);
    BOOST_CHECK(host.shown.find("rsc1qowner\n") == std::string::npos);
    host.yes = true;
    BOOST_CHECK_EQUAL(host.Code("fastblocks.pop", Obj({{"owner", UniValue{"rsc1qowner"}}, {"pubkey", UniValue{KEY}}})), -4242);
    BOOST_CHECK_EQUAL(host.node_asked.load(), 1);
    // The key, whole, cut in groups of eight.
    BOOST_CHECK(host.shown.find(std::string(8, 'a') + " " + std::string(8, 'a')) != std::string::npos);
    BOOST_CHECK_EQUAL(host.Code("fastblocks.pop", Obj({{"owner", UniValue{"rsc1qowner"}}, {"pubkey", UniValue{"abc"}}})), webui::BRIDGE_INVALID_REQUEST);
}

BOOST_AUTO_TEST_CASE(bonds_refuse_what_is_not_a_key_an_amount_or_a_proof_first)
{
    Host host;
    host.yes = true;
    const auto bond{[&](const std::string& key, const std::string& amount, const UniValue& pop = UniValue{}) {
        UniValue params{Obj({{"wallet", UniValue{"w"}}, {"pubkey", UniValue{key}}, {"amount", UniValue{amount}}})};
        if (!pop.isNull()) params.pushKV("pop", pop);
        return host.Code("fastblocks.bond", params);
    }};
    BOOST_CHECK_EQUAL(bond("abc", "1"), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(bond(std::string(64, 'g'), "1"), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(bond(KEY, "0"), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(bond(KEY, "-1"), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(bond(KEY, "1.000000001"), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(bond(KEY, "1", UniValue{"ab"}), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(host.node_asked.load(), 0);
    BOOST_CHECK_EQUAL(host.asked.load(), 0);
    // A good one goes to the node (the wallet, its bond), and only then the desktop is asked.
    BOOST_CHECK_EQUAL(bond(KEY, "1.5"), -4242);
    BOOST_CHECK_EQUAL(host.node_asked.load(), 1);
    BOOST_CHECK_EQUAL(host.asked.load(), 0);

    // Unbond: an amount, or all.
    const auto unbond{[&](const std::string& amount) {
        return host.Code("fastblocks.unbond", Obj({{"wallet", UniValue{"w"}}, {"pubkey", UniValue{KEY}}, {"amount", UniValue{amount}}}));
    }};
    BOOST_CHECK_EQUAL(unbond("most"), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(unbond("all"), -4242);
    BOOST_CHECK_EQUAL(unbond("0.25"), -4242);
    BOOST_CHECK_EQUAL(host.asked.load(), 0);
}

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

BOOST_AUTO_TEST_SUITE_END()
