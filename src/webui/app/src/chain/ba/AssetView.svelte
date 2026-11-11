<script lang="ts">
  // The asset page (owner decision, 2026-11-11): the one detail page of an asset. Every list row opens it and every
  // action on the asset starts from it, with the asset fixed by the page. In a fixed order:
  //  - the header: its logo, name, price, and its chips (where it trades: "On curve N%", "Pool", "Pool, empty", "No
  //    market"; Unlocked Supply or Fixed supply; the creator's allocation; "You control it");
  //  - its price over time with the period buttons (ba/Candles.svelte) and the figures the node has: the change and the
  //    volume of a day (from the market index's candles, where the node keeps one) and the market value. The chain lists
  //    no holders: that figure is left out;
  //  - Markets: one row per market it has. On its launch curve: the curve with its progress and fee, and a row saying
  //    its pool opens by itself at graduation. Else a row per pool, with its liquidity and fee, which opens the pool's
  //    page. Else "No market yet";
  //  - beside it, the Trade box: buy or sell this asset by the best route (ba/CurveTrade.svelte on its curve,
  //    ba/SwapPanel.svelte after), with the fee, the price impact and the slippage limit. When its only market is a pool
  //    with nothing in it, or when Add liquidity was asked for, the Add liquidity box takes its place
  //    (ba/AddLiquidity.svelte);
  //  - Your position: balance, worth, pool share;
  //  - Manage, only when the wallet holds the control coin or is the launch's creator: mint, burn, edit the image and
  //    the description (ba/ManagePanel.svelte), claim creator fees (ba/claim.ts), open a market (ba/OpenMarket.svelte);
  //  - Activity: its trades (block by block on its curve: ba/LaunchLog.svelte; the node lists no swap of a pool one by
  //    one) and the changes of its data;
  //  - Details: its supply, its launch (ba/LaunchFacts.svelte), its data, its ids, hiding it, and retiring it once dead.
  import { stacked } from '../../lib/actions';
  import SafeText from '../../components/SafeText.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import Icon from '../../components/Icon.svelte';
  import TabRow from '../../components/TabRow.svelte';
  import { errorText, inform, ask, walletRpc } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { formatCount, shortHash } from '../../lib/format';
  import { go, nav } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { toast, wallets } from '../../lib/state.svelte';
  import { barWidth } from './actions';
  import AddLiquidity from './AddLiquidity.svelte';
  import AssetBadge from './AssetBadge.svelte';
  import AssetName from './AssetName.svelte';
  import Candles from './Candles.svelte';
  import Chart from './Chart.svelte';
  import CurveTrade from './CurveTrade.svelte';
  import LaunchFacts from './LaunchFacts.svelte';
  import LaunchLog from './LaunchLog.svelte';
  import ManagePanel from './ManagePanel.svelte';
  import OpenMarket from './OpenMarket.svelte';
  import SwapPanel from './SwapPanel.svelte';
  import UnlockedSupplyBadge from './UnlockedSupplyBadge.svelte';
  import VerifiedBadge from './VerifiedBadge.svelte';
  import { assetRoute, poolRoute, type AssetBox } from './assetactions';
  import { compactNumber } from './axis';
  import { candlesOf, spanStats } from './candles';
  import { claimLaunch } from './claim';
  import { claimable, creatorAllocation, curvePointAt, curveSeries, curveShape, launchDecimals, launchSupply, poolKindText, rulesOf, type Launch, type LaunchStats } from './launch';
  import { formatChange, changeDirection } from './markets';
  import { approx, assetArg, DATA_FIELDS, formatAmount, formatPercent, formatPrice, formatUnits, isChn, type Asset, type AssetHistory, type DataField, type Pool } from './model';
  import { isShownPool, marketState, poolEcx, poolEmpty, poolFeeText, poolSides } from './pools';
  import { loadPositions, sharePercent } from './positions.svelte';
  import { assetTextOf, ba, nameOf, noteKnownAssets, noteLaunches, priceSeries, refreshBitAssets, sendAsset } from './store.svelte';
  import { ignoreAsset, ignoreName, unignoreAsset, unignoreName } from './hide.svelte';
  import { ignoredAsset } from './visible.svelte';
  import { TICKER } from '../config';

  /** box: what the route asks the page to open on ("asset/<id>/buy", "/sell", "/liquidity": ba/assetactions.ts). */
  let { id, box = '', onback }: { id: string; box?: AssetBox; onback?: () => void } = $props();

  let asset = $state<Asset | null>(null);
  let history = $state<AssetHistory>({});
  let pools = $state<Pool[]>([]);
  let launch = $state<Launch | null>(null);
  let stats = $state<LaunchStats | null>(null);
  let error = $state('');
  let loadedAt = '';

  const FIELD_NAMES: Record<DataField, string> = {
    info: 'Description',
    commitment: 'Commitment',
    ipv4: 'IPv4 address',
    ipv6: 'IPv6 address',
    encryptionkey: 'Encryption key',
    signingkey: 'Signing key',
  };

  // Asked again when the asset changes or the store refreshed (a block, a transaction of the wallet).
  $effect(() => {
    const key = `${id}@${ba.version}`;
    if (key === loadedAt) return;
    loadedAt = key;
    const arg = assetArg(id);
    // Its pools asked of the node by asset: the lists of the store are pages (audit 7 E-BA3).
    Promise.all([
      rpc.raw<Asset>('getasset', [arg]),
      rpc.raw<AssetHistory>('getassethistory', [arg]).catch(() => ({})),
      rpc.raw<Pool[]>('listpools', [arg, 1000, 0]).catch(() => []),
      // A launched asset: its launch (on its curve, or graduated).
      rpc.raw<Launch>('getlaunch', [arg]).catch(() => null),
      rpc.raw<LaunchStats>('getlaunchstats', []).catch(() => null),
    ])
      .then(([a, h, p, l, s]) => {
        if (!loadedAt.startsWith(`${id}@`)) return;
        asset = a;
        if (a && typeof a === 'object' && a.asset) noteKnownAssets([a]);
        launch = l && typeof l === 'object' ? l : null;
        if (launch) noteLaunches([launch]);
        stats = s && typeof s === 'object' ? s : null;
        history = h ?? {};
        pools = p ?? [];
        error = '';
      })
      .catch((e) => {
        if (!loadedAt.startsWith(`${id}@`)) return;
        asset = null;
        error = errorText(e);
      });
  });
  $effect(() => {
    void ba.version;
    loadPositions();
  });

  const holding = $derived(ba.mine?.assets.find((a) => a.asset === id) ?? null);
  const controlled = $derived(!!holding?.control);
  const held = $derived(approx(holding?.balance) > 0);
  const onCurve = $derived(!!launch && launch.status !== 'graduated');
  const graduated = $derived(launch?.status === 'graduated');
  const market = $derived(marketState(id, pools, launch));
  /** Its pools as the page lists them: no order book, no launch curve. */
  const shown = $derived(pools.filter(isShownPool));
  /** Its price in ECX: its curve's now, else its pool's with ECX. */
  const price = $derived.by(() => {
    if (onCurve && launch) return Number(launch.price) || undefined;
    const p = market.pool;
    if (!p || poolEmpty(p)) return undefined;
    const s = poolSides(p);
    return isChn(s.quote.id) ? s.price : undefined;
  });
  /** The market whose candles are the asset's price: its curve's pool, else its pool with ECX. */
  const chartPool = $derived(onCurve ? market.curvePool : market.pool && isChn(poolSides(market.pool).quote.id) ? market.pool.pool : '');
  const series = $derived(market.pool ? priceSeries(market.pool, id).map((p) => ({ x: p.h, y: p.p })) : []);
  const heldShare = $derived(asset && holding && approx(asset.supply) > 0 ? approx(holding.balance) / approx(asset.supply) : 0);
  /** The wallet's share of its pool: of a standard pool by its shares, of one concentrated on the chain by its full-range positions. */
  const poolShare = $derived.by(() => {
    const p = market.pool;
    if (!p) return null;
    if (p.curve === 'concentrated') return sharePercent(p);
    return ba.mine?.liquidity.find((l) => l.pool === p.pool)?.percent ?? null;
  });

  // The figures of a day, from the market index's candles of its market (hourly, the last 24 hours): none on a node
  // without the index, and then the page shows a dash, never a guess.
  let day = $state<{ change: number; volume: number } | null>(null);
  let dayKnown = $state(false);
  $effect(() => {
    const pool = chartPool;
    void ba.version;
    day = null;
    dayKnown = false;
    if (!pool) return;
    let live = true;
    rpc
      .raw<unknown>('getpoolcandles', [pool, '1h', Math.floor(Date.now() / 1000) - 86_400])
      .then((r) => {
        if (!live) return;
        day = spanStats(candlesOf(r));
        dayKnown = true;
      })
      .catch(() => undefined);
    return () => {
      live = false;
    };
  });

  // Whether the launch's creator address is this wallet's.
  let creatorMine = $state(false);
  $effect(() => {
    const address = launch?.creator ?? '';
    const wallet = wallets.current;
    creatorMine = false;
    if (!address || wallet === null) return;
    rpc
      .raw<{ ismine?: boolean }>('getaddressinfo', [address], { wallet })
      .then((info) => {
        if (launch?.creator === address) creatorMine = info?.ismine === true;
      })
      .catch(() => undefined);
  });
  const owed = $derived(launch ? claimable(launch) : { fees: false, allocation: false, tokens: false });
  const anyOwed = $derived(owed.fees || owed.allocation || owed.tokens);
  let claiming = $state(false);
  async function claim() {
    if (!launch || claiming) return;
    claiming = true;
    try {
      await claimLaunch(launch);
    } finally {
      claiming = false;
    }
  }

  // The launch's curve, for its Details.
  const rules = $derived(rulesOf(stats?.rules));
  const shape = $derived(curveShape(launch ? approx(launch.graduation) : rules.graduation, launchSupply(launch, rules.supply)));
  const DEC = $derived(launch ? launchDecimals(launch) : (asset?.decimals ?? 0));
  const now = $derived(curvePointAt(shape, graduated ? shape.graduation : launch ? approx(launch.raised) : 0));
  const allocationShown = $derived(launch ? creatorAllocation(launch) : null);
  const graduationFee = $derived(Number(stats?.rules?.graduation_fee ?? 1));

  /** The changes of each field, newest first; the registration's empty stamps left out (as chains-qt left them out). */
  const changes = $derived(
    DATA_FIELDS.flatMap((field) => (history[field] ?? []).filter((e, i) => !(i === 0 && e.value === null)).map((e) => ({ field, ...e }))).sort((a, b) => b.height - a.height),
  );
  const fields = $derived(DATA_FIELDS.filter((f) => asset?.data?.[f] !== undefined));

  /** What the side box is: the Trade box, or Add liquidity in its place (asked for, or the only market is an empty pool). */
  const liquidityBox = $derived(box === 'liquidity' || market.state === 'pool-empty');
  const setBox = (b: AssetBox) => (nav.tab = assetRoute(id, b));
  let activity = $state<'trades' | 'changes'>('trades');

  async function retire() {
    const wallet = wallets.current;
    if (!asset || wallet === null) return;
    const r = await ask({
      title: t('Retire %1', assetTextOf(asset.asset, asset.label)),
      message: t('Retire %1 for good? It and its pools are deleted and its name is free again; the %2 %3 its pools hold go to mainchain miners, as a fee.', nameOf(asset.asset, asset.label), formatAmount(asset.release_fee ?? 0, 8), TICKER),
      informative: t('Anyone may retire a dead asset; this wallet pays the fee of the transaction.'),
      level: 'warning',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Retire', kind: 'danger' },
      ],
    });
    if (r.choice !== 'yes') return;
    try {
      const done = await walletRpc<{ txid: string }>(wallet, 'releaseasset', [assetArg(id)]);
      if (!done) return;
      toast('success', t('Retiring an asset'), t('%1 is retired with the next block.', assetTextOf(asset.asset, asset.label)));
      void refreshBitAssets();
    } catch (e) {
      await inform(t('Retire'), errorText(e), 'error');
    }
  }
