// The pages of Layers of its own, beside those of the kit (with LocalPages.svelte, which shows them, and chain.json).
//
// Staking: FastBlocks' blocks of stake in one section (user decision, 2026-10-18: one section, not three, so as not to
// confuse), with tabs: Overview (the epoch, the slot and its leader, the fee split, your stake and income, your
// coins by finality), Validator (this node as a validator: its keys, staking on or off, its payout and bid, the
// slots it leads), Bonds (bond, add, unbond; the wallet's bonds) and Network (the validator set, the leader
// schedule, finality and the BMM blocks, slashing and jail). Simple mode shows the essentials only (your stake,
// your rewards, bond and unbond: fb/SimpleStaking).
//
// The assets of BitAssets, and the DEX in four (user decision, 2026-10-23, "Orca-style"): Swap (the best price over
// every venue), Pools (concentrated liquidity, the full range by default), Launch (the one create wizard: no market, a
// bonding curve, or straight into a pool the creator funds) and Portfolio (what the wallet holds and controls; once My
// assets). The pools of other kinds, the order books, the markets and the auctions are behind Swap's
// Advanced link (DEX_TABS). Browse assets is an explorer with each asset's supply, markets, auctions and history;
// sending an asset is in the Send page (LocalSend.svelte), the deposits and withdrawals in the peg's page.
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
  // The menu of the DEX (user decision, 2026-10-24): Browse assets, Portfolio, Create asset, Markets, Swap. One page
  // component for the last three (pages/Trade.svelte), each tab of it at home in one of them (DEX_TABS below); their ids
  // stay (trade: Swap; pools: Markets; launch: Create asset), and so do their links and shortcuts.
  // "Browse assets" (user feedback 2026-10-19: told apart from My assets).
  { id: 'assets', title: 'Browse assets', icon: 'gem', section: 'chain', shortcut: 'Alt+7', summary: 'Every asset of the chain: its supply, markets, auctions and history', keywords: 'bitassets explore token find search supply', quick: { label: 'Browse assets', icon: 'gem' } },
  // My assets is Portfolio (user decision, 2026-10-23).
  { id: 'my-assets', title: 'Portfolio', simple: true, simpleSummary: 'The assets you hold', icon: 'holdings', section: 'chain', shortcut: 'Alt+8', summary: 'What this wallet holds and controls: balances, pool shares, auctions to collect, activity', wallet: true, keywords: 'my assets portfolio balances holdings mint burn control liquidity receipts activity' },
  { id: 'launch', title: 'Create asset', icon: 'rocket', section: 'chain', summary: 'Create an asset: with no market yet, on a bonding curve, or straight into a pool you fund', keywords: 'launch create new asset token register reserve bonding curve pool fund ticker graduate' },
  { id: 'pools', title: 'Markets', icon: 'pool', section: 'chain', summary: 'Every market of an asset in one table: the pools of every kind and the launches on their curves or graduated; your positions, a new pool', keywords: 'markets launches bonding curve graduated pool liquidity concentrated position range full range provide fees earn create new pool' },
  { id: 'trade', title: 'Swap', simple: true, simpleSummary: 'Trade one asset for another', icon: 'swap', section: 'chain', shortcut: 'Alt+9', summary: 'Swap one asset for another at the best price over every pool, order book and curve', keywords: 'swap exchange trade amm dex route best price order book curve', quick: { label: 'Swap', icon: 'swap' } },
  { id: 'staking', title: 'Staking', simpleTitle: 'Stake and earn', simple: true, simpleSummary: 'Earn by putting your coins to work', icon: 'stake', section: 'chain', shortcut: 'Alt+6', summary: 'Blocks of stake: your stake and income, this node as a validator, the bonds, the validators, finality and slashing', keywords: 'fastblocks stake bond unbond validator epoch slot leader schedule jail income pos finality bmm anchor ticket final pot bid slash slashing evidence proof double sign banned penalty fee split', quick: { label: 'Stake', icon: 'stake', tab: 'bonds', simpleLabel: 'Stake and earn' } },
  // Shown as "Register new asset" (user decision, 2026-10-15); its id stays issue.
  // The Launch wizard with no market (user decision, 2026-10-23); go('issue') and its links lead there.
  { id: 'issue', title: 'Register new asset', tabOf: { page: 'launch', tab: 'register' }, simpleTitle: 'Create an asset', simpleSummary: 'Make an asset of your own', icon: 'issue', section: 'chain', summary: 'Create an asset of your own, with its supply, decimals and description', wallet: true, keywords: 'create new asset token register reserve mint', quick: { label: 'Register new asset', icon: 'issue', simpleLabel: 'Create an asset' } },
  // Behind the Advanced link of Swap (user decision, 2026-10-23): a tab of Swap's Advanced, not in the navigation.
  { id: 'auctions', title: 'Auctions', tabOf: { page: 'trade', tab: 'auctions' }, icon: 'gavel', section: 'chain', summary: 'Buy at a price that falls block by block, or sell by Dutch auction', keywords: 'dutch auction bid sell buy', advanced: true },
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

