// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BITASSETS_LOGO_H
#define BITCOIN_BITASSETS_LOGO_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

/**
 * The logos of assets (doc/bitassets.md, "Logos"): a PNG or a JPEG of exactly LOGO_SIZE x LOGO_SIZE
 * pixels, at most MAX_LOGO_BYTES, carried whole by the marker of the transaction that sets it.
 *
 * The rules check the STRUCTURE of the file only: no image is decoded by consensus. What they let
 * through is a file a decoder reads as one image of that size and nothing else: no metadata, no
 * chunk or segment of any other kind, nothing after its end.
 *
 *  PNG: the signature; then chunks whose lengths fit in what is left and whose CRC-32 is right, of
 *       the kinds IHDR, PLTE, tRNS, IDAT, IEND only. IHDR first (13 bytes): 256 x 256, a bit depth
 *       and colour type the PNG standard allows together, compression, filter 0, NOT interlaced.
 *       PLTE once, before the image data, for a palette image only (and required for one), 1 to
 *       2^depth entries. tRNS once, after PLTE and before the image data, of the size its colour type
 *       gives (at most one per palette entry), never with an alpha channel. IDAT chunks one after
 *       another, more than 0 bytes in all, starting with a zlib header (deflate, no preset dictionary,
 *       its check right). IEND last (empty), and nothing after it.
 *  JPEG: SOI (FFD8); then segments of the kinds SOF0 (baseline) or SOF2 (progressive), DQT, DHT, DRI
 *       and SOS only, each of the length its content gives: SOF once, before the first scan, 8-bit,
 *       256 x 256, 1 or 3 components (ids different, sampling factors 1 to 4, quantization tables 0 to
 *       3); DQT and DHT tables well formed and filling their segment; DRI 4 bytes; SOS naming 1 to 4
 *       components of the frame, with the spectral range its frame allows. After a scan's header, its
 *       entropy-coded data, in which FF is followed by 00 (a stuffed byte) or a restart marker
 *       (FFD0-FFD7) only. At least one scan, then EOI (FFD9) last, and nothing after it. No APPn (no
 *       JFIF, Exif, XMP, ICC), no COM, no other marker, no fill bytes.
 */
namespace bitassets {

/** Width and height of every logo, in pixels. */
inline constexpr uint32_t LOGO_SIZE{256};
/** Most bytes a logo may have. */
inline constexpr size_t MAX_LOGO_BYTES{32768};

enum class LogoFormat : uint8_t { PNG = 1, JPEG = 2 };

/** "image/png" or "image/jpeg". */
const char* LogoMime(LogoFormat format);

/**
 * Whether `bytes` are a logo the rules accept; if so its format, if not why (`reason`, a reject reason
 * "bad-ba-logo-..."). An empty logo is none (it removes one): not accepted here.
 */
std::optional<LogoFormat> CheckLogo(std::span<const unsigned char> bytes, std::string& reason);

/** The CRC-32 of PNG (ISO 3309, polynomial 0xEDB88320), of `data`, continued from `crc` (0 to start). */
uint32_t Crc32(std::span<const unsigned char> data, uint32_t crc = 0);

} // namespace bitassets

#endif // BITCOIN_BITASSETS_LOGO_H
