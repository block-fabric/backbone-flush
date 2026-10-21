// Concentrated liquidity as the pages see it (doc/layers.md, "Concentrated liquidity"; src/rpc/bitassets.cpp getpool's
// `concentrated`, getpoolticks, previewposition, getposition; src/wallet/rpc/bitassets.cpp openposition, changeposition,
// listpositions): a pool whose liquidity comes by positions, each between two prices (ticks: powers of 1.0001, multiples
// of the pool's spacing). The node speaks of prices of the pool's own first asset (asset0, the lower id: CHN in a pool
// with CHN) in its second; the lists put a pool's other asset first (listpools, getpool: asset_a), so the pages show
// prices of asset_a in asset_b and turn them over at the node's door. The ranges' presets, the ticks, the bars of the
// liquidity distribution and the scale of its chart. Pure: no state, no calls (tested in chain/ba/cl.test.ts).
import { approx, isChn, type Amount } from './model';

export const MIN_TICK = -887272;
export const MAX_TICK = 887272;

/** A concentrated pool's state (getpool's `concentrated`). */
export interface ClState {
  sqrt_price_x96: string;
  tick: number;
  /** In range now (a decimal: 128 bits). */
  liquidity: string;
  tick_spacing: number;
}

export interface TickInfo {
  tick: number;
  /** asset0 in asset1, whole units. */
  price: number;
  liquidity_net: string;
  liquidity_gross: string;
  /** The liquidity in force from this tick up to the next. */
  liquidity_above: string;
}

/** What getpoolticks answers (the pool's own order). */
export interface PoolTicks {
  tick: number;
  price: number;
  liquidity: string;
  tick_spacing: number;
  ticks: TickInfo[];
}

/** What previewposition answers (the pool's own order). */
export interface PositionPreview {
  tick_lower: number;
  tick_upper: number;
  price_lower: number;
  price_upper: number;
  liquidity: string;
  amount0: Amount;
  amount1: Amount;
  in_range: boolean;
}

/** A position of the wallet (listpositions; getposition gives the same, and its txid and height). */
export interface Position {
  position: string;
  pool: string;
  /** Labels (text of the chain), the pool's own order. */
  asset0: string;
  asset1: string;
  tick_lower: number;
  tick_upper: number;
  price_lower: number;
  price_upper: number;
  liquidity: string;
  in_range: boolean;
  amount0: Amount;
  amount1: Amount;
  fees0: Amount;
  fees1: Amount;
}

// --- Orientation ---

/**
 * Whether a pool as the lists give it has its assets the other way round from the node's own order: the lists put CHN
 * second, the node first (the null hash is the lowest id). Else both put the lower id first.
 */
export function flipped(pool: { asset_b_id: string }): boolean {
  return isChn(pool.asset_b_id);
}

/** A price of the node's (asset0 in asset1) as the page shows it (asset_a in asset_b), and back: the same turn. */
export function turn(price: number, flip: boolean): number {
  if (!flip) return price;
  return price > 0 && Number.isFinite(price) ? 1 / price : price === 0 ? Infinity : 0;
}

/** A range of the page (asset_a in asset_b) as the node takes it (asset0 in asset1), lower first, and back. */
export function turnRange(lower: number, upper: number, flip: boolean): [number, number] {
  if (!flip) return [lower, upper];
  return [turn(upper, true), turn(lower, true)];
}

/** The amounts of a position, its range and its fees, in the page's order (asset_a first). */
export function positionSides(p: Pick<Position, 'amount0' | 'amount1' | 'fees0' | 'fees1' | 'price_lower' | 'price_upper'>, flip: boolean): { amountA: Amount; amountB: Amount; feesA: Amount; feesB: Amount; lower: number; upper: number } {
  const [lower, upper] = turnRange(approx(p.price_lower), approx(p.price_upper), flip);
  return flip ? { amountA: p.amount1, amountB: p.amount0, feesA: p.fees1, feesB: p.fees0, lower, upper } : { amountA: p.amount0, amountB: p.amount1, feesA: p.fees0, feesB: p.fees1, lower, upper };
}

