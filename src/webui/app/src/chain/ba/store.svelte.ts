// The state of BitAssets, kept up to date while the app runs: what the node says (getbitassetsinfo, the
// assets, the pools, the auctions) and what the wallet holds and did (listmyassets, listassetactivity), asked
// again with every block of this chain and every few seconds, one refresh at a time.
//
// And a journal of what the chain does not keep, in a file of the data directory of the network (host methods
// bitassets.store.*, src/webui/local.cpp): the assets being created (the name, supply, decimals and data to
// register once the reservation is deep enough, as chains-qt kept them in QSettings; registered by themselves,
// as chains-qt did), and the prices of the pools as the app saw them (the charts).
import { BridgeError, request } from '../../lib/bridge';
import { errorText, walletRpc, withKeys } from '../../lib/dialogs.svelte';
import { t } from '../../lib/i18n.svelte';
import { every } from '../../lib/poll';
import { rpc } from '../../lib/rpc';
import { node, toast, wallets } from '../../lib/state.svelte';
import { WalletError } from '../../lib/wallet';
import type { Activity, Asset, AssetData, Auction, BitAssetsInfo, Holdings, Pool } from './model';

/** An asset this app is creating: reserved, then registered once the reservation is deep enough. */
export interface PendingIssue {
  wallet: string;
  /** The name, as typed: text of the user. */
  name: string;
  /** The hash of the name (reserveasset), by which the chain will know it. */
  asset: string;
  /** The initial supply, in whole assets, as the node takes it. */
  supply: string;
  decimals: number;
  data: AssetData;
  public: boolean;
  /** When it was reserved (seconds), and the reservation's transaction. */
  time: number;
  reservation: string;
  /** The registration, once sent. */
  registration?: string;
  /** Why the last registration failed (an error of the node, shown as it is). */
  error?: string;
}

export interface PricePoint {
  /** The height of this chain. */
  h: number;
  /** Seconds. */
  t: number;
  /** The price of the first asset (`a` of the series) in the second. */
  p: number;
}

export interface PriceSeries {
  /** The id of the asset priced: the first of the pair. */
  a: string;
  points: PricePoint[];
}

export interface Journal {
  pending: PendingIssue[];
  prices: Record<string, PriceSeries>;
}

const MAX_PENDING = 100;
const MAX_POINTS = 120;
const MAX_PRICED_POOLS = 60;

export function emptyJournal(): Journal {
  return { pending: [], prices: {} };
}

export type MineState = 'none' | 'loading' | 'ok' | 'locked' | 'error';

/** Assets read a call (listassets count), and at most in all: past that, the Assets page asks the node as the user types. */
export const ASSET_PAGE = 1000;
export const MAX_ASSETS = 20_000;
/** Entries of the asset activity read at first, and more each time "Show more" is pressed (up to MAX_ACTIVITY). */
export const ACTIVITY_PAGE = 200;
export const MAX_ACTIVITY = 5000;

/**
 * Every asset of the chain, in the order of registration, in pages of ASSET_PAGE (audit port-2 #4: a single
 * listassets [1000, 0] left the newest out once there were more); past MAX_ASSETS, the newest MAX_ASSETS
 * (`total`: the assets the node counts, getbitassetsinfo).
 */
export async function readAllAssets(total: number | Promise<number>): Promise<Asset[]> {
  // The first page at once, with the other calls of a refresh; the count of the node decides the rest.
  const first = rpc.raw<Asset[]>('listassets', [ASSET_PAGE, 0]);
  first.catch(() => undefined);
  const from = Math.max(0, (await total) - MAX_ASSETS);
  const out: Asset[] = [];
  for (let skip = from; skip < from + MAX_ASSETS; skip += ASSET_PAGE) {
    const page = (await (skip === 0 ? first : rpc.raw<Asset[]>('listassets', [ASSET_PAGE, skip]))) ?? [];
    out.push(...page);
    if (page.length < ASSET_PAGE) break;
  }
  return out;
}

