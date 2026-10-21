<script lang="ts">
  // The cards of FastBlocks on the home of the dashboard: Staking (this node's state, its bond, the wallet's
  // staking income), Finality (how far the chain is BMM-confirmed, the blocks and slots since) and Next slots (the
  // slots this node leads next, with their countdown; the leaders to come on a node that is not a validator). Each
  // opens its page.
  import Amount from '../../components/Amount.svelte';
  import Icon from '../../components/Icon.svelte';
  import { formatCount } from '../../lib/format';
  import { t, tn } from '../../lib/i18n.svelte';
  import { go } from '../../lib/nav.svelte';
  import { node, wallets } from '../../lib/state.svelte';
  import { CHAIN } from '../config';
  import { coinsToSats } from '../../lib/units';
  import { current, refreshRecords } from '../../lib/walletdata.svelte';
  import Card from '../../views/home/Card.svelte';
  import './fb.css';
  import { clock, startClock } from './clock.svelte';
  import { incomeOf, incomeTotal, midnightOf, nodeState, nodeStateText, shortKey, slotsLed, stateTone, untilText } from './model';
  import { fb, followTip, startFb } from './store.svelte';
  import { simpleMode } from '../../lib/uimode.svelte';
  import { simpleState, simpleTone } from './model';

  $effect(() => startFb());
  $effect(() => startClock());
  $effect(() => {
    void node.tip?.hash;
    void wallets.current;
    followTip();
  });
  $effect(() => {
    if (wallets.current !== null) void refreshRecords();
  });

  const pos = $derived(fb.pos);
  const staking = $derived(fb.staking);
  const state = $derived(nodeState(staking, pos?.epoch ?? 0));
  const tone = $derived(state === 'nokey' ? 'wait' : state === 'error' || state === 'off' ? 'bad' : stateTone(state));
  const bonded = $derived((staking?.keys ?? []).reduce((n, k) => n + coinsToSats(k.bond?.bonded ?? 0), 0n));
  const walletBonded = $derived((fb.bonds ?? []).reduce((n, b) => n + coinsToSats(b.bonded), 0n));
  const income = $derived(wallets.current === null ? null : incomeOf(current.records, midnightOf(new Date(clock.now * 1000))));
  const localKeys = $derived(new Set(staking?.keys.map((k) => k.pubkey) ?? []));
  const next = $derived(pos ? slotsLed(fb.schedules, localKeys, pos.slot + 1, 3) : []);
  // Not a validator: the leaders of the next slots.
  const leaders = $derived(pos ? fb.schedules.flatMap((s) => s.slots).filter((s) => s.slot > pos.slot).slice(0, 3) : []);
  const anchors = $derived(fb.anchors);
  const tip = $derived(node.tip?.height ?? fb.height);
  const waiting = $derived(anchors && tip >= 0 ? Math.max(0, tip - Math.max(anchors.anchoredheight, -1)) : 0);
  // Simple mode: one card, in words (fb/SimpleStaking says the rest).
  const simple = $derived(simpleMode());
  const plain = $derived(simpleState(state));
  const PLAIN: Record<string, string> = {
    earning: 'Earning',
    joining: 'Waiting to join',
    'paused-off': 'Paused: staking is off',
    'paused-jailed': 'Paused: missed its turns',
    banned: 'Stopped for good',
    stopping: 'Stopping',
    low: 'Not enough staked yet',
    notstaking: 'Not staking yet',
    error: 'Paused: the block maker stopped',
    nokey: 'Not set up',
  };
  const mainName = CHAIN.mainchain?.name ?? 'mainchain';
  const sinceSlots = $derived(pos && pos.firstunanchoredslot >= 0 ? pos.slot - pos.firstunanchoredslot : null);
  // No mainchain block anchoring this chain for half an hour or more (a gap of the mainchain, up to hours on eCash):
  // the blocks of stake go on, soft until an anchor comes, for max_unanchored_slots (getposinfo) at most.
  const gap = $derived(
    pos && sinceSlots !== null && pos.maxunanchoredslots && sinceSlots * pos.slotseconds >= 1800
      ? { since: sinceSlots * pos.slotseconds, left: Math.max(0, pos.maxunanchoredslots - sinceSlots) * pos.slotseconds }
      : null,
  );
</script>

