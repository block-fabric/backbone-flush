// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Layers' DEX (bitassets/dex.h): the clearing's arithmetic, the price accumulators, the oracles' signatures,
// and routes, batches, accumulators and oracle contracts on a sidechain state of their own.

#include <addresstype.h>
#include <arith_uint256.h>
#include <bitassets/dex.h>
#include <bitassets/state.h>
#include <chainparams.h>
#include <consensus/params.h>
#include <key.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <sidechain/state.h>
#include <sidechain/store.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <map>
#include <string>
#include <vector>

using namespace bitassets;

namespace {
const CScript HOLDER{GetScriptForDestination(WitnessV0KeyHash{uint160{}})};
const CScript OTHER{GetScriptForDestination(WitnessV0KeyHash{uint160{std::vector<unsigned char>(20, 7)}})};
const CScript THIRD{GetScriptForDestination(WitnessV0KeyHash{uint160{std::vector<unsigned char>(20, 9)}})};
int g_nonce{0};

Token AssetToken(const AssetId& id, uint64_t amount) { return Token{Token::Kind::ASSET, id, amount}; }

CTransaction MakeTx(const std::vector<COutPoint>& inputs, std::optional<Operation> op, const std::vector<std::optional<Token>>& outs, CAmount chn_in = 0,
                    const std::vector<CScript>& scripts = {})
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(++g_nonce % 250 + 1)}), static_cast<uint32_t>(100000 + g_nonce)});
    for (const COutPoint& in : inputs) tx.vin.emplace_back(in);
    Marker marker;
    marker.operation = std::move(op);
    for (size_t i{0}; i < outs.size(); ++i) {
        marker.outputs.push_back({static_cast<uint32_t>(tx.vout.size()), outs[i]});
        tx.vout.emplace_back(0, i < scripts.size() ? scripts[i] : HOLDER);
    }
    tx.vout.emplace_back(chn_in, MarkerScript(marker));
    return CTransaction{tx};
}

/** A chain's state on a store of its own, with the DEX's rules from `dex_height`. */
struct Chain {
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State state{store};
    Consensus::SidechainParams params;
    int height{100};
    uint32_t time{1'800'000'000};
    std::vector<CTxOut> payouts;
    std::vector<CTxOut> refunds;
    //! What the last block's transactions paid out.
    std::vector<CTxOut> paid;

    explicit Chain(int dex_height = 0)
    {
        params.bitassets_reveal_depth = 0;
        params.dex_height = dex_height;
    }
    bool Apply(const CTransaction& tx, std::string& reason)
    {
        return state.ApplyTx(tx, height, params, payouts, refunds, reason, /*main_pending=*/false, /*peg_step=*/true);
    }
    void Require(const CTransaction& tx)
    {
        std::string reason;
        BOOST_REQUIRE_MESSAGE(Apply(tx, reason), reason);
    }
    /** The block's end: its orders clear, `seconds` to the next. */
    void EndBlock(uint32_t seconds = 15)
    {
        state.FinishTxs(height, time, params, payouts);
        paid = payouts;
        payouts.clear();
        refunds.clear();
        ++height;
        time += seconds;
    }
    bitassets::State Assets() const { return state.BitAssets(); }
    std::optional<Token> TokenAt(const COutPoint& out) const { return Assets().GetToken(out); }
    COutPoint Issue(const std::string& name, uint64_t supply)
    {
        const AssetId id{HashName(name)};
        const uint256 nonce{HashName("nonce " + name)};
        const CTransaction reserved{MakeTx({}, Reserve{ReservationCommitment(id, nonce, HOLDER)}, {Token{Token::Kind::RESERVATION, uint256{}, 1}})};
        Require(reserved);
        Register reg;
        reg.name = id;
        reg.nonce = nonce;
        reg.supply = supply;
        reg.text = name;
        const CTransaction tx{MakeTx({COutPoint{reserved.GetHash(), 0}}, reg, {Token{Token::Kind::CONTROL, id, 1}, AssetToken(id, supply)})};
        Require(tx);
        return COutPoint{tx.GetHash(), 1};
    }
    /** Splits the asset coins at `from` (`total` of them) into outputs of `amounts` and the rest (last). */
    std::vector<COutPoint> Split(const COutPoint& from, const AssetId& id, uint64_t total, const std::vector<uint64_t>& amounts)
    {
        std::vector<std::optional<Token>> outs;
        uint64_t used{0};
        for (uint64_t a : amounts) {
            outs.push_back(AssetToken(id, a));
            used += a;
        }
        if (total > used) outs.push_back(AssetToken(id, total - used));
        const CTransaction tx{MakeTx({from}, std::nullopt, outs)};
        Require(tx);
        std::vector<COutPoint> result;
        for (uint32_t i{0}; i < outs.size(); ++i) result.emplace_back(tx.GetHash(), i);
        return result;
    }
    /** A pool of two assets (or of an asset and CHN: `b_coins` null), opened with what the coins carry. */
    void MakePool(const COutPoint& a_coins, const AssetId& a, uint64_t amount_a, const std::optional<COutPoint>& b_coins, const AssetId& b, uint64_t amount_b)
    {
        std::vector<COutPoint> ins{a_coins};
        if (b_coins) ins.push_back(*b_coins);
        Require(MakeTx(ins, AddLiquidity{a, b, amount_a, amount_b, 1}, {std::nullopt, std::nullopt}, b.IsNull() ? static_cast<CAmount>(amount_b) : 0));
    }
    Pool PoolOf(const AssetId& a, const AssetId& b) const { return *Assert(Assets().FindPool(a, b)); }
};

/** What `amount` of `in` gives alone, as amm::SwapOut does. */
uint64_t Alone(const Pool& pool, const AssetId& in, uint64_t amount)
{
    const bool zero_in{pool.asset0 == in};
    return amm::SwapOut(zero_in ? pool.reserve0 : pool.reserve1, zero_in ? pool.reserve1 : pool.reserve0, amount);
}
} // namespace

