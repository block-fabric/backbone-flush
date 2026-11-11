<script lang="ts">
  // The one pool form (owner decision, 2026-11-11): Add liquidity, for one asset against ECX, the asset fixed by the page
  // that shows it (an asset's page, ba/AssetView.svelte; a pool's, ba/PoolView.svelte). It has no asset to pick and keeps
  // nothing from a form opened before. What it does follows the asset's state (ba/pools.ts poolFormState), read again
  // from the node before anything is asked of the wallet:
  //  - no pool yet: a standard pool at a fee tier, made and funded in one transaction (createpool, constant product);
  //    the two amounts set the price;
  //  - its pool is empty: both amounts go in and set the price again (createpool of the pool's own kind);
  //  - its pool has liquidity: at the pool's price (addpoolliquidity by the pool's id);
  //  - its pool is concentrated on the chain (a graduated launch's): over the full range, always (openposition with
  //    prices of 0); never a price range to pick;
  //  - a pool's price may be a stranger's: wherever the asset has a reference price the pool's is held against it,
  //    whatever the pool's depth, and where the deposit would be most of the pool (empty, dust, less than what goes in)
  //    or nothing vouches for the price, the form shows it as the price the user accepts. It compares
  //    it with a reference a stranger cannot move cheaply (the price its launch graduated at, else the deepest other
  //    pool of real depth), and takes an explicit tick when they differ by more than a few percent or when there is no
  //    such reference; the same guard is worked out again from the node's fresh lists before the wallet is asked, and
  //    the form stops if the pool's price moved past the slippage limit or the guard's answer changed (ba/pools.ts
  //    priceGuard; security reviews 2026-11-11);
  //  - the asset is on its launch curve: refused here, with the reason, as consensus refuses it (bad-ba-launch-pooled):
  //    its pool opens by itself at graduation.
  // On a node from before the pool kinds, a pool is made and added to by addliquidity (the default kind, 0.3%).
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { rpc } from '../../lib/rpc';
  import { toast, wallets } from '../../lib/state.svelte';
  import UnitsInput from './UnitsInput.svelte';
  import { flipped, turn, type PositionPreview } from './cl';
  import { depositAmounts, FEE_TIERS, feeNow, kindOf, kindParam, type LiquidityQuote, type PoolPreview } from './kinds';
  import type { Launch } from './launch';
  import { amountParam, approx, assetArg, CHN_ID, formatAmount, formatPercent, formatPrice, formatUnits, MIN_OPEN_CHN, unitsAtMost, unitsOf, type Amount, type Pool } from './model';
  import { CURVE_REFUSAL, EMPTY_POOL_TOLERANCE, poolEmpty, poolFormState, poolSides, priceGuard, type PoolFormMode, type PriceGuard } from './pools';
  import { curveShape, launchSupply } from './launch';
  import { ba, decimalsOf, labelOf, launchById, nameOf, refreshBitAssets } from './store.svelte';
  import { TICKER } from '../config';

  let {
    asset,
    pools,
    onCurve,
  }: {
    /** The asset the form is fixed on: the page's. */
    asset: string;
    /** Its pools as the page read them (listpools by asset), or the one pool of a pool's page. */
    pools: readonly Pool[];
    /** It is on its launch curve, not graduated. */
    onCurve: boolean;
  } = $props();

  const curvesActive = $derived(ba.dex?.curves_active === true);
  const form = $derived(poolFormState({ asset, wallet: wallets.current !== null, onCurve, pools, curvesActive }));
  const mode = $derived(form.mode);
  const pool = $derived(form.pool);
  const label = $derived(labelOf(asset));
  const decimals = $derived(decimalsOf(asset));
  const holding = $derived(ba.mine?.assets.find((x) => x.asset === asset) ?? null);
  const heldAsset = $derived(holding ? unitsAtMost(holding.balance, decimals) : 0n);
  const heldEcx = $derived(wallets.current !== null ? (wallets.balances[wallets.current]?.trusted ?? null) : null);

  let aUnits = $state<bigint | null>(null);
  let aText = $state('');
  let eUnits = $state<bigint | null>(null);
  let eText = $state('');
  let slippage = $state(1);
  const tiers = $derived(ba.dex?.fee_tiers?.length ? ba.dex.fee_tiers.map(Number) : FEE_TIERS);
  let fee = $state(0.3);
  $effect(() => {
    if (!tiers.includes(fee)) fee = tiers.includes(0.3) ? 0.3 : tiers[0];
  });
  /** The fee of the pool the form puts into: the tier picked for a new one, the pool's own otherwise. */
  const poolFee = $derived(pool ? feeNow(kindOf(pool)) : curvesActive ? fee : 0.3);
  /** The two amounts set the price (a new pool, an empty one). */
  const setsPrice = $derived(mode === 'create' || mode === 'reopen');
  const both = $derived((aUnits ?? 0n) > 0n && (eUnits ?? 0n) > 0n);
  const any = $derived((aUnits ?? 0n) > 0n || (eUnits ?? 0n) > 0n);
  const typedPrice = $derived(both ? Number(eUnits) / 1e8 / (Number(aUnits) / 10 ** decimals) : undefined);
  const poolPrice = $derived(pool ? poolSides(pool).price : undefined);
  /** The price the asset's launch graduated at (the end of its curve: nobody's to move), when it was launched and graduated. */
  function graduationPrice(): number | undefined {
    const l = launchById(asset);
    if (!l || l.status !== 'graduated') return undefined;
    const end = curveShape(approx(l.graduation), launchSupply(l, approx(l.supply))).endPrice;
    return Number.isFinite(end) && end > 0 ? end : undefined;
  }
  /** The guard of a deposit into `p`, with these pools known for its reference (ba/pools.ts priceGuard: nothing valued at the pool's own price). */
  function guardOf(p: Pool, known: readonly Pool[], a: bigint | null, e: bigint | null): PriceGuard | null {
    return priceGuard({ asset, pool: p, pools: known, depositAsset: Number(a ?? 0n) / 10 ** decimals, depositEcx: Number(e ?? 0n) / 1e8, launchPrice: graduationPrice() });
  }
  /**
   * The pool's price is far from the asset's reference price, or nothing vouches for it, or the deposit would be most
   * of the pool: the user accepts that price, and is told. Not for a pool the user's own two amounts price (a new one,
   * or one everyone left: create, reopen).
   */
  const guard = $derived.by(() => {
    if (!pool || setsPrice || !mode) return null;
    return guardOf(pool, [...pools, ...ba.pools.filter((p) => !pools.some((x) => x.pool === p.pool))], aUnits, eUnits);
  });
  let acknowledged = $state(false);
  const amounts = $derived(
    depositAmounts([
      { id: asset, units: aUnits, text: (u) => amountParam(u, decimals) },
      { id: CHN_ID, units: eUnits, text: (u) => amountParam(u, 8) },
    ]),
  );
  /** The kind createpool makes: a standard pool at the tier picked; an empty pool's own kind again. */
  const kind = $derived.by(() => {
    if (mode === 'reopen' && pool) {
      const k = kindOf(pool);
      const first = pool.asset_a_id === asset;
      return kindParam(k.curve, k.fee, k.amp ?? 0, (first ? k.weight_a : k.weight_b) ?? 50, k.dynamic_fee === true);
    }
    return { curve: 'constant_product', fee };
  });

  // The node's word on what the deposit makes, a moment after the last change and with each block.
  let shares = $state<bigint | null>(null);
  let share = $state<number | null>(null);
  let taken = $state<{ a: Amount; e: Amount } | null>(null);
  let exists = $state(false);
  let quoteError = $state('');
  let generation = 0;
  $effect(() => {
    const m = mode;
    const p = pool;
    const given = amounts;
    const k = kind;
    const a = aUnits;
    const e = eUnits;
    const d = decimals;
    const id = asset;
    const withKinds = curvesActive;
    void ba.version;
    const mine = ++generation;
    quoteError = '';
    exists = false;
    const none = () => {
      shares = null;
      share = null;
      taken = null;
    };
    if (!m || (setsPrice ? !(a && e && a > 0n && e > 0n) : Object.keys(given).length === 0) || !withKinds) return none();
    const timer = setTimeout(() => {
      let call: Promise<void>;
      if (m === 'create' || m === 'reopen') {
        call = rpc.raw<PoolPreview>('previewpool', [assetArg(id), amountParam(a ?? 0n, d), TICKER, amountParam(e ?? 0n, 8), k]).then((r) => {
          if (mine !== generation) return;
          exists = r?.exists === true && r?.open === true;
          shares = r ? unitsOf(r.shares, 0) : null;
          share = 100;
          taken = null;
          if (r?.too_small) quoteError = r.too_small;
        });
      } else if (m === 'add' && p) {
        call = rpc.raw<LiquidityQuote>('quotepoolliquidity', [p.pool, given]).then((r) => {
          if (mine !== generation) return;
          shares = r ? unitsOf(r.shares, 0) : null;
          share = r ? approx(r.share_of_pool) : null;
          taken = null;
        });
      } else if (m === 'full-range' && p) {
        call = rpc.raw<PositionPreview>('previewposition', [p.pool, 0, 0, given]).then((r) => {
          if (mine !== generation) return;
          const flip = flipped(p);
          const first = p.asset_a_id === id;
          // The node's order (asset0, asset1) to the pool's (asset_a, asset_b), then to (the asset, ECX).
          const pa = r ? (flip ? r.amount1 : r.amount0) : 0;
          const pb = r ? (flip ? r.amount0 : r.amount1) : 0;
          taken = r ? { a: first ? pa : pb, e: first ? pb : pa } : null;
          // No share to count in a concentrated pool: whether the deposit makes any liquidity at all.
          shares = r && r.liquidity !== '' && r.liquidity !== '0' ? 1n : r ? 0n : null;
          share = null;
        });
      } else return none();
      call.catch((err) => {
        if (mine !== generation) return;
        none();
        quoteError = errorText(err);
      });
    }, 300);
    return () => clearTimeout(timer);
  });

  const problem = $derived.by(() => {
    if (!form.ok || !any) return '';
    if (aUnits !== null && aUnits > heldAsset) return t('This wallet holds less %1 than that.', label);
    if (heldEcx !== null && eUnits !== null && eUnits > heldEcx) return t('This wallet holds less %1 than that.', TICKER);
    if (setsPrice && !both) return t('Give an amount of each: they set the price, and both go in.');
    if (setsPrice && eUnits !== null && eUnits < MIN_OPEN_CHN) return t('Too little: a pool opens with at least %1 %2.', formatUnits(MIN_OPEN_CHN, 8), TICKER);
    if (exists) return t('Someone opened this pool a moment ago, at a price of their own: nothing is sent. Look at its price first.');
    if (shares === 0n) return t('Too little: it makes no share of the pool.');
    return '';
  });
  const ready = $derived(form.ok && any && (!setsPrice || both) && !problem && !quoteError && (!curvesActive || shares !== null) && (!guard?.acknowledge || acknowledged));

  /** What each mode calls, in the wallet's words, for what was confirmed. */
  function callOf(m: PoolFormMode, p: Pool | null, sA: bigint, sE: bigint, sKind: Record<string, string | number | boolean>, sSlip: number): { method: string; params: unknown[] } {
    const a = amountParam(sA, decimals);
    const e = amountParam(sE, 8);
    const given = depositAmounts([
      { id: asset, units: sA, text: (u) => amountParam(u, decimals) },
      { id: CHN_ID, units: sE, text: (u) => amountParam(u, 8) },
    ]);
    if (!curvesActive) return { method: 'addliquidity', params: [assetArg(asset), a, TICKER, e, sSlip, m !== 'add'] };
    if (m === 'add' && p) return { method: 'addpoolliquidity', params: [p.pool, given, sSlip] };
    // The pool's price shown, in the node's order: the wallet refuses at signing a pool moved further than the slippage.
    if (m === 'full-range' && p) return { method: 'openposition', params: [p.pool, 0, 0, given, sSlip, Number(turn(approx(p.price), flipped(p)).toPrecision(12))] };
    return { method: 'createpool', params: [assetArg(asset), a, TICKER, e, sKind, sSlip] };
  }

  const deviationText = (d: number | null) => (d === null ? '–' : `${d > 0 ? '+' : ''}${Math.abs(d) >= 1000 ? d.toFixed(0) : d.toFixed(1)}%`);

  let busy = $state(false);
  async function add() {
    const wallet = wallets.current;
    if (wallet === null || busy || !ready || !mode) return;
    // What the confirmation shows is what is signed: taken now (the store refreshes while the dialogs are open).
    const sMode = mode;
    const sPool = pool;
    const sA = aUnits ?? 0n;
    const sE = eUnits ?? 0n;
    const sKind = { ...kind };
    const sSlip = slippage;
    const sFee = poolFee;
    const sGuard = guard ? { ...guard } : null;
    const sPrice = poolPrice;
    const sAck = acknowledged;
    if (sGuard?.acknowledge && !sAck) return;
    busy = true;
    try {
      // The asset's state, from the node now: a form never asks the wallet for what the chain would refuse.
      const arg = assetArg(asset);
      const [launch, now] = await Promise.all([rpc.raw<Launch>('getlaunch', [arg]).catch(() => null), rpc.raw<Pool[]>('listpools', [arg, 1000, 0]).catch(() => null)]);
      if (launch && typeof launch === 'object' && launch.status !== 'graduated') {
        await inform(t('Add liquidity'), t(CURVE_REFUSAL), 'warning');
        void refreshBitAssets();
        return;
      }
      if (now) {
        const again = poolFormState({ asset, wallet: true, onCurve: false, pools: sPool ? now.filter((p) => p.pool === sPool.pool) : now, curvesActive });
        if (again.mode !== sMode || (again.pool?.pool ?? '') !== (sPool?.pool ?? '')) {
          await inform(t('Add liquidity'), t('The pool of %1 changed while you typed (someone opened it, or put liquidity in): nothing was sent. Look at it again.', nameOf(asset)), 'warning');
          void refreshBitAssets();
          return;
        }
        // The pool as the node has it now, not as the page read it a while ago: its price within the slippage limit
        // of the price shown, and the guard's answer the same (its depth, its reference); else nothing is sent.
        const fresh = sPool ? now.find((p) => p.pool === sPool.pool) : undefined;
        if (sPool && fresh) {
          const nowPrice = poolSides(fresh).price;
          const moved = !(sPrice !== undefined && sPrice > 0 && nowPrice !== undefined && nowPrice > 0) ? !poolEmpty(fresh) && sPrice !== nowPrice : Math.abs(nowPrice / sPrice - 1) * 100 > sSlip;
          const again = sMode === 'add' || sMode === 'full-range' ? guardOf(fresh, now, sA, sE) : null;
          const changed = !!again !== !!sGuard || (again?.acknowledge ?? false) !== (sGuard?.acknowledge ?? false) || (again?.thin ?? false) !== (sGuard?.thin ?? false) || (again?.reference ?? null) !== (sGuard?.reference ?? null);
          if (moved || changed || (again?.acknowledge && !sAck)) {
            await inform(
              t('Add liquidity'),
              moved
                ? t('The price of this pool moved while you typed, from %1 to %2 %3 for 1 %4: more than your slippage limit. Nothing was sent. Look at it again.', formatPrice(sPrice), formatPrice(nowPrice), TICKER, label)
                : t('This pool, or the market its price is compared with, changed while you typed: what this form showed you about its price is no longer true. Nothing was sent. Look at it again.'),
              'warning',
            );
            acknowledged = false;
            void refreshBitAssets();
            return;
          }
        }
      }
      const what = sA > 0n && sE > 0n ? t('%1 %2 and %3 %4', formatUnits(sA, decimals), nameOf(asset), formatUnits(sE, 8), TICKER) : sA > 0n ? `${formatUnits(sA, decimals)} ${nameOf(asset)}` : `${formatUnits(sE, 8)} ${TICKER}`;
      const r = await ask({
        title: t('Add liquidity'),
        message:
          sMode === 'create'
            ? t('Make the pool of %1 and %2 and put %3 in it?', nameOf(asset), TICKER, what)
            : sMode === 'reopen'
              ? t('Put %1 into the empty pool of %2 and %3?', what, nameOf(asset), TICKER)
              : t('Put %1 into the pool of %2 and %3?', what, nameOf(asset), TICKER),
        informative:
          sMode === 'create' || sMode === 'reopen'
            ? t('Your two amounts set its price: 1 %1 = %2 %3. Fee %4% of each swap. One transaction, one confirmation; liquidity covers every price.', label, formatPrice(Number(sE) / 1e8 / (Number(sA) / 10 ** decimals)), TICKER, String(sFee))
            : sGuard
              ? t('You accept this pool\'s price, %1 %2 for 1 %3, which is not a market\'s but whoever made or last moved the pool\'s. %4 Whoever trades against a wrong price takes the difference from you. Fee %5% of each swap.', formatPrice(sPrice), TICKER, label, sGuard.reference === null ? t('There is no market deep enough to compare it with.') : t('Its reference price is %1 %2: this pool is %3 from that.', formatPrice(sGuard.reference), TICKER, deviationText(sGuard.deviation)), String(sFee))
              : t('At the pool\'s price; at most %1% less should it move first. Liquidity covers every price. Fee %2% of each swap.', String(sSlip), String(sFee)),
        level: sGuard ? 'warning' : 'question',
        choices: [
          { id: 'no', label: 'Cancel' },
          { id: 'yes', label: 'Add liquidity', kind: 'primary' },
        ],
      });
      if (r.choice !== 'yes') return;
      const how = callOf(sMode, sPool, sA, sE, sKind, sSlip);
      const done = await walletRpc<{ txid: string }>(wallet, how.method, how.params);
      if (!done) return;
      toast('success', t('Adding liquidity'), sMode === 'create' ? t('The pool of %1 is made and funded with the next block.', nameOf(asset)) : t('%1 into the pool, with the next block.', what));
      aText = '';
      eText = '';
      acknowledged = false;
      void refreshBitAssets();
    } catch (e) {
      await inform(t('Add liquidity'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }
</script>

<form class="panel ba-form card" data-testid="add-liquidity-box" data-mode={mode ?? 'refused'} data-asset={asset} aria-label={t('Add liquidity: %1 / %2', label, TICKER)} onsubmit={(e) => { e.preventDefault(); void add(); }}>
  <h3 class="title">Add liquidity · <SafeText text={label} /> / {TICKER}</h3>
  {#if !form.ok}
    <p class="ba-problems" role="status" data-testid="liquidity-refused">{t(form.reason)}</p>
  {:else}
    <p class="ba-help">
      {#if mode === 'create'}It has no pool yet. You are the first, so your two amounts set the price.
      {:else if mode === 'reopen'}Its pool is empty. You are the first back in, so your two amounts set the price.
      {:else}Either amount, or both: they go in at the pool's price{#if poolPrice}, {formatPrice(poolPrice)} {TICKER} for 1 <SafeText text={label} />{/if}.{/if}
    </p>
    {#if guard}
      <div class="ba-problems stranger" class:bad={guard.acknowledge} role="alert" data-testid="empty-pool-price">
        <p><strong>{#if poolEmpty(pool!)}This pool is empty: its starting price is {formatPrice(poolPrice)} {TICKER} for 1 <SafeText text={label} />.{:else if guard.thin}This pool holds very little: its price is {formatPrice(poolPrice)} {TICKER} for 1 <SafeText text={label} />.{:else}This pool's price is {formatPrice(poolPrice)} {TICKER} for 1 <SafeText text={label} />.{/if}</strong> {#if guard.thin}What you put in would be most of it, so you accept that price.{:else}By adding to it you accept that price.{/if} It was set by whoever made or last moved the pool, not by you; if it is wrong, whoever trades against it takes the difference from you.</p>
        {#if guard.reference !== null}
          <p data-testid="empty-pool-reference">The reference price of <SafeText text={label} /> is {formatPrice(guard.reference)} {TICKER}: this pool's price is <strong translate="no">{deviationText(guard.deviation)}</strong> from that.</p>
        {:else}
          <p data-testid="empty-pool-reference">There is no market of <SafeText text={label} /> deep enough to compare this price with.</p>
        {/if}
        {#if guard.acknowledge}
          <label class="check"><input type="checkbox" data-testid="empty-pool-ack" bind:checked={acknowledged} /> <span>{guard.reference === null ? t('I checked this price myself and accept it.') : t('I accept this price though it is more than %1% from the reference.', String(EMPTY_POOL_TOLERANCE))}</span></label>
        {/if}
      </div>
    {/if}
    <UnitsInput id="al-asset" label={label} {decimals} bind:units={aUnits} bind:text={aText} max={heldAsset} unit={label} />
    <UnitsInput id="al-ecx" label={TICKER} decimals={8} bind:units={eUnits} bind:text={eText} max={heldEcx} unit={TICKER} />
    {#if mode === 'create' && curvesActive}
      <div class="field">
        <span class="label">Fee tier</span>
        <div class="ba-chips" role="group" aria-label="Fee tier">
          {#each tiers as x (x)}<button type="button" class="ba-chip" aria-pressed={fee === x} onclick={() => (fee = x)}>{x}%</button>{/each}
        </div>
        <span class="hint">What the pool keeps of each swap, for those who provide its liquidity. Low for assets that hold their price; high for those that swing.</span>
      </div>
    {/if}
    {#if !setsPrice}
      <div class="field">
        <label for="al-slippage">Slippage limit</label>
        <select id="al-slippage" bind:value={slippage}><option value={0.5}>0.5%</option><option value={1}>1%</option><option value={2}>2%</option><option value={5}>5%</option></select>
      </div>
    {/if}
    {#if problem}<p class="ba-problems" role="status">{problem}</p>
    {:else if quoteError}<p class="ba-problems bad" role="status"><SafeText text={quoteError} /></p>{/if}
    <dl class="ba-lines" data-testid="liquidity-summary">
      <dt>Price</dt><dd>{#if setsPrice}{typedPrice !== undefined ? formatPrice(typedPrice) : '–'}{:else}{formatPrice(poolPrice)}{/if} {TICKER} per <SafeText text={label} /></dd>
      {#if taken}<dt>Goes in</dt><dd>{formatAmount(taken.a, decimals)} <SafeText text={label} /> and {formatAmount(taken.e, 8)} {TICKER}; the rest comes back</dd>{/if}
      {#if share !== null && ready}<dt>Your pool share</dt><dd>{formatPercent(share)}</dd>{/if}
      <dt>You earn</dt><dd>{poolFee}% of each swap, by your share</dd>
    </dl>
    <div class="row"><button type="submit" class="btn primary wide" disabled={busy || !ready}><Icon name="pool" size={16} /> {busy ? t('Waiting for your confirmation…') : t('Add liquidity')}</button></div>
    <p class="ba-help">One step, one confirmation. No price range to choose: liquidity always covers every price.</p>
  {/if}
</form>

<style>
  .card {
    padding: 1.2rem;
  }
  .title {
    margin: 0;
  }
  .stranger p {
    margin: 0 0 0.4rem;
  }
  .wide {
    flex: 1 1 auto;
    justify-content: center;
  }
</style>
