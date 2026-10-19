// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitassets/logo.h>
#include <bitassets/state.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <streams.h>
#include <test/bitassets_logo_samples.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace bitassets;
using Bytes = std::vector<unsigned char>;

namespace {
Bytes Sample(const char* hex) { return ParseHex(hex); }

/** The reason the rules give, or "" if they take it. */
std::string Why(const Bytes& bytes)
{
    std::string reason;
    if (CheckLogo(bytes, reason)) return "";
    return reason;
}

void PutBe32(Bytes& b, size_t at, uint32_t v)
{
    for (int i{0}; i < 4; ++i) b[at + i] = static_cast<unsigned char>(v >> (24 - 8 * i));
}

Bytes Chunk(const std::string& type, const Bytes& data)
{
    Bytes c(4);
    PutBe32(c, 0, data.size());
    c.insert(c.end(), type.begin(), type.end());
    c.insert(c.end(), data.begin(), data.end());
    const uint32_t crc{Crc32(std::span{c}.subspan(4))};
    c.resize(c.size() + 4);
    PutBe32(c, c.size() - 4, crc);
    return c;
}

/** The offset of the first chunk of a type in a PNG. */
size_t FindChunk(const Bytes& png, const std::string& type)
{
    size_t pos{8};
    while (pos + 8 <= png.size()) {
        const uint32_t length{(uint32_t{png[pos]} << 24) | (uint32_t{png[pos + 1]} << 16) | (uint32_t{png[pos + 2]} << 8) | png[pos + 3]};
        if (std::memcmp(&png[pos + 4], type.data(), 4) == 0) return pos;
        pos += 12 + length;
    }
    return std::string::npos;
}

Bytes InsertBefore(Bytes png, const std::string& before, const Bytes& chunk)
{
    const size_t at{FindChunk(png, before)};
    png.insert(png.begin() + at, chunk.begin(), chunk.end());
    return png;
}

/** The IHDR of a PNG changed, its CRC made right again. */
Bytes WithIhdr(Bytes png, size_t offset, unsigned char value)
{
    png[16 + offset] = value;
    PutBe32(png, 29, Crc32(std::span{png}.subspan(12, 17)));
    return png;
}

/** A segment of a JPEG put right after SOI. */
Bytes JpegWith(Bytes jpeg, const Bytes& segment)
{
    jpeg.insert(jpeg.begin() + 2, segment.begin(), segment.end());
    return jpeg;
}

size_t FindMarker(const Bytes& jpeg, unsigned char code)
{
    for (size_t i{0}; i + 1 < jpeg.size(); ++i) {
        if (jpeg[i] == 0xFF && jpeg[i + 1] == code) return i;
    }
    return std::string::npos;
}

CTransaction MarkerTx(const Marker& marker)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 0});
    tx.vout.emplace_back(0, CScript() << OP_0 << std::vector<unsigned char>(20, 1));
    tx.vout.emplace_back(0, MarkerScript(marker));
    return CTransaction{tx};
}
} // namespace

BOOST_AUTO_TEST_SUITE(bitassets_logo_tests)

BOOST_AUTO_TEST_CASE(crc32_of_png)
{
    // The check value of CRC-32/ISO-HDLC.
    const std::string text{"123456789"};
    BOOST_CHECK_EQUAL(Crc32(std::span{reinterpret_cast<const unsigned char*>(text.data()), text.size()}), 0xCBF43926u);
}

