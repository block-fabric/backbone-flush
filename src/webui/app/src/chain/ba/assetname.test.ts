// The one identity of an asset in the pages (AssetBadge.svelte, AssetName.svelte): its logo, else the generated mark;
// its name, then its ticker muted ("Theodore Brosevelt $BROSEVELT"); no ticker, its number; no name, $TICKER alone; the
// full text in the title; and the same as one line of text for what the host draws (model.ts: assetText).
import { render } from '@testing-library/svelte';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import AssetBadge from './AssetBadge.svelte';
import AssetName from './AssetName.svelte';
import { assetText, assetTitle } from './model';

const ID = 'b'.repeat(64);
// A PNG of 1 × 1 (the badge takes a data: URL of PNG or JPEG bytes as a preview).
const PNG = 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNgYGD4DwABBAEAwS2OUAAAAABJRU5ErkJggg==';

function parts(c: HTMLElement): string[] {
  return [...c.querySelectorAll('.asset-name > *')].map((e) => e.textContent ?? '');
}

describe('an asset\'s identity', () => {
  it('the logo, the name, then $TICKER muted; copied as "Name $TICKER"', () => {
    const r = render(AssetBadge, { id: ID, label: 'BROSEVELT', display: 'Theodore Brosevelt', preview: PNG });
    const img = r.container.querySelector('img.mark')!;
    expect(img.getAttribute('src')).toBe(PNG);
    expect(parts(r.container)).toEqual(['Theodore Brosevelt', '$BROSEVELT']);
    expect(r.container.querySelector('.second')!.getAttribute('translate')).toBe('no');
    expect(r.container.querySelector('.asset-name')!.textContent).toBe('Theodore Brosevelt $BROSEVELT');
  });

  it('without a logo, the generated mark with the ticker\'s initials', () => {
    const r = render(AssetBadge, { id: ID, label: 'BROSEVELT', display: 'Theodore Brosevelt' });
    expect(r.container.querySelector('img')).toBeNull();
    expect(r.container.querySelector('.mark')!.textContent).toBe('BR');
    // A preview of another kind is not shown: the mark again.
    const svg = render(AssetBadge, { id: ID, label: 'GOLD', preview: 'data:image/svg+xml;base64,PHN2Zz4=' });
    expect(svg.container.querySelector('img')).toBeNull();
    expect(svg.container.querySelector('.mark')!.textContent).toBe('GO');
  });

  it('a long name gives way with an ellipsis, its full text in the title', () => {
    const long = 'The Extraordinarily Long Name of a Token That Goes On and On';
    const r = render(AssetName, { label: 'LONG', display: long });
    const name = r.container.querySelector('.asset-name')!;
    expect(name.getAttribute('title')).toBe(`${long} $LONG`);
    expect(r.container.querySelector('.first')!.textContent).toBe(long);
    // The rule that truncates it (jsdom computes no layout: the style sheet is the check).
    const css = readFileSync(join(__dirname, 'AssetName.svelte'), 'utf8');
    expect(css).toMatch(/\.first\) \{[^}]*min-width: 0;[^}]*text-overflow: ellipsis/);
  });

  it('no ticker: the name, and its number muted after it (names are not unique)', () => {
    const r = render(AssetName, { label: 'gold bar', seq: '1739-0029' });
    expect(parts(r.container)).toEqual(['gold bar', '#1739-0029']);
    expect(r.container.querySelector('.asset-name')!.getAttribute('title')).toBe('gold bar · #1739-0029');
    const bare = render(AssetName, { label: 'gold bar' });
    expect(parts(bare.container)).toEqual(['gold bar']);
  });

  it('no name: $TICKER alone; its number faint after it only where asked', () => {
    expect(parts(render(AssetName, { label: 'GOLD' }).container)).toEqual(['$GOLD']);
    expect(parts(render(AssetName, { label: 'GOLD', seq: '1739-0029' }).container)).toEqual(['$GOLD', '#1739-0029']);
  });

  it('neither (a hash for a label): its number, else the hash; ECX by its ticker alone', () => {
    expect(parts(render(AssetName, { label: '0xabcdef123456…', seq: '1739-0029' }).container)).toEqual(['#1739-0029']);
    expect(parts(render(AssetName, { label: '0xabcdef123456…' }).container)).toEqual(['0xabcdef123456…']);
    expect(parts(render(AssetBadge, { id: '0'.repeat(64), label: 'ECX' }).container)).toEqual(['ECX']);
  });

  it('as one line of text, for toasts and dialogs', () => {
    expect(assetText('BROSEVELT', 'Theodore Brosevelt')).toBe('Theodore Brosevelt $BROSEVELT');
    expect(assetText('GOLD')).toBe('$GOLD');
    expect(assetText('gold bar', undefined, '1739-0029')).toBe('gold bar #1739-0029');
    expect(assetText('0xabcdef…', undefined, '1739-0029')).toBe('#1739-0029');
    expect(assetText('ECX')).toBe('ECX');
    // Beside a display name, a label of another shape (from before the rules of tickers) is still its ticker.
    expect(assetText('GOLDEN-LONG', 'Gold Bar')).toBe('Gold Bar $GOLDEN-LONG');
    expect(assetTitle('GOLD', 'Gold Bar Token, 1 g', '1739-0029')).toBe('Gold Bar Token, 1 g $GOLD · #1739-0029');
  });
});
