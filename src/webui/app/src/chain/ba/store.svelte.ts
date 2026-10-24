// The state of BitAssets, kept up to date while the app runs: what the node says (getbitassetsinfo, the
// assets, the pools, the auctions) and what the wallet holds and did (listmyassets, listassetactivity), asked
// again with every block of this chain and every few seconds, one refresh at a time.
//
// And a journal of what the chain does not keep, in a file of the data directory of the network (host methods
// bitassets.store.*, src/webui/local.cpp): the assets being created (the name, supply, decimals and data to
// register once the reservation is deep enough, as chains-qt kept them in QSettings; registered by themselves,
// as chains-qt did), the pools it is making and funding over the full range (Pools' New pool, Launch's "straight into
// a pool": PendingFund), and the prices of the pools as the app saw them (the charts).
import { BridgeError, request, setExactMethods } from '../../lib/bridge';
import { errorText, walletRpc, walletRpcIfAuthorized, withKeys } from '../../lib/dialogs.svelte';
import { go } from '../../lib/nav.svelte';
import { publishPending, type PendingOp } from '../../lib/pending.svelte';
import { t } from '../../lib/i18n.svelte';
import { every } from '../../lib/poll';
import { rpc } from '../../lib/rpc';
import { node, toast, wallets } from '../../lib/state.svelte';
import { WalletError } from '../../lib/wallet';
import { loadHiding } from './hide.svelte';
import { swapFeeParts, type SwapFeeParts } from './fees';
import type { DexInfo, PoolPreview } from './kinds';
import { flipped, turn } from './cl';
import { assetArg, assetIdentity, assetText, escapeName, shortId, EXACT_METHODS, formatPrice, setLimits, type Activity, type AssetTx, type Amount, type Asset, type AssetData, type Auction, type BitAssetsInfo, type Holding, type Holdings, type Pool } from './model';
import { TICKER } from '../config';

// Amounts past 2^53 units read exactly, wherever the methods are called (audit 7 E-BA6).
setExactMethods(EXACT_METHODS);

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
  /** Its logo (base64 of a PNG or JPEG the rules take: ba/logo.ts), registered with it; none if absent. */
  logo?: string;
  /** Its display name (from ticker_height), registered with it; none if absent. Text of the user, in NFC. */
  display_name?: string;
  /**
   * A launch (the create wizard's bonding curve): launchtoken's options, sent instead of registerasset once the
   * reservation is deep enough (the host asked both with the reservation: LaunchAfterReserve). The supply and decimals
   * above are then the launch's.
   */
  launch?: Record<string, string | number>;
  /** The market the wizard opens once it is registered (a pool, an auction): offered then, never sent by itself. */
  follow?: 'pool' | 'auction';
  /** When it was reserved (seconds), and the reservation's transaction. */
  time: number;
  reservation: string;
  /** The registration, once sent, and the height of the next block then. */
  registration?: string;
  registeredAt?: number;
  /** Why the last registration failed (an error of the node, shown as it is). */
  error?: string;
  /**
   * Deep enough, and the host would have to ask (no authorization since the app started again, or a locked wallet):
   * the user confirms it on its card. Not kept in the journal.
   */
  needsConfirm?: string;
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

/**
 * A concentrated pool this app is making at the user's price and funding over the full range, in steps the chain
 * needs apart (user decision, 2026-10-23): the asset registered (Launch's "straight into a pool"; one that exists
 * already, for Pools' New pool), then its pool (createpool, of the curve concentrated, at the price the two amounts
 * name, nothing put in), then, once the pool is in a block, a position over the full range with both amounts
 * (openposition with prices of 0: no bound). Each step a transaction the wallet confirms: the user's word, on its card
 * (ba/FundCard.svelte) or its notice, never a dialog by itself.
 */
export interface PendingFund {
  wallet: string;
  /** The asset's name or label, for the card: text of the user or of the chain. */
  name: string;
  /** The asset (its id: the hash of its name while it is being created) and what it pairs with (ECX: CHN_ID). */
  asset: string;
  quote: string;
  /** What goes in of each, as exact text the node takes (their ratio, quoteAmount / amount, is the pool's price). */
  amount: string;
  quoteAmount: string;
  /** The fee tier, in percent (getdexinfo's fee_tiers). */
  fee: number;
  slippage: number;
  time: number;
  /** The pool, once made (or found made by someone else), its transaction, and the position's. */
  pool?: string;
  poolTx?: string;
  positionTx?: string;
  /** When the position was sent (ms): the card says so for a while, then the journal forgets it. */
  doneAt?: number;
  /** Why the last step failed (an error of the node, shown as it is). */
  error?: string;
  /** The last step the user was told is ready (told once). Not kept in the journal. */
  told?: FundStage;
}

/** Where a pool being funded is: waiting for its asset, for the user's word on its pool, its pool in a block, the user's word on the position, done. */
export type FundStage = 'asset' | 'pool' | 'pool-sent' | 'position' | 'done';

export interface Journal {
  pending: PendingIssue[];
  funding: PendingFund[];
  prices: Record<string, PriceSeries>;
}

const MAX_PENDING = 100;
/** The base64 of the largest logo (32768 bytes). */
const MAX_LOGO_BASE64 = 43_692;
const MAX_POINTS = 120;
const MAX_PRICED_POOLS = 60;

export function emptyJournal(): Journal {
  return { pending: [], funding: [], prices: {} };
}

export type MineState = 'none' | 'loading' | 'ok' | 'locked' | 'error';

/**
 * The newest assets the store keeps (one listassets call), for their names on the pages: never the whole list (user
 * feedback 2026-10-19: hundreds of thousands of assets, millions later). Browse assets and the pickers ask the node
 * as the user searches (ba/search.ts: searchassets, paged by a cursor).
 */
