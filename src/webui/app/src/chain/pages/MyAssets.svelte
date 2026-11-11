<script lang="ts">
  // Portfolio (owner decision, 2026-11-11): what this wallet holds (each row opens the asset's page, where it is traded
  // and managed), its launches with the creator fees to claim (ba/MyLaunches.svelte), its pool shares with the fees to
  // collect (a row opens the pool's page), what it is creating (the two steps of an asset, followed until it is listed;
  // a pool the wizard funds), and its asset activity. And "Closing old markets", shown only if the wallet has any: its
  // open order-book orders (cancel, collect: ba/MyOrders.svelte), its auctions (collect, or cancel while nobody bid) and
  // its positions of a price range (collect, remove, close: ba/Positions.svelte). Those markets are retired from the
  // interface: what a wallet has in them can be closed here, and nothing new of their kind is started anywhere.
  // One level of tabs: Portfolio and Activity. The tabs of before (holdings, issuing, liquidity, auctions) lead to
  // Portfolio; "closing" to its Closing old markets.
  import { stacked } from '../../lib/actions';
  import SafeText from '../../components/SafeText.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import Icon from '../../components/Icon.svelte';
  import NoWallet from '../../components/NoWallet.svelte';
  import TabRow from '../../components/TabRow.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { t, tn } from '../../lib/i18n.svelte';
  import { formatCount, formatTime, shortHash } from '../../lib/format';
  import { go, nav, PAGES, takeFocus } from '../../lib/nav.svelte';
  import { toast, wallets, walletsEnabled } from '../../lib/state.svelte';
  import '../ba/ba.css';
  import { barWidth } from '../ba/actions';
  import AssetBadge from '../ba/AssetBadge.svelte';
  import CreateAssetButton from '../ba/CreateAssetButton.svelte';
  import LocalTxCell from '../LocalTxCell.svelte';
  import { assetMovesOf } from '../txinfo.svelte';
  import MyOrders from '../ba/MyOrders.svelte';
  import Positions from '../ba/Positions.svelte';
  import { assetRoute, poolRoute } from '../ba/assetactions';
  import { flipped, positionSides } from '../ba/cl';
  import { marketState } from '../ba/pools';
  import { fullRangePosition, loadPositions, positionPool, positionsRead, sharePercent } from '../ba/positions.svelte';
  import { rpc } from '../../lib/rpc';
  import { selectable } from '../../lib/actions';
  import MyLaunches from '../ba/MyLaunches.svelte';
  import HiddenToggle from '../ba/HiddenToggle.svelte';
  import { withoutHidden } from '../ba/visible.svelte';
  import { chainTxHidden } from '../txinfo.svelte';
  import UnitsInput from '../ba/UnitsInput.svelte';
  import {
    amountParam,
    approx,
    assetArg,
    formatAmount,
    formatPercent,
    formatPrice,
    formatUnits,
    issueStage,
    MAX_DECIMALS,
    AUCTION_STATUS,
    OPERATIONS,
    receiptAction,
    unitsOf,
    withdraw,
    type Amount,
    type Auction,
    type Position,
    type Reservation,
    minChnText,
    assetText,
  } from '../ba/model';
  import { simpleMode } from '../../lib/uimode.svelte';
  import { ACTIVITY_PAGE, auctionById, ba, baView, chnPrice, decimalsOf, displayNameOf, dropPending, fundingOf, MAX_ACTIVITY, nameOf, pendingOf, pendingOpOf, launchById, loadLaunchFlags, refreshBitAssets, registerNow, startBitAssets, type PendingIssue, unlockHoldings } from '../ba/store.svelte';
  import PendingCard from '../../components/PendingCard.svelte';
  import FundCard from '../ba/FundCard.svelte';
  import { TICKER } from '../config';

  $effect(() => startBitAssets());

  type Tab = 'holdings' | 'activity';
  const TABS: { id: Tab; label: string }[] = [
    { id: 'holdings', label: 'Portfolio' },
    { id: 'activity', label: 'Activity' },
  ];
  const outer = $derived(nav.tab.split('/')[0]);
  const tab = $derived<Tab>(outer === 'activity' ? 'activity' : 'holdings');
  // Simple mode (lib/uimode.svelte.ts): what the wallet holds, what it is creating and its activity; its launches and
  // pool shares are Advanced's, shown in Simple mode only when it has some.
  const simple = $derived(simpleMode());

  const noWallet = $derived(!walletsEnabled() || wallets.current === null);
  const mine = $derived(ba.wallet === wallets.current ? ba.mine : null);
  // What the wallet hides (its asset filters: ba/hide.svelte.ts) is left out of its holdings and activity, unless shown.
  let showHidden = $state(false);
  const visible = $derived(withoutHidden(mine?.assets ?? [], (h) => h.asset, (h) => h.label, showHidden));
  const activity = $derived(showHidden ? ba.activity : ba.activity.filter((a) => !chainTxHidden(a.txid)));
  const holdings = $derived(visible.items.slice().sort((a, b) => (chnPrice(b.asset) ?? 0) * approx(b.balance) - (chnPrice(a.asset) ?? 0) * approx(a.balance) || a.label.localeCompare(b.label)));
  // Opened for an asset (an older link): its page.
  $effect(() => {
    void nav.focus;
    const key = takeFocus('my-assets');
    if (key) go('assets', assetRoute(key));
  });
  $effect(() => {
    void ba.version;
    loadLaunchFlags();
    loadPositions();
  });
  const worth = $derived(holdings.reduce((n, h) => n + (chnPrice(h.asset) ?? 0) * approx(h.balance), 0));
  const priced = $derived(holdings.filter((h) => chnPrice(h.asset) !== undefined).length);

  // --- Being issued: what this app is creating, and the reservations of the wallet ---
  const pending = $derived(pendingOf(wallets.current));
  /** The reservations the app does not know the supply of (made with the console, or the journal lost). */
  const strays = $derived((mine?.reservations ?? []).filter((r) => !pending.some((p) => p.name === r.name)));
  const issuingCount = $derived(pending.length + strays.length);
  /** The pools this wallet is making and funding (Launch's straight into a pool, Pools' New pool). */
  const funds = $derived(fundingOf(wallets.current));

  function stageOf(p: PendingIssue): ReturnType<typeof issueStage> {
    const r = mine?.reservations.find((x) => x.name === p.name);
    const registered = ba.assets.some((a) => a.asset === p.asset);
    if (!r && p.registration && !registered) return 'registering';
    return issueStage(r, registered);
  }
  function waitOf(name: string): number {
    return mine?.reservations.find((x) => x.name === name)?.wait ?? 0;
  }

  async function release(name: string) {
    const wallet = wallets.current;
    if (wallet === null) return;
    const r = await ask({
      title: t('Release'),
      message: t('Give up the reservation of %1?', name),
      informative: t('The name is free again once the release is in a block; anyone can then reserve it.'),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Release', kind: 'danger' },
      ],
    });
    if (r.choice !== 'yes') return;
    try {
      const done = await walletRpc<{ txid: string }>(wallet, 'releaseassetreservation', [name]);
      if (!done) return;
      dropPending(wallet, name);
      toast('success', t('Reservation released'), t('The reservation of %1 is released with the next block.', name));
      void refreshBitAssets();
    } catch (e) {
      await inform(t('Release'), errorText(e), 'error');
    }
  }

  async function register(p: PendingIssue) {
    try {
      const txid = await registerNow(p);
      if (txid) toast('success', t('Registering an asset'), t('%1 is being registered: it is in your assets after the next block.', assetText(p.name, p.display_name, undefined, p.asset)));
    } catch (e) {
      await inform(t('Register'), errorText(e), 'error');
    }
  }

  // A reservation the app has no record of: registered with what is typed here.
  let strayName = $state<string | null>(null);
  let straySupplyText = $state('0');
  let straySupply = $state<bigint | null>(0n);
  let strayDecimals = $state(2);
  let strayInfo = $state('');
  let strayPublic = $state(true);
  async function registerStray(r: Reservation) {
    const wallet = wallets.current;
    if (wallet === null || !r.name || straySupply === null) return;
    try {
      const done = await walletRpc<{ txid: string }>(wallet, 'registerasset', [r.name, amountParam(straySupply, strayDecimals), strayDecimals, strayInfo.trim() ? { info: strayInfo } : {}, strayPublic]);
      if (!done) return;
      strayName = null;
      toast('success', t('Registering an asset'), t('%1 is being registered: it is in your assets after the next block.', assetText(r.name ?? '')));
      void refreshBitAssets();
    } catch (e) {
      await inform(t('Register'), errorText(e), 'error');
    }
  }

  // --- Pool shares: of a standard pool by its shares, of a pool concentrated on the chain by the positions over its full range ---
  interface Share {
    pool: string;
    a: { id: string; label: string };
    b: { id: string; label: string };
    percent: number | null;
    valueA: number;
    valueB: number;
    feesA: number;
    feesB: number;
    positions: string[];
  }
  const shares = $derived.by(() => {
    const out: Share[] = (mine?.liquidity ?? []).map((l) => ({ pool: l.pool, a: { id: l.asset_a_id, label: l.asset_a }, b: { id: l.asset_b_id, label: l.asset_b }, percent: l.percent, valueA: approx(l.value_a), valueB: approx(l.value_b), feesA: 0, feesB: 0, positions: [] }));
    const byPool = new Map<string, Share>();
    for (const p of positionsRead.list.filter(fullRangePosition)) {
      const pool = positionPool(p);
      if (!pool) continue;
      const s = positionSides(p, flipped(pool));
      let row = byPool.get(p.pool);
      if (!row) {
        row = { pool: p.pool, a: { id: pool.asset_a_id, label: pool.asset_a }, b: { id: pool.asset_b_id, label: pool.asset_b }, percent: sharePercent(pool), valueA: 0, valueB: 0, feesA: 0, feesB: 0, positions: [] };
        byPool.set(p.pool, row);
        out.push(row);
      }
      row.valueA += approx(s.amountA);
      row.valueB += approx(s.amountB);
      row.feesA += approx(s.feesA);
      row.feesB += approx(s.feesB);
      if (approx(p.fees0) > 0 || approx(p.fees1) > 0) row.positions.push(p.position);
    }
    return out;
  });
  let collecting = $state('');
  async function collectFees(s: Share) {
    const wallet = wallets.current;
    if (wallet === null || collecting || s.positions.length === 0) return;
    const r = await ask({
      title: t('Collect fees'),
      message: t('Collect the fees your share of the %1 / %2 pool earned?', nameOf(s.a.id, s.a.label), nameOf(s.b.id, s.b.label)),
      informative: t('%1 comes by the coinbase of the next block; assets as coins of the transaction.', TICKER),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Collect', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    collecting = s.pool;
    try {
      for (const id of s.positions) {
        const done = await walletRpc<{ txid: string }>(wallet, 'changeposition', [id, { collect: true }]);
        if (!done) return;
      }
      toast('success', t('Collecting fees'), t('With the next block.'));
      void refreshBitAssets({ node: false });
    } catch (e) {
      await inform(t('Collect fees'), errorText(e), 'error');
    } finally {
      collecting = '';
    }
  }

  // --- Closing old markets: the wallet's orders, auctions and positions of a range, if it has any ---
  let orders = $state(0);
  $effect(() => {
    const wallet = wallets.current;
    void ba.version;
    if (wallet === null) {
      orders = 0;
      return;
    }
    rpc
      .raw<unknown[]>('listorders', [], { wallet })
      .then((r) => {
        if (wallets.current === wallet) orders = Array.isArray(r) ? r.length : 0;
      })
      .catch(() => {
        if (wallets.current === wallet) orders = 0;
      });
  });
  const ranged = $derived(positionsRead.list.filter((p) => !fullRangePosition(p)).length);
  // Asked for by its route ("closing": an old link to the auctions, the order book or the positions): shown, once it is there.
  $effect(() => {
    if (outer !== 'closing') return;
    queueMicrotask(() => document.getElementById('closing-old-markets')?.scrollIntoView?.({ block: 'start' }));
  });

  /** What an asset's row offers, each on the asset's own page: its Trade box, its Manage, its Add liquidity box. */
  function canOf(h: { asset: string; control: boolean; balance: Amount }) {
    const m = marketState(h.asset, ba.pools, launchById(h.asset));
    return { trade: m.state === 'curve' || m.state === 'pool', manage: h.control, liquidity: (m.state === 'none' || m.state === 'pool-empty') && approx(h.balance) > 0 };
  }

  /** Read more of the activity (it shows the newest ACTIVITY_PAGE at first: audit port-2 #13). */
  function showMoreActivity() {
    baView.activity = Math.min(MAX_ACTIVITY, baView.activity + ACTIVITY_PAGE);
    void refreshBitAssets();
  }

  // --- Auctions of the wallet (receipts) ---
  const receipts = $derived((mine?.receipts ?? []).map((r) => ({ id: r.auction, a: auctionById(r.auction) ?? null })));
  const closing = $derived(orders > 0 || receipts.length > 0 || ranged > 0);
  async function collect(a: Auction) {
    const wallet = wallets.current;
    if (wallet === null) return;
    const action = receiptAction(a);
    const r = await ask({
      title: action === 'cancel' ? t('Cancel the auction') : t('Collect the auction'),
      message:
        action === 'cancel'
          ? t('Cancel the auction of %1 %2? Nobody bid yet: everything comes back.', formatAmount(a.remaining, decimalsOf(a.base_id)), nameOf(a.base_id, a.base))
          : t('Collect %1 %2 and the %3 %4 left?', formatAmount(a.proceeds, decimalsOf(a.quote_id)), nameOf(a.quote_id, a.quote), formatAmount(a.remaining, decimalsOf(a.base_id)), nameOf(a.base_id, a.base)),
      informative: t('%1 comes by the coinbase of the next block; assets as coins of the transaction.', TICKER),
      level: 'question',
      choices: [
        { id: 'no', label: 'Back' },
        { id: 'yes', label: action === 'cancel' ? 'Cancel it' : 'Collect', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    try {
      const done = await walletRpc<{ txid: string }>(wallet, 'collectauction', [a.auction]);
      if (!done) return;
      toast('success', t('Collecting an auction'), t('With the next block: %1 by its coinbase, assets as coins.', TICKER));
      void refreshBitAssets();
    } catch (e) {
      await inform(t('Collect the auction'), errorText(e), 'error');
    }
  }

</script>

{#if noWallet}
  <NoWallet page={PAGES['my-assets']} />
{:else}
  <div class="page ba-page my-assets">
    <header class="page-head">
      <h1>Portfolio</h1>
      <div class="row">
        <!-- Simple mode has no Launch page in its navigation: the one create wizard is reached from here. -->
        {#if simple}<CreateAssetButton />{/if}
        {#if simple}<button type="button" class="btn small" onclick={() => go('trade', 'swap')}><Icon name="swap" size={15} /> Swap</button>{/if}
        <button type="button" class="btn small ghost" onclick={() => go('receive')} title="Assets are received at any address of this wallet"><Icon name="receive" size={15} /> Receive</button>
      </div>
    </header>

    {#if ba.mineState === 'locked'}
      <div class="panel locked">
        <Icon name="lock" size={22} />
        <p>This wallet is locked. It needs its passphrase to tell its reservations, so it shows its assets once unlocked for that alone.{#if mine} What is shown is as of the last time.{/if}</p>
        <button type="button" class="btn primary" onclick={() => void unlockHoldings()}><Icon name="unlock" size={15} /> Show my assets</button>
      </div>
    {:else if ba.mineState === 'error'}
      <p class="ba-problems bad" role="alert"><SafeText text={ba.mineError} /></p>
    {/if}

    <p class="worth" data-testid="portfolio-worth">Worth about <strong>{formatPrice(worth)} {TICKER}</strong> at current prices{#if priced < holdings.length}, for the {priced} of {holdings.length} that have a market{/if}. Open an asset to trade or manage it.</p>

    <TabRow label="Portfolio" tabs={TABS} current={tab} select={(id) => (nav.tab = id)} />

    {#if tab === 'holdings'}
      {#if issuingCount > 0 || funds.length > 0}
        <section class="panel" data-testid="being-created" aria-label="Being created">
          <div class="panel-head"><h3>Being created</h3><span class="muted small">in your assets once registered</span></div>
          <div class="panel-body">
            {#if funds.length}
              <div class="ba-cols funds">{#each funds as f (`${f.asset}|${f.quote}|${f.time}`)}<FundCard fund={f} />{/each}</div>
            {/if}
            <div class="ba-cols">
          {#each pending as p (p.name)}
            {@const stage = stageOf(p)}
            <section class="panel issuing">
              <header class="issue-head">
                <AssetBadge id={p.asset} label={p.name} />
                <span class="muted small">{p.supply} · {p.decimals} decimals{p.public ? '' : ` · ${t('private name')}`}</span>
              </header>
              <!-- Its progress (components/PendingCard.svelte): reserved, registering by itself in so long, registered;
                   the user's word when the app needs it (after a restart, or a locked wallet). -->
              <PendingCard op={pendingOpOf(p)} />
              {#if stage === 'waiting' && !simple}<p class="ba-help">{tn('Registered by itself in %n block(s), once the reservation is deep enough that whoever makes a block cannot take the name first.', waitOf(p.name))}</p>{/if}
              <div class="row">
                {#if stage === 'registering' && !p.registration && p.needsConfirm === undefined && !p.error}<button type="button" class="btn small" onclick={() => register(p)}>Register now</button>{/if}
                {#if stage !== 'registered' && !p.registration}<button type="button" class="btn small danger" onclick={() => release(p.name)}>Release</button>{/if}
                {#if stage === 'registered'}<button type="button" class="btn small" onclick={() => wallets.current !== null && dropPending(wallets.current, p.name)}>Dismiss</button>{/if}
              </div>
            </section>
          {/each}
          {#each strays as r (r.txid)}
            <section class="panel issuing">
              <header class="issue-head">
                {#if r.name}<AssetBadge id={r.txid} label={r.name} />{:else}<span class="muted">A reservation <span class="ba-mono" translate="no">{shortHash(r.txid, 8)}</span></span>{/if}
                {#if r.height !== undefined}<span class="faint small">reserved in block {formatCount(r.height)}</span>{/if}
              </header>
              {#if r.taken}
                <p class="ba-problems bad">Someone registered this name first. The reservation can only be released.</p>
              {:else if !r.name}
                <p class="ba-help">The name of this reservation is not known to this wallet: it can only be released by whoever knows it.</p>
              {:else if (r.wait ?? 0) > 0}
                <p class="ba-help">{tn('It can be registered in %n block(s).', r.wait ?? 0)} This app did not make it: register it with its supply once it can be.</p>
              {:else if strayName === r.name}
                <form class="ba-form" onsubmit={(e) => { e.preventDefault(); void registerStray(r); }}>
                  <UnitsInput id="stray-supply" label="Initial supply" decimals={strayDecimals} allowZero bind:units={straySupply} bind:text={straySupplyText} hint="0: none yet; mint later" />
                  <div class="field"><label for="stray-decimals">Decimals</label><input id="stray-decimals" type="number" min="0" max={MAX_DECIMALS} bind:value={strayDecimals} /></div>
                  <div class="field"><label for="stray-info">Description</label><input id="stray-info" type="text" maxlength="512" bind:value={strayInfo} /></div>
                  <label class="check"><input type="checkbox" bind:checked={strayPublic} /> <span>Publish the name, so that it is listed and found by name</span></label>
                  <div class="row"><button type="submit" class="btn primary small" disabled={straySupply === null}>Register</button><button type="button" class="btn small" onclick={() => (strayName = null)}>Cancel</button></div>
                </form>
              {:else}
                <p class="ba-help">Deep enough: it can be registered now. This app did not make it, so it asks for its supply.</p>
              {/if}
              <div class="row">
                {#if r.name && !r.taken && (r.wait ?? 0) === 0 && strayName !== r.name}<button type="button" class="btn primary small" onclick={() => (strayName = r.name ?? null)}>Register…</button>{/if}
                {#if r.name}<button type="button" class="btn small danger" onclick={() => release(r.name ?? '')}>Release</button>{/if}
              </div>
            </section>
          {/each}
            </div>
          </div>
        </section>
      {/if}

      <section class="panel" data-testid="holdings" aria-label="What you hold">
        <div class="panel-head"><h3>What you hold</h3></div>
        <HiddenToggle count={visible.hidden} bind:show={showHidden} />
        {#if holdings.length === 0}
          <div class="ba-empty">
            {#if ba.mineState === 'loading'}Loading…{:else}Nothing yet: swap {TICKER} for an asset, create one of your own, or have one sent to an address of this wallet.{/if}
            <div class="row center"><button type="button" class="btn primary" onclick={() => go('issue')}>{simple ? 'Create an asset' : 'Create asset'}</button><button type="button" class="btn" onclick={() => go('trade', 'swap')}>Swap</button></div>
          </div>
        {:else}
          <div class="ba-wrap">
            <table class="ba-table" use:stacked aria-label="Holdings">
              <thead><tr><th>Asset</th><th class="end">Balance</th><th class="end">Worth ({TICKER})</th><th>You can</th></tr></thead>
              <tbody>
                {#each holdings as h (h.asset)}
                  {@const price = chnPrice(h.asset) ?? (Number(launchById(h.asset)?.price) || undefined)}
                  {@const can = canOf(h)}
                  <tr class="pick" data-testid="holding-row" data-id={h.asset} onclick={() => go('assets', assetRoute(h.asset))} use:selectable={() => go('assets', assetRoute(h.asset))}>
                    <td><span class="who"><AssetBadge id={h.asset} label={h.label} display={displayNameOf(h.asset) ?? ''} size="s" />{#if h.control}<span class="ba-tag accent">controlled</span>{/if}</span></td>
                    <td class="end"><strong class="num">{formatAmount(h.balance, h.decimals, { max: 6 })}</strong>{#if approx(h.pending) > 0} <span class="ba-warn small">+{formatAmount(h.pending, h.decimals, { max: 6 })} waiting</span>{/if}</td>
                    <td class="end">{#if price !== undefined}{formatPrice(price * approx(h.balance))}{:else}<span class="faint">No price yet</span>{/if}</td>
                    <!-- svelte-ignore a11y_no_static_element_interactions, a11y_click_events_have_key_events (it only keeps its clicks and keys from the row under it) -->
                    <td><span class="can" onclick={(e) => e.stopPropagation()} onkeydown={(e) => e.stopPropagation()}>
                      {#if can.trade}<button type="button" class="btn small" data-action="trade" onclick={() => go('assets', assetRoute(h.asset, 'buy'))}>Trade</button>{/if}
                      {#if can.manage}<button type="button" class="btn small" data-action="manage" onclick={() => go('assets', assetRoute(h.asset))}>Manage</button>{/if}
                      {#if can.liquidity}<button type="button" class="btn small" data-action="liquidity" onclick={() => go('assets', assetRoute(h.asset, 'liquidity'))}>Add liquidity</button>{/if}
                    </span></td>
                  </tr>
                {/each}
              </tbody>
            </table>
          </div>
        {/if}
      </section>

      <MyLaunches />

      {#if !simple || shares.length > 0}
        <section class="panel" data-testid="pool-shares" aria-label="Your pool shares">
          <div class="panel-head"><h3>Your pool shares</h3></div>
          {#if shares.length === 0}
            <p class="ba-empty">None yet. Add liquidity from <button type="button" class="link" onclick={() => go('pools', 'pools')}>Pools</button> or from an asset's page, and your share and the fees it earns show here.</p>
          {:else}
            <div class="ba-wrap">
              <table class="ba-table" use:stacked aria-label="Pool shares">
                <thead><tr><th>Pool</th><th class="end">Your share</th><th>Worth</th><th>Fees to collect</th><th></th></tr></thead>
                <tbody>
                  {#each shares as s (s.pool)}
                    <tr class="pick" data-testid="share-row" data-pool={s.pool} onclick={() => go('pools', poolRoute(s.pool))} use:selectable={() => go('pools', poolRoute(s.pool))}>
                      <td><span class="who"><AssetBadge id={s.a.id} label={s.a.label} size="s" /> <span class="faint">/</span> <AssetBadge id={s.b.id} label={s.b.label} size="s" /></span></td>
                      <td class="end">{s.percent !== null ? formatPercent(s.percent) : '–'}</td>
                      <td>{formatAmount(s.valueA, decimalsOf(s.a.id), { max: 6 })} <SafeText text={s.a.label} /> · {formatAmount(s.valueB, decimalsOf(s.b.id), { max: 6 })} <SafeText text={s.b.label} /></td>
                      <td>{#if s.positions.length}{formatAmount(s.feesA, decimalsOf(s.a.id), { max: 6 })} <SafeText text={s.a.label} /> · {formatAmount(s.feesB, decimalsOf(s.b.id), { max: 6 })} <SafeText text={s.b.label} />{:else}<span class="faint">in the pool: they grow your share</span>{/if}</td>
                      <!-- svelte-ignore a11y_no_static_element_interactions, a11y_click_events_have_key_events (it only keeps its clicks and keys from the row under it) -->
                      <td><span class="can" onclick={(e) => e.stopPropagation()} onkeydown={(e) => e.stopPropagation()}>{#if s.positions.length}<button type="button" class="btn small primary" disabled={collecting !== ''} onclick={() => collectFees(s)}>Collect fees</button>{/if}</span></td>
                    </tr>
                  {/each}
                </tbody>
              </table>
            </div>
          {/if}
        </section>
      {/if}

      {#if closing}
        <section class="panel closing" id="closing-old-markets" data-testid="closing-old-markets" aria-label="Closing old markets">
          <div class="panel-head"><h3>Closing old markets</h3></div>
          <div class="panel-body stack">
            <p class="ba-help">Order books, auctions and price-range positions are no longer offered in this app. What this wallet still has in them is listed here so that you can take it back; nothing new of these kinds can be started.</p>
            {#if orders > 0}<MyOrders />{/if}
            {#if receipts.length > 0}
              <h4>Auctions</h4>
              <div class="ba-cols" data-testid="closing-auctions">
          {#each receipts as r (r.id)}
            <section class="panel position">
              {#if !r.a}
                <p class="ba-help">Auction <span class="ba-mono" translate="no">{shortHash(r.id, 8)}</span></p>
              {:else}
                {@const a = r.a}
                {@const action = receiptAction(a)}
                <header class="issue-head">
                  <span><strong>{formatAmount(a.amount, decimalsOf(a.base_id))}</strong> <SafeText text={a.base} /> <span class="faint">for</span> <SafeText text={a.quote} /></span>
                  <span class="ba-tag" class:accent={a.status === 'open'}>{t(AUCTION_STATUS[a.status] ?? 'Ended')}</span>
                </header>
                <div class="ba-bar" title={t('Sold')}><span use:barWidth={approx(a.amount) ? 1 - approx(a.remaining) / approx(a.amount) : 0}></span></div>
                <dl class="ba-lines">
                  <dt>Left</dt><dd>{formatAmount(a.remaining, decimalsOf(a.base_id))} <SafeText text={a.base} /></dd>
                  <dt>Brought in</dt><dd>{formatAmount(a.proceeds, decimalsOf(a.quote_id))} <SafeText text={a.quote} /></dd>
                  <dt>Bids</dt><dd>{a.bids}</dd>
                  <dt>Blocks</dt><dd>{formatCount(a.start_height)} → {formatCount(a.end_height)}</dd>
                </dl>
                <div class="row">
                  {#if action === 'collect'}<button type="button" class="btn primary small" onclick={() => collect(a)}>Collect</button>
                  {:else if action === 'cancel'}<button type="button" class="btn small" onclick={() => collect(a)}>Cancel it: take everything back</button>
                  {:else if action === 'wait'}<span class="faint small">It can be collected once it ends (after block {formatCount(a.end_height)}) or sells out.</span>{/if}
                </div>
              {/if}
            </section>
          {/each}
              </div>
            {/if}
            {#if ranged > 0}
              <h4>Positions with a price range</h4>
              <Positions />
            {/if}
          </div>
        </section>
      {/if}
    {:else}
      <section class="panel">
        <HiddenToggle count={ba.activity.length - (showHidden ? ba.activity.filter((a) => !chainTxHidden(a.txid)).length : activity.length)} bind:show={showHidden} />
        {#if activity.length === 0}
          <p class="ba-empty">Nothing yet.</p>
        {:else}
          <div class="ba-wrap">
            <table class="ba-table" use:stacked aria-label="Activity">
              <thead><tr><th>When</th><th>What</th><th>Details</th><th class="end">Confirmations</th><th>Transaction</th></tr></thead>
              <tbody>
                {#each activity as a (a.txid)}
                  <tr>
                    <td class="num">{formatTime(a.time)}</td>
                    <td>{t(OPERATIONS[a.operation] ?? 'Other')}</td>
                    <td><SafeText text={a.summary} />{#if assetMovesOf(a.txid).length}<div class="activity-moves"><LocalTxCell txid={a.txid} full /></div>{/if}</td>
                    <td class="end">{#if a.confirmations === 0}<span class="ba-warn">waiting</span>{:else}{formatCount(a.confirmations)}{/if}</td>
                    <td class="ba-mono"><span translate="no">{shortHash(a.txid, 6)}</span> <CopyButton text={a.txid} label="Copy the transaction" /></td>
                  </tr>
                {/each}
              </tbody>
            </table>
          </div>
          {#if ba.activity.length >= baView.activity && baView.activity < MAX_ACTIVITY}
            <p class="more"><button type="button" class="btn small" onclick={showMoreActivity}>{t('Show older activity')}</button></p>
          {/if}
        {/if}
      </section>
    {/if}
  </div>
{/if}

<style>
  .activity-moves {
    margin-top: 0.25rem;
  }
  .more {
    display: flex;
    justify-content: center;
    margin: 0.6rem 0;
  }
  .locked {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.8rem;
    padding: 0.9rem 1.1rem;
    border-color: var(--warning);
  }
  .locked p {
    flex: 1 1 18rem;
    margin: 0;
  }
  .center {
    justify-content: center;
    margin-top: 0.8rem;
  }
  .small {
    font-size: 0.85rem;
  }
  .issuing,
  .position {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
    padding: 1rem 1.1rem;
  }
  .issue-head {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 0.5rem;
  }
  .worth {
    margin: 0;
    color: var(--text-muted);
  }
  .who,
  .can {
    display: inline-flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.4rem;
    min-width: 0;
  }
  .stack {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
  }
  h4 {
    margin: 0.3rem 0 0;
  }
</style>
