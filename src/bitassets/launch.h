// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BITASSETS_LAUNCH_H
#define BITCOIN_BITASSETS_LAUNCH_H

#include <bitassets/state.h>
#include <consensus/params.h>

#include <univalue.h>

#include <optional>
#include <string>
#include <vector>

/**
 * What the wallet's reserveasset, registerasset and launchtoken and the public API's unsigned transactions
 * (POST /api/v1/psbt/…, doc/public-api.md) share: the checks of a new asset's name and of a launch's or a
 * registration's options, the operation they make, and the outputs its transaction lists before its marker. One
 * implementation, so that a launch built for an external wallet is the launch the node's own wallet would send.
 *
 * Errors are thrown as the RPC methods throw them (JSONRPCError), with the wallet's own messages.
 */
namespace bitassets {

/** What the rules say at `height` (the next block's) about new assets and launches. */
struct LaunchRules {
    int height{0};
    bool tickers{false};
    bool logos{false};
    //! The DEX's pool kinds: launches at all (getdexinfo's curves_active).
    bool curves{false};
    bool params{false};
    bool fee_flow{false};
    bool fee_split{false};
    uint16_t max_allocation{0};
    uint64_t min_chn{0};
    int reveal_depth{0};
    uint32_t network_fee{0};
    //! What a curve graduates at (its virtual coin reserve is half of it).
    int64_t graduation{0};
};
LaunchRules LaunchRulesAt(const Consensus::SidechainParams& rules, int height);

/** The name of a new asset as the wallet reserves and registers it: from ticker_height a ticker (uppercased). */
std::string NewAssetName(const std::string& given, bool tickers);
/** reserveasset's checks of a name: printable, not read as another asset, not mimicking a number. */
void CheckReserveName(const std::string& name);
/** A logo given in base64 (a PNG or JPEG the rules take); "" with `allow_empty`: none. */
std::vector<unsigned char> ParseLogo(const UniValue& value, bool allow_empty);
/** A display name, checked as the rules check it; "" with `allow_empty`: none. */
std::string ParseDisplayName(const UniValue& value, bool allow_empty);
/** A curve's fee in percent (0.3 to 5), in millionths. */
uint32_t LaunchFeeArg(const UniValue& value);
/** Refused unless `supply` units can be a curve's (IsLaunchSupply). */
void CheckLaunchSupply(uint64_t supply, uint8_t decimals);
/** Refused if a first buy alone in its block on a new curve (graduating at `graduation`) gets less than `first_min_out`. */
void CheckFirstBuy(int64_t graduation, uint64_t supply, uint32_t fee, uint64_t first_buy, uint64_t first_min_out, uint64_t min_buy, uint64_t max_buy, uint8_t decimals);
/** launchtoken's fee_split: [{"to": "creator" | "burn" | address, "bps": n}] (CheckFeeSplit). */
std::vector<FeeShare> ParseFeeSplit(const UniValue& value);

/** A launch launchtoken's options make, before the wallet's own parts (nonce, creator, chn_to). */
struct LaunchPlan {
    //! reg.nonce, creator and chn_to not set: the maker's.
    LaunchToken launch;
    uint64_t supply{0};
    uint8_t decimals{0};
    //! The curve's fee (millionths), the network's if not chosen.
    uint32_t fee{0};
};
/**
 * launchtoken's checks of a ticker and of its options (display_name, logo, info, supply, decimals, fee, allocation,
 * first_buy, first_min_out, min_buy, max_buy, fee_destination, burn_percent, fee_split, keep_control), in that RPC's
 * terms. `name` is NewAssetName's. Not checked here: that the name is free, and the reservation.
 */
LaunchPlan ParseLaunchOptions(const std::string& name, const UniValue& options, const LaunchRules& rules);

/** registerasset's checks of its arguments: a Register of `name` (NewAssetName's) without its nonce. */
Register ParseRegistration(const std::string& name, const UniValue& supply, const UniValue& decimals, const UniValue& data, const UniValue& is_public,
                           const UniValue& logo, const UniValue& display_name, const LaunchRules& rules);

/** What a token output before the marker carries: a token, or (none) the next result of the operation. */
using OutToken = std::optional<Token>;
/** The outputs a launch's transaction has before its marker: its first buy's result, then the control coin kept. */
std::vector<OutToken> LaunchOutputs(const LaunchToken& launch);
/** A registration's: its control coin, then its supply (if any). */
std::vector<OutToken> RegisterOutputs(const Register& reg);
/** A reservation's: the reservation itself. */
std::vector<OutToken> ReserveOutputs();
/** The marker of an operation whose transaction has `outputs` (in this order) first, then the marker. */
Marker MarkerOf(std::optional<Operation> operation, const std::vector<OutToken>& outputs);

} // namespace bitassets

#endif // BITCOIN_BITASSETS_LAUNCH_H
