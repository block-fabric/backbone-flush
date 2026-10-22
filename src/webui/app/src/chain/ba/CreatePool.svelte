<script lang="ts">
  // Creating a pool of a kind (Layers' DEX): step by step, the two assets (and the pools of them there are), the curve
  // (constant product, stable or weighted, each with when to choose it), its fee tier (getdexinfo's: 0.05% for a stable
  // pool, 0.3% for the others at first), its amplification A (stable) or its weights (weighted), and the first deposit,
  // which sets its price. What createpool would make is previewed by the node as the amounts are typed (previewpool:
  // its id, whether a pool of that kind is there and open, the shares, the price, a table of slippage and a depth
  // chart); a pool of the kind that is open already takes liquidity by its id instead (its page). Before the pool kinds
  // apply (getdexinfo curves_active), the page says when they will, and offers Add liquidity.
  //
  // A concentrated pool is made at a price, empty (its liquidity comes by positions between two prices): its price in
  // place of the deposit, then a way to its page to open a position. Any kind may have a dynamic fee (its tier, plus up to
  // 2% as its price moves).
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { formatCount } from '../../lib/format';
  import { nav } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { toast, wallets } from '../../lib/state.svelte';
  import AssetSelect from './AssetSelect.svelte';
  import DepthChart from './DepthChart.svelte';
  import PoolKind from './PoolKind.svelte';
  import UnitsInput from './UnitsInput.svelte';
  import { hiddenAsset } from './visible.svelte';
  import { clampAmp, CURVE_HELP, CURVE_NAMES, CURVES, DEFAULT_AMP, defaultFee, DYNAMIC_FEE_HELP, FEE_TIERS, flipKind, isDefaultKind, kindOf, kindParam, MAX_AMP, MIN_AMP, WEIGHTS, type MadeCurve, type PoolPreview } from './kinds';
  import { priceAmounts } from './cl';
  import { amountParam, assetArg, CHN_ID, formatAmount, formatPrice, formatUnits, isChn, unitsAtMost, type Amount } from './model';
  import { swapFeeSplit } from './fees';
  import { assetById, ba, decimalsOf, labelOf, nameOf, poolsOf, refreshBitAssets, swapParts, tradeIntent } from './store.svelte';

  const dex = $derived(ba.dex);
  const active = $derived(dex?.curves_active === true);
  const tiers = $derived(dex?.fee_tiers?.length ? dex.fee_tiers.map(Number) : FEE_TIERS);
  const minAmp = $derived(dex?.min_amp ?? MIN_AMP);
  const maxAmp = $derived(dex?.max_amp ?? MAX_AMP);
  const weights = $derived(dex?.weights?.length ? dex.weights.map(Number) : WEIGHTS);

  type Step = 'assets' | 'kind' | 'deposit' | 'review' | 'done';
  const order = 'assets kind deposit review'.split(' ') as Step[];
  /** A concentrated pool made: its id, for the way to open a position in it. */
  let made = $state('');
  let step = $state<Step>('assets');

  // --- The assets ---
  let a = $state('');
  let b = $state(CHN_ID);
  // The newest assets the store keeps, and those the wallet holds, without what it hides (unless chosen already).
  const choices = $derived.by(() => {
    const seen = new Set<string>();
    const all = [...ba.assets.map((x) => ({ id: x.asset, label: x.label, seq: x.seq })), ...(ba.mine?.assets ?? []).map((h) => ({ id: h.asset, label: h.label, seq: assetById(h.asset)?.seq ?? '' }))];
    return all.filter((c) => !seen.has(c.id) && (seen.add(c.id), true) && (c.id === a || c.id === b || !hiddenAsset(c.id, c.label)));
  });
  $effect(() => {
    if (!a && choices.length) a = choices[0].id;
  });
  const da = $derived(decimalsOf(a));
  const db = $derived(decimalsOf(b));
  const pairPools = $derived(a && b && a !== b ? poolsOf(a, b) : []);

  // --- The kind ---
  let curve = $state<MadeCurve>('constant_product');
  let dynamic = $state(false);
  const cl = $derived(curve === 'concentrated');
  const STEPS = $derived<{ id: Step; label: string }[]>([
    { id: 'assets', label: 'Assets' },
    { id: 'kind', label: 'Kind' },
    { id: 'deposit', label: curve === 'concentrated' ? 'Price' : 'Deposit' },
    { id: 'review', label: 'Review' },
  ]);
  let fee = $state(0.3);
  let amp = $state(DEFAULT_AMP);
  let weightA = $state(80);
  function pickCurve(c: MadeCurve) {
    curve = c;
    fee = defaultFee(c, tiers);
    if (c === 'weighted' && !weights.includes(weightA)) weightA = weights.includes(80) ? 80 : weights[0];
  }
  const kind = $derived(kindParam(curve, fee, clampAmp(amp, minAmp, maxAmp), weightA, dynamic));
  /** The kind as the badges show it. */
  const shownKind = $derived(kindOf({ curve, fee, amp: clampAmp(amp, minAmp, maxAmp), weight_a: weightA, weight_b: 100 - weightA, default_kind: isDefaultKind(curve, fee, dynamic), dynamic_fee: dynamic }));
  /** The pool of the pair of this kind the store read, if any (its weights the other way round when its first asset is b). */
  const existing = $derived(
    pairPools.find((p) => {
      const k = p.asset_a_id === a ? kindOf(p) : flipKind(kindOf(p));
      return k.curve === curve && k.fee === fee && (k.dynamic_fee === true) === dynamic && (curve !== 'stable' || k.amp === clampAmp(amp, minAmp, maxAmp)) && (curve !== 'weighted' || k.weight_a === weightA);
    }),
  );

  // --- The deposit ---
  let aText = $state('');
  let aUnits = $state<bigint | null>(null);
  let bText = $state('');
  let bUnits = $state<bigint | null>(null);
  let slippage = $state(1);
  const heldOf = (id: string, decimals: number) => {
    if (isChn(id)) return wallets.current !== null ? (wallets.balances[wallets.current]?.trusted ?? null) : null;
    const h = ba.mine?.assets.find((x) => x.asset === id);
    return h ? unitsAtMost(h.balance, decimals) : 0n;
  };

  // A concentrated pool's price (1 of the first asset in the second): the amounts that name it (nothing goes in).
  let startText = $state('');
  const startPrice = $derived(Number(startText.trim().replace(/,/g, '')));
  /** The amounts createpool and previewpool take: the deposit, or for a concentrated pool those that name its price. */
  const amounts = $derived(cl ? priceAmounts(startPrice, db) : aUnits !== null && bUnits !== null ? { a: amountParam(aUnits, da), b: amountParam(bUnits, db) } : null);

  // The node's preview, a moment after the last change (and again with each block).
  let preview = $state<PoolPreview | null>(null);
  let previewError = $state('');
  let generation = 0;
  $effect(() => {
    const mine = ++generation;
    const args = a && b && a !== b && amounts ? [assetArg(a), amounts.a, assetArg(b), amounts.b, kind] : null;
    void ba.version;
    previewError = '';
    if (!args || !active) {
      preview = null;
      return;
    }
    const timer = setTimeout(() => {
      rpc
        .raw<PoolPreview>('previewpool', args)
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

  const canNext = $derived.by(() => {
    if (step === 'assets') return !!a && !!b && a !== b;
    if (step === 'kind') return tiers.includes(fee) && (curve !== 'weighted' || weights.includes(weightA));
    if (step === 'deposit') return !!amounts && !!preview && (cl ? !preview.exists : !preview.open && !preview.too_small);
    return false;
  });
  function next() {
    if (!canNext) return;
    step = order[Math.min(order.length - 1, order.indexOf(step) + 1)];
  }
  function back() {
    step = order[Math.max(0, order.indexOf(step) - 1)];
  }
  function openPool(id: string) {
    tradeIntent.pool = id;
    nav.tab = 'pools';
  }

  let busy = $state(false);
  async function create() {
    const wallet = wallets.current;
    if (wallet === null || busy || !amounts || !preview || preview.open || preview.too_small) return;
    // What the confirmation shows is what is signed: taken now (the preview follows the chain while the dialog is open).
    const sA = a;
    const sB = b;
    const sAmounts = amounts;
    const sCl = cl;
    const sKind = { ...kind };
    const sSlippage = slippage;
    const sPreview = preview;
    const params: unknown[] = [assetArg(sA), sAmounts.a, assetArg(sB), sAmounts.b, sKind];
    // The pool may have been made meanwhile: asked again.
    let now: PoolPreview | null = null;
    try {
      now = await rpc.raw<PoolPreview>('previewpool', params);
    } catch {
      now = null;
    }
    if (now?.open && !sCl) {
      await inform(t('Create a pool'), t('A pool of this kind of these assets was opened meanwhile: put liquidity into it from its page.'), 'warning');
      preview = now;
      return;
    }
    const la = nameOf(sA);
    const lb = nameOf(sB);
    const r = await ask({
      title: t('Create a pool'),
      message: sCl ? t('Make a concentrated pool of %1 and %2 at 1 %1 = %3 %2?', la, lb, formatPrice(sPreview.price)) : t('Make a %1 pool of %2 and %3 with %4 %2 and %5 %3?', t(CURVE_NAMES[curve]), la, lb, sAmounts.a, sAmounts.b),
      informative: sCl
        ? t('Fee %1%. Nothing goes in: its liquidity comes by positions between two prices, which you open next, on its page.', String(sKind.fee))
        : t('Fee %1%. Its price starts at 1 %2 = %3 %4: check it is the price you want. About %5 shares, at least %6% fewer should someone reopen it first.', String(sKind.fee), la, formatPrice(sPreview.price), lb, String(sPreview.shares), String(sSlippage)),
      level: sPreview.exists ? 'warning' : 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Create the pool', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      const done = await walletRpc<{ txid: string; pool: string; shares: Amount }>(wallet, 'createpool', [...params, sSlippage]);
      if (!done) return;
      if (sCl) {
        toast('success', t('Creating a pool'), t('%1 / %2, with the next block: then open a position in it.', la, lb));
        made = done.pool;
        startText = '';
        step = 'done';
        void refreshBitAssets();
        return;
      }
      toast('success', t('Creating a pool'), t('%1 / %2, for about %3 shares, with the next block.', la, lb, String(done.shares)));
      aText = '';
      bText = '';
      step = 'assets';
      void refreshBitAssets();
    } catch (e) {
      await inform(t('Create a pool'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }
</script>

{#if !active}
  <section class="panel ba-empty">
    <p>
      {#if dex && dex.curves_height >= 0}Pools of other kinds (stable and weighted pools, fee tiers) come with the pool kinds, from block {formatCount(dex.curves_height)}.
      {:else if dex}This network has no pool kinds: every pool is the kind Add liquidity makes (constant product, 0.3%).
      {:else}This node says nothing of pool kinds: every pool is the kind Add liquidity makes (constant product, 0.3%).{/if}
    </p>
    <div class="row center"><button type="button" class="btn primary" onclick={() => (nav.tab = 'liquidity')}>Add liquidity</button></div>
  </section>
{:else}
  <ol class="ba-steps" aria-label="Steps">
    {#each STEPS as s, i (s.id)}
      <li class:done={order.indexOf(step) > i} class:now={step === s.id}>{t(s.label)}</li>
    {/each}
  </ol>
  <div class="ba-split">
    <section class="panel wizard">
      {#if step === 'assets'}
        <form class="ba-form" onsubmit={(e) => { e.preventDefault(); next(); }}>
          <AssetSelect id="cp-a" label="First asset" bind:value={a} {choices} />
          <AssetSelect id="cp-b" label="Second asset" bind:value={b} {choices} />
          {#if a && a === b}<p class="ba-problems" role="status">Pick two different assets.</p>{/if}
          {#if pairPools.length}
            <div class="field">
              <span class="label">Pools of these two</span>
              <ul class="ba-list existing">
                {#each pairPools as p (p.pool)}
                  <li>
                    <button type="button" class="ba-row" onclick={() => openPool(p.pool)}>
                      <PoolKind kind={p.asset_a_id === a ? kindOf(p) : flipKind(kindOf(p))} showDefault />
                      <span class="main faint small">{#if p.abandoned}closed{:else}{formatAmount(p.reserve_a, decimalsOf(p.asset_a_id), { max: 4 })} <SafeText text={p.asset_a} /> · {formatAmount(p.reserve_b, decimalsOf(p.asset_b_id), { max: 4 })} <SafeText text={p.asset_b} />{/if}</span>
                      <span class="side"><span class="btn small ghost">Open</span></span>
                    </button>
                  </li>
                {/each}
              </ul>
              <span class="hint">A pair has one pool of each kind. Another kind makes another pool.</span>
            </div>
          {/if}
          <div class="row"><button type="submit" class="btn primary" disabled={!canNext}>Next</button></div>
        </form>
      {:else if step === 'kind'}
        <form class="ba-form" onsubmit={(e) => { e.preventDefault(); next(); }}>
          <fieldset class="curves">
            <legend>Curve</legend>
            {#each CURVES as c (c)}
              <label class="curve" class:on={curve === c}>
                <input type="radio" name="cp-curve" value={c} checked={curve === c} onchange={() => pickCurve(c)} />
                <span class="what"><strong>{t(CURVE_NAMES[c])}</strong><span class="faint small">{t(CURVE_HELP[c])}</span></span>
              </label>
            {/each}
          </fieldset>
          <div class="field">
            <span class="label">Fee tier</span>
            <div class="ba-chips" role="group" aria-label="Fee tier">
              {#each tiers as f (f)}<button type="button" class="ba-chip" aria-pressed={fee === f} onclick={() => (fee = f)}>{f}%</button>{/each}
            </div>
            <span class="hint">What each trade pays: {swapFeeSplit(swapParts())}. Low for assets that hold their price against each other; high for those that swing.{#if cl} A concentrated pool's positions are ranges of ticks spaced by its tier (0.01%: 1 tick, 0.05%: 10, 0.3%: 60, 1%: 200).{/if}</span>
          </div>
          <label class="check dynamic"><input type="checkbox" bind:checked={dynamic} /> <span><strong>Dynamic fee</strong> <span class="faint small">{t(DYNAMIC_FEE_HELP)} Another pool than the one without it.</span></span></label>
          {#if curve === 'stable'}
            <div class="field">
              <label for="cp-amp">Amplification A: {clampAmp(amp, minAmp, maxAmp)}</label>
              <input id="cp-amp-range" type="range" min={Math.log10(minAmp)} max={Math.log10(maxAmp)} step="0.01" value={Math.log10(clampAmp(amp, minAmp, maxAmp))} aria-label="Amplification, on a log scale" oninput={(e) => (amp = clampAmp(10 ** Number(e.currentTarget.value), minAmp, maxAmp))} />
              <input id="cp-amp" type="number" min={minAmp} max={maxAmp} step="1" bind:value={amp} onchange={() => (amp = clampAmp(amp, minAmp, maxAmp))} />
              <span class="hint">{minAmp} to {formatCount(maxAmp)}. The higher, the flatter the price near the balance (less slippage there), and the steeper away from it. 100 suits most pairs of stablecoins.</span>
            </div>
          {:else if curve === 'weighted'}
            <div class="field">
              <span class="label">Weights</span>
              <div class="ba-chips" role="group" aria-label="Weights">
                {#each weights as w (w)}<button type="button" class="ba-chip" aria-pressed={weightA === w} onclick={() => (weightA = w)}>{w}/{100 - w}</button>{/each}
              </div>
              <span class="hint">{weightA}% of the pool's worth in <SafeText text={labelOf(a)} />, {100 - weightA}% in <SafeText text={labelOf(b)} />.</span>
            </div>
          {/if}
          <p class="ba-help"><PoolKind kind={shownKind} showDefault />
            {#if shownKind.default_kind}The kind Add liquidity makes: swaps and routes find it by its two assets alone.{/if}</p>
          {#if existing}
            <p class="ba-problems" role="status">{#if existing.abandoned}A pool of this kind is there, closed: creating it reopens it, at the price of your amounts.{:else}A pool of this kind is open already: put liquidity into it from its page.{/if}
              {#if !existing.abandoned}<button type="button" class="btn small" onclick={() => openPool(existing.pool)}>Open the pool</button>{/if}</p>
          {/if}
          <div class="row"><button type="button" class="btn" onclick={back}>Back</button><button type="submit" class="btn primary" disabled={!canNext}>Next</button></div>
        </form>
      {:else if step === 'deposit'}
        <form class="ba-form" onsubmit={(e) => { e.preventDefault(); next(); }}>
          {#if cl}
            <div class="field">
              <label for="cp-price">Its price: 1 <SafeText text={labelOf(a)} /> in <SafeText text={labelOf(b)} /></label>
              <input id="cp-price" type="text" inputmode="decimal" autocomplete="off" placeholder="Such as 1.0" bind:value={startText} aria-invalid={startText.trim() && !amounts ? 'true' : undefined} />
              {#if startText.trim() && !amounts}<span class="error">Enter a price above 0.</span>{:else}<span class="hint">The pool starts at this price, empty: nothing goes in now. Positions bring its liquidity, each between two prices.</span>{/if}
            </div>
            {#if preview?.exists}
              <p class="ba-problems" role="status">A concentrated pool of this fee tier of these assets exists: open a position in it. <button type="button" class="btn small" onclick={() => openPool(preview?.pool ?? '')}>Open the pool</button></p>
            {/if}
          {:else}
          <UnitsInput id="cp-amount-a" label="First amount" decimals={da} bind:units={aUnits} bind:text={aText} max={heldOf(a, da)} unit={labelOf(a)} />
          <UnitsInput id="cp-amount-b" label="Second amount" decimals={db} bind:units={bUnits} bind:text={bText} max={heldOf(b, db)} unit={labelOf(b)} />
          <p class="ba-help">Both amounts go in whole and set the pool's price{#if curve === 'weighted'} with its weights{/if}. It keeps 1,000 shares for good, so that it is never emptied; a pool with CHN opens with at least 0.01 CHN.</p>
          {#if previewError}<p class="ba-problems bad" role="status"><SafeText text={previewError} /></p>{/if}
          {#if preview?.too_small}<p class="ba-problems" role="status">Too little to open a pool: <SafeText text={preview.too_small} />.</p>{/if}
          {/if}
          {#if !cl && preview?.open}
            <p class="ba-problems" role="status">A pool of this kind is open already: put liquidity into it from its page. <button type="button" class="btn small" onclick={() => openPool(preview?.pool ?? '')}>Open the pool</button></p>
          {:else if !cl && preview?.exists}
            <p class="ba-help">A pool of this kind is there, closed (nobody provides liquidity to it): creating it reopens it, its dust counting toward your amounts.</p>
          {/if}
          <div class="row"><button type="button" class="btn" onclick={back}>Back</button><button type="submit" class="btn primary" disabled={!canNext}>Next</button></div>
        </form>
      {:else if step === 'review'}
        <form class="ba-form" onsubmit={(e) => { e.preventDefault(); void create(); }}>
          <h2>Review</h2>
          <dl class="ba-lines">
            <dt>Pair</dt><dd><SafeText text={nameOf(a)} /> / <SafeText text={nameOf(b)} /></dd>
            <dt>Kind</dt><dd><PoolKind kind={shownKind} showDefault /></dd>
            {#if !cl && aUnits !== null && bUnits !== null}
              <dt>Goes in</dt><dd>{formatUnits(aUnits, da)} <SafeText text={labelOf(a)} /></dd>
              <dt></dt><dd>{formatUnits(bUnits, db)} <SafeText text={labelOf(b)} /></dd>
            {/if}
            {#if preview}
              <dt>Its price</dt><dd>1 <SafeText text={labelOf(a)} /> = {formatPrice(preview.price)} <SafeText text={labelOf(b)} /></dd>
              {#if cl}<dt>Goes in</dt><dd>nothing: positions bring its liquidity</dd>{:else}<dt>Shares</dt><dd class="strong">≈ {formatAmount(preview.shares, 0)}</dd>{/if}
            {/if}
          </dl>
          <div class="field">
            <label for="cp-slippage">Slippage</label>
            <select id="cp-slippage" bind:value={slippage}><option value={0.5}>0.5%</option><option value={1}>1%</option><option value={2}>2%</option><option value={5}>5%</option></select>
            <span class="hint">How many fewer shares than quoted it may give, should someone reopen the pool first.</span>
          </div>
          <div class="row"><button type="button" class="btn" onclick={back}>Back</button><button type="submit" class="btn primary" disabled={busy || wallets.current === null || !preview || preview.open || !!preview.too_small}><Icon name="pool" size={16} /> {busy ? 'Creating…' : 'Create the pool'}</button></div>
        </form>
      {:else}
        <div class="ba-form" data-testid="pool-made">
          <h2>Made, empty</h2>
          <p class="ba-help">The concentrated pool is made with the next block, at its price, with nothing in. Open a position in it: liquidity between two prices, which earns its fees while the price is between them.</p>
          <p class="mono break small" translate="no">{made}</p>
          <div class="row"><button type="button" class="btn primary" onclick={() => openPool(made)}><Icon name="pool" size={16} /> Open a position</button><button type="button" class="btn" onclick={() => (step = 'assets')}>Create another pool</button></div>
        </div>
      {/if}
    </section>

    <section class="panel">
      <div class="panel-head"><h3>{step === 'deposit' || step === 'review' ? 'Preview' : 'Pool kinds'}</h3></div>
      <div class="panel-body">
        {#if (step === 'deposit' || step === 'review') && preview}
          <dl class="ba-lines" data-testid="pool-preview">
            <dt>Pool id</dt><dd class="mono break" translate="no">{preview.pool}</dd>
            <dt>State</dt><dd>{#if cl && preview.exists}there already{:else if preview.open}open: it has providers{:else if preview.exists}there, closed: this reopens it{:else}new{/if}</dd>
            <dt>Its price</dt><dd>1 <SafeText text={labelOf(a)} /> = {formatPrice(preview.price)} <SafeText text={labelOf(b)} /></dd>
            {#if !cl}<dt>Shares</dt><dd>{formatAmount(preview.shares, 0)}</dd>{/if}
          </dl>
          {#if preview.depth?.length}
            <h4>Slippage of the new pool</h4>
            <DepthChart depth={preview.depth} labelA={labelOf(a)} labelB={labelOf(b)} decimalsA={da} decimalsB={db} />
          {/if}
        {:else if step === 'deposit' || step === 'review'}
          <p class="ba-empty">Type both amounts: the node previews the pool.</p>
        {:else}
          <ul class="how">
            {#each CURVES as c (c)}<li><strong>{t(CURVE_NAMES[c])}.</strong> {t(CURVE_HELP[c])}</li>{/each}
            <li>Each trade pays the pool's fee: {swapFeeSplit(swapParts())}; the providers' part to each in proportion to their shares.</li>
            <li>Swaps compare every pool of a pair, and routes go through pools of any kind.</li>
          </ul>
        {/if}
      </div>
    </section>
  </div>
{/if}

<style>
  .wizard {
    padding: 1.2rem;
  }
  .curves {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
    margin: 0;
    padding: 0;
    border: 0;
  }
  .curves legend {
    margin-bottom: 0.4rem;
    font-weight: 600;
  }
  .curve {
    display: flex;
    gap: 0.6rem;
    align-items: flex-start;
    padding: 0.6rem 0.75rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-s);
    cursor: pointer;
  }
  .curve.on {
    border-color: var(--accent);
    background: var(--accent-soft);
  }
  .what {
    display: flex;
    flex-direction: column;
    gap: 0.2rem;
  }
  .existing {
    margin: 0;
  }
  .how {
    margin: 0;
    padding-left: 1.1rem;
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
    color: var(--text-muted);
    font-size: 0.92rem;
  }
  h4 {
    margin: 0.9rem 0 0.4rem;
  }
  .small {
    font-size: 0.85rem;
  }
</style>
