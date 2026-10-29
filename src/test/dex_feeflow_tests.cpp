// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// The fee-flow release (SidechainParams::fee_flow_height, doc/layers.md "Fee flow"): a launch made from its height graduates
// into a locked full-range position of a concentrated pool at GRADUATION_FEE, whose fees go 60 % to its creator and 40 %
// to the chain's security; (on hold: a security part paid in an asset sold through its pool for the coin); a launch's fee
// destination (its creator, buyback and burn, or a split); the allocation at graduation (no vesting); the control coin kept.
// Conservation throughout: the coin the assets hold is what came in less what went out (HeldChn), and no token is lost.

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
#include <test/util/random.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <cmath>
#include <map>
#include <string>
#include <tuple>
#include <vector>

using namespace bitassets;

namespace {
const CScript HOLDER{GetScriptForDestination(WitnessV0KeyHash{uint160{}})};
const CScript CREATOR{GetScriptForDestination(WitnessV0KeyHash{uint160{std::vector<unsigned char>(20, 5)}})};
const CScript PAYEE{GetScriptForDestination(WitnessV0KeyHash{uint160{std::vector<unsigned char>(20, 7)}})};
constexpr uint64_t COIN_UNITS{100'000'000};
constexpr uint64_t G{500 * COIN_UNITS};
int g_nonce{0};

CTransaction MakeTx(const std::vector<COutPoint>& inputs, std::optional<Operation> op, const std::vector<std::optional<Token>>& outs, CAmount chn_in = 0,
                    const std::vector<CScript>& scripts = {})
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(++g_nonce % 250 + 1)}), static_cast<uint32_t>(400000 + g_nonce)});
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

/** A launch's options of the fee-flow release. */
struct V5Options {
    uint8_t destination{0};
    uint16_t burn_bps{0};
    bool keep_control{false};
};

struct Chain {
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State state{store};
    Consensus::SidechainParams params;
    int height{100};
    uint32_t time{1'800'000'000};
    std::vector<CTxOut> payouts, refunds, paid;
    int64_t burned{0}, paid_out{0}, security{0};

    Chain()
    {
        params.bitassets_reveal_depth = 0;
        params.dex_height = 0;
        params.dex_curves_height = 0;
        params.ticker_height = 0;
        params.logo_height = 0;
        params.launch_params_height = 0;
        params.fee_flow_height = 0;
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
        for (const CTxOut& out : payouts) paid_out += out.nValue;
        security += state.BitAssets().BlockSecurityFees(height);
        paid_out += state.BitAssets().BlockSecurityFees(height);
        paid = payouts;
        payouts.clear();
        refunds.clear();
        ++height;
        time += 15;
        // Every block: the coin the assets hold is what came in less what went out.
        BOOST_CHECK_EQUAL(Assets().HeldChn(), burned - paid_out);
    }
    bitassets::State Assets() const { return state.BitAssets(); }
    std::optional<Token> TokenAt(const COutPoint& out) const { return Assets().GetToken(out); }
    CTransaction Launch(const std::string& name, uint16_t allocation, uint64_t first_buy, const V5Options& v5 = {}, std::vector<std::optional<Token>> extra = {})
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
        launch.allocation = allocation;
        launch.first_buy = first_buy;
        launch.chn_to = first_buy ? PAYEE : CScript{};
        launch.destination = v5.destination;
        launch.burn_bps = v5.burn_bps;
        launch.keep_control = v5.keep_control;
        std::vector<std::optional<Token>> outs{std::move(extra)};
        if (first_buy) outs.emplace_back();
        const CTransaction tx{MakeTx({COutPoint{reserved.GetHash(), 0}}, launch, outs, static_cast<CAmount>(first_buy))};
        Require(tx);
        return tx;
    }
    CTransaction Buy(const uint256& pool, uint64_t coin, uint64_t min_out = 1)
    {
        const CTransaction tx{MakeTx({}, SwapPools{CHN, {pool}, coin, min_out, PAYEE}, {std::nullopt}, static_cast<CAmount>(coin))};
        Require(tx);
        return tx;
    }
    CTransaction Sell(const COutPoint& coins, const AssetId& asset, const uint256& pool, uint64_t amount)
    {
        const CTransaction tx{MakeTx({coins}, SwapPools{asset, {pool}, amount, 1, PAYEE}, {std::nullopt, Token{Token::Kind::ASSET, asset, TokenAt(coins)->amount - amount}})};
        Require(tx);
        return tx;
    }
    /** `name` launched and taken past its curve's end (graduated at the block's end): the record. */
    LaunchRecord Graduated(const std::string& name, uint16_t allocation, const V5Options& v5 = {})
    {
        Launch(name, allocation, COIN_UNITS, v5);
        EndBlock();
        Buy(Assets().GetLaunch(HashName(name))->pool, G + 200 * COIN_UNITS);
        EndBlock();
        const auto record{Assets().GetLaunch(HashName(name))};
        BOOST_REQUIRE(record && record->Graduated());
        return *record;
    }
};

