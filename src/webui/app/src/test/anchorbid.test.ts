// Merged mining, Bidding (chain/fb/BiddingPanel.svelte): the node's one bid policy, read with getanchorbidpolicy and
// getanchorinfo's bid; the five strategies and "Prevent stalling", the fields of the strategy chosen, the limits and the
// day's spending, the triggers; a change through the host's confirmation (setanchorbidpolicy is a confirmed method, never a
// plain RPC of the page), the strategy always sent with "Prevent stalling"; a node that is not a validator; and what a
// mainchain commitment of this chain is in the table of its blocks (resolveCommitment: a block, or on FastBlocks an anchor ticket).
import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import BiddingPanel, { changedFields, curveText, STRATEGIES, type BidPolicy, type BidPolicyInfo, type BidStatus } from '../chain/fb/BiddingPanel.svelte';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { resolveCommitment, type CommitmentReads } from '../lib/side';

// Louis's app (an 8.3.5 feespreventstall, floor 0) as 8.3.6 reads it: Fees, Prevent stalling on.
const POLICY: BidPolicy = {
  strategy: 'fees',
  preventstall: true,
  anchor: true,
  floor: 0,
  max: 0.0001,
  fixed: 0,
  feeshare: 99,
  escalationstart: 0.5,
  curve: [
    { pressure: 0.5, multiplier: 1 },
    { pressure: 0.75, multiplier: 2 },
    { pressure: 0.9, multiplier: 4 },
  ],
  flatboost: true,
  boost: 4,
  interpolate: true,
  usefeeestimate: false,
  requestvsize: 189,
  rebidafter: 3,
  rebidstep: 2,
  rebidmax: 8,
  warnmissed: 3,
  periodcap: 0.001,
  periodhours: 24,
  lowbalanceblocks: 1008,
};