/** What the pages ask for beyond the usual: more of the activity of the wallet. */
export const baView = $state({ activity: ACTIVITY_PAGE });

export const ba = $state({
  info: null as BitAssetsInfo | null,
  assets: [] as Asset[],
  pools: [] as Pool[],
  /** The auctions not collected yet, newest first. */
  auctions: [] as Auction[],
  /** Of the wallet loaded: what it holds (null until known). */
  mine: null as Holdings | null,
  activity: [] as Activity[],
  /** Whether the holdings of the wallet could be read: a locked wallet needs its passphrase to tell its reservations. */
  mineState: 'none' as MineState,
  mineError: '',
  /** The wallet the holdings are of. */
  wallet: null as string | null,
  /** The height of the next block (getbitassetsinfo). */
  height: -1,
  /** Why the node could not be asked. */
  error: '',
  loaded: false,
  journal: emptyJournal(),
  /** Counts the refreshes. */
  version: 0,
});

/** What another page asks the Send page to send (an asset, by id). */
export const sendIntent = $state({ asset: '' });
/** What another page asks the Trade page to show: a pair to swap (pay, get), a pool to add to. */
export const tradeIntent = $state({ pay: '', get: '', a: '', b: '' });
/** What another page asks the Auctions page to sell. */
export const sellIntent = $state({ asset: '' });

/** Open the Send page with an asset chosen. */
export function sendAsset(id: string): void {
  sendIntent.asset = id;
}

let running = false;
let again = false;
let journalLoaded = false;
let saveTimer: ReturnType<typeof setTimeout> | null = null;
let journalWarned = false;

/** Read the journal from the file of the host (once). */
export async function loadJournal(): Promise<void> {
  if (journalLoaded) return;
  journalLoaded = true;
  try {
    ba.journal = sanitizeJournal(await request<unknown>('host', 'bitassets.store.load', {}));
  } catch {
    ba.journal = emptyJournal();
  }
}

/** A journal read back, with what is not of the expected shape left out. */
export function sanitizeJournal(raw: unknown): Journal {
  const j = emptyJournal();
  if (!raw || typeof raw !== 'object') return j;
  const r = raw as Partial<Journal>;
  const str = (v: unknown): v is string => typeof v === 'string';
  if (Array.isArray(r.pending)) {
    j.pending = r.pending
      .filter((p) => p && str(p.wallet) && str(p.name) && str(p.supply) && typeof p.decimals === 'number' && str(p.asset))
      .map((p) => ({ ...p, data: p.data && typeof p.data === 'object' ? p.data : {}, public: p.public !== false, reservation: str(p.reservation) ? p.reservation : '', time: typeof p.time === 'number' ? p.time : 0 }))
      .slice(-MAX_PENDING);
  }
  if (r.prices && typeof r.prices === 'object') {
    for (const [k, v] of Object.entries(r.prices)) {
      if (v && str(v.a) && Array.isArray(v.points)) j.prices[k] = { a: v.a, points: v.points.filter((p) => p && typeof p.h === 'number' && typeof p.p === 'number').slice(-MAX_POINTS) };
    }
  }
  return j;
}

/** Write the journal; false (and the user told, once) when it cannot be. */
export async function writeJournal(): Promise<boolean> {
  try {
    await request('host', 'bitassets.store.save', { value: $state.snapshot(ba.journal) });
    journalWarned = false;
    return true;
  } catch (e) {
    if (!journalWarned) {
      journalWarned = true;
      toast('warning', t('The journal of BitAssets could not be saved'), t('Assets being created may have to be registered again by hand after a restart: %1', errorText(e)), 15000);
    }
    return false;
  }
}

/** Write the journal back, a moment after the last change. */
export function saveJournal(): void {
  if (saveTimer) clearTimeout(saveTimer);
  saveTimer = setTimeout(() => {
    saveTimer = null;
    void writeJournal();
  }, 400);
}

