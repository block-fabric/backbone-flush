<script lang="ts">
  // The pages of BitAssets (chain/local.ts lists them): the one shown.
  import Assets from './pages/Assets.svelte';
  import Auctions from './pages/Auctions.svelte';
  import Issue from './pages/Issue.svelte';
  import MyAssets from './pages/MyAssets.svelte';
  import Trade from './pages/Trade.svelte';
  let { page }: { page: string } = $props();
</script>

{#if page === 'assets'}<Assets />
{:else if page === 'my-assets'}<MyAssets />
{:else if page === 'issue'}<Issue />
{:else if page === 'trade'}<Trade />
{:else if page === 'auctions'}<Auctions />
{/if}
