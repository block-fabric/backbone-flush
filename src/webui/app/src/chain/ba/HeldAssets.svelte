<script lang="ts">
  // "Your assets": what the wallet holds beside ECX, on the Home and the Overview. Each asset as the app shows it
  // everywhere (AssetBadge: its logo, its name, its $TICKER, its number), its balance in its decimals (exact: the store reads amounts exactly), its worth
  // in ECX where a pool prices it, units on the way marked; the first HELD_SHOWN by worth, then balance, "View all" for
  // My assets; the assets only controlled below. From the store's one listmyassets, asked again with every block and
  // transaction of the wallet. Simple mode: a compact list.
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import { go, goTo } from '../../lib/nav.svelte';
  import { wallets } from '../../lib/state.svelte';
  import { simpleMode } from '../../lib/uimode.svelte';
  import './ba.css';
  import AssetBadge from './AssetBadge.svelte';
  import { HELD_SHOWN, heldRows } from './held';
  import { assetTitle, formatAmount, formatPrice } from './model';
  import { assetById, ba, chnPrice, displayNameOf, startBitAssets, unlockHoldings } from './store.svelte';
  import HiddenToggle from './HiddenToggle.svelte';
  import { withoutHidden } from './visible.svelte';
  import { TICKER } from '../config';

  // home: titled "My assets" (on the Home this table is the My assets card: user feedback 2026-10-19, one card, not
  // two); "Your assets" on the Overview.
  let { compact = false, home = false }: { compact?: boolean; home?: boolean } = $props();
  $effect(() => startBitAssets());

  const simple = $derived(simpleMode() || compact);
  // What the wallet hides (its asset filters: ba/hide.svelte.ts) is left out, unless shown.
  let showHidden = $state(false);
  const visible = $derived(withoutHidden(ba.wallet === wallets.current ? (ba.mine?.assets ?? []) : [], (h) => h.asset, (h) => h.label, showHidden));
  const holdings = $derived(visible.items);
  const held = $derived(heldRows(holdings, chnPrice, (id) => assetById(id)?.seq ?? ''));
  const shown = $derived(held.rows.slice(0, HELD_SHOWN));
</script>

