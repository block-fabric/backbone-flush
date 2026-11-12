<script lang="ts">
  // Pools' New pool (owner decision, 2026-11-11): no form of its own. It lists only the assets this wallet holds that
  // have no pool with ECX and are not on a launch curve; picking one opens that asset's page on its Add liquidity box
  // (ba/AddLiquidity.svelte), the asset fixed, so that the pool cannot be made for another asset. The other assets held
  // are listed under it with why they are not offered (on a curve: its pool opens by itself at graduation; a pool already:
  // add to it from its page).
  import Icon from '../../components/Icon.svelte';
  import { go, nav } from '../../lib/nav.svelte';
  import { wallets } from '../../lib/state.svelte';
  import AssetBadge from './AssetBadge.svelte';
  import { assetRoute, poolRoute } from './assetactions';
  import { approx, formatAmount } from './model';
  import { fundedEcxPoolOf } from './pools';
  import { ba, displayNameOf, loadLaunchFlags, onCurve } from './store.svelte';
  import { hiddenAsset } from './visible.svelte';
  import { TICKER } from '../config';

  $effect(() => {
    void ba.version;
    loadLaunchFlags();
  });

  const held = $derived((ba.mine?.assets ?? []).filter((h) => approx(h.balance) > 0 && !hiddenAsset(h.asset, h.label)));
  const rows = $derived(
    held.map((h) => {
      // A pool with nothing in it is no pool of the asset: it is offered a new one (owner decision, 2026-11-11).
      const pool = fundedEcxPoolOf(h.asset, ba.pools);
      return { h, pool, state: onCurve(h.asset) ? ('curve' as const) : pool ? ('pool' as const) : ('free' as const) };
    }),
  );
  const free = $derived(rows.filter((r) => r.state === 'free'));
  const others = $derived(rows.filter((r) => r.state !== 'free'));
</script>

<section class="panel new-pool" data-testid="new-pool" aria-label="New pool">
  <div class="panel-head"><h3>New pool</h3><button type="button" class="btn small ghost" onclick={() => (nav.tab = 'pools')}><Icon name="back" size={14} /> Pools</button></div>
  <div class="panel-body stack">
    <p class="ba-help">Pick an asset you hold that has no pool yet. Its page opens with Add liquidity, the asset fixed: you choose a fee tier and the two amounts, and the pool with {TICKER} is made and funded in one step.</p>
    {#if wallets.current === null}
      <p class="ba-empty">Load a wallet to make a pool.</p>
    {:else if free.length === 0}
      <p class="ba-empty" data-testid="new-pool-none">This wallet holds no asset that can get a new pool: each one it holds has a pool already or is on its launch curve.</p>
    {:else}
      <ul class="ba-list" aria-label="Assets that can get a pool">
        {#each free as r (r.h.asset)}
          <li>
            <button type="button" class="ba-row" data-testid="new-pool-asset" data-id={r.h.asset} onclick={() => go('assets', assetRoute(r.h.asset, 'liquidity'))}>
              <AssetBadge id={r.h.asset} label={r.h.label} display={displayNameOf(r.h.asset) ?? ''} size="s" />
              <span class="main"><span class="faint small">{formatAmount(r.h.balance, r.h.decimals, { max: 6 })} held</span></span>
              <span class="side"><span class="btn small primary">Add liquidity</span></span>
            </button>
          </li>
        {/each}
      </ul>
    {/if}
    {#if others.length}
      <h4>Not offered</h4>
      <ul class="ba-list" aria-label="Assets that cannot get a new pool">
        {#each others as r (r.h.asset)}
          <li class="off" data-testid="new-pool-refused" data-id={r.h.asset} data-why={r.state}>
            <AssetBadge id={r.h.asset} label={r.h.label} display={displayNameOf(r.h.asset) ?? ''} size="s" />
            {#if r.state === 'curve'}
              <span class="faint small">On its launch curve: its pool opens by itself at graduation. Nobody can make one before then.</span>
              <button type="button" class="btn small ghost" onclick={() => go('assets', assetRoute(r.h.asset))}>Its page</button>
            {:else}
              <span class="faint small">It has a pool already: add to it from the pool's page.</span>
              <button type="button" class="btn small ghost" onclick={() => r.pool && go('pools', poolRoute(r.pool.pool))}>Its pool</button>
            {/if}
          </li>
        {/each}
      </ul>
    {/if}
  </div>
</section>

<style>
  .stack {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
  }
  h4 {
    margin: 0.4rem 0 0;
    font-size: 0.9rem;
    color: var(--text-muted);
  }
  .off {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.6rem;
    padding: 0.5rem 0.9rem;
  }
  .off .faint {
    flex: 1 1 14rem;
  }
  .small {
    font-size: 0.85rem;
  }
</style>
