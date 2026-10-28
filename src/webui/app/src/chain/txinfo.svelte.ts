// What BitAssets knows of a transaction of the wallet beyond ECX, for the Transactions page (pages/Transactions.svelte):
// the assets it moved (listassettransactions, read with the store's refresh: one call for the wallet, never one per row),
// what kind of move, how much, and the other side: where a send went, who paid what came in.
import { t } from '../lib/i18n.svelte';
import { wallets } from '../lib/state.svelte';
import { formatAmount, OPERATIONS, type AssetTx } from './ba/model';
import { ba, holdingOf, refreshBitAssets, startBitAssets } from './ba/store.svelte';
import { ignoreAsset } from './ba/hide.svelte';
import { hiddenAsset } from './ba/visible.svelte';
import { TICKER } from './config';
import { mergeByTxid, typeText, type TxRecord } from '../lib/wallet';

export interface ChainTx {
  type: string;
  search: string;
  csv: string[];
}

export const CHAIN_TX_FILTER = 'Assets';
export const CHAIN_TX_COLUMN = 'Asset';
export const CHAIN_TX_CSV = ['Asset', 'Asset number', 'Asset amount', 'Asset counterparty'];

/** The words of a kind of move. */
export function categoryText(e: Pick<AssetTx, 'category' | 'sign'>): string {
  switch (e.category) {
    case 'send':
      return t('Sent asset');
    case 'receive':
      return t('Received asset');
    case 'register':
      return t('Registered');
    case 'mint':
      return t('Minted');
    case 'burn':
      return t('Burned');
    case 'swap':
      return t('Swap');
    case 'pool_create':
      return t('Pool created');
    case 'pool_add':
      return t('Pool add');
    case 'pool_remove':
      return t('Pool remove');
    case 'position_open':
      return t('Position opened');
    case 'position_add':
      return t('Position added to');
    case 'position_remove':
      return t('Position reduced');
    case 'position_collect':
      return t('Position fees collected');
    case 'position_close':
      return t('Position closed');
    case 'order_place':
      return t('Order placed');
    case 'order_settle':
      return t('Order collected');
    case 'order_cancel':
      return t('Order cancelled');
    case 'launch':
      return t('Launched');
    case 'curve':
      return t('Put on a curve');
    case 'claim':
      return t('Launch claim');
    case 'contract':
      return t('Oracle contract');
    case 'contract_settle':
      return t('Contract settled');
    case 'oracle':
      return t('Oracle registered');
    case 'attest':
      return t('Attested');
    case 'auction':
      return t('Auction');
    case 'bid':
      return t('Auction bid');
    case 'collect':
      return t('Auction collect');
    case 'logo':
      return t('Logo set');
    case 'display_name':
      return t('Display name set');
    case 'update':
      return t('Asset data');
    case 'control':
      return e.sign > 0 ? t('Control received') : t('Control given');
    case 'reserve':
      return t('Name reserved');
    case 'release':
      return t('Asset retired');
    default:
      return t('Asset');
  }
}

/**
 * The operations of Layers' DEX that a node before 8.3 listed as moves of the assets (sent, received): named as
 * listassetactivity names them. Newer nodes give each its own category (categoryText).
 */
const DEX_OPERATIONS = new Set(['create pool', 'add liquidity', 'remove liquidity', 'route', 'buy', 'sell', 'launch', 'claim launch', 'open position', 'change position', 'place order', 'settle order', 'cancel order']);

/** The words of a move, with the operation listassetactivity says of its transaction (if read). */
export function operationText(e: Pick<AssetTx, 'category' | 'sign'>, operation: string | undefined): string {
  if (operation && DEX_OPERATIONS.has(operation) && OPERATIONS[operation] && (e.category === 'send' || e.category === 'receive')) return t(OPERATIONS[operation]);
  return categoryText(e);
}

