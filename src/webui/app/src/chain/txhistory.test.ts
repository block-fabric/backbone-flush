// What the history shows of each kind of asset transaction (review of 2026-10-28): what went out and what came in, the
// coin's legs (paid into the operation, or out by a coinbase), exact or at least, refunds; and the coin's own records
// (a payout of the chain, a bond, a withdrawal), from the cases of the betanet wallets that showed them wrong.
import { render } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import LocalTxCell from './LocalTxCell.svelte';
import { ba } from './ba/store.svelte';
import type { AssetTx } from './ba/model';
import { OPERATIONS } from './ba/model';
import { categoryText, chainTxOf, moveNotes, movesSummary, signedAmount, shownRecords, typeOfMoves } from './txinfo.svelte';
import { COINBASE_ROLES } from './roles';
import { setTransport } from '../lib/bridge';
import { wallets } from '../lib/state.svelte';
import { addressText, parseRecords, typeText } from '../lib/wallet';

const BROSEVELT = '73'.repeat(32);
const COINYE = '78'.repeat(32);
let n = 0;

function asset(category: string, sign: -1 | 0 | 1, label: string, amount: string | undefined, o: Partial<AssetTx> = {}): AssetTx {
  return { txid: '', time: 1, confirmations: 3, category, asset: label === 'COINYE' ? COINYE : BROSEVELT, label, decimals: label === 'COINYE' ? 6 : 2, amount, sign, to: [], from: [], abandoned: false, ...o };
}
function coin(category: string, sign: -1 | 1, amount: string | undefined, o: Partial<AssetTx> = {}): AssetTx {
  return { txid: '', time: 1, confirmations: 3, category, label: 'ECX', decimals: 8, coin: true, amount, sign, to: [], from: [], abandoned: false, ...o };
}
/** The moves of one transaction, in the order the node lists them. */
function tx(...moves: AssetTx[]): string {
  const txid = (++n).toString(16).padStart(64, '0');
  ba.transfers = [...ba.transfers, ...moves.map((m) => ({ ...m, txid }))];
  return txid;
}

beforeEach(() => {
  setTransport(async () => []);
  wallets.current = 'w';
  ba.transfersWallet = 'w';
  ba.transfers = [];
});
afterEach(() => {
  setTransport(null);
  ba.transfers = [];
  ba.activity = [];
  wallets.current = null;
});

describe('a swap, a buy, a sale', () => {
  it('a route (a8b80c95…): 500 BROSEVELT out, 21.115475 COINYE in; its change is no receive', () => {
    const id = tx(asset('swap', -1, 'BROSEVELT', '500.00'), asset('swap', 1, 'COINYE', '21.115475'));
    expect(chainTxOf(id)!.type).toBe('Swap');
    expect(movesSummary(id)).toBe('−500.00 BROSEVELT, +21.115475 COINYE');
    const cell = render(LocalTxCell, { txid: id });
    const text = cell.container.textContent ?? '';
    expect(text).toContain('−500.00');
    expect(text).toContain('+21.115475');
    expect(text).not.toContain('619');
    cell.unmount();
  });

  it('a sale (ba014d7c…): the asset out, the coin in by a coinbase, at least what it asked', () => {
    const out = coin('swap', 1, '0.00454952', { via: 'coinbase', at_least: true });
    const id = tx(asset('swap', -1, 'BROSEVELT', '500.00'), out);
    expect(chainTxOf(id)!.type).toBe('Sell');
    expect(signedAmount(out)).toBe('+≥ 0.00454952');
    expect(moveNotes(out)).toEqual(['paid by a coinbase', 'at least']);
    const cell = render(LocalTxCell, { txid: id });
    expect(cell.getByTestId('coin-move').textContent).toBe('ECX');
    expect(cell.container.textContent).toContain('paid by a coinbase');
    cell.unmount();
  });

  it('a buy whose tokens were spent since (8c1d31f7…): the coin out, the asset in at least, and why', () => {
    const got = asset('swap', 1, 'COINYE', '4061.747522', { at_least: true, unknown: 'spent' });
    const id = tx(coin('swap', -1, '0.92900000'), got);
    expect(chainTxOf(id)!.type).toBe('Buy');
    expect(signedAmount(got)).toBe('+≥ 4,061.747522');
    expect(moveNotes(got)).toEqual(['the least asked for: what came out was spent since']);
    expect(movesSummary(id)).toBe('−0.92900000 ECX, +≥ 4,061.747522 COINYE');
  });

  it('an order the batch did not fill: what went in, and the same back', () => {
    const id = tx(asset('swap', -1, 'BROSEVELT', '50.00'), asset('swap', 1, 'BROSEVELT', '50.00', { refund: true }));
    expect(chainTxOf(id)!.type).toBe('Swap not filled');
    expect(moveNotes(ba.transfers[1])).toEqual(['refund']);
  });

  it('one not in a block yet says so', () => {
    expect(moveNotes(asset('swap', 1, 'COINYE', '1.000000', { at_least: true, unknown: 'pending' }))).toEqual(['not in a block yet']);
  });
});

