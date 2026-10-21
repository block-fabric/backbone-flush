// What the pages of FastBlocks work out from what the node says (src/rpc/pos.cpp, src/wallet/rpc/sidechain.cpp),
// without the node: the kind and the finality of a block, the state of a validator, the share of a stake, the
// income of the wallet, the slots this node leads next, the checks of the forms. Pure functions, tested in
// chain/fastblocks.test.ts.
import { formatCount } from '../../lib/format';
import { t, tn } from '../../lib/i18n.svelte';
import type { TxRecord } from '../../lib/wallet';
import { coinsToSats } from '../../lib/units';

// --- What the node says ---

/** The fee split (getposinfo, getstakinginfo: feesplit; doc/fastblocks.md, "Fees, the pot and the validator pool"). */
export interface FeeSplit {
  active: boolean;
  activationheight: number;
  mainheight: number;
  spacing: number;
  bidderbps: number;
  validatorsbps: number;
  bmmmainheight: number;
  bmmspacing: number;
  bmmbidderbps: number;
  pool: number;
  poolepochs: { epoch: number; amount: number }[];
}

export interface PosInfo {
  active: boolean;
  slotseconds: number;
  slot: number;
  epoch: number;
  leader?: string;
  validators: { pubkey: string; stake: number; poolshare?: number }[];
  anchors: number;
  lastanchormainheight: number;
  lastanchored?: string;
  lastanchoredheight: number;
  anchorpoint?: string;
  lastposslot: number;
  firstunanchoredslot: number;
  /** Blocks of stake go on this many slots past the first one no anchor covers (a mainchain gap); older nodes: absent. */
  maxunanchoredslots?: number;
  /** The set of an epoch is fixed at least this long before it (the eCash networks); older nodes: absent. */
  setleadseconds?: number;
  seedlookaheadseconds?: number;
  stalled: boolean;
  pot: { accrued: number; paid: number; reserve: number };
  staking: { bonded: number; unbonding: number; rewardsowed: number; slashed: number; minstake: number; unbondinganchors: number };
  chainkey: string;
  tickets: number;
  evidence: number;
  producer?: { keys: string[]; produced: number; acknowledgements: number; failed: number; lastslot: number; error: string };
  feesplit?: FeeSplit;
}

export interface Bond {
  owner: string;
  bonded: number;
  genesisstake?: number;
  unbonding: number;
  unbonds: { amount: number; releaseanchor: number }[];
  unbondsequence: number;
  eligible: boolean;
  jaileduntilepoch: number;
  banned: boolean;
}

/** listbonds: a bond, with its validator and whether it is in the set of the current epoch. */
export interface WalletBond extends Bond {
  validator: string;
  inset: boolean;
}

export interface StakingInfo {
  validator: boolean;
  enabled: boolean;
  producererror: string;
  payout?: string;
  keys: { pubkey: string; inset: boolean; stake: number; poolshare?: number; bond?: Bond }[];
  stake: number;
  totalstake: number;
  poolshare?: number;
  feesplit?: FeeSplit;
  produced: number;
  blocksmade: number;
  blocksmadesince?: number;
  acknowledgements: number;
  failed: number;
  lastslot: number;
  lastsignedslot: number;
  error: string;
  slot: number;
  nextslot?: number;
  nexttime?: number;
}

export interface ValidatorSet {
  epoch: number;
  firstslot: number;
  known: boolean;
  seedknown: boolean;
  active: boolean;
  totalstake: number;
  validators: { pubkey: string; stake: number; slots: number; local: boolean; banned: boolean; jaileduntilepoch: number }[];
}

export interface Schedule {
  epoch: number;
  slots: { slot: number; time: number; leader?: string; banned?: string }[];
}

export interface AnchorInfo {
  anchors: number;
  finalanchors: number;
  anchoredtip?: string;
  anchoredheight: number;
  finaltip?: string;
  finalheight: number;
  recent: { block: string; height: number; kind: 'ticket' | 'bmm'; anchored: string; anchoredheight: number; mainheight: number }[];
  skipped: { mainheight: number; fork: 'any' | 'from' | 'at'; forkheight?: number }[];
  pot: { accrued: number; paid: number; due: number };
  tickets: { hash: string; anchored: string; payee: string; committedheight?: number }[];
}

