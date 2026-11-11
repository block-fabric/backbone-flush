<script lang="ts">
  // The card of BitAssets on the home of the dashboard (the table of what the wallet holds, is
  // ba/HeldAssets.svelte above it: one card since user feedback 2026-10-19): the markets (the pools that trade most,
  // with their prices). It opens Pools; an asset of it, the asset's page. The auctions' card is gone with the auctions
  // (owner decision, 2026-11-11).
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import Card from '../../views/home/Card.svelte';
  import { formatCount } from '../../lib/format';
  import { go } from '../../lib/nav.svelte';
  import './ba.css';
  import AssetBadge from './AssetBadge.svelte';
  import { assetRoute } from './assetactions';
  import { formatPrice, isChn } from './model';
  import { isShownPool, poolEmpty } from './pools';
  import { ba, startBitAssets } from './store.svelte';
  import { simpleMode } from '../../lib/uimode.svelte';

  $effect(() => startBitAssets());
  // Simple mode (lib/uimode.svelte.ts): the actions in plain words.
  const simple = $derived(simpleMode());

  /** The pools shown as pools (no order book, no launch curve) that hold liquidity. */
  const open = $derived(ba.pools.filter((p) => isShownPool(p) && !poolEmpty(p)));
  const markets = $derived(
    open
      .slice()
      .sort((a, b) => b.swaps - a.swaps)
      .slice(0, 3),
  );
</script>

{#if ba.loaded && !ba.error}
  <div class="ba-cards">
    <Card title="Markets" icon="swap" open={() => (simple ? go('trade', 'swap') : go('pools', 'pools'))} openLabel={simple ? 'Open Swap' : 'Open the pools'} stat>
      <span class="value num">{formatCount(open.length)}</span>
      <dl class="pairs">
        <dt>{simple ? 'Assets to swap' : 'Pools with liquidity'}</dt><dd class="num">{open.length}</dd>
        <dt>Trades, all told</dt><dd class="num">{formatCount(open.reduce((n, p) => n + p.swaps, 0))}</dd>
      </dl>
      {#if markets.length === 0}
        <p class="muted">No pool trades yet.</p>
      {:else}
        <ul class="mini">
          {#each markets as p (p.pool)}
            {@const asset = isChn(p.asset_a_id) ? p.asset_b_id : p.asset_a_id}
            <li>
              <button type="button" class="link" onclick={() => go('assets', assetRoute(asset))}><AssetBadge id={asset} label={isChn(p.asset_a_id) ? p.asset_b : p.asset_a} size="s" /></button>
              <span class="num small">{formatPrice(isChn(p.asset_a_id) ? (p.price ? 1 / p.price : undefined) : p.price)} <SafeText text={isChn(p.asset_a_id) ? p.asset_a : p.asset_b} /></span>
            </li>
          {/each}
        </ul>
      {/if}
      {#snippet actions()}
        <button type="button" class="btn grow" onclick={() => go('trade', 'swap')}><Icon name="swap" size={15} /> Swap</button>
        <button type="button" class="btn ghost" onclick={() => go('assets')}>Assets</button>
      {/snippet}
    </Card>
  </div>
{/if}

<style>
  .ba-cards {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(16rem, 100%), 1fr));
    gap: 1rem;
  }
  .value {
    display: block;
    margin: 0.1rem 0 0.55rem;
    font-size: 1.55rem;
    font-weight: 650;
    letter-spacing: -0.02em;
  }
  .pairs {
    display: grid;
    /* The labels keep their longest word; the values wrap when the card is narrow (as the kit's cards). */
    grid-template-columns: minmax(min-content, 1fr) minmax(0, auto);
    gap: 0.35rem 1rem;
    margin: 0;
    padding-top: 0.6rem;
    border-top: 1px solid var(--line);
  }
  .pairs dt {
    color: var(--text-muted);
    font-size: 0.86rem;
    overflow-wrap: break-word;
  }
  .pairs dd {
    min-width: 0;
    overflow-wrap: anywhere;
    margin: 0;
    text-align: right;
    font-weight: 600;
    font-size: 0.92rem;
  }
  .mini {
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
    margin: 0.7rem 0 0;
    padding: 0;
    list-style: none;
  }
  .mini li {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.5rem;
    min-width: 0;
  }
  .link {
    display: inline-flex;
    align-items: center;
    gap: 0.3rem;
    min-width: 0;
    padding: 0;
    border: 0;
    background: none;
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .link:hover {
    color: var(--accent-fg);
  }
  .small {
    font-size: 0.85rem;
    white-space: nowrap;
  }
</style>
