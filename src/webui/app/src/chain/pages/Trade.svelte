<script lang="ts">
  // Trade: swap one asset for another in their pool, or along a route (ba/SwapPanel.svelte), the pools
  // (ba/PoolsPanel.svelte), putting liquidity in them (ba/LiquidityPanel.svelte), and creating a pool of a kind
  // (Layers' DEX: ba/CreatePool.svelte), the wallet's positions of concentrated liquidity (ba/Positions.svelte; a concentrated
  // pool's page opens them: ba/RangePicker.svelte); the launches of tokens on bonding curves (ba/LaunchList.svelte, a launch's page
  // ba/LaunchView.svelte) and launching one (ba/LaunchWizard.svelte); the market index of every pair (ba/Markets.svelte, with
  // the curators trusted for the Verified badges) and the order books with their limit orders (ba/OrderBook.svelte); the treasury and the parts of the fees (ba/Treasury.svelte). The
  // Trade tab of the Qt window of BitAssets.
  import SafeText from '../../components/SafeText.svelte';
  import TabRow from '../../components/TabRow.svelte';
  import { tn } from '../../lib/i18n.svelte';
  import { nav } from '../../lib/nav.svelte';
  import '../ba/ba.css';
  import CreatePool from '../ba/CreatePool.svelte';
  import LaunchList from '../ba/LaunchList.svelte';
  import Positions from '../ba/Positions.svelte';
  import LaunchWizard from '../ba/LaunchWizard.svelte';
  import LiquidityPanel from '../ba/LiquidityPanel.svelte';
  import Markets from '../ba/Markets.svelte';
  import OrderBook from '../ba/OrderBook.svelte';
  import Treasury from '../ba/Treasury.svelte';
  import PoolsPanel from '../ba/PoolsPanel.svelte';
  import SwapPanel from '../ba/SwapPanel.svelte';
  import { swapFeeSplit } from '../ba/fees';
  import { ba, startBitAssets, swapParts } from '../ba/store.svelte';
  import Term from '../../components/Term.svelte';
  import { simpleMode } from '../../lib/uimode.svelte';
  import '../ba/terms';
  import { TICKER } from '../config';

  $effect(() => startBitAssets());

  type Tab = 'swap' | 'markets' | 'book' | 'pools' | 'liquidity' | 'create' | 'positions' | 'launches' | 'launch' | 'treasury';
  const TABS: { id: Tab; label: string }[] = [
    { id: 'swap', label: 'Swap' },
    { id: 'markets', label: 'Markets' },
    { id: 'book', label: 'Order book' },
    { id: 'pools', label: 'Pools' },
    { id: 'liquidity', label: 'Add liquidity' },
    { id: 'create', label: 'Create pool' },
    { id: 'positions', label: 'Positions' },
    { id: 'launches', label: 'Launches' },
    { id: 'launch', label: 'Launch a token' },
    { id: 'treasury', label: 'Treasury' },
  ];
  const tab = $derived<Tab>(TABS.some((x) => x.id === nav.tab) ? (nav.tab as Tab) : 'swap');
  const open = $derived(ba.pools.filter((p) => !p.abandoned && p.curve !== 'book').length);
  // Simple mode (lib/uimode.svelte.ts): the swap alone; the pools and liquidity are Advanced's, shown when asked for.
  const simple = $derived(simpleMode());
  /** The fee tiers of the pool kinds (getdexinfo). */
  const tiers = $derived((ba.dex?.fee_tiers ?? []).map(Number).filter((x) => Number.isFinite(x)));
  const tabs = $derived(simple ? TABS.filter((x) => x.id === 'swap' || x.id === tab) : TABS);
</script>

<div class="page ba-page trade">
  <header class="page-head">
    <h1>{simple ? 'Swap' : 'Trade'}</h1>
    {#if simple}
      <span class="muted">Pay {TICKER}, get a <Term id="asset">token</Term>, at the price of its market. You see what you get before you confirm.</span>
    {:else}
      <span class="muted">{tn('%n pool(s) with liquidity', open)} · {#if ba.dex?.curves_active && tiers.length}a fee of {Math.min(...tiers)}% to {Math.max(...tiers)}% per trade, by pool{:else}0.3% fee per trade{/if}: {swapFeeSplit(swapParts())}</span>
    {/if}
  </header>
  {#if ba.error}<p class="ba-problems bad" role="alert"><SafeText text={ba.error} /></p>{/if}
  {#if tabs.length > 1}<TabRow label="Trade" {tabs} current={tab} select={(id) => (nav.tab = id)} />{/if}
  {#if tab === 'swap'}
    <SwapPanel />
  {:else if tab === 'markets'}
    <Markets />
  {:else if tab === 'book'}
    <OrderBook />
  {:else if tab === 'pools'}
    <PoolsPanel />
  {:else if tab === 'create'}
    <CreatePool />
  {:else if tab === 'positions'}
    <Positions />
  {:else if tab === 'launches'}
    <LaunchList />
  {:else if tab === 'treasury'}
    <Treasury />
  {:else if tab === 'launch'}
    <LaunchWizard />
  {:else}
    <LiquidityPanel />
  {/if}
</div>
