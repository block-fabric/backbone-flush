// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/links.h>

#include <bitassets/logo.h>
#include <bitassets/state.h>
#include <crypto/sha256.h>
#include <tinyformat.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <webui/chainconfig.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <set>

namespace webui::links {

std::string_view VerbName(Verb verb)
{
    switch (verb) {
    case Verb::CONNECT: return "connect";
    case Verb::LAUNCH: return "launch";
    case Verb::BUY: return "buy";
    case Verb::SELL: return "sell";
    case Verb::SWAP: return "swap";
    }
    return "?";
}

namespace {
constexpr std::array<std::pair<std::string_view, Verb>, 5> VERBS{{
    {"connect", Verb::CONNECT},
    {"launch", Verb::LAUNCH},
    {"buy", Verb::BUY},
    {"sell", Verb::SELL},
    {"swap", Verb::SWAP},
}};

bool Lower(char c) { return c >= 'a' && c <= 'z'; }
bool Digit(char c) { return c >= '0' && c <= '9'; }
bool Unreserved(char c)
{
    return Lower(c) || (c >= 'A' && c <= 'Z') || Digit(c) || c == '-' || c == '.' || c == '_' || c == '~';
}
int HexValue(char c)
{
    if (Digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/** Whether the scheme starts `text` (case-insensitive), followed by ':'. */
bool HasScheme(std::string_view text)
{
    const std::string_view scheme{chain::URI_SCHEME};
    if (text.size() <= scheme.size() || text[scheme.size()] != ':') return false;
    for (size_t i{0}; i < scheme.size(); ++i) {
        char c{text[i]};
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != scheme[i]) return false;
    }
    return true;
}

/**
 * The code points of UTF-8 text, refused when it is not well formed, or has a character a link may not carry: a control
 * (C0 but a line break where `newlines`, DEL, C1), a mark that turns text around or hides (U+200B..U+200F, U+202A..U+202E,
 * U+2060..U+2069, U+FEFF), a private or unassigned plane's.
 */
void CheckText(const std::string& text, const char* what, bool newlines)
{
    size_t i{0};
    while (i < text.size()) {
        const auto b0{static_cast<unsigned char>(text[i])};
        uint32_t cp;
        size_t len;
        if (b0 < 0x80) {
            cp = b0;
            len = 1;
        } else if ((b0 & 0xE0) == 0xC0) {
            cp = b0 & 0x1F;
            len = 2;
        } else if ((b0 & 0xF0) == 0xE0) {
            cp = b0 & 0x0F;
            len = 3;
        } else if ((b0 & 0xF8) == 0xF0) {
            cp = b0 & 0x07;
            len = 4;
        } else {
            throw LinkError(strprintf("%s is not valid UTF-8", what));
        }
        if (i + len > text.size()) throw LinkError(strprintf("%s is not valid UTF-8", what));
        for (size_t k{1}; k < len; ++k) {
            const auto b{static_cast<unsigned char>(text[i + k])};
            if ((b & 0xC0) != 0x80) throw LinkError(strprintf("%s is not valid UTF-8", what));
            cp = (cp << 6) | (b & 0x3F);
        }
        // Overlong forms, surrogates, past U+10FFFF.
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
            throw LinkError(strprintf("%s is not valid UTF-8", what));
        }
        const bool control{(cp < 0x20 && !(newlines && cp == '\n')) || (cp >= 0x7F && cp <= 0x9F)};
        const bool hidden{(cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x2069) || cp == 0xFEFF};
        const bool other_plane{cp >= 0xE0000};
        if (control || hidden || other_plane) throw LinkError(strprintf("%s has a character a link may not carry (U+%04X)", what, cp));
        i += len;
    }
}

/** A value of the query, percent-decoded: only unreserved characters and %XX escapes are taken (a space is %20, never +). */
std::string Decode(std::string_view raw, const std::string& key)
{
    std::string out;
    out.reserve(raw.size());
    for (size_t i{0}; i < raw.size(); ++i) {
        const char c{raw[i]};
        if (c == '%') {
            if (i + 2 >= raw.size()) throw LinkError(strprintf("%s: a %% without two hex digits", key));
            const int hi{HexValue(raw[i + 1])}, lo{HexValue(raw[i + 2])};
            if (hi < 0 || lo < 0) throw LinkError(strprintf("%s: a %% without two hex digits", key));
            out.push_back(static_cast<char>(hi * 16 + lo));
            i += 2;
        } else if (Unreserved(c)) {
            out.push_back(c);
        } else {
            const auto byte{static_cast<unsigned char>(c)};
            throw LinkError(strprintf("%s: the character %s must be percent-encoded", key, byte > 0x20 && byte < 0x7F ? strprintf("'%c'", c) : strprintf("0x%02X", byte)));
        }
    }
    return out;
}

bool IsDecimal(const std::string& text)
{
    // 0 or digits without a leading zero (at most 15), then at most 8 places; no sign, no exponent.
    const size_t dot{text.find('.')};
    const std::string whole{text.substr(0, dot)};
    if (whole.empty() || whole.size() > 15 || !std::all_of(whole.begin(), whole.end(), Digit)) return false;
    if (whole.size() > 1 && whole[0] == '0') return false;
    if (dot == std::string::npos) return true;
    const std::string places{text.substr(dot + 1)};
    return !places.empty() && places.size() <= 8 && std::all_of(places.begin(), places.end(), Digit);
}

std::string Amount(const std::string& text, const char* key)
{
    if (!IsDecimal(text)) throw LinkError(strprintf("%s: an amount is a decimal of at most 8 places, without sign or exponent (\"%s\")", key, text.substr(0, 40)));
    if (text.find_first_not_of("0.") == std::string::npos) throw LinkError(strprintf("%s: the amount is 0", key));
    return text;
}

uint256 Id(const std::string& text, const char* key)
{
    if (text.size() != 64 || !std::all_of(text.begin(), text.end(), [](char c) { return Digit(c) || (c >= 'a' && c <= 'f'); })) {
        throw LinkError(strprintf("%s: an id is exactly 64 lowercase hex digits", key));
    }
    return *uint256::FromHex(text);
}

std::string Ticker(const std::string& text, const char* key, bool coin_allowed)
{
    if (coin_allowed && text == COIN_TICKER) return text;
    if (!bitassets::IsTicker(text) || text == COIN_TICKER) throw LinkError(strprintf("%s: a ticker is 2 to 12 capital letters and digits (\"%s\")", key, text.substr(0, 20)));
    return text;
}

int Slippage(const std::string& text)
{
    int bps{-1};
    const auto [end, ec]{std::from_chars(text.data(), text.data() + text.size(), bps)};
    if (ec != std::errc{} || end != text.data() + text.size() || text.empty() || (text.size() > 1 && text[0] == '0') || bps < 0 || bps > MAX_SLIPPAGE_BPS) {
        throw LinkError(strprintf("slippage_bps: whole basis points, 0 to %d", MAX_SLIPPAGE_BPS));
    }
    return bps;
}

std::string Name(const std::string& text)
{
    if (text.empty() || text.size() > MAX_NAME) throw LinkError(strprintf("name: 1 to %u bytes", MAX_NAME));
    CheckText(text, "name", false);
    if (const auto why{bitassets::WhyNotDisplayName(text)}) throw LinkError(strprintf("name: %s", *why));
    return text;
}

std::string Description(const std::string& text)
{
    if (text.size() > MAX_DESCRIPTION) throw LinkError(strprintf("desc: at most %u bytes", MAX_DESCRIPTION));
    CheckText(text, "desc", true);
    return text;
}

/** A link of a launch: https only, a host of letters, digits, dots and hyphens (and a port), no user name, printable ASCII. */
std::string WebLink(const std::string& text)
{
    if (text.size() > MAX_LINK) throw LinkError(strprintf("link: at most %u characters", MAX_LINK));
    constexpr std::string_view HTTPS{"https://"};
    if (!text.starts_with(HTTPS)) throw LinkError("link: https:// links only");
    const size_t start{HTTPS.size()};
    size_t end{text.find_first_of("/?#", start)};
    if (end == std::string::npos) end = text.size();
    const std::string authority{text.substr(start, end - start)};
    const size_t colon{authority.find(':')};
    const std::string host{authority.substr(0, colon)};
    if (host.empty() || host.size() > 253 || host.front() == '.' || host.front() == '-' ||
        !std::all_of(host.begin(), host.end(), [](char c) { return Lower(c) || (c >= 'A' && c <= 'Z') || Digit(c) || c == '.' || c == '-'; })) {
        throw LinkError("link: the host of a link is letters, digits, dots and hyphens (no user name)");
    }
    if (colon != std::string::npos) {
        const std::string port{authority.substr(colon + 1)};
        if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), Digit)) throw LinkError("link: a bad port");
    }
    constexpr std::string_view REFUSED{" \"<>\\^`{|}"};
    for (const char c : text) {
        if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7F || REFUSED.find(c) != std::string_view::npos) {
            throw LinkError("link: printable characters only, percent-encoded where needed");
        }
    }
    return text;
}

