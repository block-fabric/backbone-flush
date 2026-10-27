// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Upgrade v8 (SidechainParams::v8_height, doc/photon-slot.md "Upgrade v8"): the buybacks scanned MAX_BUYBACK_SCAN a block
// from a cursor (each comes round in turn), and one that cannot buy yet out of the index until more coin comes for it:
// the per-block work bounded whatever the count of launches. Conservation throughout (HeldChn), as in v7's tests.
//
// (The fixture of v7's tests:)
// Upgrade v7 (SidechainParams::v7_height, doc/photon-slot.md "Upgrade v7"):
//  - the fee split: a launch's curve fee 25 % security, 75 % split as its launch says (FeeShare: the creator, burn, up to 3
//    addresses), the same split for the creator's 60 % of its locked position's fees; no satoshi lost or made (SplitCoin);
//    the addresses paid by the coinbase once MIN_FEE_SHARE_PAYOUT is owed, within the block's room for payouts;
//  - instant payouts: the assets' payouts are paid by the block itself, with or without the peg actions (PayBlock), the
//    per-block bound (MAX_TX_PAYOUTS_PER_BLOCK) kept, the peg actions' queues as before.
// Conservation throughout: the coin the assets hold is what came in less what went out (HeldChn).

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

#include <map>
#include <string>
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
    std::vector<FeeShare> split;
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
        params.v7_height = 0;
        params.v8_height = 0;
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
        launch.split = v5.split;
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


const CScript ADDR_A{GetScriptForDestination(WitnessV0KeyHash{uint160{std::vector<unsigned char>(20, 11)}})};
const CScript ADDR_B{GetScriptForDestination(WitnessV0KeyHash{uint160{std::vector<unsigned char>(20, 12)}})};
const CScript ADDR_C{GetScriptForDestination(WitnessV0KeyHash{uint160{std::vector<unsigned char>(20, 13)}})};
const CScript ADDR_D{GetScriptForDestination(WitnessV0KeyHash{uint160{std::vector<unsigned char>(20, 14)}})};
FeeShare CreatorShare(uint16_t bps) { return FeeShare{FeeShare::CREATOR, {}, bps}; }
FeeShare BurnShare(uint16_t bps) { return FeeShare{FeeShare::BURN, {}, bps}; }
FeeShare To(const CScript& script, uint16_t bps) { return FeeShare{FeeShare::ADDRESS, script, bps}; }
uint64_t PaidTo(const std::vector<CTxOut>& outs, const CScript& script)
{
    uint64_t sum{0};
    for (const CTxOut& out : outs) {
        if (out.scriptPubKey == script) sum += out.nValue;
    }
    return sum;
}

struct V8Setup : BasicTestingSetup {
    V8Setup() : BasicTestingSetup{ChainType::REGTEST, {.extra_args = {"-sidechainslot=7", "-testactivationheight=dex@0", "-testactivationheight=dexcurves@0", "-testactivationheight=feeflow@0", "-testactivationheight=v7@0", "-testactivationheight=v8@0"}}} {}
};

