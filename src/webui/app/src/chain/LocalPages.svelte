<script lang="ts">
  // The pages of Layers (chain/local.ts lists them): the one shown. Staking (FastBlocks' blocks of stake, in one
  // section), then BitAssets' assets and its DEX: Swap, Pools and Launch are one page component (pages/Trade.svelte),
  // kept while one leads to another (a tab at home in the other); Portfolio is My assets.
  import Assets from './pages/Assets.svelte';
  import MyAssets from './pages/MyAssets.svelte';
  import Staking from './pages/Staking.svelte';
  import Trade from './pages/Trade.svelte';
  let { page }: { page: string } = $props();
</script>

{#if page === 'staking'}<Staking />
{:else if page === 'assets'}<Assets />
{:else if page === 'my-assets'}<MyAssets />
{:else if page === 'trade' || page === 'pools' || page === 'launch'}<Trade {page} />
{/if}
