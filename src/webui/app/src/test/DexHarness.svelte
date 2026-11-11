<script lang="ts">
  // For the tests: the pages of the chain as the app shows them, the one of nav.page (chain/LocalPages.svelte), opened
  // on the page a route names (chain/local.ts dexRoute) as a link or a command opens it, so that a route that leads to
  // another page (a retired tab, an asset's page) is followed as in the app.
  import { untrack } from 'svelte';
  import LocalPages from '../chain/LocalPages.svelte';
  import { dexRoute } from '../chain/local';
  import { nav } from '../lib/nav.svelte';

  let { page = 'trade' }: { page?: string } = $props();
  untrack(() => {
    const to = page === 'assets' || page === 'my-assets' ? { page, tab: nav.tab } : dexRoute(page, nav.tab);
    nav.page = to.page as typeof nav.page;
    nav.tab = to.tab;
  });
</script>

<LocalPages page={nav.page} />
