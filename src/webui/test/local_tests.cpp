// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Layers' host side (local.cpp): FastBlocks' and BitAssets' pages.
//
// The host side of the pages of FastBlocks (local.cpp): the methods the pages may call (read only, and a proof
// given by hand), and the host methods that must not act on the page's word alone: staking turned off and a proof
// of possession signed only after the desktop's yes, the node never asked on a no; bonds and unbonds refuse what is
// not a key, an amount or a proof before anything else.
//
// And the journal of BitAssets (local.cpp): the assets being created and the prices seen, in a file of the
// data directory, with no cap but a sanity one (in the preferences of the app, capped at 256 kB, it could
// fill up).

#include <interfaces/node.h>
#include <test/util/setup_common.h>
#include <webui/confirm.h>
#include <tinyformat.h>
#include <util/fs.h>
#include <webui/bridge.h>
#include <webui/dialog.h>
#include <webui/guard.h>
#include <webui/local.h>
#include <test/bitassets_logo_samples.h>
#include <util/strencodings.h>

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
    //! The questions with the keys (bonds, unbonds: the passphrase in the dialog of the host, audit 7 #1).
    std::atomic<int> keys_asked{0};
    std::string shown;
    webui::dialog::Level level{webui::dialog::Level::INFO};
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
                level = dialog.level;
                return yes.load();
            },
            [this](const std::string&, webui::Window::ConfirmDialog, bool, bool, const std::function<UniValue()>& run) {
                ++keys_asked;
                if (!yes) throw webui::HostError(webui::BRIDGE_CANCELLED, "declined");
                return run();
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
    // Layers: and the 17 of the assets the pages read (journal_of_bitassets below; listassettransactions with no key; the
    // searches, searchmyassets), two of which sign with the wallet's keys, and the 7 of the DEX (its info, routes, TWAP,
    // the pool kinds' quotes, simulation, preview, depth); the 23 that pay or sign (setassetlogo and setassetdisplayname,
    // routes and the pool kinds' among them) are confirmed by the host; no bond among any of them.
    BOOST_CHECK_EQUAL(methods.size(), 9U + 17U + 7U + 1U + 5U + 4U + 6U);
    const auto keyed{webui::local::KeyedRpcMethods()};
    BOOST_CHECK_EQUAL(keyed.size(), 2U);
    const auto confirmed{webui::local::ConfirmedRpcMethods()};
    BOOST_CHECK_EQUAL(confirmed.size(), 31U);
    for (const char* m : {"getposinfo", "getstakinginfo", "listbonds", "createbond", "createunbond", "setstaking", "signbondpop"}) {
        BOOST_CHECK_MESSAGE(!keyed.contains(m), m);
        BOOST_CHECK_MESSAGE(!confirmed.contains(m), m);
    }
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
    // A danger: the validator is jailed if it misses its slots (user decision).
    BOOST_CHECK(host.level == webui::dialog::Level::DANGER);
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
    // Never asked before the node told what the wallet holds; never a passphrase from the page.
    BOOST_CHECK_EQUAL(host.keys_asked.load(), 0);
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

BOOST_AUTO_TEST_CASE(logos_as_the_host_shows_them)
{
    // A logo the rules take: a data: URL of the type they found, its size and hash; the rows of a confirmation show it as a picture.
    std::string reason;
    const auto png{webui::local::ReadLogoParam(UniValue{EncodeBase64(ParseHex(LOGO_PNG_PALETTE))}, reason)};
    BOOST_REQUIRE(png);
    BOOST_CHECK_EQUAL(png->mime, "image/png");
    BOOST_CHECK(png->url.starts_with("data:image/png;base64,iVBORw0KGgo"));
    BOOST_CHECK(webui::dialog::IsImageDataUrl(png->url));
    const auto rows{webui::local::LogoRows(*png)};
    BOOST_REQUIRE_EQUAL(rows.size(), 3U);
    BOOST_CHECK(rows[0].kind == webui::dialog::Row::Kind::IMAGE);
    BOOST_CHECK_EQUAL(rows[2].value, png->hash);
    const auto jpeg{webui::local::ReadLogoParam(UniValue{EncodeBase64(ParseHex(LOGO_JPEG_BASELINE))}, reason)};
    BOOST_REQUIRE(jpeg);
    BOOST_CHECK(jpeg->url.starts_with("data:image/jpeg;base64,"));
    // Whatever the page or the chain says the type is, a file the rules refuse is no logo: an SVG, a PNG with more after it.
    BOOST_CHECK(!webui::local::ReadLogoParam(UniValue{EncodeBase64(std::string{"<svg onload=alert(1)/>"})}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-logo-format");
    auto trailing{ParseHex(LOGO_PNG_PALETTE)};
    trailing.push_back('<');
    BOOST_CHECK(!webui::local::ReadLogoParam(UniValue{EncodeBase64(trailing)}, reason));
    BOOST_CHECK(!webui::local::ReadLogoParam(UniValue{"not base64!"}, reason));
    BOOST_CHECK(!webui::local::ReadLogoParam(UniValue{std::string(50000, 'A')}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-logo-oversize");
    // Setting one pays: confirmed on the desktop; the page reads the transfers without the keys.
    const auto methods{webui::local::ConfirmedRpcMethods()};
    BOOST_CHECK(methods.contains("setassetlogo") && methods.at("setassetlogo").kind == webui::confirm::Method::Kind::SPEND);
    BOOST_CHECK(webui::local::RpcMethods().contains("listassettransactions"));
    BOOST_CHECK(!webui::local::RpcMethods().contains("setassetlogo"));
    // A display name pays too, with its two parameters (an empty one removes it), never from the page alone.
    BOOST_REQUIRE(methods.contains("setassetdisplayname"));
    BOOST_CHECK(methods.at("setassetdisplayname").kind == webui::confirm::Method::Kind::SPEND);
    BOOST_CHECK_EQUAL(methods.at("setassetdisplayname").required, 2U);
    BOOST_CHECK(!webui::local::RpcMethods().contains("setassetdisplayname"));
    // The registration sent after a reservation counts its display name in the fee it may pay.
    const auto& reveal{methods.at("reserveasset").reveal};
    BOOST_REQUIRE(reveal && reveal->second_vsize);
    UniValue plain{UniValue::VARR}, named{UniValue::VARR};
    for (UniValue* p : {&plain, &named}) {
        p->push_back("GOLD");
        p->push_back("1000");
        p->push_back(2);
        p->push_back(UniValue{UniValue::VOBJ});
        p->push_back(true);
    }
    named.push_back(UniValue{});
    named.push_back(std::string(64, 'g'));
    BOOST_CHECK_GE(reveal->second_vsize(named), reveal->second_vsize(plain) + 64);
}

BOOST_AUTO_TEST_CASE(logo_cache_counts_what_it_keeps)
{
    // The same asset asked again and again: one entry, its bytes counted once (the security review of the cache).
    webui::local::LogoCache cache;
    webui::local::Logo logo{"image/png", 3, std::string(64, 'a'), "data:image/png;base64," + std::string(40000, 'A')};
    for (int i{0}; i < 2000; ++i) cache.Put(std::string(64, 'b'), logo);
    BOOST_CHECK_EQUAL(cache.Size(), 1U);
    BOOST_CHECK_EQUAL(cache.Bytes(), logo.url.size());
    BOOST_CHECK_EQUAL(cache.Bytes(), cache.CountedBytes());
    // Many assets: the oldest go, never below an empty list, and the count stays the sum.
    for (int i{0}; i < 2000; ++i) cache.Put(strprintf("%064x", i), logo);
    BOOST_CHECK(cache.Size() <= webui::local::LogoCache::MAX_ENTRIES);
    BOOST_CHECK(cache.Bytes() <= webui::local::LogoCache::MAX_BYTES);
    BOOST_CHECK_EQUAL(cache.Bytes(), cache.CountedBytes());
    // One larger than the whole cache is not kept, and empties nothing it should not count.
    webui::local::Logo huge{logo};
    huge.url = std::string(webui::local::LogoCache::MAX_BYTES + 1, 'A');
    const size_t before{cache.Size()};
    cache.Put(std::string(64, 'c'), huge);
    BOOST_CHECK_EQUAL(cache.Size(), before);
    BOOST_CHECK_EQUAL(cache.Bytes(), cache.CountedBytes());
    // Replacing a kept asset's logo by a huge one drops the old one too.
    cache.Put(std::string(64, 'd'), logo);
    cache.Put(std::string(64, 'd'), huge);
    BOOST_CHECK(cache.Size() < before);
    BOOST_CHECK_EQUAL(cache.Bytes(), cache.CountedBytes());
}

namespace {
/** A node to read from (getasset, getpool), for the describers. */
struct ReadingNode : TestingSetup {
    std::unique_ptr<interfaces::Node> node{interfaces::MakeNode(m_node)};
    std::string Refused(const std::string& method, const UniValue& params)
    {
        const auto& methods{webui::local::ConfirmedRpcMethods()};
        BOOST_REQUIRE(methods.contains(method));
        try {
            methods.at(method).describe(webui::confirm::Call{*node, "w", method, params});
        } catch (const webui::HostError& e) {
            return e.what();
        }
        return "";
    }
};
UniValue Arr(std::initializer_list<UniValue> items)
{
    UniValue a{UniValue::VARR};
    for (const UniValue& v : items) a.push_back(v);
    return a;
}
} // namespace

BOOST_FIXTURE_TEST_CASE(confirmations_resolve_every_asset_and_pool, ReadingNode)
{
    // Security review 2026-10-21: a confirmation shows an asset as the node resolves it, or refuses. A name (or a number) the
    // page could pick to read as one asset while the node trades another is refused before anything is shown; so is an id
    // no asset has, and a pool that is not one.
    const std::string some_id{"0x" + std::string(64, '1')};
    for (const auto& [method, params] : std::vector<std::pair<std::string, UniValue>>{
             {"swapasset", Arr({UniValue{"GOLD"}, UniValue{"1"}, UniValue{"CHN"}})},
             {"swapasset", Arr({UniValue{"CHN"}, UniValue{"1"}, UniValue{"1739-0029"}})},
             {"sendasset", Arr({UniValue{"addr"}, UniValue{"gold"}, UniValue{"1"}})},
             {"addliquidity", Arr({UniValue{"0xabc"}, UniValue{"1"}, UniValue{"CHN"}, UniValue{"1"}})},
             {"swaproute", Arr({Arr({UniValue{"CHN"}, UniValue{"SILVER"}}), UniValue{"1"}})},
             {"createpool", Arr({UniValue{"USDX"}, UniValue{"1"}, UniValue{"CHN"}, UniValue{"1"}})},
             {"addpoolliquidity", Arr({UniValue{std::string(64, 'a')}, Obj({{"GOLD", UniValue{"1"}}})})},
             {"buyfromlaunch", Arr({UniValue{"MOON"}, UniValue{"1"}})},
             {"claimlaunch", Arr({UniValue{"1739-0029"}})},
         }) {
        const std::string why{Refused(method, params)};
        BOOST_CHECK_MESSAGE(why.find("id") != std::string::npos || why.find("No such pool") != std::string::npos, method << ": " << why);
    }
    // An id no asset has (this chain has no assets at all): refused, never shown as given.
    BOOST_CHECK_NE(Refused("swapasset", Arr({UniValue{some_id}, UniValue{"1"}, UniValue{"CHN"}})), "");
    BOOST_CHECK_NE(Refused("swappools", Arr({UniValue{"CHN"}, UniValue{"1"}, Arr({UniValue{std::string(64, 'b')}})})), "");
    BOOST_CHECK_NE(Refused("removepoolliquidity", Arr({UniValue{"not a pool"}})), "");
}

BOOST_AUTO_TEST_SUITE_END()