describe('every other kind', () => {
  const kinds: [AssetTx[], string][] = [
    // 7b64e8a7…: an ask of 5,000 BROSEVELT (the node said "+944,994.00 received": its change).
    [[asset('order_place', -1, 'BROSEVELT', '5000.00')], 'Order placed'],
    // b18682b8…: its cancel, the 5,000 back.
    [[asset('order_cancel', 1, 'BROSEVELT', '5000.00', { refund: true })], 'Order cancelled'],
    [[asset('order_settle', 1, 'COINYE', '1.000000')], 'Order collected'],
    // c283e0f1…: a position, about 6,011.86 BROSEVELT and at most 0.05 ECX in (the node said "+938,982.14 received").
    [[asset('position_open', -1, 'BROSEVELT', '6011.86'), coin('position_open', -1, '0.05000000', { at_most: true })], 'Position opened'],
    [[asset('position_add', -1, 'BROSEVELT', '5.00')], 'Position added to'],
    [[asset('position_remove', 1, 'BROSEVELT', '5.00')], 'Position reduced'],
    [[asset('position_collect', 1, 'BROSEVELT', '0.10'), coin('position_collect', 1, undefined, { via: 'coinbase' })], 'Position fees collected'],
    [[asset('position_close', 1, 'BROSEVELT', '5.00')], 'Position closed'],
    [[asset('pool_create', 0, 'BROSEVELT', undefined)], 'Pool created'],
    [[asset('pool_add', -1, 'BROSEVELT', '50000.00', { at_most: true }), coin('pool_add', -1, '0.01000000', { at_most: true })], 'Pool add'],
    [[asset('pool_remove', 1, 'BROSEVELT', '49977.63'), coin('pool_remove', 1, undefined, { via: 'coinbase' })], 'Pool remove'],
    // A launch with its first buy (3a57a76e…): launched, the coin in, the token out.
    [[asset('launch', 0, 'COINYE', undefined), coin('swap', -1, '0.00050000'), asset('swap', 1, 'COINYE', '2.226300')], 'Launched'],
    [[asset('curve', -1, 'COINYE', '100.000000')], 'Put on a curve'],
    // A claim: the creator's fees by a coinbase (their amount in the payout), the allocation when graduated.
    [[coin('claim', 1, undefined, { via: 'coinbase' })], 'Launch claim'],
    [[asset('burn', -1, 'BROSEVELT', '25000.00')], 'Burned'],
  ];

  it('has a type of its own, signs that say out and in, and notes for what is not exact', () => {
    for (const [moves, type] of kinds) {
      const id = tx(...moves);
      expect(chainTxOf(id)!.type, type).toBe(type);
      for (const e of moves) {
        if (e.sign < 0) expect(signedAmount(e).startsWith('−'), type).toBe(true);
        if (e.sign > 0) expect(signedAmount(e).startsWith('+'), type).toBe(true);
        if (e.at_most) expect(signedAmount(e), type).toContain('≤ ');
        if (e.via === 'coinbase') expect(moveNotes(e), type).toContain('paid by a coinbase');
      }
      expect(categoryText(moves[0]), type).not.toBe('Asset');
    }
  });

  it('names every operation of listassetactivity', () => {
    for (const op of ['buy', 'sell', 'route', 'open position', 'change position', 'place order', 'settle order', 'cancel order', 'curve', 'logo', 'display name', 'register oracle', 'attest', 'oracle contract', 'settle']) {
      expect(OPERATIONS[op], op).toBeTruthy();
    }
    expect(OPERATIONS.route).toBe(OPERATIONS.swap);
  });
});

