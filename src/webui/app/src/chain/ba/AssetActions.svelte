<script lang="ts">
  // The actions of an asset's row (user decision, 2026-10-24: Portfolio, My launches, Browse assets, an asset's page):
  // what can be done with it now, from its markets (ba/assetactions.ts): the most relevant as a button, the rest in a
  // "…" menu (components/MenuButton.svelte: arrows, Home/End, Escape; a target of 2.75rem on touch). Each leads to its
  // page with the asset, and its side, in the route (Swap "swap/get/<id>", Markets "launches/<token>/sell",
  // "pools/<pool>"), so that back, forward and a reload keep them. A click here does not open the row under it.
  import Icon, { type IconName } from '../../components/Icon.svelte';
  import MenuButton from '../../components/MenuButton.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { go, goTo } from '../../lib/nav.svelte';
  import { wallets } from '../../lib/state.svelte';
  import type { Command } from '../../lib/commands.svelte';
  import { ACTION_LABELS, assetActions, launchRoute, marketsOf, poolRoute, swapRoute, type AssetAction } from './assetactions';
  import { approx } from './model';
  import { assetTextOf, ba, holdingOf, openMarketIntent, sendAsset } from './store.svelte';

  let {
    id,
    label,
    launch = null,
    claimable = false,
    claim,
    busy = false,
  }: {
    id: string;
    /** Its label (text of the chain): for the menu's name. */
    label: string;
    /** Its launch, when the page knows it (My launches, Markets, its page). */
    launch?: { status: string; pool?: string } | null;
    /** Its launch owes the wallet now (My launches). */
    claimable?: boolean;
    claim?: () => void;
    busy?: boolean;
  } = $props();

  const holding = $derived(holdingOf(id));
  const markets = $derived(marketsOf(id, ba.pools, launch));
  const actions = $derived(assetActions({ markets, wallet: wallets.current !== null, held: approx(holding?.balance) > 0, controlled: holding?.control === true, claimable: claimable && !!claim }));
  const ICONS: Record<AssetAction, IconName> = { claim: 'receive', buy: 'rocket', 'sell-curve': 'rocket', swap: 'swap', sell: 'swap', 'add-liquidity': 'pool', 'view-market': 'pool', 'open-market': 'plus', send: 'send', receive: 'receive', view: 'gem' };

  function run(a: AssetAction) {
    switch (a) {
      case 'claim':
        claim?.();
        return;
      case 'buy':
        return go('pools', launchRoute(id, 'buy'));
      case 'sell-curve':
        return go('pools', launchRoute(id, 'sell'));
      case 'swap':
        return go('trade', swapRoute(id, 'get'));
      case 'sell':
        return go('trade', swapRoute(id, 'pay'));
      case 'add-liquidity':
        return go('pools', poolRoute(markets.bestPool));
      case 'view-market':
        return launch || markets.curve ? go('pools', launchRoute(id, 'buy')) : go('pools', poolRoute(markets.bestPool));
      case 'open-market':
        openMarketIntent.asset = id;
        return goTo('assets', '', id);
      case 'send':
        sendAsset(id);
        return go('send');
      case 'receive':
        return go('receive');
      case 'view':
        return goTo('assets', '', id);
    }
  }

  const items = $derived<Command[]>(actions.more.map((a) => ({ id: `asset.${a}`, label: ACTION_LABELS[a], group: 'Go to', icon: ICONS[a], run: () => run(a) })));
</script>

<!-- svelte-ignore a11y_no_static_element_interactions, a11y_click_events_have_key_events (it only keeps its clicks and keys from the row under it) -->
<span class="asset-actions" data-testid="asset-actions" onclick={(e) => e.stopPropagation()} onkeydown={(e) => e.stopPropagation()}>
  <button type="button" class="btn small act" class:primary={actions.primary !== 'view'} data-action={actions.primary} disabled={actions.primary === 'claim' && busy} onclick={() => run(actions.primary)}><Icon name={ICONS[actions.primary]} size={14} /> {t(ACTION_LABELS[actions.primary])}</button>
  {#if items.length}<MenuButton label={t('More actions for %1', assetTextOf(id, label))} sections={[{ title: 'Actions', items }]} />{/if}
</span>

<style>
  .asset-actions {
    display: inline-flex;
    align-items: center;
    gap: 0.25rem;
    flex: none;
  }
  .act {
    white-space: nowrap;
  }
  .asset-actions :global(.trigger) {
    min-width: 2rem;
    min-height: 2rem;
  }
  /* Touch: targets of 2.75rem. */
  @media (pointer: coarse) {
    .act,
    .asset-actions :global(.trigger) {
      min-height: 2.75rem;
      min-width: 2.75rem;
    }
  }
</style>
