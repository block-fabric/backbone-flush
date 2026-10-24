// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/chainparams.h>
#include <kernel/testnet_activation.h>

#include <arith_uint256.h>

#include <chainparamsseeds.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <crypto/hex_base.h>
#include <hash.h>
#include <kernel/messagestartchars.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/verify_flags.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <util/log.h>
#include <util/strencodings.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <span>
#include <utility>

using namespace util::hex_literals;

static CBlock CreateGenesisBlock(const char* pszTimestamp, const CScript& genesisOutputScript, uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    CMutableTransaction txNew;
    txNew.version = 1;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    txNew.vin[0].scriptSig = CScript() << 486604799 << CScriptNum(4) << std::vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    txNew.vout[0].nValue = genesisReward;
    txNew.vout[0].scriptPubKey = genesisOutputScript;

    CBlock genesis;
    genesis.nTime    = nTime;
    genesis.nBits    = nBits;
    genesis.nNonce   = nNonce;
    genesis.nVersion = nVersion;
    genesis.vtx.push_back(MakeTransactionRef(std::move(txNew)));
    genesis.hashPrevBlock.SetNull();
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

/**
 * Build the genesis block. Note that the output of its generation
 * transaction cannot be spent since it did not originally exist in the
 * database.
 *
 * CBlock(hash=000000000019d6, ver=1, hashPrevBlock=00000000000000, hashMerkleRoot=4a5e1e, nTime=1231006505, nBits=1d00ffff, nNonce=2083236893, vtx=1)
 *   CTransaction(hash=4a5e1e, ver=1, vin.size=1, vout.size=1, nLockTime=0)
 *     CTxIn(COutPoint(000000, -1), coinbase 04ffff001d0104455468652054696d65732030332f4a616e2f32303039204368616e63656c6c6f72206f6e206272696e6b206f66207365636f6e64206261696c6f757420666f722062616e6b73)
 *     CTxOut(nValue=50.00000000, scriptPubKey=0x5F1DF16B2B704C8A578D0B)
 *   vMerkleTree: 4a5e1e
 */
static CBlock CreateGenesisBlock(uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    const char* pszTimestamp = "The Times 03/Jan/2009 Chancellor on brink of second bailout for banks";
    const CScript genesisOutputScript = CScript() << "04678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef38c4f35504e51ec112de5c384df7ba0b8d578a4c702b6bf11d5f"_hex << OP_CHECKSIG;
    return CreateGenesisBlock(pszTimestamp, genesisOutputScript, nTime, nNonce, nBits, nVersion, genesisReward);
}

void CChainParams::MakeSidechain(const SidechainIdentity& identity)
{
    consensus.sidechain.enabled = true;
    consensus.sidechain.slot = identity.slot;
    // A sidechain has no sidechains of its own.
    consensus.drivechain.max_sidechains = 0;
    // The mainchain makes no rule about how the blocks of a sidechain are signed.
    consensus.signet_blocks = false;
    consensus.signet_challenge.clear();

    // The chain is secured by the work of the mainchain. The proof of work of
    // its own blocks is kept as a formality that takes a couple of hashes, so
    // that the code inherited from the mainchain needs no changes.
    consensus.powLimit = uint256{"7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
    consensus.fPowNoRetargeting = true;
    consensus.fPowAllowMinDifficultyBlocks = false;
    consensus.enforce_BIP94 = false;
    consensus.nMinimumChainWork = uint256{};
    consensus.defaultAssumeValid = uint256{};

    pchMessageStart = identity.message_start;
    nDefaultPort = identity.default_port;
    // Slot 0 of the blocks of stake starts with the first block.
    consensus.pos.genesis_time = identity.genesis_time;
    bech32_hrp = identity.bech32_hrp;

    // The first block. It creates no coins: all coins come from the mainchain.
    genesis = CreateGenesisBlock(identity.genesis_message, CScript() << OP_RETURN, identity.genesis_time, /*nNonce=*/0, 0x207fffff, 1, 0);
    arith_uint256 target;
    target.SetCompact(genesis.nBits);
    while (UintToArith256(genesis.GetHash()) > target) ++genesis.nNonce;
    consensus.hashGenesisBlock = genesis.GetHash();

    vFixedSeeds.clear();
    vSeeds.clear();
    m_assumeutxo_data = {};
    chainTxData = ChainTxData{.nTime = identity.genesis_time, .tx_count = 1, .dTxRate = 0};
}

void CChainParams::ApplyDeploymentOptions(const DeploymentOptions& opts)
{
    for (const auto& [dep, height] : opts.activation_heights) {
        switch (dep) {
        case Consensus::BuriedDeployment::DEPLOYMENT_SEGWIT:
            consensus.SegwitHeight = int{height};
            break;
        case Consensus::BuriedDeployment::DEPLOYMENT_HEIGHTINCB:
            consensus.BIP34Height = int{height};
            break;
        case Consensus::BuriedDeployment::DEPLOYMENT_DERSIG:
            consensus.BIP66Height = int{height};
            break;
        case Consensus::BuriedDeployment::DEPLOYMENT_CLTV:
            consensus.BIP65Height = int{height};
            break;
        case Consensus::BuriedDeployment::DEPLOYMENT_CSV:
            consensus.CSVHeight = int{height};
            break;
        }
    }

    for (const auto& [deployment_pos, version_bits_params] : opts.version_bits_parameters) {
        consensus.vDeployments[deployment_pos].nStartTime = version_bits_params.start_time;
        consensus.vDeployments[deployment_pos].nTimeout = version_bits_params.timeout;
        consensus.vDeployments[deployment_pos].min_activation_height = version_bits_params.min_activation_height;
    }
}

/**
 * Chains main network.
 */
class CMainParams : public CChainParams {
public:
    CMainParams(const MainNetOptions& opts) {
        m_chain_type = ChainType::MAIN;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 210000; // ~146 days at one-minute blocks
        // New chain: every buried deployment is active from the first block.
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 0;
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        consensus.nPowTargetSpacing = 60; // one-minute blocks
        consensus.nPowTargetTimespan = 2016 * consensus.nPowTargetSpacing; // only sizes version bits periods; difficulty uses aserti3
        consensus.asert_half_life = 2 * 60 * 60; // two hours, i.e. 120 blocks
        consensus.asert_anchor_height = 2016; // blocks before it are mined at the proof of work limit
        // FastBlocks' blocks: up to 1.5 million weight units, 400,000 of them for
        // transactions (as many as Bitcoin's largest standard transaction) and the
        // rest for the coinbase with its payouts. A block comes far more often than
        // the mainchain's, so they are kept small (user decision, 2026-10-16).
        consensus.max_block_weight = FASTBLOCKS_MAX_BLOCK_WEIGHT;
        consensus.max_block_tx_weight = FASTBLOCKS_MAX_BLOCK_TX_WEIGHT;
        consensus.coinbase_maturity = 360; // six hours
        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.enforce_BIP94 = false; // aserti3 is not subject to the timewarp attack
        consensus.fPowNoRetargeting = false;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 1815; // 90%;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 2016;

        ApplyDeploymentOptions(opts.dep_opts);

        consensus.nMinimumChainWork = uint256{};
        consensus.defaultAssumeValid = uint256{};

        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 32-bit integer with any alignment.
         */
        pchMessageStart[0] = 0xc7;
        pchMessageStart[1] = 0xe8;
        pchMessageStart[2] = 0xa9;
        pchMessageStart[3] = 0xd5;
        nDefaultPort = 9555;
        nPruneAfterHeight = 100000;
        m_assumed_blockchain_size = 1;
        m_assumed_chain_state_size = 1;

        // The genesis reward pays an unspendable output: there is no premine.
        genesis = CreateGenesisBlock("Chains 01/Oct/2026 One chain to mine them all: drivechains for everyone", CScript() << OP_RETURN, 1790879567, 1864237343, 0x1d00ffff, 1, 50 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"00000000942856f7f35f8af81e0bcade5750224d12b81dfb66cd348c52b7cb4a"});
        assert(genesis.hashMerkleRoot == uint256{"597434e3ce8c541d1b6b739a1f2477b8785cb4b803084ca628c06cd1c2c9b5af"});

        // No DNS or fixed seeds yet; peers are added with -addnode.
        vFixedSeeds.clear();
        vSeeds.clear();

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,28);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,88);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,156);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x88, 0xB2, 0x1E};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x88, 0xAD, 0xE4};

        bech32_hrp = "chn";

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        m_assumeutxo_data = {};

        chainTxData = ChainTxData{
            .nTime    = 1790879567,
            .tx_count = 1,
            .dTxRate  = 0,
        };

        // Bitcoin's values, to be regenerated with headerssync-params.py once the chain has history.
        m_headers_sync_params = HeadersSyncParams{
            .commitment_period = 649,
            .redownload_buffer_size = 15400,
        };

        // This chain is a sidechain of the Chains mainchain.
        // Layers: FastBlocks' blocks of stake with BitAssets' assets (doc/layers.md).
        MakeSidechain({.slot = 7, .genesis_message = "Layers: assets on a fast chain", .genesis_time = 1792281600,
                       .message_start = {0x6c, 0x79, 0x72, 0x01}, .default_port = 9255, .bech32_hrp = "ly"});
        // Set when the slot activates on the mainchain: the height of the block that activated it.
        consensus.sidechain.main_activation_height = 0;
        consensus.pos.main_activation_height = consensus.sidechain.main_activation_height;
        consensus.sidechain.bitassets_reveal_depth = 6;
        consensus.sidechain.dex_height = 0; // Layers' DEX from the first block (a new network)
        consensus.sidechain.dex_curves_height = 0; // and its pool kinds
        // The fee split from the first block: the main network has not started (doc/fastblocks.md).
        consensus.pos.fee_split_height = 0;
        consensus.pos.fee_curve2_height = 0;
        // The logos of assets from the first block (the network is not launched).
        consensus.sidechain.logo_height = 0;
        // The search index's volume window: a day of 15-second slots (bitassets/index.h; not a rule).
        consensus.sidechain.asset_volume_window = 5760;
        consensus.sidechain.ticker_height = 0;
        // A tenth of the mainchain's withdrawal_min_score (64800); proposals within a day.
        consensus.sidechain.pending_min_score = 6480;
        consensus.sidechain.unproposed_expiry_blocks = 1440;
        consensus.coinbase_maturity = 0; // what a block pays (deposits, fees) can be spent in the next
        // Blocks of stake: a slot every 15 seconds, an epoch an hour (see doc/fastblocks.md). No
        // validators yet: until the chain has them, it is the sidechain template's, merged mined only.
        // Counts of slots are of the durations they had with 30-second slots (user decision, 2026-10-16).
        consensus.pos.slot_seconds = 15;
        consensus.pos.slots_per_epoch = 240;
        consensus.pos.min_active_validators = 4;
        consensus.pos.max_validators = 100;
        consensus.pos.max_unanchored_slots = 240; // an hour: PoS blocks halt after an hour without anchors
        consensus.pos.liveness_min_assigned = 20; // five minutes of slots led
        consensus.pos.stall_seconds = 1800;
        // Staking (design section 13): 1 CHN at least, about two days to unbond.
        consensus.pos.min_stake = 1 * COIN;
        consensus.pos.unbonding_anchors = 2880;
        consensus.pos.evidence_window_anchors = 1440;
        consensus.pos.seed_lookahead_seconds = 1200;
        consensus.pos.pot_reserve_anchors = 1440;
        // Bitcoin's heuristics (headers sync and block download timeouts, stale tips, verification
        // progress) count blocks by this: a block a slot. Version bits periods stay 2016 blocks.
        consensus.nPowTargetSpacing = consensus.pos.slot_seconds;
        consensus.nPowTargetTimespan = 2016 * consensus.nPowTargetSpacing;
    }
};

