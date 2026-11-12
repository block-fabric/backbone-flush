// A pool as the app shows it since 2026-11-11 (owner decision: one kind of pool is shown to the user), whatever its
// kind on the chain: a pool the user makes is a standard (constant-product) pool against ECX at a fee tier, made and
// funded in one transaction (createpool); a pool that came from a graduated launch is concentrated on the chain, shown
// as any other, and liquidity added to it always covers the full range (openposition with prices of 0); never a price
// range to pick. And the rules of the one pool form (ba/AddLiquidity.svelte) as consensus has them
// (src/bitassets/state.cpp): an asset on its launch curve takes no pool and no liquidity until it graduates
// (bad-ba-launch-pooled), so the form is refused there with the reason said, before any confirmation.
// Pure: no state, no calls (tested in chain/ba/pools.test.ts).
import { MAX_TICK, MIN_TICK, type Position } from './cl';
import { feeNow, feePercent, kindOf, type OptionHop } from './kinds';
import { approx, isChn, type Pool } from './model';
import { TICKER } from '../config';

/** Whether a pool is one the pages list as a pool: not an order book, not a launch's curve. */
export function isShownPool(p: Pick<Pool, 'curve'>): boolean {
  return p.curve !== 'book' && p.curve !== 'launch';
}

/** Whether a pool has nothing in it to trade: nobody provides to it (abandoned), or a concentrated one holds nothing. */
export function poolEmpty(p: Pick<Pool, 'abandoned' | 'curve' | 'reserve_a' | 'reserve_b'>): boolean {
  if (p.curve === 'concentrated') return !(approx(p.reserve_a) > 0) && !(approx(p.reserve_b) > 0);
  return p.abandoned;
}

/** A pool seen from its asset: the side that is not ECX first (a pair without ECX: its asset_a), what it is quoted in second. */
export function poolSides(p: Pick<Pool, 'asset_a' | 'asset_b' | 'asset_a_id' | 'asset_b_id' | 'reserve_a' | 'reserve_b' | 'price'>): { base: { id: string; label: string }; quote: { id: string; label: string }; flip: boolean; baseReserve: Pool['reserve_a']; quoteReserve: Pool['reserve_a']; price: number | undefined } {
  const flip = isChn(p.asset_a_id);
  const a = { id: p.asset_a_id, label: p.asset_a };
  const b = { id: p.asset_b_id, label: p.asset_b };
  const price = p.price ? (flip ? 1 / p.price : p.price) : undefined;
  return flip ? { base: b, quote: a, flip, baseReserve: p.reserve_b, quoteReserve: p.reserve_a, price } : { base: a, quote: b, flip, baseReserve: p.reserve_a, quoteReserve: p.reserve_b, price };
}

/** The ECX a pool holds (undefined: a pair without ECX). */
export function poolEcx(p: Pick<Pool, 'asset_a_id' | 'asset_b_id' | 'reserve_a' | 'reserve_b'>): number | undefined {
  return isChn(p.asset_a_id) ? approx(p.reserve_a) : isChn(p.asset_b_id) ? approx(p.reserve_b) : undefined;
}

/**
 * The pools of an asset with ECX that the pages show (books and launch curves left out), the one with liquidity and the
 * most ECX first, the empty ones last: the first is the asset's pool for its page and its one pool form.
 */
export function ecxPoolsOf(asset: string, pools: readonly Pool[]): Pool[] {
  return pools
    .filter((p) => isShownPool(p) && ((p.asset_a_id === asset && isChn(p.asset_b_id)) || (p.asset_b_id === asset && isChn(p.asset_a_id))))
    .sort((x, y) => Number(poolEmpty(x)) - Number(poolEmpty(y)) || (poolEcx(y) ?? 0) - (poolEcx(x) ?? 0) || x.created - y.created);
}

/** Where an asset trades now (owner decision, 2026-11-11: the Market chip of Assets and of an asset's page). */
export type MarketState = 'curve' | 'pool' | 'pool-empty' | 'none';

