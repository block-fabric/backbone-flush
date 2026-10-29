// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Layers: what holds FastBlocks' blocks of stake and BitAssets' assets together in the state of the chain.
//  - The bound of the assets' CHN payouts (bitassets::MAX_TX_PAYOUTS_PER_BLOCK) counts the assets' payouts still
//    waiting from blocks that paid none (blocks of stake take no peg actions), and those alone: what the chain's own
//    peg actions owe (FastBlocks' unbonds and rewards, in the same queue) takes none of the assets' room, and the
//    assets' bound is still kept. The count is in the store: the journal undoes it with the block.
//  - The CHN the pools and the auctions hold are in the peg balance (bitassets::State::HeldChn).

#include <addresstype.h>
#include <bitassets/state.h>
#include <chainparams.h>
#include <consensus/params.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sidechain/state.h>
#include <sidechain/store.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

using namespace bitassets;

namespace {
const CScript HOLDER{GetScriptForDestination(WitnessV0KeyHash{uint160{}})};
int g_nonce{0};

Token AssetToken(const AssetId& id, uint64_t amount) { return Token{Token::Kind::ASSET, id, amount}; }

CTransaction MakeTx(const std::vector<COutPoint>& inputs, std::optional<Operation> op, const std::vector<std::optional<Token>>& outs, CAmount chn_in = 0)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(++g_nonce)}), static_cast<uint32_t>(1000 + g_nonce)});
    for (const COutPoint& in : inputs) tx.vin.emplace_back(in);
    Marker marker;
    marker.operation = std::move(op);
    for (const auto& token : outs) {
        marker.outputs.push_back({static_cast<uint32_t>(tx.vout.size()), token});
        tx.vout.emplace_back(0, HOLDER);
    }
    tx.vout.emplace_back(chn_in, MarkerScript(marker));
    return CTransaction{tx};
}

/** The chain's state on a store of its own (journaled: each "block" is undone by its journal). */
struct Chain {
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State state{store};
    Consensus::SidechainParams params;
    int height{100};
    std::vector<CTxOut> payouts;
    std::vector<CTxOut> refunds;

    Chain() { params.bitassets_reveal_depth = 0; }

