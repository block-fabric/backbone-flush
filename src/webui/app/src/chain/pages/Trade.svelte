<script lang="ts">
  // The DEX (user decisions, 2026-10-23, "Orca-style", and 2026-10-24: Browse assets, Portfolio, Create asset, Markets,
  // Swap), the last three here, one page each (chain/local.ts DEX_TABS: which tab is at home where), with "Create asset"
  // top right of Swap and Markets (ba/CreateAssetButton.svelte):
  //  - Swap: one asset for another at the best price over every venue: every pool of the pair of every kind, the order
  //    books, the curves, and the routes through others (ba/SwapPanel.svelte).
  //  - Markets (the page pools): every market of an asset in one table, the pools of every kind and the launches on their
  //    curves or graduated (ba/AllMarkets.svelte; a pool's page ba/PoolView.svelte, a launch's ba/LaunchView.svelte), the
  //    wallet's positions (ba/Positions.svelte), and a new pool made at the creator's price and funded over the full range
  //    (ba/NewPool.svelte).
  //  - Create asset (the page launch): the one create wizard alone (ba/CreateWizard.svelte: no market, a bonding curve, or
  //    straight into a funded pool).
  // Behind the Advanced links of Swap and Markets, fully there: the market index (ba/Markets.svelte), the order books (ba/OrderBook.svelte),
  // the pools of every kind (constant product, stable, weighted), adding to a constant-product pool
  // (ba/LiquidityPanel.svelte) and making a pool of a kind (ba/CreatePool.svelte), the Dutch auctions (pages/Auctions).
  // Portfolio is pages/MyAssets.svelte.
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import TabRow from '../../components/TabRow.svelte';
  import { tn } from '../../lib/i18n.svelte';
  import { nav } from '../../lib/nav.svelte';
  import '../ba/ba.css';
  import Auctions from './Auctions.svelte';
  import Issue from './Issue.svelte';
  import AllMarkets from '../ba/AllMarkets.svelte';
  import CreateAssetButton from '../ba/CreateAssetButton.svelte';
  import CreatePool from '../ba/CreatePool.svelte';
  import CreateWizard from '../ba/CreateWizard.svelte';
  import Positions from '../ba/Positions.svelte';
  import LaunchWizard from '../ba/LaunchWizard.svelte';
  import LiquidityPanel from '../ba/LiquidityPanel.svelte';
  import Markets from '../ba/Markets.svelte';
  import NewPool from '../ba/NewPool.svelte';
  import OrderBook from '../ba/OrderBook.svelte';
  import PoolsPanel from '../ba/PoolsPanel.svelte';
  import SwapPanel from '../ba/SwapPanel.svelte';
  import { swapFeeSplit } from '../ba/fees';
  import { ba, startBitAssets, swapParts } from '../ba/store.svelte';
  import Term from '../../components/Term.svelte';
  import { simpleMode } from '../../lib/uimode.svelte';
  import '../ba/terms';
  import { TICKER } from '../config';
  import { DEX_TABS, dexTabOf, type DexPage, type DexTab } from '../local';

  /** The page of the DEX shown (Swap: trade; Markets: pools; Create asset: launch): the tab it opens on when nav.tab names none. */
  let { page = 'trade' }: { page?: string } = $props();

  $effect(() => startBitAssets());

  const tab = $derived<DexTab>(dexTabOf(nav.tab, page));
  /** The page the tab is at home in: what the heading and the tabs are of. */
  const home = $derived<DexPage>(DEX_TABS[tab].page);
  const advanced = $derived('advanced' in DEX_TABS[tab]);
  // A tab asked for on another page of the DEX (Positions from a pool of Swap's Advanced): shown on its own page.
  $effect(() => {
    const at = nav.page;
    if ((at === 'trade' || at === 'pools' || at === 'launch') && at !== home) nav.page = home;
  });

  const open = $derived(ba.pools.filter((p) => !p.abandoned && p.curve !== 'book').length);
  // Simple mode (lib/uimode.svelte.ts): the swap alone, without the Advanced link; another tab shown when asked for.
  const simple = $derived(simpleMode());
  /** The fee tiers of the pool kinds (getdexinfo). */
  const tiers = $derived((ba.dex?.fee_tiers ?? []).map(Number).filter((x) => Number.isFinite(x)));
  const ADVANCED_TABS = (Object.keys(DEX_TABS) as DexTab[]).filter((id) => id === 'swap' || 'advanced' in DEX_TABS[id]);
  const tabsOf = (ids: DexTab[]) => ids.map((id) => ({ id, label: DEX_TABS[id].label }));
  /** The tabs of the page shown: Advanced's together (with Swap, the way back); Markets' three; Create asset none (the wizard alone). */
  const tabs = $derived.by(() => {
    if (home === 'trade') return advanced ? tabsOf(ADVANCED_TABS) : [];
    if (home === 'pools') return simple ? tabsOf([tab === 'launches' ? 'pools' : tab]) : tabsOf(['pools', 'positions', 'new']);
    return [];
  });
  /** The tab marked current: Launches (old links) is Markets' table, on its launches. */
  const current = $derived<DexTab>(tab === 'launches' ? 'pools' : tab);
  function select(id: string) {
    nav.tab = id;
  }
