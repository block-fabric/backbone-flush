// The one identity of an asset in the pages (AssetBadge.svelte, AssetName.svelte): its logo, else the generated mark;
// its name, then its identity muted in a mark of its own ($TICKER and its number, else its id shortened), so that a
// display name cannot pass for another asset (security review 2026-10-24); the full text in the title; the same as one
// line of text for what the host draws (model.ts: assetText), and the confirmations' words, unique part first (nameOf).
import { render } from '@testing-library/svelte';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import AssetBadge from './AssetBadge.svelte';
import AssetName from './AssetName.svelte';
import { assetText, assetTitle, escapeName } from './model';

const ID = 'b'.repeat(64);
// A PNG of 1 × 1 (the badge takes a data: URL of PNG or JPEG bytes as a preview).
const PNG = 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNgYGD4DwABBAEAwS2OUAAAAABJRU5ErkJggg==';

/** The words shown: the name (plain), and the identity (its mark). */
function shown(c: HTMLElement): { name: string | null; ident: string | null } {
  const ident = c.querySelector('.ident');
  const first = c.querySelector('.asset-name > .first:not(.ident)');
  return { name: first?.textContent ?? null, ident: ident?.textContent ?? null };
}

describe('an asset\'s identity', () => {
  it('the logo, the name, then $TICKER and its number in their mark; copied as "Name $TICKER #number"', () => {
    const r = render(AssetBadge, { id: ID, label: 'BROSEVELT', display: 'Theodore Brosevelt', seq: '1739-0029', preview: PNG });
    expect(r.container.querySelector('img.mark')!.getAttribute('src')).toBe(PNG);
    expect(shown(r.container)).toEqual({ name: 'Theodore Brosevelt', ident: '$BROSEVELT #1739-0029' });
    expect(r.container.querySelector('.ticker')!.getAttribute('translate')).toBe('no');
    expect(r.container.querySelector('.asset-name')!.textContent).toBe('Theodore Brosevelt $BROSEVELT #1739-0029');
  });

  it('without a number, its id shortened: the unique part is always there', () => {
    const r = render(AssetBadge, { id: ID, label: 'GOLD', display: 'Gold Bar' });
    expect(shown(r.container)).toEqual({ name: 'Gold Bar', ident: '$GOLD 0xbbbbbbbb…bbbbbbbb' });
  });

  it('without a logo, the generated mark with the ticker\'s initials', () => {
    const r = render(AssetBadge, { id: ID, label: 'BROSEVELT', display: 'Theodore Brosevelt' });
    expect(r.container.querySelector('img')).toBeNull();
    expect(r.container.querySelector('.mark')!.textContent).toBe('BR');
    const svg = render(AssetBadge, { id: ID, label: 'GOLD', preview: 'data:image/svg+xml;base64,PHN2Zz4=' });
    expect(svg.container.querySelector('img')).toBeNull();
    expect(svg.container.querySelector('.mark')!.textContent).toBe('GO');
  });

  it('a long name gives way with an ellipsis, its full text in the title', () => {
    const long = 'The Extraordinarily Long Name of a Token That Goes On and On';
    const r = render(AssetName, { label: 'LONG', display: long, seq: '1739-0029' });
    expect(r.container.querySelector('.asset-name')!.getAttribute('title')).toBe(`${long} $LONG #1739-0029`);
    expect(shown(r.container).name).toBe(long);
    const css = readFileSync(join(__dirname, 'AssetName.svelte'), 'utf8');
    expect(css).toMatch(/\.first\) \{[^}]*min-width: min\(6em, 100%\);[^}]*text-overflow: ellipsis/);
    expect(css).toMatch(/\.ident \{[^}]*flex: none;/);
  });

  it('no ticker: the name, and its number in the mark (names are not unique)', () => {
    const r = render(AssetName, { label: 'gold bar', seq: '1739-0029' });
    expect(shown(r.container)).toEqual({ name: 'gold bar', ident: '#1739-0029' });
    expect(r.container.querySelector('.asset-name')!.getAttribute('title')).toBe('gold bar #1739-0029');
  });

  it('no name: $TICKER and its number alone, in the place of the name', () => {
    const r = render(AssetName, { label: 'GOLD', seq: '1739-0029' });
    expect(shown(r.container)).toEqual({ name: null, ident: '$GOLD #1739-0029' });
    expect(r.container.querySelector('.ident.alone')).not.toBeNull();
  });

  it('neither (a hash for a label): its number, else the label; ECX by its ticker alone', () => {
    expect(shown(render(AssetName, { label: '0xabcdef123456…', seq: '1739-0029' }).container).ident).toBe('#1739-0029');
    expect(render(AssetName, { label: '0xabcdef123456…' }).container.textContent).toBe('0xabcdef123456…');
    const ecx = render(AssetBadge, { id: '0'.repeat(64), label: 'ECX' });
    expect(ecx.container.querySelector('.asset-name')!.textContent).toBe('ECX');
    expect(ecx.container.querySelector('.ident')).toBeNull();
  });

  it('a display name that imitates a ticker or a number stays plain text, the real ones in their mark after it', () => {
    const r = render(AssetBadge, { id: ID, label: 'FAKE', display: 'Bitcoin $ECX #1', seq: '9031-1144' });
    // Its "$" and "#" boxed and escaped, as the title writes them: never in the mark.
    expect(shown(r.container)).toEqual({ name: 'Bitcoin \\$ECX \\#1', ident: '$FAKE #9031-1144' });
    expect(r.container.querySelector('.ident')!.textContent).not.toContain('ECX');
    // In the title (text, no mark): escaped.
    expect(r.container.querySelector('.asset-name')!.getAttribute('title')).toBe('Bitcoin \\$ECX \\#1 $FAKE #9031-1144');
    const quoted = render(AssetName, { label: 'FAKE', display: '"FLUSH"', seq: '9031-1144' });
    expect(shown(quoted.container).name).toBe('\\"FLUSH\\"');
    expect(quoted.container.querySelector('.asset-name')!.getAttribute('title')).toBe('\\"FLUSH\\" $FAKE #9031-1144');
  });

  it('bidirectional and zero-width characters stay inside SafeText (dir="auto", translate="no"), and are written out in text', () => {
    const name = 'Gold‮raw​';
    const r = render(AssetName, { label: 'FAKE', display: name, seq: '9031-1144' });
    const first = r.container.querySelector('.asset-name > .first')!;
    // Written out and boxed, never applied.
    expect(first.textContent).toBe('Gold\\u{202E}raw\\u{200B}');
    expect(first.querySelectorAll('mark.lookalike')).toHaveLength(2);
    expect(first.getAttribute('dir')).toBe('auto');
    expect(first.getAttribute('translate')).toBe('no');
    expect(r.container.querySelector('.ticker')!.getAttribute('dir')).toBe('auto');
    expect(assetText('FAKE', name, '9031-1144')).toBe('Gold\\u{202E}raw\\u{200B} $FAKE #9031-1144');
  });

  it('as one line of text, for toasts and dialogs', () => {
    expect(assetText('BROSEVELT', 'Theodore Brosevelt', '1739-0029')).toBe('Theodore Brosevelt $BROSEVELT #1739-0029');
    expect(assetText('GOLD', undefined, undefined, ID)).toBe('$GOLD 0xbbbbbbbb…bbbbbbbb');
    expect(assetText('gold bar', undefined, '1739-0029')).toBe('gold bar #1739-0029');
    expect(assetText('0xabcdef…', undefined, '1739-0029')).toBe('#1739-0029');
    expect(assetText('ECX')).toBe('ECX');
    // Beside a display name, a label outside the rules of tickers is a name too, never $TICKER.
    expect(assetText('GOLDEN-LONG', 'Gold Bar')).toBe('Gold Bar GOLDEN-LONG');
    expect(assetTitle('GOLD', 'Gold Bar Token, 1 g', '1739-0029')).toBe('Gold Bar Token, 1 g $GOLD #1739-0029');
    expect(escapeName('a\\b "c" $d #e')).toBe('a\\\\b \\"c\\" \\$d \\#e');
  });

  it('only a label of the rules of tickers is shown as $TICKER; any other is plain text, as a name', () => {
    // 'EC X', 'ECX"', and ECX with a Cyrillic Е (U+0415): not tickers (bitassets::IsTicker), never in the mark.
    for (const label of ['EC X', 'ECX"', 'ЕCX', 'gold']) {
      const r = render(AssetName, { label, seq: '9031-1144' });
      expect(r.container.querySelector('.ticker'), label).toBeNull();
      expect(shown(r.container), label).toEqual({ name: label === 'ECX"' ? 'ECX\\"' : label, ident: '#9031-1144' });
      expect(assetText(label, undefined, '9031-1144')).not.toContain('$');
      // Beside a display name: after it, outside the mark.
      const d = render(AssetName, { label, display: 'Bitcoin', seq: '9031-1144' });
      expect(d.container.querySelector('.alias')?.textContent, label).toBe(label === 'ECX"' ? 'ECX\\"' : label);
      expect(d.container.querySelector('.ident')?.textContent, label).toBe('#9031-1144');
    }
    expect(assetText('ECX"', 'Bitcoin', '9031-1144')).toBe('Bitcoin ECX\\" #9031-1144');
    expect(assetText('ЕCX', undefined, '9031-1144')).toBe('ЕCX #9031-1144');
    expect(shown(render(AssetName, { label: 'GOLD2', seq: '9031-1144' }).container).ident).toBe('$GOLD2 #9031-1144');
  });

  it('two ids that share their first 8 hex characters look different (64 bits of the id shown)', () => {
    const one = '1a2b3c4d' + '0'.repeat(48) + '11111111';
    const two = '1a2b3c4d' + '0'.repeat(48) + '22222222';
    const a = shown(render(AssetName, { label: 'GOLD', id: one }).container).ident;
    const b = shown(render(AssetName, { label: 'GOLD', id: two }).container).ident;
    expect(a).toBe('$GOLD 0x1a2b3c4d…11111111');
    expect(b).toBe('$GOLD 0x1a2b3c4d…22222222');
    expect(assetText('GOLD', undefined, undefined, one)).not.toBe(assetText('GOLD', undefined, undefined, two));
  });

  it('a name by the allowlist (NFKC), and on top of it ids, numbers, bar-like letters and marks beyond 2: marked and escaped', () => {
    const marks = (c: HTMLElement) => [...c.querySelectorAll('.asset-name > .first mark.lookalike')].map((m) => m.textContent);
    const name = (display: string) => render(AssetName, { label: 'FAKE', display, seq: '9031-1144' }).container;
    // Fullwidth "＄" and "＃", bars, a double bar, Arabic-Indic digits: marked, escaped as the title writes them.
    expect(marks(name('＄ECX'))).toEqual(['\\$']);
    expect(marks(name('＃1739-0029'))).toEqual(['\\#1739-0029']);
    expect(marks(name('Coin ｜ECX'))).toEqual(['\\|']);
    expect(marks(name('A ‖ B'))).toEqual(['\\u{2016}']);
    expect(marks(name('١٧٣٩-٠٠٢٩'))).toEqual(['\\١٧٣٩-٠٠٢٩']);
    expect(marks(name('Fake | ECX | #1-1'))).toEqual(['\\|', '\\|', '\\#1-1']);
    // Ids and numbers, on top of the allowlist.
    expect(marks(name('Gold 0x1a2b'))).toEqual(['\\0x1a2b']);
    expect(marks(name('x 1739-0029'))).toEqual(['\\1739-0029']);
    // Bar-like letters; I, l, ا, Ι only alone between spaces.
    expect(marks(name('A ǀ B'))).toEqual(['\\u{1C0}']);
    expect(marks(name('Fake I ECX l x'))).toEqual(['\\I', '\\l']);
    expect(marks(name('Ilona'))).toEqual([]);
    // An overlay mark, and three marks stacked on one letter (the third marked).
    expect(marks(name('Go\u20D2ld'))).toEqual(['\\u{20D2}']);
    expect(marks(name('Gq\u0301\u0302\u0303x'))).toEqual(['\\u{303}']);
    // Plain: letters with their accents, short numbers.
    expect(marks(name('Café Crème'))).toEqual([]);
    expect(marks(name('Top 100 (v2)'))).toEqual([]);
    // Fullwidth letters folded (NFKC): shown as the letters they are.
    const folded = name('ＥＣＸ Coin');
    expect(folded.querySelector('.asset-name > .first')!.textContent).toBe('ECX Coin');
    expect(marks(folded)).toEqual([]);
    // The text, copied or read out: the name as assetTitle escapes it, then $TICKER #number.
    const fake = name('Fake | ECX | #1-1');
    expect(fake.querySelector('.asset-name')!.textContent).toBe('Fake \\| ECX \\| \\#1-1 $FAKE #9031-1144');
    expect(fake.querySelector('.asset-name')!.getAttribute('title')).toBe('Fake \\| ECX \\| \\#1-1 $FAKE #9031-1144');
    expect(fake.querySelector('.ident .vh')!.textContent).toBe('$');
    expect([...fake.querySelectorAll('.sep')].every((x) => x.textContent === '' && x.getAttribute('aria-hidden') === 'true')).toBe(true);
    expect(assetText('FAKE', 'x 1739-0029', '9031-1144')).toBe('x \\1739-0029 $FAKE #9031-1144');
  });

  it('a long name is cut at the end of a piece, never inside a marked one, with a fade and no "…" of text', () => {
    const proto = HTMLElement.prototype;
    const sh = Object.getOwnPropertyDescriptor(proto, 'scrollHeight');
    const ch = Object.getOwnPropertyDescriptor(proto, 'clientHeight');
    Object.defineProperty(proto, 'scrollHeight', { configurable: true, get: () => 40 });
    Object.defineProperty(proto, 'clientHeight', { configurable: true, get: () => 20 });
    try {
      const r = render(AssetName, { label: 'FAKE', display: 'A very long name of a token that goes on 0x1a2b3c4d5e6f and on', seq: '9031-1144' });
      const first = r.container.querySelector<HTMLElement>('.asset-name > .first')!;
      expect(first.classList.contains('cut')).toBe(true);
      expect(first.textContent).not.toContain('…');
      // The hex run is one marked piece, kept whole (an inline block that wraps as a whole).
      expect([...first.querySelectorAll('mark.lookalike')].map((m) => m.textContent)).toEqual(['\\0x1a2b3c4d5e6f']);
    } finally {
      if (sh) Object.defineProperty(proto, 'scrollHeight', sh);
      if (ch) Object.defineProperty(proto, 'clientHeight', ch);
    }
    const css = readFileSync(join(__dirname, 'AssetName.svelte'), 'utf8');
    expect(css).toMatch(/\.name-text\) \{[^}]*white-space: normal;[^}]*text-overflow: clip;[^}]*overflow: hidden;/);
    expect(css).toMatch(/\.lookalike \{[^}]*display: inline-block;[^}]*white-space: nowrap;/);
    expect(css).toMatch(/\.name-text\.cut\) \{[^}]*mask-image/);
  });
});
