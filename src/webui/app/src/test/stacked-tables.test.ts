// The tables of the pages' panels (ba.css .ba-table) stack into cards in a narrow panel, each cell under the words of its
// column (use:stacked gives them: lib/actions.ts). One without them shows values alone, unlabelled, one per row (a
// pool's slippage, 2026-10-23): every .ba-table is either labelled (use:stacked) or kept a table (ba-nostack).
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';

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
