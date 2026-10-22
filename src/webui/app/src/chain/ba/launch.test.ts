// Layers' launches as the pages see them (ba/launch.ts): the bonding curve from the rules, the split of the supply and
// of the fee, the queries of the list, the options of launchtoken and what is wrong with them, what a creator is owed.
import { describe, expect, it } from 'vitest';
import { appendLaunches, claimable, curvePointAt, curveSeries, curveShape, launchOptions, launchProblems, launchQuery, rulesOf, supplySplit, type Launch } from './launch';

describe('the bonding curve', () => {
  const shape = curveShape(500, 1_000_000_000);

  it('is a constant product of virtual reserves that sells 3/4 of the supply for exactly the graduation, its price rising 9 times', () => {
    expect(shape.virtualCoin).toBe(250);
    expect(shape.virtualTokens).toBe(1_125_000_000);
    expect(shape.endTokens).toBe(375_000_000);
    // V·Tv = (V + G)·yE.
    expect(shape.k).toBeCloseTo((250 + 500) * 375_000_000, 0);
    expect(shape.curveSupply).toBe(750_000_000);
    expect(shape.poolSupply).toBe(250_000_000);
    expect(shape.startPrice).toBeCloseTo((4 * 500) / (9 * 1e9), 15);
    expect(shape.endPrice).toBeCloseTo((4 * 500) / 1e9, 15);
    expect(shape.endPrice / shape.startPrice).toBeCloseTo(9, 9);
    // The market value of the supply at graduation is 4G.
    expect(shape.endCap).toBeCloseTo(2000, 6);
  });

  it('is where the CHN raised put it: nothing sold at the start, 3/4 at the end, never past it', () => {
    expect(curvePointAt(shape, 0)).toMatchObject({ sold: 0, coin: 250 });
    const end = curvePointAt(shape, 500);
    expect(end.sold).toBeCloseTo(750_000_000, 0);
    expect(end.price).toBeCloseTo(shape.endPrice, 15);
    expect(curvePointAt(shape, 900).sold).toBeCloseTo(750_000_000, 0);
    expect(curvePointAt(shape, -1).sold).toBe(0);
    const series = curveSeries(shape, 10);
    expect(series).toHaveLength(11);
    expect(series[0]).toEqual({ x: 0, y: shape.startPrice });
    expect(series.every((p, i) => i === 0 || (p.x > series[i - 1].x && p.y > series[i - 1].y))).toBe(true);
  });

  it('splits the supply (the allocation on top, a dilution) and the fee', () => {
    expect(supplySplit(shape, 0)).toEqual({ curve: 750_000_000, pool: 250_000_000, allocation: 0, total: 1_000_000_000, dilution: 0 });
    const two = supplySplit(shape, 2);
    expect(two.allocation).toBe(20_000_000);
    expect(two.total).toBe(1_020_000_000);
    expect(two.dilution).toBeCloseTo((2 / 102) * 100, 9);
  });

  it('reads the rules as numbers, those of every network until the node answers', () => {
    expect(rulesOf(null)).toEqual({ graduation: 500, supply: 1_000_000_000, fee: 1, creatorShare: 30, maxAllocation: 5, active: false });
    expect(rulesOf({ graduation: '500.00000000', fee: 1, creator_share: 30, supply: '1000000000.000000', curve_supply: 0, pool_supply: 0, max_allocation: 5, log_blocks: 1, active: true })).toMatchObject({ graduation: 500, supply: 1e9, active: true });
  });
});

describe('the list of launches', () => {
  it('asks listlaunches for a filter: near graduation is on the curve and that far', () => {
    expect(launchQuery('all', 'newest', 80, undefined, 50)).toEqual({ status: 'all', sort: 'newest', limit: 50 });
    expect(launchQuery('near', 'progress', 90, '50', 50)).toEqual({ status: 'curve', sort: 'progress', min_progress: 90, cursor: '50', limit: 50 });
    expect(launchQuery('graduated', 'volume', 80, '', 10)).toEqual({ status: 'graduated', sort: 'volume', limit: 10 });
  });

  it('adds a page without the launches listed already', () => {
    const l = (token: string) => ({ token }) as Launch;
    expect(appendLaunches([l('a'), l('b')], [l('b'), l('c')]).map((x) => x.token)).toEqual(['a', 'b', 'c']);
  });
});

describe('launching', () => {
  it('gives launchtoken only the options set', () => {
    expect(launchOptions({ displayName: '', logo: '', info: '  ', allocation: 0, firstBuy: '', minBuy: '0', maxBuy: '' })).toEqual({});
    expect(launchOptions({ displayName: 'To the moon', logo: 'iVBO', info: 'https://example.org', allocation: 2, firstBuy: '1', minBuy: '0.01', maxBuy: '50' })).toEqual({ display_name: 'To the moon', logo: 'iVBO', info: 'https://example.org', allocation: 2, first_buy: '1', min_buy: '0.01', max_buy: '50' });
  });

  it('says what the wallet would refuse, and a first buy the launch would refund', () => {
    const ok = { allocation: 0, maxAllocation: 5, firstBuy: 0n, minBuy: 0n, maxBuy: 0n, infoBytes: 10, maxInfo: 512, minChn: 50_000n };
    expect(launchProblems(ok)).toEqual([]);
    expect(launchProblems({ ...ok, allocation: 6 })).toEqual(['The allocation is 0 to 5%.']);
    expect(launchProblems({ ...ok, infoBytes: 513 })).toEqual(['The description is at most 512 bytes.']);
    expect(launchProblems({ ...ok, firstBuy: 1n })).toEqual(['The first buy pays 0.0005 CHN at least.']);
    expect(launchProblems({ ...ok, minBuy: 200n, maxBuy: 100n })).toEqual(['The most a buy pays is less than the least.']);
    expect(launchProblems({ ...ok, firstBuy: 100_000_000n, maxBuy: 50_000_000n })).toEqual(['The first buy is outside the least and the most a buy pays: it would be refunded.']);
  });

  it('pays the creator its fees from 0.0005 CHN, its allocation once graduated and not minted', () => {
    expect(claimable({ creator_fees_owed: 0.0001, status: 'curve', allocation: 2, allocation_claimed: false })).toEqual({ fees: false, allocation: false });
    expect(claimable({ creator_fees_owed: '0.01000000', status: 'graduated', allocation: 2, allocation_claimed: false })).toEqual({ fees: true, allocation: true });
    expect(claimable({ creator_fees_owed: 0, status: 'graduated', allocation: 2, allocation_claimed: true })).toEqual({ fees: false, allocation: false });
  });
});