/**
 * The kind of a transaction from its moves, for the Type column and the notices: a launch; a buy (ECX in, an asset out),
 * a sale (an asset in, ECX out), a swap, or one that did not fill (its refund); else its first move's kind.
 */
export function typeOfMoves(moves: readonly AssetTx[], operation: string | undefined): string {
  if (!moves.length) return '';
  if (moves.some((e) => e.category === 'launch')) return t('Launched');
  const swap = moves.filter((e) => e.category === 'swap');
  if (swap.length && swap.length === moves.filter((e) => e.category !== 'control').length) {
    if (swap.some((e) => e.refund)) return t('Swap not filled');
    if (swap.some((e) => e.coin && e.sign < 0)) return t('Buy');
    if (swap.some((e) => e.coin && e.sign > 0)) return t('Sell');
    return t('Swap');
  }
  const main = moves.find((e) => e.category !== 'control') ?? moves[0];
  return operationText(main, operation);
}

/** What a move's amount needs said beside it: paid by a coinbase, a refund, or why it is the least asked for. */
export function moveNotes(e: Pick<AssetTx, 'via' | 'refund' | 'unknown' | 'at_least' | 'at_most' | 'amount'>): string[] {
  const notes: string[] = [];
  if (e.refund) notes.push(t('refund'));
  if (e.via === 'coinbase') notes.push(t('paid by a coinbase'));
  if (e.unknown === 'pending') notes.push(t('not in a block yet'));
  else if (e.unknown === 'spent' && (e.at_least || e.amount === undefined)) notes.push(t('the least asked for: what came out was spent since'));
  else if (e.at_least) notes.push(t('at least'));
  if (e.at_most) notes.push(t('at most'));
  return notes;
}

/** The name a move shows: its asset's label, ECX for the coin. */
export function moveLabel(e: Pick<AssetTx, 'label' | 'coin'>): string {
  return e.coin ? TICKER : (e.label ?? '');
}

/** The operation listassetactivity says of a transaction of the wallet, if it read it. */
let opsByTxid = new Map<string, string>();
let opsOf: unknown = null;
export function activityOperationOf(txid: string): string | undefined {
  const list = ba.activity;
  if (opsOf !== list) {
    opsByTxid = new Map(list.map((a) => [a.txid, a.operation]));
    opsOf = list;
  }
  return opsByTxid.get(txid);
}

/** An amount with its sign, in the asset's decimals, exact ("≥" the least asked for, "≤" the most offered). */
export function signedAmount(e: AssetTx): string {
  // The coin paid by a coinbase without an amount the wallet can read (a creator's fees): its direction alone.
  if (e.amount === undefined) return e.coin && e.sign !== 0 ? (e.sign < 0 ? '−' : '+') : '';
  const text = formatAmount(e.amount, e.decimals ?? 0, { trim: false });
  const bound = e.at_least ? '≥ ' : e.at_most ? '≤ ' : '';
  return `${e.sign < 0 ? '−' : e.sign > 0 ? '+' : ''}${bound}${text}`;
}

/**
 * The other side of a move: where it went (a send, a mint), who paid it (a receive). Each address once; the wallet's own
 * outputs are not listed one by one (a mint pays the minted coins and the control coin to the wallet, a swap its result
 * and its change): `self` says the wallet is on that side, the parties are the others (user feedback 2026-10-19).
 */
export function counterparty(e: AssetTx): { dir: 'to' | 'from' | ''; parties: AssetTx['to']; self: boolean; own: AssetTx['to'] } {
  const side = (list: AssetTx['to']) => {
    const seen = new Set<string>();
    const others = list.filter((p) => !p.mine && !seen.has(p.address) && (seen.add(p.address), true));
    const own = list.filter((p) => p.mine && !seen.has(p.address) && (seen.add(p.address), true));
    return { parties: others, self: own.length > 0, own };
  };
  if (e.category === 'receive' || (e.sign > 0 && e.from.length)) return { dir: 'from', ...side(e.from) };
  if (e.sign < 0 || e.category === 'mint' || e.category === 'register') {
    const to = side(e.to);
    // What went out to others: the wallet's own outputs beside them are its change, not a destination.
    if (to.parties.length) return { dir: 'to', parties: to.parties, self: false, own: [] };
    return to.self ? { dir: 'to', parties: [], self: true, own: to.own } : { dir: '', parties: [], self: false, own: [] };
  }
  return { dir: '', parties: [], self: false, own: [] };
}

