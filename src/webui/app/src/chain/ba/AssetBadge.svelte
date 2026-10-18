<script lang="ts">
  // An asset as the pages show it: a round mark in the hue of its hash (the same everywhere, so that two assets
  // of names alike are told apart at a glance), its label, and its number. CHN in the colour of the chain.
  import SafeText from '../../components/SafeText.svelte';
  import { css } from '../../lib/actions';
  import { assetHue, initials, isChn } from './model';

  let {
    id,
    label,
    seq = '',
    size = 'm',
    showLabel = true,
  }: {
    id: string;
    /** Text of the chain. */
    label: string;
    seq?: string;
    size?: 's' | 'm' | 'l';
    showLabel?: boolean;
  } = $props();
</script>

<span class="badge-asset {size}">
  <span class="mark" class:chn={isChn(id)} use:css={{ '--ba-h': assetHue(id) }} aria-hidden="true"><span translate="no">{isChn(id) ? 'C' : initials(label)}</span></span>
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
