<script lang="ts">
  // Assets (owner decision, 2026-11-11): one searchable table of the chain's assets, not cards. Columns: the asset (logo,
  // name, ticker), its price in ECX, the change and the volume of a day, its market value, where it trades (a chip: "On
  // curve N%", "Pool", "Pool, empty", "No market") and what the wallet holds. A click on a column sorts by it; the
  // filters are All, On curve, Has a pool, No market and Held by you. A row opens the asset's page
  // (ba/AssetView.svelte, "asset/<id>"), where every action on it starts.
  //
  // The list is the node's, a page at a time (user feedback 2026-10-19: hundreds of thousands of assets; the page never
  // reads them all): what is typed (a name, a number such as 1739-0029, or a hash "0x…": also an asset whose name is
  // private, by Enter), the filter and the sort asked of searchassets (or, on a node without it, of listassets: its first
  // page only). The figures are those the node already gives: the price from the asset's curve (listlaunches) or its pool
  // with ECX; the change and the volume of the node's window (about a day) from its pool search (searchpools), a dash
  // where it says nothing; the market value from the price and the supply. The chain lists no holders: no such column.
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import { selectable, stacked } from '../../lib/actions';
  import { errorText } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { formatCount } from '../../lib/format';
  import { nav, takeFocus } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import '../ba/ba.css';
  import AssetBadge from '../ba/AssetBadge.svelte';
  import CreateAssetButton from '../ba/CreateAssetButton.svelte';
  import AssetView from '../ba/AssetView.svelte';
  import { assetRoute, parseDexRoute } from '../ba/assetactions';
  import { untrack } from 'svelte';
  import { changeDirection, formatChange, type Market } from '../ba/markets';
  import { approx, assetArg, formatAmount, formatPercent, formatPrice, isChn, isSeq, type Asset } from '../ba/model';
  import { marketState, poolEmpty, poolSides, type MarketState } from '../ba/pools';
  import { assetById, ba, hasPool, holdingOf, launchById, launchesRead, loadLaunchFlags, noteKnownAssets, startBitAssets } from '../ba/store.svelte';
  import { PAGE, searchAssets, searchMyAssets, searchPools, type AssetSort } from '../ba/search';
  import { hiding, searchFiltersOf } from '../ba/hide.svelte';
  import { hiddenAsset } from '../ba/visible.svelte';
  import HiddenToggle from '../ba/HiddenToggle.svelte';
  import { wallets } from '../../lib/state.svelte';
  import { CHAIN, TICKER } from '../config';

  $effect(() => startBitAssets());
  $effect(() => {
    void ba.version;
    loadLaunchFlags();
  });

  type Filter = 'all' | 'curve' | 'pool' | 'none' | 'mine';
  type Column = 'asset' | 'price' | 'change' | 'volume' | 'cap' | 'market' | 'held';
  /** The sort the node's search has for a column (ba/search.ts): the pages after the first come in that order too. */
  const SERVER_SORT: Partial<Record<Column, AssetSort>> = { asset: 'name', volume: 'volume', cap: 'pool_depth' };

  let query = $state('');
  let filter = $state<Filter>('all');
  /** The column sorted by, and which way (none: as the node lists them, the newest first). */
  let sorted = $state<{ column: Column; down: boolean } | null>(null);
  // The asset opened is in the route ("asset/<id>", ba/assetactions.ts): back, forward and a reload keep it.
  const opened = $derived(parseDexRoute(nav.tab).asset ?? null);
  const shown = $derived(opened?.id ?? null);
  const show = (id: string | null) => (nav.tab = id ? assetRoute(id) : '');
  let lookupError = $state('');
  let looking = $state(false);
  /** An asset found by its hash or number that the list does not hold (its name is private). */
  let found = $state<Asset | null>(null);
  /** What the wallet hides (its asset filters, ba/hide.svelte.ts) shown anyway. */
  let showHidden = $state(false);

  let results = $state.raw<Asset[]>([]);
  let cursor: string | null = null;
  let loading = $state(false);
  let more = $state(false);
  let fallback = $state(false);
  let searchError = $state('');
  let run = 0;

  const mineIds = $derived(new Set((ba.mine?.assets ?? []).map((a) => a.asset)));
  const controlled = $derived(new Set((ba.mine?.assets ?? []).filter((a) => a.control).map((a) => a.asset)));

  // The pools with their figures of the node's window (searchpools: the volume, the change of the price), again with
  // each block; a node without the search gives the pools the store read, without those figures.
  let marketPools = $state.raw<Market[]>([]);
  $effect(() => {
    void ba.version;
    let live = true;
    searchPools({ query: '', filters: { exclude_abandoned: false }, sort: 'depth', limit: 200 })
      .then((page) => {
        if (live) marketPools = page.fallback ? [] : (page.items as Market[]);
      })
      .catch(() => undefined);
    return () => {
      live = false;
    };
  });
  /** Every pool known here, those with the window's figures first. */
  const allPools = $derived.by(() => {
    const seen = new Set(marketPools.map((p) => p.pool));
    return [...marketPools, ...(ba.pools as Market[]).filter((p) => !seen.has(p.pool))];
  });
  const poolsByAsset = $derived.by(() => {
    const map = new Map<string, Market[]>();
    for (const p of allPools) {
      for (const id of [p.asset_a_id, p.asset_b_id]) {
        if (isChn(id)) continue;
        const list = map.get(id);
        if (list) list.push(p);
        else map.set(id, [p]);
      }
    }
    return map;
  });

  interface Row {
    a: Asset;
    state: MarketState;
    /** On its curve: how far to graduation, in percent. */
    progress: number | null;
    price: number | undefined;
    change: number | undefined;
    volume: number | undefined;
    cap: number | undefined;
    held: number;
  }
  /** The change of 1 / price, from the change of the price (in percent). */
  function invertChange(change: number | string | undefined): number | undefined {
    if (change === undefined || change === null || change === '') return undefined;
    const c = approx(change) / 100;
    return c > -1 ? (1 / (1 + c) - 1) * 100 : undefined;
  }
  function rowOf(a: Asset): Row {
    const pools = poolsByAsset.get(a.asset) ?? [];
    const launch = launchById(a.asset);
    const m = marketState(a.asset, pools, launch);
    const held = approx(holdingOf(a.asset)?.balance);
    // The market whose figures are the asset's: its curve's pool, else its pool with ECX.
    const of = m.state === 'curve' ? pools.find((p) => p.curve === 'launch') : m.pool && !poolEmpty(m.pool) ? (m.pool as Market) : undefined;
    const sides = of ? poolSides(of) : null;
    const withEcx = !!sides && isChn(sides.quote.id);
    const price = m.state === 'curve' && launch ? Number(launch.price) || undefined : withEcx ? sides?.price : undefined;
    const change = of && withEcx && of.price_change !== undefined ? (sides?.flip ? invertChange(of.price_change) : approx(of.price_change)) : undefined;
    const volume = of && withEcx && of.volume_window !== undefined ? approx(of.volume_window) : undefined;
    const cap = m.state === 'curve' && launch ? approx(launch.market_cap) : price !== undefined ? price * approx(a.supply) : undefined;
    return { a, state: m.state, progress: m.state === 'curve' && launch ? Number(launch.progress) || 0 : null, price, change, volume, cap, held };
  }

  /** The assets on their curve, as the launches read give them (the filter "On curve": the node's search has no such filter). */
  const curveAssets = $derived(
    launchesRead.list
      .filter((l) => l.status !== 'graduated')
      .map((l) => assetById(l.token) ?? ({ asset: l.token, label: l.ticker, display_name: l.display_name, seq: '', decimals: l.decimals ?? 6, supply: l.supply, minted: l.supply, burned: 0, fixed: l.mintable !== true, registered: l.height, registration: l.txid, releasable: false } as Asset)),
  );
  const source = $derived.by(() => {
    if (filter !== 'curve') return results;
    const q = query.trim().toLowerCase();
    return q ? curveAssets.filter((a) => [a.label, a.display_name ?? '', a.seq, `0x${a.asset}`].some((x) => x.toLowerCase().includes(q))) : curveAssets;
  });
  const rows = $derived(source.map(rowOf));
  /** What the filter keeps that the node's search does not tell, and what the wallet ignores. */
  function keep(r: Row): boolean {
    if (filter === 'curve' && r.state !== 'curve') return false;
    if (filter === 'pool' && r.state !== 'pool' && r.state !== 'pool-empty') return false;
    if (filter === 'none' && r.state !== 'none') return false;
    return showHidden || !hiddenAsset(r.a.asset, r.a.label);
  }
  const VALUE: Record<Column, (r: Row) => number | string | undefined> = {
    asset: (r) => (r.a.display_name || r.a.label).toLowerCase(),
    price: (r) => r.price,
    change: (r) => r.change,
    volume: (r) => r.volume,
    cap: (r) => r.cap,
    market: (r) => (r.state === 'curve' ? 3 + (r.progress ?? 0) / 100 : r.state === 'pool' ? 2 : r.state === 'pool-empty' ? 1 : 0),
    held: (r) => r.held,
  };
  const list = $derived.by(() => {
    const kept = rows.filter(keep);
    const s = sorted;
    if (!s) return kept;
    const by = VALUE[s.column];
    // What has no value for the sort goes last, whichever way.
    return kept.slice().sort((x, y) => {
      const a = by(x);
      const b = by(y);
      if (a === undefined || b === undefined) return Number(a === undefined) - Number(b === undefined);
      const c = typeof a === 'string' || typeof b === 'string' ? String(a).localeCompare(String(b)) : a - b;
      return s.down ? -c : c;
    });
  });
  const hiddenHere = $derived(showHidden ? 0 : source.filter((a) => hiddenAsset(a.asset, a.label)).length);
  const filtersOn = $derived.by(() => {
    void hiding.version;
    return Object.keys(searchFiltersOf()).length > 0;
  });
  /** A click on a column: by it, the largest (the name: A) first; again: the other way. */
  function sortBy(column: Column) {
    sorted = sorted?.column === column ? { column, down: !sorted.down } : { column, down: column !== 'asset' };
  }
  const ariaSort = (column: Column) => (sorted?.column === column ? (sorted.down ? 'descending' : 'ascending') : 'none');

  /** Ask the node for a page: the first (anew) or the next. Pages the filter empties ask for the next, a few at most. */
  async function fetchPage(anew: boolean): Promise<void> {
    const mine = anew ? ++run : run;
    if (anew) {
      cursor = null;
      results = [];
      more = false;
    }
    // On curve: the launches read are the list.
    if (filter === 'curve') return;
    loading = true;
    searchError = '';
    try {
      // The assets of the wallet: the wallet's searchmyassets (its own list on a node without it).
      const wallet = wallets.current;
      if (filter === 'mine' && wallet === null) {
        results = [];
        return;
      }
      const own = () => (ba.mine?.assets ?? []).map((h) => assetById(h.asset) ?? ({ asset: h.asset, label: h.label, seq: '', decimals: h.decimals, supply: h.supply, minted: h.supply, burned: 0, fixed: h.fixed, registered: 0, registration: '', releasable: false } as Asset));
      const order: AssetSort = (sorted && SERVER_SORT[sorted.column]) || 'newest';
      for (let i = 0; i < 5; i++) {
        const q = { query: query.trim(), filters: { ...(showHidden ? {} : searchFiltersOf()), ...(filter === 'pool' ? { has_pool: true } : {}) }, sort: order, cursor, limit: PAGE };
        const page = filter === 'mine' && wallet !== null ? await searchMyAssets(q, wallet, own, hasPool) : await searchAssets(q, hasPool);
        if (mine !== run) return;
        const have = new Set(results.map((a) => a.asset));
        results = [...results, ...page.items.filter((a) => !have.has(a.asset))];
        cursor = page.cursor;
        fallback = page.fallback;
        more = cursor !== null;
        if (!more || page.items.map(rowOf).filter(keep).length >= 12) break;
      }
    } catch (e) {
      if (mine === run) searchError = errorText(e);
    } finally {
      if (mine === run) loading = false;
    }
  }

  // Asked anew as the user types (a moment after), picks a filter or a column the node sorts by, or the wallet's filters
  // change; and when the node counts other assets (a block registered some).
  const serverSort = $derived((sorted && SERVER_SORT[sorted.column]) || 'newest');
  $effect(() => {
    void filter;
    void serverSort;
    void showHidden;
    void hiding.version;
    void ba.info?.assets;
    void wallets.current;
    const q = query;
    if (!ba.loaded) return;
    const timer = setTimeout(() => void untrack(() => fetchPage(true)), q ? 300 : 0);
    return () => clearTimeout(timer);
  });

  /** The next page when the end of the list comes into view (or "Show more"). */
  function atEnd(node: HTMLElement) {
    if (typeof IntersectionObserver === 'undefined') return;
    const io = new IntersectionObserver((entries) => {
      if (entries.some((e) => e.isIntersecting) && more && !loading) void fetchPage(false);
    });
    io.observe(node);
    return { destroy: () => io.disconnect() };
  }

  // Opened for an asset (from another page, the palette, the home).
  $effect(() => {
    void nav.focus;
    const key = takeFocus('assets');
    if (key) show(key);
  });

  const FILTERS: { id: Filter; label: string }[] = [
    { id: 'all', label: 'All' },
    { id: 'curve', label: 'On curve' },
    { id: 'pool', label: 'Has a pool' },
    { id: 'none', label: 'No market' },
    { id: 'mine', label: 'Held by you' },
  ];
  const counts = $derived<Partial<Record<Filter, number>>>({ all: ba.info?.assets ?? undefined, curve: launchesRead.loaded ? curveAssets.length : undefined, mine: mineIds.size });
  const COLUMNS: { id: Column; label: string; end?: boolean }[] = [
    { id: 'asset', label: 'Asset' },
    { id: 'price', label: `Price (${TICKER})`, end: true },
    { id: 'change', label: '24 h', end: true },
    { id: 'volume', label: 'Volume 24 h', end: true },
    { id: 'cap', label: 'Market cap', end: true },
    { id: 'market', label: 'Market' },
    { id: 'held', label: 'You hold', end: true },
  ];

  /** Enter: an asset by its exact name, number or hash, also one the list does not hold (a private name). */
  async function lookup() {
    const q = query.trim();
    if (!q) return;
    const exact = ba.assets.find((a) => a.label === q || a.seq === q || `0x${a.asset}` === q.toLowerCase());
    if (exact) {
      show(exact.asset);
      return;
    }
    if (!(isSeq(q) || /^0x[0-9a-fA-F]{64}$/.test(q) || q.length <= 64)) return;
    looking = true;
    lookupError = '';
    try {
      const a = await rpc.raw<Asset>('getasset', [q]);
      found = a;
      noteKnownAssets([a]);
      show(a.asset);
    } catch (e) {
      lookupError = errorText(e);
    } finally {
      looking = false;
    }
  }