/**
 * The market of an asset from its pools (listpools by asset, or those read) and its launch when known: on its launch
 * curve until it graduates (then nothing else trades it); else in a pool that holds liquidity; else a pool with nothing
 * in it ("Pool, empty": its Trade box gives way to Add liquidity); else none. `pool`: its pool with ECX (the first of
 * ecxPoolsOf), else any pool shown; `curvePool`: the id of its curve's pool, for its candles.
 */
export function marketState(asset: string, pools: readonly Pool[], launch?: { status: string; pool?: string } | null): { state: MarketState; pool: Pool | null; curvePool: string } {
  const curve = pools.find((p) => p.curve === 'launch' && (p.asset_a_id === asset || p.asset_b_id === asset));
  if (launch ? launch.status !== 'graduated' : !!curve) return { state: 'curve', pool: null, curvePool: curve?.pool ?? launch?.pool ?? '' };
  const own = pools.filter((p) => isShownPool(p) && (p.asset_a_id === asset || p.asset_b_id === asset));
  const pool = ecxPoolsOf(asset, pools)[0] ?? own.find((p) => !poolEmpty(p)) ?? own[0] ?? null;
  if (!pool) return { state: 'none', pool: null, curvePool: '' };
  return { state: own.some((p) => !poolEmpty(p)) ? 'pool' : 'pool-empty', pool, curvePool: '' };
}

/** Whether a position of concentrated liquidity covers the full range of its pool (its lowest and highest ticks to the spacing). */
export function isFullRange(p: Pick<Position, 'tick_lower' | 'tick_upper'>, spacing: number): boolean {
  const s = spacing > 0 ? spacing : 1;
  return p.tick_lower <= Math.ceil(MIN_TICK / s) * s && p.tick_upper >= Math.floor(MAX_TICK / s) * s;
}

/**
 * How the one pool form puts liquidity in, by the asset's state (owner decision, 2026-11-11: a form only offers what the
 * state allows, and says why otherwise):
 *  - 'create': no pool with ECX yet: a standard pool at a fee tier, made and funded in one transaction (createpool);
 *    both amounts set the price.
 *  - 'reopen': its standard pool is empty (everyone left it): both amounts go in and set the price again (createpool
 *    of the same kind and fee).
 *  - 'add': its pool has liquidity: at the pool's price (addpoolliquidity by the pool's id; addliquidity on a node from
 *    before the pool kinds).
 *  - 'full-range': its pool is concentrated on the chain (a graduated launch's, or one made empty before 2026-11-11): a
 *    position over the full range (openposition with prices of 0), at the pool's price.
 */
export type PoolFormMode = 'create' | 'reopen' | 'add' | 'full-range';

export interface PoolFormState {
  ok: boolean;
  /** Why not (shown with t()), '' when ok. */
  reason: string;
  mode: PoolFormMode | null;
  pool: Pool | null;
}

export const CURVE_REFUSAL = `This asset is on its launch curve: its pool opens by itself at graduation. Until then the chain takes no pool and no liquidity for it.`;

export function poolFormState(o: {
  /** The asset the form is fixed on. */
  asset: string;
  /** A wallet is loaded. */
  wallet: boolean;
  /** The asset is on its launch curve, not graduated (getlaunch, or a pool of the kind "launch"). */
  onCurve: boolean;
  /** The pools read of the asset (listpools by asset), or the one pool of a pool's page. */
  pools: readonly Pool[];
  /** The pool kinds apply (getdexinfo curves_active): createpool, addpoolliquidity, openposition. */
  curvesActive: boolean;
}): PoolFormState {
  const no = (reason: string): PoolFormState => ({ ok: false, reason, mode: null, pool: null });
  if (isChn(o.asset)) return no(`${TICKER} is the other side of every pool: pick the asset.`);
  // The rule of consensus first: nothing else matters while it holds.
  if (o.onCurve || o.pools.some((p) => p.curve === 'launch' && (p.asset_a_id === o.asset || p.asset_b_id === o.asset))) return no(CURVE_REFUSAL);
  if (!o.wallet) return no('Load a wallet to add liquidity.');
  const pool = ecxPoolsOf(o.asset, o.pools)[0] ?? null;
  if (!pool) return { ok: true, reason: '', mode: 'create', pool: null };
  const kind = kindOf(pool);
  if (kind.curve === 'concentrated') return o.curvesActive ? { ok: true, reason: '', mode: 'full-range', pool } : no('This node does not have the pool kinds yet.');
  if (poolEmpty(pool)) return { ok: true, reason: '', mode: 'reopen', pool };
  return { ok: true, reason: '', mode: 'add', pool };
}

