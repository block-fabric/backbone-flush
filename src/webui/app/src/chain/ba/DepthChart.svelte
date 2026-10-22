<script lang="ts">
  // The depth of a pool (Layers' DEX: previewpool's and getpoolcurve's depth): what trades of growing size, each alone
  // in its block, do to the price, each way. A chart of the price impact by the size of the trade (in percent of the
  // reserve put in, on a log scale: the line of asset_a sold, the dashed one of asset_b sold), and a table of slippage:
  // what goes in, what comes out, the impact.
  import SafeText from '../../components/SafeText.svelte';
  import { t } from '../../lib/i18n.svelte';
  import Chart from './Chart.svelte';
  import { depthSeries, slippageRows, type DepthPoint } from './kinds';
  import { formatAmount } from './model';

  let {
    depth,
    labelA,
    labelB,
    decimalsA,
    decimalsB,
    table = true,
    tall = 200,
  }: {
    depth: readonly DepthPoint[];
    /** The pool's assets (text of the chain), in the order of the depth's sides. */
    labelA: string;
    labelB: string;
    decimalsA: number;
    decimalsB: number;
    table?: boolean;
    tall?: number;
  } = $props();

  const series = $derived(depthSeries(depth));
  const rows = $derived(slippageRows(depth));
  const pct = (x: number) => `${x >= 10 ? x.toFixed(0) : x >= 1 ? x.toFixed(1) : Number(x.toPrecision(2))}%`;
  const impact = (d: DepthPoint | undefined) => (d ? `${Number(d.price_impact).toFixed(2)}%` : '–');
</script>

<div class="depth">
  {#if series.a.length === 0 && series.b.length === 0}
    <p class="ba-empty">No trade to show: the pool holds too little.</p>
  {:else}
    <Chart points={series.a.length ? series.a : series.b} compare={series.a.length ? series.b : []} logX yZero area={false} {tall} label={t('Price impact by the size of a trade, each way')} formatX={pct} formatY={(y) => `${y.toFixed(2)}%`} />
    <p class="legend small">
      <span class="key solid" aria-hidden="true"></span> Selling <SafeText text={labelA} />
      <span class="key dashed" aria-hidden="true"></span> Selling <SafeText text={labelB} />
      <span class="faint">· size in percent of what the pool holds of what is sold</span>
    </p>
    {#if table}
      <div class="ba-wrap">
        <table class="ba-table slip" aria-label="Slippage">
          <thead>
            <tr><th class="end">Size</th><th class="end">Sell <SafeText text={labelA} /></th><th class="end">Gets <SafeText text={labelB} /></th><th class="end">Impact</th><th class="end">Sell <SafeText text={labelB} /></th><th class="end">Gets <SafeText text={labelA} /></th><th class="end">Impact</th></tr>
          </thead>
          <tbody>
            {#each rows as r (r.size)}
              <tr>
                <td class="end" translate="no">{pct(r.size)}</td>
                <td class="end" translate="no">{r.a ? formatAmount(r.a.amount_in, decimalsA, { max: 6 }) : '–'}</td>
                <td class="end" translate="no">{r.a ? formatAmount(r.a.amount_out, decimalsB, { max: 6 }) : '–'}</td>
                <td class="end" translate="no">{impact(r.a)}</td>
                <td class="end" translate="no">{r.b ? formatAmount(r.b.amount_in, decimalsB, { max: 6 }) : '–'}</td>
                <td class="end" translate="no">{r.b ? formatAmount(r.b.amount_out, decimalsA, { max: 6 }) : '–'}</td>
                <td class="end" translate="no">{impact(r.b)}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
    {/if}
  {/if}
</div>

<style>
  .depth {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
  }
  .legend {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.4rem;
    margin: 0;
    color: var(--text-muted);
  }
  .key {
    display: inline-block;
    width: 1.4rem;
    height: 0;
    border-top: 2px solid var(--accent);
  }
  .key.dashed {
    border-top-style: dashed;
    border-color: var(--text-faint);
    margin-left: 0.6rem;
  }
  .small {
    font-size: 0.85rem;
  }
  .slip td,
  .slip th {
    font-size: 0.85rem;
  }
</style>
