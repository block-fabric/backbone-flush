// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
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

struct V7Setup : BasicTestingSetup {
    V7Setup() : BasicTestingSetup{ChainType::REGTEST, {.extra_args = {"-sidechainslot=7", "-testactivationheight=dex@0", "-testactivationheight=dexcurves@0", "-testactivationheight=feeflow@0", "-testactivationheight=v7@0"}}} {}
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(dex_v7_tests, V7Setup)

BOOST_AUTO_TEST_CASE(split_checks)
{
    BOOST_CHECK(CheckFeeSplit({CreatorShare(10'000)}));
    BOOST_CHECK(CheckFeeSplit({BurnShare(10'000)}));
    BOOST_CHECK(CheckFeeSplit({To(ADDR_A, 10'000)}));
    BOOST_CHECK(CheckFeeSplit({CreatorShare(7'500), To(ADDR_A, 2'500)}));
    BOOST_CHECK(CheckFeeSplit({CreatorShare(4'000), To(ADDR_A, 1'000), To(ADDR_B, 2'000), To(ADDR_C, 2'000), BurnShare(1'000)}));
    // None, too many, a share of 0, sums off by one, twice the creator or the burn, four addresses, one twice, a script on the creator.
    BOOST_CHECK(!CheckFeeSplit({}));
    BOOST_CHECK(!CheckFeeSplit({CreatorShare(3'000), To(ADDR_A, 1'000), To(ADDR_B, 2'000), To(ADDR_C, 2'000), BurnShare(1'000), To(ADDR_D, 1'000)}));
    BOOST_CHECK(!CheckFeeSplit({CreatorShare(10'000), BurnShare(0)}));
    BOOST_CHECK(!CheckFeeSplit({CreatorShare(9'999)}));
    BOOST_CHECK(!CheckFeeSplit({CreatorShare(7'501), To(ADDR_A, 2'500)}));
    BOOST_CHECK(!CheckFeeSplit({CreatorShare(5'000), CreatorShare(5'000)}));
    BOOST_CHECK(!CheckFeeSplit({BurnShare(5'000), BurnShare(5'000)}));
    BOOST_CHECK(!CheckFeeSplit({To(ADDR_A, 2'500), To(ADDR_B, 2'500), To(ADDR_C, 2'500), To(ADDR_D, 2'500)}));
    BOOST_CHECK(!CheckFeeSplit({To(ADDR_A, 5'000), To(ADDR_A, 5'000)}));
    BOOST_CHECK(!CheckFeeSplit({FeeShare{FeeShare::CREATOR, ADDR_A, 10'000}}));
    BOOST_CHECK(!CheckFeeSplit({FeeShare{FeeShare::ADDRESS, {}, 10'000}}));
    BOOST_CHECK(!CheckFeeSplit({FeeShare{3, {}, 10'000}}));
    // v5's modes map onto a split.
    BOOST_CHECK(SplitOfDestination(0, 0) == std::vector<FeeShare>{CreatorShare(10'000)});
    BOOST_CHECK(SplitOfDestination(static_cast<uint8_t>(FeeDestination::BURN), 0) == std::vector<FeeShare>{BurnShare(10'000)});
    BOOST_CHECK((SplitOfDestination(static_cast<uint8_t>(FeeDestination::SPLIT), 2'500) == std::vector<FeeShare>{BurnShare(2'500), CreatorShare(7'500)}));
}

BOOST_AUTO_TEST_CASE(split_coin_conserves)
{
    // Every amount, every split of 0 to 3 addresses with or without the creator and the burn: the parts sum to it
    // exactly, each within a satoshi of its exact share (the remainder's share within the count of shares).
    FastRandomContext rng{/*fDeterministic=*/true};
    const std::vector<CScript> addresses{ADDR_A, ADDR_B, ADDR_C};
    for (int round{0}; round < 20'000; ++round) {
        std::vector<FeeShare> split;
        const bool creator{rng.randbool()}, burn{rng.randbool()};
        const size_t extra{rng.randrange(4)};
        const size_t n{(creator ? 1U : 0U) + (burn ? 1U : 0U) + extra};
        if (n == 0) continue;
        // n positive shares summing to 10000.
        std::vector<uint16_t> bps(n, 1);
        for (int left{FEE_SPLIT_BPS - static_cast<int>(n)}; left > 0;) {
            const int take{static_cast<int>(rng.randrange(left)) + 1};
            bps[rng.randrange(n)] += take;
            left -= take;
        }
        size_t k{0};
        if (creator) split.push_back(CreatorShare(bps[k++]));
        for (size_t i{0}; i < extra; ++i) split.push_back(To(addresses[i], bps[k++]));
        if (burn) split.push_back(BurnShare(bps[k++]));
        BOOST_REQUIRE(CheckFeeSplit(split));
        const uint64_t coin{round % 3 == 0 ? rng.randrange(100) : round % 3 == 1 ? rng.randrange(uint64_t{1} << 40) : rng.rand64() >> 2};
        const auto parts{SplitCoin(split, coin)};
        unsigned __int128 sum{0};
        for (size_t i{0}; i < n; ++i) {
            sum += parts[i];
            const unsigned __int128 exact{static_cast<unsigned __int128>(coin) * split[i].bps};
            // part * 10000 within [exact - 10000, exact + 10000 * n].
            BOOST_CHECK(static_cast<unsigned __int128>(parts[i]) * FEE_SPLIT_BPS + FEE_SPLIT_BPS > exact);
            BOOST_CHECK(static_cast<unsigned __int128>(parts[i]) * FEE_SPLIT_BPS <= exact + static_cast<unsigned __int128>(FEE_SPLIT_BPS) * n);
        }
        BOOST_CHECK(sum == coin);
    }
}

BOOST_AUTO_TEST_CASE(marker_with_a_split)
{
    LaunchToken launch;
    launch.reg.name = HashName("MOON");
    launch.reg.text = "MOON";
    launch.reg.decimals = LAUNCH_DECIMALS;
    launch.creator = CREATOR;
    launch.split = {CreatorShare(7'500), To(ADDR_A, 2'500)};
    Marker marker;
    marker.operation = launch;
    const CScript script{MarkerScript(marker)};
    const auto read{ParseMarkerScript(script)};
    BOOST_REQUIRE(read);
    BOOST_CHECK(std::get<LaunchToken>(*read->operation).split == launch.split);
    // Refused before v7.
    CMutableTransaction mtx;
    mtx.vout.emplace_back(0, script);
    const CTransaction tx{mtx};
    std::string reason;
    BOOST_CHECK(!CheckFeeSplitActivation(tx, false, reason));
    BOOST_CHECK(CheckFeeSplitActivation(tx, true, reason));
}

BOOST_AUTO_TEST_CASE(curve_fees_split_and_paid)
{
    Chain chain;
    const AssetId MOON{HashName("MOON")};
    V5Options opts;
    opts.split = {CreatorShare(4'000), To(ADDR_A, 2'500), To(ADDR_B, 2'000), To(ADDR_C, 500), BurnShare(1'000)};
    chain.Launch("MOON", 0, COIN_UNITS, opts);
    chain.EndBlock();
    uint64_t paid_a{PaidTo(chain.paid, ADDR_A)}, paid_b{PaidTo(chain.paid, ADDR_B)}, paid_c{PaidTo(chain.paid, ADDR_C)};
    auto record{*chain.Assets().GetLaunch(MOON)};
    BOOST_CHECK(record.v7);
    BOOST_CHECK(record.split == opts.split);
    // 25 % security, 75 % the split's.
    BOOST_CHECK_EQUAL(record.security_share, 250'000U);
    BOOST_CHECK_EQUAL(record.creator_share, 750'000U);
    for (int i{0}; i < 12; ++i) {
        chain.Buy(record.pool, 7 * COIN_UNITS + 12'345 * i);
        chain.EndBlock();
        paid_a += PaidTo(chain.paid, ADDR_A);
        paid_b += PaidTo(chain.paid, ADDR_B);
        paid_c += PaidTo(chain.paid, ADDR_C);
        record = *chain.Assets().GetLaunch(MOON);
        // What each share was given: in all, what the tunable part was (the creator's, the burn's and the addresses').
        uint64_t given{0};
        for (const uint64_t g : record.share_given) given += g;
        BOOST_CHECK_EQUAL(record.share_given[0], record.creator_fees + record.creator_paid);
        BOOST_CHECK_EQUAL(record.share_given[1], paid_a + record.share_owed[1]);
        BOOST_CHECK_EQUAL(record.share_given[2], paid_b + record.share_owed[2]);
        BOOST_CHECK_EQUAL(record.share_given[3], paid_c + record.share_owed[3]);
        // Owed less than the least payout only: the rest was paid.
        for (size_t s{1}; s <= 3; ++s) BOOST_CHECK_LT(record.share_owed[s], MIN_FEE_SHARE_PAYOUT);
        BOOST_CHECK(given > 0);
        // The shares as split: within a satoshi a credit of their exact part.
        for (size_t s{0}; s < record.split.size(); ++s) {
            const double exact{static_cast<double>(given) * record.split[s].bps / FEE_SPLIT_BPS};
            BOOST_CHECK_LE(std::abs(static_cast<double>(record.share_given[s]) - exact), 5.0 * (i + 2));
        }
    }
    BOOST_CHECK_GT(paid_a, 0U);
    BOOST_CHECK_GT(paid_b, 0U);
    BOOST_CHECK_GT(record.burned + record.buyback, 0U);
    // The creator claims its part (as before); the addresses' are no one's to claim.
    const CTransaction claim{MakeTx({}, ClaimLaunch{MOON}, {})};
    chain.Require(claim);
    chain.EndBlock();
    BOOST_CHECK_EQUAL(PaidTo(chain.paid, CREATOR), record.creator_fees);
}

BOOST_AUTO_TEST_CASE(old_modes_map_onto_a_split)
{
    Chain chain;
    const AssetId MOON{HashName("MOON")}, STAR{HashName("STAR")};
    V5Options v5;
    v5.destination = static_cast<uint8_t>(FeeDestination::SPLIT);
    v5.burn_bps = 2'500;
    chain.Launch("MOON", 0, 0, v5);
    chain.Launch("STAR", 0, 0);
    chain.EndBlock();
    BOOST_CHECK((chain.Assets().GetLaunch(MOON)->split == std::vector<FeeShare>{BurnShare(2'500), CreatorShare(7'500)}));
    BOOST_CHECK(chain.Assets().GetLaunch(STAR)->split == std::vector<FeeShare>{CreatorShare(10'000)});
    const auto view{FeeSplitView(*chain.Assets().GetLaunch(MOON))};
    BOOST_REQUIRE_EQUAL(view.size(), 2U);
    BOOST_CHECK_EQUAL(std::string{view[0].kind}, "burn");
    BOOST_CHECK_EQUAL(std::string{view[1].kind}, "creator");
    // (A split and a destination at once cannot be written: the marker of a split has no destination.)
}

BOOST_AUTO_TEST_CASE(locked_position_fees_split)
{
    Chain chain;
    const AssetId MOON{HashName("MOON")};
    V5Options opts;
    opts.split = {CreatorShare(5'000), To(ADDR_A, 5'000)};
    const LaunchRecord graduated{chain.Graduated("MOON", 0, opts)};
    const uint64_t given_before{graduated.share_given[1]};
    // Trades in the locked pool: its fees' creator's 60 %, half of it to the address.
    for (int i{0}; i < 5; ++i) {
        chain.Buy(graduated.pool, 30 * COIN_UNITS);
        chain.EndBlock();
    }
    const auto record{*chain.Assets().GetLaunch(MOON)};
    BOOST_CHECK_GT(record.share_given[1], given_before);
    // The creator's and the address's shares stay equal within the roundings (the remainder to the creator).
    const int64_t gap{static_cast<int64_t>(record.share_given[0]) - static_cast<int64_t>(record.share_given[1])};
    BOOST_CHECK(gap >= 0 && gap < 100);
}

BOOST_AUTO_TEST_CASE(fee_share_payouts_bounded)
{
    // The addresses are paid within the room the block's payouts leave: with none left, they wait.
    Chain chain;
    const AssetId MOON{HashName("MOON")};
    V5Options opts;
    opts.split = {To(ADDR_A, 10'000)};
    chain.Launch("MOON", 0, COIN_UNITS, opts);
    chain.EndBlock();
    const uint256 pool{chain.Assets().GetLaunch(MOON)->pool};
    chain.Buy(pool, 50 * COIN_UNITS);
    // The block's payouts full (placeholders as if its transactions had paid out the rest of the bound).
    chain.payouts.insert(chain.payouts.end(), MAX_TX_PAYOUTS_PER_BLOCK, CTxOut{static_cast<CAmount>(MIN_CHN_PAYOUT_SMALL), HOLDER});
    chain.burned += static_cast<int64_t>(MIN_CHN_PAYOUT_SMALL) * static_cast<int64_t>(MAX_TX_PAYOUTS_PER_BLOCK);
    chain.EndBlock();
    BOOST_CHECK_EQUAL(PaidTo(chain.paid, ADDR_A), 0U);
    const uint64_t owed{chain.Assets().GetLaunch(MOON)->share_owed[0]};
    BOOST_CHECK_GE(owed, MIN_FEE_SHARE_PAYOUT);
    // The next block has room: paid, all of it.
    chain.EndBlock();
    BOOST_CHECK_EQUAL(PaidTo(chain.paid, ADDR_A), owed);
    BOOST_CHECK_EQUAL(chain.Assets().GetLaunch(MOON)->share_owed[0], 0U);
}

BOOST_AUTO_TEST_CASE(instant_payouts_every_block)
{
    // PayBlock from v7: the assets' payouts are the block's own, with or without the peg actions; the peg actions' owed wait.
    Chain chain;
    const CTxOut asset{static_cast<CAmount>(MIN_CHN_PAYOUT_SMALL), PAYEE}, peg{1000, HOLDER}, deposit{2000, CREATOR};
    // A block without the peg actions: the assets' paid, the peg actions' and the deposits wait.
    auto paid{chain.state.PayBlock(chain.height, chain.params, /*peg_step=*/false, {deposit}, {peg}, {asset, asset})};
    BOOST_CHECK_EQUAL(paid.size(), 2U);
    BOOST_CHECK(paid[0] == asset && paid[1] == asset);
    BOOST_CHECK_EQUAL(chain.state.TxQueueLength(), 1U);
    BOOST_CHECK_EQUAL(chain.state.AssetPayoutsQueued(), 0U);
    BOOST_CHECK_EQUAL(chain.state.Queue().size(), 1U);
    // A block with them: the queues first, then the block's own.
    paid = chain.state.PayBlock(chain.height + 1, chain.params, /*peg_step=*/true, {}, {}, {asset});
    BOOST_REQUIRE_EQUAL(paid.size(), 3U);
    BOOST_CHECK(paid[0] == deposit && paid[1] == peg && paid[2] == asset);
    // Before v7 the same block waits for the peg actions.
    Consensus::SidechainParams before{chain.params};
    before.v7_height = 1'000'000;
    paid = chain.state.PayBlock(chain.height + 2, before, /*peg_step=*/false, {}, {}, {asset});
    BOOST_CHECK(paid.empty());
    BOOST_CHECK_EQUAL(chain.state.AssetPayoutsQueued(), 1U);
    paid = chain.state.PayBlock(chain.height + 3, chain.params, /*peg_step=*/true, {}, {}, {});
    BOOST_CHECK_EQUAL(paid.size(), 1U);
}

BOOST_AUTO_TEST_CASE(instant_payouts_bound)
{
    // The per-block bound: a block pays at most MAX_PAYOUTS_PER_BLOCK, the mainchain's queue keeps its half, the block's
    // own assets' payouts (at most MAX_TX_PAYOUTS_PER_BLOCK) out of the other half; any beyond the bound would wait.
    Chain chain;
    const CTxOut asset{static_cast<CAmount>(MIN_CHN_PAYOUT_SMALL), PAYEE}, peg{1000, HOLDER}, deposit{2000, CREATOR};
    const std::vector<CTxOut> deposits(sidechain::MAX_PAYOUTS_PER_BLOCK, deposit), pegs(sidechain::MAX_PAYOUTS_PER_BLOCK, peg);
    const std::vector<CTxOut> assets(MAX_TX_PAYOUTS_PER_BLOCK, asset);
    auto paid{chain.state.PayBlock(chain.height, chain.params, /*peg_step=*/true, deposits, pegs, assets)};
    BOOST_CHECK_EQUAL(paid.size(), sidechain::MAX_PAYOUTS_PER_BLOCK);
    BOOST_CHECK_EQUAL(std::count(paid.begin(), paid.end(), deposit), static_cast<long>(sidechain::MAX_PAYOUTS_PER_BLOCK / 2));
    BOOST_CHECK_EQUAL(std::count(paid.begin(), paid.end(), asset), static_cast<long>(MAX_TX_PAYOUTS_PER_BLOCK));
    BOOST_CHECK_EQUAL(std::count(paid.begin(), paid.end(), peg), 0);
    // Without the peg actions: the assets' alone, MAX_TX_PAYOUTS_PER_BLOCK at most; one beyond waits.
    std::vector<CTxOut> over(MAX_TX_PAYOUTS_PER_BLOCK + 1, asset);
    paid = chain.state.PayBlock(chain.height + 1, chain.params, /*peg_step=*/false, {}, {}, over);
    BOOST_CHECK_EQUAL(paid.size(), MAX_TX_PAYOUTS_PER_BLOCK);
    BOOST_CHECK_EQUAL(chain.state.AssetPayoutsQueued(), 1U);
    // The transactions' bound counts it: the next block takes MAX_TX_PAYOUTS_PER_BLOCK - 1 of its own.
}

BOOST_AUTO_TEST_CASE(sub_minimum_payees_cost_no_block_work)
{
    // Many launches whose split owes an address less than MIN_FEE_SHARE_PAYOUT: kept in their records, not in the index
    // of payees (FEE_PAYEES), which a block scans only for what it pays (at most its room): the per-block work does not
    // grow with them. Conservation holds (HeldChn counts what they are owed).
    Chain chain;
    const sidechain::Table<AssetId, bool> payees{0xa4};
    std::vector<AssetId> tokens;
    for (int i{0}; i < 60; ++i) {
        const std::string name{"DUST" + std::to_string(i)};
        V5Options opts;
        opts.split = {CreatorShare(5'000), To(ADDR_A, 5'000)};
        chain.Launch(name, 0, 100'000, opts);
        tokens.push_back(HashName(name));
    }
    chain.EndBlock();
    for (int b{0}; b < 3; ++b) chain.EndBlock();
    for (const AssetId& token : tokens) {
        const auto record{*chain.Assets().GetLaunch(token)};
        BOOST_CHECK_GT(record.share_owed[1], 0U);
        BOOST_CHECK_LT(record.share_owed[1], MIN_FEE_SHARE_PAYOUT);
        BOOST_CHECK(!payees.Contains(chain.store, token));
    }
    BOOST_CHECK_EQUAL(PaidTo(chain.paid, ADDR_A), 0U);
    // One of them trades enough: indexed, paid in its block, unindexed again.
    const auto record{*chain.Assets().GetLaunch(tokens[7])};
    chain.Buy(record.pool, 5 * COIN_UNITS);
    chain.EndBlock();
    const auto after{*chain.Assets().GetLaunch(tokens[7])};
    BOOST_CHECK_EQUAL(after.share_owed[1], 0U);
    BOOST_CHECK_EQUAL(PaidTo(chain.paid, ADDR_A), after.share_given[1]);
    BOOST_CHECK(!payees.Contains(chain.store, tokens[7]));
}

BOOST_AUTO_TEST_SUITE_END()
