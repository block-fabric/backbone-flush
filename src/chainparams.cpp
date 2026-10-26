// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>

#include <chainparamsbase.h>
#include <common/args.h>
#include <consensus/amount.h>
#include <consensus/params.h>
#include <deploymentinfo.h>
#include <tinyformat.h>
#include <util/chaintype.h>
#include <util/log.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

using util::SplitString;

static void HandleDeploymentArgs(const ArgsManager& args, CChainParams::DeploymentOptions& options)
{
    for (const std::string& arg : args.GetArgs("-testactivationheight")) {
        const auto found{arg.find('@')};
        if (found == std::string::npos) {
            throw std::runtime_error(strprintf("Invalid format (%s) for -testactivationheight=name@height.", arg));
        }

        const auto value{arg.substr(found + 1)};
        const auto height{ToIntegral<int32_t>(value)};
        if (!height || *height < 0 || *height >= std::numeric_limits<int>::max()) {
            throw std::runtime_error(strprintf("Invalid height value (%s) for -testactivationheight=name@height.", arg));
        }

        const auto deployment_name{arg.substr(0, found)};
        if (deployment_name == "logo") {
            options.logo_height = *height;
        } else if (deployment_name == "feesplit") {
            options.fee_split_height = *height;
        } else if (deployment_name == "dex") {
            options.dex_height = *height;
        } else if (deployment_name == "dexcurves") {
            options.dex_curves_height = *height;
        } else if (deployment_name == "feecurve2") {
            options.fee_curve2_height = *height;
        } else if (deployment_name == "jailfix") {
            options.jail_fix_height = *height;
        } else if (deployment_name == "ticker") {
            options.ticker_height = *height;
        } else if (deployment_name == "launchparams") {
            options.launch_params_height = *height;
        } else if (deployment_name == "allocation10") {
            options.allocation10_height = *height;
        } else if (deployment_name == "smallpayouts") {
            options.small_payout_height = *height;
        } else if (deployment_name == "notreasury") {
            options.no_treasury_height = *height;
        } else if (deployment_name == "feeflow") {
            options.fee_flow_height = *height;
        } else if (deployment_name == "graduation100") {
            options.graduation100_height = *height;
        } else if (deployment_name == "v7") {
            options.v7_height = *height;
        } else if (const auto buried_deployment = GetBuriedDeployment(deployment_name)) {
            options.activation_heights[*buried_deployment] = *height;
        } else {
            throw std::runtime_error(strprintf("Invalid name (%s) for -testactivationheight=name@height.", arg));
        }
    }

    for (const std::string& strDeployment : args.GetArgs("-vbparams")) {
        std::vector<std::string> vDeploymentParams = SplitString(strDeployment, ':');
        if (vDeploymentParams.size() < 3 || 4 < vDeploymentParams.size()) {
            throw std::runtime_error("Version bits parameters malformed, expecting deployment:start:end[:min_activation_height]");
        }
        CChainParams::VersionBitsParameters vbparams{};
        const auto start_time{ToIntegral<int64_t>(vDeploymentParams[1])};
        if (!start_time) {
            throw std::runtime_error(strprintf("Invalid nStartTime (%s)", vDeploymentParams[1]));
        }
        vbparams.start_time = *start_time;
        const auto timeout{ToIntegral<int64_t>(vDeploymentParams[2])};
        if (!timeout) {
            throw std::runtime_error(strprintf("Invalid nTimeout (%s)", vDeploymentParams[2]));
        }
        vbparams.timeout = *timeout;
        if (vDeploymentParams.size() >= 4) {
            const auto min_activation_height{ToIntegral<int64_t>(vDeploymentParams[3])};
            if (!min_activation_height) {
                throw std::runtime_error(strprintf("Invalid min_activation_height (%s)", vDeploymentParams[3]));
            }
            vbparams.min_activation_height = *min_activation_height;
        } else {
            vbparams.min_activation_height = 0;
        }
        bool found = false;
        for (int j=0; j < (int)Consensus::MAX_VERSION_BITS_DEPLOYMENTS; ++j) {
            if (vDeploymentParams[0] == VersionBitsDeploymentInfo[j].name) {
                options.version_bits_parameters[Consensus::DeploymentPos(j)] = vbparams;
                found = true;
                LogInfo("Setting version bits activation parameters for %s to start=%ld, timeout=%ld, min_activation_height=%d",
                        vDeploymentParams[0], vbparams.start_time, vbparams.timeout, vbparams.min_activation_height);
                break;
            }
        }
        if (!found) {
            throw std::runtime_error(strprintf("Invalid deployment (%s)", vDeploymentParams[0]));
        }
    }
}

