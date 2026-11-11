<script lang="ts">
  // The launch of an asset in its Details (ba/AssetView.svelte): when it was launched, its supply, its fee and where the
  // creator's part goes (its split, share by share, with what each was paid and is owed; what was burned), its
  // allocation, its pool once graduated; its creator, the fees owed and paid, and the payment of what the creator is owed
  // (claimlaunch, which anyone may send: ba/claim.ts).
  import SafeText from '../../components/SafeText.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { formatCount } from '../../lib/format';
  import { wallets } from '../../lib/state.svelte';
  import { claimLaunch } from './claim';
  import { launchFeeParts, launchFeeSplit } from './fees';
  import { claimable, feeDestinationText, launchDecimals, poolKindText, shareKind, shareRecipientText, shareText, type Launch } from './launch';
  import { approx, formatAmount } from './model';
  import { TICKER } from '../config';

  let { launch, mine = false, graduationFee = 1 }: { launch: Launch; /** The creator's address is this wallet's. */ mine?: boolean; graduationFee?: number } = $props();
  const DEC = $derived(launchDecimals(launch));
  const graduated = $derived(launch.status === 'graduated');
  const split = $derived(launchFeeSplit(launchFeeParts(launch)));
  const owed = $derived(claimable(launch));
  const feeSplit = $derived(Array.isArray(launch.fee_split) ? launch.fee_split : []);
  let busy = $state(false);
  async function claim() {
    if (busy) return;
    busy = true;
    try {
      await claimLaunch(launch);
    } finally {
      busy = false;
    }
  }
</script>

<dl class="ba-lines" data-testid="launch-facts">
  <dt>Launched</dt><dd>block {formatCount(launch.height)}{#if graduated}, graduated at block {formatCount(launch.graduated_height ?? 0)}{/if}</dd>
  <dt>Curve fee</dt><dd data-testid="launch-fee-split">{split}</dd>
  {#if feeSplit.length}
    <dt>Fee split</dt><dd data-testid="launch-fee-split-shares"><ul class="split-list">{#each feeSplit as s, i (i)}<li data-testid="launch-fee-share">{#if shareKind(s.to) === 'address'}<SafeText class="mono break" text={s.to} />{:else}{shareRecipientText(s)}{/if}: {shareText(s, Number(launch.creator_share) || 0, Number(launch.fee) || 0)}{#if s.paid !== undefined}{' · '}{t('%1 paid', `${formatAmount(s.paid, 8)} ${TICKER}`)}{/if}{#if approx(s.owed ?? 0) > 0}{' · '}{t('%1 owed', `${formatAmount(s.owed ?? 0, 8)} ${TICKER}`)}{/if}</li>{/each}</ul>{#if approx(launch.burned ?? 0) > 0}<span data-testid="launch-burned">{t('%1 burned', `${formatAmount(launch.burned ?? 0, DEC, { max: 2 })} ${launch.ticker}`)}</span>{/if}</dd>
    {#if graduated && poolKindText(launch, graduationFee)}<dt>Its pool</dt><dd data-testid="launch-pool-kind">{poolKindText(launch, graduationFee)}</dd>{/if}
  {:else if launch.fee_flow}
    <dt>Creator fees</dt><dd data-testid="launch-fee-destination">{feeDestinationText(launch)}{#if approx(launch.burned ?? 0) > 0}{' · '}<span data-testid="launch-burned">{t('%1 burned', `${formatAmount(launch.burned ?? 0, DEC, { max: 2 })} ${launch.ticker}`)}</span>{/if}</dd>
    {#if graduated && poolKindText(launch, graduationFee)}<dt>Its pool</dt><dd data-testid="launch-pool-kind">{poolKindText(launch, graduationFee)}</dd>{/if}
  {/if}
  <dt>Allocation</dt><dd>{#if launch.allocation > 0}{launch.allocation}% of the supply, minted to the creator at graduation (dilution){#if launch.allocation_claimed}, minted{/if}{:else}none{/if}</dd>
  <dt>Creator</dt><dd><span class="mono break" translate="no"><SafeText text={launch.creator} /></span>{#if mine} <span class="ba-tag accent">this wallet</span>{/if}</dd>
  <dt>Fees owed</dt><dd>{formatAmount(launch.creator_fees_owed, 8)} {TICKER}</dd>
  {#if approx(launch.creator_tokens_owed ?? 0) > 0}<dt>Tokens owed</dt><dd data-testid="launch-tokens-owed">{formatAmount(launch.creator_tokens_owed ?? 0, DEC, { max: 2 })} <SafeText text={launch.ticker} /></dd>{/if}
  <dt>Fees paid</dt><dd>{formatAmount(launch.creator_fees_paid, 8)} {TICKER}</dd>
  {#if launch.security_fees !== undefined}<dt>To the chain's security</dt><dd>{formatAmount(launch.security_fees, 8)} {TICKER}</dd>{/if}
</dl>
{#if !mine && (owed.fees || owed.allocation || owed.tokens)}
  <div class="row"><button type="button" class="btn small" disabled={busy || wallets.current === null} onclick={claim}>Pay the creator what it is owed</button></div>
  <p class="ba-help">Anyone may send it: the {TICKER} go to the creator's address, by the coinbase{#if owed.allocation}, and its allocation is minted to it{/if}.</p>
{/if}

<style>
  .split-list {
    margin: 0;
    padding-left: 1.1em;
  }
</style>
