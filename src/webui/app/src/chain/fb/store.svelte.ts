// The state of FastBlocks' blocks of stake, shared by its pages and its cards on the home: what the node says
// every few seconds and with each block, while one of them is shown (startFb counts them; nothing is asked for
// while none is, nor while the window is hidden).
import { every } from '../../lib/poll';
import { rpc } from '../../lib/rpc';
import { node, wallets } from '../../lib/state.svelte';
import { errorText } from '../../lib/dialogs.svelte';
import type { AnchorInfo, Anchoring, FinalityBalances, PosInfo, Schedule, StakingInfo, ValidatorSet, WalletBond } from './model';
import { midnightOf } from './model';
import { request } from '../../lib/bridge';

/** How often the node is asked while a page of FastBlocks is shown (the Qt staking page's 3 s). */
export const POLL_MS = 3000;
/** The newest BMM blocks and tickets read (getanchorinfo's most). */
export const ANCHORS_READ = 100;

export const fb = $state({
  pos: null as PosInfo | null,
  staking: null as StakingInfo | null,
  anchors: null as AnchorInfo | null,
  /** The set of the current epoch. */
  set: null as ValidatorSet | null,
  /** The leader schedules of the current epoch and the next. */
  schedules: [] as Schedule[],
  /** The wallet's bonds (null without a wallet). */
  bonds: null as WalletBond[] | null,
  /** The wallet's coins by finality. */
  finality: null as FinalityBalances | null,
  anchoring: null as Anchoring | null,
  /** The tip the above are of. */
  height: -1,
  /** Why the node could not be asked (not a chain of stake, starting). */
  error: '',
  loaded: false,
  /** Counts the refreshes. */
  version: 0,
});

let users = 0;
let stop: (() => void) | null = null;
let running = false;
let again = false;
let lastTip = '';

async function attempt<T>(fn: () => Promise<T>): Promise<T | null> {
  try {
    return await fn();
  } catch {
    return null;
  }
}

/** Ask the node again (one refresh at a time; one asked for meanwhile follows). */
export async function refreshFb(): Promise<void> {
  if (running) {
    again = true;
    return;
  }
  running = true;
  try {
    const pos = await rpc.raw<PosInfo>('getposinfo', []);
    const wallet = wallets.current;
    const since = midnightOf(new Date());
    const [staking, anchors, set, now, next, bonds, balances, anchoring] = await Promise.all([
      attempt(() => rpc.raw<StakingInfo>('getstakinginfo', [since])),
      attempt(() => rpc.raw<AnchorInfo>('getanchorinfo', [ANCHORS_READ])),
      attempt(() => rpc.raw<ValidatorSet>('getvalidators', [])),
      attempt(() => rpc.raw<Schedule>('getleaderschedule', [pos.epoch])),
      attempt(() => rpc.raw<Schedule>('getleaderschedule', [pos.epoch + 1])),
      wallet === null ? Promise.resolve(null) : attempt(() => rpc.raw<WalletBond[]>('listbonds', [], { wallet })),
      wallet === null ? Promise.resolve(null) : attempt(() => rpc.raw<{ mine: { finality?: FinalityBalances } }>('getbalances', [], { wallet })),
      // (Read each time: the policy changes live, setanchorbidpolicy.)
      attempt(() => request<Anchoring>('host', 'fastblocks.anchoring', {})),
    ]);
    fb.pos = pos;
    fb.staking = staking;
    fb.anchors = anchors;
    fb.set = set;
    fb.schedules = [now, next].filter((s): s is Schedule => s !== null);
    fb.bonds = wallet === null ? null : (bonds ?? fb.bonds);
    fb.finality = wallet === null ? null : (balances?.mine.finality ?? null);
    fb.anchoring = anchoring;
    fb.height = node.tip?.height ?? -1;
    fb.error = '';
  } catch (e) {
    fb.error = errorText(e);
  } finally {
    fb.loaded = true;
    fb.version++;
    running = false;
    if (again) {
      again = false;
      void refreshFb();
    }
  }
}

/** Follow the state while a page or a card is shown: the stop to call when it goes. */
export function startFb(): () => void {
  users++;
  if (users === 1) stop = every(POLL_MS, () => void refreshFb(), { now: true });
  return () => {
    users = Math.max(0, users - 1);
    if (users === 0 && stop) {
      stop();
      stop = null;
    }
  };
}

/** With each block (and a wallet chosen), at once: the pages follow the chain at its pace, not only the timer's. */
export function followTip(): void {
  const tip = `${node.tip?.hash ?? ''}:${wallets.current ?? ''}`;
  if (tip === lastTip || users === 0) return;
  lastTip = tip;
  void refreshFb();
}

/** For the tests: as at start. */
export function resetFb(): void {
  fb.pos = null;
  fb.staking = null;
  fb.anchors = null;
  fb.set = null;
  fb.schedules = [];
  fb.bonds = null;
  fb.finality = null;
  fb.anchoring = null;
  fb.height = -1;
  fb.error = '';
  fb.loaded = false;
  fb.version = 0;
  lastTip = '';
}
