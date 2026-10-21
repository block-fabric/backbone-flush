<script lang="ts">
  // The pools: each pair and its kind (Layers' DEX: a pair may have several, of other curves and fees), what it
  // holds, its price, what it traded and how often; a pool opened: its page (PoolView.svelte). The Pools table of
  // the Trade tab of the Qt window of BitAssets.
  import { selectable, stacked } from '../../lib/actions';
  import Icon from '../../components/Icon.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { formatCount } from '../../lib/format';
  import { nav } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import AssetBadge from './AssetBadge.svelte';
  import PoolKind from './PoolKind.svelte';
  import PoolView from './PoolView.svelte';
  import { kindOf } from './kinds';
  import { isBook } from './markets';
  import { approx, formatAmount, formatPrice, isChn, type Pool } from './model';
  import HiddenToggle from './HiddenToggle.svelte';
  import { hiddenAsset } from './visible.svelte';
  import { ba, decimalsOf, morePools, showMorePools, tradeIntent } from './store.svelte';

  let query = $state('');
  let showClosed = $state(false);
  let open = $state<string | null>(null);
  // Sorted and filtered as the user likes (user feedback 2026-10-19), without the pools of assets the wallet hides.
  type PoolSort = 'traded' | 'trades' | 'deepest' | 'newest' | 'name';
  type PoolFilter = 'all' | 'chn' | 'mine' | 'kinds';
  let sort = $state<PoolSort>('traded');
  let filter = $state<PoolFilter>('all');
  let showHidden = $state(false);
  const positions = $derived(new Set((ba.mine?.liquidity ?? []).map((l) => l.pool)));
  const isHidden = (p: Pool) => hiddenAsset(p.asset_a_id, p.asset_a) || hiddenAsset(p.asset_b_id, p.asset_b);
  const SORTS: Record<PoolSort, (x: Pool, y: Pool) => number> = {
    traded: (x, y) => volumeChn(y) - volumeChn(x) || y.swaps - x.swaps,
    trades: (x, y) => y.swaps - x.swaps,
    deepest: (x, y) => approx(y.reserve_a) * approx(y.reserve_b) - approx(x.reserve_a) * approx(x.reserve_b),
    newest: (x, y) => y.created - x.created,
    name: (x, y) => `${x.asset_a}/${x.asset_b}`.localeCompare(`${y.asset_a}/${y.asset_b}`),
  };
  // The order books have their tab (Order book) and the market index (Markets): not pools to list here.
  const pools = $derived(ba.pools.filter((p) => !isBook(p)));
  const matching = $derived(
    pools
      .filter((p) => showClosed || !p.abandoned)
      .filter((p) => filter === 'all' || (filter === 'chn' && (isChn(p.asset_a_id) || isChn(p.asset_b_id))) || (filter === 'mine' && positions.has(p.pool)) || (filter === 'kinds' && p.default_kind === false))
      .filter((p) => {
        const q = query.trim().toLowerCase();
        return !q || p.asset_a.toLowerCase().includes(q) || p.asset_b.toLowerCase().includes(q);
      }),
  );
  const hiddenCount = $derived(showHidden ? 0 : matching.filter(isHidden).length);
  const list = $derived((showHidden ? matching : matching.filter((p) => !isHidden(p))).sort(SORTS[sort]));
  const closed = $derived(pools.filter((p) => p.abandoned).length);
  // A pool opened that is not among those read (a launch's pool, a page not read): asked of the node by its id.
  let fetched = $state<Pool | null>(null);
  $effect(() => {
    const id = open;
    void ba.version;
    if (!id || ba.pools.some((p) => p.pool === id)) return;
    rpc
      .raw<Pool>('getpool', [id])
      .then((p) => {
        if (open === id && p && typeof p === 'object') fetched = p;
      })
      .catch(() => undefined);
  });
  const pool = $derived(ba.pools.find((p) => p.pool === open) ?? (fetched && fetched.pool === open ? fetched : null));
  /** Whether the chain has pools of other kinds (getdexinfo): then their filter, and a way to make one. */
  const kinds = $derived(ba.dex?.curves_active === true || ba.pools.some((p) => p.default_kind === false));

  // A pool asked for by another page (My assets, a route's hop): opened.
  $effect(() => {
    if (!tradeIntent.pool) return;
    open = tradeIntent.pool;
    tradeIntent.pool = '';
  });

  /** What a pool traded, in CHN where one side is CHN (else in trades). */
  function volumeChn(p: Pool): number {
    if (isChn(p.asset_a_id)) return approx(p.volume_a);
    if (isChn(p.asset_b_id)) return approx(p.volume_b);
    return 0;
  }