void ReadMainNetArgs(const ArgsManager& args, CChainParams::MainNetOptions& options)
{
    HandleDeploymentArgs(args, options.dep_opts);
}

void ReadTestNetArgs(const ArgsManager& args, CChainParams::TestNetOptions& options)
{
    HandleDeploymentArgs(args, options.dep_opts);
}

void ReadSigNetArgs(const ArgsManager& args, CChainParams::SigNetOptions& options)
{
    if (!args.GetArgs("-signetseednode").empty()) {
        options.seeds.emplace(args.GetArgs("-signetseednode"));
    }
    if (!args.GetArgs("-signetchallenge").empty()) {
        const auto signet_challenge = args.GetArgs("-signetchallenge");
        if (signet_challenge.size() != 1) {
            throw std::runtime_error("-signetchallenge cannot be multiple values.");
        }
        const auto val{TryParseHex<uint8_t>(signet_challenge[0])};
        if (!val) {
            throw std::runtime_error(strprintf("-signetchallenge must be hex, not '%s'.", signet_challenge[0]));
        }
        options.challenge.emplace(*val);
    }
    HandleDeploymentArgs(args, options.dep_opts);
}

void ReadRegTestArgs(const ArgsManager& args, CChainParams::RegTestOptions& options)
{
    if (auto value = args.GetBoolArg("-fastprune")) options.fastprune = *value;
    if (HasTestOption(args, "bip94")) options.enforce_bip94 = true;
    if (const auto slot{args.GetIntArg("-sidechainslot")}) {
        if (*slot < 0 || *slot > 0xFFFF) throw std::runtime_error("Invalid -sidechainslot");
        options.sidechain_slot = static_cast<uint32_t>(*slot);
    }
    for (const std::string& list : args.GetArgs("-posvalidators")) {
        for (const std::string& item : SplitString(list, ',')) {
            if (item.empty()) continue;
            const auto colon{item.find(':')};
            const std::string key{item.substr(0, colon)};
            const auto bytes{TryParseHex<uint8_t>(key)};
            const auto stake{colon == std::string::npos ? std::optional<int64_t>{1} : ToIntegral<int64_t>(item.substr(colon + 1))};
            if (!bytes || bytes->size() != 32 || !stake || *stake <= 0 || *stake > MAX_MONEY) {
                throw std::runtime_error(strprintf("Invalid -posvalidators entry (%s): a 32-byte x-only public key in hex, optionally followed by :<stake>", item));
            }
            options.pos_validators.push_back({.pubkey = uint256{std::span<const unsigned char>{*bytes}}, .stake = *stake});
        }
    }
    if (const auto final_anchors{args.GetIntArg("-posfinalanchors")}) {
        if (*final_anchors < 1 || *final_anchors > 100) throw std::runtime_error("Invalid -posfinalanchors");
        options.pos_final_anchors = static_cast<int>(*final_anchors);
    }
    if (const auto slot_seconds{args.GetIntArg("-posslotseconds")}) {
        if (*slot_seconds < 1 || *slot_seconds > 600) throw std::runtime_error("Invalid -posslotseconds");
        options.pos_slot_seconds = *slot_seconds;
    }
    if (const auto inbound{args.GetIntArg("-posuncheckedinbound")}) {
        if (*inbound < 1 || *inbound > 100000) throw std::runtime_error("Invalid -posuncheckedinbound");
        options.pos_unchecked_inbound = *inbound;
    }
    if (const auto headers{args.GetIntArg("-posuncheckedheaders")}) {
        if (*headers < 1 || *headers > 100000) throw std::runtime_error("Invalid -posuncheckedheaders");
        options.pos_unchecked_headers = *headers;
    }
    if (const auto bytes{args.GetIntArg("-posuncheckedbytes")}) {
        if (*bytes < 1 || *bytes > (int64_t{1} << 30)) throw std::runtime_error("Invalid -posuncheckedbytes");
        options.pos_unchecked_bytes = *bytes;
    }
    if (const auto lookahead{args.GetIntArg("-posseedlookahead")}) {
        if (*lookahead < 0 || *lookahead > 365 * 24 * 60 * 60) throw std::runtime_error("Invalid -posseedlookahead");
        options.pos_seed_lookahead = *lookahead;
    }
    if (args.GetBoolArg("-testmainenforcer", false)) {
        if (!options.sidechain_slot) throw std::runtime_error("-testmainenforcer needs -sidechainslot");
        options.main_enforcer = true;
    }
    if (const auto height{args.GetIntArg("-sidechainactivationheight")}) {
        if (*height < 0 || *height > std::numeric_limits<int32_t>::max()) throw std::runtime_error("Invalid -sidechainactivationheight");
        options.sidechain_activation_height = static_cast<int>(*height);
    }
    for (const std::string& arg : args.GetArgs("-testposparam")) {
        const auto found{arg.find('@')};
        const auto value{found == std::string::npos ? std::nullopt : ToIntegral<int64_t>(arg.substr(found + 1))};
        if (!value || *value < 0) throw std::runtime_error(strprintf("Invalid format (%s) for -testposparam=name@value.", arg));
        options.pos_params.emplace_back(arg.substr(0, found), *value);
    }
    for (const std::string& arg : args.GetArgs("-testdrivechainparam")) {
        const auto found{arg.find('@')};
        const auto value{found == std::string::npos ? std::nullopt : ToIntegral<int32_t>(arg.substr(found + 1))};
        if (!value || *value < 0) throw std::runtime_error(strprintf("Invalid format (%s) for -testdrivechainparam=name@value.", arg));
        options.drivechain_params.emplace_back(arg.substr(0, found), *value);
    }

    HandleDeploymentArgs(args, options.dep_opts);
}

