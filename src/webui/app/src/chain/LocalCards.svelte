<script lang="ts">
  // The cards of Layers on the home of the dashboard: FastBlocks' (fb/Cards.svelte: Staking, Finality, Next slots)
  // and BitAssets' (ba/Cards.svelte: My assets, Markets, Auctions). Each opens its page.
  import BaCards from './ba/Cards.svelte';
  import FbCards from './fb/Cards.svelte';
</script>

<div class="layers-cards">
  <FbCards />
  <BaCards />
</div>

<style>
  .layers-cards {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
</style>
