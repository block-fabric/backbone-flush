<script lang="ts">
  // What BitAssets keeps doing on every page from the start of the app (views/Shell.svelte): the assets being created
  // are registered by themselves once their reservation is deep enough (ba/store.svelte.ts), after a restart too, and
  // their progress shows in the top bar (lib/pending.svelte.ts).
  import { startBitAssets } from './ba/store.svelte';

  $effect(() => startBitAssets());
</script>
