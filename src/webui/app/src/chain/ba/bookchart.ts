// The charts of an order book (ba/BookDepth.svelte, ba/BookCandles.svelte): its depth, the cumulative amounts of the bids
// below the middle price and of the asks above it, each level's own amount and the running total, priced by the chain's
// exact limits when the node gives them (getbook's price_exact, mid_exact: decimal strings of the rational limits; the
// doubles of an older node otherwise); the candles of its trades (the market index: getpoolcandles of the book's id, at
// 5m, 1h, 4h or 1d); and the wallet's own open orders in it, as marks at their limits. Prices are of a whole base
// (asset_a) in the quote (asset_b). Pure: no state, no calls (tested in chain/ba/bookchart.test.ts).
import { approx, type Amount } from './model';

/** A level of getbook, with the exact fields of newer nodes. */
export interface ExactLevel {
  price: number;
  amount: number;
  orders?: number;
  /** The rational limit as a decimal string (18 significant places), when the node gives it. */
  price_exact?: string;
  amount_units?: string;
}

export interface DepthPoint {
  side: 'bid' | 'ask';
  /** For the chart (a double of the exact price). */
  price: number;
  /** The price as the chain has it, for the readout: the exact string, or the double's text. */
  priceText: string;
  /** This level's amount, and the amount of it and every better level (whole base). */
  amount: number;
  total: number;
}

export interface Depth {
  bids: DepthPoint[];
  asks: DepthPoint[];
  mid: number | null;
  midText: string;
  /** The range of prices drawn, and the largest total (the height). */
  lo: number;
  hi: number;
  maxTotal: number;
}

/** A price of the node: its exact string when given (and a number), else the double. */
export function exactPrice(level: { price?: unknown; price_exact?: unknown }): { value: number; text: string } {
  const exact = typeof level.price_exact === 'string' && /^\d+(\.\d+)?$/.test(level.price_exact) ? level.price_exact : '';
  const value = exact ? Number(exact) : approx(level.price as Amount);
  return { value, text: exact || (Number.isFinite(value) ? String(Number(value.toPrecision(12))) : '–') };
}

function sideOf(list: unknown, side: 'bid' | 'ask'): DepthPoint[] {
  if (!Array.isArray(list)) return [];
  const levels = list
    .map((l) => {
      const p = exactPrice(l as ExactLevel);
      return { price: p.value, priceText: p.text, amount: approx((l as ExactLevel)?.amount) };
    })
    .filter((l) => Number.isFinite(l.price) && l.price > 0 && Number.isFinite(l.amount) && l.amount > 0)
    .sort((x, y) => (side === 'bid' ? y.price - x.price : x.price - y.price));
  let total = 0;
  return levels.map((l) => {
    total += l.amount;
    return { side, ...l, total };
  });
}

/**
 * The depth of a book: the bids from the best down, the asks from the best up, each with its running total, the middle
 * price (the node's mid_exact, else halfway between the best of each side), and a range of prices that holds them all,
 * at most `spread` times the middle on either side (a stray order far off does not flatten the rest).
 */
export function depthOf(book: { asks?: unknown; bids?: unknown; mid_exact?: unknown } | null | undefined, spread = 0.5): Depth {
  const bids = sideOf(book?.bids, 'bid');
  const asks = sideOf(book?.asks, 'ask');
  const bestBid = bids[0]?.price ?? null;
  const bestAsk = asks[0]?.price ?? null;
  const midExact = typeof book?.mid_exact === 'string' && /^\d+(\.\d+)?$/.test(book.mid_exact) ? book.mid_exact : '';
  const mid = midExact ? Number(midExact) : bestBid !== null && bestAsk !== null ? (bestBid + bestAsk) / 2 : (bestBid ?? bestAsk);
  const midText = midExact || (mid !== null ? String(Number(mid.toPrecision(12))) : '');
  const prices = [...bids, ...asks].map((p) => p.price);
  let lo = prices.length ? Math.min(...prices) : 0;
  let hi = prices.length ? Math.max(...prices) : 1;
  if (mid !== null && mid > 0) {
    lo = Math.max(lo, mid * (1 - spread));
    hi = Math.min(hi, mid * (1 + spread));
    // Room on both sides of the middle, the same each way.
    // Room beyond the outermost levels (a half again): a side of one level shows as an area, not an edge.
    const half = Math.max(mid - lo, hi - mid, mid * 0.02) * 1.5;
    lo = Math.max(0, mid - half);
    hi = mid + half;
  }
  if (hi <= lo) hi = lo + (lo || 1);
  const shown = [...bids, ...asks].filter((p) => p.price >= lo && p.price <= hi);
  const maxTotal = Math.max(1e-12, ...shown.map((p) => p.total), ...(bids.length ? [bids.filter((b) => b.price >= lo).at(-1)?.total ?? 0] : []), ...(asks.length ? [asks.filter((a) => a.price <= hi).at(-1)?.total ?? 0] : []));
  return { bids, asks, mid, midText, lo, hi, maxTotal };
}

