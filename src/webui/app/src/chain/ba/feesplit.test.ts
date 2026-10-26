// v7's launch fee split (ba/launch.ts): a draft's percents into basis points summing to 10000, what the chain rejects,
// launchtoken's fee_split option (none for the default), a launch's split (v7's, or v5's mapped) and its words.
import { describe, expect, it } from 'vitest';
import { feeSplitOf, feeSplitOfLaunch, feeSplitOptions, feeSplitProblems, formatBps, shareOfTrade, shareRecipientText, shareText } from './launch';

const A = 'rchn1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080';
const B = 'rchn1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3qccfmv3';
const C = 'rchn1qxyz';
const sum = (s: { bps: number }[]) => s.reduce((a, x) => a + x.bps, 0);

describe('feeSplitOf: percents into basis points', () => {
  it('the default: all to the creator', () => {
    expect(feeSplitOf({ creator: 100, burn: 0, addresses: [] })).toEqual([{ to: 'creator', bps: 10000 }]);
  });
  it('creator first, the addresses, burn last; 0% creator or burn left out', () => {
    expect(feeSplitOf({ creator: 60, burn: 15, addresses: [{ address: ` ${A} `, percent: 25 }] })).toEqual([{ to: 'creator', bps: 6000 }, { to: A, bps: 2500 }, { to: 'burn', bps: 1500 }]);
    expect(feeSplitOf({ creator: 0, burn: 100, addresses: [] })).toEqual([{ to: 'burn', bps: 10000 }]);
  });
  it('thirds round to exactly 10000 (largest remainders)', () => {
    const s = feeSplitOf({ creator: 33.333, burn: 33.333, addresses: [{ address: A, percent: 33.334 }] });
    expect(sum(s)).toBe(10000);
    expect(s.every((x) => Number.isInteger(x.bps))).toBe(true);
    const t = feeSplitOf({ creator: 100 / 3, burn: 100 / 3, addresses: [{ address: A, percent: 100 / 3 }] });
    expect(sum(t)).toBe(10000);
    expect(t.map((x) => x.bps).sort()).toEqual([3333, 3333, 3334]);
  });
  it('percents far from 100 stay as they round, for the problems to say', () => {
    expect(sum(feeSplitOf({ creator: 50, burn: 10, addresses: [] }))).toBe(6000);
  });
  it('0 to 3 addresses plus burn', () => {
    for (let n = 0; n <= 3; n++) {
      const addrs = [A, B, C].slice(0, n).map((address) => ({ address, percent: 10 }));
      const s = feeSplitOf({ creator: 100 - 10 * n - 5, burn: 5, addresses: addrs });
      expect(s.length).toBe(n + 2);
      expect(sum(s)).toBe(10000);
      expect(feeSplitProblems(s)).toEqual([]);
    }
  });
});

describe('feeSplitProblems: the rules of the chain', () => {
  it('takes a good split', () => {
    expect(feeSplitProblems([{ to: 'creator', bps: 7500 }, { to: A, bps: 2500 }])).toEqual([]);
  });
  it('rejects more than 5 recipients, more than 3 addresses', () => {
    const six = [{ to: 'creator', bps: 5000 }, { to: 'burn', bps: 1000 }, { to: A, bps: 1000 }, { to: B, bps: 1000 }, { to: C, bps: 1000 }, { to: 'rchn1qabc', bps: 1000 }];
    const p = feeSplitProblems(six);
    expect(p).toContain('A split has at most 5 recipients.');
    expect(p).toContain('A split has at most 3 addresses.');
  });
  it('rejects duplicates: creator, burn, an address (case aside)', () => {
    expect(feeSplitProblems([{ to: 'creator', bps: 5000 }, { to: 'creator', bps: 5000 }])).toContain('The creator is in the split once at most.');
    expect(feeSplitProblems([{ to: 'burn', bps: 5000 }, { to: 'burn', bps: 5000 }])).toContain('Buyback & burn is in the split once at most.');
    expect(feeSplitProblems([{ to: A, bps: 5000 }, { to: A.toUpperCase(), bps: 5000 }])).toContain('Each address of the split is a different one.');
  });
  it('rejects bad sums, shares under 1 bps or not whole, an empty address, none at all', () => {
    expect(feeSplitProblems([{ to: 'creator', bps: 9000 }])).toContain('The shares add up to 90%, not 100%.');
    expect(feeSplitProblems([{ to: 'creator', bps: 10001 }])).toContain('The shares add up to 100.01%, not 100%.');
    expect(feeSplitProblems([{ to: 'creator', bps: 10000 }, { to: A, bps: 0 }])).toContain('Each share of the split is at least 0.01%.');
    expect(feeSplitProblems([{ to: 'creator', bps: 9999.5 }, { to: A, bps: 0.5 }])).toContain('Each share of the split is at least 0.01%.');
    expect(feeSplitProblems([{ to: 'creator', bps: 5000 }, { to: '', bps: 5000 }])).toContain('Enter each address of the split.');
    expect(feeSplitProblems([])).toEqual(['Give the fees at least one recipient.']);
  });
});

describe('feeSplitOptions', () => {
  it('none for all to the creator; else every share, to and bps only', () => {
    expect(feeSplitOptions([{ to: 'creator', bps: 10000 }])).toEqual({});
    expect(feeSplitOptions([{ to: 'burn', bps: 10000 }])).toEqual({ fee_split: [{ to: 'burn', bps: 10000 }] });
    expect(feeSplitOptions([{ to: 'creator', bps: 7500, kind: 'creator', paid: 1 }, { to: ` ${A}`, bps: 2500 }])).toEqual({ fee_split: [{ to: 'creator', bps: 7500 }, { to: A, bps: 2500 }] });
  });
});

describe("a launch's split and its words", () => {
  it("v7's fee_split as the node says it", () => {
    const s = feeSplitOfLaunch({ fee_flow: true, fee_split: [{ to: 'creator', kind: 'creator', bps: 7500, paid: 1, owed: 0.5 }, { to: A, bps: 2500 }] });
    expect(s).toEqual([{ to: 'creator', kind: 'creator', bps: 7500, paid: 1, owed: 0.5 }, { to: A, kind: 'address', bps: 2500 }]);
  });
  it("v5's mapped: creator, burn, split with burn_percent; nothing before v5", () => {
    expect(feeSplitOfLaunch({ fee_flow: true, fee_destination: 'creator' })).toEqual([{ to: 'creator', kind: 'creator', bps: 10000 }]);
    expect(feeSplitOfLaunch({ fee_flow: true, fee_destination: 'burn' })).toEqual([{ to: 'burn', kind: 'burn', bps: 10000 }]);
    expect(feeSplitOfLaunch({ fee_flow: true, fee_destination: 'split', burn_percent: 30 })).toEqual([{ to: 'burn', kind: 'burn', bps: 3000 }, { to: 'creator', kind: 'creator', bps: 7000 }]);
    expect(feeSplitOfLaunch({})).toEqual([]);
  });
  it('each share in percent of the tunable part and of each trade', () => {
    expect(formatBps(3333)).toBe('33.33');
    expect(formatBps(10000)).toBe('100');
    expect(shareOfTrade(10000, 75, 1)).toBeCloseTo(0.75, 12);
    expect(shareText({ bps: 2500 }, 75, 1)).toBe('25% of the tunable part (0.1875% of each trade)');
    expect(shareRecipientText({ to: 'creator' })).toBe('The creator');
    expect(shareRecipientText({ to: 'burn' })).toBe('Buyback & burn');
    expect(shareRecipientText({ to: A })).toBe(A);
  });
});