export const ASSET_PAGE = 500;
export const MAX_ASSETS = ASSET_PAGE;
/** Entries of the asset activity read at first, and more each time "Show more" is pressed (up to MAX_ACTIVITY). */
export const ACTIVITY_PAGE = 200;
export const MAX_ACTIVITY = 5000;
/** Asset moves of the wallet read (listassettransactions), newest first. */
export const TRANSFERS = 1000;

/**
 * The newest ASSET_PAGE assets of the chain (audit port-2 #4: the newest, never only the first registered;
 * `total`: the assets the node counts, getbitassetsinfo), in one call.
 */
export async function readAllAssets(total: number | Promise<number>): Promise<Asset[]> {
  const from = Math.max(0, (await total) - ASSET_PAGE);
  return (await rpc.raw<Asset[]>('listassets', [ASSET_PAGE, from])) ?? [];
}

/** Whether two lists read are the same (then the one shown stays: nothing that depends on it is worked out again). */
function same(a: unknown, b: unknown): boolean {
  // A walk of the two JSON values, stopping at the first difference (a JSON text of each cost seconds in a burst).
  if (a === b) return true;
  if (typeof a !== 'object' || typeof b !== 'object' || a === null || b === null) return false;
  if (Array.isArray(a)) {
    if (!Array.isArray(b) || a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (!same(a[i], b[i])) return false;
    return true;
  }
  if (Array.isArray(b)) return false;
  const ka = Object.keys(a);
  if (ka.length !== Object.keys(b).length) return false;
  for (const k of ka) if (!same((a as Record<string, unknown>)[k], (b as Record<string, unknown>)[k])) return false;
  return true;
}


/**
 * Pools and auctions are read a page at a time (listpools, listauctions [count, skip]: audit 7 E-BA3, anyone can
 * grow those tables cheaply): the first POOL_PAGE / AUCTION_PAGE, more as the user asks ("Show more"), up to
 * MAX_POOLS / MAX_AUCTIONS. The node orders and counts them once per block.
 */
export const LIST_PAGE = 1000;
export const POOL_PAGE = 1000;
export const MAX_POOLS = 5000;
export const AUCTION_PAGE = 200;
export const MAX_AUCTIONS = 5000;
/** Auctions of the wallet's receipts read one by one when not in the page read (at most). */
const MAX_RECEIPT_AUCTIONS = 50;

/** What the pages ask for beyond the usual: more of the activity of the wallet, more pools and auctions. */
export const baView = $state({ activity: ACTIVITY_PAGE, pools: POOL_PAGE, auctions: AUCTION_PAGE });

/** Up to `limit` entries of a paged list, in pages of LIST_PAGE: `page(count, skip)` asks for one. */
export async function readPaged<T>(read: (count: number, skip: number) => Promise<T[] | null>, limit: number): Promise<T[]> {
  const out: T[] = [];
  for (let skip = 0; skip < limit; skip += LIST_PAGE) {
    const count = Math.min(LIST_PAGE, limit - skip);
    const page = (await read(count, skip)) ?? [];
    out.push(...page);
    if (page.length < count) break;
  }
  return out;
}

/** Show more auctions (the next AUCTION_PAGE), asked of the node at once. */
export function showMoreAuctions(): void {
  baView.auctions = Math.min(MAX_AUCTIONS, baView.auctions + AUCTION_PAGE);
  void refreshBitAssets();
}

/** Show more pools (the next POOL_PAGE). */
export function showMorePools(): void {
  baView.pools = Math.min(MAX_POOLS, baView.pools + POOL_PAGE);
  void refreshBitAssets();
}

export const ba = $state({
  info: null as BitAssetsInfo | null,
  /** Layers' DEX: its pool kinds, fee tiers and bounds (getdexinfo; null on a node without it). */
  dex: null as DexInfo | null,
  assets: [] as Asset[],
  pools: [] as Pool[],
  /** The auctions not collected yet, newest first: the first baView.auctions of them. */
  auctions: [] as Auction[],
  /** The auctions of the wallet's receipts that are not among those (read one by one). */
  receiptAuctions: [] as Auction[],
  /** Of the wallet loaded: what it holds (null until known). */
  mine: null as Holdings | null,
  activity: [] as Activity[],
  /** The asset moves of the wallet, with the other side (listassettransactions): the Transactions page and the activity. */
  transfers: [] as AssetTx[],
  /** The wallet they are of. */
  transfersWallet: null as string | null,
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
/** What another page asks the Trade page to show: a pair to swap (pay, get), a pool to add to, a pool or a launch to open (by id), a book (its base and quote). */
export const tradeIntent = $state({ pay: '', get: '', a: '', b: '', pool: '', launch: '', bookBase: '', bookQuote: '', createA: '', createB: '' });
/** An asset whose page is opened on its "Open a market" (ba/AssetActions.svelte): AssetView scrolls to it, once. */
export const openMarketIntent = $state({ asset: '' });
/** What another page asks the Auctions page to sell. */
export const sellIntent = $state({ asset: '' });

/** Open a market for an asset that exists: its pool with the coin (Create pool), or its auction (Sell by auction). */
export function openMarketFor(asset: string, kind: 'pool' | 'auction' | 'book'): void {
  if (kind === 'pool') {
    tradeIntent.createA = asset;
    tradeIntent.createB = '0'.repeat(64);
    go('trade', 'create');
  } else if (kind === 'book') {
    tradeIntent.bookBase = asset;
    tradeIntent.bookQuote = '0'.repeat(64);
    go('trade', 'book');
  } else {
    sellIntent.asset = asset;
    go('auctions', 'sell');
  }
}

/** Open the Send page with an asset chosen. */
export function sendAsset(id: string): void {
  sendIntent.asset = id;
}

let running = false;
let again = false;
let againNode = false;
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
      .map((p) => ({ ...p, display_name: str(p.display_name) && p.display_name.length > 0 && p.display_name.length <= 64 ? p.display_name : undefined, logo: str(p.logo) && p.logo.length <= MAX_LOGO_BASE64 && /^[A-Za-z0-9+/]+={0,2}$/.test(p.logo) ? p.logo : undefined, data: p.data && typeof p.data === 'object' ? p.data : {}, public: p.public !== false, reservation: str(p.reservation) ? p.reservation : '', time: typeof p.time === 'number' ? p.time : 0, registration: str(p.registration) ? p.registration : undefined, registeredAt: typeof p.registeredAt === 'number' ? p.registeredAt : undefined, launch: launchOf(p.launch), follow: p.follow === 'pool' || p.follow === 'auction' ? p.follow : undefined, needsConfirm: undefined }))
      .slice(-MAX_PENDING);
  }
  if (Array.isArray(r.funding)) {
    const day = Date.now() - 86_400_000;
    j.funding = r.funding
      .filter((f) => f && str(f.wallet) && str(f.name) && str(f.asset) && str(f.quote) && str(f.amount) && str(f.quoteAmount) && typeof f.fee === 'number' && Number.isFinite(f.fee) && !(typeof f.doneAt === 'number' && f.doneAt < day))
      .map((f) => ({ wallet: f.wallet, name: f.name, asset: f.asset, quote: f.quote, amount: f.amount, quoteAmount: f.quoteAmount, fee: f.fee, slippage: typeof f.slippage === 'number' && Number.isFinite(f.slippage) ? f.slippage : 1, time: typeof f.time === 'number' ? f.time : 0, pool: str(f.pool) ? f.pool : undefined, poolTx: str(f.poolTx) ? f.poolTx : undefined, positionTx: str(f.positionTx) ? f.positionTx : undefined, doneAt: typeof f.doneAt === 'number' ? f.doneAt : undefined, error: str(f.error) ? f.error : undefined }))
      .slice(-MAX_PENDING);
  }
  if (r.prices && typeof r.prices === 'object') {
    for (const [k, v] of Object.entries(r.prices)) {
      if (v && str(v.a) && Array.isArray(v.points)) j.prices[k] = { a: v.a, points: v.points.filter((p) => p && typeof p.h === 'number' && typeof p.p === 'number').slice(-MAX_POINTS) };
    }
  }
  return j;
}

/** A launch's options read back from the journal: text and numbers only, the logo bounded; anything else dropped. */
function launchOf(v: unknown): Record<string, string | number> | undefined {
  if (!v || typeof v !== 'object' || Array.isArray(v)) return undefined;
  const out: Record<string, string | number> = {};
  for (const [k, x] of Object.entries(v as Record<string, unknown>)) {
    if (!/^[a-z_]{1,20}$/.test(k)) continue;
    if (typeof x === 'number' && Number.isFinite(x)) out[k] = x;
    else if (typeof x === 'string' && x.length <= (k === 'logo' ? MAX_LOGO_BASE64 : 1024)) out[k] = x;
  }
  return out;
}

/** Write the journal; false (and the user told, once) when it cannot be. */
export async function writeJournal(): Promise<boolean> {
  try {
    const value = $state.snapshot(ba.journal);
    // What the host would ask about now is not kept: after a restart it is known again.
    value.pending = value.pending.map(({ needsConfirm: _, ...p }) => p);
    value.funding = value.funding.map(({ told: _, ...f }) => f);
    await request('host', 'bitassets.store.save', { value });
    journalWarned = false;
    return true;
  } catch (e) {
    if (!journalWarned) {
      journalWarned = true;
      toast('warning', t('The journal of the assets could not be saved'), t('Assets being created may have to be registered again by hand after a restart: %1', errorText(e)), 15000);
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
  publish();
  return writeJournal();
}

/** Forget an asset being created (released, or registered and listed). */
export function dropPending(wallet: string, name: string): void {
  const before = ba.journal.pending.length;
  ba.journal.pending = ba.journal.pending.filter((x) => !(x.wallet === wallet && x.name === name));
  if (ba.journal.pending.length !== before) saveJournal();
  publish();
}

/** The second step of an asset being created: registerasset, or launchtoken for a launch. */
export function secondStepOf(p: PendingIssue): { method: 'registerasset' | 'launchtoken'; params: unknown[] } {
  if (p.launch) return { method: 'launchtoken', params: [p.name, p.launch] };
  return { method: 'registerasset', params: registerParams(p) };
}

/** The parameters of registerasset for an asset being created. */
export function registerParams(p: PendingIssue): unknown[] {
  // The logo, then the display name, and only with one: the parameters a registration without them always had.
  if (p.display_name) return [p.name, p.supply, p.decimals, p.data, p.public, p.logo ?? null, p.display_name];
  return p.logo ? [p.name, p.supply, p.decimals, p.data, p.public, p.logo] : [p.name, p.supply, p.decimals, p.data, p.public];
}

/** Assets registered a moment ago, for their cards and the indicator of the top bar (not kept). */
const recentDone = $state<{ wallet: string; name: string; asset: string; ended: number }[]>([]);
const KEEP_DONE_MS = 10 * 60_000;

/** The registered ones of a wallet a moment ago. */
export function recentlyRegistered(wallet: string | null): { wallet: string; name: string; asset: string; ended: number }[] {
  const now = Date.now();
  return recentDone.filter((d) => d.wallet === wallet && now - d.ended < KEEP_DONE_MS);
}

/**
 * The progress of an asset being created, for its card (components/PendingCard.svelte) and the indicator of the top
 * bar: reserved, waiting for the depth (block k of reveal_depth), the user's word needed, the registration sent,
 * registered.
 */
export function pendingOpOf(p: PendingIssue): PendingOp {
  const needed = Math.max(1, ba.info?.reveal_depth ?? 1);
  const r = ba.wallet === p.wallet ? ba.mine?.reservations.find((x) => x.name === p.name) : undefined;
  const base = { key: `${p.wallet}\u0000${p.name}`, what: t('Asset'), name: p.name, wallet: p.wallet, needed, page: 'my-assets' as const, tab: 'issuing' };
  if (ba.assets.some((a) => a.asset === p.asset)) return { ...base, stage: 'done', depth: needed, ended: Date.now() };
  if (r?.taken) return { ...base, stage: 'failed', depth: null, error: t('Someone registered this name first. The reservation can only be released.') };
  if (p.registration) return { ...base, stage: 'registering', depth: needed };
  if (!r) return { ...base, stage: 'reserved', depth: null, error: p.error };
  const depth = Math.max(0, needed - (r.wait ?? 0));
  if ((r.wait ?? 0) > 0) return { ...base, stage: 'waiting', depth };
  if (p.needsConfirm !== undefined || p.error) {
    return { ...base, stage: 'confirm', depth, error: p.error, action: { label: p.error ? 'Try again' : 'Confirm registration', run: () => void registerByUser(p) } };
  }
  return { ...base, stage: 'waiting', depth };
}

/** What this store is doing, for the indicator of the top bar (lib/pending.svelte.ts). */
function publish(): void {
  const wallet = ba.wallet;
  const ops = wallet === null ? [] : pendingOf(wallet).map(pendingOpOf);
  for (const d of recentlyRegistered(wallet)) {
    ops.push({ key: `${d.wallet}\u0000${d.name}`, what: t('Asset'), name: d.name, wallet: d.wallet, stage: 'done', depth: null, needed: 0, ended: d.ended, page: 'my-assets', tab: 'holdings' });
  }
  publishPending('bitassets', ops);
}

/** The user registers it from its card or a toast: asked as the host asks (no question when it was accepted with its reservation). */
async function registerByUser(p: PendingIssue): Promise<void> {
  try {
    await registerNow(p);
  } catch (e) {
    p.error = errorText(e);
    saveJournal();
    publish();
  }
}

let registering = false;

/**
 * Register what can be: the assets being created whose reservation is deep enough (wait 0), as chains-qt did
 * every few seconds; never in a refresh (the lists go on while it runs), never with a question of the host: the
 * registration the user accepted with its reservation runs without one; else its card asks the user (a restart, a
 * locked wallet).
 */
async function registerReady(wallet: string): Promise<void> {
  if (registering) return;
  registering = true;
  try {
    await registerReadyOnce(wallet);
  } finally {
    registering = false;
    publish();
  }
}

async function registerReadyOnce(wallet: string): Promise<void> {
  const mine = ba.mine;
  if (!mine || ba.wallet !== wallet) return;
  let changed = false;
  for (const p of pendingOf(wallet)) {
    const registered = ba.assets.some((a) => a.asset === p.asset);
    if (registered) {
      // In a block: in the assets of the wallet from now on.
      ba.journal.pending = ba.journal.pending.filter((x) => x !== p);
      recentDone.push({ wallet, name: p.name, asset: p.asset, ended: Date.now() });
      if (ba.journal.funding.some((f) => f.wallet === wallet && f.asset === p.asset && !f.pool)) {
        // Straight into a pool: its own notice, once the pool can be made (fundReady).
      } else if (p.launch) {
        toast('success', t('Asset launched'), t('%1 is on its bonding curve.', assetText(p.name, p.display_name, assetById(p.asset)?.seq, p.asset)), 8000, [{ label: t('Open'), run: () => { tradeIntent.launch = p.asset; go('pools', 'launches'); } }]);
      } else if (p.follow) {
        const follow = p.follow;
        toast('success', t('Asset registered'), follow === 'pool' ? t('%1 is in your assets: now seed its pool.', assetText(p.name, p.display_name, assetById(p.asset)?.seq, p.asset)) : t('%1 is in your assets: now start its auction.', assetText(p.name, p.display_name, assetById(p.asset)?.seq, p.asset)), 15000, [{ label: follow === 'pool' ? t('Create its pool') : t('Start its auction'), run: () => openMarketFor(p.asset, follow) }]);
      } else {
        toast('success', t('Asset registered'), t('%1 is in your assets, with its supply and its control coin.', assetText(p.name, p.display_name, assetById(p.asset)?.seq, p.asset)), 8000, [{ label: t('Open'), run: () => go('my-assets', 'holdings') }]);
      }
      changed = true;
      continue;
    }
    if (p.registration) {
      // Sent, and in no block two blocks on: still waiting in the mempool, or dropped from it (stale, expired,
      // evicted), when it is sent again: the wallet frees the reservation the dropped one held (audit 7 E-BA7).
      if (p.registeredAt === undefined) p.registeredAt = ba.height;
      if (ba.height < p.registeredAt + 2) continue;
      try {
        await rpc.raw('getmempoolentry', [p.registration]);
        continue;
      } catch {
        // Not in the mempool: sent again below.
      }
    } else {
      const r = mine.reservations.find((x) => x.name === p.name);
      if (!r || r.taken || (r.wait ?? 0) > 0) continue;
    }
    if (p.needsConfirm !== undefined && !p.registration) continue;
    try {
      const second = secondStepOf(p);
      const done = await walletRpcIfAuthorized<{ txid: string }>(wallet, second.method, second.params);
      if ('needed' in done) {
        // The host would ask: the user confirms it on its card, told once.
        p.needsConfirm = done.needed;
        changed = true;
        toast('info', t('%1 is ready to register', assetText(p.name, p.display_name, assetById(p.asset)?.seq, p.asset)), t('Confirm its registration on its card in Portfolio.'), 12000, [{ label: t('Confirm'), run: () => void registerByUser(p) }]);
        continue;
      }
      p.registration = done.result.txid;
      p.registeredAt = ba.height;
      p.error = undefined;
      changed = true;
      toast('success', t('Registering an asset'), t('%1 is being registered: it is in your assets after the next block.', assetText(p.name, p.display_name, assetById(p.asset)?.seq, p.asset)));
    } catch (e) {
      const err = BridgeError.from(e);
      // A locked wallet: the user registers it ("Register now", with the passphrase).
      const message = err.code === WalletError.UNLOCK_NEEDED ? '' : errorText(e);
      if (p.error !== message) {
        p.error = message;
        changed = true;
        if (message) toast('error', t('%1 was not registered', assetText(p.name, p.display_name, assetById(p.asset)?.seq, p.asset)), message, 12000, [{ label: t('Try again'), run: () => void registerByUser(p) }]);
      }
    }
  }
  if (changed) saveJournal();
}

/** Register an asset being created now, with the passphrase if the wallet needs it. */
export async function registerNow(p: PendingIssue): Promise<string | null> {
  const second = secondStepOf(p);
  const done = await walletRpc<{ txid: string }>(p.wallet, second.method, second.params);
  if (!done) return null;
  p.registration = done.txid;
  p.registeredAt = ba.height;
  p.error = undefined;
  p.needsConfirm = undefined;
  saveJournal();
  publish();
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

/**
 * Ask the node and the wallet again. With `node` false, only the wallet (what the mempool changes without a
 * block): the lists of the chain change with blocks alone, and are asked again with each (audit 7 E-BA3).
 */
export async function refreshBitAssets(options: { node?: boolean } = {}): Promise<void> {
  const withNode = options.node ?? true;
  if (running) {
    again = true;
    againNode ||= withNode;
    return;
  }
  running = true;
  let askNode = withNode || !ba.loaded;
  try {
    do {
      again = false;
      await loadJournal();
      const wallet = wallets.current;
      // Every call goes out at once: the node's and the wallet's.
      const infoCall = rpc.raw<BitAssetsInfo>('getbitassetsinfo');
      // Layers' DEX (its pool kinds): null from a node without getdexinfo.
      const dexCall = askNode ? rpc.raw<DexInfo>('getdexinfo', []).catch(() => null) : null;
      const nodeCalls = askNode
        ? Promise.all([
            infoCall,
            readAllAssets(infoCall.then((i) => i?.assets ?? 0)),
            readPaged<Pool>((count, skip) => rpc.raw<Pool[]>('listpools', [null, count, skip]), baView.pools),
            readPaged<Auction>((count, skip) => rpc.raw<Auction[]>('listauctions', [false, count, skip]), baView.auctions),
          ])
        : null;
      nodeCalls?.catch(() => undefined);
      const walletCalls =
        wallet === null
          ? null
          : Promise.all([
              rpc.raw<Holdings>('listmyassets', [], { wallet }),
              rpc.raw<Activity[]>('listassetactivity', [baView.activity], { wallet }),
            ]);
      // Needs no key: read for a locked wallet too (one call for every row of the Transactions page).
      const transfersCall = wallet === null ? null : rpc.raw<AssetTx[]>('listassettransactions', [TRANSFERS, 0], { wallet }).catch(() => null);
      // Neither fails unheard of: each is awaited below.
      walletCalls?.catch(() => undefined);
      try {
        if (nodeCalls) {
          const [info, assets, pools, auctions] = await nodeCalls;
          ba.info = info;
          setLimits(info);
          // A list unchanged is kept (the pages work out nothing again: a burst of blocks redrew them for nothing).
          if (!same(ba.assets, assets ?? [])) ba.assets = assets ?? [];
          if (!same(ba.pools, pools ?? [])) ba.pools = pools ?? [];
          if (!same(ba.auctions, auctions ?? [])) ba.auctions = auctions ?? [];
          ba.height = info?.height ?? -1;
          const dex = await dexCall;
          if (!same(ba.dex, dex ?? null)) ba.dex = dex && typeof dex === 'object' ? dex : null;
          if (notePrices(ba.pools, ba.height, Math.floor(Date.now() / 1000))) saveJournal();
        } else {
          const info = await infoCall;
          ba.info = info;
          setLimits(info);
          ba.height = info?.height ?? -1;
        }
        ba.error = '';
      } catch (e) {
        ba.error = errorText(e);
      }
      if (transfersCall) {
        const transfers = await transfersCall;
        if (wallets.current === wallet && transfers) {
          if (ba.transfersWallet !== wallet || !same(ba.transfers, transfers)) ba.transfers = transfers;
          ba.transfersWallet = wallet;
        }
      }
      if (wallet === null || walletCalls === null) {
        ba.mine = null;
        ba.activity = [];
        ba.transfers = [];
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
            if (ba.wallet !== wallet || !same(ba.mine, mine)) ba.mine = mine;
            if (ba.wallet !== wallet || !same(ba.activity, activity ?? [])) ba.activity = activity ?? [];
            ba.mineState = 'ok';
            ba.mineError = '';
            ba.wallet = wallet;
            await readReceiptAuctions();
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
      askNode = againNode;
      againNode = false;
      publish();
    } while (again);
  } finally {
    running = false;
  }
  // The registrations ready, after the refresh: it never waits for them; then the pools being funded.
  if (ba.wallet !== null && ba.wallet === wallets.current && ba.mineState === 'ok') {
    const wallet = ba.wallet;
    void registerReady(wallet).then(() => fundReady(wallet));
  }
}

/** The auctions of the wallet's receipts not in the page of auctions read (audit 7 E-BA3: the list is paged). */
async function readReceiptAuctions(): Promise<void> {
  const listed = new Set(ba.auctions.map((a) => a.auction));
  const missing = (ba.mine?.receipts ?? []).map((r) => r.auction).filter((id) => !listed.has(id)).slice(0, MAX_RECEIPT_AUCTIONS);
  const known = new Map(ba.receiptAuctions.map((a) => [a.auction, a]));
  const out: Auction[] = [];
  for (const id of missing) {
    try {
      out.push(await rpc.raw<Auction>('getauction', [id]));
    } catch {
      const before = known.get(id);
      if (before) out.push(before);
    }
  }
  ba.receiptAuctions = out;
}

/** An auction by id: in the page read, or among those of the wallet's receipts. */
export function auctionById(id: string): Auction | undefined {
  return ba.auctions.find((a) => a.auction === id) ?? ba.receiptAuctions.find((a) => a.auction === id);
}

/** Whether the node has more auctions than the page read (not collected), or more pools. */
export function moreAuctions(): boolean {
  return (ba.info?.auctions ?? 0) > ba.auctions.length && baView.auctions < MAX_AUCTIONS;
}
export function morePools(): boolean {
  return (ba.info?.pools ?? 0) > ba.pools.length && baView.pools < MAX_POOLS;
}

let timer: (() => void) | null = null;
let lastTip = '';
let lastWallet: string | null | undefined;

/**
 * Keep the state up to date while a page of BitAssets (or its cards) is shown: at once, then with every block;
 * the wallet's part also every 5 seconds (the mempool), the chain's lists only with blocks.
 */
export function startBitAssets(): void {
  // What the wallets hide of the assets (their asset filters), read once.
  void loadHiding();
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
    if (++ticks % 2 === 0) void refreshBitAssets({ node: false });
  });
}

/** For the tests. */
export function resetBitAssets(): void {
  baView.activity = ACTIVITY_PAGE;
  baView.pools = POOL_PAGE;
  baView.auctions = AUCTION_PAGE;
  againNode = false;
  timer?.();
  timer = null;
  lastTip = '';
  lastWallet = undefined;
  journalLoaded = false;
  journalWarned = false;
  running = false;
  again = false;
  registering = false;
  funding = false;
  livePools.clear();
  recentDone.splice(0);
  if (saveTimer) clearTimeout(saveTimer);
  saveTimer = null;
  Object.assign(ba, { info: null, dex: null, assets: [], pools: [], auctions: [], receiptAuctions: [], mine: null, activity: [], transfers: [], transfersWallet: null, mineState: 'none', mineError: '', wallet: null, height: -1, error: '', loaded: false, journal: emptyJournal(), version: 0 });
  sendIntent.asset = '';
  Object.assign(tradeIntent, { pay: '', get: '', a: '', b: '', pool: '', launch: '' });
  sellIntent.asset = '';
}

// --- Pools being made and funded over the full range (PendingFund) ---

/** The pools being funded known to be in a block (getpool by id answered them). */
const livePools = new Set<string>();
let funding = false;

/** The pools this wallet is funding, the newest last (those done a while ago left out). */
export function fundingOf(wallet: string | null): PendingFund[] {
  const now = Date.now();
  return wallet === null ? [] : ba.journal.funding.filter((f) => f.wallet === wallet && (!f.doneAt || now - f.doneAt < KEEP_DONE_MS));
}

/** The kind of the pool of a PendingFund, as createpool and previewpool take it. */
export function fundKind(f: Pick<PendingFund, 'fee'>): { curve: 'concentrated'; fee: number } {
  return { curve: 'concentrated', fee: f.fee };
}

/** createpool's (and previewpool's) parameters for a pool being funded: at the price its amounts name, nothing put in. */
export function fundPoolParams(f: PendingFund): unknown[] {
  return [assetArg(f.asset), f.amount, assetArg(f.quote), f.quoteAmount, fundKind(f)];
}

/** The most a pool being funded may have moved from the creator's price before the position goes in, in percent. */
export const FUND_PRICE_TOLERANCE = 1;

/** The creator's price of a pool being funded, as a pool of the lists gives it (1 of asset_a in asset_b). */
export function fundPriceOf(f: Pick<PendingFund, 'asset' | 'amount' | 'quoteAmount'>, pool: Pick<Pool, 'asset_a_id'>): number {
  const own = Number(f.quoteAmount) / Number(f.amount);
  return pool.asset_a_id === f.asset ? own : 1 / own;
}

/**
 * openposition's parameters for a pool being funded: the full range (prices of 0: no bound), both amounts, and the
 * creator's price as the price the wallet expects (expected_price: the node's order, asset0 in asset1), so that it
 * refuses at signing a pool moved further than the slippage from it (security review 2026-10-23: a pool opened or moved
 * by someone else first).
 */
export function fundPositionParams(f: PendingFund, pool: Pick<Pool, 'asset_a_id' | 'asset_b_id'>): unknown[] {
  const expected = Number(turn(fundPriceOf(f, pool), flipped(pool)).toPrecision(12));
  return [f.pool ?? '', 0, 0, { [assetArg(f.asset)]: f.amount, [assetArg(f.quote)]: f.quoteAmount }, f.slippage, expected];
}

/** Where a pool being funded is. */
export function fundStageOf(f: PendingFund): FundStage {
  if (f.positionTx) return 'done';
  if (!f.pool) {
    const known = ba.assets.some((a) => a.asset === f.asset) || (ba.mine?.assets ?? []).some((h) => h.asset === f.asset) || !!assetById(f.asset);
    return known ? 'pool' : 'asset';
  }
  return livePools.has(f.pool) || ba.pools.some((p) => p.pool === f.pool) ? 'position' : 'pool-sent';
}

/** A pool to make and fund, journaled at once (the user's word on each step comes after). */
export async function noteFunding(f: PendingFund): Promise<boolean> {
  ba.journal.funding = [...ba.journal.funding.filter((x) => !(x.wallet === f.wallet && x.asset === f.asset && x.quote === f.quote && !x.positionTx)), f].slice(-MAX_PENDING);
  return writeJournal();
}

/** Forget a pool being funded (the user gave up on it, or it is done). */
export function dropFunding(f: PendingFund): void {
  ba.journal.funding = ba.journal.funding.filter((x) => x !== f && !(x.wallet === f.wallet && x.asset === f.asset && x.quote === f.quote && x.time === f.time));
  saveJournal();
}

/** The journaled entry itself (the pages hold snapshots of it at times). */
function fundEntry(f: PendingFund): PendingFund | undefined {
  return ba.journal.funding.find((x) => x === f || (x.wallet === f.wallet && x.asset === f.asset && x.quote === f.quote && x.time === f.time));
}

/**
 * Make the pool of a PendingFund (the user's word: the wallet confirms createpool): at the price its amounts name, of
 * the curve concentrated, nothing put in. Only the pool this step makes is funded: one of the pair at that tier made by
 * someone else first is never used (its price is theirs: security review 2026-10-23); the user is told, and stops.
 */
export async function makeFundedPool(given: PendingFund): Promise<boolean> {
  const f = fundEntry(given) ?? given;
  const params = fundPoolParams(f);
  try {
    const preview = await rpc.raw<PoolPreview>('previewpool', params).catch(() => null);
    if (preview?.exists) {
      f.error = t('Someone opened a concentrated pool of %1 at this fee first, at a price of their own: this app does not put your liquidity in it. Look at its price on its page and open a position there if you want, or forget this one.', f.name);
      saveJournal();
      return false;
    }
    const done = await walletRpc<{ txid: string; pool: string }>(f.wallet, 'createpool', [...params, f.slippage]);
    if (!done) return false;
    f.pool = done.pool;
    f.poolTx = done.txid;
    f.error = undefined;
    saveJournal();
    toast('success', t('Making its pool'), t('The pool of %1 is made with the next block; then put your liquidity in it.', assetTextOf(f.asset, f.name)));
    void refreshBitAssets();
    return true;
  } catch (e) {
    f.error = errorText(e);
    saveJournal();
    return false;
  }
}

/** Put the liquidity of a PendingFund in its pool, over the full range (the user's word: the wallet confirms openposition). */
export async function fundPosition(given: PendingFund): Promise<boolean> {
  const f = fundEntry(given) ?? given;
  if (!f.pool || !f.poolTx) return false;
  try {
    // The pool as it is now: refused if its price moved from the creator's by more than the tolerance (someone traded
    // it, or it is not the pool that was made).
    const pool = await rpc.raw<Pool>('getpool', [f.pool]);
    const want = fundPriceOf(f, pool);
    const now = Number(pool.price);
    if (!(want > 0) || !(now > 0) || Math.abs(now / want - 1) * 100 > FUND_PRICE_TOLERANCE) {
      f.error = t('The pool\'s price is %1, not %2, the price you set: it moved before your liquidity went in. Nothing was sent; look at the pool before putting liquidity in it.', formatPrice(now), formatPrice(want));
      saveJournal();
      return false;
    }
    const done = await walletRpc<{ txid: string; position: string }>(f.wallet, 'openposition', fundPositionParams(f, pool));
    if (!done) return false;
    f.positionTx = done.txid;
    f.doneAt = Date.now();
    f.error = undefined;
    saveJournal();
    toast('success', t('Funding its pool'), t('Your liquidity is in the pool of %1 with the next block, over the full range: see it in Positions.', assetTextOf(f.asset, f.name)));
    void refreshBitAssets();
    return true;
  } catch (e) {
    f.error = errorText(e);
    saveJournal();
    return false;
  }
}

/** The pools being funded: which are in a block now; the user told once when a step needs their word. */
async function fundReady(wallet: string): Promise<void> {
  if (funding) return;
  funding = true;
  try {
    for (const f of ba.journal.funding.filter((x) => x.wallet === wallet && !x.positionTx)) {
      if (f.pool && !livePools.has(f.pool)) {
        if (ba.pools.some((p) => p.pool === f.pool)) livePools.add(f.pool);
        else {
          try {
            const p = await rpc.raw<Pool>('getpool', [f.pool]);
            if (p && typeof p === 'object') livePools.add(f.pool);
          } catch {
            // Not in a block yet.
          }
        }
      }
      const stage = fundStageOf(f);
      if (stage === f.told) continue;
      f.told = stage;
      if (stage === 'pool') toast('info', t('%1 is registered', assetTextOf(f.asset, f.name)), t('Now make its pool, at your price: your wallet confirms it.'), 15000, [{ label: t('Make its pool'), run: () => void makeFundedPool(f) }]);
      else if (stage === 'position') toast('info', t('The pool of %1 is open', assetTextOf(f.asset, f.name)), t('Now put your liquidity in it, over the full range: your wallet confirms it.'), 15000, [{ label: t('Put it in'), run: () => void fundPosition(f) }]);
    }
  } finally {
    funding = false;
  }
}

// --- Lookups ---

// Lookups by id, from maps made once per list read (the lists are replaced whole): a page that names hundreds of
// assets, each by a find over thousands, took frames (user feedback 2026-10-19, item 10).
let assetMap = new Map<string, Asset>();
let assetMapOf: unknown = null;
let heldMap = new Map<string, Holding>();
let heldMapOf: unknown = null;
let poolMap = new Map<string, Pool>();
let poolMapOf: unknown = null;
let pooled = new Set<string>();
let poolIds = new Map<string, Pool>();
/** Assets the pages found apart from the list read (a search, a lookup): their names known everywhere. */
const known = new Map<string, Asset>();

/** An asset by id, among those read or found. */
export function assetById(id: string): Asset | undefined {
  const list = ba.assets;
  if (assetMapOf !== list) {
    assetMap = new Map(list.map((a) => [a.asset, a]));
    assetMapOf = list;
  }
  return assetMap.get(id) ?? known.get(id);
}

/** Keep assets found by a search or a lookup, for their names and decimals (at most 5,000, the latest kept). */
export function noteKnownAssets(list: readonly Asset[]): void {
  for (const a of list) {
    known.delete(a.asset);
    known.set(a.asset, a);
  }
  while (known.size > 5000) known.delete(known.keys().next().value as string);
}

/** What the wallet holds of an asset, by id. */
export function holdingOf(id: string): Holding | undefined {
  const list = ba.mine?.assets ?? null;
  if (heldMapOf !== list) {
    heldMap = new Map((list ?? []).map((h) => [h.asset, h]));
    heldMapOf = list;
  }
  return heldMap.get(id);
}

/** The display name of an asset, by id, if it has one (among those read or found). Text of the chain. */
export function displayNameOf(id: string): string | undefined {
  if (/^0+$/.test(id)) return undefined;
  return assetById(id)?.display_name || undefined;
}

/** Whether the chain has tickers and display names from the next block (getbitassetsinfo; older nodes: never). */
export function tickersActive(): boolean {
  return ba.info?.tickers_active === true;
}

/** The decimals of an asset, by id (ECX: 8; an asset not listed: 0). */
export function decimalsOf(id: string): number {
  if (/^0+$/.test(id)) return 8;
  return assetById(id)?.decimals ?? holdingOf(id)?.decimals ?? 0;
}

/** The label of an asset, by id. Text of the chain. */
export function labelOf(id: string): string {
  if (/^0+$/.test(id)) return TICKER;
  return assetById(id)?.label ?? holdingOf(id)?.label ?? `0x${id.slice(0, 12)}…`;
}

/** An asset as one line of text, by id, for toasts and the titles of dialogs: "Theodore Brosevelt $BROSEVELT #1739-0029" (model.ts: assetText). Text of the chain. */
export function assetTextOf(id: string, label?: string): string {
  return assetText(label ?? labelOf(id), displayNameOf(id), assetById(id)?.seq, id);
}

/**
 * How a confirmation names an asset: what is unique first, its $TICKER and its number, then its display name, quoted
 * and escaped, in brackets ('$GOLD #1739-0029 ("Gold Bar")'); the start and end of its hash for the number when it
 * has none listed (a private name). Labels are any printable ASCII, so look-alikes (G0LD and GOLD, USDl and USDI) are
 * told apart by the number, as the pickers show it (audit port-2 #5); a label or display name may hold another asset's
 * ticker or number ("Bitcoin $ECX #1", "GOLD (#1739-0029)"), which, escaped and quoted after the real ones, reads as text
 * (audit 7 E-BA4; security review 2026-10-24: model.ts escapeName). Text of the chain.
 */
export function nameOf(id: string, label?: string): string {
  if (/^0+$/.test(id)) return TICKER;
  const listed = assetById(id);
  const text = label ?? listed?.label ?? holdingOf(id)?.label ?? '';
  const tag = listed?.seq ? `#${listed.seq}` : shortId(id);
  const a = assetIdentity(text, listed?.display_name);
  const head = a.ticker ? `$${a.ticker} ${tag}` : tag;
  const names = [a.name, a.alias].filter((x) => x !== '').map((x) => `"${escapeName(x)}"`);
  return names.length ? `${head} (${names.join(', ')})` : head;
}

/**
 * The pool of two assets, either way round: the one of the kind addliquidity makes (constant product, 0.3%), which
 * swapasset, addliquidity and removeliquidity name; a pair may have pools of other kinds besides (Layers' DEX: poolById,
 * poolsOf).
 */
export function poolOf(a: string, b: string): Pool | undefined {
  const list = ba.pools;
  if (poolMapOf !== list) {
    poolMap = new Map();
    pooled = new Set();
    poolIds = new Map();
    // The first of the list either way round, as a search of it found; the default kind before any other.
    const better = (key: string, p: Pool) => {
      const had = poolMap.get(key);
      if (!had || (had.default_kind === false && p.default_kind !== false)) poolMap.set(key, p);
    };
    for (const p of list) {
      poolIds.set(p.pool, p);
      // An order book (Layers' DEX) is no pool to swap in or add to by the pair: by its id only.
      if (p.curve !== 'book') {
        better(`${p.asset_a_id}:${p.asset_b_id}`, p);
        better(`${p.asset_b_id}:${p.asset_a_id}`, p);
      }
      if (!p.abandoned) {
        pooled.add(p.asset_a_id);
        pooled.add(p.asset_b_id);
      }
    }
    poolMapOf = list;
  }
  return poolMap.get(`${a}:${b}`);
}

/** A pool by its id, among those read. */
export function poolById(id: string): Pool | undefined {
  void poolOf('', '');
  return poolIds.get(id);
}

/** The pools of two assets read, of every kind, either way round: the default kind first. */
export function poolsOf(a: string, b: string): Pool[] {
  return ba.pools
    .filter((p) => (p.asset_a_id === a && p.asset_b_id === b) || (p.asset_a_id === b && p.asset_b_id === a))
    .sort((x, y) => Number(y.default_kind !== false) - Number(x.default_kind !== false));
}

/** Whether an asset has a pool not abandoned (a market). */
export function hasPool(id: string): boolean {
  void poolOf('', '');
  return pooled.has(id);
}

/** The parts of every swap fee (getdexinfo, read with the store): null from a node that does not say them. */
export function swapParts(): SwapFeeParts | null {
  return swapFeeParts(ba.dex?.swap_fee_parts);
}

/** The price of one of an asset in ECX, from its pool with ECX (undefined when it has none, or nobody provides to it). */
export function chnPrice(id: string): number | undefined {
  if (/^0+$/.test(id)) return 1;
  const p = poolOf(id, '0'.repeat(64));
  if (!p || p.abandoned || !p.price) return undefined;
  return p.asset_a_id === id ? p.price : 1 / p.price;
}

/** What the wallet can spend of an asset, in its units (ECX: undefined, the kit knows it). */
export function heldOf(id: string): Amount | undefined {
  return ba.mine?.assets.find((a) => a.asset === id)?.balance;
}
