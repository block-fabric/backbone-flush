// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_KERNEL_CHAINPARAMS_H
#define BITCOIN_KERNEL_CHAINPARAMS_H

#include <consensus/params.h>
#include <kernel/messagestartchars.h>
#include <primitives/block.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <util/hash_type.h>
#include <util/vector.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

struct AssumeutxoHash : public BaseHash<uint256> {
    explicit AssumeutxoHash(const uint256& hash) : BaseHash(hash) {}
};

/**
 * Holds configuration for use during UTXO snapshot load and validation. The contents
 * here are security critical, since they dictate which UTXO snapshots are recognized
 * as valid.
 */
struct AssumeutxoData {
    int height;

    //! The expected hash of the deserialized UTXO set.
    AssumeutxoHash hash_serialized;

    //! Used to populate the m_chain_tx_count value, which is used during BlockManager::LoadBlockIndex().
    //!
    //! We need to hardcode the value here because this is computed cumulatively using block data,
    //! which we do not necessarily have at the time of snapshot load.
    uint64_t m_chain_tx_count;

    //! The hash of the base block for this snapshot. Used to refer to assumeutxo data
    //! prior to having a loaded blockindex.
    uint256 blockhash;
};

/**
 * Holds various statistics on transactions within a chain. Used to estimate
 * verification progress during chain sync.
 *
 * See also: CChainParams::TxData, GuessVerificationProgress.
 */
struct ChainTxData {
    int64_t nTime;    //!< UNIX timestamp of last known number of transactions
    uint64_t tx_count; //!< total number of transactions between genesis and that timestamp
    double dTxRate;   //!< estimated number of transactions per second after that timestamp
};

//! Configuration for headers sync memory usage.
struct HeadersSyncParams {
    //! Distance in blocks between header commitments.
    size_t commitment_period{0};
    //! Minimum number of validated headers to accumulate in the redownload
    //! buffer before feeding them into the permanent block index.
    size_t redownload_buffer_size{0};
};

/**
 * CChainParams defines various tweakable parameters of a given instance of the
 * Bitcoin system.
 */
class CChainParams
{
public:
    enum Base58Type {
        PUBKEY_ADDRESS,
        SCRIPT_ADDRESS,
        SECRET_KEY,
        EXT_PUBLIC_KEY,
        EXT_SECRET_KEY,

        MAX_BASE58_TYPES
    };

    const Consensus::Params& GetConsensus() const { return consensus; }
    const MessageStartChars& MessageStart() const { return pchMessageStart; }
    uint16_t GetDefaultPort() const { return nDefaultPort; }
    std::vector<int> GetAvailableSnapshotHeights() const;

    const CBlock& GenesisBlock() const { return genesis; }
    /** Default value for -checkmempool and -checkblockindex argument */
    bool DefaultConsistencyChecks() const { return fDefaultConsistencyChecks; }
    /** If this chain is exclusively used for testing */
    bool IsTestChain() const { return m_chain_type != ChainType::MAIN; }
    /** If this chain allows time to be mocked */
    bool IsMockableChain() const { return m_is_mockable_chain; }
    uint64_t PruneAfterHeight() const { return nPruneAfterHeight; }
    /** Minimum free space (in GB) needed for data directory */
    uint64_t AssumedBlockchainSize() const { return m_assumed_blockchain_size; }
    /** Minimum free space (in GB) needed for data directory when pruned; Does not include prune target*/
    uint64_t AssumedChainStateSize() const { return m_assumed_chain_state_size; }
    /** Whether it is possible to mine blocks on demand (no retargeting) */
    bool MineBlocksOnDemand() const { return consensus.fPowNoRetargeting; }
    /** Return the chain type string */
    std::string GetChainTypeString() const { return ChainTypeToString(m_chain_type); }
    /** Return the chain type */
    ChainType GetChainType() const { return m_chain_type; }
    /** Return the list of hostnames to look up for DNS seeds */
    const std::vector<std::string>& DNSSeeds() const { return vSeeds; }
    const std::vector<unsigned char>& Base58Prefix(Base58Type type) const { return base58Prefixes[type]; }
    const std::string& Bech32HRP() const { return bech32_hrp; }
    const std::vector<uint8_t>& FixedSeeds() const { return vFixedSeeds; }
    const HeadersSyncParams& HeadersSync() const { return m_headers_sync_params; }

