<script lang="ts">
  // The launches (Layers' launches: tokens sold from a bonding curve until it raises what it graduates at): the counts,
  // a list asked of the node a page at a time (listlaunches: new, on the curve, near graduation, graduated; sorted;
  // "Load more" by its cursor), each with its logo, ticker and display name, its progress to graduation, its price,
  // market value and volume, its status; a launch opened: its page (LaunchView.svelte).
  import { untrack } from 'svelte';
  import { selectable, stacked } from '../../lib/actions';
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { errorText } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { formatCount } from '../../lib/format';
  import { nav } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { barWidth } from './actions';
  import AssetBadge from './AssetBadge.svelte';
  import LaunchView from './LaunchView.svelte';
  import { appendLaunches, launchQuery, type Launch, type LaunchFilter, type LaunchPage, type LaunchSort, type LaunchStats } from './launch';
  import { formatAmount, formatPercent, formatPrice } from './model';
  import { ba, tradeIntent } from './store.svelte';
  import { TICKER } from '../config';

  const PAGE = 50;
  let filter = $state<LaunchFilter>('all');
  let sort = $state<LaunchSort>('newest');
  let near = $state(80);
  let list = $state<Launch[]>([]);
  let total = $state(0);
  let cursor = $state<string | undefined>(undefined);
  let error = $state('');
  let loading = $state(false);
  let stats = $state<LaunchStats | null>(null);
  let open = $state<string | null>(null);

  // A launch asked for by another page (the wizard once launched, a pool of a launch's curve, an asset): opened.
  $effect(() => {
    if (!tradeIntent.launch) return;
    open = tradeIntent.launch;
    tradeIntent.launch = '';
  });

  // The counts and the rules, with each block.
  $effect(() => {
    void ba.version;
    rpc
      .raw<LaunchStats>('getlaunchstats', [])
      .then((s) => {
        stats = s && typeof s === 'object' ? s : null;
      })
      .catch(() => undefined);
  });

  // The first page of the query, again as it changes; with each block, as many as are shown (in one page, up to 200).
  let generation = 0;
  let queried = '';
  $effect(() => {
    const key = `${filter}|${sort}|${near}`;
    void ba.version;
    const fresh = key !== queried;
    queried = key;
    const limit = fresh ? PAGE : Math.min(200, Math.max(PAGE, untrack(() => list.length)));
    const mine = ++generation;
    loading = true;
    rpc
      .raw<LaunchPage>('listlaunches', [launchQuery(filter, sort, near, undefined, limit)])
      .then((page) => {
        if (mine !== generation) return;
        list = page?.launches ?? [];
        total = page?.total ?? 0;
        cursor = page?.next_cursor;
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
      const page = await rpc.raw<LaunchPage>('listlaunches', [launchQuery(filter, sort, near, cursor, PAGE)]);
      if (mine !== generation) return;
      list = appendLaunches(list, page?.launches ?? []);
      total = page?.total ?? total;
      cursor = page?.next_cursor;
    } catch (e) {
      if (mine === generation) error = errorText(e);
    } finally {
      if (mine === generation) loading = false;
    }
  }

  const FILTERS: { id: LaunchFilter; label: string }[] = [
    { id: 'all', label: 'All' },
    { id: 'new', label: 'New' },
    { id: 'curve', label: 'On the curve' },
    { id: 'near', label: 'Near graduation' },
    { id: 'graduated', label: 'Graduated' },
  ];
</script>

{#if open}
  <LaunchView token={open} back={() => (open = null)} />
{:else}
  {#if stats}
    <section class="ba-figures">
      <div class="panel ba-figure"><span class="muted">Launches</span><strong>{formatCount(stats.launches)}</strong><span class="faint">{formatCount(stats.on_curve)} on their curves · {formatCount(stats.graduated)} graduated</span></div>
      <div class="panel ba-figure"><span class="muted">Raised</span><strong>{formatAmount(stats.raised, 8, { max: 2 })}</strong><span class="faint">{TICKER}, in curves and graduated</span></div>
      <div class="panel ba-figure"><span class="muted">Traded</span><strong>{formatAmount(stats.volume, 8, { max: 2 })}</strong><span class="faint">{TICKER} on the curves</span></div>
      <div class="panel ba-figure"><span class="muted">Graduation</span><strong>{formatAmount(stats.rules.graduation, 8)}</strong><span class="faint">{TICKER} raised, then a locked pool</span></div>
    </section>
  {/if}
  <section class="panel">
    <div class="panel-head">
      <div class="ba-chips" role="group" aria-label="Filter launches">
        {#each FILTERS as f (f.id)}<button type="button" class="ba-chip" aria-pressed={filter === f.id} onclick={() => (filter = f.id)}>{t(f.label)}</button>{/each}
      </div>
      {#if filter === 'near'}
        <label class="sort"><span class="muted">At least</span>
          <select bind:value={near} aria-label="Progress to graduation, at least"><option value={50}>50%</option><option value={75}>75%</option><option value={80}>80%</option><option value={90}>90%</option></select>
        </label>
      {/if}
      <label class="sort"><span class="muted">Sort</span>
        <select bind:value={sort} aria-label="Sort launches">
          <option value="newest">Newest</option>
          <option value="oldest">Oldest</option>
          <option value="volume">Most traded</option>
          <option value="progress">Nearest graduation</option>
          <option value="market_cap">Market value</option>
        </select>
      </label>
      <span class="grow"></span>
      <button type="button" class="btn small primary" onclick={() => (nav.tab = 'launch')}><Icon name="plus" size={14} /> Launch a token</button>
    </div>
    {#if error}<p class="ba-problems bad" role="alert"><SafeText text={error} /></p>{/if}
    {#if list.length === 0}
      <p class="ba-empty">{loading ? t('Reading the launches…') : filter === 'all' ? t('No launch yet: launch the first token.') : t('No launch matches.')}</p>
    {:else}
      <div class="ba-wrap">
        <table class="ba-table" use:stacked aria-label="Launches">
          <thead><tr><th>Token</th><th>To graduation</th><th class="end">Price</th><th class="end">Market value</th><th class="end">Volume</th><th>Status</th></tr></thead>
          <tbody>
            {#each list as l (l.token)}
              <tr class="pick" onclick={() => (open = l.token)} use:selectable={() => (open = l.token)}>
                <td><AssetBadge id={l.token} label={l.ticker} display={l.display_name ?? ''} size="s" /></td>
                <td>
                  <span class="progress"><span class="ba-bar"><span use:barWidth={Number(l.progress) / 100}></span></span><span class="num">{formatPercent(Number(l.progress))}</span></span>
                </td>
                <td class="end">{formatPrice(Number(l.price))}</td>
                <td class="end">{formatAmount(l.market_cap, 8, { max: 2 })}</td>
                <td class="end">{formatAmount(l.volume, 8, { max: 2 })}</td>
                <td>{#if l.status === 'graduated'}<span class="ba-tag accent">graduated</span>{:else}<span class="ba-tag">on its curve</span>{/if}{#if l.allocation > 0} <span class="ba-tag warn">allocation {l.allocation}%</span>{/if}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
      <p class="ba-help more">{t('Showing %1 of %2 launches.', formatCount(list.length), formatCount(total))} {#if cursor}<button type="button" class="btn small" disabled={loading} onclick={loadMore}>Load more</button>{/if}</p>
    {/if}
  </section>
{/if}

<style>
  .sort {
    display: inline-flex;
    align-items: center;
    gap: 0.45rem;
  }
  .progress {
    display: inline-flex;
    align-items: center;
    gap: 0.5rem;
    min-width: 9rem;
  }
  .progress .ba-bar {
    flex: 1 1 auto;
  }
</style>
