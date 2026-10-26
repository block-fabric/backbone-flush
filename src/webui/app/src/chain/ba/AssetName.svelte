<script lang="ts">
  // An asset's identity in words, the one way the pages write it (AssetBadge.svelte puts its logo before it): its name,
  // the human label, then its identity, a little smaller and muted in a mark of its own: its $TICKER and the part no
  // other asset has, its number ("Theodore Brosevelt [$BROSEVELT #1739-0029]"; model.ts: assetIdentity, uniquePart).
  // A display name is not unique and may hold "$ECX" or "#1739-0029" to pass for another asset (security review
  // 2026-10-24): it is always followed by the real ticker and number, which are in their mark, never plain text like it.
  // - Only a label of the rules of tickers (model.ts isTicker: 2 to 12 of A to Z and 0 to 9) is shown as $TICKER, in the
  //   mark; any other ("gold bar", "EC X", 'ECX"', a look-alike letter) is plain text outside it, as a name (beside a
  //   display name: after it, muted).
  // - No ticker (a name from before tickers, "gold bar"): the name, then its number.
  // - No name: its $TICKER and number alone, in the place of the name.
  // - The number: the caller's (seq), else the store's (by id), else its id shortened (model.ts shortId: its start and
  //   end, 64 bits); none known: the label.
  // - ECX: its ticker alone, the one name the pages give the coin (config.ts: TICKER).
  // - A name is shown in NFKC, what is not plain in it boxed and escaped (model.ts nameSegments, the allowlist shared with
  //   the site: letters, marks on them, space, . , ' - & ( ) ! ? : ; and short digit runs plain; anything else, and runs
  //   of 4 digits or more, marked), so that its text, copied or read out, is the one assetTitle gives.
  // The name gives way first (an ellipsis), the identity stays whole; the full text is in the title (assetTitle: the
  // name escaped). All text of the chain: SafeText (translate="no", dir="auto": a bidirectional mark stays inside it).
  // The space between the parts is an expression, which the compiler keeps (user feedback 2026-10-19: "GOLDGold Bar
  // Token"), so that the words are apart when copied and read out.
  import SafeText from '../../components/SafeText.svelte';
  import { TICKER } from '../config';
  import { assetIdentity, assetTitle, escapePiece, nameSegments, uniquePart } from './model';
  import { assetById } from './store.svelte';

  let {
    label,
    display = '',
    seq = '',
    id = '',
    nameClass = 'name',
  }: {
    /** The label of the chain (a ticker, an older name, or "0x" and a hash). */
    label: string;
    /** The display name: text of the chain; '' or undefined: none. */
    display?: string;
    /** Its number (1739-0029); '': the store's, by id. */
    seq?: string;
    /** Its id (hex), for its number from the store, else its id shortened. */
    id?: string;
    nameClass?: string;
  } = $props();

  const coin = $derived((label === TICKER && !display) || (id !== '' && /^0+$/.test(id)));
  const number = $derived(seq || (id ? (assetById(id)?.seq ?? '') : ''));
  const a = $derived(assetIdentity(label, display, number));
  // Shown as "Name | TICKER | #number" (user feedback 2026-10-26): the ticker bare, the bars drawn (CSS), never a
  // "|" of text, so that a name holding one cannot pass for them. The "$" stays in the text, hidden from the eye: copied
  // text and screen readers keep "Name $TICKER #number" (security review 2026-10-26). Confirmations: assetTitle.
  const ticker = $derived(a.ticker ?? '');
  const unique = $derived(uniquePart(number, id));
  const full = $derived(coin ? TICKER : assetTitle(label, display, number, id));
</script>

<!-- A name, what in it is not plain marked and escaped (model.ts nameSegments, escapePiece). -->
{#snippet marked(text: string)}{#each nameSegments(text) as seg, i (i)}{#if seg.flag}<mark class="lookalike" title="Part of the name, not of the identity"><SafeText text={escapePiece(seg.text)} /></mark>{:else}<SafeText text={seg.text} />{/if}{/each}{/snippet}

<span class="asset-name" title={full} data-testid="asset-name">{#if coin}<SafeText class="{nameClass} first" text={TICKER} />{:else}{#if a.name}<span class="{nameClass} first" translate="no" dir="auto">{@render marked(a.name)}</span>{' '}{/if}{#if a.alias}<span class="alias" translate="no" dir="auto">{@render marked(a.alias)}</span>{' '}{/if}{#if ticker || unique}{#if a.name || a.alias}<span class="sep" aria-hidden="true"></span>{/if}<span class="ident" class:alone={!a.name} class:first={!a.name}>{#if ticker}<span class="vh">$</span><SafeText class="ticker {a.name ? '' : nameClass}" text={ticker} />{/if}{#if ticker && unique}<span class="sep" aria-hidden="true"></span>{' '}{/if}{#if unique}<span class="unique" translate="no">{unique}</span>{/if}</span>{:else if !a.name}<SafeText class="{nameClass} first" text={label} />{/if}{/if}</span>

<style>
  /* Too narrow for both: the identity goes under the name (never cut: it is what tells the asset apart), the name
     keeping a few letters before it does. */
  .asset-name {
    display: inline-flex;
    flex-wrap: wrap;
    align-items: baseline;
    gap: 0 0.4em;
    min-width: 0;
    max-width: 100%;
    white-space: nowrap;
  }
  .asset-name :global(.first) {
    flex: 0 1 auto;
    min-width: min(6em, 100%);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  /* The identity: the ticker and the number in a mark of their own, so that a "$" or "#" in a name (plain text) cannot
     pass for them. */
  /* Visually hidden, still in the text (copy, screen readers). */
  .vh {
    position: absolute;
    width: 1px;
    height: 1px;
    overflow: hidden;
    clip-path: inset(50%);
    white-space: nowrap;
  }
  /* The bars between name, ticker and number: drawn, not text. */
  .sep {
    flex: none;
    display: inline-block;
    width: 1px;
    height: 0.95em;
    align-self: center;
    background: var(--line-strong);
  }
  .ident {
    flex: none;
    display: inline-flex;
    flex-wrap: wrap;
    max-width: 100%;
    align-items: baseline;
    gap: 0.4em;
    /* No box (user feedback 2026-10-26): the mark is told from a name by its own monospaced, muted type. */
    font-family: var(--font-mono);
    font-size: 0.78em;
    font-weight: 500;
    color: var(--text-muted);
    line-height: 1.35;
  }
  /* No name: the identity is the asset's words, in their place. */
  .ident.alone {
    flex: 0 1 auto;
    font-size: 0.85em;
  }
  .ident.alone :global(.ticker) {
    color: var(--text);
    font-weight: 600;
  }
  .asset-name :global(.alias) {
    flex: 0 1 auto;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    color: var(--text-muted);
    font-size: 0.85em;
  }
  /* In a name, what could pass for the identity: boxed, so that it reads as part of the name. */
  .lookalike {
    padding: 0 0.12em;
    border: 1px dashed var(--warning, var(--line-strong));
    border-radius: 3px;
    background: none;
    color: inherit;
  }
  .unique {
    font-variant-numeric: tabular-nums;
    font-weight: 400;
  }
</style>
