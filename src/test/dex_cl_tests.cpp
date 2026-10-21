// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Concentrated liquidity (bitassets/clmath.h; doc/layers.md "Concentrated liquidity") and dynamic fees: Uniswap v3's
// tick math bit for bit, the rounding of amounts, swap steps; on a state of their own: pools made at a price,
// positions in and out of range, orders that cross ticks in the block's batch, fees to the positions in range only,
// every position closable (the pool never owes more than it holds), and a fee that rises with volatility.

#include <addresstype.h>
#include <arith_uint256.h>
#include <bitassets/clmath.h>
#include <bitassets/curves.h>
#include <bitassets/dex.h>
#include <bitassets/state.h>
#include <consensus/params.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sidechain/state.h>
#include <sidechain/store.h>
#include <test/data/dex_cl.json.h>
#include <test/util/json.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <univalue.h>

#include <boost/test/unit_test.hpp>

#include <cstdlib>
#include <fstream>
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
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(++g_nonce % 250 + 1)}), static_cast<uint32_t>(400000 + g_nonce)});
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
        BOOST_REQUIRE_MESSAGE(Apply(tx, reason), reason << " op " << (GetMarker(tx) && GetMarker(tx)->operation ? GetMarker(tx)->operation->index() : 99));
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
    /** An asset, its supply in coins of `amounts` (and the rest, last); 2 decimals. */
    std::vector<COutPoint> Issue(const std::string& name, uint64_t supply, const std::vector<uint64_t>& amounts)
    {
        const AssetId id{HashName(name)};
        const uint256 nonce{HashName("nonce " + name)};
        const CTransaction reserved{MakeTx({}, Reserve{ReservationCommitment(id, nonce, HOLDER)}, {Token{Token::Kind::RESERVATION, uint256{}, 1}})};
        Require(reserved);
        Register reg;
        reg.name = id;
        reg.nonce = nonce;
        reg.supply = supply;
        reg.decimals = 2;
        reg.text = name;
        const CTransaction tx{MakeTx({COutPoint{reserved.GetHash(), 0}}, reg, {Token{Token::Kind::CONTROL, id, 1}, AssetToken(id, supply)})};
        Require(tx);
        std::vector<COutPoint> result;
        COutPoint rest{tx.GetHash(), 1};
        uint64_t left{supply};
        for (size_t i{0}; i < amounts.size(); i += 50) {
            std::vector<std::optional<Token>> outs;
            uint64_t used{0};
            for (size_t k{i}; k < std::min(amounts.size(), i + 50); ++k) {
                outs.push_back(AssetToken(id, amounts[k]));
                used += amounts[k];
            }
            outs.push_back(AssetToken(id, left - used));
            const CTransaction split{MakeTx({rest}, std::nullopt, outs)};
            Require(split);
            for (uint32_t k{0}; k + 1 < outs.size(); ++k) result.emplace_back(split.GetHash(), k);
            rest = COutPoint{split.GetHash(), static_cast<uint32_t>(outs.size() - 1)};
            left -= used;
        }
        result.push_back(rest);
        return result;
    }
};