/** Note the prices of the pools when they moved (the charts): the first asset of each pool in the second. */
export function notePrices(pools: readonly Pool[], height: number, now: number): boolean {
  let changed = false;
  for (const p of pools) {
    if (p.abandoned || p.price === undefined || !(p.price > 0)) continue;
    const series = ba.journal.prices[p.pool];
    // The price of the series' first asset: listpools may give the pair the other way round.
    const price = !series || series.a === p.asset_a_id ? p.price : 1 / p.price;
    const list = series?.points ?? [];
    const last = list[list.length - 1];
    if (last && last.h >= height) continue;
    if (last && Math.abs(last.p - price) <= Math.abs(price) * 1e-9) continue;
    ba.journal.prices[p.pool] = { a: series?.a ?? p.asset_a_id, points: [...list, { h: height, t: now, p: price }].slice(-MAX_POINTS) };
    changed = true;
  }
  const ids = Object.keys(ba.journal.prices);
  if (ids.length > MAX_PRICED_POOLS) {
    const live = new Set(pools.map((p) => p.pool));
    const keep = ids
      .sort((a, b) => Number(live.has(b)) - Number(live.has(a)) || (ba.journal.prices[b].points.at(-1)?.h ?? 0) - (ba.journal.prices[a].points.at(-1)?.h ?? 0))
      .slice(0, MAX_PRICED_POOLS);
    for (const id of ids) if (!keep.includes(id)) delete ba.journal.prices[id];
    changed = true;
  }
  return changed;
}

/** The prices of a pool as this app saw them, of `first` in the other asset, oldest first. */
export function priceSeries(pool: Pick<Pool, 'pool'>, first: string): PricePoint[] {
  const s = ba.journal.prices[pool.pool];
  if (!s) return [];
  return s.a === first ? s.points : s.points.map((p) => ({ ...p, p: p.p > 0 ? 1 / p.p : 0 }));
}

/** The assets being created by the wallet loaded. */
export function pendingOf(wallet: string | null): PendingIssue[] {
  return wallet === null ? [] : ba.journal.pending.filter((p) => p.wallet === wallet);
}

/** An asset reserved by this app: to be registered by itself once the reservation is deep enough. Written at once. */
export async function notePending(p: PendingIssue): Promise<boolean> {
  ba.journal.pending = [...ba.journal.pending.filter((x) => !(x.wallet === p.wallet && x.name === p.name)), p].slice(-MAX_PENDING);
  return writeJournal();
}

/** Forget an asset being created (released, or registered and listed). */
export function dropPending(wallet: string, name: string): void {
  const before = ba.journal.pending.length;
  ba.journal.pending = ba.journal.pending.filter((x) => !(x.wallet === wallet && x.name === name));
  if (ba.journal.pending.length !== before) saveJournal();
}

/** The parameters of registerasset for an asset being created. */
export function registerParams(p: PendingIssue): unknown[] {
  return [p.name, p.supply, p.decimals, p.data, p.public];
}

/**
 * Register what can be: the assets being created whose reservation is deep enough (wait 0), as chains-qt did
 * every few seconds. A wallet that needs its passphrase is left for the user ("Register now").
 */
async function registerReady(wallet: string): Promise<void> {
  const mine = ba.mine;
  if (!mine || ba.wallet !== wallet) return;
  let changed = false;
  for (const p of pendingOf(wallet)) {
    const registered = ba.assets.some((a) => a.asset === p.asset);
    if (registered) {
      // In a block: in the assets of the wallet from now on.
      ba.journal.pending = ba.journal.pending.filter((x) => x !== p);
      changed = true;
      continue;
    }
    if (p.registration) continue;
    const r = mine.reservations.find((x) => x.name === p.name);
    if (!r || r.taken || (r.wait ?? 0) > 0) continue;
    try {
      const done = await rpc.raw<{ txid: string }>('registerasset', registerParams(p), { wallet });
      p.registration = done.txid;
      p.error = undefined;
      changed = true;
      toast('success', t('Registering an asset'), t('%1 is being registered: it is in your assets after the next block.', p.name));
    } catch (e) {
      const err = BridgeError.from(e);
      // A locked wallet: the user registers it ("Register now", with the passphrase).
      const message = err.code === WalletError.UNLOCK_NEEDED ? '' : errorText(e);
      if (p.error !== message) {
        p.error = message;
        changed = true;
      }
    }
  }
  if (changed) saveJournal();
}