/** A SHA-256 as sha256sum prints it: 64 lowercase hex digits, in that order. */
std::string Sha256Hex(const std::string& text, const char* key)
{
    Id(text, key);
    return text;
}

std::string LogoHash(const std::string& text)
{
    constexpr std::string_view PREFIX{"sha256:"};
    if (!text.starts_with(PREFIX)) throw LinkError("logo: sha256: and the 64 hex digits of the logo's SHA-256");
    return Sha256Hex(text.substr(PREFIX.size()), "logo");
}

void CheckNetwork(const std::string& given, std::string_view network)
{
    if (given != "ecash" && given != "ecashbeta" && given != "ecashtest" && given != "regtest") throw LinkError(strprintf("network: \"%s\" is not a network of Flush", given.substr(0, 20)));
    if (given != network) {
        throw LinkError(strprintf("This link is for the network %s, and this app runs %s: nothing was done. Open it with a Flush app of %s, or ask the site for a link of %s.",
                                  given, std::string{network}, given, std::string{network}));
    }
}

void CheckLaunch(const Request& r)
{
    size_t info{r.description.size()};
    for (const std::string& link : r.links) info += link.size() + 1;
    if (!r.description.empty() && !r.links.empty()) ++info;
    if (info > bitassets::MAX_INFO_SIZE) throw LinkError(strprintf("The description and the links take %u bytes: the chain keeps %u", info, bitassets::MAX_INFO_SIZE));
    if (r.amount.empty() != r.floor.empty()) throw LinkError("A first buy has both ecx and min_tokens, or neither");
    if (r.amount.empty() && r.slippage_bps) throw LinkError("slippage_bps goes with a first buy");
}
} // namespace

