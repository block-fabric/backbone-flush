// The coin of the chain is named by its ticker only (chain.json: coin.ticker, chain/config.ts: TICKER): Flush
// shows ECX and Layers, which shares these pages, CHN. A name of the coin written into the text of a page, a
// message or a dialog would show one chain's coin on the other (user feedback 2026-10-22: Flush's treasury said
// CHN), so none is: a ticker in the sources is only what the pages accept as input ('ECX' or 'CHN') or the id of
// the unit of the kit ('CHN', whose label is the ticker).
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, relative } from 'node:path';
import { describe, expect, it } from 'vitest';
import { CHAIN, TICKER } from '../chain/config';
import { treasuryRows } from '../chain/ba/fees';
import { CHN_ID, tickerProblem, nameProblem } from '../chain/ba/model';
import { UNITS } from '../lib/units';

const SRC = join(__dirname, '..');

function sources(dir: string): string[] {
  return readdirSync(dir).flatMap((name) => {
    const path = join(dir, name);
    if (statSync(path).isDirectory()) return name === 'test' ? [] : sources(path);
    return /\.(svelte|ts)$/.test(name) && !/\.test\.ts$/.test(name) ? [path] : [];
  });
}

/** The code without its comments (line and block comments, and those of the markup), lines kept. */
function code(text: string): string {
  const blank = (m: string) => m.replace(/[^\n]/g, ' ');
  return text
    .replace(/<!--[\s\S]*?-->/g, blank)
    .replace(/\/\*[\s\S]*?\*\//g, blank)
    .replace(/(^|[^:'"`\\])\/\/[^\n]*/g, (m, before: string) => before + blank(m.slice(before.length)));
}

/** What may name a coin in the code: input accepted as the chain's coin, and the kit's unit ids. */
const ALLOWED = [/=== '(ECX|CHN)'/g, /'CHN' as Unit/g, /id: 'CHN'/g, /Unit = 'CHN'/g, /, 'CHN'(?=[,)])/g];

describe('the name of the chain\'s coin', () => {
  it('is never written into the text of the pages: they take it from chain.json', () => {
    const found: string[] = [];
    for (const file of sources(SRC)) {
      code(readFileSync(file, 'utf8')).split('\n').forEach((line, i) => {
        let rest = line;
        for (const allowed of ALLOWED) rest = rest.replace(allowed, '');
        if (/\b(ECX|CHN)\b/.test(rest)) found.push(`${relative(SRC, file)}:${i + 1} ${line.trim().slice(0, 160)}`);
      });
    }
    expect(found).toEqual([]);
  });

  it('is the ticker of chain.json: in the units, the treasury, the refusals of a reserved name', () => {
    expect(TICKER).toBe(CHAIN.coin.ticker);
    expect(UNITS[0].short).toBe(TICKER);
    expect(treasuryRows([{ asset: 'CHN', asset_id: CHN_ID, balance: '1', from_swaps: '1', from_launches: 0, security_held: 0 }])[0].label).toBe(TICKER);
    // The names the chain reserves for its coin: its ticker, and on Flush Layers' name too.
    for (const name of [TICKER, ...(CHAIN.name === 'Flush' ? ['CHN'] : [])]) {
      expect(tickerProblem(name)).toContain(TICKER);
      expect(nameProblem(name.toLowerCase())).toContain(TICKER);
    }
    // Flush's coin is eCash's: no page of its app says CHN.
    if (CHAIN.name === 'Flush') expect(TICKER).toBe('ECX');
  });
});
