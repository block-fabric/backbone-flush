<script lang="ts">
  // Staking: FastBlocks' blocks of stake in one section (user decision, 2026-10-18: one section, not the three pages
  // FastBlocks had, so as not to confuse). Tabs: Overview (the leader of this slot, the fee split, your stake, the
  // wallet's income and its coins by finality), Validator (this node as a validator: its keys, staking on or off,
  // its payout, what it made and missed, the slots it leads), Bonds (bond, add to a bond, unbond, unbond all, a bond
  // to another node's key, the proof of possession for one) and Network (the validator set, the leader schedule,
  // finality and the BMM blocks, slashing and jail). The slot and the epoch are in the head, for every tab. Simple
  // mode (lib/uimode.svelte.ts) shows one card (fb/SimpleStaking: your stake, your rewards, bond and unbond); the
  // rest by a link or the palette. What the node says comes from fb/store; bonds and staking off go through the host,
  // which asks the desktop first (local.cpp).
  import Amount from '../../components/Amount.svelte';
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { formatCount } from '../../lib/format';
  import { t } from '../../lib/i18n.svelte';
  import { nav } from '../../lib/nav.svelte';
  import { node, wallets } from '../../lib/state.svelte';
  import { current, refreshRecords } from '../../lib/walletdata.svelte';
  import { coinsToSats } from '../../lib/units';
  import { simpleMode } from '../../lib/uimode.svelte';
  import '../fb/fb.css';
  import { clock, startClock } from '../fb/clock.svelte';
  import { formatShare, incomeOf, midnightOf, nodeState, nodeStateText, sharePercent, slotsLed, stateTone, untilText } from '../fb/model';
  import { fb, followTip, startFb } from '../fb/store.svelte';
  import BondsTab from '../fb/BondsTab.svelte';
  import NetworkTab from '../fb/NetworkTab.svelte';
  import NodeTab from '../fb/NodeTab.svelte';
  import OverviewTab from '../fb/OverviewTab.svelte';
  import SimpleStaking from '../fb/SimpleStaking.svelte';
  import { stakingTabOf, type StakingTab } from '../local';

  $effect(() => startFb());
  $effect(() => startClock());
  $effect(() => {
    void node.tip?.hash;
    void wallets.current;
    followTip();
  });
  // The wallet's history: its staking income.
  $effect(() => {
    if (wallets.current !== null) void refreshRecords();
  });

  const TABS: { id: StakingTab; label: string }[] = [
    { id: 'overview', label: 'Overview' },
    { id: 'validator', label: 'Validator' },
    { id: 'bonds', label: 'Bonds' },
    { id: 'network', label: 'Network' },
  ];
  const where = $derived(stakingTabOf(nav.tab));
  const tab = $derived(where.tab);
  // Simple mode: one card for your stake and rewards; Validator and Network when asked for (a link, the palette).
  const simpleCard = $derived(simpleMode() && (tab === 'overview' || tab === 'bonds'));

  const pos = $derived(fb.pos);
  const staking = $derived(fb.staking);
  const state = $derived(nodeState(staking, pos?.epoch ?? 0));
  const tone = $derived(state === 'nokey' ? 'wait' : state === 'error' || state === 'off' ? 'bad' : stateTone(state));
  const localKeys = $derived(new Set(staking?.keys.map((k) => k.pubkey) ?? []));
  const bonded = $derived((staking?.keys ?? []).reduce((n, k) => n + coinsToSats(k.bond?.bonded ?? 0), 0n));
  const unbonding = $derived((staking?.keys ?? []).reduce((n, k) => n + coinsToSats(k.bond?.unbonding ?? 0), 0n));
  const share = $derived(staking ? sharePercent(staking.stake, staking.totalstake) : 0);
  const next = $derived(pos ? slotsLed(fb.schedules, localKeys, pos.slot + 1, 1)[0] : undefined);
  const income = $derived(wallets.current === null ? null : incomeOf(current.records, midnightOf(new Date(clock.now * 1000))));
</script>

<div class="page fb-page staking">
  <header class="page-head">
    <h1>Staking</h1>
    {#if pos && !simpleCard}
      <span class="muted fb-head-meta">
        <span>{t('Slot %1', formatCount(pos.slot))}</span>
        <span>{t('epoch %1', formatCount(pos.epoch))}</span>
        <span>{t('a slot every %1 s', pos.slotseconds)}</span>
      </span>
    {/if}
  </header>

  {#if fb.error}
    <p class="side-note side-bad" role="alert"><SafeText text={fb.error} /></p>
  {:else if pos && !pos.active}
    <p class="side-note side-warn" role="status">
      Blocks of stake are off in this epoch: too few validators have stake bonded. The chain's blocks are merged mined only until enough is bonded.
    </p>
  {:else if pos?.stalled}
    <p class="side-note side-warn" role="status">No blocks of stake lately: merged mined blocks carry the transactions until they come again.</p>
  {/if}

  {#if simpleCard}
    {#if !fb.loaded}
      <p class="muted"><Icon name="clock" size={15} /> Asking the node…</p>
    {:else}
      <SimpleStaking {income} />
    {/if}
  {:else}
    <div class="tabs" role="tablist" aria-label="Staking">
      {#each TABS as tb (tb.id)}
        <button type="button" role="tab" id="fb-tab-{tb.id}" aria-selected={tab === tb.id} onclick={() => (nav.tab = tb.id)}>
          {t(tb.label)}
          {#if tb.id === 'bonds' && fb.bonds?.length}<span class="badge accent">{fb.bonds.length}</span>{/if}
        </button>
      {/each}
    </div>

    {#if !fb.loaded}
      <p class="muted"><Icon name="clock" size={15} /> Asking the node…</p>
    {:else if tab === 'overview'}
      <OverviewTab {income} />
    {:else if tab === 'validator'}
      <section class="side-stats" aria-label="This node as a validator">
        <div class="panel side-stat">
          <span class="muted">This node</span>
          <strong><span class="status {tone}" id="fb-node-state">{nodeStateText(state)}</span></strong>
          <span class="faint">{staking?.validator ? (staking.totalstake > 0 ? t('%1 of the slots', formatShare(share)) : t('no set known for this epoch')) : t('no validator key')}</span>
        </div>
        <div class="panel side-stat">
          <span class="muted">Bonded to its keys</span>
          <strong class="num"><Amount sats={bonded} /></strong>
          <span class="faint">{#if unbonding > 0n}<Amount sats={unbonding} /> {t('unbonding')}{:else}{t('nothing unbonding')}{/if}</span>
        </div>
        <div class="panel side-stat">
          <span class="muted">Next slot it leads</span>
          <strong class="num">{next ? untilText(next.time - clock.now) : '–'}</strong>
          <span class="faint">{next ? t('slot %1', formatCount(next.slot)) : staking?.validator ? t('none in the next two epochs') : t('not a validator')}</span>
        </div>
      </section>
      <NodeTab />
    {:else if tab === 'bonds'}
      <BondsTab />
    {:else}
      <NetworkTab view={where.view} />
    {/if}
  {/if}
</div>
