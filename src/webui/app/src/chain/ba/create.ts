// The one wizard that creates an asset (ba/CreateWizard.svelte: "Register an asset" and "Launch" open it), and opening a
// market for an asset that exists (ba/OpenMarket.svelte): the rules of a launch's parameters (consensus "launchparams":
// its supply and decimals, its curve's trade fee; getlaunchstats' rules), what the wizard sends for each market (none:
// reserveasset then registerasset; a bonding curve: reserveasset then launchtoken; straight into a pool: the asset
// registered first, then its pool made and funded), and whether an asset can be put on a curve (curveasset). Pure:
// no state, no calls (tested in chain/ba/create.test.ts).
import { approx, MAX_AMOUNT, type Amount } from './model';
import { TICKER } from '../config';

/**
 * The three ways of the wizard (user decision, 2026-10-23): no market yet; a bonding curve; straight into a pool, a
 * concentrated pool with ECX at the creator's price, funded by the creator over the full range (reserveasset then
 * registerasset, then createpool and openposition once each step before is in a block: ba/store.svelte.ts
 * PendingFund). 'auction' is no way of the wizard any more (an auction is started from the asset's page), only read back
 * from journals of before.
 */
export type MarketKind = 'none' | 'curve' | 'pool' | 'auction';
export const MARKET_KINDS: readonly { id: MarketKind; label: string; summary: string }[] = [
  { id: 'none', label: 'No market', summary: 'Register it alone, the supply to your wallet; open a pool any time later.' },
  { id: 'curve', label: 'Bonding curve', summary: 'Sold from a curve until it raises enough, then a locked pool.' },
  { id: 'pool', label: 'Straight into a pool', summary: `A pool with ${TICKER} at your price, funded with your ${TICKER} and tokens over the full range.` },
];

/** The launch parameters of the rules (getlaunchstats' rules; the defaults of a node from before them). */
export interface LaunchParams {
  /** Whether the next block takes a launch's own supply, decimals and fee (and curveasset). */
  active: boolean;
  /** The first block of them, -1 never. */
  height: number;
  /** The curve's trade fee, in percent: the least, the most, the default. */
  feeMin: number;
  feeMax: number;
  feeDefault: number;
  /** A curve's supply in units: the least, the most, the multiple it is of. */
  minUnits: bigint;
  maxUnits: bigint;
  multiple: bigint;
  decimalsMax: number;
  decimalsDefault: number;
  /** The default supply, whole. */
  supplyDefault: string;
}

/** The parameters every launch had before "launchparams": a billion of 6 decimals, 1.15 %. */
export const FIXED_PARAMS: LaunchParams = {
  active: false,
  height: -1,
  feeMin: 1.15,
  feeMax: 1.15,
  feeDefault: 1.15,
  minUnits: 1_000_000_000_000_000n,
  maxUnits: 1_000_000_000_000_000n,
  multiple: 8n,
  decimalsMax: 6,
  decimalsDefault: 6,
  supplyDefault: '1000000000',
};

/** The rules' fields of launchparams, as getlaunchstats gives them (absent: a node from before them). */
export interface LaunchParamRules {
  fee?: Amount;
  params_active?: boolean;
  params_height?: number;
  fee_min?: Amount;
  fee_max?: Amount;
  fee_default?: Amount;
  supply_min_units?: Amount;
  supply_max_units?: Amount;
  supply_multiple_units?: Amount;
  decimals_max?: number;
  decimals_default?: number;
  supply_default?: Amount;
}

const big = (v: Amount | undefined, fallback: bigint): bigint => {
  if (v === undefined || v === null) return fallback;
  try {
    const s = String(v).trim();
    return /^\d+$/.test(s) ? BigInt(s) : fallback;
  } catch {
    return fallback;
  }
};

/** The launch parameters of the rules: those of before "launchparams" (fixed) unless the node says they apply. */
export function launchParamsOf(rules: LaunchParamRules | null | undefined): LaunchParams {
  const fee = rules?.fee !== undefined ? approx(rules.fee) : FIXED_PARAMS.feeDefault;
  if (!rules || rules.params_active !== true) {
    // The fee of a launch now (the network's), fixed; the height they come at, if the node says.
    return { ...FIXED_PARAMS, feeMin: fee, feeMax: fee, feeDefault: fee, height: Number.isInteger(rules?.params_height) ? Number(rules?.params_height) : -1 };
  }
  const feeMin = rules.fee_min !== undefined ? approx(rules.fee_min) : 0.3;
  const feeMax = rules.fee_max !== undefined ? approx(rules.fee_max) : 5;
  const feeDefault = rules.fee_default !== undefined ? approx(rules.fee_default) : fee;
  return {
    active: true,
    height: Number(rules.params_height ?? 0),
    feeMin,
    feeMax,
    feeDefault: Math.min(feeMax, Math.max(feeMin, feeDefault)),
    minUnits: big(rules.supply_min_units, 1_000_000n),
    maxUnits: big(rules.supply_max_units, 8_198_552_921_648_689_600n),
    multiple: big(rules.supply_multiple_units, 8n) || 8n,
    decimalsMax: Number.isInteger(rules.decimals_max) ? Number(rules.decimals_max) : 12,
    decimalsDefault: Number.isInteger(rules.decimals_default) ? Number(rules.decimals_default) : 6,
    supplyDefault: rules.supply_default !== undefined ? String(rules.supply_default) : '1000000000',
  };
}

/** A fee in percent from basis points (the slider's steps of 0.05 %), and back. */
export const feeFromBps = (bps: number): number => Math.round(bps) / 100;
export const bpsOf = (percent: number): number => Math.round(percent * 100);

