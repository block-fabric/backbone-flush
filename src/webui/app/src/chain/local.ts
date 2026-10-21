// The pages of Layers of its own, beside those of the kit (with LocalPages.svelte, which shows them, and chain.json).
//
// Staking: FastBlocks' blocks of stake in one section (user decision, 2026-10-18: one section, not three, so as not to
// confuse), with tabs: Overview (the epoch, the slot and its leader, the fee split, your stake and income, your
// coins by finality), Validator (this node as a validator: its keys, staking on or off, its payout and bid, the
// slots it leads), Bonds (bond, add, unbond; the wallet's bonds) and Network (the validator set, the leader
// schedule, finality and the BMM blocks, slashing and jail). Simple mode shows the essentials only (your stake,
// your rewards, bond and unbond: fb/SimpleStaking).
//
// The assets of BitAssets: Assets (an explorer with each asset's supply, markets, auctions and history), My assets
// (what the wallet holds and controls), Register new asset (the wizard), Trade (swaps and liquidity) and Auctions
// (Dutch auctions); sending an asset is in the Send page (LocalSend.svelte).
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
  { id: 'staking', title: 'Staking', simpleTitle: 'Stake and earn', simple: true, simpleSummary: 'Earn by putting your coins to work', icon: 'stake', section: 'chain', shortcut: 'Alt+6', summary: 'Blocks of stake: your stake and income, this node as a validator, the bonds, the validators, finality and slashing', keywords: 'fastblocks stake bond unbond validator epoch slot leader schedule jail income pos finality bmm anchor ticket final pot bid slash slashing evidence proof double sign banned penalty fee split', quick: { label: 'Stake', icon: 'stake', tab: 'bonds', simpleLabel: 'Stake and earn' } },
  // "Browse assets" (user feedback 2026-10-19: told apart from My assets); its id, links and shortcut stay.
  { id: 'assets', title: 'Browse assets', icon: 'gem', section: 'chain', shortcut: 'Alt+7', summary: 'Every asset of the chain: its supply, markets, auctions and history', keywords: 'bitassets explore token find search supply', quick: { label: 'Browse assets', icon: 'gem' } },
  { id: 'my-assets', title: 'My assets', simple: true, simpleSummary: 'The tokens you hold', icon: 'holdings', section: 'chain', shortcut: 'Alt+8', summary: 'What this wallet holds and controls: balances, pool shares, auctions to collect, activity', wallet: true, keywords: 'portfolio balances holdings mint burn control liquidity receipts activity' },
  // Shown as "Register new asset" (user decision, 2026-10-15); its id stays issue.
  // A tab of My assets (user feedback 2026-10-19); go('issue') and its links lead there.
  { id: 'issue', title: 'Register new asset', tabOf: { page: 'my-assets', tab: 'register' }, simpleTitle: 'Create a token', simpleSummary: 'Make a token of your own', icon: 'issue', section: 'chain', summary: 'Create an asset of your own, with its supply, decimals and description', wallet: true, keywords: 'create new asset token register reserve mint', quick: { label: 'Register new asset', icon: 'issue', simpleLabel: 'Create a token' } },
  { id: 'trade', title: 'Trade', simpleTitle: 'Swap', simple: true, simpleSummary: 'Trade one token for another', icon: 'swap', section: 'chain', shortcut: 'Alt+9', summary: 'Swap assets in their pools, provide liquidity for a share of the fees, and launch tokens on bonding curves', keywords: 'swap exchange amm pool liquidity price dex launch bonding curve', quick: { label: 'Swap', icon: 'swap' } },
  { id: 'auctions', title: 'Auctions', icon: 'gavel', section: 'chain', summary: 'Buy at a price that falls block by block, or sell by Dutch auction', keywords: 'dutch auction bid sell buy', advanced: true },
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

export const LOCAL_COMMANDS = [
  { id: 'fb.bond', label: 'Bond coins…', page: 'staking', tab: 'bonds', tip: 'Bond coins of the wallet to a validator key, add to a bond, or unbond', keywords: 'stake bond unbond validator' },
  { id: 'fb.node', label: 'This node as a validator', page: 'staking', tab: 'validator', tip: 'Its keys, staking on or off, its payout and bid, the slots it leads next', keywords: 'validator staking keys payout' },
  { id: 'fb.validators', label: 'Validator set', page: 'staking', tab: 'validators', tip: 'The validators of an epoch, their stake and share, and every bond on the chain', keywords: 'validators set stake share epoch network' },
  { id: 'fb.schedule', label: 'Leader schedule', page: 'staking', tab: 'schedule', tip: 'Who leads each slot of an epoch, and the epochs', keywords: 'slots leaders schedule epochs' },
  { id: 'fb.bmm', label: 'BMM blocks and finality', page: 'staking', tab: 'finality', tip: 'How far the chain is BMM-confirmed and final, and the newest BMM blocks', keywords: 'bmm anchors final finality pot' },
  { id: 'fb.evidence', label: 'Proofs of double signing', page: 'staking', tab: 'slashing', tip: 'Proofs that a leader signed two blocks for one slot, and jailed validators', keywords: 'slashing evidence jail banned' },
  { id: 'ba.issue', label: 'Register new asset…', page: 'issue', tip: 'Reserve a name and register an asset with its supply', keywords: 'create new token' },
  { id: 'ba.swap', label: 'Swap assets…', page: 'trade', tab: 'swap', tip: 'Trade one asset for another in their pool', keywords: 'exchange trade' },
  { id: 'ba.markets', label: 'Markets', page: 'trade', tab: 'markets', tip: 'Every pair: its price, its change and volume over a day, its liquidity', keywords: 'market index pairs volume liquidity price change verified curators issuers' },
  { id: 'ba.book', label: 'Order book…', page: 'trade', tab: 'book', tip: 'The order book of two assets: limit orders to buy or sell at a price', keywords: 'order book limit orders bid ask spread' },
  { id: 'ba.liquidity', label: 'Add liquidity…', page: 'trade', tab: 'liquidity', tip: 'Put two assets in their pool for a share of its fees', keywords: 'pool provide' },
  { id: 'ba.positions', label: 'Positions', page: 'trade', tab: 'positions', tip: 'Your positions of concentrated liquidity: in range or not, fees earned; collect, add, remove, close', keywords: 'concentrated liquidity position range ticks uniswap v3 fees collect' },
  { id: 'ba.launches', label: 'Launches', page: 'trade', tab: 'launches', tip: 'Tokens sold from a bonding curve until they graduate into a locked pool', keywords: 'launch bonding curve token new graduate pump fair' },
  { id: 'ba.launch', label: 'Launch a token…', page: 'trade', tab: 'launch', tip: 'Launch a token of your own on a bonding curve', keywords: 'launch bonding curve token create new ticker' },
  { id: 'ba.createpool', label: 'Create a pool…', page: 'trade', tab: 'create', tip: 'Make a pool of a kind: constant product, stable or weighted, at a fee tier', keywords: 'pool curve stable weighted stableswap amplification weights fee tier' },
  { id: 'ba.sell', label: 'Sell by auction…', page: 'auctions', tab: 'sell', tip: 'Start a Dutch auction of an asset', keywords: 'dutch auction' },
  { id: 'ba.activity', label: 'Asset activity', page: 'my-assets', tab: 'activity', tip: 'The asset transactions of this wallet', keywords: 'history transactions' },
] as const satisfies readonly LocalCommand[];
