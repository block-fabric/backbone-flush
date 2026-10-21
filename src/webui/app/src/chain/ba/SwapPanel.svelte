<script lang="ts">
  // A swap: pay an amount of one asset into its pool with another, get what the pool gives (quoted by the
  // node: quoteswap), at least that less the slippage chosen; the price, the spot price, the price impact
  // (asked about above 2%, refused above 10%, as the wallet refuses it), the pool's fee; the curve of the pool,
  // and where the trade moves it. The Swap card of the Trade tab of the Qt window of BitAssets.
  //
  // Layers' DEX: a pair may have pools of several kinds, and a trade may go through several pools. Every pool of the pair
  // is quoted (getswapquote) and the best routes (quoteroute); the best is chosen, or the one the user picks. One hop in
  // the pool of the default kind goes as before (swapasset); one pool of another kind by swappools; a route by swaproute,
  // its pools named (ba/kinds.ts executionOf).
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { rpc } from '../../lib/rpc';
  import { toast, wallets } from '../../lib/state.svelte';
  import AssetSelect from './AssetSelect.svelte';
  import Chart from './Chart.svelte';
  import UnitsInput from './UnitsInput.svelte';
  import { amountParam, approx, assetArg, CHN_ID, curvePoints, formatAmount, formatPrice, formatUnits, isChn, lessSlippage, MAX_IMPACT, MIN_CHN_PAYOUT, unitsAtMost, unitsOf, WARN_IMPACT, type SwapQuote, type Amount } from './model';
  import { compactNumber } from './axis';
  import { hiddenAsset } from './visible.svelte';
  import { assetById, ba, decimalsOf, labelOf, nameOf, poolById, poolOf, poolsOf, refreshBitAssets, tradeIntent } from './store.svelte';
  import { nav } from '../../lib/nav.svelte';
  import PoolKind from './PoolKind.svelte';
  import { chooseOption, CURVE_NAMES, executionOf, feePercent, flipKind, kindOf, tradeOptions, type MarketQuote, type RouteQuote, type TradeOption } from './kinds';
  import Term from '../../components/Term.svelte';
  import { simpleMode } from '../../lib/uimode.svelte';
  import './terms';

  // Simple mode (lib/uimode.svelte.ts): the quote in plain words, the slippage under "More options", how a
  // swap works in place of the curve of the pool. The same call (swapasset) as Advanced.
  const simple = $derived(simpleMode());
  let moreOptions = $state(false);

  let pay = $state(CHN_ID);
  let get = $state('');
  let amountText = $state('');
  let amount = $state<bigint | null>(null);
  let slippage = $state(0.5);
  let custom = $state('');
  let quote = $state<SwapQuote | null>(null);
  let quoteError = $state('');
  let busy = $state(false);
  /** Every pool of the pair quoted (getswapquote), and the routes through several pools (quoteroute): null without them. */
  let market = $state<MarketQuote | null>(null);
  let routes = $state<RouteQuote | null>(null);
  /** Whether the quotes asked for have not all come back yet. */
  let quoting = $state(false);
  /** The way the user picked (an option's key; '' for the best). */
  let picked = $state('');

  /** The assets that trade in a pool: those the lists offer. */
  const traded = $derived([...new Set(ba.pools.flatMap((p) => [p.asset_a_id, p.asset_b_id]))].filter((id) => !isChn(id)));
  // Without what the wallet hides (its asset filters), unless chosen already.
  const choices = $derived(traded.filter((id) => id === pay || id === get || !hiddenAsset(id, labelOf(id))).map((id) => ({ id, label: labelOf(id), seq: assetById(id)?.seq })));

  // A pair asked for by another page (an asset's Trade button).
  $effect(() => {
    if (!tradeIntent.pay && !tradeIntent.get) return;
    if (tradeIntent.pay) pay = tradeIntent.pay;
    if (tradeIntent.get) get = tradeIntent.get;
    tradeIntent.pay = '';
    tradeIntent.get = '';
  });
  // Something to get by default: the first asset that trades with what is paid.
  $effect(() => {
    if (get && get !== pay) return;
    const other = ba.pools.find((p) => p.asset_a_id === pay || p.asset_b_id === pay);
    get = other ? (other.asset_a_id === pay ? other.asset_b_id : other.asset_a_id) : (choices.find((c) => c.id !== pay)?.id ?? '');
  });

  const pool = $derived(get ? poolOf(pay, get) : undefined);
  /** The pools of the pair of every kind someone provides to. */
  const pairPools = $derived(get && get !== pay ? poolsOf(pay, get).filter((p) => !p.abandoned) : []);
  /** Layers' DEX: routes through several pools (getdexinfo active), and pools of other kinds (curves_active). */
  const routable = $derived(ba.dex?.active === true);
  const kinds = $derived(ba.dex?.curves_active === true);
  const payDecimals = $derived(decimalsOf(pay));
  const getDecimals = $derived(decimalsOf(get));
  const held = $derived.by(() => {
    if (isChn(pay)) return wallets.current !== null ? (wallets.balances[wallets.current]?.trusted ?? null) : null;
    const h = ba.mine?.assets.find((a) => a.asset === pay);
    return h ? unitsAtMost(h.balance, h.decimals) : 0n;
  });
  const slip = $derived(custom.trim() !== '' && Number(custom) >= 0 && Number(custom) < 100 ? Number(custom) : slippage);

  // The quote of the node, a moment after the last change.
  // A new block quotes again, the last quote kept meanwhile: it does not blink with every block.
  let generation = 0;
  let quoted = '';
  $effect(() => {
    const mine = ++generation;
    const a = amount;
    const p = pay;
    const g = get;
    void ba.version;
    const key = `${a}|${p}|${g}`;
    if (key !== quoted) {
      quote = null;
      quoteError = '';
      market = null;
      routes = null;
      picked = '';
      quoted = key;
    }
    const withPool = !!pool && !pool.abandoned;
    const withKinds = kinds;
    const withRoutes = routable;
    if (a === null || !g || p === g || (!withPool && !withKinds && !withRoutes)) {
      quoting = false;
      return;
    }
    quoting = true;
    const timer = setTimeout(() => {
      const amountText = amountParam(a, decimalsOf(p));
      const calls: Promise<unknown>[] = [];
      if (withPool) {
        calls.push(
          rpc
            .raw<SwapQuote>('quoteswap', [assetArg(p), amountText, assetArg(g)])
            .then((q) => {
              if (mine === generation) quote = q;
            })
            .catch((e) => {
              if (mine === generation) quoteError = errorText(e);
            }),
        );
      }
      // Every pool of the pair, of every kind (no route: quoteroute's say their hops).
      if (withKinds) {
        calls.push(
          rpc
            .raw<MarketQuote>('getswapquote', [assetArg(p), amountText, assetArg(g), { max_hops: 0 }])
            .then((q) => {
              if (mine === generation) market = q && Array.isArray(q.pools) ? q : null;
            })
            .catch(() => undefined),
        );
      }
      if (withRoutes) {
        calls.push(
          rpc
            .raw<RouteQuote>('quoteroute', [assetArg(p), amountText, assetArg(g), ba.dex?.max_route_hops ?? 4, 3])
            .then((q) => {
              if (mine === generation) routes = q && Array.isArray(q.routes) ? q : null;
            })
            .catch(() => undefined),
        );
      }
      void Promise.allSettled(calls).then(() => {
        if (mine === generation) quoting = false;
      });
    }, 300);
    return () => clearTimeout(timer);
  });

  /** The kind of a pool read, its asset `from` first. */
  function kindFrom(id: string, from: string) {
    const p = poolById(id);
    if (!p) return undefined;
    return p.asset_a_id === from ? kindOf(p) : flipKind(kindOf(p));
  }
  const options = $derived(
    tradeOptions({
      pay,
      get,
      getDecimals,
      single: quote && pool ? { pool: pool.pool, amount_out: quote.amount_out, price_impact: quote.price_impact, fee: quote.fee, price: quote.price, spot_price: quote.spot_price } : null,
      market,
      routes,
      kindById: kindFrom,
    }),
  );
  const chosen = $derived<TradeOption | null>(chooseOption(options, picked));
  /** Whether the trade goes as before the pool kinds: one hop in the default pool (swapasset, its curve and quote). */
  const plain = $derived(!chosen || (chosen.pools.length === 1 && chosen.defaultPools));
  const out = $derived(chosen ? chosen.out : null);
  const least = $derived(out !== null ? lessSlippage(out, slip) : null);
  const impact = $derived(chosen?.impact ?? 0);
  const problem = $derived.by(() => {
    if (!get || pay === get) return 'Pick two different assets.';
    if (!chosen) {
      if (!pool && pairPools.length === 0 && !routable) return simple ? 'No market trades these two yet.' : 'No pool trades these two yet: make one in Liquidity.';
      if (pool?.abandoned && pairPools.length === 0 && !routable) return 'This pool is closed: nobody provides liquidity to it any more. It holds only the minimum every pool keeps for good; adding liquidity reopens it.';
      if (quoteError) return quoteError;
      if (amount !== null && !quoting && (market || routes || !pool)) return simple ? 'No market trades these two yet.' : 'No pool or route trades these two yet: make a pool in Add liquidity or Create pool.';
      return '';
    }
    if (out !== null && isChn(get) && out < MIN_CHN_PAYOUT) return 'Too little: the least CHN a pool pays out is 0.0005 CHN.';
    if (!plain && isChn(pay) && amount !== null && amount < MIN_CHN_PAYOUT) return 'Too little: an order through the pools pays in 0.0005 CHN at least.';
    if (impact > MAX_IMPACT) return 'Too large for this pool: you would pay more than 10% above its price. Trade less, or wait for more liquidity.';
    return '';
  });

  function openPool(id: string) {
    tradeIntent.pool = id;
    nav.tab = 'pools';
  }

  // The curve of the pool, what it holds now and after the trade (in the asset paid).
  const reserves = $derived.by(() => {
    if (!pool) return null;
    const payFirst = pool.asset_a_id === pay;
    // As doubles: for the curve and the price shown, never for what is sent.
    return { pay: approx(payFirst ? pool.reserve_a : pool.reserve_b), get: approx(payFirst ? pool.reserve_b : pool.reserve_a) };
  });
  const curve = $derived(reserves ? curvePoints(reserves.pay, reserves.get) : []);
  const marks = $derived.by(() => {
    if (!reserves) return [];
    const now = { x: reserves.pay, y: reserves.get };
    if (!quote || !plain) return [now];
    return [now, { x: reserves.pay + approx(quote.amount_in), y: reserves.get - approx(quote.amount_out), alt: true }];
  });

  function flip() {
    const p = pay;
    pay = get || CHN_ID;
    get = p;
    amountText = '';
  }

  async function swap() {
    const wallet = wallets.current;
    if (wallet === null || amount === null || out === null || least === null || problem || busy) return;
    // What the confirmation shows is what is signed: taken now, not read again after the dialogs (the quote
    // follows the pool while they are open). The least shown is the least the wallet signs (min_out, audit 7
    // E-BA2): should the pool have moved below it, the wallet refuses.
    const sPay = pay;
    const sGet = get;
    const sAmount = amount;
    const sOut = out;
    const sLeast = least;
    const sSlip = slip;
    const sPayDecimals = payDecimals;
    const sGetDecimals = getDecimals;
    const sFee = chosen?.feePaid ?? 0;
    const sChosen = chosen;
    if (!sChosen) return;
    const sHops = sChosen.hops.map((h) => ({ ...h }));
    // Named with their numbers in the confirmations: look-alike labels told apart (audit port-2 #5).
    const payLabel = nameOf(sPay);
    const getLabel = nameOf(sGet);
    if (impact > WARN_IMPACT) {
      const r = await ask({
        title: t('Price impact'),
        message: t('This trade moves the pool\'s price: you pay %1% more than its current price for %2.', impact.toFixed(2), getLabel),
        level: 'warning',
        choices: [
          { id: 'no', label: 'Cancel' },
          { id: 'yes', label: 'Swap anyway', kind: 'danger' },
        ],
      });
      if (r.choice !== 'yes') return;
    }
    const sPlain = sChosen.pools.length === 1 && sChosen.defaultPools;
    // The way it goes, hop by hop, each pool by its kind (a route, or a pool of another kind).
    const along = sHops.map((h) => t('%1 to %2 in a %3 pool at %4', nameOf(h.from), nameOf(h.to), t(CURVE_NAMES[h.kind.curve]), feePercent(h.kind.fee))).join('; ');
    const r = await ask({
      title: t('Swap'),
      message: t('Pay %1 %2 for about %3 %4?', formatUnits(sAmount, sPayDecimals), payLabel, formatUnits(sOut, sGetDecimals), getLabel),
      informative: sPlain
        ? t('At least %1 %2, should the pool move first (slippage %3%). The pool keeps %4 %5 as its fee. %6', formatUnits(sLeast, sGetDecimals), getLabel, String(sSlip), formatAmount(sFee, sPayDecimals), payLabel, isChn(sGet) ? t('CHN comes by the coinbase of the next block.') : '')
        : t('At least %1 %2, should the pools move first (slippage %3%). Along: %4. The order clears at the end of its block; should it give less, what went in comes back. %5', formatUnits(sLeast, sGetDecimals), getLabel, String(sSlip), along, isChn(sGet) ? t('CHN comes by the coinbase of the next block.') : ''),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Swap', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      const how = executionOf(sChosen, { amount: amountParam(sAmount, sPayDecimals), slippage: sSlip, minOut: amountParam(sLeast, sGetDecimals), maxImpact: MAX_IMPACT });
      const done = await walletRpc<{ txid: string; quote: Amount; min_out: Amount }>(wallet, how.method, how.params);
      if (!done) return;
      toast('success', t('Swapping'), t('%1 %2 for about %3 %4 (at least %5), with the next block.', formatUnits(sAmount, sPayDecimals), payLabel, formatAmount(done.quote, sGetDecimals), getLabel, formatAmount(done.min_out, sGetDecimals)));
      amountText = '';
      void refreshBitAssets();
    } catch (e) {
      await inform(t('Swap'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }
</script>

<div class="ba-split swap">
  <form class="panel ba-form card" onsubmit={(e) => { e.preventDefault(); void swap(); }}>
    <div class="leg">
      <AssetSelect id="swap-pay" label="You pay" bind:value={pay} {choices} />
      <UnitsInput id="swap-amount" label="Amount" decimals={payDecimals} bind:units={amount} bind:text={amountText} max={held} unit={labelOf(pay)} />
    </div>
    <button type="button" class="btn icon flip" title="Swap the other way" aria-label="Swap the other way" onclick={flip}><Icon name="swap" size={18} /></button>
    <div class="leg">
      <AssetSelect id="swap-get" label="You get" bind:value={get} {choices} />
      <div class="field">
        <span class="label">About</span>
        <output class="get num" for="swap-amount" aria-live="polite">{out !== null ? formatUnits(out, getDecimals) : '–'}</output>
      </div>
    </div>

    {#if simple}
      <button type="button" class="btn ghost small more" aria-expanded={moreOptions} onclick={() => (moreOptions = !moreOptions)}><Icon name={moreOptions ? 'chevron' : 'chevron-right'} size={14} /> More options</button>
    {/if}
    {#if !simple || moreOptions}
    <div class="field">
      <span class="label">Slippage</span>
      <div class="ba-chips" role="group" aria-label="Slippage">
        {#each [0.1, 0.5, 1, 3] as s (s)}
          <button type="button" class="ba-chip" aria-pressed={custom.trim() === '' && slippage === s} onclick={() => { slippage = s; custom = ''; }}>{s}%</button>
        {/each}
        <input class="custom" type="text" inputmode="decimal" placeholder="Custom %" aria-label="Custom slippage, in percent" bind:value={custom} />
      </div>
      <span class="hint">How much less than quoted the trade may give, if the pool moves before it is mined.</span>
    </div>
    {/if}

    {#if problem && (amount !== null || !pool || pool.abandoned)}
      <p class="ba-problems" class:bad={!!quoteError || impact > MAX_IMPACT} role="status">{#if quoteError}<SafeText text={quoteError} />{:else}{t(problem)}{/if}</p>
    {/if}

    {#if simple && chosen && out !== null && least !== null && amount !== null}
      <div class="plain" data-testid="swap-plain">
        <p>You pay <strong>{formatUnits(amount, payDecimals)} <SafeText text={nameOf(pay)} /></strong> and get about <strong>{formatUnits(out, getDecimals)} <SafeText text={nameOf(get)} /></strong>.</p>
        <p class="muted small">At least {formatUnits(least, getDecimals)} <SafeText text={labelOf(get)} /> if the price moves first (<Term id="slippage">slippage</Term> {slip}%). {#if chosen.pools.length === 1}The market keeps {formatAmount(chosen.feePaid ?? 0, payDecimals)} <SafeText text={labelOf(pay)} /> as its <Term id="fee">fee</Term>.{:else}It goes through {chosen.pools.length} markets, each keeping its <Term id="fee">fee</Term>.{/if}</p>
        {#if impact > 2}<p class="ba-warn small">This is a large trade for this market: the price you get is {impact.toFixed(1)}% worse than its price now.</p>{/if}
      </div>
    {:else if chosen && out !== null && least !== null && amount !== null}
      <dl class="ba-lines">
        <dt>Price</dt><dd>1 <SafeText text={labelOf(get)} /> = {formatPrice(chosen.price ?? (out > 0n ? Number(amount) / 10 ** payDecimals / (Number(out) / 10 ** getDecimals) : undefined))} <SafeText text={labelOf(pay)} /></dd>
        {#if chosen.spotPrice !== undefined}<dt>Before the trade</dt><dd>{formatPrice(chosen.spotPrice)}</dd>{/if}
        {#if chosen.impact !== undefined}<dt>Price impact</dt><dd class:ba-good={impact <= 1} class:ba-warn={impact > 1 && impact <= 5} class:ba-bad={impact > 5}>{impact.toFixed(2)}%</dd>{/if}
        <dt>At least</dt><dd class="strong">{formatUnits(least, getDecimals)} <SafeText text={labelOf(get)} /></dd>
        {#if chosen && chosen.pools.length === 1}
          <dt>Pool fee ({feePercent(chosen.hops[0].kind.fee)})</dt><dd>{formatAmount(chosen.feePaid ?? 0, payDecimals)} <SafeText text={labelOf(pay)} /></dd>
        {:else if chosen}
          <dt>Fees</dt><dd>{chosen.hops.map((h) => feePercent(h.kind.fee)).join(' + ')}, one per pool</dd>
        {/if}
      </dl>
    {:else if pool && !pool.abandoned && reserves}
      <p class="ba-help">1 <SafeText text={labelOf(get)} /> = {formatPrice(reserves.pay / reserves.get)} <SafeText text={labelOf(pay)} /> in the pool now.</p>
    {/if}

    {#if !simple && options.length > 1}
      <div class="field">
        <span class="label">Ways to trade</span>
        <ul class="ways" aria-label="Ways to trade">
          {#each options as o, i (o.key)}
            <li>
              <button type="button" class="way" aria-pressed={chosen?.key === o.key} onclick={() => (picked = i === 0 ? '' : o.key)}>
                <span class="hops">
                  {#each o.hops as h, k (k)}{#if k > 0}<Icon name="chevron-right" size={12} />{/if}<PoolKind kind={h.kind} />{/each}
                  {#if o.pools.length > 1}<span class="faint small">via {#each o.path.slice(1, -1) as mid, k (k)}{#if k > 0}, {/if}<SafeText text={labelOf(mid)} />{/each}</span>{/if}
                </span>
                <span class="gets num">{formatUnits(o.out, getDecimals)}</span>
                <span class="faint small">{#if i === 0}best{:else if o.impact !== undefined}{o.impact.toFixed(2)}%{/if}</span>
              </button>
            </li>
          {/each}
        </ul>
        <span class="hint">Each pool of the pair, and routes through others: the best is used unless you pick another.</span>
      </div>
    {/if}

    <div class="row"><button type="submit" class="btn primary wide" disabled={busy || wallets.current === null || amount === null || out === null || !!problem}><Icon name="swap" size={16} /> {busy ? 'Swapping…' : 'Swap'}</button></div>
  </form>

  {#if simple}
  <section class="panel">
    <div class="panel-head"><h3>How a swap works</h3></div>
    <div class="panel-body how">
      <p>Each token trades with CHN in a <Term id="pool">market</Term> that anyone can use. Its price follows what it holds: buying makes it dearer.</p>
      <p>You see what you get before you confirm. The swap happens with the next block, in a minute or so; the token then shows in My assets.</p>
      <p class="muted small">Markets, liquidity and the curve of each market are in Advanced mode.</p>
    </div>
  </section>
  {:else}
  <section class="panel">
    <div class="panel-head"><h3>{t(chosen && !plain && chosen.pools.length > 1 ? 'The route' : 'The pool')}</h3>{#if plain && pool}<span class="muted small">{pool.swaps} trades</span>{/if}</div>
    <div class="panel-body">
      {#if chosen && !plain}
        <ol class="route" aria-label="Route">
          {#each chosen.hops as h, k (k)}
            <li>
              <span class="step-assets"><SafeText text={labelOf(h.from)} /> <Icon name="chevron-right" size={12} /> <SafeText text={labelOf(h.to)} /></span>
              <PoolKind kind={h.kind} showDefault />
              {#if h.out !== undefined}<span class="faint small">gives {formatAmount(h.out, decimalsOf(h.to))} <SafeText text={labelOf(h.to)} /></span>{/if}
              <button type="button" class="btn ghost small" onclick={() => openPool(h.pool)}>Pool</button>
            </li>
          {/each}
        </ol>
        <p class="ba-help">{#if chosen.pools.length > 1}The trade goes through each pool in turn, in one transaction: an order of its block, cleared at the end of it with the block's other orders, at one price per pool.{:else}A pool of another kind than the one Add liquidity makes: the trade names it by its id.{/if} Should it give less than the least shown, what went in comes back.</p>
      {:else if pool && !pool.abandoned && reserves}
        <Chart points={curve} marks={marks} logX yZero area={false} label={t('The curve of the pool: what it holds of each asset')} formatX={(x) => compactNumber(x, 4)} formatY={(y) => formatPrice(y, 4)} tall={220} />
        <p class="ba-help">The pool holds {formatPrice(reserves.pay)} <SafeText text={labelOf(pay)} /> and {formatPrice(reserves.get)} <SafeText text={labelOf(get)} />; their product never falls. The trade moves it along the curve, from the first point to the second: the further, the worse the price.</p>
      {:else}
        <p class="ba-empty">No pool to show.</p>
      {/if}
    </div>
  </section>
  {/if}
</div>

<style>
  .card {
    padding: 1.2rem;
  }
  .leg {
    display: grid;
    grid-template-columns: minmax(8rem, 0.8fr) minmax(0, 1.2fr);
    gap: 0.75rem;
    align-items: start;
  }
  .flip {
    align-self: center;
  }
  .get {
    display: block;
    padding: 0.45rem 0;
    font-size: 1.5rem;
    font-weight: 650;
    overflow-wrap: anywhere;
  }
  .custom {
    width: 6.5rem;
    min-height: 1.9rem;
    padding: 0 0.6rem;
    border-radius: 999px;
    font-size: 0.85rem;
  }
  .more {
    align-self: flex-start;
  }
  .plain p {
    margin: 0 0 0.35rem;
  }
  .plain p:first-child {
    font-size: 1.05rem;
  }
  .how {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
  }
  .how p {
    margin: 0;
    line-height: 1.45;
  }
  .ways {
    display: flex;
    flex-direction: column;
    gap: 0.3rem;
    margin: 0;
    padding: 0;
    list-style: none;
  }
  .way {
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto 4.5rem;
    align-items: center;
    gap: 0.6rem;
    width: 100%;
    padding: 0.45rem 0.6rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-s);
    background: transparent;
    color: inherit;
    text-align: start;
    cursor: pointer;
  }
  .way[aria-pressed='true'] {
    border-color: var(--accent);
    background: var(--accent-soft);
  }
  .hops {
    display: inline-flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.3rem;
    min-width: 0;
  }
  .gets {
    font-weight: 650;
  }
  .route {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
    margin: 0 0 0.6rem;
    padding-left: 1.2rem;
  }
  .route li {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.45rem;
  }
  .step-assets {
    display: inline-flex;
    align-items: center;
    gap: 0.25rem;
    font-weight: 600;
  }
  .wide {
    flex: 1 1 auto;
    justify-content: center;
  }
  .small {
    font-size: 0.85rem;
  }
</style>
