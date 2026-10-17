<script lang="ts">
  // An auction: what is left at what price now, the price falling block by block from the start to the end
  // (drawn: the price of one unit at each block, and where the next block is), what it sold and brought in;
  // a bid (quoted by the node: quotebid) or buying all that is left (cost_of_remaining, and never more). The
  // auction panel of the Qt window of BitAssets.
  import SafeText from '../../components/SafeText.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import Icon from '../../components/Icon.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { t, tn } from '../../lib/i18n.svelte';
  import { formatCount, niceTimeOffset, shortHash } from '../../lib/format';
  import { go } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { app, toast, wallets } from '../../lib/state.svelte';
  import { barWidth } from './actions';
  import Chart from './Chart.svelte';
  import UnitsInput from './UnitsInput.svelte';
  import { amountParam, AUCTION_STATUS, auctionPriceAt, auctionProgress, formatAmount, formatPrice, formatUnits, isChn, unitPrice, unitsAtLeast, unitsAtMost, unitsOf, type Auction, type AuctionUnits, type BidQuote } from './model';
  import { ba, decimalsOf, nameOf, refreshBitAssets } from './store.svelte';

  let { auction }: { auction: Auction } = $props();

  const db = $derived(decimalsOf(auction.base_id));
  const dq = $derived(decimalsOf(auction.quote_id));
  const units = $derived<AuctionUnits>({
    baseAmount: unitsOf(auction.amount, db),
    remaining: unitsOf(auction.remaining, db),
    startPrice: unitsOf(auction.start_price, dq),
    endPrice: unitsOf(auction.end_price, dq),
    startHeight: auction.start_height,
    endHeight: auction.end_height,
  });
  const next = $derived(Math.max(ba.height, auction.start_height));
  const progress = $derived(auctionProgress(auction, ba.height));
  const spacing = $derived(app.info?.blockSpacing ?? 600);
  const mine = $derived(!!ba.mine?.receipts.some((r) => r.auction === auction.auction));
  const unit = $derived(unitPrice(auction.unit_price));

  /** The price of one whole unit at each block, from the start to the end (60 points at most). */
  const curve = $derived.by(() => {
    const span = auction.end_height - auction.start_height;
    const n = Math.min(60, Math.max(1, span));
    const whole = Number(units.baseAmount) / 10 ** db;
    if (!(whole > 0)) return [];
    return Array.from({ length: n + 1 }, (_, i) => {
      const h = auction.start_height + Math.round((span * i) / n);
      return { x: h, y: Number(auctionPriceAt(units, h)) / 10 ** dq / whole };
    });
  });
  const nowPoint = $derived.by(() => {
    const whole = Number(units.baseAmount) / 10 ** db;
    return whole > 0 ? [{ x: next, y: Number(auctionPriceAt(units, next)) / 10 ** dq / whole }] : [];
  });

  // A bid: quoted by the node as it is typed.
  let bidText = $state('');
  let bid = $state<bigint | null>(null);
  let quote = $state<BidQuote | null>(null);
  let quoteError = $state('');
  let busy = $state(false);
  const heldQuote = $derived.by(() => {
    if (isChn(auction.quote_id)) return wallets.current !== null ? (wallets.balances[wallets.current]?.trusted ?? null) : null;
    const h = ba.mine?.assets.find((a) => a.asset === auction.quote_id);
    return h ? unitsAtMost(h.balance, h.decimals) : 0n;
  });
  let generation = 0;
  $effect(() => {
    const mineGen = ++generation;
    const amount = bid;
    void ba.height;
    quote = null;
    quoteError = '';
    if (amount === null) return;
    const timer = setTimeout(() => {
      rpc
        .raw<BidQuote>('quotebid', [auction.auction, amountParam(amount, dq)])
        .then((q) => {
          if (mineGen === generation) quote = q;
        })
        .catch((e) => {
          if (mineGen === generation) quoteError = errorText(e);
        });
    }, 250);
    return () => clearTimeout(timer);
  });

  async function placeBid() {
    const wallet = wallets.current;
    if (wallet === null || bid === null || !quote || busy) return;
    const r = await ask({
      title: t('Bid'),
      message: t('Pay %1 %2 for at least %3 %4?', formatUnits(bid, dq), nameOf(auction.quote_id, auction.quote), formatAmount(quote.buys, db), nameOf(auction.base_id, auction.base)),
      informative: t('At the price of the block that takes the bid: at least what it buys in the next block, more if it waits (the price only falls).'),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Bid', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      const done = await walletRpc<{ txid: string; pays: number; buys: number }>(wallet, 'bidauction', [auction.auction, amountParam(bid, dq)]);
      if (!done) return;
      toast('success', t('Bidding'), t('%1 %2 for at least %3 %4, with the next block.', formatAmount(done.pays, dq), auction.quote, formatAmount(done.buys, db), auction.base));
      bidText = '';
      void refreshBitAssets();
    } catch (e) {
      await inform(t('Bid'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }

  async function buyAll() {
    const wallet = wallets.current;
    if (wallet === null || busy) return;
    // What is left now, and what all of it costs in the next block: not the price of all the auction sold.
    let now: Auction;
    try {
      now = await rpc.raw<Auction>('getauction', [auction.auction]);
    } catch (e) {
      await inform(t('Buy all'), errorText(e), 'error');
      return;
    }
    const r = await ask({
      title: t('Buy all'),
      message: t('Pay %1 %2 for the %3 %4 left?', formatAmount(now.cost_of_remaining, dq), nameOf(now.quote_id, now.quote), formatAmount(now.remaining, db), nameOf(now.base_id, now.base)),
      informative: t('The wallet pays exactly that, never more: less, should the price fall first, and the rest comes back.'),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Buy all', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      // The cost as a cap, rounded up where the JSON number is not exact (audit 7 E-BA6): the wallet pays what
      // all that is left costs, never more, and refuses only a cost above the cap.
      const done = await walletRpc<{ txid: string; pays: number; buys: number }>(wallet, 'bidauction', [auction.auction, amountParam(unitsAtLeast(now.cost_of_remaining, dq), dq), true]);
      if (!done) return;
      toast('success', t('Bidding'), t('%1 %2 for the %3 %4 left, with the next block.', formatAmount(done.pays, dq), now.quote, formatAmount(done.buys, db), now.base));
      void refreshBitAssets();
    } catch (e) {
      await inform(t('Buy all'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }
</script>

<div class="auction-view">
  <header class="panel head">
    <div class="who">
      <h2>{formatAmount(auction.remaining, db)} <SafeText text={auction.base} /> <span class="faint">for</span> <SafeText text={auction.quote} /></h2>
      <div class="tags">
        <span class="ba-tag" class:accent={auction.status === 'open'}>{t(AUCTION_STATUS[auction.status] ?? 'Ended')}</span>
        {#if mine}<span class="ba-tag accent">Yours</span>{/if}
        <span class="faint small ba-mono"><span translate="no">{shortHash(auction.auction, 8)}</span> <CopyButton text={auction.auction} label="Copy the auction" /></span>
      </div>
    </div>
    <div class="price">
      <span class="muted small">Price of one, next block</span>
      <strong class="num">{#if unit.kind === 'each'}{unit.price} <SafeText text={auction.quote} />{:else if unit.kind === 'buys'}1 <SafeText text={auction.quote} /> buys {unit.amount} <SafeText text={auction.base} />{:else}–{/if}</strong>
    </div>
  </header>

  <section class="panel">
    <div class="panel-head"><h3>The price falls block by block</h3><span class="muted small">from block {formatCount(auction.start_height)} to {formatCount(auction.end_height)}</span></div>
    <div class="panel-body">
      <Chart points={curve} marks={nowPoint} nowX={ba.height >= auction.start_height && ba.height <= auction.end_height ? next : null} label={t('The price of one unit, block by block')} formatX={(x) => t('block %1', String(Math.round(x)))} />
      <div class="bars">
        <span class="muted small">Sold {Math.round(progress.sold * 100)}%</span>
        <div class="ba-bar"><span use:barWidth={progress.sold}></span></div>
        <span class="muted small">Time {Math.round(progress.time * 100)}%</span>
        <div class="ba-bar"><span class="time" use:barWidth={progress.time}></span></div>
      </div>
      <dl class="ba-lines">
        <dt>All that is left, next block</dt><dd class="strong">{formatAmount(auction.cost_of_remaining, dq)} <SafeText text={auction.quote} /></dd>
        <dt>Price of all of it</dt><dd>{formatAmount(auction.start_price, dq)} → {formatAmount(auction.end_price, dq)} <SafeText text={auction.quote} /></dd>
        <dt>Left</dt><dd>{formatAmount(auction.remaining, db)} of {formatAmount(auction.amount, db)}</dd>
        <dt>Brought in</dt><dd>{formatAmount(auction.proceeds, dq)} <SafeText text={auction.quote} />, {tn('%n bid(s)', auction.bids)}</dd>
        {#if auction.status === 'upcoming'}<dt>Starts</dt><dd>{tn('in %n block(s)', auction.start_height - ba.height)} (≈ {niceTimeOffset((auction.start_height - ba.height) * spacing)})</dd>{/if}
        {#if auction.status === 'open'}<dt>Ends</dt><dd>{tn('in %n block(s)', auction.end_height - ba.height + 1)} (≈ {niceTimeOffset((auction.end_height - ba.height + 1) * spacing)})</dd>{/if}
      </dl>
    </div>
  </section>

  {#if auction.status === 'open'}
    <form class="panel ba-form bid" onsubmit={(e) => { e.preventDefault(); void placeBid(); }}>
      <h3>Bid</h3>
      <UnitsInput id="bid-amount" label="What you pay" decimals={dq} bind:units={bid} bind:text={bidText} max={heldQuote} unit={auction.quote} />
      {#if quoteError}<p class="ba-problems bad" role="status"><SafeText text={quoteError} /></p>
      {:else if quote}<p class="ba-help">Buys <strong>{formatAmount(quote.buys, db)}</strong> <SafeText text={auction.base} /> in the next block, more if it waits.</p>{/if}
      <div class="row">
        <button type="submit" class="btn primary" disabled={busy || wallets.current === null || bid === null || !quote}>Bid</button>
        <button type="button" class="btn" disabled={busy || wallets.current === null} onclick={buyAll}>Buy all that is left</button>
      </div>
    </form>
  {:else if auction.status === 'upcoming'}
    <p class="ba-help">It takes bids from block {formatCount(auction.start_height)}.</p>
  {:else if mine}
    <div class="row"><button type="button" class="btn primary" onclick={() => go('my-assets', 'auctions')}><Icon name="holdings" size={15} /> Collect it in My assets</button></div>
  {/if}
</div>

<style>
  .auction-view {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    min-width: 0;
  }
  .head {
    display: flex;
    flex-wrap: wrap;
    justify-content: space-between;
    align-items: center;
    gap: 1rem;
    padding: 1rem 1.1rem;
  }
  .who h2 {
    margin: 0 0 0.35rem;
    font-size: 1.3rem;
    overflow-wrap: anywhere;
  }
  .tags {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.4rem;
  }
  .price {
    display: flex;
    flex-direction: column;
    align-items: flex-end;
    gap: 0.15rem;
  }
  .price strong {
    font-size: 1.35rem;
  }
  .bars {
    display: grid;
    grid-template-columns: auto minmax(0, 1fr);
    align-items: center;
    gap: 0.4rem 0.75rem;
    margin: 0.75rem 0;
  }
  .bid {
    padding: 1.1rem;
  }
  .bid h3 {
    margin: 0;
  }
  .small {
    font-size: 0.85rem;
  }
</style>
