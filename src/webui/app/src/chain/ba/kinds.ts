// The DEX's pool kinds as the pages see them (src/rpc/bitassets.cpp: KindResults, getdexinfo, getswapquote, quoteroute,
// previewpool, getpoolcurve; src/wallet/rpc/bitassets.cpp: createpool, addpoolliquidity, removepoolliquidity, swappools,
// swaproute): a pool's curve (constant product, StableSwap, weighted), its fee tier, its amplification or its weights;
// the trades over every pool of a pair and the routes through several, and how the wallet sends the one chosen; the
// depth of a pool (what trades of growing size do to its price), for its chart and its table of slippage.
// Pure: no state, no calls (tested in chain/ba/kinds.test.ts).
import { approx, assetArg, isChn, unitsOf, type Amount } from './model';

export type Curve = 'constant_product' | 'stable' | 'weighted';
export const CURVES: readonly Curve[] = ['constant_product', 'stable', 'weighted'];

/** The fee of the kind addliquidity makes (constant product), in percent; and that of a stable pool by default. */
export const DEFAULT_FEE = 0.3;
export const STABLE_FEE = 0.05;
/** A stable pool's amplification by default (createpool). */
export const DEFAULT_AMP = 100;
/** The bounds a node from before getdexinfo said nothing of (bitassets/curves.h). */
export const FEE_TIERS = [0.01, 0.05, 0.3, 1];
export const MIN_AMP = 1;
export const MAX_AMP = 10_000;
export const WEIGHTS = [2, 5, 10, 20, 25, 40, 60, 75, 80, 90, 95, 98];

/** The kind of a pool, as KindResults gives it (its asset_a first). */
export interface PoolKind {
  curve: Curve;
  /** In percent of what goes in: 0.01, 0.05, 0.3 or 1. */
  fee: number;
  fee_millionths?: number;
  /** A stable pool's amplification A. */
  amp?: number;
  /** A weighted pool's weights, in percent: of asset_a, of asset_b. */
  weight_a?: number;
  weight_b?: number;
  /** Whether it is the kind addliquidity makes (constant product, 0.3%): the pool swapasset, swaproute's path and addliquidity name. */
  default_kind: boolean;
}

/** What getdexinfo says of the pool kinds (and of the DEX). */
export interface DexInfo {
  dex_height?: number;
  active?: boolean;
  max_route_hops?: number;
  /** The first block of the pool kinds (-1: never on this network), and whether the next block has them. */
  curves_height: number;
  curves_active: boolean;
  fee_tiers: number[];
  curves: string[];
  min_amp: number;
  max_amp: number;
  weights: number[];
  protocol_fee_share: number;
}

export function isCurve(value: unknown): value is Curve {
  return value === 'constant_product' || value === 'stable' || value === 'weighted';
}

/**
 * The kind of a pool as the node gives it. A node from before the pool kinds gives none: its pools are all of the kind
 * addliquidity makes, constant product at 0.3%.
 */
export function kindOf(p: { curve?: unknown; fee?: unknown; fee_millionths?: unknown; amp?: unknown; weight_a?: unknown; weight_b?: unknown; default_kind?: unknown }): PoolKind {
  const curve: Curve = isCurve(p.curve) ? p.curve : 'constant_product';
  const fee = typeof p.fee === 'number' || typeof p.fee === 'string' ? approx(p.fee) : DEFAULT_FEE;
  const out: PoolKind = { curve, fee, default_kind: typeof p.default_kind === 'boolean' ? p.default_kind : curve === 'constant_product' && fee === DEFAULT_FEE };
  if (typeof p.fee_millionths === 'number') out.fee_millionths = p.fee_millionths;
  if (curve === 'stable' && (typeof p.amp === 'number' || typeof p.amp === 'string')) out.amp = approx(p.amp);
  if (curve === 'weighted' && (typeof p.weight_a === 'number' || typeof p.weight_a === 'string')) {
    out.weight_a = approx(p.weight_a);
    out.weight_b = typeof p.weight_b === 'number' || typeof p.weight_b === 'string' ? approx(p.weight_b) : 100 - out.weight_a;
  }
  return out;
}