/**
 * Chains public test network (testnet).
 */
class CTestNetParams : public CChainParams {
public:
    CTestNetParams(const TestNetOptions& opts) {
        m_chain_type = ChainType::TESTNET;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 210000; // ~146 days at one-minute blocks
        // New chain: every buried deployment is active from the first block.
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 0;
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        consensus.nPowTargetSpacing = 60; // one-minute blocks
        consensus.nPowTargetTimespan = 2016 * consensus.nPowTargetSpacing; // only sizes version bits periods; difficulty uses aserti3
        consensus.asert_half_life = 2 * 60 * 60; // two hours, i.e. 120 blocks
        consensus.asert_anchor_height = 2016; // blocks before it are mined at the proof of work limit
        // FastBlocks' blocks: up to 1.5 million weight units, 400,000 of them for
        // transactions (as many as Bitcoin's largest standard transaction) and the
        // rest for the coinbase with its payouts. A block comes far more often than
        // the mainchain's, so they are kept small (user decision, 2026-10-16).
        consensus.max_block_weight = FASTBLOCKS_MAX_BLOCK_WEIGHT;
        consensus.max_block_tx_weight = FASTBLOCKS_MAX_BLOCK_TX_WEIGHT;
        consensus.coinbase_maturity = 360; // six hours
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.enforce_BIP94 = false; // aserti3 is not subject to the timewarp attack
        consensus.fPowNoRetargeting = false;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 1512; // 75%;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 2016;

        ApplyDeploymentOptions(opts.dep_opts);

        consensus.nMinimumChainWork = uint256{};
        consensus.defaultAssumeValid = uint256{};

        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 32-bit integer with any alignment.
         */
        pchMessageStart[0] = 0xc7;
        pchMessageStart[1] = 0xe8;
        pchMessageStart[2] = 0xb4;
        pchMessageStart[3] = 0xf3;
        nDefaultPort = 19555;
        nPruneAfterHeight = 1000;
        m_assumed_blockchain_size = 1;
        m_assumed_chain_state_size = 1;

        // The genesis reward pays an unspendable output: there is no premine.
        genesis = CreateGenesisBlock("Chains testnet 01/Oct/2026 One chain to mine them all: drivechains for everyone", CScript() << OP_RETURN, 1790879566, 447901697, 0x1d00ffff, 1, 50 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"00000000b9f0e845fb2e059b3a648408ed9544d1b954f153d8a77f483a982894"});
        assert(genesis.hashMerkleRoot == uint256{"a4d0cd8aadbb7b5f90668570e675964d7bea62da9cfe9d18a92a7d06bcab398b"});

