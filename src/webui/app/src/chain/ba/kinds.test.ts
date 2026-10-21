// The DEX's pool kinds as the pages see them (ba/kinds.ts): the kinds read, the ways a trade can go and how the
// wallet sends the one chosen, the depth of a pool, liquidity by a pool's id.
import { describe, expect, it } from 'vitest';
import { alignDepth, chooseOption, clampAmp, defaultFee, depositAmounts, depthSeries, executionOf, feePercent, flipDepth, flipKind, kindOf, kindParam, quoteAmounts, sharesOf, slippageRows, tradeOptions, type DepthPoint, type MarketQuote, type RouteQuote } from './kinds';
import { amountParam, CHN_ID } from './model';

const id = (c: string) => c.repeat(64);
const GOLD = id('a');
const SILVER = id('b');
const USDX = id('c');

describe('pool kinds', () => {
  it('reads a kind, and a pool of a node from before the kinds as the default one', () => {
    expect(kindOf({})).toEqual({ curve: 'constant_product', fee: 0.3, default_kind: true });
    expect(kindOf({ curve: 'constant_product', fee: 1, default_kind: false })).toEqual({ curve: 'constant_product', fee: 1, default_kind: false });
    expect(kindOf({ curve: 'stable', fee: 0.05, fee_millionths: 500, amp: 200, default_kind: false })).toEqual({ curve: 'stable', fee: 0.05, fee_millionths: 500, amp: 200, default_kind: false });
    expect(kindOf({ curve: 'weighted', fee: '0.3', weight_a: 80, weight_b: 20, default_kind: false })).toMatchObject({ curve: 'weighted', fee: 0.3, weight_a: 80, weight_b: 20 });
    // An unknown curve reads as the default one; a weighted pool without weight_b has the rest.
    expect(kindOf({ curve: 'cubic' }).curve).toBe('constant_product');
    // An order book is a kind of its own (Layers' DEX), never the default one.
    expect(kindOf({ curve: 'book', fee: 0, default_kind: false })).toMatchObject({ curve: 'book', default_kind: false });
    expect(kindOf({ curve: 'weighted', weight_a: 75, default_kind: false }).weight_b).toBe(25);
    expect(flipKind(kindOf({ curve: 'weighted', weight_a: 80, weight_b: 20, default_kind: false }))).toMatchObject({ weight_a: 20, weight_b: 80 });
    const stable = kindOf({ curve: 'stable', amp: 100, fee: 0.05, default_kind: false });
    expect(flipKind(stable)).toBe(stable);
  });

  it('writes fees, starts each curve at its fee, bounds A, and names the kind as createpool takes it', () => {
    expect([0.01, 0.05, 0.3, 1].map(feePercent)).toEqual(['0.01%', '0.05%', '0.3%', '1%']);
    expect(defaultFee('stable')).toBe(0.05);
    expect(defaultFee('weighted')).toBe(0.3);
    expect(defaultFee('constant_product', [0.01, 1])).toBe(0.01);
    expect(clampAmp(0)).toBe(1);
    expect(clampAmp(123.6)).toBe(124);
    expect(clampAmp(1e9, 1, 5000)).toBe(5000);
    expect(clampAmp(Number.NaN)).toBe(100);
    expect(kindParam('constant_product', 1, 100, 80)).toEqual({ curve: 'constant_product', fee: 1 });
    expect(kindParam('stable', 0.05, 200, 80)).toEqual({ curve: 'stable', fee: 0.05, amp: 200 });
    expect(kindParam('weighted', 0.3, 200, 80)).toEqual({ curve: 'weighted', fee: 0.3, weight_a: 80 });
  });
});

