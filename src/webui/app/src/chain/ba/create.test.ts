import { describe, expect, it } from 'vitest';
import { canCurve, curveAssetParams, curveMaxUnits, curveSplit, curveSupplyProblem, FIXED_PARAMS, launchParamsOf, launchTokenOptions, unitsText, wholeUnits } from './create';
import { marketBadge, marketOf } from './registry';
import { candlesOf, depthOf, levelAt, orderMarks } from './bookchart';
import type { Auction, Pool } from './model';

const ACTIVE = launchParamsOf({ fee: 1.15, params_active: true, params_height: 20, fee_min: 0.3, fee_max: 5, fee_default: 1.15, supply_min_units: '1000000', supply_max_units: '8198552921648689600', supply_multiple_units: 8, decimals_max: 12, decimals_default: 6, supply_default: '1000000000' });

describe('the launch parameters', () => {
  it('fixed before they apply, the rules after', () => {
    const before = launchParamsOf({ fee: 1.15, params_active: false, params_height: 40 });
    expect(before.active).toBe(false);
    expect(before.height).toBe(40);
    expect(before.feeMin).toBe(1.15);
    expect(launchParamsOf(undefined)).toMatchObject({ active: false, height: -1 });
    expect(ACTIVE).toMatchObject({ active: true, feeMin: 0.3, feeMax: 5, feeDefault: 1.15, minUnits: 1_000_000n, multiple: 8n, decimalsMax: 12 });
  });

  it("checks a curve's supply as the rules do", () => {
    expect(curveSupplyProblem(FIXED_PARAMS.minUnits, 6, FIXED_PARAMS)).toBe('');
    expect(curveSupplyProblem(8_000_000n, 6, FIXED_PARAMS)).toMatch(/billion/);
    expect(curveSupplyProblem(999_992n, 0, ACTIVE)).toMatch(/at least/);
    expect(curveSupplyProblem(1_000_001n, 0, ACTIVE)).toMatch(/multiple of 8/);
    expect(curveSupplyProblem(1_000_000n, 0, ACTIVE)).toBe('');
    expect(curveSupplyProblem(8_198_552_921_648_689_608n, 0, ACTIVE)).toMatch(/At most/);
    expect(curveSupplyProblem(2_100_000_000_000_000n, 8, ACTIVE)).toBe('');
    expect(curveSplit(1_000_000n)).toEqual({ curve: 750_000n, pool: 250_000n });
    expect(curveMaxUnits(1_000_007n, ACTIVE)).toBe(1_000_000n);
  });

  it('units and whole amounts, exactly', () => {
    expect(unitsText(2_100_000_000_000_000n, 8)).toBe('21000000');
    expect(unitsText(1_500n, 3)).toBe('1.5');
    expect(wholeUnits('21000000', 8)).toBe(2_100_000_000_000_000n);
    expect(wholeUnits('1.234', 2)).toBeNull();
    expect(wholeUnits('99999999999999999999', 0)).toBeNull();
  });

  it("launchtoken's options: supply, decimals and fee only where the rules take them", () => {
    const base = { displayName: '', logo: '', info: 'x', allocation: 0, firstBuy: '', minBuy: '', maxBuy: '', supply: '21000000', decimals: 8, feePercent: 2.5 };
    expect(launchTokenOptions({ ...base, params: FIXED_PARAMS })).toEqual({ info: 'x' });
    expect(launchTokenOptions({ ...base, params: ACTIVE })).toEqual({ info: 'x', supply: '21000000', decimals: 8, fee: 2.5 });
    expect(curveAssetParams('0xab', '80000', { feePercent: 1.15, minBuy: '0.1' })).toEqual(['0xab', '80000', { fee: 1.15, min_buy: '0.1' }]);
  });

  it('whether an asset can go on a curve', () => {
    const ok = { params: ACTIVE, controlled: true, launched: false, ecxPool: false, heldUnits: 10_000_000n };
    expect(canCurve(ok)).toEqual({ ok: true, reason: '' });
    expect(canCurve({ ...ok, params: FIXED_PARAMS }).ok).toBe(false);
    expect(canCurve({ ...ok, launched: true }).reason).toMatch(/launched/);
    expect(canCurve({ ...ok, ecxPool: true }).reason).toMatch(/pool/);
    expect(canCurve({ ...ok, controlled: false }).reason).toMatch(/controller/);
    expect(canCurve({ ...ok, heldUnits: 5n }).reason).toMatch(/less/);
  });
});