/** The kind of a pool seen the other way round (its asset_b first): a weighted pool's weights swapped. */
export function flipKind(k: PoolKind): PoolKind {
  if (k.curve !== 'weighted' || k.weight_a === undefined) return k;
  return { ...k, weight_a: k.weight_b ?? 100 - k.weight_a, weight_b: k.weight_a };
}

/** The words of a curve (shown with t()). */
export const CURVE_NAMES: Record<Curve, string> = { constant_product: 'Constant product', stable: 'Stable', weighted: 'Weighted' };

/** When to choose each curve, in a line (shown with t()). */
export const CURVE_HELP: Record<Curve, string> = {
  constant_product: 'For two assets whose prices move freely against each other: liquidity at every price. The usual kind, and the one Add liquidity makes (at 0.3%).',
  stable: 'For two assets meant to be worth about the same (two stablecoins, a token and its wrapped twin): far less slippage near the balance; A sets how flat it is there.',
  weighted: 'For a pool that holds more of one asset than of the other (such as 80/20): less exposure to the price of the lighter one, and more slippage.',
};

/** A fee in percent, as the node writes it: "0.01%", "0.05%", "0.3%", "1%". */
export function feePercent(fee: number): string {
  if (!Number.isFinite(fee) || fee < 0) return '–';
  return `${Number(fee.toFixed(4))}%`;
}

/** The fee a curve starts with in the wizard: 0.05% for a stable pool, 0.3% for the others (createpool's defaults), among the tiers. */
export function defaultFee(curve: Curve, tiers: readonly number[] = FEE_TIERS): number {
  const want = curve === 'stable' ? STABLE_FEE : DEFAULT_FEE;
  if (tiers.length === 0 || tiers.includes(want)) return want;
  return tiers.reduce((best, x) => (Math.abs(x - want) < Math.abs(best - want) ? x : best), tiers[0]);
}

/** An amplification typed, as createpool takes it: a whole number within the bounds (the default when it is not a number). */
export function clampAmp(value: number, min = MIN_AMP, max = MAX_AMP): number {
  if (!Number.isFinite(value)) return Math.min(max, Math.max(min, DEFAULT_AMP));
  return Math.min(max, Math.max(min, Math.round(value)));
}

/** The kind of a pool to make, as createpool and previewpool take it: the curve, the fee, and A or asset_a's weight. */
export function kindParam(curve: Curve, fee: number, amp: number, weightA: number): Record<string, string | number> {
  const out: Record<string, string | number> = { curve, fee };
  if (curve === 'stable') out.amp = amp;
  if (curve === 'weighted') out.weight_a = weightA;
  return out;
}

/** Whether a kind is the one addliquidity makes. */
export function isDefaultKind(curve: Curve, fee: number): boolean {
  return curve === 'constant_product' && fee === DEFAULT_FEE;
}

// --- Trades over every pool of a pair, and routes ---

/** A trade in one pool of the pair (getswapquote's pools, TradeResults). */
export interface TradeQuote extends PoolKind {
  pool: string;
  amount_in: Amount;
  amount_out: Amount;
  price: number;
  spot_price: number;
  price_impact: number;
  fee_paid: Amount;
  spot_price_after?: number;
  abandoned: boolean;
}

/** What getswapquote answers. */
export interface MarketQuote {
  pools: TradeQuote[];
  best_pool?: string;
  route?: { path: string[]; pools: string[]; amount_out: Amount; default_pools: boolean };
}

export interface RouteHop {
  pool: string;
  curve: string;
  fee: number;
  amount_out: Amount;
}

