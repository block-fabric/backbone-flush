// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <publicapi/rest.h>

#include <publicapi/service.h>
#include <tinyformat.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <univalue.h>

#include <cmath>
#include <map>
#include <set>
#include <vector>

namespace publicapi {

namespace {

using grpcsrv::Code;
using grpcsrv::Status;

struct RouteDef {
    const char* http_method;
    //! Segments: literal, or "{field}" that sets that field of the request.
    std::vector<std::string> pattern;
    const char* method;
    //! Fields set by the route itself.
    std::vector<std::pair<std::string, std::string>> fixed;
    //! The reply is an image (Logo), served as its bytes.
    bool raw{false};
};

const std::vector<RouteDef>& Routes()
{
    static const std::vector<RouteDef> routes{
        {"GET", {"status"}, "GetStatus", {}},
        {"GET", {"stats"}, "GetStats", {}},
        {"GET", {"fees"}, "GetFees", {}},
        {"GET", {"treasury"}, "GetTreasury", {}},
        {"GET", {"launches"}, "ListLaunches", {}},
        {"GET", {"launches", "stats"}, "GetLaunchStats", {}},
        {"GET", {"launches", "log"}, "GetLaunchLog", {}},
        {"GET", {"launches", "{id}"}, "GetLaunch", {}},
        {"GET", {"launches", "{id}", "trades"}, "ListLaunchTrades", {}},
        {"GET", {"launches", "{id}", "candles"}, "GetLaunchCandles", {}},
        {"GET", {"launches", "{id}", "holders"}, "ListLaunchHolders", {}},
        {"GET", {"launches", "{id}", "quote"}, "QuoteLaunch", {}},
        {"GET", {"launches", "{id}", "logo"}, "GetLaunchLogo", {}, true},
        {"GET", {"pools"}, "ListPools", {}},
        {"GET", {"pools", "{id}", "trades"}, "ListPoolTrades", {}},
        {"GET", {"pools", "{id}", "candles"}, "GetPoolCandles", {}},
        {"GET", {"pools", "{pool}", "ticks"}, "ListPoolTicks", {}},
        {"GET", {"pools", "{pool}"}, "GetPool", {}},
        {"GET", {"pools", "{asset_a}", "{asset_b}"}, "GetPool", {}},
        {"GET", {"positions", "{id}"}, "GetPosition", {}},
        {"GET", {"books", "{base}"}, "GetBook", {}},
        {"GET", {"books", "{base}", "{quote}"}, "GetBook", {}},
        {"GET", {"tickers", "{ticker}"}, "GetTicker", {}},
        {"GET", {"addresses", "{address}", "holdings"}, "GetHoldings", {}},
        {"GET", {"addresses", "{address}", "portfolio"}, "GetPortfolio", {}},
        {"GET", {"assets"}, "ListAssets", {}},
        {"GET", {"assets", "{id}", "logo"}, "GetAssetLogo", {}, true},
        {"GET", {"assets", "{id}"}, "GetAsset", {}},
        {"GET", {"verifications"}, "GetVerifications", {}},
        {"GET", {"quote", "swap"}, "QuoteSwap", {}},
        {"GET", {"quote", "deposit"}, "QuoteLiquidity", {{"action", "deposit"}}},
        {"GET", {"quote", "withdraw"}, "QuoteLiquidity", {{"action", "withdraw"}}},
        {"GET", {"quote", "position"}, "QuotePosition", {}},
        {"GET", {"build", "{verb}"}, "BuildAction", {}},
        {"POST", {"broadcast"}, "Broadcast", {}},
    };
    return routes;
}

std::optional<std::string> PercentDecode(std::string_view s)
{
    std::string out;
    for (size_t i{0}; i < s.size(); ++i) {
        if (s[i] == '%') {
            if (i + 2 >= s.size()) return std::nullopt;
            const int hi{HexDigit(s[i + 1])}, lo{HexDigit(s[i + 2])};
            if (hi < 0 || lo < 0) return std::nullopt;
            out.push_back(static_cast<char>(hi * 16 + lo));
            i += 2;
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    // Text only: no control character, valid UTF-8 (the request messages take strings).
    for (const unsigned char c : out) {
        if (c < 0x20 || c == 0x7f) return std::nullopt;
    }
    return out;
}

/** A query value as JSON for a field of its type. */
std::optional<UniValue> FieldValue(const pb::FieldDesc& field, const std::string& value)
{
    switch (field.type) {
    case pb::FieldType::STRING:
        return UniValue{value};
    case pb::FieldType::BOOL:
        if (value == "1" || value == "true") return UniValue{true};
        if (value == "0" || value == "false" || value.empty()) return UniValue{false};
        return std::nullopt;
    case pb::FieldType::DOUBLE:
    case pb::FieldType::FLOAT: {
        if (value.empty() || value.size() > 40) return std::nullopt;
        char* end;
        const double d{std::strtod(value.c_str(), &end)};
        if (*end || !std::isfinite(d)) return std::nullopt;
        return UniValue{d};
    }
    case pb::FieldType::UINT32:
    case pb::FieldType::UINT64:
        if (value.empty() || value.size() > 20 || value.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
        if (field.type == pb::FieldType::UINT32 && !ToIntegral<uint32_t>(value)) return std::nullopt;
        if (!ToIntegral<uint64_t>(value)) return std::nullopt;
        return UniValue{value};
    case pb::FieldType::INT32:
    case pb::FieldType::INT64:
        if (!ToIntegral<int64_t>(value) || (field.type == pb::FieldType::INT32 && !ToIntegral<int32_t>(value))) return std::nullopt;
        return UniValue{value};
    default:
        return std::nullopt;
    }
}

Status Bad(const std::string& message) { return {Code::INVALID_ARGUMENT, message}; }

} // namespace

std::variant<grpcsrv::RestMatch, Status> Route(const pb::Pool& pool, const std::string& http_method, const std::string& path, const std::string& query, const std::string& body)
{
    if (path.size() > 1024 || query.size() > 4096) return Bad("request too long");
    std::vector<std::string> segments;
    for (const std::string& part : util::SplitString(path, '/')) {
        if (part.empty()) continue;
        const auto decoded{PercentDecode(part)};
        if (!decoded || decoded->empty() || decoded->size() > 128 || decoded->find('/') != std::string::npos) return Bad("a part of the path is not one");
        segments.push_back(*decoded);
    }
    const pb::ServiceDesc* service{pool.FindService(SERVICE)};
    if (!service) return Status{Code::INTERNAL, "no service"};
    // The first route whose literal segments match (literals win over fields: they come first in the list).
    for (const RouteDef& route : Routes()) {
        if (route.pattern.size() != segments.size()) continue;
        bool match{true};
        std::vector<std::pair<std::string, std::string>> bound;
        for (size_t i{0}; i < segments.size() && match; ++i) {
            const std::string& p{route.pattern[i]};
            if (p.starts_with('{')) {
                bound.emplace_back(p.substr(1, p.size() - 2), segments[i]);
            } else {
                match = p == segments[i];
            }
        }
        if (!match) continue;
        // pools/{pool} takes an id; two segments of assets are the pair's.
        if (route.method == std::string{"GetPool"} && route.pattern.size() == 2 && (segments[1].size() != 64 || !IsHex(segments[1]))) return Status{Code::NOT_FOUND, "pools/<id>: 64 hex digits; or pools/<asset>/<asset>"};
        if (http_method != route.http_method) return Status{Code::UNIMPLEMENTED, strprintf("%s this resource", route.http_method)};
        const pb::MethodDesc* method{nullptr};
        for (const pb::MethodDesc& m : service->methods) {
            if (m.name == route.method) method = &m;
        }
        if (!method) return Status{Code::INTERNAL, "no method"};
        auto request{std::make_unique<pb::Message>(*method->input)};
        std::string error;
        if (http_method == "POST") {
            UniValue json;
            if (body.size() > (256 << 10) || !json.read(body) || !json.isObject()) return Bad("the body is a JSON object");
            if (!request->FromJson(json, error)) return Bad(error);
        }
        UniValue fields(UniValue::VOBJ);
        std::map<std::string, std::vector<std::string>> lists;
        std::set<std::string> seen;
        const auto set{[&](const std::string& name, const std::string& value) -> std::optional<Status> {
            const pb::FieldDesc* field{method->input->FieldByName(name)};
            if (!field || field->name != name || field->message) return Bad("unknown parameter " + name.substr(0, 32));
            if (field->repeated) {
                // A list: comma-separated, or the parameter repeated (a link may hold a comma: repeat it then).
                auto& list{lists[name]};
                for (const std::string& item : name == "links" ? std::vector<std::string>{value} : util::SplitString(value, ',')) {
                    if (item.empty()) continue;
                    list.push_back(item);
                    if (list.size() > 64) return Bad(name + ": too many");
                }
                return std::nullopt;
            }
            if (!seen.insert(name).second) return Bad(name + " given twice");
            const auto v{FieldValue(*field, value)};
            if (!v) return Bad(strprintf("invalid value for %s", name));
            fields.pushKV(name, *v);
            return std::nullopt;
        }};
        for (const auto& [name, value] : route.fixed) {
            if (auto refused{set(name, value)}) return *refused;
        }
        for (const auto& [name, value] : bound) {
            if (auto refused{set(name, value)}) return *refused;
        }
        size_t params{0};
        for (const std::string& part : util::SplitString(query, '&')) {
            if (part.empty()) continue;
            if (++params > 32) return Bad("too many parameters");
            const auto eq{part.find('=')};
            const auto key{PercentDecode(part.substr(0, eq))};
            const auto value{PercentDecode(eq == std::string::npos ? "" : part.substr(eq + 1))};
            if (!key || !value) return Bad("a parameter is not percent-encoded text");
            // The route's own fields are its path's: not again in the query.
            if (std::any_of(bound.begin(), bound.end(), [&](const auto& b) { return b.first == *key; }) ||
                std::any_of(route.fixed.begin(), route.fixed.end(), [&](const auto& b) { return b.first == *key; })) {
                return Bad(*key + " is in the path");
            }
            if (auto refused{set(*key, *value)}) return *refused;
        }
        for (const auto& [name, list] : lists) {
            UniValue arr(UniValue::VARR);
            for (const std::string& item : list) arr.push_back(item);
            fields.pushKV(name, std::move(arr));
        }
        if (!request->FromJson(fields, error)) return Bad(error);
        grpcsrv::RestMatch match_out;
        match_out.method_path = std::string{"/"} + SERVICE + "/" + route.method;
        match_out.request = std::move(request);
        if (route.raw) {
            match_out.raw = [](const pb::Message& reply) -> std::optional<std::pair<std::string, std::string>> {
                if (reply.Desc().full_name != "flush.dex.v1.Logo") return std::nullopt;
                return std::make_pair(reply.GetString("content_type"), reply.GetString("data"));
            };
        }
        return match_out;
    }
    return Status{Code::NOT_FOUND, "no such resource (see doc/public-api.md)"};
}

grpcsrv::RestRouter MakeRestRouter(const pb::Pool& pool)
{
    return [&pool](const std::string& method, const std::string& path, const std::string& query, const std::string& body) { return Route(pool, method, path, query, body); };
}

} // namespace publicapi