/** What is wrong with a curve's supply in units ('' when the rules take it). */
export function curveSupplyProblem(units: bigint | null, decimals: number, p: LaunchParams): string {
  if (units === null) return 'Type the supply.';
  if (!p.active) return units === FIXED_PARAMS.minUnits && decimals === FIXED_PARAMS.decimalsDefault ? '' : 'Before the launch parameters apply, a launch has a billion units of 6 decimals.';
  if (decimals < 0 || decimals > p.decimalsMax) return `Decimals are 0 to ${p.decimalsMax}.`;
  if (units < p.minUnits) return `A curve needs ${p.minUnits.toLocaleString('en-US')} units at least (with ${decimals} decimals: ${unitsText(p.minUnits, decimals)}).`;
  if (units > p.maxUnits) return `At most ${unitsText(p.maxUnits, decimals)} with ${decimals} decimals.`;
  if (units % p.multiple !== 0n) return `A curve's supply is a multiple of ${p.multiple} units (a quarter of it, then half that, are whole): ${unitsText(units - (units % p.multiple), decimals)} or ${unitsText(units - (units % p.multiple) + p.multiple, decimals)}.`;
  return '';
}

/** Units as whole text (no grouping), exact. */
export function unitsText(units: bigint, decimals: number): string {
  if (decimals <= 0) return units.toString();
  const s = units.toString().padStart(decimals + 1, '0');
  const whole = s.slice(0, -decimals);
  const frac = s.slice(-decimals).replace(/0+$/, '');
  return frac ? `${whole}.${frac}` : whole;
}

/** What a launch's supply splits into: 3/4 on the curve, 1/4 into the locked pool at graduation (units). */
export function curveSplit(units: bigint): { curve: bigint; pool: bigint } {
  const pool = units / 4n;
  return { curve: units - pool, pool };
}

/** The fee's parts on a trade of `amount` coins: creator, treasury, security (percent of the fee each). */
export function feeOf(amount: number, feePercent: number): number {
  return (amount * feePercent) / 100;
}

/** The options of launchtoken: what it is, the buys' bounds, and from "launchparams" its supply, decimals and fee. */
export function launchTokenOptions(o: {
  displayName: string;
  logo: string;
  info: string;
  allocation: number;
  firstBuy: string;
  minBuy: string;
  maxBuy: string;
  params: LaunchParams;
  supply: string;
  decimals: number;
  feePercent: number;
}): Record<string, string | number> {
  const out: Record<string, string | number> = {};
  if (o.displayName) out.display_name = o.displayName;
  if (o.logo) out.logo = o.logo;
  if (o.info.trim()) out.info = o.info;
  if (o.allocation > 0) out.allocation = o.allocation;
  const given = (s: string) => s.trim() !== '' && Number(s) > 0;
  if (given(o.firstBuy)) out.first_buy = o.firstBuy;
  if (given(o.minBuy)) out.min_buy = o.minBuy;
  if (given(o.maxBuy)) out.max_buy = o.maxBuy;
  // Only where the rules take them: a node from before them refuses an option it does not know.
  if (o.params.active) {
    out.supply = o.supply;
    out.decimals = o.decimals;
    out.fee = Number(o.feePercent.toFixed(2));
  }
  return out;
}

/** The parameters of curveasset: the asset by id, the amount it puts on the curve (whole), its options. */
export function curveAssetParams(assetArg: string, amount: string, o: { feePercent: number; minBuy?: string; maxBuy?: string }): unknown[] {
  const options: Record<string, string | number> = { fee: Number(o.feePercent.toFixed(2)) };
  if (o.minBuy && Number(o.minBuy) > 0) options.min_buy = o.minBuy;
  if (o.maxBuy && Number(o.maxBuy) > 0) options.max_buy = o.maxBuy;
  return [assetArg, amount, options];
}

/** Whether an asset can be put on a curve now, and why not (the rules of curveasset, as far as the page knows). */
export function canCurve(o: { params: LaunchParams; controlled: boolean; launched: boolean; ecxPool: boolean; heldUnits: bigint }): { ok: boolean; reason: string } {
  if (!o.params.active) return { ok: false, reason: o.params.height >= 0 ? `Curves for assets that exist come with the launch parameters, from block ${o.params.height}.` : 'This network has no curves for assets that exist yet.' };
  if (o.launched) return { ok: false, reason: 'It was launched on a curve already.' };
  if (o.ecxPool) return { ok: false, reason: `It has a pool with ${TICKER} already: a curve graduates into that pool.` };
  if (!o.controlled) return { ok: false, reason: 'Only its controller (the holder of its control coin) can put it on a curve.' };
  if (o.heldUnits < o.params.minUnits) return { ok: false, reason: 'This wallet holds less of it than a curve needs.' };
  return { ok: true, reason: '' };
}

/** The most units a curve can take of what the wallet holds: a multiple of the rules' multiple, within their bounds. */
export function curveMaxUnits(held: bigint, p: LaunchParams): bigint {
  const top = held < p.maxUnits ? held : p.maxUnits;
  return top - (top % p.multiple);
}

/** The whole supply of the default curve, as text: 1e9. */
export const DEFAULT_CURVE_SUPPLY = '1000000000';

/** Units of a whole amount with decimals, or null when it does not fit (MAX_AMOUNT). */
export function wholeUnits(whole: string, decimals: number): bigint | null {
  const s = whole.trim().replace(/[ _,]/g, '');
  const m = /^(\d+)(?:\.(\d*))?$/.exec(s);
  if (!m) return null;
  const frac = (m[2] ?? '').replace(/0+$/, '');
  if (frac.length > decimals) return null;
  const units = BigInt(m[1]) * 10n ** BigInt(decimals) + (frac ? BigInt(frac.padEnd(decimals, '0')) : 0n);
  return units > MAX_AMOUNT ? null : units;
}
