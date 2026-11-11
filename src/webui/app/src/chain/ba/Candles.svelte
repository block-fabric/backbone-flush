<script lang="ts">
  // The price of a market over time, in candles, with its period buttons (owner decision, 2026-11-11: 1H, 1D, 1W, All):
  // the market index's candles of a pool or of a launch's curve (getpoolcandles of its id: ba/candles.ts), asked again
  // with each block (ba.version), never by a timer. A node without the market index (-dexindex) has none: the price as
  // this app saw it, block by block, is drawn in their place (`series`), and the page says so.
  import { errorText } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { rpc } from '../../lib/rpc';
  import Chart from './Chart.svelte';
  import { compactNumber } from './axis';
  import { candlesOf, formatCandleTime, PERIODS, type Candle, type Period } from './candles';
  import { ba } from './store.svelte';

  let { pool, quoteLabel, tall = 200, series = [] }: { pool: string; quoteLabel: string; tall?: number; /** The prices this app saw (x: the height), for a node without candles. */ series?: readonly { x: number; y: number }[] } = $props();

  let period = $state<Period>('1D');
  const asked = $derived(PERIODS.find((p) => p.id === period) ?? PERIODS[1]);
  const interval = $derived(asked.interval);
  let candles = $state.raw<Candle[]>([]);
  let error = $state('');
  $effect(() => {
    const id = pool;
    const p = asked;
    void ba.version;
    if (!id) return;
    let live = true;
    rpc
      .raw<unknown>('getpoolcandles', p.span ? [id, p.interval, Math.floor(Date.now() / 1000) - p.span] : [id, p.interval])
      .then((r) => {
        if (!live) return;
        candles = candlesOf(r).slice(-120);
        error = '';
      })
      .catch((e) => {
        if (!live) return;
        candles = [];
        error = errorText(e);
      });
    return () => {
      live = false;
    };
  });

  let width = $state(520);
  const pad = { l: 48, r: 8, t: 10, b: 22 };
  const lo = $derived(Math.min(...candles.map((c) => c.l)));
  const hi = $derived(Math.max(...candles.map((c) => c.h)));
  const span = $derived(hi - lo || hi * 0.1 || 1);
  const Y = (p: number) => pad.t + (1 - (p - (lo - span * 0.05)) / (span * 1.1)) * (tall - pad.t - pad.b);
  const slot = $derived(Math.max(1, width - pad.l - pad.r) / Math.max(1, candles.length));
  const X = (i: number) => pad.l + slot * (i + 0.5);
  let hover = $state<number | null>(null);
  function move(e: PointerEvent) {
    const r = (e.currentTarget as SVGElement).getBoundingClientRect();
    const i = Math.floor((e.clientX - r.left - pad.l) / slot);
    hover = i >= 0 && i < candles.length ? i : null;
  }
  const shown = $derived(hover !== null ? candles[hover] : candles.at(-1));
</script>

<div class="candles" data-testid="price-chart">
  <div class="ba-chips" role="group" aria-label="Period">
    {#each PERIODS as p (p.id)}<button type="button" class="ba-chip" aria-pressed={period === p.id} onclick={() => (period = p.id)}>{p.id}</button>{/each}
  </div>
  <div class="ba-chart" bind:clientWidth={width}>
    {#if error}
      {#if series.length > 1}
        <Chart points={series} step label={t('The price as this app saw it')} formatX={(x) => t('block %1', String(Math.round(x)))} />
      {/if}
      <p class="ba-help" data-testid="no-candles">This node keeps no price history (its market index is off): the chart shows the prices this app saw while it ran.</p>
    {:else if candles.length === 0}
      <p class="ba-empty">No trades in this period. The chart starts with the first trade.</p>
    {:else}
      <svg {width} height={tall} viewBox="0 0 {width} {tall}" role="img" aria-label={t('The price, in candles')} onpointermove={move} onpointerleave={() => (hover = null)}>
        {#each [hi, (hi + lo) / 2, lo] as tk, i (i)}
          <line class="grid" x1={pad.l} x2={width - pad.r} y1={Y(tk)} y2={Y(tk)} />
          <text class="axis" x={pad.l - 6} y={Y(tk) + 3.5} text-anchor="end">{compactNumber(tk, 4)}</text>
        {/each}
        {#each candles as c, i (c.t)}
          <line class="wick" class:up={c.c >= c.o} x1={X(i)} x2={X(i)} y1={Y(c.h)} y2={Y(c.l)} />
          <rect class="body" class:up={c.c >= c.o} x={X(i) - Math.max(1, slot * 0.35)} width={Math.max(2, slot * 0.7)} y={Math.min(Y(c.o), Y(c.c))} height={Math.max(1, Math.abs(Y(c.o) - Y(c.c)))} />
        {/each}
        <text class="axis" x={pad.l} y={tall - 6}>{formatCandleTime(candles[0].t, interval)}</text>
        <text class="axis" x={width - pad.r} y={tall - 6} text-anchor="end">{formatCandleTime(candles[candles.length - 1].t, interval)}</text>
      </svg>
      {#if shown}<span class="readout">{formatCandleTime(shown.t, interval)} · O {compactNumber(shown.o, 6)} H {compactNumber(shown.h, 6)} L {compactNumber(shown.l, 6)} C {compactNumber(shown.c, 6)} {quoteLabel} · vol {compactNumber(shown.v, 4)}</span>{/if}
    {/if}
  </div>
</div>

<style>
  .candles {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
  }
  .wick {
    stroke: var(--negative);
  }
  .wick.up {
    stroke: var(--positive);
  }
  .body {
    fill: var(--negative);
  }
  .body.up {
    fill: var(--positive);
  }
  .readout {
    font-size: 0.85rem;
    color: var(--text-muted);
    font-variant-numeric: tabular-nums;
  }
</style>
