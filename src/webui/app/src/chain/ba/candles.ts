// The price of a market over time, from the node's market index (-dexindex: getpoolcandles of a pool's id, or of a
// launch curve's): candles at 5m, 1h, 4h or 1d, checked as they come; the periods of an asset's price chart (owner
// decision, 2026-11-11: 1H, 1D, 1W, All) as the interval and the span asked for each; and the figures of a day worked
// out from its candles (the change of the price, the volume), where the node keeps the index. Pure: no state, no calls
// (tested in chain/ba/pools.test.ts). The time axis of a chart: a slot per interval, traded or not (timeline).
import { approx, type Amount } from './model';

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

/** The periods of a price chart: the candles asked for each (at most 500 come back), and how far back (0: all). */
export const PERIODS = [
  { id: '1H', interval: '5m', span: 3600 },
  { id: '1D', interval: '1h', span: 86_400 },
  { id: '1W', interval: '4h', span: 604_800 },
  { id: 'All', interval: '1d', span: 0 },
] as const satisfies readonly { id: string; interval: CandleInterval; span: number }[];
export type Period = (typeof PERIODS)[number]['id'];

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

/**
 * The figures of a span from its candles: the change of the price from the first open to the last close, in percent,
 * and the volume (in the quote, ECX for a market with ECX). null when there is no candle: nothing traded, or the node
 * said nothing; never a figure made up.
 */
export function spanStats(candles: readonly Candle[]): { change: number; volume: number } | null {
  if (candles.length === 0) return null;
  const first = candles[0].o;
  const last = candles[candles.length - 1].c;
  return { change: first > 0 ? (last / first - 1) * 100 : 0, volume: candles.reduce((n, c) => n + c.v, 0) };
}

/** The length of a candle of an interval, in seconds. */
export const INTERVAL_SECONDS: Record<CandleInterval, number> = { '5m': 300, '1h': 3600, '4h': 14_400, '1d': 86_400 };

/** A slot of a price chart's time axis: the candle of its time, or none traded then (`c` null) and the price carried. */
export interface Slot {
  t: number;
  c: Candle | null;
  /** The last close before a slot with no trade; null before the first trade known. */
  carried: number | null;
}

/**
 * The time axis of a price chart (owner feedback, 2026-11-12: the graph follows the actual timeline, with an empty line
 * where nothing traded): one slot per interval from `now - span` to `now` (span 0: from the first candle), each with
 * its candle or with the price carried from the last trade before it. Candles older than the axis only give the price
 * its first slots carry. At most `max` slots, the newest.
 */
export function timeline(candles: readonly Candle[], interval: CandleInterval, span: number, now: number, max = 500): Slot[] {
  const step = INTERVAL_SECONDS[interval];
  const floor = (t: number) => Math.floor(t / step) * step;
  const end = floor(now);
  let start = span > 0 ? floor(now - span) + step : candles.length ? floor(candles[0].t) : end;
  if (start > end) start = end;
  if ((end - start) / step + 1 > max) start = end - (max - 1) * step;
  const at = new Map<number, Candle>();
  let carried: number | null = null;
  for (const c of candles) {
    const t = floor(c.t);
    if (t < start) carried = c.c;
    else if (t <= end) at.set(t, c);
  }
  const out: Slot[] = [];
  for (let t = start; t <= end; t += step) {
    const c = at.get(t) ?? null;
    out.push({ t, c, carried: c ? null : carried });
    if (c) carried = c.c;
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