        // No DNS or fixed seeds yet; peers are added with -addnode.
        vFixedSeeds.clear();
        vSeeds.clear();

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "tchn";

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        m_assumeutxo_data = {};

        chainTxData = ChainTxData{
            .nTime    = 1790879566,
            .tx_count = 1,
            .dTxRate  = 0,
        };

        // Bitcoin's values, to be regenerated with headerssync-params.py once the chain has history.
        m_headers_sync_params = HeadersSyncParams{
            .commitment_period = 649,
            .redownload_buffer_size = 15400,
        };

        // This chain is a sidechain of the Chains testnet.
        // A new genesis at each restart of the test network (audit 7, finding 3): a chain of an earlier
        // start shares no genesis with this one, so it is no branch of it (its blocks, bonds, unbonds
        // and signatures name another genesis). The restart of 2026-10-19 19:45 UTC, every rule from the first
        // block; was "Layers testnet 18/Oct/2026: assets on a fast chain", 1792337400 (Layers' first, of
        // 2026-10-18 15:30 UTC).
        MakeSidechain({.slot = 7, .genesis_message = "Layers testnet 19/Oct/2026: every rule from the first block", .genesis_time = 1792439100,
                       .message_start = {0x6c, 0x79, 0x72, 0x02}, .default_port = 19255, .bech32_hrp = "tly"});
        // The mainchain of the test network votes on a withdrawal bundle within 600 blocks.
        consensus.sidechain.bundle_retry_delay = 20;
        consensus.sidechain.bitassets_reveal_depth = 3;
        // The DEX, logos, tickers and display names from the first block (the test network restarted on
        // 19/Oct/2026).
        consensus.sidechain.dex_height = 0;
        consensus.sidechain.logo_height = 0;
        // The DEX's pool kinds from their height (a change of the rules of a running network: every node
        // upgraded before it; kernel/testnet_activation.h).
        consensus.sidechain.dex_curves_height = TESTNET_DEX_CURVES_HEIGHT;
        // The search index's volume window: a day of 15-second slots (bitassets/index.h; not a rule).
        consensus.sidechain.asset_volume_window = 5760;
        consensus.sidechain.ticker_height = 0;
        // The height of the mainchain block that activated the slot; nothing is left out at 0.
        consensus.sidechain.main_activation_height = TESTNET_MAIN_ACTIVATION_HEIGHT; // set once slot 7 activates on the test network (kernel/testnet_activation.h)
        consensus.pos.main_activation_height = consensus.sidechain.main_activation_height;
        // The fee split and the second fee curve from the first block (the test network restarted on 19/Oct/2026).
        consensus.pos.fee_split_height = 0;
        consensus.pos.fee_curve2_height = 0;
        // A tenth of the testnet mainchain's withdrawal_min_score (300); proposals within an hour.
        consensus.sidechain.pending_min_score = 30;
        consensus.sidechain.unproposed_expiry_blocks = 60;
        consensus.coinbase_maturity = 0; // what a block pays (deposits, fees) can be spent in the next
        // Blocks of stake: a slot every 15 seconds, an epoch 10 minutes. No validators yet. Counts of
        // slots are of the durations they had with 30-second slots (user decision, 2026-10-16).
        consensus.pos.slot_seconds = 15;
        consensus.pos.slots_per_epoch = 40;
        consensus.pos.min_active_validators = 3;
        consensus.pos.max_validators = 32;
        consensus.pos.max_unanchored_slots = 240; // an hour
        consensus.pos.liveness_min_assigned = 20; // five minutes of slots led
        consensus.pos.stall_seconds = 600;
        consensus.pos.min_stake = COIN / 100;
        consensus.pos.unbonding_anchors = 120;
        consensus.pos.evidence_window_anchors = 60;
        // An epoch (ten minutes): the seed block is mined after the stake of the epoch is fixed (median
        // times lag the clock by some six minutes), and is on record by the epoch's start unless the
        // mainchain found no block in the four minutes before it.
        consensus.pos.seed_lookahead_seconds = 600;
        consensus.pos.pot_reserve_anchors = 60;
        // Bitcoin's heuristics (headers sync and block download timeouts, stale tips, verification
        // progress) count blocks by this: a block a slot. Version bits periods stay 2016 blocks.
        consensus.nPowTargetSpacing = consensus.pos.slot_seconds;
        consensus.nPowTargetTimespan = 2016 * consensus.nPowTargetSpacing;
        consensus.pos.final_anchors = 3;
    }
};

/**
 * Signet: test network with an additional consensus parameter (see BIP325).
 */
class SigNetParams : public CChainParams {
public:
    explicit SigNetParams(const SigNetOptions& options)
    {
        std::vector<uint8_t> bin;
        vFixedSeeds.clear();
        vSeeds.clear();

        if (!options.challenge) {
            // The default Chains signet: blocks are signed with a single key.
            bin = "5121022eb69a435e256daf541a170b29fdedbd5ef5f6efbd66f67cfec24402a2b1f35551ae"_hex_v_u8;
        } else {
            bin = *options.challenge;
            LogInfo("Signet with challenge %s", HexStr(bin));
        }
        consensus.nMinimumChainWork = uint256{};
        consensus.defaultAssumeValid = uint256{};
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        if (options.seeds) {
            vSeeds = *options.seeds;
        }

        // Apart from the block signatures and the lower proof of work limit,
        // signet follows the rules of the main network.
        m_chain_type = ChainType::SIGNET;
        consensus.signet_blocks = true;
        consensus.signet_challenge.assign(bin.begin(), bin.end());
        consensus.nSubsidyHalvingInterval = 210000;
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 1;
        consensus.nPowTargetSpacing = 60; // one-minute blocks
        consensus.nPowTargetTimespan = 2016 * consensus.nPowTargetSpacing; // only sizes version bits periods; difficulty uses aserti3
        consensus.asert_half_life = 2 * 60 * 60; // two hours, i.e. 120 blocks
        consensus.asert_anchor_height = 2016; // blocks before it are mined at the proof of work limit
        consensus.max_block_weight = FASTBLOCKS_MAX_BLOCK_WEIGHT;
        consensus.max_block_tx_weight = FASTBLOCKS_MAX_BLOCK_TX_WEIGHT;
        consensus.coinbase_maturity = 360; // six hours
        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.enforce_BIP94 = false; // aserti3 is not subject to the timewarp attack
        consensus.fPowNoRetargeting = false;
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"00000377ae000000000000000000000000000000000000000000000000000000"};
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 1815; // 90%
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 2016;

