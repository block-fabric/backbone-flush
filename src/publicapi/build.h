// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PUBLICAPI_BUILD_H
#define BITCOIN_PUBLICAPI_BUILD_H

#include <uint256.h>
#include <univalue.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * The write path of the public API without keys (doc/public-api.md, "Builds"): an action the server will never sign,
 * written two ways that say the same thing, for whoever signs:
 *
 *  - a `flush:` URI (flush.markets docs/uri.md, the app's links: flush:<verb>?v=1&network=..&exp=..&...), which the
 *    Flush app opens, quotes again from its own node, shows in its own dialog, and signs only if the user confirms,
 *    the floor (min_tokens, min_ecx, min_out...) binding;
 *  - a payload for a wallet extension: canonical JSON (keys sorted, no spaces, every amount a decimal string, integers
 *    for the version, the expiry, the slippage and the height), and its hash, SHA-256 of "flush-action/v1\n" and the
 *    payload's bytes as given, whose first 8 hex digits the wallet shows beside the action (the check code) so that
 *    the user can see that the page and the wallet speak of the same action.
 *
 * Neither is a transaction: which coins pay is the wallet's to choose. The expiry (`exp`, Unix seconds) and the floors
 * are what bind; the slippage is shown only.
 */
namespace publicapi {

/** Percent-encoding of a URI's value: every byte but A-Z a-z 0-9 - . _ ~ (RFC 3986; a space is %20). */
std::string PercentEncode(std::string_view value);

/** Units as a decimal of at most `max_places` places (rounded down past them), without trailing zeros: 1.5, 3. */
std::string Decimal(uint64_t units, int decimals, int max_places = 8);
/** Units rounded down to what a decimal of `max_places` places can say (an asset of more decimals than a link has). */
uint64_t FloorUnits(uint64_t units, int decimals, int max_places = 8);

/** The parameters of an action, in the order the URI lists them. */
using UriParams = std::vector<std::pair<std::string, std::string>>;

/** flush:<verb>?v=1&network=<network>&exp=<expires>&<params...>. */
std::string ActionUri(const std::string& verb, const std::string& network, int64_t expires, const UriParams& params);

/** The canonical JSON of an object (keys sorted, values as given: strings, integers, arrays of strings). */
std::string CanonicalJson(const std::map<std::string, UniValue>& fields);
/** SHA-256 of "flush-action/v1\n" and the payload, hex. */
std::string ActionHash(const std::string& payload);

/**
 * 32 bytes given as 64 hex digits in the order written, not reversed as a hash's display is: a reservation's nonce (the
 * HMAC key) and its commitment (the HMAC's output, as the marker carries it). nullopt if it is not that.
 */
std::optional<uint256> ParseBytes32(const std::string& hex);
/** The same 32 bytes as hex, in order. */
std::string HexBytes32(const uint256& bytes);

} // namespace publicapi

#endif // BITCOIN_PUBLICAPI_BUILD_H
