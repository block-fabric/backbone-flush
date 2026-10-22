// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// The DEX's market index (bitassets/market.h): trades, candles and holders derived block by block from what the batch
// cleared, on a state of its own (launches traded in blocks), and undone exactly by a reorganisation.

#include <addresstype.h>
#include <bitassets/index.h>
#include <bitassets/market.h>
#include <bitassets/state.h>
#include <consensus/params.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sidechain/state.h>
#include <sidechain/store.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <map>
#include <vector>

using namespace bitassets;

namespace {
const CScript ALICE{GetScriptForDestination(WitnessV0KeyHash{uint160{std::vector<unsigned char>(20, 1)}})};
const CScript BOB{GetScriptForDestination(WitnessV0KeyHash{uint160{std::vector<unsigned char>(20, 2)}})};
const CScript CREATOR{GetScriptForDestination(WitnessV0KeyHash{uint160{std::vector<unsigned char>(20, 5)}})};
constexpr uint64_t COIN_UNITS{100'000'000};
int g_nonce{0};

CTransaction MakeTx(const std::vector<COutPoint>& inputs, std::optional<Operation> op, const std::vector<std::optional<Token>>& outs, CAmount chn_in, const CScript& to)
{
    CMutableTransaction tx;
    const int nonce{++g_nonce};
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(nonce % 250 + 1)}), static_cast<uint32_t>(400000 + nonce)});
    for (const COutPoint& in : inputs) tx.vin.emplace_back(in);
    Marker marker;
    marker.operation = std::move(op);
    for (const auto& out : outs) {
        marker.outputs.push_back({static_cast<uint32_t>(tx.vout.size()), out});
        tx.vout.emplace_back(0, to);
    }
    tx.vout.emplace_back(chn_in, MarkerScript(marker));
    return CTransaction{tx};
}

/** Blocks on a store: each block's changes in an overlay of its own, its undo kept, merged into the chain's. */
struct Chain {
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay base{empty};
    Consensus::SidechainParams params;
    int height{100};
    uint32_t time{1'800'000'000};
    std::vector<sidechain::StoreUndo> undos;
    std::unique_ptr<sidechain::StoreOverlay> block;
    std::vector<CTransactionRef> txs;

    Chain()
    {
        params.bitassets_reveal_depth = 0;
        params.dex_height = 0;
        params.dex_curves_height = 0;
        params.ticker_height = 0;
        params.logo_height = 0;
        Begin();
    }
    void Begin()
    {
        block = std::make_unique<sidechain::StoreOverlay>(base, /*journal=*/true);
        txs.clear();
    }
    void Require(const CTransaction& tx)
    {
        sidechain::State state{*block};
        std::vector<CTxOut> payouts, refunds;
        std::string reason;
        BOOST_REQUIRE_MESSAGE(state.ApplyTx(tx, height, params, payouts, refunds, reason, false, true), reason);
        txs.push_back(MakeTransactionRef(tx));
    }
    void End()
    {
        sidechain::State state{*block};
        std::vector<CTxOut> payouts;
        state.FinishTxs(height, time, params, payouts);
        CBlock b;
        b.nTime = time;
        b.vtx.push_back(MakeTransactionRef(CMutableTransaction{}));
        for (const auto& tx : txs) b.vtx.push_back(tx);
        index::ConnectBlock(*block, height, 10);
        market::ConnectBlock(*block, b, height);
        undos.push_back(block->TakeUndo());
        block->MergeInto(base);
        ++height;
        time += 15;
        Begin();
    }
    void Undo()
    {
        sidechain::StoreOverlay back{static_cast<const sidechain::StoreView&>(base)};
        back.Revert(undos.back());
        back.MergeInto(base);
        undos.pop_back();
        --height;
        time -= 15;
        Begin();
    }
    State Assets() const { return State{static_cast<const sidechain::StoreView&>(base)}; }
    /** Every entry of the market index. */
    std::map<sidechain::StoreBytes, sidechain::StoreBytes> Dump() const
    {
        std::map<sidechain::StoreBytes, sidechain::StoreBytes> out;
        base.ForEach(sidechain::StoreBytes{sidechain::DERIVED_TABLE, 'X'}, [&](const auto& k, const auto& v) {
            out.emplace(k, v);
            return true;
        });
        return out;
    }
    CTransaction Launch(const std::string& name, uint64_t first_buy)
    {
        const AssetId id{HashName(name)};
        const uint256 nonce{HashName("nonce " + name)};
        const CTransaction reserved{MakeTx({}, Reserve{ReservationCommitment(id, nonce, ALICE)}, {Token{Token::Kind::RESERVATION, uint256{}, 1}}, 0, ALICE)};
        Require(reserved);
        LaunchToken launch;
        launch.reg.name = id;
        launch.reg.nonce = nonce;
        launch.reg.decimals = LAUNCH_DECIMALS;
        launch.reg.text = name;
        launch.creator = CREATOR;
        launch.first_buy = first_buy;
        launch.chn_to = ALICE;
        const CTransaction tx{MakeTx({COutPoint{reserved.GetHash(), 0}}, launch, {std::nullopt}, static_cast<CAmount>(first_buy), ALICE)};
        Require(tx);
        return tx;
    }
    CTransaction Buy(const AssetId& token, uint64_t coin, const CScript& to)
    {
        const CTransaction tx{MakeTx({}, SwapPools{CHN, {Assets().GetLaunch(token)->pool}, coin, 1, to}, {std::nullopt}, static_cast<CAmount>(coin), to)};
        Require(tx);
        return tx;
    }
};

uint64_t Held(const State& assets, const COutPoint& out) { return assets.GetToken(out) ? assets.GetToken(out)->amount : 0; }
} // namespace

