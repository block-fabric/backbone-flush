// The pages of Layers of its own, beside those of the kit (with LocalPages.svelte, which shows them, and chain.json).
//
// Staking: FastBlocks' blocks of stake in one section (user decision, 2026-10-18: one section, not three, so as not to
// confuse), with tabs: Overview (the epoch, the slot and its leader, the fee split, your stake and income, your
// coins by finality), Validator (this node as a validator: its keys, staking on or off, its payout and bid, the
// slots it leads), Bonds (bond, add, unbond; the wallet's bonds) and Network (the validator set, the leader
// schedule, finality and the BMM blocks, slashing and jail). Simple mode shows the essentials only (your stake,
// your rewards, bond and unbond: fb/SimpleStaking).
//
// The assets of BitAssets and its DEX, in five pages (owner decision, 2026-11-11, after the app proved confusing):
// Portfolio (what the wallet holds, its launches and pool shares, and the old markets it can still close), Assets (one
// table of every asset; a row opens the asset's page, the one detail page of an asset, where every action on it
// starts with the asset fixed), Swap (the one swap form), Pools (the pools, those opening at graduation, New pool) and
// Launch (the assets on their curve, and the one create wizard). Products kept: launch curves, swaps, regular liquidity
// pools. Retired from the interface the same day (still valid on chain until a later fork refuses new ones): the order
// book, the auctions, stable and weighted pools, concentrated price ranges, the market index and the Advanced links.
// Their old tabs and pages land on the nearest page kept (dexRoute, RETIRED_PAGES). Sending an asset is in the Send
// page (LocalSend.svelte), the deposits and withdrawals in the peg's page.
//
// Their RPC methods and host methods are in src/webui/local.cpp; see doc/webui.md, "Pages of a chain of its own",
// doc/webui-parity.md (sections FB and BA) and doc/layers.md.
import type { IconName } from '../components/Icon.svelte';

export interface LocalPage {
  /** Stable: the page id (go('names')), the command page.<id>. */
  id: string;
  title: string;
  icon: IconName;
  /** What the page is for, in a line (tooltips, the palette). */
  summary: string;
  /**
   * The section of the sidebars and the area of the workspace it goes in. What concerns the chain comes
   * before the tools, whatever the section (the chainBeforeTools tests).
   */
  section: 'wallet' | 'chain' | 'tools';
  /** The page needs a loaded wallet. */
  wallet?: boolean;
  shortcut?: string;
  /** More words to find it by in the palette. */
  keywords?: string;
  /** A quick action of the dashboard's home that opens it (after Send and Receive, with the chain's). */
  quick?: { label: string; icon?: IconName; tab?: string; simpleLabel?: string };
  /** Its name in Simple mode, in plain words, where it differs from the title ("My bets" for Portfolio). */
  simpleTitle?: string;
  /** Only in the navigation of Advanced mode (lib/uimode.svelte.ts): Simple mode reaches it by the palette and links. */
  advanced?: boolean;
  /**
   * One of the chain's main features, in the navigation of Simple mode (at most three pages; the rule is in
   * doc/webui.md, "Simple and Advanced modes"). It then needs a `simpleSummary`.
   */
  simple?: boolean;
  /** What it is for in Simple mode, in a few plain words (under its name in the sidebar of Simple mode). */
  simpleSummary?: string;
  /**
   * Not a page of its own in the navigation: a tab of another page of the chain (BitAssets: Register new asset
   * under My assets). go('<id>') and its commands lead there (to `tab`, then the tab asked for, "issue/…").
   */
  tabOf?: { page: string; tab: string };
}