</script>

<div class="page ba-page assets-page">
  {#if shown}
    {#key shown}<AssetView id={shown} box={opened?.box ?? ''} onback={() => show(null)} />{/key}
  {:else}
    <header class="page-head">
      <h1>Assets</h1>
      <span class="muted">Every asset on {CHAIN.name}. Open one to trade it or see its markets.{#if ba.info} · {formatCount(ba.info.assets)} assets · next block {formatCount(ba.info.height)}{/if}</span>
      <CreateAssetButton />
    </header>

    {#if ba.error}<p class="ba-problems bad" role="alert"><SafeText text={ba.error} /></p>{/if}

    <section class="panel finder">
      <form
        class="search"
        onsubmit={(e) => {
          e.preventDefault();
          void lookup();
        }}
      >
        <Icon name="search" size={18} />
        <input id="asset-search" type="search" bind:value={query} placeholder="Find an asset by its name, number (1739-0029) or hash (0x…)" autocomplete="off" spellcheck="false" aria-label="Find an asset" />
        <button type="submit" class="btn small" disabled={looking || !query.trim()}>{looking ? 'Looking…' : 'Find'}</button>
      </form>
      {#if lookupError}<p class="ba-problems" role="status"><SafeText text={lookupError} /></p>{/if}
      <div class="ba-chips" role="group" aria-label="Filter">
        {#each FILTERS as f (f.id)}
          <button type="button" class="ba-chip" aria-pressed={filter === f.id} onclick={() => (filter = f.id)}>{t(f.label)}{#if counts[f.id] !== undefined} <span class="count">{counts[f.id]}</span>{/if}</button>
        {/each}
      </div>
    </section>

    <HiddenToggle count={hiddenHere} some={filtersOn} bind:show={showHidden} />
    {#if searchError}<p class="ba-problems bad" role="alert"><SafeText text={searchError} /></p>{/if}
    {#if !ba.loaded || (loading && list.length === 0)}
      <p class="ba-empty" role="status">Loading…</p>
    {:else if list.length === 0}
      <div class="panel ba-empty">
        {#if (ba.info?.assets ?? 0) === 0}
          No asset is registered yet. Create the first one.
        {:else}
          No asset matches. Press Enter to look for one by its exact name, number or hash: a private name is not listed.
        {/if}
      </div>
    {:else}
      <section class="panel">
        <div class="ba-wrap">
          <table class="ba-table" use:stacked aria-label="Assets" data-testid="assets-table">
            <thead>
              <tr>
                {#each COLUMNS as c (c.id)}
                  <th class:end={c.end} aria-sort={ariaSort(c.id)}><button type="button" class="sort" data-column={c.id} onclick={() => sortBy(c.id)}>{t(c.label)}{#if sorted?.column === c.id}<span aria-hidden="true"> {sorted.down ? '▾' : '▴'}</span>{/if}</button></th>
                {/each}
              </tr>
            </thead>
            <tbody>
              {#each list as r (r.a.asset)}
                <tr class="pick" data-testid="asset-row" data-id={r.a.asset} data-market={r.state} onclick={() => show(r.a.asset)} use:selectable={() => show(r.a.asset)}>
                  <td><span class="who"><AssetBadge id={r.a.asset} label={r.a.label} display={r.a.display_name ?? ''} seq={r.a.seq} size="s" />{#if controlled.has(r.a.asset)}<span class="ba-tag accent" title="This wallet holds its control coin"><Icon name="crown" size={12} /></span>{/if}</span></td>
                  <td class="end">{#if r.price !== undefined}{formatPrice(r.price, 8)}{:else}<span class="faint">No price yet</span>{/if}</td>
                  <td class="end" translate="no"><span class:ba-good={changeDirection(r.change) === 'up'} class:ba-bad={changeDirection(r.change) === 'down'}>{formatChange(r.change)}</span></td>
                  <td class="end">{r.volume !== undefined ? formatAmount(r.volume, 8, { max: 2 }) : '–'}</td>
                  <td class="end">{r.cap !== undefined ? formatPrice(r.cap) : '–'}</td>
                  <td><span class="ba-tag market {r.state}" data-testid="market-badge">{#if r.state === 'curve'}{t('On curve %1', formatPercent(r.progress ?? 0))}{:else if r.state === 'pool'}Pool{:else if r.state === 'pool-empty'}Pool, empty{:else}No market{/if}</span></td>
                  <td class="end">{#if r.held > 0}{formatAmount(holdingOf(r.a.asset)?.balance ?? 0, r.a.decimals, { max: 4 })}{:else}<span class="faint">None</span>{/if}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
        <p class="ba-help foot">Click a column heading to sort by it. The Market column says where an asset trades now: on its launch curve, in a pool, or nowhere yet.</p>
      </section>
      <div class="end-mark" use:atEnd></div>
      {#if more && filter !== 'curve'}
        <p class="more"><button type="button" class="btn" disabled={loading} onclick={() => void fetchPage(false)}>{loading ? 'Loading…' : 'Show more'}</button></p>
      {/if}
    {/if}
    {#if fallback && filter !== 'curve' && (ba.info?.assets ?? 0) > PAGE}
      <p class="ba-help">{t('This node lists %1 assets at a time: type a name to find others, or a number or hash and press Enter.', formatCount(200))}</p>
    {/if}
    {#if found && !list.some((r) => r.a.asset === found?.asset)}
      <p class="ba-help">Found by its hash: <button type="button" class="btn small ghost" onclick={() => show(found?.asset ?? null)}><AssetBadge id={found.asset} label={found.label} display={found.display_name ?? ''} seq={found.seq} size="s" /></button> <span class="faint" translate="no">{assetArg(found.asset)}</span></p>
    {/if}
  {/if}
</div>

<style>
  .more {
    display: flex;
    justify-content: center;
    margin: 0.8rem 0 0;
  }
  .finder {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
    padding: 0.9rem 1rem;
  }
  .search {
    display: flex;
    align-items: center;
    gap: 0.6rem;
    color: var(--text-muted);
  }
  .search input {
    flex: 1 1 auto;
    min-width: 0;
    font-size: 1.05rem;
  }
  .sort {
    padding: 0;
    border: 0;
    background: none;
    color: inherit;
    font: inherit;
    cursor: pointer;
    white-space: nowrap;
  }
  .sort:hover {
    color: var(--text);
  }
  .who {
    display: inline-flex;
    align-items: center;
    gap: 0.4rem;
    min-width: 0;
    max-width: 100%;
  }
  .ba-tag.market {
    white-space: nowrap;
  }
  .ba-tag.market.curve {
    color: var(--accent);
    border-color: var(--accent);
  }
  .ba-tag.market.pool {
    color: var(--positive);
  }
  .ba-tag.market.pool-empty {
    color: var(--warning);
    border-color: var(--warning);
  }
  .ba-tag.market.none {
    color: var(--text-faint, var(--text-muted));
  }
  .foot {
    margin: 0;
    padding: 0.6rem 1.1rem 0.8rem;
  }
</style>