BOOST_FIXTURE_TEST_SUITE(dex_market_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(trades_candles_holders_and_a_reorganisation)
{
    market::SetEnabled(true);
    Chain chain;
    const AssetId MOON{HashName("MOON")};
    const CTransaction launched{chain.Launch("MOON", COIN_UNITS)};
    chain.End();
    BOOST_CHECK(market::LayoutCurrent(chain.base));
    const market::Market moon{true, MOON};
    std::vector<unsigned char> next;
    auto trades{market::Trades(chain.base, moon, 0, {}, 10, next)};
    BOOST_REQUIRE_EQUAL(trades.size(), 1U);
    BOOST_CHECK(trades[0].txid == launched.GetHash());
    BOOST_CHECK_EQUAL(trades[0].amount_in, COIN_UNITS);
    BOOST_CHECK_EQUAL(trades[0].amount_out, Held(chain.Assets(), COutPoint{launched.GetHash(), 0}));
    BOOST_CHECK(trades[0].to == ALICE);
    BOOST_CHECK_EQUAL(trades[0].curve, static_cast<uint8_t>(Curve::LAUNCH));
    // Holders: Alice, by her script.
    BOOST_CHECK_EQUAL(market::HolderCount(chain.base, MOON, Token::Kind::ASSET), 1U);
    const auto snapshot{chain.Dump()};

    // Two buys from Bob and Alice in one block: one price (the batch), two trades, one candle closing at the curve's price.
    const CTransaction b1{chain.Buy(MOON, 5 * COIN_UNITS, BOB)};
    const CTransaction b2{chain.Buy(MOON, 2 * COIN_UNITS, ALICE)};
    chain.End();
    trades = market::Trades(chain.base, moon, 0, {}, 10, next);
    BOOST_REQUIRE_EQUAL(trades.size(), 3U);
    BOOST_CHECK(next.empty());
    BOOST_CHECK_EQUAL(trades[0].height, chain.height - 1); // newest first
    uint64_t bob_got{0};
    for (const auto& t : trades) {
        if (t.to == BOB) bob_got = t.amount_out;
    }
    BOOST_CHECK_EQUAL(bob_got, Held(chain.Assets(), COutPoint{b1.GetHash(), 0}));
    // Pages: one at a time, the same rows.
    auto page{market::Trades(chain.base, moon, 0, {}, 1, next)};
    BOOST_REQUIRE(!next.empty());
    auto page2{market::Trades(chain.base, moon, 0, next, 1, next)};
    BOOST_REQUIRE_EQUAL(page2.size(), 1U);
    BOOST_CHECK(page2[0].txid == trades[1].txid);
    BOOST_CHECK_THROW(market::Trades(chain.base, moon, 0, {1, 2, 3}, 1, next), std::invalid_argument);
    // Candles: the volume is the coin the trades moved; the close the curve's price now.
    const auto candles{market::Candles(chain.base, moon, market::Interval::D1, 0, chain.time, 10)};
    BOOST_REQUIRE_EQUAL(candles.size(), 1U);
    BOOST_CHECK_CLOSE(candles[0].v, 8.0, 1e-9);
    BOOST_CHECK_EQUAL(candles[0].n, 3U);
    const auto pool{chain.Assets().GetPool(chain.Assets().GetLaunch(MOON)->pool)};
    const double price{*market::PoolPrice(chain.Assets(), chain.Assets().GetLaunch(MOON)->pool, *pool)};
    BOOST_CHECK_EQUAL(candles[0].c, price);
    BOOST_CHECK(candles[0].h >= candles[0].c && candles[0].l <= candles[0].o);
    BOOST_CHECK_EQUAL(market::Candles(chain.base, moon, market::Interval::M5, 0, chain.time, 10).size(), 1U);
    // Holders, largest first; Bob and Alice; a script's holdings.
    BOOST_CHECK_EQUAL(market::HolderCount(chain.base, MOON, Token::Kind::ASSET), 2U);
    const auto holders{market::Holders(chain.base, MOON, Token::Kind::ASSET, {}, 10, next)};
    BOOST_REQUIRE_EQUAL(holders.size(), 2U);
    BOOST_CHECK_GE(holders[0].amount, holders[1].amount);
    const auto bob{market::HoldingsOf(chain.base, BOB, 10)};
    BOOST_REQUIRE_EQUAL(bob.size(), 1U);
    BOOST_CHECK_EQUAL(bob[0].amount, bob_got);
    const uint64_t alice_has{Held(chain.Assets(), COutPoint{launched.GetHash(), 0}) + Held(chain.Assets(), COutPoint{b2.GetHash(), 0})};
    BOOST_CHECK_EQUAL(market::HoldingsOf(chain.base, ALICE, 10).at(0).amount, alice_has);

    // Bob sells it all back: his holding goes, the count with it.
    const CTransaction sell{MakeTx({COutPoint{b1.GetHash(), 0}}, SwapPools{MOON, {chain.Assets().GetLaunch(MOON)->pool}, bob_got, 1, BOB}, {std::nullopt}, 0, BOB)};
    chain.Require(sell);
    chain.End();
    BOOST_CHECK_EQUAL(market::HolderCount(chain.base, MOON, Token::Kind::ASSET), 1U);
    BOOST_CHECK(market::HoldingsOf(chain.base, BOB, 10).empty());
    trades = market::Trades(chain.base, moon, 0, {}, 10, next);
    BOOST_REQUIRE_EQUAL(trades.size(), 4U);
    BOOST_CHECK(trades[0].asset_in == MOON);
    BOOST_CHECK(trades[0].to == BOB);

    // A reorganisation takes the last two blocks back: the index is exactly what it was after the launch.
    chain.Undo();
    chain.Undo();
    BOOST_CHECK(chain.Dump() == snapshot);
    BOOST_CHECK_EQUAL(market::Trades(chain.base, moon, 0, {}, 10, next).size(), 1U);
    BOOST_CHECK_EQUAL(market::HolderCount(chain.base, MOON, Token::Kind::ASSET), 1U);

    // A node that does not keep it notes nothing, and a store with it can be wiped.
    market::SetEnabled(false);
    chain.Buy(MOON, COIN_UNITS, BOB);
    chain.End();
    BOOST_CHECK_EQUAL(market::Trades(chain.base, moon, 0, {}, 10, next).size(), 1U);
    BOOST_CHECK(market::LayoutCurrent(chain.base));
    BOOST_CHECK(market::Present(chain.base));
    BOOST_CHECK_GT(market::Wipe(chain.base), 0U);
    BOOST_CHECK(!market::Present(chain.base));
    market::SetEnabled(true);
    // Kept again, without what it missed: the store must be derived again.
    BOOST_CHECK(!market::LayoutCurrent(chain.base));
    market::SetEnabled(false);
}

BOOST_AUTO_TEST_CASE(intervals)
{
    BOOST_CHECK(market::ParseInterval("5m") == market::Interval::M5);
    BOOST_CHECK(market::ParseInterval("1d") == market::Interval::D1);
    BOOST_CHECK(!market::ParseInterval("2h"));
    BOOST_CHECK(!market::ParseInterval(""));
}

BOOST_AUTO_TEST_SUITE_END()
