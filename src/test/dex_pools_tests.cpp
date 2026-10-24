// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// The DEX's pool kinds on a sidechain state of their own (bitassets/curves.h): the activation, pools of
// every kind beside the pool AddLiquidity makes, orders through named pools in the block's batch, routes
// across kinds, deposits and withdrawals of one side, the price accumulators, and retiring an asset.

#include <addresstype.h>
#include <bitassets/curves.h>
#include <bitassets/dex.h>
#include <bitassets/index.h>
#include <bitassets/state.h>
#include <consensus/params.h>
#include <pos/feesplit.h>
#include <primitives/transaction.h>
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
const CScript PAYEE{GetScriptForDestination(WitnessV0KeyHash{uint160{std::vector<unsigned char>(20, 7)}})};
int g_nonce{0};

Token AssetToken(const AssetId& id, uint64_t amount) { return Token{Token::Kind::ASSET, id, amount}; }

CTransaction MakeTx(const std::vector<COutPoint>& inputs, std::optional<Operation> op, const std::vector<std::optional<Token>>& outs, CAmount chn_in = 0)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(++g_nonce % 250 + 1)}), static_cast<uint32_t>(200000 + g_nonce)});
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
    std::map<AssetId, COutPoint> control;

    explicit Chain(int curves_height = 0)
    {
        params.bitassets_reveal_depth = 0;
        params.dex_height = 0;
        params.dex_curves_height = curves_height;
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
    /** An asset of `decimals`, its supply split into coins of `amounts` (and the rest, last). */
    std::vector<COutPoint> Issue(const std::string& name, uint64_t supply, const std::vector<uint64_t>& amounts, uint8_t decimals = 2)
    {
        const AssetId id{HashName(name)};
        const uint256 nonce{HashName("nonce " + name)};
        const CTransaction reserved{MakeTx({}, Reserve{ReservationCommitment(id, nonce, HOLDER)}, {Token{Token::Kind::RESERVATION, uint256{}, 1}})};
        Require(reserved);
        Register reg;
        reg.name = id;
        reg.nonce = nonce;
        reg.supply = supply;
        reg.decimals = decimals;
        reg.text = name;
        const CTransaction tx{MakeTx({COutPoint{reserved.GetHash(), 0}}, reg, {Token{Token::Kind::CONTROL, id, 1}, AssetToken(id, supply)})};
        Require(tx);
        control[id] = COutPoint{tx.GetHash(), 0};
        std::vector<std::optional<Token>> outs;
        uint64_t used{0};
        for (uint64_t a : amounts) {
            outs.push_back(AssetToken(id, a));
            used += a;
        }
        outs.push_back(AssetToken(id, supply - used));
        const CTransaction split{MakeTx({COutPoint{tx.GetHash(), 1}}, std::nullopt, outs)};
        Require(split);
        std::vector<COutPoint> result;
        for (uint32_t i{0}; i < outs.size(); ++i) result.emplace_back(split.GetHash(), i);
        return result;
    }
    /** A pool of a kind, opened with what the coins carry (`b_coins` null: b is CHN). The transaction (its output 0: the shares). */
    CTransaction Create(const COutPoint& a_coins, const AssetId& a, uint64_t amount_a, const std::optional<COutPoint>& b_coins, const AssetId& b, uint64_t amount_b,
                        Curve curve, uint32_t fee, uint32_t amp = 0, uint8_t weight_a = 0)
    {
        std::vector<COutPoint> ins{a_coins};
        if (b_coins) ins.push_back(*b_coins);
        CreatePool create;
        create.asset_a = a;
        create.asset_b = b;
        create.curve = static_cast<uint8_t>(curve);
        create.fee = fee;
        create.amp = amp;
        create.weight_a = weight_a;
        create.amount_a = amount_a;
        create.amount_b = amount_b;
        create.min_shares = 1;
        const CTransaction tx{MakeTx(ins, create, {std::nullopt}, b.IsNull() ? static_cast<CAmount>(amount_b) : 0)};
        Require(tx);
        return tx;
    }
    Pool PoolAt(const uint256& id) const { return *Assert(Assets().GetPool(id)); }
};

PoolParams Kind(Curve curve, uint32_t fee, uint32_t amp = 0, uint8_t weight0 = 0)
{
    PoolParams p;
    p.curve = static_cast<uint8_t>(curve);
    p.fee = fee;
    p.amp = amp;
    p.weight0 = weight0;
    return p;
}
} // namespace

