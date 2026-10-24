<script lang="ts">
  // A concentrated pool's liquidity, and opening a position in it (Layers' DEX: ba/cl.ts): its price, tick and liquidity in
  // range; a chart of its liquidity by price (getpoolticks: a bar from each tick to the next, its height the liquidity in
  // force there), the price marked; the full range by default (user decision, 2026-10-23: prices of 0, no bound, as
  // the node takes them), or a custom range picked on it (its two bounds dragged, typed, or a preset: ±1%, ±5%, ±20%, the
  // whole range), the amounts, and the node's preview of the position (previewposition: the range to the pool's ticks, the
  // liquidity, what goes in of each; out of range, one asset alone); then openposition. Prices are of the pool's first
  // asset as the lists give it (asset_a) in its second, turned to the node's order (its lower id first) at its door.
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { formatCount } from '../../lib/format';
  import { nav } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { toast, wallets } from '../../lib/state.svelte';
  import UnitsInput from './UnitsInput.svelte';
  import { compactNumber } from './axis';
  import { chartDomain, flipped, fullRange, liquidityBars, presetRange, priceAtX, priceText, RANGE_PRESETS, sideAlone, turn, turnRange, visibleBars, xOfPrice, type PoolTicks, type PositionPreview, type RangePreset } from './cl';
  import { depositAmounts } from './kinds';
  import { amountParam, approx, formatAmount, formatPrice, isChn, unitsAtMost, type Pool } from './model';
  import { formatShare } from './fees';
  import { ba, decimalsOf, swapParts } from './store.svelte';

  let { pool }: { pool: Pool } = $props();

  const flip = $derived(flipped(pool));
  const da = $derived(decimalsOf(pool.asset_a_id));
  const db = $derived(decimalsOf(pool.asset_b_id));
  /** The node's order: its first asset's decimals and its second's. */
  const d0 = $derived(flip ? db : da);
  const d1 = $derived(flip ? da : db);
  const spacing = $derived(pool.concentrated?.tick_spacing ?? 60);
  const price = $derived(approx(pool.price));
  const full = $derived.by(() => {
    const [lo, hi] = fullRange(spacing, d0, d1);
    return turnRange(lo, hi, flip);
  });

  // --- Its liquidity by price ---
  let ticks = $state<PoolTicks | null>(null);
  let ticksError = $state('');
  $effect(() => {
    const id = pool.pool;
    void ba.version;
    let live = true;
    rpc
      .raw<PoolTicks>('getpoolticks', [id, 500])
      .then((r) => {
        if (!live) return;
        ticks = r && Array.isArray(r.ticks) ? r : null;
        ticksError = '';
      })
      .catch((e) => {
        if (live) ticksError = errorText(e);
      });
    return () => {
      live = false;
    };
  });
  const bars = $derived(ticks ? liquidityBars(ticks.ticks, flip) : []);

  // --- The range ---
  let lowerText = $state('');
  let upperText = $state('');
  let preset = $state<RangePreset | ''>('');
  const lower = $derived(Number(lowerText.trim().replace(/,/g, '')));
  const upper = $derived(Number(upperText.trim().replace(/,/g, '')));
  const rangeOk = $derived(lower > 0 && upper > lower && Number.isFinite(upper));
  /** A range of the user's own (Custom range); else the full range, which the node takes as prices of 0. */
  let custom = $state(false);
  /** The bounds the chart shows and drags: a custom range's. */
  const ranged = $derived(custom && rangeOk);
  function choose(p: RangePreset) {
    const [lo, hi] = presetRange(price, p, full);
    lowerText = priceText(lo);
    upperText = priceText(hi);
    preset = p;
  }
  // ±5% around the price at first.
  let started = false;
  $effect(() => {
    if (started || !(price > 0)) return;
    started = true;
    choose('5');
  });
  const isFull = $derived(!custom || preset === 'full');
  const domain = $derived(chartDomain(price, rangeOk && !isFull ? [lower, upper] : null));
  const shown = $derived(visibleBars(bars, domain));

  // --- The chart: its width, and the bounds dragged on it ---
  const H = 150;
  const PAD = 26;
  let width = $state(520);
  const W = $derived(Math.max(1, width));
  const X = (p: number) => xOfPrice(p, domain, W);
  const Y = (l: number) => (shown.max > 0 ? H - PAD - (l / shown.max) * (H - PAD - 8) : H - PAD);
  let dragging = $state<'lower' | 'upper' | null>(null);
  function at(e: PointerEvent): number {
    const r = (e.currentTarget as SVGElement).getBoundingClientRect();
    return priceAtX(e.clientX - r.left, domain, W);
  }
  function down(e: PointerEvent) {
    if (!custom) return;
    const p = at(e);
    // The bound nearest the pointer (both on a log scale).
    const near = !rangeOk || Math.abs(Math.log(p / lower)) <= Math.abs(Math.log(p / upper)) ? 'lower' : 'upper';
    dragging = near;
    (e.currentTarget as SVGElement).setPointerCapture?.(e.pointerId);
    move(e);
  }
  function move(e: PointerEvent) {
    if (!dragging) return;
    const p = at(e);
    preset = '';
    if (dragging === 'lower') lowerText = priceText(rangeOk ? Math.min(p, upper * 0.999) : p);
    else upperText = priceText(rangeOk ? Math.max(p, lower * 1.001) : p);
  }
  function up() {
    dragging = null;
  }

  // --- The amounts, and the node's preview ---
  let aUnits = $state<bigint | null>(null);
  let aText = $state('');
  let bUnits = $state<bigint | null>(null);
  let bText = $state('');
  let slippage = $state(1);
  /** Out of range, the asset the position takes alone (the other's field off). */
  const alone = $derived(ranged ? sideAlone(price, lower, upper) : null);
  const held = (id: string, decimals: number) => {
    if (isChn(id)) return wallets.current !== null ? (wallets.balances[wallets.current]?.trusted ?? null) : null;
    const h = ba.mine?.assets.find((x) => x.asset === id);
    return h ? unitsAtMost(h.balance, decimals) : 0n;
  };
  /** The range as the node takes it: the pool's first asset in its second. */
  const own = $derived(!custom ? [0, 0] : rangeOk ? turnRange(lower, upper, flip).map((p) => Number(p.toPrecision(12))) : null);
  const amounts = $derived(
    depositAmounts([
      { id: pool.asset_a_id, units: alone === 'b' ? null : aUnits, text: (u) => amountParam(u, da) },
      { id: pool.asset_b_id, units: alone === 'a' ? null : bUnits, text: (u) => amountParam(u, db) },
    ]),
  );
  let preview = $state<PositionPreview | null>(null);
  let previewError = $state('');
  let generation = 0;
  $effect(() => {
    const id = pool.pool;
    const range = own;
    const given = amounts;
    void ba.version;
    const mine = ++generation;
    previewError = '';
    if (!range || Object.keys(given).length === 0) {
      preview = null;
      return;
    }
    const timer = setTimeout(() => {
      rpc
        .raw<PositionPreview>('previewposition', [id, range[0], range[1], given])
        .then((p) => {
          if (mine === generation) preview = p && typeof p === 'object' ? p : null;
        })
        .catch((e) => {
          if (mine === generation) {
            preview = null;
            previewError = errorText(e);
          }
        });
    }, 300);
    return () => clearTimeout(timer);
  });
  /** The preview in the page's order: the range as the ticks make it, what goes in of each. */
  const seen = $derived.by(() => {
    if (!preview) return null;
    const [lo, hi] = turnRange(approx(preview.price_lower), approx(preview.price_upper), flip);
    return { full: !custom, lower: lo, upper: hi, a: flip ? preview.amount1 : preview.amount0, b: flip ? preview.amount0 : preview.amount1, liquidity: preview.liquidity, inRange: preview.in_range };
  });
  const empty = $derived(!!preview && (preview.liquidity === '0' || preview.liquidity === ''));

  let busy = $state(false);
  async function open() {
    const wallet = wallets.current;
    if (wallet === null || busy || !own || !preview || empty || !seen) return;
    // What the confirmation shows is what is signed: taken now.
    const sOwn = [...own];
    const sAmounts = { ...amounts };
    const sSlippage = slippage;
    const sSeen = seen;
    // The price shown, in the node's order: the wallet refuses at signing a pool moved further than the slippage from it
    // (expected_price; security review 2026-10-23).
    const sExpected = Number(turn(price, flip).toPrecision(12));
    const r = await ask({
      title: t('Open a position'),
      message: sSeen.full
        ? t('Put about %1 %2 and %3 %4 in the pool over the full range?', formatAmount(sSeen.a, da), pool.asset_a, formatAmount(sSeen.b, db), pool.asset_b)
        : t('Put about %1 %2 and %3 %4 between %5 and %6 %4 for 1 %2?', formatAmount(sSeen.a, da), pool.asset_a, formatAmount(sSeen.b, db), pool.asset_b, formatPrice(sSeen.lower), formatPrice(sSeen.upper)),
      informative: t('It earns the providers\' part of the pool\'s fees (%2% of them) while the price is in that range, and holds one asset alone out of it. The position is a token of its own: take it out, add to it or collect its fees in Positions. Liquidity %1% less, so that the amounts suffice should the price move first; the rest comes back.', String(sSlippage), formatShare(swapParts()?.liquidity_providers ?? 100)),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Open', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      const done = await walletRpc<{ txid: string; position: string }>(wallet, 'openposition', [pool.pool, sOwn[0], sOwn[1], sAmounts, sSlippage, sExpected]);
      if (!done) return;
      toast('success', t('Opening a position'), t('In the pool with the next block: see it in Positions.'));
      aText = '';
      bText = '';
    } catch (e) {
      await inform(t('Open a position'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }
</script>

<section class="panel cl" aria-label="Concentrated liquidity">
  <div class="panel-head"><h3>Liquidity by price</h3><span class="muted small">1 <SafeText text={pool.asset_a} /> in <SafeText text={pool.asset_b} /></span></div>
  <div class="panel-body">
    <dl class="ba-lines">
      <dt>Price</dt><dd class="strong">{formatPrice(price)} <SafeText text={pool.asset_b} /></dd>
      {#if pool.concentrated}
        <dt>Tick</dt><dd translate="no">{formatCount(pool.concentrated.tick)} · spacing {pool.concentrated.tick_spacing}</dd>
        <dt>Liquidity in range</dt><dd translate="no">{compactNumber(Number(pool.concentrated.liquidity), 4)}</dd>
      {/if}
    </dl>
    {#if ticksError}<p class="ba-problems"><SafeText text={ticksError} /></p>{/if}
    <div class="ba-chart dist" bind:clientWidth={width}>
      <svg width={W} height={H} viewBox="0 0 {W} {H}" role="img" aria-label={t('The liquidity of the pool by price, the price and the range picked')} onpointerdown={down} onpointermove={move} onpointerup={up} onpointercancel={up}>
        {#if ranged}
          <rect class="range" x={X(lower)} y="4" width={Math.max(1, X(upper) - X(lower))} height={H - PAD - 4} />
        {:else if !custom}
          <rect class="range" x="0" y="4" width={W} height={H - PAD - 4} />
        {/if}
        {#each shown.bars as bar, i (i)}
          <rect class="bar" class:in={!custom || (rangeOk && bar.to > lower && bar.from < upper)} x={X(bar.from)} y={Y(bar.liquidity)} width={Math.max(1, X(bar.to) - X(bar.from) - 1)} height={H - PAD - Y(bar.liquidity)} />
        {/each}
        <line class="now" x1={X(price)} x2={X(price)} y1="2" y2={H - PAD} />
        {#if ranged}
          <line class="bound" x1={X(lower)} x2={X(lower)} y1="2" y2={H - PAD} />
          <line class="bound" x1={X(upper)} x2={X(upper)} y1="2" y2={H - PAD} />
          <circle class="handle" cx={X(lower)} cy={(H - PAD) / 2} r="6" />
          <circle class="handle" cx={X(upper)} cy={(H - PAD) / 2} r="6" />
        {/if}
        <text class="axis" x="2" y={H - 8}>{formatPrice(domain[0], 4)}</text>
        <text class="axis" x={X(price)} y={H - 8} text-anchor="middle">{formatPrice(price, 4)}</text>
        <text class="axis" x={W - 2} y={H - 8} text-anchor="end">{formatPrice(domain[1], 4)}</text>
      </svg>
      {#if shown.bars.length === 0}<p class="ba-help">No liquidity near the price yet: the first position sets it.</p>{/if}
    </div>
    <p class="faint small">{custom ? 'Drag the bounds on the chart, type them, or pick a preset.' : 'The full range: your liquidity at every price, earning fees whatever the price does.'}</p>
  </div>
</section>

<form class="panel ba-form card" aria-label="Open a position" onsubmit={(e) => { e.preventDefault(); void open(); }}>
  <h3>Open a position</h3>
  <div class="ba-chips" role="group" aria-label="Range">
    <button type="button" class="ba-chip" aria-pressed={!custom} onclick={() => (custom = false)}>Full range</button>
    <button type="button" class="ba-chip" aria-pressed={custom} onclick={() => (custom = true)}>Custom range</button>
  </div>
  {#if custom}
    <div class="ba-chips" role="group" aria-label="Range presets">
      {#each RANGE_PRESETS as p (p.id)}<button type="button" class="ba-chip" aria-pressed={preset === p.id} onclick={() => choose(p.id)}>{t(p.label)}</button>{/each}
    </div>
    <div class="bounds">
      <div class="field"><label for="cl-lower">Lower price</label><input id="cl-lower" type="text" inputmode="decimal" autocomplete="off" bind:value={lowerText} oninput={() => (preset = '')} /></div>
      <div class="field"><label for="cl-upper">Upper price</label><input id="cl-upper" type="text" inputmode="decimal" autocomplete="off" bind:value={upperText} oninput={() => (preset = '')} /></div>
    </div>
    {#if (lowerText || upperText) && !rangeOk}<p class="ba-problems" role="status">The lower price is above 0 and below the upper.</p>{/if}
  {:else}
    <p class="ba-help">Every price, from the lowest to the highest: both assets go in at the pool's price, and the position never goes out of range. A custom range earns more while the price is in it, and nothing out of it.</p>
  {/if}
  <UnitsInput id="cl-amount-a" label={pool.asset_a} decimals={da} bind:units={aUnits} bind:text={aText} max={held(pool.asset_a_id, da)} unit={pool.asset_a} disabled={alone === 'b'} />
  <UnitsInput id="cl-amount-b" label={pool.asset_b} decimals={db} bind:units={bUnits} bind:text={bText} max={held(pool.asset_b_id, db)} unit={pool.asset_b} disabled={alone === 'a'} />
  {#if alone === 'a'}<p class="ba-help">The range is above the price: the position takes <SafeText text={pool.asset_a} /> alone, and turns it into <SafeText text={pool.asset_b} /> as the price rises through it.</p>
  {:else if alone === 'b'}<p class="ba-help">The range is below the price: the position takes <SafeText text={pool.asset_b} /> alone, and turns it into <SafeText text={pool.asset_a} /> as the price falls through it.</p>
  {:else if ranged}<p class="ba-help">The price is in the range: both assets go in, in the proportion the range takes; the rest of the larger comes back.</p>{/if}
  <div class="field">
    <label for="cl-slippage">Slippage</label>
    <select id="cl-slippage" bind:value={slippage}><option value={0.5}>0.5%</option><option value={1}>1%</option><option value={2}>2%</option><option value={5}>5%</option></select>
  </div>
  {#if previewError}<p class="ba-problems bad" role="status"><SafeText text={previewError} /></p>{/if}
  {#if seen}
    <dl class="ba-lines" data-testid="position-preview">
      <dt>Range</dt><dd>{#if seen.full}Full range{:else}{formatPrice(seen.lower)} – {formatPrice(seen.upper)} <span class="faint small">(to the pool's ticks)</span>{/if}</dd>
      <dt>Goes in</dt><dd>{formatAmount(seen.a, da)} <SafeText text={pool.asset_a} /></dd>
      <dt></dt><dd>{formatAmount(seen.b, db)} <SafeText text={pool.asset_b} /></dd>
      <dt>Liquidity</dt><dd translate="no">{compactNumber(Number(seen.liquidity), 4)}</dd>
      <dt>Now</dt><dd>{#if seen.inRange}<span class="ba-tag accent">In range</span> earning fees{:else}<span class="ba-tag warn">Out of range</span> earning nothing until the price comes into it{/if}</dd>
    </dl>
    {#if empty}<p class="ba-problems" role="status">Too little: no liquidity in that range (the amounts on the wrong side of the price?).</p>{/if}
  {/if}
  <div class="row">
    <button type="submit" class="btn primary" disabled={busy || wallets.current === null || !seen || empty}><Icon name="pool" size={16} /> {busy ? t('Opening…') : t('Open the position')}</button>
    <button type="button" class="btn ghost" onclick={() => (nav.tab = 'positions')}>Your positions</button>
  </div>
  <p class="faint small">The node's prices are of <SafeText text={flip ? pool.asset_b : pool.asset_a} /> in <SafeText text={flip ? pool.asset_a : pool.asset_b} />: {#if flip}the page turns them over.{:else}as shown.{/if} {formatPrice(turn(price, flip))} now.</p>
</form>

<style>
  .card {
    padding: 1.2rem;
  }
  .card h3 {
    margin: 0;
  }
  .bounds {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(9rem, 1fr));
    gap: 0.75rem;
  }
  .dist svg {
    touch-action: none;
    cursor: ew-resize;
  }
  .dist .bar {
    fill: var(--text-faint);
    opacity: 0.5;
  }
  .dist .bar.in {
    fill: var(--accent);
    opacity: 0.85;
  }
  .dist .range {
    fill: var(--accent-soft);
  }
  .dist .now {
    stroke: var(--warning);
    stroke-width: 2;
  }
  .dist .bound {
    stroke: var(--accent);
    stroke-width: 1.5;
    stroke-dasharray: 3 3;
  }
  .dist .handle {
    fill: var(--accent);
    stroke: var(--surface);
    stroke-width: 2;
  }
  .small {
    font-size: 0.85rem;
  }
  .strong {
    font-weight: 650;
  }
</style>
