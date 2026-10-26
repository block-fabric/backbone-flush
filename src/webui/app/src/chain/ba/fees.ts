// Where the fees go (consensus parameters of the network: getdexinfo swap_fee_parts, getlaunch and getlaunchstats' rules):
// a swap's fee split between the pool's providers (what stays in the pool) and the chain's security (85 / 15); a launch's
// curve fee between its creator and the security (80 / 20); a graduated position's fees 60 / 40. The splits in words
// (made with t()), tested in chain/ba/fees.test.ts.
import { t } from '../../lib/i18n.svelte';
import { approx } from './model';

/** The parts of every swap fee, in percent of it (getdexinfo swap_fee_parts). */
export interface SwapFeeParts {
  liquidity_providers: number;
  security: number;
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
  const parts = { liquidity_providers: n(o.liquidity_providers), security: n(o.security) };
  return Object.values(parts).every((x) => Number.isFinite(x) && x >= 0) ? parts : null;
}

/**
 * Where a swap's fee goes, in words: "85% to its providers, 15% to the chain's security"; null parts (an older node): all
 * to its providers.
 */
export function swapFeeSplit(parts: SwapFeeParts | null | undefined): string {
  if (!parts) return t('all to its providers');
  const out = [t('%1% to its providers', formatShare(parts.liquidity_providers))];
  if (parts.security > 0) out.push(t('%1% to the chain\'s security', formatShare(parts.security)));
  return out.join(', ');
}

/** The part of a fee rate that stays with the providers, in percent of what goes in (0.3% at 85%: 0.255%). */
export function providersRate(fee: number, parts: SwapFeeParts | null | undefined): number {
  return parts ? (fee * parts.liquidity_providers) / 100 : fee;
}

/** A launch's fee and its parts, in percent (of each trade; of the fee). */
export interface LaunchFeeParts {
  fee: number;
  creator: number;
  security: number;
}

/** The parts of a launch's fee from getlaunch or getlaunchstats' rules (creator_share, security_share). */
export function launchFeeParts(r: { fee?: unknown; creator_share?: unknown; security_share?: unknown } | null | undefined): LaunchFeeParts {
  const n = (v: unknown) => (typeof v === 'number' || typeof v === 'string' ? approx(v) : 0);
  return { fee: n(r?.fee), creator: n(r?.creator_share), security: n(r?.security_share) };
}

/**
 * Where a launch's fee goes, in words: "1.15% of each trade: 80% to you, 20% to the chain's security"
 * (`you`: the creator is the reader); what the parts leave (the roundings' rest) goes into the pool at graduation.
 */
export function launchFeeSplit(p: LaunchFeeParts, you = false): string {
  const out = [you ? t('%1% to you', formatShare(p.creator)) : t('%1% to the creator', formatShare(p.creator))];
  if (p.security > 0) out.push(t('%1% to the chain\'s security', formatShare(p.security)));
  const rest = 100 - p.creator - p.security;
  if (rest > 0.00005) out.push(t('%1% into the pool at graduation', formatShare(rest)));
  return t('%1% of each trade: %2', formatShare(p.fee), out.join(', '));
}