namespace {
/** The security and protocol parts of the fee on `in` at `fee` (the network's shares): what leaves the pool. */
uint64_t Taken(uint64_t in, uint32_t fee)
{
    const auto& rules{Params().GetConsensus().sidechain};
    const FeeParts parts{SwapFeeParts(static_cast<uint64_t>(static_cast<unsigned __int128>(in) * fee / FEE_DENOMINATOR), rules.dex_fee_security_share, rules.dex_fee_protocol_share)};
    return parts.security + parts.protocol;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(dex_pools_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(activation)
{
    // Before the pool kinds' height (the DEX in force), their markers are not valid; everything else is as it was.
    Chain chain{/*curves_height=*/1000};
    const AssetId GOLD{HashName("GOLD")}, SILVER{HashName("SILVER")};
    const auto g{chain.Issue("GOLD", 10'000'000, {1'000'000, 1'000, 1'000'000})};
    const auto s{chain.Issue("SILVER", 10'000'000, {1'000'000, 1'000'000})};
    CreatePool create;
    create.asset_a = GOLD;
    create.asset_b = SILVER;
    create.curve = static_cast<uint8_t>(Curve::STABLE);
    create.fee = 500;
    create.amp = 100;
    create.amount_a = 1'000'000;
    create.amount_b = 1'000'000;
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({g[0], s[0]}, create, {std::nullopt})), "bad-ba-marker");
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({g[0]}, SwapPools{GOLD, {PoolId(GOLD, SILVER)}, 1000, 1, CScript{}}, {std::nullopt})), "bad-ba-marker");
    // AddLiquidity and swaps as before.
    chain.Require(MakeTx({g[0], s[0]}, AddLiquidity{GOLD, SILVER, 1'000'000, 1'000'000, 1}, {std::nullopt, std::nullopt}));
    chain.Require(MakeTx({g[1]}, Swap{GOLD, 1000, SILVER, 1, CScript{}}, {std::nullopt}));
    chain.EndBlock();
    // From the height on: valid.
    chain.height = 1000;
    chain.Create(s[1], SILVER, 1'000'000, g[2], GOLD, 1'000'000, Curve::STABLE, 500, 100);
}

BOOST_AUTO_TEST_CASE(kinds_side_by_side)
{
    Chain chain;
    const AssetId GOLD{HashName("GOLD")}, SILVER{HashName("SILVER")};
    const auto g{chain.Issue("GOLD", 100'000'000, {10'000'000, 10'000'000, 10'000'000, 10'000'000, 10'000'000, 50'000, 50'000})};
    const auto s{chain.Issue("SILVER", 100'000'000, {10'000'000, 10'000'000, 10'000'000, 30'000'000, 10'000'000})};
    // The pool AddLiquidity makes, and the same through CreatePool's default kind: one pool, refused twice.
    chain.Require(MakeTx({g[0], s[0]}, AddLiquidity{GOLD, SILVER, 10'000'000, 10'000'000, 1}, {std::nullopt, std::nullopt}));
    CreatePool same;
    same.asset_a = GOLD;
    same.asset_b = SILVER;
    same.fee = LEGACY_FEE;
    same.amount_a = 1000;
    same.amount_b = 1000;
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({g[5]}, same, {std::nullopt})), "bad-ba-pool-exists");
    // A pool of each other kind beside it.
    const CTransaction cp5{chain.Create(g[1], GOLD, 10'000'000, s[1], SILVER, 10'000'000, Curve::CONSTANT_PRODUCT, 500)};
    const CTransaction stable{chain.Create(g[2], GOLD, 10'000'000, s[2], SILVER, 10'000'000, Curve::STABLE, 100, 200)};
    const CTransaction weighted{chain.Create(g[3], GOLD, 10'000'000, s[3], SILVER, 30'000'000, Curve::WEIGHTED, 3000, 0, 25)};
    const uint256 id_cp5{PoolIdOf(GOLD, SILVER, Kind(Curve::CONSTANT_PRODUCT, 500))};
    const uint256 id_stable{PoolIdOf(GOLD, SILVER, Kind(Curve::STABLE, 100, 200))};
    // GOLD 25 %: weight0 is GOLD's if it sorts first.
    const uint8_t w0{GOLD < SILVER ? uint8_t{25} : uint8_t{75}};
    const uint256 id_weighted{PoolIdOf(GOLD, SILVER, Kind(Curve::WEIGHTED, 3000, 0, w0))};
    BOOST_CHECK(chain.Assets().HasPoolParams(id_cp5) && chain.Assets().HasPoolParams(id_stable) && chain.Assets().HasPoolParams(id_weighted));
    BOOST_CHECK(!chain.Assets().HasPoolParams(PoolId(GOLD, SILVER)));
    BOOST_CHECK_EQUAL(chain.Assets().PoolsOf(GOLD).size(), 4U);
    // The shares: isqrt for constant product, D for stable (balanced: the sum), the geometric mean for weighted; MIN_LIQUIDITY kept.
    BOOST_CHECK(chain.TokenAt(COutPoint{cp5.GetHash(), 0}) == (Token{Token::Kind::LP, id_cp5, 10'000'000 - MIN_LIQUIDITY}));
    BOOST_CHECK(chain.TokenAt(COutPoint{stable.GetHash(), 0}) == (Token{Token::Kind::LP, id_stable, 20'000'000 - MIN_LIQUIDITY}));
    BOOST_CHECK_EQUAL(chain.PoolAt(id_weighted).shares, *curves::OpeningShares(chain.Assets().GetPoolParams(id_weighted), chain.PoolAt(id_weighted).reserve0, chain.PoolAt(id_weighted).reserve1));
    // At 10M GOLD (25 %) and 30M SILVER (75 %): 1 GOLD = 1 SILVER at the start.
    BOOST_CHECK_CLOSE(curves::SpotPrice(chain.Assets().GetPoolParams(id_weighted), chain.PoolAt(id_weighted).reserve0, chain.PoolAt(id_weighted).reserve1), 1.0, 1e-9);
    // A pool of a kind made again while it has providers: refused.
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({g[4], s[4]}, [&] {
                          CreatePool again;
                          again.asset_a = SILVER;
                          again.asset_b = GOLD;
                          again.curve = static_cast<uint8_t>(Curve::STABLE);
                          again.fee = 100;
                          again.amp = 200;
                          again.amount_a = 1'000'000;
                          again.amount_b = 1'000'000;
                          return again;
                      }(), {std::nullopt})),
                      "bad-ba-pool-exists");
    chain.EndBlock();

    // The same block: a Swap through the default pool, orders through the stable one from both sides; each
    // pool clears at its own price, the default exactly as ClearPool.
    const Pool legacy_before{chain.PoolAt(PoolId(GOLD, SILVER))}, stable_before{chain.PoolAt(id_stable)};
    const CTransaction swap{MakeTx({g[5]}, Swap{GOLD, 50'000, SILVER, 1, CScript{}}, {std::nullopt})};
    const CTransaction buy{MakeTx({g[6]}, SwapPools{GOLD, {id_stable}, 50'000, 1, CScript{}}, {std::nullopt})};
    const auto s5{chain.Assets().GetToken(s[5])};
    const CTransaction sell{MakeTx({s[5]}, SwapPools{SILVER, {id_stable}, s5->amount, 1, CScript{}}, {std::nullopt})};
    chain.Require(swap);
    chain.Require(buy);
    chain.Require(sell);
    chain.EndBlock();
    const bool gold0{GOLD < SILVER};
    const auto legacy{dex::ClearPool(legacy_before.reserve0, legacy_before.reserve1, {{gold0, 50'000}})};
    BOOST_CHECK(chain.TokenAt(COutPoint{swap.GetHash(), 0}) == AssetToken(SILVER, legacy->outs[0]));
    const auto both{curves::ClearPoolOf(Kind(Curve::STABLE, 100, 200), 100, stable_before.reserve0, stable_before.reserve1, {{gold0, 50'000}, {!gold0, s5->amount}})};
    BOOST_REQUIRE(both);
    BOOST_CHECK(chain.TokenAt(COutPoint{buy.GetHash(), 0}) == AssetToken(SILVER, both->outs[0]));
    BOOST_CHECK(chain.TokenAt(COutPoint{sell.GetHash(), 0}) == AssetToken(GOLD, both->outs[1]));
    // The round's reserves, less the security and protocol parts of each side's fee.
    BOOST_CHECK_EQUAL(chain.PoolAt(id_stable).reserve0, both->reserve0 - Taken(gold0 ? 50'000 : s5->amount, 100));
    BOOST_CHECK_EQUAL(chain.PoolAt(id_stable).reserve1, both->reserve1 - Taken(gold0 ? s5->amount : 50'000, 100));
    // A stable pool near balance: about 1:1, less the 0.01 % fee.
    BOOST_CHECK_GT(both->outs[0], 49'900U);

    // The accumulators of a stable pool add its curve's price (1 at balance), not the reserves' ratio.
    chain.EndBlock(100);
    const auto acc{chain.Assets().GetAccumulator(id_stable)};
    BOOST_REQUIRE(acc);
    const auto obs{chain.Assets().GetObservation(id_stable, 0)};
    BOOST_REQUIRE(obs);
}

BOOST_AUTO_TEST_CASE(routes_across_kinds)
{
    Chain chain;
    const AssetId A{HashName("AAA")}, B{HashName("BBB")}, C{HashName("CCC")};
    const auto a{chain.Issue("AAA", 100'000'000, {20'000'000, 100'000, 100'000})};
    const auto b{chain.Issue("BBB", 100'000'000, {20'000'000, 20'000'000})};
    const auto c{chain.Issue("CCC", 100'000'000, {5'000'000}, 8)};
    // A-B stable, B-C weighted 80/20 (B 80 %), C-CHN at 1 % constant product.
    chain.Create(a[0], A, 20'000'000, b[0], B, 20'000'000, Curve::STABLE, 500, 50);
    chain.Create(b[1], B, 20'000'000, c[0], C, 5'000'000, Curve::WEIGHTED, 10000, 0, 80);
    const uint256 ab{PoolIdOf(A, B, Kind(Curve::STABLE, 500, 50))};
    const uint256 bc{PoolIdOf(B, C, Kind(Curve::WEIGHTED, 10000, 0, B < C ? 80 : 20))};
    chain.EndBlock();
    // The search finds the route through the kinds; alone in its block, the order gets exactly its quote.
    const auto found{dex::FindRoutes(chain.Assets(), A, C, 100'000, 3)};
    BOOST_REQUIRE(!found.empty());
    BOOST_CHECK(found[0].pools == (std::vector<uint256>{ab, bc}));
    BOOST_CHECK(!found[0].legacy);
    std::vector<AssetId> path;
    const auto quote{dex::QuoteRoutePools(chain.Assets(), A, {ab, bc}, 100'000, &path)};
    BOOST_REQUIRE(quote);
    BOOST_CHECK(path == (std::vector<AssetId>{A, B, C}));
    BOOST_CHECK_EQUAL(quote->back(), found[0].outs.back());
    // Without the kinds (a Route names assets): no route.
    BOOST_CHECK(dex::FindRoutes(chain.Assets(), A, C, 100'000, 3, 5, 20000, 16, /*curves=*/false).empty());
    const CTransaction order{MakeTx({a[1]}, SwapPools{A, {ab, bc}, 100'000, quote->back(), CScript{}}, {std::nullopt})};
    chain.Require(order);
    // Policy: one that would not fill alone is not relayed.
    std::string fills;
    BOOST_CHECK(chain.Assets().OrderFillsAlone(order, fills, chain.height));
    const CTransaction greedy{MakeTx({a[2]}, SwapPools{A, {ab, bc}, 100'000, quote->back() * 2, CScript{}}, {std::nullopt})};
    std::string why;
    BOOST_CHECK(!chain.Assets().OrderFillsAlone(greedy, why, chain.height));
    chain.EndBlock();
    BOOST_CHECK(chain.TokenAt(COutPoint{order.GetHash(), 0}) == AssetToken(C, quote->back()));
    // Pools that do not chain, or repeat an asset: refused.
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({a[2]}, SwapPools{A, {bc}, 1000, 1, CScript{}}, {std::nullopt})), "bad-ba-no-pool");
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({a[2]}, SwapPools{A, {ab, ab}, 1000, 1, CScript{}}, {std::nullopt})), "bad-ba-no-pool");
}

BOOST_AUTO_TEST_CASE(liquidity_one_side)
{
    Chain chain;
    const AssetId GOLD{HashName("GOLD")};
    const auto g{chain.Issue("GOLD", 100'000'000, {10'000, 1'000, 1'000})};
    // GOLD with CHN, stable (2 and 8 decimals: GOLD's units count 10^6 satoshis): 100 GOLD, 100 CHN.
    const CTransaction made{chain.Create(g[0], GOLD, 10'000, std::nullopt, CHN, 10'000'000'000, Curve::STABLE, 500, 100)};
    const uint256 id{PoolIdOf(GOLD, CHN, Kind(Curve::STABLE, 500, 100))};
    const PoolParams params{chain.Assets().GetPoolParams(id)};
    BOOST_CHECK_EQUAL(GOLD < CHN ? params.scale0 : params.scale1, 6);
    BOOST_CHECK_CLOSE(curves::SpotPrice(params, chain.PoolAt(id).reserve0, chain.PoolAt(id).reserve1), CHN < GOLD ? 1e-6 : 1e6, 1e-6);
    chain.EndBlock();
    // GOLD alone in: shares for it, all of it in the pool, the fee on what a swap would have traded.
    const Pool before{chain.PoolAt(id)};
    const bool gold0{before.asset0 == GOLD};
    const auto expect{curves::DepositShares(params, params.fee, before, gold0 ? 1'000 : 0, gold0 ? 0 : 1'000)};
    BOOST_REQUIRE(expect);
    PoolDeposit deposit{id, gold0 ? 1'000U : 0U, gold0 ? 0U : 1'000U, *expect + 1};
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({g[1]}, deposit, {std::nullopt})), "bad-ba-liquidity-price");
    deposit.min_shares = *expect;
    const CTransaction tx{MakeTx({g[1]}, deposit, {std::nullopt})};
    chain.Require(tx);
    BOOST_CHECK(chain.TokenAt(COutPoint{tx.GetHash(), 0}) == (Token{Token::Kind::LP, id, *expect}));
    BOOST_CHECK_EQUAL(chain.PoolAt(id).shares, before.shares + *expect);
    // Out again as CHN alone, by the coinbase: what the shares are worth of CHN plus what their GOLD buys.
    const Pool now{chain.PoolAt(id)};
    const uint8_t chn_side{gold0 ? uint8_t{2} : uint8_t{1}};
    const auto [o0, o1]{curves::WithdrawOf(params, params.fee, now, *expect, chn_side)};
    const uint64_t chn_out{gold0 ? o1 : o0};
    BOOST_CHECK_GT(chn_out, 0U);
    PoolWithdraw withdraw{id, *expect, chn_side, 0, 0, PAYEE};
    chain.Require(MakeTx({COutPoint{tx.GetHash(), 0}}, withdraw, {}));
    BOOST_REQUIRE_EQUAL(chain.payouts.size(), 1U);
    BOOST_CHECK_EQUAL(chain.payouts[0].nValue, static_cast<CAmount>(chn_out));
    BOOST_CHECK(chain.payouts[0].scriptPubKey == PAYEE);
    // A round trip: less CHN than the GOLD was worth at the price before (the fees stay with the pool).
    BOOST_CHECK_LT(chn_out, 1'000U * 1'000'000U);
    BOOST_CHECK_GT(chn_out, 1'000U * 1'000'000U * 99 / 100);
    // The shares nobody holds stay: the creator's shares out, the pool closes, and orders through it are refused.
    const auto shares{chain.TokenAt(COutPoint{made.GetHash(), 0})->amount};
    chain.Require(MakeTx({COutPoint{made.GetHash(), 0}}, PoolWithdraw{id, shares, 0, 0, 0, PAYEE}, {std::nullopt}));
    BOOST_CHECK(amm::Abandoned(chain.PoolAt(id)));
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({g[2]}, SwapPools{GOLD, {id}, 1000, 1, PAYEE}, {std::nullopt})), "bad-ba-pool-closed");
}

BOOST_AUTO_TEST_CASE(release_takes_the_kinds)
{
    // An asset retired: its pools of every kind go, their kinds with them.
    Chain chain;
    const AssetId DUST{HashName("DUST")};
    const auto d{chain.Issue("DUST", 2'000'000, {1'000'000})};
    const CTransaction made{chain.Create(d[0], DUST, 1'000'000, std::nullopt, CHN, 100'000'000, Curve::WEIGHTED, 3000, 0, 60)};
    const uint256 id{PoolIdOf(DUST, CHN, Kind(Curve::WEIGHTED, 3000, 0, DUST < CHN ? 60 : 40))};
    BOOST_CHECK(chain.Assets().HasPoolParams(id));
    // Everyone out; the rest of the supply and the control coin burned: only the pool's dust is left.
    const auto shares{chain.TokenAt(COutPoint{made.GetHash(), 0})->amount};
    const CTransaction out{MakeTx({COutPoint{made.GetHash(), 0}}, PoolWithdraw{id, shares, 0, 0, 0, PAYEE}, {std::nullopt})};
    chain.Require(out);
    BOOST_CHECK(amm::Abandoned(chain.PoolAt(id)));
    const uint64_t got{chain.TokenAt(COutPoint{out.GetHash(), 0})->amount};
    chain.Require(MakeTx({d[1], COutPoint{out.GetHash(), 0}, chain.control.at(DUST)},
                         Burn{{AssetToken(DUST, 1'000'000), AssetToken(DUST, got), Token{Token::Kind::CONTROL, DUST, 1}}}, {}));
    std::string why;
    BOOST_CHECK_MESSAGE(chain.Assets().Releasable(DUST, &why), why);
    chain.Require(MakeTx({}, ReleaseAsset{DUST}, {}));
    BOOST_CHECK(!chain.Assets().GetPool(id));
    BOOST_CHECK(!chain.Assets().HasPoolParams(id));
}

BOOST_AUTO_TEST_CASE(curve_work_budget)
{
    // A block holds at most MAX_BLOCK_CURVE_WORK of the pool kinds' work: one more is left for the next
    // block (the miner skips it; a block with it is not valid), orders through constant-product pools of any
    // fee tier count none.
    Chain chain;
    const AssetId GOLD{HashName("GOLD")}, SILVER{HashName("SILVER")};
    auto g{chain.Issue("GOLD", 1'000'000'000, {10'000'000, 10'000'000})};
    // Coins of 1000, 50 a transaction (a marker lists at most 64 outputs).
    COutPoint rest{g.back()};
    uint64_t left{1'000'000'000 - 20'000'000};
    g.pop_back();
    while (g.size() < MAX_BLOCK_CURVE_WORK + 5) {
        std::vector<std::optional<Token>> outs(50, AssetToken(GOLD, 1000));
        outs.push_back(AssetToken(GOLD, left - 50'000));
        const CTransaction split{MakeTx({rest}, std::nullopt, outs)};
        chain.Require(split);
        for (uint32_t i{0}; i < 50; ++i) g.emplace_back(split.GetHash(), i);
        rest = COutPoint{split.GetHash(), 50};
        left -= 50'000;
    }
    const auto s{chain.Issue("SILVER", 100'000'000, {10'000'000, 10'000'000})};
    chain.Create(g[0], GOLD, 10'000'000, s[0], SILVER, 10'000'000, Curve::STABLE, 500, 100);
    chain.Create(g[1], GOLD, 10'000'000, s[1], SILVER, 10'000'000, Curve::CONSTANT_PRODUCT, 100);
    chain.EndBlock();
    const uint256 stable{PoolIdOf(GOLD, SILVER, Kind(Curve::STABLE, 500, 100))}, cp{PoolIdOf(GOLD, SILVER, Kind(Curve::CONSTANT_PRODUCT, 100))};
    for (uint32_t i{0}; i < MAX_BLOCK_CURVE_WORK; ++i) chain.Require(MakeTx({g[2 + i]}, SwapPools{GOLD, {stable}, 1000, 1, CScript{}}, {std::nullopt}));
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({g[2 + MAX_BLOCK_CURVE_WORK]}, SwapPools{GOLD, {stable}, 1000, 1, CScript{}}, {std::nullopt})), "bad-ba-block-curve-work-full");
    // A constant-product pool of another tier is not searched: no work.
    chain.Require(MakeTx({g[3 + MAX_BLOCK_CURVE_WORK]}, SwapPools{GOLD, {cp}, 1000, 1, CScript{}}, {std::nullopt}));
    chain.EndBlock();
    // The next block starts afresh.
    chain.Require(MakeTx({g[2 + MAX_BLOCK_CURVE_WORK]}, SwapPools{GOLD, {stable}, 1000, 1, CScript{}}, {std::nullopt}));
    chain.EndBlock();
}

BOOST_AUTO_TEST_CASE(index_prices_of_kinds)
{
    // The search index keeps a pool's second reserve as its first times its spot price (PricedReserve1): the reserves'
    // ratio is the price for every kind, as for constant product (whose rows are as they always were).
    Chain chain;
    const AssetId GOLD{HashName("GOLD")}, SILVER{HashName("SILVER")};
    const auto g{chain.Issue("GOLD", 100'000'000, {10'000'000, 10'000'000, 10'000'000})};
    const auto s{chain.Issue("SILVER", 100'000'000, {10'000'000, 2'000'000, 30'000'000})};
    chain.Create(g[0], GOLD, 10'000'000, s[0], SILVER, 10'000'000, Curve::CONSTANT_PRODUCT, 500);
    // An unbalanced stable pool: its price is near 1, its reserves' ratio 5.
    chain.Create(g[1], GOLD, 10'000'000, s[1], SILVER, 2'000'000, Curve::STABLE, 100, 200);
    chain.Create(g[2], GOLD, 10'000'000, s[2], SILVER, 30'000'000, Curve::WEIGHTED, 3000, 0, GOLD < SILVER ? 25 : 75);
    const auto state{chain.Assets()};
    for (const auto& [id, pool] : state.PoolsOf(GOLD)) {
        const PoolParams params{state.GetPoolParams(id)};
        const uint64_t priced{index::PricedReserve1(state, id, pool)};
        if (params.GetCurve() == Curve::CONSTANT_PRODUCT) {
            BOOST_CHECK_EQUAL(priced, pool.reserve1);
        } else {
            BOOST_CHECK_CLOSE(static_cast<double>(priced) / pool.reserve0, curves::SpotPrice(params, pool.reserve0, pool.reserve1), 1e-4);
        }
        if (params.GetCurve() == Curve::STABLE) {
            const double ratio{static_cast<double>(pool.reserve1) / pool.reserve0};
            BOOST_CHECK(std::abs(static_cast<double>(priced) / pool.reserve0 - 1) < 0.1);
            BOOST_CHECK(std::abs(ratio - 1) > 0.5);
        }
    }
}

BOOST_AUTO_TEST_CASE(fee_parts_exact)
{
    // Each part is the floor of its share of the fee; the liquidity providers (or a launch's pool) keep the remainder:
    // the parts never exceed the fee and add up to it with the remainder, whatever the shares (Layers' and Flush's).
    struct Shares {
        uint32_t security, protocol;
    };
    for (const Shares sh : {Shares{110'000, 0}, Shares{110'000, 40'000}, Shares{0, 0}, Shares{1'000'000, 0}}) {
        for (uint64_t fee : {uint64_t{0}, uint64_t{1}, uint64_t{9}, uint64_t{10}, uint64_t{1'000'003}, MAX_AMOUNT}) {
            const FeeParts p{SwapFeeParts(fee, sh.security, sh.protocol)};
            BOOST_CHECK_EQUAL(p.security, static_cast<uint64_t>(static_cast<unsigned __int128>(fee) * sh.security / FEE_DENOMINATOR));
            BOOST_CHECK_EQUAL(p.protocol, static_cast<uint64_t>(static_cast<unsigned __int128>(fee) * sh.protocol / FEE_DENOMINATOR));
            BOOST_CHECK_LE(p.security + p.protocol, fee);
        }
    }
    // Flush: swaps 85 / 11 / 4 of a fee of 1 000 003 units: 110 000 and 40 000, the providers 850 003.
    const FeeParts swap{SwapFeeParts(1'000'003, 110'000, 40'000)};
    BOOST_CHECK_EQUAL(swap.security, 110'000U);
    BOOST_CHECK_EQUAL(swap.protocol, 40'000U);
    // Flush: a launch's 1.15 % of 1 ECX (1 150 000 satoshis): creator 50 %, protocol 30 %, security 20 %, nothing left.
    const uint64_t fee{uint64_t{100'000'000} * 11'500 / FEE_DENOMINATOR};
    BOOST_CHECK_EQUAL(fee, 1'150'000U);
    const FeeParts launch{LaunchFeeParts(fee, 500'000, 300'000, 200'000)};
    BOOST_CHECK_EQUAL(launch.creator, 575'000U);
    BOOST_CHECK_EQUAL(launch.protocol, 345'000U);
    BOOST_CHECK_EQUAL(launch.security, 230'000U);
    // Of 115 satoshis: 57, 34, 23 and 1 left for the pool (the remainder rule).
    const FeeParts small{LaunchFeeParts(115, 500'000, 300'000, 200'000)};
    BOOST_CHECK_EQUAL(small.creator, 57U);
    BOOST_CHECK_EQUAL(small.protocol, 34U);
    BOOST_CHECK_EQUAL(small.security, 23U);
    BOOST_CHECK_EQUAL(115 - small.creator - small.protocol - small.security, 1U);
    // Layers: creator 80 %, security 20 %, protocol 0.
    const FeeParts layers{LaunchFeeParts(1'000'001, 800'000, 0, 200'000)};
    BOOST_CHECK_EQUAL(layers.creator + layers.security + layers.protocol, 1'000'000U);
    // Shares of more than a whole together never take more than the fee.
    const FeeParts over{LaunchFeeParts(1000, 900'000, 300'000, 200'000)};
    BOOST_CHECK_LE(over.creator + over.protocol + over.security, 1000U);
}

BOOST_AUTO_TEST_CASE(fee_parts_conserved_and_undone)
{
    // A block of swaps both ways through a pool with the chain's coin and a stable pool: each pool keeps the round's
    // reserves less the security and protocol parts of each side's fee; the coin's security part is the block's
    // (BlockSecurityFees: paid with its fees), an asset's held by the treasury, the protocol's parts the treasury's. The
    // block taken back takes all of it back.
    Chain chain;
    const auto& rules{Params().GetConsensus().sidechain};
    const AssetId GOLD{HashName("GOLD")}, SILVER{HashName("SILVER")};
    const auto g{chain.Issue("GOLD", 100'000'000, {10'000'000, 10'000'000, 70'000, 90'000})};
    const auto s{chain.Issue("SILVER", 100'000'000, {10'000'000, 80'000})};
    chain.Create(g[0], GOLD, 10'000'000, std::nullopt, CHN, 500'000'000, Curve::CONSTANT_PRODUCT, 10'000);
    chain.Create(g[1], GOLD, 10'000'000, s[0], SILVER, 10'000'000, Curve::STABLE, 500, 100);
    chain.EndBlock();
    const uint256 cp{PoolIdOf(GOLD, CHN, Kind(Curve::CONSTANT_PRODUCT, 10'000))}, st{PoolIdOf(GOLD, SILVER, Kind(Curve::STABLE, 500, 100))};
    const Pool cp_before{chain.PoolAt(cp)}, st_before{chain.PoolAt(st)};
    const bool gold_first{GOLD < SILVER};
    chain.Require(MakeTx({}, SwapPools{CHN, {cp}, 30'000'000, 1, HOLDER}, {std::nullopt}, 30'000'000));
    chain.Require(MakeTx({g[2]}, SwapPools{GOLD, {cp}, 70'000, 1, HOLDER}, {std::nullopt}));
    chain.Require(MakeTx({g[3]}, SwapPools{GOLD, {st}, 90'000, 1, CScript{}}, {std::nullopt}));
    chain.Require(MakeTx({s[1]}, SwapPools{SILVER, {st}, 80'000, 1, CScript{}}, {std::nullopt}));
    const uint256 hash_before{chain.state.Hash()};
    chain.store.TakeUndo();
    chain.EndBlock();
    const int block{chain.height - 1};
    const auto parts{[&](uint64_t in, uint32_t fee) { return SwapFeeParts(static_cast<uint64_t>(static_cast<unsigned __int128>(in) * fee / FEE_DENOMINATOR), rules.dex_fee_security_share, rules.dex_fee_protocol_share); }};
    // The pool with CHN (asset0: CHN): 30 000 000 CHN and 70 000 GOLD in, at 1 %.
    const FeeParts cp_chn{parts(30'000'000, 10'000)}, cp_gold{parts(70'000, 10'000)};
    const auto cp_round{curves::ClearPoolOf(Kind(Curve::CONSTANT_PRODUCT, 10'000), 10'000, cp_before.reserve0, cp_before.reserve1, {{true, 30'000'000}, {false, 70'000}})};
    BOOST_REQUIRE(cp_round);
    BOOST_CHECK_EQUAL(chain.PoolAt(cp).reserve0, cp_round->reserve0 - cp_chn.security - cp_chn.protocol);
    BOOST_CHECK_EQUAL(chain.PoolAt(cp).reserve1, cp_round->reserve1 - cp_gold.security - cp_gold.protocol);
    // The stable pool: 90 000 GOLD and 80 000 SILVER in, at 0.05 %.
    const FeeParts st_gold{parts(90'000, 500)}, st_silver{parts(80'000, 500)};
    const auto st_round{curves::ClearPoolOf(Kind(Curve::STABLE, 500, 100), 500, st_before.reserve0, st_before.reserve1, {{gold_first, 90'000}, {!gold_first, 80'000}})};
    BOOST_REQUIRE(st_round);
    const FeeParts& st0{gold_first ? st_gold : st_silver};
    const FeeParts& st1{gold_first ? st_silver : st_gold};
    BOOST_CHECK_EQUAL(chain.PoolAt(st).reserve0, st_round->reserve0 - st0.security - st0.protocol);
    BOOST_CHECK_EQUAL(chain.PoolAt(st).reserve1, st_round->reserve1 - st1.security - st1.protocol);
    // Where the parts went, exactly.
    BOOST_CHECK_EQUAL(chain.Assets().BlockSecurityFees(block), static_cast<CAmount>(cp_chn.security));
    BOOST_CHECK_EQUAL(chain.Assets().GetTreasury(GOLD).value_or(TreasuryEntry{}).security_held, cp_gold.security + st_gold.security);
    BOOST_CHECK_EQUAL(chain.Assets().GetTreasury(SILVER).value_or(TreasuryEntry{}).security_held, st_silver.security);
    BOOST_CHECK_EQUAL(chain.Assets().GetTreasury(GOLD).value_or(TreasuryEntry{}).balance, cp_gold.protocol + st_gold.protocol);
    BOOST_CHECK_EQUAL(chain.Assets().GetTreasury(CHN).value_or(TreasuryEntry{}).balance, cp_chn.protocol);
    BOOST_CHECK_GT(cp_chn.security, 0U);
    // The block taken back: the treasury, the block's security parts and the pools as they were.
    chain.store.Revert(chain.store.TakeUndo());
    BOOST_CHECK(chain.state.Hash() == hash_before);
    BOOST_CHECK(!chain.Assets().GetTreasury(GOLD));
    BOOST_CHECK_EQUAL(chain.Assets().BlockSecurityFees(block), 0);
    BOOST_CHECK(chain.PoolAt(cp) == cp_before);
}

BOOST_AUTO_TEST_CASE(security_parts_split_as_fees)
{
    // The block's security parts join its ordinary fees: the bidder and the validators share them by the curve, to the satoshi.
    for (const auto& [spacing, bidder_bps] : std::vector<std::pair<int64_t, int64_t>>{{60, 5000}, {240, 3500}, {420, 2000}, {900, 2000}}) {
        const pos::FeeSplit split{pos::SplitFees({}, {}, /*fees_without_coinbase=*/true, spacing, pos::FeeCurve::LINEAR, /*extra_ordinary=*/1'000'001)};
        BOOST_CHECK_EQUAL(split.ordinary, 1'000'001);
        BOOST_CHECK_EQUAL(split.bidder_ordinary, 1'000'001 * bidder_bps / 10'000);
        BOOST_CHECK_EQUAL(split.bidder_ordinary + split.validators, 1'000'001);
    }
}

BOOST_AUTO_TEST_CASE(fee_parts_conserve_every_kind)
{
    // Random orders, both ways, through small pools of every searched kind and constant product at the highest tier (and
    // one with a dynamic fee), many blocks: of each asset, what the orders paid in less what they got (fills and refunds)
    // is, exactly, what the pools gained plus what the treasury took (its balance and the security parts it holds). No
    // coin from nothing, none lost; a block taken back puts every pool and the treasury back.
    Chain chain;
    const AssetId GOLD{HashName("GOLD")}, SILVER{HashName("SILVER")};
    const bool g0{GOLD < SILVER};
    std::vector<uint64_t> gold_coins(40, 50'000), silver_coins(40, 50'000);
    gold_coins.insert(gold_coins.begin(), {150'000, 150'000, 150'000, 150'000});
    silver_coins.insert(silver_coins.begin(), {150'000, 150'000, 150'000, 150'000});
    const auto g{chain.Issue("GOLD", 100'000'000, gold_coins, 0)};
    const auto s{chain.Issue("SILVER", 100'000'000, silver_coins, 0)};
    chain.Create(g[0], GOLD, 150'000, s[0], SILVER, 150'000, Curve::CONSTANT_PRODUCT, 10'000);
    chain.Create(g[1], GOLD, 150'000, s[1], SILVER, 150'000, Curve::STABLE, 10'000, 50);
    chain.Create(g[2], GOLD, 150'000, s[2], SILVER, 150'000, Curve::WEIGHTED, 10'000, 0, 20);
    {
        CreatePool dynamic;
        dynamic.asset_a = GOLD;
        dynamic.asset_b = SILVER;
        dynamic.fee = 10'000;
        dynamic.flags = POOL_FLAG_DYNAMIC_FEE;
        dynamic.amount_a = 150'000;
        dynamic.amount_b = 150'000;
        dynamic.min_shares = 1;
        chain.Require(MakeTx({g[3], s[3]}, dynamic, {std::nullopt}));
    }
    chain.EndBlock();
    std::vector<uint256> pools;
    for (const auto& [id, pool] : chain.Assets().PoolsOf(GOLD)) pools.push_back(id);
    BOOST_REQUIRE_EQUAL(pools.size(), 4U);
    const auto held{[&]() {
        // Of GOLD and of SILVER: the pools' reserves and the treasury's holdings.
        std::pair<unsigned __int128, unsigned __int128> sum{0, 0};
        for (const uint256& id : pools) {
            const Pool p{chain.PoolAt(id)};
            sum.first += g0 ? p.reserve0 : p.reserve1;
            sum.second += g0 ? p.reserve1 : p.reserve0;
        }
        for (const auto& [asset, sum_ref] : {std::make_pair(GOLD, &sum.first), std::make_pair(SILVER, &sum.second)}) {
            const TreasuryEntry t{chain.Assets().GetTreasury(asset).value_or(TreasuryEntry{})};
            *sum_ref += t.balance + t.security_held;
        }
        return sum;
    }};
    FastRandomContext rng{/*fDeterministic=*/true};
    size_t gi{4}, si{4};
    uint256 last_hash;
    std::pair<unsigned __int128, unsigned __int128> last_held;
    for (int block{0}; block < 18 && gi < g.size() && si < s.size(); ++block) {
        const auto before{held()};
        std::vector<CTransaction> orders;
        unsigned __int128 in_gold{0}, in_silver{0};
        for (int k{0}, m{1 + static_cast<int>(rng.randrange(4))}; k < m && gi < g.size() && si < s.size(); ++k) {
            const uint256& pool{pools[rng.randrange(pools.size())]};
            // Up to a third of the pool's side: big orders against small pools.
            const uint64_t amount{1 + rng.randrange(50'000)};
            const bool gold_in{rng.randbool()};
            const COutPoint coin{gold_in ? g[gi++] : s[si++]};
            std::vector<std::optional<Token>> outs{std::nullopt, AssetToken(gold_in ? GOLD : SILVER, 50'000 - amount)};
            if (amount == 50'000) outs.pop_back();
            orders.push_back(MakeTx({coin}, SwapPools{gold_in ? GOLD : SILVER, {pool}, amount, 1, CScript{}}, outs));
            chain.Require(orders.back());
            (gold_in ? in_gold : in_silver) += amount;
        }
        const uint256 hash_before{chain.state.Hash()};
        chain.store.TakeUndo();
        chain.EndBlock();
        unsigned __int128 out_gold{0}, out_silver{0};
        for (const CTransaction& tx : orders) {
            const auto got{chain.TokenAt(COutPoint{tx.GetHash(), 0})};
            BOOST_REQUIRE(got);
            (got->id == GOLD ? out_gold : out_silver) += got->amount;
        }
        const auto after{held()};
        BOOST_CHECK(after.first + out_gold == before.first + in_gold);
        BOOST_CHECK(after.second + out_silver == before.second + in_silver);
        last_hash = hash_before;
        last_held = before;
    }
    // The last block taken back: every pool and the treasury as they were.
    chain.store.Revert(chain.store.TakeUndo());
    BOOST_CHECK(chain.state.Hash() == last_hash);
    BOOST_CHECK(held() == last_held);
    chain.EndBlock();
    // The security parts of fees in assets: held, none paid with blocks (no coin in these pools). (Before the last block, taken back.)
    BOOST_CHECK_GT(chain.Assets().GetTreasury(GOLD).value_or(TreasuryEntry{}).security_held + chain.Assets().GetTreasury(SILVER).value_or(TreasuryEntry{}).security_held, 0U);
    BOOST_CHECK_EQUAL(chain.Assets().GetSecurityFees().total, 0U);
}

namespace {
/** Regtest with the DEX from the first block and no treasury from block 102 (the Chain starts at 100). */
struct NoTreasurySetup : BasicTestingSetup {
    NoTreasurySetup() : BasicTestingSetup{ChainType::REGTEST, {.extra_args = {"-sidechainslot=7", "-testactivationheight=dex@0", "-testactivationheight=dexcurves@0", "-testactivationheight=notreasury@102"}}} {}
};
} // namespace

BOOST_FIXTURE_TEST_CASE(no_treasury_from_its_height, NoTreasurySetup)
{
    Chain chain;
    const auto& rules{Params().GetConsensus().sidechain};
    BOOST_REQUIRE(!rules.NoTreasury(101) && rules.NoTreasury(102));
    BOOST_REQUIRE_GT(rules.dex_fee_protocol_share, 0U);
    const AssetId GOLD{HashName("GOLD")};
    const auto g{chain.Issue("GOLD", 100'000'000, {10'000'000, 70'000, 70'000})};
    chain.Create(g[0], GOLD, 10'000'000, std::nullopt, CHN, 500'000'000, Curve::CONSTANT_PRODUCT, 10'000);
    chain.EndBlock();
    const uint256 cp{PoolIdOf(GOLD, CHN, Kind(Curve::CONSTANT_PRODUCT, 10'000))};
    // Before (block 101): the protocol's parts to the treasury, the asset's security part held by it.
    chain.Require(MakeTx({}, SwapPools{CHN, {cp}, 30'000'000, 1, HOLDER}, {std::nullopt}, 30'000'000));
    chain.Require(MakeTx({g[1]}, SwapPools{GOLD, {cp}, 70'000, 1, HOLDER}, {std::nullopt}));
    BOOST_REQUIRE_EQUAL(chain.height, 101);
    chain.EndBlock();
    const TreasuryEntry held{chain.Assets().GetTreasury(CHN).value_or(TreasuryEntry{})};
    BOOST_REQUIRE_GT(held.balance, 0U);
    BOOST_CHECK_GT(chain.Assets().GetTreasury(GOLD).value_or(TreasuryEntry{}).security_held, 0U);
    const TreasuryEntry gold_before{chain.Assets().GetTreasury(GOLD).value_or(TreasuryEntry{})};
    // From block 102: swaps both ways; the protocol's part stays in the pool, the GOLD side's security part too; only the
    // coin's security part leaves it, with the block's fees; and the treasury's coin is paid with block 102's fees.
    const Pool before{chain.PoolAt(cp)};
    chain.Require(MakeTx({}, SwapPools{CHN, {cp}, 30'000'000, 1, HOLDER}, {std::nullopt}, 30'000'000));
    chain.Require(MakeTx({g[2]}, SwapPools{GOLD, {cp}, 70'000, 1, HOLDER}, {std::nullopt}));
    chain.EndBlock();
    // 85 % providers, 15 % security (the protocol's 4 % joins the security part), and nothing to a treasury.
    BOOST_CHECK_EQUAL(rules.SwapSecurityShare(102), rules.dex_fee_security_share + rules.dex_fee_protocol_share);
    BOOST_CHECK_EQUAL(rules.SwapSecurityShare(101), rules.dex_fee_security_share);
    const FeeParts coin{SwapFeeParts(static_cast<uint64_t>(static_cast<unsigned __int128>(30'000'000) * 10'000 / FEE_DENOMINATOR), rules.dex_fee_security_share + rules.dex_fee_protocol_share, 0)};
    const auto round{curves::ClearPoolOf(Kind(Curve::CONSTANT_PRODUCT, 10'000), 10'000, before.reserve0, before.reserve1, {{true, 30'000'000}, {false, 70'000}})};
    BOOST_REQUIRE(round);
    BOOST_CHECK_EQUAL(chain.PoolAt(cp).reserve0, round->reserve0 - coin.security);
    BOOST_CHECK_EQUAL(chain.PoolAt(cp).reserve1, round->reserve1);
    BOOST_CHECK_EQUAL(chain.Assets().BlockSecurityFees(102), static_cast<CAmount>(coin.security + held.balance));
    // The treasury: its coin gone (paid), nothing more accrued, its record kept; what it held of GOLD stays, as it was.
    const TreasuryEntry after{chain.Assets().GetTreasury(CHN).value_or(TreasuryEntry{})};
    BOOST_CHECK_EQUAL(after.balance, 0U);
    BOOST_CHECK_EQUAL(after.from_swaps, held.from_swaps);
    BOOST_CHECK(chain.Assets().GetTreasury(GOLD).value_or(TreasuryEntry{}) == gold_before);
    // A launch made from the height: no protocol part, its creator's takes it.
    BOOST_CHECK_EQUAL(rules.LaunchProtocolShare(102), 0U);
    BOOST_CHECK_EQUAL(rules.LaunchCreatorShare(102), rules.launch_creator_share + rules.launch_protocol_share);
    BOOST_CHECK_EQUAL(rules.SwapProtocolShare(101), rules.dex_fee_protocol_share);
    BOOST_CHECK_EQUAL(rules.SwapProtocolShare(102), 0U);
}

BOOST_AUTO_TEST_SUITE_END()
