// Order books as the pages see them (ba/book.ts): the ladder, a limit order checked as the chain checks it, and what an
// order of the wallet is now.
import { describe, expect, it } from 'vitest';
import { buildLadder, decimalText, invertPrice, orderActions, orderStatus, parseDecimal, placeParams, planOrder, unitRatio, validDuration } from './book';
import { assetArg, CHN_ID } from './model';

const GOLD = { id: 'a'.repeat(64), decimals: 2 };
const USDX = { id: 'c'.repeat(64), decimals: 2 };
const CHN = { id: CHN_ID, decimals: 8 };

describe('the ladder', () => {
  it('puts the asks above (the best next to the spread) and the bids below, with running totals and depth', () => {
    const l = buildLadder({ asks: [{ price: 1.2, amount: 5, orders: 1 }, { price: 1.1, amount: 3, orders: 2 }], bids: [{ price: 0.9, amount: 6, orders: 1 }, { price: 1, amount: 4, orders: 1 }] });
    expect(l.asks.map((r) => [r.price, r.total])).toEqual([[1.2, 8], [1.1, 3]]);
    expect(l.bids.map((r) => [r.price, r.total])).toEqual([[1, 4], [0.9, 10]]);
    expect(l.asks.map((r) => r.depth)).toEqual([0.8, 0.3]);
    expect(l.bids.at(-1)!.depth).toBe(1);
    expect(l.bestAsk).toBe(1.1);
    expect(l.bestBid).toBe(1);
    expect(l.spread).toBeCloseTo(0.1);
    expect(l.mid).toBeCloseTo(1.05);
    expect(l.spreadPercent).toBeCloseTo(9.5238, 3);
  });

  it('keeps at most the rows asked each side, says when the book crosses, and reads an empty or odd book', () => {
    const many = Array.from({ length: 30 }, (_, i) => ({ price: 2 + i, amount: 1, orders: 1 }));
    expect(buildLadder({ asks: many, bids: [] }, 5).asks).toHaveLength(5);
    expect(buildLadder({ asks: many, bids: [] }, 5).asks.at(-1)!.price).toBe(2);
    const crossed = buildLadder({ asks: [{ price: 0.9, amount: 1, orders: 1 }], bids: [{ price: 1, amount: 1, orders: 1 }] });
    expect(crossed.spread! < 0).toBe(true);
    expect(buildLadder(null)).toMatchObject({ asks: [], bids: [], bestAsk: null, bestBid: null, spread: null, mid: null, spreadPercent: null });
    // Levels of nothing, or at no price, are left out; amounts as text (read exactly) are read.
    expect(buildLadder({ asks: [{ price: 0, amount: 1, orders: 1 }, { price: 1, amount: 0, orders: 1 }, { price: '1.5' as unknown as number, amount: '2' as unknown as number, orders: 1 }], bids: [] }).asks.map((r) => r.price)).toEqual([1.5]);
  });
});

