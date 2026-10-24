<script lang="ts">
  // A pool, whole (Layers' DEX: any kind): its pair and its kind (curve, fee, A or weights), its id, its price, what it
  // holds and traded, the share of this wallet; its price as this app saw it, its depth (getpoolcurve: what trades of
  // growing size do to its price, each way), its curve for a constant-product pool. And liquidity by its id: put in
  // either asset or both (addpoolliquidity: out of the pool's proportions, the fee is paid on the part a swap would
  // trade), take shares out for both assets or for one alone (removepoolliquidity), each quoted first by the node as the
  // rules work it out (quotepoolliquidity). The pool of the
  // kind addliquidity makes keeps its ways in besides (Add liquidity, My assets).
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { formatCount } from '../../lib/format';
  import { nav } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { toast, wallets } from '../../lib/state.svelte';
  import AssetBadge from './AssetBadge.svelte';
  import Chart from './Chart.svelte';
  import DepthChart from './DepthChart.svelte';
  import PoolKind from './PoolKind.svelte';
  import RangePicker from './RangePicker.svelte';
  import UnitsInput from './UnitsInput.svelte';
  import { compactNumber } from './axis';
  import { alignDepth, CURVE_HELP, DYNAMIC_FEE_HELP, feeNow, feePercent, depositAmounts, kindOf, quoteAmounts, sharesOf, type LiquidityQuote, type PoolCurve } from './kinds';
  import { amountParam, approx, assetArg, curvePoints, formatAmount, formatPercent, formatPrice, formatUnits, isChn, unitsAtMost, unitsOf, type Amount, type Pool } from './model';
  import { swapFeeSplit } from './fees';
  import { ba, decimalsOf, nameOf, priceSeries, refreshBitAssets, swapParts, tradeIntent } from './store.svelte';
  import { TICKER } from '../config';

  let { pool, back }: { pool: Pool; back: () => void } = $props();

  const kind = $derived(kindOf(pool));
  const da = $derived(decimalsOf(pool.asset_a_id));
  const db = $derived(decimalsOf(pool.asset_b_id));
  const position = $derived(ba.mine?.liquidity.find((l) => l.pool === pool.pool) ?? null);
  const series = $derived(priceSeries(pool, pool.asset_a_id).map((p) => ({ x: p.h, y: p.p })));
  /** The pool kinds apply from the next block: liquidity by the pool's id (getdexinfo). */
  const curves = $derived(ba.dex?.curves_active === true && kind.curve !== 'launch' && kind.curve !== 'concentrated' && kind.curve !== 'book');
  /** An order book (Layers' DEX: ba/book.ts): no liquidity, no depth curve; its orders on its own page. */
  const book = $derived(kind.curve === 'book');

  // --- Its depth (getpoolcurve), again with each block ---
  let depth = $state<PoolCurve | null>(null);
  let depthError = $state('');
  $effect(() => {
    const id = pool.pool;
    void ba.version;
    if (book) return;
    let live = true;
    rpc
      .raw<PoolCurve>('getpoolcurve', [id, 16])
      .then((c) => {
        if (!live) return;
        depth = c && Array.isArray(c.depth) ? c : null;
        depthError = '';
      })
      .catch((e) => {
        if (live) depthError = errorText(e);
      });
    return () => {
      live = false;
    };
  });
  const aligned = $derived(depth ? alignDepth(depth, pool) : []);

  function swapIn() {
    tradeIntent.pay = pool.asset_b_id;
    tradeIntent.get = pool.asset_a_id;
    nav.tab = 'swap';
  }
  function openBook() {
    tradeIntent.bookBase = isChn(pool.asset_a_id) ? pool.asset_b_id : pool.asset_a_id;
    tradeIntent.bookQuote = isChn(pool.asset_a_id) ? pool.asset_a_id : pool.asset_b_id;
    nav.tab = 'book';
  }
  function addDefault() {
    tradeIntent.a = pool.asset_a_id;
    tradeIntent.b = pool.asset_b_id;
    nav.tab = 'liquidity';
  }

  const held = (id: string, decimals: number) => {
    if (isChn(id)) return wallets.current !== null ? (wallets.balances[wallets.current]?.trusted ?? null) : null;
    const h = ba.mine?.assets.find((x) => x.asset === id);
    return h ? unitsAtMost(h.balance, decimals) : 0n;
  };

  // --- Put liquidity in, by the pool's id ---
  let addA = $state<bigint | null>(null);
  let addAText = $state('');
  let addB = $state<bigint | null>(null);
  let addBText = $state('');
  let addSlippage = $state(1);
  let busy = $state(false);
  // The node's quote of the deposit (quotepoolliquidity), a moment after the last change and with each block.
  let addQuote = $state<LiquidityQuote | null>(null);
  let addQuoteError = $state('');
  $effect(() => {
    const id = pool.pool;
    const amounts = depositAmounts([
      { id: pool.asset_a_id, units: addA, text: (u) => amountParam(u, da) },
      { id: pool.asset_b_id, units: addB, text: (u) => amountParam(u, db) },
    ]);
    void ba.version;
    addQuoteError = '';
    if (Object.keys(amounts).length === 0) {
      addQuote = null;
      return;
    }
    let live = true;
    const timer = setTimeout(() => {
      rpc
        .raw<LiquidityQuote>('quotepoolliquidity', [id, amounts])
        .then((q) => {
          if (live) addQuote = q && typeof q === 'object' ? q : null;
        })
        .catch((e) => {
          if (live) {
            addQuote = null;
            addQuoteError = errorText(e);
          }
        });
    }, 250);
    return () => {
      live = false;
      clearTimeout(timer);
    };
  });
  const addShares = $derived(addQuote ? unitsOf(addQuote.shares, 0) : 0n);
  const addProblem = $derived.by(() => {
    if ((addA ?? 0n) === 0n && (addB ?? 0n) === 0n) return '';
    if (pool.abandoned && ((addA ?? 0n) === 0n || (addB ?? 0n) === 0n)) return 'Nobody provides liquidity to this pool any more: both amounts reopen it, at their price (or make it again in Create pool).';
    if (addQuote && addShares === 0n) return 'Too little: it makes no shares.';
    return '';
  });
  const oneSided = $derived(((addA ?? 0n) === 0n) !== ((addB ?? 0n) === 0n));

  async function addLiquidity() {
    const wallet = wallets.current;
    if (wallet === null || busy || addProblem) return;
    // What the confirmation shows is what is signed: taken now (the store refreshes while the dialogs are open).
    const sPool = pool.pool;
    const sA = addA ?? 0n;
    const sB = addB ?? 0n;
    const sDa = da;
    const sDb = db;
    const sSlippage = addSlippage;
    const sShares = addShares;
    const amounts = depositAmounts([
      { id: pool.asset_a_id, units: sA, text: (u) => amountParam(u, sDa) },
      { id: pool.asset_b_id, units: sB, text: (u) => amountParam(u, sDb) },
    ]);
    if (Object.keys(amounts).length === 0) return;
    const la = nameOf(pool.asset_a_id, pool.asset_a);
    const lb = nameOf(pool.asset_b_id, pool.asset_b);
    const what = sA > 0n && sB > 0n ? t('%1 %2 and %3 %4', formatUnits(sA, sDa), la, formatUnits(sB, sDb), lb) : sA > 0n ? `${formatUnits(sA, sDa)} ${la}` : `${formatUnits(sB, sDb)} ${lb}`;
    const r = await ask({
      title: t('Add liquidity'),
      message: t('Put %1 into this pool?', what),
      informative: sShares > 0n ? t('About %1 shares; at least %2% fewer should the pool move first. Out of the pool\'s proportions, the part a swap would trade pays the pool\'s fee (%3).', sShares.toString(), String(sSlippage), feePercent(feeNow(kind))) : undefined,
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Add', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      const done = await walletRpc<{ txid: string; shares: Amount }>(wallet, 'addpoolliquidity', [sPool, amounts, sSlippage]);
      if (!done) return;
      toast('success', t('Adding liquidity'), t('%1 into the pool, for about %2 shares, with the next block.', what, String(done.shares)));
      addAText = '';
      addBText = '';
      void refreshBitAssets();
    } catch (e) {
      await inform(t('Add liquidity'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }

  // --- Take shares out, for both assets or one alone ---
  let percent = $state(100);
  let only = $state<'' | 'a' | 'b'>('');
  let outSlippage = $state(1);
  const removing = $derived(position ? sharesOf(unitsOf(position.shares, 0), percent) : 0n);
  // The node's quote of what the shares take out (quotepoolliquidity), both assets or one alone.
  let outQuote = $state<{ a: bigint; b: bigint } | null>(null);
  let outQuoteError = $state('');
  $effect(() => {
    const id = pool.pool;
    const shares = removing;
    const side = only;
    const onlyArg = side === 'a' ? assetArg(pool.asset_a_id) : side === 'b' ? assetArg(pool.asset_b_id) : null;
    const decimals = { a: da, b: db };
    void ba.version;
    outQuoteError = '';
    if (shares <= 0n) {
      outQuote = null;
      return;
    }
    let live = true;
    const timer = setTimeout(() => {
      rpc
        .raw<LiquidityQuote>('quotepoolliquidity', onlyArg ? [id, null, Number(shares), onlyArg] : [id, null, Number(shares)])
        .then((q) => {
          if (!live) return;
          if (!q || typeof q !== 'object' || !q.amounts) {
            outQuote = null;
            return;
          }
          const got = quoteAmounts(q.amounts, pool);
          outQuote = { a: unitsOf(got.a, decimals.a), b: unitsOf(got.b, decimals.b) };
        })
        .catch((e) => {
          if (live) {
            outQuote = null;
            outQuoteError = errorText(e);
          }
        });
    }, 250);
    return () => {
      live = false;
      clearTimeout(timer);
    };
  });

  async function takeOut() {
    const wallet = wallets.current;
    if (wallet === null || !position || busy || removing === 0n || !outQuote) return;
    const sPool = pool.pool;
    const sPercent = percent;
    const sOnly = only;
    const sSlippage = outSlippage;
    const la = nameOf(pool.asset_a_id, pool.asset_a);
    const lb = nameOf(pool.asset_b_id, pool.asset_b);
    const q = outQuote ?? { a: 0n, b: 0n };
    const quote = sOnly === 'a' ? t('About %1 %2', formatUnits(q.a, da), la) : sOnly === 'b' ? t('About %1 %2', formatUnits(q.b, db), lb) : t('About %1 %2 and %3 %4', formatUnits(q.a, da), la, formatUnits(q.b, db), lb);
    const r = await ask({
      title: t('Take liquidity out'),
      message: t('Take %1% of your shares of this pool out?', String(sPercent)),
      informative: t('%1, at least %2% less should the pool move first. %3 comes by the coinbase of the next block; assets as coins of the transaction.', quote, String(sSlippage), TICKER),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Take out', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      const onlyArg = sOnly === 'a' ? assetArg(pool.asset_a_id) : sOnly === 'b' ? assetArg(pool.asset_b_id) : null;
      const done = await walletRpc<{ txid: string; shares: Amount }>(wallet, 'removepoolliquidity', [sPool, sPercent, onlyArg, sSlippage]);
      if (!done) return;
      toast('success', t('Liquidity taken out'), t('%1 shares, with the next block.', String(done.shares)));
      void refreshBitAssets();
    } catch (e) {
      await inform(t('Take liquidity out'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }
</script>

<div class="pool-view">
  <button type="button" class="btn ghost small ba-back" onclick={back}><Icon name="back" size={15} /> All pools</button>
  <header class="panel head">
    <span class="pair"><AssetBadge id={pool.asset_a_id} label={pool.asset_a} /> <span class="faint">/</span> <AssetBadge id={pool.asset_b_id} label={pool.asset_b} /></span>
    <PoolKind {kind} showDefault />
    {#if pool.abandoned}<span class="ba-tag danger">Closed: no liquidity</span>{/if}
    <span class="grow"></span>
    <button type="button" class="btn primary" disabled={pool.abandoned} onclick={swapIn}><Icon name="swap" size={15} /> Swap</button>
    {#if kind.default_kind}<button type="button" class="btn" onclick={addDefault}><Icon name="plus" size={15} /> {pool.abandoned ? 'Reopen it' : 'Add liquidity'}</button>{/if}
  </header>
  <section class="ba-figures">
    <div class="panel ba-figure"><span class="muted">Price</span><strong>{formatPrice(pool.price)}</strong><span class="faint"><SafeText text={pool.asset_b} /> for 1 <SafeText text={pool.asset_a} /></span></div>
    {#if !book}<div class="panel ba-figure"><span class="muted">Holds</span><strong>{formatAmount(pool.reserve_a, da, { max: 4 })}</strong><span class="faint"><SafeText text={pool.asset_a} /> and {formatAmount(pool.reserve_b, db, { max: 4 })} <SafeText text={pool.asset_b} /></span></div>{/if}
    <div class="panel ba-figure"><span class="muted">Traded</span><strong>{formatCount(pool.swaps)}</strong><span class="faint">trades since block {formatCount(pool.created)}</span></div>
    {#if !book}<div class="panel ba-figure"><span class="muted">Your share</span><strong>{position ? formatPercent(position.percent) : '–'}</strong><span class="faint">{position ? t('%1 shares', formatAmount(position.shares, 0)) : t('of %1 shares', formatAmount(pool.shares, 0))}</span></div>{/if}
  </section>
  <section class="panel kind-panel" aria-label="Its kind">
    <dl class="ba-lines">
      <dt>Kind</dt><dd><PoolKind {kind} showDefault /></dd>
      {#if !book}<dt>Fee</dt><dd>{feeNow(kind)}% of what goes in: {swapFeeSplit(swapParts())}{#if kind.dynamic_fee} <span class="ba-tag warn">dynamic</span> its tier {kind.fee}%, at most {kind.fee + 2}%: {t(DYNAMIC_FEE_HELP)}{/if}</dd>{/if}
      {#if kind.curve === 'stable' && kind.amp !== undefined}<dt>Amplification</dt><dd>A = {kind.amp}: the higher, the flatter its price near the balance</dd>{/if}
      {#if kind.curve === 'weighted' && kind.weight_a !== undefined}<dt>Weights</dt><dd>{kind.weight_a}% <SafeText text={pool.asset_a} /> · {kind.weight_b ?? 100 - kind.weight_a}% <SafeText text={pool.asset_b} /></dd>{/if}
      <dt>Pool id</dt><dd class="id"><span class="mono break" translate="no">{pool.pool}</span> <CopyButton text={pool.pool} label="Copy the pool id" /></dd>
    </dl>
    <p class="ba-help">{t(CURVE_HELP[kind.curve])}</p>
  </section>
  <div class="ba-cols">
    <!-- The depth across the row (its two tables side by side), the price and the curve under it: no half of the
         page left empty beside a tall panel. -->
    {#if !book}<section class="panel depth-panel">
      <div class="panel-head"><h3>Depth</h3><span class="muted small">a trade alone in its block, each way</span></div>
      <div class="panel-body">
        {#if aligned.length}
          <DepthChart depth={aligned} labelA={pool.asset_a} labelB={pool.asset_b} decimalsA={da} decimalsB={db} />
        {:else if depthError}
          <p class="ba-problems"><SafeText text={depthError} /></p>
        {:else}
          <p class="ba-empty">The node gives no depth of this pool.</p>
        {/if}
      </div>
    </section>{/if}
    <section class="panel">
      <div class="panel-head"><h3>Price</h3><span class="muted small">as this app saw it</span></div>
      <div class="panel-body">
        <Chart points={series.length ? series : [{ x: ba.height, y: pool.price ?? 0 }]} step label={t('The price of %1 in %2', pool.asset_a, pool.asset_b)} formatX={(x) => t('block %1', String(Math.round(x)))} />
      </div>
    </section>
    {#if kind.curve === 'constant_product'}
      <section class="panel">
        <div class="panel-head"><h3>The curve</h3></div>
        <div class="panel-body">
          <Chart points={curvePoints(approx(pool.reserve_a), approx(pool.reserve_b))} marks={[{ x: approx(pool.reserve_a), y: approx(pool.reserve_b) }]} logX yZero area={false} label={t('The curve of the pool: what it holds of each asset')} formatX={(x) => compactNumber(x, 4)} formatY={(y) => formatPrice(y, 4)} />
          <p class="ba-help">Volume: {formatAmount(pool.volume_a, da)} <SafeText text={pool.asset_a} /> and {formatAmount(pool.volume_b, db)} <SafeText text={pool.asset_b} /> traded in.</p>
        </div>
      </section>
    {/if}
  </div>

  {#if kind.curve === 'concentrated'}
    <div class="ba-cols">
      <RangePicker {pool} />
    </div>
  {:else if curves}
    <div class="ba-cols">
      <form class="panel ba-form card" aria-label="Add liquidity to this pool" onsubmit={(e) => { e.preventDefault(); void addLiquidity(); }}>
        <h3>Add liquidity</h3>
        <p class="ba-help">Either asset, or both: all of it goes in. {#if !kind.default_kind}This pool is not the one Add liquidity makes: it is named by its id.{/if}</p>
        <UnitsInput id="pv-add-a" label={pool.asset_a} decimals={da} bind:units={addA} bind:text={addAText} max={held(pool.asset_a_id, da)} unit={pool.asset_a} />
        <UnitsInput id="pv-add-b" label={pool.asset_b} decimals={db} bind:units={addB} bind:text={addBText} max={held(pool.asset_b_id, db)} unit={pool.asset_b} />
        <div class="field">
          <label for="pv-add-slippage">Slippage</label>
          <select id="pv-add-slippage" bind:value={addSlippage}><option value={0.5}>0.5%</option><option value={1}>1%</option><option value={2}>2%</option><option value={5}>5%</option></select>
        </div>
        {#if addProblem}<p class="ba-problems" role="status">{t(addProblem)}</p>
        {:else if addQuoteError}<p class="ba-problems" role="status"><SafeText text={addQuoteError} /></p>
        {:else if addQuote && addShares > 0n}
          <dl class="ba-lines">
            <dt>Shares</dt><dd class="strong">{addShares.toString()}</dd>
            <dt>Share of the pool</dt><dd>{formatPercent(approx(addQuote.share_of_pool))}</dd>
          </dl>
          {#if oneSided}<p class="ba-help">One asset alone: the part a swap would trade to the pool's proportions pays its fee ({feeNow(kind)}%), included above.</p>{/if}
        {/if}
        <div class="row"><button type="submit" class="btn primary" disabled={busy || wallets.current === null || ((addA ?? 0n) === 0n && (addB ?? 0n) === 0n) || !!addProblem}><Icon name="pool" size={16} /> {busy ? 'Adding…' : 'Add liquidity'}</button></div>
      </form>

      <form class="panel ba-form card" aria-label="Take liquidity out of this pool" onsubmit={(e) => { e.preventDefault(); void takeOut(); }}>
        <h3>Take out</h3>
        {#if !position}
          <p class="ba-empty">This wallet holds no shares of this pool.</p>
        {:else}
          <div class="field">
            <label for="pv-percent">Take out {percent}%</label>
            <input id="pv-percent" type="range" min="1" max="100" step="1" bind:value={percent} />
            <div class="ba-chips">{#each [25, 50, 75, 100] as p (p)}<button type="button" class="ba-chip" aria-pressed={percent === p} onclick={() => (percent = p)}>{p}%</button>{/each}</div>
          </div>
          <div class="field">
            <span class="label">You get</span>
            <div class="ba-chips" role="group" aria-label="You get">
              <button type="button" class="ba-chip" aria-pressed={only === ''} onclick={() => (only = '')}>Both</button>
              <button type="button" class="ba-chip" aria-pressed={only === 'a'} onclick={() => (only = 'a')}><SafeText text={pool.asset_a} /> alone</button>
              <button type="button" class="ba-chip" aria-pressed={only === 'b'} onclick={() => (only = 'b')}><SafeText text={pool.asset_b} /> alone</button>
            </div>
          </div>
          <div class="field">
            <label for="pv-out-slippage">Slippage</label>
            <select id="pv-out-slippage" bind:value={outSlippage}><option value={0.5}>0.5%</option><option value={1}>1%</option><option value={2}>2%</option><option value={5}>5%</option></select>
          </div>
          {#if outQuote}
            <dl class="ba-lines">
              {#if only === ''}
                <dt>You get</dt><dd>{formatUnits(outQuote.a, da)} <SafeText text={pool.asset_a} /></dd>
                <dt></dt><dd>{formatUnits(outQuote.b, db)} <SafeText text={pool.asset_b} /></dd>
              {:else}
                <dt>You get</dt><dd class="strong">{formatUnits(only === 'a' ? outQuote.a : outQuote.b, only === 'a' ? da : db)} <SafeText text={only === 'a' ? pool.asset_a : pool.asset_b} /></dd>
              {/if}
            </dl>
            {#if only !== ''}<p class="ba-help">Your part of it, and what your part of the other buys of it in this pool, its fee paid.</p>{/if}
          {:else if outQuoteError}<p class="ba-problems"><SafeText text={outQuoteError} /></p>{/if}
          <div class="row"><button type="submit" class="btn primary" disabled={busy || wallets.current === null || removing === 0n || !outQuote}>{busy ? 'Taking out…' : 'Take out'}</button></div>
        {/if}
      </form>
    </div>
  {:else if book}
    <p class="ba-help">An order book: limit orders to buy or sell at a price, which trade at the end of each block in one batch. <button type="button" class="btn small" onclick={openBook}><Icon name="book" size={14} /> Its order book</button></p>
  {:else if kind.curve === 'launch'}
    <p class="ba-help">A launch's curve: buy and sell the asset on its page (Launches), until it graduates into its pool with {TICKER}. <button type="button" class="btn small" onclick={() => { tradeIntent.launch = pool.asset_a_id; nav.tab = 'launches'; }}>Its launch</button></p>
  {:else if !kind.default_kind || (ba.dex && ba.dex.curves_height >= 0)}
    <p class="ba-help">Liquidity by a pool's id (one asset alone, pools of other kinds) comes with the pool kinds{#if ba.dex && ba.dex.curves_height >= 0}, from block {formatCount(ba.dex.curves_height)}{/if}.</p>
  {/if}
</div>

<style>
  .pool-view {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .head {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.7rem;
    padding: 1rem 1.1rem;
  }
  .pair {
    display: inline-flex;
    align-items: center;
    gap: 0.4rem;
    min-width: 0;
  }
  .kind-panel {
    padding: 1rem 1.1rem;
  }
  .depth-panel {
    grid-column: 1 / -1;
  }
  .id {
    display: flex;
    align-items: center;
    gap: 0.3rem;
    min-width: 0;
  }
  .card {
    padding: 1.2rem;
  }
  .card h3 {
    margin: 0;
  }
  .small {
    font-size: 0.85rem;
  }
</style>
