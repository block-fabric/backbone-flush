// What a pool is to the user and the rules of the one pool form (ba/pools.ts), the figures of a span of candles
// (ba/candles.ts), and where the old tabs of the DEX land (chain/local.ts dexRoute): owner decisions, 2026-11-11.
import { describe, expect, it } from 'vitest';
import { spanStats, candlesOf, PERIODS } from './candles';
import { MAX_TICK, MIN_TICK } from './cl';
import type { Pool } from './model';
import { CURVE_REFUSAL, EMPTY_POOL_TOLERANCE, emptyPoolCheck, ecxPoolsOf, hopParts, isFullRange, isShownPool, marketState, poolEmpty, poolFeeText, poolFormState, poolSides } from './pools';

const A = 'a'.repeat(64);
const B = 'b'.repeat(64);
const ECX = '0'.repeat(64);
const pool = (extra: Partial<Pool> = {}): Pool => ({ pool: 'p'.repeat(64), asset_a: 'GOLD', asset_b: 'ECX', asset_a_id: A, asset_b_id: ECX, reserve_a: 500, reserve_b: 50, price: 0.1, shares: 15811388, volume_a: 20, volume_b: 2, swaps: 4, created: 9, abandoned: false, ...extra });
const curve = pool({ pool: 'c'.repeat(64), curve: 'launch' });
const book = pool({ pool: 'k'.repeat(64), curve: 'book', reserve_a: 0, reserve_b: 0 });
const emptyCl = pool({ pool: 'e'.repeat(64), curve: 'concentrated', fee: 0.3, reserve_a: 0, reserve_b: 0, price: 0.01, shares: 1001, concentrated: { sqrt_price_x96: '1', tick: 0, liquidity: '0', tick_spacing: 60 } });
const cl = pool({ pool: 'f'.repeat(64), curve: 'concentrated', fee: 1, default_kind: false, concentrated: { sqrt_price_x96: '1', tick: 0, liquidity: '5000', tick_spacing: 60 } });
const form = (o: Partial<Parameters<typeof poolFormState>[0]>) => poolFormState({ asset: A, wallet: true, onCurve: false, pools: [], curvesActive: true, ...o });

describe('the one pool form, by the state of its asset', () => {
  it('is refused for an asset on its launch curve, whatever else is true, with the reason (bad-ba-launch-pooled)', () => {
    for (const o of [{ onCurve: true }, { pools: [curve] }, { onCurve: true, pools: [pool()] }, { onCurve: true, wallet: false }]) {
      const s = form(o);
      expect(s).toEqual({ ok: false, reason: CURVE_REFUSAL, mode: null, pool: null });
    }
    expect(CURVE_REFUSAL).toMatch(/its pool opens by itself at graduation/);
    // Graduated (not on its curve any more): allowed again, into its pool.
    expect(form({ pools: [cl] })).toMatchObject({ ok: true, mode: 'full-range' });
  });

  it('makes a standard pool where there is none, adds where there is one, reopens an empty one, and takes the full range of one concentrated on the chain', () => {
    expect(form({})).toEqual({ ok: true, reason: '', mode: 'create', pool: null });
    expect(form({ pools: [pool()] })).toMatchObject({ ok: true, mode: 'add', pool: { pool: 'p'.repeat(64) } });
    expect(form({ pools: [pool({ abandoned: true })] })).toMatchObject({ ok: true, mode: 'reopen' });
    expect(form({ pools: [emptyCl] })).toMatchObject({ ok: true, mode: 'full-range', pool: { pool: emptyCl.pool } });
    // An order book of the pair is no pool: a pool can still be made.
    expect(form({ pools: [book] })).toMatchObject({ ok: true, mode: 'create' });
    // The pool with liquidity before an empty one of the same pair.
    expect(form({ pools: [emptyCl, pool()] })).toMatchObject({ mode: 'add' });
    // Its pool with another asset is not the one the form puts into.
    expect(form({ pools: [pool({ asset_b: 'SILVER', asset_b_id: B })] })).toMatchObject({ mode: 'create' });
  });

  it('says why not: no wallet, ECX itself, a concentrated pool on a node without the pool kinds', () => {
    expect(form({ wallet: false })).toMatchObject({ ok: false, reason: 'Load a wallet to add liquidity.' });
    expect(form({ asset: ECX }).ok).toBe(false);
    expect(form({ pools: [cl], curvesActive: false }).ok).toBe(false);
    expect(form({ pools: [pool()], curvesActive: false })).toMatchObject({ ok: true, mode: 'add' });
  });
});

describe("an empty pool's price is someone else's (security review 2026-11-11)", () => {
  it('needs an explicit word when it is far from the reference, or when there is none', () => {
    expect(EMPTY_POOL_TOLERANCE).toBe(3);
    expect(emptyPoolCheck(0.1, [])).toEqual({ reference: null, deviation: null, acknowledge: true });
    expect(emptyPoolCheck(0.1, [undefined, 0, NaN])).toMatchObject({ reference: null, acknowledge: true });
    expect(emptyPoolCheck(undefined, [0.1])).toMatchObject({ acknowledge: true });
    expect(emptyPoolCheck(0, [0.1])).toMatchObject({ acknowledge: true });
    const far = emptyPoolCheck(5, [0.1]);
    expect(far.acknowledge).toBe(true);
    expect(far.deviation).toBeCloseTo(4900, 6);
    expect(emptyPoolCheck(0.05, [0.1])).toMatchObject({ acknowledge: true });
    expect(emptyPoolCheck(0.05, [0.1]).deviation).toBeCloseTo(-50, 6);
    // Within the tolerance: shown, not asked about. The first reference that is a price is the one used.
    expect(emptyPoolCheck(0.102, [0.1])).toMatchObject({ reference: 0.1, acknowledge: false });
    expect(emptyPoolCheck(0.104, [0.1]).acknowledge).toBe(true);
    expect(emptyPoolCheck(0.1, [undefined, 0.1, 9])).toMatchObject({ reference: 0.1, acknowledge: false });
  });
});

