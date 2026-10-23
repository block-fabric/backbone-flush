// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Launches (bonding-curve token launches, LaunchToken; doc/layers.md "Launches"): the curve's arithmetic (its
// constants, rounds of buys and sales, the end of the curve reached exactly, conservation, the pool never losing),
// vectors (data/dex_launch.json; contrib/dex-curves/reference.py checks them), and launches on a state of their own:
// made from a reservation, traded in the block's batch, graduated into a locked pool, claimed by the creator.

#include <addresstype.h>
#include <bitassets/curves.h>
#include <bitassets/dex.h>
#include <bitassets/state.h>
#include <chainparams.h>
#include <consensus/params.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sidechain/state.h>
#include <sidechain/store.h>
#include <test/data/dex_launch.json.h>
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
using namespace bitassets::curves;

namespace {
const CScript HOLDER{GetScriptForDestination(WitnessV0KeyHash{uint160{}})};
const CScript CREATOR{GetScriptForDestination(WitnessV0KeyHash{uint160{std::vector<unsigned char>(20, 5)}})};
const CScript PAYEE{GetScriptForDestination(WitnessV0KeyHash{uint160{std::vector<unsigned char>(20, 7)}})};
constexpr uint64_t COIN_UNITS{100'000'000};
constexpr uint64_t G{500 * COIN_UNITS};
constexpr uint64_t V{G / 2};
int g_nonce{0};

LaunchCurve Curve1(uint64_t max_buy = 0, uint64_t min_buy = 0) { return LaunchCurve{10'000, LAUNCH_CURVE_END, max_buy, min_buy}; }

CTransaction MakeTx(const std::vector<COutPoint>& inputs, std::optional<Operation> op, const std::vector<std::optional<Token>>& outs, CAmount chn_in = 0,
                    const std::vector<CScript>& scripts = {})
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(++g_nonce % 250 + 1)}), static_cast<uint32_t>(300000 + g_nonce)});
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