export interface Route {
  /** Labels (text of the chain), and the ids of the assets (the null hash for CHN). */
  path: string[];
  path_ids: string[];
  amount_out: Amount;
  pools: string[];
  default_pools: boolean;
  hops: RouteHop[];
}

/** What quoteroute answers. */
export interface RouteQuote {
  amount_in: Amount;
  amount_out: Amount;
  path: string[];
  pools: string[];
  routes: Route[];
}

/** A hop of a trade: from an asset to another through a pool of a kind. */
export interface OptionHop {
  pool: string;
  from: string;
  to: string;
  kind: PoolKind;
  /** What comes out of it, in units of `to` (when the node said). */
  out?: Amount;
}

/** A way to make a trade: one pool of the pair, or a route through several pools. */
export interface TradeOption {
  /** The pools, first to last: the option's key. */
  key: string;
  pools: string[];
  /** The assets, first to last (ids). */
  path: string[];
  /** What comes out, in units of the last asset. */
  out: bigint;
  /** The price impact in percent, for one pool (the node does not say it of a route). */
  impact?: number;
  /** Whether every pool is of the kind addliquidity makes. */
  defaultPools: boolean;
  hops: OptionHop[];
  /** The pool's fee kept of what goes in (one pool). */
  feePaid?: Amount;
  spotPrice?: number;
  price?: number;
}

/**
 * The ways to make a trade, best first (most out; at the same, the pools of the default kind, then the fewer hops): each
 * pool of the pair getswapquote quoted, the routes of quoteroute through several pools; without getswapquote (a node
 * from before the pool kinds), the default pool's quote of quoteswap.
 */
export function tradeOptions(args: {
  pay: string;
  get: string;
  getDecimals: number;
  /** quoteswap's quote of the default pool, and its id. */
  single?: { pool: string; amount_out: Amount; price_impact: number; fee: Amount; price?: number; spot_price?: number } | null;
  market?: MarketQuote | null;
  routes?: RouteQuote | null;
  /** The kind of a pool by id, its asset `from` first, where the node's answer did not say it (a route's hop: its curve and fee only). */
  kindById?: (pool: string, from: string) => PoolKind | undefined;
}): TradeOption[] {
  const { pay, get, getDecimals } = args;
  const out: TradeOption[] = [];
  const seen = new Set<string>();
  const add = (o: TradeOption) => {
    if (seen.has(o.key) || o.out <= 0n) return;
    seen.add(o.key);
    out.push(o);
  };
  for (const q of args.market?.pools ?? []) {
    if (q.abandoned) continue;
    const kind = kindOf(q);
    add({ key: q.pool, pools: [q.pool], path: [pay, get], out: unitsOf(q.amount_out, getDecimals), impact: approx(q.price_impact), defaultPools: kind.default_kind, hops: [{ pool: q.pool, from: pay, to: get, kind, out: q.amount_out }], feePaid: q.fee_paid, spotPrice: approx(q.spot_price), price: approx(q.price) });
  }
  if (!args.market && args.single) {
    const s = args.single;
    add({ key: s.pool, pools: [s.pool], path: [pay, get], out: unitsOf(s.amount_out, getDecimals), impact: approx(s.price_impact), defaultPools: true, hops: [{ pool: s.pool, from: pay, to: get, kind: kindOf({}), out: s.amount_out }], feePaid: s.fee, spotPrice: s.spot_price, price: s.price });
  }
  for (const r of args.routes?.routes ?? []) {
    if (r.pools.length === 0 || r.path_ids.length !== r.pools.length + 1) continue;
    // One pool: getswapquote's quote of it says more (its price impact); a node without getswapquote, the route's.
    if (r.pools.length === 1 && args.market) continue;
    const hops = r.pools.map((pool, i) => {
      const known = args.kindById?.(pool, r.path_ids[i]);
      const hop = r.hops[i];
      const kind = known ?? kindOf({ curve: hop?.curve, fee: hop?.fee });
      return { pool, from: r.path_ids[i], to: r.path_ids[i + 1], kind, out: hop?.amount_out };
    });
    add({ key: r.pools.join(','), pools: [...r.pools], path: [...r.path_ids], out: unitsOf(r.amount_out, getDecimals), defaultPools: r.default_pools, hops });
  }
  return out.sort((x, y) => (x.out === y.out ? Number(y.defaultPools) - Number(x.defaultPools) || x.pools.length - y.pools.length : x.out > y.out ? -1 : 1));
}

