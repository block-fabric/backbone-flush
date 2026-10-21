// Layers' launches as the pages see them (doc/layers.md, "Launches"; src/rpc/bitassets.cpp getlaunch, listlaunches,
// quotelaunch, getlaunchstats, getlaunchlog; src/wallet/rpc/bitassets.cpp launchtoken, buyfromlaunch, selltolaunch,
// claimlaunch): a token sold from a bonding curve until the curve has raised what it graduates at, then traded in a
// locked pool with CHN. The curve, worked out from the rules (a constant product of virtual reserves: from (G/2, 9/8·S)
// to (3/2·G, 3/8·S), G the graduation, S the supply), for its chart and the review of a launch; the queries of the
// list; the options of launchtoken. Pure: no state, no calls (tested in chain/ba/launch.test.ts).
import { approx, type Amount } from './model';

/** A launch's token has 6 decimals and a billion units, fixed (bitassets::LAUNCH_DECIMALS, LAUNCH_SUPPLY). */
export const LAUNCH_DECIMALS = 6;
/** What getlaunchstats' rules say on every network today, for a node that did not answer yet. */
export const DEFAULT_RULES: LaunchRules = { graduation: 500, fee: 1, creator_share: 30, supply: 1_000_000_000, curve_supply: 750_000_000, pool_supply: 250_000_000, max_allocation: 5, log_blocks: 10_080, active: false };

export interface LaunchRules {
  /** CHN a curve raises before it graduates. */
  graduation: Amount;
  /** A trade's fee on a curve, in percent, and the creator's part of it, in percent of the fee. */
  fee: number;
  creator_share: number;
  /** Whole tokens of a launch, of which the curve sells, and go into the pool at graduation. */
  supply: Amount;
  curve_supply: Amount;
  pool_supply: Amount;
  /** The most a creator's allocation is, in percent of the supply. */
  max_allocation: number;
  log_blocks: number;
  /** Whether the next block takes launches. */
  active: boolean;
}

export interface LaunchStats {
  launches: number;
  on_curve: number;
  graduated: number;
  volume: Amount;
  raised: Amount;
  rules: LaunchRules;
}

export type LaunchStatus = 'curve' | 'graduated';

/** A launch (LaunchResults). */
export interface Launch {
  token: string;
  /** Text of the chain. */
  ticker: string;
  display_name?: string;
  info?: string;
  has_logo: boolean;
  creator: string;
  txid: string;
  height: number;
  status: LaunchStatus;
  /** Its curve's pool, or, graduated, its pool with CHN. */
  pool: string;
  raised: Amount;
  graduation: Amount;
  progress: number;
  /** CHN for a whole token now. */
  price: number;
  market_cap: Amount;
  supply: Amount;
  curve_tokens_left: Amount;
  fee: number;
  creator_share: number;
  creator_fees_owed: Amount;
  creator_fees_paid: Amount;
  allocation: number;
  allocation_claimed: boolean;
  min_buy: Amount;
  max_buy: Amount;
  volume: Amount;
  trades: number;
  graduated_height?: number;
}

export interface LaunchPage {
  launches: Launch[];
  total: number;
  next_cursor?: string;
}

/** What quotelaunch answers. */
export interface LaunchQuote {
  amount_in: Amount;
  /** Tokens for a buy, CHN for a sale, the fee taken. */
  amount_out: Amount;
  /** CHN back: a buy past the curve's end. */
  back: Amount;
  fee: Amount;
  price: number;
  spot_price: number;
  price_after: number;
  price_impact: number;
  progress_after: number;
  graduates: boolean;
}

/** A row of getlaunchlog: what a block did on a launch's curve. */
export interface LaunchLogRow {
  height: number;
  token: string;
  created: boolean;
  graduated: boolean;
  orders: number;
  coin_in: Amount;
  coin_out: Amount;
  tokens_in: Amount;
  tokens_out: Amount;
  fees: Amount;
  reserve_coin: Amount;
  reserve_tokens: Amount;
}

// --- The list ---

export type LaunchFilter = 'all' | 'new' | 'curve' | 'near' | 'graduated';
export type LaunchSort = 'newest' | 'oldest' | 'volume' | 'progress' | 'market_cap';

/** The options of listlaunches for a filter of the page: "near graduation" is on the curve and at least `near` percent there. */
export function launchQuery(filter: LaunchFilter, sort: LaunchSort, near: number, cursor: string | undefined, limit: number): Record<string, string | number> {
  const out: Record<string, string | number> = { status: filter === 'near' ? 'curve' : filter, sort, limit };
  if (filter === 'near') out.min_progress = near;
  if (cursor) out.cursor = cursor;
  return out;
}

/** A page read after the ones before: those already listed (a list that moved between pages repeats one at its edge) left out. */
export function appendLaunches(list: readonly Launch[], page: readonly Launch[]): Launch[] {
  const seen = new Set(list.map((l) => l.token));
  return [...list, ...page.filter((l) => !seen.has(l.token))];
}

// --- The curve ---

/** The bonding curve of a launch, in whole CHN and whole tokens. */
export interface CurveShape {
  graduation: number;
  supply: number;
  /** The virtual reserves at the start (coin, tokens), the curve's end in tokens, and their product. */
  virtualCoin: number;
  virtualTokens: number;
  endTokens: number;
  k: number;
  /** What it sells, and what goes into the pool at graduation. */
  curveSupply: number;
  poolSupply: number;
  /** CHN for a whole token at the start and at the end (9 times more), and the market value of the supply then. */
  startPrice: number;
  endPrice: number;
  startCap: number;
  endCap: number;
}

