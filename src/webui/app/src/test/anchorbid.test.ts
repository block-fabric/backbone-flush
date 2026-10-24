// Merged mining, a validator's anchor bids (chain/fb/AnchorBidPanel.svelte): the policy read with getanchorbidpolicy and
// getanchorinfo's bid, the presets and the advanced fields, a change through the host's confirmation (setanchorbidpolicy
// is a confirmed method, never a plain RPC of the page), a node that is not a validator; and what a mainchain commitment
// of this chain is in the table of its blocks (resolveCommitment: a block, or on FastBlocks an anchor ticket).
import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import AnchorBidPanel, { changedFields, curveText, PRESETS, type BidPolicy, type BidPolicyInfo, type BidStatus } from '../chain/fb/AnchorBidPanel.svelte';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { resolveCommitment, type CommitmentReads } from '../lib/side';

const POLICY: BidPolicy = {
  strategy: 'feespreventstall',
  floor: 0.000005,
  max: 0.001,
  fixed: 0.000005,
  feeshare: 99,
  escalationstart: 0.5,
  curve: [
    { pressure: 0.5, multiplier: 1 },
    { pressure: 0.75, multiplier: 2 },
    { pressure: 0.9, multiplier: 4 },
  ],
  interpolate: true,
  usefeeestimate: true,
  requestvsize: 190,
  rebidafter: 3,
  rebidstep: 2,
  rebidmax: 8,
  warnmissed: 3,
  periodcap: 0.01,
  periodhours: 24,
  lowbalanceblocks: 1008,
};

const BID: BidStatus = {
  validator: true,
  bidding: true,
  strategy: 'feespreventstall',
  effective: 0.0000076,
  normal: 0.000005,
  kind: 'escalation',
  reason: 'escalation at p=0.92: x4.00 the fee estimate (1.00 sat/vB for 190 vB)',
  pressure: 0.92,
  multiplier: 4,
  escalationstart: 0.5,
  firstunanchoredslot: 100,
  unanchoredslots: 1766,
  maxunanchoredslots: 1920,
  feerate: 1,
  feesource: 'estimatesmartfee',
  missed: 8,
  balance: 0.5,
  coverblocks: 65789,
  spentperiod: 0.0001,
  periodcap: 0.01,
  periodhours: 24,
  history: [
    { mainheight: 973961, ticket: 'aa'.repeat(32), offer: 0.0000076, kind: 'escalation', reason: 'escalation', pressure: 0.92, status: 'pending', cost: 0, time: 1 },
    { mainheight: 973960, ticket: 'bb'.repeat(32), offer: 0.000005, kind: 'floor', reason: 'floor', pressure: 0.9, status: 'missed', cost: 0, time: 0 },
  ],
  warnings: ['Your bid is too low: 8 mainchain blocks in a row came without this validator’s commitment.'],
};

let info: BidPolicyInfo;
let bid: BidStatus;
let answer: 'yes' | 'no';
const calls: BridgeRequest[] = [];

async function fake(r: BridgeRequest): Promise<unknown> {
  calls.push(r);
  switch (r.method) {
    case 'rpc.confirmedMethods':
      return { confirmed: ['setanchorbidpolicy'] };
    case 'getanchorbidpolicy':
      return info;
    case 'getanchorinfo':
      return { bid };
    case 'rpc.confirmed': {
      if (answer === 'no') throw Object.assign(new Error('declined'), { code: -32800 });
      const p = r.params as { method: string; params: [Record<string, unknown>] };
      info = { ...info, policy: { ...info.policy, ...(p.params[0] as Partial<BidPolicy>) }, overrides: { ...info.overrides, ...p.params[0] } };
      return info;
    }
    default:
      return null;
  }
}

beforeEach(() => {
  info = { validator: true, bidding: true, policy: { ...POLICY }, defaults: { ...POLICY }, overrides: {} };
  bid = { ...BID };
  answer = 'yes';
  calls.length = 0;
  setTransport(fake);
});