/** The option a trade uses: the one picked, if it is still offered; else the best. */
export function chooseOption(options: readonly TradeOption[], picked: string): TradeOption | null {
  return options.find((o) => o.key === picked) ?? options[0] ?? null;
}

/** How the wallet sends a trade: the method and its parameters. */
export interface Execution {
  method: 'swapasset' | 'swappools' | 'swaproute';
  params: unknown[];
}

/**
 * How a trade chosen is sent: one hop in the pool of the default kind by swapasset, as before the pool kinds (with its
 * bound of the price impact, max_impact); one pool of another kind by swappools; several pools by swaproute, which names
 * its pools unless they are all of the default kind (its path alone names them then). Assets by id, pools by id: the
 * host shows each resolved (src/webui/local.cpp WithAssets).
 */
export function executionOf(option: Pick<TradeOption, 'pools' | 'path' | 'defaultPools'>, a: { amount: string; slippage: number; minOut: string; maxImpact: number }): Execution {
  const path = option.path.map(assetArg);
  if (option.pools.length === 1 && option.defaultPools) return { method: 'swapasset', params: [path[0], a.amount, path[1], a.slippage, a.maxImpact, a.minOut] };
  if (option.pools.length === 1) return { method: 'swappools', params: [path[0], a.amount, [...option.pools], a.slippage, a.minOut] };
  return { method: 'swaproute', params: option.defaultPools ? [path, a.amount, a.slippage, a.minOut] : [path, a.amount, a.slippage, a.minOut, [...option.pools]] };
}

// --- Depth: what trades of growing size do ---

/** A trade of a depth chart (DEPTH_RESULT): its side (sell_a: asset_a in), its size, what it gives, its prices. */
export interface DepthPoint {
  side: 'sell_a' | 'sell_b';
  /** What goes in, in percent of the pool's reserve of it. */
  size_percent: number;
  amount_in: Amount;
  amount_out: Amount;
  /** The trade's price, and the pool's after it: of a whole asset_a in asset_b. */
  avg_price: number;
  price_after: number;
  price_impact: number;
}

/** What getpoolcurve answers: the pool's own order (its lower asset first: CHN first in a pool with CHN). */
export interface PoolCurve extends PoolKind {
  pool: string;
  asset_a: string;
  asset_b: string;
  price: number;
  depth: DepthPoint[];
}

/** What previewpool answers, in the order its assets were given. */
export interface PoolPreview extends PoolKind {
  pool: string;
  exists: boolean;
  open: boolean;
  shares: Amount;
  too_small?: string;
  price: number;
  depth: DepthPoint[];
}

/** A depth seen the other way round: the sides swapped, the prices inverted. */
export function flipDepth(depth: readonly DepthPoint[]): DepthPoint[] {
  const inv = (x: number) => (x > 0 ? 1 / x : 0);
  return depth.map((d) => ({ ...d, side: d.side === 'sell_a' ? 'sell_b' : 'sell_a', avg_price: inv(approx(d.avg_price)), price_after: inv(approx(d.price_after)) }));
}

/**
 * getpoolcurve's depth in the order of a pool as the lists give it (listpools, getpool: the asset that is not CHN first):
 * the same when its first asset is the list's, flipped when it is the list's second.
 */
