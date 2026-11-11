<script lang="ts">
  // The wallet's positions of concentrated liquidity that have a price range (listpositions, those over the full range
  // left out: they are pool shares, on their pool's page): each with its pool, its range on a bar with the pool's price
  // marked ("In range": it earns the pool's fees; "Out of range": it holds one asset alone and earns nothing), what it
  // holds now and the fees it earned. Price ranges are retired from the interface (owner decision, 2026-11-11): such a
  // position can only be closed here, in Portfolio's "Closing old markets" (changeposition: collect its fees, take a part
  // out, close it); none is opened or added to. Prices of the pool's first asset as the lists give it, in its second.
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { go, nav } from '../../lib/nav.svelte';
  import { poolRoute } from './assetactions';
  import { rpc } from '../../lib/rpc';
  import { toast, wallets } from '../../lib/state.svelte';
  import AssetBadge from './AssetBadge.svelte';
  import { flipped, positionSides, rangeBar, type Position } from './cl';
  import { isFullRange } from './pools';
  import { amountParam, approx, formatAmount, formatPrice, isChn, unitsAtMost, type Pool } from './model';
  import { ba, decimalsOf, poolById, refreshBitAssets } from './store.svelte';
  import { TICKER } from '../config';

  let all = $state<Position[] | null>(null);
  let error = $state('');
  /** Pools of the positions not among those the store read (asked by id). */
  let extra = $state<Record<string, Pool>>({});

  $effect(() => {
    const wallet = wallets.current;
    void ba.version;
    if (wallet === null) {
      all = null;
      return;
    }
    let live = true;
    rpc
      .raw<Position[]>('listpositions', [], { wallet })
      .then((r) => {
        if (!live) return;
        all = Array.isArray(r) ? r : [];
        error = '';
        for (const p of all) {
          if (poolById(p.pool) || extra[p.pool]) continue;
          rpc
            .raw<Pool>('getpool', [p.pool])
            .then((pool) => {
              if (pool && typeof pool === 'object') extra = { ...extra, [p.pool]: pool };
            })
            .catch(() => undefined);
        }
      })
      .catch((e) => {
        if (live) error = errorText(e);
      });
    return () => {
      live = false;
    };
  });
  const poolOfPosition = (p: Position): Pool | undefined => poolById(p.pool) ?? extra[p.pool];
  /** Those of a range: one over the full range is a pool share. */
  const list = $derived(all === null ? null : all.filter((p) => !isFullRange(p, poolOfPosition(p)?.concentrated?.tick_spacing ?? 60)));

  /** A position in the page's order, with its pool: its range, amounts and fees of asset_a and asset_b, the price now. */
  function view(p: Position) {
    const pool = poolOfPosition(p);
    if (!pool) return null;
    const flip = flipped(pool);
    const sides = positionSides(p, flip);
    const price = approx(pool.price);
    return { pool, ...sides, price, bar: rangeBar(price, sides.lower, sides.upper), da: decimalsOf(pool.asset_a_id), db: decimalsOf(pool.asset_b_id) };
  }

  // --- Changes ---
  let open = $state<{ id: string; what: 'remove' } | null>(null);
  let percent = $state(50);
  let busy = $state(false);
  async function change(p: Position, what: 'collect' | 'remove' | 'close') {
    const wallet = wallets.current;
    const v = view(p);
    if (wallet === null || busy || !v) return;
    let params: Record<string, unknown>;
    let message: string;
    if (what === 'collect') {
      params = { collect: true };
      message = t('Collect the fees of this position: %1 %2 and %3 %4?', formatAmount(v.feesA, v.da), v.pool.asset_a, formatAmount(v.feesB, v.db), v.pool.asset_b);
    } else if (what === 'remove') {
      params = { remove: percent, collect: true };
      message = t('Take %1% of this position out, and collect its fees?', String(percent));
    } else {
      params = { close: true };
      message = t('Close this position: all of it out, its fees collected, its token gone?');
    }
    const r = await ask({
      title: t('Change a position'),
      message,
      informative: t('%1 comes by the coinbase of the next block; assets as coins of the transaction.', TICKER),
      level: what === 'close' ? 'warning' : 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: what === 'close' ? 'Close it' : 'Confirm', kind: what === 'close' ? 'danger' : 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      const done = await walletRpc<{ txid: string }>(wallet, 'changeposition', [p.position, params]);
      if (!done) return;
      toast('success', t('Changing a position'), t('With the next block.'));
      open = null;
      void refreshBitAssets({ node: false });
    } catch (e) {
      await inform(t('Change a position'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }
  function openPool(id: string) {
    go('pools', poolRoute(id));
  }
</script>

{#if error}
  <p class="ba-problems bad" role="alert"><SafeText text={error} /></p>
{:else if list && list.length > 0}
  <div class="ba-cols" aria-label="Positions">
    {#each list as p (p.position)}
      {@const v = view(p)}
      <section class="panel position" aria-label={t('Position %1', p.position.slice(0, 8))}>
        <header class="head">
          {#if v}
            <span class="pair"><AssetBadge id={v.pool.asset_a_id} label={v.pool.asset_a} size="s" /> <span class="faint">/</span> <AssetBadge id={v.pool.asset_b_id} label={v.pool.asset_b} size="s" /></span>
          {:else}
            <span class="pair"><SafeText text={p.asset0} /> / <SafeText text={p.asset1} /></span>
          {/if}
          <span class="grow"></span>
          {#if p.in_range}<span class="ba-tag accent" data-testid="in-range">In range</span>{:else}<span class="ba-tag warn" data-testid="out-of-range">Out of range</span>{/if}
        </header>
        {#if v}
          <svg class="rangebar" viewBox="0 0 100 14" preserveAspectRatio="none" role="img" aria-label={t('Its range, %1 to %2, and the price, %3', formatPrice(v.lower), formatPrice(v.upper), formatPrice(v.price))}>
            <rect class="track" x="0" y="5" width="100" height="4" rx="2" />
            <rect class="span" class:out={!p.in_range} x={v.bar.from} y="4" width={Math.max(0.8, v.bar.to - v.bar.from)} height="6" rx="2" />
            <rect class="price" x={v.bar.price - 0.4} y="0" width="0.8" height="14" />
          </svg>
          <p class="faint small range">{formatPrice(v.lower)} – {formatPrice(v.upper)} <SafeText text={v.pool.asset_b} /> for 1 <SafeText text={v.pool.asset_a} /> · now {formatPrice(v.price)}</p>
          <dl class="ba-lines">
            <dt>Holds</dt><dd>{formatAmount(v.amountA, v.da)} <SafeText text={v.pool.asset_a} /></dd>
            <dt></dt><dd>{formatAmount(v.amountB, v.db)} <SafeText text={v.pool.asset_b} /></dd>
            <dt>Fees earned</dt><dd class="strong" data-testid="fees">{formatAmount(v.feesA, v.da)} <SafeText text={v.pool.asset_a} /></dd>
            <dt></dt><dd class="strong">{formatAmount(v.feesB, v.db)} <SafeText text={v.pool.asset_b} /></dd>
          </dl>
          {#if open?.id === p.position && open.what === 'remove'}
            <div class="ba-form">
              <div class="field">
                <label for="pos-percent">Take out {percent}%</label>
                <input id="pos-percent" type="range" min="1" max="100" step="1" bind:value={percent} />
                <div class="ba-chips">{#each [25, 50, 75, 100] as x (x)}<button type="button" class="ba-chip" aria-pressed={percent === x} onclick={() => (percent = x)}>{x}%</button>{/each}</div>
              </div>
              <div class="row"><button type="button" class="btn primary small" disabled={busy} onclick={() => change(p, 'remove')}>Take out</button><button type="button" class="btn small" onclick={() => (open = null)}>Cancel</button></div>
            </div>
          {:else}
            <div class="row actions">
              <button type="button" class="btn small primary" disabled={busy || (approx(p.fees0) === 0 && approx(p.fees1) === 0)} onclick={() => change(p, 'collect')}>Collect</button>
              <button type="button" class="btn small" disabled={busy} onclick={() => { open = { id: p.position, what: 'remove' }; percent = 50; }}>Remove</button>
              <button type="button" class="btn small danger" disabled={busy} onclick={() => change(p, 'close')}>Close</button>
              <button type="button" class="btn small ghost" onclick={() => openPool(p.pool)}><Icon name="pool" size={14} /> Pool</button>
            </div>
          {/if}
        {:else}
          <p class="ba-help">Reading its pool…</p>
        {/if}
        <p class="faint small mono break" translate="no">{p.position}</p>
      </section>
    {/each}
  </div>
{/if}

<style>
  .position {
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
    padding: 1rem 1.1rem;
  }
  .head {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.5rem;
  }
  .pair {
    display: inline-flex;
    align-items: center;
    gap: 0.35rem;
  }
  .rangebar {
    width: 100%;
    height: 14px;
  }
  .rangebar .track {
    fill: var(--raised);
  }
  .rangebar .span {
    fill: var(--accent);
  }
  .rangebar .span.out {
    fill: var(--warning);
  }
  .rangebar .price {
    fill: var(--text);
  }
  .range {
    margin: 0;
  }
  .actions {
    flex-wrap: wrap;
  }
  .small {
    font-size: 0.85rem;
  }
  .strong {
    font-weight: 650;
  }
</style>