afterEach(() => setTransport(null));

describe('the anchor bid panel', () => {
  it('shows the bid now and why, the run and its pressure, the wallet, the warnings and the last bids', async () => {
    render(AnchorBidPanel);
    await waitFor(() => expect(document.getElementById('anchor-bid-reason')?.textContent).toMatch(/escalation at p=0.92/));
    expect((document.getElementById('anchor-bid-pressure') as HTMLMeterElement).value).toBeCloseTo(0.92);
    expect(document.getElementById('anchor-bid-status')!.textContent).toMatch(/1\D?766 \/ 1\D?920 slots/);
    expect(document.getElementById('anchor-bid-status')!.textContent).toMatch(/65\D?789 mainchain blocks at this bid/);
    expect(document.getElementById('anchor-bid-missed')!.textContent).toBe('8');
    expect(screen.getByRole('alert').textContent).toMatch(/Your bid is too low/);
    const table = screen.getByRole('table', { name: 'Last anchor bids' });
    expect(table.textContent).toMatch(/973\D?960/);
    expect(table.textContent).toMatch(/missed/);
    // Every read is a read: no spending call without the host.
    expect(calls.some((c) => c.kind === 'rpc' && c.method === 'setanchorbidpolicy')).toBe(false);
  });

  it('the five presets, the default one said exactly as the user asked', async () => {
    render(AnchorBidPanel);
    await screen.findByText('Strategy');
    expect(PRESETS.map((p) => p.id)).toEqual(['fixed', 'minimal', 'fees', 'feespreventstall', 'off']);
    const radios = screen.getAllByRole('radio') as HTMLInputElement[];
    expect(radios).toHaveLength(5);
    expect(radios.find((r) => r.checked)?.value).toBe('feespreventstall');
    expect(screen.getByText('Fees + prevent stalling')).toBeTruthy();
    expect(
      screen.getByText(
        'Bids exactly the fee share it owes when there are fees, and sends a stall boost only when needed. Its minimum may be set to 0 (bid nothing when there are no fees and no stall pressure); 500 sat is the default minimum.',
      ),
    ).toBeTruthy();
  });

  it('a change goes through the host, which asks the desktop; only the fields changed', async () => {
    render(AnchorBidPanel);
    await screen.findByText('Strategy');
    await fireEvent.click(screen.getByRole('radio', { name: /Minimal: keep the chain moving/ }));
    await fireEvent.click(document.getElementById('anchor-advanced')!);
    expect(document.getElementById('anchor-advanced-fields')).not.toBeNull();
    await fireEvent.input(document.getElementById('anchor-curve')!, { target: { value: '0.5:1,0.8:5' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Apply' }));
    await waitFor(() => expect(calls.some((c) => c.method === 'rpc.confirmed')).toBe(true));
    const sent = calls.find((c) => c.method === 'rpc.confirmed')!.params as { method: string; params: unknown[] };
    expect(sent.method).toBe('setanchorbidpolicy');
    expect(sent.params).toEqual([{ strategy: 'minimal', curve: '0.5:1,0.8:5' }]);
    // The reset is offered once something differs from the options.
    await waitFor(() => expect((document.getElementById('anchor-reset') as HTMLButtonElement).disabled).toBe(false));
  });

  it('a declined change is no error; a minimum above the maximum is not sent', async () => {
    answer = 'no';
    render(AnchorBidPanel);
    await screen.findByText('Strategy');
    await fireEvent.click(screen.getByRole('radio', { name: /^Off/ }));
    expect(screen.getByText(/leaves its anchors to other bidders/)).toBeTruthy();
    await fireEvent.click(screen.getByRole('button', { name: 'Apply' }));
    await waitFor(() => expect(calls.some((c) => c.method === 'rpc.confirmed')).toBe(true));
    expect(document.querySelector('.side-bad[role="alert"]')?.textContent ?? '').not.toMatch(/declined/);
    const floor = document.getElementById('anchor-floor') as HTMLInputElement;
    await fireEvent.input(floor, { target: { value: '0.01' } });
    await waitFor(() => expect(document.getElementById('anchor-problem')?.textContent).toMatch(/above the maximum/));
    expect((screen.getByRole('button', { name: 'Apply' }) as HTMLButtonElement).disabled).toBe(true);
  });

  it('a node that is not a validator: the explanation, no form', async () => {
    info = { ...info, validator: false, bidding: false, explanation: 'This node is not a validator: only validators bid for BMM commitments to their tickets.' };
    bid = { ...bid, validator: false, bidding: false };
    render(AnchorBidPanel);
    await waitFor(() => expect(document.getElementById('anchor-bid-explanation')?.textContent).toMatch(/not a validator/));
    expect(screen.queryAllByRole('radio')).toHaveLength(0);
    expect(screen.queryByRole('button', { name: 'Apply' })).toBeNull();
  });

  it('the fields sent: amounts in coins, the curve as text, nothing unchanged', () => {
    expect(curveText(POLICY.curve)).toBe('0.5:1,0.75:2,0.9:4');
    expect(changedFields(POLICY, { ...POLICY })).toEqual({});
    expect(changedFields(POLICY, { ...POLICY, floor: 0, max: 0.002, rebidafter: 0 })).toEqual({ floor: '0', max: '0.002', rebidafter: 0 });
  });
});

describe('what a commitment of this chain is (Blocks of the mainchain)', () => {
  const answers = (map: Record<string, unknown>): CommitmentReads => {
    const answer = async (method: string) => {
      const a = map[method];
      if (a instanceof Error) throw a;
      return a as never;
    };
    return { header: () => answer('getblockheader'), ticket: () => answer('getanchorticket') };
  };
  const error = (code: number, message = 'error') => Object.assign(new Error(message), { code });

  it('a block of this chain (merged mined): the template, unchanged', async () => {
    expect(await resolveCommitment('11'.repeat(32), answers({ getblockheader: { confirmations: 3, height: 9 } }))).toEqual({ kind: 'block', inChain: true, height: 9 });
    expect(await resolveCommitment('11'.repeat(32), answers({ getblockheader: { confirmations: -1, height: 9 } }))).toEqual({ kind: 'block', inChain: false, height: 9 });
  });

  it('an anchor ticket: the block it anchors, and whether that is in this chain', async () => {
    const ask = answers({ getblockheader: error(-5, 'Block not found'), getanchorticket: { known: true, anchored: '13'.repeat(32), anchoredheight: 1341, inactivechain: true } });
    expect(await resolveCommitment('78'.repeat(32), ask)).toEqual({ kind: 'ticket', inChain: true, anchoredHeight: 1341, anchored: '13'.repeat(32) });
    const off = answers({ getblockheader: error(-5), getanchorticket: { known: true, anchored: '13'.repeat(32), anchoredheight: 1341, inactivechain: false } });
    expect(await resolveCommitment('78'.repeat(32), off)).toMatchObject({ kind: 'ticket', inChain: false });
  });

  it('neither: another chain; a chain without anchors says the same; a failed lookup is an error, not "not in this chain"', async () => {
    expect(await resolveCommitment('99'.repeat(32), answers({ getblockheader: error(-5), getanchorticket: { known: false } }))).toEqual({ kind: 'unknown' });
    expect(await resolveCommitment('99'.repeat(32), answers({ getblockheader: error(-5), getanchorticket: error(-32601, 'Method not found') }))).toEqual({ kind: 'unknown' });
    expect(await resolveCommitment('99'.repeat(32), answers({ getblockheader: error(-28, 'Loading block index') }))).toEqual({ kind: 'error', message: 'Loading block index' });
    expect(await resolveCommitment('99'.repeat(32), answers({ getblockheader: error(-5), getanchorticket: error(-32000, 'timeout') }))).toEqual({ kind: 'error', message: 'timeout' });
  });
});
