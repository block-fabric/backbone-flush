// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// The links the public API builds (publicapi/build.h, BuildAction) are links the app takes: written by the API's
// own writer, read by the app's own strict parser (webui/links.h), each verb with exactly its parameters.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <publicapi/build.h>
#include <test/util/setup_common.h>
#include <webui/links.h>

#include <boost/test/unit_test.hpp>

using namespace webui::links;

namespace {
const std::string NET{"regtest"};
constexpr int64_t NOW{1'800'000'000};
const std::string TOKEN(64, 'a');
const std::string POOL(63, 'b');
} // namespace

BOOST_FIXTURE_TEST_SUITE(publicapi_links_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(built_links_are_read_by_the_app)
{
    using publicapi::ActionUri;
    using publicapi::Decimal;
    // buy: ECX in 8 places, the floor in the token's 6 (rounded down by the API, up by the app: the same number).
    const Request buy{Parse(ActionUri("buy", NET, NOW + 600, {{"launch", "TIDE"}, {"asset", TOKEN}, {"ecx", Decimal(500'000'000, 8)}, {"min_tokens", Decimal(936'823'477'630, 6)}, {"slippage_bps", "100"}}), NET, NOW)};
    BOOST_CHECK(buy.verb == Verb::BUY);
    BOOST_CHECK_EQUAL(buy.amount, "5");
    BOOST_CHECK_EQUAL(buy.floor, "936823.47763");
    BOOST_CHECK_EQUAL(*Units(buy.floor, 6, Rounding::UP), 936'823'477'630U);
    BOOST_CHECK_EQUAL(buy.expires, NOW + 600);
    const Request sell{Parse(ActionUri("sell", NET, NOW + 600, {{"launch", "TIDE"}, {"asset", TOKEN}, {"tokens", Decimal(1'000'500'000, 6)}, {"min_ecx", Decimal(123, 8)}, {"slippage_bps", "0"}}), NET, NOW)};
    BOOST_CHECK(sell.verb == Verb::SELL);
    BOOST_CHECK_EQUAL(sell.amount, "1000.5");
    BOOST_CHECK_EQUAL(sell.floor, "0.00000123");
    const Request swap{Parse(ActionUri("swap", NET, NOW + 600, {{"pool", POOL + "c"}, {"from", "ECX"}, {"to", "NOVA9"}, {"amount", "2"}, {"min_out", "48500"}, {"slippage_bps", "50"}}), NET, NOW)};
    BOOST_CHECK(swap.verb == Verb::SWAP);
    BOOST_CHECK_EQUAL(swap.to, "NOVA9");
    // launch: text percent-encoded whole (spaces, line breaks, UTF-8, the links' own ? & =).
    const Request launch{Parse(ActionUri("launch", NET, NOW + 3600, {{"ticker", "LUMEN"}, {"name", "Lumen Wörks"}, {"logo", "sha256:" + TOKEN}, {"desc", "Light for\nnight & day"},
                                                                     {"link", "https://lumen.example/a?b=c&d=e"}, {"link", "https://x.example"}, {"ecx", "3"}, {"min_tokens", "1243396.639607"}, {"slippage_bps", "100"}}),
                               NET, NOW)};
    BOOST_CHECK(launch.verb == Verb::LAUNCH);
    BOOST_CHECK_EQUAL(launch.name, "Lumen Wörks");
    BOOST_CHECK_EQUAL(launch.description, "Light for\nnight & day");
    BOOST_REQUIRE_EQUAL(launch.links.size(), 2U);
    BOOST_CHECK_EQUAL(launch.links[0], "https://lumen.example/a?b=c&d=e");
    BOOST_CHECK(launch.logo_sha256);
    // The verbs of the asset registry, as BuildAction writes them.
    const Request reg{Parse(ActionUri("register", NET, NOW + 3600, {{"ticker", "BROSE"}, {"name", "Bro Se"}, {"supply", "21000000"}, {"decimals", "8"}}), NET, NOW)};
    BOOST_CHECK(reg.verb == Verb::REGISTER && reg.supply == "21000000" && *reg.decimals == 8);
    const Request wide{Parse(ActionUri("launch", NET, NOW + 3600, {{"ticker", "WIDE"}, {"name", "Wide"}, {"supply", "21000000"}, {"decimals", "8"}, {"fee_bps", "200"}, {"allocation_bps", "100"}}), NET, NOW)};
    BOOST_CHECK(*wide.fee_bps == 200 && *wide.allocation_bps == 100);
    const Request curve{Parse(ActionUri("curve", NET, NOW + 600, {{"asset", TOKEN}, {"ticker", "GOLD"}, {"amount", Decimal(80'000'000'000, 6, 12)}, {"fee_bps", "115"}}), NET, NOW)};
    BOOST_CHECK(curve.verb == Verb::CURVE && curve.amount == "80000");
    const Request pool{Parse(ActionUri("pool", NET, NOW + 600, {{"asset_a", "ECX"}, {"ticker_a", "ECX"}, {"asset_b", TOKEN}, {"ticker_b", "GOLD"}, {"kind", "stable"}, {"fee_ppm", "500"}, {"amount_a", "1"}, {"amount_b", "2"}, {"amp", "100"}}), NET, NOW)};
    BOOST_CHECK(pool.verb == Verb::POOL && pool.asset_a.IsNull() && *pool.amp == 100);
    const Request auction{Parse(ActionUri("auction", NET, NOW + 600, {{"base", TOKEN}, {"base_ticker", "GOLD"}, {"amount", "10"}, {"quote", "ECX"}, {"quote_ticker", "ECX"}, {"start_price", "5"}, {"end_price", "1"}, {"duration", "100"}}), NET, NOW)};
    BOOST_CHECK(auction.verb == Verb::AUCTION && auction.blocks == 100);
    const Request order{Parse(ActionUri("order", NET, NOW + 600, {{"sell", TOKEN}, {"sell_ticker", "GOLD"}, {"buy", "ECX"}, {"buy_ticker", "ECX"}, {"amount", "5"}, {"price", "0.25"}, {"expiry_blocks", "40320"}}), NET, NOW)};
    BOOST_CHECK(order.verb == Verb::ORDER && order.price == "0.25");
    // The verbs the app does not take yet (built for wallet extensions) are refused by it, as they must be.
    BOOST_CHECK_THROW(Parse(ActionUri("deposit", NET, NOW + 600, {{"pool", POOL + "c"}}), NET, NOW), LinkError);
    BOOST_CHECK_THROW(Parse(ActionUri("route", NET, NOW + 600, {{"pools", POOL + "c"}}), NET, NOW), LinkError);
}

BOOST_AUTO_TEST_CASE(the_dex_links_the_api_builds)
{
    using publicapi::ActionUri;
    using publicapi::Decimal;
    const std::string P1(64, 'b'), P2(64, 'e'), ID(64, 'c');
    // Exactly the parameters BuildAction writes for each verb of the site's DEX (publicapi/service.cpp Build).
    const Request route{Parse(ActionUri("route", NET, NOW + 600, {{"pools", P1 + "," + P2}, {"from", "ECX"}, {"to", "TIDE"}, {"amount", "1"}, {"min_out", Decimal(4000, 6)}, {"slippage_bps", "50"}}), NET, NOW)};
    BOOST_CHECK(route.verb == Verb::ROUTE && route.pools.size() == 2);
    const Request deposit{Parse(ActionUri("deposit", NET, NOW + 600, {{"pool", P1}, {"asset_a", "GOLD"}, {"asset_b", "ECX"}, {"amount_a", "20"}, {"amount_b", "1"}, {"min_shares", "990"}, {"slippage_bps", "100"}}), NET, NOW)};
    BOOST_CHECK(deposit.verb == Verb::DEPOSIT && deposit.shares == "990");
    const Request withdraw{Parse(ActionUri("withdraw", NET, NOW + 600, {{"pool", P1}, {"asset_a", "GOLD"}, {"asset_b", "ECX"}, {"shares", "500"}, {"min_a", "12"}, {"min_b", Decimal(0, 8)}, {"only", "a"}, {"slippage_bps", "100"}}), NET, NOW)};
    BOOST_CHECK(withdraw.verb == Verb::WITHDRAW && withdraw.min_b == "0" && withdraw.only == "a");
    const Request position{Parse(ActionUri("position", NET, NOW + 600, {{"pool", P1}, {"asset_a", "ECX"}, {"asset_b", "GOLD"}, {"price_lower", "10"}, {"price_upper", "30"}, {"amount_a", "1"}, {"amount_b", Decimal(0, 2, 2)}, {"slippage_bps", "100"}}), NET, NOW)};
    BOOST_CHECK(position.verb == Verb::POSITION && position.amount_b == "0");
    BOOST_CHECK(Parse(ActionUri("reposition", NET, NOW + 600, {{"position", ID}, {"action", "remove"}, {"percent", "25"}, {"slippage_bps", "100"}}), NET, NOW).percent == 25);
    BOOST_CHECK(Parse(ActionUri("settle", NET, NOW + 600, {{"order", ID}, {"cancel", "0"}}), NET, NOW).verb == Verb::SETTLE);
    BOOST_CHECK(Parse(ActionUri("bid", NET, NOW + 600, {{"auction", ID}, {"amount", "2.5"}, {"buy_all", "1"}}), NET, NOW).buy_all);
    BOOST_CHECK(Parse(ActionUri("collect", NET, NOW + 600, {{"auction", ID}}), NET, NOW).verb == Verb::COLLECT);
    BOOST_CHECK(Parse(ActionUri("claim", NET, NOW + 600, {{"launch", "TIDE"}, {"asset", TOKEN}}), NET, NOW).verb == Verb::CLAIM);
    BOOST_CHECK(Parse(ActionUri("pegin", NET, NOW + 600, {{"amount", Decimal(150'000'000, 8, 8)}}), NET, NOW).amount == "1.5");
    BOOST_CHECK(Parse(ActionUri("pegout", NET, NOW + 600, {{"address", "ecash:qz2708636snqhsxu8wnlka78h6fdp77ar59jrf5035"}, {"amount", "2"}, {"fee", Decimal(10000, 8, 8)}}), NET, NOW).fee == "0.0001");
}

BOOST_AUTO_TEST_SUITE_END()
