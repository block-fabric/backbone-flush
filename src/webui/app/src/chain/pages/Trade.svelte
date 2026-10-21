<script lang="ts">
  // Trade: swap one asset for another in their pool (ba/SwapPanel.svelte), the pools (ba/PoolsPanel.svelte),
  // and putting liquidity in them (ba/LiquidityPanel.svelte). The Trade tab of the Qt window of BitAssets.
  import SafeText from '../../components/SafeText.svelte';
  import TabRow from '../../components/TabRow.svelte';
  import { tn } from '../../lib/i18n.svelte';
  import { nav } from '../../lib/nav.svelte';
  import '../ba/ba.css';
  import LiquidityPanel from '../ba/LiquidityPanel.svelte';
  import PoolsPanel from '../ba/PoolsPanel.svelte';
  import SwapPanel from '../ba/SwapPanel.svelte';
  import { ba, startBitAssets } from '../ba/store.svelte';
  import Term from '../../components/Term.svelte';
  import { simpleMode } from '../../lib/uimode.svelte';
  import '../ba/terms';

  $effect(() => startBitAssets());

  type Tab = 'swap' | 'pools' | 'liquidity';
  const TABS: { id: Tab; label: string }[] = [
    { id: 'swap', label: 'Swap' },
    { id: 'pools', label: 'Pools' },
    { id: 'liquidity', label: 'Add liquidity' },
  ];
  const tab = $derived<Tab>(TABS.some((x) => x.id === nav.tab) ? (nav.tab as Tab) : 'swap');
  const open = $derived(ba.pools.filter((p) => !p.abandoned).length);
  // Simple mode (lib/uimode.svelte.ts): the swap alone; the pools and liquidity are Advanced's, shown when asked for.
  const simple = $derived(simpleMode());
  const tabs = $derived(simple ? TABS.filter((x) => x.id === 'swap' || x.id === tab) : TABS);
</script>

<div class="page ba-page trade">
  <header class="page-head">
    <h1>{simple ? 'Swap' : 'Trade'}</h1>
    {#if simple}
      <span class="muted">Pay ECX, get a <Term id="asset">token</Term>, at the price of its market. You see what you get before you confirm.</span>
    {:else}
      <span class="muted">{tn('%n pool(s) with liquidity', open)} · 0.3% fee per trade, to their providers</span>
    {/if}
  </header>
  {#if ba.error}<p class="ba-problems bad" role="alert"><SafeText text={ba.error} /></p>{/if}
  {#if tabs.length > 1}<TabRow label="Trade" {tabs} current={tab} select={(id) => (nav.tab = id)} />{/if}
  {#if tab === 'swap'}
    <SwapPanel />
  {:else if tab === 'pools'}
    <PoolsPanel />
  {:else}
    <LiquidityPanel />
  {/if}
</div>
