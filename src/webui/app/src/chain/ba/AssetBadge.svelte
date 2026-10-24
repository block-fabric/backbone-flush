<script lang="ts">
  // An asset as the pages show it, the one identity of the app: LOGO, its name, then its ticker a little smaller and
  // muted ("[logo] Theodore Brosevelt $BROSEVELT"). The logo is the chain's (an <img> of a data: URL the host checked:
  // logos.svelte.ts), else a round mark in the hue of its hash with its initials (the same everywhere, so that two
  // assets of names alike are told apart at a glance); ECX in the colour of the chain. The words are AssetName.svelte's,
  // whose header says the rules: the name, then the identity in a muted mark of its own ($TICKER and its number, else
  // the start of its hash), so that a name cannot pass for another asset's ticker or number. The name gives way (an ellipsis) and the full text is in its title. showLabel={false}:
  // the mark alone.
  import { css } from '../../lib/actions';
  import { assetHue, assetIdentity, initials, isChn } from './model';
  import { TICKER } from '../config';
  import AssetName from './AssetName.svelte';
  import { isLogoUrl, logoHashOf, logoUrl } from './logos.svelte';
  import { displayNameOf } from './store.svelte';

  let {
    id,
    label,
    seq = '',
    size = 'm',
    showLabel = true,
    logo,
    preview = '',
    display,
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
    /** Its display name (text of the chain); undefined: the one the store knows; '': none shown. */
    display?: string;
  } = $props();

  const shownDisplay = $derived(display ?? displayNameOf(id) ?? '');
  const parts = $derived(assetIdentity(label, shownDisplay, seq));

  const url = $derived(isChn(id) ? '' : isLogoUrl(preview) ? preview : logoUrl(id, logo ?? logoHashOf(id)));
</script>

<span class="badge-asset {size}" class:mark-only={!showLabel}>
  {#if url}
    <img class="mark logo" src={url} alt="" width="64" height="64" decoding="async" draggable="false" referrerpolicy="no-referrer" aria-hidden="true" />
  {:else}
    <span class="mark" class:chn={isChn(id)} use:css={{ '--ba-h': assetHue(id) }} aria-hidden="true"><span translate="no">{isChn(id) ? TICKER.charAt(0) : initials(parts.ticker || parts.name || label)}</span></span>
  {/if}
  {#if showLabel}
    <span class="words"><AssetName {label} display={shownDisplay} {seq} {id} nameClass="label" /></span>
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
  /* The mark alone never gives way (a row of flex items would squeeze it under its neighbour). */
  .mark-only {
    flex: none;
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
    min-width: 0;
  }
  .words :global(.label) {
    font-weight: 600;
  }
  .l .words :global(.label) {
    font-size: 1.6rem;
    font-weight: 700;
  }
  .l .words :global(.asset-name) {
    flex-wrap: wrap;
    white-space: normal;
  }
  .l .words :global(.ident) {
    font-size: 1.05rem;
  }
</style>
