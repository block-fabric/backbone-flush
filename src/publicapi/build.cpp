// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <publicapi/build.h>

#include <crypto/sha256.h>
#include <tinyformat.h>
#include <crypto/hex_base.h>
#include <util/strencodings.h>

namespace publicapi {

std::string PercentEncode(std::string_view value)
{
    std::string out;
    for (const unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out += strprintf("%%%02X", c);
        }
    }
    return out;
}

uint64_t FloorUnits(uint64_t units, int decimals, int max_places)
{
    uint64_t step{1};
    for (int i{max_places}; i < decimals; ++i) step *= 10;
    return units - units % step;
}

std::string Decimal(uint64_t units, int decimals, int max_places)
{
    units = FloorUnits(units, decimals, max_places);
    if (decimals == 0) return strprintf("%u", units);
    uint64_t scale{1};
    for (int i{0}; i < decimals; ++i) scale *= 10;
    std::string text{strprintf("%u.%0*u", units / scale, decimals, units % scale)};
    text.erase(text.find_last_not_of('0') + 1);
    if (text.back() == '.') text.pop_back();
    return text;
}

std::string ActionUri(const std::string& verb, const std::string& network, int64_t expires, const UriParams& params)
{
    std::string uri{"flush:" + verb + "?v=1&network=" + PercentEncode(network) + strprintf("&exp=%d", expires)};
    for (const auto& [key, value] : params) uri += "&" + key + "=" + PercentEncode(value);
    return uri;
}

std::string CanonicalJson(const std::map<std::string, UniValue>& fields)
{
    // std::map orders the keys by their bytes; UniValue writes them in the order pushed, with no spaces.
    UniValue obj(UniValue::VOBJ);
    for (const auto& [key, value] : fields) obj.pushKV(key, value);
    return obj.write();
}

std::string ActionHash(const std::string& payload)
{
    static constexpr std::string_view DOMAIN{"flush-action/v1\n"};
    unsigned char hash[CSHA256::OUTPUT_SIZE];
    CSHA256().Write(reinterpret_cast<const unsigned char*>(DOMAIN.data()), DOMAIN.size())
        .Write(reinterpret_cast<const unsigned char*>(payload.data()), payload.size())
        .Finalize(hash);
    return HexStr(hash);
}

std::optional<uint256> ParseBytes32(const std::string& hex)
{
    if (hex.size() != 64 || !IsHex(hex)) return std::nullopt;
    const std::vector<unsigned char> bytes{ParseHex(hex)};
    uint256 out;
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

std::string HexBytes32(const uint256& bytes)
{
    return HexStr(bytes);
}

} // namespace publicapi
