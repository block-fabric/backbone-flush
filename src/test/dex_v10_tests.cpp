// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Upgrade v10 (SidechainParams::v10_height, doc/layers.md "Upgrade v10"; owner decisions 2026-11-11): the extra markets
// retired (no new order, auction, pool kind or range; everything that closes the existing ones stays valid), a launch
// nothing is bought from cancelled by its creator (CancelLaunch: its units and a control coin back, the supply still
// fixed unless all of it is the creator's), and an existing asset put on a curve whatever empty pools of it there are
// (its graduation takes such a pool over). Before the height: the rules of before, unchanged.

#include <addresstype.h>
#include <bitassets/clmath.h>
#include <bitassets/curves.h>
#include <bitassets/dex.h>
#include <bitassets/state.h>
#include <chainparams.h>
#include <consensus/params.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sidechain/state.h>
#include <sidechain/store.h>
#include <streams.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <map>
#include <string>
#include <vector>

using namespace bitassets;

namespace {
const CScript HOLDER{GetScriptForDestination(WitnessV0KeyHash{uint160{}})};
const CScript CREATOR{GetScriptForDestination(WitnessV0KeyHash{uint160{std::vector<unsigned char>(20, 5)}})};
const CScript PAYEE{GetScriptForDestination(WitnessV0KeyHash{uint160{std::vector<unsigned char>(20, 7)}})};
const CScript PARTNER{GetScriptForDestination(WitnessV0KeyHash{uint160{std::vector<unsigned char>(20, 9)}})};
constexpr uint64_t COIN_UNITS{100'000'000};
constexpr int V10{200};
int g_nonce{0};

CTransaction MakeTx(const std::vector<COutPoint>& inputs, std::optional<Operation> op, const std::vector<std::optional<Token>>& outs, CAmount chn_in = 0,
                    const std::vector<CScript>& scripts = {})
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(++g_nonce % 250 + 1)}), static_cast<uint32_t>(500000 + g_nonce)});
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

Token Asset(const AssetId& id, uint64_t amount) { return Token{Token::Kind::ASSET, id, amount}; }