/** Register an asset being created now, with the passphrase if the wallet needs it. */
export async function registerNow(p: PendingIssue): Promise<string | null> {
  const done = await walletRpc<{ txid: string }>(p.wallet, 'registerasset', registerParams(p));
  if (!done) return null;
  p.registration = done.txid;
  p.error = undefined;
  saveJournal();
  void refreshBitAssets();
  return done.txid;
}

/** Read what a locked wallet holds, with its passphrase for this call alone. */
export async function unlockHoldings(): Promise<boolean> {
  const wallet = wallets.current;
  if (wallet === null) return false;
  const answers = await withKeys(wallet, [
    { method: 'listmyassets', params: [] },
    { method: 'listassetactivity', params: [200] },
  ]);
  if (!answers) return false;
  const [mine, activity] = answers;
  if (mine.error) {
    ba.mineState = 'error';
    ba.mineError = mine.error.message;
    return false;
  }
  if (wallets.current !== wallet) return false;
  ba.mine = mine.result as Holdings;
  ba.activity = (activity.result as Activity[] | undefined) ?? ba.activity;
  ba.mineState = 'ok';
  ba.wallet = wallet;
  await registerReady(wallet);
  return true;
}

/** Ask the node and the wallet again. */
export async function refreshBitAssets(): Promise<void> {
  if (running) {
    again = true;
    return;
  }
  running = true;
  try {
    do {
      again = false;
      await loadJournal();
      const wallet = wallets.current;
      // Every call goes out at once: the node's and the wallet's.
      const infoCall = rpc.raw<BitAssetsInfo>('getbitassetsinfo');
      const nodeCalls = Promise.all([
        infoCall,
        readAllAssets(infoCall.then((i) => i?.assets ?? 0)),
        rpc.raw<Pool[]>('listpools'),
        rpc.raw<Auction[]>('listauctions', [false]),
      ]);
      const walletCalls = wallet === null ? null : Promise.all([rpc.raw<Holdings>('listmyassets', [], { wallet }), rpc.raw<Activity[]>('listassetactivity', [baView.activity], { wallet })]);
      // Neither fails unheard of: each is awaited below.
      walletCalls?.catch(() => undefined);
      try {
        const [info, assets, pools, auctions] = await nodeCalls;
        ba.info = info;
        ba.assets = assets ?? [];
        ba.pools = pools ?? [];
        ba.auctions = auctions ?? [];
        ba.height = info?.height ?? -1;
        ba.error = '';
        if (notePrices(ba.pools, ba.height, Math.floor(Date.now() / 1000))) saveJournal();
      } catch (e) {
        ba.error = errorText(e);
      }
      if (wallet === null || walletCalls === null) {
        ba.mine = null;
        ba.activity = [];
        ba.mineState = 'none';
        ba.wallet = null;
      } else {
        if (ba.wallet !== wallet) {
          ba.mine = null;
          ba.activity = [];
          ba.mineState = 'loading';
        }
        try {
          const [mine, activity] = await walletCalls;
          if (wallets.current === wallet) {
            ba.mine = mine;
            ba.activity = activity ?? [];
            ba.mineState = 'ok';
            ba.mineError = '';
            ba.wallet = wallet;
            await registerReady(wallet);
          }
        } catch (e) {
          if (wallets.current === wallet) {
            const locked = BridgeError.from(e).code === WalletError.UNLOCK_NEEDED;
            // A locked wallet keeps what it showed when it was unlocked last (it may be out of date).
            if (ba.wallet !== wallet) ba.mine = null;
            ba.mineState = locked ? 'locked' : 'error';
            ba.mineError = locked ? '' : errorText(e);
            ba.wallet = wallet;
          }
        }
      }
      ba.loaded = true;
      ba.version++;
    } while (again);
  } finally {
    running = false;
  }
}

