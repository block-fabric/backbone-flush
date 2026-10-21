// Layers' DEX order books as the pages see them (src/rpc/bitassets.cpp: getbook, getorder; src/wallet/rpc/bitassets.cpp:
// placeorder, settleorder, listorders): the ladder of a book (asks above, bids below, the spread, the depth of each
// level), a limit order to place on either side of a pair and the rules of the chain for it (bitassets/state.cpp: not
// dust: 0.01 CHN on the CHN side of a pair with CHN, a whole unit of what it sells otherwise), and what an order of the
// wallet is now (open, filled, expired, evicted). Prices are of a whole asset_a (the base) in asset_b (the quote).
// Pure: no state, no calls (tested in chain/ba/book.test.ts).
import { approx, isChn, MAX_AMOUNT, type Amount } from './model';

/** The least an order of a pair with CHN is worth on its CHN side: 0.01 CHN (bitassets::MIN_BOOK_CHN). */
export const MIN_BOOK_CHN = 1_000_000n;
/** The latest an order may expire, in blocks after the one that places it: about a year (bitassets::MAX_ORDER_DURATION). */
export const MAX_ORDER_DURATION = 2_102_400;
/** placeorder's default: about a week of 15-second blocks. */
export const DEFAULT_ORDER_BLOCKS = 40_320;
/** How long an order trades, the presets (15-second blocks). */
export const ORDER_DURATIONS: readonly { blocks: number; label: string }[] = [
  { blocks: 240, label: '1 hour' },
  { blocks: 5_760, label: '1 day' },
  { blocks: 40_320, label: '1 week' },
  { blocks: 172_800, label: '30 days' },
];

// --- What the node answers ---

export interface BookLevel {
  /** Of a whole asset_a, in asset_b. */
  price: number;
  /** asset_a for sale (an ask) or wanted (a bid) at it, in whole units. */
  amount: number;
  orders: number;
}

/** What getbook answers. */
export interface Book {
  book: string;
  asset_a: string;
  asset_b: string;
  open_orders: number;
  best_ask?: number;
  best_bid?: number;
  /** Best (lowest) first. */
  asks: BookLevel[];
  /** Best (highest) first. */
  bids: BookLevel[];
}

/** What getorder answers. */
export interface BookOrder {
  order: string;
  book: string;
  sells?: string;
  sells_id?: string;
  buys?: string;
  buys_id?: string;
  amount?: Amount;
  remaining?: Amount;
  /** Its limit: whole units bought per whole unit sold, at least. */
  price?: number;
  got?: Amount;
  to_collect?: Amount;
  expiry?: number;
  txid?: string;
  height?: number;
  evicted_height?: number;
}

/** What listorders answers of each order of the wallet. */
export interface WalletOrder {
  order: string;
  book: string;
  /** Labels (text of the chain). */
  sells: string;
  buys: string;
  remaining: Amount;
  to_collect: Amount;
  expiry: number;
  expired: boolean;
  evicted: boolean;
}

// --- The ladder ---

export interface LadderRow {
  side: 'ask' | 'bid';
  price: number;
  amount: number;
  orders: number;
  /** What this level and the better ones hold, in whole asset_a. */
  total: number;
  /** That total as a share of the deepest side's, from 0 to 1: the width of its bar. */
  depth: number;
}

export interface Ladder {
  /** The asks shown, the worst at the top and the best at the bottom (next to the spread). */
  asks: LadderRow[];
  /** The bids shown, the best at the top. */
  bids: LadderRow[];
  bestAsk: number | null;
  bestBid: number | null;
  /** The best ask less the best bid; negative when they cross (the block's batch trades them). */
  spread: number | null;
  /** The spread in percent of the middle. */
  spreadPercent: number | null;
  mid: number | null;
}

function levels(list: unknown): BookLevel[] {
  if (!Array.isArray(list)) return [];
  return list
    .map((l) => ({ price: approx((l as BookLevel)?.price), amount: approx((l as BookLevel)?.amount), orders: Number((l as BookLevel)?.orders) || 0 }))
    .filter((l) => Number.isFinite(l.price) && l.price > 0 && Number.isFinite(l.amount) && l.amount > 0);
}

