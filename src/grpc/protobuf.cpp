// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <grpc/protobuf.h>

#include <tinyformat.h>
#include <util/strencodings.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <tuple>
#include <limits>
#include <stdexcept>

namespace pb {

namespace {

constexpr int MAX_DEPTH{100};

enum WireType : uint8_t { VARINT = 0, I64 = 1, LEN = 2, SGROUP = 3, EGROUP = 4, I32 = 5 };

/** Reads the binary format. */
class Reader
{
public:
    explicit Reader(std::span<const uint8_t> data) : m_data{data} {}
    bool AtEnd() const { return m_pos >= m_data.size(); }
    bool Varint(uint64_t& value)
    {
        value = 0;
        for (int shift{0}; shift < 64; shift += 7) {
            if (m_pos >= m_data.size()) return false;
            const uint8_t byte{m_data[m_pos++]};
            if (shift == 63 && byte > 1) return false;
            value |= uint64_t{byte & 0x7fu} << shift;
            if (!(byte & 0x80)) return true;
        }
        return false;
    }
    bool Fixed(size_t size, uint64_t& value)
    {
        if (m_data.size() - m_pos < size) return false;
        value = 0;
        for (size_t i{0}; i < size; ++i) value |= uint64_t{m_data[m_pos + i]} << (8 * i);
        m_pos += size;
        return true;
    }
    bool Bytes(std::span<const uint8_t>& bytes)
    {
        uint64_t size;
        if (!Varint(size) || size > m_data.size() - m_pos) return false;
        bytes = m_data.subspan(m_pos, size);
        m_pos += size;
        return true;
    }
    bool Tag(int32_t& number, uint8_t& wire_type)
    {
        uint64_t tag;
        if (!Varint(tag) || tag > 0xffffffffu) return false;
        number = static_cast<int32_t>(tag >> 3);
        wire_type = tag & 7;
        return number > 0;
    }
    /** Skip a value of `wire_type` (a group: up to its end tag). */
    bool Skip(int32_t number, uint8_t wire_type, int depth = 0)
    {
        uint64_t value;
        std::span<const uint8_t> bytes;
        switch (wire_type) {
        case VARINT: return Varint(value);
        case I64: return Fixed(8, value);
        case I32: return Fixed(4, value);
        case LEN: return Bytes(bytes);
        case SGROUP:
            if (depth > MAX_DEPTH) return false;
            for (;;) {
                int32_t n;
                uint8_t w;
                if (!Tag(n, w)) return false;
                if (w == EGROUP) return n == number;
                if (!Skip(n, w, depth + 1)) return false;
            }
        default: return false;
        }
    }

private:
    std::span<const uint8_t> m_data;
    size_t m_pos{0};
};

void AppendTag(std::string& out, int32_t number, uint8_t wire_type)
{
    AppendVarint(out, (uint64_t(uint32_t(number)) << 3) | wire_type);
}

void AppendFixed(std::string& out, uint64_t value, size_t size)
{
    for (size_t i{0}; i < size; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}

bool IsVarintType(FieldType t)
{
    switch (t) {
    case FieldType::INT64: case FieldType::UINT64: case FieldType::INT32: case FieldType::BOOL:
    case FieldType::UINT32: case FieldType::ENUM: case FieldType::SINT32: case FieldType::SINT64:
        return true;
    default:
        return false;
    }
}
bool IsFixed64Type(FieldType t) { return t == FieldType::DOUBLE || t == FieldType::FIXED64 || t == FieldType::SFIXED64; }
bool IsFixed32Type(FieldType t) { return t == FieldType::FLOAT || t == FieldType::FIXED32 || t == FieldType::SFIXED32; }
bool IsFloat(FieldType t) { return t == FieldType::DOUBLE || t == FieldType::FLOAT; }
bool IsSigned(FieldType t)
{
    return t == FieldType::INT32 || t == FieldType::INT64 || t == FieldType::SINT32 || t == FieldType::SINT64 ||
           t == FieldType::SFIXED32 || t == FieldType::SFIXED64 || t == FieldType::ENUM;
}
bool Is64Bit(FieldType t)
{
    return t == FieldType::INT64 || t == FieldType::UINT64 || t == FieldType::SINT64 || t == FieldType::FIXED64 || t == FieldType::SFIXED64;
}

/** A value as read from the wire, into the representation (u holds the integer as int64 for signed types). */
bool DecodeScalar(const FieldDesc& field, uint64_t raw, Message::Value& value)
{
    switch (field.type) {
    case FieldType::DOUBLE: value.d = std::bit_cast<double>(raw); return true;
    case FieldType::FLOAT: value.d = std::bit_cast<float>(static_cast<uint32_t>(raw)); return true;
    case FieldType::INT32: case FieldType::ENUM: value.u = static_cast<uint64_t>(int64_t{static_cast<int32_t>(raw)}); return true;
    case FieldType::UINT32: case FieldType::FIXED32: value.u = static_cast<uint32_t>(raw); return true;
    case FieldType::SFIXED32: value.u = static_cast<uint64_t>(int64_t{static_cast<int32_t>(static_cast<uint32_t>(raw))}); return true;
    case FieldType::SINT32: { const uint32_t v{static_cast<uint32_t>(raw)}; value.u = static_cast<uint64_t>(int64_t{static_cast<int32_t>((v >> 1) ^ (~(v & 1) + 1))}); return true; }
    case FieldType::SINT64: value.u = (raw >> 1) ^ (~(raw & 1) + 1); return true;
    case FieldType::BOOL: value.u = raw != 0; return true;
    default: value.u = raw; return true;
    }
}

void EncodeScalar(std::string& out, const FieldDesc& field, const Message::Value& value)
{
    switch (field.type) {
    case FieldType::DOUBLE: AppendFixed(out, std::bit_cast<uint64_t>(value.d), 8); break;
    case FieldType::FLOAT: AppendFixed(out, std::bit_cast<uint32_t>(static_cast<float>(value.d)), 4); break;
    case FieldType::FIXED64: case FieldType::SFIXED64: AppendFixed(out, value.u, 8); break;
    case FieldType::FIXED32: case FieldType::SFIXED32: AppendFixed(out, value.u & 0xffffffffu, 4); break;
    case FieldType::SINT32: { const int32_t v{static_cast<int32_t>(value.u)}; AppendVarint(out, (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31)); break; }
    case FieldType::SINT64: { const int64_t v{static_cast<int64_t>(value.u)}; AppendVarint(out, (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63)); break; }
    case FieldType::UINT32: AppendVarint(out, value.u & 0xffffffffu); break;
    case FieldType::BOOL: AppendVarint(out, value.u ? 1 : 0); break;
    // int32 and enum: a negative value is sign extended to 64 bits (ten bytes), as protobuf does.
    default: AppendVarint(out, value.u); break;
    }
}

uint8_t ScalarWireType(FieldType t)
{
    if (IsFixed64Type(t)) return I64;
    if (IsFixed32Type(t)) return I32;
    return VARINT;
}

bool IsDefault(const FieldDesc& field, const Message::Value& value)
{
    switch (field.type) {
    case FieldType::STRING: case FieldType::BYTES: return value.s.empty();
    case FieldType::MESSAGE: case FieldType::GROUP: return false;
    case FieldType::DOUBLE: case FieldType::FLOAT: return value.d == 0 && !std::signbit(value.d);
    default: return value.u == 0;
    }
}

bool ValidUtf8(std::string_view s)
{
    size_t i{0};
    while (i < s.size()) {
        const uint8_t c{static_cast<uint8_t>(s[i])};
        size_t n;
        uint32_t cp;
        if (c < 0x80) { ++i; continue; }
        if ((c & 0xe0) == 0xc0) { n = 1; cp = c & 0x1f; }
        else if ((c & 0xf0) == 0xe0) { n = 2; cp = c & 0x0f; }
        else if ((c & 0xf8) == 0xf0) { n = 3; cp = c & 0x07; }
        else return false;
        if (s.size() - i <= n) return false;
        for (size_t k{1}; k <= n; ++k) {
            const uint8_t cc{static_cast<uint8_t>(s[i + k])};
            if ((cc & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3f);
        }
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        i += n + 1;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Descriptors, read with the bare Reader (the descriptor types are not in the pool).

struct RawField {
    std::string name, json_name, type_name;
    int32_t number{0}, label{1}, type{0}, oneof_index{-1};
    bool proto3_optional{false};
};

bool ReadString(Reader& r, std::string& out)
{
    std::span<const uint8_t> b;
    if (!r.Bytes(b)) return false;
    out.assign(b.begin(), b.end());
    return true;
}

bool ReadMessageOptionsMapEntry(std::span<const uint8_t> data, bool& map_entry)
{
    Reader r{data};
    while (!r.AtEnd()) {
        int32_t n; uint8_t w;
        if (!r.Tag(n, w)) return false;
        if (n == 7 && w == VARINT) { uint64_t v; if (!r.Varint(v)) return false; map_entry = v != 0; }
        else if (!r.Skip(n, w)) return false;
    }
    return true;
}

bool ReadField(std::span<const uint8_t> data, RawField& f)
{
    Reader r{data};
    while (!r.AtEnd()) {
        int32_t n; uint8_t w;
        if (!r.Tag(n, w)) return false;
        uint64_t v;
        if (n == 1 && w == LEN) { if (!ReadString(r, f.name)) return false; }
        else if (n == 3 && w == VARINT) { if (!r.Varint(v)) return false; f.number = static_cast<int32_t>(v); }
        else if (n == 4 && w == VARINT) { if (!r.Varint(v)) return false; f.label = static_cast<int32_t>(v); }
        else if (n == 5 && w == VARINT) { if (!r.Varint(v)) return false; f.type = static_cast<int32_t>(v); }
        else if (n == 6 && w == LEN) { if (!ReadString(r, f.type_name)) return false; }
        else if (n == 9 && w == VARINT) { if (!r.Varint(v)) return false; f.oneof_index = static_cast<int32_t>(v); }
        else if (n == 10 && w == LEN) { if (!ReadString(r, f.json_name)) return false; }
        else if (n == 17 && w == VARINT) { if (!r.Varint(v)) return false; f.proto3_optional = v != 0; }
        else if (!r.Skip(n, w)) return false;
    }
    return true;
}

std::string JsonNameOf(const std::string& name)
{
    std::string out;
    bool upper{false};
    for (char c : name) {
        if (c == '_') { upper = true; continue; }
        out.push_back(upper ? ToUpper(c) : c);
        upper = false;
    }
    return out;
}

// Timestamps (RFC 3339, UTC), for the JSON form of google.protobuf.Timestamp.
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era{(y >= 0 ? y : y - 399) / 400};
    const unsigned yoe{static_cast<unsigned>(y - era * 400)};
    const unsigned doy{(153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1};
    const unsigned doe{yoe * 365 + yoe / 4 - yoe / 100 + doy};
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}
void CivilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d)
{
    z += 719468;
    const int64_t era{(z >= 0 ? z : z - 146096) / 146097};
    const unsigned doe{static_cast<unsigned>(z - era * 146097)};
    const unsigned yoe{(doe - doe / 1460 + doe / 36524 - doe / 146096) / 365};
    y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy{doe - (365 * yoe + yoe / 4 - yoe / 100)};
    const unsigned mp{(5 * doy + 2) / 153};
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
}

std::string FormatTimestamp(int64_t seconds, int32_t nanos)
{
    const int64_t days{seconds >= 0 ? seconds / 86400 : (seconds - 86399) / 86400};
    const int64_t secs{seconds - days * 86400};
    int64_t y; unsigned m, d;
    CivilFromDays(days, y, m, d);
    std::string out{strprintf("%04d-%02d-%02dT%02d:%02d:%02d", y, m, d, secs / 3600, (secs / 60) % 60, secs % 60)};
    if (nanos) {
        if (nanos % 1000000 == 0) out += strprintf(".%03d", nanos / 1000000);
        else if (nanos % 1000 == 0) out += strprintf(".%06d", nanos / 1000);
        else out += strprintf(".%09d", nanos);
    }
    return out + "Z";
}

bool ParseTimestamp(std::string_view s, int64_t& seconds, int32_t& nanos)
{
    // YYYY-MM-DDTHH:MM:SS[.fraction](Z|+HH:MM|-HH:MM)
    const auto digits{[&](size_t pos, size_t n, int& out) {
        if (pos + n > s.size()) return false;
        out = 0;
        for (size_t i{pos}; i < pos + n; ++i) {
            if (s[i] < '0' || s[i] > '9') return false;
            out = out * 10 + (s[i] - '0');
        }
        return true;
    }};
    int Y, M, D, h, mi, se;
    if (!digits(0, 4, Y) || s.size() < 20 || s[4] != '-' || !digits(5, 2, M) || s[7] != '-' || !digits(8, 2, D) ||
        (s[10] != 'T' && s[10] != 't') || !digits(11, 2, h) || s[13] != ':' || !digits(14, 2, mi) || s[16] != ':' || !digits(17, 2, se)) return false;
    if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || mi > 59 || se > 59) return false;
    size_t pos{19};
    nanos = 0;
    if (pos < s.size() && s[pos] == '.') {
        ++pos;
        int count{0};
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
            if (count == 9) return false;
            nanos = nanos * 10 + (s[pos] - '0');
            ++count;
            ++pos;
        }
        if (count == 0) return false;
        for (; count < 9; ++count) nanos *= 10;
    }
    int64_t offset{0};
    if (pos < s.size() && (s[pos] == 'Z' || s[pos] == 'z')) {
        ++pos;
    } else if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
        int oh, om;
        if (!digits(pos + 1, 2, oh) || pos + 3 >= s.size() || s[pos + 3] != ':' || !digits(pos + 4, 2, om)) return false;
        offset = (s[pos] == '+' ? 1 : -1) * (oh * 3600 + om * 60);
        pos += 6;
    } else {
        return false;
    }
    if (pos != s.size()) return false;
    seconds = DaysFromCivil(Y, M, D) * 86400 + h * 3600 + mi * 60 + se - offset;
    return true;
}

bool IsWrapper(const std::string& name)
{
    return name == "google.protobuf.DoubleValue" || name == "google.protobuf.FloatValue" || name == "google.protobuf.Int64Value" ||
           name == "google.protobuf.UInt64Value" || name == "google.protobuf.Int32Value" || name == "google.protobuf.UInt32Value" ||
           name == "google.protobuf.BoolValue" || name == "google.protobuf.StringValue" || name == "google.protobuf.BytesValue";
}

/** Parse a JSON number or numeric string as an integer of `field`'s type. */
bool JsonInteger(const FieldDesc& field, const UniValue& json, uint64_t& out)
{
    std::string text;
    if (json.isNum()) text = json.getValStr();
    else if (json.isStr()) text = json.get_str();
    else return false;
    // An exponent or fraction is accepted when the value is integral (1e3, 5.0), as the mapping allows.
    if (text.find_first_of(".eE") != std::string::npos) {
        const double d{std::strtod(text.c_str(), nullptr)};
        if (!std::isfinite(d) || d != std::floor(d) || std::abs(d) > 9.007199254740992e15) return false;
        text = strprintf("%.0f", d);
    }
    if (IsSigned(field.type)) {
        const auto v{ToIntegral<int64_t>(text)};
        if (!v) return false;
        if (!Is64Bit(field.type) && (*v < std::numeric_limits<int32_t>::min() || *v > std::numeric_limits<int32_t>::max())) return false;
        out = static_cast<uint64_t>(*v);
    } else {
        const auto v{ToIntegral<uint64_t>(text)};
        if (!v) return false;
        if (!Is64Bit(field.type) && *v > std::numeric_limits<uint32_t>::max()) return false;
        out = *v;
    }
    return true;
}

} // namespace

void AppendVarint(std::string& out, uint64_t value)
{
    while (value >= 0x80) {
        out.push_back(static_cast<char>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
}

std::string Base64Encode(std::string_view data) { return EncodeBase64(MakeUCharSpan(data)); }

std::optional<std::string> Base64Decode(std::string_view text)
{
    // Standard or URL-safe alphabet, padding optional (the proto3 JSON mapping takes either).
    std::string s{text};
    for (char& c : s) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (s.size() % 4) s.push_back('=');
    const auto bytes{DecodeBase64(s)};
    if (!bytes) return std::nullopt;
    return std::string(bytes->begin(), bytes->end());
}

const std::string* EnumDesc::Name(int32_t number) const
{
    for (const auto& [name, value] : values) {
        if (value == number) return &name;
    }
    return nullptr;
}

std::optional<int32_t> EnumDesc::Number(std::string_view name) const
{
    for (const auto& [n, value] : values) {
        if (n == name) return value;
    }
    return std::nullopt;
}

bool FieldDesc::IsMap() const { return repeated && message && message->map_entry; }

const FieldDesc* MessageDesc::Field(int32_t number) const
{
    const auto it{std::lower_bound(fields.begin(), fields.end(), number, [](const FieldDesc& f, int32_t n) { return f.number < n; })};
    return it != fields.end() && it->number == number ? &*it : nullptr;
}

const FieldDesc* MessageDesc::FieldByName(std::string_view name) const
{
    for (const FieldDesc& f : fields) {
        if (f.name == name) return &f;
    }
    for (const FieldDesc& f : fields) {
        if (f.json_name == name) return &f;
    }
    return nullptr;
}

bool Pool::Load(std::span<const uint8_t> file_descriptor_set, std::string& error)
{
    std::vector<std::tuple<ServiceDesc*, size_t, std::string, std::string>> pending_method_types;
    std::vector<std::string> file_symbols;
    // Messages in one pass, nested ones by their full names; type names resolved afterwards.
    const auto read_enum{[&](std::span<const uint8_t> data, const std::string& scope) -> bool {
        Reader r{data};
        auto e{std::make_unique<EnumDesc>()};
        while (!r.AtEnd()) {
            int32_t n; uint8_t w;
            if (!r.Tag(n, w)) return false;
            if (n == 1 && w == LEN) {
                std::string name;
                if (!ReadString(r, name)) return false;
                e->full_name = scope.empty() ? name : scope + "." + name;
            } else if (n == 2 && w == LEN) {
                std::span<const uint8_t> vb;
                if (!r.Bytes(vb)) return false;
                Reader vr{vb};
                std::string vname;
                int32_t vnum{0};
                while (!vr.AtEnd()) {
                    int32_t vn; uint8_t vw;
                    if (!vr.Tag(vn, vw)) return false;
                    uint64_t v;
                    if (vn == 1 && vw == LEN) { if (!ReadString(vr, vname)) return false; }
                    else if (vn == 2 && vw == VARINT) { if (!vr.Varint(v)) return false; vnum = static_cast<int32_t>(v); }
                    else if (!vr.Skip(vn, vw)) return false;
                }
                e->values.emplace_back(vname, vnum);
            } else if (!r.Skip(n, w)) {
                return false;
            }
        }
        const std::string name{e->full_name};
        file_symbols.push_back(name);
        m_enums[name] = std::move(e);
        return true;
    }};
    std::function<bool(std::span<const uint8_t>, const std::string&)> read_message;
    read_message = [&](std::span<const uint8_t> data, const std::string& scope) -> bool {
        Reader r{data};
        auto m{std::make_unique<MessageDesc>()};
        std::vector<RawField> raw;
        std::vector<std::pair<std::span<const uint8_t>, bool>> nested; // (data, is_enum)
        std::string name;
        while (!r.AtEnd()) {
            int32_t n; uint8_t w;
            if (!r.Tag(n, w)) return false;
            std::span<const uint8_t> b;
            if (n == 1 && w == LEN) {
                if (!ReadString(r, name)) return false;
            } else if (n == 2 && w == LEN) {
                if (!r.Bytes(b)) return false;
                RawField f;
                if (!ReadField(b, f)) return false;
                raw.push_back(std::move(f));
            } else if ((n == 3 || n == 4) && w == LEN) {
                if (!r.Bytes(b)) return false;
                nested.emplace_back(b, n == 4);
            } else if (n == 7 && w == LEN) {
                if (!r.Bytes(b) || !ReadMessageOptionsMapEntry(b, m->map_entry)) return false;
            } else if (n == 8 && w == LEN) {
                if (!r.Bytes(b)) return false;
                Reader orr{b};
                std::string oname;
                while (!orr.AtEnd()) {
                    int32_t on; uint8_t ow;
                    if (!orr.Tag(on, ow)) return false;
                    if (on == 1 && ow == LEN) { if (!ReadString(orr, oname)) return false; }
                    else if (!orr.Skip(on, ow)) return false;
                }
                m->oneofs.push_back(oname);
            } else if (!r.Skip(n, w)) {
                return false;
            }
        }
        m->full_name = scope.empty() ? name : scope + "." + name;
        file_symbols.push_back(m->full_name);
        for (const auto& [b, is_enum] : nested) {
            if (is_enum ? !read_enum(b, m->full_name) : !read_message(b, m->full_name)) return false;
        }
        for (RawField& f : raw) {
            FieldDesc fd;
            fd.name = f.name;
            fd.json_name = f.json_name.empty() ? JsonNameOf(f.name) : f.json_name;
            fd.number = f.number;
            fd.type = static_cast<FieldType>(f.type);
            fd.repeated = f.label == 3;
            fd.type_name = f.type_name.starts_with('.') ? f.type_name.substr(1) : f.type_name;
            if (f.proto3_optional) {
                fd.explicit_presence = true;
            } else if (f.oneof_index >= 0) {
                fd.explicit_presence = true;
                fd.oneof_index = f.oneof_index;
            }
            if (fd.type == FieldType::MESSAGE || fd.type == FieldType::GROUP) fd.explicit_presence = !fd.repeated;
            m->fields.push_back(std::move(fd));
        }
        std::sort(m->fields.begin(), m->fields.end(), [](const FieldDesc& a, const FieldDesc& b) { return a.number < b.number; });
        const std::string full{m->full_name};
        m_messages[full] = std::move(m);
        return true;
    };

    Reader set{file_descriptor_set};
    while (!set.AtEnd()) {
        int32_t n; uint8_t w;
        if (!set.Tag(n, w)) { error = "malformed descriptor set"; return false; }
        if (n != 1 || w != LEN) {
            if (!set.Skip(n, w)) { error = "malformed descriptor set"; return false; }
            continue;
        }
        std::span<const uint8_t> file;
        if (!set.Bytes(file)) { error = "malformed descriptor set"; return false; }
        // The package and the name first, wherever they are.
        std::string filename, package;
        std::vector<std::string> deps;
        {
            Reader r{file};
            while (!r.AtEnd()) {
                int32_t fn; uint8_t fw;
                if (!r.Tag(fn, fw)) { error = "malformed file descriptor"; return false; }
                if (fn == 1 && fw == LEN) { if (!ReadString(r, filename)) return false; }
                else if (fn == 2 && fw == LEN) { if (!ReadString(r, package)) return false; }
                else if (fn == 3 && fw == LEN) { std::string d; if (!ReadString(r, d)) return false; deps.push_back(d); }
                else if (!r.Skip(fn, fw)) { error = "malformed file descriptor"; return false; }
            }
        }
        m_files[filename] = std::string(file.begin(), file.end());
        m_file_deps[filename] = deps;
        file_symbols.clear();
        Reader r{file};
        while (!r.AtEnd()) {
            int32_t fn; uint8_t fw;
            if (!r.Tag(fn, fw)) return false;
            std::span<const uint8_t> b;
            if (fw == LEN && (fn == 4 || fn == 5 || fn == 6)) {
                if (!r.Bytes(b)) return false;
                if (fn == 4) {
                    if (!read_message(b, package)) { error = "malformed message descriptor in " + filename; return false; }
                } else if (fn == 5) {
                    if (!read_enum(b, package)) { error = "malformed enum descriptor in " + filename; return false; }
                } else {
                    auto s{std::make_unique<ServiceDesc>()};
                    Reader sr{b};
                    std::string sname;
                    std::vector<std::tuple<std::string, std::string, std::string, bool, bool, bool>> methods;
                    while (!sr.AtEnd()) {
                        int32_t sn; uint8_t sw;
                        if (!sr.Tag(sn, sw)) return false;
                        std::span<const uint8_t> mb;
                        if (sn == 1 && sw == LEN) {
                            if (!ReadString(sr, sname)) return false;
                        } else if (sn == 2 && sw == LEN) {
                            if (!sr.Bytes(mb)) return false;
                            Reader mr{mb};
                            std::string mname, in, out;
                            bool cs{false}, ss{false}, no_side_effects{false};
                            while (!mr.AtEnd()) {
                                int32_t mn; uint8_t mw;
                                if (!mr.Tag(mn, mw)) return false;
                                uint64_t v;
                                if (mn == 1 && mw == LEN) { if (!ReadString(mr, mname)) return false; }
                                else if (mn == 2 && mw == LEN) { if (!ReadString(mr, in)) return false; }
                                else if (mn == 3 && mw == LEN) { if (!ReadString(mr, out)) return false; }
                                else if (mn == 5 && mw == VARINT) { if (!mr.Varint(v)) return false; cs = v; }
                                else if (mn == 6 && mw == VARINT) { if (!mr.Varint(v)) return false; ss = v; }
                                else if (mn == 4 && mw == LEN) {
                                    // MethodOptions: idempotency_level = 34 (NO_SIDE_EFFECTS = 1).
                                    std::span<const uint8_t> ob;
                                    if (!mr.Bytes(ob)) return false;
                                    Reader opr{ob};
                                    while (!opr.AtEnd()) {
                                        int32_t on; uint8_t ow;
                                        if (!opr.Tag(on, ow)) return false;
                                        if (on == 34 && ow == VARINT) { uint64_t lv; if (!opr.Varint(lv)) return false; no_side_effects = lv == 1; }
                                        else if (!opr.Skip(on, ow)) return false;
                                    }
                                }
                                else if (!mr.Skip(mn, mw)) return false;
                            }
                            methods.emplace_back(mname, in, out, cs, ss, no_side_effects);
                        } else if (!sr.Skip(sn, sw)) {
                            return false;
                        }
                    }
                    s->full_name = package.empty() ? sname : package + "." + sname;
                    for (const auto& [mname, in, out, cs, ss, nse] : methods) {
                        MethodDesc md;
                        md.name = mname;
                        // Resolved below; the names kept for now in a side table.
                        md.client_streaming = cs;
                        md.server_streaming = ss;
                        md.no_side_effects = nse;
                        s->methods.push_back(md);
                        m_symbol_files[s->full_name + "." + mname] = filename;
                        pending_method_types.emplace_back(s.get(), s->methods.size() - 1, in, out);
                    }
                    file_symbols.push_back(s->full_name);
                    m_services.push_back(std::move(s));
                }
            } else if (!r.Skip(fn, fw)) {
                return false;
            }
        }
        for (const std::string& symbol : file_symbols) m_symbol_files[symbol] = filename;
    }
    for (auto& [name, m] : m_messages) {
        for (FieldDesc& f : m->fields) {
            if (f.type == FieldType::MESSAGE || f.type == FieldType::GROUP) {
                f.message = FindMessage(f.type_name);
                if (!f.message) { error = "unknown type " + f.type_name; return false; }
            } else if (f.type == FieldType::ENUM) {
                f.enum_type = FindEnum(f.type_name);
                if (!f.enum_type) { error = "unknown enum " + f.type_name; return false; }
            }
        }
    }
    for (const auto& [service, index, in, out] : pending_method_types) {
        MethodDesc& md{service->methods[index]};
        md.input = FindMessage(in.starts_with('.') ? in.substr(1) : in);
        md.output = FindMessage(out.starts_with('.') ? out.substr(1) : out);
        if (!md.input || !md.output) { error = "unknown method type in " + service->full_name; return false; }
    }
    pending_method_types.clear();
    return true;
}

const MessageDesc* Pool::FindMessage(std::string_view full_name) const
{
    const auto it{m_messages.find(full_name)};
    return it == m_messages.end() ? nullptr : it->second.get();
}

const EnumDesc* Pool::FindEnum(std::string_view full_name) const
{
    const auto it{m_enums.find(full_name)};
    return it == m_enums.end() ? nullptr : it->second.get();
}

const ServiceDesc* Pool::FindService(std::string_view full_name) const
{
    for (const auto& s : m_services) {
        if (s->full_name == full_name) return s.get();
    }
    return nullptr;
}

const std::string* Pool::FileProto(std::string_view filename) const
{
    const auto it{m_files.find(filename)};
    return it == m_files.end() ? nullptr : &it->second;
}

const std::string* Pool::FileOfSymbol(std::string_view symbol) const
{
    const auto it{m_symbol_files.find(symbol)};
    return it == m_symbol_files.end() ? nullptr : &it->second;
}

std::vector<std::string> Pool::FileWithDependencies(const std::string& filename) const
{
    std::vector<std::string> out;
    std::vector<std::string> todo{filename};
    while (!todo.empty()) {
        const std::string f{todo.back()};
        todo.pop_back();
        if (std::find(out.begin(), out.end(), f) != out.end() || !m_files.contains(f)) continue;
        out.push_back(f);
        const auto it{m_file_deps.find(f)};
        if (it != m_file_deps.end()) todo.insert(todo.end(), it->second.rbegin(), it->second.rend());
    }
    return out;
}

// ---------------------------------------------------------------------------
// Messages

Message::Value::Value(const Value& other) : u{other.u}, d{other.d}, s{other.s}, m{other.m ? std::make_unique<Message>(*other.m) : nullptr} {}

Message::Value& Message::Value::operator=(const Value& other)
{
    if (this != &other) {
        u = other.u;
        d = other.d;
        s = other.s;
        m = other.m ? std::make_unique<Message>(*other.m) : nullptr;
    }
    return *this;
}

bool operator==(const Message::Value& a, const Message::Value& b)
{
    if (a.u != b.u || a.d != b.d || a.s != b.s || bool(a.m) != bool(b.m)) return false;
    return !a.m || *a.m == *b.m;
}

Message::Message(const Message& other) = default;
Message& Message::operator=(const Message& other) = default;
Message::~Message() = default;

bool operator==(const Message& a, const Message& b) { return a.m_desc == b.m_desc && a.m_fields == b.m_fields; }

const FieldDesc& Message::Lookup(std::string_view name) const
{
    const FieldDesc* f{m_desc->FieldByName(name)};
    if (!f) throw std::logic_error(strprintf("%s has no field %s", m_desc->full_name, std::string{name}));
    return *f;
}

std::vector<Message::Value>& Message::Slot(const FieldDesc& field)
{
    // Setting a member of a oneof clears the others.
    if (field.oneof_index >= 0) {
        for (const FieldDesc& other : m_desc->fields) {
            if (other.oneof_index == field.oneof_index && other.number != field.number) m_fields.erase(other.number);
        }
    }
    return m_fields[field.number];
}

const Message::Value* Message::Singular(const FieldDesc& field) const
{
    const auto it{m_fields.find(field.number)};
    return it == m_fields.end() || it->second.empty() ? nullptr : &it->second.back();
}

bool Message::Has(std::string_view name) const { return Singular(Lookup(name)) != nullptr; }

size_t Message::Count(std::string_view name) const
{
    const auto it{m_fields.find(Lookup(name).number)};
    return it == m_fields.end() ? 0 : it->second.size();
}

void Message::Clear(std::string_view name) { m_fields.erase(Lookup(name).number); }

const FieldDesc* Message::WhichOneof(std::string_view oneof) const
{
    const auto it{std::find(m_desc->oneofs.begin(), m_desc->oneofs.end(), oneof)};
    if (it == m_desc->oneofs.end()) throw std::logic_error(strprintf("%s has no oneof %s", m_desc->full_name, std::string{oneof}));
    const int index{static_cast<int>(it - m_desc->oneofs.begin())};
    for (const FieldDesc& f : m_desc->fields) {
        if (f.oneof_index == index && Singular(f)) return &f;
    }
    return nullptr;
}

uint64_t Message::GetUInt(std::string_view name) const { const Value* v{Singular(Lookup(name))}; return v ? v->u : 0; }
int64_t Message::GetInt(std::string_view name) const { return static_cast<int64_t>(GetUInt(name)); }
bool Message::GetBool(std::string_view name) const { return GetUInt(name) != 0; }
double Message::GetDouble(std::string_view name) const { const Value* v{Singular(Lookup(name))}; return v ? v->d : 0; }

const std::string& Message::GetString(std::string_view name) const
{
    static const std::string empty;
    const Value* v{Singular(Lookup(name))};
    return v ? v->s : empty;
}

void Message::SetUInt(std::string_view name, uint64_t value)
{
    const FieldDesc& f{Lookup(name)};
    auto& slot{Slot(f)};
    slot.assign(1, Value{});
    slot[0].u = value;
}
void Message::SetInt(std::string_view name, int64_t value) { SetUInt(name, static_cast<uint64_t>(value)); }
void Message::SetBool(std::string_view name, bool value) { SetUInt(name, value ? 1 : 0); }
void Message::SetDouble(std::string_view name, double value)
{
    const FieldDesc& f{Lookup(name)};
    auto& slot{Slot(f)};
    slot.assign(1, Value{});
    slot[0].d = value;
}
void Message::SetString(std::string_view name, std::string value)
{
    const FieldDesc& f{Lookup(name)};
    auto& slot{Slot(f)};
    slot.assign(1, Value{});
    slot[0].s = std::move(value);
}

const Message* Message::Sub(std::string_view name) const
{
    const Value* v{Singular(Lookup(name))};
    return v ? v->m.get() : nullptr;
}

Message& Message::Mutable(std::string_view name)
{
    const FieldDesc& f{Lookup(name)};
    if (!f.message || f.repeated) throw std::logic_error(strprintf("%s.%s is not a singular message", m_desc->full_name, f.name));
    auto& slot{Slot(f)};
    if (slot.empty()) {
        slot.emplace_back();
        slot[0].m = std::make_unique<Message>(*f.message);
    }
    return *slot.back().m;
}

const Message& Message::At(std::string_view name, size_t index) const { return *m_fields.at(Lookup(name).number).at(index).m; }

Message& Message::Add(std::string_view name)
{
    const FieldDesc& f{Lookup(name)};
    if (!f.message || !f.repeated) throw std::logic_error(strprintf("%s.%s is not a repeated message", m_desc->full_name, f.name));
    auto& slot{m_fields[f.number]};
    slot.emplace_back();
    slot.back().m = std::make_unique<Message>(*f.message);
    return *slot.back().m;
}

uint64_t Message::UIntAt(std::string_view name, size_t index) const { return m_fields.at(Lookup(name).number).at(index).u; }
const std::string& Message::StringAt(std::string_view name, size_t index) const { return m_fields.at(Lookup(name).number).at(index).s; }
void Message::AddUInt(std::string_view name, uint64_t value) { Value v; v.u = value; m_fields[Lookup(name).number].push_back(std::move(v)); }
void Message::AddString(std::string_view name, std::string value) { Value v; v.s = std::move(value); m_fields[Lookup(name).number].push_back(std::move(v)); }

std::string Message::Serialize() const
{
    std::string out;
    SerializeTo(out);
    return out;
}

void Message::SerializeTo(std::string& out) const
{
    for (const FieldDesc& field : m_desc->fields) {
        const auto it{m_fields.find(field.number)};
        if (it == m_fields.end() || it->second.empty()) continue;
        const auto& values{it->second};
        if (field.IsPackable()) {
            std::string packed;
            for (const Value& v : values) EncodeScalar(packed, field, v);
            AppendTag(out, field.number, LEN);
            AppendVarint(out, packed.size());
            out += packed;
            continue;
        }
        if (!field.repeated) {
            // A singular field: its last value, unless it is a default without presence.
            const Value& v{values.back()};
            if (!field.explicit_presence && IsDefault(field, v)) continue;
            if (field.message) {
                std::string sub;
                v.m->SerializeTo(sub);
                AppendTag(out, field.number, LEN);
                AppendVarint(out, sub.size());
                out += sub;
            } else if (field.type == FieldType::STRING || field.type == FieldType::BYTES) {
                AppendTag(out, field.number, LEN);
                AppendVarint(out, v.s.size());
                out += v.s;
            } else {
                AppendTag(out, field.number, ScalarWireType(field.type));
                EncodeScalar(out, field, v);
            }
            continue;
        }
        for (const Value& v : values) {
            if (field.message) {
                std::string sub;
                v.m->SerializeTo(sub);
                AppendTag(out, field.number, LEN);
                AppendVarint(out, sub.size());
                out += sub;
            } else {
                AppendTag(out, field.number, LEN);
                AppendVarint(out, v.s.size());
                out += v.s;
            }
        }
    }
}

bool Message::Parse(std::span<const uint8_t> data, std::string& error) { return ParseImpl(data, 0, error); }

bool Message::ParseImpl(std::span<const uint8_t> data, int depth, std::string& error)
{
    if (depth > MAX_DEPTH) {
        error = "message nested too deeply";
        return false;
    }
    Reader r{data};
    while (!r.AtEnd()) {
        int32_t number;
        uint8_t wire_type;
        if (!r.Tag(number, wire_type)) {
            error = strprintf("invalid tag in %s", m_desc->full_name);
            return false;
        }
        const FieldDesc* field{m_desc->Field(number)};
        if (!field) {
            // Unknown fields are skipped (and not kept).
            if (!r.Skip(number, wire_type)) {
                error = strprintf("invalid unknown field %d in %s", number, m_desc->full_name);
                return false;
            }
            continue;
        }
        const auto bad{[&]() {
            error = strprintf("invalid value for field %s of %s", field->name, m_desc->full_name);
            return false;
        }};
        if (field->message) {
            std::span<const uint8_t> bytes;
            if (wire_type != LEN || !r.Bytes(bytes)) return bad();
            Message* target;
            if (field->repeated) {
                target = &Add(field->name);
            } else {
                // A message field seen again is merged into the one before.
                target = &Mutable(field->name);
            }
            if (!target->ParseImpl(bytes, depth + 1, error)) return false;
            continue;
        }
        if (field->type == FieldType::STRING || field->type == FieldType::BYTES) {
            std::span<const uint8_t> bytes;
            if (wire_type != LEN || !r.Bytes(bytes)) return bad();
            Value v;
            v.s.assign(bytes.begin(), bytes.end());
            if (field->type == FieldType::STRING && !ValidUtf8(v.s)) {
                error = strprintf("invalid UTF-8 in field %s of %s", field->name, m_desc->full_name);
                return false;
            }
            auto& slot{field->repeated ? m_fields[field->number] : Slot(*field)};
            if (!field->repeated) slot.clear();
            slot.push_back(std::move(v));
            continue;
        }
        // Scalars: packed (a repeated one) or one at a time.
        const auto read_one{[&](Reader& rd, uint8_t wt, Value& v) {
            uint64_t raw;
            if (wt == VARINT && IsVarintType(field->type)) { if (!rd.Varint(raw)) return false; }
            else if (wt == I64 && IsFixed64Type(field->type)) { if (!rd.Fixed(8, raw)) return false; }
            else if (wt == I32 && IsFixed32Type(field->type)) { if (!rd.Fixed(4, raw)) return false; }
            else return false;
            return DecodeScalar(*field, raw, v);
        }};
        if (wire_type == LEN && field->IsPackable()) {
            std::span<const uint8_t> bytes;
            if (!r.Bytes(bytes)) return bad();
            Reader pr{bytes};
            auto& slot{m_fields[field->number]};
            while (!pr.AtEnd()) {
                Value v;
                if (!read_one(pr, ScalarWireType(field->type), v)) return bad();
                slot.push_back(std::move(v));
            }
            continue;
        }
        Value v;
        if (!read_one(r, wire_type, v)) return bad();
        auto& slot{field->repeated ? m_fields[field->number] : Slot(*field)};
        if (!field->repeated) slot.clear();
        slot.push_back(std::move(v));
    }
    return true;
}

// ---------------------------------------------------------------------------
// JSON

namespace {

UniValue ScalarToJson(const FieldDesc& field, const Message::Value& v)
{
    switch (field.type) {
    case FieldType::BOOL: return UniValue{v.u != 0};
    case FieldType::STRING: return UniValue{v.s};
    case FieldType::BYTES: return UniValue{Base64Encode(v.s)};
    case FieldType::DOUBLE: case FieldType::FLOAT:
        if (std::isnan(v.d)) return UniValue{"NaN"};
        if (std::isinf(v.d)) return UniValue{v.d > 0 ? "Infinity" : "-Infinity"};
        return UniValue{v.d};
    case FieldType::ENUM: {
        if (const std::string* name{field.enum_type->Name(static_cast<int32_t>(v.u))}) return UniValue{*name};
        return UniValue{static_cast<int64_t>(static_cast<int32_t>(v.u))};
    }
    case FieldType::INT64: case FieldType::SINT64: case FieldType::SFIXED64: return UniValue{strprintf("%d", static_cast<int64_t>(v.u))};
    case FieldType::UINT64: case FieldType::FIXED64: return UniValue{strprintf("%u", v.u)};
    case FieldType::INT32: case FieldType::SINT32: case FieldType::SFIXED32: return UniValue{static_cast<int64_t>(static_cast<int32_t>(v.u))};
    default: return UniValue{v.u};
    }
}

} // namespace

UniValue Message::ToJson(const JsonOptions& options) const
{
    const std::string& name{m_desc->full_name};
    if (IsWrapper(name)) {
        const FieldDesc& f{*m_desc->Field(1)};
        const Value* v{Singular(f)};
        return ScalarToJson(f, v ? *v : Value{});
    }
    if (name == "google.protobuf.Timestamp") return UniValue{FormatTimestamp(GetInt("seconds"), static_cast<int32_t>(GetInt("nanos")))};
    UniValue obj(UniValue::VOBJ);
    for (const FieldDesc& field : m_desc->fields) {
        const std::string& key_name{options.proto_names ? field.name : field.json_name};
        const auto it{m_fields.find(field.number)};
        if (it == m_fields.end() || it->second.empty()) {
            if (!options.emit_unpopulated) continue;
            // A member of a real oneof that is not set is left out (another member may be); the rest are printed.
            if (field.oneof_index >= 0) continue;
            if (field.IsMap()) {
                obj.pushKV(key_name, UniValue{UniValue::VOBJ});
            } else if (field.repeated) {
                obj.pushKV(key_name, UniValue{UniValue::VARR});
            } else if (field.explicit_presence) {
                obj.pushKV(key_name, UniValue{});
            } else {
                obj.pushKV(key_name, ScalarToJson(field, Value{}));
            }
            continue;
        }
        const auto& values{it->second};
        if (field.IsMap()) {
            UniValue map(UniValue::VOBJ);
            const FieldDesc& key{*field.message->Field(1)};
            const FieldDesc& value{*field.message->Field(2)};
            for (const Value& entry : values) {
                const Value* k{entry.m->Singular(key)};
                const Value* val{entry.m->Singular(value)};
                const UniValue kj{ScalarToJson(key, k ? *k : Value{})};
                UniValue vj{value.message ? (val ? val->m->ToJson(options) : UniValue{UniValue::VOBJ}) : ScalarToJson(value, val ? *val : Value{})};
                map.pushKV(kj.isStr() ? kj.get_str() : kj.getValStr(), std::move(vj));
            }
            obj.pushKV(key_name, std::move(map));
        } else if (field.repeated) {
            UniValue arr(UniValue::VARR);
            for (const Value& v : values) arr.push_back(field.message ? v.m->ToJson(options) : ScalarToJson(field, v));
            obj.pushKV(key_name, std::move(arr));
        } else {
            const Value& v{values.back()};
            if (!field.explicit_presence && IsDefault(field, v)) {
                if (options.emit_unpopulated) obj.pushKV(key_name, ScalarToJson(field, Value{}));
                continue;
            }
            obj.pushKV(key_name, field.message ? v.m->ToJson(options) : ScalarToJson(field, v));
        }
    }
    return obj;
}

bool Message::FromJson(const UniValue& json, std::string& error) { return FromJsonImpl(json, 0, error); }

namespace {

bool ScalarFromJson(const FieldDesc& field, const UniValue& json, Message::Value& v, std::string& error)
{
    const auto bad{[&]() {
        error = strprintf("invalid value for field %s", field.name);
        return false;
    }};
    switch (field.type) {
    case FieldType::BOOL:
        if (!json.isBool()) return bad();
        v.u = json.get_bool();
        return true;
    case FieldType::STRING:
        if (!json.isStr()) return bad();
        v.s = json.get_str();
        return true;
    case FieldType::BYTES: {
        if (!json.isStr()) return bad();
        const auto bytes{Base64Decode(json.get_str())};
        if (!bytes) return bad();
        v.s = *bytes;
        return true;
    }
    case FieldType::DOUBLE: case FieldType::FLOAT:
        if (json.isNum()) { v.d = json.get_real(); return true; }
        if (json.isStr()) {
            const std::string& s{json.get_str()};
            if (s == "NaN") v.d = std::numeric_limits<double>::quiet_NaN();
            else if (s == "Infinity") v.d = std::numeric_limits<double>::infinity();
            else if (s == "-Infinity") v.d = -std::numeric_limits<double>::infinity();
            else { char* end; v.d = std::strtod(s.c_str(), &end); if (s.empty() || *end) return bad(); }
            return true;
        }
        return bad();
    case FieldType::ENUM:
        if (json.isStr()) {
            if (const auto n{field.enum_type->Number(json.get_str())}) { v.u = static_cast<uint64_t>(int64_t{*n}); return true; }
        }
        if (json.isNum()) {
            uint64_t n;
            if (!JsonInteger(field, json, n)) return bad();
            v.u = n;
            return true;
        }
        return bad();
    default:
        if (!JsonInteger(field, json, v.u)) return bad();
        return true;
    }
}

} // namespace

bool Message::FromJsonImpl(const UniValue& json, int depth, std::string& error)
{
    if (depth > MAX_DEPTH) {
        error = "message nested too deeply";
        return false;
    }
    const std::string& name{m_desc->full_name};
    if (IsWrapper(name)) {
        const FieldDesc& f{*m_desc->Field(1)};
        Value v;
        if (!ScalarFromJson(f, json, v, error)) return false;
        auto& slot{Slot(f)};
        slot.assign(1, std::move(v));
        return true;
    }
    if (name == "google.protobuf.Timestamp") {
        int64_t seconds;
        int32_t nanos;
        if (!json.isStr() || !ParseTimestamp(json.get_str(), seconds, nanos)) {
            error = "invalid timestamp";
            return false;
        }
        SetInt("seconds", seconds);
        SetInt("nanos", nanos);
        return true;
    }
    if (!json.isObject()) {
        error = strprintf("expected an object for %s", name);
        return false;
    }
    for (const std::string& key : json.getKeys()) {
        const UniValue& value{json[key]};
        const FieldDesc* field{m_desc->FieldByName(key)};
        // Unknown fields are ignored; null is the field left unset.
        if (!field || value.isNull()) continue;
        if (field->IsMap()) {
            if (!value.isObject()) { error = strprintf("expected an object for %s", field->name); return false; }
            const FieldDesc& kf{*field->message->Field(1)};
            const FieldDesc& vf{*field->message->Field(2)};
            for (const std::string& k : value.getKeys()) {
                Message& entry{Add(field->name)};
                Value kv;
                if (kf.type == FieldType::STRING) kv.s = k;
                else if (kf.type == FieldType::BOOL) { if (k != "true" && k != "false") { error = "invalid map key"; return false; } kv.u = k == "true"; }
                else if (!ScalarFromJson(kf, UniValue{k}, kv, error)) return false;
                entry.m_fields[1].push_back(std::move(kv));
                if (vf.message) {
                    Value vv;
                    vv.m = std::make_unique<Message>(*vf.message);
                    if (!vv.m->FromJsonImpl(value[k], depth + 1, error)) return false;
                    entry.m_fields[2].push_back(std::move(vv));
                } else {
                    Value vv;
                    if (!ScalarFromJson(vf, value[k], vv, error)) return false;
                    entry.m_fields[2].push_back(std::move(vv));
                }
            }
        } else if (field->repeated) {
            if (!value.isArray()) { error = strprintf("expected an array for %s", field->name); return false; }
            for (size_t i{0}; i < value.size(); ++i) {
                if (field->message) {
                    if (!Add(field->name).FromJsonImpl(value[i], depth + 1, error)) return false;
                } else {
                    Value v;
                    if (!ScalarFromJson(*field, value[i], v, error)) return false;
                    m_fields[field->number].push_back(std::move(v));
                }
            }
        } else if (field->message) {
            if (!Mutable(field->name).FromJsonImpl(value, depth + 1, error)) return false;
        } else {
            Value v;
            if (!ScalarFromJson(*field, value, v, error)) return false;
            if (field->type == FieldType::STRING && !ValidUtf8(v.s)) { error = "invalid UTF-8"; return false; }
            Slot(*field).assign(1, std::move(v));
        }
    }
    return true;
}

} // namespace pb
