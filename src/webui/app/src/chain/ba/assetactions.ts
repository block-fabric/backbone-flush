// What can be done with an asset right now (user decision, 2026-10-24: the actions of every asset row of Portfolio and
// Browse assets, and of an asset's page), from its markets as the node lists them: on a launch's bonding curve, buy or
// sell there (and Claim for its creator when something is owed); graduated, or with pools, Swap (to get it), Sell (to
// pay it), Add liquidity (its best pool), View market; no market at all, Open a market; and Send (held), Receive, View
// asset. The most relevant first: the row's button; the rest in its "…" menu (AssetActions.svelte). Pure: no state, no
// calls (tested in chain/ba/assetactions.test.ts).
import { kindOf } from './kinds';
import { approx, isChn, type Pool } from './model';

export type AssetAction = 'claim' | 'buy' | 'sell-curve' | 'swap' | 'sell' | 'add-liquidity' | 'view-market' | 'open-market' | 'send' | 'receive' | 'view';

/** The markets of an asset: its launch's curve (not graduated), graduated, its pools (books and launch curves left out), its best pool. */
export interface AssetMarkets {
  curve: boolean;
  graduated: boolean;
  pools: number;
  /** The pool with the most ECX (else the deepest): Add liquidity's. '' none. */
  bestPool: string;
  /** The launch's curve pool, while on its curve. */
  curvePool: string;
}

/**
 * The markets of an asset from the pools read (listpools, searchpools), and its launch when the page knows it (My
 * launches, Markets, an asset's page): a pool of curve 'launch' is its bonding curve.
 */
export function marketsOf(id: string, pools: readonly Pool[], launch?: { status: string; pool?: string } | null): AssetMarkets {
  const mine = pools.filter((p) => !p.abandoned && (p.asset_a_id === id || p.asset_b_id === id) && p.curve !== 'book');
  const curves = mine.filter((p) => kindOf(p).curve === 'launch');
  const others = mine.filter((p) => kindOf(p).curve !== 'launch');
  const chnOf = (p: Pool) => (isChn(p.asset_a_id) ? approx(p.reserve_a) : isChn(p.asset_b_id) ? approx(p.reserve_b) : -1);
  const best = [...others].sort((x, y) => chnOf(y) - chnOf(x) || approx(y.reserve_a) * approx(y.reserve_b) - approx(x.reserve_a) * approx(x.reserve_b))[0];
  const graduated = launch ? launch.status === 'graduated' : false;
  const curve = launch ? launch.status !== 'graduated' : curves.length > 0;
  return { curve, graduated, pools: others.length, bestPool: best?.pool ?? (graduated ? (launch?.pool ?? '') : ''), curvePool: curve ? (curves[0]?.pool ?? launch?.pool ?? '') : '' };
}

export interface ActionContext {
  markets: AssetMarkets;
  /** A wallet is loaded. */
  wallet: boolean;
  /** The wallet holds some of it. */
  held: boolean;
  /** The wallet controls it. */
  controlled: boolean;
  /** Its launch owes the wallet (its creator) something now. */
  claimable: boolean;
}

/** The actions of an asset, the most relevant first: `primary` (the row's button) and `more` (its menu). */
export function assetActions(c: ActionContext): { primary: AssetAction; more: AssetAction[] } {
  const list: AssetAction[] = [];
  const m = c.markets;
  const claim = c.wallet && c.claimable;
  if (m.curve && !m.graduated) {
    list.push('buy');
    if (c.held) list.push('sell-curve');
    list.push('view-market');
  } else if (m.graduated || m.pools > 0) {
    list.push('swap');
    if (c.held) list.push('sell');
    if (m.bestPool && c.wallet) list.push('add-liquidity');
    list.push('view-market');
  } else if (c.wallet && (c.held || c.controlled)) {
    list.push('open-market');
  }
  if (c.wallet && c.held) list.push('send');
  if (c.wallet) list.push('receive');
  list.push('view');
  // For the creator, what the launch owes comes first.
  // Nothing to trade and none held: to look at it first (Receive in its menu).
  const primary: AssetAction = claim ? 'claim' : list[0] === 'receive' && !c.held ? 'view' : list[0];
  return { primary, more: list.filter((a) => a !== primary) };
}

/** The words of each action (shown with t()). */
export const ACTION_LABELS: Record<AssetAction, string> = {
  claim: 'Claim',
  buy: 'Buy',
  'sell-curve': 'Sell on the curve',
  swap: 'Swap',
  sell: 'Sell',
  'add-liquidity': 'Add liquidity',
  'view-market': 'View market',
  'open-market': 'Open a market',
  send: 'Send',
  receive: 'Receive',
  view: 'View asset',
};

/**
 * The route of each place an action leads (nav.page, nav.tab: chain/local.ts DEX_TABS), with the asset and the side in
 * it, so that the page reads them from the route: Swap "swap/get/<id>" or "swap/pay/<id>"; Markets "launches/<token>/buy"
 * or "/sell" (a launch's page, its panel on that side), "pools/<pool>" (a pool's page).
 */
export function swapRoute(id: string, side: 'get' | 'pay'): string {
  return `swap/${side}/${id}`;
}
export function launchRoute(token: string, side: 'buy' | 'sell' = 'buy'): string {
  return `launches/${token}/${side}`;
}
export function poolRoute(pool: string): string {
  return `pools/${pool}`;
}

/** What a route of the DEX names (the parts after its tab): an asset and its side, a launch, a pool. */
export function parseDexRoute(tab: string): { swap?: { side: 'get' | 'pay'; id: string }; launch?: { token: string; side: 'buy' | 'sell' }; pool?: string } {
  const [head, a, b] = tab.split('/');
  // An id: 64 characters (hex on the chain; letters or digits in the tests).
  const hex = (x: string | undefined) => !!x && /^[0-9a-z]{64}$/i.test(x);
  if (head === 'swap' && (a === 'get' || a === 'pay') && hex(b)) return { swap: { side: a, id: b } };
  if (head === 'launches' && hex(a)) return { launch: { token: a, side: b === 'sell' ? 'sell' : 'buy' } };
  if (head === 'pools' && hex(a)) return { pool: a };
  return {};
}
