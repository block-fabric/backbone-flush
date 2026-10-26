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
  // The name gives way first (an ellipsis), the identity stays whole; the full text is in the title (assetTitle: the
  // name escaped). All text of the chain: SafeText (translate="no", dir="auto": a bidirectional mark stays inside it).
  // The space between the parts is an expression, which the compiler keeps (user feedback 2026-10-19: "GOLDGold Bar
  // Token"), so that the words are apart when copied and read out.
  import SafeText from '../../components/SafeText.svelte';
  import { TICKER } from '../config';
  import { assetIdentity, assetTitle, uniquePart } from './model';
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
  const ticker = $derived(a.ticker ? `$${a.ticker}` : '');
  const unique = $derived(uniquePart(number, id));
  const full = $derived(coin ? TICKER : assetTitle(label, display, number, id));
</script>

<span class="asset-name" title={full} data-testid="asset-name">{#if coin}<SafeText class="{nameClass} first" text={TICKER} />{:else}{#if a.name}<SafeText class="{nameClass} first" text={a.name} />{' '}{/if}{#if a.alias}<SafeText class="alias" text={a.alias} />{' '}{/if}{#if ticker || unique}<span class="ident" class:alone={!a.name} class:first={!a.name}>{#if ticker}<SafeText class="ticker {a.name ? '' : nameClass}" text={ticker} />{/if}{#if ticker && unique}{' '}{/if}{#if unique}<span class="unique" translate="no">{unique}</span>{/if}</span>{:else if !a.name}<SafeText class="{nameClass} first" text={label} />{/if}{/if}</span>

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
  .ident {
    flex: none;
    display: inline-flex;
    flex-wrap: wrap;
    max-width: 100%;
    align-items: baseline;
    gap: 0.3em;
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
  .unique {
    font-variant-numeric: tabular-nums;
    font-weight: 400;
  }
</style>
