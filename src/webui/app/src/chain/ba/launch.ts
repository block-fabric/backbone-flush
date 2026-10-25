// Layers' launches as the pages see them (doc/layers.md, "Launches"; src/rpc/bitassets.cpp getlaunch, listlaunches,
// quotelaunch, getlaunchstats, getlaunchlog; src/wallet/rpc/bitassets.cpp launchtoken, buyfromlaunch, selltolaunch,
// claimlaunch): a token sold from a bonding curve until the curve has raised what it graduates at, then traded in a
// locked pool with CHN. The curve, worked out from the rules (a constant product of virtual reserves: from (G/2, 9/8·S)
// to (3/2·G, 3/8·S), G the graduation, S the supply), for its chart and the review of a launch; the queries of the
// list; the options of launchtoken. Pure: no state, no calls (tested in chain/ba/launch.test.ts).
import { approx, formatUnits, minChnPayout, unitsOf, type Amount } from './model';
import type { LaunchParamRules } from './create';
import { TICKER } from '../config';
import { t } from '../../lib/i18n.svelte';

/** A launch's token has 6 decimals and a billion units, fixed (bitassets::LAUNCH_DECIMALS, LAUNCH_SUPPLY). */
export const LAUNCH_DECIMALS = 6;
/** What getlaunchstats' rules say on every network today, for a node that did not answer yet. */
export const DEFAULT_RULES: LaunchRules = { graduation: 500, fee: 1, creator_share: 30, supply: 1_000_000_000, curve_supply: 750_000_000, pool_supply: 250_000_000, max_allocation: 5, log_blocks: 10_080, active: false };

export interface LaunchRules extends LaunchParamRules {
  /** CHN a curve raises before it graduates. */
  graduation: Amount;
  /** A trade's fee on a curve, in percent, and the creator's part of it, in percent of the fee. */
  fee: number;
  creator_share: number;
  /** The treasury's and the chain's security's parts of it, in percent of the fee (newer nodes; ba/fees.ts). */
  protocol_share?: number;
  security_share?: number;
  /** Whole tokens of a launch, of which the curve sells, and go into the pool at graduation. */
  supply: Amount;
  curve_supply: Amount;
  pool_supply: Amount;
  /** The most a creator's allocation is, in percent of the supply. */
  max_allocation: number;
  log_blocks: number;
  /** Whether the next block takes launches. */
  active: boolean;
  /** v5, fee flow (newer nodes): from its height, where a creator's fees go (fee_destination), keep_control, and a
   *  graduation into a locked full-range position at graduation_fee percent, whose fees go graduated_creator_share
   *  percent to the creator, the rest to the chain's security. */
  fee_flow_active?: boolean;
  fee_flow_height?: number;
  graduation_fee?: number;
  graduated_creator_share?: number;
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
  /** The treasury's and the chain's security's parts of the fee, in percent of it, and what they were paid so far (CHN; newer nodes). */
  protocol_share?: number;
  security_share?: number;
  protocol_fees?: Amount;
  security_fees?: Amount;
  creator_fees_owed: Amount;
  creator_fees_paid: Amount;
  allocation: number;
  allocation_claimed: boolean;
  min_buy: Amount;
  max_buy: Amount;
  volume: Amount;
  trades: number;
  graduated_height?: number;
  /** v5, fee flow (newer nodes): where the creator's fees go, and what of them was burned (whole tokens); whether its
   *  creator kept the control coin (mintable: more can be made); tokens owed to the creator (a split's part); the kind of
   *  its pool once graduated and its position there. */
  fee_flow?: boolean;
  fee_destination?: FeeDestination;
  burn_percent?: number;
  mintable?: boolean;
  burned?: Amount;
  buyback_pending?: Amount;
  creator_tokens_owed?: Amount;
  pool_kind?: 'concentrated' | 'constant_product';
  position?: string;
  /** From the launch parameters (newer nodes): its asset's decimals, what its curve sells and its pool gets (whole), and
   *  whether an asset that existed was put on the curve (curveasset). A node from before: 6, 750M, 250M. */
  decimals?: number;
  curve_supply?: Amount;
  pool_supply?: Amount;
  from_asset?: boolean;
}

