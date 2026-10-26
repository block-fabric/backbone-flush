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
/** What getlaunchstats' rules say for a new launch (v6: 100 CHN to graduate), for a node that did not answer yet; a launch made before keeps its own (500): each launch's graduation is its own field. */
export const DEFAULT_RULES: LaunchRules = { graduation: 100, fee: 1, creator_share: 30, supply: 1_000_000_000, curve_supply: 750_000_000, pool_supply: 250_000_000, max_allocation: 5, log_blocks: 10_080, active: false };

export interface LaunchRules extends LaunchParamRules {
  /** CHN a curve raises before it graduates. */
  graduation: Amount;
  /** A trade's fee on a curve, in percent, and the creator's part of it, in percent of the fee. */
  fee: number;
  creator_share: number;
  /** The chain's security's part of it, in percent of the fee (newer nodes; ba/fees.ts). */
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
  /** v7, launch fee split (newer nodes): whether the next block takes a launch's fee_split (the tunable part of the
   *  fee, creator_share percent of it, split between the creator, up to 3 addresses and buyback & burn). */
  fee_split_active?: boolean;
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
  /** The chain's security's part of the fee, in percent of it, and what it was paid so far (CHN; newer nodes). */
  security_share?: number;
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
  /** v7: the split of its tunable fee part (and of the creator's part of its graduated position's fees), each share in
   *  basis points of it, with what it was paid and is owed (CHN); a v5 launch's mapped from its fee_destination. */
  fee_split?: FeeShare[];
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
  if (o.keepControl && !o.mintableConfirmed) out.push('Confirm that the asset keeps an unlocked supply.');
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

// --- v7, launch fee split ---

/** A share of a launch's tunable fee part (launchtoken fee_split, getlaunch fee_split): its recipient, its basis points
 *  of the part; from getlaunch also its kind and what it was paid and is owed. */
export interface FeeShare {
  to: string;
  bps: number;
  kind?: 'creator' | 'burn' | 'address';
  paid?: Amount;
  owed?: Amount;
}

/** A split as the wizard edits it: percents of the tunable part (creator, burn, each extra address). */
export interface FeeSplitDraft {
  creator: number;
  burn: number;
  addresses: { address: string; percent: number }[];
}

/** The most a split has: 5 recipients, of which 3 addresses (consensus, v7). */
export const MAX_SPLIT_ENTRIES = 5;
export const MAX_SPLIT_ADDRESSES = 3;
export const SPLIT_BPS = 10_000;

/** The kind of a recipient. */
export function shareKind(to: string): 'creator' | 'burn' | 'address' {
  return to === 'creator' ? 'creator' : to === 'burn' ? 'burn' : 'address';
}

/**
 * A draft's shares in basis points: creator first, then the addresses, then burn; a 0% creator or burn left out (an
 * address is kept, so that its 0 is a problem). Percents that sum to 100 (to within 0.01) round to bps that sum to
 * 10000 exactly (largest remainders); else as they round, for feeSplitProblems to say.
 */
export function feeSplitOf(d: FeeSplitDraft): FeeShare[] {
  const raw: { to: string; p: number }[] = [];
  const pct = (x: number) => (Number.isFinite(x) ? Math.max(0, x) : 0);
  if (pct(d.creator) > 0) raw.push({ to: 'creator', p: pct(d.creator) });
  for (const a of d.addresses) raw.push({ to: a.address.trim(), p: pct(a.percent) });
  if (pct(d.burn) > 0) raw.push({ to: 'burn', p: pct(d.burn) });
  const exact = raw.map((r) => (r.p * SPLIT_BPS) / 100);
  const out = exact.map((x, i) => ({ to: raw[i].to, bps: Math.floor(x + 1e-9) }));
  const total = exact.reduce((a, b) => a + b, 0);
  if (Math.abs(total - SPLIT_BPS) <= 1 + 1e-9) {
    let rest = SPLIT_BPS - out.reduce((a, b) => a + b.bps, 0);
    const order = exact.map((x, i) => ({ i, r: x - out[i].bps })).sort((a, b) => b.r - a.r || a.i - b.i);
    for (let k = 0; rest > 0 && order.length; k = (k + 1) % order.length, rest--) out[order[k].i].bps++;
    for (let k = order.length - 1; rest < 0 && k >= 0; k--) if (out[order[k].i].bps > 1) { out[order[k].i].bps--; rest++; }
  } else {
    for (let i = 0; i < out.length; i++) out[i].bps = Math.round(exact[i]);
  }
  return out;
}

/** Text that is a recipient address (lowercased when bech32), to compare. */
function addressKey(to: string): string {
  const t = to.trim();
  return /^[a-z0-9]+1/i.test(t) && (t === t.toLowerCase() || t === t.toUpperCase()) ? t.toLowerCase() : t;
}

/** What is wrong with a split (the chain's rules, v7): at most 5 shares and 3 addresses, each of a whole 1 bps or
 *  more, creator and burn at most once each, addresses given and distinct, summing to 10000 bps (100%). */
export function feeSplitProblems(split: readonly FeeShare[]): string[] {
  const out: string[] = [];
  if (split.length === 0) return ['Give the fees at least one recipient.'];
  if (split.length > MAX_SPLIT_ENTRIES) out.push(t('A split has at most %1 recipients.', String(MAX_SPLIT_ENTRIES)));
  const addrs = split.filter((s) => shareKind(s.to) === 'address');
  if (addrs.length > MAX_SPLIT_ADDRESSES) out.push(t('A split has at most %1 addresses.', String(MAX_SPLIT_ADDRESSES)));
  if (split.filter((s) => s.to === 'creator').length > 1) out.push('The creator is in the split once at most.');
  if (split.filter((s) => s.to === 'burn').length > 1) out.push('Buyback & burn is in the split once at most.');
  if (addrs.some((s) => !s.to.trim())) out.push('Enter each address of the split.');
  const keys = addrs.map((s) => addressKey(s.to)).filter(Boolean);
  if (new Set(keys).size !== keys.length) out.push('Each address of the split is a different one.');
  if (split.some((s) => !Number.isInteger(s.bps) || s.bps < 1)) out.push('Each share of the split is at least 0.01%.');
  const sum = split.reduce((a, s) => a + (Number.isFinite(s.bps) ? s.bps : 0), 0);
  if (sum !== SPLIT_BPS) out.push(t('The shares add up to %1%, not 100%.', formatBps(sum)));
  return out;
}

/** launchtoken's fee_split option: none when all goes to the creator (the default). */
export function feeSplitOptions(split: readonly FeeShare[]): { fee_split?: { to: string; bps: number }[] } {
  if (split.length === 1 && split[0].to === 'creator' && split[0].bps === SPLIT_BPS) return {};
  return { fee_split: split.map((s) => ({ to: s.kind === 'address' || shareKind(s.to) === 'address' ? s.to.trim() : s.to, bps: s.bps })) };
}

/** Basis points as a percent, at most 2 decimals ("75", "33.33"). */
export function formatBps(bps: number): string {
  return String(Number((bps / 100).toFixed(2)));
}

/** A share's part of each trade, in percent: bps of the tunable part (tunable percent of the fee, fee percent of a trade). */
export function shareOfTrade(bps: number, tunable: number, fee: number): number {
  return (bps / SPLIT_BPS) * (tunable / 100) * fee;
}

/** A launch's split: its fee_split, else (a node from before v7) its fee_destination mapped as v7 maps it; [] before v5. */
export function feeSplitOfLaunch(l: Pick<Launch, 'fee_split' | 'fee_flow' | 'fee_destination' | 'burn_percent'>): FeeShare[] {
  if (Array.isArray(l.fee_split) && l.fee_split.length) return l.fee_split.map((s) => ({ ...s, bps: Number(s.bps) || 0, kind: s.kind ?? shareKind(s.to) }));
  if (!l.fee_flow) return [];
  if (l.fee_destination === 'burn') return [{ to: 'burn', kind: 'burn', bps: SPLIT_BPS }];
  if (l.fee_destination === 'split') {
    const b = Math.round(Math.max(0, Math.min(100, Number(l.burn_percent) || 0)) * 100);
    return ([{ to: 'burn', kind: 'burn', bps: b }, { to: 'creator', kind: 'creator', bps: SPLIT_BPS - b }] as FeeShare[]).filter((s) => s.bps > 0);
  }
  return [{ to: 'creator', kind: 'creator', bps: SPLIT_BPS }];
}

/** Who a share goes to, in the words of the app (made with t()): "The creator", "Buyback & burn", or its address. */
export function shareRecipientText(s: Pick<FeeShare, 'to'>): string {
  const k = shareKind(s.to);
  return k === 'creator' ? t('The creator') : k === 'burn' ? t('Buyback & burn') : s.to;
}

/** A share in words (made with t()): "75% of the creator part (0.5625% of each trade)". */
export function shareText(s: Pick<FeeShare, 'bps'>, tunable: number, fee: number): string {
  return t('%1% of the tunable part (%2% of each trade)', formatBps(s.bps), String(Number(shareOfTrade(s.bps, tunable, fee).toFixed(4))));
}