/** The ladder of a book: up to `rows` levels each side, with their running totals and their depth for the bars. */
export function buildLadder(book: Pick<Book, 'asks' | 'bids'> | null | undefined, rows = 12): Ladder {
  const asks = levels(book?.asks)
    .sort((x, y) => x.price - y.price)
    .slice(0, Math.max(0, rows));
  const bids = levels(book?.bids)
    .sort((x, y) => y.price - x.price)
    .slice(0, Math.max(0, rows));
  const running = (list: BookLevel[], side: LadderRow['side']) => {
    let total = 0;
    return list.map((l) => ({ side, price: l.price, amount: l.amount, orders: l.orders, total: (total += l.amount), depth: 0 }));
  };
  const a = running(asks, 'ask');
  const b = running(bids, 'bid');
  const deepest = Math.max(a.at(-1)?.total ?? 0, b.at(-1)?.total ?? 0);
  for (const r of [...a, ...b]) r.depth = deepest > 0 ? r.total / deepest : 0;
  const bestAsk = a[0]?.price ?? null;
  const bestBid = b[0]?.price ?? null;
  const mid = bestAsk !== null && bestBid !== null ? (bestAsk + bestBid) / 2 : null;
  const spread = bestAsk !== null && bestBid !== null ? bestAsk - bestBid : null;
  return { asks: a.reverse(), bids: b, bestAsk, bestBid, spread, spreadPercent: spread !== null && mid ? (spread / mid) * 100 : null, mid };
}

// --- A limit order ---

/** A decimal number typed: its digits and how many of them are decimals (value = mantissa / 10^scale). */
export type DecimalParse = { ok: true; mantissa: bigint; scale: number } | { ok: false; error: string };

/** A price typed: a positive decimal number of at most 18 significant digits. */
export function parseDecimal(text: string): DecimalParse {
  const s = text.trim().replace(/,/g, '');
  if (s === '') return { ok: false, error: 'Enter a price.' };
  const m = /^(\d*)(?:\.(\d*))?$/.exec(s);
  if (!m || (m[1] === '' && (m[2] ?? '') === '')) return { ok: false, error: 'Enter a number, such as 1.25.' };
  const frac = (m[2] ?? '').replace(/0+$/, '');
  const digits = `${m[1]}${frac}`.replace(/^0+/, '');
  if (digits === '') return { ok: false, error: 'The price is zero.' };
  if (digits.length > 18) return { ok: false, error: 'The price has too many digits.' };
  return { ok: true, mantissa: BigInt(digits), scale: frac.length };
}

/** A decimal as text: mantissa / 10^scale, its trailing zeros trimmed. */
export function decimalText(mantissa: bigint, scale: number): string {
  if (scale <= 0) return (mantissa * 10n ** BigInt(-scale)).toString();
  const s = mantissa.toString().padStart(scale + 1, '0');
  const whole = s.slice(0, s.length - scale);
  const frac = s.slice(s.length - scale).replace(/0+$/, '');
  return frac ? `${whole}.${frac}` : whole;
}

const gcd = (x: bigint, y: bigint): bigint => {
  while (y !== 0n) [x, y] = [y, x % y];
  return x;
};

/**
 * A price of whole units as placeorder works it out in units (num/den: price · 10^(bought decimals − sold decimals),
 * reduced); null when either is beyond what the chain holds ("The price has too many digits").
 */
export function unitRatio(price: { mantissa: bigint; scale: number }, soldDecimals: number, boughtDecimals: number): { num: bigint; den: bigint } | null {
  let num = price.mantissa * 10n ** BigInt(boughtDecimals);
  let den = 10n ** BigInt(price.scale + soldDecimals);
  const g = gcd(num, den);
  num /= g;
  den /= g;
  if (num > MAX_AMOUNT || den > MAX_AMOUNT || num === 0n) return null;
  return { num, den };
}

/**
 * The inverse of a price, as a buy order's limit (what is bought per whole unit paid): `digits` significant digits, rounded
 * up (a stricter limit: it never pays more than the price typed).
 */
export function invertPrice(price: { mantissa: bigint; scale: number }, digits = 12): { mantissa: bigint; scale: number } {
  // 1 / (m / 10^s) = 10^s / m, times 10^scale: ceil(10^(s + scale) / m), with `digits` significant digits.
  const m = price.mantissa;
  const length = m.toString().length;
  let scale = digits + length - 1 - price.scale;
  const up = (exp: number) => (10n ** BigInt(exp) + m - 1n) / m;
  let q = up(price.scale + scale);
  // A power of ten gives a digit more.
  if (q.toString().length > digits) {
    scale -= 1;
    q = up(price.scale + scale);
  }
  while (scale > 0 && q % 10n === 0n) {
    q /= 10n;
    scale -= 1;
  }
  return { mantissa: q, scale };
}

export interface OrderAsset {
  id: string;
  decimals: number;
}

/** A problem of an order: a message of the app (shown with t()). */
export interface OrderProblem {
  error: string;
}

export interface PlannedOrder {
  /** What it sells and buys (ids), and placeorder's amount and price (exact text). */
  sell: OrderAsset;
  buy: OrderAsset;
  amount: string;
  price: string;
  /** In units: what it sells, and what it buys at its limit at least. */
  sellUnits: bigint;
  buysAtLeast: bigint;
}

export type OrderPlan = ({ ok: true } & PlannedOrder) | ({ ok: false } & OrderProblem);

