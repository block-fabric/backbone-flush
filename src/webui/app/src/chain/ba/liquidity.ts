// What the Add liquidity form (ba/LiquidityPanel.svelte) works with: the pool addliquidity names (the default kind,
// constant product at 0.3%, which getpool by the pair reads), never a pool of another kind of the same pair (a
// concentrated, stable or weighted one, a launch curve, an order book: those are added to by their id, from their
// page); and what the pool read again on submit means for what was typed.
import type { Pool } from './model';

/** Whether a pool is the kind addliquidity makes and getpool by the pair reads (a node from before the kinds says no kind: it is). */
export function isAddLiquidityPool(p: Pick<Pool, 'default_kind' | 'curve'>): boolean {
  return p.default_kind !== false && (p.curve === undefined || p.curve === 'constant_product');
}

export type LiquidityMode = 'new' | 'reopen' | 'add';

export interface LiquidityState {
  /** The pool addliquidity puts the amounts in, if there is one. */
  pool: Pool | undefined;
  /** None: a new pool, made by the amounts; abandoned: reopened by them; else: the amounts go in at its price. */
  mode: LiquidityMode;
  /** Both amounts are given, and they set the price (new or reopen). */
  setsPrice: boolean;
  /** The pair's other pools someone provides liquidity to (of other kinds): not this form's. */
  others: Pool[];
}

/** The form's state for two assets, from the pools read (any order, any kind). */
export function liquidityState(pools: readonly Pool[], a: string, b: string): LiquidityState {
  const pair = a && b && a !== b ? pools.filter((p) => (p.asset_a_id === a && p.asset_b_id === b) || (p.asset_a_id === b && p.asset_b_id === a)) : [];
  const pool = pair.find(isAddLiquidityPool);
  const others = pair.filter((p) => p !== pool && !p.abandoned && p.curve !== 'book');
  const mode: LiquidityMode = !pool ? 'new' : pool.abandoned ? 'reopen' : 'add';
  return { pool, mode, setsPrice: mode !== 'add', others };
}

/** getpool's answer for a pair without a pool (src/rpc/bitassets.cpp getpool): the one error that means "none". */
export function isNoPoolError(e: unknown): boolean {
  const message = e instanceof Error ? e.message : e && typeof e === 'object' && 'message' in e ? String((e as { message: unknown }).message) : String(e);
  return /^There is no pool of these assets/.test(message);
}

/** The pool read again on submit: the pool, none, or the error that kept it from being read. */
export type Reread = { pool: Pool } | { none: true } | { error: string };

export type Recheck =
  | { ok: true }
  /** Nothing is sent: the message says why (what changed, or the error), the form is read again. */
  | { ok: false; kind: 'error' | 'left' | 'made' | 'none' | 'reopened' | 'other'; message: string };

/**
 * Whether what was typed (for the mode shown, and that pool) still holds for the pool as read again: the amounts set
 * the price (new or abandoned) or go in at the pool's (someone provides to it). An error reading it is no "no pool".
 */
export function recheck(typed: { mode: LiquidityMode; poolId?: string }, now: Reread): Recheck {
  if ('error' in now) return { ok: false, kind: 'error', message: `The pool could not be read again: ${now.error}` };
  const setsNow = 'none' in now || now.pool.abandoned;
  const setsTyped = typed.mode !== 'add';
  if ('pool' in now && typed.poolId && now.pool.pool !== typed.poolId) return { ok: false, kind: 'other', message: 'The node answers for another pool than the form shows. Check the assets and the amounts.' };
  if (setsNow === setsTyped) return { ok: true };
  if (setsTyped) {
    return typed.mode === 'new'
      ? { ok: false, kind: 'made', message: 'A pool of these assets was made meanwhile: its price sets the second amount now. Check the amounts.' }
      : { ok: false, kind: 'reopened', message: 'Someone reopened this pool meanwhile: its price sets the second amount now. Check the amounts.' };
  }
  // The form showed a pool someone provides to: the node has none (it is a new pool), or it was left since.
  if ('none' in now) return { ok: false, kind: 'none', message: 'There is no pool of these assets yet: this makes a new one, at the price your amounts set. Give both amounts.' };
  return { ok: false, kind: 'left', message: 'Nobody provides liquidity to this pool any more since you typed the amounts: give both amounts, which set its price.' };
}
