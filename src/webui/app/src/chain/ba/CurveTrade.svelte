<script lang="ts">
  // The Trade box of an asset on its launch curve (ba/AssetView.svelte; owner decision, 2026-11-11: the asset fixed by
  // its page): buying and selling on the curve, quoted by the node (quotelaunch: what comes out, what comes back when a
  // buy passes the curve's end, the fee, the price, whether it graduates), sent as an order of the block's batch
  // (buyfromlaunch, selltolaunch), with the slippage limit. The route is the curve: an asset on its curve trades nowhere
  // else until it graduates.
  import { untrack } from 'svelte';
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { rpc } from '../../lib/rpc';
  import { toast, wallets } from '../../lib/state.svelte';
  import UnitsInput from './UnitsInput.svelte';
  import { launchDecimals, type Launch, type LaunchQuote } from './launch';
  import { amountParam, approx, assetArg, formatAmount, formatPercent, formatPrice, formatUnits, lessSlippage, minChnPayout, minChnText, unitsAtMost, unitsOf, type Amount } from './model';
  import { ba, nameOf, refreshBitAssets } from './store.svelte';
  import { TICKER } from '../config';

  /** side: the side the route names ("asset/<id>/sell": ba/assetactions.ts), its amount focused when the route asks for a side. */
  let { launch, side: routeSide = 'buy', focus = false }: { launch: Launch; side?: 'buy' | 'sell'; focus?: boolean } = $props();
  const token = $derived(launch.token);
  const DEC = $derived(launchDecimals(launch));

  let side = $state<'buy' | 'sell'>(untrack(() => routeSide));
  $effect(() => {
    side = routeSide;
  });
  let focused = false;
  $effect(() => {
    if (!focus || focused) return;
    focused = true;
    queueMicrotask(() => document.getElementById('launch-amount')?.focus());
  });
  let amount = $state<bigint | null>(null);
  let amountText = $state('');
  let slippage = $state(1);
  let quote = $state<LaunchQuote | null>(null);
  let quoteError = $state('');
  let busy = $state(false);
  const inDecimals = $derived(side === 'buy' ? 8 : DEC);
  const outDecimals = $derived(side === 'buy' ? DEC : 8);
  const held = $derived.by(() => {
    if (side === 'buy') return wallets.current !== null ? (wallets.balances[wallets.current]?.trusted ?? null) : null;
    const h = ba.mine?.assets.find((x) => x.asset === token);
    return h ? unitsAtMost(h.balance, DEC) : 0n;
  });
  let generation = 0;
  $effect(() => {
    const id = token;
    const a = amount;
    const s = side;
    const d = inDecimals;
    void ba.version;
    const gen = ++generation;
    quoteError = '';
    if (a === null) {
      quote = null;
      return;
    }
    const timer = setTimeout(() => {
      rpc
        .raw<LaunchQuote>('quotelaunch', [assetArg(id), amountParam(a, d), s])
        .then((q) => {
          if (gen === generation) quote = q && typeof q === 'object' ? q : null;
        })
        .catch((e) => {
          if (gen === generation) {
            quote = null;
            quoteError = errorText(e);
          }
        });
    }, 300);
    return () => clearTimeout(timer);
  });
  const out = $derived(quote ? unitsOf(quote.amount_out, outDecimals) : null);
  const least = $derived(out !== null ? lessSlippage(out, slippage) : null);
  const backUnits = $derived(quote ? unitsOf(quote.back, 8) : 0n);
  const problem = $derived.by(() => {
    if (amount === null) return '';
    if (side === 'buy' && amount < minChnPayout()) return `A buy pays ${minChnText()} ${TICKER} at least.`;
    if (side === 'buy' && approx(launch.min_buy) > 0 && amount < unitsOf(launch.min_buy, 8)) return 'Less than the least a buy of this launch pays: it would be refunded.';
    if (side === 'buy' && approx(launch.max_buy) > 0 && amount > unitsOf(launch.max_buy, 8)) return 'More than the most a buy of this launch pays: it would be refunded.';
    if (side === 'sell' && out !== null && out < minChnPayout()) return `Too little: the least ${TICKER} paid out is ${minChnText()} ${TICKER}.`;
    if (out === 0n) return 'Too little: it buys nothing.';
    return '';
  });

  async function trade() {
    const wallet = wallets.current;
    if (wallet === null || busy || amount === null || out === null || least === null || problem || quoteError) return;
    // What the confirmation shows is what is signed: taken now (the quote follows the curve while the dialog is open).
    const sSide = side;
    const sAmount = amount;
    const sOut = out;
    const sLeast = least;
    const sSlip = slippage;
    const sIn = inDecimals;
    const sOutD = outDecimals;
    const sBack = backUnits;
    const sGraduates = quote?.graduates === true;
    const sToken = token;
    const label = nameOf(sToken, launch.ticker);
    const r = await ask({
      title: sSide === 'buy' ? t('Buy from a launch') : t('Sell to a launch'),
      message: sSide === 'buy' ? t('Pay %1 %4 for about %2 %3?', formatUnits(sAmount, sIn), formatUnits(sOut, sOutD), label, TICKER) : t('Sell %1 %2 for about %3 %4?', formatUnits(sAmount, sIn), label, formatUnits(sOut, sOutD), TICKER),
      informative: t('At least %1, should the curve move first (slippage %2%). The order clears at the end of its block, at one price for all of the block. %3', formatUnits(sLeast, sOutD), String(sSlip), sBack > 0n ? t('It passes the curve\'s end: about %1 %2 comes back, and the launch graduates.', formatUnits(sBack, 8), TICKER) : sGraduates ? t('It takes the launch to graduation.') : ''),
      level: 'question',
      ...(sSide === 'buy' && launch.mintable === true ? { tags: [{ label: t('Unlocked Supply'), title: t('The creator can still mint more') }] } : {}),
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: sSide === 'buy' ? 'Buy' : 'Sell', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      const done = await walletRpc<{ txid: string; quote: Amount; min_out: Amount }>(wallet, sSide === 'buy' ? 'buyfromlaunch' : 'selltolaunch', [assetArg(sToken), amountParam(sAmount, sIn), sSlip, amountParam(sLeast, sOutD)]);
      if (!done) return;
      toast('success', sSide === 'buy' ? t('Buying') : t('Selling'), t('For at least %1, with the next block.', formatAmount(done.min_out, sOutD)));
      amountText = '';
      void refreshBitAssets();
    } catch (e) {
      await inform(sSide === 'buy' ? t('Buy from a launch') : t('Sell to a launch'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }
</script>

<form class="panel ba-form card" data-testid="trade-box" data-route="curve" aria-label={t('Trade %1', launch.ticker)} onsubmit={(e) => { e.preventDefault(); void trade(); }}>
  <h3 class="title">Trade <SafeText text={launch.ticker} /></h3>
  <div class="ba-chips" role="group" aria-label="Buy or sell">
    <button type="button" class="ba-chip" aria-pressed={side === 'buy'} onclick={() => { side = 'buy'; amountText = ''; }}>Buy</button>
    <button type="button" class="ba-chip" aria-pressed={side === 'sell'} onclick={() => { side = 'sell'; amountText = ''; }}>Sell</button>
  </div>
  <UnitsInput id="launch-amount" label={side === 'buy' ? t('You pay') : t('You sell')} decimals={inDecimals} bind:units={amount} bind:text={amountText} max={held} unit={side === 'buy' ? TICKER : launch.ticker} />
  {#if side === 'buy' && (approx(launch.min_buy) > 0 || approx(launch.max_buy) > 0)}
    <p class="ba-help">A buy pays {#if approx(launch.min_buy) > 0}at least {formatAmount(launch.min_buy, 8)} {TICKER}{/if}{#if approx(launch.min_buy) > 0 && approx(launch.max_buy) > 0} and {/if}{#if approx(launch.max_buy) > 0}at most {formatAmount(launch.max_buy, 8)} {TICKER}{/if}: outside, it is refunded.</p>
  {/if}
  <div class="field">
    <span class="label">Slippage limit</span>
    <div class="ba-chips" role="group" aria-label="Slippage">
      {#each [0.5, 1, 3, 5] as s (s)}<button type="button" class="ba-chip" aria-pressed={slippage === s} onclick={() => (slippage = s)}>{s}%</button>{/each}
    </div>
  </div>
  {#if quoteError}<p class="ba-problems bad" role="status"><SafeText text={quoteError} /></p>
  {:else if problem}<p class="ba-problems" role="status">{t(problem)}</p>{/if}
  {#if quote && out !== null && least !== null}
    <dl class="ba-lines" data-testid="launch-quote">
      <dt>You get about</dt><dd class="strong">{formatUnits(out, outDecimals)} {#if side === 'buy'}<SafeText text={launch.ticker} />{:else}{TICKER}{/if}</dd>
      <dt>At least</dt><dd>{formatUnits(least, outDecimals)}</dd>
      {#if backUnits > 0n}<dt>Back</dt><dd class="ba-warn">{formatUnits(backUnits, 8)} {TICKER}: the buy passes the curve's end</dd>{/if}
      <dt>Route</dt><dd>Launch curve</dd>
      <dt>Price</dt><dd>{formatPrice(Number(quote.price))} {TICKER} each</dd>
      <dt>Price impact</dt><dd class:ba-good={Number(quote.price_impact) <= 1} class:ba-warn={Number(quote.price_impact) > 1}>{Number(quote.price_impact).toFixed(2)}%</dd>
      <dt>Fee ({launch.fee}%)</dt><dd>{formatAmount(quote.fee, 8)} {TICKER}</dd>
      <dt>To graduation after</dt><dd>{formatPercent(Number(quote.progress_after))}</dd>
    </dl>
    {#if quote.graduates}<p class="ba-help strong" data-testid="launch-graduates"><Icon name="check" size={14} /> This trade takes the launch to graduation: the asset then trades in its pool with {TICKER}.</p>{/if}
  {/if}
  <div class="row"><button type="submit" class="btn primary wide" disabled={busy || wallets.current === null || amount === null || out === null || !!problem || !!quoteError}>{busy ? t('Sending…') : side === 'buy' ? t('Buy') : t('Sell')}</button></div>
  <p class="ba-help">The asset is fixed by this page. The route is chosen for you: the curve now, its pool after graduation.</p>
</form>

<style>
  .card {
    padding: 1.2rem;
  }
  .title {
    margin: 0;
  }
  .wide {
    flex: 1 1 auto;
    justify-content: center;
  }
  .strong {
    font-weight: 600;
  }
</style>