const BID: BidStatus = {
  validator: true,
  bidding: true,
  strategy: 'fees',
  effective: 0.00001,
  normal: 0,
  kind: 'stall',
  reason: 'preventing a stall at p=0.92: x4.00 the competitive base 0.0000025 (no bid: no fees)',
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
  coverblocks: 50000,
  spentperiod: 0.0003,
  periodcap: 0.001,
  periodhours: 24,
  history: [
    { mainheight: 973961, ticket: 'aa'.repeat(32), offer: 0.00001, kind: 'stall', reason: 'stall', pressure: 0.92, status: 'pending', cost: 0, time: 1 },
    { mainheight: 973960, ticket: 'bb'.repeat(32), offer: 0.000005, kind: 'fees', reason: 'fees', pressure: 0.9, status: 'missed', cost: 0, time: 0 },
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

const sentFields = () => (calls.find((c) => c.method === 'rpc.confirmed')!.params as { method: string; params: unknown[] }).params[0];

describe('the bidding panel', () => {
  it('shows the bid now and why, the run and its pressure, the day spent and left, the wallet, the warnings and the last bids', async () => {
    render(BiddingPanel);
    await waitFor(() => expect(document.getElementById('bid-reason')?.textContent).toMatch(/preventing a stall at p=0.92/));
    expect((document.getElementById('bid-pressure') as HTMLMeterElement).value).toBeCloseTo(0.92);
    expect(document.getElementById('bid-status')!.textContent).toMatch(/1\D?766 \/ 1\D?920 slots/);
    expect(document.getElementById('bid-status')!.textContent).toMatch(/50\D?000 mainchain blocks at this bid/);
    expect(document.getElementById('bid-missed')!.textContent).toBe('8');
    // Spent 0.0003 of a cap of 0.001 today: 0.0007 left.
    expect(document.getElementById('bid-left')!.textContent).toMatch(/left.*0\.0007.*of.*0\.001/);
    expect(document.getElementById('bid-wallet')!.textContent).toMatch(/Paid from your mainchain wallet/);
    expect(screen.getByRole('alert').textContent).toMatch(/Your bid is too low/);
    const table = screen.getByRole('table', { name: 'Last bids' });
    expect(table.textContent).toMatch(/973\D?960/);
    expect(table.textContent).toMatch(/missed/);
    // Every read is a read: no spending call without the host.
    expect(calls.some((c) => c.kind === 'rpc' && c.method === 'setanchorbidpolicy')).toBe(false);
  });

  it('five strategies, Fees the default, and Prevent stalling beside them', async () => {
    render(BiddingPanel);
    await screen.findByText('Strategy');
    expect(STRATEGIES.map((p) => p.id)).toEqual(['fees', 'feesminimum', 'minimum', 'fixed', 'off']);
    expect(STRATEGIES.map((p) => p.label)).toEqual(['Fees', 'Fees + minimum', 'Minimum every block', 'Fixed amount', 'Off']);
    const radios = screen.getAllByRole('radio') as HTMLInputElement[];
    expect(radios).toHaveLength(5);
    expect(radios.find((r) => r.checked)?.value).toBe('fees');
    const stall = document.getElementById('bid-preventstall') as HTMLInputElement;
    expect(stall.checked).toBe(true);
    expect(stall.disabled).toBe(false);
    // The triggers: merged mining (the page's controls) and, on a validator, Anchor my validator.
    expect(document.getElementById('bid-trigger-mine')!.textContent).toMatch(/Mine blocks when fees pay/);
    expect((document.getElementById('bid-anchor') as HTMLInputElement).checked).toBe(true);
    // Nothing changed: nothing to apply.
    expect((document.getElementById('bid-apply') as HTMLButtonElement).disabled).toBe(true);
  });

  it('the amount fields of the strategy chosen, and only those', async () => {
    render(BiddingPanel);
    await screen.findByText('Strategy');
    const shown = () => ['bid-feeshare', 'bid-floor', 'bid-fixed'].filter((id) => document.getElementById(id));
    expect(shown()).toEqual(['bid-feeshare']);
    await fireEvent.click(screen.getByRole('radio', { name: /^Fees \+ minimum/ }));
    expect(shown()).toEqual(['bid-feeshare', 'bid-floor']);
    await fireEvent.click(screen.getByRole('radio', { name: /^Minimum every block/ }));
    expect(shown()).toEqual(['bid-floor']);
    await fireEvent.click(screen.getByRole('radio', { name: /^Fixed amount/ }));
    expect(shown()).toEqual(['bid-fixed']);
    await fireEvent.click(screen.getByRole('radio', { name: /^Off/ }));
    expect(shown()).toEqual([]);
    // Off means off: Prevent stalling greyed out, its value kept; the warning.
    expect((document.getElementById('bid-preventstall') as HTMLInputElement).disabled).toBe(true);
    expect((document.getElementById('bid-preventstall') as HTMLInputElement).checked).toBe(true);
    expect(document.getElementById('bid-off-warning')!.textContent).toMatch(/leaves its anchors to other bidders/);
  });

  it('a change goes through the host, which asks the desktop; the strategy always with Prevent stalling', async () => {
    render(BiddingPanel);
    await screen.findByText('Strategy');
    await fireEvent.click(screen.getByRole('radio', { name: /^Minimum every block/ }));
    await fireEvent.input(document.getElementById('bid-floor')!, { target: { value: '0.00001' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Apply' }));
    await waitFor(() => expect(calls.some((c) => c.method === 'rpc.confirmed')).toBe(true));
    const sent = calls.find((c) => c.method === 'rpc.confirmed')!.params as { method: string; params: unknown[] };
    expect(sent.method).toBe('setanchorbidpolicy');
    expect(sent.params).toEqual([{ strategy: 'minimum', preventstall: true, floor: '0.00001' }]);
    await waitFor(() => expect((document.getElementById('bid-reset') as HTMLButtonElement).disabled).toBe(false));
  });

  it('Prevent stalling alone, Anchor my validator alone, the limits', async () => {
    render(BiddingPanel);
    await screen.findByText('Strategy');
    await fireEvent.click(document.getElementById('bid-preventstall')!);
    await fireEvent.click(document.getElementById('bid-anchor')!);
    await fireEvent.input(document.getElementById('bid-periodcap')!, { target: { value: '0.002' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Apply' }));
    await waitFor(() => expect(calls.some((c) => c.method === 'rpc.confirmed')).toBe(true));
    expect(sentFields()).toEqual({ strategy: 'fees', preventstall: false, anchor: false, periodcap: '0.002' });
  });

  it('a declined change is no error; a minimum above the maximum is not sent', async () => {
    answer = 'no';
    render(BiddingPanel);
    await screen.findByText('Strategy');
    await fireEvent.click(screen.getByRole('radio', { name: /^Off/ }));
    await fireEvent.click(screen.getByRole('button', { name: 'Apply' }));
    await waitFor(() => expect(calls.some((c) => c.method === 'rpc.confirmed')).toBe(true));
    expect(document.querySelector('.side-bad[role="alert"]')?.textContent ?? '').not.toMatch(/declined/);
    await fireEvent.click(screen.getByRole('radio', { name: /^Minimum every block/ }));
    await fireEvent.input(document.getElementById('bid-floor')!, { target: { value: '0.01' } });
    await waitFor(() => expect(document.getElementById('bid-problem')?.textContent).toMatch(/above the maximum/));
    expect((screen.getByRole('button', { name: 'Apply' }) as HTMLButtonElement).disabled).toBe(true);
  });

  it('a node that is not a validator: the same form, no Anchor my validator', async () => {
    info = { ...info, validator: false, bidding: false, explanation: 'This node is not a validator: it bids only as a merged miner (setbmm), under this policy; merged mining is off.' };
    bid = { ...bid, validator: false, bidding: false, warnings: [] };
    render(BiddingPanel);
    await waitFor(() => expect(document.getElementById('bid-explanation')?.textContent).toMatch(/not a validator/));
    expect(screen.getAllByRole('radio')).toHaveLength(5);
    expect(document.getElementById('bid-anchor')).toBeNull();
    expect(document.getElementById('bid-trigger-mine')).not.toBeNull();
  });

  it('the fields sent: amounts in coins, the curve as text, nothing unchanged, the strategy with Prevent stalling', () => {
    expect(curveText(POLICY.curve)).toBe('0.5:1,0.75:2,0.9:4');
    expect(changedFields(POLICY, { ...POLICY })).toEqual({});
    expect(changedFields(POLICY, { ...POLICY, floor: 0.00001, max: 0.002, rebidafter: 0 })).toEqual({ floor: '0.00001', max: '0.002', rebidafter: 0 });
    expect(changedFields(POLICY, { ...POLICY, strategy: 'feesminimum' })).toEqual({ strategy: 'feesminimum', preventstall: true });
    expect(changedFields(POLICY, { ...POLICY, preventstall: false })).toEqual({ strategy: 'fees', preventstall: false });
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
