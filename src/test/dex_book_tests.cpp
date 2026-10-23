// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Order books (doc/layers.md "Order books"): limit orders resting in the book of a pair, crossing one another at the end
// of a block at one price, takers through the book in the batch (all or nothing), routes through a book and a pool,
// collecting, cancelling, expiry; the book never pays more than its orders hold.

#include <addresstype.h>
#include <bitassets/curves.h>
#include <bitassets/dex.h>
#include <bitassets/state.h>
#include <consensus/params.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <streams.h>
#include <sidechain/state.h>
#include <sidechain/store.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <vector>

using namespace bitassets;

namespace {
const CScript HOLDER{GetScriptForDestination(WitnessV0KeyHash{uint160{}})};
const CScript PAYEE{GetScriptForDestination(WitnessV0KeyHash{uint160{std::vector<unsigned char>(20, 7)}})};
int g_nonce{0};

Token AssetToken(const AssetId& id, uint64_t amount) { return Token{Token::Kind::ASSET, id, amount}; }

CTransaction MakeTx(const std::vector<COutPoint>& inputs, std::optional<Operation> op, const std::vector<std::optional<Token>>& outs, CAmount chn_in = 0)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(++g_nonce % 250 + 1)}), static_cast<uint32_t>(500000 + g_nonce)});
    for (const COutPoint& in : inputs) tx.vin.emplace_back(in);
    Marker marker;
    marker.operation = std::move(op);
    for (size_t i{0}; i < outs.size(); ++i) {
        marker.outputs.push_back({static_cast<uint32_t>(tx.vout.size()), outs[i]});
        tx.vout.emplace_back(0, HOLDER);
    }
    tx.vout.emplace_back(chn_in, MarkerScript(marker));
    return CTransaction{tx};
}

