<script lang="ts">
  // The small tag of an asset whose supply is not locked: a launch whose creator kept the control coin (v5
  // keep_control, the node's "mintable"), or an asset that is not of a fixed supply. Shown on the asset's own pages
  // only (its page, its launch, its pool) and in the buy and swap confirmations; not in lists. mintable: the page's own
  // word (a launch read); else the store's, by id (ba/store.svelte.ts mintableLaunch).
  import { mintableLaunch } from './store.svelte';

  let { id = '', mintable }: { id?: string; mintable?: boolean } = $props();
  const shown = $derived(mintable ?? mintableLaunch[id] === true);
</script>

{#if shown}<span class="ba-tag" data-testid="unlocked-supply" title="The creator can still mint more">Unlocked Supply</span>{/if}
