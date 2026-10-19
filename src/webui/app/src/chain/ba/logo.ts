// The logos of assets (doc/bitassets.md, "Logos"): a PNG or a JPEG of exactly 256 x 256 pixels, at most 32768 bytes,
// carried whole by the transaction that sets it. Here: what the picker makes of any picture (cropped and scaled to
// 256 x 256, encoded as a palette PNG, a full-colour PNG and a JPEG, every metadata segment left out, the smallest
// kept), and the check of the rules (src/bitassets/logo.cpp), the same, so that the page says at once what the node
// would refuse. The host checks again (bitassets.checkLogo, and the confirmation), and the node; the page never shows
// a logo but as an <img> of a data: URL the host made.

export const LOGO_SIZE = 256;
export const MAX_LOGO_BYTES = 32768;
/** The JPEG quality tried first, then lower ones while the file is larger than the cap. */
export const JPEG_QUALITIES = [0.8, 0.7, 0.6, 0.5, 0.4, 0.3] as const;

export type LogoFormat = 'png' | 'jpeg';
export type LogoCheck = { ok: true; format: LogoFormat } | { ok: false; reason: string };

const PNG_SIGNATURE = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

export function crc32(data: Uint8Array, start = 0, end = data.length): number {
  let c = 0xffffffff;
  for (let i = start; i < end; i++) c = CRC_TABLE[(c ^ data[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

const be32 = (b: Uint8Array, at: number) => ((b[at] << 24) | (b[at + 1] << 16) | (b[at + 2] << 8) | b[at + 3]) >>> 0;
const be16 = (b: Uint8Array, at: number) => (b[at] << 8) | b[at + 1];
const bad = (reason: string): LogoCheck => ({ ok: false, reason });
const typeOf = (b: Uint8Array, at: number) => String.fromCharCode(b[at], b[at + 1], b[at + 2], b[at + 3]);

function checkPng(b: Uint8Array): LogoCheck {
  let pos = 8;
  let ihdr = false, plte = false, trns = false, idat = false;
  let depth = 0, color = 0, entries = 0, idatBytes = 0;
  const zlib = [0, 0];
  for (;;) {
    if (b.length - pos < 12) return bad('bad-ba-logo-png-truncated');
    const length = be32(b, pos);
    if (length > 0x7fffffff || length > b.length - pos - 12) return bad('bad-ba-logo-png-length');
    const type = typeOf(b, pos + 4);
    const data = pos + 8;
    if (crc32(b, pos + 4, pos + 8 + length) !== be32(b, pos + 8 + length)) return bad('bad-ba-logo-png-crc');
    pos += 12 + length;
    if (!ihdr) {
      if (type !== 'IHDR' || length !== 13) return bad('bad-ba-logo-png-ihdr');
      if (be32(b, data) !== LOGO_SIZE || be32(b, data + 4) !== LOGO_SIZE) return bad('bad-ba-logo-size');
      depth = b[data + 8];
      color = b[data + 9];
      const good =
        color === 0 ? [1, 2, 4, 8, 16].includes(depth) : color === 3 ? [1, 2, 4, 8].includes(depth) : color === 2 || color === 4 || color === 6 ? depth === 8 || depth === 16 : false;
      if (!good) return bad('bad-ba-logo-png-depth');
      if (b[data + 10] !== 0 || b[data + 11] !== 0) return bad('bad-ba-logo-png-ihdr');
      if (b[data + 12] !== 0) return bad('bad-ba-logo-png-interlace');
      ihdr = true;
      continue;
    }
    if (type === 'IEND') {
      if (length !== 0) return bad('bad-ba-logo-png-iend');
      if (!idat || idatBytes === 0) return bad('bad-ba-logo-png-idat');
      if (color === 3 && !plte) return bad('bad-ba-logo-png-plte');
      if (pos !== b.length) return bad('bad-ba-logo-trailing');
      if ((zlib[0] & 0x0f) !== 8 || zlib[0] >> 4 > 7 || zlib[1] & 0x20 || ((zlib[0] << 8) | zlib[1]) % 31 !== 0) return bad('bad-ba-logo-png-zlib');
      return { ok: true, format: 'png' };
    }
    if (type === 'IDAT') {
      if (color === 3 && !plte) return bad('bad-ba-logo-png-plte');
      for (let i = 0; i < length && idatBytes + i < 2; i++) zlib[idatBytes + i] = b[data + i];
      idat = true;
      idatBytes += length;
      continue;
    }
    if (type === 'PLTE') {
      if (plte || idat || trns || color !== 3 || length === 0 || length % 3 !== 0 || length / 3 > 1 << depth) return bad('bad-ba-logo-png-plte');
      entries = length / 3;
      plte = true;
      continue;
    }
    if (type === 'tRNS') {
      const good = color === 0 ? length === 2 : color === 2 ? length === 6 : color === 3 ? plte && length >= 1 && length <= entries : false;
      if (trns || idat || !good) return bad('bad-ba-logo-png-trns');
      trns = true;
      continue;
    }
    return bad('bad-ba-logo-png-chunk');
  }
}

function checkJpeg(b: Uint8Array): LogoCheck {
  let pos = 2;
  let frame = false, scanned = false, progressive = false;
  let components = 0;
  const ids: number[] = [];
  for (;;) {
    if (b.length - pos < 2) return bad('bad-ba-logo-jpeg-truncated');
    if (b[pos] !== 0xff) return bad('bad-ba-logo-jpeg-marker');
    const code = b[pos + 1];
    pos += 2;
    if (code === 0xd9) {
      if (!scanned) return bad('bad-ba-logo-jpeg-scan');
      if (pos !== b.length) return bad('bad-ba-logo-trailing');
      return { ok: true, format: 'jpeg' };
    }
    const sof = code === 0xc0 || code === 0xc2;
    if (!sof && code !== 0xc4 && code !== 0xdb && code !== 0xdd && code !== 0xda) {
      return bad((code >= 0xe0 && code <= 0xef) || code === 0xfe ? 'bad-ba-logo-jpeg-metadata' : 'bad-ba-logo-jpeg-marker');
    }
    if (b.length - pos < 2) return bad('bad-ba-logo-jpeg-truncated');
    const length = be16(b, pos);
    if (length < 2 || length > b.length - pos) return bad('bad-ba-logo-jpeg-length');
    const s = pos + 2;
    const n = length - 2;
    pos += length;
    if (sof) {
      if (frame || n < 6 || b[s] !== 8) return bad('bad-ba-logo-jpeg-frame');
      if (be16(b, s + 1) !== LOGO_SIZE || be16(b, s + 3) !== LOGO_SIZE) return bad('bad-ba-logo-size');
      components = b[s + 5];
      if ((components !== 1 && components !== 3) || n !== 6 + 3 * components) return bad('bad-ba-logo-jpeg-frame');
      for (let i = 0; i < components; i++) {
        const id = b[s + 6 + 3 * i], h = b[s + 7 + 3 * i] >> 4, v = b[s + 7 + 3 * i] & 15, tq = b[s + 8 + 3 * i];
        if (ids.includes(id) || h < 1 || h > 4 || v < 1 || v > 4 || tq > 3) return bad('bad-ba-logo-jpeg-frame');
        ids.push(id);
      }
      frame = true;
      progressive = code === 0xc2;
    } else if (code === 0xdb) {
      if (n === 0) return bad('bad-ba-logo-jpeg-dqt');
      for (let at = 0; at < n; at += 65) if (b[s + at] > 3 || n - at < 65) return bad('bad-ba-logo-jpeg-dqt');
    } else if (code === 0xc4) {
      if (n === 0) return bad('bad-ba-logo-jpeg-dht');
      for (let at = 0; at < n; ) {
        if (n - at < 17 || b[s + at] >> 4 > 1 || (b[s + at] & 15) > 3) return bad('bad-ba-logo-jpeg-dht');
        let values = 0, space = 0;
        for (let l = 1; l <= 16; l++) {
          values += b[s + at + l];
          space += b[s + at + l];
          if (space > 1 << l) return bad('bad-ba-logo-jpeg-dht');
          space <<= 1;
        }
        if (values === 0 || values > 256 || n - at - 17 < values) return bad('bad-ba-logo-jpeg-dht');
        at += 17 + values;
      }
    } else if (code === 0xdd) {
      if (n !== 2) return bad('bad-ba-logo-jpeg-dri');
    } else {
      if (!frame || n === 0) return bad('bad-ba-logo-jpeg-scan');
      const count = b[s];
      if (count < 1 || count > components || n !== 4 + 2 * count) return bad('bad-ba-logo-jpeg-scan');
      const seen: number[] = [];
      for (let i = 0; i < count; i++) {
        const id = b[s + 1 + 2 * i], tables = b[s + 2 + 2 * i];
        if (!ids.includes(id) || seen.includes(id) || tables >> 4 > 3 || (tables & 15) > 3) return bad('bad-ba-logo-jpeg-scan');
        seen.push(id);
      }
      const ss = b[s + 1 + 2 * count], se = b[s + 2 + 2 * count], ah = b[s + 3 + 2 * count] >> 4, al = b[s + 3 + 2 * count] & 15;
      if (progressive) {
        const dc = ss === 0 && se === 0, ac = ss >= 1 && ss <= se && se <= 63 && count === 1;
        if ((!dc && !ac) || ah > 13 || al > 13) return bad('bad-ba-logo-jpeg-scan');
      } else if (ss !== 0 || se !== 63 || ah !== 0 || al !== 0) {
        return bad('bad-ba-logo-jpeg-scan');
      }
      const start = pos;
      for (;;) {
        if (pos >= b.length) return bad('bad-ba-logo-jpeg-truncated');
        if (b[pos] !== 0xff) {
          pos++;
          continue;
        }
        if (pos + 1 >= b.length) return bad('bad-ba-logo-jpeg-truncated');
        const next = b[pos + 1];
        if (next === 0 || (next >= 0xd0 && next <= 0xd7)) {
          pos += 2;
          continue;
        }
        break;
      }
      if (pos === start) return bad('bad-ba-logo-jpeg-scan');
      scanned = true;
    }
  }
}

/** Whether the rules take these bytes as a logo (src/bitassets/logo.cpp, the same checks). */
export function checkLogo(bytes: Uint8Array): LogoCheck {
  if (bytes.length === 0) return bad('bad-ba-logo-empty');
  if (bytes.length > MAX_LOGO_BYTES) return bad('bad-ba-logo-oversize');
  if (bytes.length >= 8 && PNG_SIGNATURE.every((v, i) => bytes[i] === v)) return checkPng(bytes);
  if (bytes.length >= 2 && bytes[0] === 0xff && bytes[1] === 0xd8) return checkJpeg(bytes);
  return bad('bad-ba-logo-format');
}

/**
 * A JPEG without its APPn (JFIF, Exif, XMP, ICC, Adobe) and COM segments: what a browser's encoder writes before the
 * picture. Fill bytes between segments go too. Bytes that are no JPEG are given back as they are.
 */
export function stripJpeg(b: Uint8Array): Uint8Array {
  if (b.length < 4 || b[0] !== 0xff || b[1] !== 0xd8) return b;
  const out: number[] = [0xff, 0xd8];
  let pos = 2;
  while (pos + 4 <= b.length) {
    if (b[pos] !== 0xff) return b;
    while (b[pos + 1] === 0xff && pos + 2 < b.length) pos++; // fill bytes
    const code = b[pos + 1];
    if (code === 0xda || code === 0xd9) {
      const rest = b.subarray(pos);
      const all = new Uint8Array(out.length + rest.length);
      all.set(out);
      all.set(rest, out.length);
      return all;
    }
    const length = be16(b, pos + 2);
    if (length < 2 || pos + 2 + length > b.length) return b;
    if (!((code >= 0xe0 && code <= 0xef) || code === 0xfe)) for (let i = pos; i < pos + 2 + length; i++) out.push(b[i]);
    pos += 2 + length;
  }
  return b;
}

/** The square of the source a crop takes: `zoom` 1 (the largest square) or more, its centre at (cx, cy) in 0..1, kept inside. */
export function cropRect(width: number, height: number, zoom: number, cx: number, cy: number): { sx: number; sy: number; size: number } {
  const z = Math.max(1, Math.min(8, Number.isFinite(zoom) ? zoom : 1));
  const size = Math.max(1, Math.min(width, height) / z);
  const clamp = (v: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, v));
  const sx = clamp((Number.isFinite(cx) ? cx : 0.5) * width - size / 2, 0, width - size);
  const sy = clamp((Number.isFinite(cy) ? cy : 0.5) * height - size / 2, 0, height - size);
  return { sx, sy, size };
}

export interface Quantized {
  /** RGB of each colour. */
  palette: Uint8Array;
  /** The alpha of each colour (255: opaque). */
  alpha: Uint8Array;
  /** The colour of each pixel. */
  indices: Uint8Array;
  colors: number;
}

/**
 * At most 256 colours for the pixels (RGBA): the colours themselves when there are no more, else a median cut (each
 * box of colours split at the median of its widest channel until there are 256), each colour the mean of its box.
 */
export function quantize(rgba: Uint8Array | Uint8ClampedArray, max = 256): Quantized {
  const n = rgba.length / 4;
  const indices = new Uint8Array(n);
  const exact = new Map<number, number>();
  for (let i = 0; i < n; i++) {
    const key = ((rgba[4 * i] << 24) | (rgba[4 * i + 1] << 16) | (rgba[4 * i + 2] << 8) | rgba[4 * i + 3]) >>> 0;
    let k = exact.get(key);
    if (k === undefined) {
      if (exact.size >= max) {
        exact.clear();
        break;
      }
      k = exact.size;
      exact.set(key, k);
    }
    indices[i] = k;
  }
  if (exact.size > 0) {
    const palette = new Uint8Array(exact.size * 3), alpha = new Uint8Array(exact.size);
    for (const [key, k] of exact) {
      palette[3 * k] = key >>> 24;
      palette[3 * k + 1] = (key >>> 16) & 255;
      palette[3 * k + 2] = (key >>> 8) & 255;
      alpha[k] = key & 255;
    }
    return { palette, alpha, indices, colors: exact.size };
  }
  // Median cut over the pixels' indices.
  let boxes: Uint32Array[] = [Uint32Array.from({ length: n }, (_, i) => i)];
  const range = (box: Uint32Array) => {
    let best = 0, channel = 0;
    for (let c = 0; c < 4; c++) {
      let lo = 255, hi = 0;
      for (const p of box) {
        const v = rgba[4 * p + c];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
      }
      if (hi - lo > best) {
        best = hi - lo;
        channel = c;
      }
    }
    return { best, channel };
  };
  // The widest channel of each box, kept as boxes split (each pixel is read once per split it is in).
  let ranges = boxes.map(range);
  while (boxes.length < max) {
    let pick = -1, widest = 0;
    boxes.forEach((box, i) => {
      if (box.length < 2) return;
      const score = ranges[i].best * Math.log2(box.length + 1);
      if (score > widest) {
        widest = score;
        pick = i;
      }
    });
    if (pick < 0) break;
    const channel = ranges[pick].channel;
    const box = Array.from(boxes[pick]).sort((a, b) => rgba[4 * a + channel] - rgba[4 * b + channel]);
    const mid = box.length >> 1;
    const lo = Uint32Array.from(box.slice(0, mid)), hi = Uint32Array.from(box.slice(mid));
    boxes = [...boxes.slice(0, pick), lo, hi, ...boxes.slice(pick + 1)];
    ranges = [...ranges.slice(0, pick), range(lo), range(hi), ...ranges.slice(pick + 1)];
  }
  const palette = new Uint8Array(boxes.length * 3), alpha = new Uint8Array(boxes.length);
  boxes.forEach((box, k) => {
    const sum = [0, 0, 0, 0];
    for (const p of box) {
      for (let c = 0; c < 4; c++) sum[c] += rgba[4 * p + c];
      indices[p] = k;
    }
    for (let c = 0; c < 3; c++) palette[3 * k + c] = Math.round(sum[c] / box.length);
    alpha[k] = Math.round(sum[3] / box.length);
  });
  return { palette, alpha, indices, colors: boxes.length };
}

/** zlib (deflate) of the bytes, by the browser's own CompressionStream. */
export async function deflate(data: Uint8Array): Promise<Uint8Array> {
  const cs = new CompressionStream('deflate');
  const writer = cs.writable.getWriter();
  void writer.write(data as Uint8Array<ArrayBuffer>);
  void writer.close();
  const parts: Uint8Array[] = [];
  const reader = cs.readable.getReader();
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    parts.push(value);
  }
  return concat(parts);
}

function chunk(type: string, data: Uint8Array): Uint8Array {
  const out = new Uint8Array(12 + data.length);
  const view = new DataView(out.buffer);
  view.setUint32(0, data.length);
  for (let i = 0; i < 4; i++) out[4 + i] = type.charCodeAt(i);
  out.set(data, 8);
  view.setUint32(8 + data.length, crc32(out, 4, 8 + data.length));
  return out;
}

function concat(parts: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
}

function png(ihdr: { depth: number; color: number }, idat: Uint8Array, extra: Uint8Array[] = []): Uint8Array {
  const head = new Uint8Array(13);
  const v = new DataView(head.buffer);
  v.setUint32(0, LOGO_SIZE);
  v.setUint32(4, LOGO_SIZE);
  head[8] = ihdr.depth;
  head[9] = ihdr.color;
  return concat([Uint8Array.from(PNG_SIGNATURE), chunk('IHDR', head), ...extra, chunk('IDAT', idat), chunk('IEND', new Uint8Array())]);
}

/** A palette PNG of 256 x 256 pixels (1, 2, 4 or 8 bits deep, as few as the colours need), tRNS if any is not opaque. */
export async function encodePalettePng(q: Quantized): Promise<Uint8Array> {
  const depth = q.colors <= 2 ? 1 : q.colors <= 4 ? 2 : q.colors <= 16 ? 4 : 8;
  const perByte = 8 / depth;
  const rowBytes = Math.ceil(LOGO_SIZE / perByte);
  const raw = new Uint8Array((rowBytes + 1) * LOGO_SIZE);
  for (let y = 0; y < LOGO_SIZE; y++) {
    const row = y * (rowBytes + 1) + 1;
    for (let x = 0; x < LOGO_SIZE; x++) {
      const k = q.indices[y * LOGO_SIZE + x];
      raw[row + Math.floor(x / perByte)] |= k << (8 - depth * ((x % perByte) + 1));
    }
  }
  let last = -1;
  q.alpha.forEach((a, i) => {
    if (a !== 255) last = i;
  });
  const extra = [chunk('PLTE', q.palette)];
  if (last >= 0) extra.push(chunk('tRNS', q.alpha.slice(0, last + 1)));
  return png({ depth, color: 3 }, await deflate(raw), extra);
}

/** A full-colour PNG (RGB if every pixel is opaque, else RGBA), each row with the filter that leaves the least to compress. */
export async function encodeTruecolorPng(rgba: Uint8Array | Uint8ClampedArray): Promise<Uint8Array> {
  let opaque = true;
  for (let i = 3; i < rgba.length; i += 4) if (rgba[i] !== 255) opaque = false;
  const bpp = opaque ? 3 : 4;
  const stride = LOGO_SIZE * bpp;
  const pixels = new Uint8Array(stride * LOGO_SIZE);
  for (let i = 0, j = 0; i < rgba.length; i += 4) {
    pixels[j++] = rgba[i];
    pixels[j++] = rgba[i + 1];
    pixels[j++] = rgba[i + 2];
    if (!opaque) pixels[j++] = rgba[i + 3];
  }
  const raw = new Uint8Array((stride + 1) * LOGO_SIZE);
  const line = new Uint8Array(stride);
  for (let y = 0; y < LOGO_SIZE; y++) {
    const cur = y * stride, prev = (y - 1) * stride;
    let bestSum = Infinity, bestType = 0;
    const best = new Uint8Array(stride);
    for (let type = 0; type < 5; type++) {
      let sum = 0;
      for (let i = 0; i < stride; i++) {
        const a = i >= bpp ? pixels[cur + i - bpp] : 0, b = y > 0 ? pixels[prev + i] : 0, c = y > 0 && i >= bpp ? pixels[prev + i - bpp] : 0;
        const x = pixels[cur + i];
        let p: number;
        if (type === 0) p = x;
        else if (type === 1) p = x - a;
        else if (type === 2) p = x - b;
        else if (type === 3) p = x - ((a + b) >> 1);
        else {
          const pa = Math.abs(b - c), pb = Math.abs(a - c), pc = Math.abs(a + b - 2 * c);
          p = x - (pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
        }
        line[i] = p & 255;
        sum += line[i] < 128 ? line[i] : 256 - line[i];
      }
      if (sum < bestSum) {
        bestSum = sum;
        bestType = type;
        best.set(line);
      }
    }
    raw[y * (stride + 1)] = bestType;
    raw.set(best, y * (stride + 1) + 1);
  }
  return png({ depth: 8, color: opaque ? 2 : 6 }, await deflate(raw));
}

export interface EncodedLogo {
  bytes: Uint8Array;
  format: LogoFormat;
  /** How it was made: "PNG, 12 colours", "PNG", "JPEG 80%". */
  kind: string;
}

/**
 * The logo of 256 x 256 pixels (RGBA), as small as it gets: a palette PNG, a full-colour PNG and a JPEG (by `jpeg`, the
 * canvas's encoder, its metadata left out; its quality lowered while it is larger than the cap), the smallest the rules
 * take. A picture with transparency is a PNG while one fits (a JPEG has no transparency). Null if none fits.
 */
export async function encodeLogo(rgba: Uint8Array | Uint8ClampedArray, jpeg?: (quality: number) => Promise<Uint8Array | null>): Promise<EncodedLogo | null> {
  if (rgba.length !== LOGO_SIZE * LOGO_SIZE * 4) throw new Error('A logo is 256 x 256 pixels');
  const candidates: EncodedLogo[] = [];
  const q = quantize(rgba);
  candidates.push({ bytes: await encodePalettePng(q), format: 'png', kind: `PNG, ${q.colors} colours` });
  candidates.push({ bytes: await encodeTruecolorPng(rgba), format: 'png', kind: 'PNG' });
  let transparent = false;
  for (let i = 3; i < rgba.length; i += 4) if (rgba[i] !== 255) transparent = true;
  const fitting = (list: EncodedLogo[]) => list.filter((c) => c.bytes.length <= MAX_LOGO_BYTES && checkLogo(c.bytes).ok).sort((a, b) => a.bytes.length - b.bytes.length);
  if (jpeg && !(transparent && fitting(candidates).length > 0)) {
    for (const quality of JPEG_QUALITIES) {
      const made = await jpeg(quality);
      if (!made) break;
      const bytes = stripJpeg(made);
      candidates.push({ bytes, format: 'jpeg', kind: `JPEG ${Math.round(quality * 100)}%` });
      if (bytes.length <= MAX_LOGO_BYTES) break;
    }
  }
  return fitting(candidates)[0] ?? null;
}

export function toBase64(bytes: Uint8Array): string {
  let s = '';
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(s);
}

/** About the virtual bytes of a transaction that sets a logo of `size` bytes (the logo goes whole into its marker). */
export function logoVsize(size: number): number {
  return size + 260;
}
