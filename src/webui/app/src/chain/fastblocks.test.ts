// The pages of FastBlocks: what fb/model.ts works out (the kind and finality of a block, the state of a
// validator, shares, slots, the income of the wallet, the checks of the bond form), the roles of coinbase outputs
// in the history, the explorer's columns of the chain, and the pages driven against a pretend node: Staking (this
// node, bonds through the host which asks the desktop, the validator sets, the schedule), Finality & BMM,
// Slashing & jail, the cards of the home, where the pages are in the menus, and the RPC policy of local.cpp.
// Layers: Staking is one section (Overview, Validator, Bonds, Network), Finality & BMM and Slashing & jail views
// of its Network tab.
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { afterEach, beforeAll, beforeEach, describe, expect, it } from 'vitest';
import { CHAIN } from './config';
import { EXPLORER } from './explorer';
import { LOCAL_COMMANDS, LOCAL_PAGES, stakingTabOf, type LocalPage } from './local';
import LocalCards from './LocalCards.svelte';
import { COINBASE_ROLES } from './roles';
import DialogHost from '../components/DialogHost.svelte';
import Explorer from '../pages/Explorer.svelte';
import Transactions from '../pages/Transactions.svelte';
import Staking from './pages/Staking.svelte';
import {
  blockKind,
  bondProblem,
  bpsText,
  feeSplitText,
  incomeTotal,
  type FeeSplit,
  finalityOf,
  formatShare,
  incomeOf,
  isKey,
  nodeState,
  sharePercent,
  slotsLed,
  untilText,
  validatorState,
  type Bond,
  type PosInfo,
  type Schedule,
  type StakingInfo,
  type WalletBond,
} from './fb/model';
import { fb, refreshFb, resetFb } from './fb/store.svelte';
import { dailyEstimate, secondsUntilEpoch, simpleState } from './fb/model';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { buildCommands, menuSections } from '../lib/commands.svelte';
import { dialogs } from '../lib/dialogs.svelte';
import { AREAS, chainBeforeTools, HOME_QUICK_ACTIONS, NAV_SECTIONS } from '../lib/layout';
import { go, nav } from '../lib/nav.svelte';
import { handleShortcut } from '../lib/keyboard';
import { app, node, prefs, toasts, wallets } from '../lib/state.svelte';
import { filterRecords, parseRecords, transactionsCsv, typeText, TYPE_FILTERS, type TxRecord } from '../lib/wallet';
import { resetWalletData } from '../lib/walletdata.svelte';

const K1 = '1'.repeat(64);
const K2 = '2'.repeat(64);
const K3 = '3'.repeat(64);
const hash = (c: string) => c.repeat(64);
const NOW = Math.floor(Date.now() / 1000);
const SLOT = 1000;

function bond(extra: Partial<Bond> = {}): Bond {
  return { owner: 'rsc1qowner', bonded: 1, unbonding: 0, unbonds: [], unbondsequence: 0, eligible: true, jaileduntilepoch: 0, banned: false, ...extra };
}

let pos: PosInfo;
let staking: StakingInfo;
let bonds: WalletBond[];
let allBonds: WalletBond[];
let evidence: { pubkey: string; slot: number; hashes: string[]; hex: string }[];
let records: unknown[][];
let confirmAnswer = true;
const calls: BridgeRequest[] = [];

function schedule(epoch: number): Schedule {
  const slots = [];
  for (let s = epoch * 20; s < (epoch + 1) * 20; s++) slots.push({ slot: s, time: NOW + (s - SLOT) * 30, ...(s % 4 === 1 ? { leader: K1 } : s % 4 === 3 ? {} : { leader: K2 }) });
  return { epoch, slots };
}

function block(h: number): Record<string, unknown> {
  // Heights: 18 a BMM block (an ack), 17 merged mined, the rest blocks of stake.
  const kind = h === 17 ? 'bmm' : 'pos';
  return {
    hash: hash(String(h % 10)),
    height: h,
    time: NOW - (20 - h) * 30,
    nTx: 1,
    size: 300,
    weight: 1200,
    difficulty: 4.6e-10,
    tx: [hash('c')],
    confirmations: 21 - h,
    merkleroot: hash('m'),
    bits: '207fffff',
    nonce: 0,
    version: 0x10000,
    mediantime: NOW,
    chainwork: '00',
    kind,
    ...(kind === 'pos' ? { slot: SLOT - (20 - h), proposer: h % 2 ? K1 : K2 } : { mainheight: 400 }),
    ...(h === 18 ? { ack: { ticket: hash('t'), anchored: hash('7'), payee: '00', mainheight: 401 } } : {}),
    finality: { status: h <= 15 ? 'final' : 'soft', anchors: h <= 15 ? 2 : 0 },
  };
}

/** The fees of a block, in satoshis (getblockstats' totalfee). */
let blockFee = 0;