struct Chain {
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State state{store};
    Consensus::SidechainParams params;
    int height{100};
    uint32_t time{1'800'000'000};
    std::vector<CTxOut> payouts, refunds, paid;
    Chain()
    {
        params.bitassets_reveal_depth = 0;
        params.dex_height = 0;
        params.dex_curves_height = 0;
    }
    bool Apply(const CTransaction& tx, std::string& reason) { return state.ApplyTx(tx, height, params, payouts, refunds, reason, false, true); }
    void Require(const CTransaction& tx)
    {
        std::string reason;
        BOOST_REQUIRE_MESSAGE(Apply(tx, reason), reason);
    }
    std::string Refused(const CTransaction& tx)
    {
        std::string reason;
        BOOST_CHECK(!Apply(tx, reason));
        return reason;
    }
    void EndBlock()
    {
        state.FinishTxs(height, time, params, payouts);
        paid = payouts;
        payouts.clear();
        refunds.clear();
        ++height;
        time += 15;
    }
    bitassets::State Assets() const { return state.BitAssets(); }
    std::optional<Token> TokenAt(const COutPoint& out) const { return Assets().GetToken(out); }
    std::vector<COutPoint> Issue(const std::string& name, const std::vector<uint64_t>& amounts, uint8_t decimals = 0)
    {
        const AssetId id{HashName(name)};
        const uint256 nonce{HashName("nonce " + name)};
        uint64_t supply{0};
        for (uint64_t a : amounts) supply += a;
        const CTransaction reserved{MakeTx({}, Reserve{ReservationCommitment(id, nonce, HOLDER)}, {Token{Token::Kind::RESERVATION, uint256{}, 1}})};
        Require(reserved);
        Register reg;
        reg.name = id;
        reg.nonce = nonce;
        reg.supply = supply;
        reg.decimals = decimals;
        reg.text = name;
        std::vector<std::optional<Token>> outs{Token{Token::Kind::CONTROL, id, 1}};
        for (uint64_t a : amounts) outs.push_back(AssetToken(id, a));
        // Registered straight into coins of the amounts (the books of a registration credit its supply).
        Marker marker;
        marker.operation = reg;
        CMutableTransaction tx;
        tx.vin.emplace_back(COutPoint{reserved.GetHash(), 0});
        for (size_t i{0}; i < outs.size(); ++i) {
            marker.outputs.push_back({static_cast<uint32_t>(tx.vout.size()), outs[i]});
            tx.vout.emplace_back(0, HOLDER);
        }
        tx.vout.emplace_back(0, MarkerScript(marker));
        const CTransaction done{tx};
        Require(done);
        std::vector<COutPoint> coins;
        for (uint32_t i{1}; i <= amounts.size(); ++i) coins.emplace_back(done.GetHash(), i);
        return coins;
    }
    /** An order: the transaction (its output 0 holds the order's token). */
    CTransaction Place(const COutPoint& coin, const AssetId& sell, uint64_t amount, const AssetId& buy, uint64_t num, uint64_t den, int32_t expiry = 0)
    {
        const CTransaction tx{MakeTx({coin}, PlaceOrder{sell, buy, amount, num, den, expiry ? expiry : height + 1000}, {std::nullopt})};
        Require(tx);
        return tx;
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(dex_book_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(resting_orders_cross_at_one_price)
{
    Chain chain;
    const AssetId X{HashName("XXX")}, Y{HashName("YYY")};
    const auto x{chain.Issue("XXX", {1000, 1000, 1000})};
    const auto y{chain.Issue("YYY", {3000, 3000, 3000})};
    // Asks: 1000 X at 2 Y each at least, 1000 X at 3; a bid: 3000 Y for X at up to 2.5 Y each (X per Y at least 2/5).
    const CTransaction ask2{chain.Place(x[0], X, 1000, Y, 2, 1)};
    const CTransaction ask3{chain.Place(x[1], X, 1000, Y, 3, 1)};
    const CTransaction bid{chain.Place(y[0], Y, 3000, X, 2, 5)};
    const uint256 book{BookId(X, Y)};
    BOOST_CHECK(chain.Assets().GetPoolParams(book).GetCurve() == Curve::BOOK);
    BOOST_CHECK_EQUAL(chain.Assets().BookSide(book, X < Y, 10, chain.height).size(), 2U);
    chain.EndBlock();
    // They cross: the ask at 2 and the bid (up to 2.5). One price for both, the volume most: 1000 X.
    const auto a2{*chain.Assets().GetOrder(OrderId(ask2.GetHash()))}, b{*chain.Assets().GetOrder(OrderId(bid.GetHash()))};
    const auto a3{*chain.Assets().GetOrder(OrderId(ask3.GetHash()))};
    BOOST_CHECK_EQUAL(a2.remaining, 0U);
    BOOST_CHECK_EQUAL(a3.remaining, 1000U);
    // The bid got the 1000 X the ask sold, at one price between the two limits, and paid for them what the ask got (or more: the roundings stay in the book).
    BOOST_CHECK_EQUAL(b.got, 1000U);
    BOOST_CHECK_GE(3000 - b.remaining, a2.got);
    BOOST_CHECK(a2.got >= 2000 && a2.got <= 2500);
    // The book holds what its orders hold (what is left to sell, what was got) and the roundings.
    const auto pool{*chain.Assets().GetPool(book)};
    const bool x0{X < Y};
    const uint64_t r_x{x0 ? pool.reserve0 : pool.reserve1}, r_y{x0 ? pool.reserve1 : pool.reserve0};
    BOOST_CHECK_GE(r_x, a3.remaining + b.proceeds);
    BOOST_CHECK_GE(r_y, b.remaining + a2.proceeds);
    // Collect the ask's proceeds: its token spent, nothing left to sell: it is gone.
    chain.Require(MakeTx({COutPoint{ask2.GetHash(), 0}}, SettleOrder{OrderId(ask2.GetHash()), false, PAYEE}, {std::nullopt}));
    BOOST_CHECK(!chain.Assets().GetOrder(OrderId(ask2.GetHash())));
    // Cancel the other ask: what it sells back, its token gone.
    const CTransaction cancel{MakeTx({COutPoint{ask3.GetHash(), 0}}, SettleOrder{OrderId(ask3.GetHash()), true, PAYEE}, {std::nullopt})};
    chain.Require(cancel);
    BOOST_CHECK(chain.TokenAt(COutPoint{cancel.GetHash(), 0}) == AssetToken(X, 1000));
    // Nobody but the holder of the token settles it.
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({}, SettleOrder{OrderId(bid.GetHash()), true, PAYEE}, {std::nullopt, std::nullopt})), "bad-ba-tokens-unbacked");
}

BOOST_AUTO_TEST_CASE(takers_and_routes)
{
    Chain chain;
    const AssetId X{HashName("XXX")}, Y{HashName("YYY")}, Z{HashName("ZZZ")};
    const auto x{chain.Issue("XXX", {5000, 1000, 100, 1000})};
    const auto y{chain.Issue("YYY", {1000000, 10000, 300, 50000})};
    const auto z{chain.Issue("ZZZ", {1000000, 1000})};
    // Asks of X for Y at 2 and 3.
    chain.Place(x[0], X, 5000, Y, 2, 1);
    chain.Place(x[1], X, 1000, Y, 3, 1);
    chain.EndBlock();
    const uint256 book{BookId(X, Y)};
    // A taker paying 300 Y through the book: X at 2 (150 X), exactly what a quote alone says.
    const auto quote{dex::QuoteRoutePools(chain.Assets(), Y, {book}, 300)};
    BOOST_REQUIRE(quote);
    const CTransaction take{MakeTx({y[2]}, SwapPools{Y, {book}, 300, quote->back(), CScript{}}, {std::nullopt})};
    chain.Require(take);
    chain.EndBlock();
    BOOST_CHECK(chain.TokenAt(COutPoint{take.GetHash(), 0}) == AssetToken(X, quote->back()));
    BOOST_CHECK_EQUAL(quote->back(), 150U);
    // A taker larger than the book: all or nothing: refunded whole.
    const CTransaction whale{MakeTx({y[3]}, SwapPools{Y, {book}, 50000, 1, CScript{}}, {std::nullopt})};
    chain.Require(whale);
    chain.EndBlock();
    BOOST_CHECK(chain.TokenAt(COutPoint{whale.GetHash(), 0}) == AssetToken(Y, 50000));
    // A route: Z to Y in a pool, then Y to X through the book.
    CreatePool create;
    create.asset_a = Z;
    create.asset_b = Y;
    create.fee = 3000;
    create.amount_a = 1000000;
    create.amount_b = 1000000;
    const bool z0{Z < Y};
    chain.Require(MakeTx({z0 ? z[0] : y[0], z0 ? y[0] : z[0]}, create, {std::nullopt}));
    chain.EndBlock();
    const uint256 pool{PoolIdOf(Z, Y, PoolParams{})};
    const auto routed{dex::QuoteRoutePools(chain.Assets(), Z, {pool, book}, 1000)};
    BOOST_REQUIRE(routed);
    const CTransaction route{MakeTx({z[1]}, SwapPools{Z, {pool, book}, 1000, routed->back(), CScript{}}, {std::nullopt})};
    chain.Require(route);
    chain.EndBlock();
    BOOST_CHECK(chain.TokenAt(COutPoint{route.GetHash(), 0}) == AssetToken(X, routed->back()));
    // The route search finds the book too.
    const auto found{dex::FindRoutes(chain.Assets(), Z, X, 1000, 2)};
    BOOST_REQUIRE(!found.empty());
    BOOST_CHECK(found[0].pools.back() == book);
}

BOOST_AUTO_TEST_CASE(expiry)
{
    Chain chain;
    const AssetId X{HashName("XXX")}, Y{HashName("YYY")};
    const auto x{chain.Issue("XXX", {1000})};
    const auto y{chain.Issue("YYY", {3000})};
    const CTransaction ask{chain.Place(x[0], X, 1000, Y, 2, 1, chain.height + 1)};
    chain.EndBlock();
    chain.EndBlock();
    chain.EndBlock();
    // Expired: a crossing bid placed now finds nothing; the ask's holder gets it all back.
    const CTransaction bid{chain.Place(y[0], Y, 3000, X, 1, 3)};
    chain.EndBlock();
    BOOST_CHECK_EQUAL(chain.Assets().GetOrder(OrderId(bid.GetHash()))->got, 0U);
    const CTransaction back{MakeTx({COutPoint{ask.GetHash(), 0}}, SettleOrder{OrderId(ask.GetHash()), false, PAYEE}, {std::nullopt})};
    chain.Require(back);
    BOOST_CHECK(chain.TokenAt(COutPoint{back.GetHash(), 0}) == AssetToken(X, 1000));
    // Orders past the longest duration: refused.
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({}, PlaceOrder{X, Y, 1, 1, 1, chain.height + MAX_ORDER_DURATION + 1}, {std::nullopt})), "bad-ba-order-expiry");
}

