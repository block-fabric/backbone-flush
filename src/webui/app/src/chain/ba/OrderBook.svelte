<script lang="ts">
  // An order book (Layers' DEX: ba/book.ts): two assets picked, the base and what it is priced in; its ladder (getbook: the
  // asks above, the bids below, the spread between them, each level's depth as a bar), again with each block; a limit
  // order placed on either side (placeorder: buy or sell the base at a price at most or at least, for some blocks), checked
  // first as the chain checks it (no dust: 0.01 CHN on the CHN side of a pair with CHN, a whole unit of what it sells
  // otherwise), its preview; and the orders of the wallet (MyOrders.svelte).
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { t, tn } from '../../lib/i18n.svelte';
  import { formatCount } from '../../lib/format';
  import { rpc } from '../../lib/rpc';
  import { toast, wallets } from '../../lib/state.svelte';
  import { barWidth } from './actions';
  import AssetBadge from './AssetBadge.svelte';
  import AssetSelect from './AssetSelect.svelte';
  import MyOrders from './MyOrders.svelte';
  import BookCandles from './BookCandles.svelte';
  import BookDepth from './BookDepth.svelte';
  import { orderMarks } from './bookchart';
  import UnitsInput from './UnitsInput.svelte';
  import VerifiedBadge from './VerifiedBadge.svelte';
  import { buildLadder, DEFAULT_ORDER_BLOCKS, MAX_ORDER_DURATION, ORDER_DURATIONS, placeParams, planOrder, validDuration, type Book, type BookOrder, type WalletOrder } from './book';
  import { isBook } from './markets';
  import { assetArg, CHN_ID, formatPrice, formatUnits, isChn, unitsAtMost } from './model';
  import { assetById, ba, decimalsOf, labelOf, nameOf, refreshBitAssets, tradeIntent } from './store.svelte';
  import { hiddenAsset } from './visible.svelte';
  import { TICKER } from '../config';

  let base = $state('');
  let quote = $state(CHN_ID);
  let book = $state<Book | null>(null);
  let bookError = $state('');

  // Every asset can have a book: those the store read, without those the wallet hides (unless chosen).
  const choices = $derived(ba.assets.filter((a) => a.asset === base || a.asset === quote || !hiddenAsset(a.asset, a.label)).map((a) => ({ id: a.asset, label: a.label, seq: a.seq })));

  // A book asked for by another page (the market index, a pool's page): shown.
  $effect(() => {
    if (!tradeIntent.bookBase && !tradeIntent.bookQuote) return;
    if (tradeIntent.bookBase) base = tradeIntent.bookBase;
    if (tradeIntent.bookQuote) quote = tradeIntent.bookQuote;
    tradeIntent.bookBase = '';
    tradeIntent.bookQuote = '';
  });
  // A base by default: the first asset of a book, else of a pool, else the first asset.
  $effect(() => {
    if (base && base !== quote) return;
    const books = ba.pools.filter((p) => isBook(p));
    const first = [...books, ...ba.pools].map((p) => (isChn(p.asset_a_id) ? p.asset_b_id : p.asset_a_id)).find((id) => id !== quote);
    base = first ?? choices.find((c) => c.id !== quote)?.id ?? '';
  });

  const pair = $derived(!!base && !!quote && base !== quote);
  const dBase = $derived(decimalsOf(base));
  const dQuote = $derived(decimalsOf(quote));
  const baseLabel = $derived(labelOf(base));
  const quoteLabel = $derived(labelOf(quote));

  // The book, again with each block.
  $effect(() => {
    const a = base;
    const b = quote;
    void ba.version;
    if (!a || !b || a === b) {
      book = null;
      return;
    }
    let live = true;
    rpc
      .raw<Book>('getbook', [assetArg(a), assetArg(b), 50])
      .then((r) => {
        if (!live) return;
        book = r && typeof r === 'object' ? r : null;
        bookError = '';
      })
      .catch((e) => {
        if (!live) return;
        book = null;
        bookError = errorText(e);
      });
    return () => {
      live = false;
    };
  });
  const ladder = $derived(buildLadder(book, 12));

  // The wallet's open orders in this book, read whole (getorder: their limits), for the marks of the charts.
  let mineOrders = $state.raw<BookOrder[]>([]);
  $effect(() => {
    const wallet = wallets.current;
    const id = book?.book;
    void ba.version;
    if (wallet === null || !id) {
      mineOrders = [];
      return;
    }
    let live = true;
    rpc
      .raw<WalletOrder[]>('listorders', [], { wallet })
      .then(async (list) => {
        const here = (Array.isArray(list) ? list : []).filter((o) => o.book === id && !o.expired && !o.evicted).slice(0, 20);
        const whole = await Promise.all(here.map((o) => rpc.raw<BookOrder>('getorder', [o.order]).catch(() => null)));
        if (live) mineOrders = whole.filter((o): o is BookOrder => !!o && typeof o === 'object');
      })
      .catch(() => {
        if (live) mineOrders = [];
      });
    return () => {
      live = false;
    };
  });
  const marks = $derived(orderMarks(mineOrders, base, quote));
  const fmt = (x: number, decimals: number) => x.toLocaleString('en-US', { maximumFractionDigits: Math.min(Math.max(decimals, 0), 6) });

  // --- A limit order ---
  let side = $state<'buy' | 'sell'>('buy');
  let amount = $state<bigint | null>(null);
  let amountText = $state('');
  let priceText = $state('');
  let blocks = $state(DEFAULT_ORDER_BLOCKS);
  let busy = $state(false);

  const held = (id: string, decimals: number): bigint | null => {
    if (!id) return null;
    if (isChn(id)) return wallets.current !== null ? (wallets.balances[wallets.current]?.trusted ?? null) : null;
    const h = ba.mine?.assets.find((x) => x.asset === id);
    return h ? unitsAtMost(h.balance, decimals) : 0n;
  };
  const plan = $derived(pair && amount !== null && priceText.trim() !== '' ? planOrder(side, { id: base, decimals: dBase }, { id: quote, decimals: dQuote }, amount, priceText, side === 'sell' ? held(base, dBase) : held(quote, dQuote)) : null);
  const durationOk = $derived(validDuration(blocks));

  /** A level of the ladder picked: its price in the form, the side that would take it. */
  function pickLevel(price: number, levelSide: 'ask' | 'bid') {
    priceText = String(Number(price.toPrecision(12)));
    side = levelSide === 'ask' ? 'buy' : 'sell';
  }

  async function place() {
    const wallet = wallets.current;
    const p = plan;
    if (wallet === null || busy || !p || !p.ok || !durationOk) return;
    const sBlocks = blocks;
    const sold = nameOf(p.sell.id);
    const bought = nameOf(p.buy.id);
    const r = await ask({
      title: t('Place a limit order'),
      message: t('Sell %1 %2 for %3, at %4 %3 for each at least?', formatUnits(p.sellUnits, p.sell.decimals), sold, bought, p.price),
      informative: t('At least %1 %2 at its limit. It trades in the batches of the next %3 blocks, wholly or in part; collect what it got, or cancel it, in My orders.', formatUnits(p.buysAtLeast, p.buy.decimals), bought, formatCount(sBlocks)),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Place', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      const done = await walletRpc<{ txid: string; order: string; book: string }>(wallet, 'placeorder', placeParams(p, sBlocks, assetArg));
      if (!done) return;
      toast('success', t('Placing an order'), t('In the book with the next block.'));
      amountText = '';
      void refreshBitAssets({ node: false });
    } catch (e) {
      await inform(t('Place a limit order'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }
  function flip() {
    const b = base;
    base = quote;
    quote = b;
    priceText = '';
  }
</script>

<section class="panel picks">
  <div class="pickers">
    <AssetSelect id="book-base" label="Asset" bind:value={base} {choices} />
    <button type="button" class="btn ghost small flip" aria-label="Swap the two assets" title="Swap the two assets" onclick={flip}><Icon name="swap" size={16} /></button>
    <AssetSelect id="book-quote" label="Priced in" bind:value={quote} {choices} />
  </div>
  {#if pair}
    <div class="book-head" data-testid="book-head">
      <span class="pair"><AssetBadge id={base} label={baseLabel} /><VerifiedBadge id={base} /> <span class="faint">/</span> <AssetBadge id={quote} label={quoteLabel} /><VerifiedBadge id={quote} /></span>
      <span class="ba-kind book">Order book</span>
      <span class="grow"></span>
      {#if book}<span class="muted small">{tn('%n open order(s)', Number(book.open_orders) || 0)}</span>{/if}
    </div>
  {/if}
</section>

{#if !pair}
  <section class="panel ba-empty">Pick two different assets: the asset of the book, and what its prices are in.</section>
{:else}
  <div class="ba-split">
    <section class="panel">
      <div class="panel-head"><h3>Order book</h3><span class="muted small">{t('Prices in %1 for 1 %2', quoteLabel, baseLabel)}</span></div>
      <div class="panel-body">
        {#if bookError}
          <p class="ba-problems"><SafeText text={bookError} /></p>
        {:else if !book}
          <p class="ba-empty">Reading the book…</p>
        {:else if ladder.asks.length === 0 && ladder.bids.length === 0}
          <p class="ba-empty">No order in this book yet: place the first.</p>
        {:else}
          <div class="ladder" role="group" aria-label="Order book">
            <div class="lvl head"><span>Price</span><span class="end">Amount</span><span class="end">Total</span></div>
            {#each ladder.asks as r (`a${r.price}`)}
              <button type="button" class="lvl ask" title={t('Buy at this price')} onclick={() => pickLevel(r.price, 'ask')}>
                <span class="bar" use:barWidth={r.depth} aria-hidden="true"></span>
                <span class="price" translate="no">{formatPrice(r.price, 8)}</span>
                <span class="end" translate="no">{fmt(r.amount, dBase)}</span>
                <span class="end" translate="no">{fmt(r.total, dBase)}</span>
              </button>
            {/each}
            <div class="lvl spread" data-testid="book-spread">
              <span>
                {#if ladder.spread === null}
                  {ladder.asks.length ? t('No bid') : t('No ask')}
                {:else if ladder.spread < 0}
                  <span class="ba-warn">Crossed: they trade in the next block's batch</span>
                {:else}
                  {t('Spread %1', formatPrice(ladder.spread, 6))}{#if ladder.spreadPercent !== null} <span class="muted">({ladder.spreadPercent.toFixed(2)}%)</span>{/if}
                {/if}
              </span>
              <span class="end muted">{#if ladder.mid !== null}{t('mid %1', formatPrice(ladder.mid, 8))}{/if}</span>
            </div>
            {#each ladder.bids as r (`b${r.price}`)}
              <button type="button" class="lvl bid" title={t('Sell at this price')} onclick={() => pickLevel(r.price, 'bid')}>
                <span class="bar" use:barWidth={r.depth} aria-hidden="true"></span>
                <span class="price" translate="no">{formatPrice(r.price, 8)}</span>
                <span class="end" translate="no">{fmt(r.amount, dBase)}</span>
                <span class="end" translate="no">{fmt(r.total, dBase)}</span>
              </button>
            {/each}
          </div>
          <p class="ba-help">Amounts in <SafeText text={baseLabel} />. Pick a level to use its price. The orders trade at the end of each block, in one batch, at one price per round: never worse than their limits.</p>
        {/if}
      </div>
    </section>

    <form class="panel ba-form card" aria-label="Place a limit order" onsubmit={(e) => { e.preventDefault(); void place(); }}>
      <h3>Limit order</h3>
      <div class="ba-chips" role="group" aria-label="Side">
        <button type="button" class="ba-chip" aria-pressed={side === 'buy'} onclick={() => (side = 'buy')}>Buy <SafeText text={baseLabel} /></button>
        <button type="button" class="ba-chip" aria-pressed={side === 'sell'} onclick={() => (side = 'sell')}>Sell <SafeText text={baseLabel} /></button>
      </div>
      <UnitsInput id="order-amount" label={side === 'buy' ? t('Amount to buy') : t('Amount to sell')} decimals={dBase} bind:units={amount} bind:text={amountText} max={side === 'sell' ? held(base, dBase) : null} unit={baseLabel} />
      <div class="field">
        <label for="order-price">{side === 'buy' ? t('Price, at most') : t('Price, at least')}</label>
        <div class="ba-inline">
          <input id="order-price" type="text" inputmode="decimal" autocomplete="off" spellcheck="false" placeholder="0" bind:value={priceText} />
          <span class="unit muted small" translate="no">{t('%1 for 1 %2', quoteLabel, baseLabel)}</span>
        </div>
        {#if ladder.bestAsk !== null || ladder.bestBid !== null}
          <div class="ba-chips" role="group" aria-label="Prices of the book">
            {#if ladder.bestBid !== null}<button type="button" class="ba-chip" onclick={() => (priceText = String(Number(ladder.bestBid!.toPrecision(12))))}>Best bid {formatPrice(ladder.bestBid, 8)}</button>{/if}
            {#if ladder.bestAsk !== null}<button type="button" class="ba-chip" onclick={() => (priceText = String(Number(ladder.bestAsk!.toPrecision(12))))}>Best ask {formatPrice(ladder.bestAsk, 8)}</button>{/if}
          </div>
        {/if}
      </div>
      <div class="field">
        <label for="order-blocks">For how long</label>
        <div class="ba-chips" role="group" aria-label="Duration presets">
          {#each ORDER_DURATIONS as d (d.blocks)}<button type="button" class="ba-chip" aria-pressed={blocks === d.blocks} onclick={() => (blocks = d.blocks)}>{t(d.label)}</button>{/each}
        </div>
        <div class="ba-inline">
          <input id="order-blocks" type="number" min="1" max={MAX_ORDER_DURATION} step="1" bind:value={blocks} />
          <span class="unit muted small">blocks{#if durationOk && ba.height >= 0} · {t('until block %1', formatCount(ba.height + blocks))}{/if}</span>
        </div>
        {#if !durationOk}<span class="error">{t('From 1 to %1 blocks (about a year).', formatCount(MAX_ORDER_DURATION))}</span>{/if}
      </div>
      {#if plan && !plan.ok}
        <p class="ba-problems" role="status" data-testid="order-problem">{t(plan.error)}</p>
      {:else if plan && plan.ok}
        <dl class="ba-lines" data-testid="order-preview">
          <dt>It sells</dt><dd>{formatUnits(plan.sellUnits, plan.sell.decimals)} <SafeText text={labelOf(plan.sell.id)} /></dd>
          <dt>It buys at its limit</dt><dd class="strong">{t('at least %1', formatUnits(plan.buysAtLeast, plan.buy.decimals))} <SafeText text={labelOf(plan.buy.id)} /></dd>
          <dt>Its limit</dt><dd translate="no">{plan.price} <SafeText text={labelOf(plan.buy.id)} /> / <SafeText text={labelOf(plan.sell.id)} /></dd>
        </dl>
      {/if}
      <p class="ba-help">The least an order is worth: 0.01 {TICKER} on its {TICKER} side for a pair with {TICKER}; a whole unit of what it sells otherwise. It trades in each block's batch, wholly or in part; what it got waits in My orders.</p>
      <div class="row"><button type="submit" class="btn primary" disabled={busy || wallets.current === null || !plan || !plan.ok || !durationOk}><Icon name="book" size={16} /> {busy ? 'Placing…' : 'Place order'}</button></div>
    </form>
  </div>
{/if}

{#if pair && book}
  <div class="ba-split charts">
    <section class="panel">
      <div class="panel-head"><h3>Depth</h3><span class="muted small">{t('Cumulative %1 around the middle price', baseLabel)}</span></div>
      <div class="panel-body"><BookDepth {book} {marks} {baseLabel} {quoteLabel} /></div>
    </section>
    <section class="panel">
      <div class="panel-head"><h3>Price</h3><span class="muted small">{t('%1 for 1 %2, from its trades', quoteLabel, baseLabel)}</span></div>
      <div class="panel-body"><BookCandles pool={book.book} {marks} {quoteLabel} /></div>
    </section>
  </div>
  {#if marks.length}<p class="ba-help">Dashed lines: your open orders in this book, at their limits.</p>{/if}
{/if}

<MyOrders />

<style>
  .picks {
    display: flex;
    flex-direction: column;
    gap: 0.8rem;
    padding: 1rem 1.1rem;
  }
  .pickers {
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto minmax(0, 1fr);
    align-items: end;
    gap: 0.6rem;
  }
  .flip {
    margin-bottom: 0.3rem;
  }
  .book-head {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.6rem;
  }
  .pair {
    display: inline-flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.4rem;
    min-width: 0;
  }
  .ladder {
    display: flex;
    flex-direction: column;
    font-size: 0.9rem;
  }
  .lvl {
    position: relative;
    display: grid;
    grid-template-columns: minmax(0, 1fr) minmax(0, 1fr) minmax(0, 1fr);
    gap: 0.6rem;
    align-items: center;
    width: 100%;
    padding: 0.28rem 0.6rem;
    border: 0;
    background: none;
    color: var(--text);
    font: inherit;
    text-align: left;
    font-variant-numeric: tabular-nums;
  }
  button.lvl {
    cursor: pointer;
  }
  button.lvl:hover {
    background: var(--raised);
  }
  .lvl > span:not(.bar) {
    position: relative;
  }
  .lvl.head {
    color: var(--text-muted);
    font-size: 0.8rem;
    font-weight: 600;
    border-bottom: 1px solid var(--line);
  }
  .lvl .end {
    text-align: right;
  }
  .lvl .bar {
    position: absolute;
    inset: 1px 0 1px auto;
    border-radius: var(--radius-s);
    opacity: 0.22;
  }
  .lvl.ask .bar {
    background: var(--negative);
  }
  .lvl.bid .bar {
    background: var(--positive);
  }
  .lvl.ask .price {
    color: var(--negative);
    font-weight: 600;
  }
  .lvl.bid .price {
    color: var(--positive);
    font-weight: 600;
  }
  .lvl.spread {
    grid-template-columns: minmax(0, 2fr) minmax(0, 1fr);
    margin: 0.25rem 0;
    padding: 0.45rem 0.6rem;
    border-top: 1px solid var(--line);
    border-bottom: 1px solid var(--line);
    font-size: 0.85rem;
  }
  .card {
    padding: 1.2rem;
  }
  .card h3 {
    margin: 0;
  }
  .unit {
    align-self: center;
    white-space: nowrap;
  }
  .error {
    color: var(--negative);
    font-size: 0.85rem;
  }
  .small {
    font-size: 0.85rem;
  }
</style>
