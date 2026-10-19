// The logos of assets in the pages: the picker's work (crop, quantize, encode, strip, the check of the rules, the same as
// src/bitassets/logo.cpp on the samples of its unit tests), the cache of the host's data: URLs, the badge that shows a
// logo or falls back to the initials, and the list of the assets a wallet holds.
import { render, waitFor } from '@testing-library/svelte';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { afterEach, describe, expect, it } from 'vitest';
import { setTransport, type BridgeRequest } from '../../lib/bridge';
import AssetBadge from './AssetBadge.svelte';
import { heldRows } from './held';
import { checkLogo, cropRect, crc32, encodeLogo, encodePalettePng, encodeTruecolorPng, LOGO_SIZE, MAX_LOGO_BYTES, quantize, stripJpeg, toBase64 } from './logo';
import { isLogoUrl, logoUrl, resetLogos } from './logos.svelte';
import type { Holding } from './model';

/** The samples of the unit tests of the node (src/test/bitassets_logo_samples.h). */
function sample(name: string): Uint8Array {
  const text = readFileSync(join(__dirname, '../../../../../test/bitassets_logo_samples.h'), 'utf8');
  const body = new RegExp(`LOGO_${name}\\{([\\s\\S]*?)\\};`).exec(text)![1];
  const hex = [...body.matchAll(/"([0-9a-f]*)"/g)].map((m) => m[1]).join('');
  return Uint8Array.from(hex.match(/../g)!.map((h) => parseInt(h, 16)));
}

function pixels(fn: (x: number, y: number) => [number, number, number, number]): Uint8Array {
  const out = new Uint8Array(LOGO_SIZE * LOGO_SIZE * 4);
  for (let y = 0; y < LOGO_SIZE; y++) for (let x = 0; x < LOGO_SIZE; x++) out.set(fn(x, y), 4 * (y * LOGO_SIZE + x));
  return out;
}

function chunk(type: string, data: number[]): number[] {
  const body = [...type].map((c) => c.charCodeAt(0)).concat(data);
  const crc = crc32(Uint8Array.from(body));
  const be = (n: number) => [n >>> 24, (n >>> 16) & 255, (n >>> 8) & 255, n & 255];
  return [...be(data.length), ...body, ...be(crc)];
}

afterEach(() => {
  setTransport(null);
  resetLogos();
});

describe('the check of the rules, as the node does it', () => {
  it('takes the samples of the node, PNG and JPEG', () => {
    for (const name of ['PNG_PALETTE', 'PNG_RGBA']) expect(checkLogo(sample(name))).toEqual({ ok: true, format: 'png' });
    for (const name of ['JPEG_BASELINE', 'JPEG_PROGRESSIVE', 'JPEG_GRAY']) expect(checkLogo(sample(name))).toEqual({ ok: true, format: 'jpeg' });
  });

  it('refuses metadata, trailing bytes, other sizes and formats', () => {
    const png = sample('PNG_PALETTE');
    const at = 33; // after IHDR
    const withText = Uint8Array.from([...png.slice(0, at), ...chunk('tEXt', [104, 105]), ...png.slice(at)]);
    expect(checkLogo(withText)).toEqual({ ok: false, reason: 'bad-ba-logo-png-chunk' });
    expect(checkLogo(Uint8Array.from([...png, 0]))).toEqual({ ok: false, reason: 'bad-ba-logo-trailing' });
    const badCrc = png.slice();
    badCrc[29] ^= 1;
    expect(checkLogo(badCrc)).toEqual({ ok: false, reason: 'bad-ba-logo-png-crc' });
    const jpeg = sample('JPEG_BASELINE');
    const app0 = Uint8Array.from([0xff, 0xd8, 0xff, 0xe0, 0, 4, 1, 2, ...jpeg.slice(2)]);
    expect(checkLogo(app0)).toEqual({ ok: false, reason: 'bad-ba-logo-jpeg-metadata' });
    expect(checkLogo(new TextEncoder().encode('<svg xmlns="http://www.w3.org/2000/svg"/>'))).toEqual({ ok: false, reason: 'bad-ba-logo-format' });
    expect(checkLogo(new TextEncoder().encode('GIF89a'))).toEqual({ ok: false, reason: 'bad-ba-logo-format' });
    expect(checkLogo(new Uint8Array(MAX_LOGO_BYTES + 1))).toEqual({ ok: false, reason: 'bad-ba-logo-oversize' });
    expect(checkLogo(new Uint8Array())).toEqual({ ok: false, reason: 'bad-ba-logo-empty' });
  });

  it('strips the APPn and COM segments of a JPEG, and only them', () => {
    const jpeg = sample('JPEG_BASELINE');
    const dirty = Uint8Array.from([0xff, 0xd8, 0xff, 0xe0, 0, 6, 74, 70, 73, 70, 0xff, 0xfe, 0, 4, 104, 105, 0xff, 0xe1, 0, 3, 0, ...jpeg.slice(2)]);
    expect(checkLogo(dirty).ok).toBe(false);
    const clean = stripJpeg(dirty);
    expect([...clean]).toEqual([...jpeg]);
    expect(checkLogo(clean)).toEqual({ ok: true, format: 'jpeg' });
  });
});

