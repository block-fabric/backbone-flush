<script lang="ts">
  // Pools (owner decision, 2026-11-11): one table of the pools (searchpools, else the pools read: the liquidity in ECX,
  // the volume over the node's window of about a day where the node says it, the fee, the wallet's share), the pools
  // "opening at graduation" (the assets on their launch curve: listlaunches), and one New pool action (ba/NewPool.svelte:
  // an asset to pick, then its page). A row opens the pool's page (ba/PoolView.svelte, "pools/<pool>"). One kind of pool
  // is shown: a pool is its pair and its fee, whatever its curve on the chain (ba/pools.ts). Order books and launch
  // curves are no rows here. The node gives no fees earned over a day: that column is left out. Pools with nothing in
  // them (anyone makes one of any asset for nothing) are left out unless "Show empty pools" is on (off by default; owner
  // decision, 2026-11-11): such a pool is no pool of its asset anywhere else in the app (ba/pools.ts).
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import Term from '../../components/Term.svelte';
  import { selectable, stacked } from '../../lib/actions';
  import { errorText } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { go, nav } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import '../ba/ba.css';
  import '../ba/terms';
  import { barWidth } from '../ba/actions';
  import AssetBadge from '../ba/AssetBadge.svelte';
  import HiddenToggle from '../ba/HiddenToggle.svelte';
  import NewPool from '../ba/NewPool.svelte';
  import PoolView from '../ba/PoolView.svelte';
  import { assetRoute, parseDexRoute, poolRoute } from '../ba/assetactions';
  import type { Market } from '../ba/markets';
  import { approx, formatAmount, formatPercent, type Pool } from '../ba/model';
  import { isShownPool, poolEcx, poolEmpty, poolFeeText, poolSides } from '../ba/pools';
  import { loadPositions, sharePercent } from '../ba/positions.svelte';
  import { searchPools } from '../ba/search';
  import { assetById, ba, displayNameOf, followDexRoute, launchesRead, loadLaunchFlags, startBitAssets } from '../ba/store.svelte';
  import { hiddenAsset } from '../ba/visible.svelte';
  import { TICKER } from '../config';

  $effect(() => startBitAssets());
  $effect(() => {
    void nav.tab;
    followDexRoute('pools');
  });
  $effect(() => {
    void ba.version;
    loadLaunchFlags();
    loadPositions();
  });

  const route = $derived(parseDexRoute(nav.tab));
  const openPool = $derived(route.pool ?? null);
  const picking = $derived(nav.tab.split('/')[0] === 'new');

  // The pools, again with each block: a page of the node's search (the volume of its window), else those the store read.
  let found = $state<Market[]>([]);
  let fallback = $state(true);
  let error = $state('');
  $effect(() => {
    void ba.version;
    let live = true;
    searchPools({ query: '', filters: { exclude_abandoned: false }, sort: 'depth', limit: 200 })
      .then((page) => {
        if (!live) return;
        found = page.items as Market[];
        fallback = page.fallback;
        error = '';
      })
      .catch((e) => {
        if (live) error = errorText(e);
      });
    return () => {
      live = false;
    };
  });
  const source = $derived(fallback ? (ba.pools as Market[]) : found);

  // A pool opened that is not among those read (a page not read): asked of the node by its id.
  let fetched = $state<Pool | null>(null);
  $effect(() => {
    const id = openPool;
    void ba.version;
    if (!id || ba.pools.some((p) => p.pool === id)) return;
    rpc
      .raw<Pool>('getpool', [id])
      .then((p) => {
        if (openPool === id && p && typeof p === 'object') fetched = p;
      })
      .catch(() => undefined);
  });
  const pool = $derived(openPool ? (ba.pools.find((p) => p.pool === openPool) ?? source.find((p) => p.pool === openPool) ?? (fetched?.pool === openPool ? fetched : null)) : null);

  let query = $state('');
  let showHidden = $state(false);
  let showEmpty = $state(false);
  const shares = $derived(new Map((ba.mine?.liquidity ?? []).map((l) => [l.pool, l.percent])));
  const rows = $derived(
    source
      .filter(isShownPool)
      .map((p) => {
        const s = poolSides(p);
        const share = p.curve === 'concentrated' ? sharePercent(p) : (shares.get(p.pool) ?? null);
        return { p, s, empty: poolEmpty(p), liquidity: p.depth !== undefined ? approx(p.depth) : poolEcx(p), volume: p.volume_window !== undefined ? approx(p.volume_window) : undefined, share };
      })
      .sort((x, y) => Number(x.empty) - Number(y.empty) || (y.liquidity ?? -1) - (x.liquidity ?? -1)),
  );
  const matches = (r: (typeof rows)[number], q: string) => !q || [r.s.base, r.s.quote].some((x) => [x.label, displayNameOf(x.id) ?? '', assetById(x.id)?.seq ?? ''].some((v) => v.toLowerCase().includes(q)));
  const isHidden = (r: (typeof rows)[number]) => hiddenAsset(r.s.base.id, r.s.base.label) || hiddenAsset(r.s.quote.id, r.s.quote.label);
  const matching = $derived(rows.filter((r) => (showEmpty || !r.empty) && matches(r, query.trim().toLowerCase())));
  const emptyCount = $derived(rows.filter((r) => r.empty).length);
  const hiddenCount = $derived(showHidden ? 0 : matching.filter(isHidden).length);
  const list = $derived(showHidden ? matching : matching.filter((r) => !isHidden(r)));
  const curves = $derived(launchesRead.list.filter((l) => l.status !== 'graduated' && (showHidden || !hiddenAsset(l.token, l.ticker))));
