<script lang="ts">
  // The pages of Layers (chain/local.ts lists them): the one shown. FastBlocks' blocks of stake, then BitAssets' assets.
  import Assets from './pages/Assets.svelte';
  import Auctions from './pages/Auctions.svelte';
  import Finality from './pages/Finality.svelte';
  import Issue from './pages/Issue.svelte';
  import MyAssets from './pages/MyAssets.svelte';
  import Slashing from './pages/Slashing.svelte';
  import Staking from './pages/Staking.svelte';
  import Trade from './pages/Trade.svelte';
  let { page }: { page: string } = $props();
</script>

{#if page === 'staking'}<Staking />
{:else if page === 'finality'}<Finality />
{:else if page === 'slashing'}<Slashing />
{:else if page === 'assets'}<Assets />
{:else if page === 'my-assets'}<MyAssets />
{:else if page === 'issue'}<Issue />
{:else if page === 'trade'}<Trade />
{:else if page === 'auctions'}<Auctions />
{/if}
