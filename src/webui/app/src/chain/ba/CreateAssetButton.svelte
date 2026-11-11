<script lang="ts">
  // "Create asset", top right of Assets (owner decision, 2026-11-11; Launch has its own): to the one create wizard
  // (pages/Launch.svelte, its tab "create").
  import Icon from '../../components/Icon.svelte';
  import { go } from '../../lib/nav.svelte';
</script>

<button type="button" class="btn small primary create-asset" data-testid="create-asset" onclick={() => go('launch', 'create')}><Icon name="rocket" size={15} /> Create asset</button>

<style>
  .create-asset {
    margin-inline-start: auto;
  }
</style>
