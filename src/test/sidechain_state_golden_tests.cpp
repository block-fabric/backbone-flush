// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Golden values of the sidechain rules: a fixed sequence of blocks, connected one after another to a
// state of their own, and the hash of the state and of what each coinbase pays out after each block.
// The values were recorded with the hooks of sidechain/hooks.h off, as they are unless a sidechain
// installs its own: any change in what a block does to the state, or in the order of what its coinbase
// pays, fails here. A change meant to make one has to say so, and record the values again.

#include <addresstype.h>
#include <arith_uint256.h>
#include <bitassets/state.h>
#include <common/signmessage.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <hash.h>
#include <key.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sidechain/mainchain.h>
#include <sidechain/state.h>
#include <sidechain/store.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <functional>
#include <string>
#include <vector>

using namespace bitassets;

namespace {

/** A sidechain state, and the record of its mainchain, that blocks are connected to one after another. */
struct GoldenChain {
    Consensus::SidechainParams params;
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State state{store};
    sidechain::Mainchain mainchain;
    uint256 prev{uint256{0xab}};
    int height{0};
    //! What the coinbase of the last block connected paid out, after its first output.
    std::vector<CTxOut> paid;

    GoldenChain() { Main(); }

    /** Append a block to the record of the mainchain. */
    void Main(const std::function<void(sidechain::MainBlock&)>& fill = {})
    {
        sidechain::MainBlock block;
        const int h{mainchain.Height() + 1};
        block.hash = ArithToUint256(arith_uint256{static_cast<uint64_t>(0x10000 + h)} << 128);
        block.prev_hash = mainchain.TipHash();
        block.time = 1'700'000'000 + h;
        if (fill) fill(block);
        BOOST_REQUIRE(mainchain.Append(block));
    }

    /**
     * A block of these transactions on the last one, after a mainchain block with these events, its
     * coinbase paying what the rules want (worked out on an overlay that is dropped, as a miner does);
     * then a mainchain block that commits to it. Connected if valid; why not, if not.
     */
    std::string Connect(const std::vector<CTransactionRef>& txs, const std::function<void(sidechain::MainBlock&)>& events = {})
    {
        Main(events);
        CBlock block;
        block.nVersion = 0x20000000;
        block.hashPrevBlock = prev;
        block.nTime = 1'700'000'000 + height + 1;
        block.nBits = 0x207fffff;
        CMutableTransaction coinbase;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << (height + 1) << OP_0;
        coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
        block.vtx.push_back(MakeTransactionRef(coinbase));
        block.vtx.insert(block.vtx.end(), txs.begin(), txs.end());
        {
            sidechain::StoreOverlay scratch{store, /*journal=*/false};
            sidechain::State dry{scratch};
            std::vector<CTxOut> payouts, tx_payouts, refunds;
            std::string reason;
            if (dry.ApplyMainEvents(mainchain.Height(), mainchain, height + 1, params, payouts, reason) &&
                dry.ApplyTxs(block, height + 1, params, dry.MainPending(mainchain, params), /*peg_step=*/true, tx_payouts, refunds, reason)) {
                payouts.insert(payouts.end(), refunds.begin(), refunds.end());
                for (const CTxOut& out : dry.TakePayouts(std::move(payouts), std::move(tx_payouts))) coinbase.vout.push_back(out);
            }
        }
        block.vtx[0] = MakeTransactionRef(coinbase);
        block.hashMerkleRoot = BlockMerkleRoot(block);
        Main([&](sidechain::MainBlock& b) { b.bmm = block.GetHash(); });
        sidechain::StoreOverlay block_store{store, /*journal=*/false};
        sidechain::State block_state{block_store};
        CAmount minted{0};
        std::string reason;
        if (!block_state.ConnectBlock(block, height + 1, params, mainchain, minted, reason)) return reason;
        block_store.MergeInto(store);
        store.TakeUndo();
        ++height;
        prev = block.GetHash();
        paid.assign(coinbase.vout.begin() + 1, coinbase.vout.end());
        return {};
    }