/** The level under a price (the nearest of its side, toward the middle), for the readout. */
export function levelAt(d: Depth, price: number): DepthPoint | null {
  if (d.mid === null) return null;
  if (price <= d.mid) {
    // The bids at this price or above: the deepest of them is the cumulative total a seller down to it meets.
    const hit = d.bids.filter((b) => b.price >= price);
    return hit.at(-1) ?? null;
  }
  const hit = d.asks.filter((a) => a.price <= price);
  return hit.at(-1) ?? null;
}

/** A candle of the market index. */
export interface Candle {
  /** Its start, in seconds. */
  t: number;
  o: number;
  h: number;
  l: number;
  c: number;
  /** Volume, in the quote. */
  v: number;
}

export const CANDLE_INTERVALS = ['5m', '1h', '4h', '1d'] as const;
export type CandleInterval = (typeof CANDLE_INTERVALS)[number];

/** The candles a node answers, checked: oldest first, finite, high ≥ low; a list in {items}, {candles} or alone. */
export function candlesOf(raw: unknown): Candle[] {
  const list = Array.isArray(raw) ? raw : raw && typeof raw === 'object' ? ((raw as { items?: unknown; candles?: unknown }).items ?? (raw as { candles?: unknown }).candles) : null;
  if (!Array.isArray(list)) return [];
  const n = (v: unknown) => (typeof v === 'number' || typeof v === 'string' ? approx(v as Amount) : NaN);
  return list
    .map((c) => {
      const o = c as Record<string, unknown>;
      return { t: n(o.t ?? o.time ?? o.start), o: n(o.o ?? o.open), h: n(o.h ?? o.high), l: n(o.l ?? o.low), c: n(o.c ?? o.close), v: n(o.v ?? o.volume) || 0 };
    })
    .filter((c) => [c.t, c.o, c.h, c.l, c.c].every(Number.isFinite) && c.h >= c.l && c.l > 0)
    .sort((a, b) => a.t - b.t);
}

/** An order of the wallet in a book (getorder), as a mark: its side seen from the base, its limit in quote per base. */
export interface OrderMark {
  order: string;
  side: 'bid' | 'ask';
  price: number;
  /** What is left of it, in whole base (an ask) or quote (a bid). */
  remaining: number;
}

/**
 * The wallet's open orders of the book of `base` priced in `quote`: one selling the base is an ask at its limit; one
 * selling the quote for the base is a bid, its limit (base per quote) turned over. Others (another book, nothing left) out.
 */
export function orderMarks(orders: readonly { order: string; sells_id?: string; buys_id?: string; price?: number; remaining?: Amount }[], base: string, quote: string): OrderMark[] {
  const out: OrderMark[] = [];
  for (const o of orders) {
    const price = Number(o.price);
    const remaining = approx(o.remaining);
    if (!(price > 0) || !(remaining > 0)) continue;
    if (o.sells_id === base && o.buys_id === quote) out.push({ order: o.order, side: 'ask', price, remaining });
    else if (o.sells_id === quote && o.buys_id === base) out.push({ order: o.order, side: 'bid', price: 1 / price, remaining });
  }
  return out;
}

/** The labels of a time axis for candles of an interval: hours and minutes within a day, the day otherwise. */
export function formatCandleTime(seconds: number, interval: CandleInterval): string {
  const d = new Date(seconds * 1000);
  const pad = (x: number) => String(x).padStart(2, '0');
  if (interval === '1d' || interval === '4h') return `${d.getUTCFullYear()}-${pad(d.getUTCMonth() + 1)}-${pad(d.getUTCDate())}${interval === '4h' ? ` ${pad(d.getUTCHours())}h` : ''}`;
  return `${pad(d.getUTCHours())}:${pad(d.getUTCMinutes())}`;
}