</script>

<div class="page ba-page trade">
  <header class="page-head">
    {#if home === 'pools'}
      <h1>Markets</h1>
      <span class="muted">Every market of an asset: the <Term id="pool">pools</Term> of every kind, and the launches on their curves or graduated</span>
      <CreateAssetButton />
    {:else if home === 'launch'}
      <h1>Create asset</h1>
      <span class="muted">An asset of your own: with no market yet, on a bonding curve, or straight into a pool you fund with {TICKER}</span>
    {:else if advanced}
      <h1>Advanced</h1>
      <span class="muted">{tn('%n pool(s) with liquidity', open)} · {#if ba.dex?.curves_active && tiers.length}a fee of {Math.min(...tiers)}% to {Math.max(...tiers)}% per trade, by pool{:else}0.3% fee per trade{/if}: {swapFeeSplit(swapParts())}</span>
    {:else}
      <h1>Swap</h1>
      {#if simple}
        <span class="muted">Pay {TICKER}, get an <Term id="asset">asset</Term>, at the price of its market. You see what you get before you confirm.</span>
      {:else}
        <span class="muted">The best price over every pool, order book and curve, and the routes through others · {swapFeeSplit(swapParts())}</span>
      {/if}
      <CreateAssetButton />
    {/if}
  </header>
  {#if ba.error}<p class="ba-problems bad" role="alert"><SafeText text={ba.error} /></p>{/if}
  {#if tabs.length > 1}<TabRow label={home === 'pools' ? 'Markets' : 'Advanced'} {tabs} {current} {select} />{/if}
  {#if tab === 'swap'}
    <SwapPanel />
  {:else if tab === 'markets'}
    <Markets />
  {:else if tab === 'book'}
    <OrderBook />
  {:else if tab === 'pools' || tab === 'launches'}
    <AllMarkets kind={tab === 'launches' ? 'launch' : 'all'} />
  {:else if tab === 'classic'}
    <PoolsPanel />
  {:else if tab === 'new'}
    <NewPool />
  {:else if tab === 'create'}
    <CreatePool />
  {:else if tab === 'positions'}
    <Positions />
  {:else if tab === 'auctions'}
    <Auctions embedded />
  {:else if tab === 'launch'}
    <LaunchWizard />
  {:else if tab === 'register'}
    <Issue embedded />
  {:else if tab === 'fund'}
    <CreateWizard embedded market="pool" />
  {:else}
    <LiquidityPanel />
  {/if}
  {#if !simple && (tab === 'swap' || tab === 'pools' || tab === 'launches')}
    <p class="advanced-link"><button type="button" class="link" data-testid="dex-advanced" onclick={() => select(tab === 'swap' ? 'markets' : 'classic')}><Icon name="chevron-right" size={12} /> Advanced: {tab === 'swap' ? 'market index, order books, other pool kinds, auctions' : 'constant-product pools, order books, auctions'}</button></p>
  {/if}
</div>

<style>
  .advanced-link {
    margin: 1rem 0 0;
    text-align: center;
    font-size: 0.85rem;
  }
  .advanced-link .link {
    display: inline-flex;
    align-items: center;
    gap: 0.25rem;
    color: var(--text-muted);
  }
</style>
