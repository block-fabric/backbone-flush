<script lang="ts">
  // The launches this wallet made (My assets): their creator's address is one of the wallet's "launch creator" addresses
  // (getaddressesbylabel; launchtoken and curveasset give each launch one). For each with an allocation: "<TICKER>: x%
  // allocation (amount), claimable at graduation" with the curve's progress; once graduated, Claim (claimlaunch: the
  // allocation minted to the creator's address, and its part of the fees owed). A launch without an allocation is
  // listed when the creator is owed fees. Read again with each block.
  // TODO(rug protection): the allocation is the creator's whole at graduation; a vesting or lock of it is to come.
  import Icon from '../../components/Icon.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { go } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { toast, wallets } from '../../lib/state.svelte';
  import { barWidth } from './actions';
  import AssetBadge from './AssetBadge.svelte';
  import { claimable, creatorAllocation, type Launch, type LaunchPage } from './launch';
  import { approx, assetArg, formatAmount, formatPercent, formatUnits, type Amount } from './model';
  import { ba, refreshBitAssets, tradeIntent } from './store.svelte';
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

  function open(l: Launch) {
    tradeIntent.launch = l.token;
    go('launch', 'launches');
  }

  async function claim(l: Launch) {
    const wallet = wallets.current;
    if (wallet === null || busy) return;
    const a = creatorAllocation(l);
    const owed = claimable(l);
    const r = await ask({
      title: t('Claim'),
      message: t('Claim what your launch of %1 owes you?', l.ticker),
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
  <section class="panel my-launches" aria-labelledby="my-launches-title" data-testid="my-launches">
    <div class="panel-head"><h3 id="my-launches-title">My launches</h3><span class="muted small">{t('Your allocation and fees, claimable once each graduates')}</span></div>
    <ul class="ba-list" aria-label="My launches">
      {#each launches as l (l.token)}
        {@const a = creatorAllocation(l)}
        {@const owed = claimable(l)}
        <li class="ba-row launch" data-testid="my-launch">
          <AssetBadge id={l.token} label={l.ticker} showLabel={false} />
          <span class="main">
            <button type="button" class="linkish" onclick={() => open(l)}><strong translate="no">{l.ticker}</strong></button>{#if a}: <span data-testid="allocation-line">{a.percent}% allocation ({formatUnits(a.units, a.decimals, { max: 2 })} {l.ticker}), {a.state === 'at-graduation' ? t('claimable at graduation') : a.state === 'claimable' ? t('claimable now') : t('claimed')}</span>{:else}: {t('no allocation')}{/if}
            <span class="faint small line">
              <span class="progress"><span class="ba-bar"><span use:barWidth={a ? a.progress / 100 : Number(l.progress) / 100}></span></span><span class="num">{l.status === 'graduated' ? t('graduated') : t('%1 to graduation', formatPercent(Number(l.progress)))}</span></span>
              {#if owed.fees}<span>{t('fees owed %1 %2', formatAmount(l.creator_fees_owed, 8), TICKER)}</span>{/if}
            </span>
          </span>
          <span class="side">
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
    margin-bottom: 1rem;
  }
  .launch {
    cursor: default;
  }
  .progress {
    display: inline-flex;
    align-items: center;
    gap: 0.4rem;
  }
  .progress .ba-bar {
    width: 7rem;
  }
  .line {
    display: flex;
    flex-wrap: wrap;
    gap: 0.8rem;
  }
  .linkish {
    border: 0;
    background: none;
    padding: 0;
    color: inherit;
    font: inherit;
    cursor: pointer;
    text-decoration: underline;
  }
</style>