/**
 * The tabs of the DEX (pages/Trade.svelte), each at home in one of its pages: what Swap (trade), Markets (pools) and
 * Create asset (launch) show. A tab asked for on another of the three (nav.tab = 'positions' from Swap, 'launches' from
 * Create asset) moves to its page. `advanced`: behind the Advanced links of Swap and Markets (user decision,
 * 2026-10-23), with the tabs of Advanced shown together.
 */
export const DEX_TABS = {
  swap: { page: 'trade', label: 'Swap' },
  markets: { page: 'trade', label: 'Market index', advanced: true },
  book: { page: 'trade', label: 'Order book', advanced: true },
  classic: { page: 'trade', label: 'Other pools', advanced: true },
  liquidity: { page: 'trade', label: 'Add liquidity', advanced: true },
  create: { page: 'trade', label: 'Create pool', advanced: true },
  auctions: { page: 'trade', label: 'Auctions', advanced: true },
  pools: { page: 'pools', label: 'Markets' },
  positions: { page: 'pools', label: 'Positions' },
  new: { page: 'pools', label: 'New pool' },
  fund: { page: 'launch', label: 'Create' },
  launch: { page: 'launch', label: 'Create' },
  register: { page: 'launch', label: 'Create' },
  // The launches are in Markets (user decision, 2026-10-24): their old links land there, on its launches.
  launches: { page: 'pools', label: 'Launches' },
} as const satisfies Record<string, { page: 'trade' | 'pools' | 'launch'; label: string; advanced?: boolean }>;
export type DexTab = keyof typeof DEX_TABS;
export type DexPage = (typeof DEX_TABS)[DexTab]['page'];
/** The tab each page of the DEX opens on. */
export const DEX_HOME: Record<DexPage, DexTab> = { trade: 'swap', pools: 'pools', launch: 'fund' };
/** The tab of the DEX nav.tab names (the part before "/": Auctions' own tab follows it), else the page's own. */
export function dexTabOf(tab: string, page: string): DexTab {
  const head = tab.split('/')[0];
  if (head in DEX_TABS) return head as DexTab;
  return DEX_HOME[(page in DEX_HOME ? page : 'trade') as DexPage];
}

