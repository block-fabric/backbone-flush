// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The action links of flush.markets (webui/links.h, webui/linkhost.h): the strict parser (versions, networks, expiry,
// unknown and repeated parameters, ids, amounts, sizes, characters), the launch files, the amounts in units, the answer of
// a connect; then the host: each verb resolved by id against a chain (a pretend one), the floor checked against a fresh
// quote, the question that says where the link comes from (confirm::Preface, through the confirmed RPC methods), a link
// used once, a connect answered once and only to the one callback; and the handler of the links on a Linux desktop.

#include <crypto/sha256.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <rpc/server.h>
#include <validationinterface.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <univalue.h>
#include <util/fs.h>
#include <util/strencodings.h>
#include <webui/bridge.h>
#include <webui/confirm.h>
#include <webui/linkhost.h>
#include <webui/links.h>
#include <webui/urischeme.h>
#include <test/bitassets_logo_samples.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>
#include <map>
#include <string>
#include <vector>

using namespace webui::links;

namespace {
constexpr int64_t NOW{1'800'000'000};
const std::string NET{"regtest"};
const std::string TOKEN{"10ab000000000000000000000000000000000000000000000000000000a3ca"};
const std::string TOKEN_ID{std::string(4, '1') + std::string(60, 'a')};
const std::string POOL_ID{std::string(64, 'b')};
const std::string GOLD_ID{std::string(64, 'c')};
const std::string POOL2_ID{std::string(64, 'e')};

std::string Exp(int64_t t = NOW + 600) { return strprintf("%d", t); }
std::string Buy(const std::string& extra = "", const std::string& network = NET)
{
    return "flush:buy?v=1&network=" + network + "&exp=" + Exp() + "&launch=TIDE&asset=" + TOKEN_ID + "&ecx=5&min_tokens=900&slippage_bps=100" + extra;
}

std::string Refused(const std::string& uri, int64_t now = NOW)
{
    try {
        Parse(uri, NET, now);
    } catch (const LinkError& e) {
        return e.what();
    }
    return "";
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(webui_links_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(each_verb_reads_exactly_its_parameters)
{
    const Request buy{Parse(Buy(), NET, NOW)};
    BOOST_CHECK(buy.verb == Verb::BUY);
    BOOST_CHECK_EQUAL(buy.ticker, "TIDE");
    BOOST_CHECK_EQUAL(buy.asset.GetHex(), TOKEN_ID);
    BOOST_CHECK_EQUAL(buy.amount, "5");
    BOOST_CHECK_EQUAL(buy.floor, "900");
    BOOST_CHECK_EQUAL(*buy.slippage_bps, 100);
    BOOST_CHECK_EQUAL(buy.expires, NOW + 600);

    const Request sell{Parse("flush:sell?v=1&network=regtest&exp=" + Exp() + "&launch=TIDE&asset=" + TOKEN_ID + "&tokens=1000.5&min_ecx=0.001", NET, NOW)};
    BOOST_CHECK(sell.verb == Verb::SELL);
    BOOST_CHECK_EQUAL(sell.amount, "1000.5");
    BOOST_CHECK(!sell.slippage_bps);

    const Request swap{Parse("flush:swap?v=1&network=regtest&exp=" + Exp() + "&pool=" + POOL_ID + "&from=ECX&to=NOVA9&amount=2&min_out=48500&slippage_bps=50", NET, NOW)};
    BOOST_CHECK(swap.verb == Verb::SWAP);
    BOOST_CHECK_EQUAL(swap.from, "ECX");
    BOOST_CHECK_EQUAL(swap.to, "NOVA9");

    const Request launch{Parse("flush:launch?v=1&network=regtest&exp=" + Exp() + "&ticker=LUMEN&name=Lumen%20Works&desc=Light%20for%0Anight%20markets.&link=https%3A%2F%2Flumen.example%2Fa&link=https%3A%2F%2Fx.example&ecx=3&min_tokens=1243396.639607&slippage_bps=100", NET, NOW)};
    BOOST_CHECK(launch.verb == Verb::LAUNCH);
    BOOST_CHECK_EQUAL(launch.name, "Lumen Works");
    BOOST_CHECK_EQUAL(launch.description, "Light for\nnight markets.");
    BOOST_CHECK_EQUAL(launch.links.size(), 2U);
    BOOST_CHECK_EQUAL(launch.links[0], "https://lumen.example/a");

    const Request connect{Parse("flush:connect?v=1&network=regtest&exp=" + Exp() + "&origin=https%3A%2F%2Fflush.markets&callback=https%3A%2F%2Fflush.markets%2Fconnect&nonce=9f2c0a1b2c3d4e5f60718293", NET, NOW)};
    BOOST_CHECK(connect.verb == Verb::CONNECT);
    BOOST_CHECK_EQUAL(ConnectAnswer(connect, "rflx1qexampleaddress0"), "https://flush.markets/connect#address=rflx1qexampleaddress0&nonce=9f2c0a1b2c3d4e5f60718293&network=regtest");

    // The scheme is not case-sensitive; a payment (an address has a "1") is not an action.
    BOOST_CHECK(IsAction("FLUSH:buy?v=1"));
    BOOST_CHECK(!IsAction("flush:bflx1qxyz?amount=1"));
    BOOST_CHECK(!IsAction("flush:BUY?v=1"));
    BOOST_CHECK(!IsAction("bitcoin:buy?v=1"));
}

BOOST_AUTO_TEST_CASE(malformed_links_are_refused)
{
    for (const std::string& uri : std::vector<std::string>{
             "flush:buy",                                   // no query
             "flush:buy?",                                  // no version
             "flush:buy?v=2&network=regtest",               // another version
             Buy("&v=1"),                                   // twice
             Buy("&color=blue"),                            // unknown
             Buy("&req-thing=1"),                           // unknown, required
             Buy("&&x=1"),                                  // empty parameter
             Buy("&=1"),                                    // no name
             Buy("&Launch=TIDE"),                           // a name in capitals
             Buy("&link=https%3A%2F%2Fa.example"),          // link: launch only
             Buy("#frag"),                                  // a fragment
             Buy("&x=a b"),                                 // a space
             Buy("&x=a+b"),                                 // a plus (a space for some readers)
             Buy("&x=%zz"),                                 // a bad escape
             Buy("&x=%4"),                                  // a cut escape
         }) {
        BOOST_CHECK_MESSAGE(!Refused(uri).empty(), uri);
    }
    BOOST_CHECK_NE(Refused("flush:steal?v=1&network=regtest&exp=" + Exp()).find("not an action this app knows"), std::string::npos);
    BOOST_CHECK_NE(Refused("flush:buy?v=2&network=regtest").find("version 2"), std::string::npos);
    // Each parameter is needed.
    for (const std::string& key : {"launch", "asset", "ecx", "min_tokens", "network", "exp"}) {
        std::string uri{Buy()};
        const size_t at{uri.find("&" + std::string{key} + "=")};
        const size_t end{uri.find('&', at + 1)};
        uri.erase(at, end == std::string::npos ? std::string::npos : end - at);
        BOOST_CHECK_MESSAGE(Refused(uri).find("missing") != std::string::npos, key << ": " << Refused(uri));
    }
    // Too long, whatever it holds.
    BOOST_CHECK_NE(Refused(Buy("&x=" + std::string(MAX_URI, 'a'))).find("too long"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(another_network_is_refused_clearly)
{
    const std::string why{Refused(Buy("", "ecashbeta"))};
    BOOST_CHECK_NE(why.find("for the network ecashbeta, and this app runs regtest"), std::string::npos);
    BOOST_CHECK_NE(Refused(Buy("", "main")).find("not a network of Flush"), std::string::npos);
    BOOST_CHECK_NE(Refused(Buy("", "ECASH")).find("not a network of Flush"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(old_links_expire)
{
    BOOST_CHECK(Refused(Buy(), NOW + 600 + CLOCK_SKEW).empty());
    BOOST_CHECK_NE(Refused(Buy(), NOW + 600 + CLOCK_SKEW + 1).find("expired"), std::string::npos);
    // A link good for ever is no expiry.
    std::string far{Buy()};
    far.replace(far.find("exp=") + 4, 10, Exp(NOW + MAX_LIFETIME + CLOCK_SKEW + 10));
    BOOST_CHECK_NE(Refused(far).find("more than a day"), std::string::npos);
    std::string zero{Buy()};
    zero.replace(zero.find("exp=") + 4, 10, "0123456789");
    BOOST_CHECK_NE(Refused(zero).find("exp"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(ids_amounts_and_texts_are_exact)
{
    const auto with{[](const std::string& key, const std::string& value) {
        std::string uri{Buy()};
        const size_t at{uri.find("&" + key + "=") + key.size() + 2};
        uri.replace(at, uri.find('&', at) == std::string::npos ? std::string::npos : uri.find('&', at) - at, value);
        return Refused(uri);
    }};
    // Ids: 64 lowercase hex digits.
    BOOST_CHECK_NE(with("asset", TOKEN_ID.substr(1)), "");
    BOOST_CHECK_NE(with("asset", "0x" + TOKEN_ID.substr(2)), "");
    BOOST_CHECK_NE(with("asset", "A" + TOKEN_ID.substr(1)), "");
    // Amounts: decimals of at most 8 places, no sign, no exponent, not 0, not overflowing.
    for (const std::string& bad : {"-1", "1e3", "1.", ".5", "01", "1.123456789", "0", "0.00000000", "1234567890123456", "NaN", "0x10", "1%2C5"}) {
        BOOST_CHECK_MESSAGE(with("ecx", bad) != "", bad);
    }
    BOOST_CHECK_EQUAL(with("ecx", "999999999999999.12345678"), "");
    BOOST_CHECK_EQUAL(with("min_tokens", "0.00000001"), "");
    // Tickers.
    BOOST_CHECK_NE(with("launch", "tide"), "");
    BOOST_CHECK_NE(with("launch", "T"), "");
    BOOST_CHECK_NE(with("launch", "ABCDEFGHIJKLM"), "");
    BOOST_CHECK_NE(with("launch", "ECX"), "");
    BOOST_CHECK_NE(with("slippage_bps", "5001"), "");
    BOOST_CHECK_NE(with("slippage_bps", "1.5"), "");
    BOOST_CHECK_NE(with("slippage_bps", "-1"), "");

    // Texts: no control or hidden characters, bounded sizes.
    const auto launch{[](const std::string& extra) {
        return Refused("flush:launch?v=1&network=regtest&exp=" + Exp() + "&ticker=LUMEN&name=Lumen" + extra);
    }};
    BOOST_CHECK_EQUAL(launch(""), "");
    BOOST_CHECK_NE(launch("&desc=a%00b"), "");
    BOOST_CHECK_NE(launch("&desc=a%1Bb"), "");
    BOOST_CHECK_NE(launch("&desc=%E2%80%AEevil"), "");  // U+202E
    BOOST_CHECK_NE(launch("&desc=%C0%AF"), "");         // overlong
    BOOST_CHECK_NE(launch("&desc=" + std::string(MAX_DESCRIPTION + 1, 'a')), "");
    BOOST_CHECK_NE(launch("&link=http%3A%2F%2Fa.example"), "");
    BOOST_CHECK_NE(launch("&link=https%3A%2F%2Fuser%40a.example"), "");
    BOOST_CHECK_NE(launch("&link=javascript%3Aalert(1)"), "");
    BOOST_CHECK_NE(launch("&link=https%3A%2F%2Fa.example%2F%3Cscript%3E"), "");
    BOOST_CHECK_NE(launch("&logo=sha256%3A" + std::string(63, 'a')), "");
    BOOST_CHECK_EQUAL(launch("&logo=sha256%3A" + std::string(64, 'a')), "");
    BOOST_CHECK_NE(launch("&ecx=1"), "");   // a first buy without its floor
    BOOST_CHECK_NE(launch("&min_tokens=1"), "");
    // A slippage without a first buy binds nothing: ignored, so that the links of the public API of 7.0.0 (which wrote
    // slippage_bps=100 on every launch) still run; a bad one is still refused.
    BOOST_CHECK_EQUAL(launch("&slippage_bps=100"), "");
    BOOST_CHECK(!Parse("flush:launch?v=1&network=regtest&exp=" + Exp() + "&ticker=LUMEN&name=Lumen&supply=1000000000&decimals=6&fee_bps=115&slippage_bps=100", NET, NOW).slippage_bps);
    BOOST_CHECK_NE(launch("&slippage_bps=99999"), "");
    std::string links;
    for (int i{0}; i < 5; ++i) links += "&link=https%3A%2F%2Fa.example";
    BOOST_CHECK_NE(launch(links), "");
    // The chain keeps 512 bytes for the description and the links together.
    BOOST_CHECK_NE(launch("&desc=" + std::string(500, 'a') + "&link=https%3A%2F%2Fa.example"), "");
}

BOOST_AUTO_TEST_CASE(connect_answers_flush_markets_only)
{
    const auto connect{[](const std::string& origin, const std::string& callback, const std::string& nonce) {
        return Refused("flush:connect?v=1&network=regtest&exp=" + Exp() + "&origin=" + origin + "&callback=" + callback + "&nonce=" + nonce);
    }};
    const std::string origin{"https%3A%2F%2Fflush.markets"}, callback{"https%3A%2F%2Fflush.markets%2Fconnect"}, nonce{"0123456789abcdef01234567"};
    BOOST_CHECK_EQUAL(connect(origin, callback, nonce), "");
    BOOST_CHECK_NE(connect("https%3A%2F%2Fflush.market", callback, nonce), "");
    BOOST_CHECK_NE(connect(origin, "https%3A%2F%2Fevil.example%2Fconnect", nonce), "");
    BOOST_CHECK_NE(connect(origin, "https%3A%2F%2Fflush.markets%2Fconnect%2F..%2Fx", nonce), "");
    BOOST_CHECK_NE(connect(origin, "https%3A%2F%2Fflush.markets.evil.example%2Fconnect", nonce), "");
    BOOST_CHECK_NE(connect(origin, "http%3A%2F%2F127.0.0.1%2Fconnect", nonce), "");
    BOOST_CHECK_NE(connect(origin, callback, "short"), "");
    BOOST_CHECK_NE(connect(origin, callback, "0123456789abcdef%26address%3Devil"), "");
    BOOST_CHECK_NE(connect(origin, callback, std::string(MAX_NONCE + 1, 'a')), "");
    // The answer: nothing but checked text in it.
    Request r{Parse("flush:connect?v=1&network=regtest&exp=" + Exp() + "&origin=" + origin + "&callback=" + callback + "&nonce=" + nonce, NET, NOW)};
    BOOST_CHECK_THROW(ConnectAnswer(r, "rflx1q&x=evil"), LinkError);
    BOOST_CHECK_THROW(ConnectAnswer(r, "RFLX1QABCDEFGH"), LinkError);
}

BOOST_AUTO_TEST_CASE(units_of_amounts)
{
    BOOST_CHECK_EQUAL(*Units("5", 8, Rounding::EXACT), 500000000U);
    BOOST_CHECK_EQUAL(*Units("1.5", 6, Rounding::EXACT), 1500000U);
    BOOST_CHECK(!Units("1.1234567", 6, Rounding::EXACT));
    BOOST_CHECK_EQUAL(*Units("1.12345600", 6, Rounding::EXACT), 1123456U);
    // A floor with more places than its asset has is rounded up, never down.
    BOOST_CHECK_EQUAL(*Units("1.1234561", 6, Rounding::UP), 1123457U);
    BOOST_CHECK_EQUAL(*Units("1.12345600", 6, Rounding::UP), 1123456U);
    BOOST_CHECK_EQUAL(*Units("7", 0, Rounding::UP), 7U);
    BOOST_CHECK_EQUAL(*Units("0.1", 0, Rounding::UP), 1U);
    // Past 2^64: refused, never wrapped.
    BOOST_CHECK(!Units("999999999999999", 8, Rounding::EXACT));
    BOOST_CHECK(!Units("10", 8, Rounding::EXACT, 999999999));
    BOOST_CHECK_EQUAL(Decimal(150, 2), "1.5");
    BOOST_CHECK_EQUAL(Decimal(100000000, 8), "1");
    BOOST_CHECK_EQUAL(Decimal(1, 8), "0.00000001");
    BOOST_CHECK_EQUAL(Decimal(12, 0), "12");
}

BOOST_AUTO_TEST_CASE(launch_files)
{
    const std::vector<unsigned char> logo{ParseHex(LOGO_PNG_PALETTE)};
    unsigned char digest[CSHA256::OUTPUT_SIZE];
    CSHA256().Write(logo.data(), logo.size()).Finalize(digest);
    const std::string hash{HexStr(digest)};
    const auto file{[&](const std::string& extra, const std::string& sha = "", const std::string& network = "regtest") {
        return strprintf(R"({"format":"flush-launch","version":1,"network":"%s","ticker":"LUMEN","name":"Lumen Works","description":"Light.","links":["https://lumen.example"],)"
                         R"("logo":{"type":"image/png","sha256":"%s","base64":"%s"},"first_buy":{"ecx":"3","min_tokens":"1000","slippage_bps":100}%s})",
                         network, sha.empty() ? hash : sha, EncodeBase64(logo), extra);
    }};
    const Request r{ParseLaunchFile(file(""), NET)};
    BOOST_CHECK(r.verb == Verb::LAUNCH);
    BOOST_CHECK(r.logo == logo);
    BOOST_CHECK_EQUAL(r.amount, "3");
    BOOST_CHECK_EQUAL(r.floor, "1000");
    // A logo whose bytes are not those of its hash, another network, an unknown key, a key twice, amounts as numbers.
    BOOST_CHECK_THROW(ParseLaunchFile(file("", std::string(64, 'a')), NET), LinkError);
    BOOST_CHECK_THROW(ParseLaunchFile(file("", "", "ecash"), NET), LinkError);
    BOOST_CHECK_THROW(ParseLaunchFile(file(R"(,"creator":"me")"), NET), LinkError);
    BOOST_CHECK_THROW(ParseLaunchFile(file(R"(,"ticker":"OTHER")"), NET), LinkError);
    BOOST_CHECK_THROW(ParseLaunchFile(R"({"format":"flush-launch","version":1,"network":"regtest","ticker":"LUMEN","name":"L","first_buy":{"ecx":3,"min_tokens":"1"}})", NET), LinkError);
    BOOST_CHECK_THROW(ParseLaunchFile(R"({"format":"flush-launch","version":2,"network":"regtest","ticker":"LUMEN","name":"L"})", NET), LinkError);
    BOOST_CHECK_THROW(ParseLaunchFile("[1,2]", NET), LinkError);
    BOOST_CHECK_THROW(ParseLaunchFile(std::string(MAX_LAUNCH_FILE + 1, ' '), NET), LinkError);
    // Without a logo or a first buy.
    const Request plain{ParseLaunchFile(R"({"format":"flush-launch","version":1,"network":"regtest","ticker":"LUMEN","name":"Lumen","description":"","links":[],"logo":null,"first_buy":null})", NET)};
    BOOST_CHECK(plain.logo.empty() && plain.amount.empty() && !plain.logo_sha256);
    BOOST_CHECK(plain.split.empty() && !plain.keep_control);
    // A launchpad's split and keep_control, as a link's split= and keep_control=1.
    const std::string pad{"bflx1qknwc098ye4cjncehs8q9m79ltkg0d2kr0kezke"};
    const Request split{ParseLaunchFile(file(strprintf(R"(,"split":[{"to":"creator","bps":7500},{"to":"%s","bps":2500}],"keep_control":true)", pad)), NET)};
    BOOST_REQUIRE_EQUAL(split.split.size(), 2U);
    BOOST_CHECK(split.split[0] == std::make_pair(std::string{"creator"}, 7500));
    BOOST_CHECK(split.split[1] == std::make_pair(pad, 2500));
    BOOST_CHECK(split.keep_control);
    BOOST_CHECK(ParseLaunchFile(file(R"(,"split":null,"keep_control":false)"), NET).split.empty());
    // Refused: bps as text, out of range, an unknown key, a recipient that is no recipient, more shares than a launch has, not a list.
    BOOST_CHECK_THROW(ParseLaunchFile(file(R"(,"split":[{"to":"creator","bps":"7500"}])"), NET), LinkError);
    BOOST_CHECK_THROW(ParseLaunchFile(file(R"(,"split":[{"to":"creator","bps":0}])"), NET), LinkError);
    BOOST_CHECK_THROW(ParseLaunchFile(file(R"(,"split":[{"to":"creator","bps":10001}])"), NET), LinkError);
    BOOST_CHECK_THROW(ParseLaunchFile(file(R"(,"split":[{"to":"creator","bps":7500,"memo":"x"}])"), NET), LinkError);
    BOOST_CHECK_THROW(ParseLaunchFile(file(R"(,"split":[{"to":"Creator!","bps":7500}])"), NET), LinkError);
    BOOST_CHECK_THROW(ParseLaunchFile(file(R"(,"split":[{"to":"","bps":7500}])"), NET), LinkError);
    std::string many;
    for (size_t i{0}; i <= bitassets::MAX_FEE_SHARES; ++i) many += strprintf(R"(%s{"to":"burn","bps":1})", i ? "," : "");
    BOOST_CHECK_THROW(ParseLaunchFile(file(R"(,"split":[)" + many + "]"), NET), LinkError);
    BOOST_CHECK_THROW(ParseLaunchFile(file(R"(,"split":"creator:10000")"), NET), LinkError);
    BOOST_CHECK_THROW(ParseLaunchFile(file(R"(,"keep_control":1)"), NET), LinkError);
}

namespace {
/** A chain to read from: one launched token TIDE (6 places), one graduated GRAD, a pool of ECX and GOLD (2 places). */
struct FakeReader : Reader {
    uint64_t launch_quote{1'000'000'000};  // 1000 TIDE
    uint64_t pool_quote{5'000};            // 50.00 GOLD
    std::optional<Asset> AssetById(const uint256& id) override
    {
        if (id.GetHex() == TOKEN_ID) return Asset{id, "TIDE", "Tide Pool", 6, 7};
        if (id.GetHex() == GOLD_ID) return Asset{id, "GOLD", "", 2, 3};
        if (id.GetHex() == std::string(64, 'd')) return Asset{id, "GRAD", "", 6, 9};
        return std::nullopt;
    }
    bool NameTaken(const std::string& name) override { return name == "TIDE" || name == "GOLD"; }
    std::optional<Launch> LaunchOf(const uint256& token) override
    {
        if (token.GetHex() == TOKEN_ID) return Launch{uint256::ONE, false};
        if (token.GetHex() == std::string(64, 'd')) return Launch{uint256::ONE, true};
        return std::nullopt;
    }
    uint64_t QuoteLaunch(const uint256&, bool buy, uint64_t) override { return buy ? launch_quote : 2'000'000; }
    uint64_t QuoteFirstBuy(uint64_t, uint64_t supply = 0, uint32_t fee = 0) override
    {
        first_buy_supply = supply;
        first_buy_fee = fee;
        return 5'000'000'000;
    }
    uint64_t first_buy_supply{0};
    uint32_t first_buy_fee{0};
    bool params_active{true};
    bool LaunchParamsActive() override { return params_active; }
    bool fee_flow{true};
    bool FeeFlowActive() override { return fee_flow; }
    uint32_t LaunchFee() override { return 11'500; }
    std::optional<Pool> PoolById(const uint256& id) override
    {
        if (id.GetHex() == POOL_ID) return Pool{uint256::ZERO, *uint256::FromHex(GOLD_ID), false};
        // A second pool, of GOLD and TIDE (a route ECX → GOLD → TIDE).
        if (id.GetHex() == POOL2_ID) return Pool{*uint256::FromHex(TOKEN_ID), *uint256::FromHex(GOLD_ID), false};
        return std::nullopt;
    }
    std::vector<std::string> reads;
    UniValue Read(const std::string& method, const UniValue& params) override
    {
        reads.push_back(method + params.write());
        UniValue r{UniValue::VOBJ};
        if (method == "quotepoolliquidity") {
            r.read(params.size() > 2 ? R"({"shares":500,"amounts":{"ECX":0.5,"GOLD":12.34}})" : R"({"shares":1000,"amounts":{"ECX":1,"GOLD":20}})");
        } else if (method == "previewposition") {
            r.read(R"({"in_range":true,"liquidity":"12345"})");
        } else if (method == "getposition") {
            r.read(R"({"pool":")" + POOL_ID + R"(","asset0":"ECX","asset1":"GOLD","fees0":0.1,"fees1":2.5})");
        } else if (method == "getorder") {
            r.read(R"({"sells":"GOLD","buys":"ECX","remaining":5,"to_collect":0.2})");
        } else if (method == "getauction") {
            r.read(R"({"base":"GOLD","quote":"ECX","price":0.5,"remaining":100,"status":"open"})");
        } else if (method == "getlaunch") {
            r.read(R"({"creator_fees_owed":0.25,"allocation":5,"allocation_claimed":false,"status":"graduated"})");
        }
        return r;
    }
    uint64_t QuotePool(const uint256&, const uint256&, uint64_t) override { return pool_quote; }
    bool LaunchesActive() override { return true; }
    uint64_t min_payout{50'000};
    uint64_t MinChnPayout() override { return min_payout; }
    std::optional<int> RevealWait(const uint256&) override { return 0; }
};

std::string ResolveRefused(const std::string& uri, FakeReader& reader)
{
    try {
        Resolve(Parse(uri, NET, NOW), Source::BROWSER, reader, NOW);
    } catch (const LinkError& e) {
        return e.what();
    }
    return "";
}

bool HasRow(const std::vector<webui::dialog::Row>& rows, const std::string& label, const std::string& part)
{
    return std::any_of(rows.begin(), rows.end(), [&](const auto& r) { return r.label == label && r.value.find(part) != std::string::npos; });
}
} // namespace

BOOST_AUTO_TEST_CASE(buy_sell_swap_resolve_by_id_with_a_fresh_quote)
{
    FakeReader reader;
    // A buy: the token by its id, the floor from the link, the slippage that makes the node's floor exactly the link's.
    Plan buy{Resolve(Parse(Buy(), NET, NOW), Source::BROWSER, reader, NOW)};
    BOOST_CHECK_EQUAL(buy.method, "buyfromlaunch");
    BOOST_CHECK_EQUAL(buy.params.write(), R"(["0x)" + TOKEN_ID + R"(","5",10.00,"900"])");
    BOOST_CHECK(HasRow(buy.preface, "From a link", "opened in your browser"));
    BOOST_CHECK(HasRow(buy.preface, "It asks", "Buy TIDE with 5 ECX"));
    BOOST_CHECK(HasRow(buy.preface, "Asset", "TIDE — Tide Pool (0x1111aaaa…"));
    BOOST_CHECK(HasRow(buy.preface, "Quote now", "1000 TIDE"));
    BOOST_CHECK(HasRow(buy.preface, "Least it gives", "900 TIDE"));
    BOOST_CHECK(HasRow(buy.preface, "Slippage the site showed", "1.00 %"));
    BOOST_CHECK(buy.preface.back().kind == webui::dialog::Row::Kind::NOTE);

    // The curve moved below the floor: refused before anything is asked.
    reader.launch_quote = 899'999'999;
    BOOST_CHECK_NE(ResolveRefused(Buy(), reader).find("The curve moved"), std::string::npos);
    reader.launch_quote = 1'000'000'000;
    // A ticker that is not the token's (a ticker taken again), an id nobody has, a graduated launch.
    std::string other{Buy()};
    other.replace(other.find("launch=TIDE"), 11, "launch=TIDX");
    BOOST_CHECK_NE(ResolveRefused(other, reader).find("must be of one asset"), std::string::npos);
    std::string nobody{Buy()};
    nobody.replace(nobody.find(TOKEN_ID), 64, std::string(64, 'e'));
    BOOST_CHECK_NE(ResolveRefused(nobody, reader).find("No asset has the id"), std::string::npos);
    std::string graduated{"flush:buy?v=1&network=regtest&exp=" + Exp() + "&launch=GRAD&asset=" + std::string(64, 'd') + "&ecx=5&min_tokens=1"};
    BOOST_CHECK_NE(ResolveRefused(graduated, reader).find("graduated"), std::string::npos);
    // A buy of more places than ECX has is refused; a floor of more places than the token has goes up, never down.
    std::string places{Buy()};
    places.replace(places.find("min_tokens=900"), 14, "min_tokens=899.9999991");
    BOOST_CHECK_EQUAL(Resolve(Parse(places, NET, NOW), Source::BROWSER, reader, NOW).params[3].get_str(), "900");

    // A sale: tokens in exactly the token's places, the floor in ECX.
    Plan sell{Resolve(Parse("flush:sell?v=1&network=regtest&exp=" + Exp() + "&launch=TIDE&asset=" + TOKEN_ID + "&tokens=1000.5&min_ecx=0.01", NET, NOW), Source::PASTED, reader, NOW)};
    BOOST_CHECK_EQUAL(sell.method, "selltolaunch");
    BOOST_CHECK_EQUAL(sell.params[1].get_str(), "1000.5");
    BOOST_CHECK_EQUAL(sell.params[3].get_str(), "0.01");
    BOOST_CHECK(HasRow(sell.preface, "From a link", "pasted or dropped"));
    BOOST_CHECK_NE(ResolveRefused("flush:sell?v=1&network=regtest&exp=" + Exp() + "&launch=TIDE&asset=" + TOKEN_ID + "&tokens=1.0000001&min_ecx=0.01", reader).find("places"), std::string::npos);
    BOOST_CHECK_NE(ResolveRefused("flush:sell?v=1&network=regtest&exp=" + Exp() + "&launch=TIDE&asset=" + TOKEN_ID + "&tokens=1&min_ecx=0.03", reader).find("moved"), std::string::npos);

    // A swap: the pool by its id, its two assets by their tickers; swaproute with that one pool.
    const std::string swap{"flush:swap?v=1&network=regtest&exp=" + Exp() + "&pool=" + POOL_ID + "&from=ECX&to=GOLD&amount=2&min_out=49.5&slippage_bps=100"};
    Plan s{Resolve(Parse(swap, NET, NOW), Source::BROWSER, reader, NOW)};
    BOOST_CHECK_EQUAL(s.method, "swaproute");
    BOOST_CHECK_EQUAL(s.params.write(), R"([["ECX","0x)" + GOLD_ID + R"("],"2",1.00,"49.5",[")" + POOL_ID + R"("]])");
    BOOST_CHECK(HasRow(s.preface, "Gets", "GOLD (0xcccccccc…"));
    std::string substituted{swap};
    substituted.replace(substituted.find("to=GOLD"), 7, "to=TIDE");
    BOOST_CHECK_NE(ResolveRefused(substituted, reader).find("is not one of them"), std::string::npos);
    std::string no_pool{swap};
    no_pool.replace(no_pool.find(POOL_ID), 64, std::string(64, 'f'));
    BOOST_CHECK_NE(ResolveRefused(no_pool, reader).find("No pool"), std::string::npos);
    std::string too_fine{swap};
    too_fine.replace(too_fine.find("min_out=49.5"), 12, "min_out=50.001");
    BOOST_CHECK_NE(ResolveRefused(too_fine, reader).find("moved"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(launch_link_keeps_control)
{
    // 7.0.1: keep_control=1 asks to keep the control coin: in the launch's options, and shown loud in its one question.
    FakeReader reader;
    const std::string uri{"flush:launch?v=1&network=regtest&exp=" + Exp() + "&ticker=LUMEN&name=Lumen"};
    Plan kept{Resolve(Parse(uri + "&keep_control=1", NET, NOW), Source::BROWSER, reader, NOW)};
    BOOST_CHECK_EQUAL(kept.then_params.write(), R"(["LUMEN",{"display_name":"Lumen","keep_control":true}])");
    BOOST_CHECK(std::any_of(kept.preface.begin(), kept.preface.end(), [](const auto& r) { return r.kind == webui::dialog::Row::Kind::HEADING && r.label.starts_with("Unlocked Supply"); }));
    // Without it (or 0): not kept, nothing said.
    for (const std::string& extra : {std::string{}, std::string{"&keep_control=0"}}) {
        Plan plain{Resolve(Parse(uri + extra, NET, NOW), Source::BROWSER, reader, NOW)};
        BOOST_CHECK_EQUAL(plain.then_params.write(), R"(["LUMEN",{"display_name":"Lumen"}])");
    }
    BOOST_CHECK_THROW(Parse(uri + "&keep_control=yes", NET, NOW), LinkError);
    reader.fee_flow = false;
    BOOST_CHECK_NE(ResolveRefused(uri + "&keep_control=1", reader).find("control coin"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(launch_resolves_to_reserve_then_launch)
{
    FakeReader reader;
    const std::string uri{"flush:launch?v=1&network=regtest&exp=" + Exp() + "&ticker=LUMEN&name=Lumen%20Works&desc=Light.&link=https%3A%2F%2Flumen.example&ecx=3&min_tokens=4000&slippage_bps=100"};
    Plan plan{Resolve(Parse(uri, NET, NOW), Source::BROWSER, reader, NOW)};
    BOOST_CHECK_EQUAL(plan.method, "reserveasset");
    BOOST_CHECK_EQUAL(plan.params.write(), R"(["LUMEN"])");
    BOOST_CHECK_EQUAL(plan.then_method, "launchtoken");
    BOOST_CHECK_EQUAL(plan.then_params.write(), R"(["LUMEN",{"display_name":"Lumen Works","info":"Light.\nhttps://lumen.example","first_buy":"3","first_min_out":"4000"}])");
    BOOST_CHECK(HasRow(plan.preface, "Quote now", "5000 LUMEN"));
    // A ticker taken, a first buy whose floor the chain cannot meet.
    std::string taken{uri};
    taken.replace(taken.find("ticker=LUMEN"), 12, "ticker=GOLD");
    BOOST_CHECK_NE(ResolveRefused(taken, reader).find("taken"), std::string::npos);
    std::string greedy{uri};
    greedy.replace(greedy.find("min_tokens=4000"), 15, "min_tokens=6000");
    BOOST_CHECK_NE(ResolveRefused(greedy, reader).find("the site's quote is not the chain's"), std::string::npos);
    // A logo named by its hash: the bytes must be those.
    const std::vector<unsigned char> logo{ParseHex(LOGO_PNG_PALETTE)};
    unsigned char digest[CSHA256::OUTPUT_SIZE];
    CSHA256().Write(logo.data(), logo.size()).Finalize(digest);
    const std::string hash{HexStr(digest)};
    Request with_logo{Parse(uri + "&logo=sha256%3A" + hash, NET, NOW)};
    BOOST_CHECK_THROW(Resolve(with_logo, Source::BROWSER, reader, NOW), LinkError);
    with_logo.logo = logo;
    with_logo.logo.back() ^= 1;
    BOOST_CHECK_THROW(Resolve(with_logo, Source::BROWSER, reader, NOW), LinkError);
    with_logo.logo = logo;
    const Plan done{Resolve(with_logo, Source::BROWSER, reader, NOW)};
    BOOST_CHECK_EQUAL(done.then_params[1].find_value("logo").get_str(), EncodeBase64(logo));
    BOOST_CHECK(HasRow(done.preface, "Logo", hash.substr(0, 16)));
}

BOOST_AUTO_TEST_CASE(launch_with_parameters_and_the_new_verbs)
{
    FakeReader reader;
    const std::string head{"v=1&network=regtest&exp=" + Exp()};
    // A launch with its own supply, decimals, fee and allocation: launchtoken's options, the first buy quoted on its curve.
    const Plan launch{Resolve(Parse("flush:launch?" + head + "&ticker=WIDE&name=Wide&supply=21000000&decimals=8&fee_bps=200&allocation_bps=100&ecx=1&min_tokens=1", NET, NOW), Source::BROWSER, reader, NOW)};
    BOOST_CHECK_EQUAL(launch.then_params.write(), R"(["WIDE",{"display_name":"Wide","supply":"21000000","decimals":8,"fee":2.00,"allocation":1.00,"first_buy":"1","first_min_out":"1"}])");
    BOOST_CHECK_EQUAL(reader.first_buy_supply, 2'100'000'000'000'000ULL);
    BOOST_CHECK_EQUAL(reader.first_buy_fee, 20'000U);
    BOOST_CHECK(HasRow(launch.preface, "Curve's fee", "2.00 %"));
    // Refused: a fee out of the bounds, a supply that is not a curve's, decimals without a supply, before the rules apply.
    BOOST_CHECK_NE(Refused("flush:launch?" + head + "&ticker=WIDE&name=Wide&fee_bps=501").find("fee_bps"), std::string::npos);
    BOOST_CHECK_NE(Refused("flush:launch?" + head + "&ticker=WIDE&name=Wide&decimals=2").find("decimals go with a supply"), std::string::npos);
    BOOST_CHECK_NE(ResolveRefused("flush:launch?" + head + "&ticker=WIDE&name=Wide&supply=3&decimals=0", reader).find("multiple of 8"), std::string::npos);
    reader.params_active = false;
    BOOST_CHECK_NE(ResolveRefused("flush:launch?" + head + "&ticker=WIDE&name=Wide&fee_bps=200", reader).find("no launches with a supply"), std::string::npos);
    BOOST_CHECK_NE(ResolveRefused("flush:curve?" + head + "&asset=" + GOLD_ID + "&ticker=GOLD&amount=80000&fee_bps=100", reader).find("does not put"), std::string::npos);
    reader.params_active = true;

    // register: reserveasset then registerasset, the supply to the wallet.
    const Plan reg{Resolve(Parse("flush:register?" + head + "&ticker=BROSE&name=Bro%20Se&desc=Hi&supply=21000000&decimals=8", NET, NOW), Source::BROWSER, reader, NOW)};
    BOOST_CHECK_EQUAL(reg.method, "reserveasset");
    BOOST_CHECK_EQUAL(reg.then_method, "registerasset");
    BOOST_CHECK_EQUAL(reg.then_params.write(), R"(["BROSE","21000000",8,{"info":"Hi"},true,null,"Bro Se"])");
    BOOST_CHECK_NE(Refused("flush:register?" + head + "&ticker=BROSE&supply=21000000").find("decimals is missing"), std::string::npos);
    BOOST_CHECK_NE(Refused("flush:register?" + head + "&ticker=BROSE&supply=021&decimals=2").find("supply"), std::string::npos);
    BOOST_CHECK_NE(ResolveRefused("flush:register?" + head + "&ticker=GOLD&supply=1&decimals=0", reader).find("taken"), std::string::npos);

    // curve: the asset by id and ticker, the amount exactly in its places, a curve's supply.
    const Plan curve{Resolve(Parse("flush:curve?" + head + "&asset=" + GOLD_ID + "&ticker=GOLD&amount=80000&fee_bps=100&max_buy=10", NET, NOW), Source::BROWSER, reader, NOW)};
    BOOST_CHECK_EQUAL(curve.method, "curveasset");
    BOOST_CHECK_EQUAL(curve.params.write(), R"(["0x)" + GOLD_ID + R"(","80000",{"fee":1.00,"max_buy":"10"}])");
    BOOST_CHECK_NE(ResolveRefused("flush:curve?" + head + "&asset=" + GOLD_ID + "&ticker=TIDE&amount=80000&fee_bps=100", reader).find("one asset"), std::string::npos);
    BOOST_CHECK_NE(ResolveRefused("flush:curve?" + head + "&asset=" + TOKEN_ID + "&ticker=TIDE&amount=8000000&fee_bps=100", reader).find("already"), std::string::npos);
    BOOST_CHECK_NE(ResolveRefused("flush:curve?" + head + "&asset=" + GOLD_ID + "&ticker=GOLD&amount=80000.001&fee_bps=100", reader).find("places"), std::string::npos);

    // pool: both sides by id (ECX by its ticker), a fee tier, the kind's own parameter.
    const Plan pool{Resolve(Parse("flush:pool?" + head + "&asset_a=ECX&ticker_a=ECX&asset_b=" + GOLD_ID + "&ticker_b=GOLD&kind=weighted&fee_ppm=3000&amount_a=1&amount_b=100&weight_a=80", NET, NOW), Source::BROWSER, reader, NOW)};
    BOOST_CHECK_EQUAL(pool.method, "createpool");
    BOOST_CHECK_EQUAL(pool.params.write(), R"(["ECX","1","0x)" + GOLD_ID + R"(","100",{"curve":"weighted","fee":0.3000,"weight_a":80}])");
    BOOST_CHECK_NE(Refused("flush:pool?" + head + "&asset_a=ECX&ticker_a=ECX&asset_b=" + GOLD_ID + "&ticker_b=GOLD&kind=constant&fee_ppm=2000&amount_a=1&amount_b=100").find("fee tiers"), std::string::npos);
    BOOST_CHECK_NE(Refused("flush:pool?" + head + "&asset_a=ECX&ticker_a=GOLD&asset_b=" + GOLD_ID + "&ticker_b=GOLD&kind=constant&fee_ppm=3000&amount_a=1&amount_b=100").find("names the coin"), std::string::npos);
    BOOST_CHECK_NE(Refused("flush:pool?" + head + "&asset_a=ECX&ticker_a=ECX&asset_b=" + GOLD_ID + "&ticker_b=GOLD&kind=weighted&fee_ppm=3000&amount_a=1&amount_b=100").find("weight_a"), std::string::npos);

    // auction and order.
    const Plan auction{Resolve(Parse("flush:auction?" + head + "&base=" + GOLD_ID + "&base_ticker=GOLD&amount=10&quote=ECX&quote_ticker=ECX&start_price=5&end_price=1&duration=100", NET, NOW), Source::BROWSER, reader, NOW)};
    BOOST_CHECK_EQUAL(auction.method, "createauction");
    BOOST_CHECK_EQUAL(auction.params.write(), R"(["0x)" + GOLD_ID + R"(","10","ECX","5","1",100,1])");
    BOOST_CHECK_NE(ResolveRefused("flush:auction?" + head + "&base=" + GOLD_ID + "&base_ticker=GOLD&amount=10&quote=ECX&quote_ticker=ECX&start_price=1&end_price=5&duration=100", reader).find("falls"), std::string::npos);
    const Plan order{Resolve(Parse("flush:order?" + head + "&sell=" + GOLD_ID + "&sell_ticker=GOLD&buy=ECX&buy_ticker=ECX&amount=5&price=0.25&expiry_blocks=1000", NET, NOW), Source::BROWSER, reader, NOW)};
    BOOST_CHECK_EQUAL(order.method, "placeorder");
    BOOST_CHECK_EQUAL(order.params.write(), R"(["0x)" + GOLD_ID + R"(","5","ECX","0.25",1000])");
    BOOST_CHECK_NE(Refused("flush:order?" + head + "&sell=ECX&sell_ticker=ECX&buy=ECX&buy_ticker=ECX&amount=5&price=1&expiry_blocks=10").find("the same"), std::string::npos);
    // A parameter of another verb is refused, as ever.
    BOOST_CHECK_NE(Refused("flush:order?" + head + "&sell=ECX&sell_ticker=ECX&buy=" + GOLD_ID + "&buy_ticker=GOLD&amount=5&price=1&expiry_blocks=10&fee_bps=100").find("not a parameter"), std::string::npos);
    // A launch file with the launch's parameters.
    const Request file{ParseLaunchFile(R"({"format":"flush-launch","version":1,"network":"regtest","ticker":"LUMEN","name":"Lumen","description":"","links":[],"logo":null,"first_buy":null,"supply":"21000000","decimals":8,"fee_bps":115,"allocation_bps":0})", NET)};
    BOOST_CHECK_EQUAL(file.supply, "21000000");
    BOOST_CHECK_EQUAL(*file.decimals, 8);
    BOOST_CHECK_EQUAL(*file.fee_bps, 115);
}

namespace {
/** The host of the links over a node, with the confirmed RPC methods described as the app does and run by nobody. */
struct LinkHostSetup : TestChain100Setup {
    std::unique_ptr<interfaces::WalletLoader> loader{interfaces::MakeWalletLoader(*m_node.chain, *Assert(m_node.args))};
    std::unique_ptr<interfaces::Node> node;
    std::vector<webui::Window::ConfirmDialog> asked;
    std::vector<std::string> ran;
    std::vector<std::string> opened;
    std::vector<std::string> informed;
    std::vector<std::string> copied;
    bool accept{true};
    //! Whether the browser opens (none under WSL without a handler, 7.0.1).
    bool open_ok{true};
    //! While set, the host waits for the node (a link being handled for a long time).
    std::atomic<bool> hold{false};
    std::unique_ptr<webui::confirm::ConfirmedRpc> confirmed;
    std::unique_ptr<LinkHost> host;
    LinkHostSetup()
    {
        m_node.wallet_loader = loader.get();
        loader->registerRpcs();
        SetRPCWarmupFinished();
        node = interfaces::MakeNode(m_node);
        confirmed = std::make_unique<webui::confirm::ConfirmedRpc>(
            [this]() -> interfaces::Node& { return *node; }, [this](const webui::Window::ConfirmDialog& d) {
                asked.push_back(d);
                return webui::dialog::Answer{accept, {}};
            },
            [this](const std::string& wallet, webui::Window::ConfirmDialog dialog, bool, bool, const std::function<UniValue()>&) -> UniValue {
                asked.push_back(dialog);
                if (!accept) throw webui::HostError(webui::BRIDGE_CANCELLED, "declined");
                ran.push_back(wallet + ":" + dialog.kind);
                UniValue out{UniValue::VOBJ};
                out.pushKV("txid", std::string(64, '9'));
                return out;
            });
        std::map<std::string, webui::confirm::Method> methods;
        for (const char* name : {"buyfromlaunch", "selltolaunch", "swaproute", "reserveasset", "launchtoken"}) {
            methods[name] = {webui::confirm::Method::Kind::SPEND, webui::Bridge::Thread::LONG, webui::confirm::Generic(name, "Do", "It does.")};
        }
        confirmed->SetMethodsForTest(methods);
        LinkHost::Deps deps;
        deps.node = [this]() -> interfaces::Node& {
            while (hold) std::this_thread::sleep_for(std::chrono::milliseconds{10});
            return *node;
        };
        deps.confirmed = [this](const UniValue& call) { return confirmed->Call(call); };
        deps.ask = [this](const webui::Window::ConfirmDialog& d) {
            asked.push_back(d);
            return webui::dialog::Answer{accept, {}};
        };
        deps.open = [this](const std::string& url) {
            opened.push_back(url);
            return open_ok;
        };
        deps.copy = [this](const std::string& text) { copied.push_back(text); };
        deps.inform = [this](const std::string& title, const std::string& message, bool) { informed.push_back(title + ": " + message); };
        deps.wallet = [] { return std::string{}; };
        deps.network = [] { return NET; };
        deps.now = [] { return NOW; };
        deps.reader = [](interfaces::Node&) { return std::make_unique<FakeReader>(); };
        host = std::make_unique<LinkHost>(std::move(deps));
    }
    void CreateWallet()
    {
        UniValue args{UniValue::VARR};
        args.push_back("w");
        try {
            node->executeRpc("createwallet", args, "/");
        } catch (const UniValue& e) {
            BOOST_FAIL("createwallet: " + e.write());
        }
    }
    ~LinkHostSetup()
    {
        host->Stop();
        for (auto& w : loader->getWallets()) w->remove();
        m_node.validation_signals->SyncWithValidationInterfaceQueue();
        loader->stop();
        m_node.wallet_loader = nullptr;
    }
};
} // namespace

BOOST_FIXTURE_TEST_CASE(each_verb_asks_in_the_hosts_dialog_where_it_comes_from, LinkHostSetup)
{
    // Without a wallet, nothing is asked.
    BOOST_CHECK_EXCEPTION(host->Handle(Buy(), Source::BROWSER), LinkError, [](const LinkError& e) { return std::string{e.what()}.find("No wallet") != std::string::npos; });
    BOOST_CHECK(asked.empty());
    CreateWallet();

    // A buy: one question, its preface first, then the describer's rows; the call ran once.
    host->Handle(Buy(), Source::BROWSER);
    BOOST_REQUIRE_EQUAL(asked.size(), 1U);
    const auto& q{asked.back()};
    BOOST_CHECK_EQUAL(q.rows.front().label, "From a link");
    BOOST_CHECK(HasRow(q.rows, "It asks", "Buy TIDE with 5 ECX"));
    BOOST_CHECK(HasRow(q.rows, "Least it gives", "900 TIDE"));
    BOOST_CHECK(HasRow(q.rows, "Command", "buyfromlaunch"));
    BOOST_CHECK(HasRow(q.rows, "Wallet", "w"));
    BOOST_CHECK_NE(q.message.find("A link (opened in your browser — check it) asks the app for this"), std::string::npos);
    BOOST_CHECK_EQUAL(q.kind, "link:rpc:buyfromlaunch");
    BOOST_CHECK(q.level >= webui::dialog::Level::WARNING);
    BOOST_CHECK_EQUAL(ran.size(), 1U);
    // The same link again: refused, nothing asked.
    BOOST_CHECK_EXCEPTION(host->Handle(Buy(), Source::BROWSER), LinkError, [](const LinkError& e) { return std::string{e.what()}.find("used already") != std::string::npos; });
    BOOST_CHECK_EQUAL(asked.size(), 1U);

    // A sale and a swap.
    host->Handle("flush:sell?v=1&network=regtest&exp=" + Exp() + "&launch=TIDE&asset=" + TOKEN_ID + "&tokens=10&min_ecx=0.01", Source::PASTED);
    BOOST_CHECK(HasRow(asked.back().rows, "It asks", "Sell 10 TIDE"));
    BOOST_CHECK(HasRow(asked.back().rows, "Command", "selltolaunch"));
    host->Handle("flush:swap?v=1&network=regtest&exp=" + Exp() + "&pool=" + POOL_ID + "&from=ECX&to=GOLD&amount=2&min_out=49", Source::BROWSER);
    BOOST_CHECK(HasRow(asked.back().rows, "It asks", "Swap 2 ECX for GOLD"));
    BOOST_CHECK(HasRow(asked.back().rows, "Command", "swaproute"));

    // A decline: nothing ran, and the link is not used up.
    accept = false;
    const std::string swap2{"flush:swap?v=1&network=regtest&exp=" + Exp() + "&pool=" + POOL_ID + "&from=ECX&to=GOLD&amount=3&min_out=49"};
    BOOST_CHECK_THROW(host->Handle(swap2, Source::BROWSER), webui::HostError);
    BOOST_CHECK_EQUAL(ran.size(), 3U);
    accept = true;
    host->Handle(swap2, Source::BROWSER);
    BOOST_CHECK_EQUAL(ran.size(), 4U);

    // Another network: refused with what it is, nothing asked.
    const size_t before{asked.size()};
    BOOST_CHECK_EXCEPTION(host->Handle(Buy("", "ecash"), Source::BROWSER), LinkError, [](const LinkError& e) { return std::string{e.what()}.find("this app runs regtest") != std::string::npos; });
    BOOST_CHECK_EQUAL(asked.size(), before);
}

BOOST_FIXTURE_TEST_CASE(launch_asks_once_for_both_steps_then_sends_the_launch, LinkHostSetup)
{
    CreateWallet();
    // The test methods have no second step: reserveasset as the app has it (its launch reveal) is checked in the e2e test;
    // here the plan is refused for lack of it, and nothing runs.
    const std::string uri{"flush:launch?v=1&network=regtest&exp=" + Exp() + "&ticker=LUMEN&name=Lumen"};
    BOOST_CHECK_THROW(host->Handle(uri, Source::BROWSER), webui::HostError);
    BOOST_CHECK(ran.empty());
    // A logo named by its hash needs the file: none chosen (no chooser), nothing done.
    BOOST_CHECK_THROW(host->Handle(uri + "&logo=sha256%3A" + std::string(64, 'a'), Source::BROWSER), webui::HostError);
    BOOST_CHECK(ran.empty());
}

BOOST_FIXTURE_TEST_CASE(connect_shares_one_address_with_the_one_callback_once, LinkHostSetup)
{
    CreateWallet();
    const std::string uri{"flush:connect?v=1&network=regtest&exp=" + Exp() + "&origin=https%3A%2F%2Fflush.markets&callback=https%3A%2F%2Fflush.markets%2Fconnect&nonce=0123456789abcdef01234567"};
    // Declined: nothing opened.
    accept = false;
    BOOST_CHECK_THROW(host->Handle(uri, Source::BROWSER), webui::HostError);
    BOOST_CHECK(opened.empty());
    BOOST_REQUIRE_EQUAL(asked.size(), 1U);
    const auto& q{asked.front()};
    BOOST_CHECK_EQUAL(q.title, "Share an address with flush.markets");
    BOOST_CHECK(HasRow(q.rows, "Site", "https://flush.markets"));
    BOOST_CHECK(HasRow(q.rows, "The answer goes to", "https://flush.markets/connect"));
    const auto address{std::find_if(q.rows.begin(), q.rows.end(), [](const auto& r) { return r.label == "Address shared"; })};
    BOOST_REQUIRE(address != q.rows.end());
    BOOST_CHECK(address->kind == webui::dialog::Row::Kind::ADDRESS);
    // Accepted: the browser opens the callback with that address, the nonce and the network.
    accept = true;
    const UniValue result{host->Handle(uri, Source::BROWSER)};
    BOOST_REQUIRE_EQUAL(opened.size(), 1U);
    BOOST_CHECK_EQUAL(opened[0], "https://flush.markets/connect#address=" + address->value + "&nonce=0123456789abcdef01234567&network=regtest");
    // The same address the next time (its label), and the nonce once.
    BOOST_CHECK_THROW(host->Handle(uri, Source::BROWSER), LinkError);
    host->Handle("flush:connect?v=1&network=regtest&exp=" + Exp() + "&origin=https%3A%2F%2Fflush.markets&callback=https%3A%2F%2Fflush.markets%2Fconnect&nonce=1123456789abcdef01234567", Source::BROWSER);
    BOOST_REQUIRE_EQUAL(opened.size(), 2U);
    BOOST_CHECK_NE(opened[1].find("address=" + address->value + "&"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(connect_shares_the_wallets_addresses)
{
    // 7.0.1: the primary first, then the wallet's addresses that hold something, each once, at most MAX_CONNECT_ADDRESSES.
    bool truncated{true};
    const auto list{ConnectAddresses("rflx1qprimary0", {"rflx1qbbbbbbbb", "rflx1qprimary0", "rflx1qaaaaaaaa", "rflx1qbbbbbbbb"}, truncated)};
    BOOST_CHECK(list == (std::vector<std::string>{"rflx1qprimary0", "rflx1qbbbbbbbb", "rflx1qaaaaaaaa"}));
    BOOST_CHECK(!truncated);
    std::vector<std::string> many;
    for (int i{0}; i < 250; ++i) many.push_back(strprintf("rflx1q%08d", i));
    const auto capped{ConnectAddresses("rflx1qprimary0", many, truncated)};
    BOOST_CHECK_EQUAL(capped.size(), MAX_CONNECT_ADDRESSES);
    BOOST_CHECK(truncated);
    // The fragment: address, the list, truncated, then the nonce and the network.
    const std::string nonce{"0123456789abcdef01234567"};
    const Request r{Parse("flush:connect?v=1&network=regtest&exp=" + Exp() + "&origin=https%3A%2F%2Fflush.markets&callback=https%3A%2F%2Fflush.markets%2Fconnect&nonce=" + nonce, NET, NOW)};
    BOOST_CHECK_EQUAL(ConnectAnswer(r, "rflx1qprimary0", list), "https://flush.markets/connect#address=rflx1qprimary0&addresses=rflx1qprimary0,rflx1qbbbbbbbb,rflx1qaaaaaaaa&nonce=" + nonce + "&network=regtest");
    BOOST_CHECK_NE(ConnectAnswer(r, "rflx1qprimary0", capped, true).find("&truncated=1&nonce="), std::string::npos);
    BOOST_CHECK_EQUAL(ConnectAnswer(r, "rflx1qprimary0"), "https://flush.markets/connect#address=rflx1qprimary0&nonce=" + nonce + "&network=regtest");
    BOOST_CHECK_THROW(ConnectAnswer(r, "rflx1qprimary0", {"rflx1qprimary0", "rflx1q&x=evil"}), LinkError);
    many.push_back("rflx1qprimary0");
    BOOST_CHECK_THROW(ConnectAnswer(r, "rflx1qprimary0", many), LinkError);
    // The question: the wallet named in the title and first, every address listed, the note that the site links them.
    const auto q{webui::links::ConnectAllQuestion(r, Source::BROWSER, "validator", list, false, NOW)};
    BOOST_CHECK_EQUAL(q.title, "Share 3 addresses of wallet validator");
    BOOST_CHECK_EQUAL(q.accept, "Share all 3");
    BOOST_CHECK(HasRow(q.rows, "Wallet", "validator"));
    size_t shown{0};
    for (const auto& row : q.rows) shown += row.kind == webui::dialog::Row::Kind::ADDRESS;
    BOOST_CHECK_EQUAL(shown, 3U);
    BOOST_CHECK_NE(q.message.find("validator"), std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(connect_without_a_browser_shows_what_to_copy, LinkHostSetup)
{
    // No browser could be opened (7.0.1, WSL): the user is shown the address to paste on the site and the link, and
    // copies the address; not a bare failure.
    CreateWallet();
    open_ok = false;
    const UniValue result{host->Handle("flush:connect?v=1&network=regtest&exp=" + Exp() + "&origin=https%3A%2F%2Fflush.markets&callback=https%3A%2F%2Fflush.markets%2Fconnect&nonce=2123456789abcdef01234567", Source::BROWSER)};
    BOOST_REQUIRE_EQUAL(opened.size(), 1U);
    BOOST_REQUIRE_EQUAL(asked.size(), 2U);
    const auto& copy{asked.back()};
    BOOST_CHECK_EQUAL(copy.title, "Paste it on the site");
    BOOST_CHECK_EQUAL(copy.details, opened[0]);
    BOOST_REQUIRE_EQUAL(copy.rows.size(), 1U);
    BOOST_CHECK(copy.rows[0].kind == webui::dialog::Row::Kind::ADDRESS);
    BOOST_REQUIRE_EQUAL(copied.size(), 1U);
    BOOST_CHECK_EQUAL(copied[0], copy.rows[0].value);
    BOOST_CHECK_NE(opened[0].find("address=" + copied[0] + "&"), std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(a_flood_of_links_is_bounded, LinkHostSetup)
{
    // The thread of the links handles one at a time; while one is handled, at most MAX_QUEUED wait, and more are refused.
    hold = true;
    size_t taken{0};
    for (size_t i{0}; i < 50; ++i) taken += host->Enqueue(Buy(), Source::BROWSER) ? 1 : 0;
    BOOST_CHECK(taken >= LinkHost::MAX_QUEUED && taken <= LinkHost::MAX_QUEUED + 1);
    hold = false;
    host->Stop();
    // No wallet: each one handled was refused and said, never asked.
    BOOST_CHECK(asked.empty());
}

#ifndef WIN32
BOOST_AUTO_TEST_CASE(the_dex_from_the_site)
{
    FakeReader reader;
    const std::string head{"v=1&network=regtest&exp=" + Exp()};
    const auto plan{[&](const std::string& uri) { return Resolve(Parse(uri, NET, NOW), Source::BROWSER, reader, NOW); }};

    // A concentrated pool made at a price (a launch straight into a funded pool: register, this, then a full-range position).
    const Plan cl{plan("flush:pool?" + head + "&asset_a=ECX&ticker_a=ECX&asset_b=" + GOLD_ID + "&ticker_b=GOLD&kind=concentrated&fee_ppm=10000&amount_a=1&amount_b=20")};
    BOOST_CHECK_EQUAL(cl.method, "createpool");
    BOOST_CHECK_EQUAL(cl.params[4].find_value("curve").get_str(), "concentrated");

    // route: the pools follow one another from `from` to `to`; the quote hop by hop meets the floor; the path and pools to swaproute.
    reader.pool_quote = 5'000;
    const Plan route{plan("flush:route?" + head + "&pools=" + POOL_ID + "%2C" + POOL2_ID + "&from=ECX&to=TIDE&amount=1&min_out=0.004&slippage_bps=50")};
    BOOST_CHECK_EQUAL(route.method, "swaproute");
    BOOST_CHECK_EQUAL(route.params[0].write(), R"(["ECX","0x)" + GOLD_ID + R"(","0x)" + TOKEN_ID + R"("])");
    BOOST_CHECK_EQUAL(route.params[4].write(), R"([")" + POOL_ID + R"(",")" + POOL2_ID + R"("])");
    BOOST_CHECK(HasRow(route.preface, "Route", "ECX → GOLD → TIDE"));
    BOOST_CHECK_NE(ResolveRefused("flush:route?" + head + "&pools=" + POOL_ID + "%2C" + POOL2_ID + "&from=ECX&to=GOLD&amount=1&min_out=0.004", reader).find("ends in TIDE"), std::string::npos);
    BOOST_CHECK_NE(ResolveRefused("flush:route?" + head + "&pools=" + POOL2_ID + "%2C" + POOL_ID + "&from=ECX&to=TIDE&amount=1&min_out=0.004", reader).find("not one of them"), std::string::npos);
    BOOST_CHECK_NE(ResolveRefused("flush:route?" + head + "&pools=" + POOL_ID + "%2C" + POOL2_ID + "&from=ECX&to=TIDE&amount=1&min_out=1", reader).find("moved"), std::string::npos);
    BOOST_CHECK_NE(Refused("flush:route?" + head + "&pools=" + POOL_ID + "&from=ECX&to=GOLD&amount=1&min_out=1").find("2 pools at least"), std::string::npos);
    BOOST_CHECK_NE(Refused("flush:route?" + head + "&pools=" + POOL_ID + "%2C" + POOL_ID + "&from=ECX&to=GOLD&amount=1&min_out=1").find("twice"), std::string::npos);

    // deposit: the pool's assets named in either order, the shares quoted against the floor, the amounts by asset.
    const Plan deposit{plan("flush:deposit?" + head + "&pool=" + POOL_ID + "&asset_a=GOLD&asset_b=ECX&amount_a=20&amount_b=1&min_shares=990&slippage_bps=100")};
    BOOST_CHECK_EQUAL(deposit.method, "addpoolliquidity");
    BOOST_CHECK_EQUAL(deposit.params.write(), R"([")" + POOL_ID + R"(",{"0x)" + GOLD_ID + R"(":"20","ECX":"1"},1.00,990])");
    BOOST_CHECK_NE(ResolveRefused("flush:deposit?" + head + "&pool=" + POOL_ID + "&asset_a=GOLD&asset_b=ECX&amount_a=20&amount_b=1&min_shares=1001", reader).find("1000 shares"), std::string::npos);
    BOOST_CHECK_NE(ResolveRefused("flush:deposit?" + head + "&pool=" + POOL_ID + "&asset_a=TIDE&asset_b=ECX&amount_a=20&amount_b=1&min_shares=1", reader).find("binds its assets"), std::string::npos);
    BOOST_CHECK_NE(ResolveRefused("flush:deposit?" + head + "&pool=" + POOL_ID + "&asset_a=GOLD&asset_b=ECX&amount_a=20.001&amount_b=1&min_shares=1", reader).find("places"), std::string::npos);
    // withdraw: exactly the link's shares, the floors of both checked, the slippage that keeps the tighter one.
    const Plan withdraw{plan("flush:withdraw?" + head + "&pool=" + POOL_ID + "&asset_a=GOLD&asset_b=ECX&shares=500&min_a=12&min_b=0.49")};
    BOOST_CHECK_EQUAL(withdraw.method, "removepoolliquidity");
    BOOST_CHECK_EQUAL(withdraw.params.write(), R"([")" + POOL_ID + R"(",100,null,2.00,500,{"0x)" + GOLD_ID + R"(":"12","ECX":"0.49"}])");
    BOOST_CHECK_NE(ResolveRefused("flush:withdraw?" + head + "&pool=" + POOL_ID + "&asset_a=GOLD&asset_b=ECX&shares=500&min_a=13&min_b=0", reader).find("moved"), std::string::npos);
    BOOST_CHECK_NE(Refused("flush:withdraw?" + head + "&pool=" + POOL_ID + "&asset_a=GOLD&asset_b=ECX&shares=05&min_a=1&min_b=0").find("shares"), std::string::npos);
    BOOST_CHECK_NE(Refused("flush:withdraw?" + head + "&pool=" + POOL_ID + "&asset_a=GOLD&asset_b=ECX&shares=5&min_a=1&min_b=0&only=c").find("only"), std::string::npos);

    // position: in the pool's own order only; previewed first; its slippage binds.
    const Plan position{plan("flush:position?" + head + "&pool=" + POOL_ID + "&asset_a=ECX&asset_b=GOLD&price_lower=10&price_upper=30&amount_a=1&amount_b=0&slippage_bps=100")};
    BOOST_CHECK_EQUAL(position.method, "openposition");
    BOOST_CHECK_EQUAL(position.params.write(), R"([")" + POOL_ID + R"(","10","30",{"ECX":"1"},1.00])");
    BOOST_CHECK(HasRow(position.preface, "Now", "in this range"));
    // The full range: both prices 0 (no bound either side).
    const Plan full{plan("flush:position?" + head + "&pool=" + POOL_ID + "&asset_a=ECX&asset_b=GOLD&price_lower=0&price_upper=0&amount_a=1&amount_b=20&slippage_bps=100")};
    BOOST_CHECK_EQUAL(full.params[1].get_str(), "0");
    BOOST_CHECK(HasRow(full.preface, "Range", "the full range"));
    // With the price the site showed: given to the wallet, which refuses at signing if the pool's is not near it.
    const Plan priced{plan("flush:position?" + head + "&pool=" + POOL_ID + "&asset_a=ECX&asset_b=GOLD&price_lower=0&price_upper=0&amount_a=1&amount_b=20&price=20&slippage_bps=100")};
    BOOST_CHECK_EQUAL(priced.params.size(), 6U);
    BOOST_CHECK_EQUAL(priced.params[5].get_str(), "20");
    BOOST_CHECK(HasRow(priced.preface, "At the price", "20 GOLD per ECX"));
    BOOST_CHECK_NE(ResolveRefused("flush:position?" + head + "&pool=" + POOL_ID + "&asset_a=GOLD&asset_b=ECX&price_lower=10&price_upper=30&amount_a=1&amount_b=0&slippage_bps=100", reader).find("in that order"), std::string::npos);
    BOOST_CHECK_NE(Refused("flush:position?" + head + "&pool=" + POOL_ID + "&asset_a=ECX&asset_b=GOLD&price_lower=10&price_upper=30&amount_a=0&amount_b=0&slippage_bps=100").find("something to put in"), std::string::npos);
    // reposition: collect, remove (with its fees), close.
    BOOST_CHECK_EQUAL(plan("flush:reposition?" + head + "&position=" + GOLD_ID + "&action=collect&slippage_bps=100").params.write(), R"([")" + GOLD_ID + R"(",{"collect":true}])");
    BOOST_CHECK_EQUAL(plan("flush:reposition?" + head + "&position=" + GOLD_ID + "&action=remove&percent=25&slippage_bps=50").params.write(), R"([")" + GOLD_ID + R"(",{"remove":25,"collect":true,"slippage":0.50}])");
    BOOST_CHECK_EQUAL(plan("flush:reposition?" + head + "&position=" + GOLD_ID + "&action=close&slippage_bps=50").params.write(), R"([")" + GOLD_ID + R"(",{"close":true,"slippage":0.50}])");
    BOOST_CHECK_NE(Refused("flush:reposition?" + head + "&position=" + GOLD_ID + "&action=remove&percent=101&slippage_bps=50").find("percent"), std::string::npos);
    BOOST_CHECK_NE(Refused("flush:reposition?" + head + "&position=" + GOLD_ID + "&action=burn&slippage_bps=50").find("action"), std::string::npos);
    BOOST_CHECK_NE(Refused("flush:reposition?" + head + "&position=" + GOLD_ID + "&action=close&slippage_bps=1001").find("it binds"), std::string::npos);

    // settle, bid, collect, claim: by id, read whole for the question.
    const Plan settle{plan("flush:settle?" + head + "&order=" + GOLD_ID + "&cancel=1")};
    BOOST_CHECK_EQUAL(settle.method, "settleorder");
    BOOST_CHECK_EQUAL(settle.params.write(), R"([")" + GOLD_ID + R"(",true])");
    BOOST_CHECK_NE(Refused("flush:settle?" + head + "&order=" + GOLD_ID + "&cancel=yes").find("0 or 1"), std::string::npos);
    BOOST_CHECK_EQUAL(plan("flush:bid?" + head + "&auction=" + GOLD_ID + "&amount=2.5&buy_all=1").params.write(), R"([")" + GOLD_ID + R"(","2.5",true])");
    BOOST_CHECK_EQUAL(plan("flush:collect?" + head + "&auction=" + GOLD_ID).method, "collectauction");
    const Plan claim{plan("flush:claim?" + head + "&launch=TIDE&asset=" + TOKEN_ID)};
    BOOST_CHECK_EQUAL(claim.method, "claimlaunch");
    BOOST_CHECK_EQUAL(claim.params.write(), R"(["0x)" + TOKEN_ID + R"("])");
    BOOST_CHECK(HasRow(claim.preface, "Allocation", "minted now"));
    BOOST_CHECK_NE(ResolveRefused("flush:claim?" + head + "&launch=GOLD&asset=" + GOLD_ID, reader).find("not launched"), std::string::npos);
    BOOST_CHECK_NE(ResolveRefused("flush:claim?" + head + "&launch=GRAD&asset=" + TOKEN_ID, reader).find("one asset"), std::string::npos);

    // The peg: a deposit (no call of this wallet: the host's), a withdrawal to an eCash address said for what it is.
    const Plan pegin{plan("flush:pegin?" + head + "&amount=1.5")};
    BOOST_CHECK(pegin.method.empty());
    BOOST_CHECK_EQUAL(pegin.amount_units, 150'000'000U);
    const Plan pegout{plan("flush:pegout?" + head + "&address=ecash%3Aqz2708636snqhsxu8wnlka78h6fdp77ar59jrf5035&amount=2&fee=0.0001")};
    BOOST_CHECK_EQUAL(pegout.method, "createwithdrawal");
    BOOST_CHECK_EQUAL(pegout.params.write(), R"(["ecash:qz2708636snqhsxu8wnlka78h6fdp77ar59jrf5035","2","0.0001"])");
    BOOST_CHECK(HasRow(pegout.preface, "eCash address paid", "ecash:qz27"));
    BOOST_CHECK_NE(Refused("flush:pegout?" + head + "&address=ecash%3A%3Aq&amount=2&fee=0.0001").find("address"), std::string::npos);
    BOOST_CHECK_NE(Refused("flush:pegout?" + head + "&address=" + std::string(129, 'q') + "&amount=2&fee=0.0001").find("address"), std::string::npos);
    BOOST_CHECK_NE(Refused("flush:pegout?" + head + "&address=q&amount=2").find("fee is missing"), std::string::npos);
    // An unknown parameter of a new verb is refused as for the others.
    BOOST_CHECK_NE(Refused("flush:collect?" + head + "&auction=" + GOLD_ID + "&to=x").find("not a parameter"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(the_desktop_handler_of_the_links)
{
    const fs::path root{m_args.GetDataDirBase() / "xdg"};
    const fs::path data{root / "data"}, config{root / "config"}, system{root / "system"};
    const fs::path exe{fs::PathFromString("/opt/My \"Apps\"/$flush%/flush-app")};
    const auto read{[](const fs::path& p) {
        std::ifstream f{p.std_path()};
        return std::string{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    }};
    // First start: the entry, and the default handler of the scheme.
    BOOST_CHECK(webui::RegisterUriSchemeIn("flush", "Flush", "flush-app", exe, data, config, {system}));
    const std::string entry{read(data / "applications" / "flush-app.desktop")};
    BOOST_CHECK_NE(entry.find("MimeType=x-scheme-handler/flush;\n"), std::string::npos);
    BOOST_CHECK_NE(entry.find("Exec=\"/opt/My \\\\\"Apps\\\\\"/\\\\$flush%%/flush-app\" %u\n"), std::string::npos);
    BOOST_CHECK_EQUAL(read(config / "mimeapps.list"), "[Default Applications]\nx-scheme-handler/flush=flush-app.desktop\n");
    // Other entries kept; ours again changes nothing.
    {
        std::ofstream f{(config / "mimeapps.list").std_path()};
        f << "[Added Associations]\ntext/plain=gedit.desktop;\n\n[Default Applications]\nx-scheme-handler/http=firefox.desktop\nx-scheme-handler/flush=flush-app.desktop\n";
    }
    BOOST_CHECK(webui::RegisterUriSchemeIn("flush", "Flush", "flush-app", exe, data, config, {system}));
    BOOST_CHECK_EQUAL(read(config / "mimeapps.list"), "[Added Associations]\ntext/plain=gedit.desktop;\n\n[Default Applications]\nx-scheme-handler/http=firefox.desktop\nx-scheme-handler/flush=flush-app.desktop\n");
    // Another installation the user chose, which exists: kept.
    fs::create_directories(system / "applications");
    { std::ofstream f{(system / "applications" / "other-flush.desktop").std_path()}; f << "[Desktop Entry]\n"; }
    {
        std::ofstream f{(config / "mimeapps.list").std_path()};
        f << "[Default Applications]\nx-scheme-handler/flush=other-flush.desktop\n";
    }
    BOOST_CHECK(!webui::RegisterUriSchemeIn("flush", "Flush", "flush-app", exe, data, config, {system}));
    BOOST_CHECK_EQUAL(read(config / "mimeapps.list"), "[Default Applications]\nx-scheme-handler/flush=other-flush.desktop\n");
    // Gone: this one takes its place.
    fs::remove(system / "applications" / "other-flush.desktop");
    BOOST_CHECK(webui::RegisterUriSchemeIn("flush", "Flush", "flush-app", exe, data, config, {system}));
    BOOST_CHECK_EQUAL(read(config / "mimeapps.list"), "[Default Applications]\nx-scheme-handler/flush=flush-app.desktop\n");
    // Our entry, written by another installation (another folder) that still exists: kept; once it is gone, this one.
    const fs::path other_exe{root / "other" / "flush-app"};
    fs::create_directories(other_exe.parent_path());
    { std::ofstream f{other_exe.std_path()}; f << "#!/bin/sh\n"; }
    BOOST_CHECK(webui::RegisterUriSchemeIn("flush", "Flush", "flush-app", other_exe, data, config, {system}));
    BOOST_CHECK(!webui::RegisterUriSchemeIn("flush", "Flush", "flush-app", exe, data, config, {system}));
    BOOST_CHECK_NE(read(data / "applications" / "flush-app.desktop").find(fs::PathToString(other_exe)), std::string::npos);
    fs::remove(other_exe);
    BOOST_CHECK(webui::RegisterUriSchemeIn("flush", "Flush", "flush-app", exe, data, config, {system}));
    BOOST_CHECK_EQUAL(read(data / "applications" / "flush-app.desktop"), entry);
}
#endif

BOOST_AUTO_TEST_SUITE_END()
