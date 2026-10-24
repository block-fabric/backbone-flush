<script lang="ts">
  // "Create asset", top right of Swap, Markets, Portfolio and Browse assets (user decision, 2026-10-24): always there,
  // to the page of the creation wizard (Create asset: chain/local.ts, the page launch), by its tab (fund): a page of the
  // DEX left with no tab would take the user back to itself (pages/Trade.svelte).
  import Icon from '../../components/Icon.svelte';
  import { go } from '../../lib/nav.svelte';
</script>

<button type="button" class="btn small primary create-asset" data-testid="create-asset" onclick={() => go('launch', 'fund')}><Icon name="rocket" size={15} /> Create asset</button>

<style>
  .create-asset {
    margin-inline-start: auto;
  }
</style>