{#if fb.loaded && !fb.error && pos && simple}
  <div class="fb-cards">
    <Card title="Stake and earn" icon="stake" open={() => go('staking')} openLabel="Open Staking" stat>
      <span class="value"><span class="status {simpleTone(plain)}" id="fb-home-simple-state">{t(PLAIN[plain])}</span></span>
      <dl class="pairs">
        {#if wallets.current !== null}<dt>Staked</dt><dd><Amount sats={walletBonded} /></dd>{/if}
        <dt>Earned today</dt><dd>{#if income}<Amount sats={incomeTotal(income, true)} />{:else}–{/if}</dd>
        <dt>Earned in all</dt><dd>{#if income}<Amount sats={incomeTotal(income)} />{:else}–{/if}</dd>
      </dl>
      {#snippet actions()}
        <button type="button" class="btn grow" onclick={() => go('staking')}><Icon name="stake" size={15} /> {walletBonded > 0n ? 'Manage staking' : 'Start staking'}</button>
      {/snippet}
    </Card>
  </div>
{:else if fb.loaded && !fb.error && pos}
  <div class="fb-cards">
    <Card title="Staking" icon="stake" open={() => go('staking')} openLabel="Open Staking" stat>
      <span class="value"><span class="status {tone}">{nodeStateText(state)}</span></span>
      <dl class="pairs">
        <dt>This node's bond</dt><dd><Amount sats={bonded} /></dd>
        {#if wallets.current !== null}<dt>Bonded by this wallet</dt><dd><Amount sats={walletBonded} /></dd>{/if}
        <dt>Income today</dt><dd>{#if income}<Amount sats={incomeTotal(income, true)} />{:else}–{/if}</dd>
        <dt>Income in all</dt><dd>{#if income}<Amount sats={incomeTotal(income)} />{:else}–{/if}</dd>
      </dl>
      {#snippet actions()}
        <button type="button" class="btn grow" onclick={() => go('staking', 'bonds')}><Icon name="stake" size={15} /> Bond</button>
        <button type="button" class="btn ghost" onclick={() => go('staking', 'validators')}>Validators</button>
      {/snippet}
    </Card>

    <Card title="Finality" icon="finality" open={() => go('staking', 'finality')} openLabel="Open Finality & BMM" stat>
      <span class="value num">{anchors && anchors.anchoredheight >= 0 ? t('block %1', formatCount(anchors.anchoredheight)) : '–'}</span>
      <span class="faint small">{t('last BMM-confirmed block')}</span>
      <dl class="pairs">
        <dt>Blocks waiting</dt><dd class="num">{formatCount(waiting)}</dd>
        <dt>Slots since</dt><dd class="num">{sinceSlots !== null ? formatCount(sinceSlots) : '0'}</dd>
        <dt>Final up to</dt><dd class="num">{anchors && anchors.finalheight >= 0 ? t('block %1', formatCount(anchors.finalheight)) : '–'}</dd>
      </dl>
      {#if gap}
        <p class="gap small" role="status">{t('No %1 block for %2: the new blocks stay soft until one anchors them. Blocks of stake go on for %3 more.', mainName, untilText(gap.since), untilText(gap.left))}</p>
      {/if}
      {#snippet actions()}
        <button type="button" class="btn grow" onclick={() => go('staking', 'finality')}>BMM blocks</button>
        <button type="button" class="btn ghost" onclick={() => go('staking', 'slashing')}>Slashing</button>
      {/snippet}
    </Card>

    <Card title="Next slots" icon="clock" open={() => go('staking', 'schedule')} openLabel="Open the leader schedule" stat>
      {#if staking?.validator}
        {#if next.length === 0}
          <p class="muted">{t('This node leads none of the slots of this epoch and the next.')}</p>
        {:else}
          <span class="value num">{untilText(next[0].time - clock.now)}</span>
          <ul class="due">
            {#each next as s (s.slot)}<li><span>{t('Slot %1', formatCount(s.slot))}</span><span class="faint small">{untilText(s.time - clock.now)}</span></li>{/each}
          </ul>
        {/if}
      {:else}
        <p class="muted small">{t('Not a validator. The next leaders:')}</p>
        <ul class="due">
          {#each leaders as s (s.slot)}<li><span>{t('Slot %1', formatCount(s.slot))}</span><span class="faint small mono" translate="no">{s.leader ? shortKey(s.leader) : t('nobody')}</span></li>{/each}
        </ul>
      {/if}
      {#snippet actions()}
        <button type="button" class="btn grow" onclick={() => go('staking', 'schedule')}>Schedule</button>
        <span class="faint small epoch">{tn('epoch %1, a slot every %n s', pos.slotseconds, formatCount(pos.epoch))}</span>
      {/snippet}
    </Card>
  </div>
{/if}

<style>
  .value {
    display: block;
    margin: 0.1rem 0 0.55rem;
    font-size: 1.55rem;
    font-weight: 650;
    letter-spacing: -0.02em;
    line-height: 1.2;
  }
  .value .status {
    font-size: 0.95rem;
    letter-spacing: 0;
  }
  .pairs {
    display: grid;
    grid-template-columns: minmax(min-content, 1fr) minmax(0, auto);
    gap: 0.35rem 1rem;
    margin: 0;
    padding-top: 0.6rem;
    border-top: 1px solid var(--line);
  }
  .pairs dt {
    color: var(--text-muted);
    font-size: 0.86rem;
    overflow-wrap: break-word;
  }
  .pairs dd {
    min-width: 0;
    margin: 0;
    text-align: right;
    font-weight: 600;
    font-size: 0.92rem;
  }
  .fb-cards {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(17rem, 100%), 1fr));
    gap: 1rem;
  }
  .due {
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
    margin: 0;
    padding: 0.5rem 0 0;
    list-style: none;
    border-top: 1px solid var(--line);
  }
  .due li {
    display: flex;
    justify-content: space-between;
    gap: 0.8rem;
    min-width: 0;
  }
  .gap {
    margin: 0.4rem 0 0;
    color: var(--warning);
  }
  .small {
    font-size: 0.82rem;
  }
  .epoch {
    align-self: center;
  }
</style>
