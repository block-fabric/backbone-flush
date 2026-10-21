// The assets a wallet holds, as the Home and the Overview list them (HeldAssets.svelte): from the one listmyassets the
// store reads (no call per asset), those with a balance by their worth in ECX (where a pool prices them), then by their
// balance; those the wallet only controls (the control coin, no units) apart.
import { approx, type Holding } from './model';

export interface HeldRow {
  holding: Holding;
  /** Its number, which names it whatever its label looks like (#1739-0029). */
  seq: string;
  /** What it is worth in ECX, if a pool prices it. */
  value: number | undefined;
  /** Units not spendable yet (waiting for a block). */
  pending: boolean;
}

export const HELD_SHOWN = 8;

export function heldRows(holdings: readonly Holding[], priceOf: (id: string) => number | undefined, seqOf: (id: string) => string): { rows: HeldRow[]; controlledOnly: HeldRow[] } {
  const row = (h: Holding): HeldRow => {
    const price = priceOf(h.asset);
    return { holding: h, seq: seqOf(h.asset), value: price !== undefined && price > 0 ? price * approx(h.balance) : undefined, pending: approx(h.pending) > 0 };
  };
  const rows = holdings
    .filter((h) => approx(h.balance) > 0 || approx(h.pending) > 0)
    .map(row)
    .sort((a, b) => (b.value ?? -1) - (a.value ?? -1) || approx(b.holding.balance) - approx(a.holding.balance) || a.holding.label.localeCompare(b.holding.label));
  const controlledOnly = holdings.filter((h) => h.control && approx(h.balance) <= 0 && approx(h.pending) <= 0).map(row);
  return { rows, controlledOnly };
}