</script>

{#if pool}
  <PoolView {pool} back={() => (open = null)} />
{:else}
  <section class="panel">
    <div class="panel-head">
      <input type="search" class="search" placeholder="Find a pool by an asset" aria-label="Find a pool" bind:value={query} />
      <label class="sort"><span class="muted">Sort</span>
        <select bind:value={sort} aria-label="Sort pools">
          <option value="traded">Most traded</option>
          <option value="trades">Most trades</option>
          <option value="deepest">Deepest</option>
          <option value="newest">Newest</option>
          <option value="name">Name</option>
        </select>
      </label>
      {#if closed}<label class="check"><input type="checkbox" bind:checked={showClosed} /> <span>Closed pools ({closed})</span></label>{/if}
      {#if ba.dex?.curves_active}<button type="button" class="btn small" onclick={() => (nav.tab = 'create')}><Icon name="plus" size={14} /> Create pool</button>{/if}
    </div>
    <div class="ba-chips" role="group" aria-label="Filter pools">
      <button type="button" class="ba-chip" aria-pressed={filter === 'all'} onclick={() => (filter = 'all')}>All</button>
      <button type="button" class="ba-chip" aria-pressed={filter === 'chn'} onclick={() => (filter = 'chn')}>With CHN</button>
      <button type="button" class="ba-chip" aria-pressed={filter === 'mine'} onclick={() => (filter = 'mine')}>Yours</button>
      {#if kinds}<button type="button" class="ba-chip" aria-pressed={filter === 'kinds'} onclick={() => (filter = 'kinds')}>Other kinds</button>{/if}
    </div>
    <HiddenToggle count={hiddenCount} bind:show={showHidden} />
    {#if list.length === 0}
      <p class="ba-empty">{pools.length === 0 ? 'No pools yet: add liquidity, or create a pool, to make the first.' : 'No pool matches.'}</p>
    {:else}
      <div class="ba-wrap">
        <table class="ba-table" use:stacked aria-label="Pools">
          <thead><tr><th>Pool</th><th>Kind</th><th class="end">Price</th><th class="end">Holds</th><th class="end">Traded in</th><th class="end">Trades</th></tr></thead>
          <tbody>
            {#each list as p (p.pool)}
              <tr class="pick" onclick={() => (open = p.pool)} use:selectable={() => (open = p.pool)}>
                <td><span class="pair"><AssetBadge id={p.asset_a_id} label={p.asset_a} size="s" /> <span class="faint">/</span> <AssetBadge id={p.asset_b_id} label={p.asset_b} size="s" /></span>{#if p.abandoned} <span class="ba-tag danger">closed</span>{/if}</td>
                <td><PoolKind kind={kindOf(p)} /></td>
                <td class="end">{formatPrice(p.price)}</td>
                <td class="end">{formatAmount(p.reserve_a, decimalsOf(p.asset_a_id), { max: 4 })} · {formatAmount(p.reserve_b, decimalsOf(p.asset_b_id), { max: 4 })}</td>
                <td class="end">{formatAmount(p.volume_a, decimalsOf(p.asset_a_id), { max: 4 })} · {formatAmount(p.volume_b, decimalsOf(p.asset_b_id), { max: 4 })}</td>
                <td class="end">{formatCount(p.swaps)}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
    {/if}
      {#if morePools()}
      <p class="ba-help more-pools">{t('Showing %1 of %2 pools.', formatCount(ba.pools.length), formatCount(ba.info?.pools ?? 0))} <button type="button" class="btn small" onclick={showMorePools}>Show more</button></p>
    {/if}
  </section>
{/if}

<style>
  .sort {
    display: inline-flex;
    align-items: center;
    gap: 0.45rem;
  }
  .pair {
    display: inline-flex;
    align-items: center;
    gap: 0.4rem;
    min-width: 0;
  }
  .search {
    flex: 1 1 14rem;
    max-width: 24rem;
  }
</style>