describe("the coin's own records", () => {
  const NOW = 1793000000;
  const hash = (c: string) => c.repeat(64);
  const record = (txid: string, idx: number, type: string, amount: number, status: string, role: string, source = '') =>
    [txid, idx, type, '', '', NOW, amount > 0 ? amount : 0, amount < 0 ? amount : 0, status, 10, 0, true, `k${idx}`, 6, 'c', type === 'mined', false, true, role, source];

  it('a payout of the chain is no mining: an order payout with its transaction, a deposit, an unbond, a refund', () => {
    const r = parseRecords([
      record(hash('a'), 1, 'mined', 459502, 'confirmed', 'payout', hash('b')),
      record(hash('c'), 1, 'mined', 100000000, 'confirmed', 'deposit'),
      record(hash('d'), 2, 'mined', 4400000, 'confirmed', 'unbond'),
      record(hash('e'), 1, 'mined', 1000, 'confirmed', 'refund'),
      record(hash('f'), 1, 'mined', 1000, 'confirmed', 'chain'),
    ]);
    expect(r.map((x) => typeText(x.type, x.role))).toEqual(['Order payout', 'Deposit', 'Unbonded', 'Withdrawal refund', 'Chain payout']);
    expect(r[0].source).toBe(hash('b'));
    for (const role of ['payout', 'deposit', 'unbond', 'refund', 'chain']) expect(COINBASE_ROLES[role].source).not.toMatch(/generated|mined/i);
  });

  it('a bond, an unbond and a withdrawal say what they went to', () => {
    const r = parseRecords([
      record(hash('1'), 0, 'sendOther', -14400000, 'confirmed', 'bond'),
      record(hash('2'), 1, 'sendOther', 0, 'confirmed', 'unbond'),
      record(hash('3'), 0, 'sendOther', -100000, 'confirmed', 'withdrawal'),
    ]);
    expect(r.map((x) => typeText(x.type, x.role))).toEqual(['Bond', 'Unbond', 'Withdrawal']);
    expect(r.map((x) => addressText(x))).toEqual(["a validator's bond", "a validator's bond (paid back later)", 'the mainchain']);
  });

  it('leaves out a block of the wallet the chain did not take that paid nothing', () => {
    const r = parseRecords([record(hash('9'), 0, 'mined', 0, 'notaccepted', ''), record(hash('8'), 0, 'mined', 5000, 'confirmed', '')]);
    expect(r.map((x) => x.txid)).toEqual([hash('8')]);
  });

  it('the overview lists an asset transaction once, as the Transactions page does', () => {
    const id = tx(asset('swap', -1, 'BROSEVELT', '500.00'), asset('swap', 1, 'COINYE', '21.115475'));
    const r = parseRecords([record(id, 0, 'send', -7528, 'confirmed', ''), record(id, 0, 'recv', 0, 'confirmed', ''), record(id, 2, 'sendOther', 0, 'confirmed', 'marker')]);
    const shown = shownRecords(r);
    expect(shown).toHaveLength(1);
    expect(shown[0].amount).toBe(-7528n);
  });
});
