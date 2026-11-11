<script lang="ts">
  // Swap (owner decision, 2026-11-11): the one swap form (ba/SwapPanel.svelte), no tabs and no Advanced link. The route
  // is whatever the node's quote returns: a launch curve, a pool, or several in turn. The tabs Swap had before (the
  // market index, the order book, the pools of other kinds, Add liquidity, Create pool, the auctions) land on the
  // nearest page kept (chain/local.ts dexRoute).
  import SafeText from '../../components/SafeText.svelte';
  import Term from '../../components/Term.svelte';
  import { nav } from '../../lib/nav.svelte';
  import { simpleMode } from '../../lib/uimode.svelte';
  import '../ba/ba.css';
  import '../ba/terms';
  import SwapPanel from '../ba/SwapPanel.svelte';
  import { swapFeeSplit } from '../ba/fees';
  import { ba, followDexRoute, startBitAssets, swapParts } from '../ba/store.svelte';
  import { TICKER } from '../config';

  $effect(() => startBitAssets());
  $effect(() => {
    void nav.tab;
    followDexRoute('trade');
  });
  const simple = $derived(simpleMode());
</script>

<div class="page ba-page trade">
  <header class="page-head">
    <h1>Swap</h1>
    {#if simple}
      <span class="muted">Pay {TICKER}, get an <Term id="asset">asset</Term>, at the price of its market. You see what you get before you confirm.</span>
    {:else}
      <span class="muted">One form. The route is picked for you: a launch curve, a pool, or both · {swapFeeSplit(swapParts())}</span>
    {/if}
  </header>
  {#if ba.error}<p class="ba-problems bad" role="alert"><SafeText text={ba.error} /></p>{/if}
  <SwapPanel />
</div>
