// What BitAssets knows of a transaction of the wallet beyond CHN, for the Transactions page (pages/Transactions.svelte):
// the assets it moved (listassettransactions, read with the store's refresh: one call for the wallet, never one per row),
// what kind of move, how much, and the other side: where a send went, who paid what came in.
import { t } from '../lib/i18n.svelte';
import { wallets } from '../lib/state.svelte';
import { formatAmount, type AssetTx } from './ba/model';
import { ba, refreshBitAssets, startBitAssets } from './ba/store.svelte';

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
    case 'pool_add':
      return t('Pool add');
    case 'pool_remove':
      return t('Pool remove');
    case 'auction':
      return t('Auction');
    case 'bid':
      return t('Auction bid');
    case 'collect':
      return t('Auction collect');
    case 'logo':
      return t('Logo set');
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

/** An amount with its sign, in the asset's decimals, exact. */
export function signedAmount(e: AssetTx): string {
  if (e.amount === undefined) return '';
  const text = formatAmount(e.amount, e.decimals ?? 0, { trim: false });
  const bound = e.at_least ? '≥ ' : e.at_most ? '≤ ' : '';
  return `${e.sign < 0 ? '−' : e.sign > 0 ? '+' : ''}${bound}${text}`;
}

/** The other side of a move: where it went (a send), who paid it (a receive). */
export function counterparty(e: AssetTx): { dir: 'to' | 'from' | ''; parties: AssetTx['to'] } {
  if (e.category === 'receive' || (e.sign > 0 && e.from.length)) return { dir: 'from', parties: e.from };
  if (e.sign < 0 || e.category === 'mint') return { dir: 'to', parties: e.to.filter((p) => !p.mine || e.category === 'mint') };
  return { dir: '', parties: [] };
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

export function chainTxOf(txid: string): ChainTx | undefined {
  const moves = assetMovesOf(txid);
  if (!moves.length) return undefined;
  const parties = (e: AssetTx) => counterparty(e).parties.map((p) => p.address);
  return {
    type: categoryText(moves[0]),
    search: moves.map((e) => [e.label ?? '', e.seq ? `#${e.seq}` : '', e.asset ?? '', categoryText(e), ...parties(e)].join(' ')).join(' '),
    csv: [
      moves.map((e) => e.label ?? '').join('; '),
      moves.map((e) => e.seq ?? '').join('; '),
      moves.map((e) => signedAmount(e).replace('−', '-')).join('; '),
      moves.map((e) => { const c = counterparty(e); return c.dir ? `${c.dir} ${c.parties.map((p) => p.address).join(' ')}` : ''; }).join('; '),
    ],
  };
}

export function refreshChainTx(wallet: string): void {
  void wallet;
  startBitAssets();
  void refreshBitAssets({ node: false });
}