BOOST_AUTO_TEST_CASE(random_books_stay_solvent)
{
    // Random asks, bids and takers over many blocks: the book always holds what its orders are owed (what is left to sell,
    // what they got), and every order can be cancelled at the end, paid in full.
    Chain chain;
    const AssetId X{HashName("XXX")}, Y{HashName("YYY")};
    const auto x{chain.Issue("XXX", std::vector<uint64_t>(60, 10000))};
    const auto y{chain.Issue("YYY", std::vector<uint64_t>(60, 30000))};
    const uint256 book{BookId(X, Y)};
    const bool x0{X < Y};
    FastRandomContext rng{/*fDeterministic=*/true};
    std::vector<CTransaction> placed;
    size_t nx{0}, ny{0};
    for (int block{0}; block < 15; ++block) {
        for (int k{0}, m{1 + static_cast<int>(rng.randrange(4))}; k < m; ++k) {
            const int what{static_cast<int>(rng.randrange(3))};
            if (what == 0 && nx < x.size()) {
                placed.push_back(chain.Place(x[nx++], X, 10000, Y, 2 + rng.randrange(3), 1));
            } else if (what == 1 && ny < y.size()) {
                placed.push_back(chain.Place(y[ny++], Y, 30000, X, 1, 2 + rng.randrange(3)));
            } else if (ny < y.size() && chain.Assets().GetPool(book)) {
                chain.Require(MakeTx({y[ny++]}, SwapPools{Y, {book}, 30000, 1, CScript{}}, {std::nullopt}));
            }
        }
        chain.EndBlock();
        unsigned __int128 owed_x{0}, owed_y{0};
        for (const CTransaction& tx : placed) {
            const auto order{chain.Assets().GetOrder(OrderId(tx.GetHash()))};
            if (!order) continue;
            (order->sells0 == x0 ? owed_x : owed_y) += order->remaining;
            (order->sells0 == x0 ? owed_y : owed_x) += order->proceeds;
        }
        const auto pool{*chain.Assets().GetPool(book)};
        BOOST_CHECK(static_cast<unsigned __int128>(x0 ? pool.reserve0 : pool.reserve1) >= owed_x);
        BOOST_CHECK(static_cast<unsigned __int128>(x0 ? pool.reserve1 : pool.reserve0) >= owed_y);
    }
    // Every order settles, cancelled: what each is owed, paid.
    for (const CTransaction& tx : placed) {
        const auto order{chain.Assets().GetOrder(OrderId(tx.GetHash()))};
        if (!order) continue;
        std::vector<std::optional<Token>> outs;
        if (order->proceeds > 0) outs.emplace_back();
        if (order->remaining > 0) outs.emplace_back();
        chain.Require(MakeTx({COutPoint{tx.GetHash(), 0}}, SettleOrder{OrderId(tx.GetHash()), true, PAYEE}, outs));
    }
    BOOST_CHECK(amm::Abandoned(*chain.Assets().GetPool(book)));
}