bool IsAction(std::string_view uri)
{
    if (!HasScheme(uri)) return false;
    const std::string_view rest{uri.substr(chain::URI_SCHEME.size() + 1)};
    const size_t q{rest.find('?')};
    if (q == 0 || q == std::string_view::npos) return false;
    return std::all_of(rest.begin(), rest.begin() + q, Lower);
}

Request Parse(std::string_view uri, std::string_view network, int64_t now)
{
    if (uri.size() > MAX_URI) throw LinkError(strprintf("The link is too long (%u bytes; at most %u)", uri.size(), MAX_URI));
    if (!IsAction(uri)) throw LinkError("Not an action link of Flush (flush:<verb>?v=1&...)");
    const std::string_view rest{uri.substr(chain::URI_SCHEME.size() + 1)};
    const size_t q{rest.find('?')};
    const std::string verb_text{rest.substr(0, q)};
    const auto verb{std::find_if(VERBS.begin(), VERBS.end(), [&](const auto& v) { return v.first == verb_text; })};
    if (verb == VERBS.end()) throw LinkError(strprintf("\"%s\" is not an action this app knows (connect, launch, buy, sell, swap)", verb_text.substr(0, 20)));

    // The parameters, each once (link: up to MAX_LINKS times).
    std::map<std::string, std::string> params;
    std::vector<std::string> links;
    const std::string_view query{rest.substr(q + 1)};
    size_t pos{0};
    while (pos <= query.size()) {
        size_t amp{query.find('&', pos)};
        if (amp == std::string_view::npos) amp = query.size();
        const std::string_view pair{query.substr(pos, amp - pos)};
        pos = amp + 1;
        if (pair.empty()) {
            if (amp == query.size()) break;
            throw LinkError("An empty parameter (&&)");
        }
        const size_t eq{pair.find('=')};
        if (eq == std::string_view::npos || eq == 0) throw LinkError("A parameter without a name or a value");
        const std::string key{pair.substr(0, eq)};
        if (!std::all_of(key.begin(), key.end(), [](char c) { return Lower(c) || c == '_'; }) || key.size() > 32) throw LinkError("A parameter's name is lowercase letters and _");
        std::string value{Decode(pair.substr(eq + 1), key)};
        if (key == "link") {
            if (links.size() >= MAX_LINKS) throw LinkError(strprintf("At most %u links", MAX_LINKS));
            links.push_back(std::move(value));
            continue;
        }
        if (!params.emplace(key, std::move(value)).second) throw LinkError(strprintf("%s is given twice", key));
    }

    const auto take{[&](const char* key) -> std::optional<std::string> {
        const auto it{params.find(key)};
        if (it == params.end()) return std::nullopt;
        std::string v{std::move(it->second)};
        params.erase(it);
        return v;
    }};
    const auto need{[&](const char* key) {
        auto v{take(key)};
        if (!v) throw LinkError(strprintf("%s is missing", key));
        if (v->empty()) throw LinkError(strprintf("%s is empty", key));
        return *v;
    }};

    // The version first: a link of another version is refused for that, whatever else it has.
    const auto version{take("v")};
    if (!version) throw LinkError("v is missing: the app reads links of version 1");
    if (*version != "1") throw LinkError(strprintf("This link is of version %s: this app reads version 1 only (update the app)", version->substr(0, 8)));
    Request r;
    r.verb = verb->second;
    r.network = need("network");
    CheckNetwork(r.network, network);
    const std::string exp{need("exp")};
    if (exp.size() > 12 || !std::all_of(exp.begin(), exp.end(), Digit) || exp[0] == '0') throw LinkError("exp: the Unix time the link expires");
    r.expires = std::stoll(exp);
    if (now > r.expires + CLOCK_SKEW) throw LinkError("This link has expired: ask the site for a fresh one (a link is good for a few minutes, so that an old one cannot be used again)");
    if (r.expires > now + MAX_LIFETIME + CLOCK_SKEW) throw LinkError("This link would be good for more than a day: refused (check the computer's clock)");

    if (!links.empty() && r.verb != Verb::LAUNCH) throw LinkError("link is a parameter of launch only");
    switch (r.verb) {
    case Verb::CONNECT:
        r.origin = need("origin");
        r.callback = need("callback");
        r.nonce = need("nonce");
        if (r.origin != CONNECT_ORIGIN) throw LinkError(strprintf("This link asks to share an address with %s: the app answers %s only", r.origin.substr(0, 80), std::string{CONNECT_ORIGIN}));
        if (r.callback != CONNECT_CALLBACK) throw LinkError(strprintf("This link asks to send the answer to %s: the app sends it to %s only", r.callback.substr(0, 80), std::string{CONNECT_CALLBACK}));
        if (r.nonce.size() < MIN_NONCE || r.nonce.size() > MAX_NONCE ||
            !std::all_of(r.nonce.begin(), r.nonce.end(), [](char c) { return Lower(c) || (c >= 'A' && c <= 'Z') || Digit(c) || c == '-' || c == '_'; })) {
            throw LinkError(strprintf("nonce: %u to %u letters, digits, - and _", MIN_NONCE, MAX_NONCE));
        }
        break;
    case Verb::LAUNCH: {
        r.ticker = Ticker(need("ticker"), "ticker", false);
        r.name = Name(need("name"));
        if (auto logo{take("logo")}) r.logo_sha256 = LogoHash(*logo);
        if (auto desc{take("desc")}) r.description = Description(*desc);
        for (const std::string& link : links) r.links.push_back(WebLink(link));
        if (auto ecx{take("ecx")}) r.amount = Amount(*ecx, "ecx");
        if (auto floor{take("min_tokens")}) r.floor = Amount(*floor, "min_tokens");
        if (auto s{take("slippage_bps")}) r.slippage_bps = Slippage(*s);
        CheckLaunch(r);
        break;
    }
    case Verb::BUY:
    case Verb::SELL:
        r.ticker = Ticker(need("launch"), "launch", false);
        r.asset = Id(need("asset"), "asset");
        r.amount = Amount(need(r.verb == Verb::BUY ? "ecx" : "tokens"), r.verb == Verb::BUY ? "ecx" : "tokens");
        r.floor = Amount(need(r.verb == Verb::BUY ? "min_tokens" : "min_ecx"), r.verb == Verb::BUY ? "min_tokens" : "min_ecx");
        if (auto s{take("slippage_bps")}) r.slippage_bps = Slippage(*s);
        break;
    case Verb::SWAP:
        r.pool = Id(need("pool"), "pool");
        r.from = Ticker(need("from"), "from", true);
        r.to = Ticker(need("to"), "to", true);
        if (r.from == r.to) throw LinkError("from and to are the same");
        r.amount = Amount(need("amount"), "amount");
        r.floor = Amount(need("min_out"), "min_out");
        if (auto s{take("slippage_bps")}) r.slippage_bps = Slippage(*s);
        break;
    }
    // Nothing else: a parameter this app does not know (or one for a later version, req-...) is refused, never ignored.
    if (!params.empty()) throw LinkError(strprintf("%s is not a parameter of %s: refused (a link of a later version needs a newer app)", params.begin()->first, verb_text));
    return r;
}