export function alignDepth(curve: Pick<PoolCurve, 'asset_a' | 'asset_b' | 'depth'>, pool: { asset_a: string; asset_b: string; asset_a_id: string; asset_b_id: string }): DepthPoint[] {
  if (pool.asset_a !== pool.asset_b) {
    if (curve.asset_a === pool.asset_a && curve.asset_b === pool.asset_b) return curve.depth;
    if (curve.asset_a === pool.asset_b && curve.asset_b === pool.asset_a) return flipDepth(curve.depth);
  }
  // Labels alike: the node puts CHN, the null hash, first; the lists put it second.
  return isChn(pool.asset_b_id) ? flipDepth(curve.depth) : curve.depth;
}

export interface ChartPoint {
  x: number;
  y: number;
}

/** The lines of a depth chart: for each side, the price impact (y, percent) by the size of the trade (x, percent of the reserve put in). */
export function depthSeries(depth: readonly DepthPoint[]): { a: ChartPoint[]; b: ChartPoint[] } {
  const line = (side: DepthPoint['side']) =>
    depth
      .filter((d) => d.side === side)
      .map((d) => ({ x: approx(d.size_percent), y: approx(d.price_impact) }))
      .filter((p) => p.x > 0 && Number.isFinite(p.x) && Number.isFinite(p.y))
      .sort((p, q) => p.x - q.x);
  return { a: line('sell_a'), b: line('sell_b') };
}

/** The rows of a table of slippage: each size, the trade selling asset_a and the one selling asset_b. */
export function slippageRows(depth: readonly DepthPoint[]): { size: number; a?: DepthPoint; b?: DepthPoint }[] {
  const rows = new Map<number, { size: number; a?: DepthPoint; b?: DepthPoint }>();
  for (const d of depth) {
    const size = approx(d.size_percent);
    const row = rows.get(size) ?? { size };
    if (d.side === 'sell_a') row.a = d;
    else row.b = d;
    rows.set(size, row);
  }
  return [...rows.values()].sort((x, y) => x.size - y.size);
}

// --- Liquidity by pool id ---

/** The amounts of addpoolliquidity: by asset ("CHN" or "0x" and its id), those above zero, as exact text. */
export function depositAmounts(entries: readonly { id: string; units: bigint | null; text: (units: bigint) => string }[]): Record<string, string> {
  const out: Record<string, string> = {};
  for (const e of entries) if (e.units !== null && e.units > 0n) out[assetArg(e.id)] = e.text(e.units);
  return out;
}

/**
 * About the shares a deposit gives a pool someone provides to (no quote of the node: addpoolliquidity says it once sent):
 * its worth at the pool's price over the pool's, less the fee on the part out of the pool's proportions, which a swap to
 * them would trade. `price`: what one unit of A is worth in units of B. Exact for a deposit in proportion to a
 * constant-product pool; near for the other kinds.
 */
export function estimateDepositShares(pool: { reserveA: bigint; reserveB: bigint; shares: bigint; price: number; fee: number }, amountA: bigint, amountB: bigint): bigint {
  if (pool.shares <= 0n || !(pool.price > 0) || (amountA <= 0n && amountB <= 0n)) return 0n;
  const worthA = Number(amountA) * pool.price;
  const worth = worthA + Number(amountB);
  const total = Number(pool.reserveA) * pool.price + Number(pool.reserveB);
  if (!(total > 0)) return 0n;
  // The part of A in the pool's worth, and how much of the deposit's worth is out of it.
  const off = Math.abs(worthA - (worth * Number(pool.reserveA) * pool.price) / total);
  const kept = worth - (off * pool.fee) / 100;
  const shares = Math.floor((Number(pool.shares) * Math.max(0, kept)) / total);
  return Number.isFinite(shares) && shares > 0 ? BigInt(shares) : 0n;
}

/** The shares a percentage of those held is, as removepoolliquidity works it out (all of them at 100). */
export function sharesOf(held: bigint, percent: number): bigint {
  if (percent >= 100) return held;
  if (!(percent > 0)) return 0n;
  return (held * BigInt(Math.round(percent * 100))) / 10000n;
}