export interface Evidence {
  pubkey: string;
  slot: number;
  hashes: string[];
  hex: string;
}

/** getbalances: the wallet's coins by finality (FastBlocks adds them to "mine"). */
export interface FinalityBalances {
  soft: number;
  anchored: number;
  final: number;
}

/** fastblocks.anchoring (local.cpp): this node's own bid for BMM commitments, as a validator. */
export interface Anchoring {
  on: boolean;
  bid?: string;
  payee?: string;
  validator: boolean;
  option?: string;
  defaultbid: string;
}

// --- Blocks ---

/** A block's kind as the explorer says it: a block of stake, one that records a BMM commitment, a merged mined one. */
export type BlockKind = 'pos' | 'bmm' | 'pow' | '';

export function blockKind(block: Record<string, unknown>): BlockKind {
  if (block.kind === 'pos') return block.ack ? 'bmm' : 'pos';
  if (block.kind === 'bmm') return 'pow';
  return '';
}

export function kindText(kind: BlockKind): string {
  return kind === 'pos' ? t('PoS') : kind === 'bmm' ? t('BMM') : kind === 'pow' ? t('PoW') : '';
}

export interface FinalityTips {
  /** Heights of the highest BMM-confirmed and final blocks (getanchorinfo), -1 if none or not known. */
  anchored: number;
  final: number;
  finalAnchors: number;
}

export type Finality = 'soft' | 'anchored' | 'final';

/**
 * How final a block is: as of the tips the explorer read last (a block becomes BMM-confirmed, then final, while
 * what getblock said of it stays as it was read); a block not on the active chain has none.
 */
export function finalityOf(block: Record<string, unknown>, tips: FinalityTips): Finality | undefined {
  const own = block.finality as { status?: string } | undefined;
  if (!own || typeof own.status !== 'string') return undefined;
  const height = typeof block.height === 'number' ? block.height : -1;
  if (tips.final >= 0 && height <= tips.final) return 'final';
  if (tips.anchored >= 0 && height <= tips.anchored) return 'anchored';
  if (tips.anchored >= 0) return 'soft';
  return own.status === 'final' || own.status === 'anchored' ? own.status : 'soft';
}

export function finalityText(f: Finality): string {
  return f === 'final' ? t('final') : f === 'anchored' ? t('BMM-confirmed') : t('soft');
}

// --- Validators ---

/** The state of a validator key: what it does in the epoch shown. */
export type ValidatorState = 'banned' | 'jailed' | 'active' | 'joining' | 'unbonding' | 'low' | 'none';

/**
 * The state of a key from its bond (in the state of the active chain) and whether it is in the set of the
 * epoch: banned (signed two blocks for a slot), jailed (left out of the sets until an epoch), in the set,
 * joining (enough bonded, in a coming set), unbonding (all of its bond on its way back), too little bonded.
 */
export function validatorState(bond: Bond | undefined, inSet: boolean, epoch: number): ValidatorState {
  if (bond?.banned) return 'banned';
  if (bond && bond.jaileduntilepoch > epoch) return 'jailed';
  if (inSet) return 'active';
  if (!bond) return 'none';
  if (bond.eligible) return 'joining';
  if (bond.bonded <= 0 && bond.unbonding > 0) return 'unbonding';
  return 'low';
}

export function stateText(state: ValidatorState, bond?: Bond): string {
  switch (state) {
    case 'banned':
      return t('Banned');
    case 'jailed':
      return t('Jailed until epoch %1', bond?.jaileduntilepoch ?? 0);
    case 'active':
      return t('Active');
    case 'joining':
      return t('Joins a coming set');
    case 'unbonding':
      return t('Unbonding');
    case 'low':
      return t('Too little bonded');
    case 'none':
      return t('Not bonded');
  }
}

