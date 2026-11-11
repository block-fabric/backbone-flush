<script lang="ts">
  // The trades of an asset on its launch curve, block by block (getlaunchlog keeps 10,080 blocks; the last 300 are
  // read): the orders of each block, the ECX in and out, the amounts bought and sold, the fees, and the launch's events.
  // The Trades of an asset's Activity (ba/AssetView.svelte). The node lists no trade one by one and no wallet behind it.
  import { stacked } from '../../lib/actions';
  import { formatCount } from '../../lib/format';
  import { rpc } from '../../lib/rpc';
  import { launchDecimals, type Launch, type LaunchLogRow } from './launch';
  import { formatAmount } from './model';
  import { ba } from './store.svelte';
  import { TICKER } from '../config';

  let { launch }: { launch: Launch } = $props();
  const DEC = $derived(launchDecimals(launch));
  let log = $state<LaunchLogRow[]>([]);
  $effect(() => {
    const id = launch.token;
    const made = launch.height ?? -1;
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
</script>

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