BOOST_AUTO_TEST_CASE(worst_round)
{
    // The most a round of a book reads: MAX_BOOK_ROUND orders on each side, all at different prices. Its time bounds the
    // work of a book's hop (2 units of MAX_BLOCK_CURVE_WORK, as a concentrated pool's).
    Chain chain;
    const AssetId X{HashName("XXX")}, Y{HashName("YYY")};
    const auto split{[&](const std::vector<COutPoint>& coins, const AssetId& id, uint64_t each, size_t parts) {
        std::vector<COutPoint> out;
        for (const COutPoint& coin : coins) {
            const CTransaction tx{MakeTx({coin}, std::nullopt, std::vector<std::optional<Token>>(parts, AssetToken(id, each)))};
            chain.Require(tx);
            for (uint32_t i{0}; i < parts; ++i) out.emplace_back(tx.GetHash(), i);
        }
        return out;
    }};
    const auto x{split(chain.Issue("XXX", std::vector<uint64_t>(8, 32 * 1000)), X, 1000, 32)};
    const auto y{split(chain.Issue("YYY", std::vector<uint64_t>(8, 32 * 1000)), Y, 1000, 32)};
    for (size_t i{0}; i < MAX_BOOK_ROUND; ++i) {
        chain.Place(x[i], X, 1000, Y, 1000 + i, 1000);
        chain.Place(y[i], Y, 1000, X, 1000 - i / 2, 1000);
    }
    const uint256 book{BookId(X, Y)};
    const auto pool{*chain.Assets().GetPool(book)};
    std::map<uint256, BookOrder> resting;
    const auto start{std::chrono::steady_clock::now()};
    const auto cleared{chain.Assets().ClearBook(book, pool, resting, {dex::HopIn{X < Y, 500}, dex::HopIn{!(X < Y), 500}}, chain.height)};
    const auto us{std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()};
    BOOST_REQUIRE(cleared);
    BOOST_TEST_MESSAGE("a round of " << 2 * MAX_BOOK_ROUND << " orders: " << us << " us");
    BOOST_CHECK(!resting.empty());
    BOOST_CHECK_LT(us, 200000);
    // And the block's end crosses them: the book stays solvent.
    chain.EndBlock();
}