namespace {
/** The security and protocol parts of the fee on `in` at `fee` (the network's shares): what leaves the pool. */
uint64_t Taken(uint64_t in, uint32_t fee = 3000)
{
    const auto& rules{Params().GetConsensus().sidechain};
    const FeeParts parts{SwapFeeParts(static_cast<uint64_t>(static_cast<unsigned __int128>(in) * fee / FEE_DENOMINATOR), rules.dex_fee_security_share, rules.dex_fee_protocol_share)};
    return parts.security + parts.protocol;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(dex_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(dex_tables_take_their_own_bytes)
{
    // No two tables of the state share a key byte (the DEX's 0x80-0x89 among them; the node refuses to start otherwise).
    BOOST_CHECK(sidechain::DuplicateTableIds().empty());
}

BOOST_AUTO_TEST_CASE(clearing_one_order_is_the_curve)
{
    // Alone, an order gets exactly what the rules before the DEX gave it.
    FastRandomContext rng{/*fDeterministic=*/true};
    for (int i{0}; i < 2000; ++i) {
        const uint64_t r0{1 + rng.randrange(MAX_AMOUNT / 2 / (i % 2 ? 1 : 1'000'000))}, r1{1 + rng.randrange(MAX_AMOUNT / (i % 3 ? 1'000 : 1))};
        const uint64_t in{1 + rng.randrange(r0)};
        const auto cleared{dex::ClearPool(r0, r1, {dex::HopIn{true, in}})};
        BOOST_REQUIRE(cleared);
        BOOST_CHECK_EQUAL(cleared->outs[0], amm::SwapOut(r0, r1, in));
        BOOST_CHECK_EQUAL(cleared->reserve0, r0 + in);
        BOOST_CHECK_EQUAL(cleared->reserve1, r1 - cleared->outs[0]);
    }
}

BOOST_AUTO_TEST_CASE(clearing_uniform_price_and_conservation)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    for (int round{0}; round < 300; ++round) {
        const uint64_t r0{1'000'000 + rng.randrange(1'000'000'000'000)}, r1{1'000'000 + rng.randrange(1'000'000'000'000)};
        std::vector<dex::HopIn> orders;
        uint64_t in0{0}, in1{0};
        const int count{1 + static_cast<int>(rng.randrange(20))};
        for (int i{0}; i < count; ++i) {
            const bool zero_in{rng.randbool()};
            const uint64_t amount{1 + rng.randrange((zero_in ? r0 : r1) / 10)};
            orders.push_back({zero_in, amount});
            (zero_in ? in0 : in1) += amount;
        }
        const auto cleared{dex::ClearPool(r0, r1, orders)};
        BOOST_REQUIRE(cleared);
        // Conservation: what the pool holds after is what it held, plus what went in, less what came out.
        uint64_t out0{0}, out1{0};
        for (size_t i{0}; i < orders.size(); ++i) (orders[i].zero_in ? out1 : out0) += cleared->outs[i];
        BOOST_CHECK_EQUAL(cleared->reserve0, r0 + in0 - out0);
        BOOST_CHECK_EQUAL(cleared->reserve1, r1 + in1 - out1);
        // The product of the reserves never falls.
        BOOST_CHECK(arith_uint256{cleared->reserve0} * arith_uint256{cleared->reserve1} >= arith_uint256{r0} * arith_uint256{r1});
        // One price for all: each order's output is its input less the fee at P (asset0 sellers) or 1/P,
        // down to the unit: floor(a·997·num / (1000·den)).
        const arith_uint256 num{arith_uint256{r1} * 1000 + arith_uint256{in1} * 997}, den{arith_uint256{r0} * 1000 + arith_uint256{in0} * 997};
        for (size_t i{0}; i < orders.size(); ++i) {
            const arith_uint256 paid{arith_uint256{orders[i].amount} * 997};
            const arith_uint256 expected{orders[i].zero_in ? paid * num / (den * 1000) : paid * den / (num * 1000)};
            BOOST_CHECK_EQUAL(cleared->outs[i], expected.GetLow64());
        }
        // Two orders of the same side and size get the same.
        const auto twice{dex::ClearPool(r0, r1, {dex::HopIn{true, 5000}, dex::HopIn{true, 5000}})};
        BOOST_REQUIRE(twice);
        BOOST_CHECK_EQUAL(twice->outs[0], twice->outs[1]);
    }
    // Opposite orders of the same worth trade with each other at the pool's price, less the fee each.
    const auto opposite{dex::ClearPool(1'000'000'000, 2'000'000'000, {dex::HopIn{true, 1'000'000}, dex::HopIn{false, 2'000'000}})};
    BOOST_REQUIRE(opposite);
    BOOST_CHECK_EQUAL(opposite->outs[0], 1'994'000U); // 2 per unit, less 0.3 %
    BOOST_CHECK_EQUAL(opposite->outs[1], 997'000U);
    // Better than one after the other: the second would pay the first's price impact.
    BOOST_CHECK_GT(opposite->outs[0], amm::SwapOut(1'000'000'000, 2'000'000'000, 1'000'000) - 1);
    // Refused: a reserve of nothing, a sum past MAX_AMOUNT, an order of nothing.
    BOOST_CHECK(!dex::ClearPool(0, 10, {dex::HopIn{true, 1}}));
    BOOST_CHECK(!dex::ClearPool(MAX_AMOUNT - 10, 10, {dex::HopIn{true, 11}}));
    BOOST_CHECK(!dex::ClearPool(MAX_AMOUNT / 2, 10, {dex::HopIn{true, MAX_AMOUNT / 2}, dex::HopIn{true, MAX_AMOUNT / 2}}));
    BOOST_CHECK(!dex::ClearPool(10, 10, {dex::HopIn{true, 0}}));
}

BOOST_AUTO_TEST_CASE(twap_accumulators_wrap)
{
    // UQ64.64: 3 / 2 is 1.5 · 2^64.
    BOOST_CHECK(UintToArith256(dex::PriceQ64(3, 2)) == (arith_uint256{3} << 63));
    BOOST_CHECK(dex::PriceQ64(5, 0).IsNull());
    // The largest price (reserves of 2^63 - 1 against 1) times the most seconds of a uint32 fits in 256 bits many times over.
    const uint256 top{dex::PriceQ64(MAX_AMOUNT, 1)};
    BOOST_CHECK(UintToArith256(dex::Accumulate(uint256{}, top, std::numeric_limits<uint32_t>::max())) == UintToArith256(top) * std::numeric_limits<uint32_t>::max());
    // Modulo 2^256: the sum wraps, and the difference of two readings is what was added between them.
    const uint256 near_top{ArithToUint256(~arith_uint256{0} - 9)};
    const uint256 price{ArithToUint256(arith_uint256{7})};
    const uint256 wrapped{dex::Accumulate(near_top, price, 3)};
    BOOST_CHECK(UintToArith256(wrapped) == arith_uint256{11}); // 2^256 - 10 + 21
    BOOST_CHECK(UintToArith256(dex::CumulativeDelta(wrapped, near_top)) == arith_uint256{21});
    BOOST_CHECK_EQUAL(dex::AveragePrice(dex::CumulativeDelta(dex::Accumulate(near_top, dex::PriceQ64(3, 2), 10), near_top), 10), 1.5);
    // From an observation on: the price its reserves give, times the seconds after it.
    Observation obs;
    obs.time = 1000;
    obs.price0_cumulative = near_top;
    obs.reserve0 = 2;
    obs.reserve1 = 3;
    const auto [c0, c1]{dex::CumulativeAt(obs, 1010)};
    BOOST_CHECK(c0 == dex::Accumulate(near_top, dex::PriceQ64(3, 2), 10));
    BOOST_CHECK(c1 == dex::Accumulate(uint256{}, dex::PriceQ64(2, 3), 10));
    BOOST_CHECK(dex::CumulativeAt(obs, 900).first == near_top);
}

BOOST_AUTO_TEST_CASE(oracle_signatures)
{
    CKey key;
    key.MakeNewKey(true);
    const XOnlyPubKey xonly{key.GetPubKey()};
    uint256 pubkey;
    std::copy(xonly.begin(), xonly.end(), pubkey.begin());
    const uint256 oracle{dex::OracleId(pubkey)}, event{dex::EventId("BTCUSD 2026-12-31")};
    BOOST_CHECK(event != dex::EventId("BTCUSD 2027-01-01"));
    std::array<unsigned char, 64> sig;
    BOOST_REQUIRE(key.SignSchnorr(dex::AttestationHash(oracle, event, 123456, 500), sig, nullptr, uint256{}));
    BOOST_CHECK(dex::CheckAttestation(pubkey, oracle, event, 123456, 500, sig));
    // Bound to every part: another value, height, event or oracle, or another key, is not.
    BOOST_CHECK(!dex::CheckAttestation(pubkey, oracle, event, 123457, 500, sig));
    BOOST_CHECK(!dex::CheckAttestation(pubkey, oracle, event, 123456, 501, sig));
    BOOST_CHECK(!dex::CheckAttestation(pubkey, oracle, dex::EventId("other"), 123456, 500, sig));
    BOOST_CHECK(!dex::CheckAttestation(pubkey, dex::OracleId(uint256::ONE), event, 123456, 500, sig));
    CKey other;
    other.MakeNewKey(true);
    const XOnlyPubKey other_xonly{other.GetPubKey()};
    uint256 other_pubkey;
    std::copy(other_xonly.begin(), other_xonly.end(), other_pubkey.begin());
    BOOST_CHECK(!dex::CheckAttestation(other_pubkey, oracle, event, 123456, 500, sig));
    sig[10] ^= 1;
    BOOST_CHECK(!dex::CheckAttestation(pubkey, oracle, event, 123456, 500, sig));
    // The message, as an external signer computes it: SHA256 of the serialized tag, ids and value (functional test too).
    BOOST_CHECK_EQUAL(dex::AttestationHash(uint256{}, uint256{}, 0, 0).GetHex(), dex::AttestationHash(uint256{}, uint256{}, 0, 0).GetHex());
}

BOOST_AUTO_TEST_CASE(dex_inactive_rules_unchanged)
{
    // Before the DEX's height: a route is not a marker the rules know, a swap trades at once.
    Chain chain{/*dex_height=*/1000};
    const AssetId GOLD{HashName("GOLD")}, SILVER{HashName("SILVER")};
    const COutPoint gold{chain.Issue("GOLD", 10'000'000)}, silver{chain.Issue("SILVER", 10'000'000)};
    const auto g{chain.Split(gold, GOLD, 10'000'000, {1'000'000, 1000})};
    const auto s{chain.Split(silver, SILVER, 10'000'000, {2'000'000})};
    chain.MakePool(g[0], GOLD, 1'000'000, s[0], SILVER, 2'000'000);
    std::string reason;
    BOOST_CHECK(!chain.Apply(MakeTx({g[1]}, Route{{GOLD, SILVER}, 1000, 1, CScript{}}, {std::nullopt}), reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-marker");
    const uint64_t quote{Alone(chain.PoolOf(GOLD, SILVER), GOLD, 1000)};
    const CTransaction swap{MakeTx({g[1]}, Swap{GOLD, 1000, SILVER, 1, CScript{}}, {std::nullopt})};
    chain.Require(swap);
    // At once, not at the block's end.
    BOOST_CHECK_EQUAL(chain.TokenAt(COutPoint{swap.GetHash(), 0})->amount, quote);
    chain.EndBlock();
    BOOST_CHECK(!chain.Assets().GetAccumulator(PoolId(GOLD, SILVER)));
}

BOOST_AUTO_TEST_CASE(dex_routes_two_and_four_hops)
{
    Chain chain;
    std::vector<AssetId> ids;
    std::vector<std::vector<COutPoint>> coins;
    for (const char* name : {"A", "B", "C", "D", "E"}) {
        ids.push_back(HashName(name));
        coins.push_back(chain.Split(chain.Issue(name, 100'000'000), ids.back(), 100'000'000, {10'000'000, 10'000'000, 50'000, 50'000}));
    }
    // A-B, B-C, C-D, D-E, each at its own price.
    for (size_t i{0}; i + 1 < ids.size(); ++i) {
        chain.MakePool(coins[i][i == 0 ? 0 : 1], ids[i], 10'000'000, coins[i + 1][0], ids[i + 1], 10'000'000);
    }
    chain.EndBlock();
    const auto quote2{dex::QuoteRoute(chain.Assets(), {ids[0], ids[1], ids[2]}, 50'000)};
    BOOST_REQUIRE(quote2);
    // Hop by hop, each the curve of its pool.
    BOOST_CHECK_EQUAL((*quote2)[0], Alone(chain.PoolOf(ids[0], ids[1]), ids[0], 50'000));
    BOOST_CHECK_EQUAL((*quote2)[1], Alone(chain.PoolOf(ids[1], ids[2]), ids[1], (*quote2)[0]));
    const CTransaction two{MakeTx({coins[0][2]}, Route{{ids[0], ids[1], ids[2]}, 50'000, quote2->back(), CScript{}}, {std::nullopt})};
    chain.Require(two);
    // An order: nothing until the block's end, and its result output cannot be spent in the block.
    BOOST_CHECK(!chain.TokenAt(COutPoint{two.GetHash(), 0}));
    std::string reason;
    BOOST_CHECK(!chain.Apply(MakeTx({COutPoint{two.GetHash(), 0}}, std::nullopt, {}), reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-result-pending");
    chain.EndBlock();
    BOOST_CHECK(chain.TokenAt(COutPoint{two.GetHash(), 0}) == AssetToken(ids[2], quote2->back()));

    // Four hops, A to E, the least out its quote: alone in its block, it gets exactly that.
    const auto quote4{dex::QuoteRoute(chain.Assets(), ids, 50'000)};
    BOOST_REQUIRE(quote4);
    const Pool before_de{chain.PoolOf(ids[3], ids[4])};
    const CTransaction four{MakeTx({coins[0][3]}, Route{ids, 50'000, quote4->back(), CScript{}}, {std::nullopt})};
    chain.Require(four);
    chain.EndBlock();
    BOOST_CHECK(chain.TokenAt(COutPoint{four.GetHash(), 0}) == AssetToken(ids[4], quote4->back()));
    const Pool after_de{chain.PoolOf(ids[3], ids[4])};
    const bool d_first{after_de.asset0 == ids[3]};
    // What came in, less the fee's security and protocol parts, which leave the pool.
    BOOST_CHECK_EQUAL((d_first ? after_de.reserve0 : after_de.reserve1) - (d_first ? before_de.reserve0 : before_de.reserve1), (*quote4)[2] - Taken((*quote4)[2]));
    BOOST_CHECK_EQUAL((d_first ? before_de.reserve1 : before_de.reserve0) - (d_first ? after_de.reserve1 : after_de.reserve0), quote4->back());
    // The route search finds A to E through the four pools (the only way).
    const auto found{dex::FindRoutes(chain.Assets(), ids[0], ids[4], 1000, 4)};
    BOOST_REQUIRE_EQUAL(found.size(), 1U);
    BOOST_CHECK(found[0].path == ids);
    BOOST_CHECK(dex::FindRoutes(chain.Assets(), ids[0], ids[4], 1000, 3).empty());
    // Malformed: a pool twice, an asset with no pool, more than four hops.
    BOOST_CHECK(!chain.Apply(MakeTx({coins[1][2]}, Route{{ids[1], ids[0], ids[1]}, 1000, 1, CScript{}}, {std::nullopt}), reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-route");
    BOOST_CHECK(!chain.Apply(MakeTx({coins[1][2]}, Route{{ids[1], ids[3]}, 1000, 1, CScript{}}, {std::nullopt}), reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-no-pool");
    std::vector<AssetId> six{ids};
    six.push_back(HashName("F"));
    BOOST_CHECK(!chain.Apply(MakeTx({coins[0][1]}, Route{six, 1000, 1, CScript{}}, {std::nullopt}), reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-route");
    // An order has one result output.
    BOOST_CHECK(!chain.Apply(MakeTx({coins[1][2]}, Route{{ids[1], ids[0]}, 1000, 1, CScript{}}, {}), reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-results");
}

BOOST_AUTO_TEST_CASE(dex_batch_uniform_price_refund_and_undo)
{
    Chain chain;
    const AssetId GOLD{HashName("GOLD")};
    const COutPoint gold_all{chain.Issue("GOLD", 100'000'000)};
    const auto gold{chain.Split(gold_all, GOLD, 100'000'000, {10'000'000, 1'000'000, 1'000'000, 1'000'000})};
    // GOLD-CHN: 10 GOLD (of 6 decimals... units) against 10 CHN.
    chain.MakePool(gold[0], GOLD, 10'000'000, std::nullopt, CHN, 1'000'000'000);
    chain.EndBlock();
    const Pool start{chain.PoolOf(GOLD, CHN)};
    const bool chn_first{start.asset0 == CHN};

    // Opposing orders in one block: CHN for GOLD and GOLD for CHN, at one price.
    const CTransaction buy{MakeTx({}, Swap{CHN, 100'000'000, GOLD, 1, OTHER}, {std::nullopt}, 100'000'000)};
    const CTransaction sell{MakeTx({gold[1]}, Swap{GOLD, 1'000'000, CHN, 1, OTHER}, {std::nullopt})};
    chain.Require(buy);
    chain.Require(sell);
    BOOST_CHECK_EQUAL(chain.payouts.size(), 2U); // their places: the buy's CHN back if it misses, the sell's CHN out
    const sidechain::StoreUndo before_block{chain.store.TakeUndo()};
    (void)before_block;
    const uint256 hash_before{chain.state.Hash()};
    chain.EndBlock();
    const auto expected{dex::ClearPool(start.reserve0, start.reserve1, {dex::HopIn{chn_first, 100'000'000}, dex::HopIn{!chn_first, 1'000'000}})};
    BOOST_REQUIRE(expected);
    BOOST_CHECK(chain.TokenAt(COutPoint{buy.GetHash(), 0}) == AssetToken(GOLD, expected->outs[0]));
    BOOST_CHECK(!chain.TokenAt(COutPoint{sell.GetHash(), 0})); // CHN out: by the coinbase
    // The buy filled: no CHN back, its place gone; the sell's CHN out.
    BOOST_REQUIRE_EQUAL(chain.paid.size(), 1U);
    BOOST_CHECK(chain.paid[0] == CTxOut(static_cast<CAmount>(expected->outs[1]), OTHER));
    // The reserves: the round's, less the security and protocol parts of each side's fee (exact: ⌊in·0.3 %⌋, then
    // ⌊fee·share⌋ each). The chain's coin's security part is paid with the block's fees; GOLD's is held by the treasury.
    const Pool after{chain.PoolOf(GOLD, CHN)};
    const uint64_t in0{chn_first ? 100'000'000U : 1'000'000U}, in1{chn_first ? 1'000'000U : 100'000'000U};
    BOOST_CHECK_EQUAL(after.reserve0, expected->reserve0 - Taken(in0));
    BOOST_CHECK_EQUAL(after.reserve1, expected->reserve1 - Taken(in1));
    const auto& rules{Params().GetConsensus().sidechain};
    const FeeParts chn_parts{SwapFeeParts(uint64_t{100'000'000} * 3000 / FEE_DENOMINATOR, rules.dex_fee_security_share, rules.dex_fee_protocol_share)};
    const FeeParts gold_parts{SwapFeeParts(uint64_t{1'000'000} * 3000 / FEE_DENOMINATOR, rules.dex_fee_security_share, rules.dex_fee_protocol_share)};
    BOOST_CHECK_EQUAL(chain.Assets().BlockSecurityFees(chain.height - 1), static_cast<CAmount>(chn_parts.security));
    BOOST_CHECK_EQUAL(chain.Assets().GetTreasury(GOLD)->security_held, gold_parts.security);
    BOOST_CHECK_EQUAL(chain.Assets().GetTreasury(GOLD)->balance, gold_parts.protocol);
    // The fee, all of it: the providers' part stayed in the pool (the round's reserves are with it), the rest went.
    BOOST_CHECK_EQUAL(chn_parts.security + chn_parts.protocol, Taken(100'000'000));
    BOOST_CHECK_EQUAL(after.swaps, start.swaps + 2);
    // Both better than they would have done one after the other.
    BOOST_CHECK_GT(expected->outs[0] + 1, Alone(start, CHN, 100'000'000) - 1);

    // The block taken back: the journal puts the pool, the tokens and the accumulators back as they were.
    const sidechain::StoreUndo block{chain.store.TakeUndo()};
    chain.store.Revert(block);
    BOOST_CHECK(chain.state.Hash() == hash_before);
    // (... in a block still open: the orders are back; clear them again, the same.)
    chain.height -= 1;
    chain.time -= 15;
    chain.payouts = {CTxOut{0, OTHER}, CTxOut{0, OTHER}};
    chain.EndBlock();
    BOOST_CHECK(chain.TokenAt(COutPoint{buy.GetHash(), 0}) == AssetToken(GOLD, expected->outs[0]));

    // Same side, the second's least out is what it would get alone: together, at their one price, it misses.
    const Pool now{chain.PoolOf(GOLD, CHN)};
    const uint64_t alone{Alone(now, GOLD, 1'000'000)};
    const CTransaction loose{MakeTx({gold[2]}, Swap{GOLD, 1'000'000, CHN, 1, OTHER}, {std::nullopt})};
    const CTransaction tight{MakeTx({gold[3]}, Swap{GOLD, 1'000'000, CHN, alone, THIRD}, {std::nullopt})};
    chain.Require(loose);
    chain.Require(tight);
    chain.EndBlock();
    // The tight one is refunded (its GOLD back to its result output, no CHN); the loose one fills alone.
    BOOST_CHECK(chain.TokenAt(COutPoint{tight.GetHash(), 0}) == AssetToken(GOLD, 1'000'000));
    BOOST_REQUIRE_EQUAL(chain.paid.size(), 1U);
    BOOST_CHECK(chain.paid[0] == CTxOut(static_cast<CAmount>(alone), OTHER));
    // A CHN order that misses gets its CHN back.
    const CTransaction greedy{MakeTx({}, Swap{CHN, 1'000'000, GOLD, 1'000'000, OTHER}, {std::nullopt}, 1'000'000)};
    chain.Require(greedy);
    chain.EndBlock();
    BOOST_CHECK(chain.paid[0] == CTxOut(1'000'000, OTHER));
    BOOST_CHECK(!chain.TokenAt(COutPoint{greedy.GetHash(), 0}));
    // A CHN order names where its CHN go, and pays in no less than is paid out.
    std::string reason;
    BOOST_CHECK(!chain.Apply(MakeTx({}, Swap{CHN, 1'000'000, GOLD, 1, CScript{}}, {std::nullopt}, 1'000'000), reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-chn-to");
    BOOST_CHECK(!chain.Apply(MakeTx({}, Swap{CHN, MIN_CHN_PAYOUT - 1, GOLD, 1, OTHER}, {std::nullopt}, MIN_CHN_PAYOUT - 1), reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-chn-dust");
    // Policy: the mempool does not relay an order that cannot fill alone.
    BOOST_CHECK(!chain.Assets().OrderFillsAlone(tight, reason));
    BOOST_CHECK(chain.Assets().OrderFillsAlone(loose, reason));
    // Nothing of the block's orders is left in the store after its end.
    for (uint8_t table : {0x82, 0x83, 0x84, 0x85}) {
        const sidechain::StoreBytes prefix{table};
        BOOST_CHECK(!chain.store.Next(prefix, prefix));
    }
}

BOOST_AUTO_TEST_CASE(dex_orders_bound)
{
    Chain chain;
    const AssetId GOLD{HashName("GOLD")}, SILVER{HashName("SILVER")};
    const COutPoint gold{chain.Issue("GOLD", 100'000'000)}, silver{chain.Issue("SILVER", 100'000'000)};
    // Coins of 100 for the orders, 60 an output split (a marker lists 64 outputs at most).
    auto g{chain.Split(gold, GOLD, 100'000'000, {10'000'000})};
    const auto s{chain.Split(silver, SILVER, 100'000'000, {10'000'000})};
    chain.MakePool(g[0], GOLD, 10'000'000, s[0], SILVER, 10'000'000);
    std::vector<COutPoint> coins;
    COutPoint rest{g[1]};
    uint64_t left{90'000'000};
    while (coins.size() < MAX_BLOCK_ORDERS + 1) {
        const auto split{chain.Split(rest, GOLD, left, std::vector<uint64_t>(60, 100))};
        coins.insert(coins.end(), split.begin(), split.end() - 1);
        rest = split.back();
        left -= 6000;
    }
    chain.EndBlock();
    std::string reason;
    for (size_t i{0}; i < MAX_BLOCK_ORDERS; ++i) chain.Require(MakeTx({coins[i]}, Swap{GOLD, 100, SILVER, 1, CScript{}}, {std::nullopt}));
    const CTransaction beyond{MakeTx({coins[MAX_BLOCK_ORDERS]}, Swap{GOLD, 100, SILVER, 1, CScript{}}, {std::nullopt})};
    BOOST_CHECK(!chain.Apply(beyond, reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-block-orders-full");
    chain.EndBlock();
    // The next block takes it.
    chain.Require(beyond);
    chain.EndBlock();
    BOOST_CHECK(chain.TokenAt(COutPoint{beyond.GetHash(), 0}));
}

BOOST_AUTO_TEST_CASE(dex_twap_observations)
{
    Chain chain;
    const AssetId GOLD{HashName("GOLD")}, SILVER{HashName("SILVER")};
    const COutPoint gold{chain.Issue("GOLD", 100'000'000)}, silver{chain.Issue("SILVER", 100'000'000)};
    const auto g{chain.Split(gold, GOLD, 100'000'000, {10'000'000, 1'000'000})};
    const auto s{chain.Split(silver, SILVER, 100'000'000, {20'000'000})};
    chain.MakePool(g[0], GOLD, 10'000'000, s[0], SILVER, 20'000'000);
    const uint256 id{PoolId(GOLD, SILVER)};
    const uint32_t t0{chain.time};
    chain.EndBlock(100);
    // The block that made it: the first observation, no time accumulated yet.
    const auto acc0{*chain.Assets().GetAccumulator(id)};
    BOOST_CHECK_EQUAL(acc0.count, 1U);
    BOOST_CHECK_EQUAL(acc0.last_time, t0);
    BOOST_CHECK(acc0.price0_cumulative.IsNull());
    // Blocks that do not touch it leave it alone; a trade 100 seconds later adds the old price times 100.
    chain.EndBlock(50);
    const Pool before{chain.PoolOf(GOLD, SILVER)};
    chain.Require(MakeTx({g[1]}, Swap{GOLD, 1'000'000, SILVER, 1, CScript{}}, {std::nullopt}));
    const uint32_t t1{chain.time};
    chain.EndBlock();
    const auto acc1{*chain.Assets().GetAccumulator(id)};
    BOOST_CHECK_EQUAL(acc1.count, 2U);
    BOOST_CHECK_EQUAL(acc1.last_time, t1);
    BOOST_CHECK(acc1.price0_cumulative == dex::Accumulate(uint256{}, dex::PriceQ64(before.reserve1, before.reserve0), t1 - t0));
    BOOST_CHECK(acc1.price1_cumulative == dex::Accumulate(uint256{}, dex::PriceQ64(before.reserve0, before.reserve1), t1 - t0));
    const auto obs{*chain.Assets().GetObservation(id, 1)};
    const Pool after{chain.PoolOf(GOLD, SILVER)};
    BOOST_CHECK_EQUAL(obs.reserve0, after.reserve0);
    BOOST_CHECK_EQUAL(obs.time, t1);
    // The average over the 150 seconds: the pool's price before the trade, the whole time.
    const auto [c0, c1]{dex::CumulativeAt(obs, t1)};
    BOOST_CHECK_CLOSE(dex::AveragePrice(dex::CumulativeDelta(c0, acc0.price0_cumulative), t1 - t0), static_cast<double>(before.reserve1) / before.reserve0, 1e-9);
    // A block earlier than the last (a BMM block's time may be): no time goes back.
    chain.time = t1 - 30;
    chain.Require(MakeTx({COutPoint{chain.Split(COutPoint{g.back()}, GOLD, 100'000'000 - 11'000'000, {1000})[0]}}, Swap{GOLD, 1000, SILVER, 1, CScript{}}, {std::nullopt}));
    chain.EndBlock();
    BOOST_CHECK_EQUAL(chain.Assets().GetAccumulator(id)->last_time, t1);
    BOOST_CHECK(chain.Assets().GetAccumulator(id)->price0_cumulative == acc1.price0_cumulative);
}

BOOST_AUTO_TEST_CASE(dex_oracles_and_contracts)
{
    Chain chain;
    CKey key;
    key.MakeNewKey(true);
    const XOnlyPubKey xonly{key.GetPubKey()};
    uint256 pubkey;
    std::copy(xonly.begin(), xonly.end(), pubkey.begin());
    std::string reason;
    const uint256 genesis{Params().GetConsensus().hashGenesisBlock};
    // The proof of possession: the key's signature over the registration (the chain's first block, the name, the key).
    const auto proof_by{[&](const CKey& signer, const std::string& name, const uint256& registered) {
        std::array<unsigned char, 64> proof;
        BOOST_REQUIRE(signer.SignSchnorr(dex::RegistrationHash(name, registered, genesis), proof, nullptr, uint256{}));
        return proof;
    }};
    const auto proof{proof_by(key, "prices", pubkey)};
    BOOST_CHECK(dex::CheckRegistration("prices", pubkey, genesis, proof));
    BOOST_CHECK(!dex::CheckRegistration("prices2", pubkey, genesis, proof));
    BOOST_CHECK(!dex::CheckRegistration("prices", pubkey, uint256::ONE, proof));
    // Registering costs its fee, burned by the marker (to mainchain miners: a withdrawal).
    BOOST_CHECK(!chain.Apply(MakeTx({}, RegisterOracle{"prices", pubkey, proof}, {}, ORACLE_REGISTRATION_FEE - 1), reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-marker-value");
    // Nobody registers a key that is not theirs: with a proof by another key, or one made for another name.
    CKey squatter;
    squatter.MakeNewKey(true);
    const XOnlyPubKey squatter_xonly{squatter.GetPubKey()};
    uint256 squatter_pubkey;
    std::copy(squatter_xonly.begin(), squatter_xonly.end(), squatter_pubkey.begin());
    BOOST_CHECK(!chain.Apply(MakeTx({}, RegisterOracle{"prices", pubkey, proof_by(squatter, "prices", pubkey)}, {}, ORACLE_REGISTRATION_FEE), reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-oracle-proof");
    BOOST_CHECK(!chain.Apply(MakeTx({}, RegisterOracle{"other name", pubkey, proof}, {}, ORACLE_REGISTRATION_FEE), reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-oracle-proof");
    // A squatter who registers the same name first, with its own key, takes nothing: names are labels, the key is the oracle.
    chain.Require(MakeTx({}, RegisterOracle{"prices", squatter_pubkey, proof_by(squatter, "prices", squatter_pubkey)}, {}, ORACLE_REGISTRATION_FEE));
    const CTransaction reg{MakeTx({}, RegisterOracle{"prices", pubkey, proof}, {}, ORACLE_REGISTRATION_FEE)};
    chain.Require(reg);
    bool fee_withdrawal{false};
    chain.state.ForEachWithdrawal([&](const sidechain::Withdrawal& w) {
        fee_withdrawal |= w.outpoint.hash == reg.GetHash() && w.Burned() == ORACLE_REGISTRATION_FEE;
        return true;
    });
    BOOST_CHECK(fee_withdrawal);
    BOOST_CHECK(!chain.Apply(MakeTx({}, RegisterOracle{"prices", pubkey, proof}, {}, ORACLE_REGISTRATION_FEE), reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-oracle-taken");
    chain.EndBlock();
    const uint256 oracle{dex::OracleId(pubkey)};
    BOOST_CHECK_EQUAL(chain.Assets().GetOracle(oracle)->name, "prices");
    BOOST_CHECK(chain.Assets().GetOracle(oracle)->pubkey == pubkey);
    // The squatter's attestation is its own oracle's, never the honest one's.
    std::array<unsigned char, 64> squatter_sig;
    BOOST_REQUIRE(squatter.SignSchnorr(dex::AttestationHash(oracle, dex::EventId("BTCUSD"), 1, chain.height), squatter_sig, nullptr, uint256{}));
    BOOST_CHECK(!chain.Apply(MakeTx({}, Attest{oracle, dex::EventId("BTCUSD"), 1, chain.height, squatter_sig}, {}), reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-attestation-sig");

    // Two contracts on one event: CHN, and an asset; outcomes below and at or above 100.
    const AssetId GOLD{HashName("GOLD")};
    const COutPoint gold{chain.Issue("GOLD", 1'000'000)};
    const uint256 event{dex::EventId("BTCUSD")}, other_event{dex::EventId("ETHUSD")};
    const std::vector<OracleOutcome> outcomes{{std::numeric_limits<int64_t>::min(), 99, OTHER}, {100, std::numeric_limits<int64_t>::max(), THIRD}};
    const CTransaction chn_contract{MakeTx({}, CreateOracleContract{oracle, event, CHN, 5'000'000, outcomes, HOLDER, chain.height + 10}, {}, 5'000'000)};
    const CTransaction gold_contract{MakeTx({gold}, CreateOracleContract{oracle, event, GOLD, 1'000'000, outcomes, HOLDER, chain.height + 10}, {})};
    const CTransaction expiring{MakeTx({}, CreateOracleContract{oracle, other_event, CHN, 1'000'000, outcomes, HOLDER, chain.height + 1}, {}, 1'000'000)};
    chain.Require(chn_contract);
    chain.Require(gold_contract);
    chain.Require(expiring);
    BOOST_CHECK(!chain.Apply(MakeTx({}, CreateOracleContract{oracle, event, CHN, 1'000'000, outcomes, HOLDER, chain.height}, {}, 1'000'000), reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-contract-expiry");
    chain.EndBlock();
    // The CHN it holds is in the peg's part of the assets; the asset it holds keeps the asset alive.
    BOOST_CHECK_EQUAL(chain.Assets().HeldChn(), 6'000'000);
    BOOST_CHECK(!chain.Assets().Releasable(GOLD, &reason));

    // Before an attestation: pending.
    BOOST_CHECK(!chain.Apply(MakeTx({}, SettleOracleContract{chn_contract.GetHash()}, {}), reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-contract-pending");
    // An attestation: signed for a height not above the block's, by the oracle's key, once.
    const auto attest{[&](const uint256& ev, int64_t value, int32_t height) {
        std::array<unsigned char, 64> sig;
        BOOST_REQUIRE(key.SignSchnorr(dex::AttestationHash(oracle, ev, value, height), sig, nullptr, uint256{}));
        return MakeTx({}, Attest{oracle, ev, value, height, sig}, {});
    }};
    BOOST_CHECK(!chain.Apply(attest(event, 150, chain.height + 1), reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-attestation-early");
    const CTransaction signed_tx{attest(event, 150, chain.height)};
    CMutableTransaction forged{signed_tx};
    auto forged_marker{*GetMarker(signed_tx)};
    std::get<Attest>(*forged_marker.operation).value = 50;
    forged.vout.back().scriptPubKey = MarkerScript(forged_marker);
    BOOST_CHECK(!chain.Apply(CTransaction{forged}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-attestation-sig");
    chain.Require(attest(event, 150, chain.height));
    BOOST_CHECK(!chain.Apply(attest(event, 10, chain.height), reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-attested");
    chain.EndBlock();
    BOOST_CHECK_EQUAL(chain.Assets().GetAttestation(oracle, event)->value, 150);

    // 150 is at or above 100: THIRD gets both. CHN by the coinbase; the asset to a result output of THIRD's script, no other.
    chain.Require(MakeTx({}, SettleOracleContract{chn_contract.GetHash()}, {}));
    BOOST_CHECK(chain.payouts.back() == CTxOut(5'000'000, THIRD));
    BOOST_CHECK(!chain.Apply(MakeTx({}, SettleOracleContract{gold_contract.GetHash()}, {std::nullopt}, 0, {OTHER}), reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-contract-payee");
    const CTransaction settle_gold{MakeTx({}, SettleOracleContract{gold_contract.GetHash()}, {std::nullopt}, 0, {THIRD})};
    chain.Require(settle_gold);
    BOOST_CHECK(chain.TokenAt(COutPoint{settle_gold.GetHash(), 0}) == AssetToken(GOLD, 1'000'000));
    BOOST_CHECK(!chain.Apply(MakeTx({}, SettleOracleContract{chn_contract.GetHash()}, {}), reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-contract-settled");
    // The other event was never attested: after its expiry, the refund.
    chain.Require(MakeTx({}, SettleOracleContract{expiring.GetHash()}, {}));
    BOOST_CHECK(chain.payouts.back() == CTxOut(1'000'000, HOLDER));
    chain.EndBlock();
    BOOST_CHECK_EQUAL(chain.Assets().HeldChn(), 0);
    BOOST_CHECK(chain.Assets().GetContract(chn_contract.GetHash())->settled);
}

BOOST_AUTO_TEST_SUITE_END()