// --- Ticks ---

/** The price of a tick, asset0 in asset1, whole units (d0, d1: their decimals). */
export function priceOfTick(tick: number, d0: number, d1: number): number {
  return 1.0001 ** tick * 10 ** (d0 - d1);
}

/** The tick of a price, to the pool's spacing: down for a lower bound, up for an upper one (as the node rounds it). */
export function tickOfPrice(price: number, d0: number, d1: number, spacing: number, up: boolean): number {
  const lo = Math.ceil(MIN_TICK / spacing) * spacing;
  const hi = Math.floor(MAX_TICK / spacing) * spacing;
  if (!(price > 0)) return lo;
  if (!Number.isFinite(price)) return hi;
  const exact = Math.floor(Math.log(price * 10 ** (d1 - d0)) / Math.log(1.0001));
  const tick = Math.min(MAX_TICK, Math.max(MIN_TICK, exact));
  let aligned = Math.floor(tick / spacing) * spacing;
  if (up && aligned < tick) aligned += spacing;
  return Math.min(hi, Math.max(lo, aligned));
}

/** The whole range a pool takes (its lowest and highest ticks to its spacing), as prices of the node. */
export function fullRange(spacing: number, d0: number, d1: number): [number, number] {
  return [priceOfTick(Math.ceil(MIN_TICK / spacing) * spacing, d0, d1), priceOfTick(Math.floor(MAX_TICK / spacing) * spacing, d0, d1)];
}

export type RangePreset = '1' | '5' | '20' | 'full';
export const RANGE_PRESETS: { id: RangePreset; label: string }[] = [
  { id: '1', label: '±1%' },
  { id: '5', label: '±5%' },
  { id: '20', label: '±20%' },
  { id: 'full', label: 'Full range' },
];

/** A range around a price: within a percentage each way, or the whole range (`full`: the pool's, in the page's order). */
export function presetRange(price: number, preset: RangePreset, full: [number, number]): [number, number] {
  if (preset === 'full') return full;
  const f = Number(preset) / 100;
  return [price * (1 - f), price * (1 + f)];
}

/** Where a price is against a range: below it, in it, above it. */
export function rangeState(price: number, lower: number, upper: number): 'below' | 'in' | 'above' {
  if (price < lower) return 'below';
  if (price >= upper) return 'above';
  return 'in';
}

/**
 * Which asset a range takes alone when the price is out of it, in the page's order: a range above the price holds the
 * asset whose price must rise into it (asset_a: its price in asset_b is below the range), a range below it asset_b.
 */
export function sideAlone(price: number, lower: number, upper: number): 'a' | 'b' | null {
  const s = rangeState(price, lower, upper);
  return s === 'below' ? 'a' : s === 'above' ? 'b' : null;
}

// --- The liquidity distribution ---

export interface Bar {
  /** The prices it spans, in the page's order (lower first), and the liquidity in force there. */
  from: number;
  to: number;
  liquidity: number;
}

/** The bars of the distribution: from each tick to the next, the liquidity in force; in the page's order. */
export function liquidityBars(ticks: readonly Pick<TickInfo, 'price' | 'liquidity_above'>[], flip: boolean): Bar[] {
  const sorted = [...ticks].sort((a, b) => approx(a.price) - approx(b.price));
  const bars: Bar[] = [];
  for (let i = 0; i + 1 < sorted.length; i++) {
    const liquidity = Number(sorted[i].liquidity_above);
    if (!(liquidity > 0)) continue;
    const [from, to] = turnRange(approx(sorted[i].price), approx(sorted[i + 1].price), flip);
    bars.push({ from, to, liquidity });
  }
  return bars.sort((a, b) => a.from - b.from);
}