</script>

<div class="asset-view" data-testid="asset-page" data-market={market.state}>
  {#if onback}
    <button type="button" class="btn ghost small ba-back" onclick={onback}><Icon name="back" size={15} /> All assets</button>
  {/if}

  {#if error}
    <p class="ba-problems bad" role="alert"><SafeText text={error} /></p>
  {:else if !asset}
    <p class="ba-empty">Loading…</p>
  {:else}
    <header class="panel hero">
      <AssetBadge {id} label={asset.label} size="l" showLabel={false} logo={asset.logo?.hash} />
      <div class="who">
        <h2 class="name"><AssetName label={asset.label} display={asset.display_name ?? ''} seq={asset.seq} {id} /></h2>
        <div class="tags">
          <span class="ba-tag market {market.state}" data-testid="market-chip">{#if market.state === 'curve'}{t('On curve %1', formatPercent(Number(launch?.progress) || 0))}{:else if market.state === 'pool'}Pool{:else if market.state === 'pool-empty'}Pool, empty{:else}No market{/if}</span>
          <span class="ba-tag" translate="no">#{asset.seq}</span>
          <VerifiedBadge {id} />
          {#if asset.fixed && launch?.mintable !== true}<span class="ba-tag accent">Fixed supply</span>{:else}<UnlockedSupplyBadge mintable />{/if}
          {#if !asset.name}<span class="ba-tag warn" title="Its registration did not publish the name: it is known by its hash and number">Private name</span>{/if}
          {#if launch && launch.allocation > 0}<span class="ba-tag warn" title="Minted to the creator at graduation, on top of the supply">Creator allocation {launch.allocation}%</span>{/if}
          {#if controlled}<span class="ba-tag accent"><Icon name="crown" size={12} /> You control it</span>{/if}
          {#if asset.releasable}<span class="ba-tag danger">Dead</span>{/if}
        </div>
      </div>
      <div class="price" data-testid="asset-price">
        {#if price !== undefined}<strong class="num">{formatPrice(price, 8)} {TICKER}</strong>{:else}<strong class="faint">No price yet</strong>{/if}
      </div>
    </header>
    {#if allocationShown && launch}
      <p class="ba-problems alloc" role="note" data-testid="launch-allocation"><strong>{t('Creator allocation: %1% minted to the creator at graduation', String(launch.allocation))}</strong> ({formatUnits(allocationShown.units, allocationShown.decimals, { max: 2 })} {launch.ticker}, {t('on top of the supply: holders are diluted by it')}){#if launch.allocation_claimed}. {t('Minted.')}{/if}</p>
    {/if}

    <div class="ba-split">
      <div class="col">
        <section class="panel" aria-label="Price">
          <div class="panel-head"><h3>Price</h3><span class="muted small">{TICKER} for 1 <SafeText text={asset.label} /></span></div>
          <div class="panel-body stack">
            {#if chartPool}
              <Candles pool={chartPool} quoteLabel={TICKER} {series} />
            {:else}
              <p class="ba-empty">No price yet: it has no market that trades.</p>
            {/if}
            <dl class="stats" data-testid="asset-stats">
              <div><dt>24 h change</dt><dd translate="no"><span class:ba-good={changeDirection(day?.change) === 'up'} class:ba-bad={changeDirection(day?.change) === 'down'}>{day ? formatChange(day.change) : '–'}</span></dd></div>
              <div><dt>24 h volume</dt><dd>{#if day}{formatAmount(day.volume, 8, { max: 4 })} {TICKER}{:else if dayKnown}{t('No trades')}{:else}–{/if}</dd></div>
              <div><dt>Market cap</dt><dd>{#if onCurve && launch}{formatAmount(launch.market_cap, 8, { max: 2 })} {TICKER}{:else if price !== undefined}{formatPrice(price * approx(asset.supply))} {TICKER}{:else}–{/if}</dd></div>
              <div><dt>Supply</dt><dd>{formatAmount(asset.supply, asset.decimals, { max: 4 })}</dd></div>
            </dl>
          </div>
        </section>

        <section class="panel" data-testid="asset-markets" aria-label="Markets">
          <div class="panel-head"><h3>Markets</h3></div>
          <ul class="markets">
            {#if onCurve && launch}
              <li class="mrow" data-testid="market-row" data-kind="curve">
                <div class="mmain">
                  <span><strong>Launch curve</strong> <span class="faint">· {launch.fee}% fee</span></span>
                  <span class="ba-bar"><span use:barWidth={Number(launch.progress) / 100}></span></span>
                  <span class="faint small">{formatAmount(launch.raised, 8, { max: 4 })} of {formatAmount(launch.graduation, 8, { max: 4 })} {TICKER} raised · {formatAmount(launch.curve_tokens_left, DEC, { max: 2 })} <SafeText text={launch.ticker} /> left on the curve</span>
                </div>
                <span class="ba-tag accent">Trading here now</span>
              </li>
              <li class="mrow" data-testid="market-row" data-kind="pool-at-graduation">
                <div class="mmain">
                  <strong>Pool <SafeText text={launch.ticker} /> / {TICKER}</strong>
                  <span class="faint small">Opens by itself at graduation, with the {TICKER} raised and what is set aside for it. Nobody can open one before then.</span>
                </div>
                <span class="ba-tag">At graduation</span>
              </li>
            {:else}
              {#if graduated && launch}
                <li class="mrow" data-testid="launch-graduated">
                  <div class="mmain">
                    <span><strong>Launch curve</strong> <span class="faint">· graduated</span></span>
                    <span class="faint small">At block {formatCount(launch.graduated_height ?? 0)} its curve raised {formatAmount(launch.raised, 8, { max: 4 })} {TICKER}; that and what was left of it went into its pool with {TICKER}{#if launch.pool_kind} ({poolKindText(launch, graduationFee)}){/if}, whose shares nobody holds: the liquidity is locked for good. Anyone may add liquidity on top.</span>
                  </div>
                  <span class="ba-tag">Graduated</span>
                </li>
              {/if}
              {#each shown as p (p.pool)}
                {@const s = poolSides(p)}
                {@const empty = poolEmpty(p)}
                <li class="mrow" data-testid="market-row" data-kind={empty ? 'pool-empty' : 'pool'}>
                  <button type="button" class="mmain link-row" onclick={() => go('pools', poolRoute(p.pool))}>
                    <span><strong>Pool <SafeText text={s.base.label} /> / <SafeText text={s.quote.label} /></strong> <span class="faint">· <span translate="no">{poolFeeText(p)}</span> fee</span></span>
                    {#if empty}
                      <span class="faint small">Nothing in it yet. {#if s.price}Its starting price is {formatPrice(s.price)} <SafeText text={s.quote.label} /> for 1 <SafeText text={s.base.label} />. {/if}It cannot be traded until someone adds liquidity.</span>
                    {:else}
                      <span class="faint small">{#if poolEcx(p) !== undefined}{formatAmount(poolEcx(p) ?? 0, 8, { max: 4 })} {TICKER} of liquidity · {/if}{formatCount(p.swaps)} swaps</span>
                    {/if}
                  </button>
                  {#if empty}<span class="ba-tag warn">Needs liquidity</span>{/if}
                  {#if wallets.current !== null && isChn(s.quote.id)}<button type="button" class="btn small" data-testid="add-liquidity" onclick={() => setBox('liquidity')}>Add liquidity</button>{/if}
                </li>
              {/each}
              {#if shown.length === 0}
                <li class="mrow" data-testid="market-row" data-kind="none">
                  <div class="mmain"><strong>No market yet</strong><span class="faint small">Nothing trades it. Its controller can put it on a launch curve; anyone who holds some can fund a pool for it.</span></div>
                  {#if held}<button type="button" class="btn small" data-testid="add-liquidity" onclick={() => setBox('liquidity')}>Add liquidity</button>{/if}
                </li>
              {/if}
            {/if}
          </ul>
        </section>

        {#if wallets.current !== null}
          <section class="panel" data-testid="asset-position" aria-label="Your position">
            <div class="panel-head"><h3>Your position</h3></div>
            <div class="panel-body stack">
              <dl class="stats">
                <div><dt>Balance</dt><dd>{holding ? formatAmount(holding.balance, asset.decimals) : '0'} <SafeText text={asset.label} /></dd></div>
                <div><dt>Worth</dt><dd>{#if holding && price !== undefined}{formatPrice(price * approx(holding.balance))} {TICKER}{:else}–{/if}</dd></div>
                <div><dt>Share of supply</dt><dd>{heldShare ? formatPercent(heldShare * 100) : '–'}</dd></div>
                <div><dt>Pool share</dt><dd>{poolShare !== null ? formatPercent(poolShare) : t('None yet')}</dd></div>
              </dl>
              {#if holding && approx(holding.pending) > 0}<p class="ba-warn small">+{formatAmount(holding.pending, asset.decimals)} on the way: spendable after the next block</p>{/if}
              <div class="row">
                <button type="button" class="btn small" disabled={!held} onclick={() => { sendAsset(id); go('send'); }}><Icon name="send" size={14} /> Send</button>
                <button type="button" class="btn small ghost" onclick={() => go('receive')}><Icon name="receive" size={14} /> Receive</button>
              </div>
              <!-- A holder who does not control it: burning its own units is all it can do to the asset. -->
              {#if holding && held && !controlled}{#key id}<ManagePanel {holding} embedded />{/key}{/if}
            </div>
          </section>
        {/if}

        {#if controlled || creatorMine}
          <section class="panel" data-testid="asset-manage" aria-label="Manage">
            <div class="panel-head"><h3>Manage</h3><span class="muted small">{controlled ? t('shown because this wallet holds its control coin') : t('shown because this wallet created its launch')}</span></div>
            <div class="panel-body stack">
              {#if holding && controlled}{#key id}<ManagePanel {holding} embedded />{/key}{/if}
              {#if launch && creatorMine}
                <div class="row">
                  <button type="button" class="btn small" class:primary={anyOwed} data-testid="claim-fees" disabled={claiming || !anyOwed} onclick={claim}>Claim creator fees</button>
                  <span class="faint small">{anyOwed ? t('%1 %2 owed', formatAmount(launch.creator_fees_owed, 8), TICKER) : t('nothing owed')}{#if owed.allocation}{' · '}{t('and the allocation of %1%', String(launch.allocation))}{/if}</span>
                </div>
              {/if}
              {#if controlled && market.state === 'none'}<OpenMarket {asset} {pools} launched={!!launch} {onCurve} />{/if}
            </div>
          </section>
        {/if}

        <section class="panel" data-testid="asset-activity" aria-label="Activity">
          <div class="panel-head"><h3>Activity</h3></div>
          <TabRow label="Activity" tabs={[{ id: 'trades', label: 'Trades' }, { id: 'changes', label: 'Changes' }]} current={activity} select={(x) => (activity = x === 'changes' ? 'changes' : 'trades')} />
          {#if activity === 'trades'}
            {#if launch && onCurve}
              <LaunchLog {launch} />
            {:else if market.state === 'pool'}
              <p class="ba-empty">The node keeps the price of a pool over time (the chart above), not a list of its swaps.</p>
            {:else}
              <p class="ba-empty">No trades yet.</p>
            {/if}
          {:else if changes.length === 0}
            <p class="ba-empty">Its data never changed since its registration.</p>
          {:else}
            <div class="ba-wrap">
              <table class="ba-table" use:stacked aria-label="History of its data">
                <thead><tr><th>Block</th><th>Field</th><th>Value</th></tr></thead>
                <tbody>
                  {#each changes as c (c.field + c.txid)}
                    <tr>
                      <td class="num">{formatCount(c.height)}</td>
                      <td>{t(FIELD_NAMES[c.field])}</td>
                      <td>{#if c.value === null}<span class="faint">(deleted)</span>{:else}<SafeText class="ba-mono" text={c.value} />{/if}</td>
                    </tr>
                  {/each}
                </tbody>
              </table>
            </div>
          {/if}
        </section>

        <section class="panel" data-testid="asset-details" aria-label="Details">
          <div class="panel-head"><h3>Details</h3><span class="muted small">registered in block {formatCount(asset.registered)}</span></div>
          <div class="panel-body stack">
            {#if asset.data?.info}<p class="info"><SafeText text={asset.data.info} /></p>{/if}
            <dl class="ba-lines">
              <dt>Supply</dt><dd class="strong">{formatAmount(asset.supply, asset.decimals)}</dd>
              <dt>Minted, all told</dt><dd>{formatAmount(asset.minted, asset.decimals)}</dd>
              <dt>Burned</dt><dd>{formatAmount(asset.burned, asset.decimals)}</dd>
              <dt>Decimals</dt><dd>{asset.decimals}</dd>
              {#if onCurve && launch}
                <dt>On the curve</dt><dd>{formatAmount(launch.curve_tokens_left, DEC, { max: 4 })} left</dd>
                {#if launch.pool_supply !== undefined}<dt>Set aside for the pool</dt><dd>{formatAmount(launch.pool_supply, 0)}</dd>{/if}
              {/if}
              <dt>Control coin</dt><dd>{#if asset.fixed}Burned: nobody can mint more of it or change its data, ever.{:else}Held: whoever holds it can mint more and change its data.{/if}</dd>
            </dl>
            {#if launch}
              <h4>Its launch</h4>
              <LaunchFacts {launch} mine={creatorMine} {graduationFee} />
              {#if onCurve}
                <Chart points={curveSeries(shape)} marks={[{ x: now.sold, y: now.price }]} area label={t('The bonding curve of the launch: its price by the amount it sold, and where it is')} formatX={(x) => compactNumber(x, 3)} formatY={(y) => formatPrice(y, 4)} />
                <p class="ba-help">From {formatPrice(shape.startPrice)} to {formatPrice(shape.endPrice)} {TICKER} each, 9 times more, over {compactNumber(shape.curveSupply, 3)} of it; then {compactNumber(shape.poolSupply, 3)} go into its pool with the {TICKER} raised, at the curve's last price. It sold {compactNumber(Math.max(0, now.sold), 3)} so far.</p>
              {/if}
            {/if}
            <h4>About it</h4>
            {#if fields.length === 0}<p class="ba-help">Its issuer gave no data.</p>{/if}
            <dl class="ba-lines facts">
              {#each fields.filter((f) => f !== 'info') as f (f)}
                <dt>{t(FIELD_NAMES[f])}</dt>
                <dd class="ba-mono"><span translate="no">{asset.data?.[f]}</span> <CopyButton text={asset.data?.[f] ?? ''} label="Copy" /></dd>
              {/each}
              <dt>Hash</dt><dd class="ba-mono"><span translate="no">{shortHash(asset.asset, 12)}</span> <CopyButton text={asset.asset} label="Copy the hash" /></dd>
              <dt>Number</dt><dd><span translate="no">{asset.seq}</span></dd>
              {#if asset.logo}<dt>Logo</dt><dd class="ba-mono"><span translate="no">{asset.logo.mime === 'image/png' ? 'PNG' : 'JPEG'}, {(asset.logo.size / 1024).toFixed(1)} KB, {shortHash(asset.logo.hash, 10)}</span> <CopyButton text={asset.logo.hash} label="Copy the hash of the logo" /> <span class="faint">block {asset.logo.height}</span></dd>{/if}
              <dt>Registration</dt><dd class="ba-mono"><span translate="no">{shortHash(asset.registration, 10)}</span> <CopyButton text={asset.registration} label="Copy the transaction" /></dd>
              {#if asset.control?.address}<dt>Controlled by</dt><dd class="ba-mono"><span translate="no">{asset.control.address}</span> <CopyButton text={asset.control.address} label="Copy the address" /></dd>{/if}
            </dl>
            <!-- Hide it in this wallet's lists (display only: the coins stay), or every asset of its name (ba/hide.svelte.ts). -->
            {#if wallets.current !== null}
              <div class="row">
                {#if ignoredAsset(id, asset.label)}
                  <button type="button" class="btn small ghost" onclick={() => { unignoreAsset(id); unignoreName(asset?.label ?? ''); }}><Icon name="eye" size={15} /> Unignore</button>
                {:else}
                  <button type="button" class="btn small ghost" onclick={() => ignoreAsset(id)}><Icon name="eyeoff" size={15} /> Ignore this asset</button>
                  <button type="button" class="btn small ghost" onclick={() => ignoreName(asset?.label ?? '')}>Ignore every asset named <SafeText text={asset.label} /></button>
                {/if}
              </div>
            {/if}
          </div>
        </section>

        {#if asset.releasable}
          <section class="panel dead">
            <div class="panel-head"><h3>This asset is dead</h3></div>
            <div class="panel-body stack">
              <p class="ba-help">Its supply is fixed, nobody holds any of it, and its pools have no liquidity providers. Retiring it deletes it and its pools and frees its name; the {formatAmount(asset.release_fee ?? 0, 8)} {TICKER} its pools hold go to mainchain miners, as a fee. Anyone may do it.</p>
              <div><button type="button" class="btn danger" disabled={wallets.current === null} onclick={retire}><Icon name="flame" size={15} /> Retire it</button></div>
            </div>
          </section>
        {/if}
      </div>

      <div class="col side" data-testid="asset-box" data-box={liquidityBox ? 'liquidity' : market.state === 'none' ? 'none' : 'trade'}>
        {#if liquidityBox}
          <!-- In place of Trade: asked for, or its only market is a pool with nothing in it. -->
          {#key id}<AddLiquidity asset={id} {pools} {onCurve} />{/key}
          {#if market.state === 'pool' || onCurve}<div class="row"><button type="button" class="btn small ghost" onclick={() => setBox('')}><Icon name="back" size={14} /> Back to Trade</button></div>{/if}
        {:else if onCurve && launch}
          {#key id}<CurveTrade {launch} side={box === 'sell' ? 'sell' : 'buy'} focus={box === 'buy' || box === 'sell'} />{/key}
        {:else if market.state === 'pool'}
          {#key id}<SwapPanel asset={id} side={box === 'sell' ? 'sell' : 'buy'} />{/key}
        {:else}
          <section class="panel note" data-testid="no-market">
            <div class="panel-body stack">
              <h3>Trade <SafeText text={asset.label} /></h3>
              <p class="ba-empty">No market yet: nothing to trade it against.</p>
              {#if held}<div class="row"><button type="button" class="btn small primary" onclick={() => setBox('liquidity')}>Add liquidity</button></div>{/if}
            </div>
          </section>
        {/if}
      </div>
    </div>
  {/if}
</div>

<style>
  .asset-view {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .hero {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 1rem;
    padding: 1.1rem 1.2rem;
  }
  .who {
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
    flex: 1 1 14rem;
    min-width: 0;
  }
  .name {
    display: flex;
    min-width: 0;
    margin: 0;
    font-size: 1.7rem;
    letter-spacing: 0.01em;
  }
  .tags {
    display: flex;
    flex-wrap: wrap;
    gap: 0.35rem;
  }
  .price {
    font-size: 1.35rem;
    white-space: nowrap;
  }
  .ba-tag.market.curve {
    color: var(--accent);
    border-color: var(--accent);
  }
  .ba-tag.market.pool {
    color: var(--positive);
  }
  .ba-tag.market.pool-empty {
    color: var(--warning);
    border-color: var(--warning);
  }
  .col {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    min-width: 0;
  }
  .stack {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
  }
  .stats {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(8rem, 1fr));
    gap: 0.6rem 1rem;
    margin: 0;
  }
  .stats dt {
    color: var(--text-muted);
    font-size: 0.8rem;
  }
  .stats dd {
    margin: 0;
    font-weight: 600;
    font-variant-numeric: tabular-nums;
    overflow-wrap: anywhere;
  }
  .markets {
    display: flex;
    flex-direction: column;
    margin: 0;
    padding: 0;
    list-style: none;
  }
  .mrow {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.6rem;
    padding: 0.75rem 1.1rem;
    border-top: 1px solid var(--line);
  }
  .mrow:first-child {
    border-top: 0;
  }
  .mmain {
    display: flex;
    flex-direction: column;
    gap: 0.25rem;
    flex: 1 1 14rem;
    min-width: 0;
  }
  .mmain .ba-bar {
    max-width: 16rem;
  }
  .link-row {
    padding: 0;
    border: 0;
    background: none;
    color: inherit;
    font: inherit;
    text-align: start;
    cursor: pointer;
  }
  .link-row:hover strong {
    text-decoration: underline;
  }
  .info {
    margin: 0;
    white-space: pre-wrap;
    overflow-wrap: anywhere;
  }
  .facts dd {
    display: flex;
    align-items: center;
    gap: 0.25rem;
  }
  h4 {
    margin: 0.4rem 0 0;
  }
  .note h3 {
    margin: 0;
  }
  .dead {
    border-color: var(--negative);
  }
  .small {
    font-size: 0.85rem;
  }
</style>