BOOST_AUTO_TEST_CASE(good_logos)
{
    std::string reason;
    BOOST_CHECK(CheckLogo(Sample(LOGO_PNG_PALETTE), reason) == LogoFormat::PNG);
    BOOST_CHECK(CheckLogo(Sample(LOGO_PNG_RGBA), reason) == LogoFormat::PNG);
    BOOST_CHECK(CheckLogo(Sample(LOGO_JPEG_BASELINE), reason) == LogoFormat::JPEG);
    BOOST_CHECK(CheckLogo(Sample(LOGO_JPEG_PROGRESSIVE), reason) == LogoFormat::JPEG);
    BOOST_CHECK(CheckLogo(Sample(LOGO_JPEG_GRAY), reason) == LogoFormat::JPEG);
    // A palette with transparency (tRNS after PLTE), and the image data split in two IDAT chunks.
    const Bytes palette{Sample(LOGO_PNG_PALETTE)};
    BOOST_CHECK_EQUAL(Why(InsertBefore(palette, "IDAT", Chunk("tRNS", {0x00, 0x80}))), "");
    const size_t idat{FindChunk(palette, "IDAT")};
    const uint32_t length{(uint32_t{palette[idat]} << 24) | (uint32_t{palette[idat + 1]} << 16) | (uint32_t{palette[idat + 2]} << 8) | palette[idat + 3]};
    const Bytes data(palette.begin() + idat + 8, palette.begin() + idat + 8 + length);
    Bytes split(palette.begin(), palette.begin() + idat);
    for (const Bytes& part : {Bytes(data.begin(), data.begin() + 1), Bytes(data.begin() + 1, data.end())}) {
        const Bytes c{Chunk("IDAT", part)};
        split.insert(split.end(), c.begin(), c.end());
    }
    const Bytes iend{Chunk("IEND", {})};
    split.insert(split.end(), iend.begin(), iend.end());
    BOOST_CHECK_EQUAL(Why(split), "");
    // The largest a logo may be.
    Bytes big{palette};
    const Bytes pad{Chunk("IDAT", Bytes(MAX_LOGO_BYTES - palette.size() - 12, 0))};
    big.insert(big.begin() + FindChunk(big, "IEND"), pad.begin(), pad.end());
    BOOST_CHECK_EQUAL(big.size(), MAX_LOGO_BYTES);
    BOOST_CHECK_EQUAL(Why(big), "");
}

