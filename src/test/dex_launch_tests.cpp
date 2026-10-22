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

BOOST_AUTO_TEST_SUITE_END()
