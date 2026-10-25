<script lang="ts">
  // The loud risk marker of a launch whose creator kept the control coin (v5 keep_control): "Mintable: the creator can
  // create more", wherever the launch or its asset shows (its page, Browse assets, the pickers, My launches). mintable:
  // the page's own word (a launch read); else the store's, by id (ba/store.svelte.ts mintableLaunch). compact: the
  // word alone, the sentence in its title (the pickers).
  import Icon from '../../components/Icon.svelte';
  import { mintableLaunch } from './store.svelte';

  let { id = '', mintable, compact = false }: { id?: string; mintable?: boolean; compact?: boolean } = $props();
  const shown = $derived(mintable ?? mintableLaunch[id] === true);
</script>

{#if shown}<span class="ba-tag danger mintable" data-testid="mintable" title="Mintable: the creator can create more" role="note"><Icon name="alert" size={12} /> {#if compact}Mintable{:else}Mintable: the creator can create more{/if}</span>{/if}

<style>
  .mintable {
    display: inline-flex;
    align-items: center;
    gap: 0.25rem;
    flex: none;
    font-weight: 700;
    white-space: nowrap;
  }
</style>
