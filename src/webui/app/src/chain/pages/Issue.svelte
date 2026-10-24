<script lang="ts">
  // Register an asset: the create wizard (ba/CreateWizard.svelte), its market "none" at first: the same wizard as a
  // launch's (user decision, 2026-10-22: one wizard for both), in Launch (go('issue') leads there: chain/local.ts tabOf).
  import CreateWizard from '../ba/CreateWizard.svelte';

  /** Shown as a tab of Launch, under its heading. */
  let { embedded = false }: { embedded?: boolean } = $props();
</script>

<CreateWizard {embedded} market="none" />
