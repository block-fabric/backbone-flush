<script lang="ts">
  // The market index (Layers' DEX: ba/markets.ts): every pair of every kind, order books included, asked of the node a page
  // at a time (searchpools: what is typed, a sort, "Load more" by its cursor): its kind, its price, its change and volume
  // over the node's window (about a day), its liquidity; sorted by the node, by the headers of the columns. A pair opened:
  // its pool's page, or its book. The curators trusted for the Verified badges (CuratorsPanel.svelte) above it.
  import { untrack } from 'svelte';
  import { selectable, stacked } from '../../lib/actions';
  import SafeText from '../../components/SafeText.svelte';
  import { errorText } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { formatCount } from '../../lib/format';
  import { nav } from '../../lib/nav.svelte';
  import AssetBadge from './AssetBadge.svelte';
  import CuratorsPanel from './CuratorsPanel.svelte';
  import HiddenToggle from './HiddenToggle.svelte';
  import PoolKind from './PoolKind.svelte';
  import VerifiedBadge from './VerifiedBadge.svelte';
  import { kindOf } from './kinds';
  import { appendMarkets, changeDirection, formatChange, isBook, sortOfColumn, SORT_NAMES, SORT_NOTES, MARKET_SORTS, type Market, type MarketColumn, type MarketSort } from './markets';
  import { formatAmount, formatPrice, isChn } from './model';
  import { searchPools } from './search';
  import { ba, tradeIntent } from './store.svelte';
  import { hiddenAsset } from './visible.svelte';

  const PAGE = 50;
  let query = $state('');
  let typed = $state('');
  let sort = $state<MarketSort>('depth');
  let list = $state<Market[]>([]);
  let cursor = $state<string | null>(null);
  let fallback = $state(false);
  let error = $state('');
  let loading = $state(false);
  let showHidden = $state(false);

  // What is typed, a moment after the last key.
  $effect(() => {
    const text = typed;
    const timer = setTimeout(() => (query = text.trim()), 250);
    return () => clearTimeout(timer);
  });

  // The first page of the query, again as it changes; with each block, as many as are shown (in one page, up to 200).
  let generation = 0;
  let queried = '';
  $effect(() => {
    const key = `${query}|${sort}`;
    void ba.version;
    const fresh = key !== queried;
    queried = key;
    const limit = fresh ? PAGE : Math.min(200, Math.max(PAGE, untrack(() => list.length)));
    const mine = ++generation;
    loading = true;
    searchPools({ query, filters: {}, sort, limit })
      .then((page) => {
        if (mine !== generation) return;
        list = page.items as Market[];
        cursor = page.cursor;
        fallback = page.fallback;
        error = '';
      })
      .catch((e) => {
        if (mine === generation) error = errorText(e);
      })
      .finally(() => {
        if (mine === generation) loading = false;
      });
  });

  async function loadMore() {
    if (!cursor || loading) return;
    const mine = ++generation;
    loading = true;
    try {
      const page = await searchPools({ query, filters: {}, sort, cursor, limit: PAGE });
      if (mine !== generation) return;
      list = appendMarkets(list, page.items as Market[]);
      cursor = page.cursor;
    } catch (e) {
      if (mine === generation) error = errorText(e);
    } finally {
      if (mine === generation) loading = false;
    }
  }

  const isHidden = (m: Market) => hiddenAsset(m.asset_a_id, m.asset_a) || hiddenAsset(m.asset_b_id, m.asset_b);
  const hiddenCount = $derived(showHidden ? 0 : list.filter(isHidden).length);
  const shown = $derived(showHidden ? list : list.filter((m) => !isHidden(m)));
  const withChn = (m: Market) => isChn(m.asset_a_id) || isChn(m.asset_b_id);

  function open(m: Market) {
    if (isBook(m)) {
      tradeIntent.bookBase = isChn(m.asset_a_id) ? m.asset_b_id : m.asset_a_id;
      tradeIntent.bookQuote = isChn(m.asset_a_id) ? m.asset_a_id : m.asset_b_id;
      nav.tab = 'book';
      return;
    }
    tradeIntent.pool = m.pool;
    nav.tab = 'pools';
  }

  const COLUMNS: { id: MarketColumn; label: string; end: boolean }[] = [
    { id: 'pair', label: 'Pair', end: false },
    { id: 'kind', label: 'Kind', end: false },
    { id: 'price', label: 'Price', end: true },
    { id: 'change', label: '24h change', end: true },
    { id: 'volume', label: '24h volume', end: true },
    { id: 'liquidity', label: 'Liquidity', end: true },
  ];