async function fakeNode(r: BridgeRequest): Promise<unknown> {
  calls.push(r);
  const p = (r.params ?? []) as unknown[];
  const fail = (message: string, code = -8) => {
    throw Object.assign(new Error(message), { code });
  };
  switch (r.method) {
    case 'getposinfo':
      return pos;
    case 'getstakinginfo':
      return staking;
    case 'getanchorinfo':
      return {
        anchors: pos.anchors,
        finalanchors: 2,
        anchoredheight: 18,
        finalheight: 15,
        anchoredtip: hash('8'),
        finaltip: hash('5'),
        recent: [
          { block: hash('8'), height: 18, kind: 'ticket', anchored: hash('7'), anchoredheight: 17, mainheight: 401 },
          { block: hash('7'), height: 17, kind: 'bmm', anchored: hash('6'), anchoredheight: 16, mainheight: 400 },
        ],
        skipped: [],
        pot: { accrued: 2, paid: 1.5, due: 0.25 },
        tickets: [{ hash: hash('t'), anchored: hash('7'), payee: '0014ab', committedheight: 401 }],
      };
    case 'getvalidators': {
      const epoch = (p[0] as number | undefined) ?? pos.epoch;
      return {
        epoch,
        firstslot: epoch * 20,
        known: true,
        seedknown: true,
        active: true,
        totalstake: 300_000_000,
        validators: [
          { pubkey: K1, stake: 100_000_000, slots: 5, local: true, banned: false, jaileduntilepoch: 0 },
          { pubkey: K2, stake: 200_000_000, slots: 10, local: false, banned: false, jaileduntilepoch: epoch === pos.epoch ? epoch + 2 : 0 },
        ],
      };
    }
    case 'getleaderschedule':
      return schedule((p[0] as number | undefined) ?? pos.epoch);
    case 'listbonds':
      return p[0] === true ? allBonds : bonds;
    case 'getbalances':
      return { mine: { trusted: 3, untrusted_pending: 0, immature: 0, finality: { soft: 1, anchored: 0.5, final: 1.5 } } };
    case 'listevidence':
      return evidence;
    case 'submitevidence':
      if (p[0] !== 'abcd') return fail('The proof does not read: two extended headers are expected', -22);
      return { pubkey: K3, slot: 990, new: true };
    case 'getbestblockhash':
      return hash('0');
    case 'getblockhash':
      return hash(String((p[0] as number) % 10));
    case 'getblock': {
      const h = [...Array(21).keys()].reverse().find((x) => hash(String(x % 10)) === p[0]) ?? 20;
      return block(h);
    }
    case 'getblockstats':
      return { subsidy: 0, totalfee: blockFee, avgfeerate: 0 };
    case 'getrawtransaction':
      return { vin: [{ coinbase: '00' }] };
    case 'getrawmempool':
      return {};
    case 'explorer.blocks':
      return [20, 19, 18, 17, 16].map((h) => [h, hash(String(h % 10)), NOW - (20 - h) * 30, 1, 4.6e-10]);
    case 'fastblocks.anchoring':
      return { on: false, validator: true, defaultbid: '0.00001' };
    case 'fastblocks.bond':
    case 'fastblocks.unbond':
    case 'fastblocks.staking':
    case 'fastblocks.pop':
      // The host asks the desktop first: a no is BRIDGE_CANCELLED, and the node is not asked.
      if (!confirmAnswer) return fail('declined', -32800);
      if (r.method === 'fastblocks.bond') return { txid: hash('b'), owner: 'rsc1qowner', bonded: 2, eligible: true };
      if (r.method === 'fastblocks.unbond') return { txid: hash('u'), amount: 1, all: true, owner: 'rsc1qowner', releaseanchor: 70 };
      if (r.method === 'fastblocks.pop') return { pubkey: K1, pop: 'ab'.repeat(64) };
      staking = { ...staking, enabled: (r.params as { enabled: boolean }).enabled };
      return { enabled: staking.enabled };
    case 'wallet.transactions':
      return { height: 20, hash: hash('0'), maturity: 0, records };
    case 'app.notify':
    case 'app.copy':
      return true;
    default:
      return null;
  }
}

beforeAll(() => {
  const proto = HTMLDialogElement.prototype as unknown as Record<string, unknown>;
  proto.showModal ??= function (this: HTMLDialogElement) {
    this.setAttribute('open', '');
  };
  proto.close ??= function (this: HTMLDialogElement) {
    this.removeAttribute('open');
  };
  (globalThis as Record<string, unknown>).ResizeObserver ??= class {
    observe() {}
    unobserve() {}
    disconnect() {}
  };
  (globalThis as Record<string, unknown>).IntersectionObserver ??= class {
    observe() {}
    unobserve() {}
    disconnect() {}
  };
  window.matchMedia ??= ((query: string) => ({ matches: false, media: query, addEventListener() {}, removeEventListener() {} })) as unknown as typeof window.matchMedia;
});

beforeEach(() => {
  app.info = { name: CHAIN.name, version: 'test', bugreport: '', network: 'regtest', networkLabel: 'regtest', datadir: '/tmp', walletEnabled: true, devtools: false, test: true, platform: 'linux', blockSpacing: 30 };
  prefs.unit = 'CHN';
  prefs.mask = false;
  node.tip = { height: 20, time: NOW, hash: hash('0'), progress: 1, state: 'synced', ibd: false, loadingBlocks: false };
  wallets.list = [{ name: 'w', encrypted: false, locked: false, privateKeysDisabled: false, hd: true }];
  wallets.current = 'w';
  pos = {
    active: true,
    slotseconds: 15,
    slot: SLOT,
    epoch: 50,
    leader: K2,
    validators: [],
    anchors: 50,
    lastanchormainheight: 401,
    lastanchoredheight: 17,
    lastposslot: SLOT - 1,
    firstunanchoredslot: SLOT - 2,
    stalled: false,
    pot: { accrued: 2, paid: 1.5, reserve: 0.1 },
    staking: { bonded: 3, unbonding: 0.5, rewardsowed: 0.01, slashed: 0.1, minstake: 0.01, unbondinganchors: 20 },
    chainkey: '00',
    tickets: 1,
    evidence: 1,
  };
  staking = {
    validator: true,
    enabled: true,
    producererror: '',
    payout: 'rsc1qpayout',
    keys: [{ pubkey: K1, inset: true, stake: 100_000_000, bond: bond({ bonded: 1 }) }],
    stake: 100_000_000,
    totalstake: 300_000_000,
    produced: 7,
    blocksmade: 12,
    blocksmadesince: 3,
    acknowledgements: 2,
    failed: 1,
    lastslot: SLOT - 3,
    lastsignedslot: SLOT - 3,
    error: '',
    slot: SLOT,
    nextslot: SLOT + 1,
    nexttime: NOW + 30,
  };
  bonds = [{ ...bond({ bonded: 1, unbonding: 0.5, unbonds: [{ amount: 0.5, releaseanchor: 60 }] }), validator: K1, inset: true }];
  allBonds = [...bonds, { ...bond({ owner: 'rsc1qother', bonded: 2, jaileduntilepoch: 52 }), validator: K2, inset: true }, { ...bond({ owner: 'rsc1qbad', bonded: 0.9, banned: true, eligible: false }), validator: K3, inset: false }];
  evidence = [{ pubkey: K3, slot: 990, hashes: [hash('a'), hash('b')], hex: 'ff'.repeat(20) }];
  const rec = (i: number, role: string, amount: number, time: number) => [hash(String(i)), 0, 'mined', 'rsc1qpayout', '', time, amount, 0, 'confirmed', 10, 0, true, `k${i}`, 5, 'c', true, false, true, role];
  records = [rec(1, 'staked', 10_000_000, NOW), rec(2, 'staked', 5_000_000, NOW - 3 * 86400), rec(3, 'bmm', 2_000_000, NOW), rec(4, 'slashing', 1_000_000, NOW - 3 * 86400), rec(5, '', 50_000_000, NOW)];
  confirmAnswer = true;
  calls.length = 0;
  resetFb();
  setTransport(fakeNode);
});