struct FeeFlowSetup : BasicTestingSetup {
    FeeFlowSetup() : BasicTestingSetup{ChainType::REGTEST, {.extra_args = {"-sidechainslot=7", "-testactivationheight=dex@0", "-testactivationheight=dexcurves@0", "-testactivationheight=feeflow@0"}}} {}
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(dex_feeflow_tests, FeeFlowSetup)

BOOST_AUTO_TEST_CASE(graduates_into_a_locked_position)
{
    Chain chain;
    BOOST_REQUIRE(Params().GetConsensus().sidechain.FeeFlowActive(chain.height));
    const AssetId MOON{HashName("MOON")};
    const LaunchRecord record{chain.Graduated("MOON", 200)};
    BOOST_CHECK(record.v5);
    BOOST_CHECK_EQUAL(record.vesting, 0);
    // Its pool: concentrated at 1 %, the coin's and the token's, one full-range position nobody holds.
    BOOST_CHECK(record.pool == PoolIdOf(CHN, MOON, GraduationParams()));
    BOOST_CHECK(chain.Assets().GetPoolParams(record.pool).GetCurve() == Curve::CONCENTRATED);
    BOOST_CHECK_EQUAL(chain.Assets().GetPoolParams(record.pool).fee, GRADUATION_FEE);
    BOOST_CHECK(chain.Assets().IsLockedPool(record.pool));
    BOOST_CHECK(!chain.Assets().GetPool(PoolId(CHN, MOON)));
    const auto position{chain.Assets().GetPosition(record.position)};
    BOOST_REQUIRE(position);
    const int32_t spacing{cl::TickSpacing(GRADUATION_FEE)};
    BOOST_CHECK_EQUAL(position->tick_lower, -(cl::MAX_TICK / spacing) * spacing);
    BOOST_CHECK_EQUAL(position->tick_upper, (cl::MAX_TICK / spacing) * spacing);
    BOOST_CHECK(position->Liquidity() > 0);
    BOOST_CHECK(chain.Assets().GetClState(record.pool)->Liquidity() == position->Liquidity());
    BOOST_CHECK(!chain.Assets().FirstTokenOf(record.position, Token::Kind::POSITION));
    // The same reserves a pool of before gets: the coin raised and the fees for the pool, the tokens left and the pool's part.
    const Pool pool{*chain.Assets().GetPool(record.pool)};
    BOOST_CHECK_GE(pool.reserve0, record.raised_at_graduation);
    BOOST_CHECK_GE(pool.reserve0, G);
    BOOST_CHECK_GE(pool.reserve1, record.PoolSupply());
    // Nobody can take it out: a change of the position needs its token, which nobody has.
    ChangePosition close;
    close.position = record.position;
    close.close = true;
    close.chn_to = PAYEE;
    BOOST_CHECK(!chain.Refused(MakeTx({}, close, {std::nullopt})).empty());
}

BOOST_AUTO_TEST_CASE(locked_fees_creator_and_security)
{
    Chain chain;
    const AssetId MOON{HashName("MOON")};
    const LaunchRecord record{chain.Graduated("MOON", 0)};
    const uint64_t creator_before{record.creator_fees};
    const int64_t security_before{chain.security};
    // Buys pay their fee in the coin; sales in the token.
    const CTransaction bought{chain.Buy(record.pool, 50 * COIN_UNITS)};
    chain.EndBlock();
    const auto after_buy{*chain.Assets().GetLaunch(MOON)};
    const uint64_t creator_coin{after_buy.creator_fees - creator_before};
    const int64_t security_coin{chain.security - security_before};
    BOOST_CHECK_GT(creator_coin, 0U);
    // 1 % of 50: 0.5 coins; the swap's 15 % to security, the locked position's 85 %: 60 % to the creator, 40 % to security.
    const uint64_t expected{50 * COIN_UNITS / 100 * 85 / 100 * 60 / 100};
    BOOST_CHECK_LE(creator_coin, expected + 2);
    BOOST_CHECK_GE(creator_coin + 2, expected);
    const int64_t expected_security{static_cast<int64_t>(50 * COIN_UNITS / 100 * 15 / 100 + 50 * COIN_UNITS / 100 * 85 / 100 * 40 / 100)};
    BOOST_CHECK_LE(security_coin, expected_security + 4);
    BOOST_CHECK_GE(security_coin + 4, expected_security);
    // A sale: its fee in the coin too (out of what it gets): the creator's part in the coin, nothing in the token.
    const COutPoint coins{bought.GetHash(), 0};
    const uint64_t held{chain.TokenAt(coins)->amount};
    chain.Sell(coins, MOON, record.pool, held / 2);
    chain.EndBlock();
    const auto after_sell{*chain.Assets().GetLaunch(MOON)};
    BOOST_CHECK_EQUAL(after_sell.creator_tokens, 0U);
    BOOST_CHECK_GT(after_sell.creator_fees, after_buy.creator_fees);
    // The creator claims its coin and its tokens: all it is owed, nothing left.
    const CTransaction claim{MakeTx({}, ClaimLaunch{MOON}, {})};
    chain.Require(claim);
    BOOST_REQUIRE_EQUAL(chain.payouts.size(), 1U);
    BOOST_CHECK_EQUAL(chain.payouts[0].nValue, static_cast<CAmount>(after_sell.creator_fees));
    BOOST_CHECK_EQUAL(chain.Assets().GetLaunch(MOON)->creator_fees, 0U);
    chain.EndBlock();
}

BOOST_AUTO_TEST_CASE(fees_on_the_coin_side)
{
    // A pool of the coin (constant product, 1 %): a sale's whole units go through the curve, its fee comes out of the
    // coin it gets: 85 % back into the pool's coin, 15 % to security; a buy's fee out of the coin it pays, the same split.
    Chain chain;
    const AssetId GOLD{HashName("GOLD")};
    const uint256 nonce{HashName("nonce GOLD")};
    const CTransaction reserved{MakeTx({}, Reserve{ReservationCommitment(GOLD, nonce, HOLDER)}, {Token{Token::Kind::RESERVATION, uint256{}, 1}})};
    chain.Require(reserved);
    Register reg;
    reg.name = GOLD;
    reg.nonce = nonce;
    reg.supply = 1'000'000'000;
    reg.decimals = 2;
    reg.text = "GOLD";
    const CTransaction issued{MakeTx({COutPoint{reserved.GetHash(), 0}}, reg, {Token{Token::Kind::CONTROL, GOLD, 1}, Token{Token::Kind::ASSET, GOLD, 600'000'000}, Token{Token::Kind::ASSET, GOLD, 400'000'000}})};
    chain.Require(issued);
    PoolParams params;
    params.curve = static_cast<uint8_t>(Curve::CONSTANT_PRODUCT);
    params.fee = 10'000;
    CreatePool create;
    create.asset_a = GOLD;
    create.asset_b = CHN;
    create.curve = params.curve;
    create.fee = params.fee;
    create.amount_a = 500'000'000;
    create.amount_b = 100 * COIN_UNITS;
    create.min_shares = 1;
    chain.Require(MakeTx({COutPoint{issued.GetHash(), 1}}, create, {std::nullopt, Token{Token::Kind::ASSET, GOLD, 100'000'000}}, static_cast<CAmount>(100 * COIN_UNITS)));
    chain.EndBlock();
    const uint256 pool{PoolIdOf(CHN, GOLD, params)};
    const Pool before{*chain.Assets().GetPool(pool)};
    BOOST_REQUIRE(chain.Assets().CoinSideFees());
    // A sale of 20 % of the pool's GOLD.
    const uint64_t units{100'000'000};
    const auto gross{curves::ClearPoolOf(params, 0, before.reserve0, before.reserve1, {dex::HopIn{false, units}})};
    BOOST_REQUIRE(gross);
    const uint64_t fee{(gross->outs[0] * 10'000 + 999'999) / 1'000'000};
    // Its quote is the same: what it gets, net of the fee.
    const auto quote{dex::QuoteRoutePools(chain.Assets(), GOLD, {pool}, units)};
    BOOST_REQUIRE(quote);
    BOOST_CHECK_EQUAL(quote->back(), gross->outs[0] - fee);
    const int64_t security_before{chain.security};
    chain.Sell(COutPoint{issued.GetHash(), 2}, GOLD, pool, units);
    chain.EndBlock();
    const Pool after{*chain.Assets().GetPool(pool)};
    bool paid{false};
    for (const CTxOut& out : chain.paid) paid |= out.scriptPubKey == PAYEE && out.nValue == static_cast<CAmount>(gross->outs[0] - fee);
    BOOST_CHECK(paid);
    const uint64_t to_security{fee * 150'000 / 1'000'000};
    BOOST_CHECK_EQUAL(chain.security - security_before, static_cast<int64_t>(to_security));
    BOOST_CHECK_EQUAL(after.reserve1, before.reserve1 + units);
    BOOST_CHECK_EQUAL(after.reserve0, before.reserve0 - gross->outs[0] + (fee - to_security));
    // Nothing in GOLD to security or held.
    BOOST_CHECK_EQUAL(chain.Assets().GetTreasury(GOLD).value_or(TreasuryEntry{}).security_held, 0U);
    // A buy: its fee out of the coin it pays, the same split.
    const uint64_t coin{10 * COIN_UNITS};
    const uint64_t buy_fee{(coin * 10'000 + 999'999) / 1'000'000};
    const auto bought{curves::ClearPoolOf(params, 0, after.reserve0, after.reserve1, {dex::HopIn{true, coin - buy_fee}})};
    BOOST_REQUIRE(bought);
    const int64_t security_mid{chain.security};
    const CTransaction buy{chain.Buy(pool, coin)};
    chain.EndBlock();
    BOOST_CHECK(chain.TokenAt(COutPoint{buy.GetHash(), 0}) == (Token{Token::Kind::ASSET, GOLD, bought->outs[0]}));
    const uint64_t buy_security{buy_fee * 150'000 / 1'000'000};
    BOOST_CHECK_EQUAL(chain.security - security_mid, static_cast<int64_t>(buy_security));
    const Pool last{*chain.Assets().GetPool(pool)};
    BOOST_CHECK_EQUAL(last.reserve0, after.reserve0 + coin - buy_security);
    BOOST_CHECK_EQUAL(last.reserve1, after.reserve1 - bought->outs[0]);
}

BOOST_AUTO_TEST_CASE(allocation_at_graduation)
{
    // No vesting (user decision, 2026-10-24): the whole allocation is claimed at graduation, as before.
    Chain chain;
    const AssetId MOON{HashName("MOON")};
    const LaunchRecord record{chain.Graduated("MOON", 200)};
    const uint64_t all{record.AllocationUnits()};
    BOOST_REQUIRE_GT(all, 0U);
    const uint64_t tokens{chain.Assets().GetLaunch(MOON)->creator_tokens};
    const CTransaction claim{MakeTx({}, ClaimLaunch{MOON}, {std::nullopt}, 0, {CREATOR})};
    chain.Require(claim);
    BOOST_CHECK_EQUAL(chain.TokenAt(COutPoint{claim.GetHash(), 0})->amount, all + tokens);
    BOOST_CHECK(chain.Assets().GetLaunch(MOON)->allocation_claimed);
    chain.EndBlock();
    BOOST_CHECK_EQUAL(chain.Assets().GetAsset(MOON)->supply, record.Supply() + all);
}

BOOST_AUTO_TEST_CASE(buyback_and_burn)
{
    Chain chain;
    const AssetId MOON{HashName("MOON")};
    V5Options v5;
    v5.destination = static_cast<uint8_t>(FeeDestination::BURN);
    chain.Launch("MOON", 0, COIN_UNITS, v5);
    chain.EndBlock();
    auto record{*chain.Assets().GetLaunch(MOON)};
    BOOST_CHECK_EQUAL(record.destination, static_cast<uint8_t>(FeeDestination::BURN));
    // Buys on the curve: the creator's part of their fee waits to buy the token back, nothing to the creator.
    for (int i{0}; i < 3; ++i) {
        chain.Buy(record.pool, 20 * COIN_UNITS);
        chain.EndBlock();
    }
    record = *chain.Assets().GetLaunch(MOON);
    BOOST_CHECK_EQUAL(record.creator_fees, 0U);
    // Each block buys back what waited the block before, on the curve, and burns it.
    BOOST_CHECK_GT(record.burned, 0U);
    BOOST_CHECK_EQUAL(chain.Assets().GetAsset(MOON)->supply, record.Supply() - record.burned);
    BOOST_CHECK_EQUAL(chain.Assets().GetAsset(MOON)->burned, record.burned);
    const uint64_t burned{record.burned};
    chain.EndBlock();
    BOOST_CHECK_GE(chain.Assets().GetLaunch(MOON)->burned, burned);
}

BOOST_AUTO_TEST_CASE(split_destination)
{
    Chain chain;
    const AssetId MOON{HashName("MOON")};
    V5Options v5;
    v5.destination = static_cast<uint8_t>(FeeDestination::SPLIT);
    v5.burn_bps = 2'500;
    chain.Launch("MOON", 0, COIN_UNITS, v5);
    chain.EndBlock();
    const uint256 pool{chain.Assets().GetLaunch(MOON)->pool};
    chain.Buy(pool, 40 * COIN_UNITS);
    chain.EndBlock();
    chain.EndBlock();
    const auto record{*chain.Assets().GetLaunch(MOON)};
    BOOST_CHECK_GT(record.creator_fees, 0U);
    BOOST_CHECK_GT(record.burned + record.buyback, 0U);
    // A split needs its burn part, and a part below the whole; the destination is one of three.
    V5Options bad;
    bad.destination = static_cast<uint8_t>(FeeDestination::SPLIT);
    const AssetId STAR{HashName("STAR")};
    const uint256 nonce{HashName("nonce STAR")};
    const CTransaction reserved{MakeTx({}, Reserve{ReservationCommitment(STAR, nonce, HOLDER)}, {Token{Token::Kind::RESERVATION, uint256{}, 1}})};
    chain.Require(reserved);
    LaunchToken launch;
    launch.reg.name = STAR;
    launch.reg.nonce = nonce;
    launch.reg.decimals = LAUNCH_DECIMALS;
    launch.reg.text = "STAR";
    launch.creator = CREATOR;
    launch.destination = static_cast<uint8_t>(FeeDestination::SPLIT);
    BOOST_CHECK(!chain.Refused(MakeTx({COutPoint{reserved.GetHash(), 0}}, launch, {})).empty());
    launch.destination = 3;
    BOOST_CHECK(!chain.Refused(MakeTx({COutPoint{reserved.GetHash(), 0}}, launch, {})).empty());
}

BOOST_AUTO_TEST_CASE(keep_control_mintable)
{
    Chain chain;
    const AssetId MOON{HashName("MOON")}, STAR{HashName("STAR")};
    V5Options v5;
    v5.keep_control = true;
    const CTransaction kept{chain.Launch("MOON", 0, 0, v5, {Token{Token::Kind::CONTROL, MOON, 1}})};
    BOOST_CHECK(chain.TokenAt(COutPoint{kept.GetHash(), 0}) == (Token{Token::Kind::CONTROL, MOON, 1}));
    BOOST_CHECK(!chain.Assets().GetAsset(MOON)->fixed);
    BOOST_CHECK(chain.Assets().GetLaunch(MOON)->keep_control);
    chain.Launch("STAR", 0, 0);
    BOOST_CHECK(chain.Assets().GetAsset(STAR)->fixed);
    BOOST_CHECK(!chain.Assets().GetLaunch(STAR)->keep_control);
}

BOOST_AUTO_TEST_CASE(marker_encoding_one_way)
{
    // The network's defaults alone are written as a launch of before; anything else as LAUNCH_V5_KIND, read back the same.
    LaunchToken launch;
    launch.reg.name = HashName("MOON");
    launch.creator = CREATOR;
    BOOST_CHECK(!launch.V5());
    launch.destination = static_cast<uint8_t>(FeeDestination::SPLIT);
    launch.burn_bps = 2'500;
    launch.keep_control = true;
    BOOST_CHECK(launch.V5());
    Marker marker;
    marker.operation = launch;
    const auto read{ParseMarkerScript(MarkerScript(marker))};
    BOOST_REQUIRE(read && read->operation);
    const auto* back{std::get_if<LaunchToken>(&*read->operation)};
    BOOST_REQUIRE(back);
    BOOST_CHECK_EQUAL(back->destination, launch.destination);
    BOOST_CHECK_EQUAL(back->burn_bps, launch.burn_bps);
    BOOST_CHECK(back->keep_control);
}

namespace {
/** The fee-flow release from the first block, upgrade v9 from block 1000 (the Chain starts at 100). */
struct V9FeeSetup : BasicTestingSetup {
    V9FeeSetup() : BasicTestingSetup{ChainType::REGTEST, {.extra_args = {"-sidechainslot=7", "-testactivationheight=dex@0", "-testactivationheight=dexcurves@0", "-testactivationheight=feeflow@0", "-testactivationheight=v9@1000"}}} {}
};
constexpr int V9_HEIGHT{1000};

/** A concentrated pool of the coin and GOLD at 1 % (tick spacing 200), made at a price of 1: its id; GOLD's coin of the rest. */
std::pair<uint256, COutPoint> CoinClPool(Chain& chain)
{
    const AssetId GOLD{HashName("GOLD")};
    const uint256 nonce{HashName("nonce GOLD")};
    const CTransaction reserved{MakeTx({}, Reserve{ReservationCommitment(GOLD, nonce, HOLDER)}, {Token{Token::Kind::RESERVATION, uint256{}, 1}})};
    chain.Require(reserved);
    Register reg;
    reg.name = GOLD;
    reg.nonce = nonce;
    reg.supply = 1'000'000 * COIN_UNITS;
    reg.decimals = 8;
    reg.text = "GOLD";
    const CTransaction issued{MakeTx({COutPoint{reserved.GetHash(), 0}}, reg, {Token{Token::Kind::CONTROL, GOLD, 1}, Token{Token::Kind::ASSET, GOLD, reg.supply}})};
    chain.Require(issued);
    CreatePool create;
    create.asset_a = CHN;
    create.asset_b = GOLD;
    create.curve = static_cast<uint8_t>(Curve::CONCENTRATED);
    create.fee = 10'000;
    create.amount_a = 1000;
    create.amount_b = 1000;
    chain.Require(MakeTx({}, create, {}));
    PoolParams kind;
    kind.curve = static_cast<uint8_t>(Curve::CONCENTRATED);
    kind.fee = 10'000;
    return {PoolIdOf(CHN, GOLD, kind), COutPoint{issued.GetHash(), 1}};
}

/** A position of `liquidity` in [lo, hi): its id; `gold` becomes the coin of GOLD left. */
uint256 Open(Chain& chain, const uint256& pool, int32_t lo, int32_t hi, unsigned __int128 liquidity, COutPoint& gold)
{
    const AssetId GOLD{HashName("GOLD")};
    const auto needed{cl::AmountsForLiquidity(UintToArith256(chain.Assets().GetClState(pool)->sqrt_price), cl::SqrtRatioAtTick(lo), cl::SqrtRatioAtTick(hi), liquidity, true)};
    BOOST_REQUIRE(needed);
    const uint64_t held{chain.TokenAt(gold)->amount};
    OpenPosition open;
    open.pool = pool;
    open.tick_lower = lo;
    open.tick_upper = hi;
    open.liquidity_hi = static_cast<uint64_t>(liquidity >> 64);
    open.liquidity_lo = static_cast<uint64_t>(liquidity);
    open.max0 = needed->first;
    open.max1 = held;
    open.chn_to = PAYEE;
    const CTransaction tx{MakeTx({gold}, open, {std::nullopt, std::nullopt}, static_cast<CAmount>(needed->first))};
    chain.Require(tx);
    gold = COutPoint{tx.GetHash(), 1};
    BOOST_REQUIRE(chain.TokenAt(gold) == (Token{Token::Kind::ASSET, GOLD, held - needed->second}));
    return PositionId(tx.GetHash());
}

/** Two adjacent ranges, [-1200, -600) and [-600, 600) with the price at 0 in the upper, and a buy that crosses -600: the
 * coin fees each position earned (lower, upper), and what crossing -600 took of the walk's input against all of it. */
struct CoinFees {
    uint64_t lower, upper, fee, to_cross, used;
};
CoinFees BuyAcrossTwoRanges(int start_height)
{
    Chain chain;
    chain.height = start_height;
    auto [pool, gold]{CoinClPool(chain)};
    const unsigned __int128 l{static_cast<unsigned __int128>(1) << 40};
    const uint256 lower{Open(chain, pool, -1200, -600, l, gold)};
    const uint256 upper{Open(chain, pool, -600, 600, l, gold)};
    chain.EndBlock();
    BOOST_REQUIRE(chain.Assets().CoinSideFees());
    const uint64_t to_cross{static_cast<uint64_t>(*cl::Amount0Delta(cl::SqrtRatioAtTick(-600), cl::SqrtRatioAtTick(0), l, true).ToU128())};
    // Half again what crossing the upper range takes, net of the fee: the walk ends inside the lower range.
    const uint64_t used{to_cross * 3 / 2};
    const uint64_t coin{used * 100 / 99 + 1};
    const uint64_t fee{(coin * 10'000 + 999'999) / 1'000'000};
    chain.Buy(pool, coin);
    chain.EndBlock();
    BOOST_REQUIRE_LT(chain.Assets().GetClState(pool)->tick, -600);
    return {chain.Assets().PositionFees(*chain.Assets().GetPosition(lower)).first, chain.Assets().PositionFees(*chain.Assets().GetPosition(upper)).first,
            fee - fee * 150'000 / 1'000'000, to_cross, coin - fee};
}

/** An empty concentrated pool of the coin, a buy and a sale in the same block that cross each other (no walk, no
 * liquidity in range): the pool's coin and the chain's security fees of that block, the pool's coin before. */
std::tuple<uint64_t, uint64_t, int64_t> CrossInAnEmptyPool(int start_height)
{
    Chain chain;
    chain.height = start_height;
    auto [pool, gold]{CoinClPool(chain)};
    chain.EndBlock();
    const uint64_t before{chain.Assets().GetPool(pool)->reserve0};
    const int64_t security{chain.security};
    chain.Buy(pool, 10 * COIN_UNITS);
    chain.Sell(gold, HashName("GOLD"), pool, 10 * COIN_UNITS);
    chain.EndBlock();
    return {before, chain.Assets().GetPool(pool)->reserve0, chain.security - security};
}
} // namespace

BOOST_FIXTURE_TEST_CASE(coin_side_fees_by_the_ranges_the_walk_went_through, V9FeeSetup)
{
    // From v9: a round of a concentrated pool of the coin credits the providers' part of its coin fee stretch by stretch,
    // each position earning what the walk took in its range (as the rounds of other pools do).
    const CoinFees now{BuyAcrossTwoRanges(V9_HEIGHT)};
    BOOST_CHECK_GT(now.upper, 0U);
    BOOST_CHECK_GT(now.lower, 0U);
    const double expect_upper{double(now.fee) * now.to_cross / now.used}, expect_lower{double(now.fee) - expect_upper};
    BOOST_CHECK_MESSAGE(std::abs(double(now.upper) - expect_upper) <= 2 + double(now.fee) / 1000, now.upper << " vs " << expect_upper);
    BOOST_CHECK_MESSAGE(std::abs(double(now.lower) - expect_lower) <= 2 + double(now.fee) / 1000, now.lower << " vs " << expect_lower);
    BOOST_CHECK_LE(now.lower + now.upper, now.fee);
    BOOST_CHECK_GE(now.lower + now.upper + 2, now.fee);
    // Before it: the whole of it to the liquidity in range at the round's end (the lower range).
    const CoinFees before{BuyAcrossTwoRanges(V9_HEIGHT - 200)};
    BOOST_CHECK_GE(before.lower + 2, before.fee);
    BOOST_CHECK_LE(before.upper, before.fee / 1000);
}

BOOST_FIXTURE_TEST_CASE(coin_side_fees_nobody_earned_go_to_security, V9FeeSetup)
{
    // From v9: the providers' part of a round that nobody's liquidity earned leaves with the security part; before it, it
    // stayed in the pool's coin, nobody's. The coin the assets hold is checked every block (Chain::EndBlock).
    const auto [b_before, b_after, b_security]{CrossInAnEmptyPool(V9_HEIGHT - 200)};
    const auto [n_before, n_after, n_security]{CrossInAnEmptyPool(V9_HEIGHT)};
    BOOST_CHECK_EQUAL(b_before, n_before);
    BOOST_CHECK_GT(b_after, n_after);
    BOOST_CHECK_EQUAL(b_after - n_after, static_cast<uint64_t>(n_security - b_security));
}

BOOST_AUTO_TEST_SUITE_END()
