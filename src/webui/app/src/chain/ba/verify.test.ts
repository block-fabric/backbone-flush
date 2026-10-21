// Verified issuers (ba/verify.ts): the curators kept, the rule of the cache, who verifies an asset.
import { describe, expect, it } from 'vitest';
import { cleanCurators, curatorWarnings, fresh, isCuratorId, lookupKey, MAX_CURATORS, nameSkeleton, verifiedBy, VERIFY_TTL } from './verify';

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
    expect(verifiedBy({ asset: 'x', verified: true, curators: [{ curator: id('a'), name: 'Alpha', verified: true, round: 1 }, { curator: id('b'), name: 'Beta', verified: false, round: 2 }, { curator: id('c'), name: '', verified: true, round: 1 }] })).toEqual([`Alpha (${'a'.repeat(12)}…) #1`, `${'c'.repeat(12)}… #1`]);
    expect(verifiedBy({ asset: 'x', verified: false, curators: [] })).toEqual([]);
    expect(verifiedBy(null)).toEqual([]);
  });
});

describe('curators: names are labels, ids are curators', () => {
  const A = 'a'.repeat(64);
  const B = 'b'.repeat(64);
  const C = 'c'.repeat(64);
  it('reads look-alike names as one', () => {
    expect(nameSkeleton('Curator')).toBe(nameSkeleton('CURAT0R'));
    expect(nameSkeleton('Layers Labs')).toBe(nameSkeleton('Layers  1abs'));
    expect(nameSkeleton('Cоmpany')).toBe(nameSkeleton('Company')); // a Cyrillic о
    expect(nameSkeleton('Modern')).toBe(nameSkeleton('Modem'.replace('m', 'rn')));
    expect(nameSkeleton('Café')).toBe(nameSkeleton('Cafe'));
    expect(nameSkeleton('Alpha')).not.toBe(nameSkeleton('Beta'));
  });
  it('warns of names shared by several oracles', () => {
    const w = curatorWarnings({ oracle: A, name: 'Alpha' }, [{ oracle: A, name: 'Alpha' }, { oracle: B, name: 'ALPHA' }], []);
    expect(w.some((x) => x.includes('Another oracle goes by a name like it'))).toBe(true);
  });
  it("warns of a name like a trusted curator's under another id", () => {
    const w = curatorWarnings({ oracle: C, name: 'A1pha' }, [], [{ id: A, name: 'Alpha' }]);
    expect(w.some((x) => x.includes('posing'))).toBe(true);
    expect(w.join(' ')).toContain(A.slice(0, 12));
  });
  it('no warning for a distinct name, nor for the same curator again', () => {
    expect(curatorWarnings({ oracle: C, name: 'Gamma' }, [{ oracle: A, name: 'Alpha' }], [{ id: A, name: 'Alpha' }])).toEqual([]);
    expect(curatorWarnings({ oracle: A, name: 'Alpha' }, [{ oracle: A, name: 'Alpha' }], [{ id: A, name: 'Alpha' }])).toEqual([]);
  });
  it('warns of a nameless oracle', () => {
    expect(curatorWarnings({ oracle: A, name: '' }, [], []).length).toBe(1);
  });
  it('names a verifying curator with its id', () => {
    expect(verifiedBy({ asset: 'x', verified: true, curators: [{ curator: A, name: 'Alpha', verified: true, round: 2 }] })).toEqual([`Alpha (${A.slice(0, 12)}…) #2`]);
  });
});
