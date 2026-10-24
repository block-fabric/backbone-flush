<script lang="ts">
  // Create an asset: the one wizard behind "Register an asset" (My assets) and "Launch an asset" (Trade), the same steps
  // as flush.markets' (scratch contract "The ONE create wizard"): what it is (its ticker, checked as the chain checks it
  // and asked of the node: is it free?; a display name, a description, a logo, the issuer's data), its supply and
  // decimals, its market (none; a bonding curve with its trade fee, the creator's allocation and the bounds of a buy;
  // straight into a pool: a concentrated pool with ECX at the creator's price, funded by the creator over the full
  // range), then a review and the two steps on the way: a reservation that hides the name, then, once it
  // is deep enough, the registration (registerasset) or the launch (launchtoken), which this app sends by itself
  // (ba/store.svelte.ts: one question of the host for both, RegisterAfterReserve / LaunchAfterReserve). Straight into a
  // pool: once the asset exists, its pool (createpool, concentrated, at the price of the two amounts, nothing put in),
  // then once the pool is in a block, the creator's position over the full range (openposition, prices of 0), each
  // confirmed by the wallet on the user's word (ba/store.svelte.ts PendingFund, ba/FundCard.svelte). A reservation of
  // this wallet that no registration uses can be launched from here too.
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import NoWallet from '../../components/NoWallet.svelte';
  import PendingCard from '../../components/PendingCard.svelte';
  import { blockSeconds } from '../../lib/pending.svelte';
  import { ask, errorText, inform, prepareConfirm, walletRpc, walletRpcTwoSteps } from '../../lib/dialogs.svelte';
  import { t, tn } from '../../lib/i18n.svelte';
  import { formatCount, niceTimeOffset } from '../../lib/format';
  import { go, PAGES } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { app, node, toast, wallets, walletsEnabled } from '../../lib/state.svelte';
  import './ba.css';
  import AssetBadge from './AssetBadge.svelte';
  import UnitsInput from './UnitsInput.svelte';
  import LogoPicker, { type PickedLogo } from './LogoPicker.svelte';
  import FundCard from './FundCard.svelte';
  import { FEE_TIERS } from './kinds';
  import { compactNumber } from './axis';
  import { bpsOf, FIXED_PARAMS, curveSplit, curveSupplyProblem, feeFromBps, launchParamsOf, launchTokenOptions, MARKET_KINDS, unitsText, type MarketKind } from './create';
  import { launchFeeParts, launchFeeSplit } from './fees';
  import { curveShape, DEFAULT_RULES, launchProblems, rulesOf, type LaunchStats } from './launch';
  import { amountParam, CHN_ID, DATA_FIELDS, dataProblems, dataToSet, displayNameProblem, formatPrice, formatUnits, hashName, MAX_AMOUNT, MAX_DECIMALS, MAX_DISPLAY_NAME_BYTES, MAX_INFO, MAX_TICKER, minChnPayout, minChnText, MIN_TICKER, nameProblem, normalizeDisplayName, smallestAmount, tickerOf, tickerProblem, type DataField } from './model';
  import { ba, fundingOf, noteFunding, notePending, openMarketFor, pendingOf, pendingOpOf, recentlyRegistered, refreshBitAssets, registerParams, startBitAssets, tickersActive, tradeIntent, type PendingIssue } from './store.svelte';
  import Term from '../../components/Term.svelte';
  import { simpleMode } from '../../lib/uimode.svelte';
  import './terms';
  import { TICKER } from '../config';
  import { untrack } from 'svelte';

  let {
    embedded = false,
    market: initialMarket = 'none',
    heading = true,
  }: {
    /** Shown as a tab (My assets, Trade), under its heading. */
    embedded?: boolean;
    /** The market chosen at first: "Register an asset" none, "Launch" a bonding curve. */
    market?: MarketKind;
    heading?: boolean;
  } = $props();

  $effect(() => startBitAssets());

  const simple = $derived(simpleMode());
  let moreSupply = $state(false);
  const noWallet = $derived(!walletsEnabled() || wallets.current === null);

  // --- The rules of launches (getlaunchstats): whether a curve is open, and its parameters ---
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
  const params = $derived(launchParamsOf(stats?.rules));
  const launchesOpen = $derived(stats?.rules?.active === true);

  // svelte-ignore state_referenced_locally
  let market = $state<MarketKind>(initialMarket);
  const curve = $derived(market === 'curve');

  type Step = 'identity' | 'supply' | 'market' | 'review' | 'done';
  const STEPS: { id: Step; label: string }[] = [
    { id: 'identity', label: 'What it is' },
    { id: 'supply', label: 'Supply' },
    { id: 'market', label: 'Market' },
    { id: 'review', label: 'Review' },
  ];
  const order: Step[] = ['identity', 'supply', 'market', 'review'];
  let step = $state<Step>('identity');

  // --- What it is: from ticker_height a ticker (2 to 12 of A-Z, 0-9, uppercased as typed), and a display name ---
  const tickers = $derived(tickersActive());
  let name = $state('');
  let availability = $state<'' | 'checking' | 'free' | 'taken' | 'mine'>('');
  const problem = $derived(tickers ? tickerProblem(name) || nameProblem(name) : nameProblem(name));
  /** The reservations of this wallet that no registration of this app uses: a launch can use them. */
  const freeReservations = $derived((ba.mine?.reservations ?? []).filter((r) => r.name && !r.taken && !pendingOf(wallets.current).some((p) => p.name === r.name)));
  const reservation = $derived(curve ? freeReservations.find((r) => r.name === name) : undefined);
  const reservationReady = $derived(!!reservation && (reservation.wait ?? 0) === 0);
  function typed(input: HTMLInputElement) {
    const text = input.value;
    name = tickers ? tickerOf(text) : text;
    if (input.value !== name) {
      const at = input.selectionStart;
      input.value = name;
      if (at !== null) input.setSelectionRange(at, at);
    }
  }
  let displayName = $state('');
  const display = $derived(normalizeDisplayName(displayName));
  const displayProblem = $derived(tickers ? displayNameProblem(display) : '');
  const displayBytes = $derived(new TextEncoder().encode(display).length);
  let check = 0;
  $effect(() => {
    const text = name;
    const mine = ++check;
    availability = '';
    if (!text || nameProblem(text) || reservation) return;
    if (pendingOf(wallets.current).some((p) => p.name === text) || ba.mine?.reservations.some((r) => r.name === text)) {
      availability = 'mine';
      return;
    }
    const timer = setTimeout(async () => {
      availability = 'checking';
      try {
        await rpc.raw('getasset', [text]);
        if (mine === check) availability = 'taken';
      } catch {
        // No such asset: the name is free (a reservation of someone else is secret, and may take it first).
        if (mine === check) availability = 'free';
      }
    }, 350);
    return () => clearTimeout(timer);
  });
  let previewId = $state('1'.repeat(64));
  $effect(() => {
    const text = name;
    if (!text) return;
    hashName(text)
      .then((h) => {
        if (text === name) previewId = h;
      })
      .catch(() => undefined);
  });
  let fields = $state<Record<DataField, string>>({ info: '', commitment: '', ipv4: '', ipv6: '', encryptionkey: '', signingkey: '' });
  let advanced = $state(false);
  let published = $state(true);
  const dataIssues = $derived(dataProblems(curve ? { info: fields.info } : fields));
  let logo = $state<PickedLogo | null>(null);
  const logoHeight = $derived(ba.info?.logo_height ?? -1);
  const logosSoon = $derived(logoHeight >= 0 && logoHeight <= Math.max(ba.height, 0) + (ba.info?.reveal_depth ?? 1));
  const infoBytes = $derived(new TextEncoder().encode(fields.info).length);
  const maxInfo = MAX_INFO;
  const FIELD_NAMES: Record<DataField, string> = { info: 'Description', commitment: 'Commitment', ipv4: 'IPv4 address', ipv6: 'IPv6 address', encryptionkey: 'Encryption key', signingkey: 'Signing key' };
  const FIELD_HINTS: Record<DataField, string> = {
    info: 'What it is: a description, a link',
    commitment: 'A hash (64 hex digits) of documents kept elsewhere: terms, a prospectus',
    ipv4: 'An IPv4 address of the issuer, such as 203.0.113.7:8333',
    ipv6: 'An IPv6 address of the issuer, such as [2001:db8::7]:8333',
    encryptionkey: 'A public key (66 hex digits) to encrypt messages to the issuer to',
    signingkey: 'An x-only public key (64 hex digits) the issuer signs with',
  };

  // --- The supply: what the wallet gets (none, pool, auction), or what the curve and its pool hold (a curve) ---
  // 8 by default (6 for a curve: the rules'), the same as flush.markets' wizard.
  let decimals = $state(8);
  let supplyText = $state('');
  let supply = $state<bigint | null>(0n);
  // A curve before the launch parameters: the rules' billion of 6 decimals (no field: nothing typed).
  const fixedCurve = $derived(curve && !params.active);
  const supplyValue = $derived(fixedCurve ? FIXED_PARAMS.minUnits : supplyText.trim() === '' ? (curve ? null : 0n) : supply);
  const largest = $derived(formatUnits(MAX_AMOUNT, decimals, { max: decimals }));
  // A curve before the launch parameters: a billion of 6 decimals, fixed. From them: the rules' default at first.
  let curveTouched = false;
  $effect(() => {
    if (!curve) return;
    if (!params.active) {
      decimals = params.decimalsDefault;
      supplyText = params.supplyDefault;
    } else if (!curveTouched) {
      curveTouched = true;
      decimals = params.decimalsDefault;
      const typedNow = untrack(() => supplyText.trim());
      if (!typedNow || typedNow === '0') supplyText = params.supplyDefault;
    }
  });
  const curveProblem = $derived(curve ? curveSupplyProblem(supplyValue, decimals, params) : '');

  // --- The market ---
  let feeBps = $state(115);
  let feeTouched = false;
  $effect(() => {
    // The rules' default until the user moves it.
    const d = bpsOf(params.feeDefault);
    if (!untrack(() => feeTouched)) feeBps = d;
  });
  const feePercent = $derived(params.active ? feeFromBps(feeBps) : params.feeDefault);
  let firstBuy = $state<bigint | null>(null);
  let firstBuyText = $state('');
  let minBuy = $state<bigint | null>(null);
  let minBuyText = $state('');
  let maxBuy = $state<bigint | null>(null);
  let maxBuyText = $state('');
  let allocation = $state(0);
  const launchIssues = $derived(curve ? launchProblems({ allocation, maxAllocation: rules.maxAllocation, firstBuy: firstBuyText.trim() ? firstBuy : 0n, minBuy: minBuyText.trim() ? minBuy : 0n, maxBuy: maxBuyText.trim() ? maxBuy : 0n, infoBytes, maxInfo: MAX_INFO, minChn: minChnPayout() }) : []);
  const wholeSupply = $derived(supplyValue !== null ? Number(supplyValue) / 10 ** decimals : 0);
  const shape = $derived(curveShape(rules.graduation, wholeSupply || 1));
  const split = $derived(curveSplit(supplyValue ?? 0n));
  const allocationUnits = $derived(((supplyValue ?? 0n) * BigInt(Math.round(allocation * 100))) / 10_000n);
  const feeParts = $derived({ ...launchFeeParts(stats?.rules ?? DEFAULT_RULES), fee: feePercent });
  const chnText = (units: bigint | null, text: string) => (text.trim() && units !== null ? amountParam(units, 8) : '');

  // --- Straight into a pool: the tokens and the ECX that go in (their ratio its price), its fee tier ---
  const pooled = $derived(market === 'pool');
  /** Whether concentrated pools apply (getdexinfo curves_active): else this way is off. */
  const poolsOpen = $derived(ba.dex?.curves_active === true);
  const tiers = $derived(ba.dex?.fee_tiers?.length ? ba.dex.fee_tiers.map(Number) : FEE_TIERS);
  let poolFee = $state(0.3);
  let poolTokens = $state<bigint | null>(null);
  let poolTokensText = $state('');
  let poolChn = $state<bigint | null>(null);
  let poolChnText = $state('');
  const chnHeld = $derived(wallets.current !== null ? (wallets.balances[wallets.current]?.trusted ?? null) : null);
  /** The pool's price: 1 of the asset in ECX. */
  const poolPrice = $derived(poolTokens && poolChn ? Number(poolChn) / 1e8 / (Number(poolTokens) / 10 ** decimals) : undefined);
  const poolProblem = $derived.by(() => {
    if (!pooled) return '';
    if (!poolsOpen) return t('Concentrated pools do not apply on this network yet.');
    if (poolTokens === null || poolChn === null) return '';
    if (poolTokens <= 0n || poolChn <= 0n) return t('Give an amount of each: they set the price, and both go in.');
    if (supplyValue !== null && poolTokens > supplyValue) return t('More than the supply: at most all of it goes in.');
    if (poolChn < minChnPayout()) return t('Too little: put in at least %1 %2.', minChnText(), TICKER);
    if (chnHeld !== null && poolChn > chnHeld) return t('More %1 than this wallet holds.', TICKER);
    return '';
  });

  const depth = $derived(ba.info?.reveal_depth ?? 1);
  const spacing = $derived(blockSeconds(node.recent, app.info?.blockSpacing ?? 600));

  const canNext = $derived.by(() => {
    if (step === 'identity') return !!name && !problem && (availability === 'free' || !!reservation) && !displayProblem && Object.keys(dataIssues).length === 0;
    if (step === 'supply') return supplyValue !== null && Number.isInteger(decimals) && decimals >= 0 && decimals <= MAX_DECIMALS && !curveProblem;
    if (step === 'market' && pooled) return poolsOpen && tiers.includes(poolFee) && poolTokens !== null && poolChn !== null && !poolProblem;
    if (step === 'market') return !curve || (launchesOpen && !curveProblem && launchIssues.length === 0 && (!firstBuyText.trim() || firstBuy !== null) && (!minBuyText.trim() || minBuy !== null) && (!maxBuyText.trim() || maxBuy !== null));
    return true;
  });
  function next() {
    if (!canNext) return;
    const i = order.indexOf(step);
    if (i >= 0 && i < order.length - 1) step = order[i + 1];
  }
  function back() {
    const i = order.indexOf(step);
    if (i > 0) step = order[i - 1];
  }
  function pickMarket(k: MarketKind) {
    market = k;
    if (k === 'curve') published = true;
  }
  function useReservation(n: string) {
    market = 'curve';
    name = n;
  }

  $effect(() => {
    if (step === 'review') prepareConfirm();
  });

  /** launchtoken's options, as shown: taken when the user confirms. */
  function launchOptionsNow(): Record<string, string | number> {
    return launchTokenOptions({
      displayName: display,
      logo: logo && logosSoon ? logo.base64 : '',
      info: fields.info,
      allocation,
      firstBuy: chnText(firstBuy, firstBuyText),
      minBuy: chnText(minBuy, minBuyText),
      maxBuy: chnText(maxBuy, maxBuyText),
      params,
      supply: unitsText(supplyValue ?? 0n, decimals),
      decimals,
      feePercent,
    });
  }

  let busy = $state(false);
  let created = $state('');
  let createdFollow = $state<MarketKind>('none');
  async function create() {
    const wallet = wallets.current;
    if (wallet === null || busy || supplyValue === null) return;
    busy = true;
    try {
      if (curve && reservation) {
        // A reservation of this wallet: launched now if deep enough (a question of its own), else once it is.
        const options = launchOptionsNow();
        if (reservationReady) {
          const r = await ask({
            title: t('Launch an asset'),
            message: t('Launch %1 on a bonding curve?', name),
            informative: t('%1 of it, fixed, sold from the curve until it raises %2 %4, then in a locked pool. The fee: %3.', compactNumber(wholeSupply, 3), String(rules.graduation), launchFeeSplit(feeParts, true), TICKER),
            level: allocation > 0 ? 'warning' : 'question',
            choices: [
              { id: 'no', label: 'Cancel' },
              { id: 'yes', label: 'Launch', kind: 'primary' },
            ],
          });
          if (r.choice !== 'yes') return;
          const done = await walletRpc<{ txid: string; token: string; ticker: string; pool: string }>(wallet, 'launchtoken', [name, options]);
          if (!done) return;
          toast('success', t('Launching'), t('%1 is on its curve with the next block.', name));
          void refreshBitAssets();
          tradeIntent.launch = done.token;
          go('launch', 'launches');
          return;
        }
        await notePending({ wallet, name, asset: previewId, supply: unitsText(supplyValue, decimals), decimals, data: {}, public: true, time: Math.floor(Date.now() / 1000), reservation: reservation.txid, launch: options });
        created = name;
        createdFollow = 'curve';
        step = 'done';
        return;
      }
      const issue: PendingIssue = {
        wallet,
        name,
        asset: '',
        supply: curve ? unitsText(supplyValue, decimals) : amountParam(supplyValue, decimals),
        decimals,
        data: curve ? {} : dataToSet(fields),
        public: tickers || curve ? true : published,
        ...(tickers && display ? { display_name: display } : {}),
        time: Math.floor(Date.now() / 1000),
        reservation: '',
        ...(logo && logosSoon ? { logo: logo.base64 } : {}),
        ...(curve ? { launch: launchOptionsNow() } : {}),
        ...(market === 'auction' ? { follow: market } : {}),
      };
      // Straight into a pool: what goes in, taken now (what the review showed).
      const fund = pooled && poolTokens !== null && poolChn !== null ? { amount: amountParam(poolTokens, decimals), quoteAmount: amountParam(poolChn, 8), fee: poolFee } : null;
      // Both steps in one question of the host: the reservation now, and the registration (or the launch) exactly as
      // shown, which the app then sends by itself without asking again (confirm.h, Reveal).
      const second = curve ? { method: 'launchtoken', params: [name, issue.launch] } : { method: 'registerasset', params: registerParams(issue) };
      const done = await walletRpcTwoSteps<{ txid: string; asset: string }>(wallet, 'reserveasset', [name], second);
      if (!done) return;
      const saved = (await notePending({ ...issue, asset: done.asset, reservation: done.txid })) && (!fund || (await noteFunding({ wallet, name, asset: done.asset, quote: CHN_ID, ...fund, slippage: 1, time: Math.floor(Date.now() / 1000) })));
      if (!saved) {
        await inform(t('The journal of the assets could not be saved'), t('%1 is reserved. If this app restarts before it is registered, register it in Portfolio, with its supply and data.', name), 'warning');
      }
      created = name;
      createdFollow = market;
      step = 'done';
      toast('success', t('Name reserved'), curve ? tn('%1 is launched by itself once the reservation is %n block(s) deep, without asking again.', depth, name) : tn('%1 is registered by itself once the reservation is %n block(s) deep, without asking again.', depth, name));
      void refreshBitAssets();
    } catch (e) {
      await inform(t('Create an asset'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }

  const createdOp = $derived.by(() => {
    if (!created || wallets.current === null) return null;
    const p = pendingOf(wallets.current).find((x) => x.name === created);
    if (p) return pendingOpOf(p);
    const d = recentlyRegistered(wallets.current).find((x) => x.name === created);
    return d ? { key: d.name, what: t('Asset'), name: d.name, wallet: d.wallet, stage: 'done' as const, depth: null, needed: 0, ended: d.ended } : null;
  });
  /** Its pool being made and funded (straight into a pool). */
  const createdFund = $derived(created && createdFollow === 'pool' ? fundingOf(wallets.current).find((f) => f.name === created) : undefined);
  /** The asset made, once it is on the chain: its market can be opened. */
  const createdAsset = $derived(created ? ba.assets.find((a) => a.label === created) : undefined);

  function again() {
    name = '';
    displayName = '';
    supplyText = '';
    decimals = 8;
    curveTouched = false;
    fields = { info: '', commitment: '', ipv4: '', ipv6: '', encryptionkey: '', signingkey: '' };
    published = true;
    advanced = false;
    logo = null;
    created = '';
    allocation = 0;
    firstBuyText = minBuyText = maxBuyText = '';
    poolTokensText = poolChnText = '';
    market = initialMarket;
    step = 'identity';
  }
  const actionLabel = $derived(curve ? (reservation ? (reservationReady ? 'Launch' : 'Launch when ready') : 'Reserve and launch') : simple ? 'Create the asset' : 'Reserve and issue');
</script>

{#if noWallet}
  <NoWallet page={PAGES.issue} />
{:else}
  <div class="page ba-page issue create-wizard" class:embedded>
    {#if heading && !embedded}<header class="page-head"><h1>Create an asset</h1></header>{/if}
    {#if simple}
      <p class="ba-help lead">An <Term id="asset">asset</Term> of your own: points, tickets, shares. Give it a name and an amount; you can send it, swap it, and make more later with its <Term id="controlcoin">control coin</Term>.</p>
    {:else}
      <p class="ba-help lead">An asset of your own: shares, points, tickets, a stablecoin. Say what it is, its supply, and its market: none yet, a bonding curve, a pool or an auction.</p>
    {/if}

    {#if step !== 'done'}
      <ol class="ba-steps" aria-label="Steps">
        {#each STEPS as s, i (s.id)}
          <li class:done={order.indexOf(step) > i} class:now={step === s.id}>{t(s.label)}</li>
        {/each}
      </ol>
    {/if}

    <div class="ba-split">
      <section class="panel wizard">
        {#if step === 'identity'}
          <form class="ba-form" onsubmit={(e) => { e.preventDefault(); next(); }}>
            <div class="field">
              <label for="issue-name">{tickers ? 'Ticker' : 'Its name'}</label>
              <input id="issue-name" class="big" class:ticker={tickers} type="text" maxlength={tickers ? MAX_TICKER : 64} placeholder="Such as GOLD" autocomplete="off" autocapitalize="characters" spellcheck="false" value={name} oninput={(e) => typed(e.currentTarget)} aria-invalid={problem ? 'true' : undefined} aria-describedby="issue-name-hint" />
              {#if problem}<span class="error">{t(problem)}</span>
              {:else if reservation}<span class="hint ba-good"><Icon name="check" size={14} /> Reserved by this wallet{#if !reservationReady}: ready to launch in {tn('%n block(s)', reservation.wait ?? 0)}{/if}</span>
              {:else if availability === 'checking'}<span class="hint">Checking…</span>
              {:else if availability === 'free'}<span class="hint ba-good"><Icon name="check" size={14} /> <SafeText text={name} /> is free</span>
              {:else if availability === 'taken'}<span class="error"><SafeText text={name} /> is taken</span>
              {:else if availability === 'mine'}<span class="error">This wallet is creating <SafeText text={name} /> already: see Portfolio.</span>
              {:else if tickers}<span class="hint" id="issue-name-hint"><span translate="no">{MIN_TICKER}–{MAX_TICKER}</span> letters A–Z and digits 0–9, such as GOLD or BTC2; unique, first come first served.</span>
              {:else}<span class="hint" id="issue-name-hint">1 to 64 plain characters (ASCII), no space at either end; not {TICKER}, not "0x…", not like a number such as 1739-0029.</span>{/if}
            </div>
            {#if curve && freeReservations.length}
              <div class="field">
                <span class="label">Your reservations</span>
                <ul class="ba-list">
                  {#each freeReservations as r (r.txid)}
                    <li><button type="button" class="ba-row" onclick={() => useReservation(r.name ?? '')}><span class="main"><SafeText text={r.name ?? ''} /></span><span class="side faint small">{(r.wait ?? 0) > 0 ? tn('ready in %n block(s)', r.wait ?? 0) : t('ready')}</span></button></li>
                  {/each}
                </ul>
              </div>
            {/if}
            {#if tickers}
              <div class="field">
                <label for="issue-display">Display name <span class="faint">(optional)</span></label>
                <input id="issue-display" type="text" placeholder="Such as Gold Bar Token, 1 g" autocomplete="off" bind:value={displayName} aria-invalid={displayProblem ? 'true' : undefined} />
                {#if displayProblem}<span class="error">{t(displayProblem)}</span>
                {:else}<span class="hint"><span translate="no">{displayBytes} / {MAX_DISPLAY_NAME_BYTES}</span> bytes. Shown after the ticker; not unique: other assets may have the same.</span>{/if}
              </div>
            {/if}
            <div class="field">
              <label for="issue-info">Description and links</label>
              <textarea id="issue-info" rows="3" placeholder="What it is: a description, a website (optional)" bind:value={fields.info} aria-invalid={dataIssues.info ? 'true' : undefined}></textarea>
              {#if dataIssues.info}<span class="error">{t(dataIssues.info)}</span>{:else}<span class="hint">{infoBytes} / {maxInfo} bytes</span>{/if}
            </div>
            <div class="field">
              <label for="issue-logo-file">Logo</label>
              {#if logosSoon}
                <LogoPicker id="issue-logo" bind:logo />
              {:else if logoHeight >= 0}
                <span class="hint">Logos are on the chain from block {logoHeight}.</span>
              {:else}
                <span class="hint">This network has no logos of assets yet.</span>
              {/if}
            </div>
            {#if !tickers && !curve && (!simple || advanced)}
              <label class="check"><input type="checkbox" bind:checked={published} /> <span>Publish the name, so that it is listed and found by name</span></label>
              {#if !published}<p class="ba-help">A private name: only its hash and number are on the chain. Whoever knows the name finds it; the list of assets shows its hash.</p>{/if}
            {/if}
            {#if !curve}
              <button type="button" class="btn ghost small more" aria-expanded={advanced} onclick={() => (advanced = !advanced)}><Icon name={advanced ? 'chevron' : 'chevron-right'} size={14} /> {simple ? "More options: the issuer's commitment, addresses and keys" : "The issuer's commitment, addresses and keys"}</button>
              {#if advanced}
                {#each DATA_FIELDS.filter((f) => f !== 'info') as f (f)}
                  <div class="field">
                    <label for="issue-{f}">{t(FIELD_NAMES[f])}</label>
                    <input id="issue-{f}" type="text" class="mono" spellcheck="false" autocomplete="off" bind:value={fields[f]} aria-invalid={dataIssues[f] ? 'true' : undefined} />
                    {#if dataIssues[f]}<span class="error">{t(dataIssues[f] ?? '')}</span>{:else}<span class="hint">{t(FIELD_HINTS[f])}</span>{/if}
                  </div>
                {/each}
              {/if}
            {/if}
            <p class="ba-help">{tickers ? 'The asset is known by its ticker (the hash of it).' : 'The asset is known by the hash of its name.'} Nobody can take a name between your two steps: the reservation hides it.</p>
            <div class="row"><button type="submit" class="btn primary" disabled={!canNext}>Next</button></div>
          </form>
        {:else if step === 'supply'}
          <form class="ba-form" onsubmit={(e) => { e.preventDefault(); next(); }}>
            {#if fixedCurve}
              <dl class="ba-lines" data-testid="curve-fixed-supply">
                <dt>Supply</dt><dd>{formatUnits(1_000_000_000_000_000n, 6)}, fixed</dd>
                <dt>Decimals</dt><dd>6</dd>
              </dl>
              <p class="ba-help">{params.height >= 0 ? t('A launch chooses its own supply and decimals from block %1 (the launch parameters); until then every launch has a billion of 6 decimals.', formatCount(params.height)) : t('On this network every launch has a billion of 6 decimals, fixed.')}</p>
            {:else}
              {#if simple && !curve}
                <button type="button" class="btn ghost small more" aria-expanded={moreSupply} onclick={() => (moreSupply = !moreSupply)}><Icon name={moreSupply ? 'chevron' : 'chevron-right'} size={14} /> More options: decimals ({decimals})</button>
              {/if}
              {#if !simple || moreSupply || curve}
                <div class="field">
                  <label for="issue-decimals">Decimals: {decimals}</label>
                  <input id="issue-decimals" type="range" min="0" max={curve ? params.decimalsMax : MAX_DECIMALS} step="1" bind:value={decimals} />
                  <span class="hint">They only change how amounts are shown: with {decimals}, the smallest amount is {smallestAmount(decimals)}. They are set for good.</span>
                </div>
              {/if}
              <UnitsInput id="issue-supply" label={curve ? 'Total supply' : simple ? 'How many to create' : 'Initial supply'} {decimals} allowZero={!curve} bind:units={supply} bind:text={supplyText} placeholder="0" hint={curve ? 'Fixed for good: 3/4 sold on the curve, 1/4 into its locked pool at graduation.' : simple ? 'You can make more later.' : '0: none yet; the control coin mints more later'} />
              {#if curveProblem}<p class="ba-problems" role="status" data-testid="curve-supply-problem">{t(curveProblem)}</p>{/if}
              {#if !curve}<p class="ba-help">At most {largest} with {decimals} decimals.</p>{/if}
            {/if}
            <div class="row"><button type="button" class="btn" onclick={back}>Back</button><button type="submit" class="btn primary" disabled={!canNext}>Next</button></div>
          </form>
        {:else if step === 'market'}
          <form class="ba-form" onsubmit={(e) => { e.preventDefault(); next(); }}>
            <div class="markets" role="radiogroup" aria-label="Market">
              {#each MARKET_KINDS as k (k.id)}
                {@const off = (k.id === 'curve' && !launchesOpen) || (k.id === 'pool' && !poolsOpen)}
                <button type="button" role="radio" class="panel market" aria-checked={market === k.id} disabled={off} onclick={() => pickMarket(k.id)}>
                  <strong>{t(k.label)}</strong>
                  <span class="faint small">{off ? (k.id === 'pool' ? t('Concentrated pools do not apply on this network yet.') : t('Launches do not apply on this network yet.')) : t(k.summary)}</span>
                </button>
              {/each}
            </div>
            {#if curve}
              {#if curveProblem}<p class="ba-problems" role="status">{t(curveProblem)} <button type="button" class="btn small ghost" onclick={() => (step = 'supply')}>Change the supply</button></p>{/if}
              <div class="field">
                <label for="launch-fee">Trade fee on the curve: {feePercent.toFixed(2)}%</label>
                {#if params.active}
                  <input id="launch-fee" type="range" min={bpsOf(params.feeMin)} max={bpsOf(params.feeMax)} step="5" bind:value={feeBps} oninput={() => (feeTouched = true)} />
                  <span class="hint">{params.feeMin.toFixed(2)}% to {params.feeMax.toFixed(2)}%: {launchFeeSplit(feeParts, true)}.</span>
                {:else}
                  <span class="hint">Set by the rules until the launch parameters apply: {launchFeeSplit(feeParts, true)}.</span>
                {/if}
              </div>
              <div class="field allocation">
                <label for="launch-allocation">Creator allocation: {allocation}%</label>
                <input id="launch-allocation" type="range" min="0" max={rules.maxAllocation} step="0.1" bind:value={allocation} />
                {#if allocation > 0}
                  <p class="ba-problems" role="status"><strong>Dilution.</strong> {allocation}% of the supply ({formatUnits(allocationUnits, decimals, { max: 2 })}) is minted to you at graduation, on top of what the curve and the pool hold. Every page of the launch shows it.</p>
                {:else}
                  <span class="hint">None: all of it is the curve's and the pool's. At most {rules.maxAllocation}%.</span>
                {/if}
              </div>
              <UnitsInput id="launch-first" label="First buy (optional)" decimals={8} bind:units={firstBuy} bind:text={firstBuyText} unit={TICKER} allowZero hint="Bought in the launch's own block, at that block's price, like every buy of it" />
              <UnitsInput id="launch-min" label="Least a buy pays (optional)" decimals={8} bind:units={minBuy} bind:text={minBuyText} unit={TICKER} allowZero hint="A smaller buy is refunded: no dust orders" />
              <UnitsInput id="launch-max" label="Most a buy pays (optional)" decimals={8} bind:units={maxBuy} bind:text={maxBuyText} unit={TICKER} allowZero hint="A larger buy is refunded: no one buys the curve in one go" />
              {#each launchIssues as p (p)}<p class="ba-problems" role="status">{t(p)}</p>{/each}
            {:else if market === 'pool'}
              <UnitsInput id="fund-tokens" label="Into the pool: the asset" {decimals} bind:units={poolTokens} bind:text={poolTokensText} max={supplyValue} unit={name} hint="Of your supply; the rest stays in your wallet" />
              <UnitsInput id="fund-chn" label={t('Into the pool: %1', TICKER)} decimals={8} bind:units={poolChn} bind:text={poolChnText} max={chnHeld} unit={TICKER} hint="Your own coins, beside the asset" />
              <div class="field">
                <span class="label">Fee tier</span>
                <div class="ba-chips" role="group" aria-label="Fee tier">
                  {#each tiers as x (x)}<button type="button" class="ba-chip" aria-pressed={poolFee === x} onclick={() => (poolFee = x)}>{x}%</button>{/each}
                </div>
              </div>
              {#if poolPrice !== undefined}<p class="ba-help" data-testid="fund-price">Its price: 1 <SafeText text={name} /> = {formatPrice(poolPrice)} {TICKER}, the ratio of the two. Both go in over the full range: your liquidity at every price.</p>{/if}
              {#if poolProblem}<p class="ba-problems" role="status">{poolProblem}</p>{/if}
            {:else if market === 'auction'}
              <p class="ba-help">First the asset is registered, with its supply to your wallet. Once it is in your assets, this app offers to sell some of it by Dutch auction: a price that falls block by block from where you start it.</p>
            {:else}
              <p class="ba-help">The supply goes to your wallet. Open a market any time later from its page: a curve, a pool, an order book or an auction.</p>
            {/if}
            <div class="row"><button type="button" class="btn" onclick={back}>Back</button><button type="submit" class="btn primary" disabled={!canNext}>Next</button></div>
          </form>
        {:else if step === 'review'}
          <div class="ba-form">
            <h2>Review</h2>
            <dl class="ba-lines" data-testid={curve ? 'launch-review' : 'issue-review'}>
              <dt>{tickers ? 'Ticker' : 'Name'}</dt><dd><SafeText class="ticker-text" text={name} /></dd>
              {#if tickers}<dt>Display name</dt><dd>{#if display}<SafeText text={display} />{:else}<span class="faint">None</span>{/if}</dd>{/if}
              {#if curve}
                <dt>Supply</dt><dd>{formatUnits(supplyValue ?? 0n, decimals)}, {decimals} decimals, fixed</dd>
                <dt>On the curve</dt><dd>{formatUnits(split.curve, decimals, { max: 2 })} (75%)</dd>
                <dt>Into the pool</dt><dd>{formatUnits(split.pool, decimals, { max: 2 })} (25%), with the {TICKER} raised, locked for good</dd>
                {#if allocation > 0}<dt>Your allocation</dt><dd class="ba-warn">{compactNumber(Number(allocationUnits) / 10 ** decimals, 3)} ({allocation}%), minted at graduation: dilution</dd>{/if}
                <dt>Graduation</dt><dd>at {rules.graduation} {TICKER} raised</dd>
                <dt>Price</dt><dd>from {formatPrice(shape.startPrice)} to {formatPrice(shape.endPrice)} {TICKER} each (9 times)</dd>
                <dt>Market value</dt><dd>from {formatPrice(shape.startCap)} to {formatPrice(shape.endCap)} {TICKER}</dd>
                <dt>Fee</dt><dd data-testid="launch-fee-split">{launchFeeSplit(feeParts, true)}</dd>
                {#if firstBuyText.trim() && firstBuy}<dt>First buy</dt><dd>{formatUnits(firstBuy, 8)} {TICKER}</dd>{/if}
                {#if minBuyText.trim() && minBuy}<dt>Least a buy</dt><dd>{formatUnits(minBuy, 8)} {TICKER}</dd>{/if}
                {#if maxBuyText.trim() && maxBuy}<dt>Most a buy</dt><dd>{formatUnits(maxBuy, 8)} {TICKER}</dd>{/if}
              {:else}
                <dt>Initial supply</dt><dd>{formatUnits(supplyValue ?? 0n, decimals)}</dd>
                <dt>Decimals</dt><dd>{decimals}</dd>
                <dt>Market</dt><dd>{t(MARKET_KINDS.find((k) => k.id === market)?.label ?? '')}</dd>
                {#if pooled && poolTokens !== null && poolChn !== null}
                  <dt>Into the pool</dt><dd data-testid="fund-review">{formatUnits(poolTokens, decimals)} <SafeText text={name} /> and {formatUnits(poolChn, 8)} {TICKER}, over the full range</dd>
                  <dt>Its price</dt><dd>1 <SafeText text={name} /> = {formatPrice(poolPrice)} {TICKER} · fee {poolFee}%</dd>
                {/if}
                {#if !tickers}<dt>The name</dt><dd>{published ? 'Published' : 'Private: only its hash'}</dd>{/if}
              {/if}
              {#if logo && logosSoon}<dt>Logo</dt><dd class="logo-line"><img class="logo-review" src={logo.url} alt="" width="64" height="64" /> {logo.kind}, {(logo.size / 1024).toFixed(1)} KB</dd>{/if}
              {#each DATA_FIELDS.filter((f) => fields[f].trim() && (!curve || f === 'info')) as f (f)}
                <dt>{t(FIELD_NAMES[f])}</dt><dd><SafeText class="ba-mono" text={fields[f]} /></dd>
              {/each}
            </dl>
            <h3>On the way</h3>
            <ol class="timeline">
              {#if !reservation}<li><strong>Now: the reservation.</strong> It hides the name, so that nobody can see it coming and take it first.</li>{/if}
              <li><strong>{tn('%n block(s) later (about %1): the registration.', depth, niceTimeOffset(depth * spacing))}</strong> {curve ? 'The launch puts it on its curve, with your first buy if any.' : 'Not sooner, so that whoever makes a block cannot take the name first.'} This app sends it by itself while it runs.</li>
              {#if curve}<li><strong>Then: on its curve</strong>, until it raises {rules.graduation} {TICKER}; then its locked pool.</li>
              {:else}<li><strong>The block after: in your assets</strong>, with its supply and its control coin{#if market === 'auction'}; then start its auction{/if}.</li>{/if}
              {#if pooled}
                <li><strong>Then: its pool</strong>, at your price, nothing in it yet. Your wallet confirms it, from its card or its notice.</li>
                <li><strong>The block after: your liquidity</strong>, the asset and {TICKER}, over the full range. Your wallet confirms it too.</li>
              {/if}
            </ol>
            {#if pooled}
              <p class="ba-help" data-testid="fund-story">Four small transactions, each with the fee of the wallet: the reservation and the registration confirmed together now; the pool and your liquidity each confirmed when it can go, one block apart. A reservation can be released while it is not used.</p>
            {:else}
              <p class="ba-help">Two small transactions, each with the fee of the wallet, confirmed together once. A reservation can be released while it is not used.</p>
            {/if}
            <div class="row"><button type="button" class="btn" onclick={back}>Back</button><button type="button" class="btn primary" disabled={busy || (curve && !launchesOpen)} aria-busy={busy} onclick={create}><Icon name="issue" size={15} /> {busy ? 'Waiting for your confirmation…' : t(actionLabel)}</button></div>
            {#if busy}<p class="ba-help" role="status">Confirm both steps in the window of the app; the reservation is sent once you accept.</p>{/if}
          </div>
        {:else}
          <div class="ba-form done">
            <Icon name="check" size={28} />
            <h2><SafeText text={created} /> is reserved</h2>
            <p class="ba-help">{createdFollow === 'curve' ? tn('It is launched by itself once the reservation is %n block(s) deep. Keep the app running meanwhile, or come back to it.', depth) : tn('It is registered by itself once the reservation is %n block(s) deep, then it is in your assets after the block after. Keep the app running meanwhile, or come back to it.', depth)}</p>
            {#if createdOp}<PendingCard op={createdOp} />{/if}
            {#if createdFund}<FundCard fund={createdFund} />{/if}
            <div class="row">
              {#if createdFollow === 'auction' && createdAsset}
                <button type="button" class="btn primary" onclick={() => openMarketFor(createdAsset.asset, 'auction')}>Start its auction</button>
              {/if}
              <button type="button" class="btn" class:primary={!createdAsset || createdFollow !== 'auction'} onclick={() => go('my-assets', 'issuing')}>Follow it</button><button type="button" class="btn" onclick={again}>Create another</button>
            </div>
          </div>
        {/if}
      </section>

      {#if step !== 'done'}
        <aside class="panel preview" aria-label="Preview">
          <span class="muted small">Preview</span>
          <AssetBadge id={previewId} label={name || '?'} display={tickers ? display : ''} preview={logosSoon ? (logo?.url ?? '') : ''} />
          {#if fields.info.trim()}<SafeText class="blurb" text={fields.info} />{:else}<span class="faint small">No description.</span>{/if}
          <dl class="ba-lines">
            <dt>Supply</dt><dd>{supplyValue !== null ? formatUnits(supplyValue, decimals) : '–'}</dd>
            <dt>Smallest amount</dt><dd>{smallestAmount(decimals)}</dd>
            <dt>Control coin</dt><dd>{curve ? 'None: the supply is fixed' : 'Yours'}</dd>
            <dt>Market</dt><dd>{t(MARKET_KINDS.find((k) => k.id === market)?.label ?? '')}{#if curve} · {feePercent.toFixed(2)}%{/if}</dd>
            <dt>Listed</dt><dd>{tickers ? 'By its ticker and display name' : published || curve ? 'By its name' : 'By its hash'}</dd>
          </dl>
          {#if curve && supplyValue !== null && !curveProblem}
            <p class="faint small">Price from {formatPrice(shape.startPrice)} to {formatPrice(shape.endPrice)} {TICKER} each; graduation at {rules.graduation} {TICKER}.</p>
          {/if}
        </aside>
      {/if}
    </div>
  </div>
{/if}

<style>
  .embedded {
    padding: 0;
  }
  .lead {
    max-width: 52rem;
  }
  .wizard {
    padding: 1.2rem;
  }
  .wizard h2,
  .wizard h3 {
    margin: 0;
  }
  .big {
    font-size: 1.3rem;
    padding: 0.55rem 0.7rem;
  }
  .big.ticker {
    font-family: var(--font-mono);
    letter-spacing: 0.06em;
    text-transform: uppercase;
    max-width: 16rem;
  }
  .wizard :global(.ticker-text) {
    font-weight: 700;
    letter-spacing: 0.03em;
  }
  .more {
    align-self: flex-start;
  }
  .markets {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(min(12rem, 100%), 1fr));
    gap: 0.6rem;
  }
  .market {
    display: flex;
    flex-direction: column;
    gap: 0.3rem;
    padding: 0.75rem 0.85rem;
    color: var(--text);
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .market[aria-checked='true'] {
    border-color: var(--accent);
    box-shadow: inset 0 0 0 1px var(--accent);
  }
  .market:disabled {
    cursor: not-allowed;
    opacity: 0.6;
  }
  .timeline {
    margin: 0;
    padding-left: 1.2rem;
    display: flex;
    flex-direction: column;
    gap: 0.45rem;
    font-size: 0.92rem;
  }
  .done {
    align-items: flex-start;
  }
  .preview {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
    padding: 1.1rem;
  }
  .preview :global(.blurb) {
    color: var(--text-muted);
    font-size: 0.9rem;
    overflow-wrap: anywhere;
  }
  .small {
    font-size: 0.85rem;
  }
  .logo-line {
    display: flex;
    align-items: center;
    gap: 0.6rem;
  }
  .logo-review {
    width: 3rem;
    height: 3rem;
    border-radius: 50%;
    border: 1px solid var(--line-strong);
    object-fit: cover;
  }
</style>
