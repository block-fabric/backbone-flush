<script lang="ts">
  // The launches this wallet made (My assets): their creator's address is one of the wallet's "launch creator" addresses
  // (getaddressesbylabel; launchtoken and curveasset give each launch one). For each with an allocation, a row like the
  // holdings': the asset, its allocation (percent and amount), the curve's progress, what is claimable now (or "at
  // graduation"); once graduated, Claim (claimlaunch: the allocation minted to the creator's address, and its part of
  // the fees owed). A launch without an allocation is
  // listed when the creator is owed fees. Read again with each block.
  // TODO(rug protection): the allocation is the creator's whole at graduation; a vesting or lock of it is to come.
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { go } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { toast, wallets } from '../../lib/state.svelte';
  import { barWidth } from './actions';
  import AssetBadge from './AssetBadge.svelte';
  import { compactNumber } from './axis';
  import { claimable, creatorAllocation, launchCurveFacts, type Launch, type LaunchPage } from './launch';
  import { approx, assetArg, assetText, formatAmount, formatPercent, formatPrice, formatUnits, type Amount } from './model';
  import { assetById, ba, refreshBitAssets, tradeIntent } from './store.svelte';
  import { TICKER } from '../config';

  let launches = $state<Launch[]>([]);
  let busy = $state('');

  $effect(() => {
    const wallet = wallets.current;
    void ba.version;
    if (wallet === null) {
      launches = [];
      return;
    }
    let live = true;
    (async () => {
      const labelled = await rpc.raw<Record<string, unknown>>('getaddressesbylabel', ['launch creator'], { wallet }).catch(() => ({}));
      const mine = new Set(Object.keys(labelled ?? {}));
      if (mine.size === 0) return [];
      const out: Launch[] = [];
      let cursor = '';
      // Every launch, a page of 200 at a time (a few pages on any chain of now), those of this wallet kept.
      for (let i = 0; i < 20; i++) {
        const page = await rpc.raw<LaunchPage>('listlaunches', [{ status: 'all', sort: 'newest', limit: 200, ...(cursor ? { cursor } : {}) }]);
        for (const l of page?.launches ?? []) if (mine.has(l.creator)) out.push(l);
        if (!page?.next_cursor) break;
        cursor = page.next_cursor;
      }
      return out;
    })()
      .then((list) => {
        if (live) launches = list.filter((l) => l.allocation > 0 || claimable(l).fees);
      })
      .catch(() => undefined);
    return () => {
      live = false;
    };
  });

  /** An amount of a launch's asset, compact (a supply of 210 million, 6 decimals: "4.2M"); the exact one in its title. */
  function compactUnits(units: bigint, decimals: number): string {
    const whole = Number(units) / 10 ** decimals;
    return Number.isFinite(whole) ? compactNumber(whole) : '–';
  }

  function open(l: Launch) {
    tradeIntent.launch = l.token;
    go('pools', 'launches');
  }

  async function claim(l: Launch) {
    const wallet = wallets.current;
    if (wallet === null || busy) return;
    const a = creatorAllocation(l);
    const owed = claimable(l);
    const r = await ask({
      title: t('Claim'),
      message: t('Claim what your launch of %1 owes you?', assetText(l.ticker, l.display_name, assetById(l.token)?.seq, l.token)),
      informative: t('Your fees (%1 %3)%2. They go to the launch\'s creator address, of this wallet.', formatAmount(l.creator_fees_owed, 8), owed.allocation && a ? t(', and your allocation of %1 %2 (%3%), minted now', formatUnits(a.units, a.decimals), l.ticker, String(a.percent)) : '', TICKER),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Claim', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = l.token;
    try {
      const done = await walletRpc<{ txid: string; fees: Amount; allocation: Amount }>(wallet, 'claimlaunch', [assetArg(l.token)]);
      if (!done) return;
      toast('success', t('Claimed'), approx(done.allocation) > 0 ? t('%1 %2 and %3 %4, with the next block.', String(done.allocation), l.ticker, formatAmount(done.fees, 8), TICKER) : t('%1 %2, with the next block.', formatAmount(done.fees, 8), TICKER));
      void refreshBitAssets();
    } catch (e) {
      await inform(t('Claim'), errorText(e), 'error');
    } finally {
      busy = '';
    }
  }
</script>

{#if launches.length > 0}
  <!-- One row per launch, as the holdings below it (user feedback 2026-10-24): the asset (AssetBadge), the allocation
       (its percent and amount), the curve's progress as a bar, what can be claimed, and Claim. Narrow, each row stacks:
       the asset and Claim on top, then each value beside its word. -->
  <section class="panel my-launches" aria-labelledby="my-launches-title" data-testid="my-launches">
    <div class="panel-head"><h3 id="my-launches-title">My launches</h3><span class="muted small">{t('Your allocation and fees, claimable once each graduates')}</span></div>
    <div class="ml-grid ml-head" aria-hidden="true"><span>Asset</span><span>Allocation</span><span>Curve</span><span>Claimable</span><span></span></div>
    <ul class="ba-list" aria-label="My launches">
      {#each launches as l (l.token)}
        {@const a = creatorAllocation(l)}
        {@const owed = claimable(l)}
        {@const curve = launchCurveFacts(l)}
        <li class="ml-grid ml-row" data-testid="my-launch">
          <span class="c-asset"><button type="button" class="asset" title={t('Open its launch')} onclick={() => open(l)}><AssetBadge id={l.token} label={l.ticker} display={l.display_name ?? ''} /></button></span>
          <span class="c-alloc" data-testid="allocation-line">
            <span class="cell-label">Allocation</span>
            {#if a}<span><strong class="num">{a.percent}%</strong> <span class="faint small num" title={formatUnits(a.units, a.decimals)}>{compactUnits(a.units, a.decimals)} <SafeText text={l.ticker} /></span></span>{:else}<span class="faint" title={t('no allocation')}>–</span>{/if}
          </span>
          <span class="c-curve">
            <span class="cell-label">Curve</span>
            <span class="curve-cell">
              <span class="curve"><span class="ba-bar"><span use:barWidth={curve.progress / 100}></span></span><span class="num small">{l.status === 'graduated' ? t('graduated') : curve.traded ? formatPercent(curve.progress) : t('No trades yet')}</span></span>
              {#if curve.start && curve.price !== undefined && l.status !== 'graduated'}<span class="faint small num start" data-testid="start-price" title={t('start price %1 %2', formatPrice(curve.price), TICKER)}>{t('start price %1 %2', formatPrice(curve.price, 3), TICKER)}</span>{/if}
            </span>
          </span>
          <span class="c-claim" data-testid="claimable">
            <span class="cell-label">Claimable</span>
            {#if owed.allocation || owed.fees}
              <span class="num">{#if owed.allocation && a}{compactUnits(a.units, a.decimals)} <SafeText text={l.ticker} />{/if}{#if owed.allocation && a && owed.fees}{' + '}{/if}{#if owed.fees}{formatAmount(l.creator_fees_owed, 8)} {TICKER}{/if}</span>
            {:else if a?.state === 'at-graduation'}<span class="faint small">{t('at graduation')}</span>
            {:else if a?.state === 'claimed'}<span class="faint small">{t('claimed')}</span>
            {:else}<span class="faint">–</span>{/if}
          </span>
          <span class="c-action">
            {#if owed.allocation || owed.fees}
              <button type="button" class="btn small primary" disabled={busy !== ''} onclick={() => claim(l)}><Icon name="receive" size={14} /> {busy === l.token ? 'Claiming…' : 'Claim'}</button>
            {/if}
          </span>
        </li>
      {/each}
    </ul>
  </section>
{/if}

<style>
  .my-launches {
    container: launches / inline-size;
    margin-bottom: 1rem;
  }
  /* The columns of the rows and of their words above them (the holdings' row: .ba-row, ba.css). */
  .ml-grid {
    display: grid;
    grid-template-columns: minmax(0, 2.2fr) minmax(0, 1.3fr) minmax(0, 1.2fr) minmax(0, 1.4fr) 6.5rem;
    align-items: center;
    gap: 0.75rem;
    padding: 0.7rem 1rem;
  }
  .ml-head {
    padding-block: 0 0.3rem;
    font-size: 0.75rem;
    font-weight: 600;
    color: var(--text-muted);
  }
  .ml-row {
    position: relative;
    font-variant-numeric: tabular-nums;
  }
  .ml-row:hover {
    background: var(--raised);
  }
  .ml-row > span {
    min-width: 0;
  }
  .c-asset {
    display: flex;
  }
  .asset {
    display: inline-flex;
    max-width: 100%;
    min-width: 0;
    padding: 0;
    border: 0;
    background: none;
    color: inherit;
    font: inherit;
    text-align: start;
    cursor: pointer;
  }
  .asset:hover :global(.label) {
    text-decoration: underline;
  }
  .asset:focus-visible {
    outline: 2px solid var(--accent);
    outline-offset: 2px;
    border-radius: var(--radius-s, 4px);
  }
  .c-alloc,
  .c-claim {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .curve-cell {
    display: flex;
    flex-direction: column;
    gap: 0.1rem;
    min-width: 0;
  }
  .start {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .curve {
    display: flex;
    align-items: center;
    gap: 0.45rem;
    min-width: 0;
  }
  .curve .ba-bar {
    flex: 1 1 4rem;
    max-width: 7rem;
  }
  .curve .num {
    flex: none;
  }
  .c-action {
    display: flex;
    justify-content: flex-end;
  }
  /* The word of each value: for screen readers in the columns (the words above are for the eyes), shown narrow. */
  .cell-label {
    position: absolute;
    width: 1px;
    height: 1px;
    overflow: hidden;
    clip: rect(0 0 0 0);
    white-space: nowrap;
  }
  .num {
    font-variant-numeric: tabular-nums;
  }
  /* Narrow: each row stacks, the asset and Claim on top, each value beside its word (as the stacked tables: ba.css). */
  @container launches (max-width: 40rem) {
    .ml-head {
      display: none;
    }
    .ml-row {
      grid-template-columns: minmax(0, 1fr) auto;
      gap: 0.3rem 0.75rem;
    }
    .c-asset {
      grid-row: 1;
      grid-column: 1;
    }
    .c-action {
      grid-row: 1;
      grid-column: 2;
    }
    .c-alloc,
    .c-curve,
    .c-claim {
      grid-column: 1 / -1;
      display: flex;
      justify-content: space-between;
      align-items: baseline;
      gap: 1rem;
      text-align: end;
    }
    .cell-label {
      position: static;
      width: auto;
      height: auto;
      overflow: visible;
      clip: auto;
      flex: none;
      color: var(--text-muted);
      font-size: 0.8rem;
      text-align: start;
    }
    .c-curve {
      align-items: center;
    }
    .curve {
      flex: 0 1 12rem;
      justify-content: flex-end;
    }
  }
</style>
