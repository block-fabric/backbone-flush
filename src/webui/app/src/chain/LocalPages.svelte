<script lang="ts">
  // The pages of Layers (chain/local.ts lists them): the one shown. Staking (FastBlocks' blocks of stake, in one
  // section), then BitAssets' assets and its DEX (owner decision, 2026-11-11): Portfolio (pages/MyAssets.svelte), Assets,
  // Swap, Pools and Launch, a page component each.
  import Assets from './pages/Assets.svelte';
  import MyAssets from './pages/MyAssets.svelte';
  import Staking from './pages/Staking.svelte';
  import Launch from './pages/Launch.svelte';
  import Pools from './pages/Pools.svelte';
  import Swap from './pages/Swap.svelte';
  let { page }: { page: string } = $props();
</script>

{#if page === 'staking'}<Staking />
{:else if page === 'assets'}<Assets />
{:else if page === 'my-assets'}<MyAssets />
{:else if page === 'trade'}<Swap />
{:else if page === 'pools'}<Pools />
{:else if page === 'launch'}<Launch />
{/if}
