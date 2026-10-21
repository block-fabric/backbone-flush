<script lang="ts">
  // Launching a token on a bonding curve (Layers' launches), step by step: its ticker, reserved first (reserveasset: the
  // reservation hides it until it is deep enough, so that nobody takes a ticker they saw in the mempool); while it gets
  // deep enough, what it is (a display name, a logo, a description and links); its options (a first buy in its own block,
  // the least and the most a buy pays, and an allocation to the creator, minted at graduation on top of the supply: a
  // dilution every page shows); then a review (the split of the supply, graduation, the curve's first and last prices,
  // the fees and their split) and the launch (launchtoken), once the reservation is deep enough. Two questions of the
  // host, the reservation now and the launch when it is ready: the host pairs a reservation only with a registration.
  // A reservation of this wallet not used yet (by a registration it is creating) can be launched from here too.
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { t, tn } from '../../lib/i18n.svelte';
  import { formatCount } from '../../lib/format';
  import { nav } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { toast, wallets } from '../../lib/state.svelte';
  import AssetBadge from './AssetBadge.svelte';
  import LogoPicker, { type PickedLogo } from './LogoPicker.svelte';
  import UnitsInput from './UnitsInput.svelte';
  import { compactNumber } from './axis';
  import { curveShape, feeSplit, launchOptions, launchProblems, rulesOf, supplySplit, type LaunchStats } from './launch';
  import { amountParam, displayNameProblem, formatPrice, formatUnits, MAX_INFO, MIN_CHN_PAYOUT, nameProblem, normalizeDisplayName, tickerOf, tickerProblem } from './model';
  import { ba, pendingOf, refreshBitAssets, tickersActive, tradeIntent } from './store.svelte';

  let stats = $state<LaunchStats | null>(null);
  $effect(() => {
    void ba.version;
    rpc
      .raw<LaunchStats>('getlaunchstats', [])
      .then((s) => {
        stats = s && typeof s === 'object' ? s : null;
      })
      .catch(() => {
        stats = null;
      });
  });
  const rules = $derived(rulesOf(stats?.rules));
  const shape = $derived(curveShape(rules.graduation, rules.supply));

  type Step = 'ticker' | 'about' | 'options' | 'review';
  const STEPS: { id: Step; label: string }[] = [
    { id: 'ticker', label: 'Ticker' },
    { id: 'about', label: 'About it' },
    { id: 'options', label: 'Options' },
    { id: 'review', label: 'Review' },
  ];
  const order = STEPS.map((s) => s.id);
  let step = $state<Step>('ticker');

  // --- The ticker, and its reservation ---
  const tickers = $derived(tickersActive());
  let ticker = $state('');
  const problem = $derived(tickers ? tickerProblem(ticker) || nameProblem(ticker) : nameProblem(ticker));
  /** The reservations of this wallet that no registration of this app is using: a launch can use them. */
  const free = $derived((ba.mine?.reservations ?? []).filter((r) => r.name && !r.taken && !pendingOf(wallets.current).some((p) => p.name === r.name)));
  const reservation = $derived(free.find((r) => r.name === ticker));
  const ready = $derived(!!reservation && (reservation.wait ?? 0) === 0);
  let availability = $state<'' | 'checking' | 'free' | 'taken'>('');
  let check = 0;
  $effect(() => {
    const text = ticker;
    const mine = ++check;
    availability = '';
    if (!text || problem || reservation) return;
    const timer = setTimeout(async () => {
      availability = 'checking';
      try {
        await rpc.raw('getasset', [text]);
        if (mine === check) availability = 'taken';
      } catch {
        // No such asset: free (a reservation of someone else is secret, and may take it first).
        if (mine === check) availability = 'free';
      }
    }, 350);
    return () => clearTimeout(timer);
  });
  function typed(input: HTMLInputElement) {
    ticker = tickers ? tickerOf(input.value) : input.value;
    if (input.value !== ticker) input.value = ticker;
  }

  let busy = $state(false);
  async function reserve() {
    const wallet = wallets.current;
    if (wallet === null || busy || !ticker || problem || availability !== 'free') return;
    const sTicker = ticker;
    busy = true;
    try {
      const done = await walletRpc<{ txid: string; asset: string }>(wallet, 'reserveasset', [sTicker]);
      if (!done) return;
      toast('success', t('Ticker reserved'), tn('%1 can be launched once the reservation is %n block(s) deep: meanwhile, say what it is.', ba.info?.reveal_depth ?? 1, sTicker));
      step = 'about';
      void refreshBitAssets({ node: false });
    } catch (e) {
      await inform(t('Reserve a ticker'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }

  // --- About it ---
  let displayName = $state('');
  const display = $derived(normalizeDisplayName(displayName));
  const displayProblem = $derived(displayNameProblem(display));
  let logo = $state<PickedLogo | null>(null);
  let info = $state('');
  const infoBytes = $derived(new TextEncoder().encode(info).length);
  const logos = $derived((ba.info?.logo_height ?? -1) >= 0 && (ba.info?.logo_height ?? 0) <= Math.max(ba.height, 0));

  // --- Options ---
  let firstBuy = $state<bigint | null>(null);
  let firstBuyText = $state('');
  let minBuy = $state<bigint | null>(null);
  let minBuyText = $state('');
  let maxBuy = $state<bigint | null>(null);
  let maxBuyText = $state('');
  let allocation = $state(0);
  const problems = $derived(launchProblems({ allocation, maxAllocation: rules.maxAllocation, firstBuy: firstBuyText.trim() ? firstBuy : 0n, minBuy: minBuyText.trim() ? minBuy : 0n, maxBuy: maxBuyText.trim() ? maxBuy : 0n, infoBytes, maxInfo: MAX_INFO, minChn: MIN_CHN_PAYOUT }));
  const split = $derived(supplySplit(shape, allocation));
  const fees = $derived(feeSplit(rules.fee, rules.creatorShare));
  const maxInfo = MAX_INFO;
  const chnText = (units: bigint | null, text: string) => (text.trim() && units !== null ? amountParam(units, 8) : '');

  const canNext = $derived.by(() => {
    if (step === 'ticker') return !!reservation;
    if (step === 'about') return !displayProblem && infoBytes <= MAX_INFO;
    if (step === 'options') return problems.length === 0 && (!firstBuyText.trim() || firstBuy !== null) && (!minBuyText.trim() || minBuy !== null) && (!maxBuyText.trim() || maxBuy !== null);
    return false;
  });
  function next() {
    if (canNext) step = order[Math.min(order.length - 1, order.indexOf(step) + 1)];
  }
  function back() {
    step = order[Math.max(0, order.indexOf(step) - 1)];
  }
  function use(name: string) {
    ticker = name;
    step = 'about';
  }

  async function launch() {
    const wallet = wallets.current;
    if (wallet === null || busy || !ready || problems.length) return;
    // What the confirmation shows is what is signed: taken now.
    const sTicker = ticker;
    const options = launchOptions({ displayName: display, logo: logo && logos ? logo.base64 : '', info, allocation, firstBuy: chnText(firstBuy, firstBuyText), minBuy: chnText(minBuy, minBuyText), maxBuy: chnText(maxBuy, maxBuyText) });
    const r = await ask({
      title: t('Launch a token'),
      message: t('Launch %1 on a bonding curve?', sTicker),
      informative: t('%1 tokens, fixed, sold from the curve until it raises %2 ECX, then in a locked pool. The fee of each trade is %3%, %4% of it to you. %5', compactNumber(shape.supply, 3), String(rules.graduation), String(rules.fee), String(rules.creatorShare), allocation > 0 ? t('Your allocation of %1% is minted to you at graduation, and shown to everyone.', String(allocation)) : ''),
      level: allocation > 0 ? 'warning' : 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Launch', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      const done = await walletRpc<{ txid: string; token: string; ticker: string; pool: string }>(wallet, 'launchtoken', [sTicker, options]);
      if (!done) return;
      toast('success', t('Launching'), t('%1 is on its curve with the next block.', sTicker));
      void refreshBitAssets();
      tradeIntent.launch = done.token;
      nav.tab = 'launches';
    } catch (e) {
      await inform(t('Launch a token'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }
</script>

{#if stats && !stats.rules.active}
  <section class="panel ba-empty">
    <p>Launches come with the pool kinds{#if ba.dex && ba.dex.curves_height >= 0}, from block {formatCount(ba.dex.curves_height)}{/if}: the next block takes none.</p>
  </section>
{:else if !stats}
  <section class="panel ba-empty"><p>This node says nothing of launches.</p></section>
{:else}
  <ol class="ba-steps" aria-label="Steps">
    {#each STEPS as s, i (s.id)}
      <li class:done={order.indexOf(step) > i} class:now={step === s.id}>{t(s.label)}</li>
    {/each}
  </ol>
  {#if reservation && step !== 'ticker'}
    <p class="ba-help reserved" role="status">{#if ready}<Icon name="check" size={14} /> <SafeText text={ticker} /> is reserved and deep enough: ready to launch.{:else}<SafeText text={ticker} /> is reserved: ready to launch in {tn('%n block(s)', reservation.wait ?? 0)}. Meanwhile, say what it is.{/if}</p>
  {/if}
  <div class="ba-split">
    <section class="panel wizard">
      {#if step === 'ticker'}
        <form class="ba-form" onsubmit={(e) => { e.preventDefault(); if (reservation) next(); else void reserve(); }}>
          <div class="field">
            <label for="launch-ticker">Ticker</label>
            <input id="launch-ticker" class="big" type="text" maxlength="64" placeholder="Such as MOON" autocomplete="off" autocapitalize="characters" spellcheck="false" value={ticker} oninput={(e) => typed(e.currentTarget)} aria-invalid={problem ? 'true' : undefined} />
            {#if problem}<span class="error">{t(problem)}</span>
            {:else if reservation}<span class="hint ba-good"><Icon name="check" size={14} /> Reserved by this wallet</span>
            {:else if availability === 'checking'}<span class="hint">Checking…</span>
            {:else if availability === 'free'}<span class="hint ba-good"><Icon name="check" size={14} /> <SafeText text={ticker} /> is free</span>
            {:else if availability === 'taken'}<span class="error"><SafeText text={ticker} /> is taken</span>
            {:else}<span class="hint">Unique, first come first served. It is reserved first: hidden until the reservation is deep enough, so that nobody takes it from the mempool.</span>{/if}
          </div>
          {#if free.length}
            <div class="field">
              <span class="label">Your reservations</span>
              <ul class="ba-list">
                {#each free as r (r.txid)}
                  <li><button type="button" class="ba-row" onclick={() => use(r.name ?? '')}><span class="main"><SafeText text={r.name ?? ''} /></span><span class="side faint small">{(r.wait ?? 0) > 0 ? tn('ready in %n block(s)', r.wait ?? 0) : t('ready')}</span></button></li>
                {/each}
              </ul>
            </div>
          {/if}
          <div class="row">
            {#if reservation}<button type="submit" class="btn primary">Next</button>
            {:else}<button type="submit" class="btn primary" disabled={busy || wallets.current === null || availability !== 'free'}>{busy ? 'Reserving…' : 'Reserve the ticker'}</button>{/if}
          </div>
        </form>
      {:else if step === 'about'}
        <form class="ba-form" onsubmit={(e) => { e.preventDefault(); next(); }}>
          <div class="field">
            <label for="launch-display">Display name <span class="faint">(optional)</span></label>
            <input id="launch-display" type="text" placeholder="Such as To the moon" autocomplete="off" bind:value={displayName} aria-invalid={displayProblem ? 'true' : undefined} />
            {#if displayProblem}<span class="error">{t(displayProblem)}</span>{:else}<span class="hint">Shown after the ticker; not unique.</span>{/if}
          </div>
          <div class="field">
            <label for="launch-logo-file">Logo</label>
            {#if logos}<LogoPicker id="launch-logo" bind:logo />{:else}<span class="hint">This network has no logos of assets yet.</span>{/if}
          </div>
          <div class="field">
            <label for="launch-info">Description and links</label>
            <textarea id="launch-info" rows="4" placeholder="What it is, a website, a community (optional)" bind:value={info} aria-invalid={infoBytes > MAX_INFO ? 'true' : undefined}></textarea>
            <span class="hint" class:error={infoBytes > MAX_INFO}>{infoBytes} / {maxInfo} bytes</span>
          </div>
          <div class="row"><button type="button" class="btn" onclick={back}>Back</button><button type="submit" class="btn primary" disabled={!canNext}>Next</button></div>
        </form>
      {:else if step === 'options'}
        <form class="ba-form" onsubmit={(e) => { e.preventDefault(); next(); }}>
          <UnitsInput id="launch-first" label="First buy (optional)" decimals={8} bind:units={firstBuy} bind:text={firstBuyText} unit="ECX" allowZero hint="Bought in the launch's own block, at that block's price, like every buy of it" />
          <UnitsInput id="launch-min" label="Least a buy pays (optional)" decimals={8} bind:units={minBuy} bind:text={minBuyText} unit="ECX" allowZero hint="A smaller buy is refunded: no dust orders" />
          <UnitsInput id="launch-max" label="Most a buy pays (optional)" decimals={8} bind:units={maxBuy} bind:text={maxBuyText} unit="ECX" allowZero hint="A larger buy is refunded: no one buys the curve in one go" />
          <div class="field allocation">
            <label for="launch-allocation">Creator allocation: {allocation}%</label>
            <input id="launch-allocation" type="range" min="0" max={rules.maxAllocation} step="0.1" bind:value={allocation} />
            {#if allocation > 0}
              <p class="ba-problems" role="status"><strong>Dilution.</strong> {allocation}% of the supply ({compactNumber(split.allocation, 3)} tokens) is minted to you at graduation, on top of the billion the curve and the pool hold: {split.dilution.toFixed(2)}% of all there are then. Every page of the launch shows it.</p>
            {:else}
              <span class="hint">None: the launch's tokens are all the curve's and the pool's. At most {rules.maxAllocation}%.</span>
            {/if}
          </div>
          {#each problems as p (p)}<p class="ba-problems" role="status">{t(p)}</p>{/each}
          <div class="row"><button type="button" class="btn" onclick={back}>Back</button><button type="submit" class="btn primary" disabled={!canNext}>Next</button></div>
        </form>
      {:else}
        <form class="ba-form" onsubmit={(e) => { e.preventDefault(); void launch(); }}>
          <h2>Review</h2>
          <div class="who"><AssetBadge id={'f'.repeat(64)} label={ticker} display={display} preview={logo?.url ?? ''} size="l" /></div>
          {#if info}<p class="info"><SafeText text={info} /></p>{/if}
          <dl class="ba-lines" data-testid="launch-review">
            <dt>Supply</dt><dd>{compactNumber(shape.supply, 3)} tokens, 6 decimals, fixed</dd>
            <dt>On the curve</dt><dd>{compactNumber(split.curve, 3)} ({formatPrice((split.curve / shape.supply) * 100, 3)}%)</dd>
            <dt>Into the pool</dt><dd>{compactNumber(split.pool, 3)} ({formatPrice((split.pool / shape.supply) * 100, 3)}%), with the ECX raised, locked for good</dd>
            {#if allocation > 0}<dt>Your allocation</dt><dd class="ba-warn">{compactNumber(split.allocation, 3)} ({allocation}%), minted at graduation: dilution</dd>{/if}
            <dt>Graduation</dt><dd>at {rules.graduation} ECX raised</dd>
            <dt>Price</dt><dd>from {formatPrice(shape.startPrice)} to {formatPrice(shape.endPrice)} ECX a token (9 times)</dd>
            <dt>Market value</dt><dd>from {formatPrice(shape.startCap)} to {formatPrice(shape.endCap)} ECX</dd>
            <dt>Fee</dt><dd>{rules.fee}% of each trade on the curve: {formatPrice(fees.creator)}% to you, {formatPrice(fees.pool)}% into the pool at graduation</dd>
            {#if firstBuyText.trim() && firstBuy}<dt>First buy</dt><dd>{formatUnits(firstBuy, 8)} ECX</dd>{/if}
            {#if minBuyText.trim() && minBuy}<dt>Least a buy</dt><dd>{formatUnits(minBuy, 8)} ECX</dd>{/if}
            {#if maxBuyText.trim() && maxBuy}<dt>Most a buy</dt><dd>{formatUnits(maxBuy, 8)} ECX</dd>{/if}
          </dl>
          {#if !ready}<p class="ba-help">The reservation is not deep enough yet: ready in {tn('%n block(s)', reservation?.wait ?? 0)}.</p>{/if}
          <div class="row"><button type="button" class="btn" onclick={back}>Back</button><button type="submit" class="btn primary" disabled={busy || wallets.current === null || !ready || problems.length > 0}><Icon name="issue" size={16} /> {busy ? 'Launching…' : 'Launch'}</button></div>
        </form>
      {/if}
    </section>

    <section class="panel">
      <div class="panel-head"><h3>How a launch works</h3></div>
      <div class="panel-body">
        <ul class="how">
          <li>A billion tokens, fixed. None goes to you at launch: the curve sells {compactNumber(shape.curveSupply, 3)} of them for ECX, its price rising 9 times, and buys them back at its price.</li>
          <li>When it has raised {rules.graduation} ECX it graduates: that ECX and the last {compactNumber(shape.poolSupply, 3)} tokens go into the token's pool with ECX, whose shares nobody holds. That liquidity is locked for good.</li>
          <li>Each trade on the curve pays {rules.fee}%: {formatPrice(fees.creator)}% to you, the rest into the pool at graduation.</li>
          <li>Every buy of a block gets one price: being first in a block gains nothing.</li>
        </ul>
      </div>
    </section>
  </div>
{/if}

<style>
  .wizard {
    padding: 1.2rem;
  }
  .reserved {
    margin: 0;
  }
  .who {
    margin-bottom: 0.4rem;
  }
  .info {
    white-space: pre-wrap;
    overflow-wrap: anywhere;
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
  .small {
    font-size: 0.85rem;
  }
</style>