    bool Apply(const CTransaction& tx, std::string& reason)
    {
        return state.ApplyTx(tx, height, params, payouts, refunds, reason, /*main_pending=*/false, /*peg_step=*/false);
    }
    /** The end of a block: what the chain's own peg actions owe (`peg_owed`) and the assets' payouts of the block wait,
     * or are paid (`max`: a block with the peg actions). */
    std::vector<CTxOut> EndBlock(size_t peg_owed, uint64_t max = 0)
    {
        std::vector<CTxOut> owed(peg_owed, CTxOut{1000, HOLDER});
        const size_t assets{payouts.size()};
        owed.insert(owed.end(), payouts.begin(), payouts.end());
        payouts.clear();
        ++height;
        return state.TakePayouts({}, std::move(owed), max, assets);
    }
    std::pair<COutPoint, COutPoint> Issue(const std::string& name, uint64_t supply)
    {
        std::string reason;
        const AssetId id{HashName(name)};
        const uint256 nonce{HashName("nonce " + name)};
        CMutableTransaction reserve{MakeTx({}, Reserve{ReservationCommitment(id, nonce, HOLDER)}, {Token{Token::Kind::RESERVATION, uint256{}, 1}})};
        const CTransaction reserved{reserve};
        BOOST_REQUIRE_MESSAGE(Apply(reserved, reason), reason);
        Register reg;
        reg.name = id;
        reg.nonce = nonce;
        reg.supply = supply;
        reg.text = name;
        const CTransaction tx{MakeTx({COutPoint{reserved.GetHash(), 0}}, reg, {Token{Token::Kind::CONTROL, id, 1}, AssetToken(id, supply)})};
        BOOST_REQUIRE_MESSAGE(Apply(tx, reason), reason);
        return {COutPoint{tx.GetHash(), 0}, COutPoint{tx.GetHash(), 1}};
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(layers_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(asset_payouts_bound_counts_their_own_across_blocks_of_stake)
{
    Chain chain;
    std::string reason;
    const AssetId GOLD{HashName("GOLD")};
    const auto [control, gold]{chain.Issue("GOLD", 20'000'000)};
    const CTransaction pool{MakeTx({gold}, AddLiquidity{GOLD, CHN, 10'000'000, 1'000'000'000, 1}, {AssetToken(GOLD, 10'000'000), std::nullopt, std::nullopt}, 1'000'000'000)};
    BOOST_REQUIRE_MESSAGE(chain.Apply(pool, reason), reason);
    BOOST_CHECK_EQUAL(chain.state.BitAssets().HeldChn(), 1'000'000'000);
    chain.EndBlock(0);

    // A block of stake: 800 payouts the chain's own peg actions owe (unbonds, rewards) wait in the queue of the
    // transactions' payouts. They are not the assets': a swap that pays CHN out is still valid.
    chain.EndBlock(800);
    BOOST_CHECK_EQUAL(chain.state.TxQueueLength(), 800U);
    BOOST_CHECK_EQUAL(chain.state.AssetPayoutsQueued(), 0U);
    COutPoint left{pool.GetHash(), 0};
    uint64_t gold_left{10'000'000};
    const auto swap{[&]() {
        gold_left -= 10'000;
        return MakeTx({left}, Swap{GOLD, 10'000, CHN, 1, HOLDER}, {AssetToken(GOLD, gold_left)});
    }};
    const CTransaction first{swap()};
    BOOST_REQUIRE_MESSAGE(chain.Apply(first, reason), reason);
    left = COutPoint{first.GetHash(), 0};
    BOOST_CHECK_EQUAL(chain.payouts.size(), 1U);
    chain.EndBlock(10);
    BOOST_CHECK_EQUAL(chain.state.TxQueueLength(), 811U);
    BOOST_CHECK_EQUAL(chain.state.AssetPayoutsQueued(), 1U);

    // More of the assets' payouts wait (blocks of stake pay none), up to one short of the bound.
    for (size_t i{0}; i < MAX_TX_PAYOUTS_PER_BLOCK - 2; ++i) chain.payouts.emplace_back(MIN_CHN_PAYOUT_BEFORE_SMALL, HOLDER);
    chain.EndBlock(0);
    BOOST_CHECK_EQUAL(chain.state.AssetPayoutsQueued(), MAX_TX_PAYOUTS_PER_BLOCK - 1);

    // The bound still holds, counted with what waits: one more payout, not two.
    const CTransaction last{swap()};
    BOOST_REQUIRE_MESSAGE(chain.Apply(last, reason), reason);
    left = COutPoint{last.GetHash(), 0};
    const CTransaction beyond{swap()};
    BOOST_CHECK(!chain.Apply(beyond, reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-block-payouts-full");

    // Taken back (a reorganisation): the journal undoes the count with the block.
    (void)chain.store.TakeUndo();
    const std::vector<CTxOut> pending{chain.payouts};
    chain.EndBlock(0);
    BOOST_CHECK_EQUAL(chain.state.AssetPayoutsQueued(), MAX_TX_PAYOUTS_PER_BLOCK);
    const sidechain::StoreUndo block{chain.store.TakeUndo()};
    chain.store.Revert(block);
    BOOST_CHECK_EQUAL(chain.state.AssetPayoutsQueued(), MAX_TX_PAYOUTS_PER_BLOCK - 1);
    chain.payouts = pending;
    chain.EndBlock(0);

    // A block with the peg actions pays 1000: the oldest first (the 800, the 10, then the assets').
    const auto paid{chain.EndBlock(0, sidechain::MAX_PAYOUTS_PER_BLOCK)};
    BOOST_CHECK_EQUAL(paid.size(), sidechain::MAX_PAYOUTS_PER_BLOCK);
    const uint64_t queued_tx{800 + 10 + MAX_TX_PAYOUTS_PER_BLOCK - sidechain::MAX_PAYOUTS_PER_BLOCK};
    BOOST_CHECK_EQUAL(chain.state.TxQueueLength(), queued_tx);
    BOOST_CHECK_EQUAL(chain.state.AssetPayoutsQueued(), queued_tx);
    // Room again: the swap is valid now.
    BOOST_CHECK_MESSAGE(chain.Apply(beyond, reason), reason);

    // All paid: the queue starts again at 0, and so does the count, with no entry left in the store.
    chain.EndBlock(0, sidechain::MAX_PAYOUTS_PER_BLOCK);
    BOOST_CHECK_EQUAL(chain.state.TxQueueLength(), 0U);
    BOOST_CHECK_EQUAL(chain.state.AssetPayoutsQueued(), 0U);
    BOOST_CHECK(!chain.store.Get(sidechain::StoreBytes{'a'}));
}

/** The first block of every network, as getblockhash 0 shows it (feature_layers_genesis.py checks the
 * daemon, flush-util and doc/layers.md against the same). The test network's is that of its restart of
 * 19/Oct/2026: it changes only with a restart of the test network. */
BOOST_AUTO_TEST_CASE(genesis_hashes)
{
    const std::vector<std::pair<ChainType, std::string>> expected{
        {ChainType::MAIN, "164aeae68926b8814474e6f1580a6253df0c04d53826ea715400a8070dab0857"},
        {ChainType::TESTNET, "6ad3a3ff068fecfba95fc4aeec8404ac1db31cb840b852164db054ff31daa382"},
        {ChainType::SIGNET, "68c4854ae0f132e80760c9089066ffb4a7ef6ded09387e941f3430edbf45e101"},
        {ChainType::REGTEST, "0f9188f13cb7b2c71f2a335e3a4fc328bf5beb436012afca590b1a11466e2206"},
        // Flush on eCash (README.md).
        {ChainType::ECASH, "19b25cff92c2beec1032a50d1eff87b01e1d67e658e25ca099fe599213843662"},
        {ChainType::ECASH_BETANET, "7d66b10e351e5ad86c562ba761b5476f75c6f1b9f13aa7cf469df3a8d5701b75"},
        {ChainType::ECASH_TESTNET, "2ee6a1d1e193d4115dedda242b367454e028804ba88618ba96b728789466f38d"},
    };
    for (const auto& [chain, hash] : expected) {
        const auto params{CreateChainParams(*m_node.args, chain)};
        BOOST_CHECK_EQUAL(params->GenesisBlock().GetHash().GetHex(), hash);
        BOOST_CHECK_EQUAL(params->GetConsensus().hashGenesisBlock.GetHex(), hash);
    }
}

/** Every rule of Layers from the first block on every network but regtest (the test network since its restart
 * of 19/Oct/2026): no activation height of the test network's own, only the mainchain's slot activation. A merge
 * of FastBlocks' or BitAssets' chain parameters must not bring one of theirs back. */
BOOST_AUTO_TEST_CASE(rules_from_genesis)
{
    for (const auto chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::SIGNET, ChainType::ECASH, ChainType::ECASH_BETANET, ChainType::ECASH_TESTNET}) {
        const auto params{CreateChainParams(*m_node.args, chain)};
        const Consensus::Params& consensus{params->GetConsensus()};
        BOOST_CHECK_EQUAL(consensus.sidechain.dex_height, 0);
        BOOST_CHECK_EQUAL(consensus.sidechain.logo_height, 0);
        BOOST_CHECK_EQUAL(consensus.sidechain.ticker_height, 0);
        BOOST_CHECK_EQUAL(consensus.pos.fee_split_height, 0);
        BOOST_CHECK_EQUAL(consensus.pos.fee_curve2_height, 0);
    }
    BOOST_CHECK_EQUAL(CreateChainParams(*m_node.args, ChainType::TESTNET)->GetConsensus().sidechain.main_activation_height, 4997);
}

BOOST_AUTO_TEST_SUITE_END()
