// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_LINKS_H
#define BITCOIN_WEBUI_LINKS_H

#include <uint256.h>
#include <webui/chainconfig.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

/**
 * The action links of flush.markets (doc/flush-links.md; the site's docs/uri.md): what the site wants done, written as a
 * `flush:` URI or a launch file, which the app reads here, in the host, never in a page.
 *
 *   flush:<address>?amount=...                 a payment (BIP 21): the page's, as before (lib/uri.ts)
 *   flush:<verb>?v=1&network=<net>&exp=<t>&... an action: connect, launch, buy, sell, swap
 *
 * The parser is strict: one version, the network of the app only, an expiry within a day, each parameter of the verb
 * once (a link of a launch up to MAX_LINKS times), no other parameter, values percent-encoded from a small alphabet,
 * ids of exactly 64 lowercase hex digits, amounts as decimals of at most 8 places, every text of a bounded size and
 * without control characters. Anything else is refused with what is wrong, before anything is resolved or shown.
 * What a link asks is then resolved against the node and confirmed by the user (linkhost.h).
 */
namespace webui::links {

/** Why a link or a launch file is refused: said to the user as it is. */
struct LinkError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class Verb { CONNECT, LAUNCH, BUY, SELL, SWAP };
std::string_view VerbName(Verb verb);

//! The version of the links (v=1) and of the launch files ("version": 1).
inline constexpr int VERSION{1};
//! The most bytes of a link, as given (a launch's description and links fit the chain's 512 bytes of info).
inline constexpr size_t MAX_URI{4096};
//! The most bytes of a launch file (a logo of 32 KiB in base64, and the rest).
inline constexpr size_t MAX_LAUNCH_FILE{64 * 1024};
//! The most links of a launch.
inline constexpr size_t MAX_LINKS{4};
//! The longest link of a launch, and the longest description (bytes; the chain keeps 512 bytes for both).
inline constexpr size_t MAX_LINK{512};
inline constexpr size_t MAX_DESCRIPTION{512};
//! A display name, in bytes (the chain's rules: bitassets::WhyNotDisplayName).
inline constexpr size_t MAX_NAME{64};
//! A nonce of connect: 16 to 64 characters of [A-Za-z0-9_-].
inline constexpr size_t MIN_NONCE{16};
inline constexpr size_t MAX_NONCE{64};
//! The slippage a link may show, in basis points (display only: the floor binds).
inline constexpr int MAX_SLIPPAGE_BPS{5000};
//! How far ahead an expiry may be (a link that lives for ever is refused), and the clock difference forgiven.
inline constexpr int64_t MAX_LIFETIME{24 * 60 * 60};
inline constexpr int64_t CLOCK_SKEW{120};

//! The only site whose connect is answered, and the only callback: a constant list, never what a link says.
inline constexpr std::string_view CONNECT_ORIGIN{"https://flush.markets"};
inline constexpr std::string_view CONNECT_CALLBACK{"https://flush.markets/connect"};

//! The ticker of the chain's coin in a swap (from, to): the chain's own (chain.json).
inline constexpr std::string_view COIN_TICKER{chain::TICKER};

/** A request read from a link or a launch file. Amounts are kept as their exact decimal text (Units converts them). */
struct Request {
    Verb verb{Verb::CONNECT};
    std::string network;
    //! Unix time after which the link is refused (0: none, a launch file).
    int64_t expires{0};
    //! connect
    std::string origin;
    std::string callback;
    std::string nonce;
    //! launch: the ticker; buy, sell: the ticker of the launch (`launch=`)
    std::string ticker;
    //! launch
    std::string name;
    std::string description;
    std::vector<std::string> links;
    //! The logo's SHA-256, as sha256sum prints it (64 lowercase hex digits).
    std::optional<std::string> logo_sha256;
    //! A launch file's logo, its bytes (checked against logo_sha256 by the parser).
    std::vector<unsigned char> logo;
    //! buy, sell: the token's id
    uint256 asset;
    //! swap
    uint256 pool;
    std::string from;
    std::string to;
    //! What goes in: ECX of a buy (and of a launch's first buy: empty if none), tokens of a sale, the amount of a swap.
    std::string amount;
    //! The least that comes out (min_tokens, min_ecx, min_out): the floor that binds.
    std::string floor;
    //! What the site showed as its slippage (display only).
    std::optional<int> slippage_bps;
};

/**
 * Read an action link: refused (LinkError) unless it is exactly one of the verbs with exactly their parameters, for
 * `network` (the app's), not expired at `now` (Unix seconds) nor expiring more than MAX_LIFETIME after it.
 */
Request Parse(std::string_view uri, std::string_view network, int64_t now);

/** Whether text is an action link (the scheme, then a verb of lowercase letters and "?"), not a payment (BIP 21). */
bool IsAction(std::string_view uri);

/**
 * Read a launch file (flushlaunch.json): {format: "flush-launch", version: 1, network, ticker, name, description, links,
 * logo: {type, sha256, base64} or null, first_buy: {ecx, min_tokens, slippage_bps} or null}, no other key; its logo's
 * bytes must have the SHA-256 it says.
 */
Request ParseLaunchFile(std::string_view json, std::string_view network);

/** How a floor with more places than its asset has is taken: never lowered (rounded up), or refused (an amount paid in). */
enum class Rounding { EXACT, UP };
/**
 * A decimal of a link in units of an asset of `decimals` places: EXACT refuses more places than the asset has, UP rounds
 * them up. Nothing above `max`.
 */
std::optional<uint64_t> Units(const std::string& decimal, int decimals, Rounding rounding, uint64_t max = UINT64_MAX);
/** Units as a decimal: 1.5 for 150 units of 2 places, without trailing zeros. */
std::string Decimal(uint64_t units, int decimals);

/** The answer of a connect: the callback, its fragment with the address, the nonce and the network (all checked text). */
std::string ConnectAnswer(const Request& request, const std::string& address);

} // namespace webui::links

#endif // BITCOIN_WEBUI_LINKS_H
