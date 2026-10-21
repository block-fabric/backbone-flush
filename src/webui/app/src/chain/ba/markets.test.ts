// The market index (ba/markets.ts): the columns and the sorts of searchpools, the change of a price, pages put together.
import { describe, expect, it } from 'vitest';
import { appendMarkets, changeDirection, COLUMN_SORT, formatChange, isBook, isMarketSort, sortOfColumn, type Market } from './markets';

describe('the market index', () => {
  it('maps the headers onto the sorts of searchpools', () => {
    expect(sortOfColumn('liquidity')).toBe('depth');
    expect(sortOfColumn('volume')).toBe('volume');
    expect(sortOfColumn('change')).toBe('price_change');
    expect(sortOfColumn('pair')).toBe('newest');
    expect(sortOfColumn('kind')).toBeNull();
    expect(sortOfColumn('price')).toBeNull();
    // Each sort of the node has its column.
    expect(new Set(Object.values(COLUMN_SORT).filter(Boolean))).toEqual(new Set(['depth', 'volume', 'price_change', 'newest']));
    expect(isMarketSort('depth')).toBe(true);
    expect(isMarketSort('pool_depth')).toBe(false);
  });

  it('writes a change signed, and says which way it goes', () => {
    expect(formatChange(12.5)).toBe('+12.5%');
    expect(formatChange('-3.2')).toBe('-3.20%');
    expect(formatChange(0)).toBe('0.00%');
    expect(formatChange(1234.5)).toBe('+1235%');
    expect(formatChange(undefined)).toBe('–');
    expect([1, -1, 0, null, 'x'].map((x) => changeDirection(x as number))).toEqual(['up', 'down', 'flat', 'none', 'none']);
  });

  it('puts a page after those shown, once each, and tells a book', () => {
    const m = (pool: string) => ({ pool }) as Market;
    expect(appendMarkets([m('a'), m('b')], [m('b'), m('c')]).map((x) => x.pool)).toEqual(['a', 'b', 'c']);
    expect(isBook({ curve: 'book' })).toBe(true);
    expect(isBook({ curve: 'stable' })).toBe(false);
  });
});