// --- A thin pool's price is someone else's (security reviews 2026-11-11) ---
//
// The price of a pool that holds little was set by whoever made or last moved it, who may be a stranger: anyone can
// open a pool of an asset at an absurd price (empty, or with dust in it), and whoever then puts real liquidity in at
// that price hands the difference to the next trader. So the one pool form never funds such a pool silently
// (priceGuard): it shows the pool's price as the price the user accepts, compares it with a reference that a stranger
// cannot move cheaply, and takes an explicit tick when they differ, or when there is no such reference. Nothing is
// ever valued at the pool's own price for this (third review): that price is the one in doubt.

/** The most a thin pool's price may differ from the reference, in percent, before the form asks for an explicit word: about what a day's trading moves a price, far below what makes the attack pay. */
export const EMPTY_POOL_TOLERANCE = 3;
/**
 * A pool that holds less ECX than this is thin whatever goes in, even at a price that agrees with the reference: dust
 * costs an attacker nothing, and a price backed by less than a whole ECX is nobody's market.
 */
export const THIN_POOL_ECX = 1;
/**
 * The least ECX another pool must hold for its price to count as a reference: enough that moving it costs more than the
 * attack is likely to win, so that a second dust pool at the same absurd price is not taken for a market. Also the
 * least a pool must hold to vouch for its own price when nothing else does.
 */
export const REFERENCE_MIN_ECX = 10;

/**
 * The reference the price of a pool that is not deep is held against (priceGuard): the price the asset's launch
 * graduated at, which is the curve's and nobody's to move (given by the page from the launch), before the deepest other
 * pool of the asset with ECX that holds at least REFERENCE_MIN_ECX. The pool itself, thin pools and pools without ECX
 * never count. null: none.
 */
export function referencePrice(asset: string, target: string, pools: readonly Pool[], launchPrice?: number): number | null {
  if (launchPrice !== undefined && Number.isFinite(launchPrice) && launchPrice > 0) return launchPrice;
  for (const p of ecxPoolsOf(asset, pools)) {
    if (p.pool === target || poolEmpty(p) || (poolEcx(p) ?? 0) < REFERENCE_MIN_ECX) continue;
    const price = poolSides(p).price;
    if (price !== undefined && Number.isFinite(price) && price > 0) return price;
  }
  return null;
}

export interface PriceGuard {
  /** The pool's price, of the asset in ECX. */
  price: number | undefined;
  reference: number | null;
  /** Of the pool's price from the reference, in percent, signed; null without a reference. */
  deviation: number | null;
  /** The deposit would be most of the pool (it is empty, holds little, or less than what goes in). */
  thin: boolean;
  /** The explicit word is needed: no price, no reference, or further from it than EMPTY_POOL_TOLERANCE. */
  acknowledge: boolean;
}

/** A price held against a reference: how far it is, and whether that is beyond the tolerance (no reference: always). */
export function emptyPoolCheck(price: number | undefined, references: readonly (number | null | undefined)[]): Omit<PriceGuard, 'thin'> {
  const reference = references.find((r): r is number => r !== undefined && r !== null && Number.isFinite(r) && r > 0) ?? null;
  if (!(price !== undefined && price > 0) || reference === null) return { price, reference, deviation: null, acknowledge: true };
  const deviation = (price / reference - 1) * 100;
  return { price, reference, deviation, acknowledge: Math.abs(deviation) > EMPTY_POOL_TOLERANCE };
}

/**
 * The price of the deepest other pool of the asset with ECX that is a reference (at least REFERENCE_MIN_ECX, not empty)
 * and holds more ECX than `held`: what may contradict a deep pool's own price. null: none.
 */
export function deeperReference(asset: string, target: string, pools: readonly Pool[], held: number): number | null {
  for (const p of ecxPoolsOf(asset, pools)) {
    const ecx = poolEcx(p) ?? 0;
    if (p.pool === target || poolEmpty(p) || ecx < REFERENCE_MIN_ECX || !(ecx > held)) continue;
    const price = poolSides(p).price;
    if (price !== undefined && Number.isFinite(price) && price > 0) return price;
  }
  return null;
}

