// What a pool is to the user and the rules of the one pool form (ba/pools.ts), the figures of a span of candles
// (ba/candles.ts), and where the old tabs of the DEX land (chain/local.ts dexRoute): owner decisions, 2026-11-11.
import { describe, expect, it } from 'vitest';
import { spanStats, candlesOf, PERIODS, timeline } from './candles';
import { MAX_TICK, MIN_TICK } from './cl';
import type { Pool } from './model';
import { CURVE_REFUSAL, deeperReference, EMPTY_POOL_TOLERANCE, emptyPoolCheck, priceGuard, REFERENCE_MIN_ECX, referencePrice, THIN_POOL_ECX, ecxPoolsOf, hopParts, isFullRange, isShownPool, marketState, poolEmpty, poolFeeText, poolFormState, poolSides } from './pools';

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
    // An empty pool someone made is no pool of the asset: it is offered a new standard pool (owner decision 2026-11-11).
    expect(form({ pools: [pool({ abandoned: true })] })).toEqual({ ok: true, reason: '', mode: 'create', pool: null });
    expect(form({ pools: [emptyCl] })).toEqual({ ok: true, reason: '', mode: 'create', pool: null });
    // On the empty pool's own page it is taken as it is: reopened, or the full range of a concentrated one (price guard).
    expect(form({ pools: [pool({ abandoned: true })], exact: true })).toMatchObject({ ok: true, mode: 'reopen' });
    expect(form({ pools: [emptyCl], exact: true })).toMatchObject({ ok: true, mode: 'full-range', pool: { pool: emptyCl.pool } });
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

  const guard = (target: Pool, others: Pool[], depositAsset: number, depositEcx: number, launchPrice?: number) => priceGuard({ asset: A, pool: target, pools: [target, ...others], depositAsset, depositEcx, launchPrice });
  const deep = pool();
  const dustHigh = pool({ pool: 'x'.repeat(64), reserve_a: 0.0001, reserve_b: 0.0005, price: 5 });
  const dustLow = pool({ pool: 'x'.repeat(64), reserve_a: 5000, reserve_b: 0.0005, price: 0.0000001 });
  const dustTwin = (price: number) => pool({ pool: 'y'.repeat(64), reserve_a: 0.0002, reserve_b: 0.0002 * price, price });

  it('an empty pool: its price against the reference; no reference, the explicit word', () => {
    const empty = pool({ pool: 'x'.repeat(64), curve: 'concentrated', reserve_a: 0, reserve_b: 0, price: 5 });
    expect(guard(empty, [deep], 10, 5)).toMatchObject({ price: 5, reference: 0.1, thin: true, acknowledge: true });
    expect(guard(empty, [], 10, 5)).toEqual({ price: 5, reference: null, deviation: null, thin: true, acknowledge: true });
    // Close to a real reference: shown (the user is the first provider), no word needed.
    expect(guard({ ...empty, price: 0.101 }, [deep], 10, 1)).toMatchObject({ reference: 0.1, thin: true, acknowledge: false });
    // The price its launch graduated at is a reference too, before any pool.
    expect(guard(empty, [deep], 10, 5, 4.9)).toMatchObject({ reference: 4.9, acknowledge: false });
  });

  it('a dust pool at an absurd high price is guarded as an empty one', () => {
    const g = guard(dustHigh, [deep], 10, 5);
    expect(g).toMatchObject({ price: 5, reference: 0.1, thin: true, acknowledge: true });
    expect(g?.deviation).toBeCloseTo(4900, 6);
    expect(guard(dustHigh, [], 10, 5)).toMatchObject({ reference: null, acknowledge: true });
  });

  it('a dust pool at a near-zero price: the deposit is never valued at the pool\'s own price, so it does not look small', () => {
    // 1000 of the asset and no ECX: at the pool's price that is 0.0001 ECX, less than the dust in it. Not counted so.
    expect(guard(dustLow, [deep], 1000, 0)).toMatchObject({ reference: 0.1, thin: true, acknowledge: true });
    expect(guard(dustLow, [deep], 1000, 0)?.deviation).toBeCloseTo(-99.9999, 3);
    // Without a reference the asset side is not valued at all: the word is needed.
    expect(guard(dustLow, [], 1000, 0)).toMatchObject({ reference: null, thin: true, acknowledge: true });
    expect(guard(dustLow, [], 0, 0)).toMatchObject({ acknowledge: true });
  });

  it('a spoofed dust reference pool is no reference', () => {
    expect(REFERENCE_MIN_ECX).toBe(10);
    expect(THIN_POOL_ECX).toBe(1);
    expect(referencePrice(A, dustHigh.pool, [dustHigh, dustTwin(5)])).toBeNull();
    expect(guard(dustHigh, [dustTwin(5)], 10, 5)).toMatchObject({ reference: null, acknowledge: true });
    expect(guard(dustLow, [dustTwin(0.0000001)], 1000, 0)).toMatchObject({ reference: null, acknowledge: true });
    // Under the depth that counts, however close to it; the pool itself is never its own reference.
    expect(referencePrice(A, dustHigh.pool, [dustHigh, pool({ pool: 'z'.repeat(64), reserve_a: 99, reserve_b: 9.9 })])).toBeNull();
    expect(referencePrice(A, deep.pool, [deep])).toBeNull();
    // A real one wins over the twin; the deepest of several; the graduation price before any.
    expect(referencePrice(A, dustHigh.pool, [dustHigh, dustTwin(5), deep])).toBe(0.1);
    expect(referencePrice(A, dustHigh.pool, [dustHigh, deep, pool({ pool: 'w'.repeat(64), reserve_a: 1000, reserve_b: 200, price: 0.2 })])).toBe(0.2);
    expect(referencePrice(A, dustHigh.pool, [dustHigh, deep], 0.03)).toBe(0.03);
    expect(referencePrice(A, dustHigh.pool, [dustHigh, deep], NaN)).toBe(0.1);
  });

  it('a deep honest pool: no prompt', () => {
    // 50 ECX in it, no other market: it vouches for itself for a deposit smaller than it.
    expect(guard(deep, [], 100, 10)).toBeNull();
    expect(guard(deep, [], 1e9, 0)).toBeNull();
    // With a reference it agrees with, and a deposit smaller than it at the reference's price.
    const other = pool({ pool: 'w'.repeat(64), reserve_a: 2000, reserve_b: 200, price: 0.1 });
    expect(guard(deep, [other], 100, 10)).toBeNull();
    // Deep by its ECX and more than the ECX going in: its price is its own, whatever the asset side of the deposit.
    expect(guard(deep, [other], 1000, 0)).toBeNull();
    // More ECX going in than it holds: not deep for this deposit; shown as the price accepted, no word while it agrees.
    expect(guard(deep, [other], 0, 60)).toMatchObject({ reference: 0.1, thin: true, acknowledge: false });
  });

  it('a deep honest pool far from its graduation price: no prompt (the market moved, owner decision 2026-11-11)', () => {
    // Graduated at 0.02, trading at 0.1 since: five times the graduation price, 50 ECX in it.
    expect(guard(deep, [], 100, 10, 0.02)).toBeNull();
    expect(guard(deep, [], 1, 0.1, 0.5)).toBeNull();
    // A shallower pool of the pair elsewhere, at another price, does not contradict it; nor does a dust one.
    expect(guard(deep, [pool({ pool: 'w'.repeat(64), reserve_a: 100, reserve_b: 20, price: 0.2 })], 100, 10, 0.02)).toBeNull();
    expect(guard(deep, [dustTwin(5)], 100, 10, 0.02)).toBeNull();
  });

  it('a thin pool far from its graduation price: the word', () => {
    const thin = pool({ pool: 'x'.repeat(64), reserve_a: 50, reserve_b: 5, price: 0.1 });
    expect(guard(thin, [], 10, 1, 0.02)).toMatchObject({ price: 0.1, reference: 0.02, acknowledge: true });
    expect(guard(thin, [], 10, 1, 0.02)?.deviation).toBeCloseTo(400, 6);
    // Deep on the chain, but less than the ECX going in: the deposit would be most of it, so the graduation price holds.
    expect(guard(deep, [], 0, 80, 0.02)).toMatchObject({ reference: 0.02, thin: true, acknowledge: true });
    // Near its graduation price: shown only when the deposit is most of it.
    expect(guard(thin, [], 10, 1, 0.1)).toBeNull();
    expect(guard(thin, [], 100, 10, 0.1)).toMatchObject({ thin: true, acknowledge: false });
  });

  it('a deep pool contradicted by a deeper pool of the pair: the word, against that pool', () => {
    const deeper = pool({ pool: 'w'.repeat(64), reserve_a: 20000, reserve_b: 2000, price: 0.1 });
    const off = pool({ reserve_a: 100, reserve_b: 500, price: 5 });
    expect(deeperReference(A, off.pool, [off, deeper], 500)).toBe(0.1);
    expect(guard(off, [deeper], 1, 0.1)).toMatchObject({ price: 5, reference: 0.1, thin: false, acknowledge: true });
    // Whatever the graduation price says: the deeper market is the one that contradicts.
    expect(guard(off, [deeper], 1, 0.1, 5)).toMatchObject({ reference: 0.1, acknowledge: true });
    // One that agrees within the tolerance, or is not deeper, or is dust: nothing to say.
    expect(guard(pool({ reserve_a: 100, reserve_b: 500, price: 0.102 }), [deeper], 1, 0.1)).toBeNull();
    expect(guard(off, [pool({ pool: 'w'.repeat(64), reserve_a: 2000, reserve_b: 200, price: 0.1 })], 1, 0.1)).toBeNull();
    expect(deeperReference(A, off.pool, [off, dustTwin(0.1)], 500)).toBeNull();
    expect(deeperReference(A, off.pool, [off], 500)).toBeNull();
  });

  it("an attacker's dust pool, near zero or absurdly high, is never trusted, whatever the graduation price or a dust twin says", () => {
    expect(guard(dustLow, [], 1000, 0, 0.1)).toMatchObject({ reference: 0.1, thin: true, acknowledge: true });
    expect(guard(dustHigh, [], 10, 5, 0.1)).toMatchObject({ reference: 0.1, thin: true, acknowledge: true });
    expect(guard(dustHigh, [dustTwin(5)], 10, 5)).toMatchObject({ reference: null, thin: true, acknowledge: true });
    expect(guard(dustLow, [dustTwin(0.0000001)], 1000, 0)).toMatchObject({ reference: null, thin: true, acknowledge: true });
    // A dust twin that "agrees" with the graduation price does not make the dust pool deep either.
    expect(guard(dustHigh, [dustTwin(5)], 10, 5, 5)).toMatchObject({ reference: 5, thin: true, acknowledge: false });
  });

  it('no reference at all: the word, unless the pool\'s own ECX is deep and more than the ECX going in', () => {
    expect(guard(deep, [], 0, 80)).toMatchObject({ reference: null, thin: true, acknowledge: true });
    expect(guard(pool({ reserve_a: 99, reserve_b: 9.9 }), [], 1, 0.1)).toMatchObject({ reference: null, acknowledge: true });
    expect(guard(pool({ reserve_a: 100, reserve_b: 10 }), [], 1, 0.1)).toBeNull();
    expect(guard(pool({ reserve_a: 100, reserve_b: 10 }), [], 1, 10)).toMatchObject({ acknowledge: true });
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

  it('where an asset trades: on its curve until it graduates, then its pool, or nowhere; an empty pool is no market', () => {
    expect(marketState(A, [curve]).state).toBe('curve');
    expect(marketState(A, [pool()], { status: 'curve', pool: curve.pool })).toMatchObject({ state: 'curve', curvePool: curve.pool });
    expect(marketState(A, [pool()], { status: 'graduated' })).toMatchObject({ state: 'pool', pool: { pool: pool().pool } });
    expect(marketState(A, [emptyCl])).toEqual({ state: 'none', pool: null, curvePool: '' });
    expect(marketState(A, [pool({ abandoned: true })]).state).toBe('none');
    // A pool with liquidity next to an empty one: the asset's pool is the one with liquidity.
    expect(marketState(A, [emptyCl, pool()])).toMatchObject({ state: 'pool', pool: { pool: pool().pool } });
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

  it('the time axis of a chart: a slot per interval over the whole period, an empty line at the last price where nothing traded', () => {
    const hour = 3600;
    const now = 100 * hour + 120;
    const candle = (h: number, o: number, c: number) => ({ t: h * hour, o, h: Math.max(o, c), l: Math.min(o, c), c, v: 1 });
    // 1D by the hour: 24 slots ending at the hour of now, whatever traded.
    const slots = timeline([candle(60, 5, 6), candle(80, 6, 7), candle(99, 7, 8)], '1h', 86_400, now);
    expect(slots.length).toBe(24);
    expect(slots[0].t).toBe(77 * hour);
    expect(slots.at(-1)?.t).toBe(100 * hour);
    // Before its first trade the axis carries the last close before it (hour 60's), then each trade's own.
    expect(slots[0]).toMatchObject({ c: null, carried: 6 });
    expect(slots[3].c?.c).toBe(7);
    expect(slots[4]).toMatchObject({ c: null, carried: 7 });
    expect(slots[22].c?.c).toBe(8);
    expect(slots[23]).toMatchObject({ c: null, carried: 8 });
    expect(slots.filter((s) => s.c).length).toBe(2);
    // Nothing known before the axis: no line until the first trade.
    expect(timeline([candle(99, 7, 8)], '1h', 86_400, now)[0]).toMatchObject({ c: null, carried: null });
    // No trade at all: the slots are there, with nothing to draw.
    expect(timeline([], '1h', 86_400, now).every((s) => s.c === null && s.carried === null)).toBe(true);
    // All: from the first candle to now, by the day, the days without a trade included; never more than the cap.
    const day = 86_400;
    const all = timeline([{ t: 2 * day, o: 1, h: 1, l: 1, c: 1, v: 1 }, { t: 5 * day, o: 1, h: 2, l: 1, c: 2, v: 1 }], '1d', 0, 7 * day + 5);
    expect(all.map((s) => (s.c ? 'x' : s.carried))).toEqual(['x', 1, 1, 'x', 2, 2]);
    expect(timeline([{ t: 0, o: 1, h: 1, l: 1, c: 1, v: 1 }], '1d', 0, 2000 * day, 500).length).toBe(500);
  });
});
