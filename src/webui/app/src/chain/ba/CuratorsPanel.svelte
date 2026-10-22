<script lang="ts">
  // The curators the user trusts (Layers' DEX: verified issuers, ba/verify.svelte.ts): oracles that attest that an asset is
  // what it says. A curator IS its id: a name is a label anyone may give an oracle. Added by id (pasted, or picked among the
  // oracles registered when the node lists them: listoracles), always through a confirmation that shows the whole id and
  // warns of names that read like another oracle's or a trusted curator's (curatorWarnings); removed. Kept with the app's
  // preferences.
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { errorText } from '../../lib/dialogs.svelte';
  import { t, tn } from '../../lib/i18n.svelte';
  import { rpc } from '../../lib/rpc';
  import { curatorWarnings, idPrefix, isCuratorId, type OracleSeen, type Verifications } from './verify';
  import { addCurator, loadCurators, removeCurator, verify } from './verify.svelte';
  import { TICKER } from '../config';

  interface Oracle {
    oracle: string;
    name: string;
  }

  let idText = $state('');
  let error = $state('');
  let busy = $state(false);
  /** The oracles registered (listoracles), when the node lets the pages list them; null: not. */
  let oracles = $state<Oracle[] | null>(null);
  /** The oracle about to be trusted: shown whole, with its warnings, until confirmed or cancelled. */
  let pending = $state<{ oracle: string; name: string; warnings: string[] } | null>(null);

  $effect(() => {
    void loadCurators();
  });
  $effect(() => {
    let live = true;
    rpc
      .raw<Oracle[]>('listoracles', [])
      .then((list) => {
        if (live) oracles = Array.isArray(list) ? list.filter((o) => o && typeof o.oracle === 'string') : null;
      })
      .catch(() => {
        if (live) oracles = null;
      });
    return () => {
      live = false;
    };
  });
  const trusted = $derived(new Set(verify.curators.map((c) => c.id)));
  const others = $derived((oracles ?? []).filter((o) => !trusted.has(o.oracle.toLowerCase())));

  /** Ask to trust an oracle by its id: the node says its name (or that no oracle of that id is registered); then confirm. */
  async function ask(id: string) {
    const key = id.trim().toLowerCase();
    error = '';
    pending = null;
    if (!isCuratorId(key)) {
      error = t('An oracle\'s id: 64 hexadecimal digits.');
      return;
    }
    busy = true;
    try {
      const v = await rpc.raw<Verifications>('getverifications', [TICKER, [key]]);
      const found = v?.curators?.find((c) => c.curator.toLowerCase() === key);
      const candidate: OracleSeen = { oracle: key, name: found?.name ?? '' };
      pending = { ...candidate, warnings: curatorWarnings(candidate, oracles ?? [], verify.curators) };
    } catch (e) {
      error = errorText(e);
    } finally {
      busy = false;
    }
  }
  function confirm() {
    if (!pending) return;
    addCurator(pending.oracle, pending.name);
    pending = null;
    idText = '';
  }
</script>

<details class="panel curators" aria-label="Verified issuers">
  <summary><Icon name="shield" size={15} /> Verified issuers <span class="muted small">{verify.curators.length ? tn('%n curator(s) trusted', verify.curators.length) : t('no curator trusted: no badges')}</span></summary>
  <div class="body">
    <p class="ba-help">Curators are oracles that attest that an asset is what it says (its issuer, its backing). Assets that a curator you trust verifies show a Verified badge, here, in the order book and on their page. Not a rule of the chain: only what this app shows.</p>
    {#if verify.curators.length}
      <ul class="ba-list" aria-label="Curators trusted">
        {#each verify.curators as c (c.id)}
          <li class="curator">
            <span class="main"><span class="mono break" translate="no">{c.id}</span>{#if c.name}<span class="faint small"><SafeText text={c.name} /></span>{:else}<span class="muted small">Unnamed oracle</span>{/if}</span>
            <button type="button" class="btn small ghost" aria-label={t('Stop trusting this curator')} onclick={() => removeCurator(c.id)}><Icon name="trash" size={14} /> Remove</button>
          </li>
        {/each}
      </ul>
    {/if}
    <form class="ba-inline" onsubmit={(e) => { e.preventDefault(); void ask(idText); }}>
      <input id="curator-id" type="text" class="mono" placeholder="An oracle's id (64 hexadecimal digits)" aria-label="An oracle's id" spellcheck="false" autocomplete="off" bind:value={idText} />
      <button type="submit" class="btn small primary" disabled={busy || idText.trim() === ''}><Icon name="plus" size={14} /> Trust</button>
    </form>
    {#if error}<p class="ba-problems" role="status"><SafeText text={error} /></p>{/if}
    {#if pending}
      <div class="confirm panel" role="dialog" aria-label="Trust this oracle?" data-testid="curator-confirm">
        <strong>Trust this oracle as a curator?</strong>
        <span class="muted small">Its id: what it is (compare it with the one its curator publishes)</span>
        <code class="mono break" translate="no">{pending.oracle}</code>
        <span class="muted small">Its name: a label anyone may give an oracle</span>
        <span>{#if pending.name}<SafeText text={pending.name} />{:else}<span class="muted">none</span>{/if}</span>
        {#each pending.warnings as w (w)}<p class="ba-problems" role="status">{t(w)}</p>{/each}
        <div class="ba-inline">
          <button type="button" class="btn small primary" onclick={confirm}><Icon name="check" size={14} /> Trust this id</button>
          <button type="button" class="btn small ghost" onclick={() => (pending = null)}>Cancel</button>
        </div>
      </div>
    {/if}
    {#if others.length}
      <div class="field">
        <span class="label">Oracles registered</span>
        <ul class="ba-list" aria-label="Oracles registered">
          {#each others.slice(0, 50) as o (o.oracle)}
            <li class="curator">
              <span class="main"><span class="mono" translate="no" title={o.oracle}>{idPrefix(o.oracle)}</span><span class="faint small"><SafeText text={o.name} /></span></span>
              <button type="button" class="btn small" disabled={busy} onclick={() => ask(o.oracle)}><Icon name="plus" size={14} /> Trust…</button>
            </li>
          {/each}
        </ul>
      </div>
    {/if}
  </div>
</details>

<style>
  .curators {
    padding: 0.8rem 1.1rem;
  }
  .curators summary {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    cursor: pointer;
    font-weight: 600;
  }
  .curators summary .small {
    font-weight: 400;
  }
  .body {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
    margin-top: 0.75rem;
  }
  .curator {
    display: flex;
    align-items: center;
    gap: 0.75rem;
    padding: 0.5rem 0;
  }
  .curator .main {
    display: flex;
    flex-direction: column;
    gap: 0.1rem;
    flex: 1 1 auto;
    min-width: 0;
  }
  .small {
    font-size: 0.85rem;
  }
  .confirm {
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
    padding: 0.8rem 1rem;
  }

</style>
