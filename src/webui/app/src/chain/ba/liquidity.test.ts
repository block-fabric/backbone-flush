// The Add liquidity form's state (ba/liquidity.ts): the pool addliquidity names and no other kind of the pair, and
// what the pool read again on submit means (a pair with only a concentrated pool showed "nobody provides liquidity
// to this pool any more" on the betanet: the form took the concentrated pool for the one getpool reads).
import { describe, expect, it } from 'vitest';
import { isAddLiquidityPool, isNoPoolError, liquidityState, recheck } from './liquidity';
import { CHN_ID, type Pool } from './model';
import { BridgeError } from '../../lib/bridge';

const GOLD = 'a'.repeat(64);
const SILVER = 'b'.repeat(64);

function pool(over: Partial<Pool> = {}): Pool {
  return { pool: 'p'.repeat(64), asset_a: 'GOLD', asset_b: 'ECX', asset_a_id: GOLD, asset_b_id: CHN_ID, reserve_a: 100, reserve_b: 1, shares: 10000, volume_a: 0, volume_b: 0, swaps: 0, created: 1, abandoned: false, ...over };
}
const concentrated = pool({ pool: 'c'.repeat(64), curve: 'concentrated', fee: 0.05, default_kind: false });
const plain = pool({ pool: 'd'.repeat(64), curve: 'constant_product', fee: 0.3, default_kind: true });

describe('the Add liquidity form', () => {
  it('names the default kind only, a node from before the kinds included', () => {
    expect(isAddLiquidityPool(plain)).toBe(true);
    expect(isAddLiquidityPool(pool())).toBe(true);
    expect(isAddLiquidityPool(concentrated)).toBe(false);
    expect(isAddLiquidityPool(pool({ curve: 'constant_product', fee: 1, default_kind: false }))).toBe(false);
    expect(isAddLiquidityPool(pool({ curve: 'launch', default_kind: false }))).toBe(false);
  });

  it('is a new pool (both amounts, they set the price) for a pair with no pool at all', () => {
    const s = liquidityState([], GOLD, CHN_ID);
    expect(s).toMatchObject({ pool: undefined, mode: 'new', setsPrice: true, others: [] });
    expect(liquidityState([plain], SILVER, CHN_ID).mode).toBe('new');
  });

  it('is a new pool for a pair with only a pool of another kind, which it names', () => {
    const s = liquidityState([concentrated], CHN_ID, GOLD);
    expect(s.mode).toBe('new');
    expect(s.setsPrice).toBe(true);
    expect(s.others).toEqual([concentrated]);
    // The order book of a pair is none of the form's, nor a pool to add to.
    expect(liquidityState([pool({ curve: 'book', default_kind: false })], GOLD, CHN_ID).others).toEqual([]);
  });

  it('adds at the price of the pool someone provides to, either way round, and reopens an abandoned one', () => {
    expect(liquidityState([concentrated, plain], CHN_ID, GOLD)).toMatchObject({ pool: plain, mode: 'add', setsPrice: false, others: [concentrated] });
    const left = pool({ abandoned: true });
    expect(liquidityState([left], GOLD, CHN_ID)).toMatchObject({ pool: left, mode: 'reopen', setsPrice: true });
    expect(liquidityState([plain], GOLD, GOLD).mode).toBe('new');
  });

  it('tells getpool\'s "no pool" from any other error', () => {
    expect(isNoPoolError(new BridgeError(-8, 'There is no pool of these assets (addliquidity makes one)'))).toBe(true);
    expect(isNoPoolError({ code: -8, message: 'There is no pool of these assets' })).toBe(true);
    expect(isNoPoolError(new BridgeError(-8, 'Unknown asset: 0xzz'))).toBe(false);
    expect(isNoPoolError(new BridgeError(-1, 'No connection to the node'))).toBe(false);
  });

  it('checks again on submit what the amounts were typed for', () => {
    // A new pool, still none: it is made. The bug: the form showed a pool (the concentrated one), getpool had none.
    expect(recheck({ mode: 'new' }, { none: true })).toEqual({ ok: true });
    expect(recheck({ mode: 'add', poolId: plain.pool }, { pool: plain })).toEqual({ ok: true });
    expect(recheck({ mode: 'reopen', poolId: plain.pool }, { pool: { ...plain, abandoned: true } })).toEqual({ ok: true });
    // A form that showed a pool the node has not: a new pool, not "nobody provides to it any more".
    const none = recheck({ mode: 'add', poolId: concentrated.pool }, { none: true });
    expect(none).toMatchObject({ ok: false, kind: 'none' });
    expect(!none.ok && none.message).toMatch(/no pool of these assets yet/);
    // Left meanwhile, made meanwhile, reopened meanwhile.
    expect(recheck({ mode: 'add', poolId: plain.pool }, { pool: { ...plain, abandoned: true } })).toMatchObject({ ok: false, kind: 'left' });
    expect(recheck({ mode: 'new' }, { pool: plain })).toMatchObject({ ok: false, kind: 'made' });
    expect(recheck({ mode: 'reopen', poolId: plain.pool }, { pool: plain })).toMatchObject({ ok: false, kind: 'reopened' });
    // Another pool than the one shown, and an error, which is said as it is.
    expect(recheck({ mode: 'add', poolId: concentrated.pool }, { pool: plain })).toMatchObject({ ok: false, kind: 'other' });
    const err = recheck({ mode: 'new' }, { error: 'Unknown asset: 0xzz' });
    expect(err).toMatchObject({ ok: false, kind: 'error' });
    expect(!err.ok && err.message).toContain('Unknown asset: 0xzz');
  });
});
