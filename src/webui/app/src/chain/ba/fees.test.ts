// Where the fees go (ba/fees.ts): a swap's split and a launch's in words, the providers' rate, the treasury's rows.
import { describe, expect, it } from 'vitest';
import { launchFeeParts, launchFeeSplit, formatShare, providersRate, swapFeeParts, swapFeeSplit, treasuryRows } from './fees';
import { CHN_ID } from './model';
import { TICKER } from '../config';

describe('the parts of the fees', () => {
  it('says where a swap fee goes, the treasury only when it has a part', () => {
    expect(swapFeeSplit({ liquidity_providers: 89, security: 11, protocol: 0 })).toBe("89% to its providers, 11% to the chain's security");
    expect(swapFeeSplit({ liquidity_providers: 85, security: 11, protocol: 4 })).toBe("85% to its providers, 11% to the chain's security, 4% to the treasury");
    expect(swapFeeSplit({ liquidity_providers: 100, security: 0, protocol: 0 })).toBe('100% to its providers');
    expect(swapFeeSplit(null)).toBe('all to its providers');
  });

  it('reads the parts of getdexinfo, and none from an older node', () => {
    expect(swapFeeParts({ liquidity_providers: 89, security: '11', protocol: 0 })).toEqual({ liquidity_providers: 89, security: 11, protocol: 0 });
    expect(swapFeeParts(undefined)).toBeNull();
    expect(swapFeeParts({ liquidity_providers: 89 })).toBeNull();
    expect(providersRate(0.3, { liquidity_providers: 89, security: 11, protocol: 0 })).toBeCloseTo(0.267);
    expect(providersRate(0.3, null)).toBe(0.3);
    expect(formatShare(11.00004)).toBe('11');
    expect(formatShare(0.12345)).toBe('0.1235');
  });

  it("says where a launch's fee goes: to you or the creator, the treasury and the security, the rest into the pool", () => {
    const p = launchFeeParts({ fee: 1.15, creator_share: 50, protocol_share: 30, security_share: 20 });
    expect(launchFeeSplit(p, true)).toBe("1.15% of each trade: 50% to you, 30% to the treasury, 20% to the chain's security");
    expect(launchFeeSplit(launchFeeParts({ fee: 1, creator_share: 50, protocol_share: 0, security_share: 20 }))).toBe("1% of each trade: 50% to the creator, 20% to the chain's security, 30% into the pool at graduation");
    // An older node: the creator's part, the rest into the pool.
    expect(launchFeeSplit(launchFeeParts({ fee: 1, creator_share: 30 }), true)).toBe('1% of each trade: 30% to you, 70% into the pool at graduation');
  });
});

describe('the treasury', () => {
  it('puts the chain\'s coin first, by its ticker, then what holds most, the empty last', () => {
    const rows = treasuryRows([
      { asset: 'GOLD', asset_id: 'a'.repeat(64), balance: 1.5, from_swaps: 1.5, from_launches: 0, security_held: 0.2 },
      { asset: 'ZINC', asset_id: 'c'.repeat(64), balance: 0, from_swaps: 0, from_launches: 0, security_held: 0 },
      { asset: 'USDX', asset_id: 'b'.repeat(64), balance: '20.00', from_swaps: '20.00', from_launches: 0, security_held: 0 },
      { asset: 'whatever', asset_id: CHN_ID, balance: '0.5', from_swaps: '0.1', from_launches: '0.4', security_held: 0 },
    ]);
    expect(rows.map((r) => r.label)).toEqual([TICKER, 'USDX', 'GOLD', 'ZINC']);
    expect(rows[0]).toMatchObject({ id: CHN_ID, balance: '0.5', fromSwaps: '0.1', fromLaunches: '0.4', empty: false });
    expect(rows.at(-1)!.empty).toBe(true);
    expect(treasuryRows(null)).toEqual([]);
  });
});
