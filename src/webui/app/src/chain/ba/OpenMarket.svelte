<script lang="ts">
  // Open a market for an asset that exists (its page, ba/AssetView.svelte): a bonding curve (curveasset: its controller puts
  // part of the supply on a curve, 3/4 sold there, 1/4 into the locked pool at graduation; from the launch parameters,
  // never launched, no pool with the coin yet), a pool (Create pool: any kind and fee tier), an order-book listing (a
  // limit order in its book: Order book) or a Dutch auction (Sell by auction). The same choices as flush.markets' asset
  // page (its "Open a market", which writes flush: links of the same calls).
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { rpc } from '../../lib/rpc';
  import { toast, wallets } from '../../lib/state.svelte';
  import UnitsInput from './UnitsInput.svelte';
  import { bpsOf, canCurve, curveAssetParams, curveMaxUnits, curveSplit, curveSupplyProblem, feeFromBps, launchParamsOf } from './create';
  import { launchFeeParts, launchFeeSplit } from './fees';
  import { DEFAULT_RULES, type LaunchStats } from './launch';
  import { amountParam, assetArg, formatUnits, isChn, unitsAtMost, type Asset, type Pool } from './model';
  import { ba, openMarketFor, refreshBitAssets, tradeIntent } from './store.svelte';
  import { TICKER } from '../config';

  let { asset, pools, launched }: { asset: Asset; pools: readonly Pool[]; launched: boolean } = $props();

  let stats = $state<LaunchStats | null>(null);
  $effect(() => {
    void ba.version;
    rpc
      .raw<LaunchStats>('getlaunchstats', [])
      .then((s) => (stats = s && typeof s === 'object' ? s : null))
      .catch(() => (stats = null));
  });
  const params = $derived(launchParamsOf(stats?.rules));
  const holding = $derived(ba.mine?.assets.find((a) => a.asset === asset.asset) ?? null);
  const heldUnits = $derived(holding ? unitsAtMost(holding.balance, asset.decimals) : 0n);
  // Its pool with the coin of the default kind: the one a curve graduates into.
  const ecxPool = $derived(pools.some((p) => (isChn(p.asset_a_id) || isChn(p.asset_b_id)) && p.default_kind !== false && p.curve !== 'book' && p.curve !== 'launch'));
  const curveOk = $derived(canCurve({ params, controlled: !!holding?.control, launched, ecxPool, heldUnits }));

  let open = $state<'' | 'curve'>('');
  let amount = $state<bigint | null>(null);
  let amountText = $state('');
  let feeBps = $state(115);
  let feeTouched = $state(false);
  $effect(() => {
    if (!feeTouched) feeBps = bpsOf(params.feeDefault);
  });
  let minBuy = $state<bigint | null>(null);
  let minBuyText = $state('');
  let maxBuy = $state<bigint | null>(null);
  let maxBuyText = $state('');
  const feePercent = $derived(feeFromBps(feeBps));
  const problem = $derived(amount === null ? '' : curveSupplyProblem(amount, asset.decimals, params));
  const split = $derived(curveSplit(amount ?? 0n));
  const feeParts = $derived({ ...launchFeeParts(stats?.rules ?? DEFAULT_RULES), fee: feePercent });
  let busy = $state(false);

  async function putOnCurve() {
    const wallet = wallets.current;
    if (wallet === null || busy || amount === null || problem || !curveOk.ok) return;
    const sAmount = amountParam(amount, asset.decimals);
    const p = curveAssetParams(assetArg(asset.asset), sAmount, { feePercent, minBuy: minBuyText.trim() && minBuy !== null ? amountParam(minBuy, 8) : '', maxBuy: maxBuyText.trim() && maxBuy !== null ? amountParam(maxBuy, 8) : '' });
    const r = await ask({
      title: t('Put %1 on a curve', asset.label),
      message: t('Put %1 %2 on a bonding curve?', formatUnits(amount, asset.decimals), asset.label),
      informative: t('%1 sold from the curve for %3, %2 into its locked pool at graduation. The fee: %4.', formatUnits(split.curve, asset.decimals, { max: 2 }), formatUnits(split.pool, asset.decimals, { max: 2 }), TICKER, launchFeeSplit(feeParts, true)),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Put it on a curve', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      const done = await walletRpc<{ txid: string; asset: string; pool: string }>(wallet, 'curveasset', p);
      if (!done) return;
      toast('success', t('On a curve'), t('%1 is on its curve with the next block.', asset.label));
      open = '';
      amountText = '';
      void refreshBitAssets();
      tradeIntent.launch = asset.asset;
    } catch (e) {
      await inform(t('Put it on a curve'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }
</script>

<section class="panel open-market" aria-label="Open a market">
  <div class="panel-head"><h3>Open a market</h3></div>
  <div class="panel-body stack">
    <div class="choices">
      <button type="button" class="panel choice" aria-expanded={open === 'curve'} disabled={!curveOk.ok} title={curveOk.reason || undefined} onclick={() => (open = open === 'curve' ? '' : 'curve')}>
        <strong><Icon name="issue" size={15} /> Bonding curve</strong>
        <span class="faint small">{curveOk.ok ? t('Sell part of the supply from a curve until it raises enough, then a locked pool.') : t(curveOk.reason)}</span>
      </button>
      <button type="button" class="panel choice" disabled={wallets.current === null} onclick={() => openMarketFor(asset.asset, 'pool')}>
        <strong><Icon name="swap" size={15} /> Pool</strong>
        <span class="faint small">With {TICKER} or another asset, any kind and fee tier, at your price.</span>
      </button>
      <button type="button" class="panel choice" onclick={() => openMarketFor(asset.asset, 'book')}>
        <strong><Icon name="book" size={15} /> Order book</strong>
        <span class="faint small">List it with a limit order, at your price.</span>
      </button>
      <button type="button" class="panel choice" disabled={!holding} onclick={() => openMarketFor(asset.asset, 'auction')}>
        <strong><Icon name="gavel" size={15} /> Dutch auction</strong>
        <span class="faint small">A price that falls block by block.</span>
      </button>
    </div>
    {#if open === 'curve' && curveOk.ok}
      <form class="ba-form" aria-label="Put it on a curve" onsubmit={(e) => { e.preventDefault(); void putOnCurve(); }}>
        <UnitsInput id="curve-amount" label="Amount on the curve" decimals={asset.decimals} bind:units={amount} bind:text={amountText} max={curveMaxUnits(heldUnits, params)} unit={asset.label} hint="3/4 sold on the curve, 1/4 into its locked pool at graduation. The control coin goes with it: the supply is fixed for good." />
        {#if problem}<p class="ba-problems" role="status"><SafeText text={t(problem)} /></p>{/if}
        <div class="field">
          <label for="curve-fee">Trade fee on the curve: {feePercent.toFixed(2)}%</label>
          <input id="curve-fee" type="range" min={bpsOf(params.feeMin)} max={bpsOf(params.feeMax)} step="5" bind:value={feeBps} oninput={() => (feeTouched = true)} />
          <span class="hint">{launchFeeSplit(feeParts, true)}.</span>
        </div>
        <UnitsInput id="curve-min" label="Least a buy pays (optional)" decimals={8} bind:units={minBuy} bind:text={minBuyText} unit={TICKER} allowZero />
        <UnitsInput id="curve-max" label="Most a buy pays (optional)" decimals={8} bind:units={maxBuy} bind:text={maxBuyText} unit={TICKER} allowZero />
        <p class="ba-help">It graduates once it raised {stats?.rules?.graduation ?? DEFAULT_RULES.graduation} {TICKER}: that {TICKER} and the quarter kept back go into its pool with {TICKER}, locked for good. Until then no new pool of it can be made; its other pools keep trading.</p>
        <div class="row"><button type="submit" class="btn primary" disabled={busy || amount === null || !!problem}>{busy ? 'Putting it on a curve…' : 'Put it on a curve'}</button></div>
      </form>
    {/if}
  </div>
</section>

<style>
  .stack {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
  }
  .choices {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(min(11rem, 100%), 1fr));
    gap: 0.6rem;
  }
  .choice {
    display: flex;
    flex-direction: column;
    gap: 0.3rem;
    padding: 0.7rem 0.8rem;
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .choice strong {
    display: inline-flex;
    align-items: center;
    gap: 0.35rem;
  }
  .choice[aria-expanded='true'] {
    border-color: var(--accent);
  }
  .choice:disabled {
    cursor: not-allowed;
    opacity: 0.6;
  }
  .small {
    font-size: 0.85rem;
  }
</style>