/** An address shortened in the middle, its start and end kept ("tba1qxy…k9a2"), whole when short. */
export function middleAddress(address: string, keep = 6): string {
  return address.length > keep * 2 + 3 ? `${address.slice(0, keep)}…${address.slice(-keep)}` : address;
}

/** The roles of the wallet's own outputs in a move, as the details list them: what each address got of the asset, its ECX change. */
export function ownAddresses(e: AssetTx): { address: string; roles: string[] }[] {
  const out = new Map<string, Set<string>>();
  const add = (address: string, role: string) => {
    if (!address || address === 'unknown') return;
    const set = out.get(address) ?? new Set<string>();
    set.add(role);
    out.set(address, set);
  };
  for (const p of e.to) {
    if (!p.mine) continue;
    if (p.roles?.length) for (const r of p.roles) add(p.address, r === 'control coin' ? t('control coin') : t('asset'));
    else add(p.address, t('asset'));
  }
  for (const a of e.change ?? []) add(a, t('change'));
  return [...out].map(([address, roles]) => ({ address, roles: [...roles] }));
}

/** An address in a few characters: its start and its end ("tba1q…a9x"), whole when short. */
export function shortAddress(address: string): string {
  return address.length > 16 ? `${address.slice(0, 8)}…${address.slice(-4)}` : address;
}

let byTxid = new Map<string, AssetTx[]>();
let byTxidOf: unknown = null;

/** The asset moves of a transaction of the wallet loaded, in the order listed. */
export function assetMovesOf(txid: string): AssetTx[] {
  const list = ba.transfersWallet === wallets.current ? ba.transfers : [];
  if (byTxidOf !== list) {
    byTxid = new Map();
    for (const e of list) byTxid.set(e.txid, [...(byTxid.get(e.txid) ?? []), e]);
    byTxidOf = list;
  }
  return byTxid.get(txid) ?? [];
}

/** What chainTxOf made for each transaction, while the list read stays the same (a page asks it for every row, often). */
let made = new Map<string, ChainTx | undefined>();
let madeOf: unknown = null;
let madeOps: unknown = null;

export function chainTxOf(txid: string): ChainTx | undefined {
  const moves = assetMovesOf(txid);
  if (madeOf !== byTxidOf || madeOps !== ba.activity) {
    made = new Map();
    madeOf = byTxidOf;
    madeOps = ba.activity;
  }
  if (made.has(txid)) return made.get(txid);
  const out = makeChainTx(moves);
  made.set(txid, out);
  return out;
}

function makeChainTx(moves: AssetTx[]): ChainTx | undefined {
  if (!moves.length) return undefined;
  const parties = (e: AssetTx) => counterparty(e).parties.map((p) => p.address);
  const operation = activityOperationOf(moves[0].txid);
  return {
    type: typeOfMoves(moves, operation),
    search: moves.map((e) => [moveLabel(e), e.display_name ?? '', e.seq ? `#${e.seq}` : '', e.asset ?? '', categoryText(e), ...parties(e)].join(' ')).join(' '),
    csv: [
      moves.map((e) => moveLabel(e)).join('; '),
      moves.map((e) => e.seq ?? '').join('; '),
      moves.map((e) => [signedAmount(e).replace('−', '-'), ...moveNotes(e).map((n) => `(${n})`)].join(' ').trim()).join('; '),
      moves.map((e) => { const c = counterparty(e); return c.dir ? `${c.dir} ${[...c.parties.map((p) => p.address), ...(c.self ? ['this wallet'] : [])].join(' ')}` : ''; }).join('; '),
    ],
  };
}

