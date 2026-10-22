<script lang="ts">
  // TEMPORARY (doc/photon-slot.md): a strip above every page while the node runs a temporary network (Flush in
  // betanet's slot 99, Photon's). The node says so as a warning that starts with "Temporary:" (TemporaryNotice).
  import { node } from '../lib/state.svelte';
  import SafeText from './SafeText.svelte';

  const notice = $derived(node.warnings.find((w) => w.startsWith('Temporary:')));
</script>

{#if notice}
  <div class="temporary" role="status"><SafeText text={notice} /></div>
{/if}

<style>
  .temporary {
    padding: 0.4rem 1rem;
    background: #b45309;
    color: #fff;
    font-weight: 600;
    text-align: center;
  }
</style>