describe('a limit order', () => {
  it('reads a price, and writes a decimal', () => {
    expect(parseDecimal('0.05')).toEqual({ ok: true, mantissa: 5n, scale: 2 });
    expect(parseDecimal('1,250.500')).toEqual({ ok: true, mantissa: 12505n, scale: 1 });
    expect(parseDecimal('')).toMatchObject({ ok: false, error: 'Enter a price.' });
    expect(parseDecimal('abc')).toMatchObject({ ok: false });
    expect(parseDecimal('0.000')).toMatchObject({ ok: false, error: 'The price is zero.' });
    expect(parseDecimal('1234567890.1234567891')).toMatchObject({ ok: false, error: 'The price has too many digits.' });
    expect(decimalText(12345n, 2)).toBe('123.45');
    expect(decimalText(5n, 3)).toBe('0.005');
    expect(decimalText(100n, 2)).toBe('1');
    expect(decimalText(5n, -2)).toBe('500');
  });

  it('works a price out in units as placeorder does, and inverts it for a buy, rounded up', () => {
    // 0.05 CHN per GOLD: 5·10^8 / 10^4 = 50,000 units of CHN per unit of GOLD.
    expect(unitRatio({ mantissa: 5n, scale: 2 }, 2, 8)).toEqual({ num: 50000n, den: 1n });
    expect(unitRatio({ mantissa: 3n, scale: 0 }, 2, 2)).toEqual({ num: 3n, den: 1n });
    expect(unitRatio({ mantissa: 1n, scale: 0 }, 0, 30)).toBeNull();
    expect(invertPrice({ mantissa: 5n, scale: 2 })).toEqual({ mantissa: 20n, scale: 0 });
    expect(invertPrice({ mantissa: 2n, scale: 0 })).toEqual({ mantissa: 5n, scale: 1 });
    expect(invertPrice({ mantissa: 1n, scale: 0 })).toEqual({ mantissa: 1n, scale: 0 });
    expect(invertPrice({ mantissa: 3n, scale: 0 })).toEqual({ mantissa: 333333333334n, scale: 12 });
    expect(invertPrice({ mantissa: 8n, scale: 0 })).toEqual({ mantissa: 125n, scale: 3 });
  });

  it('sells the base at a price at least, and buys it paying a price at most', () => {
    const sell = planOrder('sell', GOLD, CHN, 1000n, '0.05');
    expect(sell).toMatchObject({ ok: true, amount: '10', price: '0.05', sellUnits: 1000n, buysAtLeast: 50_000_000n });
    expect(sell.ok && sell.sell.id).toBe(GOLD.id);
    const buy = planOrder('buy', GOLD, CHN, 1000n, '0.05');
    // It sells 0.5 CHN at 20 GOLD per CHN at least: 10 GOLD.
    expect(buy).toMatchObject({ ok: true, amount: '0.5', price: '20', sellUnits: 50_000_000n, buysAtLeast: 1000n });
    expect(buy.ok && placeParams(buy, 240, assetArg)).toEqual(['CHN', '0.5', `0x${GOLD.id}`, '20', 240]);
    // A buy at 3 pays 3 per unit, rounded up, at the inverse rounded up (it never pays more).
    const third = planOrder('buy', GOLD, USDX, 100n, '3');
    expect(third).toMatchObject({ ok: true, amount: '3', price: '0.333333333334', sellUnits: 300n, buysAtLeast: 100n });
  });

  it('refuses dust as the chain does, and more than the wallet holds', () => {
    // 0.1 GOLD at 0.05: 0.005 CHN, under 0.01 CHN.
    expect(planOrder('sell', GOLD, CHN, 10n, '0.05')).toMatchObject({ ok: false, error: expect.stringContaining('0.01 CHN') });
    // Selling CHN: 0.005 CHN.
    expect(planOrder('buy', GOLD, CHN, 10n, '0.05')).toMatchObject({ ok: false, error: expect.stringContaining('0.01 CHN') });
    // Exactly 0.01 CHN is enough.
    expect(planOrder('sell', GOLD, CHN, 20n, '0.05')).toMatchObject({ ok: true, buysAtLeast: 1_000_000n });
    // Without CHN: a whole unit of what it sells.
    expect(planOrder('sell', GOLD, USDX, 50n, '1')).toMatchObject({ ok: false, error: expect.stringContaining('whole unit') });
    expect(planOrder('sell', GOLD, USDX, 100n, '1')).toMatchObject({ ok: true });
    expect(planOrder('buy', GOLD, USDX, 50n, '1')).toMatchObject({ ok: false, error: expect.stringContaining('whole unit') });
    // Something to buy at the limit.
    expect(planOrder('sell', GOLD, USDX, 100n, '0.001')).toMatchObject({ ok: false, error: expect.stringContaining('buys nothing') });
    expect(planOrder('sell', GOLD, CHN, 1000n, '0.05', 500n)).toMatchObject({ ok: false, error: 'This wallet holds less than that.' });
    expect(planOrder('sell', GOLD, GOLD, 1000n, '1')).toMatchObject({ ok: false });
    expect(planOrder('sell', GOLD, CHN, 0n, '1')).toMatchObject({ ok: false, error: 'Enter an amount.' });
    expect(planOrder('sell', GOLD, CHN, 1000n, '')).toMatchObject({ ok: false, error: 'Enter a price.' });
  });

  it('trades 1 block to about a year', () => {
    expect([0, 1, 40320, 2_102_400, 2_102_401, 1.5].map(validDuration)).toEqual([false, true, true, true, false, false]);
  });
});

describe('the orders of the wallet', () => {
  const o = (x: Partial<{ remaining: number; to_collect: number; expired: boolean; evicted: boolean }>) => ({ remaining: 5, to_collect: 0, expired: false, evicted: false, ...x });
  it('tells open, filled, expired and evicted apart, evicted first', () => {
    expect(orderStatus(o({}))).toBe('open');
    expect(orderStatus(o({ remaining: 0 }))).toBe('filled');
    expect(orderStatus(o({ expired: true }))).toBe('expired');
    expect(orderStatus(o({ evicted: true, expired: true, remaining: 0 }))).toBe('evicted');
  });
  it('collects an open order only when it got something, cancels it, and settles one that trades no more', () => {
    expect(orderActions(o({}))).toEqual({ collect: false, cancel: true, settle: false });
    expect(orderActions(o({ to_collect: 1 }))).toEqual({ collect: true, cancel: true, settle: false });
    expect(orderActions(o({ remaining: 0, to_collect: 1 }))).toEqual({ collect: false, cancel: false, settle: true });
    expect(orderActions(o({ evicted: true }))).toEqual({ collect: false, cancel: false, settle: true });
  });
});