BOOST_AUTO_TEST_CASE(png_refused)
{
    const Bytes png{Sample(LOGO_PNG_PALETTE)};
    const Bytes rgba{Sample(LOGO_PNG_RGBA)};
    BOOST_CHECK_EQUAL(Why({}), "bad-ba-logo-empty");
    // Too large by a byte.
    Bytes big{png};
    const Bytes pad{Chunk("IDAT", Bytes(MAX_LOGO_BYTES - png.size() - 11, 0))};
    big.insert(big.begin() + FindChunk(big, "IEND"), pad.begin(), pad.end());
    BOOST_CHECK_EQUAL(Why(big), "bad-ba-logo-oversize");
    // Not 256 x 256: either side, larger or smaller.
    BOOST_CHECK_EQUAL(Why(WithIhdr(png, 3, 0xFF)), "bad-ba-logo-size");      // width 511
    BOOST_CHECK_EQUAL(Why(WithIhdr(png, 6, 0x02)), "bad-ba-logo-size");      // height 512
    BOOST_CHECK_EQUAL(Why(WithIhdr(png, 0, 0x01)), "bad-ba-logo-size");      // width 2^24 + 256
    // Depths and colour types the standard does not pair, and unknown ones.
    BOOST_CHECK_EQUAL(Why(WithIhdr(png, 8, 16)), "bad-ba-logo-png-depth");   // a palette of 16 bits
    BOOST_CHECK_EQUAL(Why(WithIhdr(rgba, 8, 4)), "bad-ba-logo-png-depth");   // RGBA of 4 bits
    BOOST_CHECK_EQUAL(Why(WithIhdr(rgba, 9, 7)), "bad-ba-logo-png-depth");
    BOOST_CHECK_EQUAL(Why(WithIhdr(png, 10, 1)), "bad-ba-logo-png-ihdr");    // compression method
    BOOST_CHECK_EQUAL(Why(WithIhdr(png, 11, 1)), "bad-ba-logo-png-ihdr");    // filter method
    BOOST_CHECK_EQUAL(Why(WithIhdr(png, 12, 1)), "bad-ba-logo-png-interlace");
    // A wrong CRC, anywhere.
    for (const std::string& type : {"IHDR", "PLTE", "IDAT", "IEND"}) {
        Bytes bad{png};
        const size_t at{FindChunk(bad, type)};
        const uint32_t length{(uint32_t{bad[at]} << 24) | (uint32_t{bad[at + 1]} << 16) | (uint32_t{bad[at + 2]} << 8) | bad[at + 3]};
        bad[at + 8 + length] ^= 0x01;
        BOOST_CHECK_EQUAL(Why(bad), "bad-ba-logo-png-crc");
    }
    // A length beyond the end; cut anywhere.
    Bytes long_chunk{png};
    long_chunk[FindChunk(long_chunk, "IDAT") + 1] = 0x7F;
    BOOST_CHECK_EQUAL(Why(long_chunk), "bad-ba-logo-png-length");
    for (size_t cut : {size_t{9}, size_t{20}, png.size() / 2, png.size() - 1}) {
        const std::string why{Why(Bytes(png.begin(), png.begin() + cut))};
        BOOST_CHECK_MESSAGE(why == "bad-ba-logo-png-truncated" || why == "bad-ba-logo-png-length", why);
    }
    // Metadata and every other chunk, even with a right CRC: text, time, gamma, profiles, private ones, a second IHDR.
    for (const std::string& type : {"tEXt", "zTXt", "iTXt", "tIME", "gAMA", "iCCP", "sRGB", "pHYs", "eXIf", "bKGD", "acTL", "prVt", "IHDR"}) {
        BOOST_CHECK_EQUAL(Why(InsertBefore(png, "IDAT", Chunk(type, {'h', 'i'}))), "bad-ba-logo-png-chunk");
        BOOST_CHECK_EQUAL(Why(InsertBefore(png, "IEND", Chunk(type, {'h', 'i'}))), "bad-ba-logo-png-chunk");
    }
    // A polyglot: HTML in a text chunk; a ZIP or a script after IEND.
    BOOST_CHECK_EQUAL(Why(InsertBefore(png, "IDAT", Chunk("tEXt", {'<', 's', 'v', 'g', '>'}))), "bad-ba-logo-png-chunk");
    Bytes trailing{png};
    for (unsigned char c : std::string{"PK\x03\x04<script>alert(1)</script>"}) trailing.push_back(c);
    BOOST_CHECK_EQUAL(Why(trailing), "bad-ba-logo-trailing");
    trailing = png;
    trailing.push_back(0);
    BOOST_CHECK_EQUAL(Why(trailing), "bad-ba-logo-trailing");
    // The order: PLTE missing, or after the data, or twice, or for an image of another colour type; tRNS in the wrong place or size.
    Bytes no_plte{png};
    const size_t plte{FindChunk(no_plte, "PLTE")};
    no_plte.erase(no_plte.begin() + plte, no_plte.begin() + plte + 12 + 9);
    BOOST_CHECK_EQUAL(Why(no_plte), "bad-ba-logo-png-plte");
    BOOST_CHECK_EQUAL(Why(InsertBefore(png, "IEND", Chunk("PLTE", {1, 2, 3}))), "bad-ba-logo-png-plte");
    BOOST_CHECK_EQUAL(Why(InsertBefore(png, "IDAT", Chunk("PLTE", {1, 2, 3}))), "bad-ba-logo-png-plte");
    BOOST_CHECK_EQUAL(Why(InsertBefore(rgba, "IDAT", Chunk("PLTE", {1, 2, 3}))), "bad-ba-logo-png-plte");
    BOOST_CHECK_EQUAL(Why(InsertBefore(png, "IDAT", Chunk("tRNS", {0, 0, 0, 0}))), "bad-ba-logo-png-trns"); // more than the palette
    BOOST_CHECK_EQUAL(Why(InsertBefore(png, "IEND", Chunk("tRNS", {0}))), "bad-ba-logo-png-trns");
    BOOST_CHECK_EQUAL(Why(InsertBefore(rgba, "IDAT", Chunk("tRNS", {0, 0, 0, 0, 0, 0}))), "bad-ba-logo-png-trns"); // an alpha channel already
    // No image data, or none at all, or an IEND that is not empty.
    Bytes no_idat{png};
    const size_t idat{FindChunk(no_idat, "IDAT")};
    no_idat.erase(no_idat.begin() + idat, no_idat.begin() + FindChunk(no_idat, "IEND"));
    BOOST_CHECK_EQUAL(Why(no_idat), "bad-ba-logo-png-idat");
    BOOST_CHECK_EQUAL(Why(InsertBefore(no_idat, "IEND", Chunk("IDAT", {}))), "bad-ba-logo-png-idat");
    Bytes iend_data{png.begin(), png.begin() + FindChunk(png, "IEND")};
    const Bytes iend{Chunk("IEND", {0})};
    iend_data.insert(iend_data.end(), iend.begin(), iend.end());
    BOOST_CHECK_EQUAL(Why(iend_data), "bad-ba-logo-png-iend");
    // Image data split by another chunk.
    BOOST_CHECK_EQUAL(Why(InsertBefore(InsertBefore(png, "IEND", Chunk("tRNS", {0})), "IEND", Chunk("IDAT", {0}))), "bad-ba-logo-png-trns");
    // Image data that is no zlib stream.
    Bytes not_zlib{png};
    const size_t at{FindChunk(not_zlib, "IDAT")};
    not_zlib[at + 8] = 0x79;
    PutBe32(not_zlib, at + 8 + 551, Crc32(std::span{not_zlib}.subspan(at + 4, 4 + 551)));
    BOOST_CHECK_EQUAL(Why(not_zlib), "bad-ba-logo-png-zlib");
    // The first chunk not IHDR.
    const Bytes plte_first{[&] { Bytes b(png.begin(), png.begin() + 8); const Bytes c{Chunk("PLTE", {1, 2, 3})}; b.insert(b.end(), c.begin(), c.end()); b.insert(b.end(), png.begin() + 8, png.end()); return b; }()};
    BOOST_CHECK_EQUAL(Why(plte_first), "bad-ba-logo-png-ihdr");
}