/** A launch's decimals (6 on a node from before the launch parameters). */
export function launchDecimals(l: Pick<Launch, 'decimals'> | null | undefined): number {
  return Number.isInteger(l?.decimals) ? Number(l?.decimals) : LAUNCH_DECIMALS;
}

/** The whole amount a launch's curve and pool started with (what its curve is worked out from); the rules' supply on a node from before. */
export function launchSupply(l: Pick<Launch, 'curve_supply' | 'pool_supply'> | null | undefined, fallback: number): number {
  return l && l.curve_supply !== undefined && l.pool_supply !== undefined ? approx(l.curve_supply) + approx(l.pool_supply) : fallback;
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

/**
 * What My launches shows of a launch's curve, the same for one that never traded (progress 0, and no last price, volume
 * or market value: user report 2026-10-24, CORN and RUG on betanet): its progress (0 to 100, 0 when absent), whether it
 * traded, and its price, the curve's start price (start) when it has none of its own.
 */
export function launchCurveFacts(l: Pick<Launch, 'progress' | 'trades' | 'volume' | 'price' | 'graduation' | 'supply' | 'curve_supply' | 'pool_supply'>): { progress: number; traded: boolean; price: number | undefined; start: boolean } {
  const progress = Math.max(0, Math.min(100, Number(l.progress) || 0));
  const traded = (Number(l.trades) || 0) > 0 || approx(l.volume ?? 0) > 0 || progress > 0;
  const live = Number(l.price);
  if (traded && Number.isFinite(live) && live > 0) return { progress, traded, price: live, start: false };
  const shape = curveShape(approx(l.graduation ?? 0), launchSupply(l, approx(l.supply ?? 0)));
  const start = shape.startPrice;
  return { progress, traded, price: Number.isFinite(start) && start > 0 ? start : undefined, start: true };
}

/** Where a curve is once it raised `raised` CHN: the amount it sold, its price (x / y), the reserves. */
export function curvePointAt(shape: CurveShape, raised: number): { sold: number; price: number; coin: number; tokens: number } {
  const r = Math.min(Math.max(0, raised), shape.graduation);
  const coin = shape.virtualCoin + r;
  const tokens = shape.k / coin;
  return { sold: shape.virtualTokens - tokens, price: coin / tokens, coin, tokens };
}

/** The curve for its chart: its price (y) by the amount it sold (x), from the start to the end. */
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
  if (o.firstBuy !== null && o.firstBuy > 0n && o.firstBuy < o.minChn) out.push(`The first buy pays ${formatUnits(o.minChn, 8)} ${TICKER} at least.`);
  if (o.minBuy !== null && o.maxBuy !== null && o.maxBuy > 0n && o.maxBuy < o.minBuy) out.push('The most a buy pays is less than the least.');
  if (o.firstBuy !== null && o.firstBuy > 0n && ((o.minBuy !== null && o.minBuy > 0n && o.firstBuy < o.minBuy) || (o.maxBuy !== null && o.maxBuy > 0n && o.firstBuy > o.maxBuy))) out.push('The first buy is outside the least and the most a buy pays: it would be refunded.');
  return out;
}

/** What the creator can be paid now (claimlaunch): its fees (from the least payout, the node's), its allocation once graduated and not minted. */
export function claimable(l: Pick<Launch, 'creator_fees_owed' | 'status' | 'allocation' | 'allocation_claimed'> & Partial<Pick<Launch, 'creator_tokens_owed'>>, minChn = Number(minChnPayout()) / 1e8): { fees: boolean; allocation: boolean; tokens: boolean } {
  return { fees: approx(l.creator_fees_owed) >= minChn, allocation: l.status === 'graduated' && l.allocation > 0 && !l.allocation_claimed, tokens: approx(l.creator_tokens_owed ?? 0) > 0 };
}

