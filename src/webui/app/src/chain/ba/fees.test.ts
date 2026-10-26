// Where the fees go (ba/fees.ts): a swap's split and a launch's in words, the providers' rate.
import { describe, expect, it } from 'vitest';
import { launchFeeParts, launchFeeSplit, formatShare, providersRate, swapFeeParts, swapFeeSplit } from './fees';

describe('the parts of the fees', () => {
  it('says where a swap fee goes: 85% to its providers, 15% to the security', () => {
    expect(swapFeeSplit({ liquidity_providers: 85, security: 15 })).toBe("85% to its providers, 15% to the chain's security");
    expect(swapFeeSplit({ liquidity_providers: 100, security: 0 })).toBe('100% to its providers');
    expect(swapFeeSplit(null)).toBe('all to its providers');
  });

  it('reads the parts of getdexinfo, and none from an older node', () => {
    expect(swapFeeParts({ liquidity_providers: 85, security: '15' })).toEqual({ liquidity_providers: 85, security: 15 });
    expect(swapFeeParts(undefined)).toBeNull();
    expect(swapFeeParts({ liquidity_providers: 85 })).toBeNull();
    expect(providersRate(0.3, { liquidity_providers: 85, security: 15 })).toBeCloseTo(0.255);
    expect(providersRate(0.3, null)).toBe(0.3);
    expect(formatShare(11.00004)).toBe('11');
    expect(formatShare(0.12345)).toBe('0.1235');
  });

  it("says where a launch's fee goes: 80% to you or the creator, 20% to the security, any rest into the pool", () => {
    const p = launchFeeParts({ fee: 1.15, creator_share: 80, security_share: 20 });
    expect(launchFeeSplit(p, true)).toBe("1.15% of each trade: 80% to you, 20% to the chain's security");
    expect(launchFeeSplit(p)).toBe("1.15% of each trade: 80% to the creator, 20% to the chain's security");
    // An older node: the creator's part, the rest into the pool.
    expect(launchFeeSplit(launchFeeParts({ fee: 1, creator_share: 30 }), true)).toBe('1% of each trade: 30% to you, 70% into the pool at graduation');
  });
});