        ApplyDeploymentOptions(options.dep_opts);

        // message start is defined as the first 4 bytes of the sha256d of the block script
        HashWriter h{};
        h << consensus.signet_challenge;
        uint256 hash = h.GetHash();
        std::copy_n(hash.begin(), 4, pchMessageStart.begin());

        nDefaultPort = 39555;
        nPruneAfterHeight = 1000;

        // The genesis reward pays an unspendable output: there is no premine.
        genesis = CreateGenesisBlock("Chains signet 01/Oct/2026 One chain to mine them all: drivechains for everyone", CScript() << OP_RETURN, 1790879568, 9736156, 0x1e0377ae, 1, 50 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"000001ca9ff240bbe9e7883dee99e91d1096c95e10cee82979a6cd1f08364073"});
        assert(genesis.hashMerkleRoot == uint256{"661cbd2140e027a506eb6ced73964fdfe58c7becce7e66ae5a9710a9266057e7"});

        m_assumeutxo_data = {};

        chainTxData = ChainTxData{
            .nTime    = 1790879568,
            .tx_count = 1,
            .dTxRate  = 0,
        };

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        // As on Bitcoin, signet shares its address format with testnet.
        bech32_hrp = "tchn";

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        // Bitcoin's values, to be regenerated with headerssync-params.py once the chain has history.
        m_headers_sync_params = HeadersSyncParams{
            .commitment_period = 629,
            .redownload_buffer_size = 15885,
        };

        // This chain is a sidechain of the Chains signet. Its own blocks are not signed.
        MakeSidechain({.slot = 7, .genesis_message = "Layers signet 18/Oct/2026: assets on a fast chain", .genesis_time = 1792281600,
                       .message_start = {0x6c, 0x79, 0x72, 0x03}, .default_port = 39255, .bech32_hrp = "tly"});
        consensus.sidechain.main_activation_height = 0;
        consensus.pos.main_activation_height = consensus.sidechain.main_activation_height;
        consensus.sidechain.bitassets_reveal_depth = 3;
        consensus.sidechain.dex_height = 0; // Layers' DEX from the first block
        consensus.sidechain.dex_curves_height = 0; // and its pool kinds
        consensus.pos.fee_split_height = 0; // not launched: from the first block
        consensus.pos.fee_curve2_height = 0;
        consensus.sidechain.logo_height = 0;
        // The search index's volume window: a day of 15-second slots (bitassets/index.h; not a rule).
        consensus.sidechain.asset_volume_window = 5760;
        consensus.sidechain.ticker_height = 0;
        // The Chains signet has the rules of its main network.
        consensus.sidechain.pending_min_score = 6480;
        consensus.sidechain.unproposed_expiry_blocks = 1440;
        consensus.coinbase_maturity = 0; // what a block pays (deposits, fees) can be spent in the next
        // Blocks of stake: a slot every 15 seconds, an epoch 10 minutes. No validators yet. Counts of
        // slots are of the durations they had with 30-second slots (user decision, 2026-10-16).
        consensus.pos.slot_seconds = 15;
        consensus.pos.slots_per_epoch = 40;
        consensus.pos.min_active_validators = 3;
        consensus.pos.max_validators = 32;
        consensus.pos.max_unanchored_slots = 240; // an hour
        consensus.pos.liveness_min_assigned = 20; // five minutes of slots led
        consensus.pos.stall_seconds = 600;
        consensus.pos.min_stake = COIN / 100;
        consensus.pos.unbonding_anchors = 120;
        consensus.pos.evidence_window_anchors = 60;
        // An epoch (ten minutes): the seed block is mined after the stake of the epoch is fixed (median
        // times lag the clock by some six minutes), and is on record by the epoch's start unless the
        // mainchain found no block in the four minutes before it.
        consensus.pos.seed_lookahead_seconds = 600;
        consensus.pos.pot_reserve_anchors = 60;
        // Bitcoin's heuristics (headers sync and block download timeouts, stale tips, verification
        // progress) count blocks by this: a block a slot. Version bits periods stay 2016 blocks.
        consensus.nPowTargetSpacing = consensus.pos.slot_seconds;
        consensus.nPowTargetTimespan = 2016 * consensus.nPowTargetSpacing;
        consensus.pos.final_anchors = 3;
    }
};

/**
 * Regression test: intended for private networks only. Has minimal difficulty to ensure that
 * blocks can be found instantly.
 */
