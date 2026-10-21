// The market index (Layers' DEX): every pair of every kind as the node's searchpools gives it, a page at a time: its
// price, its change and its volume over the node's window (about a day), its liquidity (the CHN it holds), sorted by the
// node (depth, volume, price_change, newest). The columns of the table and the sorts they stand for, and how a page is
// put after those shown. Pure: no state, no calls (tested in chain/ba/markets.test.ts).
import { approx, type Amount, type Pool } from './model';

/** The sorts of searchpools. */
export type MarketSort = 'depth' | 'volume' | 'price_change' | 'newest';
export const MARKET_SORTS: readonly MarketSort[] = ['depth', 'volume', 'price_change', 'newest'];

/** A pair of the index: a pool of searchpools, with the fields of the search. */
export interface Market extends Pool {
  /** The CHN it holds (0 for a pair without CHN). */
  depth?: Amount;
  /** The CHN traded over the window. */
  volume_window?: Amount;
  /** The change of the price over the window, in percent. */
  price_change?: number | string;
  last_activity?: number;
}

/** The columns of the table, and the sort each stands for (none: not sortable). */
export type MarketColumn = 'pair' | 'kind' | 'price' | 'change' | 'volume' | 'liquidity';
export const COLUMN_SORT: Record<MarketColumn, MarketSort | null> = { pair: 'newest', kind: null, price: null, change: 'price_change', volume: 'volume', liquidity: 'depth' };

/** The sort a column's header asks for (null: it does not sort). */
export function sortOfColumn(column: MarketColumn): MarketSort | null {
  return COLUMN_SORT[column];
}

/** The words of a sort (shown with t()). */
export const SORT_NAMES: Record<MarketSort, string> = { depth: 'Most liquidity', volume: 'Most traded', price_change: 'Largest rise', newest: 'Newest' };

/** What a sort leaves out, on the node's side (shown with t()), or ''. */
export const SORT_NOTES: Record<MarketSort, string> = {
  depth: 'Sorted by liquidity: pairs with CHN only.',
  volume: 'Sorted by volume: pairs traded in the window only.',
  price_change: '',
  newest: '',
};

export function isMarketSort(value: unknown): value is MarketSort {
  return typeof value === 'string' && (MARKET_SORTS as readonly string[]).includes(value);
}

/** Which way a change goes: up, down or flat (none: the node said nothing). */
export function changeDirection(change: number | string | undefined | null): 'up' | 'down' | 'flat' | 'none' {
  if (change === undefined || change === null || change === '') return 'none';
  const x = approx(change);
  if (!Number.isFinite(x)) return 'none';
  return x > 0 ? 'up' : x < 0 ? 'down' : 'flat';
}

/** A change in percent, signed: "+12.50%", "-3.20%", "0.00%" ('–' when none). */
export function formatChange(change: number | string | undefined | null): string {
  const dir = changeDirection(change);
  if (dir === 'none') return '–';
  const x = approx(change);
  const a = Math.abs(x);
  const text = a >= 1000 ? a.toFixed(0) : a >= 10 ? a.toFixed(1) : a.toFixed(2);
  return `${dir === 'up' ? '+' : dir === 'down' ? '-' : ''}${text}%`;
}

/** A page put after those shown, without one shown twice (a pair moved between pages as the chain moved). */
export function appendMarkets(shown: readonly Market[], page: readonly Market[]): Market[] {
  const seen = new Set(shown.map((m) => m.pool));
  return [...shown, ...page.filter((m) => !seen.has(m.pool))];
}

/** Whether a pair is an order book (its rows open the book, not a pool's page). */
export function isBook(p: { curve?: unknown }): boolean {
  return p.curve === 'book';
}