export function stateTip(state: ValidatorState): string {
  switch (state) {
    case 'banned':
      return t('It signed two blocks for one slot: never in a validator set again, and slashed once.');
    case 'jailed':
      return t('It missed too many of its slots: left out of the validator sets until then. No coins are lost.');
    case 'active':
      return t('In the validator set of this epoch: it leads slots, in proportion to its stake.');
    case 'joining':
      return t('Enough is bonded: it takes part in the validator sets from the epoch after next (on eCash, about ten hours later: a set is fixed before its seed is known).');
    case 'unbonding':
      return t('All of its bond is on its way back to the owner.');
    case 'low':
      return t('Less than the minimum stake is bonded: it takes part once it has that much.');
    case 'none':
      return t('No bond to this key in the state of the chain.');
  }
}

/** The tone of a state: the colour of its pill (status ok, wait, bad). */
export function stateTone(state: ValidatorState): 'ok' | 'wait' | 'bad' {
  if (state === 'active') return 'ok';
  if (state === 'banned' || state === 'jailed') return 'bad';
  return 'wait';
}

/** The state of this node as a validator: no key, staking off, or the state of its keys (the worst first). */
export type NodeState = 'nokey' | 'error' | 'off' | ValidatorState;

export function nodeState(staking: StakingInfo | null, epoch: number): NodeState {
  if (!staking) return 'nokey';
  if (!staking.validator) return staking.producererror ? 'error' : 'nokey';
  const states = staking.keys.map((k) => validatorState(k.bond, k.inset, epoch));
  // A banned key first; then whether a key leads slots; then the rest, the most hopeful first.
  if (states.includes('banned')) return 'banned';
  if (states.includes('active')) return staking.enabled ? 'active' : 'off';
  if (states.includes('jailed')) return 'jailed';
  if (!staking.enabled) return 'off';
  return (['joining', 'unbonding', 'low'] as const).find((s) => states.includes(s)) ?? 'none';
}

export function nodeStateText(state: NodeState): string {
  if (state === 'nokey') return t('Not a validator');
  if (state === 'error') return t('Producer stopped');
  if (state === 'off') return t('Staking off');
  return stateText(state);
}

/** The share of a stake in a set, in percent (0 for an empty set). */
export function sharePercent(stake: number, total: number): number {
  return total > 0 ? (100 * stake) / total : 0;
}

export function formatShare(percent: number): string {
  if (percent === 0) return '0 %';
  if (percent < 0.1) return '< 0.1 %';
  return `${percent.toFixed(1)} %`;
}

/** A stake as the sets weigh it: in satoshis of bonded coins (getvalidators' stake, a number). */
export function stakeSats(stake: number): bigint {
  return BigInt(Math.round(stake));
}

// --- Slots ---

/** The slots of the schedules (in order) that one of these keys leads, from slot `from` on, at most `count`. */
export function slotsLed(schedules: readonly Schedule[], keys: ReadonlySet<string>, from: number, count: number): { slot: number; time: number; leader: string }[] {
  const out: { slot: number; time: number; leader: string }[] = [];
  for (const s of schedules) {
    for (const slot of s.slots) {
      if (slot.slot < from || !slot.leader || !keys.has(slot.leader)) continue;
      out.push({ slot: slot.slot, time: slot.time, leader: slot.leader });
      if (out.length >= count) return out;
    }
  }
  return out;
}

/** A time to come, short: "1 h 5 min", "3 min 20 s", "12 s"; "now" for one that has come. */
export function untilText(seconds: number): string {
  const s = Math.max(0, Math.floor(seconds));
  if (s === 0) return t('now');
  if (s >= 3600) return t('%1 h %2 min', Math.floor(s / 3600), Math.floor((s % 3600) / 60));
  if (s >= 60) return t('%1 min %2 s', Math.floor(s / 60), s % 60);
  return t('%1 s', s);
}

