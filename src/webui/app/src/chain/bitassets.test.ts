// The pages of BitAssets: what model.ts works out (amounts with the decimals of an asset, the rules of names
// and data, the pools and the auctions as the chain does them), the journal kept in the file of the host, and
// the pages driven against a pretend node: the assets and an asset opened, an asset of the wallet minted,
// changed and burned, an asset issued by the wizard then registered by itself, a swap, a new pool, a bid and
// buying all of an auction, an auction started, an asset sent from the Send page, the cards of the home, and
// where the pages are in the menus.
import { cleanup, fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { createHash } from 'node:crypto';
import { afterEach, beforeAll, beforeEach, describe, expect, it } from 'vitest';
import { CHAIN } from './config';
import { LOCAL_COMMANDS, LOCAL_PAGES, type LocalPage } from './local';
import LocalCards from './LocalCards.svelte';
import { checkLogo } from './ba/logo';
import LocalOverview from './LocalOverview.svelte';
import LocalSend from './LocalSend.svelte';
import DialogHost from '../components/DialogHost.svelte';
import Assets from './pages/Assets.svelte';
import MyAssets from './pages/MyAssets.svelte';
import Dex from '../test/DexHarness.svelte';
import { dexRoute, RETIRED_PAGES } from './local';
import AssetView from './ba/AssetView.svelte';
import AddLiquidity from './ba/AddLiquidity.svelte';
import {
  assetArg,
  auctionBuysAt,
  auctionCostOfRemaining,
  auctionPriceAt,
  auctionProblems,
  CHN_ID,
  dataProblems,
  dataUpdates,
  formatPrice,
  formatUnits,
  hashName,
  isAbandoned,
  lessSlippage,
  nameProblem,
  pairedAmount,
  parseUnits,
  priceImpact,
  provide,
  receiptAction,
  swapIn,
  swapOut,
  unitPrice,
  unitsAtLeast,
  unitsAtMost,
  unitsOf,
  nameMimicsNumber,
  withdraw,
  assetTitle,
  displayNameProblem,
  isTicker,
  normalizeDisplayName,
  tickerProblem,
  type Asset,
  type Auction,
  type Holdings,
  type Pool,
} from './ba/model';
import { pendingList } from '../lib/pending.svelte';
import { resetPositions } from './ba/positions.svelte';
import { resetSearchSupport } from './ba/search';
import { assetHidden, assetIgnored, filtersOf, ignoreAsset, ignoreName, loadHiding, resetHiding, searchFiltersOf, setFilters, unignoreAsset, unignoreName } from './ba/hide.svelte';
import HeldAssets from './ba/HeldAssets.svelte';
import UnlockedSupplyBadge from './ba/UnlockedSupplyBadge.svelte';
import Transactions from '../pages/Transactions.svelte';
import LocalSettings from './LocalSettings.svelte';
import { LOCAL_SETTINGS_TABS } from './settings';
import { chainTxHidden, chainTxNotice } from './txinfo.svelte';
import { ACTIVITY_PAGE, ASSET_PAGE, ba, baView, nameOf, notePrices, priceSeries, refreshBitAssets, resetBitAssets, sanitizeJournal, sendIntent } from './ba/store.svelte';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { buildCommands, menuSections } from '../lib/commands.svelte';
import { dialogs } from '../lib/dialogs.svelte';
import { AREAS, chainBeforeTools, HOME_QUICK_ACTIONS, NAV_SECTIONS } from '../lib/layout';
import { go, nav } from '../lib/nav.svelte';
import { handleShortcut } from '../lib/keyboard';
import { app, node, prefs, wallets } from '../lib/state.svelte';
import { resetWalletData } from '../lib/walletdata.svelte';
import { encode, program } from '../test/bech32';

const HRP = CHAIN.networks.regtest.hrp;
const OTHER = encode(HRP, [0, ...Array.from({ length: 32 }, (_, i) => (i * 7 + 3) % 32)], false);
const ME = encode(HRP, [0, ...program(20)], false);
const id = (c: string) => c.repeat(64);
const GOLD = id('a');
const SILVER = id('b');
const PEBBLES = id('c');

function asset(a: string, extra: Partial<Asset>): Asset {
  return { asset: a, label: 'X', name: 'X', seq: '1739-0029', decimals: 2, supply: 1000, minted: 1000, burned: 0, fixed: false, registered: 5, registration: id('9'), releasable: false, ...extra };
}

let assets: Asset[] = [];
let pools: Pool[] = [];
let auctions: Auction[] = [];
let mine: Holdings = { assets: [], reservations: [], liquidity: [], receipts: [] };
let store: unknown = null;
let locked = false;
let searchSupported = false;
let prefsStore: Record<string, unknown> = {};
let history: string[] = [];
let hidden: Asset[] = [];
let activityCount = 1;
let tickersOn = false;
/** Layers' DEX with its pool kinds (getdexinfo): off, a node from before them (the method unknown). */
let dexOn = false;
/** v5, fee flow: getlaunchstats' rules from its height; fields of getlaunch's MOON and STAR besides. */
let feeFlowOn = false;
/** v7, fee split: getlaunchstats' rules say fee_split_active (and the fee 75 / 25). */
let splitOn = false;
let launchPatch: Record<string, unknown> = {};
/** Whether previewpool says the pool is there already (someone made it first). */
let previewExists = false;
/** The launch parameters (consensus "launchparams": a launch's own supply, decimals and fee; curveasset) apply. */
let paramsOn = false;
/** The launches the fake node knows (Layers' launches), and how many more past them (pages). */
let launchCount = 2;
/** The wallet's positions of concentrated liquidity (listpositions); null: two of a range in the pool k. */
let positions: Record<string, unknown>[] | null = null;
/** The market index's candles (getpoolcandles, -dexindex): none on a node without it (the method fails). */
let candles: { t: number; o: number; h: number; l: number; c: number; v: number }[] | null = null;
/** Launches of the test's own, after the others (My launches: a curve never traded, one traded). */
let extraLaunches: Record<string, unknown>[] = [];
const MOON = id('m');
const STAR = id('t');
function launchOf(token: string, i = 0): Record<string, unknown> {
  const star = token === STAR;
  return { token, ticker: token === MOON ? 'MOON' : star ? 'STAR' : `L${i}`, display_name: token === MOON ? 'To the moon' : undefined, info: token === MOON ? 'A token for the moon.\nhttps://example.org' : undefined, has_logo: false, creator: ME, txid: id('8'), height: 12, status: star ? 'graduated' : 'curve', pool: star ? id('g') : id('c'), raised: star ? '500.00000003' : '200.00000000', graduation: '500.00000000', progress: star ? 100 : 40, price: star ? 0.000002 : 0.00000081, market_cap: star ? '2000.00000000' : '810.00000000', supply: star ? '1020000000.000000' : '1000000000.000000', curve_tokens_left: star ? 0 : 400000000, fee: 1, creator_share: 30, creator_fees_owed: token === MOON ? '0.01000000' : '0.00000000', creator_fees_paid: '0.00000000', allocation: 2, allocation_claimed: false, min_buy: '0.00000000', max_buy: '0.00000000', volume: '210.00000000', trades: 5, ...(star ? { graduated_height: 18 } : {}) , ...(token === MOON || token === STAR ? launchPatch : {}) };
}
const calls: BridgeRequest[] = [];

function fresh() {
  assets = [
    asset(GOLD, { label: 'GOLD', name: 'GOLD', seq: '1739-0029', data: { info: 'One gram of gold', commitment: id('e') }, control: { txid: id('9'), vout: 0, address: ME } }),
    asset(SILVER, { label: 'SILVER', name: 'SILVER', seq: '4218-0372', decimals: 0, supply: 50, minted: 60, burned: 10, fixed: true, registered: 7 }),
    asset(PEBBLES, { label: 'PEBBLES', name: 'PEBBLES', seq: '9031-1144', decimals: 0, supply: 0, minted: 5, burned: 5, fixed: true, registered: 8, releasable: true, release_fee: 0.0001 }),
  ];
  pools = [
    { pool: id('p'), asset_a: 'GOLD', asset_b: 'ECX', asset_a_id: GOLD, asset_b_id: CHN_ID, reserve_a: 500, reserve_b: 50, price: 0.1, shares: 15811388, volume_a: 20, volume_b: 2, swaps: 4, created: 9, abandoned: false },
  ];
  auctions = [
    { auction: id('d'), base: 'GOLD', quote: 'ECX', base_id: GOLD, quote_id: CHN_ID, amount: 100, remaining: 60, proceeds: 4, start_price: 10, end_price: 5, unit_price: 0.09, price: 9, cost_of_remaining: 5.4, start_height: 10, end_height: 59, bids: 1, status: 'open' },
  ];
  mine = {
    assets: [{ asset: GOLD, label: 'GOLD', decimals: 2, balance: 200, pending: 0, outputs: 1, control: true, control_pending: false, supply: 1000, fixed: false },
      { asset: SILVER, label: 'SILVER', decimals: 0, balance: 30, pending: 0, outputs: 1, control: false, control_pending: false, supply: 50, fixed: true },
    ],
    reservations: [{ txid: id('r'), name: 'IRON', taken: false, height: 18, registers_from: 20, wait: 0 }],
    liquidity: [{ pool: id('p'), asset_a: 'GOLD', asset_b: 'ECX', asset_a_id: GOLD, asset_b_id: CHN_ID, shares: 1000000, percent: 6.3245, value_a: 31.62, value_b: 3.162 }],
    receipts: [{ auction: id('d') }],
  };
}

/** Pick an asset in the searchable picker of a field (ba/AssetPicker.svelte), as a user does: open it, click the asset. */
async function pickAsset(id: string, asset: string): Promise<void> {
  await fireEvent.click(document.querySelector(`#${id}`)!);
  const option = await waitFor(() => {
    const o = document.querySelector<HTMLElement>(`[role="option"][data-id="${asset}"]`);
    if (!o) throw new Error(`no option ${asset} in ${id}`);
    return o;
  });
  await fireEvent.click(option);
}

function units(n: number | string, d: number): bigint {
  return unitsOf(n, d);
}

// What pays or signs goes through the host (rpc.confirmed, local.cpp ConfirmedRpcMethods), which asks on the desktop and runs
// the call: here, at once, as the node would.
const CONFIRMED = ['reserveasset', 'registerasset', 'releaseassetreservation', 'sendasset', 'mintasset', 'burnasset', 'updateasset', 'transferassetcontrol', 'fixassetsupply', 'releaseasset', 'swapasset', 'addliquidity', 'removeliquidity', 'createauction', 'bidauction', 'collectauction', 'setassetlogo', 'setassetdisplayname', 'createpool', 'addpoolliquidity', 'removepoolliquidity', 'swappools', 'swaproute', 'launchtoken', 'buyfromlaunch', 'selltolaunch', 'claimlaunch', 'openposition', 'changeposition', 'curveasset', 'placeorder'];

// The questions the host asked (each a dialog on the desktop), the second steps it was told to run without one
// (confirm.h Authorizations: [wallet, method, params] as JSON), and how long a question takes to be answered.
const asked: string[] = [];
const authorized: string[] = [];
let answerDelay = 0;

async function fakeNode(r: BridgeRequest): Promise<unknown> {
  if (r.method === 'rpc.confirmedMethods') return { confirmed: CONFIRMED, keyed: ['listmyassets', 'listassetactivity'] };
  if (r.method === 'rpc.confirmed') {
    const c = r.params as { method: string; params: unknown[]; wallet?: string; then?: { method: string; params: unknown[] }; authorizedOnly?: boolean };
    expect(CONFIRMED).toContain(c.method);
    const key = JSON.stringify([c.wallet, c.method, c.params]);
    const i = authorized.indexOf(key);
    if (i >= 0) {
      authorized.splice(i, 1);
    } else if (c.authorizedOnly) {
      throw Object.assign(new Error(`${c.method} needs your confirmation`), { code: -32801 });
    } else {
      asked.push(c.then ? `${c.method}+${c.then.method}` : c.method);
      if (answerDelay) await new Promise((resolve) => setTimeout(resolve, answerDelay));
    }
    const result = await fakeNode({ ...r, kind: 'rpc', method: c.method, params: c.params, wallet: c.wallet } as BridgeRequest);
    if (!c.then) return result;
    authorized.push(JSON.stringify([c.wallet, c.then.method, c.then.params]));
    return { ...(result as object), authorized: 3600 };
  }
  calls.push(r);
  const p = (r.params ?? []) as unknown[];
  const fail = (message: string, code = -8) => {
    throw Object.assign(new Error(message), { code });
  };
  const byArg = (arg: unknown) => assets.find((a) => assetArg(a.asset) === arg || a.label === arg);
  switch (r.method) {
    case 'getbitassetsinfo':
      return { assets: assets.length + hidden.length, reservations: 1, pools: pools.length, auctions: auctions.length, height: 21, reveal_depth: 2, logo_height: 0, max_logo_bytes: 32768, ...(tickersOn ? { ticker_height: 15, tickers_active: true, min_ticker_length: 2, max_ticker_length: 12, max_display_name_bytes: 64 } : { ticker_height: -1, tickers_active: false }) };
    case 'listassets': {
      // [count, skip, match], as the node takes them; `hidden` are the newest past what the page reads.
      const all = [...assets, ...hidden].filter((a) => p[2] === undefined || a.label.toLowerCase().includes(String(p[2]).toLowerCase()));
      return p[2] === undefined ? assets.slice(Number(p[1] ?? 0), Number(p[1] ?? 0) + Number(p[0] ?? 100)) : all.slice(0, Number(p[0]));
    }
    case 'searchmyassets':
    case 'searchassets': {
      // [query, filters, sort, cursor, limit], as the node of the other work will take them; an older node has none.
      // searchmyassets: the wallet's (the same arguments); `mine` is no filter of searchassets.
      if (!searchSupported) fail('Method not found', -32601);
      const [q, f, sort, cursor, limit] = p as [string, Record<string, unknown>, string, string | null, number];
      if (f.mine !== undefined) fail('mine is the wallet\'s: searchmyassets (a wallet command, with the same arguments)');
      const held = new Set(mine.assets.map((h) => h.asset));
      let list = [...assets, ...hidden].filter((a) => (r.method !== 'searchmyassets' || held.has(a.asset)) && (!q || a.label.toLowerCase().includes(q.toLowerCase()) || a.seq === q));
      // exclude_dead: not touched in the last this many blocks (true: the volume window, 1440); the tip is 20.
      if (f.exclude_dead) list = list.filter((a) => 20 - (a.last_activity ?? 20) < (f.exclude_dead === true ? 1440 : Number(f.exclude_dead)));
      const words = ((f.exclude_keywords as string[] | undefined) ?? []).map((w) => w.toLowerCase());
      list = list.filter((a) => !words.some((w) => a.label.toLowerCase().includes(w)));
      list.sort(sort === 'name' ? (x, y) => x.label.localeCompare(y.label) : sort === 'oldest' ? (x, y) => x.registered - y.registered : (x, y) => y.registered - x.registered);
      const from = cursor ? Number(cursor) : 0;
      const page = list.slice(from, from + limit);
      return { assets: page, cursor: from + limit < list.length ? String(from + limit) : null, total: list.length };
    }
    case 'getasset': {
      // The launched tokens are assets too (their pages: ba/AssetView.svelte).
      const launched = p[0] === assetArg(MOON) ? asset(MOON, { label: 'MOON', name: 'MOON', display_name: 'To the moon', seq: '2001-0001', decimals: 6, supply: 1000000000, minted: 1000000000, fixed: true }) : p[0] === assetArg(STAR) ? asset(STAR, { label: 'STAR', name: 'STAR', seq: '2001-0002', decimals: 6, supply: 1020000000, minted: 1020000000, fixed: true }) : undefined;
      const a = byArg(p[0]) ?? (dexOn ? launched : undefined);
      if (!a) fail('No such asset is registered');
      return a;
    }
    case 'getassethistory':
      return p[0] === assetArg(GOLD) ? { info: [{ value: null, txid: id('9'), height: 5 }, { value: 'One gram of gold', txid: id('8'), height: 12 }], commitment: [{ value: null, txid: id('9'), height: 5 }] } : {};
    case 'listpools': {
      // [asset, count, skip], as the node takes them (paged: audit 7 E-BA3).
      const list = p[0] != null ? pools.filter((x) => assetArg(x.asset_a_id) === p[0] || assetArg(x.asset_b_id) === p[0]) : pools;
      return list.slice(Number(p[2] ?? 0), Number(p[2] ?? 0) + Number(p[1] ?? 100));
    }
    case 'getpool': {
      // By id, or by the pair.
      if (p.length === 1) return pools.find((q) => q.pool === p[0]) ?? fail('No such pool');
      const x = pools.find((q) => [assetArg(q.asset_a_id), assetArg(q.asset_b_id)].sort().join() === [p[0], p[1]].sort().join());
      if (!x) fail('There is no pool of these assets');
      return x;
    }
    case 'quoteswap': {
      const x = pools[0];
      const payChn = p[0] === 'ECX';
      const rin = payChn ? units(x.reserve_b, 8) : units(x.reserve_a, 2);
      const rout = payChn ? units(x.reserve_a, 2) : units(x.reserve_b, 8);
      const din = payChn ? 8 : 2;
      const dout = payChn ? 2 : 8;
      const ain = parseUnits(String(p[1]), din);
      const aout = swapOut(rin, rout, ain.ok ? ain.units : 0n);
      return { amount_in: Number(formatUnits(ain.ok ? ain.units : 0n, din, { group: false })), amount_out: Number(formatUnits(aout, dout, { group: false })), price: 1, spot_price: 1, price_impact: priceImpact(rin, rout, ain.ok ? ain.units : 0n, aout), fee: 0.003, abandoned: false };
    }
    // Layers' DEX: its pool kinds (src/rpc/bitassets.cpp), as the node answers them.
    case 'getdexinfo':
      if (!dexOn) fail('Method not found', -32601);
      return { dex_height: 0, active: true, max_route_hops: 4, curves_height: 10, curves_active: true, fee_tiers: [0.01, 0.05, 0.3, 1], curves: ['constant_product', 'stable', 'weighted'], min_amp: 1, max_amp: 10000, weights: [2, 5, 10, 20, 25, 40, 60, 75, 80, 90, 95, 98], swap_fee_parts: { liquidity_providers: 85, security: 15 } };
    case 'getswapquote': {
      // [asset_in, amount, asset_out, {max_hops}]: the pair's pools of every kind, best first.
      if (!dexOn) fail('Method not found', -32601);
      const pair = pools.filter((x) => [assetArg(x.asset_a_id), assetArg(x.asset_b_id)].sort().join() === [p[0], p[2]].sort().join() && !x.abandoned);
      const gives: Record<string, number> = { [id('s')]: 5.2, [id('p')]: 4.9 };
      const list = pair.map((x) => ({ pool: x.pool, curve: x.curve ?? 'constant_product', fee: x.fee ?? 0.3, amp: x.amp, default_kind: x.default_kind ?? true, amount_in: Number(p[1]), amount_out: gives[x.pool] ?? 1, price: 0.1, spot_price: 0.1, price_impact: x.pool === id('p') ? 1 : 0.5, fee_paid: 0.001, abandoned: false }));
      list.sort((x, y) => y.amount_out - x.amount_out);
      return { pools: list, best_pool: list[0]?.pool };
    }
    case 'quoteroute':
      // A route through SILVER, of a weighted pool and a stable one, that gives more than either pool of the pair.
      if (!dexOn) fail('Method not found', -32601);
      return p[0] === 'CHN' && p[2] === assetArg(GOLD)
        ? { amount_in: Number(p[1]), amount_out: 5.5, path: ['CHN', 'SILVER', 'GOLD'], pools: [id('q'), id('r')], routes: [{ path: ['CHN', 'SILVER', 'GOLD'], path_ids: [CHN_ID, SILVER, GOLD], amount_out: 5.5, pools: [id('q'), id('r')], default_pools: false, hops: [{ pool: id('q'), curve: 'weighted', fee: 0.3, amount_out: 3 }, { pool: id('r'), curve: 'stable', fee: 0.05, amount_out: 5.5 }] }] }
        : { amount_in: Number(p[1]), amount_out: 0, path: [], pools: [], routes: [] };
    case 'previewpool': {
      const kind = (p[4] ?? {}) as { curve?: string; fee?: number; amp?: number; weight_a?: number };
      return { pool: id('n'), exists: previewExists, open: false, shares: 999000, price: Number(p[3]) / Number(p[1]), curve: kind.curve, fee: kind.fee, amp: kind.amp, default_kind: false, depth: [
        { side: 'sell_a', size_percent: 1, amount_in: 0.1, amount_out: 0.0099, avg_price: 0.099, price_after: 0.098, price_impact: 0.4 },
        { side: 'sell_b', size_percent: 1, amount_in: 0.01, amount_out: 0.099, avg_price: 0.101, price_after: 0.102, price_impact: 0.4 },
        { side: 'sell_a', size_percent: 10, amount_in: 1, amount_out: 0.09, avg_price: 0.09, price_after: 0.08, price_impact: 9 },
        { side: 'sell_b', size_percent: 10, amount_in: 0.1, amount_out: 0.9, avg_price: 0.11, price_after: 0.12, price_impact: 9 },
      ] };
    }
    case 'getpoolcurve': {
      // The pool's own order: CHN, the null hash, first.
      const x = pools.find((q) => q.pool === p[0]);
      if (!x) fail('No such pool');
      return { pool: p[0], asset_a: 'CHN', asset_b: x!.asset_a, asset_a_id: CHN_ID, asset_b_id: x!.asset_a_id, price: 10, curve: x!.curve, fee: x!.fee, amp: x!.amp, default_kind: x!.default_kind, depth: [
        { side: 'sell_a', size_percent: 0.1, amount_in: 0.05, amount_out: 0.49, avg_price: 9.8, price_after: 9.9, price_impact: 0.2 },
        { side: 'sell_b', size_percent: 0.1, amount_in: 0.5, amount_out: 0.0499, avg_price: 0.0998, price_after: 0.0997, price_impact: 0.2 },
        { side: 'sell_a', size_percent: 50, amount_in: 25, amount_out: 160, avg_price: 6.4, price_after: 4.4, price_impact: 36 },
        { side: 'sell_b', size_percent: 50, amount_in: 250, amount_out: 16, avg_price: 0.064, price_after: 0.044, price_impact: 36 },
      ] };
    }
    case 'quotepoolliquidity': {
      // [pool, amounts] or [pool, null, shares, only]: by label, in the pool's own order (CHN first).
      const x = pools.find((q) => q.pool === p[0]);
      if (!x) fail('No such pool');
      if (p[1]) return { shares: 4321, opens: false, amounts: { CHN: 0, [x!.asset_a]: 10 }, share_of_pool: 0.34 };
      return { shares: p[2], opens: false, amounts: p[3] ? { CHN: 0, [x!.asset_a]: 2.5 } : { CHN: 0.12, [x!.asset_a]: 1.25 }, share_of_pool: 7.9 };
    }
    case 'createpool':
      return { txid: id('7'), pool: id('n'), shares: 999000 };
    // Concentrated liquidity: the node's own order (CHN first in a pool with CHN): CHN in GOLD at 10.
    case 'getpoolticks':
      return { tick: 23027, price: 10, liquidity: '5000000', tick_spacing: 60, ticks: [{ tick: 20820, price: 8, liquidity_net: '5000000', liquidity_gross: '5000000', liquidity_above: '5000000' }, { tick: 24840, price: 12, liquidity_net: '-5000000', liquidity_gross: '5000000', liquidity_above: '0' }] };
    case 'previewposition':
      return { tick_lower: 22500, tick_upper: 23520, price_lower: Number(p[1]), price_upper: Number(p[2]), liquidity: '12345', amount0: 0.5, amount1: 5, in_range: true };
    case 'openposition':
      return { txid: id('7'), position: id('o'), tick_lower: 22500, tick_upper: 23520, liquidity: '12345' };
    case 'changeposition':
      return { txid: id('7') };
    case 'listpositions':
      return positions ?? [
        { position: id('o'), pool: id('k'), asset0: 'CHN', asset1: 'GOLD', tick_lower: 22500, tick_upper: 23520, price_lower: 9.5, price_upper: 10.5, liquidity: '12345', in_range: true, amount0: 0.5, amount1: 5, fees0: 0.01, fees1: 0.25 },
        { position: id('u'), pool: id('k'), asset0: 'CHN', asset1: 'GOLD', tick_lower: 24000, tick_upper: 24840, price_lower: 11, price_upper: 12, liquidity: '999', in_range: false, amount0: 0.3, amount1: 0, fees0: 0, fees1: 0 },
      ];
    // Layers' launches (src/rpc/bitassets.cpp), as the node answers them.
    case 'getlaunchstats':
      if (!dexOn) fail('Method not found', -32601);
      return { launches: launchCount, on_curve: launchCount - 1, graduated: 1, volume: '420.00000000', raised: '700.00000003', rules: { ...(feeFlowOn ? { fee_flow_active: true, fee_flow_height: 30, graduation_fee: 1, graduated_creator_share: 60 } : {}), graduation: '500.00000000', fee: 1.15, creator_share: splitOn ? 75 : 80, security_share: splitOn ? 25 : 20, ...(splitOn ? { fee_split_active: true } : {}), supply: '1000000000.000000', curve_supply: '750000000.000000', pool_supply: '250000000.000000', max_allocation: 5, log_blocks: 10080, active: true, ...(paramsOn ? { params_active: true, params_height: 20, fee_min: 0.3, fee_max: 5, fee_default: 1.15, supply_min_units: '1000000', supply_max_units: '8198552921648689600', supply_multiple_units: 8, decimals_max: 12, decimals_default: 6, supply_default: '1000000000' } : { params_active: false, params_height: 40 }) } };
    case 'searchpools':
      // An older node has none: the pages fall back on the pools they read (ba/search.ts).
      if (!searchSupported) fail('Method not found', -32601);
      return { pools };
    case 'listlaunches': {
      const o = (p[0] ?? {}) as { status?: string; cursor?: string; limit?: number; min_progress?: number };
      const all = [launchOf(MOON), launchOf(STAR), ...Array.from({ length: launchCount - 2 }, (_, i) => launchOf((i + 0x100).toString(16).padStart(64, '0'), i)), ...extraLaunches];
      const list = all.filter((l) => (o.status === 'graduated' ? l.status === 'graduated' : o.status === 'curve' || o.status === 'new' ? l.status === 'curve' : true) && Number(l.progress) >= (o.min_progress ?? 0));
      const from = Number(o.cursor || 0);
      const limit = o.limit ?? 50;
      return { launches: list.slice(from, from + limit), total: list.length, ...(from + limit < list.length ? { next_cursor: String(from + limit) } : {}) };
    }
    case 'getlaunch':
      if (p[0] === assetArg(MOON)) return launchOf(MOON);
      if (p[0] === assetArg(STAR)) return launchOf(STAR);
      return fail('Not a launched token');
    case 'quotelaunch': {
      // [token, amount, side]: a buy of more than 300 CHN passes the curve's end.
      const amount = Number(p[1]);
      const past = p[2] === 'buy' && amount > 300;
      return p[2] === 'buy'
        ? { amount_in: amount, amount_out: past ? 400000000 : 2469135.802469, back: past ? '100.00000000' : '0.00000000', fee: '0.02000000', price: 0.00000081, spot_price: 0.0000008, price_after: 0.00000082, price_impact: 1.2, progress_after: past ? 100 : 40.4, graduates: past }
        : { amount_in: amount, amount_out: 0.79, back: '0.00000000', fee: '0.00800000', price: 0.00000079, spot_price: 0.0000008, price_after: 0.00000079, price_impact: 1.3, progress_after: 39.8, graduates: false };
    }
    case 'getpoolcandles':
      if (!candles) fail('This node keeps no market index (-dexindex)', -1);
      return candles;
    case 'getlaunchlog':
      return [{ height: 15, token: MOON, created: false, graduated: false, orders: 2, coin_in: '10.00000000', coin_out: '0.00000000', tokens_in: 0, tokens_out: 12000000, fees: '0.10000000', reserve_coin: '450.00000000', reserve_tokens: 625000000 }, { height: 12, token: MOON, created: true, graduated: false, orders: 1, coin_in: '1.00000000', coin_out: '0.00000000', tokens_in: 0, tokens_out: 4000000, fees: '0.01000000', reserve_coin: '251.00000000', reserve_tokens: 1120000000 }, { height: 13, token: STAR, created: false, graduated: false, orders: 1, coin_in: '1.00000000', coin_out: '0.00000000', tokens_in: 0, tokens_out: 1, fees: '0.01000000', reserve_coin: '1.00000000', reserve_tokens: 1 }];
    case 'launchtoken':
      return { txid: id('7'), token: MOON, ticker: p[0], pool: id('c') };
    case 'curveasset':
      if (!paramsOn) fail('Method not found', -32601);
      return { txid: id('7'), asset: String(p[0]).replace(/^0x/, ''), pool: id('c'), curve_supply: '750', pool_supply: '250', fee: 1.15 };
    case 'buyfromlaunch':
      return { txid: id('7'), quote: 2469135.802469, min_out: 2444444.444444, back: '0.00000000' };
    case 'selltolaunch':
      return { txid: id('7'), quote: '0.79000000', min_out: '0.78210000' };
    case 'claimlaunch':
      return { txid: id('7'), fees: '0.01000000', allocation: 0 };
    case 'getaddressinfo':
      return { ismine: p[0] === ME };
    // The wallet's "launch creator" addresses (My launches).
    case 'getaddressesbylabel':
      return p[0] === 'launch creator' ? { [ME]: { purpose: 'receive' } } : {};
    case 'addpoolliquidity':
      return { txid: id('7'), shares: 1234 };
    case 'removepoolliquidity':
      return { txid: id('7'), shares: 500, amounts: { GOLD: 1, CHN: 0 } };
    case 'swappools':
      return { txid: id('7'), asset_out: 'GOLD', quote: 5.2, min_out: 5.17 };
    case 'swaproute':
      return { txid: id('7'), quote: 5.5, min_out: 5.47 };
    case 'listauctions': {
      // [include_closed, count, skip, asset].
      const list = (p[0] ? [...auctions, { ...auctions[0], auction: id('f'), status: 'closed', remaining: 0 }] : auctions).filter((a) => p[3] == null || assetArg(a.base_id) === p[3] || assetArg(a.quote_id) === p[3]);
      return list.slice(Number(p[2] ?? 0), Number(p[2] ?? 0) + Number(p[1] ?? 100));
    }
    case 'getmempoolentry':
      return fail('Transaction not in mempool', -5);
    case 'getauction':
      return auctions.find((a) => a.auction === p[0]) ?? (p[0] === id('e') ? { ...auctions[0], auction: id('e') } : fail('No such auction'));
    case 'quotebid':
      return { buys: 10, remaining: 60, cost_of_remaining: 5.4, open: true };
    case 'listmyassets':
      if (locked) fail('Please enter the wallet passphrase with walletpassphrase first.', -13);
      return structuredClone(mine);
    case 'listassetactivity':
      if (locked) fail('Please enter the wallet passphrase with walletpassphrase first.', -13);
      return Array.from({ length: Math.min(Number(p[0] ?? 50), activityCount) }, (_, i) => ({ txid: `5${(i + 1).toString(16).padStart(63, '0')}`, time: 1_700_000_000 - i, confirmations: 3, operation: 'mint', summary: 'Minted 10.00 GOLD' }));
    case 'reserveasset':
      return { txid: id('6'), asset: 'f'.repeat(64) };
    case 'registerasset':
    case 'mintasset':
    case 'burnasset':
    case 'updateasset':
    case 'transferassetcontrol':
    case 'fixassetsupply':
    case 'releaseasset':
    case 'sendasset':
    case 'createauction':
    case 'collectauction':
    case 'releaseassetreservation':
    case 'setassetlogo':
    case 'setassetdisplayname':
      return { txid: id('7') };
    case 'swapasset':
      return { txid: id('7'), quote: 4.98, min_out: 4.95 };
    case 'addliquidity':
      return { txid: id('7'), amount_a: 10, amount_b: 1, shares: 9000, sets_price: true };
    case 'removeliquidity':
      return { txid: id('7'), shares: 500000, amount_a: 15.81, amount_b: 1.581 };
    case 'bidauction':
      return { txid: id('7'), pays: p[2] ? 5.4 : 1, buys: p[2] ? 60 : 10 };
    case 'bitassets.store.load':
      return structuredClone(store);
    case 'bitassets.store.save':
      store = structuredClone((r.params as { value: unknown }).value);
      return true;
    case 'getnewaddress':
      return OTHER;
    case 'validateaddress':
      return { isvalid: true, address: p[0] };
    case 'prefs.get':
      return prefsStore;
    case 'prefs.set':
      Object.assign(prefsStore, p);
      return {};
    case 'wallet.transactions':
      // The history of the wallet: one record per transaction of `history` (RecordJson in walletapi.cpp).
      return { height: 21, hash: 'ab'.repeat(32), maturity: 100, records: history.map((txid, i) => [txid, 0, 'recv', ME, '', 1_700_000_000 + i, 0, 0, 'confirmed', 3, 0, true, 'key', 19, 'c', false, false, true]) };
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
  window.matchMedia ??= ((query: string) => ({ matches: false, media: query, addEventListener() {}, removeEventListener() {} })) as unknown as typeof window.matchMedia;
});

beforeEach(() => {
  app.info = { name: CHAIN.name, version: 'test', bugreport: '', network: 'regtest', networkLabel: 'regtest', datadir: '/tmp', walletEnabled: true, devtools: false, test: true, platform: 'linux', blockSpacing: 60 };
  prefs.unit = 'CHN';
  prefs.mask = false;
  node.tip = { height: 20, time: 1_700_000_000, hash: 'ab'.repeat(32), progress: 1, state: 'synced', ibd: false, loadingBlocks: false };
  wallets.list = [{ name: 'w', encrypted: false, locked: false, privateKeysDisabled: false, hd: true }];
  wallets.current = 'w';
  wallets.balances.w = { trusted: 1_000_000_000n, untrustedPending: 0n, immature: 0n };
  fresh();
  hidden = [];
  searchSupported = false;
  prefsStore = {};
  history = [];
  resetSearchSupport();
  resetHiding();
  activityCount = 1;
  tickersOn = false;
  dexOn = false;
  feeFlowOn = false;
  splitOn = false;
  launchPatch = {};
  previewExists = false;
  paramsOn = false;
  launchCount = 2;
  positions = null;
  candles = null;
  extraLaunches = [];
  store = null;
  locked = false;
  calls.length = 0;
  asked.length = 0;
  authorized.length = 0;
  answerDelay = 0;
  resetBitAssets();
  setTransport(fakeNode);
});

afterEach(() => {
  resetBitAssets();
  resetPositions();
  setTransport(null);
  dialogs.queue.splice(0);
  wallets.list = [];
  wallets.current = null;
  resetWalletData();
  nav.tab = '';
  nav.focus = null;
});

async function answer(choice: string, timeout = 1000) {
  await waitFor(() => expect(document.querySelector(`button[data-choice="${choice}"]`)).not.toBeNull());
  await waitFor(() => expect((document.querySelector(`button[data-choice="${choice}"]`) as HTMLButtonElement).disabled).toBe(false), { timeout });
  await fireEvent.click(document.querySelector(`button[data-choice="${choice}"]`)!);
}
/** The create wizard with no market chosen at first, as go('issue') opens it (Launch, its tab register). */
const renderIssue = () => {
  nav.tab = 'register';
  return render(Dex, { props: { page: 'launch' } });
};
const called = (method: string) => calls.filter((c) => c.method === method);
const loaded = () => waitFor(() => expect(ba.loaded && ba.assets.length > 0 && ba.mineState === 'ok').toBe(true));

describe('amounts, with the decimals of an asset', () => {
  it('parses and writes them exactly, as the chain does', () => {
    expect(parseUnits('12.5', 2)).toEqual({ ok: true, units: 1250n });
    expect(parseUnits('1,000', 0)).toEqual({ ok: true, units: 1000n });
    expect(parseUnits('0.001', 2).ok).toBe(false);
    expect(parseUnits('1.5', 0).ok).toBe(false);
    expect(parseUnits('0', 2).ok).toBe(false);
    expect(parseUnits('0', 2, { allowZero: true })).toEqual({ ok: true, units: 0n });
    expect(parseUnits('92233720368547758.08', 2).ok).toBe(false);
    expect(parseUnits('abc', 2).ok).toBe(false);
    expect(formatUnits(123456789n, 2)).toBe('1,234,567.89');
    expect(formatUnits(1200n, 2)).toBe('12');
    expect(formatUnits(1200n, 2, { trim: false, group: false })).toBe('12.00');
    expect(unitsOf(0.1, 8)).toBe(10_000_000n);
    expect(unitsOf(1e-8, 8)).toBe(1n);
    expect(unitsOf('5.5', 2)).toBe(550n);
    expect(formatPrice(0.000012345678)).toBe('0.0000123457');
    expect(formatPrice(1234.5)).toBe('1,234.5');
    expect(unitPrice(0.00001)).toEqual({ kind: 'buys', amount: '100,000' });
    expect(unitPrice(0.5)).toEqual({ kind: 'each', price: '0.5' });
    expect(lessSlippage(1000n, 0.5)).toBe(995n);
    expect(lessSlippage(1n, 50)).toBe(1n);
  });

  it('knows an asset by the hash of its name, written as the node writes it', async () => {
    const hex = Buffer.from(createHash('sha256').update('GOLD').digest()).reverse().toString('hex');
    expect(await hashName('GOLD')).toBe(hex);
    expect(assetArg(CHN_ID)).toBe('ECX');
    expect(assetArg(GOLD)).toBe(`0x${GOLD}`);
  });
});

describe('names and data, as the chain checks them', () => {
  it('refuses names that read as another asset, or are not plain', () => {
    expect(nameProblem('GOLD')).toBe('');
    expect(nameProblem('Gold bars 2026')).toBe('');
    expect(nameProblem('chn')).not.toBe('');
    expect(nameProblem('0xabc')).not.toBe('');
    expect(nameProblem('1739-0029')).not.toBe('');
    expect(nameProblem('0001-1739-0029')).not.toBe('');
    expect(nameProblem('1739-002')).toBe('');
    expect(nameProblem(' GOLD')).not.toBe('');
    expect(nameProblem('GÖLD')).not.toBe('');
    expect(nameProblem('x'.repeat(65))).not.toBe('');
  });

  it('checks each field of the data, and writes the changes as updateasset takes them', () => {
    expect(dataProblems({ commitment: id('e'), ipv4: '203.0.113.7:8333', ipv6: '[2001:db8::7]:80', encryptionkey: `02${id('1')}`, signingkey: id('2') })).toEqual({});
    const bad = dataProblems({ info: 'x'.repeat(513), commitment: 'ab', ipv4: '300.1.1.1', ipv6: '2001:db8', encryptionkey: `04${id('1')}`, signingkey: 'zz' });
    expect(Object.keys(bad).sort()).toEqual(['commitment', 'encryptionkey', 'info', 'ipv4', 'ipv6', 'signingkey']);
    expect(dataUpdates({ info: 'a', commitment: id('e') }, { info: 'b', commitment: '', ipv4: '1.2.3.4' })).toEqual({ info: 'b', commitment: null, ipv4: '1.2.3.4' });
    expect(dataUpdates({ info: 'a' }, { info: 'a' })).toEqual({});
  });
});

describe('the pools and the auctions, as the chain works them out', () => {
  it('swaps on a constant product, the fee left in the pool', () => {
    const out = swapOut(1_000_000n, 1_000_000n, 1000n);
    expect(out).toBe(996n);
    // What has to go in for an amount out buys at least that amount.
    const need = swapIn(1_000_000n, 1_000_000n, 996n)!;
    expect(swapOut(1_000_000n, 1_000_000n, need)).toBeGreaterThanOrEqual(996n);
    expect(swapIn(10n, 10n, 10n)).toBeNull();
    expect(priceImpact(1_000_000n, 1_000_000n, 1000n, out)).toBeCloseTo(0.4, 3);
  });

  it('gives shares for a deposit: a new pool, a pool with providers (its price taking only what it takes), a pool everyone left', () => {
    const opened = provide({ reserve0: 0n, reserve1: 0n, shares: 0n }, 1_000_000n, 1_000_000n)!;
    expect(opened).toEqual({ shares: 999_000n, take0: 1_000_000n, take1: 1_000_000n, opens: true });
    const pool = { reserve0: 1_000_000n, reserve1: 2_000_000n, shares: 1_414_213n };
    const d = provide(pool, 1000n, 5000n)!;
    expect(d.opens).toBe(false);
    expect(d.take0).toBe(1000n);
    expect(d.take1).toBe(2000n);
    expect(pairedAmount(1000n, 1_000_000n, 2_000_000n)).toBe(2000n);
    // ECX dust that would come back stays in the pool.
    expect(provide({ ...pool, chn1: true }, 1000n, 5000n)!.take1).toBe(5000n);
    expect(isAbandoned(1000n)).toBe(true);
    const reopened = provide({ reserve0: 10n, reserve1: 10n, shares: 1000n }, 1_000_000n, 1_000_000n)!;
    expect(reopened.opens).toBe(true);
    expect(withdraw({ reserve0: 1000n, reserve1: 100_000n, shares: 100n, chn0: true }, 10n)).toEqual([0n, 10_000n]);
  });

  it('prices an auction in a straight line, rounded for the seller, and what a bid buys', () => {
    const a = { baseAmount: 100n, remaining: 60n, startPrice: 1000n, endPrice: 500n, startHeight: 10, endHeight: 59 };
    expect(auctionPriceAt(a, 5)).toBe(1000n);
    expect(auctionPriceAt(a, 10)).toBe(1000n);
    expect(auctionPriceAt(a, 59)).toBe(500n);
    expect(auctionPriceAt(a, 34)).toBe(1000n - (500n * 24n) / 49n);
    expect(auctionBuysAt(a, 59, 50n)).toBe(10n);
    expect(auctionCostOfRemaining(a, 59)).toBe(300n);
    expect(receiptAction({ status: 'open', bids: 0 })).toBe('cancel');
    expect(receiptAction({ status: 'open', bids: 2 })).toBe('wait');
    expect(receiptAction({ status: 'ended', bids: 2 })).toBe('collect');
    const ok = { sameAsset: false, amount: parseUnits('1', 2), start: parseUnits('10', 8), end: parseUnits('5', 8), duration: 60, startIn: 1, sellsChn: false };
    expect(auctionProblems(ok)).toEqual([]);
    expect(auctionProblems({ ...ok, end: parseUnits('20', 8) })).toEqual(['The price falls: the end price is at most the start price.']);
    expect(auctionProblems({ ...ok, sameAsset: true, duration: 0, startIn: 60000 }).length).toBe(3);
    expect(auctionProblems({ ...ok, sellsChn: true, amount: parseUnits('0.0001', 8) }).length).toBe(1);
  });
});

describe('the journal of the app', () => {
  it('keeps the prices it saw, either way round, and reads back only what has its shape', () => {
    expect(notePrices(pools, 21, 1)).toBe(true);
    expect(notePrices(pools, 21, 2)).toBe(false);
    expect(notePrices([{ ...pools[0], price: 0.2 }], 22, 3)).toBe(true);
    expect(priceSeries(pools[0], GOLD).map((p) => p.p)).toEqual([0.1, 0.2]);
    expect(priceSeries(pools[0], CHN_ID).map((p) => p.p)).toEqual([10, 5]);
    const j = sanitizeJournal({ pending: [{ wallet: 'w', name: 'IRON', asset: id('f'), supply: '1', decimals: 0 }, { name: 3 }], prices: { x: { a: GOLD, points: [{ h: 1, t: 1, p: 1 }, 'bad'] } } });
    expect(j.pending.length).toBe(1);
    expect(j.pending[0].public).toBe(true);
    expect(j.prices.x.points.length).toBe(1);
    expect(sanitizeJournal('nonsense')).toEqual({ pending: [], funding: [], prices: {} });
  });
});

// Layers: the pages of BitAssets on Layers (its Staking first).
describe.runIf(CHAIN.name === 'Flush')('the pages of BitAssets', () => {
  it('are in the Chain section and the menus, before the tools, with their quick actions', () => {
    const own = (LOCAL_PAGES as readonly LocalPage[]).map((p) => p.id);
    // The menu of the DEX (owner decision, 2026-11-11): Portfolio, Assets, Swap, Pools, Launch, then Staking.
    expect(own).toEqual(['my-assets', 'assets', 'trade', 'pools', 'launch', 'staking', 'issue']);
    expect((LOCAL_PAGES as readonly LocalPage[]).slice(0, 5).map((p) => p.title)).toEqual(['Portfolio', 'Assets', 'Swap', 'Pools', 'Launch']);
    // Create asset (issue) is Launch's wizard: not in the navigation.
    const navPages = own.filter((id) => id !== 'issue');
    expect(NAV_SECTIONS.find((s) => s.title === 'Chain')!.pages).toEqual(['peg', ...navPages, 'explorer']);
    expect(AREAS.find((a) => a.id === 'chain')!.tabs.map((t) => t.page)).toEqual(['peg', ...navPages, 'explorer']);
    go('issue');
    expect([nav.page, nav.tab]).toEqual(['launch', 'register']);
    expect(chainBeforeTools(HOME_QUICK_ACTIONS.map((q) => q.page), (p) => p)).toBeNull();
    expect(HOME_QUICK_ACTIONS.map((q) => q.label)).toEqual(expect.arrayContaining(['Assets', 'Create asset', 'Swap']));
    const chain = menuSections(buildCommands()).find((s) => s.group === 'Chains')!;
    for (const c of LOCAL_COMMANDS) expect(chain.items.map((i) => i.label)).toContain(c.label);
    chain.items.find((c) => c.label === 'New pool…')!.run();
    expect([nav.page, nav.tab]).toEqual(['pools', 'new']);
    // Alt+6 opened Assets in bitassets-qt (audit port-2 #8); on Layers Alt+6 is Staking, Alt+7 Assets.
    expect(handleShortcut(new KeyboardEvent('keydown', { key: '7', altKey: true }))).toBe(true);
    expect(nav.page).toBe('assets');
    expect(CHAIN.uriScheme).toBe('flush');
  });

  it('retired entry points are gone: no page, command, quick action or home card of the order book, the auctions, the market index or Advanced (owner decision, 2026-11-11)', async () => {
    const retired = /auction|order book|market index|advanced|other pools|stable|weighted|concentrated|positions/i;
    for (const p of LOCAL_PAGES as readonly LocalPage[]) expect(`${p.id} ${p.title} ${p.summary}`).not.toMatch(retired);
    for (const c of LOCAL_COMMANDS) expect(`${c.id} ${c.label} ${c.tip ?? ''}`).not.toMatch(retired);
    for (const q of HOME_QUICK_ACTIONS) expect(q.label).not.toMatch(retired);
    const all = buildCommands();
    expect(all.filter((c) => c.group === 'Chains' && retired.test(c.label))).toEqual([]);
    expect(all.some((c) => /^(Auctions|Order book|Market index)/.test(c.label))).toBe(false);
    render(LocalCards);
    await loaded();
    expect(document.body.textContent).not.toMatch(/Auctions|Order book|Advanced/);
    cleanup();
    // Swap: the one form, no tabs, no Advanced link.
    render(Dex);
    await loaded();
    expect(screen.getByRole('heading', { name: 'Swap' })).toBeTruthy();
    expect(screen.queryByTestId('dex-advanced')).toBeNull();
    expect(screen.queryByRole('tablist')).toBeNull();
    expect(document.body.textContent).not.toMatch(/Advanced|Order book|Market index|Auctions/);
  });

  it('old tab ids and retired pages fall back on the nearest page kept, never an error (chain/local.ts dexRoute, RETIRED_PAGES)', async () => {
    // Swap's old Advanced tabs.
    expect(dexRoute('trade', 'markets')).toEqual({ page: 'pools', tab: 'pools' });
    expect(dexRoute('trade', 'classic')).toEqual({ page: 'pools', tab: 'pools' });
    expect(dexRoute('trade', 'liquidity')).toEqual({ page: 'pools', tab: 'pools' });
    expect(dexRoute('trade', 'create')).toEqual({ page: 'pools', tab: 'pools' });
    expect(dexRoute('trade', 'book')).toEqual({ page: 'trade', tab: 'swap' });
    expect(dexRoute('trade', 'auctions/sell')).toEqual({ page: 'my-assets', tab: 'closing' });
    expect(dexRoute('pools', 'positions')).toEqual({ page: 'my-assets', tab: 'closing' });
    // The kept ones stay, each on its page, whatever page was asked.
    expect(dexRoute('pools', `swap/get/${GOLD}`)).toEqual({ page: 'trade', tab: `swap/get/${GOLD}` });
    expect(dexRoute('trade', `pools/${id('p')}`)).toEqual({ page: 'pools', tab: `pools/${id('p')}` });
    expect(dexRoute('trade', 'new')).toEqual({ page: 'pools', tab: 'new' });
    expect(dexRoute('trade', 'fund')).toEqual({ page: 'launch', tab: 'fund' });
    expect(dexRoute('pools', 'launches')).toEqual({ page: 'launch', tab: 'launches' });
    expect(dexRoute('launch', 'create')).toEqual({ page: 'launch', tab: 'create' });
    // Nonsense: the page's own.
    expect(dexRoute('pools', 'no-such-tab/x')).toEqual({ page: 'pools', tab: '' });
    expect(dexRoute('nowhere', '')).toEqual({ page: 'trade', tab: '' });
    expect(RETIRED_PAGES.auctions).toEqual({ page: 'my-assets', tab: 'closing' });
    // In the app: Swap asked for the order book shows the swap; asked for Create pool, Pools.
    nav.tab = 'book';
    render(Dex);
    await loaded();
    await waitFor(() => expect([nav.page, nav.tab]).toEqual(['trade', 'swap']));
    expect(screen.getByRole('heading', { name: 'Swap' })).toBeTruthy();
    nav.tab = 'create';
    await waitFor(() => expect(screen.getByRole('heading', { name: 'Pools' })).toBeTruthy());
    expect([nav.page, nav.tab]).toEqual(['pools', 'pools']);
    nav.tab = 'auctions';
    await waitFor(() => expect(nav.page).toBe('my-assets'));
  });

  it('Assets: the cards, searched and filtered; an asset opened whole, its markets and the history of its data', async () => {
    render(Assets);
    await loaded();
    const list = await screen.findByRole('list', { name: 'Assets' });
    expect(within(list).getAllByRole('listitem').length).toBe(3);
    await fireEvent.click(screen.getByRole('button', { name: /^Dead/ }));
    await waitFor(() => expect(within(screen.getByRole('list', { name: 'Assets' })).getAllByRole('listitem').length).toBe(1));
    await fireEvent.click(screen.getByRole('button', { name: /^All/ }));
    await fireEvent.input(screen.getByRole('searchbox', { name: 'Find an asset' }), { target: { value: 'gram' } });
    await waitFor(() => expect(within(screen.getByRole('list', { name: 'Assets' })).getAllByRole('listitem').length).toBe(1));
    await fireEvent.click(screen.getByRole('list', { name: 'Assets' }).querySelector('button.tile')!);
    await waitFor(() => expect(screen.getByRole('heading', { name: '$GOLD #1739-0029' })).toBeTruthy());
    expect(called('getasset').some((c) => (c.params as unknown[])[0] === assetArg(GOLD))).toBe(true);
    expect(screen.getByText('One gram of gold', { selector: '.info span' })).toBeTruthy();
    await fireEvent.click(screen.getByRole('tab', { name: 'Changes' }));
    expect(screen.getByRole('table', { name: 'History of its data' })).toBeTruthy();
    expect(screen.getByText('You control it')).toBeTruthy();
    // A private name is found by its hash.
    await fireEvent.click(screen.getByRole('button', { name: /All assets/ }));
  });

  it('Assets: a dead asset is retired, after a warning', async () => {
    render(Assets);
    render(DialogHost);
    await loaded();
    nav.focus = { page: 'assets', key: PEBBLES };
    await waitFor(() => expect(screen.getByRole('heading', { name: 'This asset is dead' })).toBeTruthy());
    await fireEvent.click(screen.getByRole('button', { name: /Retire it/ }));
    await answer('yes');
    await waitFor(() => expect(called('releaseasset')[0]?.params).toEqual([assetArg(PEBBLES)]));
  });

  it('My assets: the controller adds a logo (picked, cropped, encoded), changes it and removes it', async () => {
    // A picture as the web view decodes it, and a canvas that draws it (jsdom has neither).
    const pixels = new Uint8ClampedArray(256 * 256 * 4).map((_, i) => (i % 4 === 3 ? 255 : i % 4 === 0 ? 240 : 40));
    const proto = HTMLCanvasElement.prototype as unknown as Record<string, unknown>;
    const saved = { getContext: proto.getContext, toBlob: proto.toBlob, bitmap: (globalThis as Record<string, unknown>).createImageBitmap };
    proto.getContext = () => ({ clearRect() {}, drawImage() {}, fillRect() {}, fillStyle: '', imageSmoothingEnabled: true, imageSmoothingQuality: 'high', getImageData: () => ({ data: pixels }) });
    proto.toBlob = (cb: (b: Blob | null) => void) => cb(null);
    (globalThis as Record<string, unknown>).createImageBitmap = async () => ({ width: 640, height: 480, close() {} });
    try {
      render(MyAssets);
      render(DialogHost);
      await loaded();
      const panel = await screen.findByRole('region', { name: 'Manage GOLD' });
      await fireEvent.click(within(panel).getByRole('button', { name: 'Add a logo' }));
      const save = within(panel).getByRole('button', { name: 'Save the logo' }) as HTMLButtonElement;
      expect(save.disabled).toBe(true);
      const input = panel.querySelector('#manage-logo-file') as HTMLInputElement;
      Object.defineProperty(input, 'files', { value: [new File([new Uint8Array([1, 2, 3])], 'logo.png', { type: 'image/png' })], configurable: true });
      await fireEvent.change(input);
      await waitFor(() => expect(panel.querySelector('[data-testid="logo-size"]')?.textContent).toMatch(/PNG, 1 colour:/), { timeout: 3000 });
      await waitFor(() => expect(save.disabled).toBe(false));
      // The logo as it will look, beside the asset as it is.
      expect(panel.querySelectorAll('.logo-before-after img').length).toBe(1);
      await fireEvent.click(save);
      await waitFor(() => expect(called('setassetlogo')).toHaveLength(1));
      const [arg, base64] = called('setassetlogo')[0].params as [string, string];
      expect(arg).toBe(assetArg(GOLD));
      const bytes = Uint8Array.from(atob(base64), (c) => c.charCodeAt(0));
      expect(checkLogo(bytes)).toEqual({ ok: true, format: 'png' });
      // Once listed with it: "Change its logo", and it can be removed.
      assets[0] = { ...assets[0], logo: { hash: 'f'.repeat(64), mime: 'image/png', size: bytes.length, height: 21, txid: id('7') } };
      await refreshBitAssets();
      await waitFor(() => expect(within(panel).queryByRole('button', { name: 'Change its logo' })).not.toBeNull());
      await fireEvent.click(within(panel).getByRole('button', { name: 'Change its logo' }));
      await fireEvent.click(await within(panel).findByRole('button', { name: 'Remove the logo' }));
      await waitFor(() => expect(called('setassetlogo')[1]?.params).toEqual([assetArg(GOLD), '']));
    } finally {
      proto.getContext = saved.getContext;
      proto.toBlob = saved.toBlob;
      (globalThis as Record<string, unknown>).createImageBitmap = saved.bitmap;
    }
  });

  it('My assets: mints, changes the data and burns an asset it controls', async () => {
    render(MyAssets);
    render(DialogHost);
    await loaded();
    const panel = await screen.findByRole('region', { name: 'Manage GOLD' });
    await fireEvent.click(within(panel).getByRole('button', { name: 'Mint more' }));
    await fireEvent.input(panel.querySelector('#mint-amount')!, { target: { value: '10.5' } });
    await fireEvent.click(within(panel).getByRole('button', { name: 'Mint' }));
    await answer('yes');
    await waitFor(() => expect(called('mintasset')[0]?.params).toEqual([assetArg(GOLD), '10.5']));
    // Done: the form closes (the host confirmed and ran it).
    await waitFor(() => expect(panel.querySelector('#mint-amount')).toBeNull());

    await fireEvent.click(within(panel).getByRole('button', { name: 'Change its data' }));
    await waitFor(() => expect((panel.querySelector('#data-info') as HTMLTextAreaElement).value).toBe('One gram of gold'));
    await fireEvent.input(panel.querySelector('#data-info')!, { target: { value: 'One gram of gold, in Zurich' } });
    await fireEvent.input(panel.querySelector('#data-commitment')!, { target: { value: '' } });
    await fireEvent.input(panel.querySelector('#data-ipv4')!, { target: { value: '300.1.1.1' } });
    expect(within(panel).getByRole('button', { name: 'Save' })).toHaveProperty('disabled', true);
    await fireEvent.input(panel.querySelector('#data-ipv4')!, { target: { value: '203.0.113.7' } });
    await fireEvent.click(within(panel).getByRole('button', { name: 'Save' }));
    await answer('yes');
    await waitFor(() => expect(called('updateasset')[0]?.params).toEqual([assetArg(GOLD), { info: 'One gram of gold, in Zurich', commitment: null, ipv4: '203.0.113.7' }]));
    await waitFor(() => expect(panel.querySelector('#data-info')).toBeNull());

    await fireEvent.click(within(panel).getByRole('button', { name: 'Burn some' }));
    await fireEvent.input(panel.querySelector('#burn-amount')!, { target: { value: '500' } });
    expect(within(panel).getByText('More than this wallet holds.')).toBeTruthy();
    await fireEvent.input(panel.querySelector('#burn-amount')!, { target: { value: '5' } });
    await fireEvent.click(within(panel).getByRole('button', { name: 'Burn' }));
    await answer('yes');
    await waitFor(() => expect(called('burnasset')[0]?.params).toEqual([assetArg(GOLD), '5']));
    await waitFor(() => expect(panel.querySelector('#burn-amount')).toBeNull());

    await fireEvent.click(within(panel).getByRole('button', { name: 'Control and supply' }));
    await fireEvent.click(within(panel).getByRole('button', { name: 'Fix the supply for good' }));
    await answer('yes');
    await waitFor(() => expect(called('fixassetsupply')[0]?.params).toEqual([assetArg(GOLD), true]));
  });

  it('My assets: pool shares taken out, a receipt, the activity; a locked wallet shows them once unlocked', async () => {
    render(MyAssets);
    render(DialogHost);
    await loaded();
    nav.tab = 'liquidity';
    await fireEvent.click(await screen.findByRole('button', { name: 'Take out…' }));
    await fireEvent.click(screen.getByRole('button', { name: '50%' }));
    await fireEvent.click(screen.getByRole('button', { name: 'Take out' }));
    await answer('yes');
    await waitFor(() => expect(called('removeliquidity')[0]?.params).toEqual([assetArg(GOLD), 'ECX', 50, 1]));
    nav.tab = 'auctions';
    await waitFor(() => expect(screen.getByText(/It can be collected once it ends/)).toBeTruthy());
    nav.tab = 'activity';
    await waitFor(() => expect(screen.getByRole('table', { name: 'Activity' })).toBeTruthy());
    expect(screen.getByText('Minted 10.00 GOLD')).toBeTruthy();
  });

  it('a locked wallet: its assets once unlocked for that alone', async () => {
    locked = true;
    wallets.list = [{ name: 'w', encrypted: true, locked: true, privateKeysDisabled: false, hd: true }];
    render(MyAssets);
    await waitFor(() => expect(ba.mineState).toBe('locked'));
    expect(screen.getByRole('button', { name: /Show my assets/ })).toBeTruthy();
  });

  it('Issue: the wizard reserves a free name, journals what to register, and it is registered by itself once deep enough', async () => {
    renderIssue();
    render(DialogHost);
    await waitFor(() => expect(ba.loaded).toBe(true));
    const name = screen.getByLabelText('Its name');
    await fireEvent.input(name, { target: { value: 'GOLD' } });
    await waitFor(() => expect(screen.getByText(/is taken/)).toBeTruthy());
    await fireEvent.input(name, { target: { value: '1739-0029' } });
    expect(screen.getByText(/does not read as the number of an asset/)).toBeTruthy();
    await fireEvent.input(name, { target: { value: 'COPPER' } });
    await waitFor(() => expect(screen.getByText(/is free/)).toBeTruthy());
    // One wizard (user decision, 2026-10-22): what it is (the description with the name), the supply, the market, the review.
    await fireEvent.input(document.querySelector('#issue-info')!, { target: { value: 'Copper, by the gram' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    await fireEvent.input(document.querySelector('#issue-supply')!, { target: { value: '1000.25' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    // The market: none by default for "Register an asset".
    expect(screen.getByRole('radio', { name: /No market/ }).getAttribute('aria-checked')).toBe('true');
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    await waitFor(() => expect(screen.getByRole('heading', { name: 'Review' })).toBeTruthy());
    await fireEvent.click(screen.getByRole('button', { name: /Reserve and issue/ }));
    await waitFor(() => expect(called('reserveasset')[0]?.params).toEqual(['COPPER']));
    await waitFor(() => expect(screen.getByRole('heading', { name: /COPPER is reserved/ })).toBeTruthy());
    expect(store).toMatchObject({ pending: [{ wallet: 'w', name: 'COPPER', asset: 'f'.repeat(64), supply: '1000.25', decimals: 8, data: { info: 'Copper, by the gram' }, public: true }] });
    // One question for both steps: the reservation and the registration exactly as it will be sent.
    expect(asked).toEqual(['reserveasset+registerasset']);
    // Deep enough: registered by itself, without a second question.
    mine.reservations.push({ txid: id('6'), name: 'COPPER', taken: false, height: 21, registers_from: 23, wait: 0 });
    await refreshBitAssets();
    await waitFor(() => expect(called('registerasset').length).toBe(1));
    expect(called('registerasset')[0].params).toEqual(['COPPER', '1000.25', 8, { info: 'Copper, by the gram' }, true]);
    expect(asked).toEqual(['reserveasset+registerasset']);
    expect(authorized).toEqual([]);
  });

  it('a registration with no authorization (the app started again): its card asks the user, never a dialog by itself', async () => {
    store = { pending: [{ wallet: 'w', name: 'TIN', asset: id('f'), supply: '5', decimals: 0, data: {}, public: true, time: 1, reservation: id('6') }], prices: {} };
    mine.reservations.push({ txid: id('6'), name: 'TIN', taken: false, height: 15, registers_from: 17, wait: 0 });
    render(MyAssets);
    render(DialogHost);
    nav.tab = 'issuing';
    await waitFor(() => expect(screen.getByRole('button', { name: 'Confirm registration' })).toBeTruthy());
    expect(asked).toEqual([]);
    expect(called('registerasset')).toEqual([]);
    expect(pendingList().find((o) => o.name === 'TIN')?.stage).toBe('confirm');
    await fireEvent.click(screen.getByRole('button', { name: 'Confirm registration' }));
    await waitFor(() => expect(called('registerasset').length).toBe(1));
    expect(asked).toEqual(['registerasset']);
    await waitFor(() => expect(pendingList().find((o) => o.name === 'TIN')?.stage).toBe('registering'));
  });

  it('the pages go on while a question of the host waits (a slow dialog, a slow host): the refresh is never held', async () => {
    answerDelay = 1500;
    renderIssue();
    await waitFor(() => expect(ba.loaded).toBe(true));
    await fireEvent.input(screen.getByLabelText('Its name'), { target: { value: 'ZINC' } });
    await waitFor(() => expect(screen.getByText(/is free/)).toBeTruthy());
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    await fireEvent.input(document.querySelector('#issue-supply')!, { target: { value: '7' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    await fireEvent.click(await screen.findByRole('button', { name: /Reserve and issue/ }));
    // The question shows: the page says it waits for it, and the lists are still asked again, at once.
    await waitFor(() => expect(screen.getByRole('button', { name: /Waiting for your confirmation/ })).toBeTruthy());
    const version = ba.version;
    const t0 = Date.now();
    await refreshBitAssets();
    expect(Date.now() - t0).toBeLessThan(500);
    expect(ba.version).toBeGreaterThan(version);
    await waitFor(() => expect(screen.getByRole('heading', { name: /ZINC is reserved/ })).toBeTruthy(), { timeout: 4000 });
    // Its progress on the page, and in the top bar.
    expect(document.querySelector('.pending-card')).toBeTruthy();
    expect(pendingList().find((o) => o.name === 'ZINC')?.stage).toBe('reserved');
  });

  it('Swap: quoted by the node, confirmed with its least', async () => {
    render(Dex);
    render(DialogHost);
    await loaded();
    await waitFor(() => expect((document.querySelector('#swap-get') as HTMLElement).dataset.value).toBe(GOLD));
    await fireEvent.input(document.querySelector('#swap-amount')!, { target: { value: '0.5' } });
    await waitFor(() => expect(called('quoteswap').length).toBeGreaterThan(0));
    await waitFor(() => expect(screen.getByText('At least')).toBeTruthy());
    await fireEvent.click(screen.getByRole('button', { name: '1%' }));
    await fireEvent.click(screen.getByRole('button', { name: /^Swap$/ }));
    // The asset named with its number first: a look-alike label (G0LD) is told apart (audit port-2 #5), and a
    // label holding another number reads as text (audit 7 E-BA4).
    await waitFor(() => expect(document.querySelector('dialog[open]')?.textContent).toContain('$GOLD #1739-0029'));
    // The least confirmed is the least signed (min_out, audit 7 E-BA2).
    expect(document.querySelector('dialog[open]')?.textContent).toContain('At least 4.88 $GOLD #1739-0029');
    await answer('yes');
    await waitFor(() => expect(called('swapasset')[0]?.params).toEqual(['ECX', '0.5', assetArg(GOLD), 1, 10, '4.88']));
    // Done (the host confirmed and ran it): the form is cleared.
    await waitFor(() => expect((document.querySelector('#swap-amount') as HTMLInputElement).value).toBe(''));
    // A larger trade moves the price more than 2%: asked about first.
    await fireEvent.input(document.querySelector('#swap-amount')!, { target: { value: '2' } });
    await waitFor(() => expect(called('quoteswap').some((c) => (c.params as unknown[])[1] === '2')).toBe(true));
    await waitFor(() => expect(screen.getByRole('button', { name: /^Swap$/ })).toHaveProperty('disabled', false));
    await fireEvent.click(screen.getByRole('button', { name: /^Swap$/ }));
    await waitFor(() => expect(screen.getByRole('heading', { name: 'Price impact' })).toBeTruthy());
    await answer('no');
    expect(called('swapasset').length).toBe(1);
  });

  it('Send: an asset chosen at the top of the page, sent with sendasset', async () => {
    sendIntent.asset = GOLD;
    render(LocalSend);
    render(DialogHost);
    await loaded();
    await waitFor(() => expect(document.querySelector('#asset-amount')).not.toBeNull());
    await fireEvent.input(document.querySelector('#asset-to')!, { target: { value: OTHER } });
    await fireEvent.input(document.querySelector('#asset-amount')!, { target: { value: '12.5' } });
    const send = screen.getByRole('button', { name: /^Send$/ });
    await waitFor(() => expect(send).toHaveProperty('disabled', false), { timeout: 3000 });
    await fireEvent.click(send);
    await waitFor(() => expect(document.querySelector('dialog[open]')?.textContent).toContain('Send 12.5 $GOLD #1739-0029 to this address?'));
    await answer('yes', 4000);
    await waitFor(() => expect(called('sendasset')[0]?.params).toEqual([OTHER, assetArg(GOLD), '12.5']));
  });

  it('pools and auctions are read a page at a time, with more on demand (audit 7 E-BA3)', async () => {
    auctions = Array.from({ length: 250 }, (_, i) => ({ ...auctions[0], auction: (i + 0x1000).toString(16).padStart(64, '0') }));
    render(MyAssets);
    await loaded();
    expect(ba.auctions.length).toBe(200);
    expect(called('listauctions').every((c) => (c.params as unknown[])[0] === false && Number((c.params as unknown[])[1]) <= 1000)).toBe(true);
    // A receipt of an auction not in the page read (the fake node's getauction knows it): asked for by itself.
    mine.receipts = [{ auction: id('e') }];
    await refreshBitAssets();
    await waitFor(() => expect(ba.receiptAuctions.map((a) => a.auction)).toEqual([id('e')]));
    // The wallet's part every few seconds; the lists of the chain only with a block.
    const before = called('listpools').length;
    await refreshBitAssets({ node: false });
    expect(called('listpools').length).toBe(before);
    expect(called('listmyassets').length).toBeGreaterThan(1);
  });

  it('a registration dropped from the mempool is sent again (audit 7 E-BA7)', async () => {
    store = { pending: [{ wallet: 'w', name: 'IRON', asset: id('f'), supply: '1', decimals: 0, data: {}, public: true, time: 1, reservation: id('r'), registration: id('6'), registeredAt: 18 }], prices: {} };
    // Accepted with its reservation: the host sends it again without a question, as its transaction left the mempool.
    authorized.push(JSON.stringify(['w', 'registerasset', ['IRON', '1', 0, {}, true]]));
    await refreshBitAssets();
    // Two blocks on (21 >= 18 + 2), not in the mempool (the fake node knows no getmempoolentry): registered again.
    await waitFor(() => expect(called('registerasset').length).toBe(1));
    expect(called('getmempoolentry')[0].params).toEqual([id('6')]);
    expect(ba.journal.pending[0].registration).toBe(id('7'));
    expect(ba.journal.pending[0].registeredAt).toBe(21);
    expect(asked).toEqual([]);
  });

  it('names an asset in a confirmation by its label and number, or the start of its hash (audit port-2 #5)', async () => {
    await refreshBitAssets();
    expect(nameOf(GOLD)).toBe('$GOLD #1739-0029');
    expect(nameOf(CHN_ID)).toBe('ECX');
    // Not listed (a private name): the start and end of its hash.
    expect(nameOf('9'.repeat(60) + 'abcd', 'G0LD')).toBe('$G0LD 0x99999999…9999abcd');
    // A name of another shape, quoted and escaped after the unique part: it reads as text (audit 7 E-BA4).
    expect(nameOf('9'.repeat(60) + 'abcd', 'gold (#1739-0029)')).toBe('0x99999999…9999abcd ("gold (\\#1739-0029)")');
    // A display name that imitates another asset (security review 2026-10-24): after the real ticker and number, escaped.
    assets[0] = { ...assets[0], display_name: 'Bitcoin $ECX #1' };
    await refreshBitAssets();
    expect(nameOf(GOLD)).toBe('$GOLD #1739-0029 ("Bitcoin \\$ECX \\#1")');
    assets[0] = { ...assets[0], display_name: '"FLUSH"' };
    await refreshBitAssets();
    expect(nameOf(GOLD)).toBe('$GOLD #1739-0029 ("\\"FLUSH\\"")');
    // A label outside the rules of tickers is never $TICKER there; two ids alike in their first 8 hex differ.
    expect(nameOf('1a2b3c4d' + '0'.repeat(48) + '11111111', 'ECX"')).toBe('0x1a2b3c4d…11111111 ("ECX\\"")');
    expect(nameOf('1a2b3c4d' + '0'.repeat(48) + '22222222', '\u0415CX')).toBe('0x1a2b3c4d…22222222 ("\u0415CX")');
    expect(nameOf('8'.repeat(64))).toBe('0x88888888…88888888');
  });

  it('Browse assets: a page at a time from the node, never the whole list, the newest first (user feedback 2026-10-19; audit port-2 #4)', async () => {
    assets = Array.from({ length: 1025 }, (_, i) => asset(i.toString(16).padStart(64, '0').replace(/^0/, '7'), { label: `F${String(i + 1).padStart(4, '0')}`, name: `F${i + 1}`, seq: `${1000 + i}-0001`, registered: i + 1 }));
    // A node without searchassets: the store keeps the newest ASSET_PAGE, the page lists the newest page of listassets.
    const view = render(Assets);
    await waitFor(() => expect(ba.assets.length).toBe(ASSET_PAGE), { timeout: 10000 });
    expect(called('listassets')[0].params).toEqual([ASSET_PAGE, 1025 - ASSET_PAGE]);
    expect(ba.assets[ba.assets.length - 1].label).toBe('F1025');
    const tiles = () => within(screen.getByRole('list', { name: 'Assets' })).getAllByRole('listitem');
    await waitFor(() => expect(tiles().length).toBe(100));
    expect(called('listassets').every((c) => (c.params as number[])[0] <= ASSET_PAGE)).toBe(true);
    expect(tiles()[0].textContent).toContain('F1025');
    expect(screen.getByText(/This node lists 200 assets at a time/)).toBeTruthy();
    // Typing asks the node (its match), a moment after.
    await fireEvent.input(screen.getByRole('searchbox', { name: 'Find an asset' }), { target: { value: 'F0003' } });
    await waitFor(() => expect(called('listassets').some((c) => (c.params as unknown[])[2] === 'F0003')).toBe(true));
    await waitFor(() => expect(tiles().length).toBe(1));
    view.unmount();
    // A node with searchassets: pages by its cursor, as the end of the list comes or on "Show more"; what is typed is its.
    searchSupported = true;
    resetSearchSupport();
    hidden = [asset('6'.repeat(64), { label: 'ZINC', name: 'ZINC', seq: '5555-0001', registered: 2000 })];
    calls.length = 0;
    render(Assets);
    await waitFor(() => expect(tiles().length).toBe(100), { timeout: 10000 });
    expect(tiles()[0].textContent).toContain('ZINC');
    await fireEvent.click(screen.getByRole('button', { name: 'Show more' }));
    await waitFor(() => expect(tiles().length).toBe(200));
    expect(called('searchassets').map((c) => (c.params as unknown[])[3])).toEqual([null, '100']);
    expect(called('searchassets').every((c) => ((c.params as unknown[])[4] as number) <= 200)).toBe(true);
    await fireEvent.input(screen.getByRole('searchbox', { name: 'Find an asset' }), { target: { value: 'zin' } });
    await waitFor(() => expect(tiles().length).toBe(1));
    expect(screen.getByRole('list', { name: 'Assets' }).textContent).toContain('$ZINC');
  }, 30000);

  it('Browse assets, Held or controlled: the wallet\'s searchmyassets with the same arguments, never mine to searchassets', async () => {
    searchSupported = true;
    resetSearchSupport();
    render(Assets);
    await loaded();
    const tiles = () => within(screen.getByRole('list', { name: 'Assets' })).queryAllByRole('listitem');
    await waitFor(() => expect(called('searchassets').length).toBeGreaterThan(0));
    calls.length = 0;
    await fireEvent.click(screen.getByRole('button', { name: /Held or controlled/ }));
    await waitFor(() => expect(called('searchmyassets').length).toBe(1));
    const c = called('searchmyassets')[0];
    expect(c.wallet).toBe('w');
    expect((c.params as unknown[]).length).toBe(5);
    expect(called('searchassets').length).toBe(0);
    const held = new Set(mine.assets.map((h) => h.asset));
    await waitFor(() => expect(tiles().length).toBe(held.size));
    expect(calls.every((x) => !JSON.stringify(x.params ?? []).includes('"mine"'))).toBe(true);
  });

  it('My assets: older activity read on asking; tables are cards when narrow; no faint text under 4.5:1 (audit port-2 #13)', async () => {
    activityCount = 450;
    render(MyAssets);
    await loaded();
    nav.tab = 'activity';
    await waitFor(() => expect(within(screen.getByRole('table', { name: 'Activity' })).getAllByRole('row').length).toBe(ACTIVITY_PAGE + 1));
    await fireEvent.click(screen.getByRole('button', { name: 'Show older activity' }));
    await waitFor(() => expect(baView.activity).toBe(2 * ACTIVITY_PAGE));
    await waitFor(() => expect(within(screen.getByRole('table', { name: 'Activity' })).getAllByRole('row').length).toBe(2 * ACTIVITY_PAGE + 1));
    const css = (await import('node:fs')).readFileSync(`${__dirname}/ba/ba.css`, 'utf8');
    expect(css).toMatch(/\.ba-wrap \{[^}]*container-type: inline-size/);
    expect(css).toMatch(/@container \(max-width: 56rem\) \{[\s\S]*\.ba-table:not\(\.ba-nostack\) thead \{\s*display: none;/);
    expect(css).toMatch(/\.ba-page \.faint \{\s*color: var\(--text-muted\);/);
  });

  it('My assets: My launches, the allocation claimable at graduation with the progress, Claim once graduated (allocation and fees)', async () => {
    dexOn = true;
    render(MyAssets);
    render(DialogHost);
    await loaded();
    const panel = await waitFor(() => screen.getByTestId('my-launches'));
    expect(called('getaddressesbylabel').at(-1)?.params).toEqual(['launch creator']);
    const rows = within(panel).getAllByTestId('my-launch');
    const moon = rows.find((r) => r.textContent?.includes('MOON'))!;
    const star = rows.find((r) => r.textContent?.includes('STAR'))!;
    // A row as the holdings': the asset (its identity), the allocation, the curve, what is claimable, Claim.
    const cell = (row: HTMLElement, c: string) => row.querySelector(`.${c}`)?.textContent?.replace(/\s+/g, ' ').trim() ?? '';
    expect(moon.querySelector('.c-asset .badge-asset .asset-name')?.textContent).toBe('To the moon $MOON');
    // On its curve: the allocation and its amount; the progress; fees owed: claimable (the fees alone), Claim.
    expect(cell(moon, 'c-alloc')).toMatch(/^Allocation 2% 20M MOON$/);
    expect(cell(moon, 'c-curve')).toMatch(/^Curve 40/);
    expect(cell(moon, 'c-claim')).toMatch(/^Claimable [\d.,]+ ECX$/);
    expect(within(moon).getByRole('button', { name: /Claim/ })).toBeTruthy();
    // Graduated, not minted: the allocation claimable now, and Claim sends claimlaunch.
    expect(cell(star, 'c-alloc')).toMatch(/^Allocation 2% /);
    expect(cell(star, 'c-curve')).toBe('Curve graduated');
    expect(cell(star, 'c-claim')).toBe('Claimable 20.4M STAR');
    await fireEvent.click(within(star).getByRole('button', { name: /Claim/ }));
    await answer('yes');
    await waitFor(() => expect(called('claimlaunch').at(-1)?.params).toEqual([assetArg(STAR)]));
  });

  it('My launches: a curve never traded (6 decimals, 210M) and one traded (4 decimals), each clean in the holdings\' layout (user report 2026-10-24)', async () => {
    dexOn = true;
    const CORN = 'c0'.repeat(32);
    const CAT = 'ca'.repeat(32);
    const base = { ...launchOf(MOON), info: undefined, has_logo: false, creator_fees_owed: '0.00000000', allocation: 2, allocation_claimed: false, status: 'curve', graduated_height: undefined };
    // As the PC node's listlaunches gives them on betanet (2026-10-24), with no buys yet: progress 0, no trades.
    const CORN_LAUNCH = { ticker: 'CORN', display_name: 'ECASHCORN', height: 3997, status: 'curve', raised: 0, graduation: 500, progress: 0, price: 1.058201058201e-06, market_cap: 222.22222222, supply: 210000000, decimals: 6, curve_supply: 157500000, pool_supply: 52500000, curve_tokens_left: 157500000, fee: 1.15, allocation: 0.2, allocation_claimed: false, min_buy: 0, max_buy: 0, volume: 0, trades: 0, creator_fees_owed: 0 };
    const RUG_LAUNCH = { ticker: 'RUG', display_name: 'Rugging', status: 'curve', raised: 0, graduation: 500, progress: 0, price: 0.0001481481481481, market_cap: 222.22222222, supply: 1500000, decimals: 6, fee: 5, allocation: 10, volume: 0, trades: 0 };
    const RUG = 'd1'.repeat(32);
    // The fields the node gives besides (its token, creator, pool), and those RUG's listing left out, as the node has them.
    extraLaunches = [
      { ...base, ...CORN_LAUNCH, token: CORN, creator: ME },
      { ...base, curve_supply: undefined, pool_supply: undefined, creator_fees_owed: 0, allocation_claimed: false, ...RUG_LAUNCH, token: RUG, creator: ME },
      { ...base, token: CAT, ticker: 'SRSCAT', display_name: 'Serious Cat', decimals: 4, supply: '1000000.0000', curve_supply: undefined, pool_supply: undefined, progress: 12.5, price: 0.0001, volume: '10.00000000', raised: '62.50000000', trades: 3 },
    ];
    render(MyAssets);
    await loaded();
    const panel = await waitFor(() => screen.getByTestId('my-launches'));
    const row = (t: string) => within(panel).getAllByTestId('my-launch').find((r) => r.textContent?.includes(t))!;
    await waitFor(() => expect(row('CORN')).toBeTruthy());
    const cell = (r: HTMLElement, c: string) => r.querySelector(`.${c}`)?.textContent?.replace(/\s+/g, ' ').trim() ?? '';
    for (const r of [row('CORN'), row('RUG'), row('SRSCAT')]) {
      expect(r.textContent).not.toMatch(/NaN|Infinity|undefined|null/);
      // The shared identity, in the holdings' row.
      expect(r.querySelector('.c-asset .badge-asset .asset-name .ident')).not.toBeNull();
      expect(r.classList.contains('ml-row')).toBe(true);
    }
    // Stacked: the ticker and number under the name, no bar before them.
    expect(row('CORN').querySelector('.asset-name')?.classList.contains('stacked')).toBe(true);
    expect(row('CORN').querySelector('.asset-name')?.textContent).toMatch(/^ECASHCORN \$CORN /);
    // Never traded: the allocation compact, an empty bar, "No trades yet", the curve's start price.
    // Its allocation kept (0.2% of 210M), compact; its start price as the node has it, about 1.06e-06 ECX.
    expect(cell(row('CORN'), 'c-alloc')).toBe('Allocation 0.2% 420k CORN');
    expect(cell(row('CORN'), 'c-curve')).toBe('Curve No trades yet start price 0.00000106 ECX');
    expect(cell(row('RUG'), 'c-alloc')).toBe('Allocation 10% 150k RUG');
    expect(cell(row('RUG'), 'c-curve')).toBe('Curve No trades yet start price 0.000148 ECX');
    expect(cell(row('RUG'), 'c-claim')).toBe('Claimable at graduation');
    expect((row('CORN').querySelector('.c-curve .ba-bar > span') as HTMLElement).style.width).toMatch(/^0(%|px)?$/);
    expect(cell(row('CORN'), 'c-claim')).toBe('Claimable at graduation');
    // Traded: its progress, no start price; 4 decimals compact.
    expect(cell(row('SRSCAT'), 'c-alloc')).toBe('Allocation 2% 20k SRSCAT');
    expect(cell(row('SRSCAT'), 'c-curve')).toMatch(/^Curve 12\.5/);
    expect(row('SRSCAT').querySelector('[data-testid="start-price"]')).toBeNull();
  });

  it('v6: a launch of 100 and one of 500 side by side, each its own curve and prices (My launches)', async () => {
    dexOn = true;
    const NEW = 'e1'.repeat(32);
    const OLD = 'e2'.repeat(32);
    const base = { ...launchOf(MOON), info: undefined, has_logo: false, creator_fees_owed: 0, allocation: 1, allocation_claimed: false, status: 'curve', graduated_height: undefined, curve_supply: undefined, pool_supply: undefined, decimals: 6, supply: 1000000000, price: undefined, volume: 0, trades: 0, raised: 0, progress: 0 };
    extraLaunches = [
      { ...base, token: NEW, ticker: 'NEWB', display_name: 'New at 100', graduation: 100 },
      { ...base, token: OLD, ticker: 'OLDB', display_name: 'Old at 500', graduation: 500 },
    ];
    render(MyAssets);
    await loaded();
    const panel = await waitFor(() => screen.getByTestId('my-launches'));
    const row = (t: string) => within(panel).getAllByTestId('my-launch').find((r) => r.textContent?.includes(t))!;
    await waitFor(() => expect(row('NEWB')).toBeTruthy());
    // The curve's start price of each: 50 / 1.125e9 and 250 / 1.125e9.
    expect(row('NEWB').querySelector('[data-testid="start-price"]')?.textContent).toBe('start price 0.0000000444 ECX');
    expect(row('OLDB').querySelector('[data-testid="start-price"]')?.textContent).toBe('start price 0.000000222 ECX');
  });

  it('the cards of the home: holdings, markets', async () => {
    render(LocalCards);
    await loaded();
    await waitFor(() => expect(screen.getByRole('heading', { name: 'Portfolio' })).toBeTruthy());
    expect(screen.getByRole('heading', { name: 'Markets' })).toBeTruthy();
    expect(screen.queryByRole('heading', { name: 'Auctions' })).toBeNull();
    // 200 GOLD at 0.1 ECX: its worth in its row of My assets (no totals: user feedback 2026-10-19).
    await waitFor(() => expect(document.querySelector('[data-testid="held-row"] .c-value')?.textContent).toBe('20'));
  });

  it('the assets the wallet holds, on the Home and the Overview: by worth, with their numbers and balances', async () => {
    for (const which of ['home', 'overview']) {
      const shown = which === 'home' ? render(LocalCards) : render(LocalOverview);
      await loaded();
      const held = await waitFor(() => screen.getByTestId('held-assets'));
      // On the Home the table is the My assets card (one card, user feedback 2026-10-19); Your assets on the Overview.
      expect(within(held).getByRole('heading', { name: which === 'home' ? 'Portfolio' : 'Your assets' })).toBeTruthy();
      if (which === 'home') {
        expect(screen.getAllByRole('heading', { name: 'Portfolio' })).toHaveLength(1);
        expect(screen.queryByText('Assets held')).toBeNull();
        expect(screen.queryByText('Being issued')).toBeNull();
      }
      const rows = [...held.querySelectorAll('tbody tr')].map((r) => r.textContent ?? '');
      // GOLD first (a pool prices it: 200 at 0.1 ECX), then SILVER (no price: –).
      expect(rows[0]).toContain('GOLD');
      expect(rows[0]).toContain('#1739-0029');
      expect(rows[0]).toContain('200');
      expect(rows[0]).toContain('20');
      expect(rows[1]).toContain('SILVER');
      expect(rows[1]).toContain('–');
      shown.unmount();
    }
  });

  it('no assets yet: the way to get some', async () => {
    mine = { assets: [], reservations: [], liquidity: [], receipts: [] };
    render(LocalOverview);
    await waitFor(() => expect(ba.mineState).toBe('ok'));
    const held = await waitFor(() => screen.getByTestId('held-assets'));
    await waitFor(() => expect(within(held).getByRole('button', { name: 'Create asset' })).toBeTruthy());
    expect(within(held).getByRole('button', { name: 'Get assets in Swap' })).toBeTruthy();
  });

  it('Launch: the one create wizard, a curve of its own supply, decimals and fee (launchparams), launched by itself once deep enough', async () => {
    dexOn = true;
    paramsOn = true;
    tickersOn = true;
    nav.tab = 'launch';
    render(Dex);
    render(DialogHost);
    await loaded();
    const name = (await screen.findByLabelText('Ticker')) as HTMLInputElement;
    await fireEvent.input(name, { target: { value: 'COPPER' } });
    await waitFor(() => expect(screen.getByText(/is free/)).toBeTruthy());
    await fireEvent.input(document.querySelector('#issue-info')!, { target: { value: 'Copper on a curve' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    // The rules' defaults first: a billion of 6 decimals.
    await waitFor(() => expect((document.querySelector('#issue-supply') as HTMLInputElement).value).toBe('1000000000'));
    // Too few units for a curve, then a supply that is no multiple of 8 units.
    await fireEvent.input(document.querySelector('#issue-decimals')!, { target: { value: '0' } });
    await fireEvent.input(document.querySelector('#issue-supply')!, { target: { value: '1000' } });
    await waitFor(() => expect(screen.getByTestId('curve-supply-problem').textContent).toMatch(/1,000,000 units at least/));
    await fireEvent.input(document.querySelector('#issue-supply')!, { target: { value: '1000001' } });
    await waitFor(() => expect(screen.getByTestId('curve-supply-problem').textContent).toMatch(/multiple of 8 units/));
    expect((screen.getByRole('button', { name: 'Next' }) as HTMLButtonElement).disabled).toBe(true);
    await fireEvent.input(document.querySelector('#issue-decimals')!, { target: { value: '8' } });
    await fireEvent.input(document.querySelector('#issue-supply')!, { target: { value: '21000000' } });
    await waitFor(() => expect(screen.queryByTestId('curve-supply-problem')).toBeNull());
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    expect(screen.getByRole('radio', { name: /Launch on a curve/ }).getAttribute('aria-checked')).toBe('true');
    await fireEvent.input(document.querySelector('#launch-fee')!, { target: { value: '250' } });
    await waitFor(() => expect(screen.getByLabelText(/Trade fee on the curve: 2\.50%/)).toBeTruthy());
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    const review = await screen.findByTestId('launch-review');
    expect(review.textContent).toContain('21,000,000, fixed');
    expect(review.textContent).toContain('Decimals8 (set for good');
    expect(review.textContent).toMatch(/2\.5% of each trade: 80% to you, 20% to the chain's security/);
    await fireEvent.click(screen.getByRole('button', { name: /Reserve and launch/ }));
    await waitFor(() => expect(called('reserveasset')[0]?.params).toEqual(['COPPER']));
    // One question for both steps: the reservation and the launch exactly as it will be sent.
    expect(asked).toEqual(['reserveasset+launchtoken']);
    await waitFor(() => expect(store).toMatchObject({ pending: [{ name: 'COPPER', launch: { info: 'Copper on a curve', supply: '21000000', decimals: 8, fee: 2.5 } }] }));
    mine.reservations.push({ txid: id('6'), name: 'COPPER', taken: false, height: 21, registers_from: 23, wait: 0 });
    await refreshBitAssets();
    await waitFor(() => expect(called('launchtoken').length).toBe(1));
    expect(called('launchtoken')[0].params).toEqual(['COPPER', { info: 'Copper on a curve', supply: '21000000', decimals: 8, fee: 2.5 }]);
    expect(asked).toEqual(['reserveasset+launchtoken']);
  });

  it('Register an asset, no market: a curve and a pool off without launches and pool kinds; registered alone', async () => {
    renderIssue();
    render(DialogHost);
    await waitFor(() => expect(ba.loaded).toBe(true));
    await fireEvent.input(screen.getByLabelText('Its name'), { target: { value: 'COPPER' } });
    await waitFor(() => expect(screen.getByText(/is free/)).toBeTruthy());
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    await fireEvent.input(document.querySelector('#issue-supply')!, { target: { value: '500' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    // Three ways (user decision, 2026-10-23). No launches nor pool kinds on this node: the curve and the pool are off.
    expect(screen.getAllByRole('radio').map((r) => r.textContent?.match(/^[^A-Z]*([A-Z][a-z]+( [a-z]+)*)/)?.[1])).toEqual(['Launch on a curve', 'Straight into a pool you fund', 'No market yet']);
    expect((screen.getByRole('radio', { name: /Launch on a curve/ }) as HTMLButtonElement).disabled).toBe(true);
    expect((screen.getByRole('radio', { name: /Straight into a pool/ }) as HTMLButtonElement).disabled).toBe(true);
    expect(screen.getByRole('radio', { name: /No market/ }).getAttribute('aria-checked')).toBe('true');
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    await fireEvent.click(screen.getByRole('button', { name: /Reserve and issue/ }));
    await waitFor(() => expect(asked).toEqual(['reserveasset+registerasset']));
    await waitFor(() => expect(store).toMatchObject({ pending: [{ name: 'COPPER' }], funding: [] }));
    expect((store as { pending: { follow?: string }[] }).pending[0].follow).toBeUndefined();
  });

  it('Browse assets: the market of each asset, and Create an asset', async () => {
    pools.push({ pool: id('b'), asset_a: 'ECX', asset_b: 'SILVER', asset_a_id: CHN_ID, asset_b_id: SILVER, reserve_a: 0, reserve_b: 0, price: 0, shares: 0, volume_a: 0, volume_b: 0, swaps: 0, created: 9, abandoned: false, curve: 'book' });
    render(Assets);
    await loaded();
    const tiles = await screen.findByRole('list', { name: 'Assets' });
    await waitFor(() => expect(within(tiles).getAllByTestId('market-badge').map((b) => b.textContent?.trim())).toEqual(expect.arrayContaining(['1 pool', 'Book', 'No market'])));
    // Top right, always there: Create asset, the page of the creation wizard.
    await fireEvent.click(screen.getByTestId('create-asset'));
    expect([nav.page, nav.tab]).toEqual(['launch', 'create']);
  });

  it('an asset: open a market, a curve for its controller once the launch parameters apply', async () => {
    dexOn = true;
    paramsOn = true;
    mine.assets[0] = { ...mine.assets[0], balance: 100000000 };
    pools.splice(0);
    render(AssetView, { id: GOLD });
    render(DialogHost);
    await refreshBitAssets();
    await loaded();
    // In Manage, shown because the wallet holds its control coin; the two markets, each by what the state allows.
    const open = await screen.findByRole('region', { name: 'Open a market' });
    expect(screen.getByTestId('asset-manage').contains(open)).toBe(true);
    expect(within(open).getAllByRole('button').map((b) => b.textContent?.trim().split(/\s{2,}|(?<=curve|fund)/)[0])).toEqual(['Launch curve', 'A pool you fund']);
    await fireEvent.click(within(open).getByRole('button', { name: /Launch curve/ }));
    await fireEvent.input(within(open).getByLabelText(/Amount on the curve/), { target: { value: '80000' } });
    await fireEvent.click(within(open).getByRole('button', { name: /Put it on a curve/ }));
    await answer('yes');
    await waitFor(() => expect(called('curveasset')[0]?.params).toEqual([assetArg(GOLD), '80000', { fee: 1.15 }]));
  });
});

// Simple mode (lib/uimode.svelte.ts, user decision 2026-10-16): Swap with a plain quote, Portfolio, Create an asset;
// Assets, Pools and Launch in Advanced. The same calls as Advanced.
describe('Simple mode', () => {
  beforeEach(() => {
    prefs.uiMode = 'simple';
  });

  it('leaves the auctions out of the navigation, and has three first steps of BitAssets', async () => {
    expect((LOCAL_PAGES as readonly LocalPage[]).some((p) => p.id === ('auctions' as string))).toBe(false);
    expect((LOCAL_PAGES as readonly LocalPage[]).filter((p) => p.simple).map((p) => p.id)).toEqual(['my-assets', 'trade', 'staking']);
    const { FIRST_STEPS } = await import('./firststeps');
    expect(FIRST_STEPS.map((s) => s.action?.page)).toEqual(['peg', 'trade', 'issue']);
  });

  it('Swap: the quote in plain words, the slippage under More options, with the call of Advanced', async () => {
    render(Dex);
    render(DialogHost);
    await loaded();
    expect(screen.getByRole('heading', { name: 'Swap' })).toBeTruthy();
    // No tabs of pools nor liquidity, nor the curve of the pool.
    expect(screen.queryByRole('tab', { name: 'Pools' })).toBeNull();
    expect(screen.queryByRole('heading', { name: 'The pool' })).toBeNull();
    expect(screen.getByRole('heading', { name: 'How a swap works' })).toBeTruthy();
    await waitFor(() => expect((document.querySelector('#swap-get') as HTMLElement).dataset.value).toBe(GOLD));
    expect(screen.queryByRole('group', { name: 'Slippage' })).toBeNull();
    await fireEvent.input(document.querySelector('#swap-amount')!, { target: { value: '0.5' } });
    await waitFor(() => expect(screen.getByTestId('swap-plain').textContent).toMatch(/You pay 0\.5\s*ECX and get about/));
    expect(screen.getByTestId('swap-plain').textContent).toMatch(/At least .* if the price moves first/);
    // The plain summary names the asset with its number (audit 7 E-BA4).
    expect(screen.getByTestId('swap-plain').textContent).toContain('$GOLD #1739-0029');
    await fireEvent.click(screen.getByRole('button', { name: /More options/ }));
    await fireEvent.click(screen.getByRole('button', { name: '1%' }));
    await fireEvent.click(screen.getByRole('button', { name: /^Swap$/ }));
    await answer('yes');
    await waitFor(() => expect(called('swapasset')[0]?.params).toEqual(['ECX', '0.5', assetArg(GOLD), 1, 10, '4.88']));
    // Done (the host confirmed and ran it): the form is cleared.
    await waitFor(() => expect((document.querySelector('#swap-amount') as HTMLInputElement).value).toBe(''));
    // Asked for by a link: the pools are shown.
    nav.tab = 'classic';
    await waitFor(() => expect(screen.getByRole('table', { name: 'Pools' })).toBeTruthy());
  });

  it('Create an asset: the wizard of Advanced in plain words, the same calls', async () => {
    renderIssue();
    render(DialogHost);
    await waitFor(() => expect(ba.loaded).toBe(true));
    expect(screen.getByRole('heading', { name: 'Create an asset' })).toBeTruthy();
    await fireEvent.input(screen.getByLabelText('Its name'), { target: { value: 'COPPER' } });
    await waitFor(() => expect(screen.getByText(/is free/)).toBeTruthy());
    expect(screen.queryByRole('checkbox', { name: /Publish the name/ })).toBeNull();
    await fireEvent.input(document.querySelector('#issue-info')!, { target: { value: 'Copper, by the gram' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    // The decimals under More options (8 by default).
    expect(document.querySelector('#issue-decimals')).toBeNull();
    await fireEvent.input(document.querySelector('#issue-supply')!, { target: { value: '1000.25' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    await fireEvent.click(screen.getByRole('button', { name: /Create the asset/ }));
    await waitFor(() => expect(called('reserveasset')[0]?.params).toEqual(['COPPER']));
    await waitFor(() => expect(store).toMatchObject({ pending: [{ wallet: 'w', name: 'COPPER', supply: '1000.25', decimals: 8, data: { info: 'Copper, by the gram' }, public: true }] }));
  });

  it('My assets: holdings, what is being created and the activity; pool shares and auctions in Advanced', async () => {
    render(MyAssets);
    await loaded();
    const tabs = () => screen.getAllByRole('tab').map((x) => x.textContent?.trim());
    await waitFor(() => expect(tabs()).toEqual(['Holdings', 'Being created', 'Activity']));
    expect(screen.getByRole('list', { name: 'Holdings' })).toBeTruthy();
    expect(screen.getByRole('button', { name: /Create asset/ })).toBeTruthy();
    prefs.uiMode = 'advanced';
    await waitFor(() => expect(tabs()).toEqual(['Holdings', 'Being issued', 'Pool shares', 'My auctions', 'Activity']));
    expect(screen.queryByTestId('create-asset')).toBeNull();
  });

  it('the cards of the home: no auctions card', async () => {
    render(LocalCards);
    await loaded();
    await waitFor(() => expect(screen.getByRole('heading', { name: 'Portfolio' })).toBeTruthy());
    expect(screen.queryByRole('heading', { name: 'Auctions' })).toBeNull();
    expect(screen.getByRole('button', { name: /Create an asset/ })).toBeTruthy();
  });
});

describe('audit 7 E-BA6: amounts past 2^53 units, exact from the node to the dialog', () => {
  // 2^60 + 1 units of an asset with 8 decimals: no double holds it (1152921504606846977).
  const BIG = '11529215046.06846977';
  const NUM = '\u0000num:';
  /** The node writes these as JSON numbers: in the fixtures, marked, then written bare in the text of the reply. */
  const num = (digits: string) => `${NUM}${digits}` as unknown as number;
  const rawJson = (value: unknown) => JSON.stringify(value).replace(new RegExp(`"\\\\u0000num:([^"]+)"`, 'g'), '$1');

  beforeEach(() => {
    assets[0] = { ...assets[0], decimals: 8, supply: num('23058430092.13693953'), minted: num('23058430092.13693953') };
    mine.assets[0] = { ...mine.assets[0], decimals: 8, balance: num(BIG), supply: num('23058430092.13693953') };
    auctions[0] = { ...auctions[0], base: 'GOLD', quote: 'ECX', amount: num('11529215046.06846978'), remaining: num(BIG), cost_of_remaining: num('9007199254.74099313') };
    // The host gives the results of the methods BitAssets reads exactly as text, as the node wrote them.
    setTransport(async (r) => {
      const result = await fakeNode(r);
      if (!r.exact) {
        expect(JSON.stringify(result ?? null)).not.toContain('\\u0000num:');
        return result;
      }
      return { $rawjson: rawJson(result ?? null) };
    });
  });

  it('a balance is read, shown and filled in by Max with every digit', async () => {
    render(LocalSend);
    render(DialogHost);
    await loaded();
    expect(ba.mine!.assets[0].balance).toBe(BIG);
    sendIntent.asset = GOLD;
    await waitFor(() => expect(document.querySelector('#asset-amount')).not.toBeNull());
    await fireEvent.click(screen.getByRole('button', { name: 'Max' }));
    await waitFor(() => expect((document.querySelector('#asset-amount') as HTMLInputElement).value).toBe(BIG));
    await fireEvent.input(document.querySelector('#asset-to')!, { target: { value: OTHER } });
    const send = screen.getByRole('button', { name: /^Send$/ });
    await waitFor(() => expect(send).toHaveProperty('disabled', false), { timeout: 3000 });
    await fireEvent.click(send);
    // The confirmation of the page, then what the host is asked to confirm and run: the same digits.
    await waitFor(() => expect(document.querySelector('dialog[open]')?.textContent).toContain(`Send 11,529,215,046.06846977 $GOLD #1739-0029 to this address?`));
    await answer('yes', 4000);
    await waitFor(() => expect(called('sendasset')[0]?.params).toEqual([OTHER, assetArg(GOLD), BIG]));
  });

  it('My assets shows the balance and the supply exactly', async () => {
    render(MyAssets);
    await loaded();
    await waitFor(() => expect(document.body.textContent).toContain('11,529,215,046.06846977'));
  });

});

describe('audit 7: names and amounts', () => {
  it('names that carry a number are refused, as the wallet refuses them (E-BA4)', () => {
    for (const n of ['GOLD (#1739-0029)', 'GOLD #2', 'GOLD 1739-0029', '(x)']) expect(nameMimicsNumber(n)).toBe(true);
    for (const n of ['GOLD', 'GOLD-2026', '123-4567']) expect(nameMimicsNumber(n)).toBe(false);
    expect(nameProblem('GOLD (#1739-0029)')).toMatch(/would read as the number of another asset/);
  });

  it('amounts past 2^53 units never round above a balance, nor below a cap (E-BA6)', () => {
    // 1234567.890123456789 with 12 decimals: the double reads it as …4567 17.
    const v = 1234567.890123456789;
    const exact = 1234567890123456789n;
    expect(unitsAtMost(v, 12) <= exact).toBe(true);
    expect(unitsAtLeast(v, 12) >= exact).toBe(true);
    expect(exact - unitsAtMost(v, 12) < 10_000n).toBe(true);
    // Exact amounts are left as they are.
    expect(unitsAtMost(12.5, 2)).toBe(1250n);
    expect(unitsAtLeast('9223372.036854775807', 12)).toBe(9223372036854775807n);
  });
});

describe('the assets a wallet hides (user feedback 2026-10-19, item 19)', () => {
  it('ignores an asset in this wallet only, never ECX; names, words and filters; kept in the preferences', async () => {
    await loadHiding();
    ignoreAsset(SILVER);
    ignoreAsset(CHN_ID);
    expect(assetIgnored(SILVER, 'SILVER')).toBe(true);
    expect(assetIgnored(CHN_ID, 'ECX')).toBe(false);
    expect((prefsStore.assetFilters as Record<string, { ignored: string[] }>).w.ignored).toEqual([SILVER]);
    expect(assetIgnored(SILVER, 'SILVER', 'other')).toBe(false);
    ignoreName('gold');
    expect(assetIgnored(GOLD, 'GOLD')).toBe(true);
    expect(assetIgnored('5'.repeat(64), 'Gold')).toBe(true);
    unignoreName('GOLD');
    setFilters({ keywords: ['ebbl'] });
    expect(assetHidden({ id: PEBBLES, label: 'PEBBLES' })).toBe(true);
    setFilters({ keywords: [], hideDead: true });
    // Dead: no activity in the node's volume window (1440 blocks) by default, or in the blocks the user set.
    expect(assetHidden({ id: PEBBLES, label: 'PEBBLES', lastActivity: 100, tip: 1600 })).toBe(true);
    expect(assetHidden({ id: PEBBLES, label: 'PEBBLES', lastActivity: 200, tip: 1600 })).toBe(false);
    expect(assetHidden({ id: PEBBLES, label: 'PEBBLES' })).toBe(false);
    expect(searchFiltersOf()).toEqual({ exclude_dead: true });
    setFilters({ deadAfter: 500 });
    expect(assetHidden({ id: PEBBLES, label: 'PEBBLES', lastActivity: 1000, tip: 1600 })).toBe(true);
    expect(searchFiltersOf()).toEqual({ exclude_dead: 500 });
    setFilters({ deadAfter: 0 });
    // Read back after a restart.
    resetHiding();
    await loadHiding();
    expect(filtersOf().ignored).toEqual([SILVER]);
    expect(filtersOf().hideDead).toBe(true);
  });

  it('hides it on the home, in My assets, the Send picker, the pools and Assets, each list showing it on asking', async () => {
    prefsStore = { assetFilters: { w: { ignored: [GOLD] } } };
    render(HeldAssets);
    await loaded();
    const held = screen.getByTestId('held-assets');
    await waitFor(() => expect(held.textContent).toContain('SILVER'));
    expect(held.textContent).not.toContain('GOLD');
    await fireEvent.click(within(held).getByRole('button', { name: 'Show them' }));
    await waitFor(() => expect(held.textContent).toContain('GOLD'));
    cleanup();

    render(LocalSend);
    await loaded();
    await fireEvent.click(document.querySelector('#send-what')!);
    await waitFor(() => expect(document.querySelector(`[role="option"][data-id="${SILVER}"]`)).not.toBeNull());
    expect(document.querySelector(`[role="option"][data-id="${GOLD}"]`)).toBeNull();
    cleanup();

    nav.tab = 'pools';
    render(Dex, { props: { page: 'pools' } });
    await loaded();
    await waitFor(() => expect(screen.getByTestId('hidden-assets').textContent).toContain('1 hidden'));
    expect(screen.queryByRole('table', { name: 'Pools' })).toBeNull();
    await fireEvent.click(screen.getByRole('button', { name: 'Show them' }));
    await waitFor(() => expect(screen.getByRole('table', { name: 'Pools' })).toBeTruthy());
    cleanup();
    nav.tab = '';

    render(Assets);
    await loaded();
    const tiles = () => within(screen.getByRole('list', { name: 'Assets' })).getAllByRole('listitem').map((li) => li.textContent ?? '');
    await waitFor(() => expect(tiles().some((x) => x.includes('SILVER'))).toBe(true));
    expect(tiles().some((x) => x.includes('GOLD'))).toBe(false);
  });

  it('Pools: one table of the pools, the deepest first, an empty one last and marked; found by an asset; a row opens the pool\'s page', async () => {
    pools = [...pools, { ...pools[0], pool: id('q'), asset_a: 'SILVER', asset_a_id: SILVER, reserve_b: 80, swaps: 9, created: 1 }, { ...pools[0], pool: id('z'), asset_a: 'PEBBLES', asset_a_id: PEBBLES, reserve_a: 0, reserve_b: 0, abandoned: true }];
    nav.tab = 'pools';
    render(Dex, { props: { page: 'pools' } });
    await loaded();
    expect(screen.getByRole('heading', { name: 'Pools' })).toBeTruthy();
    const rows = () => within(screen.getByRole('table', { name: 'Pools' })).getAllByTestId('pool-row').map((r) => r.textContent ?? '');
    await waitFor(() => expect(rows().length).toBe(3));
    expect(rows()[0]).toContain('SILVER');
    expect(rows()[2]).toMatch(/PEBBLES.*Needs liquidity/);
    // One kind of pool: its fee, never its curve; the wallet's share of the pool it provides to.
    expect(rows()[1]).toMatch(/GOLD.*0\.3%.*6\.32%/);
    expect(document.querySelector('[data-testid="pools-table"]')?.textContent).not.toMatch(/Constant product|Concentrated|Stable|Weighted/);
    await fireEvent.input(screen.getByRole('searchbox', { name: 'Find a pool' }), { target: { value: 'gol' } });
    await waitFor(() => expect(rows().length).toBe(1));
    await fireEvent.click(within(screen.getByRole('table', { name: 'Pools' })).getAllByTestId('pool-row')[0]);
    expect(nav.tab).toBe(`pools/${id('p')}`);
    await waitFor(() => expect(screen.queryByRole('table', { name: 'Pools' })).toBeNull());
    await fireEvent.click(screen.getByRole('button', { name: /All pools/ }));
    expect(nav.tab).toBe('pools');
  });

  it('Pools: the assets on their curve under "Opening at graduation"; Launch lists them with what they raised; New pool is one action', async () => {
    dexOn = true;
    nav.tab = 'pools';
    const shown = render(Dex, { props: { page: 'pools' } });
    await loaded();
    const grad = await screen.findByTestId('pools-graduating');
    await waitFor(() => expect(within(grad).getAllByTestId('graduating-row').length).toBe(1));
    expect(within(grad).getByTestId('graduating-row').textContent).toMatch(/MOON.*200.*500.*On curve 40/);
    // The launch curve is no row of the pools' table.
    pools.push({ ...pools[0], pool: id('c'), asset_a: 'MOON', asset_a_id: MOON, curve: 'launch' });
    await refreshBitAssets();
    await waitFor(() => expect(within(screen.getByRole('table', { name: 'Pools' })).getAllByTestId('pool-row').length).toBe(1));
    expect(screen.getAllByTestId('new-pool-button').length).toBe(1);
    await fireEvent.click(screen.getByTestId('new-pool-button'));
    expect(nav.tab).toBe('new');
    await waitFor(() => expect(screen.getByTestId('new-pool')).toBeTruthy());
    shown.unmount();
    nav.tab = 'launches';
    render(Dex, { props: { page: 'launch' } });
    const list = await screen.findByTestId('launch-list');
    await waitFor(() => expect(within(list).getAllByTestId('launch-row').length).toBe(1));
    expect(within(list).getByTestId('launch-row').textContent).toMatch(/MOON.*200 of 500 ECX.*40.*1%/);
    await fireEvent.click(screen.getByTestId('create-asset'));
    expect([nav.page, nav.tab]).toEqual(['launch', 'create']);
    await waitFor(() => expect(screen.getByRole('heading', { name: 'Create asset' })).toBeTruthy());
  });

  it('folds the transactions of an ignored asset in the Transactions page, gives no notice of them; offers Ignore on the notice of an asset received, and warns of a look-alike', async () => {
    const T1 = id('1');
    const T2 = id('2');
    const LOOK = '4'.repeat(64);
    history = [T1, T2];
    ba.transfersWallet = 'w';
    ba.transfers = [
      { txid: T1, time: 2, confirmations: 3, category: 'receive', asset: GOLD, label: 'GOLD', seq: '1739-0029', decimals: 2, amount: '5', sign: 1, to: [{ address: ME, mine: true }], from: [{ address: OTHER, mine: false }], abandoned: false },
      { txid: T2, time: 1, confirmations: 3, category: 'receive', asset: LOOK, label: 'SILVER', seq: '7777-0001', decimals: 0, amount: '1', sign: 1, to: [{ address: ME, mine: true }], from: [{ address: OTHER, mine: false }], abandoned: false },
    ];
    await refreshBitAssets();
    ba.transfersWallet = 'w';
    prefsStore = { assetFilters: { w: { ignored: [GOLD] } } };
    resetHiding();
    await loadHiding();
    expect(chainTxHidden(T1)).toBe(true);
    expect(chainTxHidden(T2)).toBe(false);
    // The Transactions page: one line says the hidden one, which shows on asking.
    render(Transactions);
    const rows = () => within(screen.getByRole('grid', { name: 'Transactions' })).queryAllByRole('row').filter((r) => r.querySelector('[role="gridcell"], td'));
    await waitFor(() => expect(screen.getByTestId('hidden-transactions').textContent).toMatch(/1 hidden transaction/), { timeout: 5000 });
    await waitFor(() => expect(rows().length).toBe(1));
    await fireEvent.click(screen.getByRole('button', { name: 'Show them' }));
    await waitFor(() => expect(rows().length).toBe(2));
    cleanup();
    const notice = chainTxNotice(T2)!;
    expect(notice.warning).toMatch(/Same name as an asset you hold: this is #7777-0001/);
    notice.actions![0].run();
    expect(filtersOf().ignored).toContain(LOOK);
    expect(chainTxHidden(T2)).toBe(true);
    unignoreAsset(LOOK);
  });

  it('is set in Settings, Asset filters: the filters, the words, the names, the ignored assets to unignore', async () => {
    prefsStore = { assetFilters: { w: { ignored: [SILVER] } } };
    render(LocalSettings, { props: { tab: 'asset-filters' } });
    await loaded();
    await waitFor(() => expect(screen.getByRole('list', { name: 'Ignored assets' }).textContent).toContain('SILVER'));
    expect((screen.getByRole('spinbutton', { name: 'Dead after this many blocks' }) as HTMLInputElement).disabled).toBe(true);
    await fireEvent.click(screen.getByRole('checkbox', { name: /Hide dead assets/ }));
    expect(filtersOf().hideDead).toBe(true);
    expect(searchFiltersOf()).toEqual({ exclude_dead: true });
    await fireEvent.change(screen.getByRole('spinbutton', { name: 'Dead after this many blocks' }), { target: { value: '720' } });
    expect(filtersOf().deadAfter).toBe(720);
    expect(searchFiltersOf()).toEqual({ exclude_dead: 720 });
    await fireEvent.change(screen.getByRole('spinbutton', { name: 'Dead after this many blocks' }), { target: { value: '' } });
    expect(searchFiltersOf()).toEqual({ exclude_dead: true });
    await fireEvent.input(screen.getByRole('textbox', { name: 'A word to ignore' }), { target: { value: 'airdrop' } });
    await fireEvent.click(screen.getAllByRole('button', { name: 'Add' })[0]);
    expect(filtersOf().keywords).toEqual(['airdrop']);
    await fireEvent.click(screen.getByRole('button', { name: 'Unignore' }));
    expect(filtersOf().ignored).toEqual([]);
    expect(LOCAL_SETTINGS_TABS).toEqual([{ id: 'asset-filters', label: 'Asset filters' }]);
  });
});

describe('a burst of blocks and transactions (user feedback 2026-10-19, item 10)', () => {
  it('keeps the lists the node answers the same with (nothing is worked out again), and works out a row once', async () => {
    await refreshBitAssets();
    const before = { assets: ba.assets, pools: ba.pools, auctions: ba.auctions, mine: ba.mine, activity: ba.activity, transfers: ba.transfers };
    const version = ba.version;
    await refreshBitAssets();
    expect(ba.version).toBeGreaterThan(version);
    expect(ba.assets).toBe(before.assets);
    expect(ba.pools).toBe(before.pools);
    expect(ba.auctions).toBe(before.auctions);
    expect(ba.mine).toBe(before.mine);
    expect(ba.activity).toBe(before.activity);
    // A change is taken.
    assets = [...assets, asset('3'.repeat(64), { label: 'NEW', seq: '1234-0001', registered: 30 })];
    await refreshBitAssets();
    expect(ba.assets).not.toBe(before.assets);
    // What a transaction is worked out once while its list stays.
    ba.transfersWallet = 'w';
    ba.transfers = [{ txid: id('1'), time: 2, confirmations: 1, category: 'send', asset: GOLD, label: 'GOLD', seq: '1739-0029', decimals: 2, amount: '1', sign: -1, to: [{ address: OTHER, mine: false }], from: [], abandoned: false }];
    const { chainTxOf } = await import('./txinfo.svelte');
    expect(chainTxOf(id('1'))).toBe(chainTxOf(id('1')));
  });
});

// Tickers and display names (from ticker_height): the name of a new asset is a ticker, a display name beside it.
describe('tickers and display names', () => {
  it('checks tickers and display names as the rules do (the node is the authority)', () => {
    for (const good of ['GOLD', 'AB', 'BTC2', 'ABCDEFGHIJKL', '00']) expect(isTicker(good), good).toBe(true);
    for (const bad of ['G', 'ABCDEFGHIJKLM', 'gold', 'GO-LD', 'GO LD', 'ÄB']) expect(isTicker(bad), bad).toBe(false);
    expect(tickerProblem('gold')).toBe('');
    expect(tickerProblem('Gold Bar')).toMatch(/A to Z and digits/);
    expect(tickerProblem('G')).toMatch(/at least 2/);
    expect(tickerProblem('chn')).toMatch(/coin of the chain/);
    expect(displayNameProblem('Gold Bar Token, 1 g')).toBe('');
    expect(displayNameProblem('金条 Gold')).toBe('');
    expect(displayNameProblem('Café 🪙')).toBe('');
    for (const [bad, why] of [
      ['GOLD\u202eDLOG', 'U+202E'],
      ['GO\u200bLD', 'U+200B'],
      ['GO\u200dLD', 'U+200D'],
      ['\ufeffGOLD', 'U+FEFF'],
      ['Gold\u00a0Bar', 'U+00A0'],
      ['Gold\n', 'U+000A'],
      [' Gold', 'space'],
      ['Cafe\u0301', 'NFC'],
      ['\u212b', 'NFC'],
      ['x'.repeat(65), '64 bytes'],
      ['é'.repeat(33), '64 bytes'],
    ] as const) {
      expect(displayNameProblem(bad), JSON.stringify(bad)).toContain(why);
    }
    // What is typed decomposed is sent composed.
    expect(normalizeDisplayName('Cafe\u0301')).toBe('Café');
    expect(assetTitle('GOLD', 'Gold Bar Token, 1 g', '1739-0029')).toBe('Gold Bar Token, 1 g $GOLD #1739-0029');
    expect(assetTitle('gold bar', undefined, '1739-0029')).toBe('gold bar #1739-0029');
  });

  it('Register new asset: a ticker uppercased as typed, a display name, both registered with the reservation', async () => {
    tickersOn = true;
    renderIssue();
    render(DialogHost);
    await waitFor(() => expect(ba.info?.tickers_active).toBe(true));
    const ticker = (await screen.findByLabelText('Ticker')) as HTMLInputElement;
    expect(ticker.maxLength).toBe(12);
    await fireEvent.input(ticker, { target: { value: 'gold bar' } });
    expect(screen.getByText(/A to Z and digits 0 to 9 only/)).toBeTruthy();
    await fireEvent.input(ticker, { target: { value: 'copper' } });
    expect(ticker.value).toBe('COPPER');
    await waitFor(() => expect(screen.getByText(/is free/)).toBeTruthy());
    const display = screen.getByLabelText(/Display name/) as HTMLInputElement;
    await fireEvent.input(display, { target: { value: 'Copper\u202e' } });
    expect(screen.getByText(/U\+202E/)).toBeTruthy();
    expect((screen.getByRole('button', { name: 'Next' }) as HTMLButtonElement).disabled).toBe(true);
    await fireEvent.input(display, { target: { value: 'Copper, by the gram' } });
    expect((screen.getByRole('button', { name: 'Next' }) as HTMLButtonElement).disabled).toBe(false);
    // No private name from ticker_height: the ticker is published.
    expect(screen.queryByText(/Publish the name/)).toBeNull();
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    await fireEvent.input(document.querySelector('#issue-supply')!, { target: { value: '5' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    const review = await screen.findByRole('heading', { name: 'Review' });
    const lines = review.parentElement!.textContent ?? '';
    expect(lines).toContain('TickerCOPPER');
    expect(lines).toContain('Display nameCopper, by the gram');
    await fireEvent.click(screen.getByRole('button', { name: /Reserve and issue/ }));
    await waitFor(() => expect(called('reserveasset')[0]?.params).toEqual(['COPPER']));
    expect(asked).toEqual(['reserveasset+registerasset']);
    mine.reservations.push({ txid: id('6'), name: 'COPPER', taken: false, height: 21, registers_from: 23, wait: 0 });
    await refreshBitAssets();
    await waitFor(() => expect(called('registerasset').length).toBe(1));
    // The display name after the logo (none: null), as registerasset takes them.
    expect(called('registerasset')[0].params).toEqual(['COPPER', '5', 8, {}, true, null, 'Copper, by the gram']);
    expect(asked).toEqual(['reserveasset+registerasset']);
  });

  it('My assets: the controller adds, changes and removes the display name', async () => {
    tickersOn = true;
    render(MyAssets);
    render(DialogHost);
    await loaded();
    const panel = await screen.findByRole('region', { name: 'Manage GOLD' });
    await waitFor(() => expect(within(panel).getByRole('button', { name: 'Add a display name' })).toBeTruthy());
    await fireEvent.click(within(panel).getByRole('button', { name: 'Add a display name' }));
    const input = within(panel).getByLabelText('Display name') as HTMLInputElement;
    await fireEvent.input(input, { target: { value: 'Gold\u200bBar' } });
    expect((within(panel).getByRole('button', { name: 'Save the display name' }) as HTMLButtonElement).disabled).toBe(true);
    await fireEvent.input(input, { target: { value: 'Gold Bar Token, 1 g' } });
    await fireEvent.click(within(panel).getByRole('button', { name: 'Save the display name' }));
    await waitFor(() => expect(called('setassetdisplayname')[0]?.params).toEqual([assetArg(GOLD), 'Gold Bar Token, 1 g']));
    expect(asked).toContain('setassetdisplayname');
    // Once listed with it: shown after the ticker, and it can be removed.
    assets[0] = { ...assets[0], display_name: 'Gold Bar Token, 1 g' };
    await refreshBitAssets();
    await waitFor(() => expect(within(panel).getByRole('button', { name: 'Change its display name' })).toBeTruthy());
    expect(within(panel).getByRole('heading', { level: 2 }).textContent).toContain('Gold Bar Token, 1 g');
    await fireEvent.click(within(panel).getByRole('button', { name: 'Change its display name' }));
    await fireEvent.click(await within(panel).findByRole('button', { name: 'Remove it' }));
    await waitFor(() => expect(called('setassetdisplayname')[1]?.params).toEqual([assetArg(GOLD), '']));
  });

  it('Your assets on the Home and the Overview: one compact row each, ticker, display name, balance, value; the crown inline', async () => {
    assets[0] = { ...assets[0], display_name: 'Gold Bar Token, 1 g' };
    mine.assets.push({ asset: PEBBLES, label: 'PEBBLES', decimals: 0, balance: 0, pending: 0, outputs: 0, control: true, control_pending: false, supply: 0, fixed: false });
    for (const which of ['home', 'overview']) {
      const shown = which === 'home' ? render(LocalCards) : render(LocalOverview);
      await loaded();
      const held = await waitFor(() => screen.getByTestId('held-assets'));
      const rows = await waitFor(() => {
        const r = [...held.querySelectorAll<HTMLElement>('[data-testid="held-row"]')];
        if (!r.length) throw new Error('no rows');
        return r;
      });
      // Asset (logo, name, $TICKER, #number, crown), Quantity, ECX worth.
      expect([...held.querySelectorAll('thead th')].map((c) => c.textContent?.trim())).toEqual(['Asset', 'Quantity', 'ECX worth']);
      const cells = [...rows[0].querySelectorAll('td')].map((c) => c.textContent?.trim() ?? '');
      expect(rows[0].querySelector('.c-asset .badge-asset')).not.toBeNull();
      expect(rows[0].querySelector('.c-asset .asset-name')?.textContent).toBe('Gold Bar Token, 1 g $GOLD #1739-0029');
      expect(cells[1]).toBe('200.00');
      expect(cells[2]).toBe('20');
      // The crown beside the logo, in the same cell, with its tooltip; SILVER is not controlled, and has no price.
      expect(rows[0].querySelector('.c-asset [title="You control this asset"]')).not.toBeNull();
      expect(rows[1].querySelector('[title="You control this asset"]')).toBeNull();
      expect(rows[1].querySelector('.c-value')?.textContent).toBe('–');
      // Only controlled: an inline chip with its crown.
      const only = within(held).getByTestId('controlled-only');
      expect(only.querySelector('.chip [title="You control this asset"]')).not.toBeNull();
      expect(only.textContent).toContain('PEBBLES');
      shown.unmount();
    }
  });

  it('a picker and the lists show the name first, $TICKER after it, and find an asset by its display name', async () => {
    assets[0] = { ...assets[0], display_name: 'Gold Bar Token, 1 g' };
    render(Assets);
    await loaded();
    const grid = await screen.findByRole('list', { name: 'Assets' });
    await waitFor(() => expect(grid.textContent).toMatch(/Gold Bar Token, 1 g \$GOLD #1739-0029/));
  });

  it('one identity of an asset everywhere, its parts never run together: "Name $TICKER" (user feedback 2026-10-19, 2026-10-24)', async () => {
    assets[0] = { ...assets[0], display_name: 'Gold Bar Token, 1 g' };
    const SEP = /Gold Bar Token, 1 g \$GOLD/;
    // My assets → Holdings: the heading of the asset picked ("GOLDGold Bar Token, 1 g" before), and its row.
    let shown = render(MyAssets);
    await loaded();
    const panel = await screen.findByRole('region', { name: 'Manage GOLD' });
    await waitFor(() => expect(panel.querySelector('h2')?.textContent).toMatch(SEP));
    expect(within(screen.getByRole('list', { name: 'Holdings' })).getAllByRole('button')[0].textContent).toMatch(SEP);
    shown.unmount();
    // The picker of an asset (Swap): each option, with its number.
    shown = render(Dex);
    await loaded();
    await fireEvent.click(document.querySelector('#swap-get')!);
    const option = await waitFor(() => {
      const o = document.querySelector<HTMLElement>(`[role="option"][data-id="${GOLD}"]`);
      if (!o) throw new Error('no option');
      return o;
    });
    expect(option.textContent).toMatch(/Gold Bar Token, 1 g \$GOLD #1739-0029/);
    shown.unmount();
    // The badge (lists, pools, auctions, activity, details).
    shown = render(Assets);
    await loaded();
    const badge = await waitFor(() => {
      const b = [...document.querySelectorAll<HTMLElement>('.badge-asset')].find((x) => x.textContent?.includes('Gold Bar'));
      if (!b) throw new Error('no badge');
      return b;
    });
    expect(badge.textContent).toMatch(SEP);
    shown.unmount();
    // Home and Overview: the same, in the one cell of the asset.
    for (const which of ['home', 'overview']) {
      shown = which === 'home' ? render(LocalCards) : render(LocalOverview);
      await loaded();
      const row = await waitFor(() => {
        const r = document.querySelector<HTMLElement>('[data-testid="held-row"]');
        if (!r) throw new Error('no row');
        return r;
      });
      expect(row.querySelector('.c-asset')?.textContent).toMatch(SEP);
      shown.unmount();
    }
  });
});

// The DEX in four (user decision, 2026-10-23, "Orca-style"): Swap (the best over every venue), Pools (concentrated, the
// full range by default), Launch (the one wizard, three ways), Portfolio; the rest behind Swap's Advanced link.
describe.runIf(CHAIN.name === 'Flush')('the DEX in four: Swap, Pools, Launch, Portfolio', () => {
  const CL = { sqrt_price_x96: 'ab', tick: 23027, liquidity: '5000000', tick_spacing: 60 };
  /** A concentrated GOLD/ECX pool: 1 GOLD = 0.1 ECX. */
  function clPool(pool = id('k'), extra: Partial<Pool> = {}) {
    pools.push({ ...pools[0], pool, reserve_a: 50, reserve_b: 5, price: 0.1, shares: 0, swaps: 3, curve: 'concentrated', fee: 0.3, fee_millionths: 3000, default_kind: false, concentrated: CL, ...extra });
  }

  it('the actions of an asset row: the most relevant as a button, the rest in a menu by the keyboard; their routes kept by the pages', async () => {
    dexOn = true;
    nav.page = 'my-assets';
    nav.tab = '';
    let shown = render(MyAssets);
    await loaded();
    const holdings = await screen.findByRole('list', { name: 'Holdings' });
    const gold = await waitFor(() => within(holdings).getAllByTestId('asset-actions')[0]);
    // GOLD has a pool with ECX: Swap first, to get it.
    const primary = gold.querySelector<HTMLButtonElement>('.act')!;
    expect(primary.dataset.action).toBe('swap');
    // The menu: opened from the keyboard on its first item, arrows, Escape back to its button.
    const more = within(gold).getByRole('button', { name: /^More actions for/ });
    await fireEvent.keyDown(more, { key: 'ArrowDown' });
    const menu = await screen.findByRole('menu');
    const items = within(menu).getAllByRole('menuitem').map((x) => x.textContent?.trim());
    expect(items).toEqual(['Sell', 'Add liquidity', 'View market', 'Send', 'Receive', 'View asset']);
    await waitFor(() => expect(document.activeElement?.textContent?.trim()).toBe('Sell'));
    await fireEvent.keyDown(menu, { key: 'ArrowDown' });
    expect(document.activeElement?.textContent?.trim()).toBe('Add liquidity');
    await fireEvent.keyDown(menu, { key: 'Escape' });
    expect(screen.queryByRole('menu')).toBeNull();
    expect(document.activeElement).toBe(more);
    // A click on an action does not pick the row under it.
    await fireEvent.click(more);
    await fireEvent.click(within(screen.getByRole('menu')).getByRole('menuitem', { name: 'Sell' }));
    expect([nav.page, nav.tab]).toEqual(['trade', `swap/pay/${GOLD}`]);
    shown.unmount();
    // Swap takes the asset and its side from the route.
    shown = render(Dex);
    await loaded();
    await waitFor(() => expect((document.querySelector('#swap-pay') as HTMLElement).dataset.value).toBe(GOLD));
    expect((document.querySelector('#swap-get') as HTMLElement).dataset.value).toBe(CHN_ID);
    nav.tab = `swap/get/${SILVER}`;
    await waitFor(() => expect((document.querySelector('#swap-get') as HTMLElement).dataset.value).toBe(SILVER));
    shown.unmount();
    // A launch's old route: the asset's page, its Trade box on the side named, focused.
    nav.page = 'launch';
    nav.tab = `launches/${MOON}/sell`;
    shown = render(Dex, { props: { page: 'pools' } });
    await loaded();
    expect([nav.page, nav.tab]).toEqual(['assets', `asset/${MOON}/sell`]);
    await waitFor(() => expect(document.activeElement?.id).toBe('launch-amount'));
    expect(screen.getByLabelText('You sell')).toBeTruthy();
    // Back: the table of assets, its route without the asset.
    await fireEvent.click(screen.getByRole('button', { name: /All assets/ }));
    await waitFor(() => expect(nav.tab).toBe(''));
    shown.unmount();
    // My launches: Claim first for its creator; Buy in its menu, to the launch's page.
    nav.page = 'my-assets';
    nav.tab = '';
    shown = render(MyAssets);
    await loaded();
    const moon = await waitFor(() => within(screen.getByTestId('my-launches')).getAllByTestId('my-launch').find((r) => r.textContent?.includes('MOON'))!);
    expect(moon.querySelector<HTMLButtonElement>('.act')!.dataset.action).toBe('claim');
    await fireEvent.click(within(moon).getByRole('button', { name: /^More actions for/ }));
    await fireEvent.click(within(screen.getByRole('menu')).getByRole('menuitem', { name: 'Buy' }));
    expect([nav.page, nav.tab]).toEqual(['assets', `asset/${MOON}/buy`]);
    shown.unmount();
    // Browse assets: each card has its actions, outside its button.
    nav.page = 'assets';
    nav.tab = '';
    render(Assets);
    await loaded();
    const tiles = await screen.findByRole('list', { name: 'Assets' });
    await waitFor(() => expect(within(tiles).getAllByTestId('asset-actions').length).toBe(within(tiles).getAllByRole('listitem').length));
    expect(tiles.querySelector('button.tile [data-testid="asset-actions"]')).toBeNull();
  });

  it('Create asset: one way in, from Assets and from Launch, to the one wizard', async () => {
    dexOn = true;
    nav.page = 'assets';
    nav.tab = '';
    const shown = render(Assets);
    await loaded();
    await fireEvent.click(await screen.findByTestId('create-asset'));
    expect([nav.page, nav.tab]).toEqual(['launch', 'create']);
    shown.unmount();
    for (const page of ['trade', 'pools']) {
      nav.tab = '';
      const view = render(Dex, { props: { page } });
      await loaded();
      expect(screen.queryByTestId('create-asset'), page).toBeNull();
      view.unmount();
    }
  });

  it('Swap: every venue of the pair quoted; the route is the node\'s best, said in plain words, never picked from a list', async () => {
    dexOn = true;
    pools.push({ ...pools[0], pool: id('s'), reserve_a: 0, reserve_b: 0, swaps: 0, curve: 'book', fee: 0, default_kind: false });
    render(Dex);
    render(DialogHost);
    await loaded();
    await waitFor(() => expect((document.querySelector('#swap-get') as HTMLElement).dataset.value).toBe(GOLD));
    await fireEvent.input(document.querySelector('#swap-amount')!, { target: { value: '0.5' } });
    await waitFor(() => expect(screen.getByTestId('swap-venue').textContent).toMatch(/GOLD \/ ECX/));
    expect(called('getswapquote').at(-1)?.params).toEqual(['ECX', '0.5', assetArg(GOLD), { max_hops: 0 }]);
    expect(called('quoteroute').length).toBeGreaterThan(0);
    // No list of ways to trade, no pool kinds: the best venue (here the one that gives 5.2) is used.
    expect(screen.queryByRole('list', { name: 'Ways to trade' })).toBeNull();
    expect(document.body.textContent).not.toMatch(/Constant product|Concentrated|Ways to trade/);
    await fireEvent.click(screen.getByRole('button', { name: /^Swap$/ }));
    await answer('yes');
    await waitFor(() => expect(called('swappools')[0]?.params).toEqual(['ECX', '0.5', [id('s')], 0.5, '5.17']));
  });

  it('the one pool form is refused for an asset on its launch curve, with the reason, before any confirmation (consensus: bad-ba-launch-pooled)', async () => {
    dexOn = true;
    mine.assets.push({ asset: MOON, label: 'MOON', decimals: 6, balance: 5000, pending: 0, outputs: 1, control: false, control_pending: false, supply: 1000000000, fixed: true });
    // Asked for by its route (an old link, a typed one): the box says why not; nothing to type, nothing to confirm.
    nav.tab = `asset/${MOON}/liquidity`;
    const shown = render(Dex, { props: { page: 'assets' } });
    render(DialogHost);
    await loaded();
    const box = await screen.findByTestId('add-liquidity-box');
    await waitFor(() => expect(box.dataset.mode).toBe('refused'));
    expect(within(box).getByTestId('liquidity-refused').textContent).toMatch(/on its launch curve: its pool opens by itself at graduation/);
    expect(box.querySelector('input, select, button[type="submit"]')).toBeNull();
    expect(screen.queryByTestId('add-liquidity')).toBeNull();
    shown.unmount();
    // A page whose lists are behind the chain (it does not know of the curve yet): the form asks the node again
    // before the wallet, and stops there.
    render(AddLiquidity, { props: { asset: MOON, pools: [], onCurve: false } });
    await fireEvent.input(document.querySelector('#al-asset')!, { target: { value: '100' } });
    await fireEvent.input(document.querySelector('#al-ecx')!, { target: { value: '1' } });
    const submit = () => screen.getByTestId('add-liquidity-box').querySelector('button[type="submit"]') as HTMLButtonElement;
    await waitFor(() => expect(submit().disabled).toBe(false));
    await fireEvent.click(submit());
    await waitFor(() => expect(document.querySelector('dialog[open]')?.textContent).toMatch(/on its launch curve/));
    expect(called('createpool')).toHaveLength(0);
    expect(called('addliquidity')).toHaveLength(0);
    expect(asked).toEqual([]);
  });

  it('New pool: the asset picked is the asset of the form; a standard pool at a fee tier, made and funded in one transaction', async () => {
    dexOn = true;
    nav.tab = 'new';
    render(Dex, { props: { page: 'pools' } });
    render(DialogHost);
    await loaded();
    await fireEvent.click(await screen.findByTestId('new-pool-asset'));
    // SILVER's page, its Add liquidity box in place of Trade: the asset fixed, nothing to pick.
    expect([nav.page, nav.tab]).toEqual(['assets', `asset/${SILVER}/liquidity`]);
    const box = await screen.findByTestId('add-liquidity-box');
    await waitFor(() => expect(box.dataset.mode).toBe('create'));
    expect(box.dataset.asset).toBe(SILVER);
    expect(within(box).getByRole('heading', { name: /Add liquidity · SILVER \/ ECX/ })).toBeTruthy();
    expect(box.querySelector('[role="combobox"]')).toBeNull();
    expect(box.textContent).not.toMatch(/Custom range|Concentrated|Constant product/);
    await fireEvent.input(document.querySelector('#al-asset')!, { target: { value: '30' } });
    await fireEvent.input(document.querySelector('#al-ecx')!, { target: { value: '0.001' } });
    await waitFor(() => expect(box.textContent).toMatch(/Too little: a pool opens with at least 0\.01 ECX/));
    await fireEvent.input(document.querySelector('#al-ecx')!, { target: { value: '5' } });
    await waitFor(() => expect(called('previewpool').at(-1)?.params).toEqual([assetArg(SILVER), '30', 'ECX', '5', { curve: 'constant_product', fee: 0.3 }]));
    await fireEvent.click(within(box).getByRole('button', { name: '1%' }));
    await waitFor(() => expect(screen.getByTestId('liquidity-summary').textContent).toMatch(/Price\s*0\.166667 ECX per SILVER.*Your pool share\s*100.*You earn\s*1% of each swap/));
    const submit = box.querySelector('button[type="submit"]') as HTMLButtonElement;
    await waitFor(() => expect(submit.disabled).toBe(false));
    await fireEvent.click(submit);
    await waitFor(() => expect(document.querySelector('dialog[open]')?.textContent).toMatch(/Make the pool of \$SILVER #4218-0372 and ECX and put 30 \$SILVER #4218-0372 and 5 ECX in it\?/));
    expect(document.querySelector('dialog[open]')?.textContent).toMatch(/One transaction, one confirmation/);
    await answer('yes');
    await waitFor(() => expect(called('createpool')[0]?.params).toEqual([assetArg(SILVER), '30', 'ECX', '5', { curve: 'constant_product', fee: 1 }, 1]));
    expect(asked).toEqual(['createpool']);
    expect(called('openposition')).toHaveLength(0);
  });

  it('a pool with nothing in it: Add liquidity takes the place of Trade; one concentrated on the chain takes the full range, never a range to pick', async () => {
    dexOn = true;
    // SILVER's pool was made empty at 0.01 ECX (a concentrated one, as the app made them before 2026-11-11).
    clPool(id('e'), { asset_a: 'SILVER', asset_a_id: SILVER, reserve_a: 0, reserve_b: 0, price: 0.01, swaps: 0, concentrated: { ...CL, liquidity: '0' } });
    nav.tab = `asset/${SILVER}`;
    render(Dex, { props: { page: 'assets' } });
    render(DialogHost);
    await loaded();
    await waitFor(() => expect(screen.getByTestId('asset-page').dataset.market).toBe('pool-empty'));
    expect(screen.getByTestId('market-chip').textContent).toBe('Pool, empty');
    expect(screen.getByTestId('asset-box').dataset.box).toBe('liquidity');
    expect(screen.queryByTestId('trade-box')).toBeNull();
    const row = within(screen.getByTestId('asset-markets')).getByTestId('market-row');
    expect(row.dataset.kind).toBe('pool-empty');
    expect(row.textContent).toMatch(/Nothing in it yet\.\s*Its starting price is 0\.01 ECX for 1 SILVER\..*Needs liquidity/);
    const box = screen.getByTestId('add-liquidity-box');
    expect(box.dataset.mode).toBe('full-range');
    await fireEvent.input(document.querySelector('#al-asset')!, { target: { value: '20' } });
    await fireEvent.input(document.querySelector('#al-ecx')!, { target: { value: '0.2' } });
    await waitFor(() => expect(called('previewposition').at(-1)?.params).toEqual([id('e'), 0, 0, { [assetArg(SILVER)]: '20', ECX: '0.2' }]));
    expect(document.body.textContent).not.toMatch(/Custom range|Full range|Lower price|Upper price|Concentrated/);
    expect(document.querySelector('#cl-lower')).toBeNull();
    const submit = box.querySelector('button[type="submit"]') as HTMLButtonElement;
    await waitFor(() => expect(submit.disabled).toBe(false));
    await fireEvent.click(submit);
    await answer('yes');
    // Prices of 0: the full range; with the pool's price, in the node's order (ECX in SILVER: 100), for the wallet to check.
    await waitFor(() => expect(called('openposition')[0]?.params).toEqual([id('e'), 0, 0, { [assetArg(SILVER)]: '20', ECX: '0.2' }, 1, 100]));
    expect(asked).toEqual(['openposition']);
  });

  it('a pool\'s page: its figures, what is in it, Your share with Add liquidity (the asset fixed), Remove liquidity and Collect fees', async () => {
    dexOn = true;
    nav.tab = `pools/${id('p')}`;
    let shown = render(Dex, { props: { page: 'pools' } });
    render(DialogHost);
    await loaded();
    const page = await screen.findByTestId('pool-page');
    expect(screen.getByTestId('pool-figures').textContent).toMatch(/Liquidity\s*50\s*ECX in the pool.*Volume 24 h\s*–.*Swaps\s*4.*Price\s*0\.1/);
    expect(screen.getByTestId('pool-holds').textContent).toMatch(/GOLD\s*500.*ECX\s*50/);
    // One kind of pool: no curve, no depth, no range.
    expect(page.textContent).not.toMatch(/Constant product|Concentrated|Depth|The curve|range/i);
    const share = screen.getByTestId('pool-share');
    expect(share.textContent).toMatch(/Share of the pool\s*6\.32%.*Worth\s*31\.62 GOLD · 3\.162 ECX/);
    expect(within(share).getByRole('button', { name: 'Collect fees' })).toHaveProperty('disabled', true);
    // Add liquidity: the one form, on this pool's asset, at the pool's price, by the pool's id.
    await fireEvent.click(within(share).getByRole('button', { name: 'Add liquidity' }));
    const box = await screen.findByTestId('add-liquidity-box');
    expect([box.dataset.mode, box.dataset.asset]).toEqual(['add', GOLD]);
    expect(box.querySelector('[role="combobox"]')).toBeNull();
    await fireEvent.input(document.querySelector('#al-asset')!, { target: { value: '10' } });
    await waitFor(() => expect(called('quotepoolliquidity').at(-1)?.params).toEqual([id('p'), { [assetArg(GOLD)]: '10' }]));
    const submit = box.querySelector('button[type="submit"]') as HTMLButtonElement;
    await waitFor(() => expect(submit.disabled).toBe(false));
    await fireEvent.click(submit);
    await answer('yes');
    await waitFor(() => expect(called('addpoolliquidity')[0]?.params).toEqual([id('p'), { [assetArg(GOLD)]: '10' }, 1]));
    // Remove liquidity: a part of the share, by the pool's id.
    await fireEvent.click(within(share).getByRole('button', { name: 'Remove liquidity' }));
    await fireEvent.click(within(screen.getByTestId('remove-liquidity')).getByRole('button', { name: '50%' }));
    await fireEvent.click(within(screen.getByTestId('remove-liquidity')).getByRole('button', { name: 'Remove' }));
    await answer('yes');
    await waitFor(() => expect(called('removepoolliquidity')[0]?.params).toEqual([id('p'), 50, null, 1]));
    shown.unmount();
    // A pool that came from a launch is concentrated on the chain: shown as any pool; the wallet's share is its
    // position over the full range, whose fees are collected and which is removed by the position.
    clPool(id('g'), { asset_a: 'STAR', asset_a_id: STAR, reserve_a: 1000, reserve_b: 20, price: 0.02 });
    positions = [{ position: id('f'), pool: id('g'), asset0: 'CHN', asset1: 'STAR', tick_lower: -887220, tick_upper: 887220, price_lower: 0, price_upper: 1e30, liquidity: '1000000', in_range: true, amount0: 4, amount1: 200, fees0: 0.01, fees1: 0.5 }];
    nav.tab = `pools/${id('g')}`;
    shown = render(Dex, { props: { page: 'pools' } });
    await refreshBitAssets();
    const own = await screen.findByTestId('pool-share');
    await waitFor(() => expect(own.textContent).toMatch(/Share of the pool\s*20\.0%.*Worth\s*200 STAR · 4 ECX.*not collected\s*0\.5 STAR · 0\.01 ECX/));
    expect(screen.getByTestId('pool-page').textContent).not.toMatch(/Concentrated|range/i);
    await waitFor(() => expect(screen.getByTestId('pool-from-launch')).toBeTruthy());
    await fireEvent.click(within(own).getByRole('button', { name: 'Collect fees' }));
    await answer('yes');
    await waitFor(() => expect(called('changeposition')[0]?.params).toEqual([id('f'), { collect: true }]));
    await fireEvent.click(within(own).getByRole('button', { name: 'Remove liquidity' }));
    await fireEvent.click(within(screen.getByTestId('remove-liquidity')).getByRole('button', { name: 'Remove' }));
    await answer('yes');
    await waitFor(() => expect(called('changeposition').at(-1)?.params).toEqual([id('f'), { remove: 100, collect: true }]));
    await waitFor(() => expect(screen.queryByTestId('remove-liquidity')).toBeNull());
    // Adding to it: over the full range, at its price.
    await fireEvent.click(within(own).getByRole('button', { name: 'Add liquidity' }));
    await waitFor(() => expect(screen.getByTestId('add-liquidity-box').dataset.mode).toBe('full-range'));
  });

  it('before the pool kinds: a pool is made by addliquidity with both amounts, which set the price (audit 7 E-BA1)', async () => {
    nav.tab = `asset/${SILVER}/liquidity`;
    const shown = render(Dex, { props: { page: 'assets' } });
    render(DialogHost);
    await loaded();
    const box = await screen.findByTestId('add-liquidity-box');
    await waitFor(() => expect(box.dataset.mode).toBe('create'));
    expect(within(box).queryByRole('group', { name: 'Fee tier' })).toBeNull();
    await fireEvent.input(document.querySelector('#al-asset')!, { target: { value: '30' } });
    await fireEvent.input(document.querySelector('#al-ecx')!, { target: { value: '5' } });
    const submit = () => screen.getByTestId('add-liquidity-box').querySelector('button[type="submit"]') as HTMLButtonElement;
    await waitFor(() => expect(submit().disabled).toBe(false));
    await fireEvent.click(submit());
    await answer('yes');
    await waitFor(() => expect(called('addliquidity')[0]?.params).toEqual([assetArg(SILVER), '30', 'ECX', '5', 1, true]));
    shown.unmount();
    // Someone makes the pool while a form that knows of none is filled in: what it showed (a new pool, at this price) is
    // no longer true, so nothing is sent.
    pools.push({ ...pools[0], pool: id('q'), asset_a: 'SILVER', asset_a_id: SILVER });
    render(AddLiquidity, { props: { asset: SILVER, pools: [], onCurve: false } });
    await fireEvent.input(document.querySelector('#al-asset')!, { target: { value: '30' } });
    await fireEvent.input(document.querySelector('#al-ecx')!, { target: { value: '5' } });
    await waitFor(() => expect(submit().disabled).toBe(false));
    await fireEvent.click(submit());
    await waitFor(() => expect(document.querySelector('dialog[open]')?.textContent).toMatch(/changed while you typed/));
    expect(called('addliquidity')).toHaveLength(1);
  });

  it('Launch, straight into a pool you fund: reserved and registered in one question, then a standard pool made and funded in one step, on the user\'s word', async () => {
    dexOn = true;
    nav.tab = 'fund';
    render(Dex);
    render(DialogHost);
    await loaded();
    expect(screen.getByRole('heading', { name: 'Create asset' })).toBeTruthy();
    await fireEvent.input(screen.getByLabelText('Its name'), { target: { value: 'COPPER' } });
    await waitFor(() => expect(screen.getByText(/is free/)).toBeTruthy());
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    await fireEvent.input(document.querySelector('#issue-supply')!, { target: { value: '500' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    // The three ways, each open here; straight into a pool picked.
    for (const way of [/No market yet/, /Launch on a curve/, /Straight into a pool/]) expect((screen.getByRole('radio', { name: way }) as HTMLButtonElement).disabled).toBe(false);
    expect(screen.getByRole('radio', { name: /Straight into a pool/ }).getAttribute('aria-checked')).toBe('true');
    await fireEvent.input(document.querySelector('#fund-tokens')!, { target: { value: '100' } });
    await fireEvent.input(document.querySelector('#fund-chn')!, { target: { value: '0.00001' } });
    // The least ECX is the node's (getbitassetsinfo), never a number of the page.
    await waitFor(() => expect(screen.getByText(/Too little: a pool opens with at least 0\.01 ECX/)).toBeTruthy());
    expect((screen.getByRole('button', { name: 'Next' }) as HTMLButtonElement).disabled).toBe(true);
    await fireEvent.input(document.querySelector('#fund-chn')!, { target: { value: '2' } });
    await waitFor(() => expect(screen.getByTestId('fund-price').textContent).toMatch(/= 0\.02\s*ECX/));
    await fireEvent.click(screen.getByRole('button', { name: '1%' }));
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    expect(screen.getByTestId('fund-review').textContent).toMatch(/100\s*COPPER and 2\s*ECX, at a fee of 1%/);
    expect(screen.getByTestId('fund-story').textContent).toMatch(/the pool, made and funded in one step, confirmed once the asset is registered/);
    await fireEvent.click(screen.getByRole('button', { name: /Reserve and issue/ }));
    await waitFor(() => expect(asked).toEqual(['reserveasset+registerasset']));
    const F = 'f'.repeat(64);
    await waitFor(() => expect(store).toMatchObject({ pending: [{ name: 'COPPER', asset: F }], funding: [{ name: 'COPPER', asset: F, quote: CHN_ID, amount: '100', quoteAmount: '2', fee: 1, standard: true, slippage: 1 }] }));
    await waitFor(() => expect(screen.getByTestId('fund-card').dataset.stage).toBe('asset'));
    // Registered (in a block): its pool on the user's word, never by itself.
    assets.push(asset(F, { label: 'COPPER', name: 'COPPER', seq: '5555-0001', decimals: 8, supply: 500, minted: 500 }));
    mine.assets.push({ asset: F, label: 'COPPER', decimals: 8, balance: 500, pending: 0, outputs: 1, control: true, control_pending: false, supply: 500, fixed: false });
    await refreshBitAssets();
    await waitFor(() => expect(screen.getByTestId('fund-card').dataset.stage).toBe('pool'));
    expect(called('createpool')).toHaveLength(0);
    await fireEvent.click(screen.getByRole('button', { name: 'Make and fund its pool' }));
    // A standard pool at the tier chosen, both amounts in, in that one transaction; previewed first.
    await waitFor(() => expect(called('createpool')[0]?.params).toEqual([assetArg(F), '100', 'ECX', '2', { curve: 'constant_product', fee: 1 }, 1]));
    expect(called('previewpool').at(-1)?.params).toEqual([assetArg(F), '100', 'ECX', '2', { curve: 'constant_product', fee: 1 }]);
    await waitFor(() => expect(screen.getByTestId('fund-card').dataset.stage).toBe('done'));
    // One step: no position to open after it, nothing more to confirm.
    await refreshBitAssets();
    expect(called('openposition')).toHaveLength(0);
    expect(asked).toEqual(['reserveasset+registerasset', 'createpool']);
    expect(screen.queryByRole('button', { name: 'Put in your liquidity' })).toBeNull();
  });

  it('the asset page, on its curve: where it trades in a chip, the curve and "its pool opens by itself" in Markets, the Trade box on the curve, the asset fixed', async () => {
    dexOn = true;
    candles = [{ t: 1_700_000_000, o: 0.0000008, h: 0.00000082, l: 0.0000008, c: 0.00000081, v: 3 }, { t: 1_700_003_600, o: 0.00000081, h: 0.0000009, l: 0.00000081, c: 0.00000088, v: 2 }];
    mine.assets.push({ asset: MOON, label: 'MOON', decimals: 6, balance: 5000, pending: 0, outputs: 1, control: false, control_pending: false, supply: 1000000000, fixed: true });
    nav.tab = `asset/${MOON}`;
    render(Dex, { props: { page: 'assets' } });
    render(DialogHost);
    await loaded();
    const page = await screen.findByTestId('asset-page');
    await waitFor(() => expect(page.dataset.market).toBe('curve'));
    expect(screen.getByTestId('market-chip').textContent).toBe('On curve 40.0%');
    expect(screen.getByTestId('asset-price').textContent).toMatch(/0\.00000081 ECX/);
    // The sections, in their order.
    const order = ['asset-markets', 'asset-position', 'asset-manage', 'asset-activity', 'asset-details'].map((x) => screen.getByTestId(x));
    for (let i = 1; i < order.length; i++) expect(order[i - 1].compareDocumentPosition(order[i]) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
    // Markets: the curve, with its progress and fee; its pool at graduation, which nobody can open before.
    const rows = within(screen.getByTestId('asset-markets')).getAllByTestId('market-row');
    expect(rows.map((r) => r.dataset.kind)).toEqual(['curve', 'pool-at-graduation']);
    expect(rows[0].textContent).toMatch(/Launch curve.*1% fee.*200 of 500 ECX raised/);
    expect(rows[1].textContent).toMatch(/Opens by itself at graduation.*Nobody can open one before then/);
    expect(screen.queryByTestId('add-liquidity')).toBeNull();
    // The figures of a day from the node's candles: +10%, 5 ECX.
    await waitFor(() => expect(screen.getByTestId('asset-stats').textContent).toMatch(/24 h change\s*\+10\.0%.*24 h volume\s*5 ECX.*Market cap\s*810 ECX/));
    // The Trade box: no asset to pick, the route is the curve, its fee, impact and slippage limit.
    const box = screen.getByTestId('trade-box');
    expect(box.dataset.route).toBe('curve');
    expect(box.querySelector('[role="combobox"], select')).toBeNull();
    await fireEvent.input(document.querySelector('#launch-amount')!, { target: { value: '2' } });
    await waitFor(() => expect(screen.getByTestId('launch-quote').textContent).toMatch(/Route\s*Launch curve.*Price impact\s*1\.20%.*Fee \(1%\)/));
    expect(within(box).getByText('Slippage limit')).toBeTruthy();
    await fireEvent.click(box.querySelector('button[type="submit"]')!);
    await answer('yes');
    await waitFor(() => expect(called('buyfromlaunch')[0]?.params).toEqual([assetArg(MOON), '2', 1, '2444444.444444']));
    // Selling: the same box, the same asset.
    await fireEvent.click(within(box).getByRole('button', { name: 'Sell', pressed: false }));
    expect(screen.getByLabelText('You sell')).toBeTruthy();
    // Activity: its trades block by block; Details: its launch.
    expect(within(screen.getByTestId('asset-activity')).getByRole('table', { name: 'Recent blocks' })).toBeTruthy();
    expect(screen.getByTestId('launch-facts').textContent).toMatch(/Launched\s*block 12/);
    // The creator is this wallet: Manage, with the fees to claim.
    expect(within(screen.getByTestId('asset-manage')).getByTestId('claim-fees')).toHaveProperty('disabled', false);
  });

  it('the asset page, in a pool: buy and sell by the best route, the asset fixed (no picker); Your position; Manage only for its controller', async () => {
    nav.tab = `asset/${GOLD}`;
    const shown = render(Dex, { props: { page: 'assets' } });
    render(DialogHost);
    await loaded();
    await waitFor(() => expect(screen.getByTestId('asset-page').dataset.market).toBe('pool'));
    expect(screen.getByTestId('market-chip').textContent).toBe('Pool');
    expect(screen.getByTestId('asset-price').textContent).toMatch(/0\.1 ECX/);
    // A node without the market index: no figure of a day is made up.
    expect(screen.getByTestId('asset-stats').textContent).toMatch(/24 h change\s*–.*24 h volume\s*–/);
    const rows = within(screen.getByTestId('asset-markets')).getAllByTestId('market-row');
    expect(rows.map((r) => r.dataset.kind)).toEqual(['pool']);
    expect(rows[0].textContent).toMatch(/Pool GOLD \/ ECX.*0\.3% fee.*50 ECX of liquidity/);
    // The box: Trade GOLD, against ECX; nothing to pick.
    const box = screen.getByTestId('trade-box');
    expect(within(box).getByRole('heading', { name: /Trade GOLD/ })).toBeTruthy();
    expect(document.querySelector('#swap-pay')).toBeNull();
    expect(document.querySelector('#swap-get')).toBeNull();
    expect(box.querySelector('[role="combobox"], select')).toBeNull();
    await fireEvent.input(document.querySelector('#swap-amount')!, { target: { value: '0.5' } });
    await waitFor(() => expect(screen.getByTestId('swap-venue').textContent).toMatch(/Pool GOLD \/ ECX · 0\.3%/));
    expect(box.textContent).toMatch(/Price impact.*Slippage limit|Slippage limit.*Price impact/s);
    await fireEvent.click(box.querySelector('button[type="submit"]')!);
    await answer('yes');
    await waitFor(() => expect(called('swapasset')[0]?.params).toEqual(['ECX', '0.5', assetArg(GOLD), 0.5, 10, '4.9']));
    // Sell: the asset is paid, ECX got.
    await fireEvent.click(within(box).getByRole('button', { name: 'Sell', pressed: false }));
    await fireEvent.input(document.querySelector('#swap-amount')!, { target: { value: '10' } });
    await waitFor(() => expect(called('quoteswap').at(-1)?.params).toEqual([assetArg(GOLD), '10', 'ECX']));
    // Your position; Manage, for its controller (mint, data, burn; no market to open: it has one).
    expect(screen.getByTestId('asset-position').textContent).toMatch(/Balance\s*200 GOLD.*Worth\s*20 ECX.*Pool share\s*6\.32%/);
    const manage = screen.getByTestId('asset-manage');
    expect(within(manage).getByRole('button', { name: /Mint more/ })).toBeTruthy();
    expect(within(manage).getByRole('button', { name: /Burn some/ })).toBeTruthy();
    expect(screen.queryByTestId('open-market')).toBeNull();
    // The pool's row opens the pool's page.
    await fireEvent.click(rows[0].querySelector('button.link-row')!);
    expect([nav.page, nav.tab]).toEqual(['pools', `pools/${id('p')}`]);
    shown.unmount();
    // Another asset's page starts clean: SILVER is not controlled (no Manage), has no market (no Trade box).
    nav.tab = `asset/${SILVER}`;
    render(Dex, { props: { page: 'assets' } });
    await waitFor(() => expect(screen.getByTestId('market-chip').textContent).toBe('No market'));
    expect(screen.getByTestId('asset-page').dataset.market).toBe('none');
    expect(screen.getByTestId('asset-price').textContent).toMatch(/No price yet/);
    expect(screen.queryByTestId('asset-manage')).toBeNull();
    expect(screen.queryByTestId('trade-box')).toBeNull();
    expect(screen.getByTestId('no-market')).toBeTruthy();
    expect(within(screen.getByTestId('asset-markets')).getByTestId('market-row').dataset.kind).toBe('none');
    // A holder that does not control it can still burn its own.
    expect(within(screen.getByTestId('asset-position')).getByRole('button', { name: /Burn some/ })).toBeTruthy();
  });

  it('a form never keeps an asset from before: the Trade box follows the page\'s asset, its amount cleared', async () => {
    pools.push({ ...pools[0], pool: id('q'), asset_a: 'SILVER', asset_a_id: SILVER });
    nav.tab = `asset/${GOLD}/sell`;
    render(Dex, { props: { page: 'assets' } });
    await loaded();
    const amount = () => document.querySelector('#swap-amount') as HTMLInputElement;
    await waitFor(() => expect(within(screen.getByTestId('trade-box')).getByRole('heading', { name: /Trade GOLD/ })).toBeTruthy());
    await fireEvent.input(amount(), { target: { value: '7' } });
    await waitFor(() => expect(called('quoteswap').at(-1)?.params).toEqual([assetArg(GOLD), '7', 'ECX']));
    // The other asset's page, by the route (a link, a row): its own box, on Buy, empty.
    nav.tab = `asset/${SILVER}`;
    await waitFor(() => expect(within(screen.getByTestId('trade-box')).getByRole('heading', { name: /Trade SILVER/ })).toBeTruthy());
    expect(amount().value).toBe('');
    expect(within(screen.getByTestId('trade-box')).getByRole('button', { name: 'Buy', pressed: true })).toBeTruthy();
    await fireEvent.input(amount(), { target: { value: '1' } });
    await waitFor(() => expect((called('quoteswap').at(-1)?.params as unknown[])[2]).toBe(assetArg(SILVER)));
    expect(called('quoteswap').some((c) => (c.params as unknown[])[0] === assetArg(GOLD) && (c.params as unknown[])[1] === '1')).toBe(false);
  });

  it('New pool: only the assets held that have no pool and are not on a launch curve; the others with why; a pick opens that asset\'s page, the asset fixed', async () => {
    dexOn = true;
    // GOLD has its pool; MOON is held and on its curve; SILVER is held, with no pool.
    mine.assets.push({ asset: MOON, label: 'MOON', decimals: 6, balance: 1000, pending: 0, outputs: 1, control: false, control_pending: false, supply: 1000000000, fixed: true });
    nav.tab = 'new';
    render(Dex, { props: { page: 'pools' } });
    await loaded();
    const box = await screen.findByTestId('new-pool');
    await waitFor(() => expect(within(box).getAllByTestId('new-pool-refused').map((x) => [x.dataset.id, x.dataset.why])).toEqual([[GOLD, 'pool'], [MOON, 'curve']]));
    expect(within(box).getAllByTestId('new-pool-asset').map((x) => x.dataset.id)).toEqual([SILVER]);
    expect(within(box).getByText(/On its launch curve: its pool opens by itself at graduation/)).toBeTruthy();
    // No form here: no amount to type, no asset to select.
    expect(box.querySelector('input, select, [role="combobox"]')).toBeNull();
    await fireEvent.click(within(box).getByTestId('new-pool-asset'));
    expect([nav.page, nav.tab]).toEqual(['assets', `asset/${SILVER}/liquidity`]);
  });

  it('v5 fee flow: where the creator fees go, the control coin kept only on an explicit word, the graduation reviewed', async () => {
    dexOn = true;
    tickersOn = true;
    feeFlowOn = true;
    nav.tab = 'launch';
    render(Dex);
    render(DialogHost);
    await loaded();
    await fireEvent.click(await screen.findByRole('button', { name: /IRON/ }));
    await waitFor(() => expect(screen.getByText(/Reserved by this wallet/)).toBeTruthy());
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    // Creator (the default), Buy back & burn, Split with its burn.
    const dest = screen.getByRole('radiogroup', { name: 'Creator fees go to' });
    expect(within(dest).getAllByRole('radio').map((r) => [r.textContent, r.getAttribute('aria-checked')])).toEqual([['Creator', 'true'], ['Buy back & burn', 'false'], ['Split', 'false']]);
    await fireEvent.click(within(dest).getByRole('radio', { name: 'Split' }));
    await fireEvent.input(document.querySelector('#launch-burn')!, { target: { value: '100' } });
    const next = () => screen.getByRole('button', { name: 'Next' }) as HTMLButtonElement;
    await waitFor(() => expect(next().disabled).toBe(true));
    await fireEvent.input(document.querySelector('#launch-burn')!, { target: { value: '30' } });
    await waitFor(() => expect(next().disabled).toBe(false));
    // Keep control coin: off by default; on, nothing goes on before the warning is confirmed.
    const keep = screen.getByRole('checkbox', { name: 'Keep control coin' }) as HTMLInputElement;
    expect(keep.checked).toBe(false);
    await fireEvent.click(keep);
    const confirm = await screen.findByRole('checkbox', { name: "Unlocked Supply: you can create more units at any time. Its pages and its buyers' confirmations show the tag Unlocked Supply." });
    await waitFor(() => expect(next().disabled).toBe(true));
    await fireEvent.click(confirm);
    await waitFor(() => expect(next().disabled).toBe(false));
    await fireEvent.click(next());
    const review = await screen.findByTestId('launch-review');
    expect(review.textContent).toMatch(/30% bought back and burned, 70% to the creator/);
    expect(review.textContent).toMatch(/Unlocked Supply/);
    expect(screen.getByTestId('launch-graduation-review').textContent).toBe('A locked full-range position at 1 %: its fees go 60 % to the creator and 40 % to network security. All fees are taken in ECX.');
    await fireEvent.click(screen.getByRole('button', { name: /^Launch$/ }));
    await answer('yes');
    await waitFor(() => expect(called('launchtoken')[0]?.params).toEqual(['IRON', { fee_destination: 'split', burn_percent: 30, keep_control: true }]));
  });

  it('v7 fee split: all to the creator by default (no fee_split sent); burn and an address, each share in percent of the part and of each trade, reviewed in full, sent in bps', async () => {
    dexOn = true;
    tickersOn = true;
    feeFlowOn = true;
    splitOn = true;
    nav.tab = 'launch';
    render(Dex);
    render(DialogHost);
    await loaded();
    await fireEvent.click(await screen.findByRole('button', { name: /IRON/ }));
    await waitFor(() => expect(screen.getByText(/Reserved by this wallet/)).toBeTruthy());
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    await waitFor(() => expect(screen.getByTestId('fee-split-editor')).toBeTruthy());
    expect(screen.queryByRole('radiogroup', { name: 'Creator fees go to' })).toBeNull();
    expect(screen.getByTestId('fee-split-creator').textContent).toMatch(/You: 100%/);
    const next = () => screen.getByRole('button', { name: 'Next' }) as HTMLButtonElement;
    await fireEvent.input(document.querySelector('#launch-split-burn')!, { target: { value: '10' } });
    await fireEvent.click(screen.getByRole('button', { name: 'Add an address' }));
    await waitFor(() => expect(next().disabled).toBe(true));
    await fireEvent.input(document.querySelector('#launch-split-addr-0')!, { target: { value: OTHER } });
    await fireEvent.input(document.querySelector('#launch-split-pct-0')!, { target: { value: '25' } });
    await waitFor(() => expect(next().disabled).toBe(false));
    expect(screen.getByTestId('fee-split-creator').textContent).toMatch(/You: 65%/);
    await fireEvent.click(next());
    const review = await screen.findByTestId('launch-split-review');
    expect(review.textContent).toContain(OTHER);
    expect(review.textContent).toMatch(/The creator: 65% of the tunable part/);
    expect(review.textContent).toMatch(/Buyback & burn: 10% of the tunable part/);
    expect(screen.getByTestId('launch-graduation-review').textContent).toMatch(/to the split above/);
    await fireEvent.click(screen.getByRole('button', { name: /^Launch$/ }));
    await answer('yes');
    await waitFor(() => expect(called('launchtoken')[0]?.params).toEqual(['IRON', expect.objectContaining({ fee_split: [{ to: 'creator', bps: 6500 }, { to: OTHER, bps: 2500 }, { to: 'burn', bps: 1000 }] })]));
    expect((called('launchtoken')[0]?.params as unknown[])[1]).not.toHaveProperty('fee_destination');
  });

  it('v7 fee split: a launch page lists each share, paid and owed', async () => {
    dexOn = true;
    feeFlowOn = true;
    splitOn = true;
    launchPatch = { fee_flow: true, creator_share: 75, fee_split: [{ to: 'creator', kind: 'creator', bps: 7500, paid: 0.5, owed: 0.25 }, { to: OTHER, kind: 'address', bps: 2500, paid: 0.125, owed: 0 }] };
    nav.page = 'pools';
    nav.tab = `launches/${MOON}/buy`;
    render(Dex, { props: { page: 'pools' } });
    await loaded();
    await waitFor(() => expect(screen.getAllByTestId('launch-fee-share')).toHaveLength(2));
    const [c, a] = screen.getAllByTestId('launch-fee-share').map((x) => x.textContent ?? '');
    expect(c).toMatch(/^The creator: 75% of the tunable part \(0\.5625% of each trade\) · 0\.5 ECX paid · 0\.25 ECX owed$/);
    expect(a).toContain(OTHER);
    expect(a).toMatch(/25% of the tunable part .* 0\.125 ECX paid$/);
    expect(screen.queryByTestId('launch-fee-destination')).toBeNull();
  });

  it('v5 fee flow: before its height, no fee-flow options', async () => {
    dexOn = true;
    nav.tab = 'launch';
    render(Dex);
    await loaded();
    await fireEvent.click(await screen.findByRole('button', { name: /IRON/ }));
    await waitFor(() => expect(screen.getByText(/Reserved by this wallet/)).toBeTruthy());
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    await fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    await waitFor(() => expect(document.querySelector('#launch-allocation')).not.toBeNull());
    expect(screen.queryByRole('radiogroup', { name: 'Creator fees go to' })).toBeNull();
    expect(screen.queryByRole('checkbox', { name: 'Keep control coin' })).toBeNull();
  });

  it('v5 fee flow: a launch page says where the fees go, what was burned, the tokens owed in Claim, its pool once graduated; Unlocked Supply on its own pages and in the buy confirmation', async () => {
    dexOn = true;
    feeFlowOn = true;
    launchPatch = { fee_flow: true, fee_destination: 'split', burn_percent: 40, mintable: true, burned: '1234.5', creator_tokens_owed: '10', creator_fees_owed: '0', pool_kind: 'concentrated' };
    nav.page = 'pools';
    nav.tab = `launches/${MOON}/buy`;
    let shown = render(Dex, { props: { page: 'pools' } });
    render(DialogHost);
    await loaded();
    await waitFor(() => expect(screen.getByTestId('launch-fee-destination').textContent).toBe('40% bought back and burned, 60% to the creator · 1,234.5 MOON burned'));
    const tag = screen.getAllByTestId('unlocked-supply')[0];
    expect(tag.textContent?.trim()).toBe('Unlocked Supply');
    expect(tag.getAttribute('title')).toBe('The creator can still mint more');
    expect(tag.className).not.toMatch(/danger|warn/);
    // Buying it: the confirmation carries the tag.
    await fireEvent.input(document.querySelector('#launch-amount')!, { target: { value: '2' } });
    await waitFor(() => expect(screen.getByTestId('launch-quote').textContent).toBeTruthy());
    await fireEvent.click(screen.getByTestId('trade-box').querySelector('button[type="submit"]')!);
    await waitFor(() => expect(screen.getByTestId('dialog-tag').textContent).toBe('Unlocked Supply'));
    await answer('no');
    expect(screen.getByTestId('launch-tokens-owed').textContent).toMatch(/^10 MOON/);
    // Not graduated: no pool kind yet. Claim covers the tokens owed (no fees owed).
    expect(screen.queryByTestId('launch-pool-kind')).toBeNull();
    // The creator is this wallet: Claim creator fees, in Manage.
    await fireEvent.click(within(screen.getByTestId('asset-manage')).getByTestId('claim-fees'));
    await waitFor(() => expect(document.querySelector('dialog[open]')?.textContent).toContain('and 10 tokens owed'));
    await answer('yes');
    await waitFor(() => expect(called('claimlaunch').length).toBe(1));
    shown.unmount();
    // The tag by id (its pool's page): from the launches read.
    const badge = render(UnlockedSupplyBadge, { id: MOON });
    expect(badge.getByTestId('unlocked-supply').textContent?.trim()).toBe('Unlocked Supply');
    badge.unmount();
    expect(render(UnlockedSupplyBadge, { id: SILVER }).queryByTestId('unlocked-supply')).toBeNull();
    // Graduated: its pool's kind.
    nav.tab = `launches/${STAR}/buy`;
    shown = render(Dex, { props: { page: 'pools' } });
    await loaded();
    await waitFor(() => expect(screen.getByTestId('launch-pool-kind').textContent).toBe('Locked full-range position, 1 %'));
    expect(screen.getByTestId('launch-graduated').textContent).toContain('Locked full-range position, 1 %');
  });
});
