<script lang="ts">
  // A launched token, whole (getlaunch): its progress to graduation (CHN raised of what it graduates at), its price, its
  // market value and volume; its bonding curve (the constant product of virtual reserves from the rules: price by tokens
  // sold, from the start to the end, and where it is now); buying and selling on the curve, quoted by the node
  // (quotelaunch: what comes out, what comes back when a buy passes the curve's end, the fee, the price, whether it
  // graduates), sent as an order of the block's batch (buyfromlaunch, selltolaunch); the last blocks that traded on it
  // (getlaunchlog); its creator, their fees and allocation, and the payment of what they are owed (claimlaunch, which
  // anyone may send). Graduated: it trades in its pool with CHN (PoolView, Swap).
  import { untrack } from 'svelte';
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import { stacked } from '../../lib/actions';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { formatCount } from '../../lib/format';
  import { nav } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { toast, wallets } from '../../lib/state.svelte';
  import { barWidth } from './actions';
  import AssetBadge from './AssetBadge.svelte';
  import UnlockedSupplyBadge from './UnlockedSupplyBadge.svelte';
  import Chart from './Chart.svelte';
  import UnitsInput from './UnitsInput.svelte';
  import { compactNumber } from './axis';
  import { launchFeeParts, launchFeeSplit } from './fees';
  import { claimable, creatorAllocation, curvePointAt, feeDestinationText, shareKind, shareRecipientText, shareText, poolKindText, curveSeries, curveShape, launchDecimals, launchSupply, rulesOf, type Launch, type LaunchLogRow, type LaunchQuote, type LaunchStats } from './launch';
  import { amountParam, approx, assetArg, CHN_ID, formatAmount, formatPercent, formatPrice, formatUnits, lessSlippage, minChnPayout, minChnText, unitsAtMost, unitsOf, type Amount, type Asset } from './model';
  import { ba, nameOf, noteKnownAssets, noteLaunches, refreshBitAssets, tradeIntent } from './store.svelte';
  import { TICKER } from '../config';

  /** side: the panel's side the route names (Markets "launches/<token>/sell": ba/assetactions.ts), focused once shown. */
  let { token, back, side: routeSide = 'buy' }: { token: string; back: () => void; side?: 'buy' | 'sell' } = $props();

  let launch = $state<Launch | null>(null);
  let error = $state('');
  let stats = $state<LaunchStats | null>(null);
  let log = $state<LaunchLogRow[]>([]);
  let mine = $state(false);

  // The launch and the rules, again with each block; the asset once (its logo, its number).
  $effect(() => {
    const id = token;
    void ba.version;
    let live = true;
    rpc
      .raw<Launch>('getlaunch', [assetArg(id)])
      .then((l) => {
        if (!live) return;
        launch = l && typeof l === 'object' ? l : null;
        if (launch) noteLaunches([launch]);
        error = '';
      })
      .catch((e) => {
        if (live) error = errorText(e);
      });
    rpc
      .raw<LaunchStats>('getlaunchstats', [])
      .then((s) => {
        if (live && s && typeof s === 'object') stats = s;
      })
      .catch(() => undefined);
    return () => {
      live = false;
    };
  });
  $effect(() => {
    const id = token;
    rpc
      .raw<Asset>('getasset', [assetArg(id)])
      .then((a) => {
        if (a && typeof a === 'object' && a.asset) noteKnownAssets([a]);
      })
      .catch(() => undefined);
  });
  // The last blocks that traded on it (getlaunchlog keeps 10,080 blocks; read the last 300 at most).
  $effect(() => {
    const id = token;
    const made = launch?.height ?? -1;
    const tip = ba.height - 1;
    if (made < 0 || tip < made) return;
    const from = Math.max(made, tip - 299);
    let live = true;
    rpc
      .raw<LaunchLogRow[]>('getlaunchlog', [from, tip])
      .then((rows) => {
        if (live) log = (rows ?? []).filter((r) => r.token === id).sort((a, b) => b.height - a.height);
      })
      .catch(() => undefined);
    return () => {
      live = false;
    };
  });
  // Whether the creator's address is this wallet's.
  $effect(() => {
    const address = launch?.creator ?? '';
    const wallet = wallets.current;
    mine = false;
    if (!address || wallet === null) return;
    rpc
      .raw<{ ismine?: boolean }>('getaddressinfo', [address], { wallet })
      .then((info) => {
        if (launch?.creator === address) mine = info?.ismine === true;
      })
      .catch(() => undefined);
  });

  const rules = $derived(rulesOf(stats?.rules));
  const graduated = $derived(launch?.status === 'graduated');
  const shape = $derived(curveShape(launch ? approx(launch.graduation) : rules.graduation, launchSupply(launch, rules.supply)));
  /** Its asset's decimals (6 before the launch parameters). */
  const DEC = $derived(launchDecimals(launch));
  const raised = $derived(launch ? approx(launch.raised) : 0);
  const now = $derived(curvePointAt(shape, graduated ? shape.graduation : raised));
  const series = $derived(curveSeries(shape));
  const split = $derived(launch ? launchFeeSplit(launchFeeParts(launch)) : '');
  const owed = $derived(launch ? claimable(launch) : { fees: false, allocation: false, tokens: false });
  /** v5: the launch's fee flow, as its page says it (launch.ts feeDestinationText, poolKindText). */
  const graduationFee = $derived(Number(stats?.rules?.graduation_fee ?? 1));
  /** v7: its fee split, as the node says it (getlaunch fee_split: each share with what it was paid and is owed). */
  const feeSplit = $derived(launch && Array.isArray(launch.fee_split) ? launch.fee_split : []);
  // Shown to every buyer, above the curve: what the creator gets minted at graduation.
  const allocationShown = $derived(launch ? creatorAllocation(launch) : null);

  // --- Buying and selling on the curve ---
  let side = $state<'buy' | 'sell'>(untrack(() => routeSide));
  // The route's side, again as it changes (back, forward); its amount focused once the launch is read.
  $effect(() => {
    side = routeSide;
  });
  let focused = false;
  $effect(() => {
    if (!launch || focused) return;
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
    if (a === null || graduated) {
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
    if (!launch || amount === null) return '';
    if (side === 'buy' && amount < minChnPayout()) return `A buy pays ${minChnText()} ${TICKER} at least.`;
    if (side === 'buy' && approx(launch.min_buy) > 0 && amount < unitsOf(launch.min_buy, 8)) return 'Less than the least a buy of this launch pays: it would be refunded.';
    if (side === 'buy' && approx(launch.max_buy) > 0 && amount > unitsOf(launch.max_buy, 8)) return 'More than the most a buy of this launch pays: it would be refunded.';
    if (side === 'sell' && out !== null && out < minChnPayout()) return `Too little: the least ${TICKER} paid out is ${minChnText()} ${TICKER}.`;
    if (out === 0n) return 'Too little: it buys nothing.';
    return '';
  });

  async function trade() {
    const wallet = wallets.current;
    if (wallet === null || !launch || busy || amount === null || out === null || least === null || problem || quoteError) return;
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
    const label = nameOf(token, launch.ticker);
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
      const done = await walletRpc<{ txid: string; quote: Amount; min_out: Amount }>(wallet, sSide === 'buy' ? 'buyfromlaunch' : 'selltolaunch', [assetArg(token), amountParam(sAmount, sIn), sSlip, amountParam(sLeast, sOutD)]);
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

  async function claim() {
    const wallet = wallets.current;
    if (wallet === null || !launch || busy) return;
    const r = await ask({
      title: t('Pay a launch\'s creator'),
      message: t('Send the creator of %1 what it is owed?', nameOf(token, launch.ticker)),
      informative: t('Its fees (%1 %3)%2. Anyone may send this: nothing goes anywhere but to the creator.', formatAmount(launch.creator_fees_owed, 8), `${owed.allocation ? t(', and its allocation of %1% of the supply, minted now', String(launch.allocation)) : ''}${owed.tokens ? t(', and %1 tokens owed', formatAmount(launch.creator_tokens_owed ?? 0, DEC, { max: 2 })) : ''}`, TICKER),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Pay', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      const done = await walletRpc<{ txid: string; fees: Amount; allocation: Amount }>(wallet, 'claimlaunch', [assetArg(token)]);
      if (!done) return;
      toast('success', t('Paying the creator'), t('%1 %2, with the next block.', formatAmount(done.fees, 8), TICKER));
      void refreshBitAssets();
    } catch (e) {
      await inform(t('Pay a launch\'s creator'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }

  function openPool() {
    if (!launch) return;
    tradeIntent.pool = launch.pool;
    nav.tab = 'pools';
  }
  function swap() {
    tradeIntent.pay = CHN_ID;
    tradeIntent.get = token;
    nav.tab = 'swap';
  }
</script>

<div class="launch-view">
  <button type="button" class="btn ghost small ba-back" onclick={back}><Icon name="back" size={15} /> All launches</button>
  {#if error && !launch}
    <p class="ba-problems bad" role="alert"><SafeText text={error} /></p>
  {:else if !launch}
    <p class="ba-empty">Reading the launch…</p>
  {:else}
    <header class="panel head">
      <AssetBadge id={token} label={launch.ticker} display={launch.display_name ?? ''} size="l" />
      {#if graduated}<span class="ba-tag accent">Graduated</span>{:else}<span class="ba-tag">On its curve</span>{/if}
      <UnlockedSupplyBadge mintable={launch.mintable === true} />
      {#if launch.allocation > 0}<span class="ba-tag warn" title={t('Minted to the creator at graduation, on top of the supply')}>Creator allocation {launch.allocation}%</span>{/if}
      <span class="grow"></span>
      {#if graduated}
        <button type="button" class="btn primary" onclick={swap}><Icon name="swap" size={15} /> Trade</button>
        <button type="button" class="btn" onclick={openPool}><Icon name="pool" size={15} /> Its pool</button>
      {/if}
    </header>
    {#if allocationShown}
      <p class="ba-problems alloc" role="note" data-testid="launch-allocation"><strong>{t('Creator allocation: %1% minted to the creator at graduation', String(launch.allocation))}</strong> ({formatUnits(allocationShown.units, allocationShown.decimals, { max: 2 })} {launch.ticker}, {t('on top of the supply: holders are diluted by it')}){#if launch.allocation_claimed}. {t('Minted.')}{/if}</p>
    {/if}

    {#if graduated}
      <section class="panel graduated" data-testid="launch-graduated">
        <p><strong>Graduated — trades in its pool.</strong> At block {formatCount(launch.graduated_height ?? 0)} its curve raised {formatAmount(launch.raised, 8, { max: 4 })} {TICKER}; that and what was left of it went into its pool with {TICKER} ({#if launch.pool_kind}{poolKindText(launch, graduationFee)}{:else}constant product, 0.3%{/if}), whose shares nobody holds: the liquidity is locked for good. Anyone may add liquidity on top.</p>
      </section>
    {/if}

    <section class="ba-figures">
      <div class="panel ba-figure">
        <span class="muted">To graduation</span><strong>{formatPercent(Number(launch.progress))}</strong>
        <span class="ba-bar"><span use:barWidth={Number(launch.progress) / 100}></span></span>
        <span class="faint">{formatAmount(launch.raised, 8, { max: 4 })} of {formatAmount(launch.graduation, 8)} {TICKER} raised</span>
      </div>
      <div class="panel ba-figure"><span class="muted">Price</span><strong>{formatPrice(Number(launch.price))}</strong><span class="faint">{TICKER} for 1 <SafeText text={launch.ticker} /></span></div>
      <div class="panel ba-figure"><span class="muted">Market value</span><strong>{formatAmount(launch.market_cap, 8, { max: 2 })}</strong><span class="faint">{TICKER}, the price times the supply</span></div>
      <div class="panel ba-figure"><span class="muted">Volume</span><strong>{formatAmount(launch.volume, 8, { max: 2 })}</strong><span class="faint">{TICKER} in {formatCount(launch.trades)} blocks</span></div>
    </section>

    <div class="ba-cols">
      <section class="panel">
        <div class="panel-head"><h3>The curve</h3><span class="muted small">price by the amount sold</span></div>
        <div class="panel-body">
          <Chart points={series} marks={[{ x: now.sold, y: now.price }]} area label={t('The bonding curve of the launch: its price by the amount it sold, and where it is')} formatX={(x) => compactNumber(x, 3)} formatY={(y) => formatPrice(y, 4)} />
          <p class="ba-help">From {formatPrice(shape.startPrice)} to {formatPrice(shape.endPrice)} {TICKER} each, 9 times more, over {compactNumber(shape.curveSupply, 3)} of it; then {compactNumber(shape.poolSupply, 3)} go into its pool with the {TICKER} raised, at the curve's last price. {#if !graduated}It sold {compactNumber(Math.max(0, now.sold), 3)} so far.{/if}</p>
        </div>
      </section>

      {#if !graduated}
        <form class="panel ba-form card" aria-label="Trade on the curve" onsubmit={(e) => { e.preventDefault(); void trade(); }}>
          <div class="ba-chips" role="group" aria-label="Buy or sell">
            <button type="button" class="ba-chip" aria-pressed={side === 'buy'} onclick={() => { side = 'buy'; amountText = ''; }}>Buy</button>
            <button type="button" class="ba-chip" aria-pressed={side === 'sell'} onclick={() => { side = 'sell'; amountText = ''; }}>Sell</button>
          </div>
          <UnitsInput id="launch-amount" label={side === 'buy' ? t('You pay') : t('You sell')} decimals={inDecimals} bind:units={amount} bind:text={amountText} max={held} unit={side === 'buy' ? TICKER : launch.ticker} />
          {#if side === 'buy' && (approx(launch.min_buy) > 0 || approx(launch.max_buy) > 0)}
            <p class="ba-help">A buy pays {#if approx(launch.min_buy) > 0}at least {formatAmount(launch.min_buy, 8)} {TICKER}{/if}{#if approx(launch.min_buy) > 0 && approx(launch.max_buy) > 0} and {/if}{#if approx(launch.max_buy) > 0}at most {formatAmount(launch.max_buy, 8)} {TICKER}{/if}: outside, it is refunded.</p>
          {/if}
          <div class="field">
            <span class="label">Slippage</span>
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
              <dt>Price</dt><dd>{formatPrice(Number(quote.price))} {TICKER} each</dd>
              <dt>Price impact</dt><dd class:ba-good={Number(quote.price_impact) <= 1} class:ba-warn={Number(quote.price_impact) > 1}>{Number(quote.price_impact).toFixed(2)}%</dd>
              <dt>Fee ({launch.fee}%)</dt><dd>{formatAmount(quote.fee, 8)} {TICKER}</dd>
              <dt>To graduation after</dt><dd>{formatPercent(Number(quote.progress_after))}</dd>
            </dl>
            {#if quote.graduates}<p class="ba-help strong" data-testid="launch-graduates"><Icon name="check" size={14} /> This trade takes the launch to graduation: the asset then trades in its pool with {TICKER}.</p>{/if}
          {/if}
          <div class="row"><button type="submit" class="btn primary wide" disabled={busy || wallets.current === null || amount === null || out === null || !!problem || !!quoteError}>{busy ? t('Sending…') : side === 'buy' ? t('Buy') : t('Sell')}</button></div>
        </form>
      {/if}
    </div>

    <div class="ba-cols">
      <section class="panel">
        <div class="panel-head"><h3>About it</h3></div>
        <div class="panel-body">
          {#if launch.info}<p class="info"><SafeText text={launch.info} /></p>{:else}<p class="ba-empty">No description.</p>{/if}
          <dl class="ba-lines">
            <dt>Launched</dt><dd>block {formatCount(launch.height)}</dd>
            <dt>Supply</dt><dd>{formatAmount(launch.supply, DEC, { max: 0 })}</dd>
            <dt>Fee</dt><dd data-testid="launch-fee-split">{split}</dd>
            {#if feeSplit.length}
              <dt>Fee split</dt><dd data-testid="launch-fee-split-shares"><ul class="split-list">{#each feeSplit as s, i (i)}<li data-testid="launch-fee-share">{#if shareKind(s.to) === 'address'}<SafeText class="mono break" text={s.to} />{:else}{shareRecipientText(s)}{/if}: {shareText(s, Number(launch.creator_share) || 0, Number(launch.fee) || 0)}{#if s.paid !== undefined}{' · '}{t('%1 paid', `${formatAmount(s.paid, 8)} ${TICKER}`)}{/if}{#if approx(s.owed ?? 0) > 0}{' · '}{t('%1 owed', `${formatAmount(s.owed ?? 0, 8)} ${TICKER}`)}{/if}</li>{/each}</ul>{#if approx(launch.burned ?? 0) > 0}<span data-testid="launch-burned">{t('%1 burned', `${formatAmount(launch.burned ?? 0, DEC, { max: 2 })} ${launch.ticker}`)}</span>{/if}</dd>
              {#if graduated && poolKindText(launch, graduationFee)}<dt>Its pool</dt><dd data-testid="launch-pool-kind">{poolKindText(launch, graduationFee)}</dd>{/if}
            {:else if launch.fee_flow}
              <dt>Creator fees</dt><dd data-testid="launch-fee-destination">{feeDestinationText(launch)}{#if approx(launch.burned ?? 0) > 0}{' · '}<span data-testid="launch-burned">{t('%1 burned', `${formatAmount(launch.burned ?? 0, DEC, { max: 2 })} ${launch.ticker}`)}</span>{/if}</dd>
              {#if graduated && poolKindText(launch, graduationFee)}<dt>Its pool</dt><dd data-testid="launch-pool-kind">{poolKindText(launch, graduationFee)}</dd>{/if}
            {/if}
            <dt>Allocation</dt><dd>{#if launch.allocation > 0}{launch.allocation}% of the supply, minted to the creator at graduation (dilution){#if launch.allocation_claimed}, minted{/if}{:else}none{/if}</dd>
          </dl>
        </div>
      </section>
      <section class="panel">
        <div class="panel-head"><h3>Its creator</h3>{#if mine}<span class="ba-tag accent">this wallet</span>{/if}</div>
        <div class="panel-body">
          <p class="mono break" translate="no"><SafeText text={launch.creator} /></p>
          <dl class="ba-lines">
            <dt>Fees owed</dt><dd>{formatAmount(launch.creator_fees_owed, 8)} {TICKER}</dd>
            {#if approx(launch.creator_tokens_owed ?? 0) > 0}<dt>Tokens owed</dt><dd data-testid="launch-tokens-owed">{formatAmount(launch.creator_tokens_owed ?? 0, DEC, { max: 2 })} <SafeText text={launch.ticker} /></dd>{/if}
            <dt>Fees paid</dt><dd>{formatAmount(launch.creator_fees_paid, 8)} {TICKER}</dd>
            {#if launch.security_fees !== undefined}<dt>To the chain's security</dt><dd>{formatAmount(launch.security_fees, 8)} {TICKER}</dd>{/if}
          </dl>
          {#if owed.fees || owed.allocation || owed.tokens}
            <div class="row"><button type="button" class="btn small" disabled={busy || wallets.current === null} onclick={claim}>Pay the creator what it is owed</button></div>
            <p class="ba-help">Anyone may send it: the {TICKER} go to the creator's address, by the coinbase{#if owed.allocation}, and its allocation is minted to it{/if}.</p>
          {/if}
        </div>
      </section>
    </div>

    <section class="panel">
      <div class="panel-head"><h3>Recent blocks</h3><span class="muted small">what each block did on its curve</span></div>
      {#if log.length === 0}
        <p class="ba-empty">No trade in the last blocks.</p>
      {:else}
        <div class="ba-wrap">
          <table class="ba-table" use:stacked aria-label="Recent blocks">
            <thead><tr><th>Block</th><th class="end">Orders</th><th class="end">{TICKER} in</th><th class="end">{TICKER} out</th><th class="end">Bought</th><th class="end">Sold</th><th class="end">Fees</th><th>Events</th></tr></thead>
            <tbody>
              {#each log as r (r.height)}
                <tr>
                  <td>{formatCount(r.height)}</td>
                  <td class="end">{r.orders}</td>
                  <td class="end">{formatAmount(r.coin_in, 8, { max: 4 })}</td>
                  <td class="end">{formatAmount(r.coin_out, 8, { max: 4 })}</td>
                  <td class="end">{formatAmount(r.tokens_out, DEC, { max: 0 })}</td>
                  <td class="end">{formatAmount(r.tokens_in, DEC, { max: 0 })}</td>
                  <td class="end">{formatAmount(r.fees, 8, { max: 6 })}</td>
                  <td>{#if r.created}<span class="ba-tag">launched</span>{/if}{#if r.graduated} <span class="ba-tag accent">graduated</span>{/if}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
    </section>
  {/if}
</div>

<style>
  .split-list {
    margin: 0;
    padding-left: 1.1em;
  }
  .launch-view {
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
  .graduated {
    padding: 0.9rem 1.1rem;
    border-color: var(--accent);
    background: var(--accent-soft);
  }
  .graduated p {
    margin: 0;
  }
  .card {
    padding: 1.2rem;
  }
  .info {
    white-space: pre-wrap;
    overflow-wrap: anywhere;
    margin: 0 0 0.6rem;
  }
  .wide {
    flex: 1 1 auto;
    justify-content: center;
  }
  .strong {
    font-weight: 600;
  }
  .small {
    font-size: 0.85rem;
  }
</style>
