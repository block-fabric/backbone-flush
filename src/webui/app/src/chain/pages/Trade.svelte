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

  $effect(() => startBitAssets());

  type Tab = 'swap' | 'pools' | 'liquidity';
  const TABS: { id: Tab; label: string }[] = [
    { id: 'swap', label: 'Swap' },
    { id: 'pools', label: 'Pools' },
    { id: 'liquidity', label: 'Add liquidity' },
  ];
  const tab = $derived<Tab>(TABS.some((x) => x.id === nav.tab) ? (nav.tab as Tab) : 'swap');
  const open = $derived(ba.pools.filter((p) => !p.abandoned).length);
</script>

<div class="page ba-page trade">
  <header class="page-head">
    <h1>Trade</h1>
    <span class="muted">{tn('%n pool(s) with liquidity', open)} · 0.3% fee per trade, to their providers</span>
  </header>
  {#if ba.error}<p class="ba-problems bad" role="alert"><SafeText text={ba.error} /></p>{/if}
  <TabRow label="Trade" tabs={TABS} current={tab} select={(id) => (nav.tab = id)} />
  {#if tab === 'swap'}
    <SwapPanel />
  {:else if tab === 'pools'}
    <PoolsPanel />
  {:else}
    <LiquidityPanel />
  {/if}
</div>