    std::optional<AssumeutxoData> AssumeutxoForHeight(int height) const
    {
        return FindFirst(m_assumeutxo_data, [&](const auto& d) { return d.height == height; });
    }
    std::optional<AssumeutxoData> AssumeutxoForBlockhash(const uint256& blockhash) const
    {
        return FindFirst(m_assumeutxo_data, [&](const auto& d) { return d.blockhash == blockhash; });
    }

    const ChainTxData& TxData() const { return chainTxData; }

    /**
     * VersionBitsParameters holds activation parameters
     */
    struct VersionBitsParameters {
        int64_t start_time;
        int64_t timeout;
        int min_activation_height;
    };

    struct DeploymentOptions {
        std::unordered_map<Consensus::DeploymentPos, VersionBitsParameters> version_bits_parameters{};
        std::unordered_map<Consensus::BuriedDeployment, int> activation_heights{};
        //! FastBlocks: Consensus::PosParams::fee_split_height (-testactivationheight=feesplit@<height>; regtest only).
        std::optional<int> fee_split_height{};
        //! Layers: Consensus::SidechainParams::dex_height (-testactivationheight=dex@<height>; regtest only).
        std::optional<int> dex_height{};
        //! Layers: Consensus::SidechainParams::dex_curves_height (-testactivationheight=dexcurves@<height>; regtest only).
        std::optional<int> dex_curves_height{};
        //! FastBlocks: Consensus::PosParams::fee_curve2_height (-testactivationheight=feecurve2@<height>; regtest only).
        std::optional<int> fee_curve2_height{};
        //! FastBlocks: Consensus::PosParams::jail_fix_height (-testactivationheight=jailfix@<height>; regtest only).
        std::optional<int> jail_fix_height{};
        //! BitAssets: Consensus::SidechainParams::logo_height (-testactivationheight=logo@<height>; regtest only).
        std::optional<int> logo_height{};
        //! BitAssets: Consensus::SidechainParams::ticker_height (-testactivationheight=ticker@<height>; regtest only).
        std::optional<int> ticker_height{};
        //! Launches with parameters: Consensus::SidechainParams::launch_params_height (-testactivationheight=launchparams@<height>; regtest only).
        std::optional<int> launch_params_height{};
        //! Allocations of up to 10 %: Consensus::SidechainParams::allocation10_height (-testactivationheight=allocation10@<height>; regtest only).
        std::optional<int> allocation10_height{};
        //! Smaller payouts: Consensus::SidechainParams::small_payout_height (-testactivationheight=smallpayouts@<height>; regtest only).
        std::optional<int> small_payout_height{};
        //! No treasury: Consensus::SidechainParams::no_treasury_height (-testactivationheight=notreasury@<height>; regtest only).
        std::optional<int> no_treasury_height{};
        //! The fee-flow release: Consensus::SidechainParams::fee_flow_height (-testactivationheight=feeflow@<height>; regtest only).
        std::optional<int> fee_flow_height{};
        //! Graduation at 100 coins: Consensus::SidechainParams::graduation100_height (-testactivationheight=graduation100@<height>; regtest only).
        std::optional<int> graduation100_height{};
        //! Upgrade v7 (instant DEX payouts, the fee split): Consensus::SidechainParams::v7_height (-testactivationheight=v7@<height>; regtest only).
        std::optional<int> v7_height{};
        //! Upgrade v8 (bounded buybacks): Consensus::SidechainParams::v8_height (-testactivationheight=v8@<height>; regtest only).
        std::optional<int> v8_height{};
        //! Upgrade v9 (the DEX rules of Flush 8.3.2): Consensus::SidechainParams::v9_height (-testactivationheight=v9@<height>; regtest only).
        std::optional<int> v9_height{};
    };

    /**
     * SigNetOptions holds configurations for creating a signet CChainParams.
     */
    struct SigNetOptions {
        DeploymentOptions dep_opts{};
        std::optional<std::vector<uint8_t>> challenge{};
        std::optional<std::vector<std::string>> seeds{};
    };

