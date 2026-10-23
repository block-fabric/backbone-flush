// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// FastBlocks: blocks of stake signed by the leader of their slot, anchors and their
// acknowledgements, the order of chains, the fee pot, BMM blocks as the fallback, the block
// producer and its slashing protection. A chainstate of a regtest sidechain with three validators,
// and a record of the mainchain the test writes itself.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <addresstype.h>
#include <blockencodings.h>
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <common/signmessage.h>
#include <consensus/merkle.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <interfaces/chain.h>
#include <interfaces/mining.h>
#include <kernel/chainparams.h>
#include <key.h>
#include <key_io.h>
#include <logging.h>
#include <net.h>
#include <net_processing.h>
#include <netmessagemaker.h>
#include <protocol.h>
#include <node/miner.h>
#include <pos/evidence.h>
#include <pos/feesplit.h>
#include <pos/finality.h>
#include <pos/forkchoice.h>
#include <pos/header.h>
#include <pos/hooks.h>
#include <pos/income.h>
#include <pos/producer.h>
#include <pos/protection.h>
#include <pos/schedule.h>
#include <pos/staking.h>
#include <pow.h>
#include <primitives/block.h>
#include <script/sign.h>
#include <script/solver.h>
#include <streams.h>
#include <script/signingprovider.h>
#include <sidechain/mainchain.h>
#include <sidechain/state.h>
#include <algorithm>
#include <limits>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>
#include <validationinterface.h>
#ifdef ENABLE_WALLET
#include <rpc/request.h>
#include <rpc/util.h>
#include <univalue.h>
#include <wallet/context.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>
#endif

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <thread>
#include <optional>
#include <string>
#include <vector>

namespace {

//! The time the tests start at (mock time): well after the regtest chain's first block.
constexpr int64_t START_TIME{1800000000};
constexpr CAmount TX_FEE{10000};

void Mock(int64_t time) { SetMockTime(std::chrono::seconds{time}); }

std::vector<CKey> ValidatorKeys()
{
    std::vector<CKey> keys;
    for (uint8_t i{1}; i <= 3; ++i) {
        std::vector<unsigned char> secret(32, 0);
        secret[0] = 0x11;
        secret[31] = i;
        CKey key;
        key.Set(secret.begin(), secret.end(), /*fCompressedIn=*/true);
        keys.push_back(key);
    }
    return keys;
}

uint256 XOnly(const CKey& key)
{
    const XOnlyPubKey xonly{key.GetPubKey()};
    return uint256{std::span<const unsigned char>{xonly.begin(), xonly.end()}};
}

/** Installs the FastBlocks hooks before the chainstate is made, and removes them after. */
struct InstallFastBlocks {
    std::vector<CKey> keys{ValidatorKeys()};
    std::string validators;
    std::vector<std::string> args;
    std::unique_ptr<const CChainParams> params;
    std::optional<int64_t> seed_lookahead;
    std::optional<int> fee_split_height;
    std::optional<int> fee_curve2_height;
    std::optional<int> jail_fix_height;
    explicit InstallFastBlocks(bool with_validators = true, std::optional<int64_t> lookahead = std::nullopt, std::optional<int> fee_split = std::nullopt,
                               std::optional<int> fee_curve2 = std::nullopt, std::optional<int> jail_fix = std::nullopt)
        : seed_lookahead{lookahead}, fee_split_height{fee_split}, fee_curve2_height{fee_curve2}, jail_fix_height{jail_fix}
    {
        CChainParams::RegTestOptions options;
        options.sidechain_slot = 5;
        options.pos_seed_lookahead = lookahead;
        options.dep_opts.fee_split_height = fee_split;
        options.dep_opts.fee_curve2_height = fee_curve2;
        options.dep_opts.jail_fix_height = jail_fix;
        if (with_validators) {
            ECC_Context ecc;
            for (const CKey& key : keys) {
                validators += (validators.empty() ? "" : ",") + HexStr(XOnly(key));
                options.pos_validators.push_back({.pubkey = XOnly(key), .stake = 1});
            }
        }
        params = CChainParams::RegTest(options);
        pos::InstallHooks(*params);
    }
    ~InstallFastBlocks() { pos::UninstallHooks(); }
};

/** The verdict validation gave on the blocks it checked, by hash. */
struct BlockCatcher final : public CValidationInterface {
    std::map<uint256, BlockValidationState> states;
    void BlockChecked(const std::shared_ptr<const CBlock>& block, const BlockValidationState& state) override { states[block->GetHash()] = state; }
};

std::vector<const char*> SetupArgs(InstallFastBlocks& installed)
{
    installed.args = {"-sidechainslot=5"};
    if (!installed.validators.empty()) installed.args.push_back("-posvalidators=" + installed.validators);
    if (installed.seed_lookahead) installed.args.push_back(strprintf("-posseedlookahead=%d", *installed.seed_lookahead));
    if (installed.fee_split_height) installed.args.push_back(strprintf("-testactivationheight=feesplit@%d", *installed.fee_split_height));
    if (installed.fee_curve2_height) installed.args.push_back(strprintf("-testactivationheight=feecurve2@%d", *installed.fee_curve2_height));
    if (installed.jail_fix_height) installed.args.push_back(strprintf("-testactivationheight=jailfix@%d", *installed.jail_fix_height));
    std::vector<const char*> args;
    for (const std::string& arg : installed.args) args.push_back(arg.c_str());
    return args;
}

/** A P2WPKH script of a key hash made of `b` (an address nobody holds the key of). */
static CScript Wpkh(uint8_t b)
{
    uint160 hash;
    std::fill(hash.begin(), hash.end(), b);
    return GetScriptForDestination(WitnessV0KeyHash{hash});
}

struct FastBlocksSetup : public InstallFastBlocks, public TestingSetup {
    CKey key;
    CScript script;
    std::shared_ptr<BlockCatcher> catcher{std::make_shared<BlockCatcher>()};
    std::unique_ptr<pos::Producer> producer;
    int64_t base_slot{0};

    explicit FastBlocksSetup(bool with_validators = true, std::optional<int64_t> lookahead = std::nullopt, std::optional<int> fee_split = std::nullopt,
                             std::optional<int> fee_curve2 = std::nullopt, std::optional<int> jail_fix = std::nullopt)
        : InstallFastBlocks{with_validators, lookahead, fee_split, fee_curve2, jail_fix}, TestingSetup{ChainType::REGTEST, TestOpts{.extra_args = SetupArgs(*this)}}
    {
        Hooks().Attach(m_node.chainman.get());
        Mock(START_TIME);
        base_slot = pos::SlotOf(START_TIME, Pos());
        key.MakeNewKey(/*fCompressed=*/true);
        script = GetScriptForDestination(WitnessV0KeyHash{key.GetPubKey()});
        m_node.validation_signals->RegisterSharedValidationInterface(catcher);
        std::string error;
        auto protection{pos::SlashingProtection::Open(m_args.GetDataDirNet() / "validator_protection.dat", m_args.GetDataDirNet(), "keys.lock", error)};
        BOOST_REQUIRE_MESSAGE(protection, error);
        producer = std::make_unique<pos::Producer>(m_node, Hooks(), keys, script, std::move(protection));
        // Peers 0 to 15 are connected: they may hold unchecked headers (ChainstateManager::m_unchecked).
        {
            LOCK(::cs_main);
            for (int64_t peer{0}; peer < 16; ++peer) m_node.chainman->UncheckedSourceAdded(peer);
        }
        // The first block of the mainchain, which no sidechain acts on.
        Main();
    }
    ~FastBlocksSetup()
    {
        producer.reset();
        m_node.validation_signals->UnregisterSharedValidationInterface(catcher);
        SetMockTime(std::chrono::seconds{0});
    }

    pos::FastBlocksHooks& Hooks() { return *Assert(pos::InstalledHooks()); }
    const Consensus::PosParams& Pos() { return Assert(pos::InstalledHooks())->Params(); }
    ChainstateManager& Chainman() { return *Assert(m_node.chainman); }
    Chainstate& Chain() { return Chainman().ActiveChainstate(); }
    sidechain::Mainchain& Record() { return *Assert(Chainman().m_mainchain); }
    const CBlockIndex* Tip() { return WITH_LOCK(::cs_main, return Chain().m_chain.Tip()); }
    pos::FastState Fast() { return WITH_LOCK(::cs_main, return pos::GetFastState(Chain().SideState().View())); }
    pos::ChainKey Key() { return pos::ChainKey::FromWork(Tip()->nChainWork); }

    /** Append a block to the record of the mainchain, at the current time. */
    void Main(const std::function<void(sidechain::MainBlock&)>& fill = {})
    {
        sidechain::MainBlock block;
        const int height{Record().Height() + 1};
        block.hash = ArithToUint256(arith_uint256{static_cast<uint64_t>(0x20000 + height)} << 128);
        block.prev_hash = Record().TipHash();
        block.time = GetTime();
        if (fill) fill(block);
        BOOST_REQUIRE(Record().Append(block));
    }
    /** A mainchain block that commits to `hash`. */
    void Commit(const uint256& hash) { Main([&](sidechain::MainBlock& b) { b.bmm = hash; }); }

    /** Move the clock to slot base_slot + i, and return it. */
    int64_t Slot(int i)
    {
        const int64_t slot{base_slot + i};
        Mock(pos::SlotTime(slot, Pos()));
        return slot;
    }
    const CKey& Leader(int64_t slot)
    {
        const auto leader{Hooks().ActiveLeaderOf(slot)};
        BOOST_REQUIRE(leader.status == pos::FastBlocksHooks::LeaderStatus::OK);
        const uint256 pubkey{leader.pubkey};
        for (const CKey& k : keys) {
            if (XOnly(k) == pubkey) return k;
        }
        BOOST_FAIL("no key leads the slot");
        return keys[0];
    }

    /** The PoS block of `slot` on the tip, signed by its leader (the clock is moved to the slot). */
    CBlock PosBlock(int i, const std::optional<pos::AnchorAck>& ack = std::nullopt)
    {
        const int64_t slot{Slot(i)};
        std::string error;
        auto block{producer->Build(slot, ack, error)};
        BOOST_REQUIRE_MESSAGE(block, error);
        BOOST_REQUIRE(pos::Producer::Sign(*block, Leader(slot), Genesis()));
        return *block;
    }
    /** After a change: the sections hash, the merkle root, and the leader's signature again. */
    void Resign(CBlock& block, const CKey* signer = nullptr)
    {
        block.ext.hashSections = block.vSections.empty() ? uint256{} : pos::SectionsHash(block.vSections);
        node::RegenerateCommitments(block, Chainman());
        BOOST_REQUIRE(pos::Producer::Sign(block, signer ? *signer : Leader(pos::SlotOf(block.nTime, Pos())), Genesis()));
    }
    /** A block with an acknowledgement put in by hand (as no producer would make it). */
    CBlock WithAck(CBlock block, const pos::AnchorAck& ack)
    {
        pos::PosSections sections;
        sections.ack = ack;
        block.vSections = sections.Encode();
        block.nNonce = static_cast<uint32_t>(ack.main_height);
        Resign(block);
        return block;
    }
    /** Replace the coinbase's outputs. */
    void SetCoinbase(CBlock& block, const std::function<void(CMutableTransaction&)>& change)
    {
        CMutableTransaction coinbase{*block.vtx[0]};
        change(coinbase);
        block.vtx[0] = MakeTransactionRef(std::move(coinbase));
    }

    /** What validation says of a block on the tip, without taking it. */
    std::string Check(const CBlock& block)
    {
        LOCK(::cs_main);
        const BlockValidationState state{TestBlockValidity(Chain(), block, /*check_pow=*/false, /*check_merkle_root=*/true)};
        return state.IsValid() ? std::string{} : state.GetRejectReason();
    }
    void Submit(const CBlock& block)
    {
        Chainman().ProcessNewBlock(std::make_shared<const CBlock>(block), /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/nullptr);
        m_node.validation_signals->SyncWithValidationInterfaceQueue();
    }
    /** Take a valid block: it becomes the tip. */
    void Take(const CBlock& block)
    {
        BOOST_REQUIRE_EQUAL(Check(block), "");
        Submit(block);
        BOOST_REQUIRE(Tip()->GetBlockHash() == block.GetHash());
    }
    std::string Reason(const CBlock& block)
    {
        const auto it{catcher->states.find(block.GetHash())};
        if (it == catcher->states.end() || it->second.IsValid()) return {};
        return it->second.GetRejectReason();
    }

    /** The payee of the BMM blocks of the tests: an address (IsRewardPayee), which takes the pot. */
    static CScript BmmPayee() { return Wpkh(0x42); }
    /** A BMM block on the tip, not committed yet. */
    CBlock BmmBlock(bool use_mempool = true, const CScript& coinbase_script = BmmPayee())
    {
        auto block_template{interfaces::MakeMining(m_node)->createNewBlock({.use_mempool = use_mempool, .coinbase_output_script = coinbase_script, .test_block_validity = false}, /*cooldown=*/false)};
        BOOST_REQUIRE(block_template);
        CBlock block{block_template->getBlock()};
        FinishBmm(block);
        return block;
    }
    void FinishBmm(CBlock& block)
    {
        block.hashMerkleRoot = BlockMerkleRoot(block);
        while (!CheckProofOfWork(block.GetHash(), block.nBits, Chainman().GetConsensus())) ++block.nNonce;
    }
    /** Have the mainchain commit to a BMM block and take it. */
    void MineBmm(const CBlock& block)
    {
        Commit(block.GetHash());
        Submit(block);
        BOOST_REQUIRE_MESSAGE(Tip()->GetBlockHash() == block.GetHash(), Reason(block));
    }

    /** A coin of `amount` for the key, from a deposit paid by a BMM block. */
    CTxOut Fund(CAmount amount, COutPoint& coin)
    {
        Main([&](sidechain::MainBlock& block) {
            sidechain::MainDeposit deposit;
            deposit.destination = EncodeDestination(WitnessV0KeyHash{key.GetPubKey()});
            deposit.amount = amount;
            deposit.txid = uint256{static_cast<uint8_t>(Record().Height() + 1)};
            block.deposits.push_back(deposit);
        });
        const CBlock block{BmmBlock(/*use_mempool=*/false)};
        MineBmm(block);
        const CTransaction& coinbase{*block.vtx[0]};
        BOOST_REQUIRE(coinbase.vout.size() >= 2);
        BOOST_REQUIRE(coinbase.vout[1].scriptPubKey == script);
        coin = COutPoint{coinbase.GetHash(), 1};
        return coinbase.vout[1];
    }
    CMutableTransaction Spend(const COutPoint& prevout, const CTxOut& spent, std::vector<CTxOut> outputs)
    {
        CMutableTransaction tx;
        tx.vin.emplace_back(prevout);
        tx.vout = std::move(outputs);
        FillableSigningProvider keystore;
        BOOST_REQUIRE(keystore.AddKey(key));
        std::map<COutPoint, Coin> coins{{prevout, Coin{spent, 1, /*fCoinBaseIn=*/false}}};
        std::map<int, bilingual_str> errors;
        BOOST_REQUIRE(SignTransaction(tx, &keystore, coins, SignOptions{.sighash_type = SIGHASH_ALL}, errors));
        return tx;
    }
    void Accept(const CMutableTransaction& tx)
    {
        const auto result{WITH_LOCK(::cs_main, return Chainman().ProcessTransaction(MakeTransactionRef(tx)))};
        BOOST_REQUIRE_MESSAGE(result.m_result_type == MempoolAcceptResult::ResultType::VALID, result.m_state.ToString());
    }

    /** A ticket for the block `anchored`, paying the pot to `payee`. */
    pos::AnchorTicket Ticket(const uint256& anchored, const CScript& payee)
    {
        pos::AnchorTicket ticket;
        ticket.anchored = anchored;
        ticket.payee = payee;
        return ticket;
    }
    /** Have the mainchain commit to a ticket; the acknowledgement of it. */
    pos::AnchorAck Anchor(const pos::AnchorTicket& ticket)
    {
        Commit(ticket.Hash());
        Hooks().AddTicket(ticket);
        return pos::AnchorAck{.ticket = ticket, .main_height = Record().Height()};
    }
    /** Start PoS: a first block of stake after the BMM blocks of the start. */
    void StartPos()
    {
        // A BMM block first, so that the chain has an anchor point.
        MineBmm(BmmBlock(/*use_mempool=*/false));
    }

    //
    // Staking
    //

    uint256 Genesis() { return Chainman().GetConsensus().hashGenesisBlock; }
    /** The state of the active chain (the store of the chainstate, which lives as long as it). */
    const sidechain::StoreView& ActiveView()
    {
        LOCK(::cs_main);
        return Chain().SideCache();
    }
    uint160 Owner(const CKey& owner) { return uint160{owner.GetPubKey().GetID()}; }
    pos::StakeState Stake() { return WITH_LOCK(::cs_main, return pos::GetStakeState(Chain().SideState().View())); }
    std::optional<pos::ValidatorRecord> Validator(const uint256& pubkey) { return WITH_LOCK(::cs_main, return pos::GetValidator(Chain().SideState().View(), pubkey)); }
    /**
     * A bond output: `validator` signs the proof of possession for `owner`; `owner_signer`, if given,
     * signs the bond for a transaction that spends `spends` first (a bond to a validator with an owner).
     */
    CTxOut BondOut(const CKey& validator, CAmount amount, const uint160& owner, const CKey* owner_signer = nullptr, const COutPoint& spends = TxWithCoin())
    {
        std::array<unsigned char, 64> pop;
        BOOST_REQUIRE(validator.SignSchnorr(pos::BondPopHash(owner, Genesis()), pop, nullptr, uint256{}));
        std::vector<unsigned char> owner_sig;
        if (owner_signer) BOOST_REQUIRE(owner_signer->SignCompact(MessageHash(pos::BondMessage(XOnly(validator), amount, spends, Genesis())), owner_sig));
        return CTxOut{amount, pos::BondScript(XOnly(validator), owner, pop, owner_sig)};
    }
    /** An unbond output, signed by `owner`. */
    CTxOut UnbondOut(const CKey& validator, CAmount amount, uint32_t sequence, const CKey& owner)
    {
        pos::Unbond unbond;
        unbond.pubkey = XOnly(validator);
        unbond.amount = amount;
        unbond.sequence = sequence;
        // (Naming the first coin its first bond spent, as the state has it.)
        const auto record{Validator(unbond.pubkey)};
        BOOST_REQUIRE(owner.SignCompact(MessageHash(pos::UnbondMessage(unbond.pubkey, amount, sequence, record ? record->first_bond : COutPoint{}, record ? record->first_bond_anchor : uint256{}, Genesis())), unbond.signature));
        return CTxOut{0, pos::UnbondScript(unbond)};
    }
    /** A transaction with these outputs, spending the coin, its change to the key. */
    CMutableTransaction Pay(COutPoint& coin, CTxOut& spent, std::vector<CTxOut> outputs)
    {
        CAmount out{0};
        for (const CTxOut& o : outputs) out += o.nValue;
        outputs.emplace_back(spent.nValue - out - TX_FEE, script);
        const CMutableTransaction tx{Spend(coin, spent, outputs)};
        coin = COutPoint{tx.GetHash(), static_cast<uint32_t>(outputs.size() - 1)};
        spent = outputs.back();
        return tx;
    }
    /** Why the mempool turns a transaction away. */
    std::string Refused(const CMutableTransaction& tx)
    {
        const auto result{WITH_LOCK(::cs_main, return Chainman().ProcessTransaction(MakeTransactionRef(tx), /*test_accept=*/true))};
        return result.m_result_type == MempoolAcceptResult::ResultType::VALID ? std::string{} : result.m_state.GetRejectReason();
    }
    /** The staking rules on a transaction, on an overlay of the tip's state that is dropped. */
    std::string StakingRules(const CTransaction& tx)
    {
        LOCK(::cs_main);
        sidechain::StoreOverlay overlay{Chain().SideState().View()};
        std::string reason;
        if (Hooks().GetStaking().ApplyTx(overlay, tx, Tip()->nHeight + 1, Fast().anchors, reason, Fast().anchor_point)) return {};
        return reason;
    }
    /** The coin TxWith spends. */
    static COutPoint TxWithCoin() { return COutPoint{Txid::FromUint256(uint256{0x77}), 0}; }
    CTransaction TxWith(std::vector<CTxOut> outputs)
    {
        CMutableTransaction tx;
        tx.vin.emplace_back(TxWithCoin());
        tx.vout = std::move(outputs);
        return CTransaction{tx};
    }
    /** Two blocks of stake of slot base_slot + i on the tip, both signed by its leader. */
    std::pair<CBlock, CBlock> TwoBlocks(int i)
    {
        const CBlock first{PosBlock(i)};
        CBlock second{first};
        SetCoinbase(second, [](CMutableTransaction& tx) { tx.vout.emplace_back(0, CScript() << OP_RETURN); });
        Resign(second);
        return {first, second};
    }
    /** What AfterBlock says of a block on the tip, on a state over `base`. */
    std::string After(const CBlock& block, const sidechain::StoreView& base)
    {
        sidechain::StoreOverlay overlay{base};
        sidechain::State state{overlay};
        std::string reason;
        LOCK(::cs_main);
        if (Hooks().AfterBlock(state, block, Tip()->nHeight + 1, std::nullopt, 0, Chainman().GetConsensus().sidechain, reason)) return {};
        return reason;
    }
    const CKey& NotLeader(int64_t slot)
    {
        for (const CKey& k : keys) {
            if (XOnly(k) != XOnly(Leader(slot))) return k;
        }
        BOOST_FAIL("every key leads the slot");
        return keys[0];
    }
    /** The block of stake of slot base_slot + i carrying these proofs. */
    CBlock WithEvidence(int i, const std::vector<pos::DoubleProposal>& evidence, const std::optional<pos::AnchorAck>& ack = std::nullopt)
    {
        const int64_t slot{Slot(i)};
        std::string error;
        auto block{producer->Build(slot, ack, error, evidence)};
        BOOST_REQUIRE_MESSAGE(block, error);
        BOOST_REQUIRE(pos::Producer::Sign(*block, Leader(slot), Genesis()));
        return *block;
    }
    /** Whether a coinbase pays this output. */
    static bool Pays(const CBlock& block, const CTxOut& out)
    {
        const auto& vout{block.vtx.at(0)->vout};
        return std::find(vout.begin(), vout.end(), out) != vout.end();
    }

    /** Headers as a peer sends them: why they are turned away (nothing if they are taken). */
    //! Why the node refuses these headers from a peer (`source`), if it does.
    std::string HeaderReason(const std::vector<CBlockHeader>& headers, int64_t source = 0)
    {
        BlockValidationState state;
        if (Chainman().ProcessNewBlockHeaders(headers, /*min_pow_checked=*/true, state, nullptr, source)) return {};
        return state.GetRejectReason();
    }
    uint256 BestHeader() { return WITH_LOCK(::cs_main, return Chainman().m_best_header->GetBlockHash()); }
    CBlockIndex* Index(const uint256& hash) { return WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(hash)); }
    size_t Waiting() { return WITH_LOCK(::cs_main, return Chainman().m_bmm_waiting.size()); }
    /**
     * The header of a block of stake on `prev` at `slot`, acknowledging the commitment at `nonce` (0:
     * none), that nobody signed: what anyone can make. `like`: a header of a block of stake to take
     * the version from.
     */
    CBlockHeader UnsignedHeader(const CBlockHeader& like, const uint256& prev, int64_t slot, uint32_t nonce, uint8_t salt = 0)
    {
        CBlockHeader header{like};
        header.hashPrevBlock = prev;
        header.hashMerkleRoot = uint256{salt};
        header.nTime = static_cast<uint32_t>(pos::SlotTime(slot, Pos()));
        header.nNonce = nonce;
        header.ext.hashSections = nonce ? uint256{0xa0} : uint256{};
        header.ext.sig.fill(0);
        return header;
    }
    /** Acknowledge a ticket for the tip in a block of stake of slot base_slot + i, taken: an anchor. */
    pos::AnchorAck AckTip(int i)
    {
        const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
        Take(PosBlock(i, ack));
        return ack;
    }

    //! Tickets of the floods, all different.
    uint64_t flood_salt{0};
    /**
     * Push every acknowledgement noted in memory out (attack8): MAX_NOTED_ACKS + 1 blocks of slot
     * base_slot + i on the tip, signed by its leader, each acknowledging a commitment nobody made.
     * @return each block's commitment and hash, in the order noted (the first is out already)
     */
    std::vector<std::pair<uint256, uint256>> FloodNotedAcks(int i)
    {
        CBlock block{PosBlock(i)};
        const CKey& leader{Leader(pos::SlotOf(block.nTime, Pos()))};
        std::vector<std::pair<uint256, uint256>> noted;
        for (size_t n{0}; n <= pos::MAX_NOTED_ACKS; ++n) {
            pos::PosSections sections;
            sections.ack = pos::AnchorAck{.ticket = Ticket(ArithToUint256(arith_uint256{++flood_salt} << 64), script), .main_height = 1000000 + static_cast<int>(n)};
            block.vSections = sections.Encode();
            block.nNonce = static_cast<uint32_t>(sections.ack->main_height);
            block.ext.hashSections = pos::SectionsHash(block.vSections);
            BOOST_REQUIRE(pos::Producer::Sign(block, leader, Genesis()));
            BlockValidationState state;
            BOOST_REQUIRE(Hooks().CheckBlock(block, state));
            noted.emplace_back(sections.ack->ticket.Hash(), block.GetHash());
        }
        return noted;
    }
    /** How many of a flood's blocks are out of memory again: as many acknowledgements were read since. */
    size_t FloodPushedOut(const std::vector<std::pair<uint256, uint256>>& flood)
    {
        size_t out{0};
        // (The first went with the flood itself.)
        for (size_t n{1}; n < flood.size(); ++n, ++out) {
            const auto noted{Hooks().BlocksCommittedBy(flood[n].first, std::nullopt)};
            if (std::find(noted.begin(), noted.end(), flood[n].second) != noted.end()) break;
        }
        return out;
    }
};

struct NoValidatorsSetup : public FastBlocksSetup {
    NoValidatorsSetup() : FastBlocksSetup{/*with_validators=*/false} {}
};

/** The genesis validators, and the seed of an epoch taken at its start (no lookahead). */
struct SeedSetup : public FastBlocksSetup {
    SeedSetup() : FastBlocksSetup{/*with_validators=*/true, /*lookahead=*/0} {}
};

/** The seed of an epoch taken at its start (no lookahead), and the liveness fix from the chain's block `height` on. */
struct JailFixSetup : public FastBlocksSetup {
    explicit JailFixSetup(int height = 0) : FastBlocksSetup{/*with_validators=*/true, /*lookahead=*/0, std::nullopt, std::nullopt, height} {}
    /** The slots the leaders were charged with and the blocks they made, in the epochs of the test (from base_slot's). */
    std::pair<uint32_t, uint32_t> Counted()
    {
        uint32_t assigned{0}, produced{0};
        LOCK(::cs_main);
        const int64_t first{pos::EpochOf(base_slot, Pos())};
        for (int64_t e{first}; e <= first + 5; ++e) {
            for (const CKey& k : keys) {
                if (const auto liveness{pos::GetLiveness(Chain().SideState().View(), e, XOnly(k))}) {
                    assigned += liveness->assigned;
                    produced += liveness->produced;
                }
            }
        }
        return {assigned, produced};
    }
    /** The first block of stake three epochs after the BMM block the chain starts with (as on betanet slot 99: hours after the origin). */
    void LateFirstBlock()
    {
        StartPos();
        const int late{static_cast<int>(3 * Pos().slots_per_epoch)};
        Slot(late - 1);
        Main();
        Take(PosBlock(late));
    }
};
struct JailFixLaterSetup : public JailFixSetup {
    JailFixLaterSetup() : JailFixSetup{1000} {}
};

/**
 * The fee split from the chain's block `height` on (by default from the first), the seed of an epoch
 * taken at its start (no lookahead), and a record of the mainchain whose spacing the test sets.
 */
struct FeeSplitSetup : public FastBlocksSetup {
    explicit FeeSplitSetup(int height = 0, std::optional<int> curve2_height = std::nullopt)
        : FastBlocksSetup{/*with_validators=*/true, /*lookahead=*/0, height, curve2_height} {}
    /** `count` mainchain blocks `spacing` seconds apart, from now on. */
    void Spaced(int64_t spacing, int count)
    {
        for (int n{0}; n < count; ++n) {
            Mock(GetTime() + spacing);
            Main();
        }
    }
    /**
     * The clock just after the start of the next epoch, and a BMM block there, committed to by a
     * mainchain block after the epoch's start: the seed of the epoch is the last mainchain block before
     * (whose spacing the blocks of stake of the epoch take). Returns the first slot of the epoch, as i
     * of Slot(i).
     */
    int NextEpochWithBmm()
    {
        const int64_t epoch{pos::EpochOf(pos::SlotOf(GetTime(), Pos()), Pos()) + 1};
        const int64_t first{epoch * Pos().slots_per_epoch};
        Mock(pos::SlotTime(first, Pos()) + 1);
        StartPos();
        return static_cast<int>(first - base_slot);
    }
    CAmount PoolOf(int64_t epoch)
    {
        CAmount amount{0};
        for (const auto& [at, value] : WITH_LOCK(::cs_main, return pos::PoolEntries(Chain().SideState().View()))) {
            if (at == epoch) amount = value;
        }
        return amount;
    }
    static CAmount OutputsOf(const CTransaction& coinbase, bool (*is)(const CScript&))
    {
        CAmount total{0};
        for (const CTxOut& out : coinbase.vout) {
            if (is(out.scriptPubKey)) total += out.nValue;
        }
        return total;
    }
};

/** The fee split from the chain's fourth block on. */
struct FeeSplitLaterSetup : public FeeSplitSetup {
    FeeSplitLaterSetup() : FeeSplitSetup{/*height=*/4} {}
};

/** The fee split from the first block, the second fee curve from the chain's block FEE_CURVE2_TEST_HEIGHT on. */
constexpr int FEE_CURVE2_TEST_HEIGHT{6};
struct FeeCurve2LaterSetup : public FeeSplitSetup {
    FeeCurve2LaterSetup() : FeeSplitSetup{/*height=*/0, /*curve2_height=*/FEE_CURVE2_TEST_HEIGHT} {}
};

/** Wait (real time) until `done`, at most `seconds`; whether it came. */
bool WaitFor(const std::function<bool()>& done, int seconds = 20)
{
    const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{seconds}};
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return true;
}

} // namespace

#ifdef ENABLE_WALLET
namespace wallet {
RPCMethod getbalances();
RPCMethod gettransaction();
} // namespace wallet
#endif

BOOST_AUTO_TEST_SUITE(fastblocks_tests)

BOOST_AUTO_TEST_CASE(formats)
{
    // Sections: nothing is no bytes; an acknowledgement reads back; anything else does not read.
    pos::PosSections sections;
    BOOST_CHECK(sections.Encode().empty());
    pos::PosSections read;
    BOOST_CHECK(pos::PosSections::Decode({}, read) && !read.ack);
    sections.ack = pos::AnchorAck{.ticket = pos::AnchorTicket{.version = 1, .anchored = uint256{7}, .payee = CScript() << OP_TRUE}, .main_height = 42};
    const auto bytes{sections.Encode()};
    BOOST_CHECK(pos::PosSections::Decode(bytes, read));
    BOOST_CHECK(read == sections);
    auto longer{bytes};
    longer.push_back(0);
    BOOST_CHECK(!pos::PosSections::Decode(longer, read));
    auto evidence{bytes};
    evidence.back() = 1;
    BOOST_CHECK(!pos::PosSections::Decode(evidence, read));
    BOOST_CHECK(!pos::PosSections::Decode(std::vector<unsigned char>{0}, read));
    BOOST_CHECK(!pos::PosSections::Decode(std::vector<unsigned char>{1, 2}, read));

    // A ticket's hash is a tagged hash of the ticket, and changes with each part of it.
    const pos::AnchorTicket ticket{sections.ack->ticket};
    pos::AnchorTicket other{ticket};
    other.payee = CScript() << OP_2;
    BOOST_CHECK(ticket.Hash() != other.Hash());
    other = ticket;
    other.anchored = uint256{8};
    BOOST_CHECK(ticket.Hash() != other.Hash());

    // Payees: standard scripts of 80 bytes or less, no data.
    BOOST_CHECK(pos::IsValidPayee(GetScriptForDestination(WitnessV0KeyHash{uint160{}})));
    BOOST_CHECK(!pos::IsValidPayee(CScript() << OP_RETURN));
    BOOST_CHECK(!pos::IsValidPayee(CScript() << OP_TRUE << OP_DROP));
    BOOST_CHECK(!pos::IsValidPayee(CScript() << std::vector<unsigned char>(79, 1) << OP_CHECKSIG));
    // What a reward for a proof, or a BMM block's pot, may be paid to (audit 7, finding 1): the address
    // forms only. Not the pot's script, a proof's, data, bare multisig or keys, nor anything else.
    {
        ECC_Context ecc;
        CKey k;
        k.MakeNewKey(/*fCompressed=*/true);
        for (const CTxDestination& dest : std::vector<CTxDestination>{PKHash{k.GetPubKey()}, ScriptHash{CScript() << OP_TRUE}, WitnessV0KeyHash{k.GetPubKey()},
                                                                        WitnessV0ScriptHash{CScript() << OP_TRUE}, WitnessV1Taproot{XOnlyPubKey{k.GetPubKey()}}}) {
            BOOST_CHECK(pos::IsRewardPayee(GetScriptForDestination(dest)));
        }
        CScript sigops;
        for (int n{0}; n < 1000; ++n) sigops << OP_CHECKMULTISIG;
        for (const CScript& bad : {pos::PotScript(), CScript() << OP_RETURN, CScript() << OP_TRUE, sigops, GetScriptForRawPubKey(k.GetPubKey()),
                                   GetScriptForMultisig(1, {k.GetPubKey()}), CScript() << OP_2 << std::vector<unsigned char>(32, 1), CScript{}}) {
            BOOST_CHECK(!pos::IsRewardPayee(bad));
        }
    }

    // The producer's share, in integers, without overflow.
    BOOST_CHECK_EQUAL(pos::Producer::ProducerShare(10001, 5000), 5000);
    BOOST_CHECK_EQUAL(pos::Producer::ProducerShare(MAX_MONEY, 5000), MAX_MONEY / 2);
    BOOST_CHECK_EQUAL(pos::Producer::ProducerShare(MAX_MONEY, 10000), MAX_MONEY);
    BOOST_CHECK_EQUAL(pos::Producer::ProducerShare(3, 3333), 0);
}

BOOST_AUTO_TEST_CASE(chain_key)
{
    // The order of chains: anchors first, then the earlier last commitment, then the height.
    const pos::ChainKey key{.anchors = 5, .last_anchor_height = 1000, .height = 77};
    BOOST_CHECK(pos::ChainKey::FromWork(key.Work()) == key);
    const pos::ChainKey more_anchors{.anchors = 6, .last_anchor_height = 0xFFFFFFFF, .height = 1};
    const pos::ChainKey earlier{.anchors = 5, .last_anchor_height = 999, .height = 1};
    const pos::ChainKey longer{.anchors = 5, .last_anchor_height = 1000, .height = 78};
    BOOST_CHECK(more_anchors.Work() > key.Work());
    BOOST_CHECK(earlier.Work() > key.Work());
    BOOST_CHECK(longer.Work() > key.Work());
    BOOST_CHECK(more_anchors.Work() > earlier.Work());
    BOOST_CHECK(earlier.Work() > longer.Work());
}

BOOST_AUTO_TEST_CASE(network_parameters)
{
    // The user's decisions (design, section 13; 15-second slots from 2026-10-16, 30 before): blocks of
    // stake halt after an hour without anchors; BMM blocks take transactions after 30 minutes without
    // blocks of stake (10 on the test networks), which is sooner, so that the chain goes on in BMM
    // blocks. Counts of slots keep the durations they had with 30-second slots.
    for (const auto& params : {CChainParams::Main(), CChainParams::TestNet(), CChainParams::SigNet({})}) {
        const Consensus::PosParams& pos{params->GetConsensus().pos};
        BOOST_CHECK_EQUAL(pos.slot_seconds, 15);
        BOOST_CHECK_EQUAL(pos.max_unanchored_slots, 240);
        BOOST_CHECK_EQUAL(pos.slot_seconds * pos.max_unanchored_slots, 60 * 60);
        // Jailed for leading under half of five minutes of slots or more in an epoch.
        BOOST_CHECK_EQUAL(pos.liveness_min_assigned, 20);
        BOOST_CHECK_EQUAL(pos.slot_seconds * pos.liveness_min_assigned, 5 * 60);
        BOOST_CHECK_EQUAL(pos.max_future_seconds, 2);
        BOOST_CHECK_EQUAL(params->GetConsensus().nPowTargetSpacing, 15);
        BOOST_CHECK_LT(pos.stall_seconds, pos.slot_seconds * pos.max_unanchored_slots);
        BOOST_CHECK_EQUAL(pos.producer_share_bps, 5000);
        // Bitcoin's heuristics count a block a slot; version bits periods stay 2016 blocks.
        BOOST_CHECK_EQUAL(params->GetConsensus().nPowTargetSpacing, pos.slot_seconds);
        BOOST_CHECK_EQUAL(params->GetConsensus().DifficultyAdjustmentInterval(), 2016);
        // Counts of blocks keep their durations (audit 7, finding 4): the drivechain undo data, which
        // bounds the deepest reorg, covers a day of slots; a pruned node keeps those blocks (and
        // reorgs need them), and as many full blocks, with their undo data (some 15 % more), fit the
        // least prune target.
        BOOST_CHECK_GE(int64_t{DRIVECHAIN_UNDO_DEPTH} * pos.slot_seconds, 24 * 60 * 60);
        BOOST_CHECK_EQUAL(MinBlocksToKeep(params->GetConsensus()), DRIVECHAIN_UNDO_DEPTH);
        BOOST_CHECK_LE(int64_t{DRIVECHAIN_UNDO_DEPTH} * params->GetConsensus().max_block_weight * 115 / 100, static_cast<int64_t>(MinPruneTarget(params->GetConsensus())));
    }
    BOOST_CHECK_EQUAL(CChainParams::Main()->GetConsensus().pos.stall_seconds, 30 * 60);
    // Unchecked headers (review of audit 6, item 10): the worst case node-wide. Outbound and manual
    // peers have the whole budget: 19 at most (CConnman's semOutbound and semAddnode); inbound ones a
    // much smaller one, inbound_holders of them at most. Under 1 GiB of blocks on disk, and a few
    // hundred MB of memory (1 KiB an unchecked header, with room to spare: doc/fastblocks.md).
    for (const auto& params : {CChainParams::Main(), CChainParams::TestNet()}) {
        const Consensus::PosParams& pos{params->GetConsensus().pos};
        const int64_t outbound{MAX_OUTBOUND_FULL_RELAY_CONNECTIONS + MAX_BLOCK_RELAY_ONLY_CONNECTIONS + MAX_FEELER_CONNECTIONS + MAX_ADDNODE_CONNECTIONS};
        BOOST_CHECK_EQUAL(outbound, 19);
        const int64_t headers{outbound * pos.unchecked_headers_per_peer + pos.unchecked_inbound_holders * pos.unchecked_headers_per_inbound_peer};
        const int64_t bytes{outbound * pos.unchecked_block_bytes_per_peer + pos.unchecked_inbound_holders * pos.unchecked_block_bytes_per_inbound_peer};
        BOOST_CHECK_EQUAL(headers, 194'000);
        BOOST_CHECK_EQUAL(bytes, int64_t{672} << 20);
        BOOST_CHECK_LT(bytes, int64_t{1} << 30);
        BOOST_CHECK_LT(headers * 1024, int64_t{200} << 20);
        BOOST_CHECK_LE(pos.unchecked_headers_per_inbound_peer * 10, pos.unchecked_headers_per_peer);
        // An inbound peer can still give a block of the largest size.
        BOOST_CHECK_GE(pos.unchecked_block_bytes_per_inbound_peer, static_cast<int64_t>(params->GetConsensus().max_block_weight));
    }
    BOOST_CHECK_EQUAL(Consensus::PosParams{}.slot_seconds * Consensus::PosParams{}.max_unanchored_slots, 60 * 60);
    // Regtest: one-second slots, stalls of 30 slots; -posslotseconds (a soak test) sets the slot.
    CChainParams::RegTestOptions options;
    options.sidechain_slot = 5;
    BOOST_CHECK_EQUAL(CChainParams::RegTest(options)->GetConsensus().pos.slot_seconds, 1);
    BOOST_CHECK_EQUAL(CChainParams::RegTest(options)->GetConsensus().pos.stall_seconds, 30);
    options.pos_slot_seconds = 30;
    BOOST_CHECK_EQUAL(CChainParams::RegTest(options)->GetConsensus().pos.slot_seconds, 30);
    BOOST_CHECK_EQUAL(CChainParams::RegTest(options)->GetConsensus().pos.stall_seconds, 30 * 30);
}

BOOST_AUTO_TEST_CASE(leader_schedule)
{
    // Leaders are picked by stake, the same way by everyone; a larger stake leads more slots.
    Consensus::PosParams params;
    params.slots_per_epoch = 20;
    params.min_active_validators = 2;
    params.max_validators = 3;
    params.genesis_validators = {{.pubkey = uint256{3}, .stake = 1}, {.pubkey = uint256{1}, .stake = 8}, {.pubkey = uint256{2}, .stake = 1},
                                 {.pubkey = uint256{4}, .stake = 1}, {.pubkey = uint256{1}, .stake = 99}};
    const pos::ValidatorSet set{params};
    // A key given twice counts once (its first stake); the lowest stake, highest key, is left out.
    BOOST_REQUIRE_EQUAL(set.Validators().size(), 3U);
    BOOST_CHECK(set.Validators()[0].pubkey == uint256{1});
    BOOST_CHECK_EQUAL(set.Validators()[0].stake, 8);
    BOOST_CHECK(!set.Find(uint256{4}));
    BOOST_CHECK_EQUAL(set.TotalStake(), 10);
    BOOST_CHECK(set.Active());
    std::vector<int> led(3, 0);
    for (int64_t slot{0}; slot < 2000; ++slot) {
        const auto leader{set.Leader(slot, pos::ValidatorSet::Seed(pos::EpochOf(slot, params), uint256{9}))};
        BOOST_REQUIRE(leader);
        BOOST_CHECK(leader == set.Leader(slot, pos::ValidatorSet::Seed(pos::EpochOf(slot, params), uint256{9})));
        ++led[*leader];
    }
    BOOST_CHECK_GT(led[0], 1400);
    BOOST_CHECK_GT(led[1], 100);
    BOOST_CHECK_GT(led[2], 100);
    // Too few validators: no leader.
    params.min_active_validators = 4;
    BOOST_CHECK(!pos::ValidatorSet{params}.Active());
    BOOST_CHECK(!pos::ValidatorSet{params}.Leader(1, uint256{9}));
}

BOOST_AUTO_TEST_CASE(slashing_protection)
{
    const fs::path dir{fs::path{fs::temp_directory_path()} / fs::PathFromString(strprintf("fastblocks_protection_%d", static_cast<uint64_t>(GetRandHash().GetUint64(0))))};
    fs::create_directories(dir);
    const fs::path file{dir / "protection.dat"};
    std::string error;
    {
        auto protection{pos::SlashingProtection::Open(file, dir, "keys.lock", error)};
        BOOST_REQUIRE_MESSAGE(protection, error);
        // A second one with the same keys is refused while the first is open.
        BOOST_CHECK(!pos::SlashingProtection::Open(file, dir, "keys.lock", error));
        BOOST_CHECK(error.find("another validator") != std::string::npos);
        BOOST_CHECK(!protection->LastSigned(uint256{1}));
        BOOST_CHECK(protection->MaySign(uint256{1}, 10, error));
        // Never at or below a slot signed.
        BOOST_CHECK(!protection->MaySign(uint256{1}, 10, error));
        BOOST_CHECK(!protection->MaySign(uint256{1}, 9, error));
        BOOST_CHECK(protection->MaySign(uint256{2}, 5, error));
        BOOST_CHECK(protection->MaySign(uint256{1}, 11, error));
    }
    {
        // What was signed is on disk: a restarted node remembers it.
        auto protection{pos::SlashingProtection::Open(file, dir, "keys.lock", error)};
        BOOST_REQUIRE_MESSAGE(protection, error);
        BOOST_CHECK(protection->LastSigned(uint256{1}) == std::optional<int64_t>{11});
        BOOST_CHECK(protection->LastSigned(uint256{2}) == std::optional<int64_t>{5});
        BOOST_CHECK(!protection->MaySign(uint256{1}, 11, error));
    }
    {
        // A file that does not read stops the producer rather than forgetting what was signed.
        std::ofstream{file.std_path()} << "something else\n";
        BOOST_CHECK(!pos::SlashingProtection::Open(file, dir, "keys.lock", error));
        std::ofstream{file.std_path()} << "fastblocks-slashing-protection 1\nnot a line\n";
        BOOST_CHECK(!pos::SlashingProtection::Open(file, dir, "keys.lock", error));
        std::ofstream{file.std_path()} << "fastblocks-slashing-protection 2 nothex\n";
        BOOST_CHECK(!pos::SlashingProtection::Open(file, dir, "keys.lock", error, {}, uint256{7}));
    }
    {
        // The file names the chain (audit 7, finding 3). Of this chain, or of the first format: its slots count.
        std::ofstream{file.std_path()} << "fastblocks-slashing-protection 1\n" << HexStr(uint256{1}) << " 100\n";
        {
            auto protection{pos::SlashingProtection::Open(file, dir, "keys.lock", error, {}, uint256{7})};
            BOOST_REQUIRE_MESSAGE(protection, error);
            BOOST_CHECK(protection->LastSigned(uint256{1}) == std::optional<int64_t>{100});
            BOOST_CHECK(!protection->MaySign(uint256{1}, 50, error));
            BOOST_CHECK(protection->MaySign(uint256{1}, 101, error));
        }
        {
            auto protection{pos::SlashingProtection::Open(file, dir, "keys.lock", error, {}, uint256{7})};
            BOOST_REQUIRE_MESSAGE(protection, error);
            BOOST_CHECK(protection->LastSigned(uint256{1}) == std::optional<int64_t>{101});
        }
        // Of another chain (a test network started again from a new genesis, whose slots are numbered
        // from a later time): none count, and the file is kept beside.
        {
            auto protection{pos::SlashingProtection::Open(file, dir, "keys.lock", error, {}, uint256{8})};
            BOOST_REQUIRE_MESSAGE(protection, error);
            BOOST_CHECK(!protection->LastSigned(uint256{1}));
            BOOST_CHECK(protection->MaySign(uint256{1}, 3, error));
            BOOST_CHECK(!protection->MaySign(uint256{1}, 3, error));
        }
        BOOST_CHECK(fs::exists(fs::PathFromString(fs::PathToString(file) + "." + uint256{7}.GetHex() + ".old")));
        {
            // And back on this chain's file: what it signed counts.
            auto protection{pos::SlashingProtection::Open(file, dir, "keys.lock", error, {}, uint256{8})};
            BOOST_REQUIRE_MESSAGE(protection, error);
            BOOST_CHECK(protection->LastSigned(uint256{1}) == std::optional<int64_t>{3});
        }
    }
    fs::remove_all(dir);
}

BOOST_FIXTURE_TEST_CASE(blocks_of_stake, FastBlocksSetup)
{
    StartPos();
    const int height{Tip()->nHeight};
    // The producer makes the block of each slot, signed by its leader; slots may be skipped.
    for (const int i : {1, 2, 4, 5}) {
        std::string error;
        const int64_t slot{Slot(i)};
        const auto made{producer->Produce(slot, error)};
        BOOST_REQUIRE_MESSAGE(made, error);
        BOOST_CHECK(Tip()->GetBlockHash() == *made);
        const CBlockHeader header{Tip()->GetBlockHeader()};
        BOOST_CHECK(pos::IsPosHeader(header));
        BOOST_CHECK_EQUAL(header.nTime, pos::SlotTime(slot, Pos()));
        BOOST_CHECK(pos::CheckBlockSig(header, XOnly(Leader(slot)), Genesis()));
        // A slot is never signed twice.
        BOOST_CHECK(!producer->Produce(slot, error));
    }
    BOOST_CHECK_EQUAL(Tip()->nHeight, height + 4);
    // No anchor yet: the height grows, the anchors do not.
    BOOST_CHECK_EQUAL(Key().anchors, 1U);
    BOOST_CHECK_EQUAL(Key().height, static_cast<uint64_t>(height + 4));
    BOOST_CHECK_EQUAL(Fast().last_pos_slot, base_slot + 5);
    BOOST_CHECK_EQUAL(Fast().first_unanchored_slot, base_slot + 1);
    BOOST_CHECK_EQUAL(producer->GetStatus().produced, 4U);
}

BOOST_FIXTURE_TEST_CASE(header_rules, FastBlocksSetup)
{
    StartPos();
    Take(PosBlock(1));
    // The proof-of-work limit as nBits.
    {
        CBlock block{PosBlock(2)};
        block.nBits = 0x1d00ffff;
        Resign(block);
        BOOST_CHECK_EQUAL(Check(block), "bad-pos-bits");
    }
    // The time its slot starts, after the first block. (Every second starts a slot on regtest; and a time
    // before the first block's fails Bitcoin's rule first, so the hook is asked directly.)
    {
        CBlock block{PosBlock(2)};
        block.nTime = static_cast<uint32_t>(Pos().genesis_time);
        Resign(block);
        LOCK(::cs_main);
        BlockValidationState state;
        BOOST_CHECK(!Hooks().CheckHeader(block, *Chain().m_chain.Tip(), Chainman(), state));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-pos-time");
    }
    // A later slot than the last block of stake.
    {
        CBlock block{PosBlock(2)};
        block.nTime = pos::SlotTime(base_slot + 1, Pos());
        Resign(block);
        BOOST_CHECK_EQUAL(Check(block), "bad-pos-slot");
    }
    // A slot that has started (give or take max_future_seconds).
    {
        CBlock block{PosBlock(2)};
        Mock(block.nTime - Pos().max_future_seconds - 1);
        BOOST_CHECK_EQUAL(Check(block), "pos-time-future");
        // Kept to try again later only if its leader signed it: not a header anyone can make.
        CBlock unsigned_block{block};
        unsigned_block.ext.sig[0] ^= 1;
        BOOST_CHECK_EQUAL(Check(unsigned_block), "bad-pos-sig");
        Mock(block.nTime - Pos().max_future_seconds);
        BOOST_CHECK_EQUAL(Check(block), "");
    }
    // Signed by the leader of the slot, and no one else.
    {
        CBlock block{PosBlock(2)};
        const CKey& leader{Leader(pos::SlotOf(block.nTime, Pos()))};
        for (const CKey& other : keys) {
            if (XOnly(other) == XOnly(leader)) continue;
            Resign(block, &other);
            BOOST_CHECK_EQUAL(Check(block), "bad-pos-sig");
        }
        block.ext.sig[0] ^= 1;
        BOOST_CHECK_EQUAL(Check(block), "bad-pos-sig");
    }
    // An acknowledgement of a mainchain height with no commitment on record: not yet.
    {
        CBlock block{PosBlock(2)};
        pos::PosSections sections;
        sections.ack = pos::AnchorAck{.ticket = Ticket(Tip()->GetBlockHash(), script), .main_height = Record().Height() + 5};
        block.vSections = sections.Encode();
        block.nNonce = static_cast<uint32_t>(sections.ack->main_height);
        Resign(block);
        BOOST_CHECK_EQUAL(Check(block), "pos-ack-unknown");
    }
}

BOOST_FIXTURE_TEST_CASE(no_validators, NoValidatorsSetup)
{
    // Without validators the chain is the template's: merged mined blocks only, which carry transactions.
    BOOST_CHECK(!Hooks().GenesisSet().Active());
    BOOST_CHECK(!Hooks().PosActiveNow());
    COutPoint coin;
    const CTxOut spent{Fund(COIN, coin)};
    Accept(Spend(coin, spent, {CTxOut{COIN - TX_FEE, script}}));
    const CBlock block{BmmBlock()};
    BOOST_CHECK_EQUAL(block.vtx.size(), 2U);
    MineBmm(block);
    BOOST_CHECK_EQUAL(Fast().anchors, 2U);
    // A block of stake is not valid.
    CBlock pos_block{block};
    pos_block.hashPrevBlock = Tip()->GetBlockHash();
    pos_block.nVersion |= pos::POS_BLOCK_BIT;
    pos_block.nTime = pos::SlotTime(pos::SlotOf(GetTime(), Pos()) + 1, Pos());
    pos_block.nBits = pos::PosBits(Chainman().GetConsensus().powLimit);
    pos_block.nNonce = 0;
    pos_block.vtx.resize(1);
    pos_block.hashMerkleRoot = BlockMerkleRoot(pos_block);
    Mock(pos_block.nTime);
    BOOST_CHECK_EQUAL(Check(pos_block), "bad-pos-inactive");
}

BOOST_FIXTURE_TEST_CASE(block_rules, FastBlocksSetup)
{
    StartPos();
    // Only a block of stake puts coins in the pot.
    {
        CBlock block{BmmBlock(/*use_mempool=*/false)};
        SetCoinbase(block, [](CMutableTransaction& tx) { tx.vout.emplace_back(0, pos::PotScript()); });
        FinishBmm(block);
        BOOST_CHECK_EQUAL(Check(block), "bad-pos-pot-not-pos");
    }
    // One pot output at most.
    {
        CBlock block{PosBlock(1)};
        SetCoinbase(block, [](CMutableTransaction& tx) {
            tx.vout.insert(tx.vout.begin() + 1, CTxOut{0, pos::PotScript()});
            tx.vout.insert(tx.vout.begin() + 1, CTxOut{0, pos::PotScript()});
        });
        Resign(block);
        BOOST_CHECK_EQUAL(Check(block), "bad-pos-pot-multiple");
    }
    // Sections that read.
    {
        CBlock block{PosBlock(1)};
        block.vSections = {7, 7};
        Resign(block);
        BOOST_CHECK_EQUAL(Check(block), "bad-pos-sections");
    }
    // nNonce is the height of the acknowledged commitment, 0 without one.
    {
        CBlock block{PosBlock(1)};
        block.nNonce = 1;
        Resign(block);
        BOOST_CHECK_EQUAL(Check(block), "bad-pos-ack-nonce");
    }
    const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
    {
        CBlock block{PosBlock(1, ack)};
        block.nNonce += 1;
        Resign(block);
        BOOST_CHECK_EQUAL(Check(block), "bad-pos-ack-nonce");
    }
    // Tickets of version 1, with a payee.
    const auto with_ticket{[&](const std::function<void(pos::AnchorTicket&)>& change) {
        CBlock block{PosBlock(1, ack)};
        pos::PosSections sections;
        BOOST_REQUIRE(pos::PosSections::Decode(block.vSections, sections));
        change(sections.ack->ticket);
        block.vSections = sections.Encode();
        Resign(block);
        return block;
    }};
    BOOST_CHECK_EQUAL(Check(with_ticket([](pos::AnchorTicket& t) { t.version = 2; })), "bad-pos-ticket-version");
    BOOST_CHECK_EQUAL(Check(with_ticket([](pos::AnchorTicket& t) { t.payee = CScript() << OP_RETURN; })), "bad-pos-ticket-payee");
    // The block itself is valid.
    BOOST_CHECK_EQUAL(Check(PosBlock(1, ack)), "");
}

BOOST_FIXTURE_TEST_CASE(fee_split, FastBlocksSetup)
{
    COutPoint coin;
    const CTxOut spent{Fund(COIN, coin)};
    StartPos();
    const CMutableTransaction tx{Spend(coin, spent, {CTxOut{COIN - TX_FEE, script}})};
    Accept(tx);
    const CBlock block{PosBlock(1)};
    BOOST_REQUIRE_EQUAL(block.vtx.size(), 2U);
    // Half to the producer, half to the pot.
    const CTransaction& coinbase{*block.vtx[0]};
    BOOST_CHECK_EQUAL(coinbase.vout[0].nValue, TX_FEE / 2);
    const auto pot{std::find_if(coinbase.vout.begin(), coinbase.vout.end(), [](const CTxOut& out) { return pos::IsPotScript(out.scriptPubKey); })};
    BOOST_REQUIRE(pot != coinbase.vout.end());
    BOOST_CHECK_EQUAL(pot->nValue, TX_FEE / 2);
    // The producer may not keep more, by output 0 or another output.
    {
        CBlock greedy{block};
        SetCoinbase(greedy, [](CMutableTransaction& cb) { cb.vout[0].nValue += 1; for (auto& out : cb.vout) if (pos::IsPotScript(out.scriptPubKey)) out.nValue -= 1; });
        Resign(greedy);
        BOOST_CHECK_EQUAL(Check(greedy), "bad-pos-producer-share");
        CBlock extra{block};
        SetCoinbase(extra, [&](CMutableTransaction& cb) {
            for (auto& out : cb.vout) if (pos::IsPotScript(out.scriptPubKey)) out.nValue -= 1;
            cb.vout.insert(cb.vout.begin() + 1, CTxOut{1, script});
        });
        Resign(extra);
        BOOST_CHECK_EQUAL(Check(extra), "bad-pos-producer-share");
    }
    // Less is fine: what is left unclaimed is lost.
    {
        CBlock modest{block};
        SetCoinbase(modest, [](CMutableTransaction& cb) { cb.vout[0].nValue -= 1; });
        Resign(modest);
        BOOST_CHECK_EQUAL(Check(modest), "");
    }
    Take(block);
    BOOST_CHECK_EQUAL(Fast().pot_accrued, TX_FEE / 2);
    BOOST_CHECK_EQUAL(Fast().pot_paid, 0);
    BOOST_CHECK_EQUAL(pos::PotAccruedAt(WITH_LOCK(::cs_main, return Chain().SideState().View()), Tip()->nHeight), TX_FEE / 2);
}

BOOST_FIXTURE_TEST_CASE(acknowledgements, FastBlocksSetup)
{
    COutPoint coin;
    const CTxOut spent{Fund(COIN, coin)};
    StartPos();
    const CMutableTransaction tx{Spend(coin, spent, {CTxOut{COIN - TX_FEE, script}})};
    Accept(tx);
    Take(PosBlock(1));
    Take(PosBlock(2));
    const pos::ChainKey before{Key()};
    const CBlockIndex* anchored{Tip()->pprev};
    const CScript payee{GetScriptForDestination(WitnessV0KeyHash{*uint160::FromHex("0909090909090909090909090909090909090909")})};
    const CScript other_payee{GetScriptForDestination(WitnessV0KeyHash{*uint160::FromHex("0808080808080808080808080808080808080808")})};
    const pos::AnchorAck ack{Anchor(Ticket(anchored->GetBlockHash(), payee))};

    // The mainchain committed to the ticket, at the height said.
    {
        // (The mainchain block before committed to a BMM block, the last anchor: a header that names
        // it is refused as such, and the state's rule alone says the commitment is not there.)
        pos::AnchorAck wrong{ack};
        wrong.main_height = ack.main_height - 1;
        const CBlock block{WithAck(PosBlock(3), wrong)};
        BOOST_CHECK_EQUAL(Check(block), "bad-pos-ack-order");
        std::optional<sidechain::PegStep> step;
        std::string reason;
        BOOST_CHECK(!Hooks().GetPegStep(block, Record(), step, reason));
        BOOST_CHECK_EQUAL(reason, "bad-pos-ack-height");
    }
    {
        const pos::AnchorAck unknown{.ticket = Ticket(anchored->GetBlockHash(), script), .main_height = Record().Height()};
        BOOST_CHECK_EQUAL(Check(WithAck(PosBlock(3), unknown)), "bmm-unknown");
    }
    // The block anchored is on the chain, at or below the parent.
    {
        const pos::AnchorAck nowhere{Anchor(Ticket(uint256{0x55}, payee))};
        BOOST_CHECK_EQUAL(Check(WithAck(PosBlock(3), nowhere)), "bad-pos-ack-anchored");
    }

    // The acknowledgement: the pot accrued up to the block anchored goes to the payee, the anchors
    // count one more, and the bundle and deposits are as of the commitment.
    // (A block of the same slot, not acknowledging, which stays on a branch of its own.)
    const CBlock sibling{PosBlock(3)};
    Take(sibling);
    CBlockIndex* sibling_index{WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(sibling.GetHash()))};
    {
        BlockValidationState state;
        Chain().InvalidateBlock(state, sibling_index);
    }
    const CBlock acked{PosBlock(3, ack)};
    WITH_LOCK(::cs_main, Chain().ResetBlockFailureFlags(sibling_index));
    BOOST_CHECK_EQUAL(acked.nNonce, static_cast<uint32_t>(ack.main_height));
    const CTransaction& coinbase{*acked.vtx[0]};
    BOOST_REQUIRE_GE(coinbase.vout.size(), 2U);
    BOOST_CHECK(coinbase.vout[1] == CTxOut(TX_FEE / 2, payee));
    Take(acked);
    BOOST_CHECK_EQUAL(Key().anchors, before.anchors + 1);
    BOOST_CHECK_EQUAL(Key().last_anchor_height, static_cast<uint32_t>(ack.main_height));
    const pos::FastState fast{Fast()};
    BOOST_CHECK_EQUAL(fast.pot_paid, TX_FEE / 2);
    BOOST_CHECK_EQUAL(fast.last_main_height, ack.main_height);
    BOOST_CHECK(fast.last_anchored == anchored->GetBlockHash());
    BOOST_CHECK(fast.anchor_point == acked.GetHash());
    // The blocks after the one anchored are not covered yet: the first of them starts the count.
    BOOST_CHECK_EQUAL(fast.first_unanchored_slot, base_slot + 2);
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return Chain().SideState().MainHeight()), ack.main_height - 1);
    // The block rests on the commitment to the ticket.
    BOOST_CHECK(Hooks().CommitmentOf(*Tip()) == ack.ticket.Hash());
    const auto resting{Hooks().BlocksCommittedBy(ack.ticket.Hash(), ack.main_height)};
    BOOST_CHECK(std::find(resting.begin(), resting.end(), acked.GetHash()) != resting.end());

    // In the order of the mainchain: not the same commitment again, nor an older one.
    BOOST_CHECK_EQUAL(Check(WithAck(PosBlock(4), ack)), "bad-pos-ack-order");
    // The block anchored is an ancestor: not a block of another branch.
    {
        const pos::AnchorAck elsewhere{Anchor(Ticket(sibling.GetHash(), payee))};
        BOOST_CHECK_EQUAL(Check(WithAck(PosBlock(4), elsewhere)), "bad-pos-ack-anchored");
    }
    // Chained: the next ticket anchors the block the last anchor anchored, or a block after it -- one
    // below the acknowledgement too (a ticket made for the next mainchain block before the last
    // commitment was acknowledged: anchors need not wait for acknowledgements). Its anchor builds no
    // bundle (the peg actions of the acknowledgement came between the block it anchors and it); one
    // that anchors the acknowledgement or a block after it does.
    {
        const pos::AnchorAck below{Anchor(Ticket(anchored->pprev->GetBlockHash(), other_payee))};
        BOOST_CHECK_EQUAL(Check(WithAck(PosBlock(4), below)), "bad-pos-ack-chain");
        const pos::AnchorAck early{Anchor(Ticket(anchored->GetBlockHash(), other_payee))};
        const pos::AnchorAck chained{Anchor(Ticket(acked.GetHash(), payee))};
        {
            LOCK(::cs_main);
            BOOST_CHECK_EQUAL(*Hooks().AckPegStep(early, *anchored, *Tip()).max_withdrawal_height, -1);
            BOOST_CHECK_EQUAL(*Hooks().AckPegStep(chained, *Tip(), *Tip()).max_withdrawal_height, Tip()->nHeight);
        }
        Take(PosBlock(4, early));
        BOOST_CHECK_EQUAL(Key().anchors, before.anchors + 2);
        BOOST_CHECK(Fast().last_anchored == anchored->GetBlockHash());
        Take(PosBlock(5, chained));
        BOOST_CHECK_EQUAL(Key().anchors, before.anchors + 3);
        // Nothing accrued since: nothing paid.
        BOOST_CHECK_EQUAL(Tip()->nTx, 1U);
        BOOST_CHECK_EQUAL(Fast().pot_paid, TX_FEE / 2);
    }
}

BOOST_FIXTURE_TEST_CASE(refunds_wait_for_acknowledgements, FastBlocksSetup)
{
    COutPoint coin;
    const CTxOut spent{Fund(COIN, coin)};
    StartPos();
    // A withdrawal in a block of stake.
    const CMutableTransaction withdrawal{Spend(coin, spent, {CTxOut{COIN / 2, sidechain::WithdrawalScript(1000, key.GetPubKey().GetID(), script)},
                                                             CTxOut{COIN / 2 - TX_FEE, script}})};
    Accept(withdrawal);
    Take(PosBlock(1));
    // A refund request for it waits for a block that takes the peg actions.
    sidechain::RefundRequest request;
    request.withdrawal = COutPoint{withdrawal.GetHash(), 0};
    BOOST_REQUIRE(key.SignCompact(MessageHash(sidechain::RefundMessage(request.withdrawal)), request.signature));
    const CTxOut& change{withdrawal.vout[1]};
    const CMutableTransaction refund{Spend(COutPoint{withdrawal.GetHash(), 1}, change, {CTxOut{0, sidechain::RefundScript(request)}, CTxOut{change.nValue - TX_FEE, script}})};
    Accept(refund);
    CBlock plain{PosBlock(2)};
    BOOST_CHECK_EQUAL(plain.vtx.size(), 1U);
    plain.vtx.push_back(MakeTransactionRef(refund));
    Resign(plain);
    BOOST_CHECK_EQUAL(Check(plain), "bad-sc-refund-not-peg");
    // An acknowledgement takes it.
    Take(PosBlock(2));
    const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
    const CBlock acked{PosBlock(3, ack)};
    BOOST_CHECK_EQUAL(acked.vtx.size(), 2U);
    Take(acked);
}

BOOST_FIXTURE_TEST_CASE(unanchored_blocks_stop, FastBlocksSetup)
{
    StartPos();
    Take(PosBlock(1));
    // max_unanchored_slots after the first block no anchor covers, blocks of stake stop...
    const int last{static_cast<int>(Pos().max_unanchored_slots)};
    {
        CBlock block{PosBlock(last + 1)};
        BOOST_CHECK_EQUAL(Check(block), "bad-pos-unanchored");
    }
    Take(PosBlock(last));
    // ... until an anchor covers them.
    const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
    Take(PosBlock(last + 1, ack));
    BOOST_CHECK_EQUAL(Fast().first_unanchored_slot, base_slot + last + 1);
    Take(PosBlock(last + 2));
}

BOOST_FIXTURE_TEST_CASE(bmm_fallback, FastBlocksSetup)
{
    COutPoint coin;
    const CTxOut spent{Fund(COIN, coin)};
    StartPos();
    Accept(Spend(coin, spent, {CTxOut{COIN - TX_FEE, script}}));
    // A block of stake puts half its fees in the pot.
    Take(PosBlock(1));
    BOOST_CHECK_EQUAL(Fast().pot_accrued, TX_FEE / 2);

    // A BMM block without transactions is valid while blocks of stake come.
    CBlock empty{BmmBlock(/*use_mempool=*/false, script)};
    BOOST_CHECK_EQUAL(empty.vtx.size(), 1U);
    // It anchors its parent, and its first output takes the pot.
    BOOST_REQUIRE_GE(empty.vtx[0]->vout.size(), 2U);
    BOOST_CHECK(empty.vtx[0]->vout[1] == CTxOut(TX_FEE / 2, script));
    const pos::ChainKey before{Key()};
    MineBmm(empty);
    BOOST_CHECK_EQUAL(Key().anchors, before.anchors + 1);
    BOOST_CHECK_EQUAL(Fast().pot_paid, TX_FEE / 2);
    BOOST_CHECK_EQUAL(Fast().first_unanchored_slot, -1);
}

BOOST_FIXTURE_TEST_CASE(bmm_blocks_carry_transactions_when_stalled, FastBlocksSetup)
{
    COutPoint coin;
    const CTxOut spent{Fund(COIN, coin)};
    StartPos();
    Take(PosBlock(1));
    Accept(Spend(coin, spent, {CTxOut{COIN - TX_FEE, script}}));
    // Blocks of stake came recently: no transactions in a BMM block.
    {
        const CBlock block{BmmBlock()};
        BOOST_REQUIRE_EQUAL(block.vtx.size(), 2U);
        Commit(block.GetHash());
        Submit(block);
        BOOST_CHECK_EQUAL(Reason(block), "bad-pos-bmm-not-stalled");
    }
    // stall_seconds after the last of them, as the mainchain block before the commitment says, it may.
    Mock(pos::SlotTime(base_slot + 1, Pos()) + Pos().stall_seconds);
    Main();
    const CBlock block{BmmBlock()};
    BOOST_REQUIRE_EQUAL(block.vtx.size(), 2U);
    MineBmm(block);
}

BOOST_FIXTURE_TEST_CASE(fork_choice, FastBlocksSetup)
{
    StartPos();
    Take(PosBlock(1));
    const CBlockIndex* fork{Tip()};
    // Two tickets for the block at the fork, committed one after the other.
    const pos::AnchorAck first{Anchor(Ticket(fork->GetBlockHash(), script))};
    const pos::AnchorAck second{Anchor(Ticket(fork->GetBlockHash(), GetScriptForDestination(WitnessV0KeyHash{*uint160::FromHex("0707070707070707070707070707070707070707")})))};
    // Branches B and C on the fork, each acknowledging one of them (C skips the first: allowed).
    const CBlock b{PosBlock(4, first)};
    const CBlock c{PosBlock(5, second)};
    // Branch A: two blocks of stake and no anchor.
    Take(PosBlock(2));
    const CBlock a2{PosBlock(3)};
    Take(a2);
    Mock(pos::SlotTime(base_slot + 5, Pos()));
    // More anchors beat more blocks.
    Submit(c);
    BOOST_CHECK(Tip()->GetBlockHash() == c.GetHash());
    BOOST_CHECK_GT(Key().anchors, pos::ChainKey::FromWork(WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(a2.GetHash())->nChainWork)).anchors);
    // As many anchors: the one the mainchain committed to first wins, whatever came first.
    Submit(b);
    BOOST_CHECK(Tip()->GetBlockHash() == b.GetHash());
    BOOST_CHECK_EQUAL(Key().last_anchor_height, static_cast<uint32_t>(first.main_height));
    // And B grows by blocks of stake.
    Take(PosBlock(6));
    BOOST_CHECK(Tip()->pprev->GetBlockHash() == b.GetHash());
}

BOOST_FIXTURE_TEST_CASE(ticket_pool, FastBlocksSetup)
{
    // A peer that sends tickets by the thousand pushes out the oldest, but not this node's own nor
    // those the mainchain committed to.
    const pos::AnchorTicket own{Ticket(Tip()->GetBlockHash(), script)};
    BOOST_CHECK(Hooks().AddTicket(own, /*own=*/true));
    BOOST_CHECK(!Hooks().AddTicket(own));
    const pos::AnchorTicket committed{Ticket(Tip()->GetBlockHash(), GetScriptForDestination(WitnessV0KeyHash{*uint160::FromHex("0606060606060606060606060606060606060606")}))};
    Commit(committed.Hash());
    BOOST_CHECK(Hooks().AddTicket(committed));
    const pos::AnchorTicket first_junk{Ticket(uint256{1}, script)};
    for (uint32_t i{1}; i <= pos::MAX_TICKETS + 100; ++i) {
        pos::AnchorTicket junk{Ticket(ArithToUint256(arith_uint256{i}), script)};
        BOOST_CHECK(Hooks().AddTicket(junk));
    }
    BOOST_CHECK(Hooks().GetTicket(own.Hash()));
    BOOST_CHECK(Hooks().GetTicket(committed.Hash()));
    BOOST_CHECK(!Hooks().GetTicket(first_junk.Hash()));
    BOOST_CHECK_EQUAL(Hooks().Tickets().size(), pos::MAX_TICKETS);
}

BOOST_AUTO_TEST_CASE(hooks_without_context)
{
    // Hooks not attached to a chainstate refuse what they cannot check.
    CChainParams::RegTestOptions options;
    options.sidechain_slot = 5;
    const auto params{CChainParams::RegTest(options)};
    pos::FastBlocksHooks hooks{*params};
    CBlock block;
    block.nVersion = pos::POS_BLOCK_BIT;
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
    block.vtx.push_back(MakeTransactionRef(coinbase));
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty};
    sidechain::State state{store};
    std::vector<CTxOut> owed;
    std::string reason;
    BOOST_CHECK(!hooks.BeforePegStep(state, block, 1, sidechain::PegStep{}, params->GetConsensus().sidechain, owed, reason));
    BOOST_CHECK_EQUAL(reason, "bad-pos-context");
    BOOST_CHECK(!hooks.AfterBlock(state, block, 1, std::nullopt, 0, params->GetConsensus().sidechain, reason));
    BOOST_CHECK_EQUAL(reason, "bad-pos-context");
    // An acknowledgement committed on record, but no block index to find what it anchors. (Extended
    // blocks on for it, as installed hooks would have them.)
    {
        SetBlockExtensions(true);
        pos::PosSections sections;
        sections.ack = pos::AnchorAck{.ticket = pos::AnchorTicket{.version = 1, .anchored = uint256{7}, .payee = CScript() << OP_TRUE}, .main_height = 1};
        CBlock acking{block};
        acking.vSections = sections.Encode();
        sidechain::Mainchain record;
        sidechain::MainBlock first;
        first.hash = uint256{1};
        BOOST_REQUIRE(record.Append(first));
        sidechain::MainBlock committing;
        committing.hash = uint256{2};
        committing.prev_hash = first.hash;
        committing.bmm = sections.ack->ticket.Hash();
        BOOST_REQUIRE(record.Append(committing));
        std::optional<sidechain::PegStep> step;
        BOOST_CHECK(!hooks.GetPegStep(acking, record, step, reason));
        BOOST_CHECK_EQUAL(reason, "bad-pos-context");
        // Sections that do not read.
        acking.vSections = {9};
        BOOST_CHECK(!hooks.GetPegStep(acking, record, step, reason));
        BOOST_CHECK_EQUAL(reason, "bad-pos-sections");
        SetBlockExtensions(false);
    }
    // Without validators, no block of stake.
    BOOST_CHECK(!hooks.GenesisSet().Active());
}

BOOST_FIXTURE_TEST_CASE(finality, FastBlocksSetup)
{
    // From the heights the newest anchors anchor: soft, anchored (k), final.
    BOOST_CHECK(pos::FinalityAt(10, 1, {}, 3).status == pos::FinalityStatus::SOFT);
    BOOST_CHECK(pos::FinalityAt(10, 1, {9, 5}, 3).status == pos::FinalityStatus::SOFT);
    const pos::Finality anchored{pos::FinalityAt(10, 4, {12, 10, 5}, 3)};
    BOOST_CHECK(anchored.status == pos::FinalityStatus::ANCHORED);
    BOOST_CHECK_EQUAL(anchored.anchors, 2);
    BOOST_CHECK_EQUAL(anchored.depth, 4);
    BOOST_CHECK(pos::FinalityAt(10, 9, {14, 12, 10}, 3).status == pos::FinalityStatus::FINAL);
    BOOST_CHECK(pos::FinalityAt(10, 9, {14}, 1).status == pos::FinalityStatus::FINAL);
    BOOST_CHECK_EQUAL(pos::FinalityName(pos::FinalityStatus::ANCHORED), "anchored");

    // On a chain: a BMM block anchors its parent; an acknowledgement the block its ticket names.
    StartPos();
    const CBlockIndex* bmm{Tip()};
    Take(PosBlock(1));
    const CBlockIndex* p1{Tip()};
    Take(PosBlock(2));
    const CBlockIndex* p2{Tip()};
    Take(PosBlock(3));
    const CBlockIndex* p3{Tip()};
    const auto finality{[&](const CBlockIndex* block, int final_anchors) { return WITH_LOCK(::cs_main, return pos::FinalityOf(Hooks(), *block, *Tip(), final_anchors)); }};
    BOOST_CHECK(finality(p1, 3).status == pos::FinalityStatus::SOFT);
    BOOST_CHECK_EQUAL(finality(p1, 3).depth, 3);
    BOOST_CHECK(finality(bmm->pprev, 3).status == pos::FinalityStatus::ANCHORED);

    const pos::AnchorAck first{Anchor(Ticket(p2->GetBlockHash(), script))};
    Take(PosBlock(4, first));
    const CBlockIndex* q1{Tip()};
    BOOST_CHECK(finality(p1, 3).status == pos::FinalityStatus::ANCHORED);
    BOOST_CHECK_EQUAL(finality(p2, 3).anchors, 1);
    BOOST_CHECK(finality(p3, 3).status == pos::FinalityStatus::SOFT);
    // With one anchor to be final (regtest's final_anchors), anchored is final.
    BOOST_CHECK(finality(p2, 1).status == pos::FinalityStatus::FINAL);
    BOOST_CHECK_EQUAL(Pos().final_anchors, 1);

    const pos::AnchorAck second{Anchor(Ticket(q1->GetBlockHash(), script))};
    Take(PosBlock(5, second));
    BOOST_CHECK_EQUAL(finality(p1, 3).anchors, 2);
    BOOST_CHECK(finality(p3, 3).status == pos::FinalityStatus::ANCHORED);
    MineBmm(BmmBlock(/*use_mempool=*/false));
    BOOST_CHECK(finality(p1, 3).status == pos::FinalityStatus::FINAL);
    BOOST_CHECK(finality(Tip(), 3).status == pos::FinalityStatus::SOFT);

    // The newest anchors, newest first, with what they anchor and the height of their commitment.
    const auto anchors{WITH_LOCK(::cs_main, return pos::RecentAnchors(Hooks(), *Tip(), 10))};
    BOOST_REQUIRE_EQUAL(anchors.size(), 4U);
    BOOST_CHECK(anchors[0].block == Tip() && anchors[0].anchored == Tip()->pprev);
    BOOST_CHECK_EQUAL(anchors[0].main_height, static_cast<uint32_t>(Record().Height()));
    BOOST_CHECK(anchors[1].anchored == q1);
    BOOST_CHECK_EQUAL(anchors[1].main_height, static_cast<uint32_t>(second.main_height));
    BOOST_CHECK(anchors[2].block == q1 && anchors[2].anchored == p2);
    BOOST_CHECK(anchors[3].block == bmm);
    // At most as many as asked for; none at or below the floor.
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return pos::RecentAnchors(Hooks(), *Tip(), 2)).size(), 2U);
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return pos::RecentAnchors(Hooks(), *Tip(), 10, q1->nHeight)).size(), 2U);
    // A block that is no anchor anchors nothing; the acknowledgement is read from disk once forgotten.
    BOOST_CHECK(!WITH_LOCK(::cs_main, return Hooks().AnchoredBy(*p3)));
    BOOST_CHECK(WITH_LOCK(::cs_main, return Hooks().AnchoredBy(*q1)) == p2);
}

BOOST_FIXTURE_TEST_CASE(unanchored_headers, FastBlocksSetup)
{
    StartPos();
    const CBlockIndex* start{Tip()};
    Take(PosBlock(1));
    const int last{static_cast<int>(Pos().max_unanchored_slots)};
    const auto header_reason{[&](const CBlock& block) {
        LOCK(::cs_main);
        BlockValidationState state;
        const CBlockIndex* prev{Chainman().m_blockman.LookupBlockIndex(block.hashPrevBlock)};
        if (Hooks().CheckHeader(block, *prev, Chainman(), state)) return std::string{};
        BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_INVALID_HEADER);
        return state.GetRejectReason();
    }};
    // Without an acknowledgement, the header alone tells: max_unanchored_slots after the first block of
    // stake after the last BMM block, no more.
    BOOST_CHECK_EQUAL(header_reason(PosBlock(last + 1)), "bad-pos-unanchored");
    BOOST_CHECK_EQUAL(header_reason(PosBlock(last)), "");
    // An acknowledgement's header passes: what it covers is in its block. One that covers too little
    // (the block of stake after the BMM block stays uncovered) fails in the state.
    const CBlock too_little{PosBlock(last + 1, Anchor(Ticket(start->GetBlockHash(), script)))};
    BOOST_CHECK_EQUAL(header_reason(too_little), "");
    BOOST_CHECK_EQUAL(Check(too_little), "bad-pos-unanchored");
    const CBlock covering{PosBlock(last + 1, Anchor(Ticket(Tip()->GetBlockHash(), script)))};
    Take(covering);
    // After an acknowledgement, the count starts at most at it.
    BOOST_CHECK_EQUAL(header_reason(PosBlock(last + 1 + last)), "bad-pos-unanchored");
    BOOST_CHECK_EQUAL(header_reason(PosBlock(last + last)), "");
}

BOOST_FIXTURE_TEST_CASE(evidence, FastBlocksSetup)
{
    StartPos();
    const CBlock first{PosBlock(1)};
    CBlock second{first};
    SetCoinbase(second, [](CMutableTransaction& tx) { tx.vout.emplace_back(0, CScript() << OP_RETURN); });
    Resign(second);
    BOOST_REQUIRE(first.GetHash() != second.GetHash());
    const int64_t slot{pos::SlotOf(first.nTime, Pos())};
    const uint256 leader{XOnly(Leader(slot))};
    std::string reason;

    // Two headers of one slot, both signed by its leader: the offence.
    const pos::DoubleProposal proof{.a = first, .b = second};
    const auto offence{pos::CheckEvidence(proof, leader, Genesis(), Pos(), reason)};
    BOOST_REQUIRE_MESSAGE(offence, reason);
    BOOST_CHECK(offence->pubkey == XOnly(Leader(slot)));
    BOOST_CHECK_EQUAL(offence->slot, slot);
    // Headers of blocks of stake.
    BOOST_CHECK(!pos::CheckEvidence({.a = first, .b = BmmBlock(/*use_mempool=*/false)}, leader, Genesis(), Pos(), reason));
    BOOST_CHECK_EQUAL(reason, "bad-pos-evidence-kind");
    // Of one slot, which starts at their time.
    {
        CBlock later{PosBlock(2)};
        BOOST_CHECK(!pos::CheckEvidence({.a = first, .b = later}, leader, Genesis(), Pos(), reason));
        BOOST_CHECK_EQUAL(reason, "bad-pos-evidence-slot");
        CBlockHeader a{first}, b{second};
        a.nTime = b.nTime = static_cast<uint32_t>(Pos().genesis_time);
        BOOST_CHECK(!pos::CheckEvidence({.a = a, .b = b}, leader, Genesis(), Pos(), reason));
        BOOST_CHECK_EQUAL(reason, "bad-pos-evidence-slot");
    }
    // Two headers, not one twice.
    BOOST_CHECK(!pos::CheckEvidence({.a = first, .b = first}, leader, Genesis(), Pos(), reason));
    BOOST_CHECK_EQUAL(reason, "bad-pos-evidence-same");
    // Both signed by the leader of the slot.
    for (const CKey& other : keys) {
        if (XOnly(other) == XOnly(Leader(slot))) continue;
        CBlock forged{second};
        Resign(forged, &other);
        BOOST_CHECK(!pos::CheckEvidence({.a = first, .b = forged}, leader, Genesis(), Pos(), reason));
        BOOST_CHECK_EQUAL(reason, "bad-pos-evidence-sig");
    }
    // Not the leader's key: no offence of its.
    for (const CKey& other : keys) {
        if (XOnly(other) == leader) continue;
        BOOST_CHECK(!pos::CheckEvidence(proof, XOnly(other), Genesis(), Pos(), reason));
        BOOST_CHECK_EQUAL(reason, "bad-pos-evidence-sig");
    }

    // One proof per offence, whichever header comes first; it reads back as written.
    pos::DoubleProposal swapped{.a = second, .b = first};
    BOOST_CHECK(Hooks().AddEvidence(swapped, reason));
    BOOST_CHECK(Hooks().AddEvidence(proof, reason));
    BOOST_CHECK_EQUAL(Hooks().Evidence().Size(), 1U);
    DataStream stream{};
    stream << *Hooks().Evidence().Get(*offence);
    pos::DoubleProposal read;
    stream >> read;
    BOOST_CHECK(read.a.GetHash() == std::min(first.GetHash(), second.GetHash()));
    BOOST_CHECK(read.b.ext.sig == (read.b.GetHash() == first.GetHash() ? first.ext.sig : second.ext.sig));
    // Relayed when added (none to relay later); not if it is no proof.
    BOOST_CHECK(Hooks().Evidence().TakeUnrelayed().empty());
    BOOST_CHECK(!Hooks().AddEvidence({.a = first, .b = first}, reason));

    // A node finds proofs itself, in the headers it is given: two of slot 2 on the first.
    Take(first);
    const CBlock child{PosBlock(2)};
    CBlock rival{child};
    SetCoinbase(rival, [](CMutableTransaction& tx) { tx.vout.emplace_back(0, CScript() << OP_RETURN); });
    Resign(rival);
    {
        BlockValidationState state;
        const std::vector<CBlockHeader> headers{child, rival};
        BOOST_CHECK(Chainman().ProcessNewBlockHeaders(headers, /*min_pow_checked=*/true, state));
    }
    const pos::Offence found{.pubkey = XOnly(Leader(slot + 1)), .slot = slot + 1};
    BOOST_CHECK(Hooks().Evidence().Get(found));
    BOOST_CHECK_EQUAL(Hooks().Evidence().Size(), 2U);
    // ... and relays them (in the follower's round).
    const auto unrelayed{Hooks().Evidence().TakeUnrelayed()};
    BOOST_REQUIRE_EQUAL(unrelayed.size(), 1U);
    BOOST_CHECK(std::min(child.GetHash(), rival.GetHash()) == unrelayed[0].a.GetHash());

    // Bounded: the proofs of the oldest slots go first, and so do the headers noted.
    for (int64_t i{0}; i < static_cast<int64_t>(pos::MAX_EVIDENCE) + 5; ++i) Hooks().Evidence().Add({.pubkey = uint256{2}, .slot = slot + 1000 + i}, proof);
    BOOST_CHECK_EQUAL(Hooks().Evidence().Size(), pos::MAX_EVIDENCE);
    BOOST_CHECK(!Hooks().Evidence().Get(*offence));
    BOOST_CHECK(!Hooks().Evidence().Get(found));
    BOOST_CHECK(!Hooks().Evidence().Get({.pubkey = uint256{2}, .slot = slot + 1000 + 4}));
    BOOST_CHECK(Hooks().Evidence().Get({.pubkey = uint256{2}, .slot = slot + 1000 + 5}));
    pos::EvidencePool pool;
    CBlockHeader header{first};
    for (int64_t i{0}; i <= static_cast<int64_t>(pos::MAX_SIGNED_HEADERS); ++i) BOOST_CHECK(!pool.NoteSigned(header, i, uint256{1}));
    BOOST_CHECK(!pool.NoteSigned(second, 0, uint256{1}));
    BOOST_CHECK(pool.NoteSigned(second, static_cast<int64_t>(pos::MAX_SIGNED_HEADERS), uint256{1}));
    BOOST_CHECK(!pool.NoteSigned(header, 5, uint256{1}));
}

BOOST_FIXTURE_TEST_CASE(waiting_headers, FastBlocksSetup)
{
    // Headers that wait: for a commitment, for their slot, for an acknowledged commitment.
    BOOST_CHECK(Hooks().IsWaitReason("bmm-unknown"));
    BOOST_CHECK(Hooks().IsWaitReason("pos-time-future"));
    BOOST_CHECK(Hooks().IsWaitReason("pos-ack-unknown"));
    BOOST_CHECK(!Hooks().IsWaitReason("bad-pos-sig"));
    StartPos();
    const int height{Tip()->nHeight};
    const auto ready{[&](const CBlockHeader& header) { return WITH_LOCK(::cs_main, return Hooks().WaitingReady(header, Record())); }};
    const auto wait{[&](const CBlockHeader& header) { WITH_LOCK(::cs_main, Chainman().AddBmmWaiting(header, /*peer=*/-1)); }};

    // A BMM block: once committed to, ordered by its height (as blocks of stake are).
    const CBlock bmm{BmmBlock(/*use_mempool=*/false)};
    BOOST_CHECK(!ready(bmm));

    // A block of stake that acknowledges a commitment not on record yet, and one on top of it.
    const pos::AnchorTicket ticket{Ticket(Tip()->GetBlockHash(), script)};
    const CBlock acking{WithAck(PosBlock(1), pos::AnchorAck{.ticket = ticket, .main_height = Record().Height() + 1})};
    BOOST_CHECK_EQUAL(Check(acking), "pos-ack-unknown");
    CBlock child{PosBlock(2)};
    child.hashPrevBlock = acking.GetHash();
    Resign(child);
    BOOST_CHECK(!ready(acking));
    // (Validation keeps the header that waits; the one on it, the peer that sends both does.)
    wait(acking);
    wait(child);
    BOOST_CHECK(!ready(child)); // its parent waits
    Commit(ticket.Hash());
    BOOST_CHECK(ready(acking) == std::optional<int64_t>{height + 1});
    BOOST_CHECK(ready(child) == std::optional<int64_t>{height + 2});
    Commit(bmm.GetHash());
    BOOST_CHECK(ready(bmm) == std::optional<int64_t>{height + 1});

    // A block of stake whose slot has not started: not before it does.
    const CBlock future{PosBlock(5)};
    Mock(future.nTime - Pos().max_future_seconds - 1);
    BOOST_CHECK(!ready(future));
    Mock(future.nTime);
    BOOST_CHECK(ready(future) == std::optional<int64_t>{height + 1});

    // Taken in, in that order, by the follower's round: the chain moves to the acknowledgement.
    {
        BlockValidationState state;
        const std::vector<CBlockHeader> headers{acking, child};
        BOOST_CHECK(Chainman().ProcessNewBlockHeaders(headers, /*min_pow_checked=*/true, state));
    }
    BOOST_CHECK(WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(child.GetHash())) != nullptr);
}

BOOST_FIXTURE_TEST_CASE(ticket_requests, FastBlocksSetup)
{
    StartPos();
    Take(PosBlock(1));
    // A commitment on record of a ticket this node does not have: asked for, once per mainchain block.
    const pos::AnchorTicket ticket{Ticket(Tip()->GetBlockHash(), script)};
    Commit(ticket.Hash());
    BOOST_CHECK(Hooks().AskMissingTickets() == std::vector<uint256>{ticket.Hash()});
    BOOST_CHECK(Hooks().AskMissingTickets().empty());
    // Known now: not asked for again. (Nor are commitments to blocks.)
    BOOST_CHECK(Hooks().AddTicket(ticket));
    Main();
    BOOST_CHECK(Hooks().AskMissingTickets().empty());

    // From a peer: a ticket of a block this node does not know only if the mainchain committed to it.
    const auto message{[&](const std::string& type, const auto& payload) {
        DataStream stream{};
        stream << payload;
        std::string misbehaving;
        BOOST_CHECK(Hooks().ProcessMessage(/*peer=*/7, type, stream, misbehaving));
        return misbehaving;
    }};
    const pos::AnchorTicket unknown_block{Ticket(uint256{0x77}, script)};
    BOOST_CHECK_EQUAL(message(pos::MSG_TICKET, unknown_block), "");
    BOOST_CHECK(!Hooks().GetTicket(unknown_block.Hash()));
    const pos::AnchorTicket committed{Ticket(uint256{0x78}, script)};
    Commit(committed.Hash());
    BOOST_CHECK_EQUAL(message(pos::MSG_TICKET, committed), "");
    BOOST_CHECK(Hooks().GetTicket(committed.Hash()));
    // Requests: a bounded list.
    BOOST_CHECK_EQUAL(message(pos::MSG_GETTICKETS, std::vector<uint256>(pos::MAX_TICKETS_ASKED, uint256{1})), "");
    BOOST_CHECK_EQUAL(message(pos::MSG_GETTICKETS, std::vector<uint256>(pos::MAX_TICKETS_ASKED + 1, uint256{1})), "too many anchor tickets asked for");
    BOOST_CHECK_EQUAL(message(pos::MSG_GETTICKETS, std::vector<unsigned char>{5}), "malformed request for anchor tickets");
    BOOST_CHECK_EQUAL(message(pos::MSG_EVIDENCE, std::vector<unsigned char>{1, 2}), "malformed evidence");
    {
        DataStream stream{};
        stream << uint8_t{1};
        std::string misbehaving;
        BOOST_CHECK(Hooks().ProcessMessage(7, pos::MSG_TICKET, stream, misbehaving));
        BOOST_CHECK_EQUAL(misbehaving, "malformed anchor ticket");
    }
    // Other messages are not FastBlocks'.
    DataStream empty{};
    std::string misbehaving;
    BOOST_CHECK(!Hooks().ProcessMessage(7, "other", empty, misbehaving));
}

BOOST_FIXTURE_TEST_CASE(staking_switch, FastBlocksSetup)
{
    StartPos();
    BOOST_CHECK(producer->Enabled());
    BOOST_CHECK_EQUAL(producer->LastSignedSlot(), -1);
    producer->SetEnabled(false);
    BOOST_CHECK(!producer->Enabled());
    // (The switch stops the producer's thread; a block asked for directly is still made.)
    std::string error;
    const int64_t slot{Slot(1)};
    BOOST_CHECK(producer->Produce(slot, error));
    BOOST_CHECK_EQUAL(producer->LastSignedSlot(), slot);
    producer->SetEnabled(true);
    BOOST_CHECK(producer->Enabled());
}

BOOST_FIXTURE_TEST_CASE(withheld_header, FastBlocksSetup)
{
    // A leader signs a header that claims an acknowledgement (more anchors than any chain) and keeps
    // its block: the node's best header has more work than its tip for as long as the block does not
    // come. The validators go on making blocks all the same.
    StartPos();
    Take(PosBlock(1));
    // (A commitment after the last anchor's, whose ticket the leader claims to have.)
    Commit(uint256{0xc1});
    CBlock claimed{PosBlock(2)};
    pos::PosSections sections;
    sections.ack = pos::AnchorAck{.ticket = Ticket(Tip()->GetBlockHash(), script), .main_height = Record().Height()};
    claimed.vSections = sections.Encode();
    claimed.nNonce = static_cast<uint32_t>(Record().Height());
    Resign(claimed);
    {
        BlockValidationState state;
        BOOST_REQUIRE_MESSAGE(Chainman().ProcessNewBlockHeaders({{claimed}}, /*min_pow_checked=*/true, state), state.ToString());
    }
    BOOST_CHECK(WITH_LOCK(::cs_main, return Chainman().m_best_header->GetBlockHash()) == claimed.GetHash());
    BOOST_CHECK(!WITH_LOCK(::cs_main, return pos::Producer::Behind(Chainman())));
    std::string error;
    const auto made{producer->Produce(Slot(3), error)};
    BOOST_REQUIRE_MESSAGE(made, error);
    BOOST_CHECK(Tip()->GetBlockHash() == *made);

    // A block of more work the node has, and has yet to connect: behind, no block.
    const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
    const CBlock better{PosBlock(4, ack)};
    {
        LOCK(::cs_main);
        BlockValidationState state;
        CBlockIndex* index{nullptr};
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(Chainman().AcceptBlock(std::make_shared<const CBlock>(better), state, &index, /*fRequested=*/true, /*dbp=*/nullptr, &new_block, /*min_pow_checked=*/true),
                              state.ToString());
        BOOST_CHECK(Chain().setBlockIndexCandidates.contains(index));
        BOOST_CHECK(pos::Producer::Behind(Chainman()));
    }
    BOOST_CHECK(!producer->Produce(Slot(5), error));
    BOOST_CHECK_EQUAL(error, "the node is still taking in the best chain");
    {
        BlockValidationState state;
        BOOST_REQUIRE(Chain().ActivateBestChain(state));
    }
    BOOST_CHECK(Tip()->GetBlockHash() == better.GetHash());
    BOOST_CHECK(!WITH_LOCK(::cs_main, return pos::Producer::Behind(Chainman())));
}

BOOST_FIXTURE_TEST_CASE(ticket_flood, FastBlocksSetup)
{
    // Anyone can make tickets for a known block by the thousand, and each new one would go to every
    // peer: a node takes MAX_PEER_TICKETS a peer per mainchain block of those the mainchain did not
    // commit to; those it committed to, always.
    StartPos();
    const auto send{[&](int64_t peer, const pos::AnchorTicket& ticket) {
        DataStream stream{};
        stream << ticket;
        std::string misbehaving;
        BOOST_CHECK(Hooks().ProcessMessage(peer, pos::MSG_TICKET, stream, misbehaving));
        BOOST_CHECK_EQUAL(misbehaving, "");
        return Hooks().GetTicket(ticket.Hash()).has_value();
    }};
    const auto ticket{[&](uint32_t i) {
        return Ticket(Tip()->GetBlockHash(), GetScriptForDestination(WitnessV0KeyHash{uint160{std::vector<unsigned char>(20, static_cast<unsigned char>(i))}}));
    }};
    for (uint32_t i{0}; i < pos::MAX_PEER_TICKETS; ++i) BOOST_CHECK(send(1, ticket(i)));
    BOOST_CHECK(!send(1, ticket(100)));
    // Another peer has its own count.
    BOOST_CHECK(send(2, ticket(101)));
    // A ticket the mainchain committed to is taken, past the count.
    const pos::AnchorTicket committed{ticket(102)};
    Commit(committed.Hash());
    BOOST_CHECK(send(1, committed));
    // A new mainchain block: the counts start again.
    BOOST_CHECK(send(1, ticket(100)));
}

BOOST_FIXTURE_TEST_CASE(acks_noted_only_when_signed, FastBlocksSetup)
{
    // The blocks that rest on a commitment are noted from blocks that read, before their header is
    // checked: only those signed by the leader of their slot, or a flood of blocks anyone can make
    // would push those that count out of memory.
    StartPos();
    Take(PosBlock(1));
    const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
    CBlock forged{PosBlock(2, ack)};
    forged.ext.sig[5] ^= 1;
    BlockValidationState state;
    BOOST_CHECK(Hooks().CheckBlock(forged, state));
    auto resting{Hooks().BlocksCommittedBy(ack.ticket.Hash(), ack.main_height)};
    BOOST_CHECK(std::find(resting.begin(), resting.end(), forged.GetHash()) == resting.end());
    // The header rules turn it away.
    BOOST_CHECK_EQUAL(Check(forged), "bad-pos-sig");
    const CBlock signed_block{PosBlock(2, ack)};
    BOOST_CHECK(Hooks().CheckBlock(signed_block, state));
    resting = Hooks().BlocksCommittedBy(ack.ticket.Hash(), ack.main_height);
    BOOST_CHECK(std::find(resting.begin(), resting.end(), signed_block.GetHash()) != resting.end());
}

BOOST_FIXTURE_TEST_CASE(false_evidence_misbehaves, FastBlocksSetup)
{
    // A proof whose form is wrong is the peer's fault, whatever chain the node has: it misbehaves.
    // (One whose signatures do not check against the leader this node finds is dropped, not blamed:
    // that leader depends on the chain the node has; attack4_fresh_node_blames_honest_evidence_relay.)
    StartPos();
    const CBlock first{PosBlock(1)};
    const auto send{[&](const pos::DoubleProposal& proof) {
        DataStream stream{};
        stream << proof;
        std::string misbehaving;
        BOOST_CHECK(Hooks().ProcessMessage(3, pos::MSG_EVIDENCE, stream, misbehaving));
        return misbehaving;
    }};
    BOOST_CHECK_EQUAL(send({.a = first, .b = first}), "invalid evidence: bad-pos-evidence-same");
    CBlock second{first};
    SetCoinbase(second, [](CMutableTransaction& tx) { tx.vout.emplace_back(0, CScript() << OP_RETURN); });
    Resign(second);
    BOOST_CHECK_EQUAL(send({.a = first, .b = second}), "");
    // Known already: no fault.
    BOOST_CHECK_EQUAL(send({.a = second, .b = first}), "");
    BOOST_CHECK_EQUAL(Hooks().Evidence().Size(), 1U);
}

BOOST_FIXTURE_TEST_CASE(bmm_time_ahead, FastBlocksSetup)
{
    // A BMM block may be at most MAX_FUTURE_BLOCK_TIME_POS ahead of the clock while blocks of stake are
    // on (not two hours): a run of them two hours ahead would push the median time past, which blocks
    // of stake must come after, two hours ahead.
    BOOST_CHECK_EQUAL(Hooks().MaxFutureBlockTime(), pos::MAX_FUTURE_BLOCK_TIME_POS);
    StartPos();
    const auto header_reason{[&](int64_t ahead) {
        CBlock block{BmmBlock(/*use_mempool=*/false)};
        block.nTime = static_cast<uint32_t>(GetTime() + ahead);
        FinishBmm(block);
        Commit(block.GetHash());
        BlockValidationState state;
        if (Chainman().ProcessNewBlockHeaders({{block}}, /*min_pow_checked=*/true, state)) return std::string{};
        BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_TIME_FUTURE);
        return state.GetRejectReason();
    }};
    BOOST_CHECK_EQUAL(header_reason(pos::MAX_FUTURE_BLOCK_TIME_POS + 1), "time-too-new");
    BOOST_CHECK_EQUAL(header_reason(pos::MAX_FUTURE_BLOCK_TIME_POS), "");
}

BOOST_FIXTURE_TEST_CASE(bmm_time_ahead_without_validators, NoValidatorsSetup)
{
    // Without blocks of stake the chain is the template's: two hours.
    BOOST_CHECK_EQUAL(Hooks().MaxFutureBlockTime(), MAX_FUTURE_BLOCK_TIME);
}

BOOST_FIXTURE_TEST_CASE(moved_commitment, FastBlocksSetup)
{
    // The order of chains counts a BMM block as an anchor committed at the height its commitment has
    // on the record. A reorg of the mainchain that moves the commitment to another height (the
    // follower takes the block back and checks it again) moves it in the key too: else nodes that saw
    // it before the reorg and nodes that saw it after would order chains differently.
    StartPos();
    Take(PosBlock(1));
    const CBlock bmm{BmmBlock(/*use_mempool=*/false)};
    MineBmm(bmm);
    const int committed_at{Record().Height()};
    BOOST_CHECK_EQUAL(Key().last_anchor_height, static_cast<uint32_t>(committed_at));
    // The mainchain drops the block that committed to it, and commits to it again one block later.
    Record().Truncate(committed_at - 1);
    Main();
    Commit(bmm.GetHash());
    BOOST_REQUIRE_EQUAL(Record().Height(), committed_at + 1);
    CBlockIndex* index{WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(bmm.GetHash()))};
    BlockValidationState state;
    BOOST_REQUIRE(Chain().InvalidateBlock(state, index, /*by_record=*/true));
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return Chain().ReconsiderRecordFailure(index)));
    BOOST_REQUIRE(Chain().ActivateBestChain(state));
    BOOST_REQUIRE(Tip() == index);
    BOOST_CHECK_EQUAL(Key().last_anchor_height, static_cast<uint32_t>(committed_at + 1));
    // As a node that never saw it at the old height computes it.
    BOOST_CHECK(WITH_LOCK(::cs_main, return Hooks().ChainWork(index->pprev, index->GetBlockHeader())) == index->nChainWork);
    // And blocks on it follow.
    Take(PosBlock(2));
    BOOST_CHECK_EQUAL(Key().last_anchor_height, static_cast<uint32_t>(committed_at + 1));
}

BOOST_AUTO_TEST_CASE(net_anchors)
{
    // Anchors of a chain (newest first): C acknowledged at 10 covers up to 9 (commitment 102), A at 7
    // up to 5 (101), Z at 3 up to 2 (99).
    std::vector<CBlockIndex> blocks(11);
    for (int i{0}; i < 11; ++i) blocks[i].nHeight = i;
    const std::vector<pos::ChainAnchor> anchors{{.block = &blocks[10], .anchored = &blocks[9], .main_height = 102},
                                                {.block = &blocks[7], .anchored = &blocks[5], .main_height = 101},
                                                {.block = &blocks[3], .anchored = &blocks[2], .main_height = 99}};
    // Without commitments skipped: the blocks the anchors anchor.
    BOOST_CHECK(pos::NetAnchoredHeights(anchors, {}, 3) == (std::vector<int>{9, 5, 2}));
    // The commitment at 100 skipped: a chain that forks at 6 and acknowledges it and A's ticket (whose
    // block, 5, it has) has as many anchors as this one after 6 (A and C), the last one earlier (101
    // against 102): block 8, which only C covers, is not anchored; block 5 is, by one.
    const std::vector<int> net{pos::NetAnchoredHeights(anchors, {{.main_height = 100}}, 3)};
    BOOST_CHECK(net == (std::vector<int>{5, 2, 2}));
    BOOST_CHECK(pos::FinalityAt(8, 3, net, 3).status == pos::FinalityStatus::SOFT);
    BOOST_CHECK_EQUAL(pos::FinalityAt(5, 6, net, 3).anchors, 1);
    // A commitment skipped after C counts against every block C covers.
    BOOST_CHECK(pos::NetAnchoredHeights(anchors, {{.main_height = 103}}, 3) == (std::vector<int>{5, 2, 2}));
    BOOST_CHECK(pos::NetAnchoredHeights({}, {{.main_height = 100}}, 3).empty());
    // Known to rest on a block of another branch, which leaves this chain at 8: only a fork there has
    // it (block 9, covered by C, is not anchored; block 8 is, as a fork below it cannot have it).
    BOOST_CHECK(pos::NetAnchoredHeights(anchors, {{.main_height = 103, .exact_fork = 8}}, 3) == (std::vector<int>{8, 5, 2}));
    // Known to name block 6 of this chain: forks at 6 or above have it.
    BOOST_CHECK(pos::NetAnchoredHeights(anchors, {{.main_height = 100, .min_fork = 6}}, 3) == (std::vector<int>{6, 5, 2}));
}

BOOST_FIXTURE_TEST_CASE(hidden_anchor, FastBlocksSetup)
{
    // The order of chains: with as many anchors, the last committed first wins. A bidder that wins one
    // auction with the ticket of a block it keeps to itself, on a branch of its own, can later show the
    // branch with that acknowledgement and take back a block of the chain that an anchor committed
    // after covers. Such a block is not anchored, net of the commitment skipped.
    StartPos();
    Take(PosBlock(1));
    const CBlock hidden{PosBlock(2)};
    Take(PosBlock(3));
    Take(PosBlock(4));
    const CBlockIndex* covered{Tip()};
    const pos::AnchorTicket kept{Ticket(hidden.GetHash(), script)};
    Commit(kept.Hash());
    const int kept_height{Record().Height()};
    Take(PosBlock(5, Anchor(Ticket(covered->GetBlockHash(), script))));
    const auto finality{[&](const CBlockIndex* block) { return WITH_LOCK(::cs_main, return pos::FinalityOf(Hooks(), *block, *Tip(), 3)); }};
    // An anchor covers it, but the commitment skipped before (one this node has no ticket of) may be
    // the anchor of a chain that leaves it out.
    BOOST_CHECK(finality(covered).status == pos::FinalityStatus::SOFT);
    BOOST_CHECK(WITH_LOCK(::cs_main, return pos::AnchoredHeights(Hooks(), *Tip(), 3)).empty() ||
                WITH_LOCK(::cs_main, return pos::AnchoredHeights(Hooks(), *Tip(), 3)).front() < covered->nHeight - 2);

    // And so it is: the branch, with the acknowledgement of the ticket kept, takes over.
    Submit(hidden);
    CBlockIndex* fork_child{WITH_LOCK(::cs_main, return Chain().m_chain[covered->nHeight - 1])};
    {
        BlockValidationState state;
        BOOST_REQUIRE(Chain().InvalidateBlock(state, fork_child));
        BOOST_REQUIRE(Chain().ActivateBestChain(state));
    }
    BOOST_REQUIRE(Tip()->GetBlockHash() == hidden.GetHash());
    const CBlock attack{PosBlock(6, pos::AnchorAck{.ticket = kept, .main_height = kept_height})};
    WITH_LOCK(::cs_main, Chain().ResetBlockFailureFlags(fork_child));
    Submit(attack);
    BOOST_CHECK(Tip()->GetBlockHash() == attack.GetHash());
    BOOST_CHECK(!WITH_LOCK(::cs_main, return Chain().m_chain.Contains(*covered)));

    // On the branch that won, the commitment of the other branch's ticket is skipped in turn; but the
    // node knows where that ticket's block leaves this chain: below the attack, so it counts only
    // against blocks above that fork and below the attack, not against the attack.
    const CBlockIndex* attacked{Tip()};
    Take(PosBlock(7, Anchor(Ticket(Tip()->GetBlockHash(), script))));
    BOOST_CHECK_EQUAL(finality(attacked).anchors, 1);
    BOOST_CHECK(finality(attacked).status == pos::FinalityStatus::ANCHORED);
    // The hidden block itself, just above the fork of the other branch: that branch has one anchor
    // since the fork, this one two (the ticket kept, and the new one).
    BOOST_CHECK_EQUAL(finality(attacked->pprev).anchors, 1);
    Take(PosBlock(8, Anchor(Ticket(Tip()->GetBlockHash(), script))));
    BOOST_CHECK_EQUAL(finality(attacked).anchors, 2);
    BOOST_CHECK_EQUAL(finality(attacked->pprev).anchors, 2);
}

BOOST_FIXTURE_TEST_CASE(stale_tickets, FastBlocksSetup)
{
    // Blocks of stake stopped for want of an anchor, long enough that a ticket for the block before
    // the tip (ticket depth 1) can no longer be acknowledged: the blocks above it would have been
    // uncovered too long. Leaders do not try it (it would fail the slot), the merged miner does not
    // wait for it (it would wait for ever), and a new ticket names the tip, which is in time.
    StartPos();
    Take(PosBlock(1));
    Take(PosBlock(2));
    const CBlockIndex* stopped{Tip()};
    const pos::AnchorAck old{Anchor(Ticket(stopped->pprev->GetBlockHash(), script))};
    const int last{static_cast<int>(Pos().max_unanchored_slots)};
    // Still in time: the leader acknowledges it.
    BOOST_CHECK(WITH_LOCK(::cs_main, return Hooks().NextAck(*stopped, Fast(), base_slot + 3)) == std::optional<pos::AnchorAck>{old});
    BOOST_CHECK(WITH_LOCK(::cs_main, return Hooks().TicketTarget(*stopped, Fast(), base_slot + 3)) == stopped->pprev);
    // Too late: the block of stake above it (slot 2) would be uncovered for max_unanchored_slots.
    const int64_t late{base_slot + 2 + last};
    BOOST_CHECK_EQUAL(Check(PosBlock(2 + last, old)), "bad-pos-unanchored");
    BOOST_CHECK(!WITH_LOCK(::cs_main, return Hooks().Acknowledgeable(old.ticket, *stopped, Fast(), late)));
    BOOST_CHECK(!WITH_LOCK(::cs_main, return Hooks().NextAck(*stopped, Fast(), late)));
    BOOST_CHECK(WITH_LOCK(::cs_main, return Hooks().TicketTarget(*stopped, Fast(), late)) == stopped);
    std::string error;
    BOOST_CHECK(!producer->Produce(Slot(2 + last), error));
    BOOST_CHECK_EQUAL(error, "no anchor for too long: blocks of stake wait for one");
    // A ticket for the tip is in time, and starts the blocks of stake again.
    const pos::AnchorAck fresh{Anchor(Ticket(stopped->GetBlockHash(), script))};
    BOOST_CHECK(WITH_LOCK(::cs_main, return Hooks().NextAck(*stopped, Fast(), late + 1)) == std::optional<pos::AnchorAck>{fresh});
    const auto made{producer->Produce(Slot(3 + last), error)};
    BOOST_REQUIRE_MESSAGE(made, error);
    BOOST_CHECK_EQUAL(Fast().last_main_height, fresh.main_height);
    Take(PosBlock(4 + last));
}

BOOST_FIXTURE_TEST_CASE(swapped_signature, FastBlocksSetup)
{
    // The block hash does not cover the leader's signature: a node that has the header of a block of
    // stake turns away a block with that hash and another signature (another key's, or none) as a
    // mutated block, without taking the header for invalid; the block its leader signed is taken after.
    StartPos();
    Take(PosBlock(1));
    const CBlock honest{PosBlock(2)};
    const CKey& leader{Leader(pos::SlotOf(honest.nTime, Pos()))};
    const CKey* other{nullptr};
    for (const CKey& k : keys) {
        if (XOnly(k) != XOnly(leader)) other = &k;
    }
    BOOST_REQUIRE(other);
    CBlock swapped{honest};
    BOOST_REQUIRE(pos::Producer::Sign(swapped, *other, Genesis()));
    CBlock zeroed{honest};
    zeroed.ext.sig.fill(0);
    BOOST_REQUIRE(swapped.GetHash() == honest.GetHash() && zeroed.GetHash() == honest.GetHash());
    BOOST_REQUIRE(swapped.ext.sig != honest.ext.sig);

    // Without the header, the signature is checked with it: bad-pos-sig, and no index.
    Submit(swapped);
    BOOST_CHECK_EQUAL(Reason(swapped), "bad-pos-sig");
    BOOST_CHECK(!WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(honest.GetHash())));

    // With the header: a mutated block.
    BlockValidationState header_state;
    BOOST_REQUIRE_MESSAGE(Chainman().ProcessNewBlockHeaders({{honest}}, /*min_pow_checked=*/true, header_state), header_state.ToString());
    const CBlockIndex* index{WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(honest.GetHash()))};
    BOOST_REQUIRE(index);
    for (const CBlock& bad : {swapped, zeroed}) {
        BlockValidationState state;
        BOOST_CHECK(!WITH_LOCK(::cs_main, return Chainman().AcceptBlock(std::make_shared<const CBlock>(bad), state, nullptr, true, nullptr, nullptr, true)));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-blk-extension");
        BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_MUTATED);
        Submit(bad);
        BOOST_CHECK_EQUAL(Reason(bad), "bad-blk-extension");
        LOCK(::cs_main);
        BOOST_CHECK(!(index->nStatus & BLOCK_FAILED_VALID));
        BOOST_CHECK(!(index->nStatus & BLOCK_HAVE_DATA));
        BOOST_CHECK(index->m_ext->sig == honest.ext.sig);
    }
    BOOST_CHECK(Tip() == index->pprev);
    // The block its leader signed: taken, and stored with its signature.
    Take(honest);
    CBlock read;
    BOOST_REQUIRE(Chainman().m_blockman.ReadBlock(read, *index));
    BOOST_CHECK(read.ext == honest.ext);
    // Afterwards, another signature is still a mutated block, and changes nothing.
    BlockValidationState state;
    BOOST_CHECK(!WITH_LOCK(::cs_main, return Chainman().AcceptBlock(std::make_shared<const CBlock>(swapped), state, nullptr, true, nullptr, nullptr, true)));
    BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_MUTATED);
    BOOST_CHECK(Tip() == index);
    Take(PosBlock(3));
}

BOOST_FIXTURE_TEST_CASE(bundle_fixed_by_anchored_block, FastBlocksSetup)
{
    // Design 4.3.4: the bundle is the anchored block P's -- its withdrawals, its hash as the nonce,
    // its height as the lock time -- whatever the height of the block Q that acknowledges the
    // anchor: two branches that acknowledge the same one build the same bundle.
    COutPoint coin;
    const CTxOut spent{Fund(COIN, coin)};
    StartPos();
    const CMutableTransaction withdrawal{Spend(coin, spent, {CTxOut{COIN / 2, sidechain::WithdrawalScript(1000, key.GetPubKey().GetID(), script)},
                                                             CTxOut{COIN / 2 - TX_FEE, script}})};
    Accept(withdrawal);
    Take(PosBlock(1));
    Take(PosBlock(2));
    const CBlockIndex* anchored{Tip()};
    const pos::AnchorAck ack{Anchor(Ticket(anchored->GetBlockHash(), script))};
    const auto bundle_of{[&](const CBlock& block) {
        std::optional<uint256> found;
        for (const CTxOut& out : block.vtx[0]->vout) {
            if (const auto hash{sidechain::ParseBundleCommitScript(out.scriptPubKey)}) found = hash;
        }
        return found;
    }};
    // Branch 1: Q right on P.
    const CBlock q1{PosBlock(3, ack)};
    const auto bundle1{bundle_of(q1)};
    BOOST_REQUIRE(bundle1);
    Take(q1);
    {
        LOCK(::cs_main);
        const auto pending{Chain().SideState().Bundle()};
        BOOST_REQUIRE(pending);
        BOOST_CHECK(pending->hash == *bundle1);
        BOOST_CHECK_EQUAL(pending->height, anchored->nHeight);
        BOOST_CHECK(pending->nonce == anchored->GetBlockHash());
        BOOST_CHECK_EQUAL(Assert(Chain().SideState().BundleTx())->nLockTime, static_cast<uint32_t>(anchored->nHeight));
    }
    // The step the chain's rules give Q: P's height for the peg actions.
    {
        std::optional<sidechain::PegStep> step;
        std::string reason;
        BOOST_REQUIRE_MESSAGE(Hooks().GetPegStep(q1, Record(), step, reason), reason);
        BOOST_REQUIRE(step);
        BOOST_CHECK(step->peg_height == std::optional<int32_t>{anchored->nHeight});
        BOOST_CHECK(step->max_withdrawal_height == std::optional<int32_t>{anchored->nHeight});
    }
    // Branch 2: a block of stake between P and Q.
    CBlockIndex* q1_index{WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(q1.GetHash()))};
    {
        BlockValidationState state;
        BOOST_REQUIRE(Chain().InvalidateBlock(state, q1_index));
    }
    BOOST_REQUIRE(Tip() == anchored);
    Take(PosBlock(4));
    const CBlock q2{PosBlock(5, ack)};
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return Chain().m_chain.Height()) + 1, anchored->nHeight + 2);
    BOOST_CHECK(bundle_of(q2) == bundle1);
    // A Q that commits to the bundle built at its own height (as before) is refused.
    {
        const int q2_height{anchored->nHeight + 2};
        const auto own{WITH_LOCK(::cs_main, return Chain().SideState().NextBundle(q2_height, anchored->GetBlockHash(), Chainman().GetConsensus().sidechain, nullptr, anchored->nHeight))};
        BOOST_REQUIRE(own);
        BOOST_CHECK_EQUAL(own->nLockTime, static_cast<uint32_t>(q2_height));
        BOOST_REQUIRE(own->GetHash().ToUint256() != *bundle1);
        CBlock own_height{q2};
        SetCoinbase(own_height, [&](CMutableTransaction& coinbase) {
            for (CTxOut& out : coinbase.vout) {
                if (sidechain::ParseBundleCommitScript(out.scriptPubKey)) out.scriptPubKey = sidechain::BundleCommitScript(own->GetHash().ToUint256());
            }
        });
        Resign(own_height);
        BOOST_CHECK_EQUAL(Check(own_height), "bad-sc-bundle-hash");
    }
    Take(q2);
    BOOST_CHECK(WITH_LOCK(::cs_main, return Chain().SideState().Bundle()->hash) == *bundle1);
}

BOOST_AUTO_TEST_CASE(set_boundary_and_lead)
{
    // Without a set lead (the Chains networks): the first slot of the epoch before.
    Consensus::PosParams p{CChainParams::Main()->GetConsensus().pos};
    BOOST_REQUIRE_EQUAL(p.set_lead_seconds, 0);
    for (const int64_t epoch : {1, 2, 10, 1000}) BOOST_CHECK_EQUAL(pos::SetBoundary(epoch, p), (epoch - 1) * p.slots_per_epoch);
    // With one (the eCash networks): also before the time the seed is taken at, so that no anchor at or after the slot
    // of the seed's time fixes the set; never higher for a later epoch (the pruning of the sets counts on it).
    for (const ChainType type : {ChainType::ECASH, ChainType::ECASH_BETANET, ChainType::ECASH_TESTNET}) {
        const Consensus::PosParams e{CChainParams::Ecash(type)->GetConsensus().pos};
        BOOST_CHECK_EQUAL(e.set_lead_seconds, e.seed_lookahead_seconds);
        int64_t last{std::numeric_limits<int64_t>::min()};
        for (int64_t epoch{0}; epoch < 400; ++epoch) {
            const int64_t boundary{pos::SetBoundary(epoch, e)};
            BOOST_CHECK_LE(boundary, std::max<int64_t>(0, (epoch - 1) * e.slots_per_epoch));
            if (boundary > 0) BOOST_CHECK_LE(pos::SlotTime(boundary, e), pos::SeedTime(epoch, e));
            BOOST_CHECK_GE(boundary, last);
            last = boundary;
        }
    }
}

BOOST_AUTO_TEST_CASE(staking_parameters)
{
    // The user's decisions (design, section 13): a minimum stake of 1 CHN (0.01 on the test networks,
    // 0.0001 on regtest), 4 to 100 validators (3 to 32; 1 to 8), slashing of 10 % with 1/8 to the
    // reporter, about two days to unbond. The main and test networks start with no validators: merged
    // mined only until enough stake is bonded.
    const auto main{CChainParams::Main()};
    const Consensus::PosParams& m{main->GetConsensus().pos};
    BOOST_CHECK_EQUAL(m.min_stake, COIN);
    BOOST_CHECK_EQUAL(m.min_active_validators, 4U);
    BOOST_CHECK_EQUAL(m.max_validators, 100U);
    BOOST_CHECK_EQUAL(m.slots_per_epoch, 240);
    BOOST_CHECK_EQUAL(m.slots_per_epoch * m.slot_seconds, 60 * 60);
    // Two days of anchors, one a mainchain block (a minute).
    BOOST_CHECK_EQUAL(m.unbonding_anchors, 2 * 24 * 60);
    for (const auto& params : {CChainParams::Main(), CChainParams::TestNet(), CChainParams::SigNet({})}) {
        const Consensus::PosParams& pos{params->GetConsensus().pos};
        BOOST_CHECK(pos.genesis_validators.empty());
        BOOST_CHECK(!pos::ValidatorSet{pos}.Active());
        BOOST_CHECK_EQUAL(pos.slash_bps, 1000);
        BOOST_CHECK_EQUAL(pos.reporter_share_divisor, 8);
        // A proof lands while the stake is still bonded or unbonding (the eCash networks: sidechain_mainclient_tests).
        BOOST_CHECK_GE(pos.unbonding_anchors, 2 * pos.evidence_window_anchors);
        // The stake is fixed an epoch before the seed exists (the seed block is mined some minutes after
        // its median time, which is at most an epoch before the epoch starts).
        BOOST_CHECK_LE(pos.seed_lookahead_seconds, pos.slots_per_epoch * pos.slot_seconds);
    }
    for (const auto& params : {CChainParams::TestNet(), CChainParams::SigNet({})}) {
        const Consensus::PosParams& pos{params->GetConsensus().pos};
        BOOST_CHECK_EQUAL(pos.min_stake, COIN / 100);
        BOOST_CHECK_EQUAL(pos.min_active_validators, 3U);
        BOOST_CHECK_EQUAL(pos.max_validators, 32U);
        // Ten minutes (20 slots of 30 seconds before 2026-10-16).
        BOOST_CHECK_EQUAL(pos.slots_per_epoch, 40);
        BOOST_CHECK_EQUAL(pos.slots_per_epoch * pos.slot_seconds, 10 * 60);
    }
    CChainParams::RegTestOptions options;
    options.sidechain_slot = 5;
    const auto regtest{CChainParams::RegTest(options)};
    const Consensus::PosParams& reg{regtest->GetConsensus().pos};
    BOOST_CHECK_EQUAL(reg.min_stake, COIN / 10000);
    BOOST_CHECK_EQUAL(reg.min_active_validators, 1U);
    BOOST_CHECK_EQUAL(reg.max_validators, 8U);
    BOOST_CHECK_EQUAL(reg.slots_per_epoch, 20);
    BOOST_CHECK_EQUAL(reg.unbonding_anchors, 20);
}

BOOST_AUTO_TEST_CASE(staking_scripts)
{
    ECC_Context ecc;
    CKey validator, owner;
    validator.MakeNewKey(/*fCompressed=*/true);
    owner.MakeNewKey(/*fCompressed=*/true);
    const uint160 owner_hash{owner.GetPubKey().GetID()};
    const uint256 genesis{1};
    std::array<unsigned char, 64> pop;
    BOOST_REQUIRE(validator.SignSchnorr(pos::BondPopHash(owner_hash, genesis), pop, nullptr, uint256{}));
    // A bond reads back as written; the proof of possession is for one owner, on one chain.
    const CTxOut bond_out{5000, pos::BondScript(XOnly(validator), owner_hash, pop)};
    const auto bond{pos::ParseBond(bond_out)};
    BOOST_REQUIRE(bond);
    BOOST_CHECK(bond->pubkey == XOnly(validator) && bond->owner == owner_hash && bond->pop == pop && bond->amount == 5000);
    BOOST_CHECK(pos::IsBondScript(bond_out.scriptPubKey) && !pos::IsUnbondScript(bond_out.scriptPubKey));
    BOOST_CHECK(pos::CheckBondPop(XOnly(validator), owner_hash, pop, genesis));
    BOOST_CHECK(!pos::CheckBondPop(XOnly(validator), uint160{}, pop, genesis));
    BOOST_CHECK(!pos::CheckBondPop(XOnly(validator), owner_hash, pop, uint256{2}));
    BOOST_CHECK(!pos::CheckBondPop(XOnly(owner), owner_hash, pop, genesis));
    // A script with the tag claims to be a bond, read or not.
    const CScript truncated{CScript() << OP_RETURN << std::vector<unsigned char>{0xDB, 'S', 'T', 'K', 1, 2, 3}};
    BOOST_CHECK(pos::IsBondScript(truncated));
    BOOST_CHECK(!pos::ParseBond(CTxOut{1, truncated}));
    // An unbond reads back; an amount above MAX_MONEY does not read.
    pos::Unbond unbond;
    unbond.pubkey = XOnly(validator);
    unbond.amount = 4000;
    unbond.sequence = 7;
    const COutPoint first_bond{Txid::FromUint256(uint256{5}), 1};
    BOOST_REQUIRE(owner.SignCompact(MessageHash(pos::UnbondMessage(unbond.pubkey, unbond.amount, unbond.sequence, first_bond, uint256{4}, genesis)), unbond.signature));
    const auto read{pos::ParseUnbond(CTxOut{0, pos::UnbondScript(unbond)})};
    BOOST_REQUIRE(read);
    BOOST_CHECK(read->pubkey == unbond.pubkey && read->amount == 4000 && read->sequence == 7 && read->signature == unbond.signature);
    BOOST_CHECK(pos::IsUnbondScript(pos::UnbondScript(unbond)));
    pos::Unbond huge{unbond};
    huge.amount = MAX_MONEY + 1;
    BOOST_CHECK(!pos::ParseUnbond(CTxOut{0, pos::UnbondScript(huge)}));
    // The message names the validator, the amount, the sequence and the chain.
    BOOST_CHECK(pos::UnbondMessage(unbond.pubkey, 4000, 7, first_bond, uint256{4}, genesis) != pos::UnbondMessage(unbond.pubkey, 4000, 8, first_bond, uint256{4}, genesis));
    BOOST_CHECK(pos::UnbondMessage(unbond.pubkey, 4000, 7, first_bond, uint256{4}, genesis) != pos::UnbondMessage(unbond.pubkey, 4000, 7, first_bond, uint256{4}, uint256{2}));
    // ... and the first coin its first bond spent (audit 7, finding 3: not replayable on another chain
    // of the same genesis, whose validator bonded with another coin).
    BOOST_CHECK(pos::UnbondMessage(unbond.pubkey, 4000, 7, first_bond, uint256{4}, genesis) != pos::UnbondMessage(unbond.pubkey, 4000, 7, COutPoint{Txid::FromUint256(uint256{5}), 2}, uint256{4}, genesis));
    BOOST_CHECK(pos::UnbondMessage(unbond.pubkey, 4000, 7, first_bond, uint256{4}, genesis).find(first_bond.hash.GetHex() + ":1 ") != std::string::npos);
    // ... and the anchor block of the chain when that bond was applied (the same coin on another chain).
    BOOST_CHECK(pos::UnbondMessage(unbond.pubkey, 4000, 7, first_bond, uint256{4}, genesis) != pos::UnbondMessage(unbond.pubkey, 4000, 7, first_bond, uint256{6}, genesis));
    // A bond with the owner's signature reads back; one with a signature of another size does not.
    const std::vector<unsigned char> owner_sig(CPubKey::COMPACT_SIGNATURE_SIZE, 3);
    const auto signed_bond{pos::ParseBond(CTxOut{5000, pos::BondScript(XOnly(validator), owner_hash, pop, owner_sig)})};
    BOOST_REQUIRE(signed_bond);
    BOOST_CHECK(signed_bond->owner_sig == owner_sig && signed_bond->pop == pop && bond->owner_sig.empty());
    BOOST_CHECK(!pos::ParseBond(CTxOut{5000, pos::BondScript(XOnly(validator), owner_hash, pop, std::vector<unsigned char>(64, 3))}));
    // The owner's message names the validator, the amount, the coin spent and the chain.
    const COutPoint coin{Txid::FromUint256(uint256{5}), 1};
    const std::string bond_message{pos::BondMessage(unbond.pubkey, 4000, coin, genesis)};
    BOOST_CHECK(bond_message != pos::BondMessage(unbond.pubkey, 4001, coin, genesis));
    BOOST_CHECK(bond_message != pos::BondMessage(unbond.pubkey, 4000, COutPoint{coin.hash, 2}, genesis));
    BOOST_CHECK(bond_message != pos::BondMessage(unbond.pubkey, 4000, coin, uint256{2}));
    BOOST_CHECK(bond_message != pos::BondMessage(XOnly(owner), 4000, coin, genesis));
    // An unbond of all says so, and reads back.
    BOOST_CHECK(pos::UnbondMessage(unbond.pubkey, pos::UNBOND_ALL, 7, first_bond, uint256{4}, genesis).find(" all ") != std::string::npos);
    pos::Unbond all{unbond};
    all.amount = pos::UNBOND_ALL;
    BOOST_CHECK_EQUAL(pos::ParseUnbond(CTxOut{0, pos::UnbondScript(all)})->amount, pos::UNBOND_ALL);
    // The penalty: 10 %, in integers, without overflow.
    Consensus::PosParams params;
    const pos::Staking staking{params, genesis};
    BOOST_CHECK_EQUAL(staking.Penalty(100000), 10000);
    BOOST_CHECK_EQUAL(staking.Penalty(9), 0);
    BOOST_CHECK_EQUAL(staking.Penalty(MAX_MONEY), MAX_MONEY / 10);
}

BOOST_AUTO_TEST_CASE(leader_seed)
{
    // The seed of an epoch: the hash of the highest mainchain block whose median time is at least
    // seed_lookahead_seconds before the epoch starts; known once a later block is on record.
    Consensus::PosParams params;
    params.genesis_time = 1000;
    params.slot_seconds = 30;
    params.slots_per_epoch = 120;
    params.seed_lookahead_seconds = 1200;
    sidechain::Mainchain record;
    BOOST_CHECK(!pos::SeedHeight(1, params, record));
    const auto append{[&](int height, uint8_t salt = 0) {
        sidechain::MainBlock block;
        block.hash = uint256{static_cast<uint8_t>(height + salt)};
        block.prev_hash = record.TipHash();
        block.time = 1000 + 60 * height;
        BOOST_REQUIRE(record.Append(block));
    }};
    // Epoch 1 starts at 1000 + 3600; its seed is taken at 3400: block 40.
    BOOST_CHECK_EQUAL(pos::SeedTime(1, params), 3400);
    for (int height{0}; height <= 40; ++height) append(height);
    BOOST_CHECK(!pos::SeedHeight(1, params, record));
    BOOST_CHECK(!pos::EpochSeed(1, params, record));
    append(41);
    BOOST_CHECK(pos::SeedHeight(1, params, record) == std::optional<int>{40});
    BOOST_CHECK(pos::EpochSeed(1, params, record) == pos::ValidatorSet::Seed(1, uint256{40}));
    // Before the first block on record: a null hash.
    BOOST_CHECK(pos::SeedHeight(0, params, record) == std::optional<int>{-1});
    BOOST_CHECK(pos::EpochSeed(0, params, record) == pos::ValidatorSet::Seed(0, uint256{}));
    // A mainchain reorg that replaces the block moves the seed.
    record.Truncate(39);
    append(40, 100);
    append(41, 100);
    BOOST_CHECK(pos::EpochSeed(1, params, record) == pos::ValidatorSet::Seed(1, uint256{140}));
    BOOST_CHECK(pos::ValidatorSet::Seed(1, uint256{40}) != pos::ValidatorSet::Seed(2, uint256{40}));
}

BOOST_FIXTURE_TEST_CASE(bonds, FastBlocksSetup)
{
    StartPos();
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    CKey validator, stranger;
    validator.MakeNewKey(/*fCompressed=*/true);
    stranger.MakeNewKey(/*fCompressed=*/true);
    const uint256 pubkey{XOnly(validator)};
    const CAmount min{Pos().min_stake};
    // The mempool (and blocks) refuse a bond whose proof of possession another key signed.
    {
        const auto signed_by_stranger{*pos::ParseBond(BondOut(stranger, 2 * min, Owner(key)))};
        COutPoint c{coin};
        CTxOut s{spent};
        BOOST_CHECK_EQUAL(Refused(Pay(c, s, {CTxOut{2 * min, pos::BondScript(pubkey, Owner(key), signed_by_stranger.pop)}})), "bad-pos-bond-pop");
    }
    // A bond: the coins leave circulation into the state, the key's owner is the bond's.
    Accept(Pay(coin, spent, {BondOut(validator, 2 * min, Owner(key), &key, coin)}));
    const CBlock block{PosBlock(1)};
    BOOST_REQUIRE_EQUAL(block.vtx.size(), 2U);
    Take(block);
    auto record{Validator(pubkey)};
    BOOST_REQUIRE(record);
    BOOST_CHECK(record->owner == Owner(key));
    BOOST_CHECK_EQUAL(record->bonded, 2 * min);
    BOOST_CHECK_EQUAL(record->first_bond_height, Tip()->nHeight);
    BOOST_CHECK(record->Eligible(Pos()));
    BOOST_CHECK_EQUAL(Stake().bonded, 2 * min);
    // More, by the same owner, who signs them (the amount and the coin the transaction spends first).
    Accept(Pay(coin, spent, {BondOut(validator, min, Owner(key), &key, coin)}));
    Take(PosBlock(2));
    BOOST_CHECK_EQUAL(Validator(pubkey)->bonded, 3 * min);
    BOOST_CHECK_EQUAL(Stake().bonded, 3 * min);
    // The rules.
    BOOST_CHECK_EQUAL(StakingRules(TxWith({BondOut(validator, min, Owner(stranger))})), "bad-pos-bond-owner");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({BondOut(validator, min, uint160{})})), "bad-pos-bond-owner");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({BondOut(validator, 0, Owner(key))})), "bad-pos-bond-amount");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({BondOut(validator, MAX_MONEY + 1, Owner(key))})), "bad-pos-bond-amount");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({CTxOut{min, CScript() << OP_RETURN << std::vector<unsigned char>{0xDB, 'S', 'T', 'K', 1}}})), "bad-pos-bond");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({BondOut(validator, min, Owner(key), &key)})), "");
    // Without the owner's signature, or with another's, or with one for another coin or amount: refused.
    BOOST_CHECK_EQUAL(StakingRules(TxWith({BondOut(validator, min, Owner(key))})), "bad-pos-bond-owner-sig");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({BondOut(validator, min, Owner(key), &stranger)})), "bad-pos-bond-owner-sig");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({BondOut(validator, min, Owner(key), &key, coin)})), "bad-pos-bond-owner-sig");
    {
        CTxOut other_amount{BondOut(validator, min, Owner(key), &key)};
        other_amount.nValue = min + 1;
        BOOST_CHECK_EQUAL(StakingRules(TxWith({other_amount})), "bad-pos-bond-owner-sig");
    }
    // A smaller bond is taken, but the key takes part only with min_stake.
    Accept(Pay(coin, spent, {BondOut(stranger, min - 1, Owner(key), &key, coin)}));
    Take(PosBlock(3));
    BOOST_CHECK_EQUAL(Validator(XOnly(stranger))->bonded, min - 1);
    BOOST_CHECK(!Validator(XOnly(stranger))->Eligible(Pos()));
    // The sets made at anchors: the next anchor's set has the bonded key, with its stake as weight
    // (and the genesis validators); used from the epoch after the next.
    MineBmm(BmmBlock(/*use_mempool=*/false));
    const int64_t epoch{pos::EpochOf(pos::SlotOf(Tip()->nTime, Pos()), Pos()) + 2};
    const auto set{WITH_LOCK(::cs_main, return Hooks().SetFor(*Tip(), epoch))};
    BOOST_REQUIRE(set);
    const auto index{set->Find(pubkey)};
    BOOST_REQUIRE(index);
    BOOST_CHECK_EQUAL(set->Validators()[*index].stake, 3 * min);
    BOOST_CHECK(!set->Find(XOnly(stranger)));
    BOOST_CHECK_EQUAL(set->Validators().size(), 4U);
    // The set of the epoch before is still the genesis set.
    const auto before{WITH_LOCK(::cs_main, return Hooks().SetFor(*Tip(), epoch - 1))};
    BOOST_REQUIRE(before);
    BOOST_CHECK(!before->Find(pubkey));
}

BOOST_FIXTURE_TEST_CASE(unbonds, FastBlocksSetup)
{
    StartPos();
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    CKey validator, stranger;
    validator.MakeNewKey(/*fCompressed=*/true);
    stranger.MakeNewKey(/*fCompressed=*/true);
    const uint256 pubkey{XOnly(validator)};
    const CAmount min{Pos().min_stake};
    Accept(Pay(coin, spent, {BondOut(validator, 3 * min, Owner(key), &key, coin)}));
    Take(PosBlock(1));
    // The rules.
    BOOST_CHECK_EQUAL(StakingRules(TxWith({CTxOut{0, CScript() << OP_RETURN << std::vector<unsigned char>{0xDC, 'U', 'N', 'B', 1}}})), "bad-pos-unbond");
    CTxOut valued{UnbondOut(validator, min, 0, key)};
    valued.nValue = 1;
    BOOST_CHECK_EQUAL(StakingRules(TxWith({valued})), "bad-pos-unbond-value");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({UnbondOut(stranger, min, 0, key)})), "bad-pos-unbond-unknown");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({UnbondOut(validator, min, 1, key)})), "bad-pos-unbond-sequence");
    // (UNBOND_ALL, 0: all that is bonded.)
    BOOST_CHECK_EQUAL(StakingRules(TxWith({UnbondOut(validator, pos::UNBOND_ALL, 0, key)})), "");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({UnbondOut(validator, 3 * min + 1, 0, key)})), "bad-pos-unbond-amount");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({UnbondOut(validator, 3 * min - min / 2, 0, key)})), "bad-pos-unbond-remainder");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({UnbondOut(validator, min, 0, stranger)})), "bad-pos-unbond-sig");
    {
        COutPoint c{coin};
        CTxOut s{spent};
        BOOST_CHECK_EQUAL(Refused(Pay(c, s, {UnbondOut(validator, min, 0, stranger)})), "bad-pos-unbond-sig");
    }
    // An unbond: off the stake at once, paid unbonding_anchors anchors later to the owner's key.
    const uint64_t anchors{Fast().anchors};
    Accept(Pay(coin, spent, {UnbondOut(validator, min, 0, key)}));
    Take(PosBlock(2));
    auto record{*Validator(pubkey)};
    BOOST_CHECK_EQUAL(record.bonded, 2 * min);
    BOOST_CHECK_EQUAL(record.unbond_seq, 1U);
    BOOST_REQUIRE_EQUAL(record.unbonding.size(), 1U);
    BOOST_CHECK(record.unbonding[0] == (pos::UnbondingEntry{.release_anchor = anchors + Pos().unbonding_anchors, .sequence = 0, .amount = min}));
    BOOST_CHECK_EQUAL(Stake().bonded, 2 * min);
    BOOST_CHECK_EQUAL(Stake().unbonding, min);
    // Not twice.
    BOOST_CHECK_EQUAL(StakingRules(TxWith({UnbondOut(validator, min, 0, key)})), "bad-pos-unbond-sequence");
    // All that is left: the key no longer takes part.
    Accept(Pay(coin, spent, {UnbondOut(validator, 2 * min, 1, key)}));
    Take(PosBlock(3));
    BOOST_CHECK(!Validator(pubkey)->Eligible(Pos()));
    // Nothing bonded: nothing to unbond, all of it or not.
    BOOST_CHECK_EQUAL(StakingRules(TxWith({UnbondOut(validator, pos::UNBOND_ALL, 2, key)})), "bad-pos-unbond-amount");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({UnbondOut(validator, 1, 2, key)})), "bad-pos-unbond-amount");
    BOOST_CHECK_EQUAL(Stake().unbonding, 3 * min);
    // Paid at the anchor it is due, through the payouts of the peg actions: not before.
    const CTxOut first{min, script}, second{2 * min, script};
    while (Fast().anchors + 1 < anchors + Pos().unbonding_anchors) {
        const CBlock bmm{BmmBlock(/*use_mempool=*/false)};
        BOOST_CHECK(!Pays(bmm, first) && !Pays(bmm, second));
        MineBmm(bmm);
    }
    const CBlock paying{BmmBlock(/*use_mempool=*/false)};
    BOOST_CHECK(Pays(paying, first));
    BOOST_CHECK(Pays(paying, second));
    MineBmm(paying);
    BOOST_CHECK_EQUAL(Stake().unbonding, 0);
    BOOST_CHECK_EQUAL(Stake().bonded, 0);
    BOOST_CHECK(Validator(pubkey)->unbonding.empty());
}

BOOST_FIXTURE_TEST_CASE(staking_starts_pos, NoValidatorsSetup)
{
    // The main and test networks start so: no validators, merged mined blocks only (with
    // transactions), until enough stake is bonded. Then blocks of stake from the epoch after next.
    BOOST_CHECK(!Hooks().PosActiveNow());
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    const CKey& validator{keys[0]};
    Accept(Pay(coin, spent, {BondOut(validator, Pos().min_stake, Owner(key), &key, coin)}));
    const CBlock bonding{BmmBlock()};
    BOOST_REQUIRE_EQUAL(bonding.vtx.size(), 2U);
    MineBmm(bonding);
    // The anchor after it makes a set with it.
    MineBmm(BmmBlock(/*use_mempool=*/false));
    const int64_t epoch{pos::EpochOf(pos::SlotOf(Tip()->nTime, Pos()), Pos()) + 2};
    std::string error;
    {
        const int64_t slot{(epoch - 1) * Pos().slots_per_epoch};
        Mock(pos::SlotTime(slot, Pos()));
        BOOST_CHECK(!Hooks().PosActiveNow());
        BOOST_CHECK(!producer->Produce(slot, error));
        BOOST_CHECK_EQUAL(error, "no blocks of stake in this epoch: too few validators");
    }
    const int64_t slot{epoch * Pos().slots_per_epoch};
    Mock(pos::SlotTime(slot, Pos()));
    BOOST_CHECK(Hooks().PosActiveNow());
    const auto made{producer->Produce(slot, error)};
    BOOST_REQUIRE_MESSAGE(made, error);
    BOOST_CHECK(Tip()->GetBlockHash() == *made);
    BOOST_CHECK(pos::CheckBlockSig(Tip()->GetBlockHeader(), XOnly(validator), Genesis()));
    // Off again once the stake leaves: the set an anchor makes then is empty.
    Accept(Pay(coin, spent, {UnbondOut(validator, Pos().min_stake, 0, key)}));
    Mock(pos::SlotTime(slot + 1, Pos()));
    BOOST_REQUIRE_MESSAGE(producer->Produce(slot + 1, error), error);
    BOOST_CHECK_EQUAL(Validator(XOnly(validator))->bonded, 0);
    MineBmm(BmmBlock(/*use_mempool=*/false));
    const int64_t off{pos::EpochOf(pos::SlotOf(Tip()->nTime, Pos()), Pos()) + 2};
    Mock(pos::SlotTime(off * Pos().slots_per_epoch, Pos()));
    BOOST_CHECK(!Hooks().PosActiveNow());
    BOOST_CHECK(!producer->Produce(off * Pos().slots_per_epoch, error));
    BOOST_CHECK_EQUAL(error, "no blocks of stake in this epoch: too few validators");
}

BOOST_AUTO_TEST_CASE(jailing)
{
    // A validator that made less than half the blocks it led in an epoch (and led at least ten) is
    // left out of the sets made in the next epochs, jail_epochs more; it keeps its coins.
    ECC_Context ecc;
    std::vector<CKey> validators(3);
    for (CKey& k : validators) k.MakeNewKey(/*fCompressed=*/true);
    Consensus::PosParams params;
    params.slots_per_epoch = 20;
    params.min_active_validators = 1;
    params.max_validators = 8;
    params.min_stake = 1000;
    const uint256 genesis{1};
    CKey owner_key;
    owner_key.MakeNewKey(/*fCompressed=*/true);
    const uint160 owner{owner_key.GetPubKey().GetID()};
    const pos::Staking staking{params, genesis};
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay out{empty};
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{1}), 0});
    for (const CKey& k : validators) {
        std::array<unsigned char, 64> pop;
        BOOST_REQUIRE(k.SignSchnorr(pos::BondPopHash(owner, genesis), pop, nullptr, uint256{}));
        std::vector<unsigned char> owner_sig;
        BOOST_REQUIRE(owner_key.SignCompact(MessageHash(pos::BondMessage(XOnly(k), 5000, tx.vin[0].prevout, genesis)), owner_sig));
        tx.vout.emplace_back(5000, pos::BondScript(XOnly(k), owner, pop, owner_sig));
    }
    std::string reason;
    BOOST_REQUIRE(staking.ApplyTx(out, CTransaction{tx}, 1, 0, reason, uint256{9}));
    const uint256 a{XOnly(validators[0])}, b{XOnly(validators[1])}, c{XOnly(validators[2])};
    // Epoch 0: A made its ten; B led twelve and made none; C led four and made none (too few to tell).
    for (int64_t slot{0}; slot < 10; ++slot) staking.NoteLiveness(out, slot, a, {});
    std::vector<std::pair<int64_t, uint256>> missed;
    for (int64_t slot{10}; slot < 18; ++slot) missed.emplace_back(slot, b);
    for (int64_t slot{0}; slot < 4; ++slot) missed.emplace_back(slot, b);
    for (int64_t slot{0}; slot < 4; ++slot) missed.emplace_back(slot, c);
    staking.NoteLiveness(out, 19, a, missed);
    BOOST_CHECK(pos::GetLiveness(out, 0, a) == (pos::Liveness{.assigned = 11, .produced = 11}));
    BOOST_CHECK(pos::GetLiveness(out, 0, b) == (pos::Liveness{.assigned = 12, .produced = 0}));
    // The first anchor of epoch 1 jails B; its set leaves B out.
    std::vector<CTxOut> owed;
    const auto made{staking.PegStep(out, uint256{0xa1}, 1, 20, 1, 1, owed)};
    BOOST_CHECK(made.set_active);
    BOOST_CHECK(owed.empty());
    BOOST_CHECK_EQUAL(pos::GetValidator(out, b)->jailed_until_epoch, 3);
    BOOST_CHECK_EQUAL(pos::GetValidator(out, a)->jailed_until_epoch, 0);
    BOOST_CHECK_EQUAL(pos::GetValidator(out, c)->jailed_until_epoch, 0);
    BOOST_CHECK(!pos::GetLiveness(out, 0, b));
    const auto set{pos::GetSetVersion(out, pos::GetAnchorSet(out, uint256{0xa1})->version)};
    BOOST_REQUIRE(set);
    BOOST_CHECK_EQUAL(set->size(), 2U);
    BOOST_CHECK(!pos::ValidatorSet(*set, params).Find(b));
    // No coins lost.
    BOOST_CHECK_EQUAL(pos::GetValidator(out, b)->bonded, 5000);
    BOOST_CHECK_EQUAL(pos::GetStakeState(out).bonded, 15000);
    // Out of the sets made in epoch 2 too; back in those of epoch 3.
    BOOST_CHECK(!staking.MakeSet(out, 2).Find(b));
    BOOST_CHECK(staking.MakeSet(out, 3).Find(b));
    staking.PegStep(out, uint256{0xa2}, 2, 60, 3, 3, owed);
    BOOST_CHECK(pos::ValidatorSet(*pos::GetSetVersion(out, pos::GetAnchorSet(out, uint256{0xa2})->version), params).Find(b));
}

BOOST_AUTO_TEST_CASE(jailing_keeps_active)
{
    // From jail_fix_height (keep_active): no liveness jails that would leave the set with fewer than
    // min_active_validators; before it, they were given (betanet slot 99: every genesis validator jailed).
    ECC_Context ecc;
    std::vector<CKey> validators(3);
    for (CKey& k : validators) k.MakeNewKey(/*fCompressed=*/true);
    const auto run{[&](uint32_t min_active, bool keep_active, int missing) {
        Consensus::PosParams params;
        params.slots_per_epoch = 20;
        params.min_active_validators = min_active;
        params.max_validators = 8;
        for (const CKey& k : validators) params.genesis_validators.push_back({.pubkey = XOnly(k), .stake = 1});
        const pos::Staking staking{params, uint256{1}};
        sidechain::EmptyStore empty;
        sidechain::StoreOverlay out{empty};
        staking.LoadGenesis(out);
        // Epoch 0: the first `missing` validators led twelve slots each and made none; the others made theirs.
        std::vector<std::pair<int64_t, uint256>> missed;
        for (int i{0}; i < 3; ++i) {
            for (int64_t slot{0}; slot < 12; ++slot) {
                if (i < missing) {
                    missed.emplace_back(slot, XOnly(validators[i]));
                } else {
                    staking.NoteLiveness(out, slot, XOnly(validators[i]), {});
                }
            }
        }
        staking.NoteLiveness(out, 19, XOnly(validators[2]), missed);
        std::vector<CTxOut> owed;
        const auto made{staking.PegStep(out, uint256{0xa1}, 1, 20, 1, 1, owed, keep_active)};
        int jailed{0};
        for (const CKey& k : validators) jailed += pos::GetValidator(out, XOnly(k))->jailed_until_epoch > 0;
        // The liveness of the epoch is dropped either way.
        BOOST_CHECK(!pos::GetLiveness(out, 0, XOnly(validators[0])));
        return std::make_pair(jailed, made.set_active);
    }};
    // All three missed theirs, three needed: before the fix all are jailed and blocks of stake stop; with it none.
    BOOST_CHECK((run(3, /*keep_active=*/false, 3) == std::make_pair(3, false)));
    BOOST_CHECK((run(3, /*keep_active=*/true, 3) == std::make_pair(0, true)));
    // One of three missed, three needed: not jailed with the fix.
    BOOST_CHECK((run(3, /*keep_active=*/true, 1) == std::make_pair(0, true)));
    // One of three missed, two needed: jailed (two are left), with the fix as before it.
    BOOST_CHECK((run(2, /*keep_active=*/true, 1) == std::make_pair(1, true)));
    BOOST_CHECK((run(2, /*keep_active=*/false, 1) == std::make_pair(1, true)));
    // Two of three missed, two needed: neither jailed (all or none).
    BOOST_CHECK((run(2, /*keep_active=*/true, 2) == std::make_pair(0, true)));
}

BOOST_FIXTURE_TEST_CASE(liveness_first_block_late, JailFixSetup)
{
    // The live case of betanet slot 99: the first block of stake well after the slot the chain could start at.
    // With the fix, it charges nobody for the slots before it: itself only.
    LateFirstBlock();
    BOOST_CHECK((Counted() == std::make_pair(1U, 1U)));
    // And the next ones as before: the slots since the last block of stake.
    Take(PosBlock(3 * Pos().slots_per_epoch + 3));
    BOOST_CHECK((Counted() == std::make_pair(4U, 2U)));
}

BOOST_FIXTURE_TEST_CASE(liveness_first_block_late_before_fix, JailFixLaterSetup)
{
    // Before jail_fix_height, the rules as they were: the first block of stake charges an epoch of slots
    // before it (every slot of it had a leader: three genesis validators, one needed).
    LateFirstBlock();
    BOOST_CHECK((Counted() == std::make_pair(static_cast<uint32_t>(Pos().slots_per_epoch + 1), 1U)));
}

BOOST_FIXTURE_TEST_CASE(liveness_noted, FastBlocksSetup)
{
    // Each block of stake: its leader made one; the leaders of the slots since the last one (an epoch
    // of them at most) made none.
    StartPos();
    Take(PosBlock(1));
    Take(PosBlock(4));
    const auto epoch{pos::EpochOf(base_slot, Pos())};
    uint32_t assigned{0}, produced{0};
    LOCK(::cs_main);
    for (int64_t e{epoch - 1}; e <= epoch + 1; ++e) {
        for (const CKey& k : keys) {
            const auto liveness{pos::GetLiveness(Chain().SideState().View(), e, XOnly(k))};
            if (!liveness) continue;
            assigned += liveness->assigned;
            produced += liveness->produced;
        }
    }
    // The first block of stake: itself (the slots up to the BMM block it follows are nobody's to
    // fill); the second: two slots and itself.
    BOOST_CHECK_EQUAL(assigned, static_cast<uint32_t>(1 + 2 + 1));
    BOOST_CHECK_EQUAL(produced, 2U);
}

BOOST_FIXTURE_TEST_CASE(slashing, FastBlocksSetup)
{
    StartPos();
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    const CAmount bond{100000};
    // Every validator bonds.
    Accept(Pay(coin, spent, {BondOut(keys[0], bond, Owner(key), &key, coin), BondOut(keys[1], bond, Owner(key), &key, coin), BondOut(keys[2], bond, Owner(key), &key, coin)}));
    const CBlock bonding{PosBlock(1)};
    Take(bonding);
    BOOST_CHECK_EQUAL(Stake().bonded, 3 * bond);
    // The leader of slot 2 signs two blocks for it; the first is taken.
    const auto [first, second]{TwoBlocks(2)};
    const pos::DoubleProposal proof{.a = first, .b = second};
    const uint256 offender{XOnly(Leader(base_slot + 2))};
    const CKey& other{NotLeader(base_slot + 2)};
    Take(first);

    // Proofs the chain refuses (asked of the rules directly: blocks of one slot given to validation
    // would be proofs that its leader signed them all). One of a slot after the block's.
    const sidechain::StoreView& view{ActiveView()};
    {
        const auto [x, y]{TwoBlocks(4)};
        BOOST_CHECK_EQUAL(After(WithEvidence(3, {{.a = x, .b = y}}), view), "bad-pos-evidence-late");
    }
    // Not signed by the slot's leader.
    {
        CBlock a{first}, b{second};
        Resign(a, &other);
        Resign(b, &other);
        BOOST_CHECK_EQUAL(After(WithEvidence(3, {{.a = a, .b = b}}), view), "bad-pos-evidence-sig");
    }
    // Not of the form of one (context free, CheckBlock): two blocks of stake of one slot, different.
    const auto form{[&](const CBlock& block) {
        BlockValidationState state;
        return Hooks().CheckBlock(block, state) ? std::string{} : state.GetRejectReason();
    }};
    BOOST_CHECK_EQUAL(form(WithEvidence(3, {{.a = first, .b = first}})), "bad-pos-evidence-same");
    BOOST_CHECK_EQUAL(form(WithEvidence(3, {{.a = first, .b = bonding}})), "bad-pos-evidence-slot");
    BOOST_CHECK_EQUAL(form(WithEvidence(3, {{.a = first, .b = BmmBlock(/*use_mempool=*/false)}})), "bad-pos-evidence-kind");
    // At most MAX_SECTION_EVIDENCE in a block.
    BOOST_CHECK_EQUAL(form(WithEvidence(3, std::vector<pos::DoubleProposal>(pos::MAX_SECTION_EVIDENCE + 1, proof))), "bad-pos-sections");
    BOOST_CHECK_EQUAL(form(WithEvidence(3, std::vector<pos::DoubleProposal>(pos::MAX_SECTION_EVIDENCE, proof))), "");
    // Two in a block for one offence.
    BOOST_CHECK_EQUAL(After(WithEvidence(3, {proof, proof}), view), "bad-pos-evidence-known");
    BOOST_CHECK_EQUAL(After(WithEvidence(3, {proof}), view), "");

    // The proof, in the block of slot 3 (as the producer puts the proofs it knows in): the leader loses
    // 10 % of its stake and is banned, the rest is unbonded; the block is owed 1/8 of the penalty.
    std::string reason;
    BOOST_REQUIRE_MESSAGE(Hooks().AddEvidence(proof, reason), reason);
    const int64_t slot3{Slot(3)};
    std::string error;
    // (The producer's key for slot 3 is one of the fixture's.)
    BOOST_REQUIRE_MESSAGE(producer->Produce(slot3, error), error);
    CBlock carrying;
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return Chainman().m_blockman.ReadBlock(carrying, *Tip())));
    pos::PosSections sections;
    BOOST_REQUIRE(pos::PosSections::Decode(carrying.vSections, sections));
    BOOST_REQUIRE_EQUAL(sections.evidence.size(), 1U);
    const uint256 carrying_hash{carrying.GetHash()};
    const CAmount penalty{bond / 10};
    const auto record{Validator(offender)};
    BOOST_REQUIRE(record);
    BOOST_CHECK(record->tombstoned);
    BOOST_CHECK_EQUAL(record->bonded, 0);
    BOOST_CHECK_EQUAL(record->Unbonding(), bond - penalty);
    BOOST_CHECK(!record->Eligible(Pos()));
    BOOST_CHECK_EQUAL(Stake().bonded, 2 * bond);
    BOOST_CHECK_EQUAL(Stake().unbonding, bond - penalty);
    BOOST_CHECK_EQUAL(Stake().rewards_owed, penalty / 8);
    BOOST_CHECK_EQUAL(Stake().pot_reserve, penalty - penalty / 8);
    BOOST_CHECK_EQUAL(Stake().slashed, penalty);
    // Every coin still accounted for.
    BOOST_CHECK_EQUAL(Stake().bonded + Stake().unbonding + Stake().pot_reserve + Stake().rewards_owed, 3 * bond);
    // Not twice; and no more bonds of its key.
    BOOST_CHECK_EQUAL(After(WithEvidence(4, {proof}), view), "bad-pos-evidence-known");
    for (const CKey& k : keys) {
        if (XOnly(k) == offender) BOOST_CHECK_EQUAL(StakingRules(TxWith({BondOut(k, bond, Owner(key))})), "bad-pos-bond-tombstoned");
    }
    // The producer does not put it in again.
    BOOST_REQUIRE_MESSAGE(producer->Produce(Slot(4), error), error);
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return Chainman().m_blockman.ReadBlock(carrying, *Tip())));
    BOOST_REQUIRE(pos::PosSections::Decode(carrying.vSections, sections));
    BOOST_CHECK(sections.evidence.empty());

    // At the next peg step: the reward, to the script of the first output of the block that carried
    // the proof; a share of the reserve, with the pot, to the anchor's payee.
    const CScript reporter{GetScriptForDestination(WitnessV0KeyHash{key.GetPubKey()})};
    const CBlock bmm{BmmBlock(/*use_mempool=*/false)};
    BOOST_CHECK(Pays(bmm, CTxOut{penalty / 8, reporter}));
    const CAmount due{Fast().pot_accrued - Fast().pot_paid};
    const CAmount reserve{penalty - penalty / 8};
    const CAmount release{reserve / Pos().pot_reserve_anchors + (reserve % Pos().pot_reserve_anchors != 0)};
    BOOST_CHECK(Pays(bmm, CTxOut{due + release, BmmPayee()}));
    MineBmm(bmm);
    BOOST_CHECK_EQUAL(Stake().rewards_owed, 0);
    // What a wallet shows of them (not consensus): the reward to the reporter, the pot to the payee;
    // the first output of the block of stake that carried the proof, its producer's share.
    {
        using Role = interfaces::Chain::CoinbaseRole;
        const auto chain{interfaces::MakeChain(m_node)};
        const auto roles{chain->getCoinbaseRoles(bmm.GetHash())};
        const auto& vout{bmm.vtx[0]->vout};
        BOOST_REQUIRE_EQUAL(roles.size(), vout.size());
        size_t rewards{0}, pots{0};
        for (size_t i{0}; i < vout.size(); ++i) {
            const Role expected{i > 0 && vout[i] == CTxOut{penalty / 8, reporter} ? Role::REPORTER :
                                i > 0 && vout[i] == CTxOut{due + release, BmmPayee()} ? Role::ANCHOR : Role::OTHER};
            BOOST_CHECK(roles[i] == expected);
            rewards += roles[i] == Role::REPORTER;
            pots += roles[i] == Role::ANCHOR;
        }
        BOOST_CHECK_EQUAL(rewards, 1U);
        BOOST_CHECK_EQUAL(pots, 1U);
        const auto carried{chain->getCoinbaseRoles(carrying_hash)};
        BOOST_REQUIRE(!carried.empty());
        BOOST_CHECK(carried[0] == Role::STAKED);
        BOOST_CHECK(chain->getCoinbaseRoles(uint256::ONE).empty());
    }
    BOOST_CHECK_EQUAL(Stake().pot_reserve, reserve - release);
    // The sets made from then on leave the offender out.
    const auto set{WITH_LOCK(::cs_main, return Hooks().SetFor(*Tip(), pos::EpochOf(pos::SlotOf(Tip()->nTime, Pos()), Pos()) + 2))};
    BOOST_REQUIRE(set);
    BOOST_CHECK(!set->Find(offender));
    BOOST_CHECK_EQUAL(set->Validators().size(), 2U);

    // A proof of a slot whose set was made more than evidence_window_anchors anchors before: too old.
    const auto [late_a, late_b]{TwoBlocks(5)};
    Take(late_a);
    for (int64_t i{0}; i <= Pos().evidence_window_anchors; ++i) MineBmm(BmmBlock(/*use_mempool=*/false));
    BOOST_CHECK_EQUAL(Check(WithEvidence(60, {{.a = late_a, .b = late_b}})), "bad-pos-evidence-old");
}

BOOST_AUTO_TEST_CASE(coinbase_roles)
{
    // A merged mined block that pays, after its first output (the payee's): a deposit to the payee,
    // the pot to the payee, an unbond, a reward to the payee and one to another reporter, a refund.
    const CScript payee{Wpkh(1)};
    const CScript other{Wpkh(2)};
    const CScript owner{Wpkh(3)};
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vout = {CTxOut{0, payee}, CTxOut{5, payee}, CTxOut{7, payee}, CTxOut{3, owner}, CTxOut{1, payee}, CTxOut{2, other}, CTxOut{4, owner},
                     CTxOut{0, CScript() << OP_RETURN}};
    CBlock bmm;
    bmm.vtx.push_back(MakeTransactionRef(coinbase));
    BOOST_CHECK(pos::IsAnchorBlock(bmm));
    BOOST_CHECK(pos::AnchorPayee(bmm) == payee);
    using Role = pos::CoinbaseRole;
    BOOST_CHECK(pos::CoinbaseRoles(bmm, {payee, other}) == (std::vector<Role>{Role::OTHER, Role::OTHER, Role::ANCHOR, Role::OTHER, Role::REPORTER, Role::REPORTER, Role::OTHER, Role::OTHER}));
    // No rewards owed: the last payout to the payee is the pot.
    BOOST_CHECK(pos::CoinbaseRoles(bmm, {}) == (std::vector<Role>{Role::OTHER, Role::OTHER, Role::OTHER, Role::OTHER, Role::ANCHOR, Role::OTHER, Role::OTHER, Role::OTHER}));
    // Proofs in its coinbase: one reward each, to its first output's script.
    CMutableTransaction with_proof{coinbase};
    CBlockHeader a, b;
    a.nTime = 1;
    b.nTime = 2;
    with_proof.vout.emplace_back(0, pos::EvidenceScript({.a = a, .b = b}));
    CBlock carrying;
    carrying.vtx.push_back(MakeTransactionRef(with_proof));
    BOOST_CHECK(pos::ReporterScripts(carrying, std::nullopt, uint256{}) == std::vector<CScript>{payee});
    BOOST_CHECK(pos::ReporterScripts(bmm, std::nullopt, uint256{}).empty());
    // A first output that is no address (IsRewardPayee): the chain pays it no pot and no reward (they
    // go to the reserve), and the wallet calls nothing so; another reporter's reward still is one.
    CMutableTransaction not_address{with_proof};
    not_address.vout[0].scriptPubKey = CScript() << OP_TRUE;
    CBlock odd;
    odd.vtx.push_back(MakeTransactionRef(not_address));
    BOOST_CHECK(!pos::AnchorPayee(odd));
    BOOST_CHECK(pos::ReporterScripts(odd, std::nullopt, uint256{}).empty());
    BOOST_CHECK(pos::CoinbaseRoles(odd, {other}) == (std::vector<Role>{Role::OTHER, Role::OTHER, Role::OTHER, Role::OTHER, Role::OTHER, Role::REPORTER, Role::OTHER, Role::OTHER, Role::OTHER}));
}

BOOST_FIXTURE_TEST_CASE(leader_checked_when_connected, FastBlocksSetup)
{
    StartPos();
    // A block whose epoch's set this state does not have (the header was taken before the node knew
    // it): refused when connected. (The set of the anchor of an epoch two on.) Even once this node has
    // that set in memory: the rules of a block read its chain's state alone, so that a set the node
    // read before (one the state pruned since, or an earlier record made) does not decide.
    {
        const int64_t slot{Slot(2 * static_cast<int>(Pos().slots_per_epoch) + 5)};
        std::string error;
        const auto block{producer->Build(slot, std::nullopt, error)};
        BOOST_REQUIRE_MESSAGE(block, error);
        sidechain::EmptyStore empty;
        BOOST_CHECK_EQUAL(After(*block, empty), "bad-pos-set-unknown");
        BOOST_REQUIRE(WITH_LOCK(::cs_main, return Hooks().SetFor(*Tip(), pos::EpochOf(slot, Pos()))));
        BOOST_CHECK_EQUAL(After(*block, empty), "bad-pos-set-unknown");
    }
    // Signed by another key than the leader's: refused when connected, whatever the header check said.
    CBlock block{PosBlock(1)};
    const sidechain::StoreView& view{ActiveView()};
    BOOST_CHECK_EQUAL(After(block, view), "");
    Resign(block, &NotLeader(pos::SlotOf(block.nTime, Pos())));
    BOOST_CHECK_EQUAL(After(block, view), "bad-pos-sig");
}

BOOST_FIXTURE_TEST_CASE(inactive_when_connected, NoValidatorsSetup)
{
    // No validators: a block of stake is refused when connected, too.
    StartPos();
    const int64_t slot{Slot(1)};
    std::string error;
    const auto block{producer->Build(slot, std::nullopt, error)};
    BOOST_REQUIRE_MESSAGE(block, error);
    const sidechain::StoreView& view{ActiveView()};
    BOOST_CHECK_EQUAL(After(*block, view), "bad-pos-inactive");
}

BOOST_FIXTURE_TEST_CASE(seed_from_the_mainchain, SeedSetup)
{
    // The seed of an epoch is the hash of the highest mainchain block whose median time is at or
    // before the epoch's start (no lookahead here): a block of stake of an epoch whose seed the record
    // does not have yet waits.
    StartPos();
    BOOST_CHECK(Hooks().IsWaitReason("pos-seed-unknown"));
    // The first slot of the next epoch.
    const int64_t epoch{pos::EpochOf(base_slot, Pos()) + 1};
    const int i{static_cast<int>(epoch * Pos().slots_per_epoch - base_slot)};
    const int64_t slot{Slot(i)};
    // Its leader will be that of the seed of the block on record now.
    const uint256 seed{pos::ValidatorSet::Seed(epoch, Record().TipHash())};
    const auto leader{Hooks().GenesisSet().Leader(slot, seed)};
    BOOST_REQUIRE(leader);
    std::string error;
    BOOST_CHECK(!producer->Produce(slot, error));
    BOOST_CHECK_EQUAL(error, "the mainchain block that seeds the epoch is not on record yet");
    auto block{producer->Build(slot, std::nullopt, error)};
    BOOST_REQUIRE_MESSAGE(block, error);
    for (const CKey& k : keys) {
        if (XOnly(k) == Hooks().GenesisSet().Validators()[*leader].pubkey) BOOST_REQUIRE(pos::Producer::Sign(*block, k, Genesis()));
    }
    BOOST_CHECK_EQUAL(Check(*block), "pos-seed-unknown");
    BOOST_CHECK(!WITH_LOCK(::cs_main, return Hooks().WaitingReady(*block, Record())));
    const sidechain::StoreView& view{ActiveView()};
    BOOST_CHECK_EQUAL(After(*block, view), "pos-seed-unknown");
    // A mainchain block after the epoch's start: the seed is known, the block taken.
    Mock(GetTime() + 1);
    Main();
    BOOST_CHECK(WITH_LOCK(::cs_main, return Hooks().WaitingReady(*block, Record())));
    Take(*block);
    // A mainchain reorg that replaces the block that seeds the epoch moves its leaders: the blocks of
    // stake of the epoch whose leader changed are taken back, and given back when the record is.
    std::vector<uint256> blocks{block->GetHash()};
    for (int n{1}; n <= 6; ++n) {
        Take(PosBlock(i + n));
        blocks.push_back(Tip()->GetBlockHash());
    }
    const uint256 tip{Tip()->GetBlockHash()};
    Hooks().CheckSeeds();
    BOOST_CHECK(Tip()->GetBlockHash() == tip);
    const int seed_height{Record().Height() - 1};
    const auto replaced{Record().Truncate(seed_height - 1)};
    for (sidechain::MainBlock main : replaced) {
        main.prev_hash = Record().TipHash();
        main.hash = ArithToUint256(UintToArith256(main.hash) + 1);
        BOOST_REQUIRE(Record().Append(main));
    }
    Hooks().CheckSeeds();
    // (Whether a block's leader changed is up to the new seed: with seven blocks, some did.)
    const auto changed{[&](const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        const CBlockIndex* index{Chainman().m_blockman.LookupBlockIndex(hash)};
        const auto now{Hooks().LeaderOf(*index->pprev, pos::SlotOf(index->nTime, Pos()))};
        return now.status != pos::FastBlocksHooks::LeaderStatus::OK || !pos::CheckBlockSig(index->GetBlockHeader(), now.pubkey, Genesis());
    }};
    {
        LOCK(::cs_main);
        const auto first_changed{std::find_if(blocks.begin(), blocks.end(), changed)};
        if (first_changed != blocks.end()) {
            BOOST_CHECK(Chainman().m_blockman.LookupBlockIndex(*first_changed)->nStatus & BLOCK_FAILED_VALID);
            BOOST_CHECK(!Chain().m_chain.Contains(*Chainman().m_blockman.LookupBlockIndex(*first_changed)));
        } else {
            BOOST_CHECK(Tip()->GetBlockHash() == tip);
        }
    }
    // The record as it was: the blocks are given back.
    Record().Truncate(seed_height - 1);
    for (const sidechain::MainBlock& main : replaced) BOOST_REQUIRE(Record().Append(main));
    Hooks().CheckSeeds();
    BOOST_CHECK(Tip()->GetBlockHash() == tip);
}

BOOST_FIXTURE_TEST_CASE(record_failures_pardoned, FastBlocksSetup)
{
    // A block that failed against the record (its anchor's commitment lost, say), and the blocks on
    // it: a peer that sends their headers is not punished (its record may be ahead), unlike for a
    // block the operator marked invalid.
    BOOST_CHECK(Hooks().PardonRecordFailures());
    StartPos();
    const CBlock first{PosBlock(1)};
    Take(first);
    const CBlock second{PosBlock(2)};
    Take(second);
    CBlockIndex* failed{WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(first.GetHash()))};
    BlockValidationState invalidated;
    BOOST_REQUIRE(Chain().InvalidateBlock(invalidated, failed, /*by_record=*/true));
    const auto result{[&](const CBlockHeader& header) {
        BlockValidationState state;
        Chainman().ProcessNewBlockHeaders({{header}}, /*min_pow_checked=*/true, state);
        return std::make_pair(state.GetResult(), state.GetRejectReason());
    }};
    BOOST_CHECK(result(second) == std::make_pair(BlockValidationResult::BLOCK_TIME_FUTURE, std::string{"record-failed"}));
    BOOST_CHECK(result(first) == std::make_pair(BlockValidationResult::BLOCK_TIME_FUTURE, std::string{"record-failed"}));
    BOOST_REQUIRE(Chain().InvalidateBlock(invalidated, failed, /*by_record=*/false));
    BOOST_CHECK(result(second).first == BlockValidationResult::BLOCK_CACHED_INVALID);
}

BOOST_FIXTURE_TEST_CASE(block_failing_against_the_record_pardoned, FastBlocksSetup)
{
    // A block that fails against the record when it is connected (here: its acknowledgement names
    // the height of another commitment, as after a mainchain reorg moved it) is no fault of the peer
    // that sent it, whose record may say otherwise: not BLOCK_CONSENSUS, which a peer is punished
    // for. (After a mainchain reorg, the nodes that connected such a block disconnected the
    // validator that had relayed it: the soak test.) A block that breaks a rule whatever the record
    // says still is.
    StartPos();
    Take(PosBlock(1));
    Take(PosBlock(2));
    const pos::AnchorAck ack{Anchor(Ticket(Tip()->pprev->GetBlockHash(), script))};
    Commit(uint256{7});
    pos::AnchorAck moved{ack};
    moved.main_height = Record().Height();
    const CBlock block{WithAck(PosBlock(3), moved)};
    const auto result{[&](const CBlock& b) {
        LOCK(::cs_main);
        const BlockValidationState state{TestBlockValidity(Chain(), b, /*check_pow=*/false, /*check_merkle_root=*/true)};
        return std::make_pair(state.GetResult(), state.GetRejectReason());
    }};
    BOOST_CHECK(result(block) == std::make_pair(BlockValidationResult::BLOCK_TIME_FUTURE, std::string{"bad-pos-ack-height"}));
    // Its producer keeping more than its share: whatever the record says.
    CBlock greedy{PosBlock(3)};
    CMutableTransaction coinbase{*greedy.vtx[0]};
    coinbase.vout[0].nValue += 1;
    greedy.vtx[0] = MakeTransactionRef(std::move(coinbase));
    Resign(greedy, nullptr);
    BOOST_CHECK(result(greedy).first == BlockValidationResult::BLOCK_CONSENSUS);
}

BOOST_FIXTURE_TEST_CASE(no_second_ack_while_one_waits, FastBlocksSetup)
{
    // The acknowledgement of a commitment the record lost and has again waits for the follower to
    // give its block back: a leader does not acknowledge the commitment again on another branch.
    // One the operator marked invalid, it may.
    StartPos();
    Take(PosBlock(1));
    const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
    const CBlockIndex* parent{Tip()};
    const CBlock acking{PosBlock(2, ack)};
    Take(acking);
    CBlockIndex* index{WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(acking.GetHash()))};
    const auto next_ack{[&](int i) {
        const int64_t slot{Slot(i)};
        LOCK(::cs_main);
        return Hooks().NextAck(*Tip(), pos::GetFastState(Chain().SideState().View()), slot);
    }};
    BlockValidationState state;
    BOOST_REQUIRE(Chain().InvalidateBlock(state, index, /*by_record=*/true));
    BOOST_REQUIRE(Tip() == parent);
    BOOST_CHECK(!next_ack(3));
    BOOST_REQUIRE(Chain().InvalidateBlock(state, index, /*by_record=*/false));
    const auto again{next_ack(3)};
    BOOST_REQUIRE(again);
    BOOST_CHECK(*again == ack);
}

BOOST_FIXTURE_TEST_CASE(one_signature_per_slot, FastBlocksSetup)
{
    // A block the node finds invalid once signed (here: one with an acknowledgement, refused by a
    // check of the test's) is not made again without what made it invalid: a second block of the slot
    // would be a second signature, which the node, having checked the first header, would take for
    // proof against its own key, relay, and have the chain punish. The slot is lost; the
    // acknowledgement is left out of the blocks to come.
    StartPos();
    Take(PosBlock(1));
    const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
    producer->SetExtraCheckForTesting([](const CBlock& block) -> std::optional<std::string> {
        if (!block.vSections.empty()) return "refused by the test";
        return std::nullopt;
    });
    const uint256 tip{Tip()->GetBlockHash()};
    std::string error;
    BOOST_CHECK(!producer->Produce(Slot(2), error));
    BOOST_CHECK(error.find("refused by the test") != std::string::npos);
    BOOST_CHECK(Tip()->GetBlockHash() == tip);
    BOOST_CHECK_EQUAL(Hooks().Evidence().Size(), 0U);
    BOOST_CHECK(Hooks().Evidence().TakeUnrelayed().empty());
    BOOST_CHECK(producer->RefusedTickets() == std::set<uint256>{ack.ticket.Hash()});
    // The next slot: a block without the acknowledgement; still no proof against the producer.
    const auto made{producer->Produce(Slot(3), error)};
    BOOST_REQUIRE_MESSAGE(made, error);
    BOOST_CHECK(Tip()->GetBlockHash() == *made);
    BOOST_CHECK_EQUAL(Tip()->nNonce, 0U);
    BOOST_CHECK_EQUAL(Hooks().Evidence().Size(), 0U);
    // Proofs that made a block invalid are left out too.
    producer->SetExtraCheckForTesting({});
    const auto [first, second]{TwoBlocks(4)};
    Take(first);
    std::string reason;
    const auto offence{Hooks().AddEvidence({.a = first, .b = second}, reason)};
    BOOST_REQUIRE_MESSAGE(offence, reason);
    producer->SetExtraCheckForTesting([](const CBlock& block) -> std::optional<std::string> {
        pos::PosSections sections;
        if (pos::PosSections::Decode(block.vSections, sections) && !sections.evidence.empty()) return "refused by the test";
        return std::nullopt;
    });
    BOOST_CHECK(!producer->Produce(Slot(5), error));
    BOOST_CHECK(producer->RefusedOffences() == std::set<pos::Offence>{*offence});
    BOOST_CHECK_EQUAL(Hooks().Evidence().Size(), 1U);
    const auto next{producer->Produce(Slot(6), error)};
    BOOST_REQUIRE_MESSAGE(next, error);
    CBlock block;
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return Chainman().m_blockman.ReadBlock(block, *Tip())));
    BOOST_CHECK(block.vSections.empty());
}

BOOST_FIXTURE_TEST_CASE(no_jail_for_slots_nobody_could_fill, FastBlocksSetup)
{
    // The leaders of slots at which no block of stake could be made are not charged for them: slots
    // max_unanchored_slots or more after the first block no anchor covered (blocks of stake stopped),
    // and slots up to a BMM block the block follows (which ends such a stop, or a stall).
    const auto total{[&]() {
        uint32_t assigned{0};
        LOCK(::cs_main);
        for (int64_t e{pos::EpochOf(base_slot, Pos()) - 1}; e <= pos::EpochOf(base_slot, Pos()) + 20; ++e) {
            for (const CKey& k : keys) {
                if (const auto liveness{pos::GetLiveness(Chain().SideState().View(), e, XOnly(k))}) assigned += liveness->assigned;
            }
        }
        return assigned;
    }};
    StartPos();
    Take(PosBlock(1));
    BOOST_CHECK_EQUAL(total(), 1U);
    // Blocks of stake stop at slot 1 + max_unanchored_slots; an acknowledgement 10 slots later: an
    // epoch of slots back, but only those before the stop are charged.
    const int stop{1 + static_cast<int>(Pos().max_unanchored_slots)};
    const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
    const CBlock late{PosBlock(stop + 10, ack)};
    BOOST_REQUIRE_EQUAL(Check(late), "");
    Take(late);
    // (The anchor's peg actions took the liveness of the epochs before its own into account, and
    // dropped it, before the block noted its own.) Ten slots and the block, not an epoch of slots.
    BOOST_CHECK_EQUAL(total(), 10U + 1);
    // A BMM block, then a block of stake: the slots up to the BMM block are not charged.
    const int bmm_at{stop + 40};
    Slot(bmm_at);
    MineBmm(BmmBlock(/*use_mempool=*/false));
    const uint32_t after_bmm{total()};
    Take(PosBlock(bmm_at + 5));
    BOOST_CHECK_EQUAL(total(), after_bmm + 4 + 1);
}

BOOST_FIXTURE_TEST_CASE(seed_moved_state_connected_again, SeedSetup)
{
    // A mainchain reorg that moves the seed of an epoch changes more than the leaders of its blocks:
    // the leaders charged for the slots the blocks skipped (and the sets anchors make after). The
    // blocks of stake of the epoch are connected again, also those whose own leader did not change, so
    // that the state is the one the record gives now, as a node that takes the chain in afresh has it.
    StartPos();
    const int64_t spe{Pos().slots_per_epoch};
    const int64_t e1{pos::EpochOf(base_slot, Pos()) + 1};
    const int64_t e2{e1 + 1};
    const auto at{[&](int64_t slot) { return static_cast<int>(slot - base_slot); }};
    // Epoch e1 (the genesis set): its seed is the block of the BMM block's commitment.
    Slot(at(e1 * spe) + 1);
    Main();
    Take(PosBlock(at(e1 * spe) + 2));
    Take(PosBlock(at(e1 * spe) + 3));
    // Epoch e2 (the set of the BMM block, an anchor): its seed is the block just added.
    Slot(at(e2 * spe) + 1);
    Main();
    for (int k : {7, 9, 11, 13, 15, 17}) Take(PosBlock(at(e2 * spe) + k));
    const uint256 tip{Tip()->GetBlockHash()};
    using Snapshot = std::map<std::pair<int64_t, uint256>, pos::Liveness>;
    const auto liveness{[&]() {
        Snapshot snapshot;
        LOCK(::cs_main);
        for (int64_t e{e1 - 1}; e <= e2; ++e) {
            for (const CKey& k : keys) {
                if (const auto l{pos::GetLiveness(Chain().SideState().View(), e, XOnly(k))}) snapshot[{e, XOnly(k)}] = *l;
            }
        }
        return snapshot;
    }};
    const Snapshot original{liveness()};
    Hooks().CheckSeeds();
    BOOST_CHECK(Tip()->GetBlockHash() == tip);
    // The reorg: the block that seeds e2 replaced (that of e1 stays); by one with which the first block
    // of e2 keeps its leader, and a slot it skipped does not (it charged another leader for it).
    const int seed_height{Record().Height() - 1};
    const CBlockIndex* first{WITH_LOCK(::cs_main, return Chain().m_chain[Tip()->nHeight - 5])};
    BOOST_REQUIRE_EQUAL(pos::SlotOf(first->nTime, Pos()), e2 * spe + 7);
    const auto leaders{[&]() {
        std::vector<uint256> keys_of;
        LOCK(::cs_main);
        for (int64_t slot{e2 * spe}; slot <= e2 * spe + 7; ++slot) keys_of.push_back(Hooks().LeaderOf(*first->pprev, slot).pubkey);
        return keys_of;
    }};
    const std::vector<uint256> leaders_before{leaders()};
    std::vector<sidechain::MainBlock> replaced;
    bool found{false};
    for (int salt{1}; salt < 100 && !found; ++salt) {
        replaced = Record().Truncate(seed_height - 1);
        for (sidechain::MainBlock main : replaced) {
            main.prev_hash = Record().TipHash();
            main.hash = ArithToUint256(UintToArith256(main.hash) + salt);
            BOOST_REQUIRE(Record().Append(main));
        }
        const std::vector<uint256> leaders_after{leaders()};
        found = leaders_after.back() == leaders_before.back() && !std::equal(leaders_after.begin(), leaders_after.end() - 1, leaders_before.begin());
        if (!found) {
            Record().Truncate(seed_height - 1);
            for (const sidechain::MainBlock& main : replaced) BOOST_REQUIRE(Record().Append(main));
        }
    }
    BOOST_REQUIRE(found);
    Hooks().CheckSeeds();
    const CBlockIndex* now{Tip()};
    const Snapshot checked{liveness()};
    // As the chain is connected afresh under the record as it is now.
    CBlockIndex* first_e2{nullptr};
    {
        LOCK(::cs_main);
        for (CBlockIndex* index{Chain().m_chain.Tip()}; index && index->nHeight > 0; index = index->pprev) {
            if (index->m_ext && pos::SlotOf(index->nTime, Pos()) >= e2 * spe) first_e2 = index;
        }
    }
    BOOST_REQUIRE(first_e2 == first);
    {
        BlockValidationState state;
        BOOST_REQUIRE(Chain().InvalidateBlock(state, first_e2, /*by_record=*/true));
        WITH_LOCK(::cs_main, Chain().ReconsiderRecordFailure(first_e2));
        BOOST_REQUIRE(Chain().ActivateBestChain(state));
        BOOST_CHECK(Tip() == now);
        BOOST_CHECK(liveness() == checked);
    }
    BOOST_CHECK(checked != original);
    // The record as it was: the blocks are given back (the leaders of blocks taken back read the set of
    // an anchor), and the state is the first one again.
    Record().Truncate(seed_height - 1);
    for (const sidechain::MainBlock& main : replaced) BOOST_REQUIRE(Record().Append(main));
    Hooks().CheckSeeds();
    BOOST_CHECK(Tip()->GetBlockHash() == tip);
    BOOST_CHECK(liveness() == original);
}

BOOST_FIXTURE_TEST_CASE(seed_moved_while_the_node_was_down, SeedSetup)
{
    // As seed_moved_state_connected_again, but the reorg comes while the node is down (or before its
    // first check after it started): what it compares the record with is what its blocks read, in the
    // chain's state, not what it saw before in memory. A node restarted (hooks made afresh, nothing in
    // memory) connects the blocks of the epoch again too, and has the state a node that takes the
    // chain in afresh has (review of audit 6, the check of BMM-anchor and seed timing).
    StartPos();
    const int64_t spe{Pos().slots_per_epoch};
    const int64_t e1{pos::EpochOf(base_slot, Pos()) + 1};
    const int64_t e2{e1 + 1};
    const auto at{[&](int64_t slot) { return static_cast<int>(slot - base_slot); }};
    Slot(at(e1 * spe) + 1);
    Main();
    Take(PosBlock(at(e1 * spe) + 2));
    Take(PosBlock(at(e1 * spe) + 3));
    Slot(at(e2 * spe) + 1);
    Main();
    for (int k : {7, 9, 11, 13, 15, 17}) Take(PosBlock(at(e2 * spe) + k));
    const uint256 tip{Tip()->GetBlockHash()};
    using Snapshot = std::map<std::pair<int64_t, uint256>, pos::Liveness>;
    const auto liveness{[&]() {
        Snapshot snapshot;
        LOCK(::cs_main);
        for (int64_t e{e1 - 1}; e <= e2; ++e) {
            for (const CKey& k : keys) {
                if (const auto l{pos::GetLiveness(Chain().SideState().View(), e, XOnly(k))}) snapshot[{e, XOnly(k)}] = *l;
            }
        }
        return snapshot;
    }};
    const Snapshot original{liveness()};
    // The node goes down; the mainchain replaces the block that seeds e2 (by one with which the first
    // block of e2 keeps its leader, and a slot it skipped does not).
    const int seed_height{Record().Height() - 1};
    const CBlockIndex* first{WITH_LOCK(::cs_main, return Chain().m_chain[Tip()->nHeight - 5])};
    BOOST_REQUIRE_EQUAL(pos::SlotOf(first->nTime, Pos()), e2 * spe + 7);
    const auto leaders{[&]() {
        std::vector<uint256> keys_of;
        LOCK(::cs_main);
        for (int64_t slot{e2 * spe}; slot <= e2 * spe + 7; ++slot) keys_of.push_back(Hooks().LeaderOf(*first->pprev, slot).pubkey);
        return keys_of;
    }};
    const std::vector<uint256> leaders_before{leaders()};
    std::vector<sidechain::MainBlock> replaced;
    bool found{false};
    for (int salt{1}; salt < 100 && !found; ++salt) {
        replaced = Record().Truncate(seed_height - 1);
        for (sidechain::MainBlock main : replaced) {
            main.prev_hash = Record().TipHash();
            main.hash = ArithToUint256(UintToArith256(main.hash) + salt);
            BOOST_REQUIRE(Record().Append(main));
        }
        const std::vector<uint256> leaders_after{leaders()};
        found = leaders_after.back() == leaders_before.back() && !std::equal(leaders_after.begin(), leaders_after.end() - 1, leaders_before.begin());
        if (!found) {
            Record().Truncate(seed_height - 1);
            for (const sidechain::MainBlock& main : replaced) BOOST_REQUIRE(Record().Append(main));
        }
    }
    BOOST_REQUIRE(found);
    // The node starts again: its first check.
    {
        pos::FastBlocksHooks restarted{*params};
        restarted.Attach(&Chainman());
        restarted.CheckSeeds();
    }
    const CBlockIndex* now{Tip()};
    const Snapshot checked{liveness()};
    BOOST_CHECK_MESSAGE(checked != original, "the state still reads the seed the record no longer has");
    // As the chain is connected afresh under the record as it is now.
    {
        BlockValidationState state;
        BOOST_REQUIRE(Chain().InvalidateBlock(state, const_cast<CBlockIndex*>(first), /*by_record=*/true));
        WITH_LOCK(::cs_main, Chain().ReconsiderRecordFailure(const_cast<CBlockIndex*>(first)));
        BOOST_REQUIRE(Chain().ActivateBestChain(state));
        BOOST_CHECK(Tip() == now);
        BOOST_CHECK(liveness() == checked);
    }
    // The record as it was, the node down again: the first state again.
    Record().Truncate(seed_height - 1);
    for (const sidechain::MainBlock& main : replaced) BOOST_REQUIRE(Record().Append(main));
    {
        pos::FastBlocksHooks restarted{*params};
        restarted.Attach(&Chainman());
        restarted.CheckSeeds();
    }
    BOOST_CHECK(Tip()->GetBlockHash() == tip);
    BOOST_CHECK(liveness() == original);
}

BOOST_FIXTURE_TEST_CASE(anchor_slot_from_the_commitment, FastBlocksSetup)
{
    // A BMM block whose time is set back (as far as the median time of the chain allows: after a
    // stall, to before an epoch whose seed is known already) does not make the set of the epochs
    // after its time, but of those after the median time of the mainchain block that committed to it.
    // Otherwise whoever makes it could bond, see the seed, and then fix the set (stake grinding).
    StartPos();
    Take(PosBlock(1));
    const int64_t spe{Pos().slots_per_epoch};
    const int64_t later{Slot(static_cast<int>(3 * spe))};
    CBlock bmm{BmmBlock(/*use_mempool=*/false)};
    bmm.nTime = WITH_LOCK(::cs_main, return Tip()->GetMedianTimePast()) + 1;
    FinishBmm(bmm);
    MineBmm(bmm);
    const auto entry{pos::GetAnchorSet(ActiveView(), bmm.GetHash())};
    BOOST_REQUIRE(entry);
    BOOST_CHECK_EQUAL(entry->slot, later);
    BOOST_CHECK_EQUAL(entry->anchors, 2U);
    // The epoch whose set an anchor of the block's time would make: the earlier anchor's set.
    const int64_t epoch{pos::EpochOf(pos::SlotOf(bmm.nTime, Pos()), Pos()) + 2};
    BOOST_REQUIRE(pos::EpochOf(later, Pos()) >= epoch - 1);
    uint64_t set_anchors{0};
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return Hooks().SetFor(*Tip(), epoch, nullptr, &set_anchors)));
    BOOST_CHECK_EQUAL(set_anchors, 1U);
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return Hooks().SetFor(*Tip(), epoch, &ActiveView(), &set_anchors)));
    BOOST_CHECK_EQUAL(set_anchors, 1U);
    // Its own: from the epoch after the next of its commitment.
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return Hooks().SetFor(*Tip(), pos::EpochOf(later, Pos()) + 2, nullptr, &set_anchors)));
    BOOST_CHECK_EQUAL(set_anchors, 2U);
}

BOOST_FIXTURE_TEST_CASE(jailed_validator_back_after_pos_stops, FastBlocksSetup)
{
    // Review of audit 6, item 9 (testnet, 2026-10-13): a validator is jailed, and blocks of stake stop
    // without it (every validator is needed); BMM blocks follow, at the clock's time, one an epoch.
    // The sets their anchors make take the validator back as soon as its jail is over: they are made
    // as of the epoch of the median time of the mainchain block that committed to the anchor, not of
    // the earliest epoch the BMM block's own time allows (two hours before it, as a block's time may
    // be ahead of the clock), which left it out for two hours more and blocks of stake stopped.
    auto& params{const_cast<Consensus::PosParams&>(Pos())};
    const Consensus::PosParams before{params};
    params.min_active_validators = keys.size();
    params.liveness_min_assigned = 1;
    const int64_t spe{params.slots_per_epoch};
    const auto at{[&](int64_t slot) { return static_cast<int>(slot - base_slot); }};
    StartPos();
    // An epoch of blocks of stake in which the last validator makes none of those it leads.
    const uint256 absent{XOnly(keys.back())};
    const int64_t e0{pos::EpochOf(base_slot + 1, params) + 1};
    int skipped{0};
    for (int64_t slot{e0 * spe}; slot < (e0 + 1) * spe; ++slot) {
        Slot(at(slot));
        if (Hooks().ActiveLeaderOf(slot).pubkey == absent) {
            ++skipped;
            continue;
        }
        Take(PosBlock(at(slot)));
    }
    BOOST_REQUIRE(skipped > 0);
    // The first anchor of the next epoch jails it: its set, and those after, are not active.
    AckTip(at((e0 + 1) * spe + 1));
    const int64_t jailed_until{Validator(absent)->jailed_until_epoch};
    BOOST_REQUIRE_EQUAL(jailed_until, e0 + 2 + params.jail_epochs);
    BOOST_REQUIRE(!WITH_LOCK(::cs_main, return Hooks().SetFor(*Tip(), e0 + 3))->Active());
    // Blocks of stake stop; BMM blocks, one an epoch, at the clock's time.
    bool back{false};
    for (int64_t epoch{e0 + 2}; epoch <= e0 + 1 + params.jail_epochs + 2 && !back; ++epoch) {
        Slot(at(epoch * spe + 1));
        MineBmm(BmmBlock(/*use_mempool=*/false));
        const auto set{WITH_LOCK(::cs_main, return Hooks().SetFor(*Tip(), epoch + 2))};
        BOOST_REQUIRE(set);
        back = set->Active() && set->Find(absent);
        BOOST_TEST_MESSAGE("epoch " << epoch << ": the set of epoch " << epoch + 2 << (back ? " has" : " has not") << " the validator back");
    }
    BOOST_CHECK(back);
    BOOST_CHECK_GE(Validator(absent)->jailed_until_epoch, jailed_until);
    params = before;
}

BOOST_AUTO_TEST_CASE(penalty_from_unbonding)
{
    // A penalty larger than what is still bonded takes the rest from what the validator is unbonding,
    // the latest unbond first (an unbond taken to nothing leaves the queue); the coins stay accounted for.
    ECC_Context ecc;
    CKey validator, owner;
    validator.MakeNewKey(/*fCompressed=*/true);
    owner.MakeNewKey(/*fCompressed=*/true);
    Consensus::PosParams params;
    params.min_stake = 100;
    params.unbonding_anchors = 10;
    const uint256 genesis{1};
    const pos::Staking staking{params, genesis};
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay out{empty};
    const uint160 owner_hash{owner.GetPubKey().GetID()};
    const uint256 pubkey{XOnly(validator)};
    std::array<unsigned char, 64> pop;
    BOOST_REQUIRE(validator.SignSchnorr(pos::BondPopHash(owner_hash, genesis), pop, nullptr, uint256{}));
    std::string reason;
    CMutableTransaction bond;
    bond.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{1}), 0});
    std::vector<unsigned char> owner_sig;
    BOOST_REQUIRE(owner.SignCompact(MessageHash(pos::BondMessage(pubkey, 10000, bond.vin[0].prevout, genesis)), owner_sig));
    bond.vout.emplace_back(10000, pos::BondScript(pubkey, owner_hash, pop, owner_sig));
    BOOST_REQUIRE(staking.ApplyTx(out, CTransaction{bond}, 1, 0, reason, uint256{9}));
    const auto unbond{[&](CAmount amount, uint32_t sequence, uint64_t anchors) {
        pos::Unbond u;
        u.pubkey = pubkey;
        u.amount = amount;
        u.sequence = sequence;
        BOOST_REQUIRE(owner.SignCompact(MessageHash(pos::UnbondMessage(pubkey, amount, sequence, bond.vin[0].prevout, uint256{9}, genesis)), u.signature));
        CMutableTransaction tx;
        tx.vout.emplace_back(0, pos::UnbondScript(u));
        BOOST_REQUIRE_MESSAGE(staking.ApplyTx(out, CTransaction{tx}, 2, anchors, reason, uint256{9}), reason);
    }};
    unbond(9400, 0, 0);
    unbond(500, 1, 1);
    BOOST_CHECK_EQUAL(pos::GetValidator(out, pubkey)->bonded, 100);
    // 10 % of 10000: 100 from the bond, 500 from the latest unbond (gone), 400 from the first.
    BOOST_REQUIRE(staking.Punish(out, pos::Offence{.pubkey = pubkey, .slot = 7}, CScript() << OP_TRUE, 2, reason));
    const auto record{*pos::GetValidator(out, pubkey)};
    BOOST_CHECK(record.tombstoned);
    BOOST_CHECK_EQUAL(record.bonded, 0);
    BOOST_REQUIRE_EQUAL(record.unbonding.size(), 1U);
    BOOST_CHECK(record.unbonding[0] == (pos::UnbondingEntry{.release_anchor = 10, .sequence = 0, .amount = 9000}));
    std::vector<std::pair<uint64_t, CAmount>> queue;
    pos::ForEachUnbonding(out, [&](uint64_t release, const uint256& key, CAmount amount) {
        BOOST_CHECK(key == pubkey);
        queue.emplace_back(release, amount);
        return true;
    });
    BOOST_CHECK(queue == (std::vector<std::pair<uint64_t, CAmount>>{{10, 9000}}));
    const pos::StakeState stake{pos::GetStakeState(out)};
    BOOST_CHECK_EQUAL(stake.bonded, 0);
    BOOST_CHECK_EQUAL(stake.unbonding, 9000);
    BOOST_CHECK_EQUAL(stake.slashed, 1000);
    BOOST_CHECK_EQUAL(stake.rewards_owed + stake.pot_reserve, 1000);
}

BOOST_FIXTURE_TEST_CASE(deterministic_signatures, FastBlocksSetup)
{
    // The producer signs a block the same way every time (BIP340, all-zero auxiliary randomness). A
    // block with another valid signature of its leader (made with other randomness) is the same block:
    // a node that has the header with one takes the block with the other, keeps its own, and the chain
    // goes on (no mutated block, no peer punished, no split). Another key's signature stays a mutated block.
    StartPos();
    Take(PosBlock(1));
    const CBlock honest{PosBlock(2)};
    const int64_t slot{pos::SlotOf(honest.nTime, Pos())};
    const CKey& leader{Leader(slot)};
    CBlock again{honest};
    again.ext.sig.fill(0);
    BOOST_REQUIRE(pos::Producer::Sign(again, leader, Genesis()));
    BOOST_CHECK(again.ext.sig == honest.ext.sig);
    std::array<unsigned char, 64> zero_aux;
    BOOST_REQUIRE(leader.SignSchnorr(pos::BlockSigHash(honest, Genesis()), zero_aux, nullptr, uint256::ZERO));
    BOOST_CHECK(zero_aux == honest.ext.sig);

    CBlock other{honest};
    BOOST_REQUIRE(leader.SignSchnorr(pos::BlockSigHash(honest, Genesis()), other.ext.sig, nullptr, GetRandHash()));
    BOOST_REQUIRE(other.ext.sig != honest.ext.sig && other.GetHash() == honest.GetHash());
    BOOST_REQUIRE(pos::CheckBlockSig(other, XOnly(leader), Genesis()));
    CBlock swapped{honest};
    BOOST_REQUIRE(pos::Producer::Sign(swapped, NotLeader(slot), Genesis()));

    // The node has the header with the other signature.
    BlockValidationState header_state;
    BOOST_REQUIRE_MESSAGE(Chainman().ProcessNewBlockHeaders({{other}}, /*min_pow_checked=*/true, header_state), header_state.ToString());
    const CBlockIndex* index{WITH_LOCK(::cs_main, return Chainman().m_blockman.LookupBlockIndex(honest.GetHash()))};
    BOOST_REQUIRE(index && index->m_ext->sig == other.ext.sig);
    BOOST_CHECK(WITH_LOCK(::cs_main, return Hooks().CheckOtherExtension(honest, *index)) == sidechain::OtherExtension::EQUIVALENT);
    BOOST_CHECK(WITH_LOCK(::cs_main, return Hooks().CheckOtherExtension(swapped, *index)) == sidechain::OtherExtension::INVALID);
    // Another key's: a mutated block, as before.
    {
        BlockValidationState state;
        BOOST_CHECK(!WITH_LOCK(::cs_main, return Chainman().AcceptBlock(std::make_shared<const CBlock>(swapped), state, nullptr, true, nullptr, nullptr, true)));
        BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_MUTATED);
    }
    // The producer's: taken, stored with the signature of the header the node had.
    {
        BlockValidationState state;
        bool new_block{false};
        BOOST_CHECK(WITH_LOCK(::cs_main, return Chainman().AcceptBlock(std::make_shared<const CBlock>(honest), state, nullptr, true, nullptr, &new_block, true)));
        BOOST_CHECK(state.IsValid());
        BOOST_CHECK(new_block);
    }
    Submit(honest);
    BOOST_CHECK(Tip() == index);
    BOOST_CHECK_EQUAL(Reason(honest), "");
    CBlock read;
    BOOST_REQUIRE(Chainman().m_blockman.ReadBlock(read, *index));
    BOOST_CHECK(read.ext.sig == other.ext.sig);
    // Either again: a duplicate, no failure.
    for (const CBlock& block : {honest, other}) {
        BlockValidationState state;
        BOOST_CHECK(WITH_LOCK(::cs_main, return Chainman().AcceptBlock(std::make_shared<const CBlock>(block), state, nullptr, true, nullptr, nullptr, true)));
        BOOST_CHECK(state.IsValid());
    }
    BOOST_CHECK(WITH_LOCK(::cs_main, return !(index->nStatus & BLOCK_FAILED_VALID)));
    // No proof that the leader signed twice: one block, one hash.
    BOOST_CHECK(Hooks().Evidence().All().empty());
    Take(PosBlock(3));
}

BOOST_FIXTURE_TEST_CASE(bond_griefing, FastBlocksSetup)
{
    // Anyone could add to a validator's bond (the proof of possession is public): a satoshi added
    // made the owner's signed unbond of all of it leave a remainder under the minimum stake. Now only
    // the owner adds bonds (it signs them), and an unbond of all takes all there is when it is applied.
    // (An unbond signed before the owner's own top-up no longer counts: unbond_replay_after_topup.)
    StartPos();
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    CKey validator, attacker;
    validator.MakeNewKey(/*fCompressed=*/true);
    attacker.MakeNewKey(/*fCompressed=*/true);
    const uint256 pubkey{XOnly(validator)};
    const CAmount min{Pos().min_stake};
    Accept(Pay(coin, spent, {BondOut(validator, 2 * min, Owner(key), &key, coin)}));
    Take(PosBlock(1));
    // The owner signs an unbond of all of it.
    const CTxOut unbond_all{UnbondOut(validator, pos::UNBOND_ALL, 0, key)};
    // The attacker's top-up of a satoshi, with the public proof of possession: refused, without the
    // owner's signature, with its own, or with the owner's of another bond.
    const pos::Bond public_bond{*pos::ParseBond(BondOut(validator, 2 * min, Owner(key)))};
    {
        COutPoint c{coin};
        CTxOut s{spent};
        BOOST_CHECK_EQUAL(Refused(Pay(c, s, {CTxOut{1, pos::BondScript(pubkey, Owner(key), public_bond.pop)}})), "bad-pos-bond-owner-sig");
    }
    {
        COutPoint c{coin};
        CTxOut s{spent};
        BOOST_CHECK_EQUAL(Refused(Pay(c, s, {BondOut(validator, 1, Owner(key), &attacker, coin)})), "bad-pos-bond-owner-sig");
    }
    BOOST_CHECK_EQUAL(StakingRules(TxWith({BondOut(validator, 1, Owner(key), &attacker)})), "bad-pos-bond-owner-sig");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({BondOut(validator, 1, Owner(attacker))})), "bad-pos-bond-owner");
    // Nor in a block.
    {
        CBlock block{PosBlock(2)};
        COutPoint c{coin};
        CTxOut s{spent};
        block.vtx.push_back(MakeTransactionRef(Pay(c, s, {CTxOut{1, pos::BondScript(pubkey, Owner(key), public_bond.pop)}})));
        Resign(block);
        BOOST_CHECK_EQUAL(Check(block), "bad-pos-bond-owner-sig");
    }
    BOOST_CHECK_EQUAL(Validator(pubkey)->bonded, 2 * min);
    // The unbond of all the owner signed takes all there is.
    BOOST_CHECK_EQUAL(StakingRules(TxWith({unbond_all})), "");
    // The owner's own top-up, signed, is taken (and the unbond signed before it is void); a new unbond
    // of all takes all of it.
    Accept(Pay(coin, spent, {BondOut(validator, min + 1, Owner(key), &key, coin)}));
    Take(PosBlock(2));
    BOOST_CHECK_EQUAL(Validator(pubkey)->bonded, 3 * min + 1);
    BOOST_CHECK_EQUAL(StakingRules(TxWith({unbond_all})), "bad-pos-unbond-sequence");
    Accept(Pay(coin, spent, {UnbondOut(validator, pos::UNBOND_ALL, 1, key)}));
    Take(PosBlock(3));
    const auto record{*Validator(pubkey)};
    BOOST_CHECK_EQUAL(record.bonded, 0);
    BOOST_CHECK_EQUAL(record.Unbonding(), 3 * min + 1);
    BOOST_CHECK_EQUAL(record.unbond_seq, 2U);
    BOOST_CHECK(!record.Eligible(Pos()));
    BOOST_CHECK_EQUAL(Stake().bonded, 0);
    BOOST_CHECK_EQUAL(Stake().unbonding, 3 * min + 1);
}

BOOST_FIXTURE_TEST_CASE(slashed_once_and_out_of_the_schedule, FastBlocksSetup)
{
    // A validator is slashed once: a proof of another of its offences, once it is banned, takes
    // nothing more (bad-pos-evidence-banned), nor does a second proof in the block that bans it. And
    // from the block after the one that bans it, it leads no more: its blocks are invalid
    // (bad-pos-leader-banned), its producer makes none, and the others go on.
    StartPos();
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    const CAmount bond{100000};
    Accept(Pay(coin, spent, {BondOut(keys[0], bond, Owner(key), &key, coin), BondOut(keys[1], bond, Owner(key), &key, coin), BondOut(keys[2], bond, Owner(key), &key, coin)}));
    Take(PosBlock(1));
    const auto [a1, b1]{TwoBlocks(2)};
    const pos::DoubleProposal first_offence{.a = a1, .b = b1};
    const uint256 offender{XOnly(Leader(base_slot + 2))};
    Take(a1);
    const auto leads{[&](int i) { return XOnly(Leader(base_slot + i)) == offender; }};
    int k{3};
    while (leads(k)) ++k;
    int j{k + 1};
    while (!leads(j)) ++j;
    int m{j + 1};
    while (leads(m)) ++m;
    // Another offence, at slot j (on the slot-2 block); its first block is valid as yet.
    const auto [a2, b2]{TwoBlocks(j)};
    const pos::DoubleProposal second_offence{.a = a2, .b = b2};
    BOOST_CHECK_EQUAL(Check(a2), "");
    const sidechain::StoreView& before{ActiveView()};
    BOOST_CHECK_EQUAL(After(WithEvidence(m, {second_offence}), before), "");
    BOOST_CHECK_EQUAL(After(WithEvidence(m, {first_offence, second_offence}), before), "bad-pos-evidence-banned");

    // The first proof, in the block of slot k: banned, 10 % gone.
    Take(WithEvidence(k, {first_offence}));
    const auto banned{*Validator(offender)};
    BOOST_REQUIRE(banned.tombstoned);
    const pos::StakeState stake{Stake()};
    BOOST_CHECK_EQUAL(stake.slashed, bond / 10);
    // The second proof takes nothing more. (Asked of the rules directly: blocks of one slot given to
    // validation would be proofs that its leader signed them all.)
    BOOST_CHECK_EQUAL(After(WithEvidence(m, {second_offence}), ActiveView()), "bad-pos-evidence-banned");
    // The offender's block of slot j, on the block that banned it: invalid; its producer makes none.
    {
        const int64_t slot_j{Slot(j)};
        std::string error;
        auto block{producer->Build(slot_j, std::nullopt, error)};
        BOOST_REQUIRE_MESSAGE(block, error);
        BOOST_REQUIRE(pos::Producer::Sign(*block, Leader(slot_j), Genesis()));
        BOOST_CHECK_EQUAL(Check(*block), "bad-pos-leader-banned");
        BOOST_CHECK(!producer->Produce(slot_j, error));
        BOOST_CHECK(error.find("banned") != std::string::npos);
    }
    // The producer leaves the second proof out; the others' blocks go on. Nothing more is taken.
    std::string reason;
    BOOST_REQUIRE_MESSAGE(Hooks().AddEvidence(second_offence, reason), reason);
    std::string error;
    BOOST_REQUIRE_MESSAGE(producer->Produce(Slot(m), error), error);
    CBlock made;
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return Chainman().m_blockman.ReadBlock(made, *Tip())));
    pos::PosSections sections;
    BOOST_REQUIRE(pos::PosSections::Decode(made.vSections, sections));
    BOOST_CHECK(sections.evidence.empty());
    BOOST_CHECK(Stake() == stake);
    BOOST_CHECK(*Validator(offender) == banned);
    // A merged mined block leaves it out too.
    const CBlock bmm{BmmBlock(/*use_mempool=*/false)};
    for (const CTxOut& out : bmm.vtx[0]->vout) BOOST_CHECK(!pos::IsEvidenceScript(out.scriptPubKey));
}

BOOST_FIXTURE_TEST_CASE(self_reported, FastBlocksSetup)
{
    // The offender carrying the proof against itself: the penalty all the same, no reward, its
    // share to the reserve.
    StartPos();
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    const CAmount bond{100000};
    Accept(Pay(coin, spent, {BondOut(keys[0], bond, Owner(key), &key, coin), BondOut(keys[1], bond, Owner(key), &key, coin), BondOut(keys[2], bond, Owner(key), &key, coin)}));
    Take(PosBlock(1));
    const auto [first, second]{TwoBlocks(2)};
    const pos::DoubleProposal proof{.a = first, .b = second};
    const uint256 offender{XOnly(Leader(base_slot + 2))};
    Take(first);
    int k{3};
    while (XOnly(Leader(base_slot + k)) != offender) ++k;
    const CBlock carrying{WithEvidence(k, {proof})};
    Take(carrying);
    const CAmount penalty{bond / 10};
    BOOST_CHECK(Validator(offender)->tombstoned);
    BOOST_CHECK_EQUAL(Stake().slashed, penalty);
    BOOST_CHECK_EQUAL(Stake().rewards_owed, 0);
    BOOST_CHECK_EQUAL(Stake().pot_reserve, penalty);
    BOOST_CHECK_EQUAL(Stake().bonded + Stake().unbonding + Stake().pot_reserve + Stake().rewards_owed, 3 * bond);
    // The next peg step pays no reward to the block that carried it.
    const CBlock bmm{BmmBlock(/*use_mempool=*/false)};
    BOOST_CHECK(!Pays(bmm, CTxOut{penalty / 8, carrying.vtx[0]->vout[0].scriptPubKey}));
    MineBmm(bmm);
    BOOST_CHECK_EQUAL(Stake().rewards_owed, 0);
}

BOOST_FIXTURE_TEST_CASE(evidence_in_bmm_blocks, FastBlocksSetup)
{
    // A merged mined block carries proofs in its coinbase (OP_RETURN 0xDD'EVD' | the proof), under the
    // rules of a block of stake's; the reward goes to its payee (its coinbase's first output) at the
    // next peg step. The node's merged miner puts the proofs it knows in.
    StartPos();
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    const CAmount bond{100000};
    Accept(Pay(coin, spent, {BondOut(keys[0], bond, Owner(key), &key, coin), BondOut(keys[1], bond, Owner(key), &key, coin), BondOut(keys[2], bond, Owner(key), &key, coin)}));
    Take(PosBlock(1));
    const auto [first, second]{TwoBlocks(2)};
    pos::DoubleProposal proof{.a = first, .b = second};
    proof.Normalize();
    const uint256 offender{XOnly(Leader(base_slot + 2))};
    Take(first);

    // The form, context free (CheckBlock).
    const auto form{[&](const CBlock& block) {
        BlockValidationState state;
        return Hooks().CheckBlock(block, state) ? std::string{} : state.GetRejectReason();
    }};
    const auto with_outputs{[&](CBlock block, const std::vector<CTxOut>& outputs) {
        SetCoinbase(block, [&](CMutableTransaction& tx) { tx.vout.insert(tx.vout.end(), outputs.begin(), outputs.end()); });
        return block;
    }};
    const CBlock plain{BmmBlock(/*use_mempool=*/false)};
    BOOST_CHECK_EQUAL(form(with_outputs(plain, {CTxOut{0, pos::EvidenceScript(proof)}})), "");
    BOOST_CHECK_EQUAL(form(with_outputs(plain, {CTxOut{0, CScript() << OP_RETURN << std::vector<unsigned char>{0xDD, 'E', 'V', 'D', 1, 2}}})), "bad-pos-evidence-output");
    BOOST_CHECK_EQUAL(form(with_outputs(plain, {CTxOut{0, pos::EvidenceScript({.a = first, .b = first})}})), "bad-pos-evidence-same");
    BOOST_CHECK_EQUAL(form(with_outputs(plain, std::vector<CTxOut>(pos::MAX_BMM_EVIDENCE, CTxOut{0, pos::EvidenceScript(proof)}))), "");
    BOOST_CHECK_EQUAL(form(with_outputs(plain, std::vector<CTxOut>(pos::MAX_BMM_EVIDENCE + 1, CTxOut{0, pos::EvidenceScript(proof)}))), "bad-pos-evidence-count");
    BOOST_CHECK_EQUAL(form(with_outputs(PosBlock(3), {CTxOut{0, pos::EvidenceScript(proof)}})), "bad-pos-evidence-not-bmm");
    BOOST_CHECK(pos::ParseEvidenceScript(pos::EvidenceScript(proof)) == std::optional<pos::DoubleProposal>{proof});

    // Two for one offence, in a BMM block: the second is known.
    {
        CBlock twice{with_outputs(plain, {CTxOut{0, pos::EvidenceScript(proof)}, CTxOut{0, pos::EvidenceScript(proof)}})};
        FinishBmm(twice);
        Commit(twice.GetHash());
        BOOST_CHECK_EQUAL(Check(twice), "bad-pos-evidence-known");
    }

    // The merged miner puts the proof the node knows in its block.
    std::string reason;
    BOOST_REQUIRE_MESSAGE(Hooks().AddEvidence(proof, reason), reason);
    CKey payee_key;
    payee_key.MakeNewKey(/*fCompressed=*/true);
    const CScript payee{GetScriptForDestination(WitnessV0KeyHash{payee_key.GetPubKey()})};
    const CBlock carrying{BmmBlock(/*use_mempool=*/false, payee)};
    std::vector<pos::DoubleProposal> carried;
    for (const CTxOut& out : carrying.vtx[0]->vout) {
        if (const auto read{pos::ParseEvidenceScript(out.scriptPubKey)}) carried.push_back(*read);
    }
    BOOST_REQUIRE_EQUAL(carried.size(), 1U);
    BOOST_CHECK(carried[0] == proof);
    MineBmm(carrying);
    const CAmount penalty{bond / 10};
    BOOST_CHECK(Validator(offender)->tombstoned);
    BOOST_CHECK_EQUAL(Stake().slashed, penalty);
    BOOST_CHECK_EQUAL(Stake().rewards_owed, penalty / 8);
    BOOST_CHECK_EQUAL(Stake().pot_reserve, penalty - penalty / 8);
    // Not again: the next one carries nothing, and one that does is refused.
    const CBlock next{BmmBlock(/*use_mempool=*/false)};
    for (const CTxOut& out : next.vtx[0]->vout) BOOST_CHECK(!pos::IsEvidenceScript(out.scriptPubKey));
    {
        CBlock again{with_outputs(next, {CTxOut{0, pos::EvidenceScript(proof)}})};
        FinishBmm(again);
        Commit(again.GetHash());
        BOOST_CHECK_EQUAL(Check(again), "bad-pos-evidence-known");
    }
    // The reward, to the BMM block's payee, at the next peg step.
    BOOST_CHECK(Pays(next, CTxOut{penalty / 8, payee}));
    MineBmm(next);
    BOOST_CHECK_EQUAL(Stake().rewards_owed, 0);
}

BOOST_FIXTURE_TEST_CASE(forged_ack_headers, FastBlocksSetup)
{
    // An acknowledgement counts as an anchor in the order of chains from its header on. Its height
    // comes after the last anchor of the chain below it (the state's bad-pos-ack-order, as far as
    // the headers tell): otherwise one commitment on record could count again and again.
    StartPos();
    const int bmm_height{Record().Height()};
    Take(PosBlock(1));
    const pos::AnchorAck ack{AckTip(2)};
    BOOST_REQUIRE_EQUAL(Key().last_anchor_height, static_cast<uint32_t>(ack.main_height));
    const uint256 best{BestHeader()};
    for (const int height : {ack.main_height, bmm_height}) {
        // Signed by the leader of its slot, as it may be: the leader too counts only new commitments.
        const CBlock claimed{WithAck(PosBlock(3), pos::AnchorAck{.ticket = ack.ticket, .main_height = height})};
        BOOST_CHECK_EQUAL(HeaderReason({claimed}), "bad-pos-ack-order");
        BOOST_CHECK(BestHeader() == best);
    }

    // A branch from a block whose epoch's set the state pruned (anchors since, over epochs), on a
    // node that does not have it in memory either (it restarted): its headers cannot be checked, and
    // are refused, unsigned ones that claim old commitments with them.
    const CBlockHeader like{Tip()->GetBlockHeader()};
    const uint256 fork{Tip()->GetBlockHash()};
    std::vector<int> heights;
    for (int n{1}; n <= 16; ++n) heights.push_back(AckTip(3 + 6 * n).main_height);
    Hooks().ForgetKnownSets();
    BOOST_REQUIRE(!WITH_LOCK(::cs_main, return Hooks().SetFor(*Index(fork), pos::EpochOf(pos::SlotOf(GetTime(), Pos()), Pos()))));
    const uint256 tip_best{BestHeader()};
    uint256 prev{fork};
    std::vector<CBlockHeader> branch;
    for (size_t n{0}; n < 4; ++n) {
        branch.push_back(UnsignedHeader(like, prev, pos::SlotOf(GetTime(), Pos()) - 3 + static_cast<int64_t>(n), static_cast<uint32_t>(heights[n]), static_cast<uint8_t>(n)));
        prev = branch.back().GetHash();
    }
    BOOST_CHECK_EQUAL(HeaderReason(branch), "bad-pos-fork-too-deep");
    BOOST_CHECK(!Index(branch[0].GetHash()));
    BOOST_CHECK(BestHeader() == tip_best);
}

BOOST_FIXTURE_TEST_CASE(unsigned_ack_header_unknown_set, FastBlocksSetup)
{
    // A header whose epoch's set is that of an anchor on a branch this node never connected: its
    // signature cannot be checked until its block is connected. Taken if the branch leaves the active
    // chain above its final block (a node that catches up takes in such headers), not below: a header
    // nobody checked does not take back a final block.
    StartPos();
    Take(PosBlock(1));
    const CBlockHeader like{Tip()->GetBlockHeader()};
    const uint256 fork{Tip()->GetBlockHash()};
    // A BMM block on it, committed to, that this node has the header of only.
    const CBlock other{BmmBlock(/*use_mempool=*/false)};
    Commit(other.GetHash());
    BOOST_REQUIRE_EQUAL(HeaderReason({other}), "");
    // The active chain goes on with anchors of its own, over epochs.
    std::vector<int> heights;
    for (int n{1}; n <= 4; ++n) heights.push_back(AckTip(1 + 12 * n).main_height);
    const int64_t slot{pos::SlotOf(GetTime(), Pos())};
    BOOST_REQUIRE_GE(pos::EpochOf(slot, Pos()), pos::EpochOf(pos::SlotOf(other.nTime, Pos()), Pos()) + 2);
    BOOST_REQUIRE(!WITH_LOCK(::cs_main, return Hooks().SetFor(*Index(other.GetHash()), pos::EpochOf(slot, Pos()))));
    const uint256 best{BestHeader()};
    // Below the final block: refused, unsigned or not (nobody can tell).
    const CBlockHeader claimed{UnsignedHeader(like, other.GetHash(), slot, static_cast<uint32_t>(heights.back()))};
    BOOST_CHECK_EQUAL(HeaderReason({claimed}), "bad-pos-fork-too-deep");
    BOOST_CHECK(!Index(claimed.GetHash()));
    BOOST_CHECK(BestHeader() == best);
    (void)fork;
}

BOOST_FIXTURE_TEST_CASE(future_unsigned_header_not_waiting, FastBlocksSetup)
{
    // A header of a slot to come waits only if its leader signed it: one whose leader the node cannot
    // tell (the seed of its epoch, or the set, not known) is turned away, not kept (anyone can make
    // them, and those kept push out the headers that wait for good reasons).
    StartPos();
    Take(PosBlock(1));
    const CBlockHeader like{Tip()->GetBlockHeader()};
    const size_t waiting{Waiting()};
    // A day ahead: the seed of its epoch is not on record.
    const int64_t now{pos::SlotOf(GetTime(), Pos())};
    const CBlockHeader far{UnsignedHeader(like, Tip()->GetBlockHash(), now + 24 * 60 * 60, 0)};
    BOOST_CHECK(!Hooks().IsWaitReason(HeaderReason({far})));
    BOOST_CHECK_EQUAL(Waiting(), waiting);
    // A minute ahead, on a branch whose set the node does not know.
    const CBlock other{BmmBlock(/*use_mempool=*/false)};
    Commit(other.GetHash());
    BOOST_REQUIRE_EQUAL(HeaderReason({other}), "");
    Mock(pos::SlotTime(now + 2 * Pos().slots_per_epoch, Pos()));
    Main();
    const int64_t later{pos::SlotOf(GetTime(), Pos()) + 60};
    BOOST_REQUIRE(!WITH_LOCK(::cs_main, return Hooks().SetFor(*Index(other.GetHash()), pos::EpochOf(later, Pos()))));
    const CBlockHeader soon{UnsignedHeader(like, other.GetHash(), later, 0)};
    BOOST_CHECK(!Hooks().IsWaitReason(HeaderReason({soon})));
    BOOST_CHECK_EQUAL(Waiting(), waiting);
    // Its leader's, a few slots ahead: waits.
    const CBlock signed_block{PosBlock(static_cast<int>(pos::SlotOf(GetTime(), Pos()) - base_slot) + 5)};
    Mock(signed_block.nTime - 5);
    BOOST_CHECK_EQUAL(HeaderReason({signed_block}), "pos-time-future");
    BOOST_CHECK_EQUAL(Waiting(), waiting + 1);
}

BOOST_FIXTURE_TEST_CASE(unchecked_siblings_not_taken, FastBlocksSetup)
{
    // Headers whose leader the node cannot tell yet (a branch with an anchor it never connected, as for
    // a node that catches up), above the final block: unchecked headers, of which a peer holds at most
    // its budget (unchecked_headers_per_peer) until their blocks are connected. Siblings by the
    // thousand, which anyone can make, are not taken; nor a message padded with headers that claim
    // whatever the record allows; what such headers claim stops nobody else's; and when their peer
    // leaves they go, and the best header they inflated with them.
    // (A budget of 3 for the test. The params are the hooks', made for this test.)
    auto& params{const_cast<Consensus::PosParams&>(Pos())};
    const int64_t budget_before{params.unchecked_headers_per_peer};
    params.unchecked_headers_per_peer = 3;
    StartPos();
    Take(PosBlock(1));
    const CBlockHeader like{Tip()->GetBlockHeader()};
    const CBlock other{BmmBlock(/*use_mempool=*/false)};
    Commit(other.GetHash());
    BOOST_REQUIRE_EQUAL(HeaderReason({other}), "");
    BOOST_REQUIRE(BestHeader() == other.GetHash());
    // Commitments on record that headers nobody checks can claim to acknowledge.
    std::vector<uint32_t> commitments;
    for (uint8_t i{1}; i <= 4; ++i) {
        Commit(uint256{static_cast<uint8_t>(0xc0 + i)});
        commitments.push_back(static_cast<uint32_t>(Record().Height()));
    }
    Mock(pos::SlotTime(pos::SlotOf(GetTime(), Pos()) + 2 * Pos().slots_per_epoch, Pos()));
    Main();
    const int64_t slot{pos::SlotOf(GetTime(), Pos())};
    BOOST_REQUIRE(!WITH_LOCK(::cs_main, return Hooks().SetFor(*Index(other.GetHash()), pos::EpochOf(slot - 10, Pos()))));

    // An attacker first: a branch that claims every commitment on record, to inflate the best header.
    std::vector<CBlockHeader> inflated;
    uint256 prev{other.GetHash()};
    for (size_t i{0}; i < 3; ++i) {
        inflated.push_back(UnsignedHeader(like, prev, slot - 10 + static_cast<int64_t>(i), commitments[i], 1));
        prev = inflated.back().GetHash();
    }
    BOOST_CHECK_EQUAL(HeaderReason(inflated, /*source=*/1), "");
    BOOST_CHECK(BestHeader() == inflated.back().GetHash());
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return Chainman().UncheckedHeld(1)), 3U);
    // Its budget is spent: siblings of the newest header are not taken, the peer not punished.
    for (uint8_t salt{2}; salt < 12; ++salt) {
        const CBlockHeader sibling{UnsignedHeader(like, inflated[1].GetHash(), slot - 8, 0, salt)};
        BlockValidationState state;
        BOOST_CHECK(!Chainman().ProcessNewBlockHeaders({{sibling}}, /*min_pow_checked=*/true, state, nullptr, /*source=*/1));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "unchecked-budget");
        BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_HEADER_LOW_WORK);
        BOOST_CHECK(!Index(sibling.GetHash()));
    }
    // A message padded with headers that claim more (the last commitment too): taken up to the
    // budget of its peer, not further, whatever it claims.
    std::vector<CBlockHeader> padded;
    prev = other.GetHash();
    for (size_t i{0}; i < 5; ++i) {
        padded.push_back(UnsignedHeader(like, prev, slot - 10 + static_cast<int64_t>(i), i < commitments.size() ? commitments[i] : 0, 2));
        prev = padded.back().GetHash();
    }
    BOOST_CHECK_EQUAL(HeaderReason(padded, /*source=*/2), "unchecked-budget");
    for (size_t i{0}; i < padded.size(); ++i) BOOST_CHECK_EQUAL(Index(padded[i].GetHash()) != nullptr, i < 3);

    // An honest peer, whose branch claims less than the inflated best header (fewer anchors): its
    // headers are taken all the same (a node catching up needs them).
    std::vector<CBlockHeader> honest;
    prev = other.GetHash();
    for (size_t i{0}; i < 3; ++i) {
        honest.push_back(UnsignedHeader(like, prev, slot - 10 + static_cast<int64_t>(i), 0, 3));
        prev = honest.back().GetHash();
    }
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return Index(honest.back().GetHash()) == nullptr && Chainman().m_best_header->nChainWork > Hooks().ChainWork(Index(other.GetHash()), honest[0])));
    BOOST_CHECK_EQUAL(HeaderReason(honest, /*source=*/3), "");
    for (const CBlockHeader& header : honest) BOOST_CHECK(Index(header.GetHash()));
    // Not from the node itself (its operator, its disk).
    const CBlockHeader own{UnsignedHeader(like, inflated[1].GetHash(), slot - 8, 0, 40)};
    BOOST_CHECK_EQUAL(HeaderReason({own}, /*source=*/-1), "");
    // The attacker leaves: what it alone held goes (its newest, which nobody else holds; the node holds
    // those below its own), and the best header it inflated with it.
    WITH_LOCK(::cs_main, Chainman().UncheckedSourceGone(1));
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return Chainman().UncheckedHeld(1)), 0U);
    BOOST_CHECK(!Index(inflated[2].GetHash()));
    BOOST_CHECK(Index(inflated[1].GetHash()) && Index(own.GetHash()));
    BOOST_CHECK(BestHeader() != inflated[2].GetHash());
    // Peer 2 leaves too: its padded headers go; the honest peer's stay.
    WITH_LOCK(::cs_main, Chainman().UncheckedSourceGone(2));
    for (const CBlockHeader& header : padded) BOOST_CHECK(!Index(header.GetHash()));
    for (const CBlockHeader& header : honest) BOOST_CHECK(Index(header.GetHash()));
    WITH_LOCK(::cs_main, Chainman().CheckUnchecked());
    params.unchecked_headers_per_peer = budget_before;
}

BOOST_FIXTURE_TEST_CASE(unchecked_headers_checked_give_room, FastBlocksSetup)
{
    // The unchecked headers a peer holds give their place back once checked (their blocks connected):
    // an honest peer's budget is not used up as the node catches up.
    auto& params{const_cast<Consensus::PosParams&>(Pos())};
    const int64_t budget_before{params.unchecked_headers_per_peer};
    params.unchecked_headers_per_peer = 1;
    StartPos();
    Take(PosBlock(1));
    const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
    const CBlock anchor{PosBlock(2, ack)};
    Take(anchor);
    const int64_t epoch{pos::EpochOf(pos::SlotOf(anchor.nTime, Pos()), Pos())};
    const int last{static_cast<int>((epoch + 2) * Pos().slots_per_epoch - base_slot)};
    const CBlock honest{PosBlock(last)};
    // The node as it was before it connected the anchor: the anchor taken back, its set forgotten.
    CBlockIndex* anchor_index{Index(anchor.GetHash())};
    {
        BlockValidationState state;
        BOOST_REQUIRE(Chain().InvalidateBlock(state, anchor_index));
        LOCK(::cs_main);
        Chain().ResetBlockFailureFlags(anchor_index);
        Chainman().RecalculateBestHeader();
    }
    Hooks().ForgetKnownSets();
    // The header, from peer 5: taken unchecked, its budget spent.
    BOOST_REQUIRE_EQUAL(HeaderReason({honest}, /*source=*/5), "");
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return Chainman().UncheckedHeld(5)), 1U);
    CBlockHeader sibling{honest};
    sibling.hashMerkleRoot = uint256{0x77};
    sibling.ext.sig.fill(0);
    BOOST_CHECK_EQUAL(HeaderReason({sibling}, /*source=*/5), "unchecked-budget");
    // Its block connected (checked): room again.
    {
        BlockValidationState state;
        BOOST_REQUIRE(Chain().ActivateBestChain(state));
    }
    Submit(honest);
    BOOST_REQUIRE(Tip() == Index(honest.GetHash()));
    {
        // A checked header now, kept and written like any other.
        LOCK(::cs_main);
        BOOST_CHECK_EQUAL(Chainman().UncheckedHeld(5), 0U);
        BOOST_CHECK(!Index(honest.GetHash())->m_unchecked);
    }
    params.unchecked_headers_per_peer = budget_before;
}

BOOST_FIXTURE_TEST_CASE(record_independent_failures, FastBlocksSetup)
{
    // A block that breaks a rule of FastBlocks whatever the record says (its producer's share, here)
    // did not fail against the record: not noted so, not tried again when the record changes, and the
    // peers that send headers on it are not pardoned. One whose leader the record decides is.
    BOOST_CHECK(Hooks().RecordIndependent("bad-pos-producer-share"));
    BOOST_CHECK(sidechain::RecordIndependent("bad-pos-producer-share"));
    BOOST_CHECK(sidechain::RecordIndependent("bad-pos-bond-owner-sig"));
    BOOST_CHECK(sidechain::RecordIndependent("bad-pos-evidence-same"));
    BOOST_CHECK(!sidechain::RecordIndependent("bad-pos-sig"));
    BOOST_CHECK(!sidechain::RecordIndependent("bad-pos-ack-order"));
    BOOST_CHECK(!sidechain::RecordIndependent("pos-seed-unknown"));
    COutPoint coin;
    const CTxOut spent{Fund(COIN, coin)};
    StartPos();
    Accept(Spend(coin, spent, {CTxOut{COIN - TX_FEE, script}}));
    CBlock greedy{PosBlock(1)};
    SetCoinbase(greedy, [](CMutableTransaction& cb) {
        cb.vout[0].nValue += 1;
        for (auto& out : cb.vout) {
            if (pos::IsPotScript(out.scriptPubKey)) out.nValue -= 1;
        }
    });
    Resign(greedy);
    const CBlockIndex* tip{Tip()};
    Submit(greedy);
    BOOST_CHECK_EQUAL(Reason(greedy), "bad-pos-producer-share");
    BOOST_CHECK(Tip() == tip);
    CBlockIndex* index{Index(greedy.GetHash())};
    BOOST_REQUIRE(index);
    BOOST_CHECK(WITH_LOCK(::cs_main, return index->nStatus & BLOCK_FAILED_VALID));
    BOOST_CHECK(Record().GetFailure(greedy.GetHash()) != sidechain::Mainchain::Failure::RECORD);
    BOOST_CHECK(!WITH_LOCK(::cs_main, return Chain().ReconsiderRecordFailure(index)));
    // A header on it: its sender is punished.
    CBlock child{greedy};
    child.hashPrevBlock = greedy.GetHash();
    child.nTime += 1;
    BlockValidationState state;
    BOOST_CHECK(!Chainman().ProcessNewBlockHeaders({{child}}, /*min_pow_checked=*/true, state));
    BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_INVALID_PREV);
}

BOOST_FIXTURE_TEST_CASE(forged_signature_replaced, FastBlocksSetup)
{
    // A node that takes a header before it can check its signature (the set of its epoch is that of
    // an anchor it has not connected yet: it is catching up) may be given the header with a forged
    // signature first (the block hash does not cover it). Once it can check it, the block its leader
    // signed replaces it -- whether the node has the forged block too, which failed (bad-pos-sig), or not.
    StartPos();
    Take(PosBlock(1));
    int last{1};
    for (const bool forged_body : {false, true}) {
        const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
        const CBlock anchor{PosBlock(last + 1, ack)};
        Take(anchor);
        const int64_t epoch{pos::EpochOf(pos::SlotOf(anchor.nTime, Pos()), Pos())};
        last = static_cast<int>((epoch + 2) * Pos().slots_per_epoch - base_slot);
        const CBlock honest{PosBlock(last)};
        CBlock forged{honest};
        forged.ext.sig.fill(0);
        // The node as it was before it connected the anchor: the anchor taken back, its set forgotten.
        CBlockIndex* anchor_index{Index(anchor.GetHash())};
        {
            BlockValidationState state;
            BOOST_REQUIRE(Chain().InvalidateBlock(state, anchor_index));
            LOCK(::cs_main);
            Chain().ResetBlockFailureFlags(anchor_index);
            Chainman().RecalculateBestHeader();
        }
        Hooks().ForgetKnownSets();
        BOOST_REQUIRE(Tip() == anchor_index->pprev);
        // The forged header: taken, its signature not checked yet.
        BOOST_REQUIRE_EQUAL(HeaderReason({forged}), "");
        const CBlockIndex* index{Index(honest.GetHash())};
        BOOST_REQUIRE(index);
        if (forged_body) {
            // ... and the forged block: it fails once the anchor is connected.
            Submit(forged);
            BOOST_CHECK_EQUAL(Reason(forged), "bad-pos-sig");
            BOOST_CHECK(Tip() == anchor_index);
            // The copy's fault, not the block's: failed in memory, and neither the forged header again
            // nor a header on it is any peer's fault (an honest peer's chain goes through the block).
            BOOST_CHECK(WITH_LOCK(::cs_main, return Chainman().UncheckedExtensionFailed(*index)));
            BOOST_CHECK_EQUAL(HeaderReason({forged}, /*source=*/3), "extension-failed");
            BOOST_CHECK_EQUAL(HeaderReason({UnsignedHeader(honest, honest.GetHash(), base_slot + last + 1, 0)}, /*source=*/3), "extension-failed");
            // The honest header, from a peer: it replaces the forged one, and the block (the same
            // transactions) is connected with it.
            BOOST_CHECK_EQUAL(HeaderReason({honest}), "");
            BOOST_CHECK(WITH_LOCK(::cs_main, return !(index->nStatus & BLOCK_FAILED_VALID)));
            BlockValidationState state;
            BOOST_REQUIRE(Chain().ActivateBestChain(state));
        } else {
            BlockValidationState state;
            BOOST_REQUIRE(Chain().ActivateBestChain(state));
            BOOST_REQUIRE(Tip() == anchor_index);
            // The honest block: it replaces the forged header.
            Submit(honest);
        }
        BOOST_CHECK(Tip() == index);
        BOOST_CHECK(WITH_LOCK(::cs_main, return index->m_ext->sig == honest.ext.sig));
        CBlock read;
        BOOST_REQUIRE(Chainman().m_blockman.ReadBlock(read, *index));
        BOOST_CHECK(read.ext == honest.ext);
        // A forged one again changes nothing: a mutated block.
        BlockValidationState state;
        BOOST_CHECK(!WITH_LOCK(::cs_main, return Chainman().AcceptBlock(std::make_shared<const CBlock>(forged), state, nullptr, true, nullptr, nullptr, true)));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-blk-extension");
        BOOST_CHECK(Tip() == index);
        ++last;
    }
    Take(PosBlock(last));
}

BOOST_FIXTURE_TEST_CASE(evidence_other_network, FastBlocksSetup)
{
    // A header signed on another network (another genesis block; the slots of the networks line up)
    // is no signature here: paired with one of this network, no proof that the leader signed twice.
    StartPos();
    const CBlock local{PosBlock(1)};
    const int64_t slot{pos::SlotOf(local.nTime, Pos())};
    const CKey& leader{Leader(slot)};
    const uint256 elsewhere{CChainParams::TestNet()->GetConsensus().hashGenesisBlock};
    BOOST_REQUIRE(elsewhere != Genesis());
    CBlock foreign{local};
    SetCoinbase(foreign, [](CMutableTransaction& tx) { tx.vout.emplace_back(0, CScript() << OP_RETURN); });
    node::RegenerateCommitments(foreign, Chainman());
    BOOST_REQUIRE(pos::Producer::Sign(foreign, leader, elsewhere));
    BOOST_REQUIRE(foreign.GetHash() != local.GetHash() && foreign.nTime == local.nTime);
    BOOST_CHECK(pos::BlockSigHash(local, elsewhere) != pos::BlockSigHash(local, Genesis()));
    const pos::DoubleProposal proof{.a = local, .b = foreign};
    const uint64_t anchors{Fast().anchors};
    const sidechain::StoreView& view{ActiveView()};
    std::string reason;
    {
        LOCK(::cs_main);
        BOOST_CHECK(!Hooks().CheckChainEvidence(proof, *Tip(), slot, view, anchors, reason));
        BOOST_CHECK_EQUAL(reason, "bad-pos-evidence-sig");
    }
    BOOST_CHECK(!Hooks().AddEvidence(proof, reason));
    BOOST_CHECK(Hooks().Evidence().All().empty());
    // Signed for this network: a proof.
    CBlock twice{foreign};
    BOOST_REQUIRE(pos::Producer::Sign(twice, leader, Genesis()));
    LOCK(::cs_main);
    BOOST_CHECK(Hooks().CheckChainEvidence({.a = local, .b = twice}, *Tip(), slot, view, anchors, reason));
}

BOOST_AUTO_TEST_CASE(note_signed_per_leader)
{
    // The headers noted to find proofs, by slot and signer: a header of another leader of the slot
    // (another branch's, or one an earlier record gave) does not hide the two of this one.
    CBlockHeader a, b1, b2;
    a.nVersion = b1.nVersion = b2.nVersion = pos::POS_BLOCK_BIT;
    a.nTime = b1.nTime = b2.nTime = 1000;
    a.nNonce = 1;
    b1.nNonce = 2;
    b2.nNonce = 3;
    const uint256 leader_a{1}, leader_b{2};
    pos::EvidencePool pool;
    BOOST_CHECK(!pool.NoteSigned(a, 7, leader_a));
    BOOST_CHECK(!pool.NoteSigned(b1, 7, leader_b));
    const auto proof{pool.NoteSigned(b2, 7, leader_b)};
    BOOST_REQUIRE(proof);
    BOOST_CHECK((proof->a.GetHash() == b1.GetHash() && proof->b.GetHash() == b2.GetHash()) || (proof->a.GetHash() == b2.GetHash() && proof->b.GetHash() == b1.GetHash()));
    // The same header again is no proof.
    BOOST_CHECK(!pool.NoteSigned(b1, 7, leader_b));
    BOOST_CHECK(!pool.NoteSigned(a, 7, leader_a));
}

BOOST_FIXTURE_TEST_CASE(evidence_replay_checked_first, FastBlocksSetup)
{
    // A proof the node keeps already costs no signature check: one sent again is found in the pool
    // first, and its peer is not blamed (whatever the node can tell of the leader by then: here, with
    // no chainstate to tell it).
    StartPos();
    const auto [first, second]{TwoBlocks(1)};
    std::string reason;
    const auto offence{Hooks().AddEvidence({.a = first, .b = second}, reason)};
    BOOST_REQUIRE_MESSAGE(offence, reason);
    Hooks().Attach(nullptr);
    const auto again{Hooks().AddEvidence({.a = second, .b = first}, reason)};
    DataStream stream{};
    stream << pos::DoubleProposal{.a = first, .b = second};
    std::string misbehaving;
    BOOST_CHECK(Hooks().ProcessMessage(3, pos::MSG_EVIDENCE, stream, misbehaving));
    Hooks().Attach(&Chainman());
    BOOST_CHECK(again && *again == *offence);
    BOOST_CHECK_EQUAL(misbehaving, "");
    BOOST_CHECK_EQUAL(Hooks().Evidence().Size(), 1U);
}

BOOST_FIXTURE_TEST_CASE(unbond_replay_after_topup, FastBlocksSetup)
{
    // The owner signs an unbond of all, which is not mined (its transaction dropped from the
    // mempool, say), then adds to the bond and goes on validating. The unbond it signed is public: a
    // top-up moves the validator's unbond sequence on, so that nobody can mine it later.
    StartPos();
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    CKey validator;
    validator.MakeNewKey(/*fCompressed=*/true);
    const uint256 pubkey{XOnly(validator)};
    const CAmount min{Pos().min_stake};
    Accept(Pay(coin, spent, {BondOut(validator, 2 * min, Owner(key), &key, coin)}));
    Take(PosBlock(1));
    const uint32_t sequence{Validator(pubkey)->unbond_seq};
    const CTxOut unbond_all{UnbondOut(validator, pos::UNBOND_ALL, sequence, key)};
    BOOST_CHECK_EQUAL(StakingRules(TxWith({unbond_all})), "");
    Accept(Pay(coin, spent, {BondOut(validator, min, Owner(key), &key, coin)}));
    Take(PosBlock(2));
    BOOST_CHECK_EQUAL(Validator(pubkey)->bonded, 3 * min);
    // A stranger's transaction with it: refused, in the mempool and in blocks.
    BOOST_CHECK_EQUAL(StakingRules(TxWith({unbond_all})), "bad-pos-unbond-sequence");
    {
        COutPoint c{coin};
        CTxOut s{spent};
        BOOST_CHECK_EQUAL(Refused(Pay(c, s, {unbond_all})), "bad-pos-unbond-sequence");
    }
    // The owner's new one: taken.
    BOOST_CHECK_EQUAL(StakingRules(TxWith({UnbondOut(validator, pos::UNBOND_ALL, Validator(pubkey)->unbond_seq, key)})), "");
}

BOOST_FIXTURE_TEST_CASE(first_bond_signed, FastBlocksSetup)
{
    // The proof of possession of a first bond is public as soon as its transaction is: a stranger
    // could copy it into a bond of a satoshi of its own, mined first, and the owner's bond after it
    // would need a signature it does not carry. The owner signs first bonds too.
    StartPos();
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    CKey validator, stranger;
    validator.MakeNewKey(/*fCompressed=*/true);
    stranger.MakeNewKey(/*fCompressed=*/true);
    const uint256 pubkey{XOnly(validator)};
    const CAmount min{Pos().min_stake};
    const CTxOut pending{BondOut(validator, 2 * min, Owner(key), &key, coin)};
    const pos::Bond copied{*pos::ParseBond(pending)};
    BOOST_CHECK_EQUAL(StakingRules(TxWith({CTxOut{1, pos::BondScript(pubkey, Owner(key), copied.pop)}})), "bad-pos-bond-owner-sig");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({BondOut(validator, 1, Owner(key), &stranger)})), "bad-pos-bond-owner-sig");
    BOOST_CHECK_EQUAL(StakingRules(TxWith({CTxOut{1, pos::BondScript(pubkey, Owner(key), copied.pop, copied.owner_sig)}})), "bad-pos-bond-owner-sig");
    Accept(Pay(coin, spent, {pending}));
    Take(PosBlock(1));
    BOOST_CHECK_EQUAL(Validator(pubkey)->bonded, 2 * min);
    BOOST_CHECK(Validator(pubkey)->owner == Owner(key));
}

BOOST_FIXTURE_TEST_CASE(staking_seen_from_the_state, NoValidatorsSetup)
{
    // Blocks may be at most 10 minutes ahead of the clock while the network has blocks of stake, and
    // two hours before (the template's). That is the state's: a node that starts on a chain whose
    // newest anchor made an active set knows it before the next anchor; a reorg back below it, not.
    BOOST_CHECK_EQUAL(Hooks().MaxFutureBlockTime(), MAX_FUTURE_BLOCK_TIME);
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    Accept(Pay(coin, spent, {BondOut(keys[0], Pos().min_stake, Owner(key), &key, coin)}));
    MineBmm(BmmBlock());
    const CBlock anchor{BmmBlock(/*use_mempool=*/false)};
    MineBmm(anchor);
    BOOST_CHECK_EQUAL(Hooks().MaxFutureBlockTime(), pos::MAX_FUTURE_BLOCK_TIME_POS);
    // A node that starts now (hooks of their own over the same chain).
    pos::FastBlocksHooks restarted{*params};
    restarted.Attach(&Chainman());
    BOOST_CHECK_EQUAL(restarted.MaxFutureBlockTime(), pos::MAX_FUTURE_BLOCK_TIME_POS);
    // Back below the anchor that made the set.
    BlockValidationState state;
    BOOST_REQUIRE(Chain().InvalidateBlock(state, Index(anchor.GetHash())));
    BOOST_CHECK_EQUAL(Hooks().MaxFutureBlockTime(), MAX_FUTURE_BLOCK_TIME);
    BOOST_CHECK_EQUAL(restarted.MaxFutureBlockTime(), MAX_FUTURE_BLOCK_TIME);
    restarted.Attach(nullptr);
}

BOOST_FIXTURE_TEST_CASE(producer_retries_before_signing, SeedSetup)
{
    // A slot whose block cannot be made at first, for a reason that comes before the slashing
    // protection notes it (nothing signed), is tried again while the slot lasts: here the mainchain
    // block that seeds the epoch comes just after the slot starts. (So too a block that arrives as the
    // slot starts: the node still taking it in, or the tip moving while the block was built.)
    StartPos();
    const int64_t epoch{pos::EpochOf(base_slot, Pos()) + 1};
    const int i{static_cast<int>(epoch * Pos().slots_per_epoch - base_slot)};
    const int64_t slot{Slot(i)};
    const uint256 before{Tip()->GetBlockHash()};
    producer->Start();
    BOOST_REQUIRE(WaitFor([&] { return producer->GetStatus().tries >= 1; }));
    BOOST_CHECK(Tip()->GetBlockHash() == before);
    // The seed's mainchain block, while the clock is still in the slot.
    Main([&](sidechain::MainBlock& block) { block.time = GetTime() + 1; });
    BOOST_CHECK_MESSAGE(WaitFor([&] { return Tip()->GetBlockHash() != before; }), "no block for the slot after the seed came");
    producer->Stop();
    BOOST_CHECK_EQUAL(pos::SlotOf(Tip()->nTime, Pos()), slot);
    BOOST_CHECK_EQUAL(producer->GetStatus().produced, 1U);
    // Not a failure: the slot was made.
    BOOST_CHECK_EQUAL(producer->GetStatus().failed, 0U);
    // A failure after the slot is noted is not tried again (a second block would be a second signature).
    std::string error;
    pos::Producer::Retry retry{pos::Producer::Retry::COUNTED};
    BOOST_CHECK(!producer->Produce(slot, error, &retry));
    BOOST_CHECK(retry != pos::Producer::Retry::NO); // (the chain has a block of the slot: before the signature)
    producer->SetExtraCheckForTesting([](const CBlock&) -> std::optional<std::string> { return "refused by the test"; });
    BOOST_CHECK(!producer->Produce(Slot(i + 1), error, &retry));
    BOOST_CHECK_EQUAL(error, "the block is not valid: refused by the test");
    BOOST_CHECK(retry == pos::Producer::Retry::NO);
}

BOOST_FIXTURE_TEST_CASE(protection_next_to_the_key_file, FastBlocksSetup)
{
    // The slashing protection lives next to the validator key file, not in the data directory: wiping
    // the data directory to sync again keeps the key, and must keep the slots it signed. The file an
    // older version kept in the data directory counts too.
    ArgsManager& args{*m_node.args};
    const fs::path dir{args.GetDataDirBase() / "validator_keys"};
    fs::create_directories(dir);
    const fs::path key_file{dir / "validator.key"};
    {
        std::ofstream out{key_file.std_path()};
        out << HexStr(keys[2]) << "\n";
    }
    const fs::path legacy{args.GetDataDirNet() / "validator_protection.dat"};
    {
        std::ofstream out{legacy.std_path()};
        out << "fastblocks-slashing-protection 1\n" << HexStr(XOnly(keys[2])) << " 100\n";
    }
    args.ForceSetArg("-validatorkeyfile", fs::PathToString(key_file));
    std::string error;
    {
        const auto made{pos::Producer::FromArgs(m_node, Hooks(), error)};
        BOOST_REQUIRE_MESSAGE(made, error);
        BOOST_CHECK_EQUAL(made->LastSignedSlot(), 100);
        // Without a wallet and -validatorpayout: the key's own address.
        BOOST_CHECK(made->Payout() == GetScriptForDestination(WitnessV0KeyHash{keys[2].GetPubKey()}));
    }
    BOOST_CHECK(fs::exists(dir / "validator.key.protection"));
    // The data directory wiped: the slot is still known.
    fs::remove(legacy);
    {
        const auto made{pos::Producer::FromArgs(m_node, Hooks(), error)};
        BOOST_REQUIRE_MESSAGE(made, error);
        BOOST_CHECK_EQUAL(made->LastSignedSlot(), 100);
    }
    // A higher slot in an old file still counts (the highest of each key).
    {
        std::ofstream out{legacy.std_path()};
        out << "fastblocks-slashing-protection 1\n" << HexStr(XOnly(keys[2])) << " 150\n";
    }
    {
        const auto made{pos::Producer::FromArgs(m_node, Hooks(), error)};
        BOOST_REQUIRE_MESSAGE(made, error);
        BOOST_CHECK_EQUAL(made->LastSignedSlot(), 150);
    }
    fs::remove(legacy);
    {
        const auto made{pos::Producer::FromArgs(m_node, Hooks(), error)};
        BOOST_REQUIRE_MESSAGE(made, error);
        BOOST_CHECK_EQUAL(made->LastSignedSlot(), 150);
    }
}

BOOST_FIXTURE_TEST_CASE(mempool_drops_stale_staking_txs, FastBlocksSetup)
{
    // A bond or an unbond is checked against the tip when it enters the mempool. A block can then make
    // it one no block will take: it must leave the mempool, or it holds its inputs (and the wallet that
    // made it) until the mempool expires it, two weeks later.
    StartPos();
    COutPoint a, b, c, d;
    CTxOut spent_a{Fund(COIN, a)}, spent_b{Fund(COIN, b)}, spent_c{Fund(COIN, c)}, spent_d{Fund(COIN, d)};
    CKey validator, newcomer, other_owner;
    validator.MakeNewKey(/*fCompressed=*/true);
    newcomer.MakeNewKey(/*fCompressed=*/true);
    other_owner.MakeNewKey(/*fCompressed=*/true);
    const CAmount min{Pos().min_stake};
    Accept(Pay(a, spent_a, {BondOut(validator, 3 * min, Owner(key), &key, a)}));
    // (After the blocks of the deposits.)
    Take(PosBlock(10));
    BOOST_REQUIRE_EQUAL(m_node.mempool->size(), 0U);
    // Two unbonds the owner signed with the same sequence (the second, say, after the first seemed
    // lost), each valid on the tip: a block takes one, the other can never be mined.
    const uint32_t sequence{Validator(XOnly(validator))->unbond_seq};
    const CMutableTransaction first{Pay(a, spent_a, {UnbondOut(validator, min, sequence, key)})};
    const CMutableTransaction second{Pay(b, spent_b, {UnbondOut(validator, 2 * min, sequence, key)})};
    Accept(first);
    Accept(second);
    // Two first bonds of a key naming two owners (a wallet that bonded twice in a block): once one
    // is mined, the other names the wrong owner.
    const CMutableTransaction bond_x{Pay(c, spent_c, {BondOut(newcomer, min, Owner(key), &key, c)})};
    const CMutableTransaction bond_y{Pay(d, spent_d, {BondOut(newcomer, min, Owner(other_owner), &other_owner, d)})};
    Accept(bond_x);
    Accept(bond_y);
    BOOST_REQUIRE_EQUAL(m_node.mempool->size(), 4U);
    const CBlock block{PosBlock(11)};
    BOOST_REQUIRE_EQUAL(block.vtx.size(), 3U);
    Take(block);
    // One of each pair was mined; the other left the mempool with the block.
    BOOST_CHECK_EQUAL(m_node.mempool->size(), 0U);
    BOOST_CHECK_EQUAL(Validator(XOnly(validator))->unbond_seq, sequence + 1);
    BOOST_CHECK(Validator(XOnly(newcomer))->owner == Owner(key) || Validator(XOnly(newcomer))->owner == Owner(other_owner));
    // The inputs of those not mined are free again.
    for (const CMutableTransaction* tx : {&first, &second, &bond_x, &bond_y}) {
        const bool mined{std::any_of(block.vtx.begin(), block.vtx.end(), [&](const CTransactionRef& in) { return in->GetHash() == tx->GetHash(); })};
        if (mined) continue;
        BOOST_CHECK(!WITH_LOCK(m_node.mempool->cs, return m_node.mempool->GetConflictTx(tx->vin.at(0).prevout)));
    }
}

BOOST_FIXTURE_TEST_CASE(mempool_staking_rechecks_bounded_per_tip, FastBlocksSetup)
{
    // The mempool checks a bond or an unbond again only when a block changed the record of the
    // validator it names (TxRuleKeys), and MAX_RULE_CHECKS_PER_TIP at most at each change of the tip,
    // the rest at the next ones: a block costs the work of what it changed, bounded, however many
    // such transactions anyone put in the mempool. Here unbonds of one validator, all signed with
    // the same sequence: a block takes one, and the others, void then, leave over two blocks.
    StartPos();
    const size_t N{MAX_RULE_CHECKS_PER_TIP + 100};
    constexpr CAmount EACH{30000};
    COutPoint coin, bonded;
    CTxOut spent{Fund(COIN, coin)};
    CTxOut spent_bond{Fund(COIN, bonded)};
    CKey validator, bystander;
    validator.MakeNewKey(/*fCompressed=*/true);
    bystander.MakeNewKey(/*fCompressed=*/true);
    const CAmount min{Pos().min_stake};
    std::vector<CTxOut> outs(N + 1, CTxOut{EACH, script});
    outs.emplace_back(spent.nValue - (N + 1) * EACH - 200000, script);
    const CMutableTransaction split{Spend(coin, spent, outs)};
    Accept(split);
    Accept(Pay(bonded, spent_bond, {BondOut(validator, 3 * min, Owner(key), &key, bonded)}));
    // (After the blocks of the deposits.)
    Take(PosBlock(10));
    BOOST_REQUIRE_EQUAL(m_node.mempool->size(), 0U);
    // A bond of another key, whose record no block changes while it waits: not checked again.
    const COutPoint last{split.GetHash(), static_cast<uint32_t>(N)};
    const CMutableTransaction other_bond{Spend(last, outs[N], {BondOut(bystander, 1, Owner(key), &key, last), CTxOut{EACH - 1 - TX_FEE, script}})};
    Accept(other_bond);
    const uint32_t sequence{Validator(XOnly(validator))->unbond_seq};
    for (size_t i{0}; i < N; ++i) {
        const COutPoint in{split.GetHash(), static_cast<uint32_t>(i)};
        Accept(Spend(in, outs[i], {UnbondOut(validator, min, sequence, key), CTxOut{EACH - TX_FEE, script}}));
    }
    BOOST_REQUIRE_EQUAL(m_node.mempool->size(), N + 1);
    {
        LOCK(m_node.mempool->cs);
        BOOST_CHECK_EQUAL(m_node.mempool->m_rule_keys_of.size(), N + 1);
        BOOST_CHECK_EQUAL(m_node.mempool->m_rule_txs.size(), 2U);
        BOOST_CHECK(m_node.mempool->m_rule_txs_any.empty());
    }
    // Nothing changed: nothing checked again.
    {
        LOCK2(::cs_main, m_node.mempool->cs);
        Chain().RemoveStaleDrivechainTxs();
        BOOST_CHECK(m_node.mempool->m_rule_checks.empty());
    }
    // A block takes one unbond (and the other bond, if it likes): the other unbonds are void,
    // MAX_RULE_CHECKS_PER_TIP of them leave now ...
    const CBlock block{PosBlock(11)};
    Take(block);
    BOOST_CHECK_EQUAL(Validator(XOnly(validator))->unbond_seq, sequence + 1);
    const bool other_mined{std::any_of(block.vtx.begin(), block.vtx.end(), [&](const CTransactionRef& tx) { return tx->GetHash() == other_bond.GetHash(); })};
    const size_t left{N - 1 - MAX_RULE_CHECKS_PER_TIP};
    BOOST_CHECK_EQUAL(m_node.mempool->size(), left + (other_mined ? 0 : 1));
    BOOST_CHECK_EQUAL(WITH_LOCK(m_node.mempool->cs, return m_node.mempool->m_rule_checks.size()), left);
    // ... the rest at the next block.
    Take(PosBlock(12));
    BOOST_CHECK_EQUAL(m_node.mempool->size(), 0U);
    BOOST_CHECK(WITH_LOCK(m_node.mempool->cs, return m_node.mempool->m_rule_checks.empty()));
}

#ifdef ENABLE_WALLET
BOOST_FIXTURE_TEST_CASE(wallet_finality_on_its_own_chain, FastBlocksSetup)
{
    // The finality the wallet shows is that of its own chain, the one that ends at its last block: in
    // a reorg the node's tip moves first, and the anchors of the new branch must not make a payment
    // the reorg took out look anchored or final. (The wallet here never hears of the reorg, as one
    // that has not caught up with the validation queue.)
    StartPos();
    Take(PosBlock(1));
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    CKey wallet_key;
    wallet_key.MakeNewKey(/*fCompressed=*/true);
    const CMutableTransaction payment{Pay(coin, spent, {CTxOut{COIN / 2, GetScriptForDestination(WitnessV0KeyHash{wallet_key.GetPubKey()})}})};
    Accept(payment);
    const CBlockIndex* fork{Tip()};
    const CBlock paying{PosBlock(2)};
    Take(paying);
    const int height{Tip()->nHeight};
    std::shared_ptr<wallet::CWallet> wallet{wallet::CreateSyncedWallet(*m_node.chain, Chain().m_chain, wallet_key)};
    wallet::WalletContext context;
    context.chain = m_node.chain.get();
    context.args = m_node.args;
    BOOST_REQUIRE(wallet::AddWallet(context, wallet));
    const auto call{[&](RPCMethod (*method)(), UniValue params) {
        JSONRPCRequest request;
        request.context = &context;
        request.params = std::move(params);
        return method().HandleRequest(request);
    }};
    // On its chain: soft (no anchor covers the block yet).
    BOOST_CHECK_EQUAL(call(wallet::getbalances, UniValue{UniValue::VARR})["mine"]["finality"]["soft"].getValStr(), "0.50000000");
    // Another branch from below the payment's block, without it, with an anchor that covers its height.
    BlockValidationState state;
    BOOST_REQUIRE(Chain().InvalidateBlock(state, Index(paying.GetHash())));
    BOOST_REQUIRE(Tip() == fork);
    WITH_LOCK(m_node.mempool->cs, m_node.mempool->removeRecursive(CTransaction{payment}, MemPoolRemovalReason::CONFLICT));
    Take(PosBlock(3));
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, height);
    AckTip(4);
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return pos::AnchoredHeights(Hooks(), *Tip(), Pos().final_anchors)).at(0), height);
    // The wallet still has the payment at that height, in the block of the other branch.
    BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->GetLastBlockHash()) == paying.GetHash());
    const UniValue finality{call(wallet::getbalances, UniValue{UniValue::VARR})["mine"]["finality"]};
    BOOST_CHECK_EQUAL(finality["soft"].getValStr(), "0.50000000");
    BOOST_CHECK_EQUAL(finality["anchored"].getValStr(), "0.00000000");
    BOOST_CHECK_EQUAL(finality["final"].getValStr(), "0.00000000");
    UniValue params{UniValue::VARR};
    params.push_back(payment.GetHash().GetHex());
    BOOST_CHECK_EQUAL(call(wallet::gettransaction, params)["finality"].get_str(), "soft");
    BOOST_REQUIRE(wallet::RemoveWallet(context, wallet, /*load_on_start=*/std::nullopt));
}
#endif // ENABLE_WALLET


//
// Adversarial review (2026-10-13): attacks on the headers and blocks the node cannot check yet, by
// peers without validator keys. Each test asserts what doc/fastblocks.md promises; one that FAILS is
// an attack that SUCCEEDS on the code it ran against.
//

namespace {
/**
 * A fork the node cannot check: a BMM block on the tip, committed to, whose header alone the node has
 * (one mainchain commitment, which anyone can buy), and the clock two epochs later. Headers of blocks
 * of stake on it are unchecked (their epoch's set is that of an anchor the node never connected), and
 * anyone can make them. Returns {the BMM block, a header to copy the version from, a slot to use}.
 */
struct UncheckedFork {
    CBlock other;
    CBlockHeader like;
    int64_t slot{0};
};
} // namespace

#define MAKE_UNCHECKED_FORK(fork)                                                                                       \
    StartPos();                                                                                                         \
    Take(PosBlock(1));                                                                                                  \
    UncheckedFork fork;                                                                                                 \
    fork.like = Tip()->GetBlockHeader();                                                                                \
    fork.other = BmmBlock(/*use_mempool=*/false);                                                                       \
    Commit(fork.other.GetHash());                                                                                       \
    BOOST_REQUIRE_EQUAL(HeaderReason({fork.other}), "");                                                                \
    Mock(pos::SlotTime(pos::SlotOf(GetTime(), Pos()) + 2 * Pos().slots_per_epoch, Pos()));                              \
    Main();                                                                                                             \
    fork.slot = pos::SlotOf(GetTime(), Pos());                                                                          \
    BOOST_REQUIRE(!WITH_LOCK(::cs_main, return Hooks().SetFor(*Index(fork.other.GetHash()), pos::EpochOf(fork.slot - 10, Pos()))));

BOOST_FIXTURE_TEST_CASE(attack_unchecked_budget_locked_by_shared_headers, FastBlocksSetup)
{
    // ATTACK THAT SUCCEEDS (this test fails on the code it was written against): attackers fill the
    // node's whole budget of unchecked headers with headers that two of them hold each. Eviction takes
    // only headers a peer holds *alone* (m_unchecked_sole), so nothing is ever evicted, and an honest
    // peer that holds nothing gets `unchecked-budget` for as long as the attackers stay connected: a
    // node on the other side of a partition (doc (b)) never takes the honest branch. Four inbound
    // connections on regtest (4000 = 2 x 2000), ten on main (50000 = 5 x 10000), one commitment.
    auto& params{const_cast<Consensus::PosParams&>(Pos())};
    const int64_t per_peer_before{params.unchecked_headers_per_peer};
    params.unchecked_headers_per_peer = 4;
    MAKE_UNCHECKED_FORK(fork);
    for (const int64_t first : {1, 3}) {
        std::vector<CBlockHeader> chain;
        uint256 prev{fork.other.GetHash()};
        for (int i{0}; i < 4; ++i) {
            chain.push_back(UnsignedHeader(fork.like, prev, fork.slot - 10 + i, 0, static_cast<uint8_t>(first)));
            prev = chain.back().GetHash();
        }
        BOOST_REQUIRE_EQUAL(HeaderReason(chain, /*source=*/first), "");
        // The second attacker sends the newest header only: it holds the chain below it too.
        BOOST_REQUIRE_EQUAL(HeaderReason({chain.back()}, /*source=*/first + 1), "");
    }
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return Chainman().m_unchecked.size()), 8U);
    // The honest peer holds nothing; its budget is its own (doc: "attackers cannot use, take nor drop" it).
    const CBlockHeader honest{UnsignedHeader(fork.like, fork.other.GetHash(), fork.slot - 10, 0, 9)};
    BOOST_CHECK_EQUAL(HeaderReason({honest}, /*source=*/5), "");
    WITH_LOCK(::cs_main, Chainman().CheckUnchecked());
    params.unchecked_headers_per_peer = per_peer_before;
}

BOOST_FIXTURE_TEST_CASE(attack_unchecked_sybils_drop_honest_headers, FastBlocksSetup)
{
    // ATTACK THAT SUCCEEDS (this test fails on the code it was written against): an honest peer that
    // holds its whole budget (unchecked_headers_per_peer) loses headers to new attacker connections:
    // each newcomer evicts from the peer that alone holds the most (ties: the lowest peer id, which
    // an honest outbound peer, connected first, has). With N attackers the honest peer keeps about
    // total / (N + 1) headers (50000 / 115 = 434 slots on main), not per_peer: doc (b) says the
    // honest peer's budget cannot be taken nor dropped, and the "Limit" is per_peer slots.
    auto& params{const_cast<Consensus::PosParams&>(Pos())};
    const int64_t per_peer_before{params.unchecked_headers_per_peer};
    params.unchecked_headers_per_peer = 6;
    MAKE_UNCHECKED_FORK(fork);
    const auto chain_of{[&](uint8_t salt) {
        std::vector<CBlockHeader> chain;
        uint256 prev{fork.other.GetHash()};
        for (int i{0}; i < 6; ++i) {
            chain.push_back(UnsignedHeader(fork.like, prev, fork.slot - 10 + i, 0, salt));
            prev = chain.back().GetHash();
        }
        return chain;
    }};
    // The honest peer (the lowest id), with as many headers as its budget allows.
    const std::vector<CBlockHeader> honest{chain_of(0x40)};
    BOOST_REQUIRE_EQUAL(HeaderReason(honest, /*source=*/1), "");
    // Sybils, each with its own siblings.
    for (int64_t peer{2}; peer <= 6; ++peer) HeaderReason(chain_of(static_cast<uint8_t>(0x40 + peer)), peer);
    size_t kept{0};
    for (const CBlockHeader& header : honest) kept += Index(header.GetHash()) != nullptr;
    BOOST_TEST_MESSAGE("honest headers kept: " << kept << " of " << honest.size());
    BOOST_CHECK_EQUAL(kept, honest.size());
    WITH_LOCK(::cs_main, Chainman().CheckUnchecked());
    params.unchecked_headers_per_peer = per_peer_before;
}

BOOST_FIXTURE_TEST_CASE(attack_unchecked_blocks_fill_the_disk, FastBlocksSetup)
{
    // ATTACK THAT SUCCEEDS (this test fails on the code it was written against): the block of an
    // unchecked header is written to the block files (unrequested: it has more work than the tip and
    // is near it), and when its holder disconnects the header leaves the block index but the bytes stay
    // in blk*.dat for ever. Reconnecting and sending the same block again writes it again: disk grows
    // without bound, for free (no signature, no punishment, and the "new block" even protects the
    // attacker from inbound eviction: CNode::m_last_block_time).
    MAKE_UNCHECKED_FORK(fork);
    const int height{Tip()->nHeight + 2};
    CBlock block;
    static_cast<CBlockHeader&>(block) = UnsignedHeader(fork.like, fork.other.GetHash(), fork.slot - 10, 0, 0x55);
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << height << OP_0;
    coinbase.vout.emplace_back(0, CScript() << OP_RETURN << std::vector<unsigned char>(100000, 0x42));
    block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    block.hashMerkleRoot = BlockMerkleRoot(block);
    const size_t size{::GetSerializeSize(TX_WITH_WITNESS(block))};
    std::optional<std::pair<int, unsigned int>> first;
    std::pair<int, unsigned int> last;
    for (int64_t peer{100}; peer < 120; ++peer) {
        WITH_LOCK(::cs_main, Chainman().UncheckedSourceAdded(peer));
        bool new_block{false};
        Chainman().ProcessNewBlock(std::make_shared<const CBlock>(block), /*force_processing=*/false, /*min_pow_checked=*/true, &new_block, peer);
        const CBlockIndex* index{Index(block.GetHash())};
        BOOST_REQUIRE(index);
        BOOST_REQUIRE_MESSAGE(WITH_LOCK(::cs_main, return index->m_unchecked && (index->nStatus & BLOCK_HAVE_DATA)), "the attacker's block was not stored: " + Reason(block));
        last = WITH_LOCK(::cs_main, return std::make_pair(index->nFile, index->nDataPos));
        if (!first) first = last;
        // The peer disconnects: the header leaves the block index, the block's bytes stay on disk.
        WITH_LOCK(::cs_main, Chainman().UncheckedSourceGone(peer));
        BOOST_REQUIRE(!Index(block.GetHash()));
    }
    const int64_t grown{last.first == first->first ? int64_t{last.second} - int64_t{first->second} : int64_t{1} << 40};
    BOOST_TEST_MESSAGE("block of " << size << " bytes; block files grew by " << grown << " bytes over 20 reconnections");
    // Bounded disk: the same block, sent again, is not written again (nor are blocks no one holds kept).
    BOOST_CHECK_LE(grown, static_cast<int64_t>(size));
}

BOOST_FIXTURE_TEST_CASE(attack_inflated_best_header_does_not_stop_the_chain, FastBlocksSetup)
{
    // Attack that fails (a regression test): an attacker that stays connected keeps a best header
    // inflated by unchecked headers that claim every commitment on record (more anchors than any
    // chain). Nothing that makes progress waits on it: the leader still makes its blocks on the tip
    // (Producer::Behind looks at blocks, not headers), and they are connected.
    MAKE_UNCHECKED_FORK(fork);
    std::vector<uint32_t> commitments;
    for (uint8_t i{1}; i <= 3; ++i) {
        Commit(uint256{static_cast<uint8_t>(0xd0 + i)});
        commitments.push_back(static_cast<uint32_t>(Record().Height()));
    }
    std::vector<CBlockHeader> inflated;
    uint256 prev{fork.other.GetHash()};
    for (size_t i{0}; i < commitments.size(); ++i) {
        inflated.push_back(UnsignedHeader(fork.like, prev, fork.slot - 10 + static_cast<int64_t>(i), commitments[i], 0x66));
        prev = inflated.back().GetHash();
    }
    BOOST_REQUIRE_EQUAL(HeaderReason(inflated, /*source=*/1), "");
    BOOST_REQUIRE(BestHeader() == inflated.back().GetHash());
    const int next{static_cast<int>(pos::SlotOf(GetTime(), Pos()) - base_slot) + 1};
    for (int i{0}; i < 3; ++i) {
        const CBlockIndex* tip{Tip()};
        const int64_t slot{Slot(next + i)};
        std::string error;
        const auto made{producer->Produce(slot, error)};
        BOOST_CHECK_MESSAGE(made, error);
        m_node.validation_signals->SyncWithValidationInterfaceQueue();
        BOOST_CHECK(made && Tip()->GetBlockHash() == *made && Tip()->pprev == tip);
    }
    BOOST_CHECK(BestHeader() == inflated.back().GetHash());
}

BOOST_FIXTURE_TEST_CASE(attack_waiting_block_held_by_the_node, FastBlocksSetup)
{
    // ATTACK THAT SUCCEEDS (this test fails on the code it was written against): unchecked headers
    // that leave no trace in any peer's budget, and never leave the block index. A peer sends, as
    // `block` messages (unsolicited), blocks of stake on a branch the node cannot check, each
    // acknowledging the next mainchain height (not on record yet: `pos-ack-unknown`, a wait reason).
    // ContextualCheckBlockHeader keeps each header to try again with peer -1 (AddBmmWaiting), and only
    // the headers message path puts the peer's id there: the block path does not. When the mainchain
    // has a commitment at that height (any BMM block or anchor), RetryBmmWaiting takes them with
    // source -1, the node's own: no budget is checked, and the node holds them (and the unchecked
    // headers below them) until it restarts. 1000 a commitment (MAX_BMM_WAITING), again and again:
    // memory grows without bound, beyond the documented worst case (doc: "gone with the peers").
    auto& params{const_cast<Consensus::PosParams&>(Pos())};
    const int64_t per_peer_before{params.unchecked_headers_per_peer};
    params.unchecked_headers_per_peer = 3;
    MAKE_UNCHECKED_FORK(fork);
    constexpr int64_t attacker{9};
    const uint32_t next_height{static_cast<uint32_t>(Record().Height() + 1)};
    const size_t waiting_before{Waiting()};
    const size_t unchecked_before{WITH_LOCK(::cs_main, return Chainman().m_unchecked.size())};
    std::vector<uint256> sent;
    for (uint8_t salt{1}; salt <= 10; ++salt) {
        CBlock block;
        static_cast<CBlockHeader&>(block) = UnsignedHeader(fork.like, fork.other.GetHash(), fork.slot - 10, next_height, salt);
        pos::PosSections sections;
        sections.ack = pos::AnchorAck{.ticket = Ticket(uint256{salt}, script), .main_height = static_cast<int32_t>(next_height)};
        block.vSections = sections.Encode();
        block.ext.hashSections = pos::SectionsHash(block.vSections);
        CMutableTransaction coinbase;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << (Tip()->nHeight + 2) << static_cast<int64_t>(salt);
        coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        bool new_block{false};
        // As net_processing's ProcessBlock does for a `block` message from the attacker.
        Chainman().ProcessNewBlock(std::make_shared<const CBlock>(block), /*force_processing=*/false, /*min_pow_checked=*/true, &new_block, attacker);
        BOOST_REQUIRE(!new_block);
        sent.push_back(block.GetHash());
    }
    // They wait, with no peer (as the node's own).
    BOOST_REQUIRE_EQUAL(Waiting(), waiting_before + sent.size());
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return Chainman().m_bmm_waiting.at(sent[0]).peer), -1);
    // The mainchain block at that height commits to something (any anchor or BMM block of the chain).
    Commit(uint256{0xee});
    BOOST_REQUIRE_EQUAL(static_cast<uint32_t>(Record().Height()), next_height);
    (void)Chainman().RetryBmmWaiting();
    // The attacker leaves.
    WITH_LOCK(::cs_main, Chainman().UncheckedSourceGone(attacker));
    size_t kept{0};
    for (const uint256& hash : sent) kept += Index(hash) != nullptr;
    const size_t unchecked_after{WITH_LOCK(::cs_main, return Chainman().m_unchecked.size())};
    BOOST_TEST_MESSAGE("attacker's headers still in the block index after it left: " << kept << " of " << sent.size()
                       << " (its budget: " << params.unchecked_headers_per_peer << "); unchecked headers " << unchecked_before << " -> " << unchecked_after
                       << ", the node holds " << WITH_LOCK(::cs_main, return Chainman().UncheckedHeld(-1)));
    // What a peer sent leaves with it, and it never held more than its budget.
    BOOST_CHECK_EQUAL(kept, 0U);
    BOOST_CHECK_EQUAL(unchecked_after, unchecked_before);
    WITH_LOCK(::cs_main, Chainman().CheckUnchecked());
    params.unchecked_headers_per_peer = per_peer_before;
}

//
// Adversarial review 4 (attacks; a failing check is an attack that works).
//

BOOST_FIXTURE_TEST_CASE(evidence_pool_of_a_banned_validator, FastBlocksSetup)
{
    // A full pool (MAX_EVIDENCE proofs) against one validator that is banned already: none of them
    // takes anything more. Leaving them out of a block of stake, or of a merged mined block, costs
    // the node little: no signature is checked again (cs_main is held all the while).
    StartPos();
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    const CAmount bond{100000};
    Accept(Pay(coin, spent, {BondOut(keys[0], bond, Owner(key), &key, coin), BondOut(keys[1], bond, Owner(key), &key, coin), BondOut(keys[2], bond, Owner(key), &key, coin)}));
    Take(PosBlock(1));
    const auto [a1, b1]{TwoBlocks(2)};
    const CKey& offender_key{Leader(base_slot + 2)};
    const uint256 offender{XOnly(offender_key)};
    Take(a1);
    int k{3};
    while (XOnly(Leader(base_slot + k)) == offender) ++k;
    Take(WithEvidence(k, {{.a = a1, .b = b1}}));
    BOOST_REQUIRE(Validator(offender)->tombstoned);

    // Two headers signed by the offender for each of MAX_EVIDENCE slots it leads.
    const CBlock base{a1};
    int64_t slot{base_slot + k + 1};
    while (Hooks().Evidence().Size() < pos::MAX_EVIDENCE) {
        const auto leader{Hooks().ActiveLeaderOf(slot)};
        BOOST_REQUIRE(leader.status == pos::FastBlocksHooks::LeaderStatus::OK);
        if (leader.pubkey == offender) {
            CBlock a{base};
            a.nTime = pos::SlotTime(slot, Pos());
            BOOST_REQUIRE(pos::Producer::Sign(a, offender_key, Genesis()));
            CBlock b{a};
            b.hashMerkleRoot = ArithToUint256(arith_uint256{static_cast<uint64_t>(slot)});
            BOOST_REQUIRE(pos::Producer::Sign(b, offender_key, Genesis()));
            BOOST_REQUIRE(Hooks().Evidence().Add({.pubkey = offender, .slot = slot}, {.a = a, .b = b}));
        }
        ++slot;
    }
    // A slot after them all, led by another validator.
    int m{static_cast<int>(slot - base_slot)};
    while (XOnly(Leader(base_slot + m)) == offender) ++m;

    // A merged mined block: no proof in its coinbase, and no signature checked (the work counted, not timed).
    {
        LOCK(::cs_main);
        const uint64_t sigs_before{pos::BlockSigChecks()};
        const auto outputs{Hooks().MergeMinedOutputs(Chain().SideCache(), *Tip(), pos::SlotTime(base_slot + m, Pos()))};
        const uint64_t sigs{pos::BlockSigChecks() - sigs_before};
        BOOST_TEST_MESSAGE("MergeMinedOutputs with " << pos::MAX_EVIDENCE << " proofs against a banned validator: " << sigs << " signatures checked");
        BOOST_CHECK(outputs.empty());
        BOOST_CHECK_EQUAL(sigs, 0U);
    }
    // A block of stake (with an acknowledgement: the slot is far past the last anchor): none in its
    // sections, and no signature of a proof checked (the block's own are its producer's).
    Anchor(Ticket(Tip()->GetBlockHash(), script));
    std::string error;
    const uint64_t pool_sigs_before{Hooks().Evidence().SigChecks()};
    BOOST_REQUIRE_MESSAGE(producer->Produce(Slot(m), error), error);
    const uint64_t pool_sigs{Hooks().Evidence().SigChecks() - pool_sigs_before};
    BOOST_TEST_MESSAGE("Produce with " << pos::MAX_EVIDENCE << " proofs against a banned validator: " << pool_sigs << " signatures of proofs checked");
    BOOST_CHECK_EQUAL(pool_sigs, 0U);
    CBlock made;
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return Chainman().m_blockman.ReadBlock(made, *Tip())));
    pos::PosSections sections;
    BOOST_REQUIRE(pos::PosSections::Decode(made.vSections, sections));
    BOOST_CHECK(sections.evidence.empty());
}

BOOST_FIXTURE_TEST_CASE(evidence_sig_checks_bounded, FastBlocksSetup)
{
    // Proofs the pool keeps under a leader another chain names (signed by a key that does not lead
    // their slots here): on this chain their signatures fail, which only checking them tells. A block
    // this node makes checks those of MAX_EVIDENCE_SIG_CHECKS proofs at most, and remembers each
    // result: the next block checks others, and none is checked twice.
    StartPos();
    const CBlock base{PosBlock(1)};
    const int64_t until{base_slot + 400};
    std::vector<std::pair<pos::DoubleProposal, uint256>> proofs; // with the leader of its slot here
    for (int64_t slot{base_slot + 2}; slot < until; ++slot) {
        const auto leader{Hooks().ActiveLeaderOf(slot)};
        BOOST_REQUIRE(leader.status == pos::FastBlocksHooks::LeaderStatus::OK);
        const CKey* signer{nullptr};
        for (const CKey& k : keys) {
            if (XOnly(k) != leader.pubkey) signer = &k;
        }
        BOOST_REQUIRE(signer);
        CBlock a{base};
        a.nTime = pos::SlotTime(slot, Pos());
        BOOST_REQUIRE(pos::Producer::Sign(a, *signer, Genesis()));
        CBlock b{a};
        b.hashMerkleRoot = ArithToUint256(arith_uint256{static_cast<uint64_t>(slot)});
        BOOST_REQUIRE(pos::Producer::Sign(b, *signer, Genesis()));
        const pos::DoubleProposal proof{.a = a, .b = b};
        BOOST_REQUIRE(Hooks().Evidence().Add({.pubkey = XOnly(*signer), .slot = slot}, proof));
        proofs.emplace_back(proof, leader.pubkey);
    }
    BOOST_REQUIRE_GT(proofs.size(), 3 * pos::MAX_EVIDENCE_SIG_CHECKS);
    const auto checked{[&] {
        return std::count_if(proofs.begin(), proofs.end(), [&](const auto& p) { return Hooks().Evidence().SigsKnown(p.first, p.second); });
    }};
    BOOST_CHECK_EQUAL(checked(), 0);
    for (size_t round{1}; round <= 3; ++round) {
        LOCK(::cs_main);
        BOOST_CHECK(Hooks().MergeMinedOutputs(Chain().SideCache(), *Tip(), pos::SlotTime(until, Pos())).empty());
        BOOST_CHECK_EQUAL(checked(), round * pos::MAX_EVIDENCE_SIG_CHECKS);
    }
    // The pool's own proofs, checked against the key they were kept under, are known as good.
    std::string reason;
    for (const auto& [offence, proof] : Hooks().Evidence().All()) {
        BOOST_CHECK(Hooks().Evidence().SigsKnown(proof, offence.pubkey));
        BOOST_CHECK(Hooks().Evidence().CheckSigs(proof, offence.pubkey, Genesis()));
    }
    // A result is pure: checked against a leader, the same as the check itself.
    for (const auto& [proof, leader] : proofs) {
        const bool known{Hooks().Evidence().SigsKnown(proof, leader)};
        bool now{known};
        BOOST_CHECK(!Hooks().Evidence().CheckSigs(proof, leader, Genesis(), &now));
        BOOST_CHECK_EQUAL(now, !known);
        BOOST_CHECK(Hooks().Evidence().SigsKnown(proof, leader));
        BOOST_CHECK(!pos::CheckEvidence(proof, leader, Genesis(), Pos(), reason));
    }
    // Bounded: the oldest results go first.
    pos::EvidencePool pool;
    const pos::DoubleProposal& first{proofs[0].first};
    for (size_t i{0}; i < pos::MAX_CHECKED_SIGNATURES; ++i) pool.CheckSigs(proofs[i % proofs.size()].first, uint256{static_cast<uint8_t>(i / proofs.size() + 1)}, Genesis());
    BOOST_CHECK(pool.SigsKnown(first, uint256{1}));
    pool.CheckSigs(first, uint256{0xff}, Genesis());
    BOOST_CHECK(!pool.SigsKnown(first, uint256{1}));
    BOOST_CHECK(pool.SigsKnown(first, uint256{0xff}));
}

BOOST_AUTO_TEST_CASE(evidence_rate_limiter)
{
    using namespace std::chrono_literals;
    pos::EvidenceRateLimiter limiter;
    const std::chrono::milliseconds start{1'000'000};
    // A burst, then nothing more...
    for (int i{0}; i < static_cast<int>(pos::EVIDENCE_CHECKS_BURST); ++i) BOOST_CHECK(limiter.Allow(1, start));
    BOOST_CHECK(!limiter.Allow(1, start));
    BOOST_CHECK(!limiter.Allow(1, start + 400ms));
    // ... but the rate (2 a second: one each half second); another peer has its own.
    BOOST_CHECK(limiter.Allow(2, start));
    BOOST_CHECK(limiter.Allow(1, start + 500ms));
    BOOST_CHECK(!limiter.Allow(1, start + 500ms));
    BOOST_CHECK(limiter.Allow(1, start + 1500ms));
    BOOST_CHECK(limiter.Allow(1, start + 1500ms));
    BOOST_CHECK(!limiter.Allow(1, start + 1500ms));
    // A clock that goes back gives nothing.
    BOOST_CHECK(!limiter.Allow(1, start));
    // A burst again after a long while, no more.
    for (int i{0}; i < static_cast<int>(pos::EVIDENCE_CHECKS_BURST); ++i) BOOST_CHECK(limiter.Allow(1, start + 1h));
    BOOST_CHECK(!limiter.Allow(1, start + 1h));
    // Bounded: peers whose buckets are full go first (peer 2's refilled), then the oldest.
    for (int64_t peer{10}; peer < 10 + static_cast<int64_t>(pos::MAX_EVIDENCE_RATE_PEERS) - 1; ++peer) BOOST_CHECK(limiter.Allow(peer, start + 1h));
    BOOST_CHECK_EQUAL(limiter.Peers(), pos::MAX_EVIDENCE_RATE_PEERS);
    BOOST_CHECK(!limiter.Allow(1, start + 1h));
    BOOST_CHECK(limiter.Allow(5000, start + 1h));
    BOOST_CHECK_EQUAL(limiter.Peers(), pos::MAX_EVIDENCE_RATE_PEERS);
    // (Peer 1 went: a new bucket, full.)
    BOOST_CHECK(limiter.Allow(1, start + 1h));
}

BOOST_FIXTURE_TEST_CASE(evidence_relay_rate, FastBlocksSetup)
{
    // A peer relays proofs whose signatures fail here (not its fault: its chain may name other
    // leaders), without end. The node checks EVIDENCE_CHECKS_BURST of them, then EVIDENCE_CHECKS_PER_SECOND
    // a second: the rest are dropped unchecked -- a true proof among them too -- and the peer is not
    // blamed. Other peers have their own rate; a proof the node keeps already costs nothing.
    StartPos();
    const CBlock first{PosBlock(1)};
    const int64_t slot{pos::SlotOf(first.nTime, Pos())};
    const CKey& other{NotLeader(slot)};
    const auto send{[&](int64_t peer, const pos::DoubleProposal& proof) {
        DataStream stream{};
        stream << proof;
        std::string misbehaving;
        BOOST_CHECK(Hooks().ProcessMessage(peer, pos::MSG_EVIDENCE, stream, misbehaving));
        return misbehaving;
    }};
    const auto forged{[&](uint64_t n) {
        CBlock a{first}, b{first};
        a.hashMerkleRoot = ArithToUint256(arith_uint256{2 * n + 1});
        b.hashMerkleRoot = ArithToUint256(arith_uint256{2 * n + 2});
        BOOST_REQUIRE(pos::Producer::Sign(a, other, Genesis()));
        BOOST_REQUIRE(pos::Producer::Sign(b, other, Genesis()));
        return pos::DoubleProposal{.a = a, .b = b};
    }};
    CBlock second{first};
    SetCoinbase(second, [](CMutableTransaction& tx) { tx.vout.emplace_back(0, CScript() << OP_RETURN); });
    Resign(second);
    const pos::DoubleProposal proof{.a = first, .b = second};

    int dropped{0};
    const auto callback{LogInstance().PushBackCallback([&](const std::string& line) {
        if (line.find("dropped unchecked") != std::string::npos) ++dropped;
    })};
    const bool net_logged{LogInstance().WillLogCategory(BCLog::NET)};
    LogInstance().EnableCategory(BCLog::NET);
    uint64_t n{0};
    for (; n < static_cast<uint64_t>(pos::EVIDENCE_CHECKS_BURST); ++n) BOOST_CHECK_EQUAL(send(3, forged(n)), "");
    BOOST_CHECK_EQUAL(dropped, 0);
    for (int i{0}; i < 100; ++i) BOOST_CHECK_EQUAL(send(3, forged(n++)), "");
    BOOST_CHECK_EQUAL(dropped, 100);
    // The true proof, from that peer: dropped as well; from another, taken.
    BOOST_CHECK_EQUAL(send(3, proof), "");
    BOOST_CHECK_EQUAL(dropped, 101);
    BOOST_CHECK_EQUAL(Hooks().Evidence().Size(), 0U);
    BOOST_CHECK_EQUAL(send(4, proof), "");
    BOOST_CHECK_EQUAL(Hooks().Evidence().Size(), 1U);
    // Known now: from the first peer, no check, nothing dropped.
    BOOST_CHECK_EQUAL(send(3, proof), "");
    BOOST_CHECK_EQUAL(dropped, 101);
    // A second later, the first peer has its rate again.
    Mock(GetTime() + 1);
    for (int i{0}; i < static_cast<int>(pos::EVIDENCE_CHECKS_PER_SECOND); ++i) BOOST_CHECK_EQUAL(send(3, forged(n++)), "");
    BOOST_CHECK_EQUAL(dropped, 101);
    BOOST_CHECK_EQUAL(send(3, forged(n++)), "");
    BOOST_CHECK_EQUAL(dropped, 102);
    // A proof whose form is wrong is the peer's fault, over its rate or not.
    BOOST_CHECK_EQUAL(send(3, {.a = first, .b = first}), "invalid evidence: bad-pos-evidence-same");
    LogInstance().DeleteCallback(callback);
    if (!net_logged) LogInstance().DisableCategory(BCLog::NET);
}

BOOST_FIXTURE_TEST_CASE(attack4_evicted_proof_relayed_again, FastBlocksSetup)
{
    // ATTACK (a single validator, or one that has unbonded: it loses nothing). The evidence pool keeps
    // MAX_EVIDENCE proofs, the oldest slots go first, and relay has no window: a validator signs two
    // headers for each of MAX_EVIDENCE + 1 slots it leads (old slots are as good as new ones). Once the
    // pool is full, the proof of the oldest slot is taken again each time it comes (it is no longer
    // known, it is added, and it is the one pushed out at once): each time logged as a warning and
    // relayed to every peer but the sender. Nodes in a cycle send it round for ever.
    StartPos();
    const CBlock base{PosBlock(1)};
    const uint256 attacker{XOnly(keys[0])};
    std::vector<pos::DoubleProposal> proofs;
    for (int64_t slot{base_slot + 2}; proofs.size() <= pos::MAX_EVIDENCE && slot < base_slot + 3500; ++slot) {
        const auto leader{Hooks().ActiveLeaderOf(slot)};
        if (leader.status != pos::FastBlocksHooks::LeaderStatus::OK || leader.pubkey != attacker) continue;
        CBlock a{base};
        a.nTime = pos::SlotTime(slot, Pos());
        BOOST_REQUIRE(pos::Producer::Sign(a, keys[0], Genesis()));
        CBlock b{a};
        b.hashMerkleRoot = ArithToUint256(arith_uint256{static_cast<uint64_t>(slot)});
        BOOST_REQUIRE(pos::Producer::Sign(b, keys[0], Genesis()));
        proofs.push_back({.a = a, .b = b});
    }
    BOOST_REQUIRE_EQUAL(proofs.size(), pos::MAX_EVIDENCE + 1);
    std::string reason;
    for (const auto& proof : proofs) BOOST_REQUIRE_MESSAGE(Hooks().AddEvidence(proof, reason), reason);
    BOOST_REQUIRE_EQUAL(Hooks().Evidence().Size(), pos::MAX_EVIDENCE);
    // The oldest one, sent again (by the attacker, or by an honest peer that relays it on).
    std::atomic<int> taken{0};
    const auto callback{LogInstance().PushBackCallback([&](const std::string& line) {
        if (line.find("signed two blocks for slot") != std::string::npos) ++taken;
    })};
    for (int i{0}; i < 3; ++i) BOOST_CHECK(Hooks().AddEvidence(proofs[0], reason));
    LogInstance().DeleteCallback(callback);
    BOOST_TEST_MESSAGE("the oldest proof, sent 3 times more, was taken as new (logged and relayed) " << taken.load() << " times");
    BOOST_CHECK_MESSAGE(taken.load() == 0, "attack works: a proof pushed out of the pool is relayed again each time it comes back");
}

BOOST_AUTO_TEST_CASE(attack4_fresh_node_blames_honest_evidence_relay)
{
    // ATTACK (a single validator signs twice; or one that unbonded, for an old slot: at no cost). The
    // main and test networks start with no validators. A node that does not have the chain yet (a
    // new node, before its headers) has an inactive set at its tip, and none of the headers' parents:
    // a true proof, relayed by an honest peer, is "bad-pos-evidence-sig", the honest peer misbehaves
    // and is disconnected. (Only an unknown set or seed is excused, not an inactive set.)
    std::vector<unsigned char> message;
    {
        // A node with the chain: validators bond, blocks of stake, and the leader signs twice.
        NoValidatorsSetup node;
        COutPoint coin;
        CTxOut spent{node.Fund(COIN, coin)};
        const CKey& validator{node.keys[0]};
        node.Accept(node.Pay(coin, spent, {node.BondOut(validator, node.Pos().min_stake, node.Owner(node.key), &node.key, coin)}));
        node.MineBmm(node.BmmBlock());
        node.MineBmm(node.BmmBlock(/*use_mempool=*/false));
        const int64_t epoch{pos::EpochOf(pos::SlotOf(node.Tip()->nTime, node.Pos()), node.Pos()) + 2};
        const int64_t slot{epoch * node.Pos().slots_per_epoch};
        Mock(pos::SlotTime(slot, node.Pos()));
        std::string error;
        BOOST_REQUIRE_MESSAGE(node.producer->Produce(slot, error), error);
        CBlock rival{node.Tip()->GetBlockHeader()};
        rival.hashMerkleRoot = uint256{7};
        BOOST_REQUIRE(pos::Producer::Sign(rival, validator, node.Genesis()));
        const pos::DoubleProposal proof{.a = node.Tip()->GetBlockHeader(), .b = rival};
        std::string reason;
        BOOST_REQUIRE_MESSAGE(node.Hooks().AddEvidence(proof, reason), reason);
        DataStream s{};
        s << proof;
        message.assign(UCharCast(s.data()), UCharCast(s.data()) + s.size());
    }
    // A new node (same network, same time) gets it from an honest peer.
    NoValidatorsSetup fresh;
    DataStream recv{message};
    std::string misbehaving;
    BOOST_CHECK(fresh.Hooks().ProcessMessage(3, pos::MSG_EVIDENCE, recv, misbehaving));
    BOOST_TEST_MESSAGE("the new node says of the honest peer that relayed a true proof: '" << misbehaving << "'");
    BOOST_CHECK_MESSAGE(misbehaving.empty(), "attack works: an honest peer relaying a true proof is punished");
}

BOOST_FIXTURE_TEST_CASE(attack5_old_slot_evidence_walks_the_chain, FastBlocksSetup)
{
    // ATTACK (any inbound peer, no stake). A relayed proof is checked against the leader of its slot on
    // the chain below either header and at the tip (AddEvidence), under cs_main. CheckEvidenceForm asks
    // only that both headers sit at the start of one slot: a proof of slot 1 has LeaderOf walk SetFor
    // from each prev down to genesis (no anchor is before the epoch before epoch 0). Its signatures then
    // fail, which is not the peer's fault, so nothing is blamed, and each new connection has a fresh
    // burst of EVIDENCE_CHECKS_BURST. The cost grows with the chain: a year of 30-second slots is about
    // a million blocks. Here a chain of CHAIN headers (as an honest chain grows; added to the index
    // directly, from genesis) stands for it.
    StartPos();
    const CBlock base{PosBlock(1)};
    constexpr int CHAIN{200000};
    const CBlockIndex* long_tip{nullptr};
    {
        LOCK(::cs_main);
        CBlockIndex* best{nullptr};
        CBlockHeader header{Tip()->GetBlockHeader()};
        header.nVersion = 0x20000000; // a BMM header: no extension
        header.ext = {};
        // From genesis, a block a slot (anchors throughout, as a real chain has): the walk for a slot of
        // the last epochs stops at once, that for slot 1 goes down to genesis.
        uint256 prev{Genesis()};
        for (int i{0}; i < CHAIN; ++i) {
            header.hashPrevBlock = prev;
            header.nTime = static_cast<uint32_t>(pos::SlotTime(i + 1, Pos()));
            header.hashMerkleRoot = ArithToUint256(arith_uint256{static_cast<uint64_t>(i + 1)});
            CBlockIndex* index{Chainman().m_blockman.AddToBlockIndex(header, best)};
            prev = index->GetBlockHash();
            long_tip = index;
        }
    }
    const auto proof_at{[&](int64_t slot, const uint256& prev_a, const uint256& prev_b, uint64_t salt) {
        CBlock a{base}, b{base};
        a.nTime = b.nTime = static_cast<uint32_t>(pos::SlotTime(slot, Pos()));
        a.hashPrevBlock = prev_a;
        b.hashPrevBlock = prev_b;
        a.hashMerkleRoot = ArithToUint256(arith_uint256{2 * salt + 1});
        b.hashMerkleRoot = ArithToUint256(arith_uint256{2 * salt + 2});
        return pos::DoubleProposal{.a = a, .b = b};
    }};
    const auto send{[&](int64_t peer, const pos::DoubleProposal& proof) {
        DataStream stream{};
        stream << proof;
        std::string misbehaving;
        BOOST_CHECK(Hooks().ProcessMessage(peer, pos::MSG_EVIDENCE, stream, misbehaving));
        return misbehaving;
    }};
    const uint256 tip_a{long_tip->GetBlockHash()}, tip_b{long_tip->pprev->GetBlockHash()};
    const int64_t recent{pos::SlotOf(base.nTime, Pos())};
    constexpr int PROOFS{32};
    // Proofs of a recent slot, then of slot 1, from as many peers (each new: a fresh burst), same prevs.
    // The work counted (block index entries the walks for validator sets pass), not timed.
    const auto walks{[&](int64_t slot, int64_t first_peer) {
        const uint64_t before{Hooks().IndexWalks()};
        for (int i{0}; i < PROOFS; ++i) BOOST_CHECK_EQUAL(send(first_peer + i, proof_at(slot, tip_a, tip_b, first_peer + i)), "");
        return Hooks().IndexWalks() - before;
    }};
    const uint64_t recent_walks{walks(recent, 1000)};
    const uint64_t old_walks{walks(1, 2000)};
    BOOST_CHECK_EQUAL(Hooks().Evidence().Size(), 0U);
    BOOST_TEST_MESSAGE(PROOFS << " proofs of a recent slot: " << recent_walks << " block index entries walked; of slot 1, over a chain of " << CHAIN << " blocks: " << old_walks
                              << " (cs_main held, no peer blamed; a peer's burst is " << pos::EVIDENCE_CHECKS_BURST << ")");
    BOOST_CHECK_MESSAGE(old_walks < 10 * recent_walks + PROOFS * 100, "attack works: a proof of an old slot costs a walk of the whole chain under cs_main, unblamed, "
                                                                        << old_walks / PROOFS << " entries each against " << recent_walks / PROOFS);
}

BOOST_FIXTURE_TEST_CASE(attack5_mempool_staking_recheck_per_block, FastBlocksSetup)
{
    // ATTACK (anyone with coins, fees at the relay minimum). Every block connected (ConnectTip) runs
    // RemoveStaleDrivechainTxs under cs_main and the mempool's lock, which applies the staking rules
    // again to every bond and unbond in the mempool (ChainHooks::HasTxRules): an ECDSA key recovery
    // (the owner's signature) and a BIP340 check (the proof of possession) per bond, per block, whether
    // the block touched it or not. Bonds of a satoshi to a key of the attacker's own are valid (no
    // minimum for a bond), so the mempool can be filled with them at the relay fee; the work per block
    // is then the mempool's size, not the block's. Here N such bonds: the cost of one block's recheck,
    // and what a mempool of the default 300 MB of them would cost each block.
    StartPos();
    constexpr int N{2000};
    constexpr CAmount EACH{30000};
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    std::vector<CTxOut> outs(N, CTxOut{EACH, script});
    outs.emplace_back(spent.nValue - N * EACH - 200000, script);
    const CMutableTransaction split{Spend(coin, spent, outs)};
    Accept(split);
    Take(PosBlock(1));
    BOOST_REQUIRE_EQUAL(m_node.mempool->size(), 0U);
    CKey validator;
    validator.MakeNewKey(/*fCompressed=*/true);
    for (int i{0}; i < N; ++i) {
        const COutPoint in{split.GetHash(), static_cast<uint32_t>(i)};
        Accept(Spend(in, outs[i], {BondOut(validator, 1, Owner(key), &key, in), CTxOut{EACH - 1 - TX_FEE, script}}));
    }
    BOOST_REQUIRE_EQUAL(m_node.mempool->size(), static_cast<size_t>(N));
    const size_t usage{WITH_LOCK(m_node.mempool->cs, return m_node.mempool->DynamicMemoryUsage())};
    // The work counted (transactions the staking rules are applied to again, each a key recovery and a
    // BIP340 check), not timed: at each block, at most so many, whatever the mempool holds.
    uint64_t most{0};
    for (int run{0}; run < 3; ++run) {
        LOCK2(::cs_main, m_node.mempool->cs);
        const uint64_t before{Hooks().TxRuleChecks()};
        Chain().RemoveStaleDrivechainTxs();
        most = std::max(most, Hooks().TxRuleChecks() - before);
    }
    BOOST_REQUIRE_EQUAL(m_node.mempool->size(), static_cast<size_t>(N));
    // A mempool of the default size (-maxmempool 300 MB) of such bonds.
    const double full{300.0 * 1000 * 1000 / (static_cast<double>(usage) / N)};
    BOOST_TEST_MESSAGE("recheck of " << N << " bonds in the mempool at each block: " << most << " rechecked at most under cs_main; a full mempool holds "
                                     << static_cast<int64_t>(full) << " bonds");
    BOOST_CHECK_MESSAGE(most <= MAX_RULE_CHECKS_PER_TIP, "attack works: each block rechecks the signatures of every bond in the mempool under cs_main: " << most
                                                                                                                                                      << " a block, of " << N);
}

namespace {
/** A peer of the node's (PeerManager), whose messages the test sends. */
struct AttackPeer {
    CNode node;
    AttackPeer(NodeId id, ConnmanTestMsg& connman, ConnectionType type) EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex)
        : node{id, /*sock=*/nullptr, CAddress{CService{CNetAddr{in_addr{htonl(0x0a000001 + static_cast<uint32_t>(id))}}, 8333}, NODE_NETWORK},
               /*nKeyedNetGroupIn=*/0, /*nLocalHostNonceIn=*/0, CAddress{}, /*addrNameIn=*/"", type,
               /*inbound_onion=*/false, /*network_key=*/0}
    {
        connman.Handshake(node, /*successfully_connected=*/true, ServiceFlags(NODE_NETWORK | NODE_WITNESS), ServiceFlags(NODE_NETWORK | NODE_WITNESS),
                          PROTOCOL_VERSION, /*relay_txs=*/false);
    }
    void Send(ConnmanTestMsg& connman, CSerializedNetMsg&& msg) EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex)
    {
        connman.FlushSendBuffer(node);
        (void)connman.ReceiveMsgFrom(node, std::move(msg));
        node.fPauseSend = false;
        while (connman.ProcessMessagesOnce(node)) {}
        connman.FlushSendBuffer(node);
    }
};
} // namespace

BOOST_FIXTURE_TEST_CASE(attack5_compact_block_forged_extension_unpunished, FastBlocksSetup)
{
    // ATTACK (any inbound peer). A compact block whose header the node has, with another signature
    // (the block hash does not cover it), is checked against the leader (CheckOtherExtension: the
    // leader found, a BIP340 check) under cs_main before anything else is done with it, even for a
    // block of the active chain the node has whole, and the peer is not punished ("BIP 152 lets peers
    // relay compact blocks before they validate them"). So a peer has the node check a signature for
    // every 200 bytes it sends, for ever: a tip's header with random signatures. Against the same
    // compact block with the right signature (no check) as the measure.
    LOCK(NetEventsInterface::g_msgproc_mutex);
    StartPos();
    for (int i{1}; i <= 5; ++i) Take(PosBlock(i));
    ConnmanTestMsg& connman{static_cast<ConnmanTestMsg&>(*m_node.connman)};
    connman.SetPeerConnectTimeout(99999s);
    AttackPeer attacker{100, connman, ConnectionType::INBOUND};
    // (Any peer may announce compact blocks: version 2, low bandwidth.)
    attacker.Send(connman, NetMsg::Make(NetMsgType::SENDCMPCT, /*high_bandwidth=*/false, /*version=*/uint64_t{2}));
    CBlock tip_block;
    {
        LOCK(::cs_main);
        BOOST_REQUIRE(Chainman().m_blockman.ReadBlock(tip_block, *Chain().m_chain.Tip()));
    }
    constexpr int MESSAGES{3000};
    // The work counted (signatures of blocks of stake checked), not timed.
    const auto flood{[&](bool forged) EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex) {
        const uint64_t before{pos::BlockSigChecks()};
        for (int i{0}; i < MESSAGES; ++i) {
            CBlock copy{tip_block};
            if (forged) {
                copy.ext.sig[0] = static_cast<unsigned char>(i);
                copy.ext.sig[1] = static_cast<unsigned char>(i >> 8);
                copy.ext.sig[2] ^= 0x5a;
            }
            const CBlockHeaderAndShortTxIDs cmpct{copy, static_cast<uint64_t>(i)};
            attacker.Send(connman, NetMsg::Make(NetMsgType::CMPCTBLOCK, cmpct));
        }
        m_node.peerman->SendMessages(attacker.node);
        return pos::BlockSigChecks() - before;
    }};
    const uint64_t honest_sigs{flood(false)};
    const uint64_t forged_sigs{flood(true)};
    const bool disconnected{attacker.node.fDisconnect};
    BOOST_TEST_MESSAGE(MESSAGES << " compact blocks of the tip: " << honest_sigs << " signatures checked with its signature, " << forged_sigs << " with forged ones; peer disconnected: " << disconnected);
    BOOST_CHECK_MESSAGE(disconnected || forged_sigs <= honest_sigs, "attack works: a compact block of a known header with a forged signature costs a signature check under cs_main, unpunished: "
                                                                        << forged_sigs - honest_sigs << " signatures checked for " << MESSAGES << " messages");
    m_node.peerman->FinalizeNode(attacker.node);
}

BOOST_FIXTURE_TEST_CASE(attack5_ticket_cap_reset_by_reconnecting, FastBlocksSetup)
{
    // ATTACK (one inbound attacker that reconnects). Tickets nobody committed to are taken MAX_PEER_TICKETS
    // a peer per mainchain block, counted by peer id: a new connection is a new peer, with a new
    // count. Each one taken is new, so it is sent whole (no inv) to every other peer (RelayTicket), and
    // pushes the oldest out of the pool (1000). So one attacker that reconnects sends the node's peers
    // as many tickets as it likes, each of its messages multiplied by the number of peers, and pushes
    // honest tickets not committed yet out (a bidder's ticket for the next mainchain block).
    StartPos();
    Take(PosBlock(1));
    const auto ticket{[&](uint32_t i) {
        std::vector<unsigned char> hash(20, 0);
        hash[0] = static_cast<unsigned char>(i);
        hash[1] = static_cast<unsigned char>(i >> 8);
        hash[2] = 0xa5;
        return Ticket(Tip()->GetBlockHash(), GetScriptForDestination(WitnessV0KeyHash{uint160{hash}}));
    }};
    const auto send{[&](int64_t peer, const pos::AnchorTicket& t) {
        DataStream stream{};
        stream << t;
        std::string misbehaving;
        BOOST_CHECK(Hooks().ProcessMessage(peer, pos::MSG_TICKET, stream, misbehaving));
        BOOST_CHECK_EQUAL(misbehaving, "");
        return Hooks().GetTicket(t.Hash()).has_value();
    }};
    // An honest bidder's ticket for the next mainchain block, relayed by an honest peer.
    const pos::AnchorTicket honest{Ticket(Tip()->GetBlockHash(), script)};
    BOOST_REQUIRE(send(1, honest));
    // Within one mainchain block, the attacker connects 150 times (peer ids 1000 on), 8 tickets each.
    size_t taken{0};
    uint32_t n{0};
    for (int64_t connection{0}; connection < 150; ++connection) {
        for (size_t i{0}; i < pos::MAX_PEER_TICKETS; ++i) taken += send(1000 + connection, ticket(n++));
    }
    const bool honest_kept{Hooks().GetTicket(honest.Hash()).has_value()};
    BOOST_TEST_MESSAGE("tickets taken (and relayed to every peer) from one attacker in one mainchain block: " << taken << "; the honest ticket kept: " << honest_kept);
    BOOST_CHECK_MESSAGE(taken <= 4 * pos::MAX_PEER_TICKETS && honest_kept, "attack works: reconnecting resets the cap of tickets a peer per mainchain block: "
                                                                               << taken << " taken and relayed, honest ticket pushed out: " << !honest_kept);
}

BOOST_FIXTURE_TEST_CASE(attack6_leader_signs_headers_without_bound, FastBlocksSetup)
{
    // ATTACK (a single malicious validator, or one that left: its key costs it nothing once unbonded or
    // banned). The leader of a slot signs as many headers of that slot as it likes (another merkle root
    // each), on the block before its slot, deep in the chain if it led a slot there. Each is checked
    // (the node knows the leader), so no unchecked budget applies; the custom chain order skips the
    // anti-DoS work threshold; and a second header of a slot is noted as a proof but still taken. Each
    // header becomes a permanent entry of the block index (written to its database), on every node,
    // at the cost of one signature for the attacker.
    StartPos();
    for (int i{1}; i <= 10; ++i) Take(PosBlock(i));
    const CBlockIndex* victim{WITH_LOCK(::cs_main, return Tip()->GetAncestor(Tip()->nHeight - 7))};
    BOOST_REQUIRE(victim->m_ext);
    const CBlockHeader original{WITH_LOCK(::cs_main, return victim->GetBlockHeader())};
    const int64_t slot{pos::SlotOf(original.nTime, Pos())};
    const CKey& leader{Leader(slot)};
    constexpr int HEADERS{2000};
    const size_t index_before{WITH_LOCK(::cs_main, return Chainman().m_blockman.m_block_index.size())};
    size_t taken{0};
    const auto start{std::chrono::steady_clock::now()};
    for (int n{0}; n < HEADERS; ++n) {
        CBlock block{original};
        block.hashMerkleRoot = ArithToUint256(arith_uint256{static_cast<uint64_t>(n + 1)} << 100);
        BOOST_REQUIRE(pos::Producer::Sign(block, leader, Genesis()));
        taken += HeaderReason({static_cast<const CBlockHeader&>(block)}, /*source=*/4).empty();
    }
    const auto elapsed_ms{Ticks<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)};
    const size_t growth{WITH_LOCK(::cs_main, return Chainman().m_blockman.m_block_index.size()) - index_before};
    const size_t unchecked{WITH_LOCK(::cs_main, return Chainman().UncheckedHeld(4))};
    BOOST_TEST_MESSAGE(HEADERS << " headers one leader signed for slot " << slot << " (" << (Tip()->nHeight - victim->nHeight) << " blocks deep): " << taken
                               << " taken, block index +" << growth << " (" << unchecked << " of them unchecked, the rest permanent), " << elapsed_ms << " ms");
    BOOST_CHECK_MESSAGE(growth <= 16, "attack works: one validator key adds " << growth << " permanent block index entries for one slot, without bound");
}

BOOST_FIXTURE_TEST_CASE(slot_spare_header_taken_on_a_branch, FastBlocksSetup)
{
    // Two headers of a slot are in the block index for good (the first, and the proof that its leader
    // signed twice); a further one from a peer is spare: refused alone (header-spare, not punished),
    // taken unchecked with a header on top of it (the branch a block builds on, which the node may
    // need: an honest node may have seen other headers of the slot first), and gone with its peer.
    StartPos();
    for (int i{1}; i <= 5; ++i) Take(PosBlock(i));
    const CBlockHeader victim{WITH_LOCK(::cs_main, return Tip()->pprev->GetBlockHeader())};
    const int64_t slot{pos::SlotOf(victim.nTime, Pos())};
    BOOST_REQUIRE_EQUAL(slot, Slot(4));
    const auto sibling{[&](uint8_t n) {
        CBlock block{victim};
        block.hashMerkleRoot = uint256{n};
        BOOST_REQUIRE(pos::Producer::Sign(block, Leader(slot), Genesis()));
        return static_cast<CBlockHeader>(block);
    }};
    const size_t before{WITH_LOCK(::cs_main, return Chainman().m_blockman.m_block_index.size())};
    BOOST_CHECK_EQUAL(HeaderReason({sibling(1)}, /*source=*/4), "");
    BOOST_CHECK(Index(sibling(1).GetHash()) && !Index(sibling(1).GetHash())->m_unchecked);
    BOOST_CHECK_EQUAL(Hooks().Evidence().Size(), 1U);
    BlockValidationState state;
    const std::vector<CBlockHeader> alone{sibling(2)};
    BOOST_CHECK(!Chainman().ProcessNewBlockHeaders(alone, /*min_pow_checked=*/true, state, nullptr, /*source=*/4));
    BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_HEADER_LOW_WORK);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "header-spare");
    BOOST_CHECK(!Index(sibling(2).GetHash()));
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return Chainman().m_blockman.m_block_index.size()), before + 1);
    // On a branch: the next slot's leader signed a header on top of it.
    CBlock on{UnsignedHeader(victim, sibling(2).GetHash(), Slot(5), 0)};
    BOOST_REQUIRE(pos::Producer::Sign(on, Leader(Slot(5)), Genesis()));
    BOOST_CHECK_EQUAL(HeaderReason({sibling(2), on}, /*source=*/4), "");
    {
        LOCK(::cs_main);
        const CBlockIndex* spare{Chainman().m_blockman.LookupBlockIndex(sibling(2).GetHash())};
        BOOST_REQUIRE(spare);
        BOOST_CHECK(spare->m_unchecked && Chainman().HoldsUnchecked(*spare, 4));
        BOOST_CHECK(Chainman().m_blockman.LookupBlockIndex(on.GetHash())->m_unchecked);
        Chainman().UncheckedSourceGone(4);
        BOOST_CHECK(!Chainman().m_blockman.LookupBlockIndex(sibling(2).GetHash()));
        BOOST_CHECK(!Chainman().m_blockman.LookupBlockIndex(on.GetHash()));
        BOOST_CHECK_EQUAL(Chainman().m_blockman.m_block_index.size(), before + 1);
        Chainman().UncheckedSourceAdded(4);
    }
}


namespace {
/** A `headers` message of these headers (each a block without transactions: an extended one, no sections). */
CSerializedNetMsg HeadersMessage(const std::vector<CBlockHeader>& headers)
{
    DataStream s{};
    WriteCompactSize(s, headers.size());
    for (const CBlockHeader& header : headers) {
        s << header;
        WriteCompactSize(s, 0);
        if (header.IsExtended()) WriteCompactSize(s, 0);
    }
    CSerializedNetMsg msg;
    msg.m_type = NetMsgType::HEADERS;
    msg.data.assign(UCharCast(s.data()), UCharCast(s.data()) + s.size());
    return msg;
}
} // namespace

BOOST_FIXTURE_TEST_CASE(slot_junk_headers_then_the_announced_block, FastBlocksSetup)
{
    // Review of 960f12f34e (audit 6, item 14). The leader of a slot signs two junk headers of it (or
    // anyone relays them): the two places of its slot are taken. The block the network builds on, on
    // another branch (a sibling), is announced alone, as tips are: from an outbound peer it is taken
    // (unchecked, the peer's) and its block fetched at once, not once a block on top of it exists.
    LOCK(NetEventsInterface::g_msgproc_mutex);
    StartPos();
    for (int i{1}; i <= 3; ++i) Take(PosBlock(i));
    const CBlock honest{PosBlock(4)};
    const int64_t slot{pos::SlotOf(honest.nTime, Pos())};
    const CKey& leader{Leader(slot)};
    const auto junk{[&](uint8_t n) {
        CBlock block{honest};
        block.hashMerkleRoot = uint256{n};
        BOOST_REQUIRE(pos::Producer::Sign(block, leader, Genesis()));
        return static_cast<CBlockHeader>(block);
    }};
    BOOST_REQUIRE_EQUAL(HeaderReason({junk(1)}, /*source=*/5), "");
    BOOST_REQUIRE_EQUAL(HeaderReason({junk(2)}, /*source=*/5), "");
    BOOST_CHECK_EQUAL(Hooks().Evidence().Size(), 1U);
    // A third junk header alone, from an inbound peer: spare (no better than the best header, junk 1).
    WITH_LOCK(::cs_main, Chainman().UncheckedSourceAdded(40, /*inbound=*/true));
    BOOST_CHECK_EQUAL(HeaderReason({junk(3)}, /*source=*/40), "header-spare");

    ConnmanTestMsg& connman{static_cast<ConnmanTestMsg&>(*m_node.connman)};
    connman.SetPeerConnectTimeout(99999s);
    AttackPeer peer{101, connman, ConnectionType::OUTBOUND_FULL_RELAY};
    peer.Send(connman, HeadersMessage({honest}));
    m_node.peerman->SendMessages(peer.node);
    const CBlockIndex* index{Index(honest.GetHash())};
    BOOST_CHECK_MESSAGE(index, "the honest block's header, announced alone after two junk headers of its slot, is refused");
    CNodeStateStats stats;
    BOOST_REQUIRE(m_node.peerman->GetNodeStateStats(peer.node.GetId(), stats));
    BOOST_CHECK_MESSAGE(std::ranges::find(stats.vHeightInFlight, Tip()->nHeight + 1) != stats.vHeightInFlight.end(), "its block is not asked for");
    // The block comes: the tip.
    peer.Send(connman, NetMsg::Make(NetMsgType::BLOCK, TX_WITH_WITNESS(honest)));
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    BOOST_CHECK(Tip()->GetBlockHash() == honest.GetHash());
    BOOST_CHECK(!WITH_LOCK(::cs_main, return Assert(Index(honest.GetHash()))->m_unchecked));
    m_node.peerman->FinalizeNode(peer.node);
    WITH_LOCK(::cs_main, Chainman().UncheckedSourceGone(40));
}

BOOST_FIXTURE_TEST_CASE(slot_headers_counted_by_signer, SeedSetup)
{
    // Review of 960f12f34e (audit 6, item 14). Two headers of a slot are kept for good per signer: once
    // its leader signed two (the proof), a further one of that leader is spare, but a header of another
    // leader of the slot (the record moved the epoch's seed, say) is taken, checked, for good.
    StartPos();
    const int64_t epoch{pos::EpochOf(base_slot, Pos()) + 1};
    const int i{static_cast<int>(epoch * Pos().slots_per_epoch - base_slot)};
    Slot(i);
    Mock(GetTime() + 1);
    Main();
    Take(PosBlock(i));
    const CBlock model{PosBlock(i + 1)};
    const int64_t slot{pos::SlotOf(model.nTime, Pos())};
    const uint256 first_leader{XOnly(Leader(slot))};
    const auto header{[&](uint8_t n, const CKey& signer) {
        CBlock block{model};
        block.hashMerkleRoot = uint256{n};
        BOOST_REQUIRE(pos::Producer::Sign(block, signer, Genesis()));
        return static_cast<CBlockHeader>(block);
    }};
    WITH_LOCK(::cs_main, Chainman().UncheckedSourceAdded(40, /*inbound=*/true));
    BOOST_REQUIRE_EQUAL(HeaderReason({header(1, Leader(slot))}, /*source=*/40), "");
    BOOST_REQUIRE_EQUAL(HeaderReason({header(2, Leader(slot))}, /*source=*/40), "");
    BOOST_CHECK_EQUAL(HeaderReason({header(3, Leader(slot))}, /*source=*/40), "header-spare");
    // The record moves the seed of the slot's epoch, until the slot has another leader.
    const auto seed_height{pos::SeedHeight(epoch, Pos(), Record())};
    BOOST_REQUIRE(seed_height && *seed_height > 0);
    const auto replaced{Record().Truncate(*seed_height - 1)};
    bool moved{false};
    for (uint64_t k{1}; k <= 64 && !moved; ++k) {
        Record().Truncate(*seed_height - 1);
        for (sidechain::MainBlock main : replaced) {
            main.prev_hash = Record().TipHash();
            main.hash = ArithToUint256(UintToArith256(main.hash) + k);
            BOOST_REQUIRE(Record().Append(main));
        }
        moved = XOnly(Leader(slot)) != first_leader;
    }
    BOOST_REQUIRE(moved);
    // The new leader's header of the slot, alone, from the inbound peer: taken for good.
    const CBlockHeader other{header(4, Leader(slot))};
    BOOST_CHECK_EQUAL(HeaderReason({other}, /*source=*/40), "");
    BOOST_CHECK(Index(other.GetHash()) && !WITH_LOCK(::cs_main, return Index(other.GetHash())->m_unchecked));
    WITH_LOCK(::cs_main, Chainman().UncheckedSourceGone(40));
}

BOOST_FIXTURE_TEST_CASE(evidence_built_as_validated_across_pruning, FastBlocksSetup)
{
    // Review of audit 6 (the check of evidence once a peg step prunes validator sets). A block of
    // stake with an acknowledgement takes a peg step before its proofs are checked (AfterBlock): the
    // anchors count one more, and the step may prune the set of an old anchor. The producer chooses
    // its proofs before the step (ChainEvidence on the tip's state, with that one more anchor). At
    // every anchor, up to and past the end of a proof's window, both agree: a proof the producer
    // would carry makes a valid block; one it leaves out, an invalid one (bad-pos-evidence-old).
    StartPos();
    // A slot of an epoch whose set the first anchor made (not the genesis set: never pruned).
    int i{static_cast<int>((pos::EpochOf(base_slot, Pos()) + 2) * Pos().slots_per_epoch - base_slot)};
    Take(PosBlock(i - 1));
    const auto [a, b]{TwoBlocks(i)};
    const pos::Offence offence{.pubkey = XOnly(Leader(pos::SlotOf(a.nTime, Pos()))), .slot = pos::SlotOf(a.nTime, Pos())};
    Take(a);
    const pos::DoubleProposal proof{.a = a, .b = b};
    std::string why;
    BOOST_REQUIRE_MESSAGE(Hooks().AddEvidence(proof, why), why);
    const auto set_of_proof{[&] {
        LOCK(::cs_main);
        return Hooks().LeaderOf(*Tip(), offence.slot, &Chain().SideCache(), Hooks().EvidenceWithin());
    }};
    BOOST_REQUIRE(set_of_proof().status == pos::FastBlocksHooks::LeaderStatus::OK);
    BOOST_REQUIRE_GT(set_of_proof().set_anchors, 0U);
    bool left_out{false};
    size_t carried{0};
    // (On, past the window, until a peg step prunes the proof's set.)
    for (int anchor{0}; anchor < 8 * Pos().evidence_window_anchors && set_of_proof().status != pos::FastBlocksHooks::LeaderStatus::SET_UNKNOWN; ++anchor) {
        i += 3;
        const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
        const int64_t slot{Slot(i)};
        bool chosen;
        {
            LOCK(::cs_main);
            const sidechain::StoreView& view{Chain().SideCache()};
            const auto evidence{Hooks().ChainEvidence(*Tip(), slot, view, pos::GetFastState(view).anchors + 1, pos::MAX_SECTION_EVIDENCE)};
            // (The pool has others: each block checked here and the one taken are two of a slot.)
            chosen = std::any_of(evidence.begin(), evidence.end(), [&](const auto& e) { return e.first == offence; });
        }
        const std::string reason{Check(WithEvidence(i, {proof}, ack))};
        BOOST_CHECK_MESSAGE(chosen == reason.empty(), "at anchor " << anchor << ": the producer would " << (chosen ? "" : "not ") << "carry the proof, and the block is " << (reason.empty() ? "valid" : reason));
        if (!chosen) {
            BOOST_CHECK_EQUAL(reason, "bad-pos-evidence-old");
            left_out = true;
        } else {
            BOOST_CHECK(!left_out);
            ++carried;
        }
        // The anchor, without the proof (its offender stays unpunished, the window runs on).
        Take(PosBlock(i, ack));
    }
    BOOST_TEST_MESSAGE("the proof could be carried at " << carried << " anchors, then was out of its window");
    BOOST_CHECK(left_out);
    BOOST_CHECK_GT(carried, 0U);
    // Its set pruned by a peg step at last.
    BOOST_CHECK(set_of_proof().status == pos::FastBlocksHooks::LeaderStatus::SET_UNKNOWN);
}

BOOST_FIXTURE_TEST_CASE(evidence_true_proof_among_a_flood, FastBlocksSetup)
{
    // Review of 81aeb47467 (audit 6, item 11). The pool is full of costly but useless proofs (1000,
    // of the right form, signed by validators that do not lead their slots here: their signatures fail
    // against the leader), and one true proof; between blocks a flood of checks (relayed proofs, from
    // many peers) pushes the results the node remembers out. The true one is carried, and its offender
    // slashed, within EVIDENCE_CHECK_ROUNDS blocks; and every useless one is checked within as many.
    StartPos();
    const CBlock base{PosBlock(1)};
    const auto proof_at{[&](int64_t slot, const CKey& signer, uint64_t salt) {
        CBlock a{base};
        a.nTime = static_cast<uint32_t>(pos::SlotTime(slot, Pos()));
        a.hashMerkleRoot = ArithToUint256(arith_uint256{2 * salt + 1});
        BOOST_REQUIRE(pos::Producer::Sign(a, signer, Genesis()));
        CBlock b{a};
        b.hashMerkleRoot = ArithToUint256(arith_uint256{2 * salt + 2});
        BOOST_REQUIRE(pos::Producer::Sign(b, signer, Genesis()));
        return pos::DoubleProposal{.a = a, .b = b};
    }};
    //! Each useless proof, its slot, and its signer.
    std::vector<std::tuple<pos::DoubleProposal, int64_t, uint256>> junk;
    int64_t slot{base_slot + 2};
    for (; junk.size() < pos::MAX_EVIDENCE; ++slot) {
        const auto leader{Hooks().ActiveLeaderOf(slot)};
        BOOST_REQUIRE(leader.status == pos::FastBlocksHooks::LeaderStatus::OK);
        for (const CKey& k : keys) {
            if (XOnly(k) == leader.pubkey || junk.size() >= pos::MAX_EVIDENCE) continue;
            const pos::DoubleProposal proof{proof_at(slot, k, junk.size())};
            BOOST_REQUIRE(Hooks().Evidence().Add({.pubkey = XOnly(k), .slot = slot}, proof));
            junk.emplace_back(proof, slot, XOnly(k));
        }
    }
    // The true proof: the leader of a slot after them signed twice (the oldest junk one goes).
    const int64_t offence_slot{slot};
    const CKey& offender_key{Leader(offence_slot)};
    const uint256 offender{XOnly(offender_key)};
    std::string reason;
    BOOST_REQUIRE_MESSAGE(Hooks().AddEvidence(proof_at(offence_slot, offender_key, 1u << 20), reason), reason);
    BOOST_REQUIRE_EQUAL(Hooks().Evidence().Size(), pos::MAX_EVIDENCE);

    uint64_t flood_key{0};
    const auto flood{[&] {
        // As many results as the node remembers, new ones (relayed proofs checked against leaders).
        for (size_t n{0}; n < pos::MAX_CHECKED_SIGNATURES; ++n) Hooks().Evidence().CheckSigs(std::get<0>(junk[n % junk.size()]), ArithToUint256(arith_uint256{++flood_key}), Genesis());
    }};
    std::set<size_t> checked;
    const auto note_checked{[&] {
        for (size_t n{0}; n < junk.size(); ++n) {
            const auto leader{Hooks().ActiveLeaderOf(std::get<1>(junk[n]))};
            if (leader.status == pos::FastBlocksHooks::LeaderStatus::OK && Hooks().Evidence().SigsKnown(std::get<0>(junk[n]), leader.pubkey)) checked.insert(n);
        }
    }};
    std::optional<size_t> slashed_at;
    for (size_t round{1}; round <= pos::EVIDENCE_CHECK_ROUNDS && !slashed_at; ++round) {
        flood();
        // A merged mined block at a slot after them all.
        Mock(pos::SlotTime(offence_slot + static_cast<int64_t>(round), Pos()));
        MineBmm(BmmBlock(/*use_mempool=*/false));
        note_checked();
        if (Validator(offender) && Validator(offender)->tombstoned) slashed_at = round;
    }
    BOOST_TEST_MESSAGE("the offender of the true proof among " << pos::MAX_EVIDENCE << " useless ones, under a flood of checks: slashed at block " << (slashed_at ? std::to_string(*slashed_at) : "never"));
    BOOST_CHECK_MESSAGE(slashed_at, "evidence censored: the true proof is not carried within " << pos::EVIDENCE_CHECK_ROUNDS << " blocks");
    // Every useless one is checked within EVIDENCE_CHECK_ROUNDS blocks the node makes, whatever the flood
    // (but those against the offender, banned now: a cheap check leaves them out).
    checked.clear();
    for (size_t round{1}; round <= pos::EVIDENCE_CHECK_ROUNDS; ++round) {
        flood();
        LOCK(::cs_main);
        BOOST_CHECK(Hooks().MergeMinedOutputs(Chain().SideCache(), *Tip(), GetTime()).empty());
        note_checked();
    }
    // (The oldest one left the pool when the true one came.)
    const auto pooled{Hooks().Evidence().All()};
    const size_t expected{static_cast<size_t>(std::count_if(pooled.begin(), pooled.end(), [&](const auto& p) { return p.first.pubkey != offender; }))};
    BOOST_CHECK_EQUAL(pooled.size(), junk.size());
    BOOST_CHECK_EQUAL(checked.size(), expected);
}

namespace {
/** Peers in the node's connman (not its PeerManager), for what reads their address and network group. */
struct ConnmanPeers {
    ConnmanTestMsg& connman;
    explicit ConnmanPeers(ConnmanTestMsg& c) : connman{c} {}
    ~ConnmanPeers() { connman.ClearTestNodes(); }
    NodeId Add(NodeId id, uint32_t ipv4, uint64_t netgroup, bool inbound_onion = false)
    {
        CNode* node{new CNode{id, /*sock=*/nullptr, CAddress{CService{CNetAddr{in_addr{htonl(ipv4)}}, 8333}, NODE_NETWORK}, netgroup, /*nLocalHostNonceIn=*/0, CAddress{},
                              /*addrNameIn=*/"", ConnectionType::INBOUND, inbound_onion, /*network_key=*/0}};
        node->fSuccessfullyConnected = true;
        connman.AddTestNode(*node);
        return id;
    }
};

/** A ticket for `anchored` paying a key of its own (`n` makes it another). */
pos::AnchorTicket NumberedTicket(const uint256& anchored, uint32_t n)
{
    std::vector<unsigned char> hash(20, 0);
    hash[0] = static_cast<unsigned char>(n);
    hash[1] = static_cast<unsigned char>(n >> 8);
    hash[2] = 0x5c;
    pos::AnchorTicket ticket;
    ticket.anchored = anchored;
    ticket.payee = GetScriptForDestination(WitnessV0KeyHash{uint160{hash}});
    return ticket;
}
} // namespace

BOOST_FIXTURE_TEST_CASE(tickets_reconnect_flood_over_tor_bounded, FastBlocksSetup)
{
    // Review of 803bb68cc0 (audit 6, item 13). Peers on addresses that are not routable (Tor's inbound
    // connections are local; private ones too) share one budget of tickets nobody committed to, per
    // mainchain block: an attacker that reconnects over Tor 150 times in a mainchain block has
    // MAX_NONROUTABLE_TICKETS taken (and relayed), not a fresh MAX_PEER_TICKETS each time. A ticket the
    // node has costs nothing (honest peers relay each new one to one another).
    StartPos();
    Take(PosBlock(1));
    Hooks().StartServices(m_node);
    {
        ConnmanPeers peers{static_cast<ConnmanTestMsg&>(*m_node.connman)};
        const auto send{[&](int64_t peer, const pos::AnchorTicket& t) {
            DataStream stream{};
            stream << t;
            std::string misbehaving;
            BOOST_CHECK(Hooks().ProcessMessage(peer, pos::MSG_TICKET, stream, misbehaving));
            BOOST_CHECK_EQUAL(misbehaving, "");
            return Hooks().GetTicket(t.Hash()).has_value();
        }};
        size_t taken{0};
        uint32_t n{0};
        std::vector<pos::AnchorTicket> sent;
        for (int64_t connection{0}; connection < 150; ++connection) {
            // Over Tor (local, inbound onion), or from a private address: a new id each time.
            const NodeId id{peers.Add(1000 + connection, connection % 2 ? 0x7f000001 : 0x0a000001 + static_cast<uint32_t>(connection), /*netgroup=*/connection, /*inbound_onion=*/connection % 2 == 1)};
            for (size_t i{0}; i < pos::MAX_PEER_TICKETS; ++i) {
                sent.push_back(NumberedTicket(Tip()->GetBlockHash(), n++));
                taken += send(id, sent.back());
            }
        }
        BOOST_TEST_MESSAGE("tickets taken (and relayed to every peer) from 150 connections over Tor or local addresses in one mainchain block: " << taken);
        BOOST_CHECK_MESSAGE(taken <= pos::MAX_NONROUTABLE_TICKETS, "attack works: reconnecting over Tor gives a new budget of tickets each time: " << taken << " taken");
        // An honest local peer relays the ones the node has: nothing counted, nothing new.
        const NodeId honest{peers.Add(5000, 0x7f000001, 0)};
        for (size_t i{0}; i < pos::MAX_NONROUTABLE_TICKETS; ++i) BOOST_CHECK(send(honest, sent[i]));
        // A new mainchain block: the budget again, shared.
        Main();
        size_t again{0};
        for (size_t i{0}; i < 2 * pos::MAX_NONROUTABLE_TICKETS; ++i) again += send(i % 2 ? honest : 1000, NumberedTicket(Tip()->GetBlockHash(), n++));
        BOOST_CHECK_EQUAL(again, pos::MAX_NONROUTABLE_TICKETS);
    }
    Hooks().StopServices(m_node);
}

BOOST_FIXTURE_TEST_CASE(tickets_honest_relay_under_a_flood, FastBlocksSetup)
{
    // Review of 4e7951b31a and 803bb68cc0 (audit 6, items 12 and 13). In one mainchain block, a flood of
    // tickets nobody committed to: 150 reconnections over Tor, and 32 network groups 8 each. Then an
    // honest bidder's ticket, relayed by a peer of another group: taken and relayed (the cap in all is
    // first come, but past it a group's first ticket is its share), and acknowledged by the first block
    // of stake after the mainchain commits to it: no wait for a fetch of the ticket (AskMissingTickets).
    StartPos();
    Take(PosBlock(1));
    Hooks().StartServices(m_node);
    std::optional<int> latency;
    {
        ConnmanPeers peers{static_cast<ConnmanTestMsg&>(*m_node.connman)};
        const auto send{[&](int64_t peer, const pos::AnchorTicket& t) {
            DataStream stream{};
            stream << t;
            std::string misbehaving;
            BOOST_CHECK(Hooks().ProcessMessage(peer, pos::MSG_TICKET, stream, misbehaving));
            return Hooks().GetTicket(t.Hash()).has_value();
        }};
        size_t taken{0};
        uint32_t n{0};
        for (int64_t connection{0}; connection < 150; ++connection) {
            const NodeId id{peers.Add(1000 + connection, 0x7f000001, /*netgroup=*/0, /*inbound_onion=*/true)};
            for (size_t i{0}; i < pos::MAX_PEER_TICKETS; ++i) taken += send(id, NumberedTicket(Tip()->GetBlockHash(), n++));
        }
        for (uint32_t group{0}; group < 32; ++group) {
            // 1.<group>.0.1: a routable address, a network group of its own.
            const NodeId id{peers.Add(2000 + group, 0x01000001 + (group << 16), /*netgroup=*/100 + group)};
            for (size_t i{0}; i < pos::MAX_PEER_TICKETS; ++i) taken += send(id, NumberedTicket(Tip()->GetBlockHash(), n++));
        }
        const NodeId relay{peers.Add(3000, 0x02020001, /*netgroup=*/999)};
        const pos::AnchorTicket honest{Ticket(Tip()->GetBlockHash(), script)};
        const bool relayed{send(relay, honest)};
        BOOST_TEST_MESSAGE("tickets of the flood taken: " << taken << "; the honest one: " << relayed);
        BOOST_CHECK(taken <= pos::MAX_UNCOMMITTED_TICKETS_SHARED);
        BOOST_CHECK_MESSAGE(relayed, "an honest ticket relayed under a flood is dropped");
        // The mainchain commits to it: the slots until a block of stake acknowledges it.
        Commit(honest.Hash());
        for (int i{2}; i <= 12 && !latency; ++i) {
            std::string error;
            BOOST_REQUIRE_MESSAGE(producer->Produce(Slot(i), error), error);
            CBlock made;
            BOOST_REQUIRE(WITH_LOCK(::cs_main, return Chainman().m_blockman.ReadBlock(made, *Tip())));
            pos::PosSections sections;
            BOOST_REQUIRE(pos::PosSections::Decode(made.vSections, sections));
            if (sections.ack && sections.ack->ticket.Hash() == honest.Hash()) latency = i - 1;
        }
    }
    Hooks().StopServices(m_node);
    BOOST_TEST_MESSAGE("acknowledgement of the honest ticket, slots after its commitment: " << (latency ? std::to_string(*latency) : "none in 11"));
    BOOST_CHECK(latency == 1);
}

BOOST_FIXTURE_TEST_CASE(attack7_spare_header_replayed_costs_signatures, FastBlocksSetup)
{
    // ATTACK (review 7; audit 6 review, item 15). A validator that signed two headers of a slot (slashed,
    // or not caring) signs a third. Any peer can then send that third header again and again: it is
    // spare (refused alone, header-spare, not the peer's fault), so it never enters the block index and
    // each copy is checked afresh: HeaderSpare is asked before the signature is, CheckHeader checks the
    // signature, and NoteSigned makes a proof of it with the first header, whose two signatures
    // CheckEvidence checks again (the pool knows the offence already). Nothing is cached, nothing is
    // rate limited, nobody is punished: Schnorr checks under cs_main for one small message each.
    StartPos();
    for (int i{1}; i <= 5; ++i) Take(PosBlock(i));
    const CBlockHeader victim{WITH_LOCK(::cs_main, return Tip()->pprev->GetBlockHeader())};
    const int64_t slot{pos::SlotOf(victim.nTime, Pos())};
    const auto sibling{[&](uint8_t n) {
        CBlock block{victim};
        block.hashMerkleRoot = uint256{n};
        BOOST_REQUIRE(pos::Producer::Sign(block, Leader(slot), Genesis()));
        return static_cast<CBlockHeader>(block);
    }};
    BOOST_REQUIRE_EQUAL(HeaderReason({sibling(1)}, /*source=*/4), "");
    const CBlockHeader third{sibling(2)};
    WITH_LOCK(::cs_main, Chainman().UncheckedSourceAdded(40, /*inbound=*/true));
    constexpr int COPIES{500};
    const uint64_t sigs_before{pos::BlockSigChecks()};
    const uint64_t walks_before{Hooks().IndexWalks()};
    int spare{0};
    for (int n{0}; n < COPIES; ++n) spare += HeaderReason({third}, /*source=*/40) == "header-spare";
    const uint64_t sigs{pos::BlockSigChecks() - sigs_before};
    const uint64_t walks{Hooks().IndexWalks() - walks_before};
    WITH_LOCK(::cs_main, Chainman().UncheckedSourceGone(40));
    BOOST_CHECK_EQUAL(spare, COPIES);
    BOOST_CHECK(!Index(third.GetHash()));
    BOOST_TEST_MESSAGE(COPIES << " copies of one spare header from one inbound peer: " << sigs << " signature checks, " << walks
                              << " block index entries walked, none punished");
    BOOST_CHECK_MESSAGE(sigs <= static_cast<uint64_t>(pos::EVIDENCE_CHECKS_BURST),
                        "attack works: a spare header sent again costs " << sigs / COPIES << " signature checks a copy, without bound or blame");
}

BOOST_FIXTURE_TEST_CASE(attack7_early_slot_header_walks_the_chain, FastBlocksSetup)
{
    // ATTACK (review 7; any inbound peer, no stake). AcceptBlockHeader asks HeaderCheckable (and
    // HeaderSpare) before ContextualCheckBlockHeader's time-too-old: HeaderCheckable calls LeaderOf with
    // no bound (`within`), so a header of slot 1 on the tip walks SetFor down to genesis (no anchor is
    // before the epoch before epoch 0), under cs_main, before the median-time check turns it away. The
    // peer is disconnected for it, but each new connection costs a walk of the whole chain: the fix of
    // attack5_old_slot_evidence_walks_the_chain (EvidenceWithin) was made for proofs, not for headers.
    StartPos();
    const CBlock base{PosBlock(1)};
    constexpr int CHAIN{200000};
    const CBlockIndex* long_tip{nullptr};
    {
        LOCK(::cs_main);
        CBlockIndex* best{nullptr};
        CBlockHeader header{Tip()->GetBlockHeader()};
        header.nVersion = 0x20000000; // a BMM header: no extension
        header.ext = {};
        uint256 prev{Genesis()};
        for (int i{0}; i < CHAIN; ++i) {
            header.hashPrevBlock = prev;
            header.nTime = static_cast<uint32_t>(pos::SlotTime(i + 1, Pos()));
            header.hashMerkleRoot = ArithToUint256(arith_uint256{static_cast<uint64_t>(i + 1)});
            CBlockIndex* index{Chainman().m_blockman.AddToBlockIndex(header, best)};
            prev = index->GetBlockHash();
            long_tip = index;
        }
    }
    const uint256 tip{long_tip->GetBlockHash()};
    constexpr int HEADERS{16};
    const auto walks{[&](int64_t slot, std::string& reason) {
        const uint64_t before{Hooks().IndexWalks()};
        // (The next slot's: taken unchecked, the set of its epoch not known on that branch; slot 1's: time-too-old.)
        for (int i{0}; i < HEADERS; ++i) reason = HeaderReason({UnsignedHeader(base, tip, slot, 0, static_cast<uint8_t>(i + 1))}, /*source=*/i % 16);
        return Hooks().IndexWalks() - before;
    }};
    std::string recent_reason, early_reason;
    const uint64_t recent{walks(CHAIN + 1, recent_reason)};
    const uint64_t early{walks(1, early_reason)};
    BOOST_TEST_MESSAGE(HEADERS << " headers on a chain of " << CHAIN << ": of the next slot " << recent << " entries walked (" << recent_reason << "), of slot 1 " << early
                               << " (" << early_reason << ", one a connection)");
    BOOST_CHECK_EQUAL(early_reason, "time-too-old");
    BOOST_CHECK_MESSAGE(early < 10 * recent + HEADERS * 100, "attack works: a header of an old slot costs a walk of the whole chain under cs_main before its time is checked, "
                                                               << early / HEADERS << " entries each against " << recent / HEADERS);
}

BOOST_FIXTURE_TEST_CASE(attack7_seed_moved_beyond_the_kept_epochs, SeedSetup)
{
    // ATTACK (review 7; audit 6 review, item 16: a mainchain reorg some epochs deep). EPOCH_SEEDS keeps
    // the seeds of the last three epochs only (SEED_EPOCHS_KEPT). A mainchain reorg that moves the seed of
    // an older epoch is not seen by a running node: CheckSeeds compares only what the table has, so the
    // state of that epoch (the leaders charged for the slots its blocks skipped) stays what the old seed
    // gave, while a node that takes the chain in afresh connects those blocks under the record as it is
    // now: the two states differ (consensus split; later sets, jailing).
    StartPos();
    const int64_t spe{Pos().slots_per_epoch};
    const int64_t e1{pos::EpochOf(base_slot, Pos()) + 1};
    const int64_t e2{e1 + 1};
    const int64_t e5{e2 + 3};
    const auto at{[&](int64_t slot) { return static_cast<int>(slot - base_slot); }};
    Slot(at(e1 * spe) + 1);
    Main();
    Take(PosBlock(at(e1 * spe) + 2));
    Slot(at(e2 * spe) + 1);
    Main();
    Take(PosBlock(at(e2 * spe) + 7));
    CBlockIndex* first{WITH_LOCK(::cs_main, return Chain().m_chain.Tip())};
    for (int64_t e{e2 + 1}; e <= e5; ++e) {
        Slot(at(e * spe) + 1);
        Main();
    }
    Take(PosBlock(at(e5 * spe) + 2));
    const CBlockIndex* last{Tip()};
    using Snapshot = std::map<uint256, pos::Liveness>;
    const auto liveness{[&]() {
        Snapshot snapshot;
        LOCK(::cs_main);
        for (const CKey& k : keys) {
            if (const auto l{pos::GetLiveness(Chain().SideState().View(), e2, XOnly(k))}) snapshot[XOnly(k)] = *l;
        }
        return snapshot;
    }};
    const Snapshot original{liveness()};
    Hooks().CheckSeeds();
    BOOST_REQUIRE(Tip() == last);
    const auto leaders{[&]() {
        std::vector<uint256> keys_of;
        LOCK(::cs_main);
        for (int64_t slot{e2 * spe}; slot <= e2 * spe + 7; ++slot) keys_of.push_back(Hooks().LeaderOf(*first->pprev, slot).pubkey);
        keys_of.push_back(Hooks().LeaderOf(*last->pprev, pos::SlotOf(last->nTime, Pos())).pubkey);
        return keys_of;
    }};
    const std::vector<uint256> before{leaders()};
    const auto seed_height{pos::SeedHeight(e2, Pos(), Record())};
    BOOST_REQUIRE(seed_height && *seed_height > 0);
    // The mainchain reorg: from the block that seeds e2 on, other blocks (the same commitments). One with
    // which the block of e2 keeps its leader, and a slot of e2 it skipped has another. (The block of e5
    // may lose its leader: both nodes then turn it away alike.)
    std::vector<sidechain::MainBlock> replaced;
    bool found{false};
    for (int salt{1}; salt < 300 && !found; ++salt) {
        replaced = Record().Truncate(*seed_height - 1);
        for (sidechain::MainBlock main : replaced) {
            main.prev_hash = Record().TipHash();
            main.hash = ArithToUint256(UintToArith256(main.hash) + salt);
            BOOST_REQUIRE(Record().Append(main));
        }
        const std::vector<uint256> after{leaders()};
        found = after[7] == before[7] && !std::equal(after.begin(), after.begin() + 7, before.begin());
        if (!found) {
            Record().Truncate(*seed_height - 1);
            for (const sidechain::MainBlock& main : replaced) BOOST_REQUIRE(Record().Append(main));
        }
    }
    BOOST_REQUIRE(found);
    // The running node sees the reorg.
    Hooks().CheckSeeds();
    const Snapshot running{liveness()};
    const CBlockIndex* running_tip{Tip()};
    // As a node that takes the chain in afresh connects it, under the record as it is now.
    {
        BlockValidationState state;
        BOOST_REQUIRE(Chain().InvalidateBlock(state, first, /*by_record=*/true));
        WITH_LOCK(::cs_main, Chain().ReconsiderRecordFailure(first));
        BOOST_REQUIRE(Chain().ActivateBestChain(state));
    }
    const Snapshot fresh{liveness()};
    BOOST_CHECK(Tip() == running_tip);
    BOOST_TEST_MESSAGE("liveness of epoch " << e2 << ": running node " << (running == original ? "as before the reorg" : "changed") << ", fresh node "
                                             << (fresh == original ? "as before the reorg" : "changed"));
    BOOST_CHECK_MESSAGE(running == fresh, "attack works: a mainchain reorg moving the seed of an epoch older than the three kept splits a running node from a fresh one");
}

BOOST_FIXTURE_TEST_CASE(seed_moved_then_a_block_before_the_check, SeedSetup)
{
    // As attack7_seed_moved_beyond_the_kept_epochs, but a block of stake comes after the mainchain
    // reorg and before the follower's check (CheckSeeds). Taken, it would note the new seed of its
    // epoch over the old one, and the check, which stops at the first epoch whose seed has not moved,
    // would not see the older one that did: so it waits (pos-seed-moved, a failure against the record,
    // not the peer's fault) until the check has connected the blocks from the moved epoch again, and is
    // connected then. The state is that of a node that takes the chain in afresh.
    StartPos();
    const int64_t spe{Pos().slots_per_epoch};
    const int64_t e1{pos::EpochOf(base_slot, Pos()) + 1};
    const int64_t e2{e1 + 1};
    const int64_t e5{e2 + 3};
    const auto at{[&](int64_t slot) { return static_cast<int>(slot - base_slot); }};
    Slot(at(e1 * spe) + 1);
    Main();
    Take(PosBlock(at(e1 * spe) + 2));
    Slot(at(e2 * spe) + 1);
    Main();
    Take(PosBlock(at(e2 * spe) + 7));
    CBlockIndex* first{WITH_LOCK(::cs_main, return Chain().m_chain.Tip())};
    for (int64_t e{e2 + 1}; e <= e5; ++e) {
        Slot(at(e * spe) + 1);
        Main();
    }
    Take(PosBlock(at(e5 * spe) + 2));
    Hooks().CheckSeeds();
    const CBlock late{PosBlock(at(e5 * spe) + 4)};
    const int64_t late_slot{pos::SlotOf(late.nTime, Pos())};
    using Snapshot = std::map<uint256, pos::Liveness>;
    const auto liveness{[&]() {
        Snapshot snapshot;
        LOCK(::cs_main);
        for (const CKey& k : keys) {
            if (const auto l{pos::GetLiveness(Chain().SideState().View(), e2, XOnly(k))}) snapshot[XOnly(k)] = *l;
        }
        return snapshot;
    }};
    const Snapshot original{liveness()};
    const auto leaders{[&]() {
        std::vector<uint256> keys_of;
        LOCK(::cs_main);
        for (int64_t slot{e2 * spe}; slot <= e2 * spe + 7; ++slot) keys_of.push_back(Hooks().LeaderOf(*first->pprev, slot).pubkey);
        const CBlockIndex* tip{Chain().m_chain.Tip()};
        keys_of.push_back(Hooks().LeaderOf(*tip->pprev, pos::SlotOf(tip->nTime, Pos())).pubkey);
        keys_of.push_back(Hooks().LeaderOf(*tip, late_slot).pubkey);
        return keys_of;
    }};
    const std::vector<uint256> before{leaders()};
    const auto seed_height{pos::SeedHeight(e2, Pos(), Record())};
    BOOST_REQUIRE(seed_height && *seed_height > 0);
    // The mainchain reorg (the same commitments): the block of e2 and the late block keep their
    // leaders (so does the block of e5 before it), a slot of e2 the block skipped has another.
    std::vector<sidechain::MainBlock> replaced;
    bool found{false};
    for (int salt{1}; salt < 1000 && !found; ++salt) {
        replaced = Record().Truncate(*seed_height - 1);
        for (sidechain::MainBlock main : replaced) {
            main.prev_hash = Record().TipHash();
            main.hash = ArithToUint256(UintToArith256(main.hash) + salt);
            BOOST_REQUIRE(Record().Append(main));
        }
        const std::vector<uint256> after{leaders()};
        found = after[7] == before[7] && after[8] == before[8] && after[9] == before[9] && !std::equal(after.begin(), after.begin() + 7, before.begin());
        if (!found) {
            Record().Truncate(*seed_height - 1);
            for (const sidechain::MainBlock& main : replaced) BOOST_REQUIRE(Record().Append(main));
        }
    }
    BOOST_REQUIRE(found);
    const CBlockIndex* last{Tip()};
    // Before the check: the late block waits, through no fault of its peer's.
    Submit(late);
    BOOST_CHECK(Tip() == last);
    BOOST_CHECK_EQUAL(Reason(late), "pos-seed-moved");
    BOOST_CHECK(Chainman().m_mainchain->GetFailure(late.GetHash()) == sidechain::Mainchain::Failure::RECORD);
    // The check: the blocks from e2 on connected again, the late block with them.
    Hooks().CheckSeeds();
    BOOST_CHECK_MESSAGE(Tip()->GetBlockHash() == late.GetHash(), "the late block is not connected after the check: " << Reason(late));
    const Snapshot running{liveness()};
    BOOST_CHECK(running != original);
    // As a node that takes the chain in afresh connects it.
    {
        BlockValidationState state;
        BOOST_REQUIRE(Chain().InvalidateBlock(state, first, /*by_record=*/true));
        WITH_LOCK(::cs_main, Chain().ReconsiderRecordFailure(first));
        BOOST_REQUIRE(Chain().ActivateBestChain(state));
    }
    BOOST_CHECK(Tip()->GetBlockHash() == late.GetHash());
    BOOST_CHECK(liveness() == running);
}


BOOST_FIXTURE_TEST_CASE(attack8_acks_pushed_out_still_taken_back, FastBlocksSetup)
{
    // ATTACK (one validator key, a current or a former one). The blocks that rest on a commitment were
    // found only in the acknowledgements noted in memory, MAX_NOTED_ACKS of them, the oldest dropped
    // first; and a block is noted when it reads and its slot's leader signed it, before its header is
    // checked. A leader signs as many blocks of its slot as it likes, each acknowledging a commitment
    // nobody made: the acknowledgements that count are pushed out. When the mainchain then drops the
    // commitment to a ticket, the node finds no block resting on it and keeps the block that
    // acknowledged it (an anchor that no longer exists), while the nodes that were not flooded take it
    // back: a split. The blocks resting on a commitment come from the chain itself, whatever is noted.
    StartPos();
    Take(PosBlock(1));
    const pos::AnchorAck ack{AckTip(2)};
    const uint256 commitment{ack.ticket.Hash()};
    const uint256 acked{Tip()->GetBlockHash()};
    Take(PosBlock(3));
    const auto resting{[&] { return Hooks().BlocksCommittedBy(commitment, ack.main_height); }};
    {
        const auto blocks{resting()};
        BOOST_REQUIRE(std::find(blocks.begin(), blocks.end(), acked) != blocks.end());
    }

    // The flood: blocks of slot 4 on the tip, signed by its leader, each acknowledging nothing on record.
    const auto flood{[&] { FloodNotedAcks(4); }};
    flood();

    // The mainchain drops the commitment; the follower takes back what rests on it.
    Record().Truncate(ack.main_height - 1);
    for (const uint256& hash : resting()) {
        if (!Index(hash)) continue;
        BlockValidationState state;
        Chain().InvalidateBlock(state, hash, /*by_record=*/true);
    }
    BOOST_CHECK_MESSAGE(!WITH_LOCK(::cs_main, return Chain().m_chain.Contains(*Index(acked))),
                        "attack works: the block that acknowledged a dropped commitment stays on the chain");
    // And it comes back with its commitment, though pushed out again meanwhile.
    flood();
    Commit(commitment);
    {
        LOCK(::cs_main);
        for (const uint256& hash : resting()) {
            if (CBlockIndex* index{Chainman().m_blockman.LookupBlockIndex(hash)}) Chain().ReconsiderRecordFailure(index);
        }
    }
    BlockValidationState state;
    BOOST_REQUIRE(Chain().ActivateBestChain(state));
    BOOST_CHECK(WITH_LOCK(::cs_main, return Chain().m_chain.Contains(*Index(acked))));
}

BOOST_FIXTURE_TEST_CASE(review8_ack_below_a_bmm_block_committed_lower_taken_back, FastBlocksSetup)
{
    // REVIEW 8 (attack8's fix, a split). The blocks resting on a dropped commitment were looked for
    // from the tip down, stopping at the first BMM block the record has committed below the height
    // the commitment had. But the record is that after the mainchain reorg: a BMM block above the
    // acknowledgement, which the reorg committed to again, lower, stops the walk above it. The node
    // keeps a block whose anchor the mainchain dropped, if it did not note it (it restarted, or a
    // leader pushed it out: attack8), while the nodes that noted it take it back.
    StartPos();
    Take(PosBlock(1));
    const int common{Record().Height()};
    Main();
    const pos::AnchorAck ack{AckTip(2)};
    BOOST_REQUIRE_EQUAL(ack.main_height, common + 2);
    const uint256 acked{Tip()->GetBlockHash()};
    const CBlock bmm{BmmBlock(/*use_mempool=*/false)};
    MineBmm(bmm);
    FloodNotedAcks(3);

    // The mainchain drops its blocks above `common` and commits to the BMM block again, first.
    const std::vector<sidechain::MainBlock> removed{Record().Truncate(common)};
    Commit(bmm.GetHash());
    // As the follower does (Act): what rests on each commitment dropped is taken back, then what has a
    // commitment again is checked again.
    for (size_t i{0}; i < removed.size(); ++i) {
        if (!removed[i].bmm) continue;
        for (const uint256& hash : Hooks().BlocksCommittedBy(*removed[i].bmm, common + 1 + static_cast<int>(i))) {
            if (!Index(hash)) continue;
            BlockValidationState state;
            Chain().InvalidateBlock(state, hash, /*by_record=*/true);
        }
    }
    {
        LOCK(::cs_main);
        for (const uint256& hash : Hooks().BlocksCommittedBy(bmm.GetHash(), Record().CommittedHeight(bmm.GetHash()))) {
            if (CBlockIndex* index{Chainman().m_blockman.LookupBlockIndex(hash)}) Chain().ReconsiderRecordFailure(index);
        }
    }
    BlockValidationState state;
    Chain().ActivateBestChain(state);
    BOOST_CHECK_MESSAGE(!WITH_LOCK(::cs_main, return Chain().m_chain.Contains(*Index(acked))),
                        "split: the block that acknowledged a dropped commitment stays on the chain");
}

BOOST_FIXTURE_TEST_CASE(review8_blocks_committed_by_reads_only_the_candidates, FastBlocksSetup)
{
    // REVIEW 8 (attack8's fix, a denial of service). Finding the blocks resting on a commitment read
    // from disk, under cs_main, every block that failed against the record (which went only once out of
    // reach of any reorg, record_failures_bounded: a leader makes two of each slot it leads at no cost, acknowledging a ticket nobody committed) and
    // every acknowledgement of the active chain above the height -- for every commitment the mainchain
    // makes (the follower), drops (the follower), and every ticket a producer may acknowledge, each
    // slot (NextAck). With the noted acknowledgements pushed out (attack8), each is a read. Only an
    // acknowledgement whose nNonce is the height can rest on the commitment: only those are read.
    StartPos();
    Take(PosBlock(1));
    const pos::AnchorAck first{AckTip(2)};
    const uint256 first_block{Tip()->GetBlockHash()};
    AckTip(3);
    AckTip(4);
    // Blocks that fail against the record: each acknowledges, at the height of a commitment on record,
    // a ticket nobody committed to.
    Commit(uint256{0xc0});
    const int other{Record().Height()};
    constexpr int FAILED{5};
    for (int i{5}; i < 5 + FAILED; ++i) {
        const CBlock failed{WithAck(PosBlock(i), pos::AnchorAck{.ticket = Ticket(uint256{static_cast<uint8_t>(i)}, script), .main_height = other})};
        Submit(failed);
        BOOST_REQUIRE(Record().GetFailure(failed.GetHash()) == sidechain::Mainchain::Failure::RECORD);
    }
    const auto flood{FloodNotedAcks(5 + FAILED)};
    BOOST_REQUIRE_EQUAL(FloodPushedOut(flood), 0U);

    // A new commitment (the follower, for each mainchain block): no block of ours rests on it.
    Commit(uint256{0xc1});
    const auto returned{Hooks().BlocksCommittedBy(uint256{0xc1}, Record().Height())};
    BOOST_CHECK_EQUAL(returned.size(), 1U);
    BOOST_CHECK_EQUAL(FloodPushedOut(flood), 0U);
    // The first acknowledgement's commitment dropped: it alone is read, not those above it.
    const auto dropped{Hooks().BlocksCommittedBy(first.ticket.Hash(), first.main_height)};
    BOOST_CHECK(std::find(dropped.begin(), dropped.end(), first_block) != dropped.end());
    BOOST_CHECK_LE(FloodPushedOut(flood), 1U);
}

BOOST_FIXTURE_TEST_CASE(record_failures_bounded, FastBlocksSetup)
{
    // The blocks that failed against the record, which BlocksCommittedBy scans for every commitment the
    // mainchain makes or drops: a leader makes two of each slot it leads at no cost, each acknowledging
    // a ticket nobody committed to. They are kept only while a reorg can reach them: those that leave
    // the active chain where nothing can be taken back any more (DRIVECHAIN_UNDO_DEPTH below the flushed
    // block, SweepDrivechainUndo) are forgotten. Whatever the flood below that, what is kept is the
    // same, and so is everything the node decides.
    StartPos();
    Take(PosBlock(1));
    const int base{WITH_LOCK(::cs_main, return Chain().m_chain.Height())};
    Commit(uint256{0xc0});
    const int other{Record().Height()};
    uint8_t salt{0};
    const auto failing{[&](int i) {
        const pos::AnchorTicket ticket{Ticket(uint256{++salt}, script)};
        const CBlock failed{WithAck(PosBlock(i), pos::AnchorAck{.ticket = ticket, .main_height = other})};
        Submit(failed);
        BOOST_REQUIRE(Record().GetFailure(failed.GetHash()) == sidechain::Mainchain::Failure::RECORD);
        return std::make_pair(failed.GetHash(), ticket.Hash());
    }};
    // The flood, on the tip (at `base`).
    constexpr int FLOOD{40};
    std::vector<std::pair<uint256, uint256>> old_failures;
    for (int i{2}; i < 2 + FLOOD; ++i) old_failures.push_back(failing(i));
    // The chain goes on, and a few blocks fail on its tip.
    for (int i{2 + FLOOD}; i < 2 + FLOOD + 4; ++i) Take(PosBlock(i));
    const int tip_height{WITH_LOCK(::cs_main, return Chain().m_chain.Height())};
    BOOST_REQUIRE_EQUAL(tip_height, base + 4);
    std::vector<std::pair<uint256, uint256>> recent;
    for (int i{2 + FLOOD + 4}; i < 2 + FLOOD + 7; ++i) recent.push_back(failing(i));
    BOOST_REQUIRE_EQUAL(Record().RecordFailures().size(), static_cast<size_t>(FLOOD + 3));

    // What the node decides of the blocks within reach, and its chain, before.
    const auto resting{[&](const std::vector<std::pair<uint256, uint256>>& failures) {
        std::vector<std::vector<uint256>> found;
        for (const auto& [_, commitment] : failures) {
            auto blocks{Hooks().BlocksCommittedBy(commitment, other)};
            std::sort(blocks.begin(), blocks.end());
            found.push_back(std::move(blocks));
        }
        return found;
    }};
    const auto before{resting(recent)};
    for (size_t n{0}; n < recent.size(); ++n) BOOST_CHECK(std::find(before[n].begin(), before[n].end(), recent[n].first) != before[n].end());
    const uint256 tip{Tip()->GetBlockHash()};

    // Nothing at base + 1 or below can be taken back any more: the flood, off the chain at `base`, goes.
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return Chain().ForgetUnreachableRecordFailures(base + 1)), static_cast<size_t>(FLOOD));
    // Bounded: what is left is the failures within reach, however large the flood was.
    BOOST_CHECK_EQUAL(Record().RecordFailures().size(), recent.size());
    for (const auto& [hash, _] : recent) BOOST_CHECK(Record().GetFailure(hash) == sidechain::Mainchain::Failure::RECORD);
    // Unchanged: the blocks resting on the commitments of those within reach, the chain, and the
    // flood's blocks, failed for good either way.
    BOOST_CHECK(resting(recent) == before);
    BOOST_CHECK(Tip()->GetBlockHash() == tip);
    {
        LOCK(::cs_main);
        for (const auto& [hash, _] : old_failures) {
            const CBlockIndex* index{Index(hash)};
            BOOST_REQUIRE(index);
            BOOST_CHECK(index->nStatus & BLOCK_FAILED_VALID);
            BOOST_CHECK(!Chain().m_chain.Contains(*index));
        }
    }
    BlockValidationState state;
    BOOST_REQUIRE(Chain().ActivateBestChain(state));
    BOOST_CHECK(Tip()->GetBlockHash() == tip);
}

BOOST_FIXTURE_TEST_CASE(review8_forged_signature_failing_against_the_record, FastBlocksSetup)
{
    // REVIEW 8 (a lead, confirmed): the peg step (GetPegStep) was read before the signature was
    // checked (AfterBlock). A header taken unchecked, with a forged signature (anyone's to make: the
    // block hash does not cover it), whose block acknowledges a ticket the record does not commit to,
    // failed bmm-unknown: against the record, noted on disk for the block. It is the copy's failure,
    // that of its signature (an extension failure, in memory): checked first (CheckExtension).
    StartPos();
    Take(PosBlock(1));
    const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
    const CBlock anchor{PosBlock(2, ack)};
    Take(anchor);
    const int64_t epoch{pos::EpochOf(pos::SlotOf(anchor.nTime, Pos()), Pos())};
    const int last{static_cast<int>((epoch + 2) * Pos().slots_per_epoch - base_slot)};
    // A commitment on record, to something else, at the height the block acknowledges.
    Commit(uint256{0xc0});
    const int other{Record().Height()};
    const CBlock honest{WithAck(PosBlock(last), pos::AnchorAck{.ticket = Ticket(anchor.GetHash(), script), .main_height = other})};
    CBlock forged{honest};
    forged.ext.sig.fill(0);
    // The node as it was before it connected the anchor: the anchor taken back, its set forgotten.
    CBlockIndex* anchor_index{Index(anchor.GetHash())};
    {
        BlockValidationState state;
        BOOST_REQUIRE(Chain().InvalidateBlock(state, anchor_index));
        LOCK(::cs_main);
        Chain().ResetBlockFailureFlags(anchor_index);
        Chainman().RecalculateBestHeader();
    }
    Hooks().ForgetKnownSets();
    BOOST_REQUIRE(Tip() == anchor_index->pprev);
    BOOST_REQUIRE_EQUAL(HeaderReason({forged}), "");
    const CBlockIndex* index{Index(honest.GetHash())};
    BOOST_REQUIRE(index);
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return index->m_unchecked));
    // The forged block, once the anchor is connected: it fails for its signature.
    Submit(forged);
    BOOST_CHECK(Tip() == anchor_index);
    BOOST_CHECK_EQUAL(Reason(forged), "bad-pos-sig");
    BOOST_CHECK(WITH_LOCK(::cs_main, return Chainman().UncheckedExtensionFailed(*index)));
    BOOST_CHECK(Record().GetFailure(honest.GetHash()) != sidechain::Mainchain::Failure::RECORD);
    // The honest header replaces it; with it, the block fails against the record.
    BOOST_CHECK_EQUAL(HeaderReason({honest}), "");
    BlockValidationState state;
    Chain().ActivateBestChain(state);
    BOOST_CHECK(Tip() == anchor_index);
    BOOST_CHECK_EQUAL(Reason(honest), "bmm-unknown");
    BOOST_CHECK(Record().GetFailure(honest.GetHash()) == sidechain::Mainchain::Failure::RECORD);
}


// AUDIT 7, finding 1 (regression; the PoC halted the chain): the reward for a proof was owed to the
// carrier's first coinbase output script verbatim. A script of 1000 OP_CHECKMULTISIG (80,000 sigop
// cost) fit the carrier exactly; every later peg step had to pay it, and any other payout with a
// signature check (a P2PKH deposit anyone can make) put it over the limit: no peg step could be valid
// any more, blocks of stake stopped at max_unanchored_slots. Now a carrier whose first output is no
// address (IsRewardPayee) is paid nothing: the reward goes to the reserve, and the chain goes on.
BOOST_FIXTURE_TEST_CASE(audit7_reporter_script_halts_peg_steps, FastBlocksSetup)
{
    StartPos();
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    const CAmount bond{100000};
    Accept(Pay(coin, spent, {BondOut(keys[0], bond, Owner(key), &key, coin), BondOut(keys[1], bond, Owner(key), &key, coin), BondOut(keys[2], bond, Owner(key), &key, coin)}));
    Take(PosBlock(1));
    const auto [first, second]{TwoBlocks(2)};
    const pos::DoubleProposal proof{.a = first, .b = second};
    const uint256 offender{XOnly(Leader(base_slot + 2))};
    Take(first);
    int i{3};
    while (XOnly(Leader(base_slot + i)) == offender) ++i;
    CBlock carrier{WithEvidence(i, {proof})};
    CScript poison;
    for (int n{0}; n < 1000; ++n) poison << OP_CHECKMULTISIG;
    SetCoinbase(carrier, [&](CMutableTransaction& tx) { tx.vout[0].scriptPubKey = poison; });
    Resign(carrier);
    BOOST_CHECK_EQUAL(GetLegacySigOpCount(*carrier.vtx[0]) * WITNESS_SCALE_FACTOR, 80000U);
    const CAmount reserve_before{Stake().pot_reserve};
    Take(carrier);
    const CAmount penalty{bond / 10};
    // Punished all the same; nothing owed to the script: all of the penalty to the reserve.
    BOOST_CHECK(Validator(offender)->tombstoned);
    BOOST_CHECK_EQUAL(Stake().slashed, penalty);
    BOOST_CHECK_EQUAL(Stake().rewards_owed, 0);
    BOOST_CHECK_EQUAL(Stake().pot_reserve, reserve_before + penalty);
    BOOST_CHECK_EQUAL(Stake().bonded + Stake().unbonding + Stake().pot_reserve + Stake().rewards_owed, 3 * bond + reserve_before);
    // Anyone deposits to a P2PKH address on the sidechain.
    Main([&](sidechain::MainBlock& block) {
        sidechain::MainDeposit deposit;
        deposit.destination = EncodeDestination(PKHash{key.GetPubKey()});
        deposit.amount = COIN / 100;
        deposit.txid = uint256{0x99};
        block.deposits.push_back(deposit);
    });
    // Peg steps to come pay no reward to the script, and are valid: a BMM block, an acknowledgement.
    {
        const CBlock bmm{BmmBlock(/*use_mempool=*/false)};
        for (const CTxOut& out : bmm.vtx[0]->vout) BOOST_CHECK(out.scriptPubKey != poison);
        BOOST_CHECK_EQUAL(Check(bmm), "");
    }
    const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
    int j{i + 1};
    while (XOnly(Leader(base_slot + j)) == offender) ++j;
    const int64_t slot{Slot(j)};
    std::string error;
    auto block{producer->Build(slot, ack, error)};
    BOOST_REQUIRE_MESSAGE(block, error);
    BOOST_REQUIRE(pos::Producer::Sign(*block, Leader(slot), Genesis()));
    for (const CTxOut& out : block->vtx[0]->vout) BOOST_CHECK(out.scriptPubKey != poison);
    BOOST_CHECK_EQUAL(Check(*block), "");
    Take(*block);
    // The producer goes on.
    int k{j + 1};
    while (XOnly(Leader(base_slot + k)) == offender) ++k;
    BOOST_CHECK_MESSAGE(producer->Produce(Slot(k), error), error);
    BOOST_CHECK(Tip()->pprev->GetBlockHash() == block->GetHash());
}

// AUDIT 7, finding 1 (regression), the pot's own script as the carrier's first output: the reward was
// a pot output in every peg step to come, which no block can carry (BMM: bad-pos-pot-not-pos; block of
// stake: bad-pos-producer-share / bad-pos-pot-multiple). Now it goes to the reserve.
BOOST_FIXTURE_TEST_CASE(audit7_reporter_pot_script_halts_peg_steps, FastBlocksSetup)
{
    StartPos();
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    const CAmount bond{100000};
    Accept(Pay(coin, spent, {BondOut(keys[0], bond, Owner(key), &key, coin), BondOut(keys[1], bond, Owner(key), &key, coin), BondOut(keys[2], bond, Owner(key), &key, coin)}));
    Take(PosBlock(1));
    const auto [first, second]{TwoBlocks(2)};
    const pos::DoubleProposal proof{.a = first, .b = second};
    const uint256 offender{XOnly(Leader(base_slot + 2))};
    Take(first);
    int i{3};
    while (XOnly(Leader(base_slot + i)) == offender) ++i;
    CBlock carrier{WithEvidence(i, {proof})};
    BOOST_REQUIRE_EQUAL(carrier.vtx[0]->vout.size(), 2U); // the share (no fees: 0), the witness commitment
    SetCoinbase(carrier, [&](CMutableTransaction& tx) { tx.vout[0].scriptPubKey = pos::PotScript(); });
    Resign(carrier);
    Take(carrier);
    BOOST_CHECK_EQUAL(Stake().rewards_owed, 0);
    BOOST_CHECK_EQUAL(Stake().pot_reserve, bond / 10);
    const CBlock bmm{BmmBlock(/*use_mempool=*/false)};
    for (const CTxOut& out : bmm.vtx[0]->vout) BOOST_CHECK(!pos::IsPotScript(out.scriptPubKey));
    BOOST_CHECK_EQUAL(Check(bmm), "");
    const pos::AnchorAck ack{Anchor(Ticket(Tip()->GetBlockHash(), script))};
    int j{i + 1};
    while (XOnly(Leader(base_slot + j)) == offender) ++j;
    const int64_t slot{Slot(j)};
    std::string error;
    auto block{producer->Build(slot, ack, error)};
    BOOST_REQUIRE_MESSAGE(block, error);
    BOOST_REQUIRE(pos::Producer::Sign(*block, Leader(slot), Genesis()));
    BOOST_CHECK(!Pays(*block, CTxOut{bond / 10 / 8, pos::PotScript()}));
    BOOST_CHECK_EQUAL(Check(*block), "");
}

// AUDIT 7, finding 1 (regression), the BMM routes: one key plus a BMM win (a BMM block carries the
// proof, its first output poisoned), and a BMM block's own pot, owed to its first output (deferred to
// later peg steps when the payout queue is behind). Neither is owed to a script that is no address:
// the reward and the pot go to the reserve, and the next peg steps are valid.
BOOST_FIXTURE_TEST_CASE(audit7_bmm_payee_not_address_pays_reserve, FastBlocksSetup)
{
    StartPos();
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    const CAmount bond{100000};
    Accept(Pay(coin, spent, {BondOut(keys[0], bond, Owner(key), &key, coin), BondOut(keys[1], bond, Owner(key), &key, coin), BondOut(keys[2], bond, Owner(key), &key, coin)}));
    Take(PosBlock(1));
    const auto [first, second]{TwoBlocks(2)};
    pos::DoubleProposal proof{.a = first, .b = second};
    proof.Normalize();
    const uint256 offender{XOnly(Leader(base_slot + 2))};
    Take(first);
    CScript poison;
    for (int n{0}; n < 1000; ++n) poison << OP_CHECKMULTISIG;
    // The merged miner puts the proof in its block; its payee is the poison.
    std::string reason;
    BOOST_REQUIRE_MESSAGE(Hooks().AddEvidence(proof, reason), reason);
    const CBlock carrying{BmmBlock(/*use_mempool=*/false, poison)};
    bool carried{false};
    for (const CTxOut& out : carrying.vtx[0]->vout) carried |= pos::ParseEvidenceScript(out.scriptPubKey).has_value();
    BOOST_REQUIRE(carried);
    MineBmm(carrying);
    BOOST_CHECK(Tip()->GetBlockHash() == carrying.GetHash());
    const CAmount penalty{bond / 10};
    BOOST_CHECK(Validator(offender)->tombstoned);
    BOOST_CHECK_EQUAL(Stake().rewards_owed, 0);
    // (Its own peg step released a share of the reserve before the penalty came in.)
    const CAmount reserve{Stake().pot_reserve};
    BOOST_CHECK_GE(reserve, penalty);
    // (A BMM block whose first output is the pot's own script is no block: a BMM block has no pot.)
    BOOST_CHECK_EQUAL(Check(BmmBlock(/*use_mempool=*/false, pos::PotScript())), "bad-pos-pot-not-pos");
    // A BMM block whose first output is the poison, then a nonstandard script: the pot and the share of
    // the reserve that anchor releases are paid to neither, and go back to the reserve.
    for (const CScript& payee : {poison, CScript() << OP_TRUE}) {
        const CAmount before{Stake().pot_reserve};
        const CAmount paid_before{Fast().pot_paid};
        const CBlock bmm{BmmBlock(/*use_mempool=*/false, payee)};
        for (size_t n{1}; n < bmm.vtx[0]->vout.size(); ++n) BOOST_CHECK(bmm.vtx[0]->vout[n].scriptPubKey != payee);
        BOOST_CHECK_EQUAL(Check(bmm), "");
        MineBmm(bmm);
        BOOST_CHECK(Tip()->GetBlockHash() == bmm.GetHash());
        // (The share released, and the pot of this anchor, back in the reserve.)
        BOOST_CHECK_EQUAL(Stake().pot_reserve, before + (Fast().pot_paid - paid_before));
    }
    // Then an address takes the pot again, with the share of the reserve, and the chain goes on.
    const CBlock paid{BmmBlock(/*use_mempool=*/false)};
    const CAmount release{Stake().pot_reserve / Pos().pot_reserve_anchors + (Stake().pot_reserve % Pos().pot_reserve_anchors != 0)};
    BOOST_CHECK(Pays(paid, CTxOut{Fast().pot_accrued - Fast().pot_paid + release, BmmPayee()}));
    MineBmm(paid);
    BOOST_CHECK(Tip()->GetBlockHash() == paid.GetHash());
    BOOST_CHECK_LT(Stake().pot_reserve, reserve);
}


// AUDIT 7, finding 2 (regression; the PoC took a "final" block back): commitments older than the
// oldest of the AnchorsWalked newest anchors were not counted against forks. A bidder that won more
// commitments since a fork (blocks kept to itself) than the window holds won the order of chains after
// the chain called blocks after the fork final. Now every skipped commitment since the fork counts
// (DeepForkMargin): no block above the fork is final, and no final block is taken back.
BOOST_FIXTURE_TEST_CASE(audit7_final_block_taken_back_by_old_hidden_commitments, FastBlocksSetup)
{
    StartPos();
    Take(PosBlock(1));
    const CBlockIndex* fork{Tip()};
    const size_t walked{pos::AnchorsWalked(Pos().final_anchors)};
    // The bidder's branch: walked + 1 BMM blocks on the fork, each committed (the mainchain's record
    // has them); "kept to itself" here = taken, then marked invalid so that the node's chain leaves it.
    std::vector<uint256> hidden;
    for (size_t n{0}; n <= walked; ++n) {
        Slot(2 + static_cast<int>(n));
        const CBlock block{BmmBlock(/*use_mempool=*/false)};
        MineBmm(block);
        hidden.push_back(block.GetHash());
    }
    {
        BlockValidationState state;
        BOOST_REQUIRE(Chain().InvalidateBlock(state, Index(hidden[0])));
        BOOST_REQUIRE(Chain().ActivateBestChain(state));
    }
    BOOST_REQUIRE(Tip() == fork);
    // The chain: `walked` anchors since the fork (BMM blocks of another payee), one fewer than the branch.
    for (size_t n{0}; n < walked; ++n) {
        Slot(40 + static_cast<int>(n));
        MineBmm(BmmBlock(/*use_mempool=*/false, Wpkh(2)));
    }
    const int final_height{WITH_LOCK(::cs_main, return Hooks().FinalHeight())};
    BOOST_TEST_MESSAGE("audit7: fork at " << fork->nHeight << ", chain tip " << Tip()->nHeight << ", final height " << final_height);
    // Nothing above the fork is final: the branch has as many anchors since it, and more.
    BOOST_CHECK_LE(final_height, fork->nHeight);
    const CBlockIndex* was_final{final_height >= 0 ? WITH_LOCK(::cs_main, return Chain().m_chain[final_height]) : nullptr};
    // The bidder shows its branch (here: the operator's mark taken back, as if it came from a peer).
    {
        LOCK(::cs_main);
        Chain().ResetBlockFailureFlags(Index(hidden[0]));
        Chainman().RecalculateBestHeader();
    }
    {
        BlockValidationState state;
        BOOST_REQUIRE(Chain().ActivateBestChain(state));
    }
    // The branch wins (the order of chains is unchanged), and the final block stays.
    BOOST_CHECK(Tip()->GetBlockHash() == hidden.back());
    if (was_final) BOOST_CHECK(WITH_LOCK(::cs_main, return Chain().m_chain.Contains(*was_final)));

    // Once the chain has more anchors since the fork than the commitments it skipped (all of them, the
    // branch's included), blocks are final again: here the branch's own chain, which skipped the
    // `walked` commitments of the other.
    for (size_t n{0}; n < 2 * walked; ++n) {
        Slot(80 + static_cast<int>(n));
        MineBmm(BmmBlock(/*use_mempool=*/false));
    }
    const int later{WITH_LOCK(::cs_main, return Hooks().FinalHeight())};
    BOOST_CHECK_GT(later, fork->nHeight);
    BOOST_CHECK_LT(later, Tip()->nHeight);
}

// AUDIT 7, finding 2: the margin of deep forks is the same whether computed at once or step by step
// (what DeepForkMargin keeps between calls), and a mainchain reorg that takes commitments away is seen.
BOOST_FIXTURE_TEST_CASE(audit7_deep_fork_margin_incremental, FastBlocksSetup)
{
    StartPos();
    Take(PosBlock(1));
    const size_t walked{pos::AnchorsWalked(Pos().final_anchors)};
    std::vector<std::vector<int>> stepwise;
    for (size_t n{0}; n < 3 * walked; ++n) {
        Slot(2 + static_cast<int>(n));
        MineBmm(BmmBlock(/*use_mempool=*/false));
        // A commitment nobody shows (a skipped one), every fourth mainchain block.
        if (n % 4 == 0) Commit(uint256{static_cast<uint8_t>(0x80 + n)});
        stepwise.push_back(WITH_LOCK(::cs_main, return pos::AnchoredHeights(Hooks(), *Tip(), Pos().final_anchors)));
    }
    const auto heights{[&] { return WITH_LOCK(::cs_main, return pos::AnchoredHeights(Hooks(), *Tip(), Pos().final_anchors)); }};
    // Computed at once (nothing kept), the same at the tip.
    Hooks().ForgetDeepFinality();
    const auto at_once{heights()};
    BOOST_CHECK(at_once == stepwise.back());
    BOOST_REQUIRE(!at_once.empty());
    BOOST_CHECK_LT(at_once.back(), Tip()->nHeight);
    // The record loses its last blocks (a mainchain reorg): counted again where it changed, the same
    // as computed at once.
    BOOST_CHECK(heights() == at_once);
    Record().Truncate(Record().Height() - 3);
    const auto after_reorg{heights()};
    Hooks().ForgetDeepFinality();
    BOOST_CHECK(heights() == after_reorg);
}


// AUDIT 7, finding 3: an unbond names the first coin its validator's first bond spent and the newest
// anchor block of the chain when that bond was applied, so an unbond signed on another chain with the
// same genesis (a test network restarted without a new genesis), where the same owner bonded the same
// key, with the same sequence, is refused here: with another coin, and also with the same coin (the
// deposits credited again after a restart give the same transactions) under another anchor block.
BOOST_AUTO_TEST_CASE(audit7_unbond_not_replayable_across_chains)
{
    ECC_Context ecc;
    CKey validator, owner;
    validator.MakeNewKey(/*fCompressed=*/true);
    owner.MakeNewKey(/*fCompressed=*/true);
    Consensus::PosParams params;
    params.min_stake = 100;
    const uint256 genesis{3};
    const pos::Staking staking{params, genesis};
    const uint160 owner_hash{owner.GetPubKey().GetID()};
    const uint256 pubkey{XOnly(validator)};
    std::array<unsigned char, 64> pop;
    BOOST_REQUIRE(validator.SignSchnorr(pos::BondPopHash(owner_hash, genesis), pop, nullptr, uint256{}));
    const auto bond_on{[&](sidechain::StoreOverlay& out, const COutPoint& spent, const uint256& anchor_point) {
        std::string reason;
        CMutableTransaction bond;
        bond.vin.emplace_back(spent);
        std::vector<unsigned char> owner_sig;
        BOOST_REQUIRE(owner.SignCompact(MessageHash(pos::BondMessage(pubkey, 10000, spent, genesis)), owner_sig));
        bond.vout.emplace_back(10000, pos::BondScript(pubkey, owner_hash, pop, owner_sig));
        BOOST_REQUIRE_MESSAGE(staking.ApplyTx(out, CTransaction{bond}, 1, 0, reason, anchor_point), reason);
        const auto record{pos::GetValidator(out, pubkey)};
        BOOST_CHECK(record->first_bond == spent);
        BOOST_CHECK(record->first_bond_anchor == anchor_point);
    }};
    const auto unbond_tx{[&](const COutPoint& first_bond, const uint256& anchor) {
        pos::Unbond u;
        u.pubkey = pubkey;
        u.amount = pos::UNBOND_ALL;
        u.sequence = 0;
        BOOST_REQUIRE(owner.SignCompact(MessageHash(pos::UnbondMessage(pubkey, u.amount, u.sequence, first_bond, anchor, genesis)), u.signature));
        CMutableTransaction tx;
        tx.vout.emplace_back(0, pos::UnbondScript(u));
        return CTransaction{tx};
    }};
    const COutPoint coin{Txid::FromUint256(uint256{0x11}), 0}, other_coin{Txid::FromUint256(uint256{0x22}), 1};
    const uint256 anchor_a{0xA1}, anchor_b{0xB2};
    // Chain A: bonded, then unbonded (the unbond is public).
    sidechain::EmptyStore empty_a;
    sidechain::StoreOverlay chain_a{empty_a};
    bond_on(chain_a, coin, anchor_a);
    const CTransaction unbond_a{unbond_tx(coin, anchor_a)};
    std::string reason;
    BOOST_CHECK_MESSAGE(staking.ApplyTx(chain_a, unbond_a, 2, 0, reason, anchor_a), reason);
    // Chain B, same genesis, keys, owner and sequence, and the same first coin, under another anchor:
    // A's unbond is no unbond here; B's own is.
    sidechain::EmptyStore empty_b;
    sidechain::StoreOverlay chain_b{empty_b};
    bond_on(chain_b, coin, anchor_b);
    BOOST_CHECK(!staking.ApplyTx(chain_b, unbond_a, 2, 0, reason, anchor_b));
    BOOST_CHECK_EQUAL(reason, "bad-pos-unbond-sig");
    BOOST_CHECK_EQUAL(pos::GetValidator(chain_b, pubkey)->bonded, 10000);
    BOOST_CHECK_MESSAGE(staking.ApplyTx(chain_b, unbond_tx(coin, anchor_b), 2, 0, reason, anchor_b), reason);
    BOOST_CHECK_EQUAL(pos::GetValidator(chain_b, pubkey)->bonded, 0);
    // Chain C, the same anchor block as A but another first coin: refused too.
    sidechain::EmptyStore empty_c;
    sidechain::StoreOverlay chain_c{empty_c};
    bond_on(chain_c, other_coin, anchor_a);
    BOOST_CHECK(!staking.ApplyTx(chain_c, unbond_a, 2, 0, reason, anchor_a));
    BOOST_CHECK_EQUAL(reason, "bad-pos-unbond-sig");
    // Before a chain's first anchor (no anchor block to name: every chain of the genesis would name the
    // same nothing), no first bond: so no unbond of one chain instance can serve on another.
    for (int instance{0}; instance < 2; ++instance) {
        sidechain::EmptyStore empty;
        sidechain::StoreOverlay unanchored{empty};
        CMutableTransaction bond;
        bond.vin.emplace_back(coin);
        std::vector<unsigned char> owner_sig;
        BOOST_REQUIRE(owner.SignCompact(MessageHash(pos::BondMessage(pubkey, 10000, coin, genesis)), owner_sig));
        bond.vout.emplace_back(10000, pos::BondScript(pubkey, owner_hash, pop, owner_sig));
        BOOST_CHECK(!staking.ApplyTx(unanchored, CTransaction{bond}, 1, 0, reason, uint256{}));
        BOOST_CHECK_EQUAL(reason, "bad-pos-bond-unanchored");
        BOOST_CHECK(!pos::GetValidator(unanchored, pubkey));
        BOOST_CHECK(!staking.ApplyTx(unanchored, unbond_tx(coin, uint256{}), 2, 0, reason, uint256{}));
        BOOST_CHECK_EQUAL(reason, "bad-pos-unbond-unknown");
    }
    // A top-up (not a first bond) needs none: the binding is the first bond's.
    BOOST_CHECK(pos::GetValidator(chain_c, pubkey)->first_bond_anchor == anchor_a);
}

// AUDIT 7, finding 3: on a chain, the first bond records the newest anchor block (FastState::
// anchor_point), and the node's unbond, made as the wallet makes it, is taken.
BOOST_FIXTURE_TEST_CASE(audit7_first_bond_anchor_on_chain, FastBlocksSetup)
{
    StartPos();
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    const COutPoint first{coin};
    Accept(Pay(coin, spent, {BondOut(keys[0], 100000, Owner(key), &key, coin)}));
    Take(PosBlock(1));
    const auto record{Validator(XOnly(keys[0]))};
    BOOST_REQUIRE(record);
    BOOST_CHECK(record->first_bond == first);
    BOOST_CHECK(!record->first_bond_anchor.IsNull());
    BOOST_CHECK(Index(record->first_bond_anchor) != nullptr);
    BOOST_CHECK_EQUAL(StakingRules(TxWith({UnbondOut(keys[0], pos::UNBOND_ALL, record->unbond_seq, key)})), "");
}

// The fee split (pos/feesplit.h): b = max(10 %, 50 % · min(1, 60 / S)) of the ordinary fees, rounded down.
BOOST_AUTO_TEST_CASE(fee_split_math)
{
    const CAmount fee{10000};
    const std::vector<std::tuple<int64_t, CAmount, int64_t>> cases{
        {30, 5000, 5000}, {60, 5000, 5000}, {90, 3333, 3333}, {120, 2500, 2500}, {300, 1000, 1000}, {600, 1000, 1000}, {3600, 1000, 1000}};
    for (const auto& [spacing, bidder, bps] : cases) {
        BOOST_CHECK_EQUAL(pos::BidderShare(fee, spacing), bidder);
        BOOST_CHECK_EQUAL(pos::BidderShareBps(spacing), bps);
    }
    // The floor of 10 % is reached at S = 300 s exactly; below, the share falls with S.
    BOOST_CHECK_EQUAL(pos::BidderShare(fee, 299), 1003);
    BOOST_CHECK_EQUAL(pos::BidderShare(fee, 301), 1000);
    BOOST_CHECK_EQUAL(pos::BidderShare(fee, 61), 4918);
    // Rounding: down for the bidder, the rest (the odd satoshi of a 50/50 split) the validators'.
    BOOST_CHECK_EQUAL(pos::BidderShare(10001, 60), 5000);
    BOOST_CHECK_EQUAL(pos::BidderShare(10001, 90), 3333);
    BOOST_CHECK_EQUAL(pos::BidderShare(7, 600), 0);
    BOOST_CHECK_EQUAL(pos::BidderShare(19, 600), 1);
    BOOST_CHECK_EQUAL(pos::BidderShare(1, 30), 0);
    BOOST_CHECK_EQUAL(pos::BidderShare(0, 30), 0);
    // Spacings of 0 or below (median times that did not move) count as fast blocks: 50/50.
    BOOST_CHECK_EQUAL(pos::BidderShare(fee, 0), 5000);
    BOOST_CHECK_EQUAL(pos::BidderShare(fee, -5), 5000);
    // At the largest amounts, no overflow (30 · MAX_MONEY fits).
    BOOST_CHECK_EQUAL(pos::BidderShare(MAX_MONEY, 90), MAX_MONEY / 3);
    BOOST_CHECK_EQUAL(pos::BidderShare(MAX_MONEY, 1000000), MAX_MONEY / 10);

    // The median of the spacings: the middle one, the upper middle for an even count, 60 for none.
    BOOST_CHECK_EQUAL(pos::MedianSpacing({}), 60);
    BOOST_CHECK_EQUAL(pos::MedianSpacing({90}), 90);
    BOOST_CHECK_EQUAL(pos::MedianSpacing({300, 10, 90}), 90);
    BOOST_CHECK_EQUAL(pos::MedianSpacing({10, 300, 90, 20}), 90);
    BOOST_CHECK_EQUAL(pos::MedianSpacing({-3, 0, 5000}), 0);

    // S from a record of median times: the last 11 spacings, none below the slot's activation.
    sidechain::Mainchain record;
    BOOST_REQUIRE(record.Start(100));
    std::vector<int64_t> times{1000};
    for (int i{0}; i < 5; ++i) times.push_back(times.back() + 600);
    for (int i{0}; i < 11; ++i) times.push_back(times.back() + (i % 2 ? 30 : 90));
    for (size_t i{0}; i < times.size(); ++i) {
        sidechain::MainBlock block;
        block.hash = uint256{static_cast<uint8_t>(i + 1)};
        block.prev_hash = record.TipHash();
        block.time = times[i];
        BOOST_REQUIRE(record.Append(block));
    }
    const int tip{record.Height()};
    BOOST_CHECK_EQUAL(tip, 100 + static_cast<int>(times.size()) - 1);
    // The newest 11: six of 90, five of 30.
    BOOST_CHECK_EQUAL(pos::MedianSpacingAt(record, tip, 100), 90);
    // Five blocks back: 6 of 600 ... the oldest spacings dominate.
    BOOST_CHECK_EQUAL(pos::MedianSpacingAt(record, 100 + 7, 100), 600);
    // From the activation only: at its second block one spacing, at its first none.
    BOOST_CHECK_EQUAL(pos::MedianSpacingAt(record, 101, 100), 600);
    BOOST_CHECK_EQUAL(pos::MedianSpacingAt(record, 100, 100), 60);
    BOOST_CHECK_EQUAL(pos::MedianSpacingAt(record, 103, 102), 600);
    BOOST_CHECK_EQUAL(pos::MedianSpacingAt(record, 102, 102), 60);
    BOOST_CHECK_EQUAL(pos::MedianSpacingAt(record, -1, 0), 60);
}

// The second curve (pos/feesplit.h, "the fee curve of 2026-10-18 (2)"): b = clamp(50 % - 5 % · (S - 60) / 60, 20 %, 50 %),
// the basis points rounded down, B = floor(F · bps / 10000).
BOOST_AUTO_TEST_CASE(fee_curve2_math)
{
    using pos::FeeCurve;
    const CAmount fee{10000};
    const std::vector<std::tuple<int64_t, int64_t>> cases{
        {30, 5000}, {60, 5000}, {90, 4750}, {120, 4500}, {180, 4000}, {240, 3500}, {300, 3000}, {360, 2500}, {420, 2000}, {600, 2000}, {3600, 2000}};
    for (const auto& [spacing, bps] : cases) {
        BOOST_CHECK_EQUAL(pos::BidderShareBps(spacing, FeeCurve::LINEAR), bps);
        BOOST_CHECK_EQUAL(pos::BidderShare(fee, spacing, FeeCurve::LINEAR), bps);
    }
    // Between whole steps the basis points round down: 61 s is 4991.67, 4991; 419 s is 2008.33, 2008.
    BOOST_CHECK_EQUAL(pos::BidderShareBps(61, FeeCurve::LINEAR), 4991);
    BOOST_CHECK_EQUAL(pos::BidderShareBps(66, FeeCurve::LINEAR), 4950);
    BOOST_CHECK_EQUAL(pos::BidderShareBps(67, FeeCurve::LINEAR), 4941);
    BOOST_CHECK_EQUAL(pos::BidderShareBps(419, FeeCurve::LINEAR), 2008);
    BOOST_CHECK_EQUAL(pos::BidderShareBps(421, FeeCurve::LINEAR), 2000);
    // B rounds down, the odd satoshis are the validators'.
    BOOST_CHECK_EQUAL(pos::BidderShare(10001, 60, FeeCurve::LINEAR), 5000);
    BOOST_CHECK_EQUAL(pos::BidderShare(10001, 90, FeeCurve::LINEAR), 4750);
    BOOST_CHECK_EQUAL(pos::BidderShare(19999, 90, FeeCurve::LINEAR), 9499);
    BOOST_CHECK_EQUAL(pos::BidderShare(7, 600, FeeCurve::LINEAR), 1);
    BOOST_CHECK_EQUAL(pos::BidderShare(4, 600, FeeCurve::LINEAR), 0);
    BOOST_CHECK_EQUAL(pos::BidderShare(19, 600, FeeCurve::LINEAR), 3);
    BOOST_CHECK_EQUAL(pos::BidderShare(1, 30, FeeCurve::LINEAR), 0);
    BOOST_CHECK_EQUAL(pos::BidderShare(0, 90, FeeCurve::LINEAR), 0);
    BOOST_CHECK_EQUAL(pos::BidderShare(-5, 90, FeeCurve::LINEAR), 0);
    // Spacings of 0 or below: 50/50; the largest: the floor.
    BOOST_CHECK_EQUAL(pos::BidderShare(fee, 0, FeeCurve::LINEAR), 5000);
    BOOST_CHECK_EQUAL(pos::BidderShare(fee, -5, FeeCurve::LINEAR), 5000);
    BOOST_CHECK_EQUAL(pos::BidderShareBps(std::numeric_limits<int64_t>::max(), FeeCurve::LINEAR), 2000);
    BOOST_CHECK_EQUAL(pos::BidderShareBps(std::numeric_limits<int64_t>::min(), FeeCurve::LINEAR), 5000);
    // No overflow at the largest amounts (MAX_MONEY · 5000 does not fit in 64 bits).
    BOOST_CHECK_EQUAL(pos::BidderShare(MAX_MONEY, 90, FeeCurve::LINEAR), MAX_MONEY / 10000 * 4750);
    BOOST_CHECK_EQUAL(pos::BidderShare(MAX_MONEY, 1000000, FeeCurve::LINEAR), MAX_MONEY / 5);
    BOOST_CHECK_EQUAL(pos::BidderShare(MAX_MONEY - 1, 61, FeeCurve::LINEAR), static_cast<CAmount>((static_cast<__int128>(MAX_MONEY - 1) * 4991) / 10000));
    // Everywhere: bps = floor((5000 · 60 - 500 · (S - 60)) / 60) within [2000, 5000], never rising with S, and
    // B = floor(F · bps / 10000) exactly; the first curve unchanged by default.
    int64_t last{5000};
    for (int64_t spacing{-10}; spacing <= 1000; ++spacing) {
        const int64_t bps{pos::BidderShareBps(spacing, FeeCurve::LINEAR)};
        const int64_t exact_num{5000 * 60 - 500 * (spacing - 60)};
        const int64_t expected{std::clamp<int64_t>(exact_num >= 0 ? exact_num / 60 : -((-exact_num + 59) / 60), 2000, 5000)};
        BOOST_CHECK_EQUAL(bps, expected);
        BOOST_CHECK(bps <= last);
        last = bps;
        for (const CAmount f : {CAmount{1}, CAmount{9999}, CAmount{123456789}, MAX_MONEY}) {
            BOOST_CHECK_EQUAL(pos::BidderShare(f, spacing, FeeCurve::LINEAR), static_cast<CAmount>(static_cast<__int128>(f) * bps / 10000));
        }
        BOOST_CHECK_EQUAL(pos::BidderShare(fee, spacing), pos::BidderShare(fee, spacing, FeeCurve::FIRST));
        BOOST_CHECK_EQUAL(pos::BidderShareBps(spacing), pos::BidderShareBps(spacing, FeeCurve::FIRST));
    }
    // SplitFees takes the curve it is given.
    CMutableTransaction plain;
    plain.vout.emplace_back(COIN, CScript() << OP_TRUE);
    const std::vector<CTransactionRef> vtx{MakeTransactionRef(CMutableTransaction{}), MakeTransactionRef(plain)};
    const std::vector<CAmount> fees{fee};
    BOOST_CHECK_EQUAL(pos::SplitFees(vtx, fees, /*fees_without_coinbase=*/true, 600, FeeCurve::FIRST).validators, 9000);
    BOOST_CHECK_EQUAL(pos::SplitFees(vtx, fees, /*fees_without_coinbase=*/true, 600, FeeCurve::LINEAR).validators, 8000);
}

BOOST_AUTO_TEST_CASE(fee_split_peg_transactions)
{
    const CScript dest{Wpkh(0x01)};
    CMutableTransaction plain;
    plain.vout.emplace_back(1000, dest);
    BOOST_CHECK(!pos::IsPegTx(CTransaction{plain}));
    CMutableTransaction withdrawal{plain};
    withdrawal.vout.emplace_back(5000, sidechain::WithdrawalScript(100, uint160{}, dest));
    BOOST_CHECK(pos::IsPegTx(CTransaction{withdrawal}));
    sidechain::RefundRequest request;
    request.withdrawal = COutPoint{Txid::FromUint256(uint256::ONE), 0};
    request.signature.assign(65, 1);
    CMutableTransaction refund{plain};
    refund.vout.emplace_back(0, sidechain::RefundScript(request));
    BOOST_CHECK(pos::IsPegTx(CTransaction{refund}));
    // Bonds and unbonds are staking, not the peg.
    CMutableTransaction bond{plain};
    bond.vout.emplace_back(COIN, pos::BondScript(uint256::ONE, uint160{}, std::array<unsigned char, 64>{}));
    BOOST_CHECK(!pos::IsPegTx(CTransaction{bond}));
    pos::Unbond unbond;
    unbond.pubkey = uint256::ONE;
    CMutableTransaction unbonding{plain};
    unbonding.vout.emplace_back(0, pos::UnbondScript(unbond));
    BOOST_CHECK(!pos::IsPegTx(CTransaction{unbonding}));

    // A block's fees split by kind: peg fees whole to the bidder, the others by S.
    std::vector<CTransactionRef> vtx{MakeTransactionRef(CMutableTransaction{}), MakeTransactionRef(plain), MakeTransactionRef(withdrawal),
                                     MakeTransactionRef(refund), MakeTransactionRef(bond)};
    const std::vector<CAmount> fees{0, 1000, 700, 300, 501};
    const pos::FeeSplit split{pos::SplitFees(vtx, fees, /*fees_without_coinbase=*/false, 90)};
    BOOST_CHECK_EQUAL(split.peg, 1000);
    BOOST_CHECK_EQUAL(split.ordinary, 1501);
    BOOST_CHECK_EQUAL(split.bidder_ordinary, 500);
    BOOST_CHECK_EQUAL(split.validators, 1001);
    BOOST_CHECK_EQUAL(split.Bidder(), 1500);
    // The same with the block template's list of fees (no entry for the coinbase).
    const std::vector<CAmount> template_fees{1000, 700, 300, 501};
    BOOST_CHECK(pos::SplitFees(vtx, template_fees, /*fees_without_coinbase=*/true, 90).validators == split.validators);

    // The markers.
    BOOST_CHECK(pos::IsPoolScript(pos::PoolScript()));
    BOOST_CHECK(!pos::IsPoolScript(pos::PotScript()));
    BOOST_CHECK(!pos::IsRewardPayee(pos::PoolScript()));
    BOOST_CHECK(pos::PoolScript().IsUnspendable());
    for (const uint32_t count : {0U, 1U, 100U, 0xFFFFFFFFU}) BOOST_CHECK(pos::ParsePoolSharesMarker(pos::PoolSharesMarker(count)) == count);
    BOOST_CHECK(!pos::ParsePoolSharesMarker(pos::PoolScript()));
    BOOST_CHECK(!pos::ParsePoolSharesMarker(dest));
}

BOOST_AUTO_TEST_CASE(fee_split_pool_distribution)
{
    const auto member{[](uint8_t b, int64_t weight, bool excluded = false, bool payee = true) {
        return pos::PoolMember{.pubkey = uint256{b}, .weight = weight, .excluded = excluded, .payee = payee ? std::optional{Wpkh(b)} : std::nullopt};
    }};
    // By stake.
    {
        const auto paid{pos::DistributePool(600, {member(1, 1), member(2, 2), member(3, 3)})};
        BOOST_REQUIRE_EQUAL(paid.shares.size(), 3U);
        BOOST_CHECK(paid.shares[0] == CTxOut(100, Wpkh(1)));
        BOOST_CHECK(paid.shares[1] == CTxOut(200, Wpkh(2)));
        BOOST_CHECK(paid.shares[2] == CTxOut(300, Wpkh(3)));
        BOOST_CHECK_EQUAL(paid.remainder, 0);
        BOOST_CHECK_EQUAL(paid.to_reserve, 0);
    }
    // Jailed or banned: no share; the others share it all.
    {
        const auto paid{pos::DistributePool(600, {member(1, 1), member(2, 2, /*excluded=*/true), member(3, 3)})};
        BOOST_REQUIRE_EQUAL(paid.shares.size(), 2U);
        BOOST_CHECK(paid.shares[0] == CTxOut(150, Wpkh(1)));
        BOOST_CHECK(paid.shares[1] == CTxOut(450, Wpkh(3)));
        BOOST_CHECK_EQUAL(paid.remainder, 0);
    }
    // Rounding down; the remainder carried.
    {
        const auto paid{pos::DistributePool(10, {member(1, 1), member(2, 1), member(3, 1)})};
        BOOST_REQUIRE_EQUAL(paid.shares.size(), 3U);
        for (const CTxOut& share : paid.shares) BOOST_CHECK_EQUAL(share.nValue, 3);
        BOOST_CHECK_EQUAL(paid.remainder, 1);
    }
    // A share of 0 is not paid (it stays in the remainder).
    {
        const auto paid{pos::DistributePool(5, {member(1, 1), member(2, 1000)})};
        BOOST_REQUIRE_EQUAL(paid.shares.size(), 1U);
        BOOST_CHECK(paid.shares[0] == CTxOut(4, Wpkh(2)));
        BOOST_CHECK_EQUAL(paid.remainder, 1);
    }
    // Without a payee (a genesis validator that never bonded): to the reserve.
    {
        const auto paid{pos::DistributePool(90, {member(1, 1, false, /*payee=*/false), member(2, 2)})};
        BOOST_REQUIRE_EQUAL(paid.shares.size(), 1U);
        BOOST_CHECK(paid.shares[0] == CTxOut(60, Wpkh(2)));
        BOOST_CHECK_EQUAL(paid.to_reserve, 30);
        BOOST_CHECK_EQUAL(paid.remainder, 0);
    }
    // Nobody to share it: all carried.
    BOOST_CHECK_EQUAL(pos::DistributePool(77, {member(1, 5, /*excluded=*/true)}).remainder, 77);
    BOOST_CHECK_EQUAL(pos::DistributePool(77, {}).remainder, 77);
    BOOST_CHECK(pos::DistributePool(0, {member(1, 1)}).shares.empty());
    // Large stakes and pools: exact (256-bit products).
    {
        const auto paid{pos::DistributePool(MAX_MONEY, {member(1, MAX_MONEY / 2), member(2, MAX_MONEY / 2)})};
        BOOST_REQUIRE_EQUAL(paid.shares.size(), 2U);
        BOOST_CHECK_EQUAL(paid.shares[0].nValue + paid.shares[1].nValue + paid.remainder, MAX_MONEY);
    }

    // Wallets tell the shares by their marker: after the pot, the rewards; before the refunds.
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vout = {CTxOut{5, Wpkh(1)}, CTxOut{7, Wpkh(1)}, CTxOut{0, pos::PoolSharesMarker(2)}, CTxOut{3, Wpkh(2)}, CTxOut{4, Wpkh(1)}, CTxOut{9, Wpkh(3)}};
    CBlock block;
    block.vtx.push_back(MakeTransactionRef(coinbase));
    const auto roles{pos::CoinbaseRoles(block, {})};
    const std::vector<pos::CoinbaseRole> expected{pos::CoinbaseRole::OTHER, pos::CoinbaseRole::ANCHOR, pos::CoinbaseRole::OTHER,
                                                  pos::CoinbaseRole::VALIDATOR, pos::CoinbaseRole::VALIDATOR, pos::CoinbaseRole::OTHER};
    BOOST_CHECK(roles == expected);
}

// From fee_split_height: the producer keeps nothing; the bidder's share of ordinary fees and all the
// fees of peg transactions go to the pot, the validators' share to the pool, shared at the end of the
// epoch (genesis validators, which have no bond's owner: to the reserve; the rounding carried).
BOOST_FIXTURE_TEST_CASE(fee_split_blocks_of_stake, FeeSplitSetup)
{
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    // A mainchain of 90-second blocks: b = 1/3.
    Spaced(90, 12);
    const int first{NextEpochWithBmm()};
    const int64_t epoch{pos::EpochOf(base_slot + first, Pos())};
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return Hooks().FeeSplitSpacing(*Hooks().FeeSplitMainHeight(true, base_slot + first + 2, -1))), 90);
    const CMutableTransaction plain{Pay(coin, spent, {})};
    Accept(plain);
    const CBlock block{PosBlock(first + 2)};
    BOOST_REQUIRE_EQUAL(block.vtx.size(), 2U);
    const CTransaction& coinbase{*block.vtx[0]};
    BOOST_CHECK_EQUAL(coinbase.vout[0].nValue, 0);
    BOOST_CHECK_EQUAL(OutputsOf(coinbase, pos::IsPotScript), TX_FEE / 3);
    BOOST_CHECK_EQUAL(OutputsOf(coinbase, pos::IsPoolScript), TX_FEE - TX_FEE / 3);
    // The producer may keep nothing, and the split is exact.
    {
        CBlock greedy{block};
        SetCoinbase(greedy, [](CMutableTransaction& cb) { cb.vout[0].nValue += 1; for (auto& out : cb.vout) if (pos::IsPoolScript(out.scriptPubKey)) out.nValue -= 1; });
        Resign(greedy);
        BOOST_CHECK_EQUAL(Check(greedy), "bad-pos-producer-share");
        CBlock skewed{block};
        SetCoinbase(skewed, [](CMutableTransaction& cb) {
            for (auto& out : cb.vout) {
                if (pos::IsPoolScript(out.scriptPubKey)) out.nValue -= 1;
                if (pos::IsPotScript(out.scriptPubKey)) out.nValue += 1;
            }
        });
        Resign(skewed);
        BOOST_CHECK_EQUAL(Check(skewed), "bad-pos-fee-split");
        CBlock modest{block};
        SetCoinbase(modest, [](CMutableTransaction& cb) { for (auto& out : cb.vout) if (pos::IsPoolScript(out.scriptPubKey)) out.nValue -= 1; });
        Resign(modest);
        BOOST_CHECK_EQUAL(Check(modest), "bad-pos-fee-split");
        CBlock two{block};
        SetCoinbase(two, [](CMutableTransaction& cb) { cb.vout.emplace_back(0, pos::PoolScript()); });
        Resign(two);
        BOOST_CHECK_EQUAL(Check(two), "bad-pos-pool-multiple");
        BOOST_CHECK(Hooks().RecordIndependent("bad-pos-pool-multiple"));
        BOOST_CHECK(!Hooks().RecordIndependent("bad-pos-fee-split"));
    }
    Take(block);
    BOOST_CHECK_EQUAL(PoolOf(epoch), TX_FEE - TX_FEE / 3);
    BOOST_CHECK_EQUAL(Fast().pot_accrued, TX_FEE / 3);

    // A withdrawal: its whole fee to the pot.
    const CMutableTransaction withdrawal{Pay(coin, spent, {CTxOut{COIN / 4, sidechain::WithdrawalScript(1000, key.GetPubKey().GetID(), script)}})};
    Accept(withdrawal);
    const CBlock peg{PosBlock(first + 3)};
    BOOST_REQUIRE_EQUAL(peg.vtx.size(), 2U);
    BOOST_CHECK_EQUAL(OutputsOf(*peg.vtx[0], pos::IsPotScript), TX_FEE);
    BOOST_CHECK_EQUAL(OutputsOf(*peg.vtx[0], pos::IsPoolScript), 0);
    Take(peg);
    BOOST_CHECK_EQUAL(PoolOf(epoch), TX_FEE - TX_FEE / 3);
    BOOST_CHECK_EQUAL(Fast().pot_accrued, TX_FEE / 3 + TX_FEE);

    // An anchor in the epoch pays the pot, not the pool (the epoch is not over).
    const pos::StakeState before{Stake()};
    Take(PosBlock(first + 4, Anchor(Ticket(Tip()->GetBlockHash(), script))));
    BOOST_CHECK_EQUAL(PoolOf(epoch), TX_FEE - TX_FEE / 3);
    BOOST_CHECK_EQUAL(Fast().pot_paid, TX_FEE / 3 + TX_FEE);
    // The first anchor after the epoch: the pool shared by stake (1, 1, 1: 2222 each, to the reserve;
    // the satoshi left to the next epoch's pool).
    const int next{first + static_cast<int>(Pos().slots_per_epoch) + 1};
    // (A mainchain block after the next epoch starts: its seed known.)
    Mock(pos::SlotTime(base_slot + next - 1, Pos()) + 1);
    Main();
    Take(PosBlock(next - 1));
    Mock(pos::SlotTime(base_slot + next, Pos()) + 1);
    Take(PosBlock(next, Anchor(Ticket(Tip()->GetBlockHash(), script))));
    const CAmount pool{TX_FEE - TX_FEE / 3};
    BOOST_CHECK_EQUAL(PoolOf(epoch), 0);
    BOOST_CHECK_EQUAL(PoolOf(epoch + 1), pool % 3);
    // (The reserve paid out its share with each pot meanwhile: none was there before.)
    BOOST_CHECK_EQUAL(before.pot_reserve, 0);
    BOOST_CHECK_EQUAL(Stake().pot_reserve, pool - pool % 3);
}

// Before fee_split_height the old rule holds exactly; from it, the new one.
BOOST_FIXTURE_TEST_CASE(fee_split_activation, FeeSplitLaterSetup)
{
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    StartPos();
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, 2);
    Accept(Pay(coin, spent, {}));
    const CBlock old_rule{PosBlock(1)};
    BOOST_CHECK_EQUAL(old_rule.vtx[0]->vout[0].nValue, TX_FEE / 2);
    BOOST_CHECK_EQUAL(OutputsOf(*old_rule.vtx[0], pos::IsPotScript), TX_FEE / 2);
    BOOST_CHECK_EQUAL(OutputsOf(*old_rule.vtx[0], pos::IsPoolScript), 0);
    // (A pool output before the height is only an output that burns its coins.)
    {
        CBlock burn{old_rule};
        SetCoinbase(burn, [](CMutableTransaction& cb) { cb.vout[0].nValue -= 1; cb.vout.emplace_back(1, pos::PoolScript()); cb.vout.emplace_back(0, pos::PoolScript()); });
        Resign(burn);
        BOOST_CHECK_EQUAL(Check(burn), "");
    }
    Take(old_rule);
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return pos::PoolTotal(Chain().SideState().View())), 0);
    Accept(Pay(coin, spent, {}));
    // (Slot 2 starts an epoch: a mainchain block after it, for its seed.)
    Mock(pos::SlotTime(base_slot + 2, Pos()) + 1);
    Main();
    const CBlock new_rule{PosBlock(2)};
    BOOST_CHECK_EQUAL(new_rule.vtx[0]->vout[0].nValue, 0);
    // (S of a record that starts as the test does: 60 or less, 50/50.)
    BOOST_CHECK_EQUAL(OutputsOf(*new_rule.vtx[0], pos::IsPotScript), TX_FEE / 2);
    BOOST_CHECK_EQUAL(OutputsOf(*new_rule.vtx[0], pos::IsPoolScript), TX_FEE / 2);
    {
        CBlock old_way{new_rule};
        SetCoinbase(old_way, [](CMutableTransaction& cb) { cb.vout[0].nValue = TX_FEE / 2; std::erase_if(cb.vout, [](const CTxOut& out) { return pos::IsPoolScript(out.scriptPubKey); }); });
        Resign(old_way);
        BOOST_CHECK_EQUAL(Check(old_way), "bad-pos-producer-share");
    }
    Take(new_rule);
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return pos::PoolTotal(Chain().SideState().View())), TX_FEE / 2);
}

// A slow mainchain (S = 600: the floor, 10 %), then a fast one (S = 30: 50 %), for blocks of stake and BMM
// blocks; a BMM block's pool output is checked too (its miner keeps the bidder's share and the peg fees).
BOOST_FIXTURE_TEST_CASE(fee_split_spacing, FeeSplitSetup)
{
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    Spaced(600, 12);
    int first{NextEpochWithBmm()};
    Accept(Pay(coin, spent, {}));
    CBlock block{PosBlock(first + 2)};
    BOOST_CHECK_EQUAL(OutputsOf(*block.vtx[0], pos::IsPotScript), TX_FEE / 10);
    BOOST_CHECK_EQUAL(OutputsOf(*block.vtx[0], pos::IsPoolScript), TX_FEE - TX_FEE / 10);
    Take(block);

    // A BMM block made now takes the mainchain's tip: S of the slow blocks (with the one after the epoch
    // started), the floor. Its pool output is checked against its fees.
    CMutableTransaction plain;
    plain.vout.emplace_back(COIN, script);
    std::vector<CTransactionRef> vtx{MakeTransactionRef(CMutableTransaction{}), MakeTransactionRef(plain)};
    CBlock bmm;
    bmm.vtx = vtx;
    std::vector<CTxOut> outputs;
    const CAmount taken{Hooks().MergeMinedFeeOutputs(bmm, Tip()->nHeight + 1, Record().Height(), {TX_FEE}, outputs)};
    BOOST_CHECK_EQUAL(taken, TX_FEE - TX_FEE / 10);
    BOOST_REQUIRE_EQUAL(outputs.size(), 1U);
    BOOST_CHECK(outputs[0] == CTxOut(TX_FEE - TX_FEE / 10, pos::PoolScript()));
    // (Before the height: nothing.)
    outputs.clear();
    BOOST_CHECK_EQUAL(Hooks().MergeMinedFeeOutputs(bmm, -1, Record().Height(), {TX_FEE}, outputs), 0);
    BOOST_CHECK(outputs.empty());

    // A fast mainchain: 50/50 again once the median moves.
    Spaced(30, 12);
    first = NextEpochWithBmm();
    Accept(Pay(coin, spent, {}));
    block = PosBlock(first + 2);
    BOOST_CHECK_EQUAL(OutputsOf(*block.vtx[0], pos::IsPotScript), TX_FEE / 2);
    BOOST_CHECK_EQUAL(OutputsOf(*block.vtx[0], pos::IsPoolScript), TX_FEE / 2);
    Take(block);
}

// Before fee_curve2_height the first curve holds exactly (S = 600: 10 %), from it the second (20 %), for
// blocks of stake and BMM blocks; a block of either side's split is refused on the other.
BOOST_FIXTURE_TEST_CASE(fee_curve2_activation, FeeCurve2LaterSetup)
{
    COutPoint coin;
    CTxOut spent{Fund(COIN, coin)};
    Spaced(600, 12);
    const int first{NextEpochWithBmm()};
    BOOST_REQUIRE_LT(Tip()->nHeight + 1, FEE_CURVE2_TEST_HEIGHT);
    BOOST_CHECK(Hooks().FeeCurveAt(FEE_CURVE2_TEST_HEIGHT - 1) == pos::FeeCurve::FIRST);
    BOOST_CHECK(Hooks().FeeCurveAt(FEE_CURVE2_TEST_HEIGHT) == pos::FeeCurve::LINEAR);

    // A BMM block: the curve of its height.
    CMutableTransaction plain;
    plain.vout.emplace_back(COIN, script);
    CBlock bmm;
    bmm.vtx = {MakeTransactionRef(CMutableTransaction{}), MakeTransactionRef(plain)};
    std::vector<CTxOut> outputs;
    BOOST_CHECK_EQUAL(Hooks().MergeMinedFeeOutputs(bmm, FEE_CURVE2_TEST_HEIGHT - 1, Record().Height(), {TX_FEE}, outputs), TX_FEE - TX_FEE / 10);
    outputs.clear();
    BOOST_CHECK_EQUAL(Hooks().MergeMinedFeeOutputs(bmm, FEE_CURVE2_TEST_HEIGHT, Record().Height(), {TX_FEE}, outputs), TX_FEE - TX_FEE / 5);

    // Blocks of stake on both sides of the height.
    bool before{false}, after{false};
    for (int i{2}; !after; ++i) {
        BOOST_REQUIRE_LT(i, 20);
        Accept(Pay(coin, spent, {}));
        const CBlock block{PosBlock(first + i)};
        const int height{Tip()->nHeight + 1};
        const bool curve2{height >= FEE_CURVE2_TEST_HEIGHT};
        const CAmount bidder{curve2 ? TX_FEE / 5 : TX_FEE / 10};
        BOOST_CHECK_EQUAL(OutputsOf(*block.vtx[0], pos::IsPotScript), bidder);
        BOOST_CHECK_EQUAL(OutputsOf(*block.vtx[0], pos::IsPoolScript), TX_FEE - bidder);
        // The other curve's split: refused.
        const CAmount other{curve2 ? TX_FEE / 10 : TX_FEE / 5};
        CBlock wrong{block};
        SetCoinbase(wrong, [&](CMutableTransaction& cb) {
            for (auto& out : cb.vout) {
                if (pos::IsPotScript(out.scriptPubKey)) out.nValue = other;
                if (pos::IsPoolScript(out.scriptPubKey)) out.nValue = TX_FEE - other;
            }
        });
        Resign(wrong);
        BOOST_CHECK_EQUAL(Check(wrong), "bad-pos-fee-split");
        (curve2 ? after : before) = true;
        Take(block);
    }
    BOOST_CHECK(before);
}

BOOST_AUTO_TEST_SUITE_END()
