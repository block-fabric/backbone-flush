<script lang="ts">
  // Staking in Simple mode: one card. What this node's staking is doing, in words (earning, waiting to join,
  // paused, not set up); "Stake [amount]" with about what it would earn a day (an estimate, from the total
  // stake, a block a slot and the fees of the latest blocks); Start staking, Add more, Stop staking (and when
  // the coins come back); what it earned today and in all. The same host methods as the Bonds tab of Advanced
  // (fastblocks.bond, fastblocks.unbond with "all", fastblocks.staking), which ask the desktop first.
  import Amount from '../../components/Amount.svelte';
  import AmountInput from '../../components/AmountInput.svelte';
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import Term from '../../components/Term.svelte';
  import { BridgeError, request } from '../../lib/bridge';
  import { errorText, inform, withUnlock } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { go } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { node, prefs, toast, wallets } from '../../lib/state.svelte';
  import { coinsToSats, formatAmount, satsToCoins } from '../../lib/units';
  import { setUiMode } from '../../lib/uimode.svelte';
  import { clock } from './clock.svelte';
  import { anchorsTime, incomeTotal, bondProblem, dailyEstimate, nodeState, secondsUntilEpoch, simpleState, simpleTone, stakeSats, untilText, type Income } from './model';
  import { fb, refreshFb } from './store.svelte';

  let { income }: { income: Income | null } = $props();

  const pos = $derived(fb.pos);
  const staking = $derived(fb.staking);
  const plain = $derived(simpleState(nodeState(staking, pos?.epoch ?? 0)));
  /** The key this node stakes with (its first). */
  const key = $derived(staking?.keys[0]?.pubkey ?? '');
  const localKeys = $derived(new Set(staking?.keys.map((k) => k.pubkey) ?? []));
  /** What this wallet has bonded to this node's keys. */
  const mine = $derived((fb.bonds ?? []).filter((b) => localKeys.has(b.validator)));
  const myBond = $derived(mine.find((b) => b.validator === key));
  const bonded = $derived(mine.reduce((n, b) => n + coinsToSats(b.bonded), 0n));
  const unbonding = $derived(mine.reduce((n, b) => n + coinsToSats(b.unbonding), 0n));
  const minStake = $derived(coinsToSats(pos?.staking.minstake ?? 0));
  const epochSlots = $derived(fb.schedules[0]?.slots.length ?? 0);
  const keyBond = $derived(staking?.keys[0]?.bond);

  // The average fees of the latest blocks, for the estimate (again every minute at most).
  const FEE_BLOCKS = 12;
  let avgFee = $state(0);
  let feesAt = 0;
  $effect(() => {
    const height = node.tip?.height ?? -1;
    if (height < 1 || Date.now() - feesAt < 60_000) return;
    feesAt = Date.now();
    void (async () => {
      let total = 0;
      let n = 0;
      for (let h = height; h > Math.max(0, height - FEE_BLOCKS); h--) {
        try {
          const s = await rpc.raw<{ totalfee: number }>('getblockstats', [h, ['totalfee']]);
          total += s.totalfee;
          n++;
        } catch {
          break;
        }
      }
      avgFee = n ? total / n : 0;
    })();
  });

  let amount = $state<bigint | null>(null);
  let amountKey = $state(0);
  let busy = $state(false);

  /** What the amount typed (or what is bonded now) would earn a day. */
  const others = $derived(stakeSats(Math.max(0, (staking?.totalstake ?? fb.set?.totalstake ?? 0) - (staking?.stake ?? 0))));
  const ownStake = $derived(stakeSats(staking?.stake ?? 0));
  const perDayNow = $derived(dailyEstimate(ownStake, others, pos?.slotseconds ?? 0, avgFee));
  const perDayWith = $derived(amount !== null && amount > 0n ? dailyEstimate(ownStake + amount, others, pos?.slotseconds ?? 0, avgFee) : null);

  const joinIn = $derived(pos && fb.set ? secondsUntilEpoch(pos.epoch + 2, fb.set, epochSlots, pos.slot, pos.slotseconds) : 0);
  const jailIn = $derived(pos && fb.set && keyBond ? secondsUntilEpoch(keyBond.jaileduntilepoch, fb.set, epochSlots, pos.slot, pos.slotseconds) : 0);
  const backIn = $derived(anchorsTime(pos?.staking.unbondinganchors ?? 0));

  const headline = $derived.by(() => {
    switch (plain) {
      case 'earning':
        return t('Earning');
      case 'joining':
        return joinIn > 0 ? t('Waiting to join: about %1', untilText(joinIn)) : t('Waiting to join');
      case 'paused-off':
        return t('Paused: staking is turned off');
      case 'paused-jailed':
        return t('Paused: your node missed its turns');
      case 'banned':
        return t('Stopped for good: your node signed two blocks for one turn');
      case 'stopping':
        return t('Stopping: your coins are on their way back');
      case 'low':
        return t('Not enough staked yet');
      case 'notstaking':
        return t('Not staking yet');
      case 'error':
        return t('Paused: the block maker of this node stopped');
      case 'nokey':
        return t('Not set up: this node has no validator key');
    }
  });

  /** This wallet has coins staked on this node now (not only some on their way back). */
  const staked = $derived(!!myBond && myBond.bonded > 0);
  const canStake = $derived(!!key && plain !== 'banned' && wallets.current !== null);
  const problem = $derived(canStake ? bondProblem(staked ? 'add' : 'bond', key, amount, myBond, minStake) : '');

  async function send(method: 'fastblocks.bond' | 'fastblocks.unbond', params: Record<string, unknown>): Promise<Record<string, unknown> | null> {
    const wallet = wallets.current;
    if (wallet === null) return null;
    busy = true;
    try {
      const all = { wallet, ...params };
      const result = await withUnlock(wallet, () => request<Record<string, unknown>>('host', method, all));
      await refreshFb();
      return result ?? null;
    } catch (e) {
      if (BridgeError.from(e).code !== -32800) await inform(t('Staking'), errorText(e), 'warning');
      return null;
    } finally {
      busy = false;
    }
  }

  async function stake() {
    if (problem || amount === null || busy) return;
    const result = await send('fastblocks.bond', { pubkey: key, amount: satsToCoins(amount) });
    if (!result) return;
    amount = null;
    amountKey++;
    toast('success', t('Staking'), result.eligible === false ? t('Staked. That is less than the minimum: staking starts once at least that much is staked.') : t('Staked. Your node joins the validators in a little while.'));
  }

  async function stopAll() {
    if (!staked || busy) return;
    const result = await send('fastblocks.unbond', { pubkey: key, amount: 'all' });
    if (result) toast('success', t('Staking'), t('Stopping. Your coins come back in %1.', backIn));
  }

  async function startAgain() {
    busy = true;
    try {
      await request('host', 'fastblocks.staking', { enabled: true });
      await refreshFb();
    } catch (e) {
      if (BridgeError.from(e).code !== -32800) await inform(t('Staking'), errorText(e), 'warning');
    } finally {
      busy = false;
    }
  }
