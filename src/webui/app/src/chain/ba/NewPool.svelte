<script lang="ts">
  // Pools' New pool (user decision, 2026-10-23): a concentrated pool of two assets, made at the price the two amounts
  // name (1 of the first = second / first of the second) at a fee tier, then funded with both amounts over the full
  // range. Two transactions the chain needs apart, each confirmed by the wallet: createpool (curve concentrated, nothing
  // put in), then, once the pool is in a block, openposition with prices of 0 (no bound). The steps are journaled
  // (ba/store.svelte.ts PendingFund) and shown on their card (ba/FundCard.svelte). A concentrated pool of the pair at that
  // tier that is open already takes a position from its page instead. Other kinds of pools: Advanced, Create pool.
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { ask, errorText, inform } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { formatCount } from '../../lib/format';
  import { nav } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { wallets } from '../../lib/state.svelte';
  import AssetSelect from './AssetSelect.svelte';
  import FundCard from './FundCard.svelte';
  import UnitsInput from './UnitsInput.svelte';
  import { hiddenAsset } from './visible.svelte';
  import { FEE_TIERS, type PoolPreview } from './kinds';
  import { amountParam, CHN_ID, formatPrice, isChn, minChnPayout, minChnText, unitsAtMost } from './model';
  import { assetById, ba, decimalsOf, fundingOf, fundPoolParams, labelOf, makeFundedPool, nameOf, noteFunding, tradeIntent, type PendingFund } from './store.svelte';
  import { TICKER } from '../config';

  const active = $derived(ba.dex?.curves_active === true);
  const tiers = $derived(ba.dex?.fee_tiers?.length ? ba.dex.fee_tiers.map(Number) : FEE_TIERS);

  let a = $state('');
  let b = $state(CHN_ID);
  const choices = $derived.by(() => {
    const seen = new Set<string>();
    const all = [...(ba.mine?.assets ?? []).map((h) => ({ id: h.asset, label: h.label, seq: assetById(h.asset)?.seq ?? '' })), ...ba.assets.map((x) => ({ id: x.asset, label: x.label, seq: x.seq }))];
    return all.filter((c) => !seen.has(c.id) && (seen.add(c.id), true) && (c.id === a || c.id === b || !hiddenAsset(c.id, c.label)));
  });
  $effect(() => {
    if (!tradeIntent.createA) return;
    a = tradeIntent.createA;
    if (tradeIntent.createB) b = tradeIntent.createB;
    tradeIntent.createA = '';
    tradeIntent.createB = '';
  });
  $effect(() => {
    if (!a && choices.length) a = choices[0].id;
  });
  let fee = $state(0.3);
  $effect(() => {
    if (!tiers.includes(fee)) fee = tiers.includes(0.3) ? 0.3 : tiers[0];
  });
  const da = $derived(decimalsOf(a));
  const db = $derived(decimalsOf(b));
  let aText = $state('');
  let aUnits = $state<bigint | null>(null);
  let bText = $state('');
  let bUnits = $state<bigint | null>(null);
  const heldOf = (id: string, decimals: number) => {
    if (isChn(id)) return wallets.current !== null ? (wallets.balances[wallets.current]?.trusted ?? null) : null;
    const h = ba.mine?.assets.find((x) => x.asset === id);
    return h ? unitsAtMost(h.balance, decimals) : 0n;
  };
  const price = $derived(aUnits && bUnits ? Number(bUnits) / 10 ** db / (Number(aUnits) / 10 ** da) : undefined);
  const amounts = $derived(aUnits && bUnits && aUnits > 0n && bUnits > 0n ? { a: amountParam(aUnits, da), b: amountParam(bUnits, db) } : null);

  // The node's preview: whether a concentrated pool of the pair at this tier is there already.
  let preview = $state<PoolPreview | null>(null);
  let previewError = $state('');
  let generation = 0;
  $effect(() => {
    const mine = ++generation;
    const draft = a && b && a !== b && amounts ? ({ asset: a, quote: b, amount: amounts.a, quoteAmount: amounts.b, fee } as PendingFund) : null;
    void ba.version;
    previewError = '';
    if (!draft || !active) {
      preview = null;
      return;
    }
    const timer = setTimeout(() => {
      rpc
        .raw<PoolPreview>('previewpool', fundPoolParams(draft))
        .then((p) => {
          if (mine === generation) preview = p && typeof p === 'object' ? p : null;
        })
        .catch((e) => {
          if (mine === generation) {
            preview = null;
            previewError = errorText(e);
          }
        });
    }, 300);
    return () => clearTimeout(timer);
  });

  const problem = $derived.by(() => {
    if (!a || !b || a === b) return t('Pick two different assets.');
    if (aUnits === null || bUnits === null) return '';
    if (!amounts) return t('Give an amount of each: they set the price, and both go in.');
    const chn = isChn(a) ? aUnits : isChn(b) ? bUnits : null;
    if (chn !== null && chn < minChnPayout()) return t('Too little: put in at least %1 %2.', minChnText(), TICKER);
    return '';
  });
  const mine = $derived(fundingOf(wallets.current));

  let busy = $state(false);
  async function create() {
    const wallet = wallets.current;
    if (wallet === null || busy || !amounts || problem || !preview || preview.exists) return;
    // What the confirmation shows is what is journaled and signed: taken now.
    const f: PendingFund = { wallet, name: labelOf(a), asset: a, quote: b, amount: amounts.a, quoteAmount: amounts.b, fee, slippage: 1, time: Math.floor(Date.now() / 1000) };
    const la = nameOf(f.asset);
    const lb = nameOf(f.quote);
    const r = await ask({
      title: t('New pool'),
      message: t('Make a concentrated pool of %1 and %2 at 1 %1 = %3 %2, and put %4 %1 and %5 %2 in it over the full range?', la, lb, formatPrice(price), f.amount, f.quoteAmount),
      informative: t('Fee %1%. Two transactions, each confirmed by your wallet: the pool now, with nothing in it; your liquidity once the pool is in a block (about a block later), from its card here or its notice.', String(f.fee)),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Make the pool', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      await noteFunding(f);
      if (await makeFundedPool(f)) {
        aText = '';
        bText = '';
      }
    } catch (e) {
      await inform(t('New pool'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }
  function openPool(id: string) {
    tradeIntent.pool = id;
    nav.tab = 'pools';
  }
</script>

{#if !active}
  <section class="panel ba-empty">
    <p>{#if ba.dex && ba.dex.curves_height >= 0}Concentrated pools come with the pool kinds, from block {formatCount(ba.dex.curves_height)}.{:else}This network has no concentrated pools: every pool is a constant-product pool.{/if}</p>
    <div class="row center"><button type="button" class="btn primary" onclick={() => (nav.tab = 'liquidity')}>Add liquidity</button></div>
  </section>
{:else}
  <div class="ba-split">
    <form class="panel ba-form card" aria-label="New pool" onsubmit={(e) => { e.preventDefault(); void create(); }}>
      <div class="leg">
        <AssetSelect id="np-a" label="First asset" bind:value={a} {choices} />
        <UnitsInput id="np-amount-a" label="Amount" decimals={da} bind:units={aUnits} bind:text={aText} max={heldOf(a, da)} unit={labelOf(a)} />
      </div>
      <div class="plus" aria-hidden="true"><Icon name="plus" size={18} /></div>
      <div class="leg">
        <AssetSelect id="np-b" label="Second asset" bind:value={b} {choices} />
        <UnitsInput id="np-amount-b" label="Amount" decimals={db} bind:units={bUnits} bind:text={bText} max={heldOf(b, db)} unit={labelOf(b)} />
      </div>
      <div class="field">
        <span class="label">Fee tier</span>
        <div class="ba-chips" role="group" aria-label="Fee tier">
          {#each tiers as x (x)}<button type="button" class="ba-chip" aria-pressed={fee === x} onclick={() => (fee = x)}>{x}%</button>{/each}
        </div>
        <span class="hint">Low for assets that hold their price against each other; high for those that swing.</span>
      </div>
      {#if price !== undefined}<p class="ba-help" data-testid="new-pool-price">Its price: 1 <SafeText text={labelOf(a)} /> = {formatPrice(price)} <SafeText text={labelOf(b)} />, the ratio of your amounts. Both go in over the full range.</p>{/if}
      {#if problem}<p class="ba-problems" role="status">{problem}</p>{/if}
      {#if previewError}<p class="ba-problems bad" role="status"><SafeText text={previewError} /></p>{/if}
      {#if preview?.exists}
        <p class="ba-problems" role="status">A concentrated pool of these assets at {fee}% is there already: open a position in it, at its price. <button type="button" class="btn small" onclick={() => openPool(preview?.pool ?? '')}>Open the pool</button></p>
      {/if}
      <div class="row"><button type="submit" class="btn primary wide" disabled={busy || wallets.current === null || !preview || preview.exists || !!problem}><Icon name="pool" size={16} /> {busy ? t('Waiting for your confirmation…') : t('Make and fund the pool')}</button></div>
    </form>
    <section class="panel">
      <div class="panel-head"><h3>Your new pools</h3></div>
      <div class="panel-body list">
        {#each mine as f (`${f.asset}|${f.quote}|${f.time}`)}<FundCard fund={f} />{:else}<p class="faint small">A pool you make shows here, step by step: made with the next block, then your liquidity put in, over the full range, with your word.</p>{/each}
      </div>
    </section>
  </div>
{/if}

<style>
  .card {
    padding: 1.2rem;
  }
  .leg {
    display: grid;
    grid-template-columns: minmax(8rem, 0.8fr) minmax(0, 1.2fr);
    gap: 0.75rem;
    align-items: start;
  }
  .plus {
    display: grid;
    place-items: center;
    color: var(--text-muted);
  }
  .wide {
    flex: 1 1 auto;
    justify-content: center;
  }
  .list {
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
  }
  .small {
    font-size: 0.85rem;
  }
</style>