class CRegTestParams : public CChainParams
{
public:
    explicit CRegTestParams(const RegTestOptions& opts)
    {
        m_chain_type = ChainType::REGTEST;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 150;
        // Short drivechain periods so that tests can run through them.
        consensus.drivechain.activation_period = 20;
        consensus.drivechain.activation_max_failures = 9;
        consensus.drivechain.replacement_period = 40;
        consensus.drivechain.withdrawal_period = 60;
        consensus.drivechain.withdrawal_min_score = 30;
        consensus.drivechain.upvote_expiry_blocks = 20;
        consensus.drivechain.unvoted_forget_blocks = 20;
        for (const auto& [name, value] : opts.drivechain_params) {
            auto& dc{consensus.drivechain};
            if (name == "max_sidechains") {
                dc.max_sidechains = value;
            } else if (name == "activation_period") {
                dc.activation_period = value;
            } else if (name == "activation_max_failures") {
                dc.activation_max_failures = value;
            } else if (name == "replacement_period") {
                dc.replacement_period = value;
            } else if (name == "withdrawal_period") {
                dc.withdrawal_period = value;
            } else if (name == "withdrawal_min_score") {
                dc.withdrawal_min_score = value;
            } else if (name == "max_pending_bundles") {
                dc.max_pending_bundles = value;
            } else if (name == "idle_expiry_blocks") {
                dc.idle_expiry_blocks = value;
            } else if (name == "upvote_expiry_blocks") {
                dc.upvote_expiry_blocks = value;
            } else if (name == "unvoted_forget_blocks") {
                dc.unvoted_forget_blocks = value;
            } else {
                throw std::runtime_error(strprintf("Invalid name (%s) for -testdrivechainparam=name@value.", name));
            }
        }
        consensus.BIP34Height = 1; // Always active unless overridden
        consensus.BIP34Hash = uint256();
        consensus.BIP65Height = 1;  // Always active unless overridden
        consensus.BIP66Height = 1;  // Always active unless overridden
        consensus.CSVHeight = 1;    // Always active unless overridden
        consensus.SegwitHeight = 0; // Always active unless overridden
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        consensus.nPowTargetTimespan = 24 * 60 * 60; // one day
        consensus.nPowTargetSpacing = 10 * 60;
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.enforce_BIP94 = opts.enforce_bip94;
        consensus.fPowNoRetargeting = true;
        // FastBlocks: the block weight of the other networks, so that tests run against the real limit.
        consensus.max_block_weight = FASTBLOCKS_MAX_BLOCK_WEIGHT;
        consensus.max_block_tx_weight = FASTBLOCKS_MAX_BLOCK_TX_WEIGHT;

        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 108; // 75%
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 144; // Faster than normal for regtest (144 instead of 2016)

        consensus.nMinimumChainWork = uint256{};
        consensus.defaultAssumeValid = uint256{};

        pchMessageStart[0] = 0x6c;
        pchMessageStart[1] = 0x79;
        pchMessageStart[2] = 0x72;
        pchMessageStart[3] = 0x04;
        nDefaultPort = 29255;
        nPruneAfterHeight = opts.fastprune ? 100 : 1000;

        // On request this chain is a sidechain, with what tests need to be quick.
        if (opts.sidechain_slot) {
            consensus.sidechain.enabled = true;
            consensus.sidechain.slot = *opts.sidechain_slot;
            consensus.sidechain.main_activation_height = opts.sidechain_activation_height;
            consensus.pos.main_activation_height = consensus.sidechain.main_activation_height;
            // The fee split never, unless a test asks (-testactivationheight=feesplit@<height>): the
            // tests of the rules before it run unchanged.
            consensus.pos.fee_split_height = opts.dep_opts.fee_split_height.value_or(std::numeric_limits<int>::max());
            // The second fee curve the same (-testactivationheight=feecurve2@<height>): the first until then.
            consensus.pos.fee_curve2_height = opts.dep_opts.fee_curve2_height.value_or(std::numeric_limits<int>::max());
            // The liveness fix from the first block, unless a test asks (-testactivationheight=jailfix@<height>).
            consensus.pos.jail_fix_height = opts.dep_opts.jail_fix_height.value_or(0);
            consensus.sidechain.bundle_retry_delay = 5;
            consensus.sidechain.bitassets_reveal_depth = 2;
            // Layers' DEX never, unless a test asks (-testactivationheight=dex@<height>): the tests of the
            // rules before it run unchanged.
            consensus.sidechain.dex_height = opts.dep_opts.dex_height.value_or(std::numeric_limits<int>::max());
            // The DEX's pool kinds with the DEX, unless a test asks (-testactivationheight=dexcurves@<height>).
            consensus.sidechain.dex_curves_height = opts.dep_opts.dex_curves_height.value_or(0);
            // Flush's fees of the DEX, as on its eCash networks (doc/layers.md "Fees of the DEX"): swaps 85 % providers /
            // 11 % security / 4 % treasury; launch curves 1.15 %, 50 % creator / 30 % treasury / 20 % security.
            consensus.sidechain.dex_fee_security_share = 110'000;
            consensus.sidechain.dex_fee_protocol_share = 40'000;
            consensus.sidechain.launch_fee = 11'500;
            consensus.sidechain.launch_creator_share = 500'000;
            consensus.sidechain.launch_protocol_share = 300'000;
            consensus.sidechain.launch_security_share = 200'000;
            // The logos of assets from the first block, unless a test asks (-testactivationheight=logo@<height>).
            consensus.sidechain.logo_height = opts.dep_opts.logo_height.value_or(0);
            // Tickers and display names only if a test asks (-testactivationheight=ticker@<height>):
            // the names of the tests of before are not tickers.
            if (opts.dep_opts.ticker_height) consensus.sidechain.ticker_height = *opts.dep_opts.ticker_height;
            // Launches with parameters from the first block, unless a test asks (-testactivationheight=launchparams@<height>).
            consensus.sidechain.launch_params_height = opts.dep_opts.launch_params_height.value_or(0);
            // Allocations of up to 10 % from the first block, unless a test asks (-testactivationheight=allocation10@<height>).
            consensus.sidechain.allocation10_height = opts.dep_opts.allocation10_height.value_or(0);
            // Smaller payouts with the DEX from the first block, unless a test asks (-testactivationheight=smallpayouts@<height>).
            consensus.sidechain.small_payout_height = opts.dep_opts.small_payout_height.value_or(0);
            // No treasury with the DEX from the first block, unless a test asks (-testactivationheight=notreasury@<height>).
            consensus.sidechain.no_treasury_height = opts.dep_opts.no_treasury_height.value_or(0);
            // A short window of volume for the search index, that tests can see expire.
            consensus.sidechain.asset_volume_window = 10;
            // A tenth of the regtest mainchain's withdrawal_min_score (30).
            consensus.sidechain.pending_min_score = 3;
            consensus.sidechain.unproposed_expiry_blocks = 20;
            if (opts.main_enforcer) {
                // The mainchain of Backbone's eCash regtest (backboned -testecash): the enforcer's rules, a bundle
                // paid with more than 5 votes, in 10 blocks (Thresholds::SHORT), and no single payout.
                consensus.sidechain.main_single_payout = false;
                consensus.sidechain.pending_min_score = 2;
                // Layers: the fee curves drawn for eCash's ten-minute blocks.
                consensus.pos.fee_target_spacing = 600;
                // The rules of the eCash networks: from the enforcer's events alone (SidechainParams::main_events_only).
                consensus.sidechain.main_events_only = true;
                consensus.sidechain.bundle_handoff_depth = 2;
            }
            // A sidechain has no sidechains of its own.
            consensus.drivechain.max_sidechains = 0;
            consensus.coinbase_maturity = 0;
            // Blocks of stake with the validators given (-posvalidators): a slot every second, so that
            // tests are quick. Without validators, the chain is merged mined only, as the template's.
            // (-posslotseconds: the slots of the real networks, for a soak test.)
            consensus.pos.slot_seconds = opts.pos_slot_seconds.value_or(1);
            consensus.pos.slots_per_epoch = 20;
            consensus.pos.min_active_validators = 1;
            consensus.pos.max_validators = 8;
            consensus.pos.max_unanchored_slots = 100;
            consensus.pos.stall_seconds = 30 * consensus.pos.slot_seconds;
            consensus.pos.final_anchors = opts.pos_final_anchors.value_or(1);
            // (-posuncheckedheaders and -posuncheckedbytes: smaller budgets, for a test of a branch
            // several budgets long.)
            consensus.pos.unchecked_headers_per_peer = opts.pos_unchecked_headers.value_or(2000);
            consensus.pos.unchecked_block_bytes_per_peer = opts.pos_unchecked_bytes.value_or(int64_t{32} << 20);
            // Inbound peers as outbound ones (the test framework's peers are inbound), unless a test
            // gives them less (-posuncheckedinbound); 8 of them hold unchecked headers at most.
            consensus.pos.unchecked_headers_per_inbound_peer = opts.pos_unchecked_inbound.value_or(2000);
            consensus.pos.unchecked_block_bytes_per_inbound_peer = opts.pos_unchecked_bytes.value_or(int64_t{32} << 20);
            consensus.pos.genesis_validators = opts.pos_validators;
            consensus.pos.min_stake = COIN / 10000;
            consensus.pos.unbonding_anchors = 20;
            consensus.pos.evidence_window_anchors = 10;
            // An hour: the median time of a regtest mainchain lags or leads the clock by minutes, as
            // tests mine it; the seed is then known for every slot (-posseedlookahead to change it).
            consensus.pos.seed_lookahead_seconds = opts.pos_seed_lookahead.value_or(3600);
            consensus.pos.pot_reserve_anchors = 5;
            // -testposparam=name@value: eCash's anchoring scaled down (a mainchain gap, a week of unbonding).
            for (const auto& [name, value] : opts.pos_params) {
                auto& pos{consensus.pos};
                if (name == "max_unanchored_slots" && value >= 1) {
                    pos.max_unanchored_slots = value;
                } else if (name == "stall_seconds") {
                    pos.stall_seconds = value;
                } else if (name == "slots_per_epoch" && value >= 1) {
                    pos.slots_per_epoch = value;
                } else if (name == "set_lead_seconds") {
                    pos.set_lead_seconds = value;
                } else if (name == "unbonding_anchors" && value >= 1) {
                    pos.unbonding_anchors = value;
                } else if (name == "evidence_window_anchors" && value >= 1) {
                    pos.evidence_window_anchors = value;
                } else if (name == "pot_reserve_anchors" && value >= 1) {
                    pos.pot_reserve_anchors = value;
                } else {
                    throw std::runtime_error(strprintf("Invalid name or value (%s@%d) for -testposparam=name@value.", name, value));
                }
            }
        }
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        ApplyDeploymentOptions(opts.dep_opts);

        genesis = CreateGenesisBlock(1296688602, 2, 0x207fffff, 1, 50 * COIN);
        consensus.pos.genesis_time = genesis.nTime;
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"0f9188f13cb7b2c71f2a335e3a4fc328bf5beb436012afca590b1a11466e2206"});
        assert(genesis.hashMerkleRoot == uint256{"4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b"});

        vFixedSeeds.clear(); //!< Regtest mode doesn't have any fixed seeds.
        vSeeds.clear();
        vSeeds.emplace_back("dummySeed.invalid.");

        fDefaultConsistencyChecks = true;
        m_is_mockable_chain = true;

        m_assumeutxo_data = {
            {   // For use by unit tests
                .height = 110,
                .hash_serialized = AssumeutxoHash{uint256{"86e9a1205b418b16dde3a18a78c730e30137e28466bda5dbf6b33ab8fc05447c"}},
                .m_chain_tx_count = 111,
                .blockhash = uint256{"135eec25a6fb277884e5824e7aa7d052c4868161c99a5122170b5266f86c273d"},
            },
            {
                // For use by fuzz target src/test/fuzz/utxo_snapshot.cpp
                .height = 200,
                .hash_serialized = AssumeutxoHash{uint256{"17dcc016d188d16068907cdeb38b75691a118d43053b8cd6a25969419381d13a"}},
                .m_chain_tx_count = 201,
                .blockhash = uint256{"385901ccbd69dff6bbd00065d01fb8a9e464dede7cfe0372443884f9b1dcf6b9"},
            },
            {
                // For use by test/functional/feature_assumeutxo.py and test/functional/tool_bitcoin_chainstate.py
                .height = 299,
                .hash_serialized = AssumeutxoHash{uint256{"106b2c56233e378a824cf0d5ff2be42ed32c72f1605c9be288d00942908a40ac"}},
                .m_chain_tx_count = 334,
                .blockhash = uint256{"0c552ced4721c249a389eb9b08cb8da261cd46f0e7b5f9d064d48f3113406853"},
            },
        };

        chainTxData = ChainTxData{
            .nTime = 0,
            .tx_count = 0,
            .dTxRate = 0.001, // Set a non-zero rate to make it testable
        };

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        // Its own, so that an address of another chain is not taken for one of this chain.
        bech32_hrp = "rsc";

        // Copied from Testnet4.
        m_headers_sync_params = HeadersSyncParams{
            .commitment_period = 275,
            .redownload_buffer_size = 7017, // 7017/275 = ~25.5 commitments
        };
    }
};

