// Layers: the pages of FastBlocks (its blocks of stake) and of BitAssets (its assets), side by side.
//
// The pages of FastBlocks of its own, beside those of the kit (with LocalPages.svelte, which shows them, and
// chain.json): its blocks of stake. Staking (this node as a validator, the wallet's bonds, the validator sets, the
// leader schedule), Finality & BMM (how far the chain is BMM-confirmed and final, the BMM blocks, the fee pot, the
// node's bids) and Slashing & jail (proofs of double signing, jailed and banned validators). Their RPC methods
// and host methods are in src/webui/local.cpp; see doc/webui.md, "Pages of a chain of its own", and
// doc/webui-parity.md, section FB.
//
// The pages of this chain of its own, beside those of the kit: the one file of the interface (with
// LocalPages.svelte, which shows them, and chain.json) that a chain made from the template changes, so
// that merges of the template stay clean. See doc/webui.md, "Pages of a chain of its own".
//
// BitAssets: the assets of the chain (an explorer with each asset's supply, markets, auctions and history),
// what the wallet holds and controls, the wizard that issues an asset, trading in the pools (swaps and
// liquidity) and the Dutch auctions; their components in LocalPages.svelte (chain/pages, chain/ba), their RPC
// methods in src/webui/local.cpp, the cards of the home in LocalCards.svelte, their icons in icons.ts, and
// sending an asset in the Send page (LocalSend.svelte).
//
// The template has none. A sidechain adds an entry per page, for example
//
//   { id: 'names', title: 'Names', icon: 'book', section: 'chain', summary: 'Register and manage names', wallet: true,
//     quick: { label: 'Register a name' }, keywords: 'bitnames register' }
//
// then its component in LocalPages.svelte, the RPC methods it calls in src/webui/local.cpp, and a
// command or a dashboard card if it wants one (LOCAL_COMMANDS, LocalCards in LocalPages.svelte).
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
  quick?: { label: string; icon?: IconName; tab?: string };
}

export const LOCAL_PAGES = [
  { id: 'staking', title: 'Staking', icon: 'stake', section: 'chain', shortcut: 'Alt+6', summary: 'This node as a validator, the bonds of the wallet, the validator sets and the leader schedule', keywords: 'fastblocks stake bond unbond validator epoch slot leader schedule jail income pos', quick: { label: 'Stake', icon: 'stake', tab: 'bonds' } },
  { id: 'finality', title: 'Finality & BMM', icon: 'finality', section: 'chain', shortcut: 'Alt+7', summary: 'How far the chain is BMM-confirmed and final, the BMM blocks, the fee pot and the bids', keywords: 'fastblocks bmm anchor acknowledgement ticket final finality confirmed pot bid validatoranchorbid' },
  { id: 'slashing', title: 'Slashing & jail', icon: 'slash', section: 'chain', shortcut: 'Alt+8', summary: 'Proofs that a leader signed twice, and the validators jailed or banned', keywords: 'fastblocks slash slashing evidence proof double sign jail jailed banned penalty' },
  { id: 'assets', title: 'Assets', icon: 'gem', section: 'chain', shortcut: 'Alt+9', summary: 'Every asset of the chain: its supply, markets, auctions and history', keywords: 'bitassets explore token find search supply', quick: { label: 'Explore assets', icon: 'gem' } },
  { id: 'my-assets', title: 'My assets', icon: 'holdings', section: 'chain', summary: 'What this wallet holds and controls: balances, pool shares, auctions to collect, activity', wallet: true, keywords: 'portfolio balances holdings mint burn control liquidity receipts activity' },
  // Shown as "Register new asset" (user decision, 2026-10-15); its id stays issue.
  { id: 'issue', title: 'Register new asset', icon: 'issue', section: 'chain', summary: 'Create an asset of your own, with its supply, decimals and description', wallet: true, keywords: 'create new asset token register reserve mint', quick: { label: 'Register new asset', icon: 'issue' } },
  { id: 'trade', title: 'Trade', icon: 'swap', section: 'chain', summary: 'Swap assets in their pools, and provide liquidity for a share of the fees', keywords: 'swap exchange amm pool liquidity price dex', quick: { label: 'Swap', icon: 'swap' } },
  { id: 'auctions', title: 'Auctions', icon: 'gavel', section: 'chain', summary: 'Buy at a price that falls block by block, or sell by Dutch auction', keywords: 'dutch auction bid sell buy' },
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

export const LOCAL_COMMANDS = [
  { id: 'fb.bond', label: 'Bond coins…', page: 'staking', tab: 'bonds', tip: 'Bond coins of the wallet to a validator key, add to a bond, or unbond', keywords: 'stake bond unbond validator' },
  { id: 'fb.node', label: 'This node as a validator', page: 'staking', tab: 'node', tip: 'Its keys, staking on or off, the slots it leads next, its income', keywords: 'validator staking income keys' },
  { id: 'fb.validators', label: 'Validator set', page: 'staking', tab: 'validators', tip: 'The validators of an epoch, their stake and share, and every bond on the chain', keywords: 'validators set stake share epoch' },
  { id: 'fb.schedule', label: 'Leader schedule', page: 'staking', tab: 'schedule', tip: 'Who leads each slot of an epoch, and the epochs', keywords: 'slots leaders schedule epochs' },
  { id: 'fb.bmm', label: 'BMM blocks and finality', page: 'finality', tip: 'How far the chain is BMM-confirmed and final, and the newest BMM blocks', keywords: 'bmm anchors final finality pot' },
  { id: 'fb.evidence', label: 'Proofs of double signing', page: 'slashing', tip: 'Proofs that a leader signed two blocks for one slot, and jailed validators', keywords: 'slashing evidence jail banned' },
  { id: 'ba.issue', label: 'Register new asset…', page: 'issue', tip: 'Reserve a name and register an asset with its supply', keywords: 'create new token' },
  { id: 'ba.swap', label: 'Swap assets…', page: 'trade', tab: 'swap', tip: 'Trade one asset for another in their pool', keywords: 'exchange trade' },
  { id: 'ba.liquidity', label: 'Add liquidity…', page: 'trade', tab: 'liquidity', tip: 'Put two assets in their pool for a share of its fees', keywords: 'pool provide' },
  { id: 'ba.sell', label: 'Sell by auction…', page: 'auctions', tab: 'sell', tip: 'Start a Dutch auction of an asset', keywords: 'dutch auction' },
  { id: 'ba.activity', label: 'Asset activity', page: 'my-assets', tab: 'activity', tip: 'The asset transactions of this wallet', keywords: 'history transactions' },
] as const satisfies readonly LocalCommand[];