let timer: (() => void) | null = null;
let lastTip = '';
let lastWallet: string | null | undefined;

/** Keep the state up to date while a page of BitAssets (or its cards) is shown: at once, then with every block and every 5 seconds. */
export function startBitAssets(): void {
  const tick = () => {
    const tip = node.tip?.hash ?? '';
    if (tip !== lastTip || wallets.current !== lastWallet) {
      lastTip = tip;
      lastWallet = wallets.current;
      void refreshBitAssets();
    }
  };
  tick();
  if (timer) return;
  let ticks = 0;
  // Resting while the window is hidden (lib/poll.ts). What waits for the mempool changes without a block.
  timer = every(2500, () => {
    tick();
    if (++ticks % 2 === 0) void refreshBitAssets();
  });
}

/** For the tests. */
export function resetBitAssets(): void {
  baView.activity = ACTIVITY_PAGE;
  timer?.();
  timer = null;
  lastTip = '';
  lastWallet = undefined;
  journalLoaded = false;
  journalWarned = false;
  running = false;
  again = false;
  if (saveTimer) clearTimeout(saveTimer);
  saveTimer = null;
  Object.assign(ba, { info: null, assets: [], pools: [], auctions: [], mine: null, activity: [], mineState: 'none', mineError: '', wallet: null, height: -1, error: '', loaded: false, journal: emptyJournal(), version: 0 });
  sendIntent.asset = '';
  Object.assign(tradeIntent, { pay: '', get: '', a: '', b: '' });
  sellIntent.asset = '';
}

// --- Lookups ---

/** The decimals of an asset, by id (CHN: 8; an asset not listed: 0). */
export function decimalsOf(id: string): number {
  if (/^0+$/.test(id)) return 8;
  return ba.assets.find((a) => a.asset === id)?.decimals ?? ba.mine?.assets.find((a) => a.asset === id)?.decimals ?? 0;
}

/** The label of an asset, by id. Text of the chain. */
export function labelOf(id: string): string {
  if (/^0+$/.test(id)) return 'CHN';
  return ba.assets.find((a) => a.asset === id)?.label ?? ba.mine?.assets.find((a) => a.asset === id)?.label ?? `0x${id.slice(0, 12)}…`;
}

/**
 * How a confirmation names an asset: its label and its number ("G0LD (#1739-0929)"), or the start of its hash
 * when it has none listed (a private name): labels are any printable ASCII, so look-alikes (G0LD and GOLD,
 * USDl and USDI) are told apart by the number, as the pickers show it (audit port-2 #5). Text of the chain.
 */
export function nameOf(id: string, label?: string): string {
  if (/^0+$/.test(id)) return 'CHN';
  const listed = ba.assets.find((a) => a.asset === id);
  const text = label ?? listed?.label ?? ba.mine?.assets.find((a) => a.asset === id)?.label ?? '';
  const tag = listed?.seq ? `#${listed.seq}` : `0x${id.slice(0, 8)}…${id.slice(-4)}`;
  return text ? `${text} (${tag})` : tag;
}

/** The pool of two assets, either way round. */
export function poolOf(a: string, b: string): Pool | undefined {
  return ba.pools.find((p) => (p.asset_a_id === a && p.asset_b_id === b) || (p.asset_a_id === b && p.asset_b_id === a));
}

/** The price of one of an asset in CHN, from its pool with CHN (undefined when it has none, or nobody provides to it). */
export function chnPrice(id: string): number | undefined {
  if (/^0+$/.test(id)) return 1;
  const p = poolOf(id, '0'.repeat(64));
  if (!p || p.abandoned || !p.price) return undefined;
  return p.asset_a_id === id ? p.price : 1 / p.price;
}

/** What the wallet can spend of an asset, in its units (CHN: undefined, the kit knows it). */
export function heldOf(id: string): number | undefined {
  return ba.mine?.assets.find((a) => a.asset === id)?.balance;
}