    /** The hash of what the last block paid out, in order. */
    uint256 PaidHash() const
    {
        HashWriter hasher{};
        hasher << paid;
        return hasher.GetHash();
    }

    /** The state and what the last block paid out are those recorded. */
    void Check(const std::string& state_hash, const std::string& paid_hash, size_t paid_count)
    {
        BOOST_CHECK_EQUAL(state.Hash().GetHex(), state_hash);
        BOOST_CHECK_EQUAL(PaidHash().GetHex(), paid_hash);
        BOOST_CHECK_EQUAL(paid.size(), paid_count);
    }
};

/** The key of the tests: always the same, so that its signatures are too. */
CKey FixedKey(uint8_t n)
{
    std::vector<unsigned char> secret(32, 0);
    secret[0] = 0x5e;
    secret[31] = n;
    CKey key;
    key.Set(secret.begin(), secret.end(), /*fCompressedIn=*/true);
    return key;
}

/** Transactions, each spending a coin of its own so that every one is different. */
struct TxMaker {
    uint32_t nonce{0};

    CMutableTransaction Base(const std::vector<COutPoint>& inputs = {})
    {
        CMutableTransaction tx;
        ++nonce;
        tx.vin.emplace_back(COutPoint{Txid::FromUint256(ArithToUint256(arith_uint256{nonce} << 64)), nonce});
        for (const COutPoint& in : inputs) tx.vin.emplace_back(in);
        return tx;
    }
    CTransactionRef Outputs(std::vector<CTxOut> outputs)
    {
        CMutableTransaction tx{Base()};
        tx.vout = std::move(outputs);
        return MakeTransactionRef(std::move(tx));
    }
    /** Spends `inputs`, one token output (to `holder`) per entry of `outs`, then the marker. */
    CTransactionRef Assets(const std::vector<COutPoint>& inputs, std::optional<Operation> op, const std::vector<std::optional<Token>>& outs, const CScript& holder, CAmount chn_in = 0)
    {
        CMutableTransaction tx{Base(inputs)};
        Marker marker;
        marker.operation = std::move(op);
        for (const auto& token : outs) {
            marker.outputs.push_back({static_cast<uint32_t>(tx.vout.size()), token});
            tx.vout.emplace_back(0, holder);
        }
        tx.vout.emplace_back(chn_in, MarkerScript(marker));
        return MakeTransactionRef(std::move(tx));
    }
};

Token Asset(const AssetId& id, uint64_t amount) { return Token{Token::Kind::ASSET, id, amount}; }

CTxOut RefundOut(const CKey& key, const COutPoint& withdrawal)
{
    sidechain::RefundRequest request;
    request.withdrawal = withdrawal;
    BOOST_REQUIRE(key.SignCompact(MessageHash(sidechain::RefundMessage(withdrawal)), request.signature));
    return CTxOut{0, sidechain::RefundScript(request)};
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(sidechain_state_golden_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(refunds_asset_payouts_and_a_release)
{
    // More refunds than a block takes (MAX_REFUNDS_PER_BLOCK) and as many asset payouts as it takes
    // (MAX_TX_PAYOUTS_PER_BLOCK), with deposits; then a dead asset retired, which makes a withdrawal.
    GoldenChain chain;
    TxMaker make;
    const CKey key{FixedKey(1)};
    const CScript pay{GetScriptForDestination(WitnessV0KeyHash{key.GetPubKey()})};
    const CScript holder{GetScriptForDestination(WitnessV0KeyHash{FixedKey(2).GetPubKey()})};
    const size_t refunds{sidechain::MAX_REFUNDS_PER_BLOCK + 10};

    // Block 1: withdrawals, refundable by the key, and the reservation of GOLD.
    std::vector<CTxOut> withdrawal_outs;
    for (size_t i{0}; i < refunds; ++i) withdrawal_outs.emplace_back(COIN + static_cast<CAmount>(i), sidechain::WithdrawalScript(1000, key.GetPubKey().GetID(), pay));
    const CTransactionRef withdrawals{make.Outputs(withdrawal_outs)};
    const AssetId gold{HashName("GOLD")};
    const uint256 nonce{HashName("nonce GOLD")};
    const CTransactionRef reserve{make.Assets({}, Reserve{ReservationCommitment(gold, nonce, holder)}, {Token{Token::Kind::RESERVATION, uint256{}, 1}}, holder)};
    BOOST_REQUIRE_EQUAL(chain.Connect({withdrawals, reserve}), "");
    chain.Check("832c79901f9e49e9e006642a8eb138cefc628d06c22df1ecb8e90696a3b10589", "9a538906e6466ebd2617d321f71bc94e56056ce213d366773699e28158e00614", 0);

    // Block 2: GOLD registered, and a pool of GOLD and CHN.
    Register reg;
    reg.name = gold;
    reg.nonce = nonce;
    reg.supply = 20'000'000;
    reg.text = "GOLD";
    const CTransactionRef registered{make.Assets({COutPoint{reserve->GetHash(), 0}}, reg, {Token{Token::Kind::CONTROL, gold, 1}, Asset(gold, 20'000'000)}, holder)};
    const CTransactionRef pool{make.Assets({COutPoint{registered->GetHash(), 1}}, AddLiquidity{gold, CHN, 10'000'000, 1'000'000'000, 1},
                                           {Asset(gold, 10'000'000), std::nullopt, std::nullopt}, holder, 1'000'000'000)};
    BOOST_REQUIRE_EQUAL(chain.Connect({registered, pool}), "");
    chain.Check("8a1a77029452d82e0e583d76339f0fc0d8cfab2df04c0e96154a64d4e2c11310", "9a538906e6466ebd2617d321f71bc94e56056ce213d366773699e28158e00614", 0);

    // Swaps of GOLD for CHN, one after another, each paid out by the coinbase.
    std::vector<CTransactionRef> swaps;
    COutPoint coin{pool->GetHash(), 0};
    uint64_t left{10'000'000};
    for (size_t i{0}; i < MAX_TX_PAYOUTS_PER_BLOCK + 1; ++i) {
        left -= 1000;
        swaps.push_back(make.Assets({coin}, Swap{gold, 1000, CHN, 1, pay}, {Asset(gold, left)}, holder));
        coin = COutPoint{swaps.back()->GetHash(), 0};
    }
    // Refunds, 40 a transaction.
    std::vector<CTransactionRef> refund_txs;
    for (size_t from{0}; from < refunds; from += 40) {
        std::vector<CTxOut> outs;
        for (size_t i{from}; i < std::min(refunds, from + 40); ++i) outs.push_back(RefundOut(key, COutPoint{withdrawals->GetHash(), static_cast<uint32_t>(i)}));
        refund_txs.push_back(make.Outputs(outs));
    }
    BOOST_REQUIRE_EQUAL(refund_txs.size(), 3U);
    // The last transaction of refunds, but those beyond MAX_REFUNDS_PER_BLOCK.
    std::vector<CTxOut> fitting_outs{refund_txs[2]->vout.begin(), refund_txs[2]->vout.begin() + (sidechain::MAX_REFUNDS_PER_BLOCK - 80)};
    const CTransactionRef fitting{make.Outputs(fitting_outs)};
    std::vector<CTxOut> rest_outs{refund_txs[2]->vout.begin() + (sidechain::MAX_REFUNDS_PER_BLOCK - 80), refund_txs[2]->vout.end()};
    const CTransactionRef rest{make.Outputs(rest_outs)};

    // Block 3, too many refunds: not valid, and nothing changes.
    const uint256 before{chain.state.Hash()};
    BOOST_CHECK_EQUAL(chain.Connect({refund_txs[0], refund_txs[1], refund_txs[2]}), "bad-sc-block-refunds-full");
    // Too many asset payouts: the same.
    std::vector<CTransactionRef> too_many{refund_txs[0], refund_txs[1], fitting};
    too_many.insert(too_many.end(), swaps.begin(), swaps.end());
    BOOST_CHECK_EQUAL(chain.Connect(too_many), "bad-ba-block-payouts-full");
    BOOST_CHECK(chain.state.Hash() == before);

    // Block 3: as many of each as a block takes, and more deposits than half a block, all owed on top.
    std::vector<CTransactionRef> full{refund_txs[0], refund_txs[1], fitting};
    full.insert(full.end(), swaps.begin(), swaps.end() - 1);
    const auto deposits{[](sidechain::MainBlock& b) {
        for (int i{0}; i < 450; ++i) {
            sidechain::MainDeposit deposit;
            deposit.destination = "deposit " + std::to_string(i);
            deposit.amount = COIN + i;
            deposit.txid = ArithToUint256(arith_uint256{static_cast<uint64_t>(i + 1)});
            b.deposits.push_back(deposit);
        }
    }};
    BOOST_REQUIRE_EQUAL(chain.Connect(full, deposits), "");
    chain.Check("15d6c8e8ea624f6048c3de8a5c5cb735490d9557859580217b815937fbca5c3d", "bbff4bbaf5c426f3a960e71bc4651cdf43e11b218a9f668167c9eb8b5b99adcd", sidechain::MAX_PAYOUTS_PER_BLOCK);
    // Deposits and refunds first, as many as half a block; then the swaps, all of them.
    BOOST_CHECK(chain.paid[0].nValue == COIN);
    BOOST_CHECK(chain.paid[sidechain::MAX_PAYOUTS_PER_BLOCK / 2].scriptPubKey == pay);
    BOOST_CHECK_EQUAL(chain.state.Queue().size(), 450U + sidechain::MAX_REFUNDS_PER_BLOCK - sidechain::MAX_PAYOUTS_PER_BLOCK / 2);
    BOOST_CHECK(chain.state.TxQueue().empty());

    // Block 4: the other refunds, the last swap.
    BOOST_REQUIRE_EQUAL(chain.Connect({rest, swaps.back()}), "");
    chain.Check("fcac9e32323dfc5d0ba257ba57d8b0aecb1ee22263e33e4619b55858519562d1", "74ab590408a0bc5b00e338f4f13c0f3c5807b03df1d716b33a7891f46472c6e8", 450U + refunds - sidechain::MAX_PAYOUTS_PER_BLOCK / 2 + 1);
    BOOST_CHECK(chain.state.Queue().empty());

    // Block 5: GOLD dies, and is retired: the CHN its pool held go to mainchain miners, as the fee of a withdrawal.
    const Pool live{*Assert(chain.state.BitAssets().FindPool(gold, CHN))};
    const uint64_t held{live.shares - MIN_LIQUIDITY};
    const auto [out0, out1]{amm::Withdraw(live, held)};
    const bool gold_first{live.asset0 == gold};
    const uint64_t out_gold{gold_first ? out0 : out1}, out_chn{gold_first ? out1 : out0};
    const CTransactionRef removed{make.Assets({COutPoint{pool->GetHash(), 1}}, RemoveLiquidity{gold, CHN, held, out_gold, out_chn, pay}, {std::nullopt}, holder)};
    const CTransactionRef burned{make.Assets({COutPoint{registered->GetHash(), 0}, coin, COutPoint{removed->GetHash(), 0}}, Burn{{Token{Token::Kind::CONTROL, gold, 1}, Asset(gold, left + out_gold)}}, {}, holder)};
    const CTransactionRef release{make.Assets({}, ReleaseAsset{gold}, {}, holder)};
    BOOST_REQUIRE_EQUAL(chain.Connect({removed, burned, release}), "");
    chain.Check("4152056ff7af3c0823139b22aae02f53ee4c8716bd3d73f1ae87ac7e9ccad62d", "c4fa3d67a995a66f37cb16f11022efdfa5b0ebb680b6faae51fdff7dd223c84b", 1);
    BOOST_CHECK(!chain.state.BitAssets().FindPool(gold, CHN));
    const auto released{chain.state.GetWithdrawal(COutPoint{release->GetHash(), sidechain::RELEASE_WITHDRAWAL_INDEX})};
    BOOST_REQUIRE(released);
    BOOST_CHECK_EQUAL(released->amount, 1);
}

BOOST_AUTO_TEST_SUITE_END()
