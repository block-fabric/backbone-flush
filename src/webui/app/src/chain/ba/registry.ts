// The registry of assets (Browse assets): each asset's market, as the pages know it from the pools, books and auctions the
// store read: on a launch's curve (and how far to graduation), in pools, in an order book, at auction, or none. The same
// statuses as the public API's (doc/public-api.md: AssetSummary.market.status), in the same order of priority:
// curve > pool > book > auction > none. Pure: no state, no calls (tested in chain/ba/create.test.ts).
import { approx, isChn, type Auction, type Pool } from './model';

export type MarketStatus = 'none' | 'curve' | 'pool' | 'book' | 'auction';

export interface AssetMarket {
  status: MarketStatus;
  /** Every kind it has, in the order of priority. */
  kinds: MarketStatus[];
  /** On a curve: the share of the graduation it raised, 0 to 1. */
  progress: number | null;
  pools: number;
  books: number;
  auctions: number;
}

export const MARKET_LABELS: Record<MarketStatus, string> = { none: 'No market', curve: 'On curve', pool: 'Pool', book: 'Book', auction: 'Auction' };

/** The market of `asset` from the pools (a launch's curve is a pool of the kind "launch"), books and auctions known. */
export function marketOf(asset: string, pools: readonly Pool[], auctions: readonly Auction[], graduation = 500): AssetMarket {
  let progress: number | null = null;
  let poolCount = 0;
  let books = 0;
  for (const p of pools) {
    if (p.asset_a_id !== asset && p.asset_b_id !== asset) continue;
    if (p.curve === 'launch') {
      // Its coin less the virtual half of the graduation, of the graduation.
      const coin = approx(isChn(p.asset_a_id) ? p.reserve_a : p.reserve_b);
      progress = graduation > 0 ? Math.min(1, Math.max(0, (coin - graduation / 2) / graduation)) : 0;
    } else if (p.curve === 'book') {
      books += 1;
    } else if (!p.abandoned) {
      poolCount += 1;
    }
  }
  const live = auctions.filter((a) => a.base_id === asset && (a.status === 'open' || a.status === 'upcoming')).length;
  const kinds: MarketStatus[] = [];
  if (progress !== null) kinds.push('curve');
  if (poolCount > 0) kinds.push('pool');
  if (books > 0) kinds.push('book');
  if (live > 0) kinds.push('auction');
  return { status: kinds[0] ?? 'none', kinds, progress, pools: poolCount, books, auctions: live };
}

/** The badge of a market: "On curve 42%", "3 pools", "Book", "Auction", "No market". */
export function marketBadge(m: AssetMarket): string {
  if (m.status === 'curve') return `On curve ${Math.round((m.progress ?? 0) * 100)}%`;
  if (m.status === 'pool') return m.pools === 1 ? '1 pool' : `${m.pools} pools`;
  return MARKET_LABELS[m.status];
}
