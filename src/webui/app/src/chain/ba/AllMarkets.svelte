<script lang="ts">
  // Markets (user decision, 2026-10-24): every market of an asset in one table, the pools of every kind (searchpools,
  // else the pools read) and the launches (listlaunches: on their bonding curves, with their progress, or graduated into
  // their pool), what Swap trades through. Filtered by kind (Pool, Launch, Graduated), with ECX or yours (a position, or
  // the asset held), found by an asset (its ticker, display name or number), sorted by volume, liquidity, price change or
  // newest. Each row is its asset as the app shows it (AssetBadge); opened: its pool's page (PoolView: swap, add
  // liquidity) or its launch's (LaunchView: buy, sell). A launch's pool is its launch's row, not a row of its own. The
  // order books are behind the Advanced link (Order book, Market index).
  import { untrack } from 'svelte';
  import { selectable, stacked } from '../../lib/actions';
  import { errorText } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { formatCount } from '../../lib/format';
  import { rpc } from '../../lib/rpc';
  import SafeText from '../../components/SafeText.svelte';
  import { barWidth } from './actions';
  import AssetBadge from './AssetBadge.svelte';
  import HiddenToggle from './HiddenToggle.svelte';
  import LaunchView from './LaunchView.svelte';
  import PoolKind from './PoolKind.svelte';
  import PoolView from './PoolView.svelte';
  import { kindOf } from './kinds';
  import { launchQuery, type Launch, type LaunchPage } from './launch';
  import { changeDirection, formatChange, isBook, type Market } from './markets';
  import { approx, CHN_ID, formatAmount, formatPercent, formatPrice, isChn, type Pool } from './model';
  import { searchPools } from './search';
  import { launchRoute, parseDexRoute, poolRoute } from './assetactions';
  import { nav } from '../../lib/nav.svelte';
  import { assetById, ba, displayNameOf, holdingOf, tradeIntent } from './store.svelte';
  import { hiddenAsset } from './visible.svelte';
  import { TICKER } from '../config';

  type Kind = 'all' | 'pool' | 'launch' | 'graduated';
  type Sort = 'volume' | 'liquidity' | 'change' | 'newest';
  /** The kind shown first (Launches' old links: 'launch'). */
  let { kind: initialKind = 'all' }: { kind?: Kind } = $props();

  // The rows: one per market, its asset first (the side that is not ECX), what it is quoted in second.
  interface Row {
    key: string;
    kind: 'pool' | 'launch' | 'graduated';
    base: { id: string; label: string };
    quote: { id: string; label: string };
    pool?: Pool;
    launch?: Launch;
    /** In the quote, for one of the base. */
    price?: number;
    change?: number | string;
    /** ECX traded, ECX held (a pool's ECX side; a curve: what it raised). */
    volume?: number;
    liquidity?: number;
    newest: number;
    mine: boolean;
  }

  let kind = $state<Kind>(untrack(() => initialKind));
  let sort = $state<Sort>('volume');
  let withChn = $state(false);
  let yours = $state(false);
  let query = $state('');
  let showHidden = $state(false);
  let pools = $state<Market[]>([]);
  // A node without searchpools: the pools the store reads, as they come.
  let fallback = $state(false);
  const source = $derived(fallback ? (ba.pools as Market[]) : pools);
  let launches = $state<Launch[]>([]);
  let moreLaunches = $state(0);
  let error = $state('');
  // The pool or the launch opened, in the route (ba/assetactions.ts: "pools/<pool>", "launches/<token>/buy|sell"), so
  // that back, forward and a reload keep it; the list's own route to go back to.
  const route = $derived(parseDexRoute(nav.tab));
  const openPool = $derived(route.pool ?? null);
  const openLaunch = $derived(route.launch?.token ?? null);
  // Opened from the list: back to it; from a link (an asset's action, a reload): to the list of its kind.
  let backTo = '';
  function show(tab: string) {
    const head = nav.tab.split('/')[0];
    if (!parseDexRoute(nav.tab).pool && !parseDexRoute(nav.tab).launch) backTo = head === 'launches' ? 'launches' : 'pools';
    nav.tab = tab;
  }
  function back() {
    nav.tab = backTo || nav.tab.split('/')[0];
    backTo = '';
  }
  // A pool opened that is not among those read (a launch's pool, a page not read): asked of the node by its id.
  let fetched = $state<Pool | null>(null);
  $effect(() => {
    const id = openPool;
    void ba.version;
    if (!id || source.some((p) => p.pool === id) || ba.pools.some((p) => p.pool === id)) return;
    rpc
      .raw<Pool>('getpool', [id])
      .then((p) => {
        if (openPool === id && p && typeof p === 'object') fetched = p;
      })
      .catch(() => undefined);
  });

  // A pool or a launch asked for by another page (a route's hop, the wizard once launched, an asset): opened.
  $effect(() => {
    if (tradeIntent.pool) {
      const id = tradeIntent.pool;
      tradeIntent.pool = '';
      untrack(() => show(poolRoute(id)));
    }
    if (tradeIntent.launch) {
      const token = tradeIntent.launch;
      tradeIntent.launch = '';
      untrack(() => show(launchRoute(token)));
    }
  });

  // The pools and the launches, again with each block (a page of each, up to 200).
  $effect(() => {
    void ba.version;
    let live = true;
    searchPools({ query: '', filters: {}, sort: 'depth', limit: 200 })
      .then((page) => {
        if (!live) return;
        pools = page.items as Market[];
        fallback = page.fallback;
      })
      .catch((e) => {
        if (live) error = errorText(e);
      });
    rpc
      .raw<LaunchPage>('listlaunches', [launchQuery('all', 'newest', 80, undefined, 200)])
      .then((page) => {
        if (!live) return;
        launches = page?.launches ?? [];
        moreLaunches = Math.max(0, (page?.total ?? 0) - launches.length);
      })
      .catch(() => undefined);
    return () => {
      live = false;
    };
  });

  const positions = $derived(new Set((ba.mine?.liquidity ?? []).map((l) => l.pool)));
  const held = (id: string) => approx(holdingOf(id)?.balance) > 0;
  /** The ECX side of a pool's amounts (none: a pair without ECX). */
  const chnSide = (p: Pool, a: unknown, b: unknown): number | undefined => (isChn(p.asset_a_id) ? approx(a as number) : isChn(p.asset_b_id) ? approx(b as number) : undefined);

  /** The change of 1 / price, from the change of the price (in percent). */
  function invertChange(change: number | string | undefined): number | undefined {
    if (change === undefined || change === null || change === '') return undefined;
    const c = approx(change) / 100;
    return c > -1 ? (1 / (1 + c) - 1) * 100 : undefined;
  }

  const rows = $derived.by(() => {
    const ofLaunch = new Set(launches.map((l) => l.pool));
    const out: Row[] = [];
    for (const p of source) {
      if (isBook(p) || p.abandoned || ofLaunch.has(p.pool)) continue;
      const flip = isChn(p.asset_a_id);
      const base = flip ? { id: p.asset_b_id, label: p.asset_b } : { id: p.asset_a_id, label: p.asset_a };
      const quote = flip ? { id: p.asset_a_id, label: p.asset_a } : { id: p.asset_b_id, label: p.asset_b };
      const price = p.price ? (flip ? 1 / p.price : p.price) : undefined;
      out.push({
        key: `p${p.pool}`,
        kind: 'pool',
        base,
        quote,
        pool: p,
        price,
        change: flip ? invertChange(p.price_change) : p.price_change,
        volume: p.volume_window !== undefined ? approx(p.volume_window) : chnSide(p, p.volume_a, p.volume_b),
        liquidity: p.depth !== undefined ? approx(p.depth) : chnSide(p, p.reserve_a, p.reserve_b),
        newest: p.created,
        mine: positions.has(p.pool) || held(base.id),
      });
    }
    for (const l of launches) {
      out.push({
        key: `l${l.token}`,
        kind: l.status === 'graduated' ? 'graduated' : 'launch',
        base: { id: l.token, label: l.ticker },
        quote: { id: CHN_ID, label: TICKER },
        launch: l,
        price: Number(l.price) || undefined,
        volume: approx(l.volume),
        liquidity: approx(l.raised),
        newest: l.height,
        mine: held(l.token) || positions.has(l.pool),
      });
    }
    return out;
  });

  const matches = (r: Row, q: string) => {
    if (!q) return true;
    return [r.base, r.quote].some((s) => [s.label, displayNameOf(s.id) ?? (s.id === r.launch?.token ? (r.launch?.display_name ?? '') : ''), assetById(s.id)?.seq ?? ''].some((x) => x.toLowerCase().includes(q)));
  };
  const isHidden = (r: Row) => hiddenAsset(r.base.id, r.base.label) || hiddenAsset(r.quote.id, r.quote.label);
  const SORTS: Record<Sort, (r: Row) => number | undefined> = {
    volume: (r) => r.volume,
    liquidity: (r) => r.liquidity,
    change: (r) => (r.change === undefined || r.change === null || r.change === '' ? undefined : approx(r.change)),
    newest: (r) => r.newest,
  };
  const matching = $derived(
    rows.filter((r) => (kind === 'all' || r.kind === kind) && (!withChn || isChn(r.quote.id)) && (!yours || r.mine) && matches(r, query.trim().toLowerCase())),
  );
  const hiddenCount = $derived(showHidden ? 0 : matching.filter(isHidden).length);
  const list = $derived.by(() => {
    const by = SORTS[sort];
    // What has no value for the sort goes last.
    return (showHidden ? matching : matching.filter((r) => !isHidden(r))).sort((x, y) => (by(y) ?? -Infinity) - (by(x) ?? -Infinity));
  });

  const pool = $derived(openPool ? (ba.pools.find((p) => p.pool === openPool) ?? source.find((p) => p.pool === openPool) ?? (fetched?.pool === openPool ? fetched : null)) : null);

  function open(r: Row) {
    if (r.launch) show(launchRoute(r.launch.token));
    else if (r.pool) show(poolRoute(r.pool.pool));
  }

  const KINDS: { id: Kind; label: string }[] = [
    { id: 'all', label: 'All' },
    { id: 'pool', label: 'Pools' },
    { id: 'launch', label: 'Launches' },
    { id: 'graduated', label: 'Graduated' },
  ];
