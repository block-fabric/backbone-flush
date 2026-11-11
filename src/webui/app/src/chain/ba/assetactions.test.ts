// The routes of the asset pages (ba/assetactions.ts): they carry the asset, its side or its box, and read back.
import { describe, expect, it } from 'vitest';
import { assetRoute, parseDexRoute, poolRoute, swapRoute } from './assetactions';

const A = 'a1'.repeat(32);
const B = 'b2'.repeat(32);

describe('the routes of the asset pages', () => {
  it('the routes carry the asset and its side, and read back', () => {
    expect(parseDexRoute(swapRoute(A, 'get'))).toEqual({ swap: { side: 'get', id: A } });
    expect(parseDexRoute(swapRoute(A, 'pay'))).toEqual({ swap: { side: 'pay', id: A } });
    // An asset's page, and the box it opens on.
    expect(parseDexRoute(assetRoute(A))).toEqual({ asset: { id: A, box: '' } });
    expect(parseDexRoute(assetRoute(A, 'sell'))).toEqual({ asset: { id: A, box: 'sell' } });
    expect(parseDexRoute(assetRoute(A, 'liquidity'))).toEqual({ asset: { id: A, box: 'liquidity' } });
    expect(parseDexRoute(`asset/${A}/nonsense`)).toEqual({ asset: { id: A, box: '' } });
    expect(parseDexRoute(poolRoute(B))).toEqual({ pool: B });
    for (const tab of ['swap', 'pools', 'launches', `launches/${A}`, 'asset', 'asset/x', 'swap/get/x', 'pools/../x']) expect(parseDexRoute(tab)).toEqual({});
  });
});
