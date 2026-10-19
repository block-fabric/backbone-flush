<script lang="ts">
  // Choosing an asset (CHN first): each by its id, its number first and its label quoted, since labels may
  // look alike, and even hold another asset's number (audit 7 E-BA4), while numbers never do.
  import AssetBadge from './AssetBadge.svelte';
  import { CHN_ID } from './model';

  interface Choice {
    id: string;
    /** Text of the chain. */
    label: string;
    seq?: string;
    /** What the wallet holds of it, written. */
    held?: string;
  }
  let {
    id,
    label,
    value = $bindable(''),
    choices,
    chn = true,
    disabled = false,
    placeholder = '',
  }: {
    id: string;
    label: string;
    value?: string;
    choices: readonly Choice[];
    /** CHN among the choices, first. */
    chn?: boolean;
    disabled?: boolean;
    placeholder?: string;
  } = $props();
  const current = $derived(choices.find((c) => c.id === value));
  const list = $derived(chn ? [{ id: CHN_ID, label: 'CHN', seq: '' } as Choice, ...choices.filter((c) => c.id !== CHN_ID)] : choices);
</script>

<div class="field asset-select">
  <label for={id}>{label}</label>
  <div class="pick">
  {#if value}<AssetBadge id={value} label={value === CHN_ID ? 'CHN' : (current?.label ?? '')} size="s" showLabel={false} />{/if}
  <select {id} bind:value {disabled}>
    {#if placeholder}<option value="" disabled>{placeholder}</option>{/if}
    {#each list as c (c.id)}
      <option value={c.id} translate="no">{c.seq ? `#${c.seq} · "${c.label}"` : c.label}{c.held ? ` · ${c.held}` : ''}</option>
    {/each}
  </select>
  </div>
</div>

<style>
  .pick {
    display: flex;
    align-items: center;
    gap: 0.5rem;
  }
  .pick select {
    flex: 1;
    min-width: 0;
  }
</style>