export const LOCAL_PAGES = [
  // The menu of the DEX (owner decision, 2026-11-11): Portfolio, Assets, Swap, Pools, Launch, in this order. Their ids
  // stay (my-assets, assets, trade, pools, launch), and so do their links and shortcuts.
  { id: 'my-assets', title: 'Portfolio', simple: true, simpleSummary: 'The assets you hold', icon: 'holdings', section: 'chain', shortcut: 'Alt+8', summary: 'What this wallet holds, its launches and their fees to claim, its pool shares and their fees to collect', wallet: true, keywords: 'my assets portfolio balances holdings launches creator fees claim pool shares liquidity activity' },
  { id: 'assets', title: 'Assets', icon: 'gem', section: 'chain', shortcut: 'Alt+7', summary: 'Every asset of the chain in one table: its price, its market, what you hold; a row opens its page', keywords: 'bitassets browse explore token find search price market cap', quick: { label: 'Assets', icon: 'gem' } },
  { id: 'trade', title: 'Swap', simple: true, simpleSummary: 'Trade one asset for another', icon: 'swap', section: 'chain', shortcut: 'Alt+9', summary: 'Swap one asset for another, by the best route the node quotes', keywords: 'swap exchange trade amm dex route best price curve pool', quick: { label: 'Swap', icon: 'swap' } },
  { id: 'pools', title: 'Pools', icon: 'pool', section: 'chain', summary: 'The liquidity pools, those that open at graduation, your share of each; a new pool', keywords: 'pools liquidity provide fees earn share add remove collect new pool graduation' },
  { id: 'launch', title: 'Launch', icon: 'rocket', section: 'chain', summary: 'The assets on their launch curve, and Create asset: on a curve, straight into a pool you fund, or with no market yet', keywords: 'launch create new asset token register reserve bonding curve pool fund ticker graduate' },
  { id: 'staking', title: 'Staking', simpleTitle: 'Stake and earn', simple: true, simpleSummary: 'Earn by putting your coins to work', icon: 'stake', section: 'chain', shortcut: 'Alt+6', summary: 'Blocks of stake: your stake and income, this node as a validator, the bonds, the validators, finality and slashing', keywords: 'fastblocks stake bond unbond validator epoch slot leader schedule jail income pos finality bmm anchor ticket final pot bid slash slashing evidence proof double sign banned penalty fee split', quick: { label: 'Stake', icon: 'stake', tab: 'bonds', simpleLabel: 'Stake and earn' } },
  // Shown as "Create asset" (owner decision, 2026-11-11; once "Register new asset"); its id stays issue.
  // The create wizard with no market chosen at first; go('issue') and its links lead there.
  { id: 'issue', title: 'Create asset', tabOf: { page: 'launch', tab: 'register' }, simpleTitle: 'Create an asset', simpleSummary: 'Make an asset of your own', icon: 'issue', section: 'chain', summary: 'Create an asset of your own, with its supply, decimals and description', wallet: true, keywords: 'create new asset token register reserve mint', quick: { label: 'Create asset', icon: 'issue', simpleLabel: 'Create an asset' } },
] as const satisfies readonly LocalPage[];

export type LocalPageId = (typeof LOCAL_PAGES)[number]['id'];

/** Items of the menus (the Menus button and the palette) of this chain: in the group 'Chains' (before the tools). */
export interface LocalCommand {
  id: string;
  label: string;
  page: LocalPageId;
  tab?: string;
  tip?: string;
  keywords?: string;
}

/**
 * The tabs of Staking (pages/Staking.svelte), and the views of its Network tab, by the ids deep links give as the
 * tab: go('staking', 'finality') opens Network on Finality & BMM. The ids of the three pages FastBlocks had
 * (finality, slashing) and of its tabs (node, validators, schedule) still lead where they did.
 */
export const STAKING_TABS = ['overview', 'validator', 'bonds', 'network'] as const;
export const NETWORK_VIEWS = ['validators', 'schedule', 'finality', 'slashing'] as const;
export type StakingTab = (typeof STAKING_TABS)[number];
export type NetworkView = (typeof NETWORK_VIEWS)[number];
/** The tab and the view of Network a deep link's tab names (an unknown one: Overview). */
export function stakingTabOf(tab: string | undefined | null): { tab: StakingTab; view: NetworkView } {
  if (tab === 'node') return { tab: 'validator', view: 'validators' };
  if ((NETWORK_VIEWS as readonly string[]).includes(tab ?? '')) return { tab: 'network', view: tab as NetworkView };
  if ((STAKING_TABS as readonly string[]).includes(tab ?? '')) return { tab: tab as StakingTab, view: 'validators' };
  return { tab: 'overview', view: 'validators' };
}

/** The three pages of the DEX that share routes (pages/Swap.svelte, pages/Pools.svelte, pages/Launch.svelte). */
export type DexPage = 'trade' | 'pools' | 'launch';
/** Where a route of the DEX leads: a page (the DEX's, Assets or Portfolio) and its tab. */
export interface DexRoute {
  page: DexPage | 'assets' | 'my-assets';
  tab: string;
}
/** The tab each page of the DEX opens on. */
export const DEX_HOME: Record<DexPage, string> = { trade: 'swap', pools: 'pools', launch: 'launches' };
/**
 * Pages retired from the interface (owner decision, 2026-11-11) and where their old links land: the auctions, in
 * Portfolio's "Closing old markets". Not in PAGES: App.svelte's navigate asks here for a page it does not know.
 */
export const RETIRED_PAGES: Record<string, { page: LocalPageId; tab: string }> = { auctions: { page: 'my-assets', tab: 'closing' } };