Request ParseLaunchFile(std::string_view json, std::string_view network)
{
    if (json.size() > MAX_LAUNCH_FILE) throw LinkError(strprintf("The launch file is too large (%u bytes; at most %u)", json.size(), MAX_LAUNCH_FILE));
    UniValue doc;
    if (!doc.read(json) || !doc.isObject()) throw LinkError("Not a launch file: not a JSON object");
    const auto keys_only{[](const UniValue& obj, std::set<std::string> allowed, const char* what) {
        std::set<std::string> seen;
        for (const std::string& key : obj.getKeys()) {
            if (!seen.insert(key).second) throw LinkError(strprintf("%s: %s is given twice", what, key));
            if (!allowed.contains(key)) throw LinkError(strprintf("%s: %s is not a field of a launch file of version 1", what, key.substr(0, 40)));
        }
    }};
    keys_only(doc, {"format", "version", "network", "ticker", "name", "description", "links", "logo", "first_buy"}, "The launch file");
    const auto str{[](const UniValue& obj, const char* key, bool required) -> std::string {
        const UniValue& v{obj.find_value(key)};
        if (v.isNull() && !required) return {};
        if (!v.isStr()) throw LinkError(strprintf("The launch file: %s is text", key));
        return v.get_str();
    }};
    if (str(doc, "format", true) != "flush-launch") throw LinkError("Not a launch file: its format is not flush-launch");
    const UniValue& version{doc.find_value("version")};
    if (!version.isNum() || version.getValStr() != "1") throw LinkError("This launch file is not of version 1: update the app");
    Request r;
    r.verb = Verb::LAUNCH;
    r.network = str(doc, "network", true);
    CheckNetwork(r.network, network);
    r.ticker = Ticker(str(doc, "ticker", true), "ticker", false);
    r.name = Name(str(doc, "name", true));
    r.description = Description(str(doc, "description", false));
    const UniValue& links{doc.find_value("links")};
    if (!links.isNull()) {
        if (!links.isArray() || links.size() > MAX_LINKS) throw LinkError(strprintf("The launch file: links is a list of at most %u", MAX_LINKS));
        for (const UniValue& link : links.getValues()) {
            if (!link.isStr()) throw LinkError("The launch file: a link is text");
            r.links.push_back(WebLink(link.get_str()));
        }
    }
    const UniValue& logo{doc.find_value("logo")};
    if (!logo.isNull()) {
        if (!logo.isObject()) throw LinkError("The launch file: logo is {type, sha256, base64} or null");
        keys_only(logo, {"type", "sha256", "base64"}, "logo");
        const std::string type{str(logo, "type", true)};
        if (type != "image/png" && type != "image/jpeg") throw LinkError("logo: a PNG or a JPEG");
        r.logo_sha256 = Sha256Hex(str(logo, "sha256", true), "logo.sha256");
        const auto bytes{DecodeBase64(str(logo, "base64", true))};
        if (!bytes) throw LinkError("logo: its base64 cannot be read");
        if (bytes->size() > bitassets::MAX_LOGO_BYTES) throw LinkError(strprintf("logo: at most %u bytes", bitassets::MAX_LOGO_BYTES));
        unsigned char hash[CSHA256::OUTPUT_SIZE];
        CSHA256().Write(bytes->data(), bytes->size()).Finalize(hash);
        if (HexStr(hash) != *r.logo_sha256) throw LinkError("logo: its bytes do not have the SHA-256 the file says: refused (the file was changed)");
        r.logo = *bytes;
    }
    const UniValue& buy{doc.find_value("first_buy")};
    if (!buy.isNull()) {
        if (!buy.isObject()) throw LinkError("The launch file: first_buy is {ecx, min_tokens, slippage_bps} or null");
        keys_only(buy, {"ecx", "min_tokens", "slippage_bps"}, "first_buy");
        r.amount = Amount(str(buy, "ecx", true), "first_buy.ecx");
        r.floor = Amount(str(buy, "min_tokens", true), "first_buy.min_tokens");
        const UniValue& s{buy.find_value("slippage_bps")};
        if (!s.isNull()) {
            if (!s.isNum()) throw LinkError("first_buy.slippage_bps: whole basis points");
            r.slippage_bps = Slippage(s.getValStr());
        }
    }
    CheckLaunch(r);
    return r;
}

