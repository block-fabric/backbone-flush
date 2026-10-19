// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitassets/logo.h>

#include <array>
#include <cstring>

namespace bitassets {

namespace {
constexpr std::array<uint32_t, 256> CRC_TABLE{[] {
    std::array<uint32_t, 256> table{};
    for (uint32_t n{0}; n < 256; ++n) {
        uint32_t c{n};
        for (int k{0}; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[n] = c;
    }
    return table;
}()};

uint32_t Be32(std::span<const unsigned char> b, size_t at)
{
    return (uint32_t{b[at]} << 24) | (uint32_t{b[at + 1]} << 16) | (uint32_t{b[at + 2]} << 8) | uint32_t{b[at + 3]};
}
uint32_t Be16(std::span<const unsigned char> b, size_t at) { return (uint32_t{b[at]} << 8) | uint32_t{b[at + 1]}; }

std::optional<LogoFormat> Bad(std::string& reason, const char* why)
{
    reason = why;
    return std::nullopt;
}

constexpr unsigned char PNG_SIGNATURE[8]{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

std::optional<LogoFormat> CheckPng(std::span<const unsigned char> b, std::string& reason)
{
    size_t pos{sizeof(PNG_SIGNATURE)};
    // What has been seen, in the order the chunks must come in.
    bool ihdr{false}, plte{false}, trns{false}, idat{false};
    uint8_t depth{0}, color{0};
    uint32_t palette_entries{0};
    uint64_t idat_bytes{0};
    // The first two bytes of the image data (the zlib header), across IDAT chunks.
    unsigned char zlib[2]{};
    while (true) {
        if (b.size() - pos < 12) return Bad(reason, "bad-ba-logo-png-truncated");
        const uint32_t length{Be32(b, pos)};
        if (length > 0x7FFFFFFFu || length > b.size() - pos - 12) return Bad(reason, "bad-ba-logo-png-length");
        const std::span<const unsigned char> type{b.subspan(pos + 4, 4)};
        const std::span<const unsigned char> data{b.subspan(pos + 8, length)};
        if (Crc32(b.subspan(pos + 4, 4 + length)) != Be32(b, pos + 8 + length)) return Bad(reason, "bad-ba-logo-png-crc");
        pos += 12 + length;
        const auto is{[&](const char* name) { return std::memcmp(type.data(), name, 4) == 0; }};
        if (!ihdr) {
            if (!is("IHDR")) return Bad(reason, "bad-ba-logo-png-ihdr");
            if (length != 13) return Bad(reason, "bad-ba-logo-png-ihdr");
            if (Be32(data, 0) != LOGO_SIZE || Be32(data, 4) != LOGO_SIZE) return Bad(reason, "bad-ba-logo-size");
            depth = data[8];
            color = data[9];
            bool good;
            switch (color) {
            case 0: good = depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16; break;
            case 3: good = depth == 1 || depth == 2 || depth == 4 || depth == 8; break;
            case 2: case 4: case 6: good = depth == 8 || depth == 16; break;
            default: good = false;
            }
            if (!good) return Bad(reason, "bad-ba-logo-png-depth");
            // Compression and filter method 0; interlace 0 (Adam7 is refused: one way to lay the image out).
            if (data[10] != 0 || data[11] != 0) return Bad(reason, "bad-ba-logo-png-ihdr");
            if (data[12] != 0) return Bad(reason, "bad-ba-logo-png-interlace");
            ihdr = true;
            continue;
        }
        if (is("IEND")) {
            if (length != 0) return Bad(reason, "bad-ba-logo-png-iend");
            if (!idat || idat_bytes == 0) return Bad(reason, "bad-ba-logo-png-idat");
            if (color == 3 && !plte) return Bad(reason, "bad-ba-logo-png-plte");
            if (pos != b.size()) return Bad(reason, "bad-ba-logo-trailing");
            // deflate (CM 8), a window of at most 32 KiB (CINFO <= 7), no preset dictionary, the check of FCHECK.
            if ((zlib[0] & 0x0F) != 8 || (zlib[0] >> 4) > 7 || (zlib[1] & 0x20) || ((uint32_t{zlib[0]} << 8) | zlib[1]) % 31 != 0) {
                return Bad(reason, "bad-ba-logo-png-zlib");
            }
            return LogoFormat::PNG;
        }
        if (is("IDAT")) {
            if (color == 3 && !plte) return Bad(reason, "bad-ba-logo-png-plte");
            for (size_t i{0}; i < data.size() && idat_bytes + i < 2; ++i) zlib[idat_bytes + i] = data[i];
            idat = true;
            idat_bytes += length;
            continue;
        }
        // After the image data, only more of it (one IDAT after another) or IEND: PLTE and tRNS come before.
        if (is("PLTE")) {
            if (plte || idat || trns || color != 3) return Bad(reason, "bad-ba-logo-png-plte");
            if (length == 0 || length % 3 != 0 || length / 3 > (1u << depth)) return Bad(reason, "bad-ba-logo-png-plte");
            palette_entries = length / 3;
            plte = true;
            continue;
        }
        if (is("tRNS")) {
            if (trns || idat) return Bad(reason, "bad-ba-logo-png-trns");
            bool good;
            switch (color) {
            case 0: good = length == 2; break;
            case 2: good = length == 6; break;
            case 3: good = plte && length >= 1 && length <= palette_entries; break;
            default: good = false; // an alpha channel already
            }
            if (!good) return Bad(reason, "bad-ba-logo-png-trns");
            trns = true;
            continue;
        }
        // Any other chunk: text, time, gamma, colour profiles, private chunks, a second IHDR...
        return Bad(reason, "bad-ba-logo-png-chunk");
    }
}

std::optional<LogoFormat> CheckJpeg(std::span<const unsigned char> b, std::string& reason)
{
    size_t pos{2}; // after SOI
    bool frame{false}, scanned{false};
    bool progressive{false};
    uint8_t components{0};
    std::array<uint8_t, 3> component_ids{};
    while (true) {
        if (b.size() - pos < 2) return Bad(reason, "bad-ba-logo-jpeg-truncated");
        // No fill bytes: every marker is exactly FF and its code.
        if (b[pos] != 0xFF) return Bad(reason, "bad-ba-logo-jpeg-marker");
        const uint8_t code{b[pos + 1]};
        pos += 2;
        if (code == 0xD9) { // EOI
            if (!scanned) return Bad(reason, "bad-ba-logo-jpeg-scan");
            if (pos != b.size()) return Bad(reason, "bad-ba-logo-trailing");
            return LogoFormat::JPEG;
        }
        const bool sof{code == 0xC0 || code == 0xC2};
        if (!sof && code != 0xC4 && code != 0xDB && code != 0xDD && code != 0xDA) {
            // APPn (JFIF, Exif, XMP, ICC...), COM, other frames (extended, lossless, arithmetic), DNL, DHP, EXP, JPGn, a stray RSTn, SOI...
            return Bad(reason, (code >= 0xE0 && code <= 0xEF) || code == 0xFE ? "bad-ba-logo-jpeg-metadata" : "bad-ba-logo-jpeg-marker");
        }
        if (b.size() - pos < 2) return Bad(reason, "bad-ba-logo-jpeg-truncated");
        const uint32_t length{Be16(b, pos)};
        if (length < 2 || length > b.size() - pos) return Bad(reason, "bad-ba-logo-jpeg-length");
        const std::span<const unsigned char> seg{b.subspan(pos + 2, length - 2)};
        pos += length;
        if (sof) {
            if (frame) return Bad(reason, "bad-ba-logo-jpeg-frame");
            if (seg.size() < 6) return Bad(reason, "bad-ba-logo-jpeg-frame");
            if (seg[0] != 8) return Bad(reason, "bad-ba-logo-jpeg-frame");
            if (Be16(seg, 1) != LOGO_SIZE || Be16(seg, 3) != LOGO_SIZE) return Bad(reason, "bad-ba-logo-size");
            components = seg[5];
            if ((components != 1 && components != 3) || seg.size() != 6 + 3u * components) return Bad(reason, "bad-ba-logo-jpeg-frame");
            for (uint8_t i{0}; i < components; ++i) {
                const uint8_t id{seg[6 + 3 * i]}, sampling{seg[7 + 3 * i]}, table{seg[8 + 3 * i]};
                for (uint8_t j{0}; j < i; ++j) {
                    if (component_ids[j] == id) return Bad(reason, "bad-ba-logo-jpeg-frame");
                }
                component_ids[i] = id;
                if ((sampling >> 4) < 1 || (sampling >> 4) > 4 || (sampling & 0x0F) < 1 || (sampling & 0x0F) > 4 || table > 3) return Bad(reason, "bad-ba-logo-jpeg-frame");
            }
            frame = true;
            progressive = code == 0xC2;
        } else if (code == 0xDB) { // DQT: 8-bit tables (the frame is 8-bit), 64 values each
            if (seg.empty()) return Bad(reason, "bad-ba-logo-jpeg-dqt");
            for (size_t at{0}; at < seg.size();) {
                if (seg[at] != (seg[at] & 0x03) || seg.size() - at < 65) return Bad(reason, "bad-ba-logo-jpeg-dqt");
                at += 65;
            }
        } else if (code == 0xC4) { // DHT
            if (seg.empty()) return Bad(reason, "bad-ba-logo-jpeg-dht");
            for (size_t at{0}; at < seg.size();) {
                if (seg.size() - at < 17 || (seg[at] >> 4) > 1 || (seg[at] & 0x0F) > 3) return Bad(reason, "bad-ba-logo-jpeg-dht");
                uint32_t values{0}, code_space{0};
                for (int l{1}; l <= 16; ++l) {
                    values += seg[at + l];
                    // A prefix code: no more codes of a length than there is room for.
                    code_space += seg[at + l];
                    if (code_space > (1u << l)) return Bad(reason, "bad-ba-logo-jpeg-dht");
                    code_space <<= 1;
                }
                if (values == 0 || values > 256 || seg.size() - at - 17 < values) return Bad(reason, "bad-ba-logo-jpeg-dht");
                at += 17 + values;
            }
        } else if (code == 0xDD) { // DRI
            if (seg.size() != 2) return Bad(reason, "bad-ba-logo-jpeg-dri");
        } else { // SOS
            if (!frame) return Bad(reason, "bad-ba-logo-jpeg-scan");
            if (seg.empty()) return Bad(reason, "bad-ba-logo-jpeg-scan");
            const uint8_t count{seg[0]};
            if (count < 1 || count > components || seg.size() != 4 + 2u * count) return Bad(reason, "bad-ba-logo-jpeg-scan");
            for (uint8_t i{0}; i < count; ++i) {
                const uint8_t id{seg[1 + 2 * i]}, tables{seg[2 + 2 * i]};
                bool in_frame{false};
                for (uint8_t j{0}; j < components; ++j) in_frame |= component_ids[j] == id;
                for (uint8_t j{0}; j < i; ++j) {
                    if (seg[1 + 2 * j] == id) in_frame = false;
                }
                if (!in_frame || (tables >> 4) > 3 || (tables & 0x0F) > 3) return Bad(reason, "bad-ba-logo-jpeg-scan");
            }
            const uint8_t ss{seg[1 + 2 * count]}, se{seg[2 + 2 * count]}, ah{static_cast<uint8_t>(seg[3 + 2 * count] >> 4)}, al{static_cast<uint8_t>(seg[3 + 2 * count] & 0x0F)};
            if (progressive) {
                // DC scans (0..0), any components; AC scans (s..e, 1 <= s <= e <= 63) of one component.
                const bool dc{ss == 0 && se == 0}, ac{ss >= 1 && ss <= se && se <= 63 && count == 1};
                if ((!dc && !ac) || ah > 13 || al > 13) return Bad(reason, "bad-ba-logo-jpeg-scan");
            } else if (ss != 0 || se != 63 || ah != 0 || al != 0) {
                return Bad(reason, "bad-ba-logo-jpeg-scan");
            }
            // The entropy-coded data: up to the next marker, FF only before 00 (a stuffed byte) or a restart marker.
            const size_t start{pos};
            while (true) {
                if (pos >= b.size()) return Bad(reason, "bad-ba-logo-jpeg-truncated");
                if (b[pos] != 0xFF) {
                    ++pos;
                    continue;
                }
                if (pos + 1 >= b.size()) return Bad(reason, "bad-ba-logo-jpeg-truncated");
                const uint8_t next{b[pos + 1]};
                if (next == 0x00 || (next >= 0xD0 && next <= 0xD7)) {
                    pos += 2;
                    continue;
                }
                break; // a marker: the loop above reads it
            }
            if (pos == start) return Bad(reason, "bad-ba-logo-jpeg-scan");
            scanned = true;
        }
    }
}
} // namespace

const char* LogoMime(LogoFormat format)
{
    return format == LogoFormat::PNG ? "image/png" : "image/jpeg";
}

uint32_t Crc32(std::span<const unsigned char> data, uint32_t crc)
{
    crc = ~crc;
    for (const unsigned char c : data) crc = CRC_TABLE[(crc ^ c) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

std::optional<LogoFormat> CheckLogo(std::span<const unsigned char> bytes, std::string& reason)
{
    if (bytes.empty()) return Bad(reason, "bad-ba-logo-empty");
    if (bytes.size() > MAX_LOGO_BYTES) return Bad(reason, "bad-ba-logo-oversize");
    if (bytes.size() >= sizeof(PNG_SIGNATURE) && std::memcmp(bytes.data(), PNG_SIGNATURE, sizeof(PNG_SIGNATURE)) == 0) return CheckPng(bytes, reason);
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xD8) return CheckJpeg(bytes, reason);
    // SVG, GIF, WebP, BMP, ICO, AVIF, a PNG or JPEG behind anything...
    return Bad(reason, "bad-ba-logo-format");
}

} // namespace bitassets