/** About how long n BMM commitments take: one per mainchain block, ten minutes each on the main networks. */
export function anchorsTime(n: number, mainBlockSeconds = 600): string {
  const secs = n * mainBlockSeconds;
  if (secs >= 2 * 86400) return tn('about %n day(s)', Math.round(secs / 86400));
  if (secs >= 2 * 3600) return tn('about %n hour(s)', Math.round(secs / 3600));
  return tn('about %n minute(s)', Math.max(1, Math.round(secs / 60)));
}

// --- The wallet's income ---

export interface Income {
  staked: bigint;
  bmm: bigint;
  slashing: bigint;
  validator: bigint;
  blocks: number;
  today: { staked: bigint; bmm: bigint; slashing: bigint; validator: bigint; blocks: number };
}

/** All of an income's parts (today's with `today`). */
export function incomeTotal(income: Income, today = false): bigint {
  const i = today ? income.today : income;
  return i.staked + i.bmm + i.slashing + i.validator;
}

/** Basis points as a percentage: 5000 is "50 %", 3333 "33.33 %". */
export function bpsText(bps: number): string {
  const whole = bps % 100 === 0;
  return `${whole ? String(bps / 100) : (bps / 100).toFixed(2)} %`;
}

/**
 * The split of ordinary fees between the validators and the BMM bidder, as the next block of stake takes it
 * ("Fee split now: validators 90 % / BMM 10 %"); before the height it activates at, when it does.
 */
export function feeSplitText(split: FeeSplit | undefined): string {
  if (!split) return '';
  const parts = t('validators %1 / BMM %2', bpsText(split.validatorsbps), bpsText(split.bidderbps));
  return split.active ? t('Fee split now: %1', parts) : t('Fee split from block %1: %2', formatCount(split.activationheight), parts);
}

/** The staking income of the wallet: its coinbase outputs by role (chain/roles.ts), today (from midnight) and in all. */
export function incomeOf(records: readonly TxRecord[], midnight: number): Income {
  const income: Income = { staked: 0n, bmm: 0n, slashing: 0n, validator: 0n, blocks: 0, today: { staked: 0n, bmm: 0n, slashing: 0n, validator: 0n, blocks: 0 } };
  for (const r of records) {
    if (r.type !== 'mined' || r.status === 'conflicted' || r.status === 'notaccepted') continue;
    const today = r.time >= midnight;
    if (r.role === 'staked') {
      income.staked += r.amount;
      income.blocks++;
      if (today) {
        income.today.staked += r.amount;
        income.today.blocks++;
      }
    } else if (r.role === 'bmm') {
      income.bmm += r.amount;
      if (today) income.today.bmm += r.amount;
    } else if (r.role === 'slashing') {
      income.slashing += r.amount;
      if (today) income.today.slashing += r.amount;
    } else if (r.role === 'validator') {
      income.validator += r.amount;
      if (today) income.today.validator += r.amount;
    }
  }
  return income;
}

/** Local midnight of a time, in seconds. */
export function midnightOf(now: Date): number {
  return Math.floor(new Date(now.getFullYear(), now.getMonth(), now.getDate()).getTime() / 1000);
}

// --- The forms ---

/** Whether the text is a validator key: an x-only key, 64 hex digits. */
export function isKey(text: string): boolean {
  return /^[0-9a-fA-F]{64}$/.test(text.trim());
}

export type BondAction = 'bond' | 'add' | 'unbond' | 'unbondAll';

/**
 * What is wrong with a bond or an unbond before it is sent ('' if nothing): the key, the amount, a bond to add
 * to or take from, at most what is bonded, and what stays bonded nothing or at least the minimum stake.
 */