</script>

<section class="panel simple-stake" aria-labelledby="fb-simple-title">
  <div class="panel-head">
    <h2 id="fb-simple-title">Stake and earn</h2>
    <span class="status {simpleTone(plain)}" id="fb-simple-state">{headline}</span>
  </div>
  <div class="panel-body body">
    <p class="muted lead">
      Lock some ECX so that your node takes turns making the blocks of this chain, and earns part of their fees. You can stop at any time; the coins then come back after a waiting time.
      <Term id="bond">stake</Term> · <Term id="validator">validator</Term> · <Term id="slot">slot</Term>
    </p>

    {#if plain === 'nokey'}
      <p class="side-note side-warn">This node has no validator key, so it cannot take turns. Starting the node with a key file (-validatorkeyfile) makes it a validator: see Validator in <button type="button" class="link" onclick={() => { setUiMode('advanced'); go('staking', 'validator'); }}>Advanced mode</button>.</p>
    {:else if plain === 'error'}
      <p class="side-note side-bad"><SafeText text={staking?.producererror ?? ''} /></p>
    {:else if plain === 'paused-jailed'}
      <p class="side-note side-warn">It was left out for a while because it missed too many of its turns{jailIn > 0 ? t(': back in about %1', untilText(jailIn)) : ''}. No coins are lost. Keep the node running and online.</p>
    {:else if plain === 'low'}
      <p class="side-note side-warn">{t('Staking starts once at least %1 is staked.', formatAmount(minStake, prefs.unit, { withUnit: true }))}</p>
    {/if}

    <dl class="facts figures">
      <dt>Staked by you</dt><dd><Amount sats={bonded} /></dd>
      {#if unbonding > 0n}<dt>Coming back</dt><dd><Amount sats={unbonding} /> <span class="faint">{t('in %1', backIn)}</span></dd>{/if}
      <dt>Earned today</dt><dd>{#if income}<Amount sats={incomeTotal(income, true)} />{:else}–{/if}</dd>
      <dt>Earned in all</dt><dd>{#if income}<Amount sats={incomeTotal(income)} />{:else}–{/if}</dd>
      {#if ownStake > 0n && avgFee > 0}<dt>This node earns</dt><dd>{t('about %1 a day', formatAmount(perDayNow, prefs.unit, { withUnit: true }))}</dd>{/if}
    </dl>

    {#if plain === 'paused-off'}
      <button type="button" class="btn primary" id="fb-simple-start-again" disabled={busy} onclick={startAgain}><Icon name="power" size={15} /> Start staking again</button>
    {/if}

    {#if canStake}
      <form class="stake-form" onsubmit={(e) => { e.preventDefault(); stake(); }}>
        {#key amountKey}<AmountInput id="fb-simple-amount" bind:value={amount} label={staked ? 'Add to your stake' : 'Stake'} />{/key}
        {#if perDayWith !== null && avgFee <= 0}
          <p class="estimate" id="fb-simple-estimate">{t('The latest blocks paid next to no fees, so staking earns next to nothing right now.')} <span class="faint">{t('Validators earn part of the fees of the blocks they make: more payments, more earnings.')}</span></p>
        {:else if perDayWith !== null}
          <p class="estimate" id="fb-simple-estimate">{t("You'd earn about %1 a day.", formatAmount(perDayWith, prefs.unit, { withUnit: true }))} <span class="faint">{t('An estimate, from the coins staked by everyone, a block every %1 s and the fees of the latest blocks: it changes with them.', pos?.slotseconds ?? 0)}</span></p>
        {/if}
        {#if problem && amount !== null}<p class="fb-error fb-small" role="alert">{problem}</p>{/if}
        <div class="row">
          <button type="submit" class="btn primary" id="fb-simple-stake" disabled={!!problem || busy}><Icon name="stake" size={15} /> {staked ? 'Add more' : 'Start staking'}</button>
          {#if staked}
            <button type="button" class="btn danger-ghost" id="fb-simple-stop" disabled={busy} onclick={stopAll} title={t('Take all of your stake back: it comes back in %1', backIn)}>Stop staking</button>
          {/if}
        </div>
        {#if staked}<p class="faint fb-small">{t('Stopping takes all of your stake back; it comes back to this wallet in %1 and earns nothing meanwhile.', backIn)}</p>{/if}
      </form>
    {:else if wallets.current === null && plain !== 'nokey'}
      <p class="muted">Open a wallet to stake from it.</p>
    {/if}

    <p class="faint fb-small">Validators, the schedule of turns, finality and the rest are in <button type="button" class="link" onclick={() => setUiMode('advanced')}>Advanced mode</button>.</p>
  </div>
</section>

<style>
  .simple-stake {
    max-width: 46rem;
  }
  .body {
    display: flex;
    flex-direction: column;
    gap: 0.8rem;
  }
  .lead {
    margin: 0;
    line-height: 1.45;
  }
  .figures {
    margin: 0;
  }
  .stake-form {
    display: flex;
    flex-direction: column;
    gap: 0.55rem;
    padding-top: 0.6rem;
    border-top: 1px solid var(--line);
  }
  .estimate {
    margin: 0;
    font-weight: 600;
  }
  .estimate .faint {
    display: block;
    font-weight: 400;
    font-size: 0.84rem;
  }
  .row {
    display: flex;
    flex-wrap: wrap;
    gap: 0.5rem;
  }
  .danger-ghost {
    color: var(--negative);
  }
  :global(.simple-stake .panel-head .status) {
    font-size: 0.92rem;
  }
</style>
