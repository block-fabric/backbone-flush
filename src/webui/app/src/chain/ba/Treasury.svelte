<script lang="ts">
  // The treasury (Layers' DEX: gettreasury, ba/fees.ts), read only: the parts of the fees in force on this network (a swap's:
  // its providers, the chain's security, the treasury; a launch's: its creator, the treasury, the chain's security), what the
  // treasury holds per asset and where it came from (swaps, launches), the security parts held in assets other than CHN, and
  // the security parts paid with the blocks in all. Held by the chain itself: no keys, and no rule spends from it yet.
  import { stacked } from '../../lib/actions';
  import SafeText from '../../components/SafeText.svelte';
  import { errorText } from '../../lib/dialogs.svelte';
  import { rpc } from '../../lib/rpc';
  import AssetBadge from './AssetBadge.svelte';
  import { launchFeeSplit, formatShare, swapFeeSplit, treasuryRows, type Treasury } from './fees';
  import { formatAmount } from './model';
  import { ba, decimalsOf } from './store.svelte';
  import { TICKER } from '../config';

  let treasury = $state<Treasury | null>(null);
  let error = $state('');

  // Again with each block.
  $effect(() => {
    void ba.version;
    let live = true;
    rpc
      .raw<Treasury>('gettreasury', [])
      .then((r) => {
        if (!live) return;
        treasury = r && typeof r === 'object' ? r : null;
        error = '';
      })
      .catch((e) => {
        if (live) error = errorText(e);
      });
    return () => {
      live = false;
    };
  });
  const parts = $derived(treasury?.fee_parts ?? null);
  const rows = $derived(treasuryRows(treasury?.balances));
  const amount = (value: Parameters<typeof formatAmount>[0], id: string) => formatAmount(value, decimalsOf(id), { max: 8 });
</script>

{#if error}
  <p class="ba-problems bad" role="alert"><SafeText text={error} /></p>
{:else if !treasury}
  <section class="panel ba-empty">Reading the treasury…</section>
{:else}
  <section class="ba-figures" aria-label="Fee parts">
    {#if parts}
      <div class="panel ba-figure"><span class="muted">A swap's fee</span><strong>{formatShare(parts.swap_liquidity_providers)}% · {formatShare(parts.swap_security)}% · {formatShare(parts.swap_protocol)}%</strong><span class="faint">{swapFeeSplit({ liquidity_providers: parts.swap_liquidity_providers, security: parts.swap_security, protocol: parts.swap_protocol })}</span></div>
      <div class="panel ba-figure"><span class="muted">A launch's fee</span><strong>{formatShare(parts.launch_fee)}%</strong><span class="faint">{launchFeeSplit({ fee: parts.launch_fee, creator: parts.launch_creator, protocol: parts.launch_protocol, security: parts.launch_security })}</span></div>
    {/if}
    <div class="panel ba-figure"><span class="muted">Security paid</span><strong>{formatAmount(treasury.security_paid ?? 0, 8, { max: 4 })}</strong><span class="faint">{TICKER}, with the blocks' fees, to the BMM bidders and the validators</span></div>
  </section>
  <section class="panel">
    <div class="panel-head"><h3>Treasury</h3><span class="muted small">by asset</span></div>
    <p class="ba-help note">Held by the chain itself: no keys; no rule spends from it yet. The security parts of swap fees in {TICKER} are paid with the blocks; in another asset, the treasury holds them.</p>
    {#if rows.length === 0}
      <p class="ba-empty">Nothing yet: the treasury fills with the fees of swaps and launches.</p>
    {:else}
      <div class="ba-wrap">
        <table class="ba-table" use:stacked aria-label="Treasury">
          <thead><tr><th>Asset</th><th class="end">Balance</th><th class="end">From swaps</th><th class="end">From launches</th><th class="end">Security held</th></tr></thead>
          <tbody>
            {#each rows as r (r.id)}
              <tr data-testid="treasury-row">
                <td><AssetBadge id={r.id} label={r.label} size="s" /></td>
                <td class="end strong" translate="no">{amount(r.balance, r.id)}</td>
                <td class="end" translate="no">{amount(r.fromSwaps, r.id)}</td>
                <td class="end" translate="no">{amount(r.fromLaunches, r.id)}</td>
                <td class="end" translate="no">{amount(r.securityHeld, r.id)}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
    {/if}
  </section>
{/if}

<style>
  .note {
    padding: 0 1rem 0.6rem;
  }
  .strong {
    font-weight: 650;
  }
  .small {
    font-size: 0.85rem;
  }
</style>