BOOST_AUTO_TEST_CASE(jpeg_refused)
{
    const Bytes jpeg{Sample(LOGO_JPEG_BASELINE)};
    const Bytes progressive{Sample(LOGO_JPEG_PROGRESSIVE)};
    // Metadata: APP0 (JFIF), APP1 (Exif, XMP), APP2 (ICC), APP13, APP14 (Adobe), COM.
    for (unsigned char code : {0xE0, 0xE1, 0xE2, 0xED, 0xEE, 0xEF, 0xFE}) {
        BOOST_CHECK_EQUAL(Why(JpegWith(jpeg, {0xFF, code, 0x00, 0x04, 'h', 'i'})), "bad-ba-logo-jpeg-metadata");
    }
    // Other frames (extended, lossless, hierarchical, arithmetic), DAC, DNL, DHP, EXP, JPGn, a stray RST, a second SOI, TEM, reserved.
    for (unsigned char code : {0xC1, 0xC3, 0xC5, 0xC7, 0xC9, 0xCB, 0xCC, 0xCF, 0xDC, 0xDE, 0xDF, 0xF0, 0xFD, 0xD0, 0xD8, 0x01, 0x02}) {
        BOOST_CHECK_EQUAL(Why(JpegWith(jpeg, {0xFF, code, 0x00, 0x04, 0x00, 0x00})), "bad-ba-logo-jpeg-marker");
    }
    // A fill byte.
    BOOST_CHECK_EQUAL(Why(JpegWith(jpeg, {0xFF})), "bad-ba-logo-jpeg-marker");
    // Not 256 x 256.
    for (size_t offset : {size_t{5}, size_t{6}, size_t{7}, size_t{8}}) {
        Bytes bad{jpeg};
        bad[FindMarker(bad, 0xC0) + offset] ^= 0x01;
        BOOST_CHECK_EQUAL(Why(bad), "bad-ba-logo-size");
    }
    // 12-bit samples; four components (CMYK).
    Bytes precision{jpeg};
    precision[FindMarker(precision, 0xC0) + 4] = 12;
    BOOST_CHECK_EQUAL(Why(precision), "bad-ba-logo-jpeg-frame");
    Bytes cmyk{jpeg};
    cmyk[FindMarker(cmyk, 0xC0) + 9] = 4;
    BOOST_CHECK_EQUAL(Why(cmyk), "bad-ba-logo-jpeg-frame");
    // Two frames.
    const size_t sof{FindMarker(jpeg, 0xC0)};
    const Bytes frame(jpeg.begin() + sof, jpeg.begin() + sof + 2 + 17);
    BOOST_CHECK_EQUAL(Why(JpegWith(jpeg, frame)), "bad-ba-logo-jpeg-frame");
    // A scan before the frame.
    BOOST_CHECK_EQUAL(Why(JpegWith(jpeg, {0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00, 0x12})), "bad-ba-logo-jpeg-scan");
    // Lengths beyond the end, or too short; tables that do not fill their segment; a DRI of another size.
    Bytes long_segment{jpeg};
    long_segment[FindMarker(long_segment, 0xDB) + 2] = 0xFF;
    BOOST_CHECK_EQUAL(Why(long_segment), "bad-ba-logo-jpeg-length");
    BOOST_CHECK_EQUAL(Why(JpegWith(jpeg, {0xFF, 0xDB, 0x00, 0x01})), "bad-ba-logo-jpeg-length");
    BOOST_CHECK_EQUAL(Why(JpegWith(jpeg, {0xFF, 0xDB, 0x00, 0x05, 0x00, 1, 2})), "bad-ba-logo-jpeg-dqt");
    BOOST_CHECK_EQUAL(Why(JpegWith(jpeg, {0xFF, 0xC4, 0x00, 0x05, 0x00, 1, 2})), "bad-ba-logo-jpeg-dht");
    BOOST_CHECK_EQUAL(Why(JpegWith(jpeg, {0xFF, 0xDD, 0x00, 0x05, 0x00, 1, 2})), "bad-ba-logo-jpeg-dri");
    // A Huffman table with more codes than room for them (three codes of one bit).
    Bytes dht{0xFF, 0xC4, 0x00, 0x16, 0x00, 3};
    dht.resize(dht.size() + 15, 0);
    dht.insert(dht.end(), {1, 2, 3});
    BOOST_CHECK_EQUAL(Why(JpegWith(jpeg, dht)), "bad-ba-logo-jpeg-dht");
    // Anything after EOI; cut anywhere; no EOI.
    Bytes trailing{jpeg};
    for (unsigned char c : std::string{"<html><script>alert(1)</script>"}) trailing.push_back(c);
    BOOST_CHECK_EQUAL(Why(trailing), "bad-ba-logo-trailing");
    trailing = progressive;
    trailing.insert(trailing.end(), {0xFF, 0xD9});
    BOOST_CHECK_EQUAL(Why(trailing), "bad-ba-logo-trailing");
    for (size_t cut : {size_t{3}, size_t{10}, jpeg.size() / 2, jpeg.size() - 1, jpeg.size() - 2}) {
        const std::string why{Why(Bytes(jpeg.begin(), jpeg.begin() + cut))};
        BOOST_CHECK_MESSAGE(why.starts_with("bad-ba-logo-jpeg"), why);
    }
    // A baseline scan of another spectral range.
    Bytes range{jpeg};
    const size_t sos{FindMarker(range, 0xDA)};
    range[sos + 2 + 2 + 1 + 2 * 3 + 1] = 0x20;
    BOOST_CHECK_EQUAL(Why(range), "bad-ba-logo-jpeg-scan");
}

