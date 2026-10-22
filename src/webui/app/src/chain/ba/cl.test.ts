// Concentrated liquidity as the pages see it (ba/cl.ts): prices turned between the node's order and the page's, ticks to
// the pool's spacing, the presets of a range, the bars of the liquidity distribution and the scale of its chart.
import { describe, expect, it } from 'vitest';
import { chartDomain, flipped, fullRange, liquidityBars, positionSides, presetRange, priceAmounts, priceAtX, priceOfTick, priceText, rangeBar, rangeState, sideAlone, tickOfPrice, turn, turnRange, visibleBars, xOfPrice } from './cl';
import { feeNow, kindOf, kindParam } from './kinds';
import { CHN_ID } from './model';

const GOLD = 'a'.repeat(64);

describe('orientation', () => {
  it('turns the node\'s prices (CHN first in a pool with CHN) into the page\'s (CHN second), a range lower first', () => {
    expect(flipped({ asset_b_id: CHN_ID })).toBe(true);
    expect(flipped({ asset_b_id: GOLD })).toBe(false);
    expect(turn(4, true)).toBe(0.25);
    expect(turn(4, false)).toBe(4);
    expect(turnRange(2, 4, true)).toEqual([0.25, 0.5]);
    expect(turnRange(2, 4, false)).toEqual([2, 4]);
    expect(turn(0, true)).toBe(Infinity);
    const sides = positionSides({ amount0: 1, amount1: 2, fees0: 0.1, fees1: 0.2, price_lower: 2, price_upper: 4 }, true);
    expect(sides).toEqual({ amountA: 2, amountB: 1, feesA: 0.2, feesB: 0.1, lower: 0.25, upper: 0.5 });
  });
});

describe('ticks', () => {
  it('prices a tick, and snaps a price to the spacing: a lower bound down, an upper one up', () => {
    expect(priceOfTick(0, 2, 2)).toBe(1);
    expect(priceOfTick(0, 8, 2)).toBe(1e6);
    expect(priceOfTick(6932, 0, 0)).toBeCloseTo(2, 3);
    expect(tickOfPrice(1, 2, 2, 60, false)).toBe(0);
    expect(tickOfPrice(1.0001 ** 61.5, 2, 2, 60, false)).toBe(60);
    expect(tickOfPrice(1.0001 ** 61.5, 2, 2, 60, true)).toBe(120);
    expect(tickOfPrice(1.0001 ** -61.5, 2, 2, 60, false)).toBe(-120);
    expect(tickOfPrice(0, 2, 2, 60, false)).toBe(-887220);
    expect(tickOfPrice(Infinity, 2, 2, 60, true)).toBe(887220);
    const [lo, hi] = fullRange(200, 0, 0);
    expect(lo).toBeLessThan(1e-37);
    expect(hi).toBeGreaterThan(1e37);
  });
});

describe('ranges', () => {
  it('gives presets around the price, and the whole range', () => {
    expect(presetRange(100, '1', [1, 2])).toEqual([99, 101]);
    expect(presetRange(100, '5', [1, 2])).toEqual([95, 105]);
    expect(presetRange(100, '20', [1, 2])).toEqual([80, 120]);
    expect(presetRange(100, 'full', [1e-30, 1e30])).toEqual([1e-30, 1e30]);
  });

  it('says where the price is, and which asset a range out of it takes alone', () => {
    expect(rangeState(1, 2, 3)).toBe('below');
    expect(rangeState(2, 2, 3)).toBe('in');
    expect(rangeState(3, 2, 3)).toBe('above');
    // Above the price: the first asset alone (sold for the second as the price rises through it).
    expect(sideAlone(1, 2, 3)).toBe('a');
    expect(sideAlone(4, 2, 3)).toBe('b');
    expect(sideAlone(2.5, 2, 3)).toBeNull();
  });

  it('writes prices for the fields, and the amounts that name a concentrated pool\'s price', () => {
    expect(priceText(1.23456789)).toBe('1.23457');
    expect(priceText(0)).toBe('');
    expect(priceText(1e-9)).toBe('1.00000e-9');
    expect(priceAmounts(2.5, 2)).toEqual({ a: '100000', b: '250000' });
    expect(priceAmounts(1234.5, 8)).toEqual({ a: '1', b: '1234.5' });
    expect(priceAmounts(0, 2)).toBeNull();
    const tiny = priceAmounts(0.00001, 2)!;
    expect(Number(tiny.b) / Number(tiny.a)).toBeCloseTo(0.00001, 10);
  });
});