struct Chain {
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State state{store};
    Consensus::SidechainParams params;
    int height{100};
    uint32_t time{1'800'000'000};
    std::vector<CTxOut> payouts, refunds, paid;
    //! The coin the markers burned, what the coinbases paid, and what a cancelled launch held that nobody is paid (dust).
    int64_t burned{0}, paid_out{0}, dropped{0};

    explicit Chain(int fee_flow = 0)
    {
        params.bitassets_reveal_depth = 0;
        params.dex_height = 0;
        params.dex_curves_height = 0;
        params.ticker_height = 0;
        params.logo_height = 0;
        params.launch_params_height = 0;
        params.fee_flow_height = fee_flow;
        params.v7_height = fee_flow;
        params.v8_height = 0;
        params.v9_height = 0;
        params.v10_height = V10;
        params.no_treasury_height = 0;
        params.small_payout_height = 0;
        params.allocation10_height = 0;
    }
    bool Apply(const CTransaction& tx, std::string& reason)
    {
        const bool ok{state.ApplyTx(tx, height, params, payouts, refunds, reason, false, true)};
        if (ok) {
            for (const CTxOut& out : tx.vout) {
                if (IsMarkerScript(out.scriptPubKey)) burned += out.nValue;
            }
        }
        return ok;
    }
    void Require(const CTransaction& tx)
    {
        std::string reason;
        BOOST_REQUIRE_MESSAGE(Apply(tx, reason), reason << " (operation " << (GetMarker(tx) && GetMarker(tx)->operation ? GetMarker(tx)->operation->index() : 99) << " at " << height << ")");
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
        for (const CTxOut& out : payouts) paid_out += out.nValue;
        paid_out += state.BitAssets().BlockSecurityFees(height);
        paid = payouts;
        payouts.clear();
        refunds.clear();
        ++height;
        time += 15;
        // Every block: the coin the assets hold is what came in less what went out (and what a cancel left nobody's).
        BOOST_CHECK_EQUAL(Assets().HeldChn(), burned - paid_out - dropped);
    }
    void Until(int h)
    {
        while (height < h) EndBlock();
    }
    bitassets::State Assets() const { return state.BitAssets(); }
    std::optional<Token> TokenAt(const COutPoint& out) const { return Assets().GetToken(out); }
    /** A reservation made at `script`: its output (what proves a launch's creator, at the creator's script). */
    COutPoint Proof(const CScript& script)
    {
        const CTransaction tx{MakeTx({}, Reserve{HashName("proof " + std::to_string(++g_nonce))}, {Token{Token::Kind::RESERVATION, uint256{}, 1}}, 0, {script})};
        Require(tx);
        return COutPoint{tx.GetHash(), 0};
    }
    /** Registers `name`, its supply in coins of `parts` (to HOLDER): the control coin's output, then the coins'. */
    std::pair<COutPoint, std::vector<COutPoint>> Issue(const std::string& name, const std::vector<uint64_t>& parts, uint8_t decimals = 0)
    {
        const AssetId id{HashName(name)};
        const uint256 nonce{HashName("nonce " + name)};
        const CTransaction reserved{MakeTx({}, Reserve{ReservationCommitment(id, nonce, HOLDER)}, {Token{Token::Kind::RESERVATION, uint256{}, 1}})};
        Require(reserved);
        Register reg;
        reg.name = id;
        reg.nonce = nonce;
        reg.decimals = decimals;
        reg.text = name;
        std::vector<std::optional<Token>> outs{Token{Token::Kind::CONTROL, id, 1}};
        for (const uint64_t part : parts) {
            reg.supply += part;
            outs.emplace_back(Asset(id, part));
        }
        const CTransaction tx{MakeTx({COutPoint{reserved.GetHash(), 0}}, reg, outs)};
        Require(tx);
        std::vector<COutPoint> coins;
        for (uint32_t i{0}; i < parts.size(); ++i) coins.emplace_back(tx.GetHash(), i + 1);
        return {COutPoint{tx.GetHash(), 0}, coins};
    }
    /** A launch of `name` by CREATOR (the launch's transaction): `keep` keeps the control coin (its output 0). */
    CTransaction Launch(const std::string& name, uint64_t first_buy = 0, bool keep = false, std::vector<FeeShare> split = {})
    {
        const AssetId id{HashName(name)};
        const uint256 nonce{HashName("nonce " + name)};
        const CTransaction reserved{MakeTx({}, Reserve{ReservationCommitment(id, nonce, HOLDER)}, {Token{Token::Kind::RESERVATION, uint256{}, 1}})};
        Require(reserved);
        LaunchToken launch;
        launch.reg.name = id;
        launch.reg.nonce = nonce;
        launch.reg.decimals = LAUNCH_DECIMALS;
        launch.reg.text = name;
        launch.creator = CREATOR;
        launch.first_buy = first_buy;
        launch.chn_to = first_buy ? PAYEE : CScript{};
        launch.keep_control = keep;
        launch.split = std::move(split);
        std::vector<std::optional<Token>> outs;
        if (keep) outs.emplace_back(Token{Token::Kind::CONTROL, id, 1});
        if (first_buy) outs.emplace_back();
        const CTransaction tx{MakeTx({COutPoint{reserved.GetHash(), 0}}, launch, outs, static_cast<CAmount>(first_buy))};
        Require(tx);
        return tx;
    }
    CTransaction Buy(const AssetId& token, uint64_t coin)
    {
        const CTransaction tx{MakeTx({}, SwapPools{CHN, {Assets().GetLaunch(token)->pool}, coin, 1, PAYEE}, {std::nullopt}, static_cast<CAmount>(coin))};
        Require(tx);
        return tx;
    }
    /** Sells all the units of `coins` into the curve of `token`. */
    CTransaction SellAll(const COutPoint& coins, const AssetId& token)
    {
        const CTransaction tx{MakeTx({coins}, SwapPools{token, {Assets().GetLaunch(token)->pool}, TokenAt(coins)->amount, 1, PAYEE}, {std::nullopt})};
        Require(tx);
        return tx;
    }
    /** What a cancel of `token`'s launch leaves nobody's: what its curve kept beyond its virtual coin, the fees kept for its pool, and what is owed under the least payout. */
    int64_t Dust(const AssetId& token) const
    {
        const LaunchRecord record{*Assets().GetLaunch(token)};
        const Pool curve{*Assets().GetPool(record.pool)};
        const uint64_t min_chn{MinChnPayout(height)};
        uint64_t dust{curve.reserve0 - record.virtual_coin + record.pool_fees};
        if (record.creator_fees + record.buyback < min_chn) dust += record.creator_fees + record.buyback;
        for (size_t i{0}; i < record.share_owed.size(); ++i) {
            if (record.split[i].kind != FeeShare::ADDRESS || record.share_owed[i] < min_chn) dust += record.share_owed[i];
        }
        return static_cast<int64_t>(dust);
    }
    /** `asset` (its control coin, and `coins` of exactly `amount`) on a curve at `fee`, by CREATOR. */
    CTransaction Curve(const AssetId& asset, const COutPoint& control, const COutPoint& coins, uint64_t amount, uint32_t fee = 10'000)
    {
        CurveAsset curve;
        curve.asset = asset;
        curve.amount = amount;
        curve.fee = fee;
        curve.creator = CREATOR;
        std::vector<std::optional<Token>> outs;
        if (const uint64_t rest{TokenAt(coins)->amount - amount}) outs.emplace_back(Asset(asset, rest));
        return MakeTx({control, coins}, curve, outs);
    }
    /** A position of `amount` of each asset at most in a concentrated pool, between two ticks: its transaction (output 0: its token). */
    CTransaction Open(const uint256& pool, int32_t lower, int32_t upper, const COutPoint& coin0, const COutPoint& coin1, uint64_t amount)
    {
        const arith_uint256 price{UintToArith256(Assets().GetClState(pool)->sqrt_price)};
        const unsigned __int128 l{cl::LiquidityForAmounts(price, cl::SqrtRatioAtTick(lower), cl::SqrtRatioAtTick(upper), amount, amount)};
        OpenPosition open;
        open.pool = pool;
        open.tick_lower = lower;
        open.tick_upper = upper;
        open.liquidity_hi = static_cast<uint64_t>(l >> 64);
        open.liquidity_lo = static_cast<uint64_t>(l);
        open.max0 = amount;
        open.max1 = amount;
        const auto needed{cl::AmountsForLiquidity(price, cl::SqrtRatioAtTick(lower), cl::SqrtRatioAtTick(upper), l, true)};
        std::vector<std::optional<Token>> outs{std::nullopt};
        if (needed->first < amount) outs.emplace_back();
        if (needed->second < amount) outs.emplace_back();
        return MakeTx({coin0, coin1}, open, outs);
    }
};

/** Every entry of a store: what a reorganization has to put back, to the byte. */
std::map<sidechain::StoreBytes, sidechain::StoreBytes> Dump(const sidechain::StoreView& view)
{
    std::map<sidechain::StoreBytes, sidechain::StoreBytes> all;
    sidechain::StoreBytes at;
    while (const auto entry{view.Next(at, {})}) {
        all.emplace(entry->first, entry->second);
        at = entry->first;
        at.push_back(0);
    }
    return all;
}

std::pair<int32_t, int32_t> FullRange(uint32_t fee)
{
    const int32_t spacing{cl::TickSpacing(fee)};
    return {-(cl::MAX_TICK / spacing) * spacing, (cl::MAX_TICK / spacing) * spacing};
}

struct V10Setup : BasicTestingSetup {
    V10Setup() : BasicTestingSetup{ChainType::REGTEST, {.extra_args = {"-sidechainslot=7", "-testactivationheight=dex@0", "-testactivationheight=dexcurves@0", "-testactivationheight=feeflow@0",
                                                                       "-testactivationheight=v7@0", "-testactivationheight=v8@0", "-testactivationheight=v9@0", "-testactivationheight=v10@200"}}} {}
};
/** The fee-flow release from block 150: a launch made before it is one of before (no `v5` record). */
struct V10OldLaunchSetup : BasicTestingSetup {
    V10OldLaunchSetup() : BasicTestingSetup{ChainType::REGTEST, {.extra_args = {"-sidechainslot=7", "-testactivationheight=dex@0", "-testactivationheight=dexcurves@0", "-testactivationheight=feeflow@150",
                                                                                "-testactivationheight=v7@150", "-testactivationheight=v8@0", "-testactivationheight=v9@0", "-testactivationheight=v10@200"}}} {}
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(dex_v10_tests, V10Setup)

BOOST_AUTO_TEST_CASE(extra_markets_retired)
{
    Chain chain;
    const AssetId A{HashName("AAA")}, B{HashName("BBB")};
    BOOST_REQUIRE(A < B || B < A);
    const uint64_t M{1'000'000};
    const auto [control_a, a]{chain.Issue("AAA", std::vector<uint64_t>(30, M))};
    const auto [control_b, b]{chain.Issue("BBB", std::vector<uint64_t>(30, M))};
    const bool a0{A < B};
    size_t na{0}, nb{0};
    const auto coin0{[&] { return a0 ? a[na++] : b[nb++]; }};
    const auto coin1{[&] { return a0 ? b[nb++] : a[na++]; }};
    BOOST_REQUIRE(!Params().GetConsensus().sidechain.V10Active(chain.height));

    // Before the height, every market there was: an order, two auctions, a stable pool, a concentrated pool with a
    // ranged and a full-range position.
    const CTransaction order{MakeTx({a[na++]}, PlaceOrder{A, B, M, 1, 1, chain.height + 100'000}, {std::nullopt})};
    chain.Require(order);
    const CTransaction resting{MakeTx({a[na++]}, PlaceOrder{A, B, M, 1, 1, chain.height + 100'000}, {std::nullopt})};
    chain.Require(resting);
    const CTransaction auction{MakeTx({a[na++]}, CreateAuction{A, M, B, M, M, chain.height, 100'000}, {Token{Token::Kind::RECEIPT, uint256{}, 1}})};
    chain.Require(auction);
    const CTransaction unsold{MakeTx({a[na++]}, CreateAuction{A, M, B, M, M, chain.height, 10}, {Token{Token::Kind::RECEIPT, uint256{}, 1}})};
    chain.Require(unsold);
    CreatePool stable;
    stable.asset_a = A;
    stable.asset_b = B;
    stable.curve = static_cast<uint8_t>(Curve::STABLE);
    stable.fee = 500;
    stable.amp = 100;
    stable.amount_a = stable.amount_b = M;
    const CTransaction made_stable{MakeTx({a[na++], b[nb++]}, stable, {std::nullopt})};
    chain.Require(made_stable);
    const uint256 stable_id{PoolIdOf(A, B, MakePoolParams(stable, 0, 0))};
    BOOST_REQUIRE(chain.Assets().GetPool(stable_id));
    CreatePool concentrated;
    concentrated.asset_a = A;
    concentrated.asset_b = B;
    concentrated.curve = static_cast<uint8_t>(Curve::CONCENTRATED);
    concentrated.fee = 3000;
    concentrated.amount_a = concentrated.amount_b = 1000;
    chain.Require(MakeTx({}, concentrated, {}));
    const uint256 cl_id{PoolIdOf(A, B, MakePoolParams(concentrated, 0, 0))};
    const auto [lower, upper]{FullRange(3000)};
    const CTransaction ranged{chain.Open(cl_id, -600, 600, coin0(), coin1(), M)};
    chain.Require(ranged);
    const CTransaction full{chain.Open(cl_id, lower, upper, coin0(), coin1(), M)};
    chain.Require(full);
    chain.EndBlock();
    chain.Until(V10);
    BOOST_REQUIRE(Params().GetConsensus().sidechain.V10Active(chain.height));

    // From the height: none of them starts.
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({a[na]}, PlaceOrder{A, B, M, 1, 1, chain.height + 1000}, {std::nullopt})), "bad-ba-retired-order");
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({a[na]}, CreateAuction{A, M, B, M, M, chain.height, 1000}, {Token{Token::Kind::RECEIPT, uint256{}, 1}})), "bad-ba-retired-auction");
    CreatePool other{stable};
    other.amp = 200;
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({a[na], b[nb]}, other, {std::nullopt})), "bad-ba-retired-pool-kind");
    CreatePool weighted;
    weighted.asset_a = A;
    weighted.asset_b = B;
    weighted.curve = static_cast<uint8_t>(Curve::WEIGHTED);
    weighted.fee = 3000;
    weighted.weight_a = 80;
    weighted.amount_a = weighted.amount_b = M;
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({a[na], b[nb]}, weighted, {std::nullopt})), "bad-ba-retired-pool-kind");
    CreatePool narrow{concentrated};
    narrow.fee = 500;
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({}, narrow, {})), "bad-ba-retired-pool-kind");
    CreatePool dynamic;
    dynamic.asset_a = A;
    dynamic.asset_b = B;
    dynamic.fee = 3000;
    dynamic.flags = POOL_FLAG_DYNAMIC_FEE;
    dynamic.amount_a = dynamic.amount_b = M;
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({a[na], b[nb]}, dynamic, {std::nullopt})), "bad-ba-retired-pool-kind");
    // The standard pool, at every fee tier, and the pool AddLiquidity makes: as before.
    for (const uint32_t fee : FEE_TIERS) {
        CreatePool standard;
        standard.asset_a = A;
        standard.asset_b = B;
        standard.fee = fee;
        standard.amount_a = standard.amount_b = M;
        if (fee == LEGACY_FEE) {
            chain.Require(MakeTx({a[na++], b[nb++]}, AddLiquidity{A, B, M, M, 1}, {std::nullopt, std::nullopt}));
            BOOST_CHECK_EQUAL(chain.Refused(MakeTx({a[na], b[nb]}, standard, {std::nullopt})), "bad-ba-pool-exists");
        } else {
            chain.Require(MakeTx({a[na++], b[nb++]}, standard, {std::nullopt}));
        }
    }