</script>

<div class="page ba-page pools-page">
  {#if pool}
    <PoolView {pool} back={() => (nav.tab = 'pools')} />
  {:else if openPool}
    <p class="ba-empty" role="status">Loading…</p>
  {:else}
    <header class="page-head">
      <h1>Pools</h1>
      <span class="muted">Put an asset and {TICKER} in a <Term id="pool">pool</Term> and earn a share of every swap. One kind of pool, no price ranges.</span>
      <button type="button" class="btn small primary new-pool" data-testid="new-pool-button" onclick={() => (nav.tab = 'new')}><Icon name="plus" size={15} /> New pool</button>
    </header>
    {#if ba.error}<p class="ba-problems bad" role="alert"><SafeText text={ba.error} /></p>{/if}
    {#if picking}<NewPool />{/if}

    <section class="panel" data-testid="pools-table">
      <div class="panel-head">
        <h3>Open pools</h3>
        <input type="search" class="search" placeholder="Find a pool by an asset" aria-label="Find a pool" bind:value={query} />
      </div>
      <label class="check show-empty" data-testid="show-empty-pools"><input type="checkbox" role="switch" bind:checked={showEmpty} /> <span>Show empty pools{#if emptyCount > 0} <span class="faint">({emptyCount})</span>{/if}</span></label>
      <HiddenToggle count={hiddenCount} bind:show={showHidden} />
      {#if error}<p class="ba-problems bad" role="alert"><SafeText text={error} /></p>{/if}
      {#if list.length === 0}
        <p class="ba-empty">{rows.length - (showEmpty ? 0 : emptyCount) === 0 ? t('No pool yet. A pool opens when an asset graduates from its launch curve, or when someone makes one with New pool.') : t('No pool matches.')}</p>
      {:else}
        <div class="ba-wrap">
          <table class="ba-table" use:stacked aria-label="Pools">
            <thead><tr><th>Pool</th><th class="end">Liquidity ({TICKER})</th><th class="end">Volume 24 h ({TICKER})</th><th class="end">Fee</th><th class="end">Your share</th></tr></thead>
            <tbody>
              {#each list as r (r.p.pool)}
                <tr class="pick" data-testid="pool-row" data-pool={r.p.pool} onclick={() => (nav.tab = poolRoute(r.p.pool))} use:selectable={() => (nav.tab = poolRoute(r.p.pool))}>
                  <td><span class="pair"><AssetBadge id={r.s.base.id} label={r.s.base.label} display={displayNameOf(r.s.base.id) ?? ''} size="s" /><span class="faint">/</span> <AssetBadge id={r.s.quote.id} label={r.s.quote.label} size="s" />{#if r.empty}<span class="ba-tag warn">Needs liquidity</span>{/if}</span></td>
                  <td class="end">{r.liquidity !== undefined ? formatAmount(r.liquidity, 8, { max: 2 }) : '–'}</td>
                  <td class="end">{r.volume !== undefined ? formatAmount(r.volume, 8, { max: 2 }) : '–'}</td>
                  <td class="end" translate="no">{poolFeeText(r.p)}</td>
                  <td class="end">{r.share !== null ? formatPercent(r.share) : t('None')}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
    </section>

    <section class="panel" data-testid="pools-graduating">
      <div class="panel-head"><h3>Opening at graduation</h3><span class="muted small">assets on their launch curve</span></div>
      {#if curves.length === 0}
        <p class="ba-empty">No asset is on a launch curve now.</p>
      {:else}
        <div class="ba-wrap">
          <table class="ba-table" use:stacked aria-label="Pools opening at graduation">
            <thead><tr><th>Pool</th><th class="end">Raised so far ({TICKER})</th><th class="end">Opens at ({TICKER})</th><th>Status</th></tr></thead>
            <tbody>
              {#each curves as l (l.token)}
                <tr class="pick" data-testid="graduating-row" onclick={() => go('assets', assetRoute(l.token))} use:selectable={() => go('assets', assetRoute(l.token))}>
                  <td><span class="pair"><AssetBadge id={l.token} label={l.ticker} display={l.display_name ?? ''} size="s" /><span class="faint">/ {TICKER}</span></span></td>
                  <td class="end">{formatAmount(l.raised, 8, { max: 4 })}</td>
                  <td class="end">{formatAmount(l.graduation, 8, { max: 4 })}</td>
                  <td><span class="curve"><span class="ba-bar"><span use:barWidth={Math.min(100, Math.max(0, Number(l.progress) || 0)) / 100}></span></span><span class="ba-tag">{t('On curve %1', formatPercent(Number(l.progress) || 0))}</span></span></td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
        <p class="ba-help">Each of these pools opens by itself when its asset graduates, with the {TICKER} raised. Nobody can make one or add liquidity before then.</p>
      {/if}
    </section>
  {/if}
</div>

<style>
  .new-pool {
    margin-inline-start: auto;
  }
  .show-empty {
    display: inline-flex;
    align-items: center;
    gap: 0.4rem;
    margin: 0.3rem 0;
    font-size: 0.88rem;
  }
  .search {
    flex: 1 1 14rem;
    max-width: 24rem;
  }
  .pair {
    display: inline-flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.4rem;
    min-width: 0;
    max-width: 100%;
  }
  .curve {
    display: inline-flex;
    align-items: center;
    gap: 0.4rem;
  }
  .curve .ba-bar {
    width: 4rem;
  }
  .small {
    font-size: 0.85rem;
  }
</style>