/**
 * The guard of a deposit into a pool that exists, in one place (ba/AddLiquidity.svelte asks it as the user types, and
 * again on the node's fresh lists right before the wallet is asked). null: nothing to say, the pool's price is a
 * market's. Else what the form shows, and whether it needs the user's explicit word:
 *  - A deep pool (its ECX at least REFERENCE_MIN_ECX, what would make it a reference for another pool, and more than
 *    the ECX going in) vouches for its own price: a market moves away from the price its launch graduated at, and an
 *    honest add to it is not asked about (owner decision, 2026-11-11). Unless a deeper reference pool of the same pair
 *    (deeperReference) disagrees beyond EMPTY_POOL_TOLERANCE: then the word is needed, against that pool's price.
 *  - A pool that is not deep is held against the reference (referencePrice: the graduation price first, else the
 *    deepest other pool that counts): beyond EMPTY_POOL_TOLERANCE the word is needed. Within it, the price is only
 *    shown, and only when the deposit would be most of the pool; the deposit is valued at the reference, never at the
 *    pool's own price (an attacker who sets the pool's price near zero would otherwise make a real deposit look smaller
 *    than the dust in it).
 *  - Not deep and without a reference, nothing vouches for the price: the word is needed. The asset side of the
 *    deposit is not valued at all then.
 * Nothing is ever valued at the price of a thin pool, and dust pools are no reference, deeper or not.
 * depositAsset: whole units of the asset going in; depositEcx: the ECX going in, alone.
 */
export function priceGuard(o: { asset: string; pool: Pool; pools: readonly Pool[]; depositAsset: number; depositEcx: number; launchPrice?: number }): PriceGuard | null {
  const price = poolSides(o.pool).price;
  const held = poolEmpty(o.pool) ? 0 : (poolEcx(o.pool) ?? 0);
  const ecxIn = Number.isFinite(o.depositEcx) && o.depositEcx > 0 ? o.depositEcx : 0;
  const assetIn = Number.isFinite(o.depositAsset) && o.depositAsset > 0 ? o.depositAsset : 0;
  if (held >= REFERENCE_MIN_ECX && held > ecxIn && price !== undefined && price > 0) {
    const deeper = deeperReference(o.asset, o.pool.pool, o.pools, held);
    if (deeper === null) return null;
    const against = emptyPoolCheck(price, [deeper]);
    return against.acknowledge ? { ...against, thin: false } : null;
  }
  const check = emptyPoolCheck(price, [referencePrice(o.asset, o.pool.pool, o.pools, o.launchPrice)]);
  if (check.reference === null) return { ...check, thin: true };
  const thin = held < THIN_POOL_ECX || held < ecxIn + assetIn * check.reference;
  return check.acknowledge || thin ? { ...check, thin } : null;
}

/** What a pool is said to be on the pages: its fee alone ("0.3% fee"); never its curve. */
export function poolFeeText(p: Parameters<typeof kindOf>[0]): string {
  const k = kindOf(p);
  const fee = k.dynamic_fee && k.fee_now !== undefined ? k.fee_now : k.fee;
  return `${Number(fee.toFixed(4))}%`;
}

/**
 * A step of a trade's route as the pages say it (never a pool's curve): a launch's bonding curve, an order book (retired
 * from the interface, a venue of the node's routes all the same) or a pool, with its asset (the side that is not ECX),
 * what it is quoted in, and its fee. The words are the page's (ba/SwapPanel.svelte hopWords).
 */
export function hopParts(h: Pick<OptionHop, 'from' | 'to' | 'kind'>): { venue: 'curve' | 'book' | 'pool'; base: string; quote: string; fee: string } {
  const base = isChn(h.from) ? h.to : h.from;
  const quote = isChn(h.from) ? h.from : h.to;
  return { venue: h.kind.curve === 'launch' ? 'curve' : h.kind.curve === 'book' ? 'book' : 'pool', base, quote, fee: feePercent(feeNow(h.kind)) };
}