/** The curve from the rules: V = G/2, Tv = 9/8·S, yE = 3/8·S, k = V·Tv = (V + G)·yE. */
export function curveShape(graduation: number, supply: number): CurveShape {
  const virtualCoin = graduation / 2;
  const virtualTokens = (supply * 9) / 8;
  const endTokens = (supply * 3) / 8;
  const k = virtualCoin * virtualTokens;
  const startPrice = virtualCoin / virtualTokens;
  const endPrice = (virtualCoin + graduation) / endTokens;
  return { graduation, supply, virtualCoin, virtualTokens, endTokens, k, curveSupply: virtualTokens - endTokens, poolSupply: supply / 4, startPrice, endPrice, startCap: startPrice * supply, endCap: endPrice * supply };
}

/** Where a curve is once it raised `raised` CHN: the tokens it sold, its price (x / y), the reserves. */
export function curvePointAt(shape: CurveShape, raised: number): { sold: number; price: number; coin: number; tokens: number } {
  const r = Math.min(Math.max(0, raised), shape.graduation);
  const coin = shape.virtualCoin + r;
  const tokens = shape.k / coin;
  return { sold: shape.virtualTokens - tokens, price: coin / tokens, coin, tokens };
}

/** The curve for its chart: its price (y) by the tokens it sold (x), from the start to the end. */
export function curveSeries(shape: CurveShape, n = 48): { x: number; y: number }[] {
  return Array.from({ length: n + 1 }, (_, i) => {
    const p = curvePointAt(shape, (shape.graduation * i) / n);
    return { x: p.sold, y: p.price };
  });
}

/** Where the supply goes: the curve's part, the pool's at graduation, and the creator's allocation minted then, on top (dilution). */
export function supplySplit(shape: Pick<CurveShape, 'supply' | 'curveSupply' | 'poolSupply'>, allocationPercent: number): { curve: number; pool: number; allocation: number; total: number; dilution: number } {
  const allocation = (shape.supply * Math.max(0, allocationPercent)) / 100;
  const total = shape.supply + allocation;
  return { curve: shape.curveSupply, pool: shape.poolSupply, allocation, total, dilution: total > 0 ? (allocation / total) * 100 : 0 };
}

/** The split of the fee: the creator's part and the pool's, in percent of each trade. */
export function feeSplit(fee: number, creatorShare: number): { creator: number; pool: number } {
  const creator = (fee * creatorShare) / 100;
  return { creator, pool: fee - creator };
}

/** The rules as numbers (the node gives amounts as text or numbers). */
export function rulesOf(rules: LaunchRules | null | undefined): { graduation: number; supply: number; fee: number; creatorShare: number; maxAllocation: number; active: boolean } {
  const r = rules ?? DEFAULT_RULES;
  return { graduation: approx(r.graduation), supply: approx(r.supply), fee: approx(r.fee), creatorShare: approx(r.creator_share), maxAllocation: approx(r.max_allocation), active: r.active === true };
}

// --- Launching ---

export interface LaunchOptions {
  displayName: string;
  /** Base64 of the logo, or ''. */
  logo: string;
  info: string;
  allocation: number;
  /** CHN as exact text ('' or '0': none). */
  firstBuy: string;
  minBuy: string;
  maxBuy: string;
}

/** The options of launchtoken: only those given. */
export function launchOptions(o: LaunchOptions): Record<string, string | number> {
  const out: Record<string, string | number> = {};
  if (o.displayName) out.display_name = o.displayName;
  if (o.logo) out.logo = o.logo;
  if (o.info.trim()) out.info = o.info;
  if (o.allocation > 0) out.allocation = o.allocation;
  const given = (s: string) => s.trim() !== '' && Number(s) > 0;
  if (given(o.firstBuy)) out.first_buy = o.firstBuy;
  if (given(o.minBuy)) out.min_buy = o.minBuy;
  if (given(o.maxBuy)) out.max_buy = o.maxBuy;
  return out;
}

/** What is wrong with the options, in the order of the form ('' each when nothing). */
export function launchProblems(o: { allocation: number; maxAllocation: number; firstBuy: bigint | null; minBuy: bigint | null; maxBuy: bigint | null; infoBytes: number; maxInfo: number; minChn: bigint }): string[] {
  const out: string[] = [];
  if (!(o.allocation >= 0 && o.allocation <= o.maxAllocation)) out.push(`The allocation is 0 to ${o.maxAllocation}%.`);
  if (o.infoBytes > o.maxInfo) out.push(`The description is at most ${o.maxInfo} bytes.`);
  if (o.firstBuy !== null && o.firstBuy > 0n && o.firstBuy < o.minChn) out.push('The first buy pays 0.0005 ECX at least.');
  if (o.minBuy !== null && o.maxBuy !== null && o.maxBuy > 0n && o.maxBuy < o.minBuy) out.push('The most a buy pays is less than the least.');
  if (o.firstBuy !== null && o.firstBuy > 0n && ((o.minBuy !== null && o.minBuy > 0n && o.firstBuy < o.minBuy) || (o.maxBuy !== null && o.maxBuy > 0n && o.firstBuy > o.maxBuy))) out.push('The first buy is outside the least and the most a buy pays: it would be refunded.');
  return out;
}

/** What the creator can be paid now (claimlaunch): its fees (from 0.0005 CHN), its allocation once graduated and not minted. */
export function claimable(l: Pick<Launch, 'creator_fees_owed' | 'status' | 'allocation' | 'allocation_claimed'>, minChn = 0.0005): { fees: boolean; allocation: boolean } {
  return { fees: approx(l.creator_fees_owed) >= minChn, allocation: l.status === 'graduated' && l.allocation > 0 && !l.allocation_claimed };
}
