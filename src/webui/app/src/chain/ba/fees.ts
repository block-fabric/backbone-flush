// Where the fees go (consensus parameters of the network: getdexinfo swap_fee_parts, getlaunch and getlaunchstats' rules,
// gettreasury): a swap's fee split between the pool's providers (what stays in the pool), the chain's security (the block's
// BMM bidder and validators when it is in CHN, held by the treasury per asset otherwise) and the protocol (the treasury); a
// launch's fee between its creator, the protocol and the security. The treasury, held by the chain itself (no keys, no rule
// spends from it yet), per asset. The splits in words (made with t()) and the rows of the treasury's table (tested in
// chain/ba/fees.test.ts).
import { t } from '../../lib/i18n.svelte';
import { approx, isChn, type Amount } from './model';

/** The parts of every swap fee, in percent of it (getdexinfo swap_fee_parts). */
export interface SwapFeeParts {
  liquidity_providers: number;
  security: number;
  protocol: number;
}

/** The parts of the fees as gettreasury gives them, in percent (launch_fee: the fee of a launch's trade, in percent of it). */
export interface TreasuryFeeParts {
  swap_security: number;
  swap_protocol: number;
  swap_liquidity_providers: number;
  launch_fee: number;
  launch_creator: number;
  launch_protocol: number;
  launch_security: number;
}

export interface TreasuryBalance {
  /** Its label (text of the chain). */
  asset: string;
  asset_id: string;
  balance: Amount;
  from_swaps: Amount;
  from_launches: Amount;
  security_held: Amount;
}

/** What gettreasury answers. */
export interface Treasury {
  fee_parts: TreasuryFeeParts;
  balances: TreasuryBalance[];
  /** The security parts paid with the blocks' fees, in all (CHN). */
  security_paid: Amount;
  spending_rule: boolean;
}

/** A percent as the node writes it: at most 4 decimals, no trailing zeros ("89", "11", "0.5"). */
export function formatShare(x: number): string {
  return Number.isFinite(x) ? String(Number(x.toFixed(4))) : '–';
}

/** The parts of the swap fees as numbers; null when the node does not say them (an older node). */
export function swapFeeParts(raw: unknown): SwapFeeParts | null {
  if (!raw || typeof raw !== 'object') return null;
  const o = raw as Record<string, unknown>;
  const n = (v: unknown) => (typeof v === 'number' || typeof v === 'string' ? approx(v) : NaN);
  const parts = { liquidity_providers: n(o.liquidity_providers), security: n(o.security), protocol: n(o.protocol) };
  return Object.values(parts).every((x) => Number.isFinite(x) && x >= 0) ? parts : null;
}

/**
 * Where a swap's fee goes, in words: "89% to its providers, 11% to the chain's security", the treasury's part after them when
 * there is one; null parts (an older node): all to its providers.
 */
export function swapFeeSplit(parts: SwapFeeParts | null | undefined): string {
  if (!parts) return t('all to its providers');
  const out = [t('%1% to its providers', formatShare(parts.liquidity_providers))];
  if (parts.security > 0) out.push(t('%1% to the chain\'s security', formatShare(parts.security)));
  if (parts.protocol > 0) out.push(t('%1% to the treasury', formatShare(parts.protocol)));
  return out.join(', ');
}

/** The part of a fee rate that stays with the providers, in percent of what goes in (0.3% at 89%: 0.267%). */
export function providersRate(fee: number, parts: SwapFeeParts | null | undefined): number {
  return parts ? (fee * parts.liquidity_providers) / 100 : fee;
}

/** A launch's fee and its parts, in percent (of each trade; of the fee). */
export interface LaunchFeeParts {
  fee: number;
  creator: number;
  protocol: number;
  security: number;
}

/** The parts of a launch's fee from getlaunch or getlaunchstats' rules (creator_share, protocol_share, security_share). */
export function launchFeeParts(r: { fee?: unknown; creator_share?: unknown; protocol_share?: unknown; security_share?: unknown } | null | undefined): LaunchFeeParts {
  const n = (v: unknown) => (typeof v === 'number' || typeof v === 'string' ? approx(v) : 0);
  return { fee: n(r?.fee), creator: n(r?.creator_share), protocol: n(r?.protocol_share), security: n(r?.security_share) };
}

/**
 * Where a launch's fee goes, in words: "1.15% of each trade: 50% to you, 30% to the treasury, 20% to the chain's security"
 * (`you`: the creator is the reader); what the parts leave (the roundings' rest) goes into the pool at graduation.
 */
export function launchFeeSplit(p: LaunchFeeParts, you = false): string {
  const out = [you ? t('%1% to you', formatShare(p.creator)) : t('%1% to the creator', formatShare(p.creator))];
  if (p.protocol > 0) out.push(t('%1% to the treasury', formatShare(p.protocol)));
  if (p.security > 0) out.push(t('%1% to the chain\'s security', formatShare(p.security)));
  const rest = 100 - p.creator - p.protocol - p.security;
  if (rest > 0.00005) out.push(t('%1% into the pool at graduation', formatShare(rest)));
  return t('%1% of each trade: %2', formatShare(p.fee), out.join(', '));
}

/** A row of the treasury's table. */
export interface TreasuryRow {
  id: string;
  /** Text of the chain. */
  label: string;
  balance: Amount;
  fromSwaps: Amount;
  fromLaunches: Amount;
  securityHeld: Amount;
  /** Whether it holds anything at all. */
  empty: boolean;
}

/** The rows of the treasury: CHN first, then the largest balances (by their units as numbers), then by label. */
export function treasuryRows(list: readonly TreasuryBalance[] | null | undefined): TreasuryRow[] {
  const rows = (list ?? [])
    .filter((b) => b && typeof b.asset_id === 'string')
    .map((b) => ({ id: b.asset_id, label: isChn(b.asset_id) ? 'CHN' : String(b.asset ?? ''), balance: b.balance ?? 0, fromSwaps: b.from_swaps ?? 0, fromLaunches: b.from_launches ?? 0, securityHeld: b.security_held ?? 0, empty: approx(b.balance) <= 0 && approx(b.security_held) <= 0 }));
  return rows.sort((x, y) => Number(isChn(y.id)) - Number(isChn(x.id)) || Number(x.empty) - Number(y.empty) || approx(y.balance) - approx(x.balance) || x.label.localeCompare(y.label));
}