ClState StateAt(const arith_uint256& sqrt, unsigned __int128 liquidity)
{
    ClState s;
    s.sqrt_price = ArithToUint256(sqrt);
    s.tick = cl::TickAtSqrtRatio(sqrt);
    s.SetLiquidity(liquidity);
    return s;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(dex_cl_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(tick_math_is_uniswaps)
{
    // Uniswap v3's TickMath at its bounds and around 0 (its own test values).
    BOOST_CHECK(cl::SqrtRatioAtTick(cl::MIN_TICK) == cl::MinSqrtRatio());
    BOOST_CHECK(cl::SqrtRatioAtTick(cl::MAX_TICK) == cl::MaxSqrtRatio());
    BOOST_CHECK(cl::SqrtRatioAtTick(0) == arith_uint256{1} << 96);
    // Ticks 1 and −1: 79232123823359799118286999568 and 79224201403219477170569942574.
    BOOST_CHECK(curves::BigUint::FromArith(cl::SqrtRatioAtTick(1)) == curves::BigUint::U128((static_cast<unsigned __int128>(4295182038ULL) << 64) | 18379584988209926160ULL));
    BOOST_CHECK(curves::BigUint::FromArith(cl::SqrtRatioAtTick(-1)) == curves::BigUint::U128((static_cast<unsigned __int128>(4294752563ULL) << 64) | 13650319450173150766ULL));
    // Monotone, and TickAtSqrtRatio its inverse: the largest tick whose ratio is at most the price.
    FastRandomContext rng{/*fDeterministic=*/true};
    for (int i{0}; i < 2000; ++i) {
        const int32_t t{static_cast<int32_t>(rng.randrange(2 * cl::MAX_TICK)) - cl::MAX_TICK};
        const arith_uint256 r{cl::SqrtRatioAtTick(t)}, next{cl::SqrtRatioAtTick(t + 1)};
        BOOST_CHECK(r < next);
        BOOST_CHECK_EQUAL(cl::TickAtSqrtRatio(r), t);
        BOOST_CHECK_EQUAL(cl::TickAtSqrtRatio(next - 1), t);
    }
}

BOOST_AUTO_TEST_CASE(amounts_round_toward_the_pool)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    for (int i{0}; i < 1000; ++i) {
        const int32_t a{static_cast<int32_t>(rng.randrange(200000)) - 100000}, b{a + 1 + static_cast<int32_t>(rng.randrange(20000))};
        const unsigned __int128 l{(static_cast<unsigned __int128>(rng.rand64() >> 20) << 20) + rng.rand64() % 1000 + 1};
        const arith_uint256 sa{cl::SqrtRatioAtTick(a)}, sb{cl::SqrtRatioAtTick(b)};
        const auto up0{cl::Amount0Delta(sa, sb, l, true)}, down0{cl::Amount0Delta(sa, sb, l, false)};
        const auto up1{cl::Amount1Delta(sa, sb, l, true)}, down1{cl::Amount1Delta(sa, sb, l, false)};
        BOOST_CHECK(up0 >= down0 && up0 <= down0 + curves::BigUint{1});
        BOOST_CHECK(up1 >= down1 && up1 <= down1 + curves::BigUint{1});
        // A liquidity the amounts give needs no more than them.
        const uint64_t x{1 + rng.randrange(uint64_t{1} << 50)}, y{1 + rng.randrange(uint64_t{1} << 50)};
        const arith_uint256 p{cl::SqrtRatioAtTick(a + static_cast<int32_t>(rng.randrange(b - a + 1)))};
        const unsigned __int128 lq{cl::LiquidityForAmounts(p, sa, sb, x, y)};
        const auto needed{cl::AmountsForLiquidity(p, sa, sb, lq, true)};
        BOOST_REQUIRE(needed);
        BOOST_CHECK(needed->first <= x + 1 && needed->second <= y + 1);
        // Out of what goes in, never more comes back out.
        const auto back{cl::AmountsForLiquidity(p, sa, sb, lq, false)};
        BOOST_CHECK(back->first <= needed->first && back->second <= needed->second);
    }
}

BOOST_AUTO_TEST_CASE(swap_steps)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    for (int i{0}; i < 1000; ++i) {
        const int32_t t{static_cast<int32_t>(rng.randrange(100000)) - 50000};
        const bool zero_in{rng.randbool()};
        const int32_t target_tick{zero_in ? t - 1 - static_cast<int32_t>(rng.randrange(5000)) : t + 1 + static_cast<int32_t>(rng.randrange(5000))};
        const arith_uint256 p{cl::SqrtRatioAtTick(t)}, target{cl::SqrtRatioAtTick(target_tick)};
        const unsigned __int128 l{(static_cast<unsigned __int128>(1 + rng.randrange(1 << 20)) << 40)};
        const uint64_t remaining{1 + rng.randrange(uint64_t{1} << 40)};
        const cl::Step step{cl::SwapStep(p, target, l, remaining, zero_in)};
        BOOST_CHECK(step.amount_in <= remaining);
        // The price moves the right way, never past the target.
        BOOST_CHECK(zero_in ? (step.sqrt_next <= p && step.sqrt_next >= target) : (step.sqrt_next >= p && step.sqrt_next <= target));
        // What comes out is at most what the curve between the two prices holds (rounded down).
        const curves::BigUint most{zero_in ? cl::Amount1Delta(step.sqrt_next, p, l, false) : cl::Amount0Delta(p, step.sqrt_next, l, false)};
        BOOST_CHECK(curves::BigUint{step.amount_out} <= most);
        // More in never gives less out.
        const cl::Step more{cl::SwapStep(p, target, l, remaining + 1 + rng.randrange(1000), zero_in)};
        BOOST_CHECK(more.amount_out >= step.amount_out);
    }
}

