// Amounts filled in for the user (Max, held balances): the asset's own decimals, rounded down, never a float's digits.
import { describe, expect, it } from 'vitest';
import { fireEvent, render } from '@testing-library/svelte';
import UnitsInput from './UnitsInput.svelte';
import { amountParam, formatAmount, shortestText, unitsAtMost } from './model';

describe('held amounts in units', () => {
  it('a 2-decimal asset: exact for clean numbers, rounded down past its decimals', () => {
    expect(unitsAtMost(0.29, 2)).toBe(29n);
    expect(unitsAtMost(99.6, 2)).toBe(9960n);
    expect(unitsAtMost(99.59999999999999, 2)).toBe(9960n); // the same double as 99.6
    expect(unitsAtMost(99.5999999, 2)).toBe(9959n);
    expect(unitsAtMost(1.239, 2)).toBe(123n);
    expect(unitsAtMost('1.239', 2)).toBe(123n);
    expect(unitsAtMost(1234567.89, 2)).toBe(123456789n);
  });
  it('a 4-decimal asset: the same', () => {
    expect(unitsAtMost(12.3456, 4)).toBe(123456n);
    expect(unitsAtMost(12.34569, 4)).toBe(123456n);
    expect(unitsAtMost(0.0001, 4)).toBe(1n);
    expect(unitsAtMost(0.00009, 4)).toBe(0n);
    expect(unitsAtMost(1e-7, 8)).toBe(10n);
  });
  it('a number written out, exponents included', () => {
    expect(shortestText(1e-7)).toBe('0.0000001');
    expect(shortestText(1.5e21)).toBe('1500000000000000000000');
    expect(shortestText(-0.25)).toBe('-0.25');
  });
  it('the text given the node and shown: its decimals only', () => {
    expect(amountParam(9960n, 2)).toBe('99.6');
    expect(amountParam(123456n, 4)).toBe('12.3456');
    expect(formatAmount(0.1 + 0.2, 2)).toBe('0.3');
  });
});

describe('Max', () => {
  for (const [decimals, max, text] of [
    [2, 9959n, '99.59'],
    [4, 123456n, '12.3456'],
    [4, 10000n, '1'],
  ] as const) {
    it(`fills what is held, with ${decimals} decimals`, async () => {
      const shown = render(UnitsInput, { props: { id: 'a', label: 'Amount', decimals, max } });
      await fireEvent.click(shown.getByRole('button', { name: 'Max' }));
      expect((shown.getByLabelText('Amount') as HTMLInputElement).value).toBe(text);
      expect(shown.queryByText('More than this wallet holds.')).toBeNull();
      shown.unmount();
    });
  }
});