namespace {
/** `parts` coins of `each` of an asset, issued and split (at most 32 outputs a transaction). */
std::vector<COutPoint> Coins(Chain& chain, const std::string& name, uint64_t each, size_t parts)
{
    // One coin of it all, split as a tree (at most 32 outputs a transaction) into coins of `each`.
    const AssetId id{HashName(name)};
    std::vector<std::pair<COutPoint, size_t>> level{{chain.Issue(name, {each * parts})[0], parts}};
    std::vector<COutPoint> out;
    while (!level.empty()) {
        std::vector<std::pair<COutPoint, size_t>> next;
        for (const auto& [coin, n] : level) {
            if (n == 1) {
                out.push_back(coin);
                continue;
            }
            const size_t pieces{std::min<size_t>(32, n)};
            std::vector<size_t> sizes(pieces, n / pieces);
            for (size_t i{0}; i < n % pieces; ++i) ++sizes[i];
            std::vector<std::optional<Token>> outs;
            for (size_t k : sizes) outs.push_back(AssetToken(id, each * k));
            const CTransaction tx{MakeTx({coin}, std::nullopt, outs)};
            chain.Require(tx);
            for (uint32_t i{0}; i < pieces; ++i) next.emplace_back(COutPoint{tx.GetHash(), i}, sizes[i]);
        }
        level = std::move(next);
    }
    return out;
}
} // namespace

BOOST_AUTO_TEST_CASE(stuffed_with_expiring_orders)
{
    // A side filled with orders that expire: while they live, a worse order is refused (a better one would take the
    // worst's place); once they expired, the block's end sweeps them out, and new orders are placed and trade.
    Chain chain;
    const AssetId X{HashName("XXX")}, Y{HashName("YYY")};
    const auto x{Coins(chain, "XXX", 10, MAX_BOOK_SIDE + 4)};
    const auto y{Coins(chain, "YYY", 100, 2)};
    const int expiry{chain.height + 1};
    for (size_t i{0}; i < MAX_BOOK_SIDE; ++i) chain.Place(x[i], X, 10, Y, 5, 1, expiry);
    const uint256 book{BookId(X, Y)};
    const bool x0{X < Y};
    BOOST_CHECK_EQUAL(chain.Assets().BookSide(book, x0, MAX_BOOK_SIDE + 10, chain.height).size(), MAX_BOOK_SIDE);
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({x[MAX_BOOK_SIDE]}, PlaceOrder{X, Y, 10, 6, 1, chain.height + 100}, {std::nullopt})), "bad-ba-book-full");
    chain.EndBlock();
    // Still live in their last block.
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({x[MAX_BOOK_SIDE]}, PlaceOrder{X, Y, 10, 6, 1, chain.height + 100}, {std::nullopt})), "bad-ba-book-full");
    chain.EndBlock();
    // Expired and swept: out of the index, not counted; the book's open orders back to none.
    BOOST_CHECK(chain.Assets().BookSide(book, x0, MAX_BOOK_SIDE, chain.height).empty());
    BOOST_CHECK_EQUAL(chain.Assets().GetPool(book)->shares, MIN_LIQUIDITY);
    BOOST_CHECK_EQUAL(chain.Assets().BookCount(book, x0), 0U);
    BOOST_CHECK_EQUAL(bitassets::State{chain.store}.SweepBooks(chain.height), 0U);
    const CTransaction ask{chain.Place(x[MAX_BOOK_SIDE], X, 10, Y, 6, 1)};
    const CTransaction bid{chain.Place(y[0], Y, 100, X, 1, 10)};
    chain.EndBlock();
    BOOST_CHECK_EQUAL(chain.Assets().GetOrder(OrderId(ask.GetHash()))->remaining, 0U);
    BOOST_CHECK_EQUAL(chain.Assets().GetOrder(OrderId(bid.GetHash()))->got, 10U);
}