BOOST_AUTO_TEST_CASE(positions_trades_fees_and_solvency)
{
    Chain chain;
    const AssetId A{HashName("AAA")}, B{HashName("BBB")};
    std::vector<uint64_t> coins(140, 1'000'000);
    const auto a{chain.Issue("AAA", 10'000'000'000, coins)};
    const auto b{chain.Issue("BBB", 10'000'000'000, coins)};
    // A pool at 1:1, 0.30 % (tick spacing 60).
    CreatePool create;
    create.asset_a = A;
    create.asset_b = B;
    create.curve = static_cast<uint8_t>(Curve::CONCENTRATED);
    create.fee = 3000;
    create.amount_a = 1000;
    create.amount_b = 1000;
    chain.Require(MakeTx({}, create, {}));
    PoolParams kind;
    kind.curve = static_cast<uint8_t>(Curve::CONCENTRATED);
    kind.fee = 3000;
    const uint256 id{PoolIdOf(A, B, kind)};
    const auto cl0{chain.Assets().GetClState(id)};
    BOOST_REQUIRE(cl0);
    BOOST_CHECK_EQUAL(cl0->tick, 0);
    BOOST_CHECK(UintToArith256(cl0->sqrt_price) == arith_uint256{1} << 96);
    const bool a0{A < B};
    // Positions: around the price (in range), wide, and above it (out of range: asset0 alone).
    struct Pos {
        int32_t lo, hi;
        unsigned __int128 l;
        uint256 id;
        COutPoint token;
    };
    std::vector<Pos> positions{{-600, 600, 0, {}, {}}, {-6000, 6000, 0, {}, {}}, {1200, 2400, 0, {}, {}}};
    size_t next_a{0}, next_b{0};
    for (Pos& p : positions) {
        const arith_uint256 price{UintToArith256(chain.Assets().GetClState(id)->sqrt_price)};
        p.l = cl::LiquidityForAmounts(price, cl::SqrtRatioAtTick(p.lo), cl::SqrtRatioAtTick(p.hi), 1'000'000, 1'000'000);
        BOOST_REQUIRE(p.l > 0);
        OpenPosition open;
        open.pool = id;
        open.tick_lower = p.lo;
        open.tick_upper = p.hi;
        open.liquidity_hi = static_cast<uint64_t>(p.l >> 64);
        open.liquidity_lo = static_cast<uint64_t>(p.l);
        open.max0 = 1'000'000;
        open.max1 = 1'000'000;
        // Its token, then what comes back (asset coins).
        const auto needed{cl::AmountsForLiquidity(price, cl::SqrtRatioAtTick(p.lo), cl::SqrtRatioAtTick(p.hi), p.l, true)};
        std::vector<std::optional<Token>> outs{std::nullopt};
        if (needed->first < 1'000'000) outs.emplace_back();
        if (needed->second < 1'000'000) outs.emplace_back();
        const COutPoint ca{a[next_a++]}, cb{b[next_b++]};
        const CTransaction tx{MakeTx({a0 ? ca : cb, a0 ? cb : ca}, open, outs)};
        chain.Require(tx);
        p.id = PositionId(tx.GetHash());
        p.token = COutPoint{tx.GetHash(), 0};
        BOOST_CHECK(chain.TokenAt(p.token) == (Token{Token::Kind::POSITION, p.id, 1}));
    }
    // Ticks a spacing off: refused.
    OpenPosition bad;
    bad.pool = id;
    bad.tick_lower = -61;
    bad.tick_upper = 60;
    bad.liquidity_lo = 1000;
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({}, bad, {std::nullopt})), "bad-ba-position-ticks");
    chain.EndBlock();
    const unsigned __int128 in_range{chain.Assets().GetClState(id)->Liquidity()};
    BOOST_CHECK(in_range == positions[0].l + positions[1].l);

    // Trades both ways in many blocks, some of them crossing ticks (into the wide position alone, and back),
    // batches of opposite orders included; the pool's price walks.
    FastRandomContext rng{/*fDeterministic=*/true};
    std::vector<COutPoint> orders_out;
    for (int block{0}; block < 20; ++block) {
        for (int k{0}, m{1 + static_cast<int>(rng.randrange(3))}; k < m; ++k) {
            const bool sell_a{rng.randbool()};
            const COutPoint coin{sell_a ? a[next_a++] : b[next_b++]};
            const CTransaction tx{MakeTx({coin}, SwapPools{sell_a ? A : B, {id}, 1'000'000, 1, CScript{}}, {std::nullopt})};
            chain.Require(tx);
            orders_out.emplace_back(tx.GetHash(), 0);
        }
        chain.EndBlock();
    }
    const auto after{chain.Assets().GetClState(id)};
    BOOST_CHECK(after->sqrt_price != cl0->sqrt_price);
    // Fees: the positions in range earned; the one above the price, never in range, nothing.
    const auto fees_out{chain.Assets().PositionFees(*chain.Assets().GetPosition(positions[2].id))};
    const auto fees_mid{chain.Assets().PositionFees(*chain.Assets().GetPosition(positions[0].id))};
    if (after->tick < 1200) BOOST_CHECK(fees_out.first == 0 && fees_out.second == 0);
    BOOST_CHECK(fees_mid.first + fees_mid.second > 0);
    // Solvency: every position closes, collecting all it holds and is owed, and the pool keeps what is left.
    for (Pos& p : positions) {
        ChangePosition close;
        close.position = p.id;
        close.close = true;
        close.collect = true;
        close.chn_to = PAYEE;
        const auto pos{*chain.Assets().GetPosition(p.id)};
        const auto cl{*chain.Assets().GetClState(id)};
        const auto amounts{cl::AmountsForLiquidity(UintToArith256(cl.sqrt_price), cl::SqrtRatioAtTick(pos.tick_lower), cl::SqrtRatioAtTick(pos.tick_upper), pos.Liquidity(), false)};
        const auto fees{chain.Assets().PositionFees(pos)};
        std::vector<std::optional<Token>> outs;
        if (amounts->first + (a0 ? fees.first : fees.second) > 0 || true) {
            if (amounts->first + fees.first > 0) outs.emplace_back();
            if (amounts->second + fees.second > 0) outs.emplace_back();
        }
        chain.Require(MakeTx({p.token}, close, outs));
        BOOST_CHECK(!chain.Assets().GetPosition(p.id));
    }
    chain.EndBlock();
    // Every position out: no liquidity left in range, and the pool still holds what is left of the rounding.
    BOOST_CHECK(chain.Assets().GetClState(id)->Liquidity() == 0);
    BOOST_CHECK(chain.Assets().ClTicksOf(id, 100).empty());
    // An order through a pool with no liquidity gets nothing: it is refunded.
    const CTransaction late{MakeTx({a[next_a++]}, SwapPools{A, {id}, 1'000'000, 1, CScript{}}, {std::nullopt})};
    chain.Require(late);
    chain.EndBlock();
    BOOST_CHECK(chain.TokenAt(COutPoint{late.GetHash(), 0}) == AssetToken(A, 1'000'000));
}

BOOST_AUTO_TEST_CASE(dynamic_fee)
{
    Chain chain;
    const AssetId A{HashName("AAA")}, B{HashName("BBB")};
    std::vector<uint64_t> coins(40, 10'000'000);
    const auto a{chain.Issue("AAA", 10'000'000'000, coins)};
    const auto b{chain.Issue("BBB", 10'000'000'000, coins)};
    CreatePool create;
    create.asset_a = A;
    create.asset_b = B;
    create.curve = static_cast<uint8_t>(Curve::CONSTANT_PRODUCT);
    create.fee = 3000;
    create.flags = POOL_FLAG_DYNAMIC_FEE;
    create.amount_a = 10'000'000;
    create.amount_b = 10'000'000;
    const bool a0{A < B};
    chain.Require(MakeTx({a0 ? a[0] : b[0], a0 ? b[0] : a[0]}, create, {std::nullopt}));
    // Its kind differs from the pool without the flag: another pool.
    PoolParams kind;
    kind.fee = 3000;
    kind.flags = POOL_FLAG_DYNAMIC_FEE;
    const uint256 id{PoolIdOf(A, B, kind)};
    kind.flags = 0;
    BOOST_CHECK(id != PoolIdOf(A, B, kind));
    kind.flags = POOL_FLAG_DYNAMIC_FEE;
    chain.EndBlock();
    BOOST_CHECK_EQUAL(chain.Assets().FeeOf(id, kind, chain.height), 3000U);
    // Big moves, block after block: the fee rises, bounded.
    for (int i{1}; i < 12; ++i) {
        chain.Require(MakeTx({i % 2 ? a[i] : b[i]}, SwapPools{i % 2 ? A : B, {id}, 10'000'000, 1, CScript{}}, {std::nullopt}));
        // (each a tenth of the pool's depth or so: the price moves a few percent a block)
        chain.EndBlock();
    }
    const uint32_t high{chain.Assets().FeeOf(id, kind, chain.height)};
    BOOST_CHECK_GT(high, 3000U);
    BOOST_CHECK_LE(high, 3000U + 20'000U);
    // Quiet blocks: it decays toward the tier (halving every 40 blocks without trades).
    BOOST_CHECK_LT(chain.Assets().FeeOf(id, kind, chain.height + 400), high);
    BOOST_CHECK_EQUAL(chain.Assets().FeeOf(id, kind, chain.height + 40 * 70), 3000U);
}

/** Vectors of the tick math and the amounts, which other implementations (the reference in Python) check. */
BOOST_AUTO_TEST_CASE(vectors)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    UniValue cases(UniValue::VARR);
    for (int i{0}; i < 80; ++i) {
        const int32_t t{i < 4 ? std::array<int32_t, 4>{cl::MIN_TICK, cl::MAX_TICK, 0, -1}[i] : static_cast<int32_t>(rng.randrange(2 * cl::MAX_TICK)) - cl::MAX_TICK};
        const int32_t lo{std::max(cl::MIN_TICK, t - 1 - static_cast<int32_t>(rng.randrange(30000)))}, hi{std::min(cl::MAX_TICK, t + 1 + static_cast<int32_t>(rng.randrange(30000)))};
        const unsigned __int128 l{(static_cast<unsigned __int128>(rng.rand64() >> 4) << 12) + 1};
        const uint64_t x{1 + rng.randrange(uint64_t{1} << 60)}, y{1 + rng.randrange(uint64_t{1} << 60)};
        const bool zero_in{rng.randbool()};
        const uint64_t amount{1 + rng.randrange(uint64_t{1} << 50)};
        UniValue c(UniValue::VOBJ);
        c.pushKV("tick", t);
        c.pushKV("sqrt", cl::SqrtRatioAtTick(t).GetHex());
        c.pushKV("lower", lo);
        c.pushKV("upper", hi);
        c.pushKV("liquidity", strprintf("%016x%016x", static_cast<uint64_t>(l >> 64), static_cast<uint64_t>(l)));
        const arith_uint256 p{cl::SqrtRatioAtTick(t)}, sa{cl::SqrtRatioAtTick(lo)}, sb{cl::SqrtRatioAtTick(hi)};
        const auto up{cl::AmountsForLiquidity(p, sa, sb, l, true)}, down{cl::AmountsForLiquidity(p, sa, sb, l, false)};
        c.pushKV("amounts_up", up ? strprintf("%u %u", up->first, up->second) : "");
        c.pushKV("amounts_down", down ? strprintf("%u %u", down->first, down->second) : "");
        c.pushKV("x", strprintf("%u", x));
        c.pushKV("y", strprintf("%u", y));
        const unsigned __int128 lq{cl::LiquidityForAmounts(p, sa, sb, x, y)};
        c.pushKV("liquidity_for", strprintf("%016x%016x", static_cast<uint64_t>(lq >> 64), static_cast<uint64_t>(lq)));
        c.pushKV("zero_in", zero_in);
        c.pushKV("amount", strprintf("%u", amount));
        const cl::Step step{cl::SwapStep(p, zero_in ? sa : sb, l, amount, zero_in)};
        c.pushKV("step", strprintf("%s %u %u", step.sqrt_next.GetHex(), step.amount_in, step.amount_out));
        cases.push_back(c);
    }
    if (const char* out{std::getenv("DEX_CL_VECTORS")}) {
        std::ofstream file{out};
        file << cases.write(1) << "\n";
    }
    const UniValue committed{read_json(json_tests::dex_cl)};
    BOOST_REQUIRE_EQUAL(committed.size(), cases.size());
    for (size_t i{0}; i < cases.size(); ++i) BOOST_CHECK_MESSAGE(committed[i].write() == cases[i].write(), "vector " << i);
}

BOOST_AUTO_TEST_SUITE_END()
