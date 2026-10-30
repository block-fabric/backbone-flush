// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/income.h>

#include <addresstype.h>
#include <consensus/amount.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

using pos::CoinbaseRole;

namespace {
CScript Payee()
{
    return GetScriptForDestination(WitnessV0KeyHash{uint160{std::vector<unsigned char>(20, 7)}});
}

/** A merged-mined block (no header of stake) whose coinbase has these outputs. */
CBlock BmmBlock(const std::vector<CTxOut>& outs)
{
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vout = outs;
    CBlock block;
    block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    return block;
}

CTxOut Commitment() { return CTxOut{0, CScript{} << OP_RETURN << std::vector<unsigned char>(36, 0xaa)}; }
} // namespace

BOOST_FIXTURE_TEST_SUITE(pos_income_tests, BasicTestingSetup)

// Flush mainnet block 1: the BMM payee is also the destination of the deposit the block credits; no pot to pay.
BOOST_AUTO_TEST_CASE(deposit_to_the_payee_is_no_pot)
{
    const CBlock block{BmmBlock({CTxOut{0, Payee()}, CTxOut{150'000'000, Payee()}, Commitment()})};
    // No pot accrued: no output is the pot, the deposit included.
    const auto roles{pos::CoinbaseRoles(block, {}, CAmount{0})};
    BOOST_REQUIRE_EQUAL(roles.size(), 3U);
    for (const CoinbaseRole role : roles) BOOST_CHECK(role == CoinbaseRole::OTHER);
    // (Without knowing the pot, the old reading: the last payout to the payee.)
    BOOST_CHECK(pos::CoinbaseRoles(block, {})[1] == CoinbaseRole::ANCHOR);
}

BOOST_AUTO_TEST_CASE(deposit_and_pot_to_the_payee)
{
    // Deposits come first in the payouts, the pot after them.
    const CBlock block{BmmBlock({CTxOut{1000, Payee()}, CTxOut{150'000'000, Payee()}, CTxOut{20'000, Payee()}, Commitment()})};
    for (const std::optional<CAmount> pot : {std::optional<CAmount>{20'000}, std::optional<CAmount>{}}) {
        const auto roles{pos::CoinbaseRoles(block, {}, pot)};
        BOOST_REQUIRE_EQUAL(roles.size(), 4U);
        BOOST_CHECK(roles[0] == CoinbaseRole::OTHER);
        BOOST_CHECK(roles[1] == CoinbaseRole::OTHER);
        BOOST_CHECK(roles[2] == CoinbaseRole::ANCHOR);
        BOOST_CHECK(roles[3] == CoinbaseRole::OTHER);
    }
    // A pot of more than any output to the payee (the payouts waited for the next block): none.
    for (const CoinbaseRole role : pos::CoinbaseRoles(block, {}, CAmount{200'000'000})) BOOST_CHECK(role == CoinbaseRole::OTHER);
}

BOOST_AUTO_TEST_SUITE_END()
