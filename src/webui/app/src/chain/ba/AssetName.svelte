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
  // - A name is shown in NFKC, what is not plain in it boxed and escaped (model.ts nameSegments, the rule shared with the
  //   site: an allowlist, and on top of it what reads as an id or a number, and bar-like letters), so that its text,
  //   copied or read out, is the one assetTitle gives. Too long, it is cut at a piece's end, with a fade, not a "…".
  // The name gives way first (cut, a fade), the identity stays whole; the full text is in the title (assetTitle: the
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
    stacked = false,
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
    /** The identity under the name (My launches): no bar before it, the bar between ticker and number kept. */
    stacked?: boolean;
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

  // A name too long for its place is cut where a word or a marked piece ends (they wrap to a hidden line: a marked piece
  // is never split), the cut drawn by a fade, never a "…" of text (a name's text could pose as an id's "0x…").
  function cutMark(node: HTMLElement) {
    const check = () => node.classList.toggle('cut', node.scrollHeight > node.clientHeight + 1);
    check();
    const observer = typeof ResizeObserver === 'undefined' ? null : new ResizeObserver(check);
    observer?.observe(node);
    return { destroy: () => observer?.disconnect() };
  }
</script>

<!-- A name, what in it is not plain marked and escaped (model.ts nameSegments, escapePiece). -->
{#snippet marked(text: string)}{#each nameSegments(text) as seg, i (i)}{#if seg.flag}<mark class="lookalike" title="Part of the name, not of the identity"><SafeText text={escapePiece(seg.text)} /></mark>{:else}<SafeText text={seg.text} />{/if}{/each}{/snippet}

<span class="asset-name" class:stacked title={full} data-testid="asset-name">{#if coin}<SafeText class="{nameClass} first" text={TICKER} />{:else}{#if a.name}<span class="{nameClass} first name-text" translate="no" dir="auto" use:cutMark>{@render marked(a.name)}</span>{' '}{/if}{#if a.alias}<span class="alias name-text" translate="no" dir="auto" use:cutMark>{@render marked(a.alias)}</span>{' '}{/if}{#if ticker || unique}<span class="ident" class:alone={!a.name} class:first={!a.name} class:after-name={!!(a.name || a.alias)}>{#if ticker}<span class="vh">$</span><SafeText class="ticker {a.name ? '' : nameClass}" text={ticker} />{/if}{#if ticker && unique}<span class="sep" aria-hidden="true"></span>{' '}{/if}{#if unique}<span class="unique" translate="no">{unique}</span>{/if}</span>{:else if !a.name}<SafeText class="{nameClass} first" text={label} />{/if}{/if}</span>

<style>
  /* Name, 0.4em, the bar, 0.4em, ticker. Too narrow for both: the identity goes under the name (never cut: it is what
     tells the asset apart). Clipped at its left edge only: the bar before an identity that wrapped to a line of its own
     falls outside it and is not drawn. */
  .asset-name {
    display: inline-flex;
    flex-wrap: wrap;
    align-items: baseline;
    gap: 0 0.8em;
    min-width: 0;
    max-width: 100%;
    white-space: nowrap;
    clip-path: inset(-1em -100vw -1em 0);
  }
  /* Stacked (My launches): the identity under the name, no bar before it. */
  .asset-name.stacked {
    flex-direction: column;
    align-items: flex-start;
    gap: 0.1em;
  }
  .asset-name :global(.first) {
    flex: 0 1 auto;
    min-width: 0;
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
    position: relative;
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
  /* The bar between the name and the identity: drawn in the gap before it (0.8em of the name, the identity's type being
     0.78 of it), so that it goes with the identity and is clipped when the identity starts a line. */
  .ident.after-name::before {
    content: '';
    position: absolute;
    left: calc(-0.4em / 0.78 - 0.5px);
    top: 50%;
    width: 1px;
    height: 1.2em;
    transform: translateY(-50%);
    background: var(--line-strong);
  }
  .asset-name.stacked .ident.after-name::before {
    display: none;
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
  /* A name: one line, cut between words or pieces (the rest wraps to a hidden line), no "…" of text. */
  .asset-name :global(.name-text) {
    white-space: normal;
    overflow-wrap: anywhere;
    text-overflow: clip;
    line-height: 1.4;
    max-height: 1.4em;
    overflow: hidden;
  }
  .asset-name :global(.name-text.cut) {
    mask-image: linear-gradient(to right, #000 calc(100% - 1.5em), transparent);
  }
  /* In a name, what is not plain: boxed, so that it reads as part of the name, and never split. */
  .lookalike {
    display: inline-block;
    white-space: nowrap;
    line-height: 1.15;
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
