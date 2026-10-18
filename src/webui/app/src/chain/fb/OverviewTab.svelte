<script lang="ts">
  // Staking, Overview: the chain's blocks of stake as they stand (the leader of this slot, how the fees of a block of
  // stake are split), your stake (the wallet's bonds), the staking income of the wallet and its coins by finality.
  // What this node does as a validator is in Validator, the bonds one by one in Bonds, the validators, the schedule,
  // finality and slashing in Network: no fact twice.
  import Amount from '../../components/Amount.svelte';
  import Icon from '../../components/Icon.svelte';
  import { css } from '../../lib/actions';
  import { formatCount } from '../../lib/format';
  import { t, tn } from '../../lib/i18n.svelte';
  import { go } from '../../lib/nav.svelte';
  import { wallets } from '../../lib/state.svelte';
  import { coinsToSats } from '../../lib/units';
  import { feeSplitOf, shortKey, type Income } from './model';
  import { fb } from './store.svelte';

  let { income }: { income: Income | null } = $props();

  const pos = $derived(fb.pos);
  const localKeys = $derived(new Set(fb.staking?.keys.map((k) => k.pubkey) ?? []));
  const leader = $derived(pos?.leader);
  const walletBonded = $derived((fb.bonds ?? []).reduce((n, b) => n + coinsToSats(b.bonded), 0n));
  const walletUnbonding = $derived((fb.bonds ?? []).reduce((n, b) => n + coinsToSats(b.unbonding), 0n));
  const split = $derived(pos ? feeSplitOf(pos) : null);
  const todayIncome = $derived(income ? income.today.staked + income.today.bmm + income.today.slashing : 0n);

  const finality = $derived(fb.finality);
  const finalityTotal = $derived(finality ? coinsToSats(finality.soft) + coinsToSats(finality.anchored) + coinsToSats(finality.final) : 0n);
  const part = (coins: number) => (finalityTotal > 0n ? Number((coinsToSats(coins) * 10000n) / finalityTotal) / 100 : 0);
</script>