/**
 * The networks of this sidechain on Backbone, on eCash (ECX): a sidechain of backboned on -chain=ecash, ecashbeta
 * and ecashtest. The rules of the Chains networks of this chain (main, test), with an identity of their own and the
 * parameters tied to the blocks of the mainchain for eCash's: ten-minute blocks (one block of this chain per
 * mainchain block), and the drivechain rules of the bip300301_enforcer (a bundle is paid with more than 13,150
 * votes in 26,300 blocks; on the test network, more than 5 in 10), without single payout.
 */
class CEcashSidechainParams : public CChainParams {
public:
    CEcashSidechainParams(ChainType chain, const MainNetOptions& opts)
    {
        // The rules of the main network of this chain (the test network's for eCash's test network).
        std::unique_ptr<const CChainParams> base;
        if (chain == ChainType::ECASH_TESTNET) {
            base = CChainParams::TestNet(TestNetOptions{.dep_opts = opts.dep_opts});
        } else {
            base = CChainParams::Main(opts);
        }
        static_cast<CChainParams&>(*this) = *base;
        m_chain_type = chain;
        // One block of this chain per block of the mainchain, every ten minutes: the stall and sync timers
        // (net_processing, the warnings of a stale tip) and the estimates of the app count in those.
        consensus.nPowTargetSpacing = 10 * 60;
        consensus.nPowTargetTimespan = 2016 * consensus.nPowTargetSpacing;
        // Without single payout on the mainchain, this chain keeps it (SidechainParams::main_single_payout).
        consensus.sidechain.main_single_payout = false;
        // Rules that need only what the bip300301_enforcer reports of each block, which a node can follow through
        // an enforcer alone (-mainchaingrpc): new networks, nothing on them to fork (SidechainParams::main_events_only).
        consensus.sidechain.main_events_only = true;
        // Flush has not launched on any eCash network: the DEX's pool kinds and launches are its rules from the first
        // block, on every one (Layers' test network takes them at a height of its own: kernel/testnet_activation.h).
        consensus.sidechain.dex_curves_height = 0;
        // Launches graduate at 500 ECX (a consensus parameter; doc/layers.md "Launches").
        consensus.sidechain.launch_graduation = 500 * COIN;
        // The liveness fix (PosParams::jail_fix_height): never on the eCash networks until a height is decided.
        consensus.pos.jail_fix_height = std::numeric_limits<int>::max();
        // The fees of the DEX (doc/layers.md "Fees of the DEX"; decided 2026-10-21): swap fees 85 % to the liquidity
        // providers, 11 % to the chain's security (the BMM bidder and the validators, by the curve of the network fees),
        // 4 % to the protocol's treasury; a launch's curve charges 1.15 % of a trade: 50 % to its creator, 30 % to the
        // treasury, 20 % to the chain's security. Network fees: the bidder and the validators only (pos/feesplit.h).
        consensus.sidechain.dex_fee_security_share = 110'000;
        consensus.sidechain.dex_fee_protocol_share = 40'000;
        consensus.sidechain.launch_fee = 11'500;
        consensus.sidechain.launch_creator_share = 500'000;
        consensus.sidechain.launch_protocol_share = 300'000;
        consensus.sidechain.launch_security_share = 200'000;
        // Launches with parameters (supply, decimals, the curve's fee; an existing asset on a curve): never on the eCash
        // networks until a height is decided (a network that takes them sets it; doc/dex-math.md "Launch curve").
        consensus.sidechain.launch_params_height = std::numeric_limits<int>::max();
        // Allocations of up to 10 % of a launch's supply: never on the eCash networks until a height is decided (5 % at most until then).
        consensus.sidechain.allocation10_height = std::numeric_limits<int>::max();
        // Payouts of 0.00005 ECX at least (0.0005 until then): never on the eCash networks until a height is decided.
        consensus.sidechain.small_payout_height = std::numeric_limits<int>::max();
        // No treasury (swap fees 85 % providers / 15 % security, launches 80 % creator / 20 % security): never on the eCash
        // networks until a height is decided.
        consensus.sidechain.no_treasury_height = std::numeric_limits<int>::max();
        // A bundle is handed to an enforcer once its commitment is a day deep (144 blocks; two hours on the test
        // network): nothing next to the three months a bundle takes to be paid.
        consensus.sidechain.bundle_handoff_depth = chain == ChainType::ECASH_TESTNET ? 12 : 144;
        // Set when the slot activates on the mainchain: the height of the block that activated it.
        consensus.sidechain.main_activation_height = 0;
        // Ten-minute blocks: a block in which a new withdrawal can be taken back, and a link two blocks behind
        // the mainchain node's headers is syncing.
        consensus.sidechain.refund_grace_blocks = 1;
        consensus.sidechain.main_behind_margin = 2;
        switch (chain) {
        case ChainType::ECASH:
            // Flush on eCash: slot 46 (doc/ecash-sidechains.md). Its own identity on each eCash network (first
            // block, message start "flx" + 0x21/0x22/0x23, address prefixes flx, bflx, tflx); the ports of Layers on
            // eCash, which Flush (formerly Starbase) is.
            MakeSidechain({.slot = 46, .genesis_message = "Flush: tokens, assets and a DEX on a fast chain over eCash", .genesis_time = 1792540800,
                           .message_start = {0x66, 0x6c, 0x78, 0x21}, .default_port = 8255, .bech32_hrp = "flx"});
            break;
        case ChainType::ECASH_BETANET:
            if (opts.betanet_slot99) {
                // TEMPORARY (doc/photon-slot.md): Flush in betanet's slot 99, voted in for Photon (activated at 968998,
                // never deposited to). Its own first block, message start ("flx" + 0x99), ports (38994/38995) and
                // data folder (betanet-photon), so it never mixes with a slot-46 Flush or with Photon's own chain; the
                // betanet addresses (bflx), as the app knows them. The activation height pins this slot's sidechain:
                // the node stops if another one ever takes slot 99 (Follower::CheckSlot).
                MakeSidechain({.slot = 99, .genesis_message = "TEMPORARY Flush on eCash betanet slot 99 (Photon) 22/Oct/2026", .genesis_time = 1792627200,
                               .message_start = {0x66, 0x6c, 0x78, 0x99}, .default_port = 38995, .bech32_hrp = "bflx"});
                consensus.sidechain.main_activation_height = 968998;
                // Its bootstrap: three genesis validators (each key made on its own machine: the lab node server, the
                // server 5.78.75.193 and the UI PC), so blocks of stake start without 4 ECX bonded first; at least 3
                // of them active, and a bond of 0.1 ECX makes another validator (the pool server's, anyone's).
                // (x-only keys, in their byte order as -posvalidators takes them, not uint256's display order.)
                const auto key{[](std::string_view hex) { const auto bytes{ParseHex(hex)}; return uint256{std::span<const unsigned char>{bytes}}; }};
                consensus.pos.genesis_validators = {
                    {.pubkey = key("50be1e4d85e06cb7535fee19de6021d57e36d846db8bc78579b6a1799fb5b8ba"), .stake = 1}, // lab
                    {.pubkey = key("46e52427c3a472f9700624d7b6c91d87c7c4af184021b870cefb0ae50b752d83"), .stake = 1}, // 5.78
                    {.pubkey = key("3b39aea3d8f7b8fdb0f8887c91eb8a00802351f79b930a0caddce8d50d8b3ba0"), .stake = 1}, // PC
                };
                consensus.pos.min_active_validators = 3;
                consensus.pos.min_stake = COIN / 10;
                // Upgrade v2 (doc/photon-slot.md, "Upgrade v2"), from block SLOT99_V2_HEIGHT: launches with parameters and an
                // existing asset on a curve (the unified assets), and the liveness fix (which jailed all three genesis
                // validators at the chain's start). A height in the fingerprint: each node rebuilds its sidechain state once.
                static constexpr int SLOT99_V2_HEIGHT{3300};
                consensus.sidechain.launch_params_height = SLOT99_V2_HEIGHT;
                consensus.pos.jail_fix_height = SLOT99_V2_HEIGHT;
                // Upgrade v3 (doc/photon-slot.md, "Upgrade v3"), from block SLOT99_V3_HEIGHT: a launch's allocation up to
                // 10 % (5 % before). In the fingerprint too: each node of v3 rebuilds its sidechain state once.
                static constexpr int SLOT99_V3_HEIGHT{4400};
                consensus.sidechain.allocation10_height = SLOT99_V3_HEIGHT;
                // Upgrade v4 (doc/photon-slot.md, "Upgrade v4"), from block SLOT99_V4_HEIGHT: payouts and book orders of
                // 0.00005 ECX at least (0.0005 and 0.01 before), and no treasury. In the fingerprint too: each node of v4 rebuilds its state once.
                static constexpr int SLOT99_V4_HEIGHT{9000};
                consensus.sidechain.small_payout_height = SLOT99_V4_HEIGHT;
                // And no treasury (swap fees 89 % providers / 11 % security, launches 80 % creator / 20 % security; the
                // treasury's ECX paid with block SLOT99_V4_HEIGHT's fees).
                consensus.sidechain.no_treasury_height = SLOT99_V4_HEIGHT;
                m_temporary_notice = "Temporary: Flush on betanet slot 99 (Photon). A test chain in the slot voted in for Photon; it will be "
                                     "dropped when Flush gets its own slot (46). Do not keep value here.";
                break;
            }
            MakeSidechain({.slot = 46, .genesis_message = "Flush on eCash betanet 21/Oct/2026", .genesis_time = 1792540800,
                           .message_start = {0x66, 0x6c, 0x78, 0x22}, .default_port = 38255, .bech32_hrp = "bflx"});
            break;
        case ChainType::ECASH_TESTNET:
            MakeSidechain({.slot = 46, .genesis_message = "Flush on the eCash test network 21/Oct/2026", .genesis_time = 1792540800,
                           .message_start = {0x66, 0x6c, 0x78, 0x23}, .default_port = 48255, .bech32_hrp = "tflx"});
            break;
        default:
            assert(false);
        }
        if (chain == ChainType::ECASH_TESTNET) {
            // The enforcer's Thresholds::SHORT on eCash's test network: a bundle is paid with more than 5 votes,
            // and fails older than 10 blocks.
            consensus.sidechain.bundle_retry_delay = 3; // half an hour
            consensus.sidechain.pending_min_score = 2;
            consensus.sidechain.unproposed_expiry_blocks = 12; // two hours
        } else {
            // A tenth of the enforcer's threshold of a bundle (13,150 votes), as on the Chains networks (6,480 of
            // 64,800); retry and proposal delays as long in time as there: 15 blocks (2.5 hours, 144 one-minute
            // blocks on Chains) and 144 blocks (a day, 1,440 on Chains).
            consensus.sidechain.bundle_retry_delay = 15;
            consensus.sidechain.pending_min_score = 1315;
            consensus.sidechain.unproposed_expiry_blocks = 144;
        }

        // Layers: blocks of stake every slot (15 s), not ten minutes: Bitcoin's heuristics count a block a slot,
        // as on the Chains networks; and a withdrawal can be taken back for as many blocks as there, and the
        // retry delay is as there, in blocks of this chain.
        consensus.nPowTargetSpacing = consensus.pos.slot_seconds;
        consensus.nPowTargetTimespan = 2016 * consensus.nPowTargetSpacing;
        consensus.sidechain.refund_grace_blocks = base->GetConsensus().sidechain.refund_grace_blocks;
        consensus.sidechain.bundle_retry_delay = base->GetConsensus().sidechain.bundle_retry_delay;
        consensus.pos.main_activation_height = consensus.sidechain.main_activation_height;
        // The anchoring of the blocks of stake, for an anchor (a mainchain block) every ten minutes rather than
        // every minute (doc/ecash-sidechains.md, "Layers"): what counts anchors as long in time as on Chains,
        // what counts time with room for eCash's gaps between blocks (an hour without one is not rare) and
        // for its median time, which lags the clock by about 50 minutes (5 minutes on Chains).
        consensus.pos.fee_target_spacing = 600; // the bidder's share 50/50 at ten-minute blocks
        // Blocks of stake go on through a gap of the mainchain of up to six hours (user requirement), with room: no new
        // eCash block, so no anchor and no new seed. Three things bound how long, all set for it
        // (doc/ecash-sidechains.md, "A mainchain gap"):
        // - max_unanchored_slots: 8 hours of 15-second slots after the first block of stake no anchor covers (that
        //   block comes about a block before the gap: the last ticket anchors the chain up to it);
        // - seed_lookahead_seconds: an epoch has a leader once the mainchain record has a block whose median time
        //   is past its start less this; in a gap the median time stops about 50 minutes behind the last block, so
        //   9 hours lets epochs start up to about 8 hours after it (an hour, the earlier value, stopped blocks of
        //   stake 10 to 70 minutes into a gap);
        // - set_lead_seconds: as long, so that the validator set of an epoch is fixed before its seed's block can
        //   exist (no key or stake picked for a known seed). Bonds and jails take effect about ten hours later.
        // stall_seconds stays an hour: it compares the mainchain's median time with the last block of stake, which a
        // gap does not stall (BMM blocks carry transactions only when blocks of stake stop).
        consensus.pos.max_unanchored_slots = 1920;       // 8 hours of 15-second slots (an hour on Chains)
        consensus.pos.stall_seconds = 3600;              // BMM blocks carry transactions after an hour without blocks of stake (30 min)
        consensus.pos.seed_lookahead_seconds = 9 * 3600; // nine hours (twenty minutes on Chains)
        consensus.pos.set_lead_seconds = 9 * 3600;       // as long: the set fixed before the seed (an epoch on Chains)
        consensus.pos.final_anchors = 3;                 // half an hour (6 minutes on Chains); not consensus
        // Unbonding: a week on ecash and ecashbeta (user requirement), a day on ecashtest; a proof of double signing is
        // taken until the stake it slashes is paid back: the window one anchor less than unbonding (an unbond of a set's
        // stake is made at that set's anchor or later, so its coins stay until that anchor's count plus the unbonding).
        // Both well past the ten hours a validator still leads after it unbonds (set_lead_seconds and an epoch).
        if (chain == ChainType::ECASH_TESTNET) {
            consensus.pos.unbonding_anchors = 144;       // a day of ten-minute anchors (2 hours before: no longer past the set lead)
            consensus.pos.evidence_window_anchors = 143; // until the stake is paid back
            consensus.pos.pot_reserve_anchors = 6;       // an hour
        } else {
            consensus.pos.unbonding_anchors = 1008;       // seven days of ten-minute anchors
            consensus.pos.evidence_window_anchors = 1007; // until the stake is paid back
            consensus.pos.pot_reserve_anchors = 144;      // a day: each anchor pays 1/144 of the reserve
        }
    }
};