describe('a pool as the pages show it', () => {
  it('is its pair and its fee, the asset first; books and launch curves are no pools; empty ones are told', () => {
    expect(isShownPool(pool())).toBe(true);
    expect(isShownPool(curve)).toBe(false);
    expect(isShownPool(book)).toBe(false);
    expect(poolEmpty(pool())).toBe(false);
    expect(poolEmpty(pool({ abandoned: true }))).toBe(true);
    // A concentrated pool is never "abandoned" on the chain: empty when it holds nothing.
    expect(poolEmpty(emptyCl)).toBe(true);
    expect(poolEmpty(cl)).toBe(false);
    const flipped = pool({ asset_a: 'ECX', asset_b: 'GOLD', asset_a_id: ECX, asset_b_id: A, reserve_a: 50, reserve_b: 500, price: 10 });
    expect(poolSides(flipped)).toMatchObject({ base: { id: A, label: 'GOLD' }, quote: { id: ECX }, flip: true, baseReserve: 500, quoteReserve: 50, price: 0.1 });
    expect(poolFeeText(pool())).toBe('0.3%');
    expect(poolFeeText(cl)).toBe('1%');
    expect(ecxPoolsOf(A, [emptyCl, book, curve, pool()]).map((p) => p.pool)).toEqual([pool().pool, emptyCl.pool]);
  });

  it('where an asset trades: on its curve until it graduates, then its pool, an empty pool, or nowhere', () => {
    expect(marketState(A, [curve]).state).toBe('curve');
    expect(marketState(A, [pool()], { status: 'curve', pool: curve.pool })).toMatchObject({ state: 'curve', curvePool: curve.pool });
    expect(marketState(A, [pool()], { status: 'graduated' })).toMatchObject({ state: 'pool', pool: { pool: pool().pool } });
    expect(marketState(A, [emptyCl]).state).toBe('pool-empty');
    expect(marketState(A, [book]).state).toBe('none');
    expect(marketState(A, []).state).toBe('none');
  });

  it('a position over the full range is a pool share; one of a range is not', () => {
    const lo = Math.ceil(MIN_TICK / 60) * 60;
    const hi = Math.floor(MAX_TICK / 60) * 60;
    expect(isFullRange({ tick_lower: lo, tick_upper: hi }, 60)).toBe(true);
    expect(isFullRange({ tick_lower: MIN_TICK, tick_upper: MAX_TICK }, 1)).toBe(true);
    expect(isFullRange({ tick_lower: lo + 60, tick_upper: hi }, 60)).toBe(false);
    expect(isFullRange({ tick_lower: 22500, tick_upper: 23520 }, 60)).toBe(false);
  });

  it("a route's step in the user's words: a launch curve, a pool, never a pool's curve", () => {
    expect(hopParts({ from: ECX, to: A, kind: { curve: 'launch', fee: 1, default_kind: false } })).toEqual({ venue: 'curve', base: A, quote: ECX, fee: '1%' });
    expect(hopParts({ from: A, to: ECX, kind: { curve: 'concentrated', fee: 0.3, default_kind: false } })).toEqual({ venue: 'pool', base: A, quote: ECX, fee: '0.3%' });
    expect(hopParts({ from: A, to: B, kind: { curve: 'stable', fee: 0.05, default_kind: false } })).toMatchObject({ venue: 'pool', base: A, quote: B });
    expect(hopParts({ from: ECX, to: A, kind: { curve: 'book', fee: 0, default_kind: false } }).venue).toBe('book');
  });
});

describe('the figures of a day, from the candles the node has', () => {
  it('the change from the first open to the last close and the volume; nothing made up without candles', () => {
    expect(spanStats([])).toBeNull();
    const stats = spanStats(candlesOf([{ t: 1, o: 2, h: 3, l: 2, c: 2.5, v: 4 }, { t: 2, o: 2.5, h: 3, l: 2, c: 3, v: 1.5 }]));
    expect(stats?.change).toBeCloseTo(50, 9);
    expect(stats?.volume).toBeCloseTo(5.5, 9);
    expect(candlesOf({ items: [{ t: 2, o: 1, h: 2, l: 0.5, c: 1.5, v: 3 }, { t: 1, o: '1', h: '1', l: '1', c: '1', v: '0' }, { t: 3, o: 1, h: 0, l: 1, c: 1 }] }).map((c) => c.t)).toEqual([1, 2]);
    expect(candlesOf('nope')).toEqual([]);
    expect(PERIODS.map((p) => p.id)).toEqual(['1H', '1D', '1W', 'All']);
  });
});