BOOST_AUTO_TEST_CASE(other_formats_refused)
{
    const auto text{[](const std::string& s) { return Bytes(s.begin(), s.end()); }};
    BOOST_CHECK_EQUAL(Why(text("<svg xmlns=\"http://www.w3.org/2000/svg\" onload=\"alert(1)\"/>")), "bad-ba-logo-format");
    BOOST_CHECK_EQUAL(Why(text("<?xml version=\"1.0\"?><svg/>")), "bad-ba-logo-format");
    BOOST_CHECK_EQUAL(Why(text("GIF89a\x00\x01\x00\x01")), "bad-ba-logo-format");
    BOOST_CHECK_EQUAL(Why(text("RIFF\x10\x00\x00\x00WEBPVP8 ")), "bad-ba-logo-format");
    BOOST_CHECK_EQUAL(Why(text("BM\x36\x00\x00\x00")), "bad-ba-logo-format");
    BOOST_CHECK_EQUAL(Why(text("<html>")), "bad-ba-logo-format");
    // A PNG behind something else (a polyglot that starts as another file).
    Bytes behind{text("PK\x03\x04")};
    const Bytes png{Sample(LOGO_PNG_PALETTE)};
    behind.insert(behind.end(), png.begin(), png.end());
    BOOST_CHECK_EQUAL(Why(behind), "bad-ba-logo-format");
    BOOST_CHECK_EQUAL(Why(Bytes(png.begin(), png.begin() + 7)), "bad-ba-logo-format");
}