afterEach(() => {
  resetFb();
  setTransport(null);
  dialogs.queue.splice(0);
  toasts.list.splice(0);
  wallets.list = [];
  wallets.current = null;
  resetWalletData();
  nav.tab = '';
  nav.focus = null;
});

const called = (method: string) => calls.filter((c) => c.method === method);
const loaded = () => waitFor(() => expect(fb.loaded && fb.pos !== null).toBe(true));

describe('what the pages of FastBlocks work out', () => {
  it('a block is PoS, BMM (a block of stake that records a BMM commitment) or PoW (merged mined)', () => {
    expect(blockKind(block(16))).toBe('pos');
    expect(blockKind(block(18))).toBe('bmm');
    expect(blockKind(block(17))).toBe('pow');
    expect(blockKind({ height: 1 })).toBe('');
  });

  it('how final a block is, as of the tips read last, not as getblock said when it was read', () => {
    const tips = { anchored: 18, final: 15, finalAnchors: 2 };
    expect(finalityOf(block(15), tips)).toBe('final');
    expect(finalityOf(block(17), tips)).toBe('anchored');
    expect(finalityOf(block(19), tips)).toBe('soft');
    // Without the tips, the block's own word; off the active chain, none.
    expect(finalityOf(block(15), { anchored: -1, final: -1, finalAnchors: 2 })).toBe('final');
    expect(finalityOf({ ...block(15), finality: undefined }, tips)).toBeUndefined();
  });

  it('the state of a validator and of this node', () => {
    expect(validatorState(bond(), true, 50)).toBe('active');
    expect(validatorState(bond({ banned: true }), true, 50)).toBe('banned');
    expect(validatorState(bond({ jaileduntilepoch: 52 }), false, 50)).toBe('jailed');
    expect(validatorState(bond({ jaileduntilepoch: 50 }), false, 50)).toBe('joining');
    expect(validatorState(bond({ bonded: 0, unbonding: 1, eligible: false }), false, 50)).toBe('unbonding');
    expect(validatorState(bond({ eligible: false }), false, 50)).toBe('low');
    expect(validatorState(undefined, false, 50)).toBe('none');
    expect(nodeState(null, 50)).toBe('nokey');
    expect(nodeState({ ...staking, validator: false, producererror: 'cannot lock' }, 50)).toBe('error');
    expect(nodeState(staking, 50)).toBe('active');
    expect(nodeState({ ...staking, enabled: false }, 50)).toBe('off');
    expect(nodeState({ ...staking, keys: [{ pubkey: K1, inset: false, stake: 0, bond: bond({ jaileduntilepoch: 51 }) }] }, 50)).toBe('jailed');
    expect(nodeState({ ...staking, keys: [{ pubkey: K1, inset: true, stake: 1, bond: bond() }, { pubkey: K2, inset: false, stake: 0, bond: bond({ banned: true }) }] }, 50)).toBe('banned');
  });

  it('shares, slots and countdowns', () => {
    expect(sharePercent(1, 3)).toBeCloseTo(33.33, 1);
    expect(sharePercent(1, 0)).toBe(0);
    expect(formatShare(33.333)).toBe('33.3 %');
    expect(formatShare(0.01)).toBe('< 0.1 %');
    const next = slotsLed([schedule(50), schedule(51)], new Set([K1]), SLOT + 1, 3);
    expect(next.map((s) => s.slot)).toEqual([1001, 1005, 1009]);
    expect(slotsLed([schedule(50)], new Set([K3]), SLOT, 3)).toEqual([]);
    expect(untilText(0)).toBe('now');
    expect(untilText(42)).toBe('42 s');
    expect(untilText(200)).toBe('3 min 20 s');
    expect(untilText(3900)).toBe('1 h 5 min');
  });

  it("the wallet's staking income: its coinbase outputs by role, today and in all", () => {
    const r = parseRecords(records);
    const income = incomeOf(r, NOW - 3600);
    expect(income.staked).toBe(15_000_000n);
    expect(income.today.staked).toBe(10_000_000n);
    expect(income.bmm).toBe(2_000_000n);
    expect(income.slashing).toBe(1_000_000n);
    expect(income.today.slashing).toBe(0n);
    expect(income.blocks).toBe(2);
    expect(income.today.blocks).toBe(1);
  });

  it('the bond form says what is wrong before anything is sent', () => {
    const minStake = 1_000_000n;
    expect(isKey(K1)).toBe(true);
    expect(isKey('xyz')).toBe(false);
    expect(bondProblem('bond', '', 1n, undefined, minStake)).toMatch(/validator key/);
    expect(bondProblem('bond', 'abc', 1n, undefined, minStake)).toMatch(/64 hexadecimal/);
    expect(bondProblem('bond', K1, null, undefined, minStake)).toMatch(/amount/);
    expect(bondProblem('bond', K1, 0n, undefined, minStake)).toMatch(/positive/);
    expect(bondProblem('bond', K1, 100n, undefined, minStake)).toBe('');
    expect(bondProblem('add', K1, 100n, undefined, minStake)).toMatch(/no bond/);
    expect(bondProblem('bond', K1, 100n, bond({ banned: true }), minStake)).toMatch(/banned/);
    expect(bondProblem('unbond', K1, 100n, undefined, minStake)).toMatch(/nothing bonded/);
    expect(bondProblem('unbond', K1, 200_000_000n, bond({ bonded: 1 }), minStake)).toMatch(/at most/);
    // What stays must be nothing or at least the minimum stake.
    expect(bondProblem('unbond', K1, 99_500_000n, bond({ bonded: 1 }), minStake)).toMatch(/minimum stake/);
    expect(bondProblem('unbond', K1, 50_000_000n, bond({ bonded: 1 }), minStake)).toBe('');
    expect(bondProblem('unbond', K1, 100_000_000n, bond({ bonded: 1 }), minStake)).toBe('');
    expect(bondProblem('unbondAll', K1, null, bond({ bonded: 1 }), minStake)).toBe('');
  });
});