    /**
     * RegTestOptions holds configurations for creating a regtest CChainParams.
     */
    struct RegTestOptions {
        DeploymentOptions dep_opts{};
        bool fastprune{false};
        bool enforce_bip94{false};
        //! If set, the chain is a sidechain in this slot of the mainchain.
        std::optional<uint32_t> sidechain_slot{};
        //! With sidechain_slot: SidechainParams::main_activation_height.
        int sidechain_activation_height{0};
        //! With sidechain_slot (-testmainenforcer): the mainchain has the rules of the bip300301_enforcer, as
        //! Backbone on eCash (backboned -testecash): no single payout (SidechainParams::main_single_payout).
        bool main_enforcer{false};
        //! Drivechain parameters to set, by the name of their field in Consensus::DrivechainParams.
        std::vector<std::pair<std::string, int>> drivechain_params{};
        //! FastBlocks: the fixed validator set of a regtest sidechain (-posvalidators).
        std::vector<Consensus::PosValidator> pos_validators{};
        std::optional<int64_t> pos_seed_lookahead{};
        //! -testposparam=name@value: a FastBlocks parameter of regtest (PosParams), for tests of eCash's values scaled.
        std::vector<std::pair<std::string, int64_t>> pos_params{};
        //! FastBlocks: how many anchors make a block final (-posfinalanchors; not consensus).
        std::optional<int> pos_final_anchors{};
        //! FastBlocks: the length of a slot in seconds (-posslotseconds; 1 by default), and of a stall (30 slots).
        std::optional<int64_t> pos_slot_seconds{};
        //! FastBlocks: the unchecked headers an inbound peer may hold (-posuncheckedinbound; not consensus).
        std::optional<int64_t> pos_unchecked_inbound{};
        //! FastBlocks: the unchecked headers an outbound peer may hold, and the bytes of their blocks any
        //! peer may (-posuncheckedheaders, -posuncheckedbytes; not consensus).
        std::optional<int64_t> pos_unchecked_headers{};
        std::optional<int64_t> pos_unchecked_bytes{};
    };

    struct MainNetOptions {
        DeploymentOptions dep_opts{};
    };

    struct TestNetOptions {
        DeploymentOptions dep_opts{};
    };

    static std::unique_ptr<const CChainParams> RegTest(const RegTestOptions& options);
    static std::unique_ptr<const CChainParams> RegTest() { const RegTestOptions opts{}; return RegTest(opts); }
    static std::unique_ptr<const CChainParams> SigNet(const SigNetOptions& options);
    static std::unique_ptr<const CChainParams> SigNet() { const SigNetOptions opts{}; return SigNet(opts); }
    static std::unique_ptr<const CChainParams> Main(const MainNetOptions& options);
    static std::unique_ptr<const CChainParams> Main() { const MainNetOptions opts{}; return Main(opts); }
    static std::unique_ptr<const CChainParams> TestNet(const TestNetOptions& options);
    static std::unique_ptr<const CChainParams> TestNet() { const TestNetOptions opts{}; return TestNet(opts); }
    /** A network of a sidechain of Backbone on eCash: `chain` is ChainType::ECASH, ECASH_BETANET or ECASH_TESTNET. */
    static std::unique_ptr<const CChainParams> Ecash(ChainType chain, const MainNetOptions& options);
    static std::unique_ptr<const CChainParams> Ecash(ChainType chain) { const MainNetOptions opts{}; return Ecash(chain, opts); }

protected:
    CChainParams() = default;

    Consensus::Params consensus;
    MessageStartChars pchMessageStart;
    uint16_t nDefaultPort;
    uint64_t nPruneAfterHeight;
    uint64_t m_assumed_blockchain_size;
    uint64_t m_assumed_chain_state_size;
    std::vector<std::string> vSeeds;
    std::vector<unsigned char> base58Prefixes[MAX_BASE58_TYPES];
    std::string bech32_hrp;
    ChainType m_chain_type;
    CBlock genesis;
    std::vector<uint8_t> vFixedSeeds;
    bool fDefaultConsistencyChecks;
    bool m_is_mockable_chain;
    std::vector<AssumeutxoData> m_assumeutxo_data;
    ChainTxData chainTxData;
    HeadersSyncParams m_headers_sync_params;

    void ApplyDeploymentOptions(const DeploymentOptions& opts);

    /** What sets a sidechain apart on one of its networks. */
    struct SidechainIdentity {
        //! The slot the mainchain gave to the sidechain on this network.
        uint32_t slot;
        //! Text in the first block. Change it and the chain is another chain.
        const char* genesis_message;
        uint32_t genesis_time;
        MessageStartChars message_start;
        uint16_t default_port;
        const char* bech32_hrp;
    };
    /**
     * Turn the chain being set up into a sidechain: its blocks are blind
     * merged mined instead of mined with proof of work, its coins come from
     * the mainchain, and it has an identity of its own.
     */
    void MakeSidechain(const SidechainIdentity& identity);
};

std::optional<ChainType> GetNetworkForMagic(const MessageStartChars& pchMessageStart);

#endif // BITCOIN_KERNEL_CHAINPARAMS_H