<section class="side-stats" aria-label="Overview">
  <div class="panel side-stat">
    <span class="muted">Leader of this slot</span>
    <strong class="mono" id="fb-leader-now" translate="no">{leader ? (localKeys.has(leader) ? t('this node') : shortKey(leader)) : '–'}</strong>
    <span class="faint">{pos ? (pos.active ? tn('%n validator(s) in the set', pos.validators.length) : t('blocks of stake are off')) : '–'}</span>
  </div>
  <div class="panel side-stat">
    <span class="muted">Your stake</span>
    <strong class="num" id="fb-your-stake">{#if fb.bonds}<Amount sats={walletBonded} />{:else}–{/if}</strong>
    <span class="faint">{#if wallets.current === null}{t('no wallet open')}{:else if walletUnbonding > 0n}<Amount sats={walletUnbonding} /> {t('unbonding')}{:else}{tn('%n bond(s) of this wallet', fb.bonds?.length ?? 0)}{/if}</span>
  </div>
  <div class="panel side-stat">
    <span class="muted" title={t("How the fees of a block of stake are shared: the producer's share, and the rest (the fee pot, paid to BMM bidders, or to the validators)")}>Fee split</span>
    <strong class="num" id="fb-fee-split">{split ? split.headline : '–'}</strong>
    <span class="faint">{split ? split.detail : ''}</span>
  </div>
  <div class="panel side-stat">
    <span class="muted">Rewards today</span>
    <strong class="num">{#if income}<Amount sats={todayIncome} />{:else}–{/if}</strong>
    <span class="faint">{income ? tn('from %n block(s) of stake today', income.today.blocks) : t('no wallet open')}</span>
  </div>
</section>

<div class="row fb-overview-actions">
  <button type="button" class="btn primary" onclick={() => go('staking', 'bonds')}><Icon name="stake" size={15} /> Bond coins</button>
  <button type="button" class="btn ghost" onclick={() => go('staking', 'validator')}>This node as a validator</button>
  <button type="button" class="btn ghost" onclick={() => go('staking', 'validators')}>The validators</button>
</div>

<div class="side-cols">
  <section class="panel" aria-labelledby="fb-income-title">
    <div class="panel-head">
      <h2 id="fb-income-title">Staking income of this wallet</h2>
      <button type="button" class="btn small ghost" onclick={() => go('transactions')}>Transactions <Icon name="arrow" size={14} /></button>
    </div>
    {#if wallets.current === null || !income}
      <p class="side-empty">Open a wallet to see what it earned from staking.</p>
    {:else}
      <div class="side-table-wrap">
        <table class="side-table" aria-label="Staking income">
          <thead><tr><th></th><th class="end">Today</th><th class="end">In all</th></tr></thead>
          <tbody>
            <tr><td title={t("The producer's share of the fees of the blocks of stake made, paid to this wallet")}>Staked</td><td class="end"><Amount sats={income.today.staked} /></td><td class="end"><Amount sats={income.staked} /></td></tr>
            <tr><td title={t('The fee pot (and a share of the penalties) paid to this wallet for BMM: a BMM commitment it bid for, or a merged mined block')}>BMM rewards</td><td class="end"><Amount sats={income.today.bmm} /></td><td class="end"><Amount sats={income.bmm} /></td></tr>
            <tr><td title={t("The reporter's share of the penalty of a validator that signed two blocks for a slot, for proofs this wallet's blocks carried")}>Slashing rewards</td><td class="end"><Amount sats={income.today.slashing} /></td><td class="end"><Amount sats={income.slashing} /></td></tr>
            <tr class="total"><td>Total</td><td class="end"><Amount sats={income.today.staked + income.today.bmm + income.today.slashing} /></td><td class="end"><Amount sats={income.staked + income.bmm + income.slashing} /></td></tr>
          </tbody>
        </table>
      </div>
      <p class="side-note fb-small pad">{tn('From %n block(s) of stake whose share this wallet received (%1 today).', income.blocks, formatCount(income.today.blocks))}</p>
    {/if}
  </section>

  <section class="panel" aria-labelledby="fb-coins-title">
    <div class="panel-head"><h2 id="fb-coins-title">Coins of this wallet by finality</h2></div>
    <div class="panel-body">
      {#if wallets.current === null}
        <p class="side-note">Open a wallet to see how final its coins are.</p>
      {:else if !finality}
        <p class="side-note">The node does not say how final the coins are.</p>
      {:else}
        <div class="fb-finality">
          <div class="stack" role="img" aria-label={t('Soft %1 %, BMM-confirmed %2 %, final %3 %', part(finality.soft), part(finality.anchored), part(finality.final))}>
            <span class="final" use:css={{ width: `${part(finality.final)}%` }}></span>
            <span class="anchored" use:css={{ width: `${part(finality.anchored)}%` }}></span>
            <span class="soft" use:css={{ width: `${part(finality.soft)}%` }}></span>
          </div>
          <dl>
            <dt><i class="final"></i><span>Final</span></dt><dd><Amount sats={coinsToSats(finality.final)} /></dd>
            <dt><i class="anchored"></i><span>BMM-confirmed</span></dt><dd><Amount sats={coinsToSats(finality.anchored)} /></dd>
            <dt><i class="soft"></i><span>Soft</span></dt><dd><Amount sats={coinsToSats(finality.soft)} /></dd>
          </dl>
          <p class="side-note fb-small">
            Soft: only blocks of stake are on it, like an unconfirmed payment with slashing behind it. BMM-confirmed: a BMM commitment on the mainchain covers it; undoing it takes the mainchain's merged miners, not stake. Final: enough BMM commitments cover it.
            <button type="button" class="link" onclick={() => go('staking', 'finality')}>{t('How far the chain is final: Network, Finality & BMM')}</button>
          </p>
        </div>
      {/if}
    </div>
  </section>
</div>


<style>
  .total td {
    font-weight: 650;
  }
  .pad {
    padding: 0.6rem 0.8rem 0.9rem;
  }
  .fb-overview-actions {
    margin: 0 0 1rem;
    flex-wrap: wrap;
  }
  .link {
    padding: 0;
    border: 0;
    background: none;
    color: var(--accent-fg);
    font: inherit;
    cursor: pointer;
    text-decoration: underline;
  }
</style>