BOOST_AUTO_TEST_CASE(dust_refused)
{
    Chain chain;
    const AssetId X{HashName("XXX")}, Y{HashName("YYY")};
    const AssetId CHN{};
    const auto x{chain.Issue("XXX", {1'000'000, 100}, /*decimals=*/2)};
    const auto y{chain.Issue("YYY", {1})};
    const int h{chain.height};
    // What it buys at its limit: nothing.
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({x[0]}, PlaceOrder{X, CHN, 1, 1, 2, h + 10}, {std::nullopt})), "bad-ba-order-dust");
    // The CHN side under 0.01 CHN, either way: refused; at it: placed.
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({x[0]}, PlaceOrder{X, CHN, 999'999, 1, 1, h + 10}, {std::nullopt})), "bad-ba-order-dust");
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({}, PlaceOrder{CHN, X, 999'999, 1, 1, h + 10}, {std::nullopt}, 999'999)), "bad-ba-order-dust");
    chain.Place(x[0], X, 1'000'000, CHN, 1, 1);
    // Without CHN: a whole unit of what it sells at least (X has 2 decimals: 100 units; Y none: 1).
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({x[1]}, PlaceOrder{X, Y, 99, 1, 1, h + 10}, {std::nullopt})), "bad-ba-order-dust");
    chain.Place(x[1], X, 100, Y, 1, 1);
    chain.Place(y[0], Y, 1, X, 100, 1);
}

BOOST_AUTO_TEST_CASE(full_side_evicts_its_worst)
{
    Chain chain;
    const AssetId X{HashName("XXX")}, Y{HashName("YYY")};
    const auto x{Coins(chain, "XXX", 10, MAX_BOOK_SIDE + 3)};
    chain.Issue("YYY", {1});
    std::vector<CTransaction> placed;
    // Limits 2 .. : the worst is the last, the highest limit (the highest id among the same limit).
    for (size_t i{0}; i < MAX_BOOK_SIDE; ++i) placed.push_back(chain.Place(x[i], X, 10, Y, 2 + i / 10, 1));
    const uint256 book{BookId(X, Y)};
    const bool x0{X < Y};
    BOOST_CHECK_EQUAL(chain.Assets().BookCount(book, x0), MAX_BOOK_SIDE);
    BOOST_CHECK_EQUAL(chain.Assets().BookCount(book, !x0), 0U);
    chain.EndBlock();
    const auto live{chain.Assets().BookSide(book, x0, MAX_BOOK_SIDE, chain.height)};
    const uint256 worst{live.back().first};
    const uint64_t shares{chain.Assets().GetPool(book)->shares};
    // No better than the worst: refused. Better but smaller than what the worst has left: refused too (no cheap griefing).
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({x[MAX_BOOK_SIDE]}, PlaceOrder{X, Y, 10, 2 + (MAX_BOOK_SIDE - 1) / 10, 1, chain.height + 100}, {std::nullopt})), "bad-ba-book-full");
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({x[MAX_BOOK_SIDE]}, PlaceOrder{X, Y, 9, 1, 1, chain.height + 100}, {std::nullopt, AssetToken(X, 1)})), "bad-ba-book-full");
    // Better and no smaller: placed; the worst out of the book, flagged, the count the same. The block undone: all back.
    chain.store.TakeUndo();
    const CTransaction better{chain.Place(x[MAX_BOOK_SIDE], X, 10, Y, 1, 1)};
    BOOST_CHECK_EQUAL(chain.Assets().GetPool(book)->shares, shares);
    BOOST_CHECK_EQUAL(chain.Assets().BookCount(book, x0), MAX_BOOK_SIDE);
    BOOST_CHECK_EQUAL(chain.Assets().GetOrder(worst)->evicted, chain.height);
    const auto after{chain.Assets().BookSide(book, x0, MAX_BOOK_SIDE + 1, chain.height)};
    BOOST_CHECK_EQUAL(after.size(), MAX_BOOK_SIDE);
    BOOST_CHECK(after.front().first == OrderId(better.GetHash()));
    BOOST_CHECK(std::none_of(after.begin(), after.end(), [&](const auto& o) { return o.first == worst; }));
    chain.EndBlock();
    {
        const sidechain::StoreUndo undo{chain.store.TakeUndo()};
        chain.store.Revert(undo);
        BOOST_CHECK(!chain.Assets().GetOrder(OrderId(better.GetHash())));
        BOOST_CHECK_EQUAL(chain.Assets().GetOrder(worst)->evicted, 0);
        BOOST_CHECK_EQUAL(chain.Assets().BookCount(book, x0), MAX_BOOK_SIDE);
        BOOST_CHECK(chain.Assets().BookSide(book, x0, MAX_BOOK_SIDE + 1, chain.height - 1).back().first == worst);
        BOOST_CHECK_EQUAL(chain.Assets().GetPool(book)->shares, shares);
    }
    // Again, and on: its holder takes it all back.
    chain.height -= 1;
    chain.Place(x[MAX_BOOK_SIDE], X, 10, Y, 1, 1);
    chain.EndBlock();
    const auto it{std::find_if(placed.begin(), placed.end(), [&](const CTransaction& tx) { return OrderId(tx.GetHash()) == worst; })};
    BOOST_REQUIRE(it != placed.end());
    const CTransaction back{MakeTx({COutPoint{it->GetHash(), 0}}, SettleOrder{worst, false, PAYEE}, {std::nullopt})};
    chain.Require(back);
    BOOST_CHECK(chain.TokenAt(COutPoint{back.GetHash(), 0}) == AssetToken(X, 10));
    BOOST_CHECK(!chain.Assets().GetOrder(worst));
    BOOST_CHECK_EQUAL(chain.Assets().GetPool(book)->shares, shares);
    BOOST_CHECK_EQUAL(chain.Assets().BookCount(book, x0), MAX_BOOK_SIDE);
    // Cancelling a live one: one less.
    const CTransaction cancel{MakeTx({COutPoint{placed[0].GetHash(), 0}}, SettleOrder{OrderId(placed[0].GetHash()), true, PAYEE}, {std::nullopt})};
    chain.Require(cancel);
    BOOST_CHECK_EQUAL(chain.Assets().BookCount(book, x0), MAX_BOOK_SIDE - 1);
}

