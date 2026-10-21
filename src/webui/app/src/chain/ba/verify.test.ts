// Verified issuers (ba/verify.ts): the curators kept, the rule of the cache, who verifies an asset.
import { describe, expect, it } from 'vitest';
import { cleanCurators, fresh, isCuratorId, lookupKey, MAX_CURATORS, verifiedBy, VERIFY_TTL } from './verify';

const id = (c: string) => c.repeat(64);

describe('verified issuers', () => {
  it('keeps valid oracle ids, in lower case, once each, with their names', () => {
    expect(isCuratorId(id('a'))).toBe(true);
    expect(isCuratorId(id('g'))).toBe(false);
    expect(isCuratorId('abc')).toBe(false);
    expect(cleanCurators([id('A'), { id: id('a'), name: 'dup' }, { id: id('b'), name: 'Curator B' }, 'nope', 7, null])).toEqual([
      { id: id('a'), name: '' },
      { id: id('b'), name: 'Curator B' },
    ]);
    expect(cleanCurators('x')).toEqual([]);
    const many = Array.from({ length: 80 }, (_, i) => i.toString(16).padStart(64, '0'));
    expect(cleanCurators(many)).toHaveLength(MAX_CURATORS);
  });

  it('asks again after a minute, or with other curators', () => {
    expect(lookupKey('x', [id('b'), id('a')])).toBe(lookupKey('x', [id('a'), id('b')]));
    expect(lookupKey('x', [id('a')])).not.toBe(lookupKey('x', [id('a'), id('b')]));
    expect(fresh(1000, 1000 + VERIFY_TTL - 1)).toBe(true);
    expect(fresh(1000, 1000 + VERIFY_TTL)).toBe(false);
    expect(fresh(1000, 999)).toBe(false);
  });

  it('names the curators that verify an asset now, and none when none does', () => {
    expect(verifiedBy({ asset: 'x', verified: true, curators: [{ curator: id('a'), name: 'Alpha', verified: true, round: 1 }, { curator: id('b'), name: 'Beta', verified: false, round: 2 }, { curator: id('c'), name: '', verified: true, round: 1 }] })).toEqual(['Alpha #1', `${'c'.repeat(12)}… #1`]);
    expect(verifiedBy({ asset: 'x', verified: false, curators: [] })).toEqual([]);
    expect(verifiedBy(null)).toEqual([]);
  });
});
