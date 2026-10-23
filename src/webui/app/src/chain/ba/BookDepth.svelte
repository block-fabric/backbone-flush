<script lang="ts">
  // The depth of an order book (ba/bookchart.ts depthOf): the cumulative bids below the middle price and asks above it, as
  // steps, the middle marked, the wallet's own orders marked at their limits; under the pointer the price, the level's
  // amount and the total to it. Drawn again only when the book changes (each block): no timer, no animation.
  import { t } from '../../lib/i18n.svelte';
  import { compactNumber } from './axis';
  import { depthOf, levelAt, type OrderMark } from './bookchart';

  let { book, marks = [], baseLabel, quoteLabel, tall = 180 }: { book: unknown; marks?: readonly OrderMark[]; baseLabel: string; quoteLabel: string; tall?: number } = $props();

  let width = $state(520);
  const d = $derived(depthOf(book as never));
  const pad = { l: 8, r: 8, t: 10, b: 22 };
  const X = (p: number) => pad.l + ((p - d.lo) / (d.hi - d.lo || 1)) * Math.max(1, width - pad.l - pad.r);
  const Y = (v: number) => pad.t + (1 - v / d.maxTotal) * (tall - pad.t - pad.b);
  const base = $derived(Y(0));

  // Steps from the middle outward: a bid holds its total down to the next lower price; an ask up to the next higher.
  const bidPath = $derived.by(() => {
    const pts = d.bids.filter((b) => b.price >= d.lo);
    if (!pts.length || d.mid === null) return '';
    let path = `M${X(d.mid).toFixed(1)} ${base.toFixed(1)}`;
    for (const p of pts) path += `H${X(p.price).toFixed(1)}V${Y(p.total).toFixed(1)}`;
    return `${path}H${X(d.lo).toFixed(1)}V${base.toFixed(1)}Z`;
  });
  const askPath = $derived.by(() => {
    const pts = d.asks.filter((a) => a.price <= d.hi);
    if (!pts.length || d.mid === null) return '';
    let path = `M${X(d.mid).toFixed(1)} ${base.toFixed(1)}`;
    for (const p of pts) path += `H${X(p.price).toFixed(1)}V${Y(p.total).toFixed(1)}`;
    return `${path}H${X(d.hi).toFixed(1)}V${base.toFixed(1)}Z`;
  });

  let hover = $state<number | null>(null);
  const hit = $derived(hover === null ? null : levelAt(d, hover));
  function move(e: PointerEvent) {
    const r = (e.currentTarget as SVGElement).getBoundingClientRect();
    const x = e.clientX - r.left;
    hover = d.lo + ((x - pad.l) / Math.max(1, width - pad.l - pad.r)) * (d.hi - d.lo);
  }
</script>

<div class="ba-chart book-depth" bind:clientWidth={width}>
  {#if d.bids.length === 0 && d.asks.length === 0}
    <p class="ba-empty">No order to draw yet.</p>
  {:else}
    <svg {width} height={tall} viewBox="0 0 {width} {tall}" role="img" aria-label={t('Depth of the book: cumulative bids and asks around the middle price')} onpointermove={move} onpointerleave={() => (hover = null)}>
      <line class="grid" x1={pad.l} x2={width - pad.r} y1={base} y2={base} />
      {#if bidPath}<path class="bids" d={bidPath} />{/if}
      {#if askPath}<path class="asks" d={askPath} />{/if}
      {#if d.mid !== null}<line class="now" x1={X(d.mid)} x2={X(d.mid)} y1={pad.t} y2={base} />{/if}
      {#each marks.filter((m) => m.price >= d.lo && m.price <= d.hi) as m (m.order)}
        <line class="mine {m.side}" x1={X(m.price)} x2={X(m.price)} y1={pad.t} y2={base}><title>{t('Your order at %1', String(Number(m.price.toPrecision(10))))}</title></line>
      {/each}
      <text class="axis" x={pad.l} y={tall - 6}>{compactNumber(d.lo, 4)}</text>
      {#if d.mid !== null}<text class="axis" x={X(d.mid)} y={tall - 6} text-anchor="middle">{compactNumber(d.mid, 5)}</text>{/if}
      <text class="axis" x={width - pad.r} y={tall - 6} text-anchor="end">{compactNumber(d.hi, 4)}</text>
      {#if hover !== null}<line class="cursor" x1={X(hover)} x2={X(hover)} y1={pad.t} y2={base} />{/if}
    </svg>
    <span class="readout" data-testid="depth-readout">
      {#if hit}{hit.side === 'bid' ? t('Bid') : t('Ask')} {hit.priceText} {quoteLabel} · {t('amount')} {compactNumber(hit.amount, 6)} · {t('total')} {compactNumber(hit.total, 6)} {baseLabel}
      {:else if d.mid !== null}{t('mid %1', d.midText)} {quoteLabel}{/if}
    </span>
  {/if}
</div>

<style>
  .book-depth svg {
    display: block;
    touch-action: none;
  }
  .bids {
    fill: color-mix(in srgb, var(--positive) 22%, transparent);
    stroke: var(--positive);
    stroke-width: 1.5;
  }
  .asks {
    fill: color-mix(in srgb, var(--negative) 22%, transparent);
    stroke: var(--negative);
    stroke-width: 1.5;
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
  .cursor {
    stroke: var(--text-muted);
    stroke-width: 1;
  }
  .readout {
    font-size: 0.85rem;
    color: var(--text-muted);
    font-variant-numeric: tabular-nums;
  }
</style>