BOOST_AUTO_TEST_CASE(full_of_unswept_expired_orders)
{
    // A side full of orders that expired but are not swept yet (a backlog: more expired at once than a block's end sweeps,
    // in other books too): an order placed there takes them out itself, and is placed. No other book's orders keep it full.
    Chain chain;
    const AssetId X{HashName("XXX")}, Y{HashName("YYY")};
    const auto x{Coins(chain, "XXX", 10, MAX_BOOK_SIDE + 1)};
    chain.Issue("YYY", {1});
    for (size_t i{0}; i < MAX_BOOK_SIDE; ++i) chain.Place(x[i], X, 10, Y, 2, 1, chain.height);
    const uint256 book{BookId(X, Y)};
    const bool x0{X < Y};
    BOOST_CHECK_EQUAL(chain.Assets().BookCount(book, x0), MAX_BOOK_SIDE);
    const uint64_t shares{chain.Assets().GetPool(book)->shares};
    // The next block, before any sweep: all of them expired, all still in the index.
    chain.height += 1;
    BOOST_CHECK(chain.Assets().BookSide(book, x0, MAX_BOOK_SIDE, chain.height).empty());
    BOOST_CHECK_EQUAL(chain.Assets().BookEntries(book, x0).size(), MAX_BOOK_SIDE);
    const CTransaction placed{chain.Place(x[MAX_BOOK_SIDE], X, 10, Y, 3, 1)};
    BOOST_CHECK_EQUAL(chain.Assets().BookCount(book, x0), 1U);
    BOOST_CHECK_EQUAL(chain.Assets().BookEntries(book, x0).size(), 1U);
    BOOST_CHECK_EQUAL(chain.Assets().GetPool(book)->shares, shares - MAX_BOOK_SIDE + 1);
    // The block's end: the global sweep finds them out of the index already; the count stays right.
    chain.EndBlock();
    BOOST_CHECK_EQUAL(chain.Assets().BookCount(book, x0), 1U);
    BOOST_CHECK(chain.Assets().GetOrder(OrderId(placed.GetHash())));
}

BOOST_AUTO_TEST_CASE(layout_of_books)
{
    // A store with orders written before the books' counts (no layout marker) is not current: the node derives it again
    // from the blocks, which rebuilds the counts exactly. One with the marker, or without orders, is current.
    {
        Chain chain;
        BOOST_CHECK(StoreLayoutCurrent(chain.store));
        const auto x{chain.Issue("XXX", {10})};
        chain.Issue("YYY", {1});
        chain.Place(x[0], HashName("XXX"), 10, HashName("YYY"), 2, 1);
        BOOST_CHECK(StoreLayoutCurrent(chain.store));
        chain.store.Erase(sidechain::StoreBytes{0x9d});
        BOOST_CHECK(!StoreLayoutCurrent(chain.store));
    }
    // A record without `evicted` (written before it) reads as never evicted.
    BookOrder order;
    order.amount = 5;
    order.evicted = 7;
    DataStream full{};
    full << order;
    std::vector<std::byte> bytes{full.begin(), full.end()};
    bytes.resize(bytes.size() - 4);
    DataStream old{bytes};
    BookOrder read;
    old >> read;
    BOOST_CHECK_EQUAL(read.amount, 5U);
    BOOST_CHECK_EQUAL(read.evicted, 0);
}