/**
 * A limit order of the book of `base` in `quote`: sell `amount` units of the base at `price` of the quote at least, or buy
 * `amount` units of the base paying `price` at most (it sells the quote: amount · price, rounded up, at the inverse limit).
 * Checked as the chain checks it (bitassets/state.cpp): it buys something at its limit; of a pair with CHN, its CHN side is
 * 0.01 CHN at least; of another pair, it sells a whole unit at least. `held`: what the wallet holds of what it sells.
 */
export function planOrder(side: 'buy' | 'sell', base: OrderAsset, quote: OrderAsset, amount: bigint, priceText: string, held?: bigint | null): OrderPlan {
  if (base.id === quote.id) return { ok: false, error: 'Two different assets make a book.' };
  if (amount <= 0n) return { ok: false, error: 'Enter an amount.' };
  const price = parseDecimal(priceText);
  if (!price.ok) return { ok: false, error: price.error };
  let sell: OrderAsset;
  let buy: OrderAsset;
  let sellUnits: bigint;
  let limit: { mantissa: bigint; scale: number };
  if (side === 'sell') {
    sell = base;
    buy = quote;
    sellUnits = amount;
    limit = price;
  } else {
    sell = quote;
    buy = base;
    // What the base costs at the price, rounded up: amount · m · 10^dq / (10^s · 10^db).
    const num = amount * price.mantissa * 10n ** BigInt(quote.decimals);
    const den = 10n ** BigInt(price.scale + base.decimals);
    sellUnits = (num + den - 1n) / den;
    limit = invertPrice(price);
  }
  if (sellUnits > MAX_AMOUNT) return { ok: false, error: 'Too large.' };
  const ratio = unitRatio(limit, sell.decimals, buy.decimals);
  if (!ratio) return { ok: false, error: 'The price has too many digits.' };
  const buysAtLeast = (sellUnits * ratio.num) / ratio.den;
  if (held !== undefined && held !== null && sellUnits > held) return { ok: false, error: 'This wallet holds less than that.' };
  if (buysAtLeast === 0n) return { ok: false, error: 'Too little: at this price it buys nothing.' };
  if (isChn(sell.id) || isChn(buy.id)) {
    const chn = isChn(sell.id) ? sellUnits : buysAtLeast;
    if (chn < MIN_BOOK_CHN) return { ok: false, error: 'Too little: an order of a pair with CHN is worth 0.01 CHN at least on its CHN side.' };
  } else if (sellUnits < 10n ** BigInt(sell.decimals)) {
    return { ok: false, error: 'Too little: an order of a pair without CHN sells a whole unit at least of what it sells.' };
  }
  return { ok: true, sell, buy, amount: decimalText(sellUnits, sell.decimals), price: decimalText(limit.mantissa, limit.scale), sellUnits, buysAtLeast };
}

/** placeorder's parameters for a planned order: [asset_sell, amount, asset_buy, price, blocks], the assets by id. */
export function placeParams(plan: PlannedOrder, blocks: number, assetArg: (id: string) => string): unknown[] {
  return [assetArg(plan.sell.id), plan.amount, assetArg(plan.buy.id), plan.price, blocks];
}

/** Whether a number of blocks is one an order may trade for. */
export function validDuration(blocks: number): boolean {
  return Number.isInteger(blocks) && blocks >= 1 && blocks <= MAX_ORDER_DURATION;
}

// --- The orders of the wallet ---

export type OrderStatus = 'open' | 'filled' | 'expired' | 'evicted';

/** What an order is now: evicted (it trades no more), expired, filled (nothing left to sell), or open. */
export function orderStatus(o: Pick<WalletOrder, 'remaining' | 'expired' | 'evicted'>): OrderStatus {
  if (o.evicted) return 'evicted';
  if (o.expired) return 'expired';
  if (approx(o.remaining) <= 0) return 'filled';
  return 'open';
}

/** The words of a status (shown with t()). */
export const ORDER_STATUS: Record<OrderStatus, string> = { open: 'Open', filled: 'Filled', expired: 'Expired', evicted: 'Evicted' };

/** The tag class of a status. */
export const ORDER_STATUS_TAG: Record<OrderStatus, string> = { open: 'accent', filled: '', expired: 'warn', evicted: 'danger' };

/**
 * What can be done with an order: collect what it got and leave it open (open, with something to collect), cancel it (open:
 * what is left back too), or settle it (closed: everything back, the order gone).
 */
export function orderActions(o: Pick<WalletOrder, 'remaining' | 'expired' | 'evicted' | 'to_collect'>): { collect: boolean; cancel: boolean; settle: boolean } {
  const status = orderStatus(o);
  if (status !== 'open') return { collect: false, cancel: false, settle: true };
  return { collect: approx(o.to_collect) > 0, cancel: true, settle: false };
}
