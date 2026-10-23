<script lang="ts">
  // The price of an order book from the market index's candles (getpoolcandles of the book's id: its trades at 5m, 1h,
  // 4h or 1d), the wallet's own orders drawn at their limits. Asked again with each block (ba.version), never by a timer.
  // A node without the market index (or without the method) says so.
  import { errorText } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { rpc } from '../../lib/rpc';
  import { compactNumber } from './axis';
  import { CANDLE_INTERVALS, candlesOf, formatCandleTime, type Candle, type CandleInterval, type OrderMark } from './bookchart';
  import { ba } from './store.svelte';

  let { pool, marks = [], quoteLabel, tall = 200 }: { pool: string; marks?: readonly OrderMark[]; quoteLabel: string; tall?: number } = $props();

  let interval = $state<CandleInterval>('1h');
  let candles = $state.raw<Candle[]>([]);
  let error = $state('');
  $effect(() => {
    const id = pool;
    const iv = interval;
    void ba.version;
    if (!id) return;
    let live = true;
    rpc
      .raw<unknown>('getpoolcandles', [id, iv])
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
  const lo = $derived(Math.min(...candles.map((c) => c.l), ...marks.map((m) => m.price)));
  const hi = $derived(Math.max(...candles.map((c) => c.h), ...marks.map((m) => m.price)));
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

<div class="book-candles">
  <div class="ba-chips" role="group" aria-label="Interval">
    {#each CANDLE_INTERVALS as iv (iv)}<button type="button" class="ba-chip" aria-pressed={interval === iv} onclick={() => (interval = iv)}>{iv}</button>{/each}
  </div>
  <div class="ba-chart" bind:clientWidth={width}>
    {#if error}
      <p class="ba-empty">{t('No candles for this book from this node (its market index, -dexindex):')} {error}</p>
    {:else if candles.length === 0}
      <p class="ba-empty">No trade in this book yet.</p>
    {:else}
      <svg {width} height={tall} viewBox="0 0 {width} {tall}" role="img" aria-label={t('The price of the book, in candles')} onpointermove={move} onpointerleave={() => (hover = null)}>
        {#each [hi, (hi + lo) / 2, lo] as tk, i (i)}
          <line class="grid" x1={pad.l} x2={width - pad.r} y1={Y(tk)} y2={Y(tk)} />
          <text class="axis" x={pad.l - 6} y={Y(tk) + 3.5} text-anchor="end">{compactNumber(tk, 4)}</text>
        {/each}
        {#each candles as c, i (c.t)}
          <line class="wick" class:up={c.c >= c.o} x1={X(i)} x2={X(i)} y1={Y(c.h)} y2={Y(c.l)} />
          <rect class="body" class:up={c.c >= c.o} x={X(i) - Math.max(1, slot * 0.35)} width={Math.max(2, slot * 0.7)} y={Math.min(Y(c.o), Y(c.c))} height={Math.max(1, Math.abs(Y(c.o) - Y(c.c)))} />
        {/each}
        {#each marks as m (m.order)}
          <line class="mine {m.side}" x1={pad.l} x2={width - pad.r} y1={Y(m.price)} y2={Y(m.price)}><title>{t('Your order at %1', String(Number(m.price.toPrecision(10))))}</title></line>
        {/each}
        <text class="axis" x={pad.l} y={tall - 6}>{formatCandleTime(candles[0].t, interval)}</text>
        <text class="axis" x={width - pad.r} y={tall - 6} text-anchor="end">{formatCandleTime(candles[candles.length - 1].t, interval)}</text>
      </svg>
      {#if shown}<span class="readout">{formatCandleTime(shown.t, interval)} · O {compactNumber(shown.o, 6)} H {compactNumber(shown.h, 6)} L {compactNumber(shown.l, 6)} C {compactNumber(shown.c, 6)} {quoteLabel} · vol {compactNumber(shown.v, 4)}</span>{/if}
    {/if}
  </div>
</div>

<style>
  .book-candles {
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
  .mine {
    stroke-dasharray: 4 3;
    stroke-width: 1.5;
  }
  .mine.bid {
    stroke: var(--positive);
  }
  .mine.ask {
    stroke: var(--negative);
  }
  .readout {
    font-size: 0.85rem;
    color: var(--text-muted);
    font-variant-numeric: tabular-nums;
  }
</style>