describe('the staking income in the history', () => {
  it('a coinbase output with a role is Staked, BMM reward or Slashing reward, and the type filter has them', () => {
    const r = parseRecords(records);
    expect(r.map((x) => typeText(x.type, x.role))).toEqual(['Staked', 'Staked', 'BMM reward', 'Slashing reward', 'Mined']);
    expect(Object.keys(COINBASE_ROLES)).toEqual(['staked', 'bmm', 'slashing', 'validator']);
    expect(typeText('mined', 'validator')).toBe('Validator share');
    expect(TYPE_FILTERS.map((f) => f.label)).toEqual(expect.arrayContaining(['Staked', 'BMM reward', 'Slashing reward']));
    const only = (type: string) => filterRecords(r, { type: type as never, search: '', minAmount: 0n, showInactive: false }).map((x: TxRecord) => x.role);
    expect(only('role:staked')).toEqual(['staked', 'staked']);
    expect(only('role:bmm')).toEqual(['bmm']);
    expect(only('mined')).toHaveLength(5);
    // A record of an older host (no role) is the kit's.
    expect(parseRecords([records[0].slice(0, 18)])[0].role).toBe('');
  });

  it('a block of stake without fees pays its producer a Staked output of 0: not a row of the history, the notices or the CSV', async () => {
    const zero = [hash('z'), 0, 'mined', 'rsc1qpayout', '', NOW, 0, 0, 'confirmed', 10, 0, true, 'kz', 6, 'c', true, false, true, 'staked'];
    const zeroBmm = [hash('y'), 1, 'mined', 'rsc1qpayout', '', NOW, 0, 0, 'confirmed', 10, 0, true, 'ky', 6, 'c', true, false, true, 'bmm'];
    const r = parseRecords([...records, zero, zeroBmm]);
    expect(r.map((x) => x.role)).toEqual(['staked', 'staked', 'bmm', 'slashing', '']);
    expect(r.some((x) => x.amount === 0n)).toBe(false);
    expect(transactionsCsv(parseRecords([zero]), 'CHN').trim().split('\n')).toHaveLength(1);
    // Those that pay stay, whatever their role.
    expect(parseRecords([records[0], zero]).map((x) => x.amount)).toEqual([10_000_000n]);
    // The history of the page leaves it out too.
    records = [...records, zero, zeroBmm];
    render(Transactions);
    await waitFor(() => expect(screen.getAllByText('Staked').length).toBeGreaterThan(0));
    expect(document.body.textContent).toContain('5 of 5');
    expect([...document.querySelectorAll('.amt')].map((a) => a.textContent)).not.toContain('0.00000000');
  });

  it('the host gives the roles of a coinbase (walletapi.cpp, local::CoinbaseRoles)', () => {
    const walletapi = readFileSync(join(__dirname, '../../../walletapi.cpp'), 'utf8');
    expect(walletapi).toMatch(/roles = local::CoinbaseRoles\(w\.chain\(\), confirmed->confirmed_block_hash\)/);
    expect(walletapi).toMatch(/r\.push_back\(rec\.role\)/);
    const local = readFileSync(join(__dirname, '../../../local.cpp'), 'utf8');
    expect(local).toMatch(/CoinbaseRole::STAKED: roles\.emplace_back\("staked"\)/);
    expect(local).toMatch(/CoinbaseRole::ANCHOR: roles\.emplace_back\("bmm"\)/);
    expect(local).toMatch(/CoinbaseRole::REPORTER: roles\.emplace_back\("slashing"\)/);
  });
});

describe('the explorer of FastBlocks', () => {
  it('its columns after the height: kind, slot, producer, mainchain block, finality; no difficulty', async () => {
    expect(EXPLORER.columns.map((c) => c.label)).toEqual(['Kind', 'Slot', 'Producer', 'Mainchain', 'Finality']);
    expect([16, 17, 18].map((h) => EXPLORER.columns[3].value(block(h)))).toEqual([undefined, 400, 401]);
    expect(EXPLORER.hide).toEqual(['difficulty', 'weight']);
    const kind = EXPLORER.columns[0];
    expect([16, 17, 18].map((h) => kind.value(block(h)))).toEqual(['PoS', 'PoW', 'BMM']);
    expect(kind.badge!(block(18))!.tone).toBe('fb-kind-bmm');
    await EXPLORER.refresh!();
    const finality = EXPLORER.columns[4];
    expect([15, 17, 19].map((h) => finality.value(block(h)))).toEqual(['final', 'BMM-confirmed', 'soft']);
    // This node's key is "this node".
    expect(EXPLORER.columns[2].value(block(19))).toBe('this node');
    expect(EXPLORER.columns[2].value(block(16))).toMatch(/^222222…/);
    expect(EXPLORER.tag!(block(18))).toBe('BMM');
  });

  it("a block's details start with its kind, producer, BMM commitment, finality and proofs", () => {
    const facts = EXPLORER.facts({ ...block(18), evidence: [{ slot: 1, hashes: [] }] });
    expect(facts.map((f) => f.dt)).toEqual(['Kind', 'Producer', 'BMM block', 'Finality', 'Proofs']);
    expect(facts[2].dd).toMatch(/yes: it records the BMM commitment of mainchain block .*401/);
    // The block of this chain the commitment covers links to its view (when the block names it).
    const anchored = (block(18).ack as { anchored?: string } | undefined)?.anchored;
    expect(facts[2].link).toEqual(anchored ? { kind: 'block', id: anchored } : undefined);
    expect(EXPLORER.facts(block(17))[0].dd).toMatch(/PoW: merged mined, committed at mainchain block .*400/);
    expect(EXPLORER.facts({ height: 1 })).toEqual([]);
  });

  it("the ribbon's target is a slot: chain.json's 15 s, then the slot of the network as the node says it (regtest's own)", async () => {
    expect(CHAIN.blockTarget).toBe(15);
    pos.slotseconds = 1;
    await EXPLORER.refresh!();
    expect(EXPLORER.target!()).toBe(1);
    pos.slotseconds = 15;
    await EXPLORER.refresh!();
    expect(EXPLORER.target!()).toBe(15);
    render(Explorer);
    await waitFor(() => expect(document.querySelector('.ribbon-head')?.textContent).toContain('target 15s'));
    expect(document.querySelector('.ribbon-head')!.textContent).not.toContain('target 1m');
  });

  it('the page lists the blocks with their kind, and leaves the difficulty out', async () => {
    render(Explorer);
    await waitFor(() => expect(screen.getAllByText('BMM').length).toBeGreaterThan(0));
    const heads = [...document.querySelectorAll('[role="columnheader"]')].map((h) => h.textContent?.replace(/[▲▼]/g, '').trim());
    expect(heads.slice(0, 6)).toEqual(['Height', 'Kind', 'Slot', 'Producer', 'Mainchain', 'Finality']);
    expect(heads).not.toContain('Difficulty');
    expect(screen.getAllByText('PoW').length).toBeGreaterThan(0);
  });
});

