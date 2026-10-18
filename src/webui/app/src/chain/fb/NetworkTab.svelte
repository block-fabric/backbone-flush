<script lang="ts">
  // Staking, Network: the chain's validators and how far its blocks are final, in four views (one at a time): the
  // validator set of an epoch and every bond (ValidatorsTab), the leader schedule (ScheduleTab), finality and the
  // BMM blocks (FinalityView) and slashing and jail (SlashingView). The view is the tab of the deep link
  // (go('staking', 'finality')): chain/local.ts stakingTabOf.
  import { t } from '../../lib/i18n.svelte';
  import { nav } from '../../lib/nav.svelte';
  import type { NetworkView } from '../local';
  import FinalityView from './FinalityView.svelte';
  import ScheduleTab from './ScheduleTab.svelte';
  import SlashingView from './SlashingView.svelte';
  import ValidatorsTab from './ValidatorsTab.svelte';
  import { fb } from './store.svelte';

  let { view }: { view: NetworkView } = $props();

  const VIEWS: { id: NetworkView; label: string }[] = [
    { id: 'validators', label: 'Validator set' },
    { id: 'schedule', label: 'Leader schedule' },
    { id: 'finality', label: 'Finality & BMM' },
    { id: 'slashing', label: 'Slashing & jail' },
  ];
</script>

<div class="fb-subnav" role="tablist" aria-label="Network">
  {#each VIEWS as v (v.id)}
    <button type="button" role="tab" class="chip" aria-selected={view === v.id} id="fb-view-{v.id}" onclick={() => (nav.tab = v.id)}>
      {t(v.label)}
      {#if v.id === 'validators' && fb.set?.validators.length}<span class="badge">{fb.set.validators.length}</span>{/if}
      {#if v.id === 'slashing' && fb.pos?.evidence}<span class="badge">{fb.pos.evidence}</span>{/if}
    </button>
  {/each}
</div>

{#if view === 'validators'}
  <ValidatorsTab />
{:else if view === 'schedule'}
  <ScheduleTab />
{:else if view === 'finality'}
  <FinalityView />
{:else}
  <SlashingView />
{/if}

<style>
  .fb-subnav {
    display: flex;
    flex-wrap: wrap;
    gap: 0.4rem;
    margin: 0 0 1rem;
  }
  .chip {
    display: inline-flex;
    align-items: center;
    gap: 0.35rem;
    padding: 0.3rem 0.75rem;
    border: 1px solid var(--line);
    border-radius: 999px;
    background: transparent;
    color: var(--text);
    font: inherit;
    font-size: 0.88rem;
    cursor: pointer;
  }
  .chip[aria-selected='true'] {
    border-color: var(--accent);
    background: var(--accent-soft, transparent);
    color: var(--accent-fg);
    font-weight: 600;
  }
</style>
