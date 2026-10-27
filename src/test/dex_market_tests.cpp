// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// The DEX's market index (bitassets/market.h): trades, candles and holders derived block by block from what the batch
// cleared, on a state of its own (launches traded in blocks), and undone exactly by a reorganisation; the orders'
// outcomes (8.2.0): swaps filled or missed, limit orders filled, collected, cancelled and expired, by address.

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
    /** An asset registered into coins of `amounts`, held by `to` (outputs 1...). */
    std::vector<COutPoint> Issue(const std::string& name, const std::vector<uint64_t>& amounts, const CScript& to)
    {
        const AssetId id{HashName(name)};
        const uint256 nonce{HashName("nonce " + name)};
        uint64_t supply{0};
        for (uint64_t a : amounts) supply += a;
        const CTransaction reserved{MakeTx({}, Reserve{ReservationCommitment(id, nonce, to)}, {Token{Token::Kind::RESERVATION, uint256{}, 1}}, 0, to)};
        Require(reserved);
        Register reg;
        reg.name = id;
        reg.nonce = nonce;
        reg.supply = supply;
        reg.text = name;
        std::vector<std::optional<Token>> outs{Token{Token::Kind::CONTROL, id, 1}};
        for (uint64_t a : amounts) outs.push_back(Token{Token::Kind::ASSET, id, a});
        Marker marker;
        marker.operation = reg;
        CMutableTransaction tx;
        tx.vin.emplace_back(COutPoint{reserved.GetHash(), 0});
        for (const auto& out : outs) {
            marker.outputs.push_back({static_cast<uint32_t>(tx.vout.size()), out});
            tx.vout.emplace_back(0, to);
        }
        tx.vout.emplace_back(0, MarkerScript(marker));
        const CTransaction done{tx};
        Require(done);
        std::vector<COutPoint> coins;
        for (uint32_t i{1}; i <= amounts.size(); ++i) coins.emplace_back(done.GetHash(), i);
        return coins;
    }
    /** A limit order (its output 0 holds the order's token). */
    CTransaction Place(const COutPoint& coin, const AssetId& sell, uint64_t amount, const AssetId& buy, uint64_t num, uint64_t den, int32_t expiry, const CScript& to)
    {
        const CTransaction tx{MakeTx({coin}, PlaceOrder{sell, buy, amount, num, den, expiry}, {std::nullopt}, 0, to)};
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

BOOST_AUTO_TEST_CASE(swap_outcomes)
{
    market::SetEnabled(true);
    Chain chain;
    const AssetId X{HashName("XXX")}, Y{HashName("YYY")}, Z{HashName("ZZZ")};
    const auto x{chain.Issue("XXX", {1000000, 1000, 1000}, ALICE)};
    const auto y{chain.Issue("YYY", {1000000, 1000000}, ALICE)};
    const auto z{chain.Issue("ZZZ", {1000000}, ALICE)};
    const auto pool{[&](const AssetId& a, const COutPoint& ca, const AssetId& b, const COutPoint& cb) {
        CreatePool create;
        create.asset_a = a;
        create.asset_b = b;
        create.fee = 3000;
        create.amount_a = 1000000;
        create.amount_b = 1000000;
        chain.Require(MakeTx({a < b ? ca : cb, a < b ? cb : ca}, create, {std::nullopt}, 0, ALICE));
        return PoolIdOf(a, b, PoolParams{});
    }};
    const uint256 xy{pool(X, x[0], Y, y[0])}, yz{pool(Y, y[1], Z, z[0])};
    chain.End();
    // A route X -> Y -> Z that fills (Bob's), and one that asks too much and misses (Alice's): refunded at its result output.
    const CTransaction route{MakeTx({x[1]}, SwapPools{X, {xy, yz}, 1000, 1, CScript{}}, {std::nullopt}, 0, BOB)};
    chain.Require(route);
    const CTransaction greedy{MakeTx({x[2]}, SwapPools{X, {xy, yz}, 1000, 999999, CScript{}}, {std::nullopt}, 0, ALICE)};
    chain.Require(greedy);
    chain.End();
    const auto filled{market::GetOutcome(chain.base, route.GetHash(), 10)};
    BOOST_REQUIRE(filled && filled->swap);
    const market::Fill& f{filled->swap->fill};
    BOOST_CHECK(f.filled);
    BOOST_CHECK_EQUAL(filled->swap->height, chain.height - 1);
    BOOST_CHECK_EQUAL(filled->swap->time, chain.time - 15);
    BOOST_REQUIRE_EQUAL(f.path.size(), 3U);
    BOOST_CHECK(f.path[0] == X && f.path[1] == Y && f.path[2] == Z);
    BOOST_REQUIRE_EQUAL(f.pools.size(), 2U);
    BOOST_CHECK(f.pools[0] == xy && f.pools[1] == yz);
    BOOST_CHECK_EQUAL(f.curves[1], static_cast<uint8_t>(Curve::CONSTANT_PRODUCT));
    BOOST_CHECK(f.launches[0].IsNull());
    BOOST_REQUIRE_EQUAL(f.amounts.size(), 3U);
    BOOST_CHECK_EQUAL(f.amounts[0], 1000U);
    BOOST_CHECK_EQUAL(f.amounts[2], Held(chain.Assets(), COutPoint{route.GetHash(), 0}));
    BOOST_CHECK(filled->swap->Payee() == BOB);
    const auto missed{market::GetOutcome(chain.base, greedy.GetHash(), 10)};
    BOOST_REQUIRE(missed && missed->swap);
    BOOST_CHECK(!missed->swap->fill.filled);
    BOOST_CHECK_EQUAL(missed->swap->fill.amounts.size(), 1U);
    BOOST_CHECK_EQUAL(missed->swap->fill.amounts[0], 1000U);
    BOOST_CHECK(missed->swap->Payee() == ALICE);
    BOOST_CHECK_EQUAL(Held(chain.Assets(), COutPoint{greedy.GetHash(), 0}), 1000U);
    // Not an order: nothing (the pools' creation, a registration, any id).
    BOOST_CHECK(!market::GetOutcome(chain.base, Txid::FromUint256(uint256::ONE), 10));
    BOOST_CHECK(!market::GetOutcome(chain.base, x[0].hash, 10));
    // By address: Bob's one, Alice's one.
    std::vector<unsigned char> next;
    BOOST_CHECK_EQUAL(market::OutcomeCount(chain.base, BOB), 1U);
    BOOST_CHECK_EQUAL(market::OutcomesOf(chain.base, BOB, {}, 10, 10, next).size(), 1U);
    BOOST_CHECK_EQUAL(market::OutcomeCount(chain.base, ALICE), 1U);
    const auto snapshot{chain.Dump()};

    // A launch buy past the curve's end: what it could not spend comes back (back), paid by the coinbase.
    const AssetId MOON{HashName("MOON")};
    chain.Launch("MOON", COIN_UNITS);
    chain.End();
    const CTransaction whale{chain.Buy(MOON, 5000 * COIN_UNITS, BOB)};
    chain.End();
    const auto bought{market::GetOutcome(chain.base, whale.GetHash(), 10)};
    BOOST_REQUIRE(bought && bought->swap);
    BOOST_CHECK(bought->swap->fill.filled);
    BOOST_CHECK_GT(bought->swap->fill.back, 0U);
    BOOST_CHECK(!bought->swap->fill.launches[0].IsNull());
    BOOST_CHECK(bought->swap->fill.launches[0] == MOON);
    BOOST_CHECK_EQUAL(bought->swap->fill.curves[0], static_cast<uint8_t>(Curve::LAUNCH));
    BOOST_CHECK_EQUAL(bought->swap->fill.amounts.back(), Held(chain.Assets(), COutPoint{whale.GetHash(), 0}));
    // Bob's, newest first, a page at a time.
    BOOST_CHECK_EQUAL(market::OutcomeCount(chain.base, BOB), 2U);
    auto page{market::OutcomesOf(chain.base, BOB, {}, 1, 10, next)};
    BOOST_REQUIRE_EQUAL(page.size(), 1U);
    BOOST_CHECK(page[0].swap->fill.txid == whale.GetHash());
    BOOST_REQUIRE(!next.empty());
    page = market::OutcomesOf(chain.base, BOB, next, 1, 10, next);
    BOOST_REQUIRE_EQUAL(page.size(), 1U);
    BOOST_CHECK(page[0].swap->fill.txid == route.GetHash());
    BOOST_CHECK(next.empty());
    BOOST_CHECK_THROW(market::OutcomesOf(chain.base, BOB, {1, 2, 3}, 1, 10, next), std::invalid_argument);

    // A reorganisation takes them back.
    chain.Undo();
    chain.Undo();
    BOOST_CHECK(chain.Dump() == snapshot);
    BOOST_CHECK(!market::GetOutcome(chain.base, whale.GetHash(), 10));
    BOOST_CHECK_EQUAL(market::OutcomeCount(chain.base, BOB), 1U);
    market::SetEnabled(false);
}

BOOST_AUTO_TEST_CASE(limit_order_outcomes)
{
    market::SetEnabled(true);
    Chain chain;
    const AssetId X{HashName("XXX")}, Y{HashName("YYY")};
    const auto x{chain.Issue("XXX", {1000, 1000, 1000}, ALICE)};
    const auto y{chain.Issue("YYY", {1000, 1000, 1000}, BOB)};
    chain.End();
    // Alice's ask: 1000 X at 2 Y each at least. Bob's bid: 1000 Y for X at 2 at most: 500 X of it.
    const CTransaction ask{chain.Place(x[0], X, 1000, Y, 2, 1, chain.height + 100, ALICE)};
    const CTransaction bid{chain.Place(y[0], Y, 1000, X, 1, 2, chain.height + 100, BOB)};
    chain.End();
    auto outcome{market::GetOutcome(chain.base, ask.GetHash(), 10)};
    BOOST_REQUIRE(outcome && outcome->order);
    const market::OrderRow* row{&*outcome->order};
    BOOST_CHECK(row->owner == ALICE);
    BOOST_CHECK_EQUAL(row->placed.token_n, 0U);
    BOOST_CHECK(row->placed.sell == X && row->placed.buy == Y);
    BOOST_CHECK_EQUAL(row->placed.order.amount, 1000U);
    BOOST_CHECK_EQUAL(row->placed.order.height, chain.height - 1);
    BOOST_CHECK_EQUAL(row->remaining, chain.Assets().GetOrder(OrderId(ask.GetHash()))->remaining);
    BOOST_CHECK_EQUAL(row->remaining, 500U);
    BOOST_CHECK_EQUAL(row->fills, 1U);
    BOOST_REQUIRE_EQUAL(outcome->events.size(), 1U);
    BOOST_CHECK_EQUAL(outcome->events[0].kind, market::OrderEvent::FILL);
    BOOST_CHECK_EQUAL(outcome->events[0].sold, 500U);
    BOOST_CHECK_EQUAL(outcome->events[0].got, row->got);
    BOOST_CHECK_EQUAL(outcome->events[0].height, chain.height - 1);
    // The bid filled whole, in the same crossing.
    const auto bid_outcome{market::GetOutcome(chain.base, bid.GetHash(), 10)};
    BOOST_REQUIRE(bid_outcome && bid_outcome->order);
    BOOST_CHECK_EQUAL(bid_outcome->order->remaining, 0U);
    BOOST_CHECK(bid_outcome->order->owner == BOB);

    // Alice collects what she got: a collect; the order stays.
    const uint64_t got{row->got};
    const CTransaction collect{MakeTx({COutPoint{ask.GetHash(), 0}}, SettleOrder{OrderId(ask.GetHash()), false, ALICE}, {std::nullopt, std::nullopt}, 0, ALICE)};
    chain.Require(collect);
    // Another bid fills the rest.
    const CTransaction bid2{chain.Place(y[1], Y, 1000, X, 1, 2, chain.height + 100, BOB)};
    chain.End();
    outcome = market::GetOutcome(chain.base, ask.GetHash(), 10);
    BOOST_REQUIRE(outcome && outcome->order);
    row = &*outcome->order;
    BOOST_CHECK_EQUAL(row->remaining, 0U);
    BOOST_CHECK_EQUAL(row->fills, 2U);
    BOOST_CHECK_EQUAL(row->collects, 1U);
    BOOST_CHECK_EQUAL(row->closed, 0);
    BOOST_REQUIRE_EQUAL(outcome->events.size(), 3U);
    BOOST_CHECK_EQUAL(outcome->events[1].kind, market::OrderEvent::COLLECT);
    BOOST_CHECK(outcome->events[1].settle && outcome->events[1].settle->txid == collect.GetHash());
    BOOST_CHECK_EQUAL(outcome->events[1].got, got);
    BOOST_REQUIRE(outcome->events[1].settle->proceeds_n);
    BOOST_CHECK_EQUAL(*outcome->events[1].settle->proceeds_n, 1U);
    BOOST_CHECK_EQUAL(Held(chain.Assets(), COutPoint{collect.GetHash(), 1}), got);
    BOOST_CHECK_EQUAL(outcome->events[2].kind, market::OrderEvent::FILL);
    BOOST_CHECK_EQUAL(outcome->events[2].sold, 500U);
    // The collect's txid finds the order too.
    const auto by_settle{market::GetOutcome(chain.base, collect.GetHash(), 10)};
    BOOST_REQUIRE(by_settle && by_settle->order);
    BOOST_CHECK(by_settle->by_settle);
    BOOST_CHECK(by_settle->order->placed.order.txid == ask.GetHash());

    // Filled: its holder closes it (settled, not cancelled).
    const CTransaction close{MakeTx({COutPoint{collect.GetHash(), 0}}, SettleOrder{OrderId(ask.GetHash()), false, ALICE}, {std::nullopt}, 0, ALICE)};
    chain.Require(close);
    // Another ask, cancelled in the block it was placed in; one that expires.
    const CTransaction ask2{chain.Place(x[1], X, 1000, Y, 9, 1, chain.height + 100, ALICE)};
    const CTransaction cancel{MakeTx({COutPoint{ask2.GetHash(), 0}}, SettleOrder{OrderId(ask2.GetHash()), true, ALICE}, {std::nullopt}, 0, ALICE)};
    chain.Require(cancel);
    const CTransaction ask3{chain.Place(x[2], X, 1000, Y, 9, 1, chain.height + 1, ALICE)};
    chain.End();
    outcome = market::GetOutcome(chain.base, ask.GetHash(), 10);
    BOOST_REQUIRE(outcome && outcome->order);
    BOOST_CHECK_EQUAL(outcome->order->closed, chain.height - 1);
    BOOST_CHECK(!outcome->order->cancelled);
    BOOST_CHECK(outcome->order->settle.txid == close.GetHash());
    BOOST_CHECK(outcome->order->settle.closes);
    const auto cancelled{market::GetOutcome(chain.base, ask2.GetHash(), 10)};
    BOOST_REQUIRE(cancelled && cancelled->order);
    BOOST_CHECK(cancelled->order->cancelled);
    BOOST_CHECK_EQUAL(cancelled->order->settle.left, 1000U);
    BOOST_REQUIRE(cancelled->order->settle.left_n);
    BOOST_CHECK_EQUAL(*cancelled->order->settle.left_n, 0U);
    BOOST_CHECK(cancelled->order->fills == 0 && cancelled->events.empty());
    const auto snapshot{chain.Dump()};
    chain.End();
    chain.End();
    // Expired: nothing filled, still not closed (the tip says it expired).
    const auto expired{market::GetOutcome(chain.base, ask3.GetHash(), 10)};
    BOOST_REQUIRE(expired && expired->order);
    BOOST_CHECK_EQUAL(expired->order->remaining, 1000U);
    BOOST_CHECK_EQUAL(expired->order->closed, 0);
    BOOST_CHECK_LT(expired->order->placed.order.expiry, chain.height);
    // Alice's: her four orders (the bids are Bob's), newest first.
    std::vector<unsigned char> next;
    BOOST_CHECK_EQUAL(market::OutcomeCount(chain.base, ALICE), 3U);
    BOOST_CHECK_EQUAL(market::OutcomeCount(chain.base, BOB), 2U);
    const auto alice{market::OutcomesOf(chain.base, ALICE, {}, 10, 10, next)};
    BOOST_REQUIRE_EQUAL(alice.size(), 3U);
    BOOST_CHECK(alice.back().order->placed.order.txid == ask.GetHash());

    // A reorganisation of the blocks of the fills undoes them: the ask as it was placed, no fill.
    chain.Undo();
    chain.Undo();
    BOOST_CHECK(chain.Dump() == snapshot);
    chain.Undo();
    chain.Undo();
    outcome = market::GetOutcome(chain.base, ask.GetHash(), 10);
    BOOST_REQUIRE(outcome && outcome->order);
    BOOST_CHECK_EQUAL(outcome->order->remaining, 500U);
    BOOST_CHECK_EQUAL(outcome->events.size(), 1U);
    BOOST_CHECK(!market::GetOutcome(chain.base, collect.GetHash(), 10));
    BOOST_CHECK(!market::GetOutcome(chain.base, bid2.GetHash(), 10));
    chain.Undo();
    BOOST_CHECK(!market::GetOutcome(chain.base, ask.GetHash(), 10));
    BOOST_CHECK_EQUAL(market::OutcomeCount(chain.base, ALICE), 0U);
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