BOOST_AUTO_TEST_CASE(logo_markers)
{
    const Bytes png{Sample(LOGO_PNG_PALETTE)};
    // SetLogo, a logo and none, written and read back the same.
    for (const Bytes& logo : {png, Bytes{}}) {
        Marker marker;
        marker.operation = SetLogo{uint256::ONE, logo};
        marker.outputs.push_back({0, Token{Token::Kind::CONTROL, uint256::ONE, 1}});
        const auto parsed{ParseMarkerScript(MarkerScript(marker))};
        BOOST_REQUIRE(parsed);
        BOOST_CHECK(*parsed == marker);
        BOOST_CHECK(IsLogoOperation(*parsed->operation));
        std::string reason;
        BOOST_CHECK(CheckMarker(MarkerTx(marker), reason));
        BOOST_CHECK(!CheckLogoActivation(MarkerTx(marker), /*active=*/false, reason));
        BOOST_CHECK_EQUAL(reason, "bad-ba-logo-inactive");
        BOOST_CHECK(CheckLogoActivation(MarkerTx(marker), /*active=*/true, reason));
    }
    // A registration with a logo is a marker of its own kind; without one, as it always was.
    Register reg;
    reg.name = HashName("GOLD");
    reg.text = "GOLD";
    Marker marker;
    marker.operation = reg;
    const CScript plain{MarkerScript(marker)};
    BOOST_CHECK(!IsLogoOperation(*marker.operation));
    std::get<Register>(*marker.operation).logo = png;
    const CScript with_logo{MarkerScript(marker)};
    BOOST_CHECK(with_logo.size() > plain.size() + png.size());
    const auto parsed{ParseMarkerScript(with_logo)};
    BOOST_REQUIRE(parsed);
    BOOST_CHECK(*parsed == marker);
    BOOST_CHECK(IsLogoOperation(*parsed->operation));
    std::string reason;
    BOOST_CHECK(CheckLogoActivation(MarkerTx(*ParseMarkerScript(plain)), /*active=*/false, reason));
    // Kind 33 with an empty logo is not how a marker is written: refused.
    Bytes empty_logo{std::begin(TAG), std::end(TAG)};
    empty_logo.push_back(MARKER_VERSION);
    empty_logo.push_back(REGISTER_WITH_LOGO_KIND);
    {
        DataStream s{};
        s << reg << Bytes{};
        WriteCompactSize(s, 0); // no outputs
        empty_logo.insert(empty_logo.end(), UCharCast(s.data()), UCharCast(s.data()) + s.size());
    }
    BOOST_CHECK(!ParseMarkerScript(CScript() << OP_RETURN << empty_logo));
    // A bad logo makes the marker bad, whatever else it does.
    Bytes bad{png};
    bad.push_back(0);
    Marker set;
    set.operation = SetLogo{uint256::ONE, bad};
    BOOST_CHECK(!CheckMarker(MarkerTx(set), reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-logo-trailing");
    std::get<Register>(*marker.operation).logo = bad;
    BOOST_CHECK(!CheckMarker(MarkerTx(marker), reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-logo-trailing");
    set.operation = SetLogo{uint256{}, png};
    BOOST_CHECK(!CheckMarker(MarkerTx(set), reason));
    BOOST_CHECK_EQUAL(reason, "bad-ba-logo-asset");
}

BOOST_AUTO_TEST_SUITE_END()