std::unique_ptr<const CChainParams> CChainParams::Ecash(ChainType chain, const MainNetOptions& options)
{
    return std::make_unique<const CEcashSidechainParams>(chain, options);
}

std::unique_ptr<const CChainParams> CChainParams::SigNet(const SigNetOptions& options)
{
    return std::make_unique<const SigNetParams>(options);
}

std::unique_ptr<const CChainParams> CChainParams::RegTest(const RegTestOptions& options)
{
    return std::make_unique<const CRegTestParams>(options);
}

std::unique_ptr<const CChainParams> CChainParams::Main(const MainNetOptions& options)
{
    return std::make_unique<const CMainParams>(options);
}

std::unique_ptr<const CChainParams> CChainParams::TestNet(const TestNetOptions& options)
{
    return std::make_unique<const CTestNetParams>(options);
}


std::vector<int> CChainParams::GetAvailableSnapshotHeights() const
{
    std::vector<int> heights;
    heights.reserve(m_assumeutxo_data.size());

    for (const auto& data : m_assumeutxo_data) {
        heights.emplace_back(data.height);
    }
    return heights;
}

std::optional<ChainType> GetNetworkForMagic(const MessageStartChars& message)
{
    const auto mainnet_msg = CChainParams::Main()->MessageStart();
    const auto testnet_msg = CChainParams::TestNet()->MessageStart();
    const auto regtest_msg = CChainParams::RegTest()->MessageStart();
    const auto signet_msg = CChainParams::SigNet()->MessageStart();
    for (const ChainType ecash : {ChainType::ECASH, ChainType::ECASH_BETANET, ChainType::ECASH_TESTNET}) {
        if (std::ranges::equal(message, CChainParams::Ecash(ecash)->MessageStart())) return ecash;
    }

    if (std::ranges::equal(message, mainnet_msg)) {
        return ChainType::MAIN;
    } else if (std::ranges::equal(message, testnet_msg)) {
        return ChainType::TESTNET;
    } else if (std::ranges::equal(message, regtest_msg)) {
        return ChainType::REGTEST;
    } else if (std::ranges::equal(message, signet_msg)) {
        return ChainType::SIGNET;
    }
    return std::nullopt;
}
