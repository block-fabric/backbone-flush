<script lang="ts">
  // The curators the user trusts (Layers' DEX: verified issuers, ba/verify.svelte.ts): oracles that attest that an asset is
  // what it says. Added by id (checked by the node: getverifications names it, or says no such oracle is registered), or
  // among the oracles registered when the node lists them (listoracles); removed. Kept with the app's preferences.
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { errorText } from '../../lib/dialogs.svelte';
  import { t, tn } from '../../lib/i18n.svelte';
  import { rpc } from '../../lib/rpc';
  import { isCuratorId, type Verifications } from './verify';
  import { addCurator, loadCurators, removeCurator, verify } from './verify.svelte';

  interface Oracle {
    oracle: string;
    name: string;
  }

  let idText = $state('');
  let error = $state('');
  let busy = $state(false);
  /** The oracles registered (listoracles), when the node lets the pages list them; null: not. */
  let oracles = $state<Oracle[] | null>(null);

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

  /** Add a curator by its id: the node says its name, or that no oracle of that id is registered. */
  async function add(id: string) {
    const key = id.trim().toLowerCase();
    error = '';
    if (!isCuratorId(key)) {
      error = t('An oracle\'s id: 64 hexadecimal digits.');
      return;
    }
    busy = true;
    try {
      const v = await rpc.raw<Verifications>('getverifications', ['CHN', [key]]);
      const found = v?.curators?.find((c) => c.curator.toLowerCase() === key);
      addCurator(key, found?.name ?? '');
      idText = '';
    } catch (e) {
      error = errorText(e);
    } finally {
      busy = false;
    }
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
            <span class="main">{#if c.name}<SafeText text={c.name} />{:else}<span class="muted">Unnamed oracle</span>{/if}<span class="mono faint small break" translate="no">{c.id}</span></span>
            <button type="button" class="btn small ghost" aria-label={t('Stop trusting this curator')} onclick={() => removeCurator(c.id)}><Icon name="trash" size={14} /> Remove</button>
          </li>
        {/each}
      </ul>
    {/if}
    <form class="ba-inline" onsubmit={(e) => { e.preventDefault(); void add(idText); }}>
      <input id="curator-id" type="text" class="mono" placeholder="An oracle's id (64 hexadecimal digits)" aria-label="An oracle's id" spellcheck="false" autocomplete="off" bind:value={idText} />
      <button type="submit" class="btn small primary" disabled={busy || idText.trim() === ''}><Icon name="plus" size={14} /> Trust</button>
    </form>
    {#if error}<p class="ba-problems" role="status"><SafeText text={error} /></p>{/if}
    {#if others.length}
      <div class="field">
        <span class="label">Oracles registered</span>
        <ul class="ba-list" aria-label="Oracles registered">
          {#each others.slice(0, 50) as o (o.oracle)}
            <li class="curator">
              <span class="main"><SafeText text={o.name} /><span class="mono faint small break" translate="no">{o.oracle}</span></span>
              <button type="button" class="btn small" disabled={busy} onclick={() => add(o.oracle)}><Icon name="plus" size={14} /> Trust</button>
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
</style>