    // Positions: no new range; liquidity added over the full range only; any position reduced, collected and closed.
    const uint256 ranged_id{PositionId(ranged.GetHash())}, full_id{PositionId(full.GetHash())};
    BOOST_CHECK_EQUAL(chain.Refused(chain.Open(cl_id, -600, 600, coin0(), coin1(), M)), "bad-ba-retired-range");
    BOOST_CHECK_EQUAL(chain.Refused(chain.Open(cl_id, lower, 600, coin0(), coin1(), M)), "bad-ba-retired-range");
    const CTransaction full2{chain.Open(cl_id, lower, upper, coin0(), coin1(), M)};
    chain.Require(full2);
    const auto change{[&](const uint256& id, bool add, unsigned __int128 l, bool close) {
        ChangePosition change;
        change.position = id;
        change.add = add;
        change.liquidity_lo = static_cast<uint64_t>(l);
        change.limit0 = change.limit1 = add ? M : 0;
        change.close = close;
        return change;
    }};
    const unsigned __int128 ranged_l{chain.Assets().GetPosition(ranged_id)->Liquidity()};
    {
        // Adding needs both assets; the ranged position refuses before it looks at them.
        const CTransaction add{MakeTx({COutPoint{ranged.GetHash(), 0}, coin0(), coin1()}, change(ranged_id, true, 1000, false), {std::nullopt, std::nullopt, std::nullopt})};
        BOOST_CHECK_EQUAL(chain.Refused(add), "bad-ba-retired-range");
    }
    const CTransaction less{MakeTx({COutPoint{ranged.GetHash(), 0}}, change(ranged_id, false, ranged_l / 2, false), {std::nullopt, std::nullopt, std::nullopt})};
    chain.Require(less);
    BOOST_CHECK(chain.Assets().GetPosition(ranged_id)->Liquidity() == ranged_l - ranged_l / 2);
    const CTransaction closed{MakeTx({COutPoint{less.GetHash(), 0}}, change(ranged_id, false, 0, true), {std::nullopt, std::nullopt})};
    chain.Require(closed);
    BOOST_CHECK(!chain.Assets().GetPosition(ranged_id));
    {
        const unsigned __int128 before{chain.Assets().GetPosition(full_id)->Liquidity()};
        const arith_uint256 price{UintToArith256(chain.Assets().GetClState(cl_id)->sqrt_price)};
        const auto needed{cl::AmountsForLiquidity(price, cl::SqrtRatioAtTick(lower), cl::SqrtRatioAtTick(upper), 1000, true)};
        std::vector<std::optional<Token>> outs{std::nullopt};
        if (needed->first < M) outs.emplace_back();
        if (needed->second < M) outs.emplace_back();
        chain.Require(MakeTx({COutPoint{full.GetHash(), 0}, coin0(), coin1()}, change(full_id, true, 1000, false), outs));
        BOOST_CHECK(chain.Assets().GetPosition(full_id)->Liquidity() == before + 1000);
    }

