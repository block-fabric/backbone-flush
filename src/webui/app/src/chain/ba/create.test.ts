import { describe, expect, it } from 'vitest';
import { canCurve, curveDecimals, curveAssetParams, curveMaxUnits, curveSplit, curveSupplyProblem, FIXED_PARAMS, launchParamsOf, launchTokenOptions, unitsText, wholeUnits } from './create';
import type { Auction, Pool } from './model';

const ACTIVE = launchParamsOf({ fee: 1.15, params_active: true, params_height: 20, fee_min: 0.3, fee_max: 5, fee_default: 1.15, supply_min_units: '1000000', supply_max_units: '8198552921648689600', supply_multiple_units: 8, decimals_max: 12, decimals_default: 6, supply_default: '1000000000' });

describe('the launch parameters', () => {
  it('fixed before they apply, the rules after', () => {
    const before = launchParamsOf({ fee: 1.15, params_active: false, params_height: 40 });
    expect(before.active).toBe(false);
    expect(before.height).toBe(40);
    expect(before.feeMin).toBe(1.15);
    expect(launchParamsOf(undefined)).toMatchObject({ active: false, height: -1 });
    expect(ACTIVE).toMatchObject({ active: true, feeMin: 0.3, feeMax: 5, feeDefault: 1.15, minUnits: 1_000_000n, multiple: 8n, decimalsMax: 12 });
  });

  it("checks a curve's supply as the rules do", () => {
    expect(curveSupplyProblem(FIXED_PARAMS.minUnits, 6, FIXED_PARAMS)).toBe('');
    expect(curveSupplyProblem(8_000_000n, 6, FIXED_PARAMS)).toMatch(/billion/);
    expect(curveSupplyProblem(999_992n, 0, ACTIVE)).toMatch(/at least/);
    expect(curveSupplyProblem(1_000_001n, 0, ACTIVE)).toMatch(/multiple of 8/);
    expect(curveSupplyProblem(1_000_000n, 0, ACTIVE)).toBe('');
    expect(curveSupplyProblem(8_198_552_921_648_689_608n, 0, ACTIVE)).toMatch(/At most/);
    expect(curveSupplyProblem(2_100_000_000_000_000n, 8, ACTIVE)).toBe('');
    expect(curveSplit(1_000_000n)).toEqual({ curve: 750_000n, pool: 250_000n });
    expect(curveMaxUnits(1_000_007n, ACTIVE)).toBe(1_000_000n);
  });

  it('units and whole amounts, exactly', () => {
    expect(unitsText(2_100_000_000_000_000n, 8)).toBe('21000000');
    expect(unitsText(1_500n, 3)).toBe('1.5');
    expect(wholeUnits('21000000', 8)).toBe(2_100_000_000_000_000n);
    expect(wholeUnits('1.234', 2)).toBeNull();
    expect(wholeUnits('99999999999999999999', 0)).toBeNull();
  });

  it("launchtoken's options: supply, decimals and fee only where the rules take them", () => {
    const base = { displayName: '', logo: '', info: 'x', allocation: 0, firstBuy: '', minBuy: '', maxBuy: '', supply: '21000000', decimals: 8, feePercent: 2.5 };
    expect(launchTokenOptions({ ...base, params: FIXED_PARAMS })).toEqual({ info: 'x' });
    expect(launchTokenOptions({ ...base, params: ACTIVE })).toEqual({ info: 'x', supply: '21000000', decimals: 8, fee: 2.5 });
    expect(curveAssetParams('0xab', '80000', { feePercent: 1.15, minBuy: '0.1' })).toEqual(['0xab', '80000', { fee: 1.15, min_buy: '0.1' }]);
  });

  it('whether an asset can go on a curve', () => {
    const ok = { params: ACTIVE, controlled: true, launched: false, ecxPool: false, heldUnits: 10_000_000n };
    expect(canCurve(ok)).toEqual({ ok: true, reason: '' });
    expect(canCurve({ ...ok, params: FIXED_PARAMS }).ok).toBe(false);
    expect(canCurve({ ...ok, launched: true }).reason).toMatch(/launched/);
    expect(canCurve({ ...ok, ecxPool: true }).reason).toMatch(/pool/);
    expect(canCurve({ ...ok, controlled: false }).reason).toMatch(/controller/);
    expect(canCurve({ ...ok, heldUnits: 5n }).reason).toMatch(/less/);
  });
});

describe("a curve's decimals", () => {
  it("keeps the creator's choice when the curve is picked after the supply step", () => {
    // Set 2 on the supply step, then pick the curve: it stays 2 (it was reset to 6 before).
    expect(curveDecimals(2, true, ACTIVE)).toBe(2);
    expect(curveDecimals(4, true, ACTIVE)).toBe(4);
    // Untouched: the rules' default; above the most a curve allows: brought down to it.
    expect(curveDecimals(8, false, ACTIVE)).toBe(6);
    expect(curveDecimals(12, true, { ...ACTIVE, decimalsMax: 8 })).toBe(8);
    // Before the launch parameters: the rules' fixed 6, whatever was chosen.
    expect(curveDecimals(2, true, FIXED_PARAMS)).toBe(FIXED_PARAMS.decimalsDefault);
  });
});
