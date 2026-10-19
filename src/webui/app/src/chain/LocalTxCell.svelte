<script lang="ts">
  // The Asset column of the Transactions page: each asset the transaction moved (its logo, label and number), the amount
  // with its sign, and the other side: "to" the addresses of a send, "from" those that paid what came in ("yours" marks
  // the wallet's own), with the transaction in the explorer.
  import SafeText from '../components/SafeText.svelte';
  import { go } from '../lib/nav.svelte';
  import AssetBadge from './ba/AssetBadge.svelte';
  import { assetMovesOf, counterparty, signedAmount } from './txinfo.svelte';

  // In the table: one line, cut to its column (the row's menu has the explorer); in the activity: whole, with the link.
  let { txid, full = false }: { txid: string; full?: boolean } = $props();
  const moves = $derived(assetMovesOf(txid));
</script>

{#if moves.length}
  <span class="moves" class:full data-testid="asset-moves">
    {#each moves as e, i (i)}
      {@const side = counterparty(e)}
      <span class="move">
        {#if e.asset}<span class="badge" title={e.seq ? `${e.label ?? ''} #${e.seq}` : (e.label ?? '')}><AssetBadge id={e.asset} label={e.label ?? ''} seq={full ? (e.seq ?? '') : ''} size="s" /></span>{/if}
        {#if e.amount !== undefined}<span class="amt" class:neg={e.sign < 0} class:pos={e.sign > 0} translate="no">{signedAmount(e)}</span>{/if}
        {#if side.dir && side.parties.length}
          <span class="party faint">{side.dir === 'to' ? 'to' : 'from'}
            {#each side.parties.slice(0, 3) as p (p.address)}<span class="addr" translate="no">{#if p.address === 'unknown'}unknown{:else}<SafeText text={p.address} />{/if}</span>{#if p.mine} <span class="ba-tag">yours</span>{/if} {/each}{#if side.parties.length > 3}+{side.parties.length - 3}{/if}
          </span>
        {/if}
      </span>
    {/each}
    {#if full}<button type="button" class="link small" onclick={(ev) => { ev.stopPropagation(); go('explorer', `search:${txid}`); }}>Explorer</button>{/if}
  </span>
{/if}

<style>
  .moves {
    display: flex;
    gap: 0.6rem;
    min-width: 0;
    max-width: 100%;
    overflow: hidden;
    white-space: nowrap;
    text-overflow: ellipsis;
  }
  .moves.full {
    flex-direction: column;
    gap: 0.15rem;
    white-space: normal;
  }
  .move {
    display: inline-flex;
    align-items: center;
    gap: 0.35rem;
    min-width: 0;
  }
  .full .move {
    flex-wrap: wrap;
  }
  .badge {
    flex: 0 1 auto;
    min-width: 1.6rem;
    max-width: 6rem;
    overflow: hidden;
  }
  .full .badge {
    max-width: none;
  }
  .amt {
    flex: none;
  }
  .amt {
    font-variant-numeric: tabular-nums;
  }
  .party {
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .full .party {
    overflow: visible;
  }
  .amt.neg {
    color: var(--negative);
  }
  .amt.pos {
    color: var(--positive);
  }
  .addr {
    font-family: var(--font-mono);
    font-size: 0.8rem;
  }
  .full .addr {
    overflow-wrap: anywhere;
  }
  .small {
    font-size: 0.8rem;
    align-self: flex-start;
  }
</style>