/**
 * Where a tab asked of a page of the DEX leads (nav.page, nav.tab), the tabs of before 2026-11-11 included: each lands
 * on the nearest page kept, never on an error. Kept: Swap "swap" (with "swap/get/<id>", "swap/pay/<id>"); Pools "pools"
 * (the table), "pools/<pool>" (a pool's page), "new" (New pool); Launch "launches" (the list) and the wizard, "create",
 * or with its market chosen: "launch" (a curve), "fund" (a pool you fund), "register" (none yet). Retired: the market
 * index, the pools of other kinds, Add liquidity and Create pool (Pools); the order book (Swap); the auctions and the
 * positions (Portfolio's "Closing old markets"); a launch's own page "launches/<token>/buy|sell" (the asset's page, on
 * that side). A tab at home on another page moves there; one unknown: the page's own.
 */
export function dexRoute(page: string, tab: string): DexRoute {
  const home: DexPage = page === 'pools' || page === 'launch' ? page : 'trade';
  const [head, a, b] = tab.split('/');
  switch (head) {
    case '':
      return { page: home, tab: '' };
    case 'swap':
      return { page: 'trade', tab };
    case 'book':
      return { page: 'trade', tab: 'swap' };
    case 'pools':
      return { page: 'pools', tab: a ? `pools/${a}` : 'pools' };
    case 'new':
      return { page: 'pools', tab: 'new' };
    case 'markets':
    case 'classic':
    case 'liquidity':
    case 'create':
      // Launch's own "create" is the wizard; Swap's and Pools' was Create pool.
      return home === 'launch' ? { page: 'launch', tab: 'create' } : { page: 'pools', tab: 'pools' };
    case 'positions':
    case 'auctions':
      return { page: 'my-assets', tab: 'closing' };
    case 'launches':
      return { page: 'launch', tab: a ? `launches/${a}/${b === 'sell' ? 'sell' : 'buy'}` : 'launches' };
    case 'fund':
    case 'launch':
    case 'register':
      return { page: 'launch', tab: head };
    default:
      return { page: home, tab: '' };
  }
}

export const LOCAL_COMMANDS = [
  { id: 'fb.bond', label: 'Bond coins…', page: 'staking', tab: 'bonds', tip: 'Bond coins of the wallet to a validator key, add to a bond, or unbond', keywords: 'stake bond unbond validator' },
  { id: 'fb.node', label: 'This node as a validator', page: 'staking', tab: 'validator', tip: 'Its keys, staking on or off, its payout and bid, the slots it leads next', keywords: 'validator staking keys payout' },
  { id: 'fb.validators', label: 'Validator set', page: 'staking', tab: 'validators', tip: 'The validators of an epoch, their stake and share, and every bond on the chain', keywords: 'validators set stake share epoch network' },
  { id: 'fb.schedule', label: 'Leader schedule', page: 'staking', tab: 'schedule', tip: 'Who leads each slot of an epoch, and the epochs', keywords: 'slots leaders schedule epochs' },
  { id: 'fb.bmm', label: 'BMM blocks and finality', page: 'staking', tab: 'finality', tip: 'How far the chain is BMM-confirmed and final, and the newest BMM blocks', keywords: 'bmm anchors final finality pot' },
  { id: 'fb.evidence', label: 'Proofs of double signing', page: 'staking', tab: 'slashing', tip: 'Proofs that a leader signed two blocks for one slot, and jailed validators', keywords: 'slashing evidence jail banned' },
  { id: 'ba.launch', label: 'Create asset…', page: 'launch', tab: 'create', tip: 'Create an asset of your own: on a launch curve, straight into a pool you fund, or with no market yet', keywords: 'launch bonding curve token create new ticker pool fund register' },
  { id: 'ba.swap', label: 'Swap assets…', page: 'trade', tab: 'swap', tip: 'Trade one asset for another, by the best route the node quotes', keywords: 'exchange trade route' },
  { id: 'ba.pools', label: 'Pools', page: 'pools', tab: 'pools', tip: 'The liquidity pools: their liquidity, fee and your share, and those that open at graduation', keywords: 'pool liquidity provide share' },
  { id: 'ba.newpool', label: 'New pool…', page: 'pools', tab: 'new', tip: 'Pick an asset you hold that has no pool and is not on a launch curve; its page opens with Add liquidity', keywords: 'create pool fund liquidity' },
  { id: 'ba.launches', label: 'Assets on their curve', page: 'launch', tab: 'launches', tip: 'Assets sold from a launch curve until they graduate into a locked pool', keywords: 'launch bonding curve token new graduate' },
  { id: 'ba.activity', label: 'Asset activity', page: 'my-assets', tab: 'activity', tip: 'The asset transactions of this wallet', keywords: 'history transactions' },
] as const satisfies readonly LocalCommand[];