static std::unique_ptr<const CChainParams> globalChainParams;

const CChainParams &Params() {
    assert(globalChainParams);
    return *globalChainParams;
}

std::unique_ptr<const CChainParams> CreateChainParams(const ArgsManager& args, const ChainType chain)
{
    switch (chain) {
    case ChainType::MAIN: {
        auto opts = CChainParams::MainNetOptions{};
        ReadMainNetArgs(args, opts);
        return CChainParams::Main(opts);
    }
    case ChainType::TESTNET: {
        auto opts = CChainParams::TestNetOptions{};
        ReadTestNetArgs(args, opts);
        return CChainParams::TestNet(opts);
    }
    case ChainType::SIGNET: {
        auto opts = CChainParams::SigNetOptions{};
        ReadSigNetArgs(args, opts);
        return CChainParams::SigNet(opts);
    }
    case ChainType::REGTEST: {
        auto opts = CChainParams::RegTestOptions{};
        ReadRegTestArgs(args, opts);
        return CChainParams::RegTest(opts);
    }
    case ChainType::ECASH:
    case ChainType::ECASH_BETANET:
    case ChainType::ECASH_TESTNET: {
        auto opts = CChainParams::MainNetOptions{};
        ReadMainNetArgs(args, opts);
        return CChainParams::Ecash(chain, opts);
    }
    }
    assert(false);
}

void SelectParams(const ChainType chain)
{
    SelectBaseParams(chain);
    globalChainParams = CreateChainParams(gArgs, chain);
}