</script>

{#if openLaunch}
  <LaunchView token={openLaunch} side={route.launch?.side ?? 'buy'} {back} />
{:else if pool}
  <PoolView {pool} {back} />
{:else if openPool}
  <p class="ba-empty" role="status">Loading…</p>
{:else}
  <section class="panel all-markets" data-testid="all-markets">
    <div class="panel-head">
      <input type="search" class="search" placeholder="Find a market by an asset" aria-label="Find a market" bind:value={query} />
      <label class="sort"><span class="muted">Sort</span>
        <select bind:value={sort} aria-label="Sort markets">
          <option value="volume">Most traded</option>
          <option value="liquidity">Most liquidity</option>
          <option value="change">Largest rise</option>
          <option value="newest">Newest</option>
        </select>
      </label>
    </div>
    <div class="filters">
      <div class="ba-chips" role="group" aria-label="Kind of market">
        {#each KINDS as k (k.id)}<button type="button" class="ba-chip" aria-pressed={kind === k.id} onclick={() => (kind = k.id)}>{t(k.label)}</button>{/each}
      </div>
      <div class="ba-chips" role="group" aria-label="Filter markets">
        <button type="button" class="ba-chip" aria-pressed={withChn} onclick={() => (withChn = !withChn)}>With {TICKER}</button>
        <button type="button" class="ba-chip" aria-pressed={yours} onclick={() => (yours = !yours)}>Yours</button>
      </div>
    </div>
    <HiddenToggle count={hiddenCount} bind:show={showHidden} />
    {#if error}<p class="ba-problems bad" role="alert"><SafeText text={error} /></p>{/if}
    {#if list.length === 0}
      <p class="ba-empty">{rows.length === 0 ? t('No market yet: create an asset, on a bonding curve or straight into a pool.') : t('No market matches.')}</p>
    {:else}
      <div class="ba-wrap">
        <table class="ba-table" use:stacked aria-label="Markets">
          <thead><tr><th>Market</th><th>Kind</th><th class="end">Price</th><th class="end">Change</th><th class="end">Volume ({TICKER})</th><th class="end">Liquidity ({TICKER})</th></tr></thead>
          <tbody>
            {#each list as r (r.key)}
              <tr class="pick" data-testid="market-row" data-kind={r.kind} onclick={() => open(r)} use:selectable={() => open(r)}>
                <td><span class="pair"><AssetBadge id={r.base.id} label={r.base.label} display={r.launch?.display_name} size="s" /> <span class="faint">/</span> <AssetBadge id={r.quote.id} label={r.quote.label} size="s" /></span></td>
                <td>
                  {#if r.pool}<PoolKind kind={kindOf(r.pool)} />
                  {:else if r.kind === 'graduated'}<span class="ba-tag accent">Graduated</span>
                  {:else if r.launch}<span class="curve"><span class="ba-tag">Launch</span><span class="ba-bar"><span use:barWidth={Math.min(100, Math.max(0, Number(r.launch.progress) || 0)) / 100}></span></span><span class="faint small">{formatPercent(Number(r.launch.progress) || 0)}</span></span>{/if}
                </td>
                <td class="end">{r.price !== undefined ? formatPrice(r.price) : '–'}</td>
                <td class="end" translate="no"><span class:ba-good={changeDirection(r.change) === 'up'} class:ba-bad={changeDirection(r.change) === 'down'}>{formatChange(r.change)}</span></td>
                <td class="end">{r.volume !== undefined ? formatAmount(r.volume, 8, { max: 2 }) : '–'}</td>
                <td class="end">{r.liquidity !== undefined ? formatAmount(r.liquidity, 8, { max: 2 }) : '–'}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
    {/if}
    {#if moreLaunches > 0}<p class="ba-help">{t('And %1 older launches: find one by its asset.', formatCount(moreLaunches))}</p>{/if}
  </section>
{/if}

<style>
  .filters {
    display: flex;
    flex-wrap: wrap;
    justify-content: space-between;
    gap: 0.5rem 1rem;
    padding: 0 1.3rem 0.5rem;
  }
  .sort {
    display: inline-flex;
    align-items: center;
    gap: 0.45rem;
  }
  .search {
    flex: 1 1 14rem;
    max-width: 24rem;
  }
  .pair {
    display: inline-flex;
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
    font-size: 0.8rem;
  }
</style>
