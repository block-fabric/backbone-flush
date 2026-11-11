// The wallet's positions of concentrated liquidity (listpositions), read once per refresh of the store for every page
// that shows them (owner decision, 2026-11-11): those over the full range are the wallet's share of a pool that is
// concentrated on the chain (a graduated launch's), shown as a pool share (Pools, a pool's page, Portfolio); those of a
// range are old markets the wallet can only close (Portfolio, "Closing old markets": collect, remove).
import { rpc } from '../../lib/rpc';
import { wallets } from '../../lib/state.svelte';
import type { Position } from './cl';
import { isFullRange } from './pools';
import type { Pool } from './model';
import { ba, poolById } from './store.svelte';

export const positionsRead = $state({ list: [] as Position[], wallet: null as string | null, loaded: false, error: '', pools: {} as Record<string, Pool> });
let readAt = '';

/** Read them again when the store refreshed or the wallet changed (call it in an $effect that reads ba.version). */
export function loadPositions(): void {
  const wallet = wallets.current;
  const key = `${wallet}@${ba.version}`;
  if (key === readAt) return;
  readAt = key;
  if (wallet === null) {
    Object.assign(positionsRead, { list: [], wallet: null, loaded: false, error: '' });
    return;
  }
  rpc
    .raw<Position[]>('listpositions', [], { wallet })
    .then((r) => {
      if (wallets.current !== wallet) return;
      const list = Array.isArray(r) ? r : [];
      Object.assign(positionsRead, { list, wallet, loaded: true, error: '' });
      // Their pools not among those the store read: asked by id, once.
      for (const p of list) {
        if (poolById(p.pool) || positionsRead.pools[p.pool]) continue;
        rpc
          .raw<Pool>('getpool', [p.pool])
          .then((pool) => {
            if (pool && typeof pool === 'object') positionsRead.pools[p.pool] = pool;
          })
          .catch(() => undefined);
      }
    })
    .catch((e) => {
      // A node from before concentrated pools has no such method: none.
      if (wallets.current === wallet) Object.assign(positionsRead, { list: [], wallet, loaded: true, error: e instanceof Error ? e.message : '' });
    });
}

/** For the tests. */
export function resetPositions(): void {
  readAt = '';
  Object.assign(positionsRead, { list: [], wallet: null, loaded: false, error: '', pools: {} });
}

/** The pool of a position: among those read, or asked by id. */
export function positionPool(p: Pick<Position, 'pool'>): Pool | undefined {
  return poolById(p.pool) ?? positionsRead.pools[p.pool];
}

/** Whether a position covers the full range of its pool (unknown pool: by the usual spacing). */
export function fullRangePosition(p: Position): boolean {
  return isFullRange(p, positionPool(p)?.concentrated?.tick_spacing ?? 60);
}

/** The wallet's positions over the full range in a pool: its share of that pool. */
export function sharesIn(pool: string): Position[] {
  return positionsRead.list.filter((p) => p.pool === pool && fullRangePosition(p));
}

/** The wallet's positions of a range: old markets to close. */
export function rangedPositions(): Position[] {
  return positionsRead.list.filter((p) => !fullRangePosition(p));
}

/** The wallet's part of a concentrated pool's liquidity in force, in percent, from its full-range positions (null: unknown). */
export function sharePercent(pool: Pick<Pool, 'pool' | 'concentrated'>): number | null {
  const mine = sharesIn(pool.pool);
  if (mine.length === 0) return null;
  try {
    const total = BigInt(pool.concentrated?.liquidity ?? '0');
    if (total <= 0n) return null;
    const own = mine.reduce((n, p) => n + BigInt(p.liquidity || '0'), 0n);
    return Math.min(100, Number((own * 1_000_000n) / total) / 10_000);
  } catch {
    return null;
  }
}