</script>

<CuratorsPanel />
<section class="panel">
  <div class="panel-head">
    <input type="search" class="search" placeholder="Find a pair by an asset" aria-label="Find a pair" bind:value={typed} />
    <label class="sort"><span class="muted">Sort</span>
      <select bind:value={sort} aria-label="Sort pairs">
        {#each MARKET_SORTS as s (s)}<option value={s}>{t(SORT_NAMES[s])}</option>{/each}
      </select>
    </label>
  </div>
  <p class="ba-help note">Volume and liquidity are counted in ECX, for pairs with ECX (the ECX traded over about a day, the ECX held). {#if SORT_NOTES[sort]}{t(SORT_NOTES[sort])}{/if}{#if fallback} This node has no market index: the pools it lists, without their change and volume.{/if}</p>
  <HiddenToggle count={hiddenCount} bind:show={showHidden} />
  {#if error}<p class="ba-problems bad" role="alert"><SafeText text={error} /></p>{/if}
  {#if shown.length === 0}
    <p class="ba-empty">{loading ? t('Reading the markets…') : query ? t('No pair matches.') : t('No market yet: create a pool, or place an order in a book.')}</p>
  {:else}
    <div class="ba-wrap">
      <table class="ba-table" use:stacked aria-label="Markets">
        <thead>
          <tr>
            {#each COLUMNS as c (c.id)}
              {@const by = sortOfColumn(c.id)}
              <th class:end={c.end} aria-sort={by && by === sort ? 'descending' : undefined}>
                {#if by}<button type="button" class="sorter" class:on={by === sort} title={t('Sort: %1', t(SORT_NAMES[by]))} onclick={() => (sort = by)}>{t(c.label)}{#if by === sort} <span aria-hidden="true">▾</span>{/if}</button>{:else}{t(c.label)}{/if}
              </th>
            {/each}
          </tr>
        </thead>
        <tbody>
          {#each shown as m (m.pool)}
            {@const dir = changeDirection(m.price_change)}
            <tr class="pick" onclick={() => open(m)} use:selectable={() => open(m)}>
              <td>
                <span class="pair">
                  <AssetBadge id={m.asset_a_id} label={m.asset_a} size="s" /><VerifiedBadge id={m.asset_a_id} />
                  <span class="faint">/</span>
                  <AssetBadge id={m.asset_b_id} label={m.asset_b} size="s" /><VerifiedBadge id={m.asset_b_id} />
                </span>
              </td>
              <td><PoolKind kind={kindOf(m)} /></td>
              <td class="end" translate="no">{formatPrice(m.price)}</td>
              <td class="end" translate="no"><span class:ba-good={dir === 'up'} class:ba-bad={dir === 'down'} data-testid="price-change">{formatChange(m.price_change)}</span></td>
              <td class="end" translate="no">{withChn(m) && m.volume_window !== undefined ? formatAmount(m.volume_window, 8, { max: 2 }) : '–'}</td>
              <td class="end" translate="no">{withChn(m) && m.depth !== undefined ? formatAmount(m.depth, 8, { max: 2 }) : '–'}</td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
    <p class="ba-help more">{t('Showing %1 pairs.', formatCount(shown.length))} {#if cursor}<button type="button" class="btn small" disabled={loading} onclick={loadMore}>Load more</button>{/if}</p>
  {/if}
</section>

<style>
  .sort {
    display: inline-flex;
    align-items: center;
    gap: 0.45rem;
  }
  .search {
    flex: 1 1 14rem;
    max-width: 24rem;
  }
  .note {
    padding: 0 1rem;
  }
  .pair {
    display: inline-flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.4rem;
    min-width: 0;
  }
  .sorter {
    padding: 0;
    border: 0;
    background: none;
    color: inherit;
    font: inherit;
    cursor: pointer;
  }
  .sorter:hover,
  .sorter.on {
    color: var(--text);
  }
  .more {
    padding: 0.6rem 1rem;
  }
</style>