    // The order of before: cancelled. The other still trades (a swap through the book), and so does the stable pool.
    chain.Require(MakeTx({COutPoint{order.GetHash(), 0}}, SettleOrder{OrderId(order.GetHash()), true, PAYEE}, {std::nullopt}));
    BOOST_CHECK(!chain.Assets().GetOrder(OrderId(order.GetHash())));
    const CTransaction take{MakeTx({b[nb++]}, SwapPools{B, {BookId(A, B)}, M / 2, 1, CScript{}}, {std::nullopt, Asset(B, M - M / 2)})};
    chain.Require(take);
    const CTransaction swap{MakeTx({a[na++]}, SwapPools{A, {stable_id}, M / 10, 1, CScript{}}, {std::nullopt, Asset(A, M - M / 10)})};
    chain.Require(swap);
    // The auctions of before: a bid buys, the receipt collects; the one nobody bid on is collected too.
    const CTransaction bid{MakeTx({b[nb++]}, Bid{auction.GetHash(), M, 1, CScript{}}, {std::nullopt})};
    chain.Require(bid);
    BOOST_CHECK(chain.TokenAt(COutPoint{bid.GetHash(), 0}) == Asset(A, M));
    chain.Require(MakeTx({COutPoint{auction.GetHash(), 0}}, Collect{auction.GetHash(), HOLDER}, {std::nullopt}));
    chain.Require(MakeTx({COutPoint{unsold.GetHash(), 0}}, Collect{unsold.GetHash(), HOLDER}, {std::nullopt}));
    chain.EndBlock();
    BOOST_CHECK(chain.TokenAt(COutPoint{take.GetHash(), 0}) == Asset(A, M / 2));
    const auto swapped{chain.TokenAt(COutPoint{swap.GetHash(), 0})};
    BOOST_CHECK(swapped && swapped->id == B && swapped->amount > 0);
    BOOST_CHECK_EQUAL(chain.Assets().GetOrder(OrderId(resting.GetHash()))->remaining, M / 2);

