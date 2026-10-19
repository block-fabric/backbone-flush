<script lang="ts">
  // The cards of Layers on the home of the dashboard: FastBlocks' (fb/Cards.svelte: Staking, Finality, Next slots)
  // and BitAssets' (ba/Cards.svelte: My assets, Markets, Auctions). Each opens its page.
  // Above them, what the wallet holds (ba/HeldAssets.svelte: Your assets, the same as on the Overview).
  import BaCards from './ba/Cards.svelte';
  import FbCards from './fb/Cards.svelte';
  import HeldAssets from './ba/HeldAssets.svelte';
  import { wallets } from '../lib/state.svelte';
</script>

<div class="layers-cards">
  {#if wallets.current !== null}<HeldAssets />{/if}
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
