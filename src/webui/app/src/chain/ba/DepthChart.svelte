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
  /** The sizes the table shows at first. */
  const SHOWN_ROWS = 12;
  let allRows = $state(false);
  const shownRows = $derived(allRows ? rows : rows.slice(0, SHOWN_ROWS));
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
      <!-- One table each way, four columns: they fit a narrow panel as they are (not stacked into cards), a dozen rows
           at first (the site's ladder), all on asking. -->
      <div class="slips">
        {#each [{ side: 'a' as const, sold: labelA, got: labelB, din: decimalsA, dout: decimalsB }, { side: 'b' as const, sold: labelB, got: labelA, din: decimalsB, dout: decimalsA }] as w (w.side)}
          <div class="ba-wrap">
            <table class="ba-table ba-nostack slip" aria-label={t('Slippage, selling %1', w.sold)}>
              <thead>
                <tr><th class="end">Size</th><th class="end">Sell <SafeText text={w.sold} /></th><th class="end">Gets <SafeText text={w.got} /></th><th class="end">Impact</th></tr>
              </thead>
              <tbody>
                {#each shownRows as r (r.size)}
                  {@const d = w.side === 'a' ? r.a : r.b}
                  <tr>
                    <td class="end" translate="no">{pct(r.size)}</td>
                    <td class="end" translate="no">{d ? formatAmount(d.amount_in, w.din, { max: 6 }) : '–'}</td>
                    <td class="end" translate="no">{d ? formatAmount(d.amount_out, w.dout, { max: 6 }) : '–'}</td>
                    <td class="end" translate="no">{impact(d)}</td>
                  </tr>
                {/each}
              </tbody>
            </table>
          </div>
        {/each}
      </div>
      {#if rows.length > SHOWN_ROWS}
        <button type="button" class="btn small ghost all" aria-expanded={allRows} onclick={() => (allRows = !allRows)}>{allRows ? t('Fewer sizes') : t('All %1 sizes', String(rows.length))}</button>
      {/if}
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
  .slips {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(17rem, 100%), 1fr));
    gap: 0.75rem;
  }
  .slip td,
  .slip th {
    font-size: 0.85rem;
    padding-inline: 0.45rem;
  }
  .all {
    align-self: flex-start;
  }
</style>
