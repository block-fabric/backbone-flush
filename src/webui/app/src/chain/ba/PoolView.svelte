<script lang="ts">
  // A pool's page (owner decision, 2026-11-11): its pair and fee, its figures (the liquidity in ECX, the volume of a day
  // where the node keeps a market index, its swaps, its price), its price over time (ba/Candles.svelte), what is in it,
  // and "Your share" with Add liquidity (the one pool form, ba/AddLiquidity.svelte, the pool's asset fixed), Remove
  // liquidity and Collect fees. One kind of pool is shown, whatever it is on the chain: a pool that came from a graduated
  // launch is concentrated there, and the wallet's share of it is its positions over the full range
  // (ba/positions.svelte.ts: removed and collected by changeposition); a standard pool's share is its shares
  // (removepoolliquidity; its fees stay in the pool and grow the share: nothing to collect). Never a price range, a
  // depth table or a curve. The node lists no swap one by one, and no fees earned over a day: neither is shown.
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { formatCount } from '../../lib/format';
  import { go } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { toast, wallets } from '../../lib/state.svelte';
  import AddLiquidity from './AddLiquidity.svelte';
  import AssetBadge from './AssetBadge.svelte';
  import Candles from './Candles.svelte';
  import UnlockedSupplyBadge from './UnlockedSupplyBadge.svelte';
  import { assetRoute } from './assetactions';
  import { candlesOf, spanStats } from './candles';
  import { flipped, positionSides } from './cl';
  import { quoteAmounts, sharesOf, type LiquidityQuote } from './kinds';
  import { approx, assetArg, formatAmount, formatPercent, formatPrice, formatUnits, isChn, unitsOf, type Amount, type Asset, type Pool } from './model';
  import { poolEcx, poolEmpty, poolFeeText, poolSides } from './pools';
  import { loadPositions, sharePercent, sharesIn } from './positions.svelte';
  import { assetById, ba, decimalsOf, displayNameOf, launchAllocation, launchById, loadLaunchFlags, nameOf, noteKnownAssets, onCurve, priceSeries, refreshBitAssets } from './store.svelte';
  import { TICKER } from '../config';

  let { pool, back }: { pool: Pool; back: () => void } = $props();
  $effect(() => {
    void ba.version;
    loadLaunchFlags();
    loadPositions();
  });

  const sides = $derived(poolSides(pool));
  const withEcx = $derived(isChn(sides.quote.id));
  const empty = $derived(poolEmpty(pool));
  const concentrated = $derived(pool.curve === 'concentrated');
  /** Not a pool the pages show: an order book or a launch's curve, reached by an old link. */
  const other = $derived(pool.curve === 'book' ? 'book' : pool.curve === 'launch' ? 'curve' : '');
  // Its assets, when they are not among the newest the store read (their decimals): asked of the node by id.
  let read = $state<Record<string, Asset>>({});
  $effect(() => {
    for (const id of [pool.asset_a_id, pool.asset_b_id]) {
      if (isChn(id) || assetById(id) || read[id]) continue;
      rpc
        .raw<Asset>('getasset', [assetArg(id)])
        .then((a) => {
          if (a && typeof a === 'object' && a.asset) {
            noteKnownAssets([a]);
            read = { ...read, [a.asset]: a };
          }
        })
        .catch(() => undefined);
    }
  });
  const decimalsHere = (id: string) => read[id]?.decimals ?? decimalsOf(id);
  const da = $derived(decimalsHere(pool.asset_a_id));
  const db = $derived(decimalsHere(pool.asset_b_id));
  const baseDecimals = $derived(decimalsHere(sides.base.id));
  const quoteDecimals = $derived(decimalsHere(sides.quote.id));
  const series = $derived(priceSeries(pool, sides.base.id).map((p) => ({ x: p.h, y: p.p })));
  /** The launch its asset graduated from, if it did (its first liquidity is locked). */
  const launch = $derived(launchById(sides.base.id));

  // The volume of a day, from the market index's candles (none on a node without it: a dash).
  let day = $state<{ change: number; volume: number } | null>(null);
  let dayKnown = $state(false);
  $effect(() => {
    const id = pool.pool;
    void ba.version;
    let live = true;
    rpc
      .raw<unknown>('getpoolcandles', [id, '1h', Math.floor(Date.now() / 1000) - 86_400])
      .then((r) => {
        if (!live) return;
        day = spanStats(candlesOf(r));
        dayKnown = true;
      })
      .catch(() => {
        if (live) dayKnown = false;
      });
    return () => {
      live = false;
    };
  });

  // --- Your share ---
  /** A standard pool: the wallet's shares of it. */
  const position = $derived(ba.mine?.liquidity.find((l) => l.pool === pool.pool) ?? null);
  /** A pool concentrated on the chain: the wallet's positions over the full range. */
  const mine = $derived(concentrated ? sharesIn(pool.pool) : []);
  const share = $derived(concentrated ? sharePercent(pool) : (position?.percent ?? null));
  const has = $derived(concentrated ? mine.length > 0 : !!position);
  /** What the share is worth, and its fees not collected, of the pool's two assets (asset_a, asset_b). */
  const worth = $derived.by(() => {
    if (concentrated) {
      const flip = flipped(pool);
      let a = 0;
      let b = 0;
      let fa = 0;
      let fb = 0;
      for (const p of mine) {
        const s = positionSides(p, flip);
        a += approx(s.amountA);
        b += approx(s.amountB);
        fa += approx(s.feesA);
        fb += approx(s.feesB);
      }
      return { a, b, fa, fb };
    }
    return position ? { a: approx(position.value_a), b: approx(position.value_b), fa: 0, fb: 0 } : { a: 0, b: 0, fa: 0, fb: 0 };
  });
  const fees = $derived(worth.fa > 0 || worth.fb > 0);
  const pairOf = (a: number, b: number) => `${formatAmount(a, da, { max: 6 })} ${pool.asset_a} · ${formatAmount(b, db, { max: 6 })} ${pool.asset_b}`;

  let open = $state<'' | 'add' | 'remove'>('');
  let busy = $state(false);
  let percent = $state(100);
  let slippage = $state(1);
  const kindsActive = $derived(ba.dex?.curves_active === true);
  /** What the shares taken out give, as the node works it out (a standard pool: quotepoolliquidity). */
  let outQuote = $state<{ a: bigint; b: bigint } | null>(null);
  $effect(() => {
    const id = pool.pool;
    const removing = open === 'remove' && position && !concentrated ? sharesOf(unitsOf(position.shares, 0), percent) : 0n;
    const d = { a: da, b: db };
    void ba.version;
    outQuote = null;
    if (removing <= 0n || !kindsActive) return;
    let live = true;
    const timer = setTimeout(() => {
      rpc
        .raw<LiquidityQuote>('quotepoolliquidity', [id, null, Number(removing)])
        .then((q) => {
          if (!live || !q?.amounts) return;
          const got = quoteAmounts(q.amounts, pool);
          outQuote = { a: unitsOf(got.a, d.a), b: unitsOf(got.b, d.b) };
        })
        .catch(() => undefined);
    }, 250);
    return () => {
      live = false;
      clearTimeout(timer);
    };
  });

  async function remove() {
    const wallet = wallets.current;
    if (wallet === null || busy || !has) return;
    const sPercent = percent;
    const sSlip = slippage;
    const sPool = pool.pool;
    const sMine = mine.map((p) => p.position);
    const sQuote = outQuote;
    const r = await ask({
      title: t('Remove liquidity'),
      message: t('Take %1% of your share of the pool of %2 and %3 out?', String(sPercent), nameOf(pool.asset_a_id, pool.asset_a), nameOf(pool.asset_b_id, pool.asset_b)),
      informative: `${sQuote ? t('About %1 %2 and %3 %4, at least %5% less should the pool move first. ', formatUnits(sQuote.a, da), pool.asset_a, formatUnits(sQuote.b, db), pool.asset_b, String(sSlip)) : ''}${t('%1 comes by the coinbase of the next block; assets as coins of the transaction.', TICKER)}`,
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Remove', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      if (concentrated) {
        // Each of its full-range positions (one, as a rule), its fees collected with it.
        for (const id of sMine) {
          const done = await walletRpc<{ txid: string }>(wallet, 'changeposition', [id, { remove: sPercent, collect: true }]);
          if (!done) return;
        }
      } else if (kindsActive) {
        const done = await walletRpc<{ txid: string; shares: Amount }>(wallet, 'removepoolliquidity', [sPool, sPercent, null, sSlip]);
        if (!done) return;
      } else {
        const done = await walletRpc<{ txid: string }>(wallet, 'removeliquidity', [assetArg(pool.asset_a_id), assetArg(pool.asset_b_id), sPercent, sSlip]);
        if (!done) return;
      }
      toast('success', t('Liquidity removed'), t('%1% of your share, with the next block.', String(sPercent)));
      open = '';
      void refreshBitAssets();
    } catch (e) {
      await inform(t('Remove liquidity'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }

  async function collect() {
    const wallet = wallets.current;
    if (wallet === null || busy || !concentrated || !fees) return;
    const sMine = mine.filter((p) => approx(p.fees0) > 0 || approx(p.fees1) > 0).map((p) => p.position);
    const r = await ask({
      title: t('Collect fees'),
      message: t('Collect the fees your share earned: %1?', pairOf(worth.fa, worth.fb)),
      informative: t('%1 comes by the coinbase of the next block; assets as coins of the transaction.', TICKER),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Collect', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      for (const id of sMine) {
        const done = await walletRpc<{ txid: string }>(wallet, 'changeposition', [id, { collect: true }]);
        if (!done) return;
      }
      toast('success', t('Collecting fees'), t('With the next block.'));
      void refreshBitAssets({ node: false });
    } catch (e) {
      await inform(t('Collect fees'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }
</script>

<div class="pool-view" data-testid="pool-page">
  <button type="button" class="btn ghost small ba-back" onclick={back}><Icon name="back" size={15} /> All pools</button>
  <header class="panel head">
    <span class="pair"><AssetBadge id={sides.base.id} label={sides.base.label} display={displayNameOf(sides.base.id) ?? ''} /> <span class="faint">/</span> <AssetBadge id={sides.quote.id} label={sides.quote.label} /></span>
    {#if !other}<span class="ba-tag" translate="no">{poolFeeText(pool)} {t('fee')}</span>{/if}
    {#if empty && !other}<span class="ba-tag warn">Needs liquidity</span>{/if}
    {#each [pool.asset_a_id, pool.asset_b_id] as side (side)}
      <UnlockedSupplyBadge id={side} />
      {#if (launchAllocation[side] ?? 0) > 0}<span class="ba-tag warn" data-testid="pool-allocation" title="Minted to the creator at graduation, on top of the supply">Creator allocation {launchAllocation[side]}%</span>{/if}
    {/each}
    <span class="grow"></span>
    <button type="button" class="btn" onclick={() => go('assets', assetRoute(sides.base.id))}><SafeText text={sides.base.label} /></button>
    {#if !other}<button type="button" class="btn primary" disabled={empty} onclick={() => go('assets', assetRoute(sides.base.id, 'buy'))}><Icon name="swap" size={15} /> Trade</button>{/if}
  </header>

  {#if other}
    <section class="panel ba-empty" data-testid="pool-other">
      {#if other === 'curve'}
        <p>This is a launch curve, not a pool: the asset is bought and sold on its page until it graduates, and its pool opens by itself then.</p>
      {:else}
        <p>This is an order book. Order books are no longer shown in this app; an order of yours that is still open can be cancelled or collected in Portfolio, under "Closing old markets".</p>
      {/if}
      <div class="row center"><button type="button" class="btn primary" onclick={() => go('assets', assetRoute(sides.base.id))}>Its page</button>{#if other === 'book'}<button type="button" class="btn" onclick={() => go('my-assets', 'closing')}>Portfolio</button>{/if}</div>
    </section>
  {:else}
    <section class="ba-figures" data-testid="pool-figures">
      <div class="panel ba-figure"><span class="muted">Liquidity</span><strong>{withEcx ? formatAmount(poolEcx(pool) ?? 0, 8, { max: 4 }) : '–'}</strong><span class="faint">{TICKER} in the pool</span></div>
      <div class="panel ba-figure"><span class="muted">Volume 24 h</span><strong>{#if day}{formatAmount(day.volume, 8, { max: 4 })}{:else if dayKnown}0{:else}–{/if}</strong><span class="faint"><SafeText text={sides.quote.label} /></span></div>
      <div class="panel ba-figure"><span class="muted">Swaps</span><strong>{formatCount(pool.swaps)}</strong><span class="faint">since block {formatCount(pool.created)}</span></div>
      <div class="panel ba-figure"><span class="muted">{empty ? t('Starting price') : t('Price')}</span><strong>{formatPrice(sides.price)}</strong><span class="faint"><SafeText text={sides.quote.label} /> for 1 <SafeText text={sides.base.label} /></span></div>
    </section>

    <div class="ba-split">
      <div class="col">
        <section class="panel" aria-label="Price">
          <div class="panel-head"><h3>Price over time</h3></div>
          <div class="panel-body"><Candles pool={pool.pool} quoteLabel={sides.quote.label} {series} /></div>
        </section>
        <section class="panel" data-testid="pool-holds" aria-label="In the pool">
          <div class="panel-head"><h3>In the pool</h3></div>
          <div class="panel-body">
            <dl class="ba-lines">
              <dt><SafeText text={sides.base.label} /></dt><dd>{formatAmount(sides.baseReserve, baseDecimals, { max: 6 })}</dd>
              <dt><SafeText text={sides.quote.label} /></dt><dd>{formatAmount(sides.quoteReserve, quoteDecimals, { max: 6 })}</dd>
              <dt>Opened</dt><dd>block {formatCount(pool.created)}</dd>
              <dt>Pool id</dt><dd class="id"><span class="mono break" translate="no">{pool.pool}</span> <CopyButton text={pool.pool} label="Copy the pool id" /></dd>
            </dl>
            {#if launch && launch.status === 'graduated'}
              <p class="ba-help" data-testid="pool-from-launch">This pool came from a launch: what the curve raised and what was set aside for it went in at graduation, and that first liquidity is locked for good. Anyone can add liquidity on top.</p>
            {/if}
          </div>
        </section>
        <section class="panel" aria-label="Recent swaps">
          <div class="panel-head"><h3>Recent swaps</h3></div>
          <p class="ba-empty">{pool.swaps > 0 ? t('The node keeps the price of a pool over time (the chart above), not a list of its swaps.') : t('None yet.')}</p>
        </section>
      </div>

      <div class="col side">
        <section class="panel" data-testid="pool-share" aria-label="Your share">
          <div class="panel-head"><h3>Your share</h3></div>
          <div class="panel-body stack">
            <dl class="ba-lines">
              <dt>Share of the pool</dt><dd class="strong">{share !== null ? formatPercent(share) : has ? '–' : t('None yet')}</dd>
              <dt>Worth</dt><dd>{has ? pairOf(worth.a, worth.b) : '–'}</dd>
              <dt>Fees earned, not collected</dt><dd>{#if concentrated}{has ? pairOf(worth.fa, worth.fb) : '–'}{:else if has}{t('added to the pool: they grow your share')}{:else}–{/if}</dd>
            </dl>
            <div class="row">
              <button type="button" class="btn small primary" aria-expanded={open === 'add'} disabled={wallets.current === null || !withEcx} onclick={() => (open = open === 'add' ? '' : 'add')}>Add liquidity</button>
              <button type="button" class="btn small" aria-expanded={open === 'remove'} disabled={!has} onclick={() => (open = open === 'remove' ? '' : 'remove')}>Remove liquidity</button>
              <button type="button" class="btn small" disabled={busy || !concentrated || !fees} onclick={collect}>Collect fees</button>
            </div>
            {#if !has}<p class="ba-help">Remove and Collect switch on once you have a share.</p>{:else if !concentrated}<p class="ba-help">This pool's fees stay in it and grow every share: there is nothing to collect apart; they come out with the liquidity.</p>{/if}
            {#if open === 'remove' && has}
              <div class="ba-form" data-testid="remove-liquidity">
                <div class="field">
                  <label for="pv-percent">Remove {percent}%</label>
                  <input id="pv-percent" type="range" min="1" max="100" step="1" bind:value={percent} />
                  <div class="ba-chips">{#each [25, 50, 75, 100] as p (p)}<button type="button" class="ba-chip" aria-pressed={percent === p} onclick={() => (percent = p)}>{p}%</button>{/each}</div>
                </div>
                {#if !concentrated}
                  <div class="field">
                    <label for="pv-out-slippage">Slippage limit</label>
                    <select id="pv-out-slippage" bind:value={slippage}><option value={0.5}>0.5%</option><option value={1}>1%</option><option value={2}>2%</option><option value={5}>5%</option></select>
                  </div>
                {/if}
                {#if outQuote}
                  <dl class="ba-lines">
                    <dt>You get about</dt><dd>{formatUnits(outQuote.a, da)} <SafeText text={pool.asset_a} /></dd>
                    <dt></dt><dd>{formatUnits(outQuote.b, db)} <SafeText text={pool.asset_b} /></dd>
                  </dl>
                {/if}
                <div class="row"><button type="button" class="btn primary small" disabled={busy} onclick={remove}>{busy ? t('Waiting for your confirmation…') : t('Remove')}</button><button type="button" class="btn small" onclick={() => (open = '')}>Cancel</button></div>
              </div>
            {/if}
          </div>
        </section>
        {#if (open === 'add' || empty) && withEcx}
          {#key pool.pool}<AddLiquidity asset={sides.base.id} pools={[pool]} onCurve={onCurve(sides.base.id)} exact />{/key}
        {/if}
      </div>
    </div>
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
  .col {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    min-width: 0;
  }
  .stack {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
  }
  .id {
    display: flex;
    align-items: center;
    gap: 0.3rem;
    min-width: 0;
  }
  .center {
    justify-content: center;
    margin-top: 0.8rem;
  }
</style>
