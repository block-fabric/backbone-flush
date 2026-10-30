// Merged mining, Bidding (chain/fb/BiddingPanel.svelte): the node's one bid policy, read with getanchorbidpolicy and
// getanchorinfo's bid (and from 8.3.7 getbmminfo's "anchoring"); the four plans and "Prevent stalling", the amount of the
// plan chosen, every other number under Advanced, the status line, the two switches (Anchor my validator at once, merged
// mining as a backup producer); the 8.3.7 fields shown only when the node has them; a change through the host's confirmation (setanchorbidpolicy is a confirmed method, never a
// plain RPC of the page), the strategy always sent with "Prevent stalling"; a node that is not a validator; and what a
// mainchain commitment of this chain is in the table of its blocks (resolveCommitment: a block, or on FastBlocks an anchor ticket).
import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import BiddingPanel, { changedFields, curveText, isRecommended, planOf, PLANS, recommended, type AnchoringInfo, type BidPolicy, type BidPolicyInfo, type BidStatus } from '../chain/fb/BiddingPanel.svelte';
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

// The same on an 8.3.7 node: the fields of demand-driven anchoring, at their defaults.
const POLICY_837: BidPolicy = { ...POLICY, batch: 10, heartbeathours: 6, valueatrisk: 1, reservepct: 20, reservetarget: 0.0001, turns: true, turnbackup: 2 };

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
    expect(screen.getByRole('alert').textContent).toMatch(/Your bid is too low/);
    const table = screen.getByRole('table', { name: 'Last bids' });
    expect(table.textContent).toMatch(/973\D?960/);
    expect(table.textContent).toMatch(/missed/);
    // The limits, under Advanced, say who pays.
    await fireEvent.click(document.getElementById('bid-advanced')!);
    expect(document.getElementById('bid-wallet')!.textContent).toMatch(/Paid from your mainchain wallet/);
    // Every read is a read: no spending call without the host.
    expect(calls.some((c) => c.kind === 'rpc' && c.method === 'setanchorbidpolicy')).toBe(false);
  });

  it('four plain plans, Recommended the default, Prevent stalling under them, and two separate switches', async () => {
    render(BiddingPanel);
    await screen.findByText('How to pay for anchors');
    expect(PLANS.map((p) => p.id)).toEqual(['recommended', 'minimum', 'fixed', 'off']);
    expect(PLANS.map((p) => p.label)).toEqual(['Recommended: pay from the fees', 'Anchor every block (minimum)', 'Fixed amount', 'Off']);
    const radios = screen.getAllByRole('radio') as HTMLInputElement[];
    expect(radios).toHaveLength(4);
    expect(radios.find((r) => r.checked)?.value).toBe('recommended');
    // An 8.3.6 node: no batches, heartbeat or reserve in the words of the card.
    expect(screen.getByRole('radio', { name: /^Recommended/ }).closest('label')!.textContent).toMatch(/no fees, no bid/);
    const stall = document.getElementById('bid-preventstall') as HTMLInputElement;
    expect(stall.checked).toBe(true);
    // The switches: Anchor my validator (a validator) and merged mining as a backup producer, each explained.
    expect(document.getElementById('bid-trigger-anchor')!.textContent).toMatch(/keeps the chain alive/);
    expect(document.getElementById('bid-trigger-mine')!.textContent).toMatch(/Merged-mine as a backup producer/);
    expect(document.getElementById('bid-trigger-mine')!.textContent).toMatch(/only if blocks of stake stop/);
    expect((document.getElementById('bid-anchor') as HTMLInputElement).checked).toBe(true);
    // Every number is under Advanced, collapsed.
    expect(document.getElementById('bid-advanced-fields')).toBeNull();
    expect(document.getElementById('bid-max')).toBeNull();
    // An 8.3.6 node says no more than it knows: the bid now and the day's spending.
    expect(document.getElementById('bid-line')!.textContent).toMatch(/Bid now/);
    expect(document.getElementById('bid-line')!.textContent).toMatch(/spent today.*0\.0003.*of.*0\.001/);
    expect(document.getElementById('bid-line-heartbeat')).toBeNull();
    // Nothing changed: nothing to apply.
    expect((document.getElementById('bid-apply') as HTMLButtonElement).disabled).toBe(true);
  });

  it('the amount of the plan chosen beside it, every other number under Advanced', async () => {
    render(BiddingPanel);
    await screen.findByText('How to pay for anchors');
    const shown = () => ['bid-floor', 'bid-fixed'].filter((id) => document.getElementById(id));
    expect(shown()).toEqual([]);
    await fireEvent.click(screen.getByRole('radio', { name: /^Anchor every block/ }));
    expect(shown()).toEqual(['bid-floor']);
    await fireEvent.click(screen.getByRole('radio', { name: /^Fixed amount/ }));
    expect(shown()).toEqual(['bid-fixed']);
    await fireEvent.click(document.getElementById('bid-advanced')!);
    // Fixed shown beside the card: under Advanced, the floor alone.
    expect(document.getElementById('bid-fixed-advanced')).toBeNull();
    expect(document.getElementById('bid-floor-advanced')).not.toBeNull();
    for (const id of ['bid-max', 'bid-periodcap', 'bid-stallreserve', 'bid-feeshare', 'bid-rebid', 'bid-curve']) expect(document.getElementById(id), id).not.toBeNull();
    // Not on the recommended plan: no Fees + minimum.
    expect((document.getElementById('bid-feesminimum') as HTMLInputElement).disabled).toBe(true);
    // The 8.3.7 fields of an 8.3.6 node: not there.
    for (const id of ['bid-batch', 'bid-heartbeat', 'bid-valueatrisk', 'bid-reservepct', 'bid-reservetarget', 'bid-turns', 'bid-turnbackup']) expect(document.getElementById(id), id).toBeNull();
    await fireEvent.click(screen.getByRole('radio', { name: /^Off/ }));
    expect(shown()).toEqual([]);
    // Off means off: no Prevent stalling (its value kept), the warning.
    expect(document.getElementById('bid-preventstall')).toBeNull();
    expect(document.getElementById('bid-off-warning')!.textContent).toMatch(/leaves its anchors to other bidders/);
  });

  it('a change goes through the host, which asks the desktop; the strategy always with Prevent stalling', async () => {
    render(BiddingPanel);
    await screen.findByText('How to pay for anchors');
    await fireEvent.click(screen.getByRole('radio', { name: /^Anchor every block/ }));
    await fireEvent.input(document.getElementById('bid-floor')!, { target: { value: '0.00001' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Apply' }));
    await waitFor(() => expect(calls.some((c) => c.method === 'rpc.confirmed')).toBe(true));
    const sent = calls.find((c) => c.method === 'rpc.confirmed')!.params as { method: string; params: unknown[] };
    expect(sent.method).toBe('setanchorbidpolicy');
    expect(sent.params).toEqual([{ strategy: 'minimum', preventstall: true, floor: '0.00001' }]);
    await waitFor(() => expect((document.getElementById('bid-reset') as HTMLButtonElement).disabled).toBe(false));
  });

  it('Prevent stalling and the limits with Apply; Anchor my validator at once, alone', async () => {
    render(BiddingPanel);
    await screen.findByText('How to pay for anchors');
    await fireEvent.click(document.getElementById('bid-preventstall')!);
    await fireEvent.click(document.getElementById('bid-advanced')!);
    await fireEvent.input(document.getElementById('bid-periodcap')!, { target: { value: '0.002' } });
    // The switch sends itself, without the edits of the form, which stay.
    await fireEvent.click(document.getElementById('bid-anchor')!);
    await waitFor(() => expect(calls.some((c) => c.method === 'rpc.confirmed')).toBe(true));
    expect(sentFields()).toEqual({ anchor: false });
    await waitFor(() => expect(document.getElementById('bid-anchorer-off')?.textContent).toMatch(/relies on other bidders/));
    expect((document.getElementById('bid-anchor') as HTMLInputElement).checked).toBe(false);
    expect((document.getElementById('bid-preventstall') as HTMLInputElement).checked).toBe(false);
    calls.length = 0;
    await fireEvent.click(screen.getByRole('button', { name: 'Apply' }));
    await waitFor(() => expect(calls.some((c) => c.method === 'rpc.confirmed')).toBe(true));
    expect(sentFields()).toEqual({ strategy: 'fees', preventstall: false, periodcap: '0.002' });
  });

  it('a declined switch stays as it was', async () => {
    answer = 'no';
    render(BiddingPanel);
    await screen.findByText('How to pay for anchors');
    const box = document.getElementById('bid-anchor') as HTMLInputElement;
    await fireEvent.click(box);
    await waitFor(() => expect(calls.some((c) => c.method === 'rpc.confirmed')).toBe(true));
    await waitFor(() => expect(box.checked).toBe(true));
    expect(document.getElementById('bid-anchorer-off')).toBeNull();
  });

  it('Fees + minimum: the recommended plan with a minimum on every block, under Advanced', async () => {
    render(BiddingPanel);
    await screen.findByText('How to pay for anchors');
    await fireEvent.click(document.getElementById('bid-advanced')!);
    await fireEvent.click(document.getElementById('bid-feesminimum')!);
    expect(document.getElementById('bid-with-minimum')).not.toBeNull();
    expect(document.getElementById('bid-floor')).not.toBeNull();
    expect((screen.getByRole('radio', { name: /^Recommended/ }) as HTMLInputElement).checked).toBe(true);
    await fireEvent.click(screen.getByRole('button', { name: 'Apply' }));
    await waitFor(() => expect(calls.some((c) => c.method === 'rpc.confirmed')).toBe(true));
    expect(sentFields()).toEqual({ strategy: 'feesminimum', preventstall: true });
  });

  it('a declined change is no error; a minimum above the maximum is not sent', async () => {
    answer = 'no';
    render(BiddingPanel);
    await screen.findByText('How to pay for anchors');
    await fireEvent.click(screen.getByRole('radio', { name: /^Off/ }));
    await fireEvent.click(screen.getByRole('button', { name: 'Apply' }));
    await waitFor(() => expect(calls.some((c) => c.method === 'rpc.confirmed')).toBe(true));
    expect(document.querySelector('.side-bad[role="alert"]')?.textContent ?? '').not.toMatch(/declined/);
    await fireEvent.click(screen.getByRole('radio', { name: /^Anchor every block/ }));
    await fireEvent.input(document.getElementById('bid-floor')!, { target: { value: '0.01' } });
    await waitFor(() => expect(document.getElementById('bid-problem')?.textContent).toMatch(/above the maximum/));
    expect((screen.getByRole('button', { name: 'Apply' }) as HTMLButtonElement).disabled).toBe(true);
  });

  it('a node that is not a validator: the same plans, no Anchor my validator, no warning', async () => {
    info = { ...info, validator: false, bidding: false, explanation: 'This node is not a validator: it bids only as a merged miner (setbmm), under this policy; merged mining is off.' };
    bid = { ...bid, validator: false, bidding: false, warnings: [] };
    render(BiddingPanel);
    await waitFor(() => expect(document.getElementById('bid-explanation')?.textContent).toMatch(/not a validator/));
    expect(screen.getAllByRole('radio')).toHaveLength(4);
    expect(document.getElementById('bid-anchor')).toBeNull();
    expect(document.getElementById('bid-trigger-mine')).not.toBeNull();
    expect(document.getElementById('bid-anchorer-off')).toBeNull();
  });

  it('a validator whose bidding is off: the warning that the chain relies on other bidders', async () => {
    info = { ...info, policy: { ...info.policy, strategy: 'off' } };
    render(BiddingPanel);
    await waitFor(() => expect(document.getElementById('bid-anchorer-off')?.textContent).toMatch(/Bidding is off.*relies on other bidders/));
    expect((document.getElementById('bid-anchor') as HTMLInputElement).disabled).toBe(true);
  });

  it('the fields sent: amounts in coins, the curve as text, nothing unchanged, the strategy with Prevent stalling', () => {
    expect(curveText(POLICY.curve)).toBe('0.5:1,0.75:2,0.9:4');
    expect(changedFields(POLICY, { ...POLICY })).toEqual({});
    expect(changedFields(POLICY, { ...POLICY, floor: 0.00001, max: 0.002, rebidafter: 0 })).toEqual({ floor: '0.00001', max: '0.002', rebidafter: 0 });
    expect(changedFields(POLICY, { ...POLICY, strategy: 'feesminimum' })).toEqual({ strategy: 'feesminimum', preventstall: true });
    expect(changedFields(POLICY, { ...POLICY, preventstall: false })).toEqual({ strategy: 'fees', preventstall: false });
    // 8.3.7: its amounts in coins too; a field the node does not have is never sent.
    expect(changedFields(POLICY_837, { ...POLICY_837, valueatrisk: 2, reservetarget: 0.0002, batch: 20, turns: false })).toEqual({ valueatrisk: '2', reservetarget: '0.0002', batch: 20, turns: false });
    expect(changedFields(POLICY, { ...POLICY, batch: undefined })).toEqual({});
  });

  it('the plans of a strategy, and what Recommended is', () => {
    expect(planOf('fees')).toBe('recommended');
    expect(planOf('feesminimum')).toBe('recommended');
    expect(planOf('minimum')).toBe('minimum');
    expect(isRecommended(POLICY)).toBe(true);
    expect(isRecommended(POLICY_837)).toBe(true);
    expect(isRecommended({ ...POLICY_837, batch: 5 })).toBe(false);
    expect(isRecommended({ ...POLICY, preventstall: false })).toBe(false);
    // Only the fields the node has.
    expect(recommended({ ...POLICY, strategy: 'fixed', preventstall: false })).toEqual(POLICY);
    expect(recommended({ ...POLICY_837, strategy: 'minimum', batch: 3, turns: false })).toEqual(POLICY_837);
  });
});

describe('the bidding panel on an 8.3.7 node (demand-driven anchoring)', () => {
  const ANCHORING: AnchoringInfo = {
    reserve: 0.00004,
    reservetarget: 0.0001,
    deficit: 0,
    valueatrisk: 0.3,
    batchneeded: 0.0000378,
    pot: 0.000012,
    nextheartbeat: 0,
    turn: { designated: false, mainheight: 973962 },
    day: { collected: 0.0002, paid: 0.00015, anchors: { fees: 3, heartbeat: 1 } },
    '7d': { collected: 0.001, paid: 0.0008 },
  };
  beforeEach(() => {
    info = { ...info, policy: { ...POLICY_837 }, defaults: { ...POLICY_837 } };
    ANCHORING.nextheartbeat = Math.floor(Date.now() / 1000) + 4 * 3600 + 12 * 60 + 30;
  });

  it('the status line: waiting for fees, the heartbeat, the value at risk, the reserve, the turn, the day spent', async () => {
    render(BiddingPanel, { anchoring: ANCHORING });
    await waitFor(() => expect(document.getElementById('bid-line')).not.toBeNull());
    const line = document.getElementById('bid-line')!.textContent!;
    expect(document.getElementById('bid-line-batch')!.textContent).toMatch(/Waiting for fees:.*0\.000012.*of.*0\.0000378.*needed/);
    expect(document.getElementById('bid-line-heartbeat')!.textContent).toMatch(/heartbeat in 4 h 12 m/);
    expect(document.getElementById('bid-line-risk')!.textContent).toMatch(/unanchored value.*0\.3.*\/.*1/);
    expect(document.getElementById('bid-line-reserve')!.textContent).toMatch(/reserve.*0\.00004.*\/.*0\.0001/);
    expect(document.getElementById('bid-line-reserve')!.textContent).not.toMatch(/short/);
    expect(document.getElementById('bid-line-turn')!.textContent).toMatch(/another validator's turn/);
    expect(line).toMatch(/spent today.*0\.0003.*of.*0\.001/);
    // The windows, under either spelling.
    expect(document.getElementById('bid-day')!.textContent).toMatch(/0\.0002.*paid.*0\.00015/);
    expect(document.getElementById('bid-week')!.textContent).toMatch(/0\.001.*paid.*0\.0008/);
    // The card says what the plan does now.
    expect(screen.getByRole('radio', { name: /^Recommended/ }).closest('label')!.textContent).toMatch(/in batches, with a heartbeat/);
  });

  it('a full batch, a reserve short, a heartbeat due, my turn', async () => {
    render(BiddingPanel, { anchoring: { ...ANCHORING, pot: 0.00005, deficit: 0.00002, nextheartbeat: 1, turn: { designated: true } } });
    await waitFor(() => expect(document.getElementById('bid-line-batch')?.textContent).toMatch(/Fees ready for an anchor/));
    expect(document.getElementById('bid-line-heartbeat')!.textContent).toMatch(/heartbeat due/);
    expect(document.getElementById('bid-line-reserve')!.textContent).toMatch(/short by.*0\.00002/);
    expect(document.getElementById('bid-line-turn')!.textContent).toMatch(/your turn to bid/);
  });

  it('getanchorinfo\'s bid may carry "anchoring" too', async () => {
    bid = { ...bid, anchoring: { reserve: 0.00001, reservetarget: 0.0001 } };
    render(BiddingPanel);
    await waitFor(() => expect(document.getElementById('bid-line-reserve')?.textContent).toMatch(/reserve.*0\.00001/));
  });

  it('the new fields under Advanced, sent as the node takes them; back to the recommended values', async () => {
    render(BiddingPanel, { anchoring: ANCHORING });
    await screen.findByText('How to pay for anchors');
    await fireEvent.click(document.getElementById('bid-advanced')!);
    expect((document.getElementById('bid-batch') as HTMLInputElement).value).toBe('10');
    expect((document.getElementById('bid-heartbeat') as HTMLInputElement).value).toBe('6');
    expect((document.getElementById('bid-reservepct') as HTMLInputElement).value).toBe('20');
    expect((document.getElementById('bid-turns') as HTMLInputElement).checked).toBe(true);
    expect((document.getElementById('bid-turnbackup') as HTMLInputElement).value).toBe('2');
    expect(document.getElementById('bid-adjusted')).toBeNull();
    await fireEvent.input(document.getElementById('bid-batch')!, { target: { value: '20' } });
    await fireEvent.input(document.getElementById('bid-valueatrisk')!, { target: { value: '2.5' } });
    await fireEvent.input(document.getElementById('bid-reservetarget')!, { target: { value: '0.0002' } });
    await fireEvent.click(document.getElementById('bid-turns')!);
    expect((document.getElementById('bid-turnbackup') as HTMLInputElement).disabled).toBe(true);
    // Off the recommended values: the card says so, and offers them back.
    await waitFor(() => expect(document.getElementById('bid-adjusted')).not.toBeNull());
    await fireEvent.click(screen.getByRole('button', { name: 'Apply' }));
    await waitFor(() => expect(calls.some((c) => c.method === 'rpc.confirmed')).toBe(true));
    expect(sentFields()).toEqual({ batch: 20, valueatrisk: '2.5', reservetarget: '0.0002', turns: false });
    await waitFor(() => expect(document.getElementById('bid-adjusted')).not.toBeNull());
    calls.length = 0;
    await fireEvent.click(screen.getByRole('button', { name: 'Use the recommended values' }));
    expect(document.getElementById('bid-adjusted')).toBeNull();
    await fireEvent.click(screen.getByRole('button', { name: 'Apply' }));
    await waitFor(() => expect(calls.some((c) => c.method === 'rpc.confirmed')).toBe(true));
    expect(sentFields()).toEqual({ batch: 10, valueatrisk: '1', reservetarget: '0.0001', turns: true });
  });

  it('choosing Recommended from another plan brings the recommended values with it', async () => {
    info = { ...info, policy: { ...POLICY_837, strategy: 'minimum', preventstall: false, heartbeathours: 0 } };
    render(BiddingPanel, { anchoring: ANCHORING });
    await screen.findByText('How to pay for anchors');
    // A heartbeat of 0 is off: none in the line.
    expect(document.getElementById('bid-line-heartbeat')).toBeNull();
    await fireEvent.click(screen.getByRole('radio', { name: /^Recommended/ }));
    await fireEvent.click(screen.getByRole('button', { name: 'Apply' }));
    await waitFor(() => expect(calls.some((c) => c.method === 'rpc.confirmed')).toBe(true));
    expect(sentFields()).toEqual({ strategy: 'fees', preventstall: true, heartbeathours: 6 });
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