describe('the pages of FastBlocks', () => {
  it('are in the Chain section and the menus, before the tools, with Alt+6 to Alt+9 and a quick action', () => {
    const own = (LOCAL_PAGES as readonly LocalPage[]).map((p) => p.id);
    // Layers: Staking, one section, then the pages of the assets.
    expect(own).toEqual(['staking', 'assets', 'my-assets', 'issue', 'trade', 'auctions']);
    expect(NAV_SECTIONS.find((s) => s.title === 'Chain')!.pages).toEqual(['peg', ...own, 'explorer']);
    expect(AREAS.find((a) => a.id === 'chain')!.tabs.map((t) => t.page)).toEqual(['peg', ...own, 'explorer']);
    expect(chainBeforeTools(HOME_QUICK_ACTIONS.map((q) => q.page), (p) => p)).toBeNull();
    expect(HOME_QUICK_ACTIONS.map((q) => q.label)).toContain('Stake');
    const chain = menuSections(buildCommands()).find((s) => s.group === 'Chains')!;
    for (const c of LOCAL_COMMANDS) expect(chain.items.map((i) => i.label)).toContain(c.label);
    chain.items.find((c) => c.label === 'Bond coins…')!.run();
    expect([nav.page, nav.tab]).toEqual(['staking', 'bonds']);
    for (const [key, page] of [['6', 'staking'], ['7', 'assets'], ['8', 'my-assets'], ['9', 'trade']]) {
      go('overview');
      expect(handleShortcut(new KeyboardEvent('keydown', { key, altKey: true }))).toBe(true);
      expect(nav.page).toBe(page);
    }
    expect(CHAIN.name).toBe('Layers');
    expect(CHAIN.uriScheme).toBe('layers');
    expect(CHAIN.networks.test.p2p).toBe(19255);
    expect(CHAIN.networks.main.hrp).toBe('ly');
    // The menus' items of the views of Network open Staking there.
    chain.items.find((c) => c.label === 'BMM blocks and finality')!.run();
    expect([nav.page, nav.tab]).toEqual(['staking', 'finality']);
    chain.items.find((c) => c.label === 'Proofs of double signing')!.run();
    expect([nav.page, nav.tab]).toEqual(['staking', 'slashing']);
  });

  it("Staking, Overview: the leader of this slot, the fee split, the wallet's stake, income and coins by finality", async () => {
    render(Staking);
    await loaded();
    expect(screen.getByRole('tab', { name: /Overview/ }).getAttribute('aria-selected')).toBe('true');
    await waitFor(() => expect(screen.getByRole('table', { name: 'Staking income' }).textContent).toMatch(/0\.10000000.*0\.15000000/));
    expect(screen.getByText('BMM-confirmed')).toBeTruthy();
    // (The fee split is in Overview too, when the node says it: 'the fee split' tests below.)
    expect(document.querySelector('#fb-leader-now')!.textContent).not.toBe('');
    // No fact of another tab: this node's state is in Validator.
    expect(document.querySelector('#fb-node-state')).toBeNull();
    expect(screen.queryByRole('list', { name: 'Slots this node leads next' })).toBeNull();
  });

  it("Staking, Validator: its state, keys, next slots; stopping asks the desktop", async () => {
    nav.tab = 'validator';
    render(Staking);
    render(DialogHost);
    await loaded();
    await waitFor(() => expect(document.querySelector('#fb-node-state')!.textContent).toBe('Active'));
    expect(screen.getByText('rsc1qpayout')).toBeTruthy();
    expect(screen.getByRole('list', { name: 'Slots this node leads next' }).textContent).toMatch(/Slot 1.001/);
    // The wallet's income is in Overview, not here.
    expect(screen.queryByRole('table', { name: 'Staking income' })).toBeNull();
    // Stop staking: through the host, which asks the desktop (a no changes nothing).
    confirmAnswer = false;
    await fireEvent.click(document.querySelector('#fb-staking-stop')!);
    await waitFor(() => expect(called('fastblocks.staking').length).toBe(1));
    expect(called('setstaking')).toHaveLength(0);
    expect(dialogs.queue).toHaveLength(0);
    confirmAnswer = true;
    await fireEvent.click(document.querySelector('#fb-staking-stop')!);
    await waitFor(() => expect(document.querySelector('#fb-node-state')!.textContent).toBe('Staking off'));
    expect(called('fastblocks.staking')[1].params).toEqual({ enabled: false });
  });

  it('Staking, Bonds: bond, add, unbond all through the host; nothing sent while the form is wrong', async () => {
    nav.tab = 'bonds';
    render(Staking);
    render(DialogHost);
    await loaded();
    await waitFor(() => expect(screen.getByRole('table', { name: 'Bonds of this wallet' })).toBeTruthy());
    const key = document.querySelector<HTMLInputElement>('#fb-bond-key')!;
    // This node's key by default.
    await waitFor(() => expect(key.value).toBe(K1));
    const submit = document.querySelector<HTMLButtonElement>('#fb-bond-submit')!;
    expect(submit.disabled).toBe(true);
    await fireEvent.input(key, { target: { value: 'not a key' } });
    await waitFor(() => expect(screen.getByRole('alert').textContent).toMatch(/64 hexadecimal/));
    await fireEvent.input(key, { target: { value: K1 } });
    await fireEvent.input(document.querySelector('#fb-bond-amount')!, { target: { value: '1' } });
    await waitFor(() => expect(submit.disabled).toBe(false));
    await fireEvent.click(screen.getByRole('button', { name: 'Add to bond' }));
    await fireEvent.click(submit);
    await waitFor(() => expect(called('fastblocks.bond').length).toBe(1));
    expect(called('fastblocks.bond')[0].params).toEqual({ wallet: 'w', pubkey: K1, amount: '1' });
    expect(called('createbond')).toHaveLength(0);
    await waitFor(() => expect(toasts.list.map((x) => x.title)).toContain('Bond sent'));
    // Unbond all: no amount.
    await fireEvent.click(screen.getByRole('button', { name: 'Unbond all' }));
    await waitFor(() => expect(submit.disabled).toBe(false));
    await fireEvent.click(submit);
    await waitFor(() => expect(called('fastblocks.unbond').length).toBe(1));
    expect(called('fastblocks.unbond')[0].params).toEqual({ wallet: 'w', pubkey: K1, amount: 'all' });
    // A no on the desktop: nothing said, nothing sent.
    confirmAnswer = false;
    await waitFor(() => expect(submit.disabled).toBe(false));
    await fireEvent.click(submit);
    await waitFor(() => expect(called('fastblocks.unbond').length).toBe(2));
    expect(dialogs.queue).toHaveLength(0);
    // Unbond more than is bonded: refused here.
    await fireEvent.click(screen.getByRole('button', { name: 'Unbond' }));
    await fireEvent.input(document.querySelector('#fb-bond-amount')!, { target: { value: '5' } });
    await waitFor(() => expect(screen.getByRole('alert').textContent).toMatch(/at most/));
    expect(submit.disabled).toBe(true);
  });

  it("Staking, Bonds: a bond to another node's key takes its proof of possession; this node signs one through the host", async () => {
    nav.tab = 'bonds';
    render(Staking);
    render(DialogHost);
    await loaded();
    const key = document.querySelector<HTMLInputElement>('#fb-bond-key')!;
    await fireEvent.input(key, { target: { value: K3 } });
    await fireEvent.input(document.querySelector('#fb-bond-amount')!, { target: { value: '0.5' } });
    await fireEvent.click(await screen.findByLabelText(/The key is of another node/));
    await fireEvent.input(document.querySelector('#fb-bond-pop')!, { target: { value: 'cd'.repeat(64) } });
    await fireEvent.input(document.querySelector('#fb-bond-owner')!, { target: { value: 'rsc1qmine' } });
    await fireEvent.click(document.querySelector('#fb-bond-submit')!);
    await waitFor(() => expect(called('fastblocks.bond').length).toBe(1));
    expect(called('fastblocks.bond')[0].params).toEqual({ wallet: 'w', pubkey: K3, amount: '0.5', pop: 'cd'.repeat(64), owner: 'rsc1qmine' });
    await fireEvent.input(document.querySelector('#fb-pop-owner')!, { target: { value: 'rsc1qelsewhere' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Sign the proof' }));
    await waitFor(() => expect(screen.getByText('ab'.repeat(64))).toBeTruthy());
    expect(called('fastblocks.pop')[0].params).toEqual({ owner: 'rsc1qelsewhere' });
  });

  it('Staking, Network, Validator set: the set of an epoch, jailed ones said, another epoch on request; every bond on the chain', async () => {
    nav.tab = 'validators';
    render(Staking);
    await loaded();
    expect(screen.getByRole('tab', { name: /Network/ }).getAttribute('aria-selected')).toBe('true');
    expect(document.querySelector('#fb-view-validators')!.getAttribute('aria-selected')).toBe('true');
    const table = await screen.findByRole('table', { name: 'Validator set' });
    await waitFor(() => expect(within(table).getAllByRole('row')).toHaveLength(3));
    expect(table.textContent).toMatch(/33\.3 %/);
    expect(table.textContent).toMatch(/Jailed until epoch 52/);
    expect(table.textContent).toMatch(/this node/);
    await fireEvent.click(screen.getAllByRole('button', { name: 'Previous epoch' })[0]);
    await waitFor(() => expect(called('getvalidators').some((c) => (c.params as unknown[])[0] === 49)).toBe(true));
    await waitFor(() => expect(screen.getByText('Validator set of epoch 49')).toBeTruthy());
    const all = await screen.findByRole('table', { name: 'Every validator on the chain' });
    await waitFor(() => expect(all.textContent).toMatch(/Banned/));
    expect(called('listbonds').some((c) => (c.params as unknown[])[0] === true)).toBe(true);
  });

  it("Staking, Schedule: the leaders of the epoch's slots, this node's only, and the epochs", async () => {
    nav.tab = 'schedule';
    render(Staking);
    await loaded();
    const table = await screen.findByRole('table', { name: 'Leader schedule' });
    expect(table.textContent).toMatch(/nobody/);
    await fireEvent.click(screen.getByLabelText("This node's slots"));
    await waitFor(() => expect(within(screen.getByRole('table', { name: 'Leader schedule' })).getAllByRole('row')).toHaveLength(6));
    const epochs = await screen.findByRole('table', { name: 'Epochs' });
    await waitFor(() => expect(within(epochs).getAllByRole('row').length).toBeGreaterThan(5));
  });

  it('Staking, Network, Finality & BMM: how far the chain is confirmed, the blocks waiting, the newest BMM blocks, the pot and the bids', async () => {
    nav.tab = 'finality';
    render(Staking);
    await loaded();
    expect(document.querySelector('#fb-view-finality')!.getAttribute('aria-selected')).toBe('true');
    await waitFor(() => expect(document.querySelector('#fb-anchored-height')!.textContent).toBe('block 18'));
    const waiting = await screen.findByRole('table', { name: 'Blocks waiting for BMM confirmation' });
    await waitFor(() => expect(within(waiting).getAllByRole('row')).toHaveLength(3));
    const bmm = screen.getByRole('table', { name: 'Newest BMM blocks' });
    expect(bmm.textContent).toMatch(/BMM/);
    expect(bmm.textContent).toMatch(/PoW/);
    expect(screen.getByText(/own bidder is off/)).toBeTruthy();
    expect(screen.getByRole('table', { name: 'Tickets' }).textContent).toMatch(/401/);
  });

  it('Staking, Network, Slashing & jail: the proofs, the jailed and banned validators, and a proof given by hand', async () => {
    nav.tab = 'slashing';
    render(Staking);
    render(DialogHost);
    await loaded();
    // The views of Network one click apart.
    await fireEvent.click(document.querySelector('#fb-view-schedule')!);
    expect(nav.tab).toBe('schedule');
    nav.tab = 'slashing';
    const proofs = await screen.findByRole('table', { name: 'Proofs of double signing' });
    expect(proofs.textContent).toMatch(/990/);
    const held = await screen.findByRole('table', { name: 'Jailed and banned validators' });
    await waitFor(() => expect(within(held).getAllByRole('row')).toHaveLength(3));
    expect(held.textContent).toMatch(/Banned/);
    expect(held.textContent).toMatch(/Jailed until epoch 52/);
    await fireEvent.input(screen.getByLabelText('The proof, in hex'), { target: { value: 'abcd' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Give the proof' }));
    await waitFor(() => expect(called('submitevidence')[0].params).toEqual(['abcd']));
  });

  it('the cards of the home: staking, finality, next slots', async () => {
    render(LocalCards);
    await loaded();
    await waitFor(() => expect(document.querySelector('.fb-cards')).not.toBeNull());
    const text = document.querySelector('.fb-cards')!.textContent!;
    expect(text).toMatch(/Active/);
    expect(text).toMatch(/block 18/);
    expect(text).toMatch(/Slot 1.001/);
  });

  it('the store follows the node; a chain without blocks of stake says so', async () => {
    await refreshFb();
    expect(fb.staking?.validator).toBe(true);
    expect(fb.schedules.map((s) => s.epoch)).toEqual([50, 51]);
    setTransport(async (r) => {
      if (r.method === 'getposinfo') throw Object.assign(new Error('This chain is not a sidechain: it has no blocks of stake'), { code: -1 });
      return null;
    });
    await refreshFb();
    expect(fb.error).toMatch(/no blocks of stake/);
  });
});

describe('the host side of the pages (local.cpp)', () => {
  const local = readFileSync(join(__dirname, '../../../local.cpp'), 'utf8');
  const allowed = [...local.slice(local.indexOf('RpcMethods()'), local.indexOf('KeyedRpcMethods()')).matchAll(/\{"(\w+)", Bridge::Thread/g)].map((m) => m[1]);

  it('the pages read with the RPCs of the node, and nothing on the list spends, bonds or signs', () => {
    // Layers: FastBlocks' nine, then those of the assets (bitassets.test.ts checks them).
    const fbs = ['getanchorinfo', 'getleaderschedule', 'getposinfo', 'getstakinginfo', 'getvalidator', 'getvalidators', 'listbonds', 'listevidence', 'submitevidence'];
    expect(allowed.filter((m) => fbs.includes(m)).sort()).toEqual(fbs);
    expect(allowed.filter((m) => !fbs.includes(m)).every((m) => /asset|pool|auction|quote/.test(m))).toBe(true);
    for (const m of ['createbond', 'createunbond', 'setstaking', 'signbondpop']) expect(allowed).not.toContain(m);
  });

  it('bonds, unbonds, staking off and proofs of possession ask the desktop first', () => {
    // Bonds and unbonds: in the dialog of the host with the keys (the passphrase of a locked wallet typed there), the RPC run
    // only inside it (audit 7 #1).
    for (const [method, rpc] of [['fastblocks.bond', 'createbond'], ['fastblocks.unbond', 'createunbond']]) {
      const body = local.slice(local.indexOf(`"${method}"`));
      const ask = body.indexOf('return keys(wallet_name, ');
      expect(ask).toBeGreaterThan(0);
      expect(body.indexOf(`[&] { return n.executeRpc("${rpc}"`)).toBeGreaterThan(ask);
      expect(body.slice(0, body.indexOf('}, Bridge::Thread::LONG);'))).not.toMatch(/passphrase/);
    }
    {
      const body = local.slice(local.indexOf('"fastblocks.pop"'));
      const ask = body.indexOf('Ask(confirm');
      expect(ask).toBeGreaterThan(0);
      expect(body.indexOf('executeRpc("signbondpop"')).toBeGreaterThan(ask);
    }
    expect(local).toMatch(/if \(!enabled\.get_bool\(\)\) Ask\(confirm, StopStakingConfirmation\(\)/);
  });
});

// Simple mode (lib/uimode.svelte.ts, user decision 2026-10-16): staking as one card, in words; the same host
// methods as the Bonds tab; the home's one card.
describe('Staking in Simple mode', () => {
  beforeEach(() => {
    prefs.uiMode = 'simple';
    blockFee = 10_000;
  });
  afterEach(() => {
    blockFee = 0;
  });

  it('says the state in words, and estimates what a stake earns a day', () => {
    expect(simpleState('active')).toBe('earning');
    expect(simpleState('joining')).toBe('joining');
    expect(simpleState('jailed')).toBe('paused-jailed');
    expect(simpleState('off')).toBe('paused-off');
    expect(simpleState('nokey')).toBe('nokey');
    // A quarter of the stake, a block every 15 s (5760 a day), 10 000 sat of fees a block, half to the producer.
    expect(dailyEstimate(100_000_000n, 300_000_000n, 15, 10_000)).toBe(7_200_000n);
    expect(dailyEstimate(0n, 300_000_000n, 15, 10_000)).toBe(0n);
    expect(dailyEstimate(100_000_000n, 0n, 15, 0)).toBe(0n);
    // The epoch after next, 20 slots an epoch, from slot 1000 of epoch 50 (first slot 1000): 40 slots of 15 s.
    expect(secondsUntilEpoch(52, { epoch: 50, firstslot: 1000 }, 20, 1000, 15)).toBe(600);
    expect(secondsUntilEpoch(50, { epoch: 50, firstslot: 1000 }, 20, 1000, 15)).toBe(0);
  });

  it('one card: earning, staked, an estimate, Add more and Stop staking with the calls of Advanced', async () => {
    render(Staking);
    render(DialogHost);
    await loaded();
    await waitFor(() => expect(document.querySelector('#fb-simple-state')?.textContent).toBe('Earning'));
    expect(screen.queryByRole('tab', { name: /Validators/ })).toBeNull();
    expect(document.querySelector('.side-stats')).toBeNull();
    await fireEvent.input(document.querySelector('#fb-simple-amount')!, { target: { value: '1' } });
    await waitFor(() => expect(document.querySelector('#fb-simple-estimate')?.textContent).toMatch(/You'd earn about .* a day\. An estimate/));
    const stake = document.querySelector<HTMLButtonElement>('#fb-simple-stake')!;
    expect(stake.textContent).toMatch(/Add more/);
    await waitFor(() => expect(stake.disabled).toBe(false));
    await fireEvent.click(stake);
    await waitFor(() => expect(called('fastblocks.bond').length).toBe(1));
    expect(called('fastblocks.bond')[0].params).toEqual({ wallet: 'w', pubkey: K1, amount: '1' });
    await waitFor(() => expect(document.querySelector<HTMLButtonElement>('#fb-simple-stop')?.disabled).toBe(false));
    await fireEvent.click(document.querySelector('#fb-simple-stop')!);
    await waitFor(() => expect(called('fastblocks.unbond').length).toBe(1));
    expect(called('fastblocks.unbond')[0].params).toEqual({ wallet: 'w', pubkey: K1, amount: 'all' });
    expect(called('createbond')).toHaveLength(0);
  });

  it('without fees, says that staking earns next to nothing now', async () => {
    blockFee = 0;
    render(Staking);
    await loaded();
    await fireEvent.input(await waitFor(() => document.querySelector('#fb-simple-amount')!), { target: { value: '1' } });
    await waitFor(() => expect(document.querySelector('#fb-simple-estimate')?.textContent).toMatch(/next to no fees/));
  });

  it('a first stake is Start staking; the validators when asked for', async () => {
    bonds = [];
    render(Staking);
    await loaded();
    await waitFor(() => expect(document.querySelector('#fb-simple-stake')?.textContent).toMatch(/Start staking/));
    expect(document.querySelector('#fb-simple-stop')).toBeNull();
    nav.tab = 'validators';
    await waitFor(() => expect(screen.getByRole('tab', { name: /Network/ }).getAttribute('aria-selected')).toBe('true'));
    expect(document.querySelector('#fb-view-validators')!.getAttribute('aria-selected')).toBe('true');
  });

  it('not set up, paused or jailed, in words', async () => {
    staking = { ...staking, validator: false, keys: [] };
    const first = render(Staking);
    await loaded();
    await waitFor(() => expect(document.querySelector('#fb-simple-state')?.textContent).toBe('Not set up: this node has no validator key'));
    expect(document.querySelector('#fb-simple-amount')).toBeNull();
    first.unmount();
    resetFb();
    staking = { ...staking, validator: true, enabled: false, keys: [{ pubkey: K1, inset: true, stake: 100_000_000, bond: bond({ bonded: 1 }) }] };
    const second = render(Staking);
    await loaded();
    await waitFor(() => expect(document.querySelector('#fb-simple-state')?.textContent).toBe('Paused: staking is turned off'));
    await fireEvent.click(document.querySelector('#fb-simple-start-again')!);
    await waitFor(() => expect(called('fastblocks.staking')[0]?.params).toEqual({ enabled: true }));
    second.unmount();
    resetFb();
    staking = { ...staking, enabled: true, keys: [{ pubkey: K1, inset: false, stake: 0, bond: bond({ bonded: 1, jaileduntilepoch: 52 }) }] };
    render(Staking);
    await loaded();
    await waitFor(() => expect(document.querySelector('#fb-simple-state')?.textContent).toBe('Paused: your node missed its turns'));
  });

  it('the home: one card, in words', async () => {
    render(LocalCards);
    await loaded();
    await waitFor(() => expect(document.querySelector('#fb-home-simple-state')?.textContent).toBe('Earning'));
    expect(document.querySelectorAll('.fb-cards .card').length).toBe(1);
    expect(document.querySelector('.fb-cards')!.textContent).not.toMatch(/block 18/);
  });

  it('Finality and Slashing leave the navigation, in Staking now; Staking stays', () => {
    const pages = LOCAL_PAGES as readonly LocalPage[];
    // Layers: one section for staking, in Simple mode as in Advanced; the two pages are views of its Network tab.
    expect(pages.find((p) => p.id === 'finality')).toBeUndefined();
    expect(pages.find((p) => p.id === 'slashing')).toBeUndefined();
    expect(pages.find((p) => p.id === 'staking')?.advanced).toBeFalsy();
    expect(stakingTabOf('finality')).toEqual({ tab: 'network', view: 'finality' });
    expect(stakingTabOf('slashing')).toEqual({ tab: 'network', view: 'slashing' });
    expect(stakingTabOf('node')).toEqual({ tab: 'validator', view: 'validators' });
    expect(stakingTabOf('bonds').tab).toBe('bonds');
    expect(stakingTabOf('').tab).toBe('overview');
  });
});

describe('the fee split (doc/fastblocks.md, "Fees, the pot and the validator pool")', () => {
  const split = (extra: Partial<FeeSplit> = {}): FeeSplit => ({
    active: true,
    activationheight: 30,
    mainheight: 400,
    spacing: 600,
    bidderbps: 1000,
    validatorsbps: 9000,
    bmmmainheight: 410,
    bmmspacing: 60,
    bmmbidderbps: 5000,
    pool: 0.0009,
    poolepochs: [{ epoch: 50, amount: 0.0009 }],
    ...extra,
  });

  it('says the split now, in percent, and before its height when it starts', () => {
    expect(bpsText(5000)).toBe('50 %');
    expect(bpsText(1000)).toBe('10 %');
    expect(bpsText(3333)).toBe('33.33 %');
    expect(bpsText(6667)).toBe('66.67 %');
    expect(feeSplitText(split())).toBe('Fee split now: validators 90 % / BMM 10 %');
    expect(feeSplitText(split({ spacing: 60, bidderbps: 5000, validatorsbps: 5000 }))).toBe('Fee split now: validators 50 % / BMM 50 %');
    expect(feeSplitText(split({ active: false, bidderbps: 5000, validatorsbps: 5000 }))).toBe('Fee split from block 30: validators 50 % / BMM 50 %');
    expect(feeSplitText(undefined)).toBe('');
  });

  it("a validator's share of the pool is income of the wallet: Validator share, in the totals", () => {
    const share = [hash('6'), 0, 'mined', 'rsc1qowner', '', NOW, 3_000_000, 0, 'confirmed', 10, 0, true, 'k6', 5, 'c', true, false, true, 'validator'];
    const r = parseRecords([...records, share]);
    const income = incomeOf(r, NOW - 3600);
    expect(income.validator).toBe(3_000_000n);
    expect(income.today.validator).toBe(3_000_000n);
    expect(incomeTotal(income)).toBe(income.staked + income.bmm + income.slashing + 3_000_000n);
    expect(incomeTotal(income, true)).toBe(income.today.staked + income.today.bmm + income.today.slashing + 3_000_000n);
    expect(TYPE_FILTERS.map((f) => f.label)).toContain('Validator share');
    const local = readFileSync(join(__dirname, '../../../local.cpp'), 'utf8');
    expect(local).toMatch(/CoinbaseRole::VALIDATOR: roles\.emplace_back\("validator"\)/);
  });

  it('the Staking page shows the split now, the pool and this node\'s share', async () => {
    pos = { ...pos, feesplit: split() };
    staking = { ...staking, poolshare: 0.0003, feesplit: split() };
    render(Staking);
    await loaded();
    await waitFor(() => expect(document.querySelector('#fb-fee-split')!.textContent).toContain('Fee split now: validators 90 % / BMM 10 %'));
    expect(document.querySelector('#fb-fee-split')!.textContent).toContain('every 600 s');
    expect(document.querySelector('#fb-pool')!.textContent).toMatch(/0\.00090000/);
    expect(document.querySelector('#fb-pool-share')!.textContent).toMatch(/0\.00030000/);
    await waitFor(() => expect(screen.getByRole('table', { name: 'Staking income' }).textContent).toContain('Validator shares'));
  });

  it('without a split on the chain (an older node) the page says nothing of it', async () => {
    render(Staking);
    await loaded();
    expect(document.querySelector('#fb-fee-split')).toBeNull();
  });
});