export function bondProblem(action: BondAction, key: string, sats: bigint | null, existing: Bond | undefined, minStake: bigint): string {
  if (!key.trim()) return t('Enter a validator key, or pick a bond of the wallet.');
  if (!isKey(key)) return t('A validator key is an x-only key: 64 hexadecimal digits.');
  if (existing?.banned && (action === 'bond' || action === 'add')) return t('This validator is banned: it signed two blocks for a slot.');
  if (action === 'add' && !existing) return t('This wallet has no bond to this key: bond to it first.');
  if (action === 'unbond' || action === 'unbondAll') {
    if (!existing || existing.bonded <= 0) return t('This wallet has nothing bonded to this key.');
    if (action === 'unbondAll') return '';
  }
  if (sats === null) return t('Enter the amount.');
  if (sats <= 0n) return t('The amount must be positive.');
  if (action === 'unbond' && existing) {
    const bonded = coinsToSats(existing.bonded);
    if (sats > bonded) return t('The bond has %1 bonded: unbond at most that much.', String(existing.bonded));
    if (sats < bonded && bonded - sats < minStake) return t('What stays bonded must be nothing or at least the minimum stake: unbond all, or less.');
  }
  return '';
}

/** A short key for a table: its first and last digits. */
export function shortKey(key: string): string {
  return key.length > 20 ? `${key.slice(0, 10)}…${key.slice(-8)}` : key;
}

// --- Simple mode: staking in plain words ---

/** The state of this node's staking as Simple mode says it. */
export type SimpleState = 'earning' | 'joining' | 'paused-off' | 'paused-jailed' | 'banned' | 'stopping' | 'low' | 'notstaking' | 'nokey' | 'error';

export function simpleState(state: NodeState): SimpleState {
  switch (state) {
    case 'active':
      return 'earning';
    case 'joining':
      return 'joining';
    case 'off':
      return 'paused-off';
    case 'jailed':
      return 'paused-jailed';
    case 'banned':
      return 'banned';
    case 'unbonding':
      return 'stopping';
    case 'low':
      return 'low';
    case 'none':
      return 'notstaking';
    case 'error':
      return 'error';
    case 'nokey':
      return 'nokey';
  }
}

/** The tone of a state in Simple mode: the colour of its pill. */
export function simpleTone(s: SimpleState): 'ok' | 'wait' | 'bad' {
  if (s === 'earning') return 'ok';
  if (s === 'paused-jailed' || s === 'banned' || s === 'error' || s === 'paused-off') return 'bad';
  return 'wait';
}

/**
 * Seconds until a slot of an epoch to come, from the set of the current epoch (its first slot) and the length
 * of an epoch in slots: when a key that is waiting joins (the epoch after next), when a jail ends.
 */
export function joinEpoch(slot: number, epochSlots: number, slotSeconds: number, setLeadSeconds = 0): number {
  // The first epoch whose set an anchor after `slot` fixes (pos::SetBoundary): an epoch before it starts, and
  // set_lead_seconds before it (the eCash networks).
  if (epochSlots <= 0) return 0;
  const lead = slotSeconds > 0 ? Math.ceil(setLeadSeconds / slotSeconds) : 0;
  let e = Math.floor(slot / epochSlots) + 2;
  while (Math.min((e - 1) * epochSlots, e * epochSlots - lead) <= slot) e++;
  return e;
}

export function secondsUntilEpoch(epoch: number, current: { epoch: number; firstslot: number }, epochSlots: number, slot: number, slotSeconds: number): number {
  if (epochSlots <= 0 || epoch <= current.epoch) return 0;
  const first = current.firstslot + (epoch - current.epoch) * epochSlots;
  return Math.max(0, (first - slot) * slotSeconds);
}

/**
 * About what a stake would earn a day, in satoshis: its share of the total stake (with it), of the blocks of a
 * day (one a slot), of the producer's share of their fees (at most half: the rest goes to the pot), at the average
 * fees of the latest blocks. An estimate: fees change, a validator misses slots, the set changes.
 */
export function dailyEstimate(stake: bigint, otherStake: bigint, slotSeconds: number, avgFeeSats: number, producerShare = 0.5): bigint {
  const total = stake + otherStake;
  if (stake <= 0n || total <= 0n || slotSeconds <= 0 || avgFeeSats <= 0) return 0n;
  const blocksPerDay = 86400 / slotSeconds;
  const share = Number((stake * 1_000_000n) / total) / 1_000_000;
  return BigInt(Math.round(share * blocksPerDay * avgFeeSats * producerShare));
}