describe('the picker: crop, colours, encoding', () => {
  it('crops the largest square at the centre, zoomed and moved within the picture', () => {
    expect(cropRect(1000, 500, 1, 0.5, 0.5)).toEqual({ sx: 250, sy: 0, size: 500 });
    expect(cropRect(1000, 500, 2, 0.5, 0.5)).toEqual({ sx: 375, sy: 125, size: 250 });
    expect(cropRect(1000, 500, 1, 0, 0)).toEqual({ sx: 0, sy: 0, size: 500 });
    expect(cropRect(1000, 500, 1, 1, 1)).toEqual({ sx: 500, sy: 0, size: 500 });
    expect(cropRect(300, 300, 99, NaN, 0.5).size).toBeCloseTo(300 / 8);
  });

  it('keeps the colours of a simple picture, and cuts a photo to 256', () => {
    const flat = quantize(pixels((x) => (x < 128 ? [255, 0, 0, 255] : [0, 0, 255, 128])));
    expect(flat.colors).toBe(2);
    expect([...flat.alpha].sort()).toEqual([128, 255]);
    const photo = quantize(pixels((x, y) => [x, y, (x * y) & 255, 255]));
    expect(photo.colors).toBe(256);
    expect(Math.max(...photo.indices)).toBeLessThan(256);
  });

  it('encodes PNGs the rules take: a palette with transparency, full colour', async () => {
    const logo = pixels((x, y) => ((x - 128) ** 2 + (y - 128) ** 2 < 100 ** 2 ? [240, 180, 40, 255] : [0, 0, 0, 0]));
    const palette = await encodePalettePng(quantize(logo));
    expect(checkLogo(palette)).toEqual({ ok: true, format: 'png' });
    expect(palette[24]).toBe(1); // 2 colours: 1 bit
    expect(new TextDecoder().decode(palette)).toContain('tRNS');
    const full = await encodeTruecolorPng(pixels((x, y) => [x, y, 128, 255]));
    expect(checkLogo(full)).toEqual({ ok: true, format: 'png' });
    expect(full[25]).toBe(2); // opaque: RGB
  });

  it('keeps the smallest that fits: a palette for a flat logo, a JPEG for a photo, never metadata', async () => {
    const flat = await encodeLogo(pixels((x) => (x < 128 ? [255, 0, 0, 255] : [0, 0, 255, 255])), async () => sample('JPEG_BASELINE'));
    expect(flat?.format).toBe('png');
    expect(flat?.kind).toBe('PNG, 2 colours');
    // A photo-like picture: its PNGs are larger than the cap; the JPEG (with a JFIF header the encoder writes) is kept, stripped.
    let seed = 7;
    const noise = pixels(() => {
      seed = (seed * 1103515245 + 12345) & 0x7fffffff;
      return [seed & 255, (seed >> 8) & 255, (seed >> 16) & 255, 255];
    });
    const jfif = Uint8Array.from([0xff, 0xd8, 0xff, 0xe0, 0, 4, 1, 2, ...sample('JPEG_BASELINE').slice(2)]);
    const photo = await encodeLogo(noise, async () => jfif);
    expect(photo?.format).toBe('jpeg');
    expect(checkLogo(photo!.bytes)).toEqual({ ok: true, format: 'jpeg' });
    expect(photo!.bytes.length).toBeLessThanOrEqual(MAX_LOGO_BYTES);
    // What fits is never larger than the cap, whatever the picture.
    const any = await encodeLogo(noise);
    if (any) expect(any.bytes.length).toBeLessThanOrEqual(MAX_LOGO_BYTES);
    expect(toBase64(Uint8Array.from([1, 2, 3]))).toBe('AQID');
  });
});