/** `n` launches all to buyback and burn, no first buy, a block each 20. */
std::vector<AssetId> BurnLaunches(Chain& chain, int n, const std::string& stem)
{
    std::vector<AssetId> tokens;
    V5Options opts;
    opts.split = {BurnShare(10'000)};
    for (int i{0}; i < n; ++i) {
        const std::string name{stem + std::to_string(i)};
        chain.Launch(name, 0, 0, opts);
        tokens.push_back(HashName(name));
        if (i % 20 == 19) chain.EndBlock();
    }
    chain.EndBlock();
    return tokens;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(dex_v8_tests, V8Setup)

BOOST_AUTO_TEST_CASE(buybacks_take_turns_a_bounded_number_a_block)
{
    Chain chain;
    // The index of buybacks (BUYBACKS), read here: a table of the store's own id, for the test only (it takes its id back when it goes).
    const sidechain::Table<AssetId, bool> buyback_index{0xa2};
    BOOST_REQUIRE(Params().GetConsensus().sidechain.BoundedBuybacksActive(chain.height));
    constexpr int N{80};
    static_assert(N > int{MAX_BUYBACK_SCAN});
    const auto tokens{BurnLaunches(chain, N, "BB")};
    // A buy on each curve in one block: each fee's tunable part waits to buy its token back.
    for (const AssetId& token : tokens) chain.Buy(chain.Assets().GetLaunch(token)->pool, COIN_UNITS);
    chain.EndBlock();
    // Bought back: some of the token burned (the buyback's own fee comes back to it as a little more coin waiting).
    const auto bought{[&]() {
        int n{0};
        for (const AssetId& token : tokens) n += chain.Assets().GetLaunch(token)->burned > 0;
        return n;
    }};
    for (const AssetId& token : tokens) {
        BOOST_CHECK_GE(chain.Assets().GetLaunch(token)->buyback, MIN_CHN_PAYOUT_SMALL);
        BOOST_CHECK(buyback_index.Contains(chain.store, token));
    }
    BOOST_CHECK_EQUAL(bought(), 0);
    // The next block buys MAX_BUYBACK_SCAN back, the one after the rest: every one in turn.
    chain.EndBlock();
    BOOST_CHECK_EQUAL(bought(), int{MAX_BUYBACK_SCAN});
    chain.EndBlock();
    BOOST_CHECK_EQUAL(bought(), N);
    // What comes back of their own fees is less than the least payout: out of the index once looked at again.
    for (int b{0}; b < 3; ++b) chain.EndBlock();
    for (const AssetId& token : tokens) {
        const auto record{*chain.Assets().GetLaunch(token)};
        BOOST_CHECK_LT(record.buyback, MIN_CHN_PAYOUT_SMALL);
        BOOST_CHECK_EQUAL(chain.Assets().GetAsset(token)->supply, record.Supply() - record.burned);
        BOOST_CHECK(!buyback_index.Contains(chain.store, token));
    }
}

BOOST_AUTO_TEST_CASE(tiny_buybacks_leave_the_index)
{
    // Many launches whose coin waiting to buy back is less than the least payout: kept in their records, out of the
    // index once looked at (at most MAX_BUYBACK_SCAN a block), so that no block looks at them again; more coin puts one back.
    Chain chain;
    const sidechain::Table<AssetId, bool> buyback_index{0xa2};
    constexpr int N{100};
    const auto tokens{BurnLaunches(chain, N, "TB")};
    for (const AssetId& token : tokens) chain.Buy(chain.Assets().GetLaunch(token)->pool, 50'000);
    chain.EndBlock();
    for (const AssetId& token : tokens) {
        const uint64_t waiting{chain.Assets().GetLaunch(token)->buyback};
        BOOST_CHECK_GT(waiting, 0U);
        BOOST_CHECK_LT(waiting, MIN_CHN_PAYOUT_SMALL);
    }
    for (int b{0}; b < 2; ++b) chain.EndBlock();
    for (const AssetId& token : tokens) {
        BOOST_CHECK(!buyback_index.Contains(chain.store, token));
        BOOST_CHECK_GT(chain.Assets().GetLaunch(token)->buyback, 0U);
    }
    // More for one: back in the index, bought back the next block.
    const AssetId& one{tokens[42]};
    chain.Buy(chain.Assets().GetLaunch(one)->pool, 2 * COIN_UNITS);
    chain.EndBlock();
    BOOST_CHECK(buyback_index.Contains(chain.store, one));
    chain.EndBlock();
    BOOST_CHECK_GT(chain.Assets().GetLaunch(one)->burned, 0U);
    // Its own buybacks' fees come back smaller each time: below the least, it leaves the index.
    for (int b{0}; b < 6 && chain.Assets().GetLaunch(one)->buyback >= MIN_CHN_PAYOUT_SMALL; ++b) chain.EndBlock();
    BOOST_CHECK_LT(chain.Assets().GetLaunch(one)->buyback, MIN_CHN_PAYOUT_SMALL);
    chain.EndBlock();
    BOOST_CHECK(!buyback_index.Contains(chain.store, one));
}

BOOST_AUTO_TEST_SUITE_END()