BOOST_AUTO_TEST_CASE(sweep_bounded)
{
    // A block's end sweeps MAX_BOOK_SWEEP expired orders at most, oldest first; the rest at the next.
    Chain chain;
    const AssetId X{HashName("XXX")}, Y{HashName("YYY")};
    const auto x{Coins(chain, "XXX", 10, 300)};
    chain.Issue("YYY", {1});
    for (size_t i{0}; i < 300; ++i) chain.Place(x[i], X, 10, Y, 2, 1, chain.height);
    chain.EndBlock();
    // A smaller budget than the expired orders: that many, then the rest.
    Chain other;
    const auto z{Coins(other, "XXX", 10, 300)};
    other.Issue("YYY", {1});
    for (size_t i{0}; i < 300; ++i) other.Place(z[i], X, 10, Y, 2, 1, other.height);
    bitassets::State writable{other.store};
    BOOST_CHECK_EQUAL(writable.SweepBooks(other.height, 120), 120U);
    BOOST_CHECK_EQUAL(writable.SweepBooks(other.height, 1000), 180U);
    BOOST_CHECK_EQUAL(writable.SweepBooks(other.height, 1000), 0U);
    BOOST_CHECK(chain.Assets().BookSide(BookId(X, Y), X < Y, 1000, 0).empty());
}

namespace {
/** Regtest with the DEX from the first block and smaller payouts from block 102 (the Chain starts at 100). */
struct SmallPayoutsSetup : BasicTestingSetup {
    SmallPayoutsSetup() : BasicTestingSetup{ChainType::REGTEST, {.extra_args = {"-sidechainslot=7", "-testactivationheight=dex@0", "-testactivationheight=dexcurves@0", "-testactivationheight=smallpayouts@102"}}} {}
};
} // namespace

BOOST_FIXTURE_TEST_CASE(smaller_orders_and_payouts_from_their_height, SmallPayoutsSetup)
{
    // The rules on both sides of the height: 0.0005 ECX paid out at least and 0.01 ECX an order before, 0.00005 for both from it.
    BOOST_CHECK_EQUAL(MinChnPayout(101), MIN_CHN_PAYOUT_BEFORE_SMALL);
    BOOST_CHECK_EQUAL(MinBookChn(101), MIN_BOOK_CHN_BEFORE_SMALL);
    BOOST_CHECK_EQUAL(MinChnPayout(102), MIN_CHN_PAYOUT_SMALL);
    BOOST_CHECK_EQUAL(MinBookChn(102), MIN_CHN_PAYOUT_SMALL);
    BOOST_CHECK_EQUAL(MIN_CHN_PAYOUT_SMALL, 5'000U);
    Chain chain;
    const AssetId X{HashName("XXX")};
    const AssetId CHN{};
    const auto x{chain.Issue("XXX", {10'000, 5'000, 1'000'000}, /*decimals=*/2)};
    // Before: an order selling X for 0.0001 ECX is dust (under 0.01 ECX), one paying 0.0001 ECX too.
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({x[0]}, PlaceOrder{X, CHN, 10'000, 1, 1, chain.height + 10}, {std::nullopt})), "bad-ba-order-dust");
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({}, PlaceOrder{CHN, X, 10'000, 1, 1, chain.height + 10}, {std::nullopt}, 10'000)), "bad-ba-chn-dust");
    chain.EndBlock();
    chain.EndBlock();
    BOOST_REQUIRE_EQUAL(chain.height, 102);
    // From the height: 0.00005 ECX (5 000 sat) either way; less still refused.
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({x[1]}, PlaceOrder{X, CHN, 5'000, 4'999, 5'000, chain.height + 10}, {std::nullopt})), "bad-ba-order-dust");
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({}, PlaceOrder{CHN, X, 4'999, 1, 1, chain.height + 10}, {std::nullopt}, 4'999)), "bad-ba-chn-dust");
    const CTransaction sell{chain.Place(x[1], X, 5'000, CHN, 1, 1)};
    const CTransaction buy{MakeTx({}, PlaceOrder{CHN, X, 5'000, 1, 1, chain.height + 10}, {std::nullopt}, 5'000)};
    chain.Require(buy);
    chain.EndBlock();
    // They cross: the seller's 5 000 sat of ECX are paid out by the coinbase (the least payout now), none refused.
    bool paid{false};
    for (const CTxOut& out : chain.paid) paid |= out.nValue == 5'000;
    const auto order{chain.Assets().GetOrder(OrderId(sell.GetHash()))};
    BOOST_CHECK(paid || (order && order->proceeds == 5'000));
}

BOOST_AUTO_TEST_SUITE_END()
