<script lang="ts">
  // Launch (owner decision, 2026-11-11): the assets on their launch curve (listlaunches: price, what each raised of its
  // graduation with a bar, its fee; a row opens the asset's page, where it is bought and sold) and Create asset, the one
  // create wizard (ba/CreateWizard.svelte: on a curve, straight into a pool the creator funds, or with no market yet),
  // opened by the tabs "create" (the market to choose), "launch", "fund" and "register" (the market chosen at first).
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import { selectable, stacked } from '../../lib/actions';
  import { t } from '../../lib/i18n.svelte';
  import { formatCount } from '../../lib/format';
  import { nav } from '../../lib/nav.svelte';
  import { simpleMode } from '../../lib/uimode.svelte';
  import '../ba/ba.css';
  import { barWidth } from '../ba/actions';
  import AssetBadge from '../ba/AssetBadge.svelte';
  import CreateWizard from '../ba/CreateWizard.svelte';
  import HiddenToggle from '../ba/HiddenToggle.svelte';
  import LaunchView from '../ba/LaunchView.svelte';
  import { launchRoute, parseDexRoute } from '../ba/assetactions';
  import type { MarketKind } from '../ba/create';
  import { approx, formatAmount, formatPercent, formatPrice } from '../ba/model';
  import { ba, followDexRoute, launchesRead, loadLaunchFlags, startBitAssets } from '../ba/store.svelte';
  import { hiddenAsset } from '../ba/visible.svelte';
  import { TICKER } from '../config';

  $effect(() => startBitAssets());
  $effect(() => {
    void nav.tab;
    followDexRoute('launch');
  });
  $effect(() => {
    void ba.version;
    loadLaunchFlags();
  });

  const simple = $derived(simpleMode());
  const head = $derived(nav.tab.split('/')[0]);
  /** The wizard, and the market it opens on: "create" leaves it to the user (a curve first, as the list above it). */
  const WIZARD: Record<string, MarketKind> = { create: 'curve', launch: 'curve', fund: 'pool', register: 'none' };
  const wizard = $derived<MarketKind | null>(head in WIZARD ? WIZARD[head] : null);

  let showHidden = $state(false);
  const curves = $derived(launchesRead.list.filter((l) => l.status !== 'graduated'));
  const hiddenCount = $derived(showHidden ? 0 : curves.filter((l) => hiddenAsset(l.token, l.ticker)).length);
  const list = $derived((showHidden ? curves : curves.filter((l) => !hiddenAsset(l.token, l.ticker))).slice().sort((x, y) => approx(y.raised) - approx(x.raised) || y.height - x.height));
  const more = $derived(Math.max(0, launchesRead.total - launchesRead.list.length));
  const route = $derived(parseDexRoute(nav.tab).launch);
  const open = (token: string) => (nav.tab = launchRoute(token));
</script>

<div class="page ba-page launch-page">
  {#if route}
    <LaunchView token={route.token} side={route.side} back={() => (nav.tab = 'launches')} />
  {:else if wizard}
    <button type="button" class="btn ghost small ba-back" onclick={() => (nav.tab = 'launches')}><Icon name="back" size={15} /> Back to Launch</button>
    <header class="page-head">
      <h1>{simple ? 'Create an asset' : 'Create asset'}</h1>
      <span class="muted">An asset of your own: on a launch curve, straight into a pool you fund with {TICKER}, or with no market yet</span>
    </header>
    {#key head}<CreateWizard embedded market={wizard} />{/key}
  {:else}
    <header class="page-head">
      <h1>Launch</h1>
      <span class="muted">Assets sold from a curve. Once an asset has raised its target it graduates and its pool opens by itself.</span>
      <button type="button" class="btn small primary create-asset" data-testid="create-asset" onclick={() => (nav.tab = 'create')}><Icon name="rocket" size={15} /> Create asset</button>
    </header>
    {#if ba.error}<p class="ba-problems bad" role="alert"><SafeText text={ba.error} /></p>{/if}
    <section class="panel" data-testid="launch-list">
      <div class="panel-head"><h3>On their curve now</h3><span class="muted small">{formatCount(curves.length)}</span></div>
      <HiddenToggle count={hiddenCount} bind:show={showHidden} />
      {#if !launchesRead.loaded}
        <p class="ba-empty" role="status">Loading…</p>
      {:else if list.length === 0}
        <p class="ba-empty">No asset is on a launch curve now. Create one: it is sold from its curve until it graduates.</p>
      {:else}
        <div class="ba-wrap">
          <table class="ba-table" use:stacked aria-label="Assets on their curve">
            <thead><tr><th>Asset</th><th class="end">Price ({TICKER})</th><th>Raised of its target</th><th class="end">Fee</th></tr></thead>
            <tbody>
              {#each list as l (l.token)}
                <tr class="pick" data-testid="launch-row" onclick={() => open(l.token)} use:selectable={() => open(l.token)}>
                  <td><AssetBadge id={l.token} label={l.ticker} display={l.display_name ?? ''} size="s" /></td>
                  <td class="end">{formatPrice(Number(l.price))}</td>
                  <td>
                    <span class="raised">
                      <span class="ba-bar"><span use:barWidth={Math.min(100, Math.max(0, Number(l.progress) || 0)) / 100}></span></span>
                      <span>{formatAmount(l.raised, 8, { max: 4 })} of {formatAmount(l.graduation, 8, { max: 4 })} {TICKER} · {formatPercent(Number(l.progress) || 0)}</span>
                    </span>
                  </td>
                  <td class="end" translate="no">{Number(l.fee)}%</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
      {#if more > 0}<p class="ba-help">{t('And %1 older launches: find one by its asset in Assets.', formatCount(more))}</p>{/if}
    </section>
  {/if}
</div>

<style>
  .create-asset {
    margin-inline-start: auto;
  }
  .raised {
    display: inline-flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.5rem;
  }
  .raised .ba-bar {
    width: 6rem;
  }
  .small {
    font-size: 0.85rem;
  }
</style>