std::optional<uint64_t> Units(const std::string& decimal, int decimals, Rounding rounding, uint64_t max)
{
    if (!IsDecimal(decimal) || decimals < 0 || decimals > 18) return std::nullopt;
    const size_t dot{decimal.find('.')};
    const std::string whole{decimal.substr(0, dot)};
    std::string places{dot == std::string::npos ? std::string{} : decimal.substr(dot + 1)};
    bool round_up{false};
    if (places.size() > static_cast<size_t>(decimals)) {
        const std::string extra{places.substr(decimals)};
        places.resize(decimals);
        if (extra.find_first_not_of('0') != std::string::npos) {
            if (rounding == Rounding::EXACT) return std::nullopt;
            round_up = true;
        }
    }
    places.resize(decimals, '0');
    unsigned __int128 units{0};
    for (const char c : whole + places) {
        units = units * 10 + static_cast<unsigned>(c - '0');
        if (units > max) return std::nullopt;
    }
    if (round_up) ++units;
    if (units > max) return std::nullopt;
    return static_cast<uint64_t>(units);
}

std::string Decimal(uint64_t units, int decimals)
{
    std::string digits{std::to_string(units)};
    if (decimals <= 0) return digits;
    if (digits.size() <= static_cast<size_t>(decimals)) digits.insert(0, decimals - digits.size() + 1, '0');
    std::string out{digits.substr(0, digits.size() - decimals) + "." + digits.substr(digits.size() - decimals)};
    while (out.back() == '0') out.pop_back();
    if (out.back() == '.') out.pop_back();
    return out;
}

std::string ConnectAnswer(const Request& request, const std::string& address)
{
    // Every part is checked text (the callback a constant, the nonce and the network by the parser): nothing to escape, and
    // nothing that could be read as another part of the address.
    if (request.verb != Verb::CONNECT || request.callback != CONNECT_CALLBACK) throw LinkError("Not a connect");
    if (address.size() < 8 || address.size() > 100 || !std::all_of(address.begin(), address.end(), [](char c) { return Lower(c) || Digit(c); })) {
        throw LinkError("The address to share is not a bech32 address");
    }
    return strprintf("%s#address=%s&nonce=%s&network=%s", request.callback, address, request.nonce, request.network);
}

} // namespace webui::links
