<script lang="ts">
  // A pool being made and funded over the full range (ba/store.svelte.ts PendingFund): its steps, where it is, and the
  // step that needs the user's word now (the wallet confirms each: createpool, then openposition), never a dialog by
  // itself. In Launch (straight into a pool), Pools (New pool) and Portfolio (Being issued).
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { go } from '../../lib/nav.svelte';
  import { dropFunding, fundPosition, fundStageOf, labelOf, makeFundedPool, tradeIntent, type PendingFund } from './store.svelte';

  let { fund }: { fund: PendingFund } = $props();

  const stage = $derived(fundStageOf(fund));
  let busy = $state(false);
  async function run(step: () => Promise<boolean>) {
    if (busy) return;
    busy = true;
    try {
      await step();
    } finally {
      busy = false;
    }
  }
  function openPool() {
    tradeIntent.pool = fund.pool ?? '';
    go('pools', 'pools');
  }
  const price = $derived(Number(fund.quoteAmount) / Number(fund.amount));
</script>

<section class="panel fund-card" data-testid="fund-card" data-stage={stage} aria-label={t('The pool of %1', fund.name)}>
  <div class="head">
    <Icon name={stage === 'done' ? 'check' : 'pool'} size={18} />
    <strong><SafeText text={fund.name} /> / <SafeText text={labelOf(fund.quote)} /></strong>
    <span class="faint small">{fund.fee}% · 1 = {Number.isFinite(price) ? price.toPrecision(6) : '–'} <SafeText text={labelOf(fund.quote)} /></span>
  </div>
  <ol class="steps">
    <li class:ok={stage !== 'asset'} class:now={stage === 'asset'}>{stage === 'asset' ? t('The asset: being registered') : t('The asset: registered')}</li>
    <li class:ok={stage === 'position' || stage === 'done'} class:now={stage === 'pool' || stage === 'pool-sent'}>{stage === 'pool' ? t('Its pool: your confirmation') : stage === 'pool-sent' ? t('Its pool: in the next block') : t('Its pool')}</li>
    <li class:ok={stage === 'done'} class:now={stage === 'position'}>{stage === 'position' ? t('Your liquidity, over the full range: your confirmation') : stage === 'done' ? t('Your liquidity, over the full range: sent') : t('Your liquidity, over the full range')}</li>
  </ol>
  <p class="faint small">{fund.amount} <SafeText text={fund.name} /> and {fund.quoteAmount} <SafeText text={labelOf(fund.quote)} /> go in.</p>
  {#if fund.error}<p class="ba-problems bad" role="status"><SafeText text={fund.error} /></p>{/if}
  <div class="row">
    {#if stage === 'pool'}<button type="button" class="btn small primary" disabled={busy} onclick={() => run(() => makeFundedPool(fund))}>Make its pool</button>
    {:else if stage === 'position'}<button type="button" class="btn small primary" disabled={busy} onclick={() => run(() => fundPosition(fund))}>Put in your liquidity</button>{/if}
    {#if fund.pool && (stage === 'position' || stage === 'done')}<button type="button" class="btn small" onclick={openPool}>Open the pool</button>{/if}
    {#if stage !== 'done' && !fund.poolTx}<button type="button" class="btn small ghost" onclick={() => dropFunding(fund)}>Forget it</button>{/if}
  </div>
</section>

<style>
  .fund-card {
    padding: 0.9rem 1rem;
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
  }
  .head {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.5rem;
  }
  .steps {
    margin: 0;
    padding-left: 1.2rem;
    display: flex;
    flex-direction: column;
    gap: 0.2rem;
    color: var(--text-muted);
  }
  .steps .ok {
    color: var(--text);
  }
  .steps .now {
    color: var(--accent);
    font-weight: 600;
  }
  .small {
    font-size: 0.85rem;
  }
</style>