// --- v5, fee flow ---

/** Where a launch's creator fees go (launchtoken fee_destination). */
export type FeeDestination = 'creator' | 'burn' | 'split';
export const FEE_DESTINATIONS: { id: FeeDestination; label: string }[] = [
  { id: 'creator', label: 'Creator' },
  { id: 'burn', label: 'Buy back & burn' },
  { id: 'split', label: 'Split' },
];

/** The fee-flow options of launchtoken, from the v5 height: only those that differ from the defaults. */
export function feeFlowOptions(o: { destination: FeeDestination; burnPercent: number; keepControl: boolean }): Record<string, string | number | boolean> {
  const out: Record<string, string | number | boolean> = {};
  if (o.destination !== 'creator') out.fee_destination = o.destination;
  if (o.destination === 'split') out.burn_percent = o.burnPercent;
  if (o.keepControl) out.keep_control = true;
  return out;
}

/** What is wrong with them ('' each when nothing): a split's burn above 0 and below 100; a control coin kept only on the creator's word. */
export function feeFlowProblems(o: { destination: FeeDestination; burnPercent: number; keepControl: boolean; mintableConfirmed: boolean }): string[] {
  const out: string[] = [];
  if (o.destination === 'split' && !(o.burnPercent > 0 && o.burnPercent < 100)) out.push('A split burns more than 0% and less than 100% of the fees.');
  if (o.keepControl && !o.mintableConfirmed) out.push('Confirm that the asset stays mintable.');
  return out;
}

/** Where a launch's creator fees go, in the words of the app (made with t()): "To the creator", "Bought back and burned", "40% bought back and burned, 60% to the creator". */
export function feeDestinationText(l: Pick<Launch, 'fee_destination' | 'burn_percent'>): string {
  if (l.fee_destination === 'burn') return t('Bought back and burned');
  if (l.fee_destination === 'split') {
    const b = Math.max(0, Math.min(100, Number(l.burn_percent) || 0));
    return t('%1% bought back and burned, %2% to the creator', String(b), String(100 - b));
  }
  return t('To the creator');
}

/** The kind of a graduated launch's pool, in the words of the app (made with t()); '' when the node does not say. */
export function poolKindText(l: Pick<Launch, 'pool_kind'>, graduationFee = 1): string {
  return l.pool_kind === 'concentrated' ? t('Locked full-range position, %1 %', String(graduationFee)) : l.pool_kind === 'constant_product' ? t('Locked constant-product pool') : '';
}

/**
 * The creator's allocation of a launch, as My launches shows it: its percent, its amount (whole, exact: ⌊S·bps/10000⌋
 * units as the chain mints it), the curve's progress, and what it is now: to come at graduation, to claim, or minted.
 * null: no allocation.
 */
export function creatorAllocation(l: Pick<Launch, 'supply' | 'decimals' | 'allocation' | 'allocation_claimed' | 'status' | 'progress' | 'creator_fees_owed' | 'curve_supply' | 'pool_supply'>): { percent: number; units: bigint; decimals: number; progress: number; state: 'at-graduation' | 'claimable' | 'claimed' } | null {
  if (!(l.allocation > 0)) return null;
  const decimals = launchDecimals(l);
  // The supply the allocation is of: the launch's own (curve + pool), the chain's supply on a node from before.
  const supply = l.curve_supply !== undefined && l.pool_supply !== undefined ? unitsOf(l.curve_supply, decimals) + unitsOf(l.pool_supply, decimals) : unitsOf(l.supply, decimals);
  const units = (supply * BigInt(Math.round(l.allocation * 100))) / 10_000n;
  const state = l.status !== 'graduated' ? 'at-graduation' : l.allocation_claimed ? 'claimed' : 'claimable';
  return { percent: l.allocation, units, decimals, progress: Math.max(0, Math.min(100, Number(l.progress) || 0)), state };
}
