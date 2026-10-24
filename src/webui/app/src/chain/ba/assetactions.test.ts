// The actions of an asset's row (ba/assetactions.ts): from its markets, the most relevant first; the routes that carry
// the asset and its side.
import { describe, expect, it } from 'vitest';
import { assetActions, launchRoute, marketsOf, parseDexRoute, poolRoute, swapRoute, type ActionContext } from './assetactions';
import { CHN_ID, type Pool } from './model';

const A = 'a1'.repeat(32);
const B = 'b2'.repeat(32);
const pool = (id: string, extra: Partial<Pool> = {}): Pool => ({ pool: id, asset_a: 'GOLD', asset_b: 'ECX', asset_a_id: A, asset_b_id: CHN_ID, reserve_a: 100, reserve_b: 10, price: 0.1, shares: 1, volume_a: 0, volume_b: 0, swaps: 0, created: 1, abandoned: false, ...extra });
const none = { curve: false, graduated: false, pools: 0, bestPool: '', curvePool: '' };
const ctx = (extra: Partial<ActionContext>): ActionContext => ({ markets: none, wallet: true, held: false, controlled: false, claimable: false, ...extra });

describe('the markets of an asset', () => {
  it('a launch curve, pools of other kinds, the best pool by its ECX, books and closed pools left out', () => {
    const pools = [
      pool('c'.repeat(64), { curve: 'launch' }),
      pool('d'.repeat(64), { reserve_b: 5 }),
      pool('e'.repeat(64), { reserve_b: 50, curve: 'concentrated' }),
      pool('f'.repeat(64), { curve: 'book' }),
      pool('9'.repeat(64), { abandoned: true, reserve_b: 1000 }),
    ];
    expect(marketsOf(A, pools)).toEqual({ curve: true, graduated: false, pools: 2, bestPool: 'e'.repeat(64), curvePool: 'c'.repeat(64) });
    expect(marketsOf(B, pools)).toEqual(none);
    // The launch known: graduated into its pool.
    expect(marketsOf(B, [], { status: 'graduated', pool: '7'.repeat(64) })).toMatchObject({ curve: false, graduated: true, bestPool: '7'.repeat(64) });
  });
});

describe('the actions of an asset', () => {
  it('on a curve: Buy first, Sell on the curve when held, View market; Send, Receive, View asset', () => {
    const m = { ...none, curve: true, curvePool: 'c'.repeat(64) };
    expect(assetActions(ctx({ markets: m }))).toEqual({ primary: 'buy', more: ['view-market', 'receive', 'view'] });
    expect(assetActions(ctx({ markets: m, held: true }))).toEqual({ primary: 'buy', more: ['sell-curve', 'view-market', 'send', 'receive', 'view'] });
  });

  it('graduated or with pools: Swap first, Sell when held, Add liquidity, View market', () => {
    const m = { ...none, pools: 1, bestPool: 'd'.repeat(64) };
    expect(assetActions(ctx({ markets: m, held: true }))).toEqual({ primary: 'swap', more: ['sell', 'add-liquidity', 'view-market', 'send', 'receive', 'view'] });
    expect(assetActions(ctx({ markets: { ...none, graduated: true, bestPool: '7'.repeat(64) } }))).toEqual({ primary: 'swap', more: ['add-liquidity', 'view-market', 'receive', 'view'] });
  });

  it('no market: Open a market for its holder or controller; else View asset', () => {
    expect(assetActions(ctx({ held: true }))).toEqual({ primary: 'open-market', more: ['send', 'receive', 'view'] });
    expect(assetActions(ctx({ controlled: true }))).toEqual({ primary: 'open-market', more: ['receive', 'view'] });
    expect(assetActions(ctx({}))).toEqual({ primary: 'view', more: ['receive'] });
    // No wallet: only to look.
    expect(assetActions(ctx({ wallet: false, held: true }))).toEqual({ primary: 'view', more: [] });
  });

  it('claimable: Claim first, for the creator', () => {
    const m = { ...none, curve: true };
    expect(assetActions(ctx({ markets: m, claimable: true }))).toEqual({ primary: 'claim', more: ['buy', 'view-market', 'receive', 'view'] });
    expect(assetActions(ctx({ markets: { ...none, graduated: true, bestPool: '7'.repeat(64) }, claimable: true, held: true })).primary).toBe('claim');
    expect(assetActions(ctx({ markets: m, claimable: true, wallet: false })).primary).toBe('buy');
  });

  it('the routes carry the asset and its side, and read back', () => {
    expect(parseDexRoute(swapRoute(A, 'get'))).toEqual({ swap: { side: 'get', id: A } });
    expect(parseDexRoute(swapRoute(A, 'pay'))).toEqual({ swap: { side: 'pay', id: A } });
    expect(parseDexRoute(launchRoute(A, 'sell'))).toEqual({ launch: { token: A, side: 'sell' } });
    expect(parseDexRoute(`launches/${A}`)).toEqual({ launch: { token: A, side: 'buy' } });
    expect(parseDexRoute(poolRoute(B))).toEqual({ pool: B });
    for (const tab of ['swap', 'pools', 'launches', 'swap/get/x', 'pools/../x']) expect(parseDexRoute(tab)).toEqual({});
  });
});