describe('showing logos', () => {
  const GOLD = 'a'.repeat(64);
  const URL = `data:image/png;base64,${toBase64(sample('PNG_PALETTE'))}`;

  it('asks the host once per asset and hash, and shows only PNG or JPEG data: URLs', async () => {
    const asked: unknown[] = [];
    setTransport(async (r: BridgeRequest) => {
      if (r.method === 'bitassets.logo') {
        asked.push(r.params);
        return { asset: GOLD, hash: 'f'.repeat(64), mime: 'image/png', size: 10, url: URL };
      }
      return null;
    });
    expect(logoUrl(GOLD, 'f'.repeat(64))).toBe('');
    await waitFor(() => expect(logoUrl(GOLD, 'f'.repeat(64))).toBe(URL));
    logoUrl(GOLD, 'f'.repeat(64));
    expect(asked).toEqual([{ asset: GOLD, hash: 'f'.repeat(64) }]);
    // No hash (no logo listed): nothing asked.
    expect(logoUrl(GOLD, undefined)).toBe('');
    expect(isLogoUrl(URL)).toBe(true);
    expect(isLogoUrl('data:image/svg+xml;base64,PHN2Zz4=')).toBe(false);
    expect(isLogoUrl('javascript:alert(1)')).toBe(false);
    expect(isLogoUrl('data:text/html;base64,PGI+')).toBe(false);
  });

  it('a URL of another kind from the host is not shown', async () => {
    setTransport(async () => ({ asset: GOLD, hash: 'e'.repeat(64), mime: 'image/png', size: 10, url: 'data:image/svg+xml;base64,PHN2Zz4=' }));
    logoUrl(GOLD, 'e'.repeat(64));
    await new Promise((r) => setTimeout(r, 20));
    expect(logoUrl(GOLD, 'e'.repeat(64))).toBe('');
  });

  it('the badge: the logo as an <img>, else the initials', async () => {
    const shown = render(AssetBadge, { id: GOLD, label: 'GOLD', preview: URL });
    const img = shown.container.querySelector('img')!;
    expect(img.getAttribute('src')).toBe(URL);
    expect(shown.container.textContent).not.toContain('GO ');
    shown.unmount();
    const fallback = render(AssetBadge, { id: GOLD, label: 'GOLD', preview: 'data:image/svg+xml;base64,PHN2Zz4=' });
    expect(fallback.container.querySelector('img')).toBeNull();
    expect(fallback.container.querySelector('.mark')!.textContent).toBe('GO');
    fallback.unmount();
    const chn = render(AssetBadge, { id: '0'.repeat(64), label: 'CHN', preview: URL });
    expect(chn.container.querySelector('img')).toBeNull();
  });
});

describe('the assets a wallet holds (Home, Overview)', () => {
  const h = (asset: string, label: string, balance: number, extra: Partial<Holding> = {}): Holding => ({
    asset,
    label,
    decimals: 2,
    balance,
    pending: 0,
    outputs: 1,
    control: false,
    control_pending: false,
    supply: 1000,
    fixed: false,
    ...extra,
  });

  it('by worth in CHN, then balance; the assets only controlled apart', () => {
    const list = [h('1'.repeat(64), 'A', 10), h('2'.repeat(64), 'B', 500), h('3'.repeat(64), 'C', 5), h('4'.repeat(64), 'D', 0, { control: true }), h('5'.repeat(64), 'E', 0, { pending: 3 })];
    const prices: Record<string, number> = { ['3'.repeat(64)]: 100, ['1'.repeat(64)]: 1 };
    const { rows, controlledOnly } = heldRows(list, (id) => prices[id], (id) => `seq-${id[0]}`);
    expect(rows.map((r) => r.holding.label)).toEqual(['C', 'A', 'B', 'E']);
    expect(rows[0].value).toBe(500);
    expect(rows[2].value).toBeUndefined();
    expect(rows[3].pending).toBe(true);
    expect(rows[0].seq).toBe('seq-3');
    expect(controlledOnly.map((r) => r.holding.label)).toEqual(['D']);
  });
});

describe('asset transfers in the transactions', () => {
  it('a send says where it went, a receive who paid it; the CSV and the search have them', async () => {
    const { ba } = await import('./store.svelte');
    const { wallets } = await import('../../lib/state.svelte');
    const { chainTxOf, counterparty, signedAmount } = await import('../txinfo.svelte');
    const LocalTxCell = (await import('../LocalTxCell.svelte')).default;
    wallets.current = 'w';
    ba.transfersWallet = 'w';
    ba.transfers = [
      { txid: '1'.repeat(64), time: 2, confirmations: 1, category: 'send', asset: 'a'.repeat(64), label: 'GOLD', seq: '1739-0029', decimals: 2, amount: '12.50', sign: -1, fee: 0.0000141, to: [{ address: 'rsc1qother', mine: false }], from: [], abandoned: false },
      { txid: '2'.repeat(64), time: 1, confirmations: 0, category: 'receive', asset: 'a'.repeat(64), label: 'GOLD', seq: '1739-0029', decimals: 2, amount: '3', sign: 1, to: [{ address: 'rsc1qme', mine: true }], from: [{ address: 'rsc1qpayer', mine: false }, { address: 'unknown', mine: false }], abandoned: false },
    ];
    const sent = chainTxOf('1'.repeat(64))!;
    expect(sent.type).toBe('Sent asset');
    expect(sent.search).toContain('rsc1qother');
    expect(sent.search).toContain('#1739-0029');
    expect(sent.csv).toEqual(['GOLD', '1739-0029', '-12.50', 'to rsc1qother']);
    const received = chainTxOf('2'.repeat(64))!;
    expect(received.type).toBe('Received asset');
    expect(received.csv[3]).toBe('from rsc1qpayer unknown');
    expect(signedAmount(ba.transfers[1])).toBe('+3.00');
    expect(counterparty(ba.transfers[0]).dir).toBe('to');
    expect(chainTxOf('3'.repeat(64))).toBeUndefined();
    const cell = render(LocalTxCell, { txid: '2'.repeat(64) });
    expect(cell.container.textContent).toContain('from');
    expect(cell.container.textContent).toContain('rsc1qpayer');
    expect(cell.container.textContent).toContain('+3.00');
    cell.unmount();
    ba.transfers = [];
    wallets.current = null;
  });
});