describe('the registry: the market of each asset', () => {
  const A = 'a'.repeat(64);
  const CHN = '0'.repeat(64);
  const pool = (extra: Partial<Pool>): Pool => ({ pool: 'p', asset_a: 'ECX', asset_b: 'A', asset_a_id: CHN, asset_b_id: A, reserve_a: 0, reserve_b: 0, shares: 0, volume_a: 0, volume_b: 0, swaps: 0, created: 1, abandoned: false, ...extra });
  it('curve first, then pools, books, auctions', () => {
    expect(marketOf(A, [], []).status).toBe('none');
    const curve = marketOf(A, [pool({ curve: 'launch', reserve_a: 450 }), pool({ curve: 'book' })], [], 500);
    expect(curve).toMatchObject({ status: 'curve', kinds: ['curve', 'book'], books: 1 });
    expect(curve.progress).toBeCloseTo(0.4);
    expect(marketBadge(curve)).toBe('On curve 40%');
    // v6: each launch's own graduation. 70 CHN in a curve of 100 (virtual 50): 20%; the same coin of one of 500 reads as none raised.
    expect(marketOf(A, [pool({ curve: 'launch', reserve_a: 70 })], [], 100).progress).toBeCloseTo(0.2);
    expect(marketOf(A, [pool({ curve: 'launch', reserve_a: 70 })], [], 500).progress).toBe(0);
    // Not known: on its curve, its progress not guessed.
    const unknown = marketOf(A, [pool({ curve: 'launch', reserve_a: 70 })], []);
    expect(unknown).toMatchObject({ status: 'curve', progress: null });
    expect(marketBadge(unknown)).toBe('On curve');
    expect(marketBadge(marketOf(A, [pool({}), pool({ curve: 'stable' }), pool({ abandoned: true })], []))).toBe('2 pools');
    const auction = { base_id: A, status: 'open' } as Auction;
    expect(marketOf(A, [], [auction]).status).toBe('auction');
    expect(marketOf(A, [], [{ ...auction, status: 'ended' }]).status).toBe('none');
  });
});

describe('the charts of a book', () => {
  const book = { asks: [{ price: 1.1, amount: 2 }, { price: 1.2, amount: 3, price_exact: '1.2' }], bids: [{ price: 0.9, amount: 1 }, { price: 0.95, amount: 4 }], mid_exact: '1.025' };
  it('the depth: running totals each way from the middle, the exact middle', () => {
    const d = depthOf(book);
    expect(d.bids.map((b) => [b.price, b.total])).toEqual([[0.95, 4], [0.9, 5]]);
    expect(d.asks.map((a) => [a.price, a.total])).toEqual([[1.1, 2], [1.2, 5]]);
    expect(d.midText).toBe('1.025');
    expect(d.asks[1].priceText).toBe('1.2');
    expect(d.lo).toBeLessThanOrEqual(0.9);
    expect(d.hi).toBeGreaterThanOrEqual(1.2);
    expect(levelAt(d, 0.92)?.total).toBe(4);
    expect(levelAt(d, 1.15)?.total).toBe(2);
    expect(depthOf(null).bids).toEqual([]);
  });
  it('candles checked, oldest first; the own orders as marks', () => {
    expect(candlesOf({ items: [{ t: 2, o: 1, h: 2, l: 0.5, c: 1.5, v: 3 }, { t: 1, o: '1', h: '1', l: '1', c: '1', v: '0' }, { t: 3, o: 1, h: 0, l: 1, c: 1 }] }).map((c) => c.t)).toEqual([1, 2]);
    expect(candlesOf('nope')).toEqual([]);
    const marks = orderMarks([{ order: 'x', sells_id: 'A', buys_id: 'B', price: 2, remaining: 1 }, { order: 'y', sells_id: 'B', buys_id: 'A', price: 0.5, remaining: 3 }, { order: 'z', sells_id: 'A', buys_id: 'C', price: 1, remaining: 1 }], 'A', 'B');
    expect(marks).toEqual([{ order: 'x', side: 'ask', price: 2, remaining: 1 }, { order: 'y', side: 'bid', price: 2, remaining: 3 }]);
  });
});