<section class="panel held" aria-labelledby="held-title" data-testid="held-assets">
  <div class="head">
    <h2 id="held-title">{#if home}Portfolio{:else}Your assets{/if}</h2>
    {#if held.rows.length > 0}<button type="button" class="btn ghost small" onclick={() => go('my-assets')}>View all <Icon name="arrow" size={14} /></button>{/if}
  </div>
  <HiddenToggle count={visible.hidden} bind:show={showHidden} />
  {#if wallets.current === null}
    <p class="muted">No wallet is loaded.</p>
  {:else if ba.mineState === 'locked'}
    <p class="muted">The wallet is locked: its assets show once it is unlocked. <button type="button" class="btn small" onclick={() => void unlockHoldings()}>Unlock</button></p>
  {:else if !ba.mine && (ba.mineState === 'loading' || ba.mineState === 'none')}
    <p class="muted" role="status">Loading…</p>
  {:else if ba.mineState === 'error' && !ba.mine}
    <p class="muted">The assets of the wallet could not be read: <SafeText text={ba.mineError} /></p>
  {:else if held.rows.length === 0 && held.controlledOnly.length === 0}
    <p class="muted empty">No assets yet. <button type="button" class="btn small" onclick={() => go('issue')}>Register new asset</button> <button type="button" class="btn ghost small" onclick={() => go('trade', 'swap')}>Get assets in Swap</button></p>
  {:else}
    <!-- One compact row per asset (user feedback 2026-10-19): the asset as the app shows it everywhere (its logo, its
         name, its $TICKER and its number: AssetBadge), the crown if the wallet controls it, its balance and its worth;
         narrow, the worth goes. -->
    <table class="held-table" class:compact={simple} aria-labelledby="held-title">
      <thead class:sr-only={simple}><tr><th scope="col" class="c-asset">Asset</th><th scope="col" class="c-balance num">Quantity</th><th scope="col" class="c-value num">{TICKER} worth</th></tr></thead>
      <tbody>
        {#each shown as r (r.holding.asset)}
          <tr class="hrow" data-testid="held-row" title={assetTitle(r.holding.label, displayNameOf(r.holding.asset), r.seq, r.holding.asset)} onclick={() => goTo('assets', '', r.holding.asset)}>
            <td class="c-asset">
              <button type="button" class="asset" onclick={(e) => { e.stopPropagation(); goTo('assets', '', r.holding.asset); }}>
                <AssetBadge id={r.holding.asset} label={r.holding.label} seq={r.seq} size="s" />
                {#if r.holding.control}<span class="crown" title="You control this asset"><Icon name="crown" size={12} label="You control this asset" /></span>{/if}
              </button>
            </td>
            <td class="c-balance num" translate="no">{formatAmount(r.holding.balance, r.holding.decimals, { trim: false })}{#if r.pending}{' '}<span class="ba-warn on-way" title="Spendable after the next block">+{formatAmount(r.holding.pending, r.holding.decimals, { trim: false })}</span>{/if}</td>
            <td class="c-value num" translate="no">{r.value !== undefined ? formatPrice(r.value) : '–'}</td>
          </tr>
        {/each}
      </tbody>
    </table>
    {#if held.rows.length > HELD_SHOWN}<p class="faint small">And {held.rows.length - HELD_SHOWN} more in Portfolio.</p>{/if}
    {#if held.controlledOnly.length}
      <p class="small controlled" data-testid="controlled-only"><span class="muted">You control, without holding any:</span>
        {#each held.controlledOnly as r (r.holding.asset)}
          <button type="button" class="chip" title={assetTitle(r.holding.label, displayNameOf(r.holding.asset), r.seq, r.holding.asset)} onclick={() => goTo('assets', '', r.holding.asset)}>
            <AssetBadge id={r.holding.asset} label={r.holding.label} size="s" />
            <span class="crown" title="You control this asset"><Icon name="crown" size={11} label="You control this asset" /></span>
          </button>
        {/each}
      </p>
    {/if}
  {/if}
  {#if home && wallets.current !== null}
    <div class="actions">
      <button type="button" class="btn small" onclick={() => go('issue')}><Icon name="issue" size={15} /> {simpleMode() ? 'Create an asset' : 'Issue'}</button>
      <button type="button" class="btn ghost small" onclick={() => go('send')}>Send</button>
    </div>
  {/if}
</section>

<style>
  .held {
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
    padding: 1rem 1.1rem;
    min-width: 0;
  }
  .head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.5rem;
  }
  .head h2 {
    margin: 0;
    font-size: 1.05rem;
  }
  .held {
    container: held / inline-size;
  }
  .held-table {
    width: 100%;
    border-collapse: collapse;
    table-layout: fixed;
  }
  .hrow {
    height: 2.25rem;
    cursor: pointer;
    border-top: 1px solid var(--line);
  }
  .hrow:first-child {
    border-top: none;
  }
  .hrow:hover {
    background: var(--surface-2);
  }
  .hrow td {
    padding: 0 0.35rem;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    vertical-align: middle;
  }
  .c-asset {
    width: 56%;
  }
  .c-balance {
    width: 24%;
  }
  .c-value {
    width: 20%;
  }
  thead th {
    padding: 0 0.35rem 0.3rem;
    font-size: 0.75rem;
    font-weight: 600;
    color: var(--text-muted);
    text-align: start;
    white-space: nowrap;
  }
  thead th.num {
    text-align: end;
  }
  .asset {
    display: inline-flex;
    align-items: center;
    gap: 0.4rem;
    max-width: 100%;
    min-width: 0;
    color: inherit;
    text-decoration: none;
    background: none;
    border: none;
    padding: 0;
    font: inherit;
    cursor: pointer;
  }
  .asset:focus-visible {
    outline: 2px solid var(--accent);
    outline-offset: 2px;
    border-radius: var(--radius-s, 4px);
  }
  .crown {
    display: inline-flex;
    color: var(--warning, var(--accent));
    flex: none;
  }
  .on-way {
    font-size: 0.75rem;
  }
  .chip {
    display: inline-flex;
    align-items: center;
    gap: 0.3rem;
    margin: 0.15rem 0.3rem 0 0;
    padding: 0.1rem 0.5rem 0.1rem 0.15rem;
    border: 1px solid var(--line);
    border-radius: 999px;
    background: var(--surface-2);
    color: inherit;
    font: inherit;
    cursor: pointer;
    max-width: 14rem;
  }
  .sr-only {
    position: absolute;
    width: 1px;
    height: 1px;
    overflow: hidden;
    clip: rect(0 0 0 0);
    white-space: nowrap;
  }
  /* Narrow: the worth goes; the name gives way before the ticker (AssetName). */
  @container held (max-width: 21rem) {
    .c-value {
      display: none;
    }
    .c-asset {
      width: 60%;
    }
    .c-balance {
      width: 40%;
    }
  }
  .num {
    text-align: end;
    font-variant-numeric: tabular-nums;
  }
  .small {
    font-size: 0.85rem;
  }
  .controlled {
    margin: 0;
  }
  .actions {
    display: flex;
    flex-wrap: wrap;
    gap: 0.4rem;
  }
  .empty {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.5rem;
  }
</style>
