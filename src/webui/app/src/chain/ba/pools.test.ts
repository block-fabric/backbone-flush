// What a pool is to the user and the rules of the one pool form (ba/pools.ts), the figures of a span of candles
// (ba/candles.ts), and where the old tabs of the DEX land (chain/local.ts dexRoute): owner decisions, 2026-11-11.
import { describe, expect, it } from 'vitest';
import { spanStats, candlesOf, PERIODS } from './candles';
import { MAX_TICK, MIN_TICK } from './cl';
import type { Pool } from './model';
import { CURVE_REFUSAL, EMPTY_POOL_TOLERANCE, emptyPoolCheck, priceGuard, REFERENCE_MIN_ECX, referencePrice, THIN_POOL_ECX, thinPool, ecxPoolsOf, hopParts, isFullRange, isShownPool, marketState, poolEmpty, poolFeeText, poolFormState, poolSides } from './pools';

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

describe("a thin pool's price is someone else's (security reviews 2026-11-11)", () => {
  it('needs an explicit word when it is far from the reference, or when there is none', () => {
    expect(EMPTY_POOL_TOLERANCE).toBe(3);
    expect(emptyPoolCheck(0.1, [])).toEqual({ price: 0.1, reference: null, deviation: null, acknowledge: true });
    expect(emptyPoolCheck(0.1, [undefined, null, 0, NaN])).toMatchObject({ reference: null, acknowledge: true });
    expect(emptyPoolCheck(undefined, [0.1])).toMatchObject({ acknowledge: true });
    expect(emptyPoolCheck(0, [0.1])).toMatchObject({ acknowledge: true });
    const far = emptyPoolCheck(5, [0.1]);
    expect(far.acknowledge).toBe(true);
    expect(far.deviation).toBeCloseTo(4900, 6);
    expect(emptyPoolCheck(0.05, [0.1]).deviation).toBeCloseTo(-50, 6);
    // Within the tolerance: shown, not asked about.
    expect(emptyPoolCheck(0.102, [0.1])).toMatchObject({ reference: 0.1, acknowledge: false });
    expect(emptyPoolCheck(0.104, [0.1]).acknowledge).toBe(true);
  });

  it('is guarded whenever the deposit would be most of the pool: empty, dust, or less than what goes in', () => {
    expect(THIN_POOL_ECX).toBe(1);
    expect(thinPool(emptyCl, 0)).toBe(true);
    // Dust at an absurd price is as thin as nothing.
    expect(thinPool(pool({ reserve_a: 0.0001, reserve_b: 0.0005, price: 5 }), 0)).toBe(true);
    expect(thinPool(pool({ reserve_b: 0.99 }), 0.01)).toBe(true);
    // 50 ECX in it: a market for a deposit of 10, not for one of 80.
    expect(thinPool(pool(), 10)).toBe(false);
    expect(thinPool(pool(), 80)).toBe(true);
    expect(priceGuard({ asset: A, pool: pool(), pools: [pool()], depositEcx: 10 })).toBeNull();
    expect(priceGuard({ asset: A, pool: pool(), pools: [pool()], depositEcx: 80 })).toMatchObject({ price: 0.1, reference: null, acknowledge: true });
  });

  it('takes no dust pool for a reference: the graduation price first, then the deepest other pool of real depth, else none', () => {
    expect(REFERENCE_MIN_ECX).toBe(10);
    const target = pool({ pool: 'x'.repeat(64), reserve_a: 0.0001, reserve_b: 0.0005, price: 5 });
    const dust = pool({ pool: 'y'.repeat(64), reserve_a: 0.0002, reserve_b: 0.001, price: 5 });
    const shallow = pool({ pool: 'z'.repeat(64), reserve_a: 99, reserve_b: 9.9, price: 0.1 });
    const deep = pool();
    const deeper = pool({ pool: 'w'.repeat(64), reserve_a: 1000, reserve_b: 200, price: 0.2 });
    // A second dust pool at the same absurd price does not make it a market.
    expect(referencePrice(A, target.pool, [target, dust])).toBeNull();
    expect(priceGuard({ asset: A, pool: target, pools: [target, dust], depositEcx: 5 })).toMatchObject({ reference: null, acknowledge: true });
    expect(referencePrice(A, target.pool, [target, dust, shallow])).toBeNull();
    // The pool itself is never its own reference, however deep.
    expect(referencePrice(A, deep.pool, [deep])).toBeNull();
    expect(referencePrice(A, target.pool, [target, dust, deep])).toBe(0.1);
    expect(referencePrice(A, target.pool, [target, deep, deeper])).toBe(0.2);
    // The price its launch graduated at comes before any pool.
    expect(referencePrice(A, target.pool, [target, deeper], 0.03)).toBe(0.03);
    expect(referencePrice(A, target.pool, [target, deeper], NaN)).toBe(0.2);
    const g = priceGuard({ asset: A, pool: target, pools: [target, dust, deep], depositEcx: 5 });
    expect(g).toMatchObject({ price: 5, reference: 0.1, acknowledge: true });
    expect(g?.deviation).toBeCloseTo(4900, 6);
    // Close to a real reference: shown, no word needed.
    expect(priceGuard({ asset: A, pool: pool({ pool: 'x'.repeat(64), reserve_a: 0, reserve_b: 0, abandoned: true, price: 0.101 }), pools: [deep], depositEcx: 5 })).toMatchObject({ reference: 0.1, acknowledge: false });
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