    // A stable pool of before takes deposits and gives them back; once everyone left, nobody opens it again.
    const uint256 stable_pool{stable_id};
    const CTransaction deposit{MakeTx({a[na++], b[nb++]}, PoolDeposit{stable_pool, a0 ? M : M, M, 1}, {std::nullopt})};
    chain.Require(deposit);
    const uint64_t shares1{chain.TokenAt(COutPoint{made_stable.GetHash(), 0})->amount}, shares2{chain.TokenAt(COutPoint{deposit.GetHash(), 0})->amount};
    chain.Require(MakeTx({COutPoint{made_stable.GetHash(), 0}}, PoolWithdraw{stable_pool, shares1, 0, 0, 0, PAYEE}, {std::nullopt, std::nullopt}));
    chain.Require(MakeTx({COutPoint{deposit.GetHash(), 0}}, PoolWithdraw{stable_pool, shares2, 0, 0, 0, PAYEE}, {std::nullopt, std::nullopt}));
    BOOST_REQUIRE(amm::Abandoned(*chain.Assets().GetPool(stable_pool)));
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({a[na], b[nb]}, PoolDeposit{stable_pool, M, M, 1}, {std::nullopt})), "bad-ba-retired-pool-kind");
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({a[na], b[nb]}, stable, {std::nullopt})), "bad-ba-retired-pool-kind");
    chain.EndBlock();
    (void)control_a;
    (void)control_b;
}

BOOST_AUTO_TEST_SUITE_END()