describe('ways to trade', () => {
  const market: MarketQuote = {
    pools: [
      { pool: id('1'), curve: 'stable', fee: 0.05, amp: 100, default_kind: false, amount_in: 10, amount_out: 9.9, price: 1.01, spot_price: 1, price_impact: 0.6, fee_paid: 0.005, abandoned: false },
      { pool: id('2'), curve: 'constant_product', fee: 0.3, default_kind: true, amount_in: 10, amount_out: 9.5, price: 1.05, spot_price: 1, price_impact: 4.2, fee_paid: 0.03, abandoned: false },
      { pool: id('3'), curve: 'constant_product', fee: 1, default_kind: false, amount_in: 10, amount_out: 9.99, price: 1, spot_price: 1, price_impact: 0, fee_paid: 0.1, abandoned: true },
    ],
    best_pool: id('1'),
  };
  const routes: RouteQuote = {
    amount_in: 10,
    amount_out: 9.95,
    path: ['GOLD', 'CHN', 'SILVER'],
    pools: [id('4'), id('5')],
    routes: [
      { path: ['GOLD', 'CHN', 'SILVER'], path_ids: [GOLD, CHN_ID, SILVER], amount_out: 9.95, pools: [id('4'), id('5')], default_pools: false, hops: [{ pool: id('4'), curve: 'weighted', fee: 0.3, amount_out: 0.5 }, { pool: id('5'), curve: 'constant_product', fee: 0.3, amount_out: 9.95 }] },
      { path: ['GOLD', 'SILVER'], path_ids: [GOLD, SILVER], amount_out: 9.9, pools: [id('1')], default_pools: false, hops: [{ pool: id('1'), curve: 'stable', fee: 0.05, amount_out: 9.9 }] },
    ],
  };

  it('lists every pool of the pair and the routes through others, best first, closed pools left out', () => {
    const options = tradeOptions({ pay: GOLD, get: SILVER, getDecimals: 2, market, routes, kindById: (pool, from) => (pool === id('4') && from === GOLD ? kindOf({ curve: 'weighted', fee: 0.3, weight_a: 80, weight_b: 20, default_kind: false }) : undefined) });
    expect(options.map((o) => o.key)).toEqual([`${id('4')},${id('5')}`, id('1'), id('2')]);
    expect(options[0].out).toBe(995n);
    expect(options[0].path).toEqual([GOLD, CHN_ID, SILVER]);
    // The hop's kind from the pools read (its weights), else from the route (its curve and fee).
    expect(options[0].hops.map((h) => h.kind)).toEqual([kindOf({ curve: 'weighted', fee: 0.3, weight_a: 80, weight_b: 20, default_kind: false }), kindOf({ curve: 'constant_product', fee: 0.3 })]);
    expect(options[0].impact).toBeUndefined();
    expect(options[1]).toMatchObject({ pools: [id('1')], impact: 0.6, defaultPools: false, out: 990n });
    expect(chooseOption(options, '')?.key).toBe(options[0].key);
    expect(chooseOption(options, id('2'))?.key).toBe(id('2'));
    // A way picked that is no longer offered: the best.
    expect(chooseOption(options, id('9'))?.key).toBe(options[0].key);
    expect(chooseOption([], '')).toBeNull();
  });

  it('at the same amount, prefers the default pool, then fewer hops; without getswapquote, quoteswap\'s quote and single-pool routes', () => {
    const tie: MarketQuote = { pools: [{ ...market.pools[0], amount_out: 9.5 }, market.pools[1]] };
    expect(tradeOptions({ pay: GOLD, get: SILVER, getDecimals: 2, market: tie }).map((o) => o.key)).toEqual([id('2'), id('1')]);
    const old = tradeOptions({ pay: GOLD, get: SILVER, getDecimals: 2, single: { pool: id('2'), amount_out: 9.5, price_impact: 4.2, fee: 0.03 }, routes });
    expect(old.map((o) => o.key)).toEqual([`${id('4')},${id('5')}`, id('1'), id('2')]);
    expect(old.find((o) => o.key === id('2'))).toMatchObject({ defaultPools: true, impact: 4.2 });
    expect(tradeOptions({ pay: GOLD, get: SILVER, getDecimals: 2 })).toEqual([]);
  });

  it('sends one hop in the default pool by swapasset, one pool of another kind by swappools, a route by swaproute', () => {
    const a = { amount: '10', slippage: 0.5, minOut: '9.85', maxImpact: 10 };
    expect(executionOf({ pools: [id('2')], path: [CHN_ID, GOLD], defaultPools: true }, a)).toEqual({ method: 'swapasset', params: ['CHN', '10', `0x${GOLD}`, 0.5, 10, '9.85'] });
    expect(executionOf({ pools: [id('1')], path: [GOLD, SILVER], defaultPools: false }, a)).toEqual({ method: 'swappools', params: [`0x${GOLD}`, '10', [id('1')], 0.5, '9.85'] });
    expect(executionOf({ pools: [id('4'), id('5')], path: [GOLD, CHN_ID, SILVER], defaultPools: false }, a)).toEqual({ method: 'swaproute', params: [[`0x${GOLD}`, 'CHN', `0x${SILVER}`], '10', 0.5, '9.85', [id('4'), id('5')]] });
    // Every pool of the default kind: the path alone names them (as before the pool kinds).
    expect(executionOf({ pools: [id('4'), id('5')], path: [GOLD, CHN_ID, SILVER], defaultPools: true }, a)).toEqual({ method: 'swaproute', params: [[`0x${GOLD}`, 'CHN', `0x${SILVER}`], '10', 0.5, '9.85'] });
  });
});