/**
 * Whether the transaction only moved assets the wallet hides (ignored, or left out by its filters: ba/hide.svelte.ts):
 * the Transactions page folds it into "N hidden transactions", the notices leave it out (user feedback 2026-10-19).
 */
export function chainTxHidden(txid: string): boolean {
  const moves = assetMovesOf(txid);
  const assets = moves.filter((e) => !e.coin);
  return assets.length > 0 && assets.every((e) => !!e.asset && hiddenAsset(e.asset, e.label));
}

/**
 * The ECX side of an asset operation in its details (user feedback 2026-10-19): no Debit or Credit of nothing, the output
 * index (of the ECX record, meaningless for the asset) under "Raw", and in Simple mode only the status and the fee.
 */
export function chainTxDetails(txid: string): { hideZero: boolean; raw: Record<string, string>; simpleRows?: string[] } | undefined {
  if (!assetMovesOf(txid).length) return undefined;
  // Simple mode keeps the net amount: what the operation took of the wallet's ECX (a buy, a pool), beside the fee.
  return { hideZero: true, raw: { 'Output index': `Output index of the ${TICKER} record` }, simpleRows: ['Status', 'Date', 'Transaction fee', 'Net amount'] };
}

/** What the notice of an asset received offers (Ignore this asset) and warns of (a name like one the wallet holds). */
export function chainTxNotice(txid: string): { warning?: string; actions?: { label: string; run: () => void }[] } | undefined {
  const got = assetMovesOf(txid).filter((e) => e.asset && e.sign > 0 && e.category === 'receive');
  if (!got.length) return undefined;
  const e = got[0];
  const asset = e.asset!;
  const label = (e.label ?? '').toLowerCase();
  // Another asset of the wallet of the same name (any case): a look-alike, told apart by its number.
  const twin = label ? (ba.mine?.assets ?? []).find((h) => h.asset !== asset && h.label.toLowerCase() === label && !!holdingOf(h.asset)) : undefined;
  return {
    warning: twin ? t('Same name as an asset you hold: this is #%1, not the one you hold.', e.seq ?? asset.slice(0, 8)) : undefined,
    actions: [{ label: t('Ignore this asset'), run: () => ignoreAsset(asset) }],
  };
}

// The asset moves of the wallet read again on its news, at most once a second while transactions keep coming (a
// script making assets: each read worked out the Transactions page again; user feedback 2026-10-19, item 10).
let lastRead = 0;
let readLater: ReturnType<typeof setTimeout> | null = null;

export function refreshChainTx(wallet: string): void {
  void wallet;
  startBitAssets();
  if (readLater) return;
  const wait = Math.max(0, lastRead + 1000 - Date.now());
  const read = () => {
    readLater = null;
    lastRead = Date.now();
    void refreshBitAssets({ node: false });
  };
  if (wait === 0) read();
  else readLater = setTimeout(read, wait);
}

/** What a transaction moved, in a line (the overview's recent transactions): "−500.00 BROSEVELT, +21.115475 COINYE". */
export function movesSummary(txid: string): string {
  return assetMovesOf(txid)
    .filter((e) => e.amount !== undefined || e.coin)
    .map((e) => `${signedAmount(e)} ${moveLabel(e)}`)
    .join(', ');
}

/**
 * The records of a short list (the overview, the home's tables) as the Transactions page shows them: those of an asset
 * transaction merged into one (mergeByTxid), its type and what it moved from the chain.
 */
export function shownRecords(records: readonly TxRecord[]): TxRecord[] {
  return mergeByTxid(records, (txid) => !!chainTxOf(txid));
}

/** The type of a record: what the chain knows of its transaction, else the wallet's words. */
export function recordTypeText(r: Pick<TxRecord, 'txid' | 'type' | 'role'>): string {
  return chainTxOf(r.txid)?.type || typeText(r.type, r.role) || t('Other');
}
