<script lang="ts">
  // An asset's identity in words, the one way the pages write it (AssetBadge.svelte puts its logo before it): its name,
  // then its ticker after it, a little smaller and muted: "Theodore Brosevelt $BROSEVELT" (model.ts: assetIdentity).
  // - A label beside a display name is its ticker, whatever its shape (before the rules of tickers, "GOLD-BAR").
  // - No ticker (a name from before tickers, "gold bar"): the name, its number muted after it ("gold bar #1739-0029"),
  //   the number being what tells it apart (names are not unique).
  // - No name: "$GOLD" alone (and its number, faint, where the caller asks for it: seq).
  // - Neither (a label that is a hash, a private name): its number, else the start of its hash.
  // - ECX: its ticker alone, the one name the pages give the coin (config.ts: TICKER).
  // With a ticker, the number is shown only where the caller passes it (seq), faint, after the ticker.
  // The name gives way first (an ellipsis), the ticker and the number stay whole; the full text is in the title. All text
  // of the chain: SafeText. The space between the parts is an expression, which the compiler keeps (user feedback
  // 2026-10-19: "GOLDGold Bar Token"), so that the words are apart when copied and read out.
  import SafeText from '../../components/SafeText.svelte';
  import { TICKER } from '../config';
  import { assetIdentity, assetTitle } from './model';

  let {
    label,
    display = '',
    seq = '',
    nameClass = 'name',
  }: {
    /** The label of the chain (a ticker, an older name, or "0x" and a hash). */
    label: string;
    /** The display name: text of the chain; '' or undefined: none. */
    display?: string;
    /** Its number (1739-0029): shown after a name without ticker always, after a ticker when given. */
    seq?: string;
    nameClass?: string;
  } = $props();

  const coin = $derived(label === TICKER && !display);
  const a = $derived(assetIdentity(label, display, seq));
  // The first part (bold) and the second (muted), as the rules above say.
  const first = $derived(coin ? TICKER : a.name || (a.ticker ? `$${a.ticker}` : a.seq ? `#${a.seq}` : label));
  const second = $derived(coin ? '' : a.name && a.ticker ? `$${a.ticker}` : a.name && a.seq ? `#${a.seq}` : '');
  const third = $derived(!coin && a.ticker && a.seq ? `#${a.seq}` : '');
  const full = $derived(coin ? TICKER : assetTitle(label, display, seq));
</script>

<span class="asset-name" title={full} data-testid="asset-name"><SafeText class="{nameClass} first" text={first} />{#if second}{' '}<SafeText class="second" text={second} />{/if}{#if third}{' '}<span class="third" translate="no">{third}</span>{/if}</span>

<style>
  .asset-name {
    display: inline-flex;
    align-items: baseline;
    gap: 0.35em;
    min-width: 0;
    max-width: 100%;
    white-space: nowrap;
  }
  .asset-name :global(.first) {
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .asset-name :global(.second) {
    flex: none;
    font-size: 0.85em;
    font-weight: 500;
    color: var(--text-muted);
  }
  .third {
    flex: none;
    font-size: 0.8em;
    font-weight: 400;
    color: var(--text-muted);
  }
</style>
