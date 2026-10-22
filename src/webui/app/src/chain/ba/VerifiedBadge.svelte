<script lang="ts">
  // "Verified" next to an asset's name (Layers' DEX: verified issuers, ba/verify.svelte.ts) when a curator the user trusts
  // verifies it now (getverifications), its tooltip naming them; nothing when the user trusts no curator, or none of them
  // verifies it. Asked again with the blocks, at most once a minute per asset.
  import Icon from '../../components/Icon.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { isChn } from './model';
  import { ba } from './store.svelte';
  import { verifiedBy } from './verify';
  import { ensureVerification, loadCurators, verificationOf, verify } from './verify.svelte';

  let { id }: { id: string } = $props();

  $effect(() => {
    void loadCurators();
  });
  $effect(() => {
    void verify.curators;
    void ba.version;
    if (id && !isChn(id)) ensureVerification(id);
  });
  const by = $derived(id && !isChn(id) ? verifiedBy(verificationOf(id)) : []);
</script>

{#if by.length > 0}
  <span class="ba-tag accent verified" data-testid="verified-badge" title={t('Verified by %1', by.join(', '))}><Icon name="check" size={12} /> Verified</span>
{/if}

<style>
  .verified {
    vertical-align: middle;
  }
</style>