export const LOCAL_COMMANDS = [
  { id: 'fb.bond', label: 'Bond coins…', page: 'staking', tab: 'bonds', tip: 'Bond coins of the wallet to a validator key, add to a bond, or unbond', keywords: 'stake bond unbond validator' },
  { id: 'fb.node', label: 'This node as a validator', page: 'staking', tab: 'validator', tip: 'Its keys, staking on or off, its payout and bid, the slots it leads next', keywords: 'validator staking keys payout' },
  { id: 'fb.validators', label: 'Validator set', page: 'staking', tab: 'validators', tip: 'The validators of an epoch, their stake and share, and every bond on the chain', keywords: 'validators set stake share epoch network' },
  { id: 'fb.schedule', label: 'Leader schedule', page: 'staking', tab: 'schedule', tip: 'Who leads each slot of an epoch, and the epochs', keywords: 'slots leaders schedule epochs' },
  { id: 'fb.bmm', label: 'BMM blocks and finality', page: 'staking', tab: 'finality', tip: 'How far the chain is BMM-confirmed and final, and the newest BMM blocks', keywords: 'bmm anchors final finality pot' },
  { id: 'fb.evidence', label: 'Proofs of double signing', page: 'staking', tab: 'slashing', tip: 'Proofs that a leader signed two blocks for one slot, and jailed validators', keywords: 'slashing evidence jail banned' },
  { id: 'ba.issue', label: 'Register new asset…', page: 'issue', tip: 'Reserve a name and register an asset with its supply', keywords: 'create new token' },
  { id: 'ba.swap', label: 'Swap assets…', page: 'trade', tab: 'swap', tip: 'Trade one asset for another at the best price over every pool, order book and curve', keywords: 'exchange trade route' },
  { id: 'ba.pools', label: 'Markets', page: 'pools', tab: 'pools', tip: 'Every market of an asset: the pools of every kind and the launches, their price, volume and liquidity', keywords: 'markets pool liquidity concentrated provide launches' },
  { id: 'ba.newpool', label: 'New pool…', page: 'pools', tab: 'new', tip: 'Make a concentrated pool of two assets at your price, and put your liquidity in it over the full range', keywords: 'create pool concentrated full range seed fund' },
  { id: 'ba.positions', label: 'Positions', page: 'pools', tab: 'positions', tip: 'Your positions of concentrated liquidity: in range or not, fees earned; collect, add, remove, close', keywords: 'concentrated liquidity position range ticks uniswap v3 fees collect' },
  { id: 'ba.launch', label: 'Create asset…', page: 'launch', tab: 'fund', tip: 'Create an asset of your own: with no market yet, on a bonding curve, or straight into a pool you fund', keywords: 'launch bonding curve token create new ticker pool fund' },
  { id: 'ba.launches', label: 'Launches', page: 'pools', tab: 'launches', tip: 'Assets sold from a bonding curve until they graduate into a locked pool', keywords: 'launch bonding curve token new graduate pump fair' },
  { id: 'ba.markets', label: 'Market index', page: 'trade', tab: 'markets', tip: 'Every pair, order books included: its price, its change and volume over a day, its liquidity (Advanced)', keywords: 'market index pairs volume liquidity price change verified curators issuers advanced' },
  { id: 'ba.book', label: 'Order book…', page: 'trade', tab: 'book', tip: 'The order book of two assets: limit orders to buy or sell at a price (Advanced)', keywords: 'order book limit orders bid ask spread advanced' },
  { id: 'ba.classic', label: 'Other pools', page: 'trade', tab: 'classic', tip: 'The pools of every kind: constant product, stable, weighted, concentrated (Advanced)', keywords: 'pool constant product stable weighted kinds advanced' },
  { id: 'ba.liquidity', label: 'Add liquidity…', page: 'trade', tab: 'liquidity', tip: 'Put two assets in their constant-product pool for a share of its fees (Advanced)', keywords: 'pool provide constant product advanced' },
  { id: 'ba.createpool', label: 'Create a pool…', page: 'trade', tab: 'create', tip: 'Make a pool of a kind: constant product, stable, weighted or concentrated, at a fee tier (Advanced)', keywords: 'pool curve stable weighted stableswap amplification weights fee tier advanced' },
  { id: 'ba.sell', label: 'Sell by auction…', page: 'auctions', tab: 'sell', tip: 'Start a Dutch auction of an asset (Advanced)', keywords: 'dutch auction advanced' },
  { id: 'ba.activity', label: 'Asset activity', page: 'my-assets', tab: 'activity', tip: 'The asset transactions of this wallet', keywords: 'history transactions' },
] as const satisfies readonly LocalCommand[];
