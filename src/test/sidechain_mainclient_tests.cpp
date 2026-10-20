// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <chainparamsbase.h>
#include <kernel/chainparams.h>
#include <sidechain/follower.h>
#include <sidechain/mainclient.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <util/chaintype.h>
#include <util/fs_helpers.h>
#include <util/translation.h>

#include <boost/test/unit_test.hpp>

#include <fstream>
#include <set>
#include <vector>

using namespace sidechain;

namespace {
fs::path Write(const fs::path& path, const std::string& content)
{
    std::ofstream file{path.std_path(), std::ios::binary | std::ios::trunc};
    file << content;
    return path;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(sidechain_mainclient_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(password_file_first_line)
{
    const fs::path path{Write(m_args.GetDataDirBase() / "pw", "s3cret\r\nignored\n")};
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    const auto password{ReadPasswordFile(path)};
    BOOST_REQUIRE(password);
    BOOST_CHECK_EQUAL(*password, "s3cret");
}

BOOST_AUTO_TEST_CASE(password_file_errors_hide_content)
{
    BOOST_CHECK(!ReadPasswordFile(m_args.GetDataDirBase() / "missing"));
    const fs::path empty{Write(m_args.GetDataDirBase() / "empty", "\nsecond-line\n")};
    fs::permissions(empty, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    const auto result{ReadPasswordFile(empty)};
    BOOST_REQUIRE(!result);
    BOOST_CHECK(util::ErrorString(result).original.find("second-line") == std::string::npos);
    const fs::path large{Write(m_args.GetDataDirBase() / "large", std::string(5000, 'x'))};
    fs::permissions(large, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    BOOST_CHECK(!ReadPasswordFile(large));
}

#ifndef WIN32
BOOST_AUTO_TEST_CASE(password_file_refused_when_others_can_read)
{
    const fs::path path{Write(m_args.GetDataDirBase() / "pw", "topsecret\n")};
    for (const fs::perms extra : {fs::perms::group_read, fs::perms::others_read, fs::perms::group_write, fs::perms::others_write}) {
        fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write | extra, fs::perm_options::replace);
        const auto result{ReadPasswordFile(path)};
        BOOST_REQUIRE(!result);
        const std::string error{util::ErrorString(result).original};
        BOOST_CHECK(error.find("chmod 600") != std::string::npos);
        BOOST_CHECK(error.find("topsecret") == std::string::npos);
    }
    fs::permissions(path, fs::perms::owner_read, fs::perm_options::replace);
    BOOST_CHECK(ReadPasswordFile(path));
    // A directory is not a password file.
    fs::create_directories(m_args.GetDataDirBase() / "dir");
    fs::permissions(m_args.GetDataDirBase() / "dir", fs::perms::owner_all, fs::perm_options::replace);
    BOOST_CHECK(!ReadPasswordFile(m_args.GetDataDirBase() / "dir"));
}
#endif

// The state of the link to the mainchain node (getmainchaininfo, the alert of the apps), from what the
// follower knows: errors of MainClient and of the follower, and what the mainchain node said of itself.
BOOST_AUTO_TEST_CASE(link_error_kinds)
{
    using K = Follower::LinkError;
    // A real call to a port nobody listens on (port 1: privileged, nothing there).
    MainClient::Options options;
    options.port = 1;
    options.credentials = "u:p";
    std::string refused;
    try {
        MainClient{options}.Call("getblockcount", UniValue{UniValue::VARR});
    } catch (const MainClientError& e) {
        refused = e.what();
    }
    BOOST_CHECK_EQUAL(Follower::LinkErrorName(Follower::ClassifyError(refused)), "refused");
    BOOST_CHECK(Follower::ClassifyError("Cannot reach the mainchain node at 127.0.0.1:19554: timeout") == K::TIMEOUT);
    BOOST_CHECK(Follower::ClassifyError("Cannot reach the mainchain node at nowhere:1: unknown host") == K::UNREACHABLE);
    BOOST_CHECK(Follower::ClassifyError("Cannot reach the mainchain node at h:1: not an HTTP reply") == K::UNREACHABLE);
    BOOST_CHECK(Follower::ClassifyError("The mainchain node refused the credentials") == K::AUTH);
    BOOST_CHECK(Follower::ClassifyError("The mainchain node does not allow connections from this address (rpcallowip)") == K::AUTH);
    BOOST_CHECK(Follower::ClassifyError("Cannot reach the mainchain node at h:1: no credentials: set -mainchainrpcuser and -mainchainrpcpassword, or check that the cookie file x can be read") == K::AUTH);
    BOOST_CHECK(Follower::ClassifyError("The mainchain node is on another chain than the one on record (its block at height 0 is a, the record's b); check -mainchainrpcconnect and -mainchainrpcport") == K::WRONG_NETWORK);
    BOOST_CHECK(Follower::ClassifyError("The mainchain node is still syncing; waiting for it to catch up before following it") == K::NOT_SYNCED);
    BOOST_CHECK(Follower::ClassifyError("The mainchain node is behind the record (at height 5, the record at 9); waiting for it to catch up") == K::NOT_SYNCED);
    BOOST_CHECK(Follower::ClassifyError("The mainchain node answered getsidechainevents with an error: boom") == K::OTHER);
    BOOST_CHECK(Follower::ClassifyError("") == K::NONE);
    for (const K kind : {K::NONE, K::REFUSED, K::UNREACHABLE, K::AUTH, K::TIMEOUT, K::WRONG_NETWORK, K::NOT_SYNCED, K::OTHER}) {
        BOOST_CHECK_EQUAL(Follower::LinkErrorName(kind).empty(), kind == K::NONE);
    }
}

BOOST_AUTO_TEST_CASE(link_state_mapping)
{
    using S = Follower::LinkState;
    using K = Follower::LinkError;
    const auto link{[](const Follower::Status& s, const std::string& own = "regtest") { return Follower::Link(s, own); }};
    Follower::Status s;
    // Down: it does not answer, with the reason.
    s.error = "Cannot reach the mainchain node at 127.0.0.1:19554: connection refused";
    BOOST_CHECK(link(s) == std::pair(S::DOWN, K::REFUSED));
    s.error = "The mainchain node answered getsidechain with an error: no";
    BOOST_CHECK(link(s) == std::pair(S::DOWN, K::OTHER));
    s.error.clear();
    BOOST_CHECK(link(s) == std::pair(S::DOWN, K::OTHER));
    // It answers but has not got as far as the record: syncing.
    s.error = "The mainchain node is behind the record (at height 5, the record at 9); waiting for it to catch up";
    BOOST_CHECK(link(s) == std::pair(S::SYNCING, K::NOT_SYNCED));
    // Up and in step.
    s = {};
    s.connected = true;
    s.main_chain = "regtest";
    s.main_blocks = s.main_headers = 100;
    s.followed_height = 100;
    BOOST_CHECK(link(s) == std::pair(S::OK, K::NONE));
    BOOST_CHECK_EQUAL(s.Behind(), 0);
    // Not looked at yet: nothing known of its sync, ok.
    Follower::Status fresh;
    fresh.connected = true;
    BOOST_CHECK(link(fresh) == std::pair(S::OK, K::NONE));
    BOOST_CHECK_EQUAL(fresh.Behind(), 0);
    // A few blocks behind its headers is normal as blocks come in; more is syncing.
    s.main_headers = 100 + Follower::BEHIND_MARGIN;
    BOOST_CHECK(link(s) == std::pair(S::OK, K::NONE));
    s.main_headers = 101 + Follower::BEHIND_MARGIN;
    BOOST_CHECK(link(s) == std::pair(S::SYNCING, K::NOT_SYNCED));
    BOOST_CHECK_EQUAL(s.Behind(), 1 + Follower::BEHIND_MARGIN);
    // In initial block download: syncing, however close.
    s.main_headers = 100;
    s.main_ibd = true;
    BOOST_CHECK(link(s) == std::pair(S::SYNCING, K::NOT_SYNCED));
    // On another network than this node: down, whatever its sync.
    BOOST_CHECK(link(s, "test") == std::pair(S::DOWN, K::WRONG_NETWORK));
    BOOST_CHECK_EQUAL(Follower::LinkStateName(S::OK), "ok");
    BOOST_CHECK_EQUAL(Follower::LinkStateName(S::SYNCING), "syncing");
    BOOST_CHECK_EQUAL(Follower::LinkStateName(S::DOWN), "down");
}

BOOST_AUTO_TEST_CASE(ecash_networks)
{
    // The networks of a sidechain of Backbone on eCash, beside those of a sidechain of Chains.
    for (const auto& [name, type] : std::vector<std::pair<std::string, ChainType>>{{"ecash", ChainType::ECASH}, {"ecashbeta", ChainType::ECASH_BETANET}, {"ecashtest", ChainType::ECASH_TESTNET}}) {
        BOOST_CHECK(ChainTypeFromString(name) == type);
        BOOST_CHECK_EQUAL(ChainTypeToString(type), name);
        BOOST_CHECK(IsEcashChain(type));
    }
    BOOST_CHECK(!IsEcashChain(ChainType::MAIN) && !IsEcashChain(ChainType::REGTEST));
    // The mainchain node: Backbone's ports and folders for the eCash networks, Chains' for the others.
    BOOST_CHECK_EQUAL(DefaultMainchainRPCPort(ChainType::ECASH), 8532);
    BOOST_CHECK_EQUAL(DefaultMainchainRPCPort(ChainType::ECASH_BETANET), 8532);
    BOOST_CHECK_EQUAL(DefaultMainchainRPCPort(ChainType::ECASH_TESTNET), 48532);
    BOOST_CHECK_EQUAL(DefaultMainchainRPCPort(ChainType::MAIN), 9554);
    BOOST_CHECK_EQUAL(DefaultMainchainRPCPort(ChainType::REGTEST), 29554);
    BOOST_CHECK_EQUAL(MainchainDataSubdir(ChainType::ECASH), "");
    BOOST_CHECK_EQUAL(MainchainDataSubdir(ChainType::ECASH_BETANET), "betanet");
    BOOST_CHECK_EQUAL(MainchainDataSubdir(ChainType::ECASH_TESTNET), "testnet4");
    BOOST_CHECK_EQUAL(MainchainName(ChainType::ECASH), "Backbone");
    BOOST_CHECK_EQUAL(MainchainName(ChainType::TESTNET), "Chains");
    // A data directory of its own for the eCash networks.
    BOOST_CHECK(GetDefaultDataDir(/*ecash=*/true) != GetDefaultDataDir(/*ecash=*/false));
    BOOST_CHECK(GetDefaultDataDir() == GetDefaultDataDir(/*ecash=*/false));
    BOOST_CHECK(fs::PathToString(GetDefaultDataDir(/*ecash=*/true).filename()).find("ackbone-") != std::string::npos);

    // Identities of their own: genesis, message start, ports, address prefix; no two networks alike.
    std::set<uint256> genesis;
    std::set<std::string> hrps;
    std::set<uint16_t> ports;
    for (const ChainType type : {ChainType::MAIN, ChainType::TESTNET, ChainType::SIGNET, ChainType::REGTEST, ChainType::ECASH, ChainType::ECASH_BETANET, ChainType::ECASH_TESTNET}) {
        const auto params{CreateChainParams(m_args, type)};
        BOOST_CHECK(params->GetChainType() == type);
        BOOST_CHECK(genesis.insert(params->GenesisBlock().GetHash()).second);
        if (type != ChainType::SIGNET && type != ChainType::REGTEST) BOOST_CHECK(hrps.insert(params->Bech32HRP()).second);
        BOOST_CHECK(ports.insert(params->GetDefaultPort()).second);
        BOOST_CHECK(ports.insert(CreateBaseChainParams(type)->RPCPort()).second);
        BOOST_CHECK(GetNetworkForMagic(params->MessageStart()) == type);
        const Consensus::Params& consensus{params->GetConsensus()};
        if (IsEcashChain(type)) {
            BOOST_CHECK(consensus.sidechain.enabled);
            // No single payout on the enforcer's eCash, and ten-minute mainchain blocks.
            BOOST_CHECK(!consensus.sidechain.main_single_payout);
            // Layers: a block a slot (15 s), the grace and retry in blocks of this chain as on Chains.
            BOOST_CHECK_EQUAL(consensus.nPowTargetSpacing, consensus.pos.slot_seconds);
            BOOST_CHECK_EQUAL(consensus.sidechain.refund_grace_blocks, 6);
            BOOST_CHECK_EQUAL(consensus.sidechain.main_behind_margin, 2);
            BOOST_CHECK_EQUAL(consensus.sidechain.pending_min_score, type == ChainType::ECASH_TESTNET ? 2U : 1315U);
            BOOST_CHECK_EQUAL(consensus.sidechain.unproposed_expiry_blocks, type == ChainType::ECASH_TESTNET ? 12 : 144);
            const auto base{CreateChainParams(m_args, type == ChainType::ECASH_TESTNET ? ChainType::TESTNET : ChainType::MAIN)};
            BOOST_CHECK_EQUAL(consensus.sidechain.bundle_retry_delay, base->GetConsensus().sidechain.bundle_retry_delay);
            // The anchoring of the blocks of stake, for ten-minute anchors.
            BOOST_CHECK_EQUAL(consensus.pos.fee_target_spacing, 600);
            BOOST_CHECK_EQUAL(base->GetConsensus().pos.fee_target_spacing, 60);
            BOOST_CHECK_EQUAL(consensus.pos.max_unanchored_slots, 720);
            BOOST_CHECK_EQUAL(consensus.pos.stall_seconds, 3600);
            BOOST_CHECK_EQUAL(consensus.pos.seed_lookahead_seconds, 3600);
            BOOST_CHECK_EQUAL(consensus.pos.final_anchors, 3);
            BOOST_CHECK_EQUAL(consensus.pos.unbonding_anchors, type == ChainType::ECASH_TESTNET ? 12 : 288);
            BOOST_CHECK_EQUAL(consensus.pos.evidence_window_anchors, type == ChainType::ECASH_TESTNET ? 6 : 144);
            BOOST_CHECK_EQUAL(consensus.pos.pot_reserve_anchors, type == ChainType::ECASH_TESTNET ? 6 : 144);
            BOOST_CHECK_EQUAL(consensus.pos.slot_seconds, base->GetConsensus().pos.slot_seconds);
            BOOST_CHECK_EQUAL(consensus.pos.genesis_time, params->GenesisBlock().nTime);
            BOOST_CHECK_EQUAL(consensus.coinbase_maturity, 0);
        } else if (type != ChainType::REGTEST) {
            BOOST_CHECK(consensus.sidechain.main_single_payout);
        }
    }

    // The network of a mainchain node: Backbone's "network" (its "chain" is eCash's name), else Chains' "chain".
    UniValue backbone{UniValue::VOBJ};
    backbone.pushKV("chain", "main");
    backbone.pushKV("network", "ecashbeta");
    BOOST_CHECK_EQUAL(Follower::MainNetwork(backbone), "ecashbeta");
    UniValue chains{UniValue::VOBJ};
    chains.pushKV("chain", "test");
    BOOST_CHECK_EQUAL(Follower::MainNetwork(chains), "test");
    Follower::Status s;
    s.connected = true;
    s.main_chain = "main";
    BOOST_CHECK(Follower::Link(s, "ecash").second == Follower::LinkError::WRONG_NETWORK);
    s.main_chain = "ecash";
    BOOST_CHECK(Follower::Link(s, "ecash").first == Follower::LinkState::OK);
    // Two ten-minute blocks behind is syncing on eCash, not on Chains.
    s.main_blocks = s.main_headers = 103;
    s.followed_height = 100;
    BOOST_CHECK(Follower::Link(s, "ecash", 2).first == Follower::LinkState::SYNCING);
    BOOST_CHECK(Follower::Link(s, "ecash").first == Follower::LinkState::OK);
}

BOOST_AUTO_TEST_SUITE_END()