/**
 * The prices a distribution chart spans (log scale): around the pool's price, the range picked when it is not the whole
 * range, at most `span` times either way of the price (a bar of the whole range reaches 10^38), and the bars clipped to it.
 */
export function chartDomain(price: number, range: [number, number] | null, span = 4): [number, number] {
  if (!(price > 0)) return [0.5, 2];
  let lo = price / 1.5;
  let hi = price * 1.5;
  if (range && range[0] > 0 && range[1] > range[0]) {
    lo = Math.min(lo, range[0] / 1.15);
    hi = Math.max(hi, range[1] * 1.15);
  }
  return [Math.max(lo, price / span), Math.min(hi, price * span)];
}

/** x of a price on a chart `width` wide (log scale over the domain), and the price at an x (dragging a bound). */
export function xOfPrice(price: number, domain: [number, number], width: number): number {
  const [lo, hi] = domain;
  const t = (Math.log(Math.max(price, 1e-300)) - Math.log(lo)) / (Math.log(hi) - Math.log(lo) || 1);
  return Math.max(0, Math.min(1, t)) * width;
}
export function priceAtX(x: number, domain: [number, number], width: number): number {
  const [lo, hi] = domain;
  const t = Math.max(0, Math.min(1, x / (width || 1)));
  return Math.exp(Math.log(lo) + t * (Math.log(hi) - Math.log(lo)));
}

/** The bars within a domain (clipped to it), and the tallest of them (the scale of y). */
export function visibleBars(bars: readonly Bar[], domain: [number, number]): { bars: Bar[]; max: number } {
  const out = bars.filter((b) => b.to > domain[0] && b.from < domain[1]).map((b) => ({ from: Math.max(b.from, domain[0]), to: Math.min(b.to, domain[1]), liquidity: b.liquidity }));
  return { bars: out, max: out.reduce((m, b) => Math.max(m, b.liquidity), 0) };
}

/** A price for a field: 6 significant figures, plain (no exponent below 1e21), '' for nothing. */
export function priceText(price: number): string {
  if (!(price > 0) || !Number.isFinite(price)) return '';
  const p = Number(price.toPrecision(6));
  if (p >= 1e21) return p.toExponential(5);
  if (p < 1e-6) return p.toExponential(5);
  return String(p);
}

/**
 * Amounts that name a price for createpool of a concentrated pool (made at amount_b / amount_a, nothing in): 1 of asset_a
 * and the price of it in asset_b, or more of asset_a when the price would not hold 7 significant digits in asset_b's units.
 */
export function priceAmounts(price: number, db: number): { a: string; b: string } | null {
  if (!(price > 0) || !Number.isFinite(price)) return null;
  const need = 1e7 / (price * 10 ** db);
  const exponent = Math.max(0, Math.ceil(Math.log10(need)));
  const a = 10 ** Math.min(exponent, 12);
  const units = Math.round(a * price * 10 ** db);
  if (units <= 0 || units > Number.MAX_SAFE_INTEGER) return null;
  const b = BigInt(units);
  const scale = 10n ** BigInt(db);
  const frac = db ? (b % scale).toString().padStart(db, '0').replace(/0+$/, '') : '';
  return { a: String(a), b: `${b / scale}${frac ? `.${frac}` : ''}` };
}

/**
 * A position's range and the price on a bar of 0 to 100 (log scale): the range and the price with a margin either way;
 * a range far wider than the price's surroundings (the whole range) clipped to a thousand times either way of it.
 */
export function rangeBar(price: number, lower: number, upper: number): { from: number; to: number; price: number } {
  const p = price > 0 ? price : Math.sqrt(lower * upper) || 1;
  const lo = Math.max(Math.min(lower, p) / 1.25, p / 1000);
  const hi = Math.min(Math.max(upper, p) * 1.25, p * 1000);
  const at = (x: number) => Math.round(xOfPrice(x, [lo, hi], 100) * 100) / 100;
  return { from: at(lower), to: at(upper), price: at(p) };
}