describe('the liquidity distribution', () => {
  const ticks = [
    { price: 0.9, liquidity_above: '1000' },
    { price: 1.1, liquidity_above: '0' },
    { price: 0.95, liquidity_above: '3000' },
    { price: 1.05, liquidity_above: '1000' },
  ];

  it('makes a bar from each tick to the next, with the liquidity in force; turned over for a pool with CHN', () => {
    expect(liquidityBars(ticks, false)).toEqual([
      { from: 0.9, to: 0.95, liquidity: 1000 },
      { from: 0.95, to: 1.05, liquidity: 3000 },
      { from: 1.05, to: 1.1, liquidity: 1000 },
    ]);
    const turned = liquidityBars(ticks, true);
    expect(turned[0].from).toBeCloseTo(1 / 1.1, 12);
    expect(turned[2].to).toBeCloseTo(1 / 0.9, 12);
    expect(turned.map((b) => b.liquidity)).toEqual([1000, 3000, 1000]);
  });

  it('scales the chart: around the price and the range, at most 4 times either way; x and price inverse', () => {
    expect(chartDomain(1, null)).toEqual([1 / 1.5, 1.5]);
    const [lo, hi] = chartDomain(1, [0.5, 3]);
    expect(lo).toBeCloseTo(0.5 / 1.15, 12);
    expect(hi).toBeCloseTo(3 * 1.15, 12);
    expect(chartDomain(1, [1e-30, 1e30])).toEqual([0.25, 4]);
    const domain: [number, number] = [0.5, 2];
    expect(xOfPrice(1, domain, 200)).toBeCloseTo(100, 9);
    expect(xOfPrice(10, domain, 200)).toBe(200);
    expect(priceAtX(xOfPrice(1.3, domain, 200), domain, 200)).toBeCloseTo(1.3, 9);
    const v = visibleBars(liquidityBars(ticks, false), [0.92, 1.0]);
    expect(v.bars).toEqual([{ from: 0.92, to: 0.95, liquidity: 1000 }, { from: 0.95, to: 1.0, liquidity: 3000 }]);
    expect(v.max).toBe(3000);
  });

  it('puts a position\'s range and the price on a bar of 0 to 100', () => {
    const bar = rangeBar(1, 0.8, 1.25);
    expect(bar.from).toBeGreaterThan(0);
    expect(bar.to).toBeLessThan(100);
    expect(bar.price).toBeCloseTo(50, 0);
    // The whole range is clipped around the price.
    const wide = rangeBar(1, 1e-30, 1e30);
    expect(wide.from).toBe(0);
    expect(wide.to).toBe(100);
    expect(wide.price).toBeCloseTo(50, 0);
  });
});

describe('dynamic fees', () => {
  it('reads a dynamic fee and its value now, and asks createpool for one', () => {
    const k = kindOf({ curve: 'concentrated', fee: 0.3, default_kind: false, dynamic_fee: true, fee_now: 0.85 });
    expect(k).toMatchObject({ curve: 'concentrated', dynamic_fee: true, fee_now: 0.85 });
    expect(feeNow(k)).toBe(0.85);
    expect(feeNow(kindOf({ fee: 0.3 }))).toBe(0.3);
    expect(kindParam('stable', 0.05, 100, 80, true)).toEqual({ curve: 'stable', fee: 0.05, amp: 100, dynamic_fee: true });
    expect(kindParam('concentrated', 0.3, 100, 80)).toEqual({ curve: 'concentrated', fee: 0.3 });
  });
});
