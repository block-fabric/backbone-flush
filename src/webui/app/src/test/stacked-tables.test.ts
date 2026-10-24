// The tables of the pages' panels (ba.css .ba-table) stack into cards in a narrow panel, each cell under the words of its
// column (use:stacked gives them: lib/actions.ts). One without them shows values alone, unlabelled, one per row (a
// pool's slippage, 2026-10-23): every .ba-table is either labelled (use:stacked) or kept a table (ba-nostack).
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { fireEvent, render, screen, within } from '@testing-library/svelte';
import { describe, expect, it } from 'vitest';
import DepthChart from '../chain/ba/DepthChart.svelte';

function svelteFiles(dir: string, out: string[] = []): string[] {
  for (const f of readdirSync(dir)) {
    const p = join(dir, f);
    if (statSync(p).isDirectory()) svelteFiles(p, out);
    else if (p.endsWith('.svelte')) out.push(p);
  }
  return out;
}

describe('the tables of the panels', () => {
  it('stack into cards with their labels, or stay tables (ba-nostack)', () => {
    const bad: string[] = [];
    let seen = 0;
    for (const file of svelteFiles(join(__dirname, '..'))) {
      const text = readFileSync(file, 'utf8');
      for (const m of text.matchAll(/<table\b[^>]*>/g)) {
        const tag = m[0];
        if (!/class="[^"]*\bba-table\b/.test(tag)) continue;
        seen++;
        if (!/\buse:stacked\b/.test(tag) && !/\bba-nostack\b/.test(tag)) bad.push(`${file.slice(file.indexOf('src/'))}: ${tag}`);
      }
    }
    expect(seen).toBeGreaterThan(5);
    expect(bad).toEqual([]);
  });
});

describe('a pool\'s slippage (ba/DepthChart.svelte)', () => {
  it('one table each way, four labelled columns kept as a table, a dozen sizes at first and all on asking', async () => {
    (globalThis as Record<string, unknown>).ResizeObserver ??= class {
      observe() {}
      unobserve() {}
      disconnect() {}
    };
    const depth = Array.from({ length: 20 }, (_, i) => [
      { side: 'sell_a' as const, size_percent: i + 1, amount_in: i + 1, amount_out: (i + 1) / 10, avg_price: 0.1, price_after: 0.1, price_impact: i / 10 },
      { side: 'sell_b' as const, size_percent: i + 1, amount_in: (i + 1) / 10, amount_out: i + 1, avg_price: 10, price_after: 10, price_impact: i / 10 },
    ]).flat();
    render(DepthChart, { props: { depth, labelA: 'GOLD', labelB: 'ECX', decimalsA: 2, decimalsB: 8 } });
    const tables = screen.getAllByRole('table', { name: /^Slippage, selling/ });
    expect(tables.map((x) => x.getAttribute('aria-label'))).toEqual(['Slippage, selling GOLD', 'Slippage, selling ECX']);
    for (const table of tables) {
      expect(table.classList.contains('ba-nostack')).toBe(true);
      expect(within(table).getAllByRole('columnheader')).toHaveLength(4);
      expect(within(table).getAllByRole('row')).toHaveLength(13);
    }
    await fireEvent.click(screen.getByRole('button', { name: 'All 20 sizes' }));
    expect(within(tables[0]).getAllByRole('row')).toHaveLength(21);
  });
});
