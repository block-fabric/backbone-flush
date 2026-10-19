<script lang="ts">
  // An asset as the pages show it: its logo from the chain (an <img> of a data: URL the host checked: logos.svelte.ts),
  // else a round mark in the hue of its hash (the same everywhere, so that two assets of names alike are told apart at a
  // glance), its label, and its number. CHN in the colour of the chain.
  import SafeText from '../../components/SafeText.svelte';
  import { css } from '../../lib/actions';
  import { assetHue, initials, isChn } from './model';
  import { isLogoUrl, logoHashOf, logoUrl } from './logos.svelte';

  let {
    id,
    label,
    seq = '',
    size = 'm',
    showLabel = true,
    logo,
    preview = '',
  }: {
    id: string;
    /** Text of the chain. */
    label: string;
    seq?: string;
    size?: 's' | 'm' | 'l';
    showLabel?: boolean;
    /** The hash of its logo, when the caller knows it (else the list of assets says it). */
    logo?: string;
    /** A logo not on the chain yet (the picker's): a data: URL of PNG or JPEG bytes. */
    preview?: string;
  } = $props();

  const url = $derived(isChn(id) ? '' : isLogoUrl(preview) ? preview : logoUrl(id, logo ?? logoHashOf(id)));
</script>

<span class="badge-asset {size}">
  {#if url}
    <img class="mark logo" src={url} alt="" width="64" height="64" decoding="async" draggable="false" referrerpolicy="no-referrer" aria-hidden="true" />
  {:else}
    <span class="mark" class:chn={isChn(id)} use:css={{ '--ba-h': assetHue(id) }} aria-hidden="true"><span translate="no">{isChn(id) ? 'C' : initials(label)}</span></span>
  {/if}
  {#if showLabel}
    <span class="words">
      <SafeText class="label" text={label} />
      {#if seq}<span class="seq faint" translate="no">#{seq}</span>{/if}
    </span>
  {/if}
</span>

<style>
  .badge-asset {
    display: inline-flex;
    align-items: center;
    gap: 0.55rem;
    min-width: 0;
    max-width: 100%;
  }
  .mark {
    --ba-h: 160;
    flex: none;
    display: inline-grid;
    place-items: center;
    width: 2rem;
    height: 2rem;
    border-radius: 50%;
    border: 2px solid hsl(var(--ba-h) 62% 52%);
    background: hsl(var(--ba-h) 62% 52% / 0.16);
    color: var(--text);
    font-size: 0.72rem;
    font-weight: 750;
    letter-spacing: 0.02em;
  }
  .mark.logo {
    display: block;
    object-fit: cover;
    border: 1px solid var(--line-strong);
    background: var(--surface-2);
    padding: 0;
  }
  .mark.chn {
    border-color: var(--brand, var(--accent));
    background: var(--accent-soft);
  }
  .s .mark {
    width: 1.5rem;
    height: 1.5rem;
    font-size: 0.6rem;
    border-width: 1.5px;
  }
  .s .mark.logo,
  .l .mark.logo {
    border-width: 1px;
  }
  .l .mark {
    width: 3.2rem;
    height: 3.2rem;
    font-size: 1.05rem;
    border-width: 2.5px;
  }
  .words {
    display: inline-flex;
    align-items: baseline;
    gap: 0.4rem;
    min-width: 0;
  }
  .words :global(.label) {
    font-weight: 600;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    min-width: 0;
  }
  .l .words :global(.label) {
    font-size: 1.6rem;
    font-weight: 700;
  }
  .seq {
    font-size: 0.8rem;
    white-space: nowrap;
  }
</style>