struct Chain {
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State state{store};
    Consensus::SidechainParams params;
    int height{100};
    uint32_t time{1'800'000'000};
    std::vector<CTxOut> payouts, refunds, paid;
    //! The CHN markers burned and the coinbase paid: what the assets must hold (HeldChn) is the difference.
    int64_t burned{0}, paid_out{0}, security{0};

    Chain()
    {
        params.bitassets_reveal_depth = 0;
        params.dex_height = 0;
        params.dex_curves_height = 0;
        params.ticker_height = 0;
        params.logo_height = 0;
        params.launch_params_height = 0;
    }
    bool Apply(const CTransaction& tx, std::string& reason)
    {
        const size_t before{payouts.size()};
        const bool ok{state.ApplyTx(tx, height, params, payouts, refunds, reason, false, true)};
        if (ok) {
            for (const CTxOut& out : tx.vout) {
                if (IsMarkerScript(out.scriptPubKey)) burned += out.nValue;
            }
        }
        (void)before;
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
        // The security parts of the fees the block paid with its fees (out of what the assets held).
        security += state.BitAssets().BlockSecurityFees(height);
        paid_out += state.BitAssets().BlockSecurityFees(height);
        paid = payouts;
        payouts.clear();
        refunds.clear();
        ++height;
        time += 15;
    }
    bitassets::State Assets() const { return state.BitAssets(); }
    std::optional<Token> TokenAt(const COutPoint& out) const { return Assets().GetToken(out); }
    /** Launches `name`: a reservation, then the launch (with a first buy if `first_buy`). */
    CTransaction Launch(const std::string& name, uint16_t allocation = 0, uint64_t first_buy = 0, uint64_t max_buy = 0, uint64_t min_buy = 0)
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
        launch.reg.display_name = name + " the token";
        launch.creator = CREATOR;
        launch.allocation = allocation;
        launch.max_buy = max_buy;
        launch.min_buy = min_buy;
        launch.first_buy = first_buy;
        launch.chn_to = first_buy ? PAYEE : CScript{};
        std::vector<std::optional<Token>> outs;
        if (first_buy) outs.emplace_back();
        const CTransaction tx{MakeTx({COutPoint{reserved.GetHash(), 0}}, launch, outs, static_cast<CAmount>(first_buy))};
        Require(tx);
        return tx;
    }
    /** Launches `name` with parameters: `supply` units of `decimals`, the curve's `fee`; the launch transaction (unapplied if `apply` is false). */
    CTransaction LaunchWith(const std::string& name, uint64_t supply, uint8_t decimals, uint32_t fee, uint16_t allocation = 0, uint64_t first_buy = 0, bool apply = true)
    {
        const AssetId id{HashName(name)};
        const uint256 nonce{HashName("nonce " + name)};
        const CTransaction reserved{MakeTx({}, Reserve{ReservationCommitment(id, nonce, HOLDER)}, {Token{Token::Kind::RESERVATION, uint256{}, 1}})};
        Require(reserved);
        LaunchToken launch;
        launch.reg.name = id;
        launch.reg.nonce = nonce;
        launch.reg.supply = supply;
        launch.reg.decimals = decimals;
        launch.reg.text = name;
        launch.creator = CREATOR;
        launch.allocation = allocation;
        launch.first_buy = first_buy;
        launch.chn_to = first_buy ? PAYEE : CScript{};
        launch.fee = fee;
        std::vector<std::optional<Token>> outs;
        if (first_buy) outs.emplace_back();
        const CTransaction tx{MakeTx({COutPoint{reserved.GetHash(), 0}}, launch, outs, static_cast<CAmount>(first_buy))};
        if (apply) Require(tx);
        return tx;
    }
    /** Registers `name` (`supply` units of `decimals`, to HOLDER): the outputs of its control coin and of its supply. */
    std::pair<COutPoint, COutPoint> Issue(const std::string& name, uint64_t supply, uint8_t decimals)
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
        const CTransaction tx{MakeTx({COutPoint{reserved.GetHash(), 0}}, reg, {Token{Token::Kind::CONTROL, id, 1}, Token{Token::Kind::ASSET, id, supply}})};
        Require(tx);
        return {COutPoint{tx.GetHash(), 0}, COutPoint{tx.GetHash(), 1}};
    }
    /** A buy of `coin` through the curve: the order's transaction (its output 0 gets the tokens). */
    CTransaction Buy(const AssetId& token, uint64_t coin, uint64_t min_out = 1)
    {
        const CTransaction tx{MakeTx({}, SwapPools{CHN, {Assets().GetLaunch(token)->pool}, coin, min_out, PAYEE}, {std::nullopt}, static_cast<CAmount>(coin))};
        Require(tx);
        return tx;
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(dex_launch_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(curve_constants)
{
    // The curve from (V, Tv) to (V + G, yE): selling LAUNCH_CURVE_SUPPLY raises exactly G, and the pool at
    // graduation, (G, LAUNCH_POOL_SUPPLY), has the curve's last price.
    const unsigned __int128 k{static_cast<unsigned __int128>(V) * LAUNCH_VIRTUAL_TOKENS};
    BOOST_CHECK(k == static_cast<unsigned __int128>(V + G) * LAUNCH_CURVE_END);
    // Last price (V+G)/yE (marginal: (V+G)²/k) against the pool's G/LAUNCH_POOL_SUPPLY.
    BOOST_CHECK(static_cast<unsigned __int128>(V + G) * (V + G) * LAUNCH_POOL_SUPPLY == k * G);
}

BOOST_AUTO_TEST_CASE(one_buy_to_the_end)
{
    // A buy of more than the whole curve costs: it gets exactly the curve's tokens for exactly G (plus the fee),
    // the rest back, and the curve is at its end.
    const uint64_t pay{G * 2};
    const auto cleared{ClearLaunch(Curve1(), V, LAUNCH_VIRTUAL_TOKENS, {{true, pay}}, {true})};
    BOOST_REQUIRE(cleared);
    BOOST_CHECK(cleared->reached_end);
    BOOST_CHECK_EQUAL(cleared->outs[0], LAUNCH_CURVE_SUPPLY);
    BOOST_CHECK_EQUAL(cleared->reserve0, V + G);
    BOOST_CHECK_EQUAL(cleared->reserve1, LAUNCH_CURVE_END);
    // The fee: on what it paid, G, rounded up: G·fee/(F − fee).
    const uint64_t fee{static_cast<uint64_t>((static_cast<unsigned __int128>(G) * 10'000 + 990'000 - 1) / 990'000)};
    BOOST_CHECK_EQUAL(cleared->fees, fee);
    BOOST_CHECK_EQUAL(cleared->refunds[0], pay - G - fee);
    // One that reaches the curve through another pool first misses there (a partial fill could not be given back).
    const auto routed{ClearLaunch(Curve1(), V, LAUNCH_VIRTUAL_TOKENS, {{true, pay}}, {false})};
    BOOST_CHECK_EQUAL(routed->outs[0], 0U);
}

BOOST_AUTO_TEST_CASE(rounds_keep_the_curve)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    for (int run{0}; run < 200; ++run) {
        uint64_t x{V}, y{LAUNCH_VIRTUAL_TOKENS};
        uint64_t held_tokens{0};
        const unsigned __int128 k0{static_cast<unsigned __int128>(x) * y};
        for (int round{0}; round < 30 && y > LAUNCH_CURVE_END; ++round) {
            std::vector<dex::HopIn> orders;
            std::vector<bool> direct;
            for (int i{0}, m{1 + static_cast<int>(rng.randrange(4))}; i < m; ++i) {
                const bool buy{held_tokens == 0 || rng.randbool()};
                const uint64_t amount{buy ? 1 + rng.randrange(G / 10) : 1 + rng.randrange(std::max<uint64_t>(1, held_tokens / 2))};
                if (!buy) {
                    if (amount > held_tokens) continue;
                    held_tokens -= amount;
                }
                orders.push_back({buy, amount});
                direct.push_back(true);
            }
            if (orders.empty()) continue;
            const auto cleared{ClearLaunch(Curve1(), x, y, orders, direct)};
            BOOST_REQUIRE(cleared);
            // Conservation: the coin in (less what goes back) is the curve's coin after plus the fees plus what sellers get.
            unsigned __int128 coin_in{0}, coin_out{0}, tokens_in{0}, tokens_out{0};
            for (size_t i{0}; i < orders.size(); ++i) {
                if (orders[i].zero_in) {
                    if (cleared->outs[i] > 0) {
                        coin_in += orders[i].amount - cleared->refunds[i];
                        tokens_out += cleared->outs[i];
                    }
                } else {
                    tokens_in += orders[i].amount;
                    coin_out += cleared->outs[i];
                }
            }
            BOOST_CHECK(static_cast<unsigned __int128>(x) + coin_in == static_cast<unsigned __int128>(cleared->reserve0) + coin_out + cleared->fees);
            BOOST_CHECK(static_cast<unsigned __int128>(y) + tokens_in == static_cast<unsigned __int128>(cleared->reserve1) + tokens_out);
            // The curve never loses: its product never falls, and it never sells past its end.
            BOOST_CHECK(static_cast<unsigned __int128>(cleared->reserve0) * cleared->reserve1 >= k0);
            BOOST_CHECK_GE(cleared->reserve1, LAUNCH_CURVE_END);
            // Sellers' tokens come back; buyers' go out.
            for (size_t i{0}; i < orders.size(); ++i) {
                if (orders[i].zero_in) held_tokens += cleared->outs[i];
            }
            if (cleared->reached_end) {
                // At the end: everything the curve had, sold (when the buyers could pay for it).
                BOOST_CHECK_EQUAL(cleared->reserve1, LAUNCH_CURVE_END);
                BOOST_CHECK_GE(cleared->reserve0, V + G);
            }
            x = cleared->reserve0;
            y = cleared->reserve1;
        }
        // Whatever was bought can be sold back: the curve holds the coin for it (it raised at least what the tokens are worth on it).
        if (held_tokens > 0 && y > LAUNCH_CURVE_END) {
            const auto back{ClearLaunch(Curve1(), x, y, {{false, held_tokens}}, {true})};
            BOOST_REQUIRE(back);
            BOOST_CHECK_GE(back->reserve0, V);
        }
    }
}

BOOST_AUTO_TEST_CASE(bounds_of_a_buy)
{
    const auto cleared{ClearLaunch(Curve1(/*max_buy=*/COIN_UNITS, /*min_buy=*/COIN_UNITS / 100), V, LAUNCH_VIRTUAL_TOKENS, {{true, 2 * COIN_UNITS}, {true, COIN_UNITS / 1000}, {true, COIN_UNITS / 2}}, {true, true, true})};
    BOOST_REQUIRE(cleared);
    BOOST_CHECK_EQUAL(cleared->outs[0], 0U);
    BOOST_CHECK_EQUAL(cleared->outs[1], 0U);
    BOOST_CHECK_GT(cleared->outs[2], 0U);
}

UniValue LaunchCase(uint64_t x, uint64_t y, const std::vector<dex::HopIn>& orders, const std::vector<bool>& direct, const LaunchCurve& curve)
{
    UniValue c(UniValue::VOBJ);
    c.pushKV("launch", true);
    c.pushKV("fee", curve.fee);
    c.pushKV("end", strprintf("%u", curve.end));
    c.pushKV("max_buy", strprintf("%u", curve.max_buy));
    c.pushKV("min_buy", strprintf("%u", curve.min_buy));
    c.pushKV("x", strprintf("%u", x));
    c.pushKV("y", strprintf("%u", y));
    UniValue ords(UniValue::VARR);
    for (size_t i{0}; i < orders.size(); ++i) {
        UniValue e(UniValue::VARR);
        e.push_back(orders[i].zero_in ? 0 : 1);
        e.push_back(strprintf("%u", orders[i].amount));
        e.push_back(direct[i]);
        ords.push_back(e);
    }
    c.pushKV("orders", ords);
    const auto cleared{ClearLaunch(curve, x, y, orders, direct)};
    UniValue result(UniValue::VOBJ);
    if (cleared) {
        UniValue outs(UniValue::VARR), refunds(UniValue::VARR);
        for (uint64_t o : cleared->outs) outs.push_back(strprintf("%u", o));
        for (uint64_t r : cleared->refunds) refunds.push_back(strprintf("%u", r));
        result.pushKV("outs", outs);
        result.pushKV("refunds", refunds);
        result.pushKV("x", strprintf("%u", cleared->reserve0));
        result.pushKV("y", strprintf("%u", cleared->reserve1));
        result.pushKV("fees", strprintf("%u", cleared->fees));
        result.pushKV("reached_end", cleared->reached_end);
    }
    c.pushKV("clearing", result);
    return c;
}

BOOST_AUTO_TEST_CASE(vectors)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    UniValue cases(UniValue::VARR);
    for (int i{0}; i < 60; ++i) {
        // Points along the curve: some coin raised already.
        const uint64_t raised{rng.randrange(G)};
        const uint64_t x{V + raised};
        const uint64_t y{static_cast<uint64_t>((static_cast<unsigned __int128>(V) * LAUNCH_VIRTUAL_TOKENS + x - 1) / x)};
        std::vector<dex::HopIn> orders;
        std::vector<bool> direct;
        for (int k{0}, m{1 + static_cast<int>(rng.randrange(5))}; k < m; ++k) {
            const bool buy{rng.randrange(3) != 0};
            orders.push_back({buy, buy ? 1 + rng.randrange(i % 7 == 0 ? 2 * G : G / 5) : 1 + rng.randrange(LAUNCH_CURVE_SUPPLY / 50)});
            direct.push_back(rng.randrange(5) != 0);
        }
        cases.push_back(LaunchCase(x, y, orders, direct, LaunchCurve{FEE_TIERS[i % 4] * (i % 3 == 0 ? 10 : 1), LAUNCH_CURVE_END, i % 5 == 0 ? G / 20 : 0, i % 6 == 0 ? 1000 : 0}));
    }
    if (const char* out{std::getenv("DEX_LAUNCH_VECTORS")}) {
        std::ofstream file{out};
        file << cases.write(1) << "\n";
    }
    const UniValue committed{read_json(json_tests::dex_launch)};
    BOOST_REQUIRE_EQUAL(committed.size(), cases.size());
    for (size_t i{0}; i < cases.size(); ++i) BOOST_CHECK_MESSAGE(committed[i].write() == cases[i].write(), "vector " << i << ": " << cases[i].write());
}

BOOST_AUTO_TEST_CASE(launch_trade_graduate_claim)
{
    Chain chain;
    // The chain's rules of launches are the network's (regtest: the defaults, 500 coins).
    const Consensus::SidechainParams& rules{Params().GetConsensus().sidechain};
    BOOST_REQUIRE_EQUAL(rules.launch_graduation, static_cast<int64_t>(G));
    const AssetId MOON{HashName("MOON")};
    const CTransaction launched{chain.Launch("MOON", /*allocation=*/200, /*first_buy=*/COIN_UNITS)};
    const auto record{chain.Assets().GetLaunch(MOON)};
    BOOST_REQUIRE(record);
    const auto asset{chain.Assets().GetAsset(MOON)};
    BOOST_CHECK(asset->fixed);
    BOOST_CHECK_EQUAL(asset->supply, LAUNCH_SUPPLY);
    BOOST_CHECK_EQUAL(asset->decimals, LAUNCH_DECIMALS);
    BOOST_CHECK_EQUAL(chain.Assets().GetDisplayName(MOON)->text, "MOON the token");
    BOOST_CHECK(chain.Assets().GetPoolParams(record->pool).GetCurve() == Curve::LAUNCH);
    BOOST_CHECK(chain.Assets().Launching(MOON));
    // No pool of it but its curve until it graduates.
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({}, AddLiquidity{MOON, CHN, 1000, 1000, 1}, {std::nullopt, std::nullopt}, 1000)), "bad-ba-launch-pooled");
    // The first buy and another in its block: one price.
    const CTransaction other{chain.Buy(MOON, COIN_UNITS)};
    chain.EndBlock();
    const auto first_got{chain.TokenAt(COutPoint{launched.GetHash(), 0})};
    const auto other_got{chain.TokenAt(COutPoint{other.GetHash(), 0})};
    BOOST_REQUIRE(first_got && other_got);
    BOOST_CHECK_EQUAL(first_got->amount, other_got->amount);
    BOOST_CHECK_GT(first_got->amount, 0U);
    // What the assets hold of the coin: what the markers burned less what the coinbase paid.
    BOOST_CHECK_EQUAL(chain.Assets().HeldChn(), chain.burned - chain.paid_out);
    // A sale back: coin by the coinbase, less the fee.
    const CTransaction sell{MakeTx({COutPoint{other.GetHash(), 0}}, SwapPools{MOON, {record->pool}, other_got->amount, 1, PAYEE}, {std::nullopt})};
    chain.Require(sell);
    chain.EndBlock();
    BOOST_REQUIRE_EQUAL(chain.paid.size(), 1U);
    BOOST_CHECK_LT(chain.paid[0].nValue, static_cast<CAmount>(COIN_UNITS));
    BOOST_CHECK_GT(chain.paid[0].nValue, static_cast<CAmount>(COIN_UNITS * 97 / 100));
    BOOST_CHECK_EQUAL(chain.Assets().HeldChn(), chain.burned - chain.paid_out);

    // A buy of more than the rest of the curve costs: the curve ends, exactly; it graduates at the block's end.
    const CTransaction whale{chain.Buy(MOON, 2 * G)};
    chain.EndBlock();
    const auto done{chain.Assets().GetLaunch(MOON)};
    BOOST_REQUIRE(done && done->Graduated());
    BOOST_CHECK_EQUAL(done->graduated, chain.height - 1);
    BOOST_CHECK_GE(done->raised_at_graduation, G);
    BOOST_CHECK_LT(done->raised_at_graduation, G + 100);
    BOOST_CHECK(!chain.Assets().Launching(MOON));
    BOOST_CHECK(!chain.Assets().GetPool(record->pool));
    // The curve's fees, every satoshi: the creator's, the protocol's (the treasury), the chain's security (paid with the
    // blocks' fees), and the remainder of the roundings into the pool: their sum is what the log says it charged.
    {
        uint64_t charged{0};
        for (int h{record->height}; h < chain.height; ++h) {
            for (const auto& [token, entry] : chain.Assets().GetLaunchLog(h)) {
                if (token == MOON) charged += entry.fees;
            }
        }
        const uint64_t rest{charged - done->creator_fees - done->creator_paid - done->protocol_fees - done->security_fees};
        BOOST_CHECK_EQUAL(done->creator_fees + done->creator_paid + done->protocol_fees + done->security_fees + rest, charged);
        // Each part the floor of its share of each block's fees: the remainder is at most a few satoshis a block.
        BOOST_CHECK_LE(rest, 3U * (chain.height - record->height));
        BOOST_CHECK_EQUAL(done->creator_share, Params().GetConsensus().sidechain.launch_creator_share);
        BOOST_CHECK_EQUAL(static_cast<int64_t>(done->security_fees), chain.security);
        BOOST_CHECK_EQUAL(chain.Assets().GetTreasury(CHN).value_or(TreasuryEntry{}).from_launches, done->protocol_fees);
    }
    // Its pool with the coin: the raised coin and the fees for it, the tokens left and LAUNCH_POOL_SUPPLY; shares nobody holds.
    const auto pool{chain.Assets().FindPool(MOON, CHN)};
    BOOST_REQUIRE(pool);
    BOOST_CHECK_EQUAL(pool->reserve1, LAUNCH_POOL_SUPPLY + (LAUNCH_CURVE_SUPPLY - chain.TokenAt(COutPoint{launched.GetHash(), 0})->amount - chain.TokenAt(COutPoint{whale.GetHash(), 0})->amount));
    BOOST_CHECK(!chain.Assets().FirstTokenOf(PoolId(MOON, CHN), Token::Kind::LP));
    BOOST_CHECK_GT(pool->shares, MIN_LIQUIDITY);
    // The whale got back what the curve's end did not take.
    bool refunded{false};
    for (const CTxOut& out : chain.paid) refunded |= out.scriptPubKey == PAYEE && out.nValue > 0;
    BOOST_CHECK(refunded);
    BOOST_CHECK_EQUAL(chain.Assets().HeldChn(), chain.burned - chain.paid_out);
    // Trading goes on in the pool; the curve takes no more.
    chain.Require(MakeTx({}, Swap{CHN, COIN_UNITS, MOON, 1, PAYEE}, {std::nullopt}, COIN_UNITS));
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({}, SwapPools{CHN, {record->pool}, COIN_UNITS, 1, PAYEE}, {std::nullopt}, COIN_UNITS)), "bad-ba-no-pool");
    chain.EndBlock();

    // The creator's claim: its part of the fees, and its allocation (2 %), to its own script only.
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({}, ClaimLaunch{MOON}, {std::nullopt})), "bad-ba-contract-payee");
    const CTransaction claim{MakeTx({}, ClaimLaunch{MOON}, {std::nullopt}, 0, {CREATOR})};
    chain.Require(claim);
    BOOST_CHECK(chain.TokenAt(COutPoint{claim.GetHash(), 0}) == (Token{Token::Kind::ASSET, MOON, LAUNCH_SUPPLY / 10'000 * 200}));
    BOOST_REQUIRE_EQUAL(chain.payouts.size(), 1U);
    BOOST_CHECK(chain.payouts[0].scriptPubKey == CREATOR);
    chain.EndBlock();
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({}, ClaimLaunch{MOON}, {})), "bad-ba-launch-nothing-owed");
    BOOST_CHECK_EQUAL(chain.Assets().GetAsset(MOON)->supply, LAUNCH_SUPPLY + LAUNCH_SUPPLY / 10'000 * 200);
    BOOST_CHECK_EQUAL(chain.Assets().HeldChn(), chain.burned - chain.paid_out);
    // The log of the blocks: made, traded, graduated.
    BOOST_CHECK(chain.Assets().GetLaunchLog(100)[0].second.created);
    BOOST_CHECK(chain.Assets().GetLaunchLog(102)[0].second.graduated);
}

// Launches with parameters (launch_params_height; doc/dex-math.md "Launch curve"): any supply S (a multiple of 8 units,
// MIN_LAUNCH_SUPPLY..MAX_LAUNCH_SUPPLY) and decimals, a fee of 0.3 % to 5 %; an existing asset on a curve.

namespace {
/** Supplies at the edges and in between: the least, the original, the most, odd multiples of 8. */
std::vector<uint64_t> Supplies(FastRandomContext& rng, int random)
{
    std::vector<uint64_t> out{MIN_LAUNCH_SUPPLY, MIN_LAUNCH_SUPPLY + 8, 21'000'000ULL * 100'000'000, LAUNCH_SUPPLY, MAX_LAUNCH_SUPPLY, MAX_LAUNCH_SUPPLY - 8, 8'000'000'000'000'000'000ULL};
    for (int i{0}; i < random; ++i) out.push_back(MIN_LAUNCH_SUPPLY + rng.randrange((MAX_LAUNCH_SUPPLY - MIN_LAUNCH_SUPPLY) / 8) * 8);
    return out;
}
LaunchCurve CurveOf(uint64_t supply, uint32_t fee) { return LaunchCurve{fee, supply / 8 * 3, 0, 0, /*strict_end=*/true}; }
} // namespace

BOOST_AUTO_TEST_CASE(curve_constants_any_supply)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    for (const uint64_t S : Supplies(rng, 500)) {
        BOOST_REQUIRE(IsLaunchSupply(S));
        LaunchRecord record;
        record.supply = S;
        // The parts are exact: 3/4 on the curve, 1/4 into the pool, the virtual 9/8, the end 3/8.
        BOOST_CHECK_EQUAL(record.CurveSupply() + record.PoolSupply(), S);
        BOOST_CHECK_EQUAL(record.VirtualUnits() - record.CurveEnd(), record.CurveSupply());
        BOOST_CHECK_EQUAL(record.CurveSupply(), 3 * record.PoolSupply());
        BOOST_CHECK_LE(record.VirtualUnits(), MAX_AMOUNT);
        // V·Tv = (V + G)·yE: selling the curve's part raises exactly G.
        const unsigned __int128 k{static_cast<unsigned __int128>(V) * record.VirtualUnits()};
        BOOST_CHECK(k == static_cast<unsigned __int128>(V + G) * record.CurveEnd());
        // The graduation pool (G, S/4) has the curve's last marginal price (V + G)²/k.
        BOOST_CHECK(static_cast<unsigned __int128>(V + G) * (V + G) * record.PoolSupply() == k * G);
        // The allocation: the floor of its share, within what an amount can be.
        record.allocation = MAX_LAUNCH_ALLOCATION;
        BOOST_CHECK(static_cast<unsigned __int128>(S) + record.AllocationUnits() <= MAX_AMOUNT);
    }
    // A launch of before: LAUNCH_SUPPLY, and its allocation as it was.
    LaunchRecord before;
    before.allocation = 200;
    BOOST_CHECK_EQUAL(before.Supply(), LAUNCH_SUPPLY);
    BOOST_CHECK_EQUAL(before.AllocationUnits(), LAUNCH_SUPPLY / 10'000 * 200);
    BOOST_CHECK_EQUAL(before.CurveEnd(), LAUNCH_CURVE_END);
    BOOST_CHECK_EQUAL(before.VirtualUnits(), LAUNCH_VIRTUAL_TOKENS);
    BOOST_CHECK_EQUAL(before.PoolSupply(), LAUNCH_POOL_SUPPLY);
    // Not a launch's supply: not a multiple of 8, too little, too much.
    BOOST_CHECK(!IsLaunchSupply(MIN_LAUNCH_SUPPLY + 4));
    BOOST_CHECK(!IsLaunchSupply(MIN_LAUNCH_SUPPLY - 8));
    BOOST_CHECK(!IsLaunchSupply(MAX_LAUNCH_SUPPLY + 8));
    BOOST_CHECK(!IsLaunchSupply(0));
}

BOOST_AUTO_TEST_CASE(graduation_exact_any_supply_and_fee)
{
    // One buy of more than the curve costs: exactly 3S/4 out for exactly G in, the end exact, the rest back, at any
    // supply and fee in the bounds.
    FastRandomContext rng{/*fDeterministic=*/true};
    for (const uint64_t S : Supplies(rng, 50)) {
        for (const uint32_t fee : {MIN_LAUNCH_FEE, uint32_t{11'500}, MAX_LAUNCH_FEE}) {
            const uint64_t pay{G * 2};
            const auto cleared{ClearLaunch(CurveOf(S, fee), V, S / 8 * 9, {{true, pay}}, {true})};
            BOOST_REQUIRE(cleared);
            BOOST_CHECK(cleared->reached_end);
            BOOST_CHECK_EQUAL(cleared->outs[0], S / 8 * 6);
            BOOST_CHECK_EQUAL(cleared->reserve0, V + G);
            BOOST_CHECK_EQUAL(cleared->reserve1, S / 8 * 3);
            const uint64_t f{static_cast<uint64_t>((static_cast<unsigned __int128>(G) * fee + (1'000'000 - fee) - 1) / (1'000'000 - fee))};
            BOOST_CHECK_EQUAL(cleared->fees, f);
            BOOST_CHECK_EQUAL(cleared->refunds[0], pay - G - f);
            // At its end the curve takes no more trades (it graduates); just before, all but one unit sold back gives at most G.
            const auto almost{ClearLaunch(CurveOf(S, fee), V, S / 8 * 9, {{true, G / 2}}, {true})};
            BOOST_REQUIRE(almost && !almost->reached_end);
            const auto back{ClearLaunch(CurveOf(S, fee), almost->reserve0, almost->reserve1, {{false, almost->outs[0]}}, {true})};
            BOOST_REQUIRE(back);
            BOOST_CHECK_GE(back->reserve0, V);
            BOOST_CHECK_LE(back->outs[0] + back->fees, G / 2);
        }
    }
}

BOOST_AUTO_TEST_CASE(rounds_conserve_any_supply)
{
    // Random rounds of buys and sales at random supplies and fees: no overflow (every round clears), the coin and the
    // units conserved, the product never falls, the end never passed.
    FastRandomContext rng{/*fDeterministic=*/true};
    for (int run{0}; run < 300; ++run) {
        const uint64_t S{run % 3 == 0 ? MAX_LAUNCH_SUPPLY : run % 3 == 1 ? MIN_LAUNCH_SUPPLY : MIN_LAUNCH_SUPPLY + rng.randrange((MAX_LAUNCH_SUPPLY - MIN_LAUNCH_SUPPLY) / 8) * 8};
        const uint32_t fee{static_cast<uint32_t>(MIN_LAUNCH_FEE + rng.randrange(MAX_LAUNCH_FEE - MIN_LAUNCH_FEE + 1))};
        const LaunchCurve curve{CurveOf(S, fee)};
        uint64_t x{V}, y{S / 8 * 9}, held{0};
        const unsigned __int128 k0{static_cast<unsigned __int128>(x) * y};
        for (int round{0}; round < 25 && y > curve.end; ++round) {
            std::vector<dex::HopIn> orders;
            std::vector<bool> direct;
            for (int i{0}, m{1 + static_cast<int>(rng.randrange(4))}; i < m; ++i) {
                const bool buy{held == 0 || rng.randbool()};
                const uint64_t amount{buy ? 1 + rng.randrange(round % 9 == 8 ? 2 * G : G / 8) : 1 + rng.randrange(std::max<uint64_t>(1, held / 2))};
                if (!buy) {
                    if (amount > held) continue;
                    held -= amount;
                }
                orders.push_back({buy, amount});
                direct.push_back(rng.randrange(6) != 0);
            }
            if (orders.empty()) continue;
            const auto cleared{ClearLaunch(curve, x, y, orders, direct)};
            // A round past the end whose buyers came in through another pool (they miss there) and whose sellers would be
            // paid more than came in does not clear (the batch then runs its orders one by one): never for any other reason.
            if (!cleared) {
                BOOST_CHECK_MESSAGE(std::find(direct.begin(), direct.end(), false) != direct.end(), "S " << S << " fee " << fee);
                for (size_t i{0}; i < orders.size(); ++i) {
                    if (!orders[i].zero_in) held += orders[i].amount;
                }
                continue;
            }
            unsigned __int128 coin_in{0}, coin_out{0}, units_in{0}, units_out{0};
            for (size_t i{0}; i < orders.size(); ++i) {
                if (orders[i].zero_in) {
                    if (cleared->outs[i] > 0) {
                        coin_in += orders[i].amount - cleared->refunds[i];
                        units_out += cleared->outs[i];
                    }
                    held += cleared->outs[i];
                } else {
                    units_in += orders[i].amount;
                    coin_out += cleared->outs[i];
                }
            }
            BOOST_CHECK(static_cast<unsigned __int128>(x) + coin_in == static_cast<unsigned __int128>(cleared->reserve0) + coin_out + cleared->fees);
            BOOST_CHECK(static_cast<unsigned __int128>(y) + units_in == static_cast<unsigned __int128>(cleared->reserve1) + units_out);
            BOOST_CHECK(static_cast<unsigned __int128>(cleared->reserve0) * cleared->reserve1 >= k0);
            BOOST_CHECK_GE(cleared->reserve1, curve.end);
            // The end reached is the end exactly (strict_end: a buy routed in that misses there does not end it).
            if (cleared->reached_end) {
                BOOST_CHECK_EQUAL(cleared->reserve1, curve.end);
                BOOST_CHECK_GE(cleared->reserve0, V + G);
            }
            x = cleared->reserve0;
            y = cleared->reserve1;
        }
    }
}

BOOST_AUTO_TEST_CASE(launch_record_layout)
{
    // A record of before (no supply, not from an asset) is written as it was, and read back with its supply 0
    // (LAUNCH_SUPPLY); one with parameters round-trips with them.
    LaunchRecord before;
    before.height = 7;
    before.fee = 11'500;
    before.allocation = 100;
    DataStream a{};
    a << before;
    const size_t old_size{a.size()};
    LaunchRecord read;
    a >> read;
    BOOST_CHECK(read == before);
    LaunchRecord with{before};
    with.supply = 21'000'000ULL * 100'000'000;
    with.from_asset = true;
    DataStream b{};
    b << with;
    BOOST_CHECK_GT(b.size(), old_size);
    LaunchRecord back;
    b >> back;
    BOOST_CHECK(back == with);
    BOOST_CHECK_EQUAL(back.Supply(), with.supply);
}

BOOST_AUTO_TEST_CASE(launch_markers_of_parameters)
{
    // A launch with parameters is a marker of its own kind, written one way; fee 0 there is not (it is a launch of before).
    LaunchToken launch;
    launch.reg.name = HashName("WIDE");
    launch.reg.text = "WIDE";
    launch.reg.supply = 8'000'000;
    launch.reg.decimals = 2;
    launch.creator = CREATOR;
    launch.fee = 20'000;
    Marker marker;
    marker.operation = launch;
    const CScript script{MarkerScript(marker)};
    const auto parsed{ParseMarkerScript(script)};
    BOOST_REQUIRE(parsed && parsed->operation);
    BOOST_CHECK(std::get<LaunchToken>(*parsed->operation) == launch);
    BOOST_CHECK(IsLaunchParamsOperation(*parsed->operation));
    launch.fee = 0;
    marker.operation = launch;
    BOOST_CHECK(!IsLaunchParamsOperation(*ParseMarkerScript(MarkerScript(marker))->operation));
    // The kind of parameters with a fee of 0 (written by hand from one of fee 1, its last bytes the fee and no outputs): not a marker.
    {
        launch.fee = 1;
        marker.operation = launch;
        const CScript good{MarkerScript(marker)};
        std::vector<unsigned char> bytes{good.begin(), good.end()};
        BOOST_REQUIRE(ParseMarkerScript(CScript(bytes.begin(), bytes.end())));
        BOOST_REQUIRE_EQUAL(bytes[bytes.size() - 2], 1);
        bytes[bytes.size() - 2] = 0;
        BOOST_CHECK(!ParseMarkerScript(CScript(bytes.begin(), bytes.end())));
    }
    CurveAsset curve;
    curve.asset = HashName("GOLD");
    curve.amount = 8'000'000;
    curve.fee = 3'000;
    curve.creator = CREATOR;
    marker.operation = curve;
    const auto curve_parsed{ParseMarkerScript(MarkerScript(marker))};
    BOOST_REQUIRE(curve_parsed && curve_parsed->operation);
    BOOST_CHECK(std::get<CurveAsset>(*curve_parsed->operation) == curve);
}

BOOST_AUTO_TEST_CASE(launch_with_parameters_trade_graduate)
{
    Chain chain;
    // 21 million of 8 decimals at 2 %, a 1 % allocation and a first buy.
    const uint64_t S{21'000'000ULL * 100'000'000};
    const AssetId BTC2{HashName("BTCTWO")};
    const CTransaction launched{chain.LaunchWith("BTCTWO", S, 8, 20'000, /*allocation=*/100, /*first_buy=*/COIN_UNITS)};
    const auto record{chain.Assets().GetLaunch(BTC2)};
    BOOST_REQUIRE(record);
    BOOST_CHECK_EQUAL(record->supply, S);
    BOOST_CHECK_EQUAL(record->fee, 20'000U);
    BOOST_CHECK(!record->from_asset);
    const auto asset{chain.Assets().GetAsset(BTC2)};
    BOOST_CHECK(asset->fixed);
    BOOST_CHECK_EQUAL(asset->supply, S);
    BOOST_CHECK_EQUAL(asset->decimals, 8);
    BOOST_CHECK(!chain.Assets().FirstTokenOf(BTC2, Token::Kind::CONTROL));
    const auto curve{chain.Assets().GetPool(record->pool)};
    BOOST_REQUIRE(curve);
    BOOST_CHECK_EQUAL(curve->reserve1, S / 8 * 9);
    chain.EndBlock();
    const CTransaction whale{chain.Buy(BTC2, 2 * G)};
    chain.EndBlock();
    const auto done{chain.Assets().GetLaunch(BTC2)};
    BOOST_REQUIRE(done && done->Graduated());
    const auto pool{chain.Assets().FindPool(BTC2, CHN)};
    BOOST_REQUIRE(pool);
    const uint64_t bought{chain.TokenAt(COutPoint{launched.GetHash(), 0})->amount + chain.TokenAt(COutPoint{whale.GetHash(), 0})->amount};
    // Every unit accounted: what was bought, and the rest in the pool with the last quarter.
    BOOST_CHECK_EQUAL(bought + pool->reserve1, S);
    BOOST_CHECK_EQUAL(pool->reserve1, S / 4 + (S / 8 * 6 - bought));
    BOOST_CHECK_EQUAL(chain.Assets().HeldChn(), chain.burned - chain.paid_out);
    // The allocation: 1 % of S.
    const CTransaction claim{MakeTx({}, ClaimLaunch{BTC2}, {std::nullopt}, 0, {CREATOR})};
    chain.Require(claim);
    BOOST_CHECK(chain.TokenAt(COutPoint{claim.GetHash(), 0}) == (Token{Token::Kind::ASSET, BTC2, S / 100}));
}

BOOST_AUTO_TEST_CASE(launch_parameters_refused)
{
    Chain chain;
    // Out of the bounds: the fee, the supply (not a multiple of 8, too little), its decimals past MAX_DECIMALS.
    BOOST_CHECK_EQUAL(chain.Refused(chain.LaunchWith("FEELOW", 8'000'000, 2, MIN_LAUNCH_FEE - 1, 0, 0, false)), "bad-ba-launch");
    BOOST_CHECK_EQUAL(chain.Refused(chain.LaunchWith("FEEHIGH", 8'000'000, 2, MAX_LAUNCH_FEE + 1, 0, 0, false)), "bad-ba-launch");
    BOOST_CHECK_EQUAL(chain.Refused(chain.LaunchWith("ODD", 8'000'004, 2, 11'500, 0, 0, false)), "bad-ba-launch");
    BOOST_CHECK_EQUAL(chain.Refused(chain.LaunchWith("TINY", MIN_LAUNCH_SUPPLY - 8, 2, 11'500, 0, 0, false)), "bad-ba-launch");
    BOOST_CHECK_EQUAL(chain.Refused(chain.LaunchWith("DEEP", 8'000'000, MAX_DECIMALS + 1, 11'500, 0, 0, false)), "bad-ba-supply");
    // Before launch_params_height: not a marker of its time.
    chain.params.launch_params_height = chain.height + 10;
    BOOST_CHECK_EQUAL(chain.Refused(chain.LaunchWith("EARLY", 8'000'000, 2, 11'500, 0, 0, false)), "bad-ba-marker");
    // A launch of before still is.
    chain.Launch("PLAIN");
    BOOST_CHECK(chain.Assets().GetLaunch(HashName("PLAIN")));
}

BOOST_AUTO_TEST_CASE(existing_asset_on_a_curve)
{
    Chain chain;
    const AssetId GOLD{HashName("GOLD")};
    // A million GOLD of 6 decimals; 800 000 of them on a curve.
    const uint64_t supply{1'000'000ULL * 1'000'000}, D{800'000ULL * 1'000'000};
    const auto [control, coins]{chain.Issue("GOLD", supply, 6)};
    chain.EndBlock();
    CurveAsset curve;
    curve.asset = GOLD;
    curve.amount = D;
    curve.fee = 10'000;
    curve.creator = CREATOR;
    // Without the control coin: refused.
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({coins}, curve, {Token{Token::Kind::ASSET, GOLD, supply - D}})), "bad-ba-no-control");
    // Before launch_params_height: not a marker of its time.
    chain.params.launch_params_height = chain.height + 1;
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({control, coins}, curve, {Token{Token::Kind::CONTROL, GOLD, 1}, Token{Token::Kind::ASSET, GOLD, supply - D}})), "bad-ba-marker");
    chain.params.launch_params_height = 0;
    // The units have to go in: listing them all out again is unbacked.
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({control, coins}, curve, {Token{Token::Kind::CONTROL, GOLD, 1}, Token{Token::Kind::ASSET, GOLD, supply}})), "bad-ba-tokens-unbacked");
    const CTransaction put{MakeTx({control, coins}, curve, {Token{Token::Kind::CONTROL, GOLD, 1}, Token{Token::Kind::ASSET, GOLD, supply - D}})};
    chain.Require(put);
    const auto record{chain.Assets().GetLaunch(GOLD)};
    BOOST_REQUIRE(record);
    BOOST_CHECK(record->from_asset);
    BOOST_CHECK_EQUAL(record->Supply(), D);
    BOOST_CHECK_EQUAL(record->allocation, 0);
    BOOST_CHECK(chain.Assets().Launching(GOLD));
    // The asset as it was: its supply, its control coin (with the holder).
    BOOST_CHECK_EQUAL(chain.Assets().GetAsset(GOLD)->supply, supply);
    BOOST_CHECK(!chain.Assets().GetAsset(GOLD)->fixed);
    chain.EndBlock();
    // Not twice.
    {
        CurveAsset small{curve};
        small.amount = 8'000'000;
        BOOST_CHECK_EQUAL(chain.Refused(MakeTx({COutPoint{put.GetHash(), 0}, COutPoint{put.GetHash(), 1}}, small,
                                               {Token{Token::Kind::CONTROL, GOLD, 1}, Token{Token::Kind::ASSET, GOLD, supply - D - small.amount}})), "bad-ba-launch-again");
    }
    // Traded to graduation: every unit of the asset accounted for.
    const CTransaction whale{chain.Buy(GOLD, 2 * G)};
    chain.EndBlock();
    BOOST_REQUIRE(chain.Assets().GetLaunch(GOLD)->Graduated());
    const auto pool{chain.Assets().FindPool(GOLD, CHN)};
    BOOST_REQUIRE(pool);
    const uint64_t bought{chain.TokenAt(COutPoint{whale.GetHash(), 0})->amount};
    BOOST_CHECK_EQUAL(bought, D / 8 * 6);
    BOOST_CHECK_EQUAL(pool->reserve1, D / 4);
    BOOST_CHECK_EQUAL(bought + pool->reserve1 + chain.TokenAt(COutPoint{put.GetHash(), 1})->amount, supply);
    BOOST_CHECK_EQUAL(chain.Assets().HeldChn(), chain.burned - chain.paid_out);
    // The controller keeps control: it mints after graduation as before.
    chain.Require(MakeTx({COutPoint{put.GetHash(), 0}}, Mint{GOLD, 5}, {Token{Token::Kind::CONTROL, GOLD, 1}, Token{Token::Kind::ASSET, GOLD, 5}}));
}

BOOST_AUTO_TEST_CASE(existing_asset_with_a_pool_refused)
{
    Chain chain;
    const AssetId SILVER{HashName("SILVER")};
    const uint64_t supply{1'000'000ULL * 1'000'000};
    const auto [control, coins]{chain.Issue("SILVER", supply, 6)};
    chain.EndBlock();
    // A pool with the coin of the kind it would graduate into: refused (the curve would graduate into it).
    const CTransaction pooled{MakeTx({coins}, AddLiquidity{SILVER, CHN, 1'000'000'000, 10'000'000, 1}, {std::nullopt, std::nullopt, Token{Token::Kind::ASSET, SILVER, supply - 1'000'000'000}},
                                     10'000'000)};
    chain.Require(pooled);
    chain.EndBlock();
    CurveAsset curve;
    curve.asset = SILVER;
    curve.amount = 8'000'000;
    curve.fee = 10'000;
    curve.creator = CREATOR;
    BOOST_CHECK_EQUAL(chain.Refused(MakeTx({control, COutPoint{pooled.GetHash(), 2}}, curve,
                                           {Token{Token::Kind::CONTROL, SILVER, 1}, Token{Token::Kind::ASSET, SILVER, supply - 1'000'000'000 - curve.amount}})), "bad-ba-launch-pooled");
}

BOOST_AUTO_TEST_SUITE_END()
