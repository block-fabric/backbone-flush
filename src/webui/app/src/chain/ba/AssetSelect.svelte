<script lang="ts">
  // Choosing an asset (ECX first): each by its id, its number first and its label quoted, since labels may
  // look alike, and even hold another asset's number (audit 7 E-BA4), while numbers never do. Among hundreds:
  // the searchable picker (AssetPicker.svelte: type a name or #number, favourites and the last picked on top).
  import AssetPicker from './AssetPicker.svelte';
  import { CHN_ID } from './model';
  import type { PickChoice } from './picker';
  import { TICKER } from '../config';

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
    choices: readonly PickChoice[];
    /** ECX among the choices, first. */
    chn?: boolean;
    disabled?: boolean;
    placeholder?: string;
  } = $props();
  const list = $derived(chn ? [{ id: CHN_ID, label: TICKER, seq: '' } as PickChoice, ...choices.filter((c) => c.id !== CHN_ID)] : choices);
</script>

<AssetPicker {id} {label} bind:value choices={list} {disabled} placeholder={placeholder || undefined} />