describe('depth', () => {
  const depth: DepthPoint[] = [
    { side: 'sell_a', size_percent: 10, amount_in: 100, amount_out: 9, avg_price: 0.09, price_after: 0.082, price_impact: 9.3 },
    { side: 'sell_b', size_percent: 10, amount_in: 10, amount_out: 90, avg_price: 0.111, price_after: 0.121, price_impact: 9.1 },
    { side: 'sell_a', size_percent: 0.1, amount_in: 1, amount_out: 0.0997, avg_price: 0.0997, price_after: 0.0998, price_impact: 0.4 },
    { side: 'sell_b', size_percent: 0.1, amount_in: 0.1, amount_out: 0.997, avg_price: 0.1003, price_after: 0.1002, price_impact: 0.4 },
  ];

  it('makes the lines of the chart, sorted by size, and the rows of the table of slippage', () => {
    expect(depthSeries(depth)).toEqual({ a: [{ x: 0.1, y: 0.4 }, { x: 10, y: 9.3 }], b: [{ x: 0.1, y: 0.4 }, { x: 10, y: 9.1 }] });
    // A size of nothing has no place on a log scale.
    expect(depthSeries([{ ...depth[0], size_percent: 0 }]).a).toEqual([]);
    const rows = slippageRows(depth);
    expect(rows.map((r) => r.size)).toEqual([0.1, 10]);
    expect(rows[1].a?.amount_in).toBe(100);
    expect(rows[1].b?.amount_in).toBe(10);
  });

  it('turns getpoolcurve\'s depth (CHN first) into the order of the lists (CHN second)', () => {
    const flipped = flipDepth(depth);
    expect(flipped[0]).toMatchObject({ side: 'sell_b', avg_price: 1 / 0.09, price_impact: 9.3 });
    const pool = { asset_a: 'GOLD', asset_b: 'CHN', asset_a_id: GOLD, asset_b_id: CHN_ID };
    expect(alignDepth({ asset_a: 'CHN', asset_b: 'GOLD', depth }, pool)[0].side).toBe('sell_b');
    expect(alignDepth({ asset_a: 'GOLD', asset_b: 'CHN', depth }, pool)).toBe(depth);
    // Labels the lists do not tell apart: CHN, the null hash, is the node's first.
    expect(alignDepth({ asset_a: 'X', asset_b: 'X', depth }, { ...pool, asset_a: 'X', asset_b: 'X' })[0].side).toBe('sell_b');
    expect(alignDepth({ asset_a: 'X', asset_b: 'X', depth }, { asset_a: 'X', asset_b: 'X', asset_a_id: GOLD, asset_b_id: USDX })).toBe(depth);
    // A node that says the ids: by them.
    expect(alignDepth({ asset_a: 'GOLD', asset_b: 'GOLD', asset_a_id: CHN_ID, asset_b_id: GOLD, depth }, pool)[0].side).toBe('sell_b');
    expect(alignDepth({ asset_a: 'CHN', asset_b: 'GOLD', asset_a_id: GOLD, asset_b_id: CHN_ID, depth }, pool)).toBe(depth);
  });
});

describe('liquidity by a pool id', () => {
  it('names the amounts by asset id, those above zero', () => {
    expect(depositAmounts([
      { id: GOLD, units: 1050n, text: (u) => amountParam(u, 2) },
      { id: CHN_ID, units: 0n, text: (u) => amountParam(u, 8) },
    ])).toEqual({ [`0x${GOLD}`]: '10.5' });
    expect(depositAmounts([{ id: CHN_ID, units: 50_000_000n, text: (u) => amountParam(u, 8) }, { id: GOLD, units: null, text: () => 'x' }])).toEqual({ CHN: '0.5' });
  });

  it('reads the amounts of a quote of quotepoolliquidity in the order of the lists', () => {
    const pool = { asset_a: 'GOLD', asset_b: 'CHN', asset_b_id: CHN_ID };
    expect(quoteAmounts({ CHN: 1, GOLD: 10 }, pool)).toEqual({ a: 10, b: 1 });
    // Labels alike: the node's order, CHN first.
    expect(quoteAmounts({ X: 1, Y: 10 }, { asset_a: 'X', asset_b: 'X', asset_b_id: CHN_ID })).toEqual({ a: 10, b: 1 });
    expect(quoteAmounts({ X: 1, Y: 10 }, { asset_a: 'X', asset_b: 'X', asset_b_id: USDX })).toEqual({ a: 1, b: 10 });
  });

  it('takes a percentage of the shares held, all of them at 100', () => {
    expect(sharesOf(1_000_001n, 100)).toBe(1_000_001n);
    expect(sharesOf(1_000_000n, 50)).toBe(500_000n);
    expect(sharesOf(1_000n, 0)).toBe(0n);
  });
});
