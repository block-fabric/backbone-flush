<script lang="ts">
  // Staking, Network, Slashing & jail (the page of its own FastBlocks had): the rules a validator lives by, the penalties and rewards so far (getposinfo), the proofs this
  // node knows that a leader signed two blocks for one slot (listevidence: the validator, the slot, the two blocks,
  // the proof to copy), the validators jailed or banned now (listbonds with all, or the set's own word without a
  // wallet), and a proof given by hand (submitevidence: the node checks it, keeps it and relays it).
  import Amount from '../../components/Amount.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import Pager from '../../components/Pager.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { stacked } from '../../lib/actions';
  import { errorText, inform } from '../../lib/dialogs.svelte';
  import { formatCount, shortHash } from '../../lib/format';
  import { t } from '../../lib/i18n.svelte';
  import { go } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { node, wallets } from '../../lib/state.svelte';
  import { coinsToSats } from '../../lib/units';
  import './fb.css';
  import Key from './Key.svelte';
  import type { Evidence, WalletBond } from './model';
  import { fb, followTip, refreshFb, startFb } from './store.svelte';

  $effect(() => startFb());
  $effect(() => {
    void node.tip?.hash;
    followTip();
  });

  const SIZES = [10, 25, 50, 100];
  const pos = $derived(fb.pos);
  const epoch = $derived(pos?.epoch ?? 0);
  const localKeys = $derived(new Set(fb.staking?.keys.map((k) => k.pubkey) ?? []));

  let evidence = $state<Evidence[]>([]);
  let evidenceError = $state('');
  $effect(() => {
    void fb.version;
    rpc
      .raw<Evidence[]>('listevidence', [])
      .then((e) => {
        evidence = e.slice().sort((a, b) => b.slot - a.slot);
        evidenceError = '';
      })
      .catch((e) => (evidenceError = errorText(e)));
  });
  let evPage = $state(0);
  let evSize = $state(10);
  const evShown = $derived(evidence.slice(evPage * evSize, (evPage + 1) * evSize));

  // Jailed and banned: every bond in the state of the chain (a wallet is needed to ask), else the set's own word.
  let bonds = $state<WalletBond[] | null>(null);
  $effect(() => {
    const wallet = wallets.current;
    void fb.version;
    if (wallet === null) {
      bonds = null;
      return;
    }
    rpc
      .raw<WalletBond[]>('listbonds', [true], { wallet })
      .then((b) => (bonds = b))
      .catch(() => (bonds = null));
  });
  interface Held {
    key: string;
    banned: boolean;
    until: number;
    bonded?: number;
  }
  const held = $derived.by((): Held[] => {
    if (bonds) return bonds.filter((b) => b.banned || b.jaileduntilepoch > epoch).map((b) => ({ key: b.validator, banned: b.banned, until: b.jaileduntilepoch, bonded: b.bonded }));
    return (fb.set?.validators ?? []).filter((v) => v.banned || v.jaileduntilepoch > epoch).map((v) => ({ key: v.pubkey, banned: v.banned, until: v.jaileduntilepoch }));
  });
  let heldPage = $state(0);
  let heldSize = $state(10);
  const heldShown = $derived(held.slice(heldPage * heldSize, (heldPage + 1) * heldSize));

  let proof = $state('');
  let submitting = $state(false);
  async function submit() {
    submitting = true;
    try {
      const r = await rpc.raw<{ pubkey: string; slot: number; new: boolean }>('submitevidence', [proof.trim()]);
      await inform(t('Slashing'), r.new ? t('The node took the proof that %1 signed two blocks for slot %2, and relays it. A block of stake carries it, and the validator is slashed and banned.', shortHash(r.pubkey, 10), formatCount(r.slot)) : t('The node knew this proof already.'));
      proof = '';
      await refreshFb();
    } catch (e) {
      await inform(t('Slashing'), errorText(e), 'warning');
    } finally {
      submitting = false;
    }
  }
</script>

<div class="fb-view slashing">

  <section class="side-stats" aria-label="Penalties">
    <div class="panel side-stat">
      <span class="muted">Penalties so far</span>
      <strong class="num">{#if pos}<Amount sats={coinsToSats(pos.staking.slashed)} />{:else}–{/if}</strong>
      <span class="faint">{t('since the chain began')}</span>
    </div>
    <div class="panel side-stat">
      <span class="muted">Rewards owed to reporters</span>
      <strong class="num">{#if pos}<Amount sats={coinsToSats(pos.staking.rewardsowed)} />{:else}–{/if}</strong>
      <span class="faint">{t('to the blocks that carried the proofs')}</span>
    </div>
    <div class="panel side-stat">
      <span class="muted">Proofs this node knows</span>
      <strong class="num">{formatCount(evidence.length)}</strong>
      <span class="faint">{t('of a leader signing twice')}</span>
    </div>
    <div class="panel side-stat">
      <span class="muted">Jailed or banned now</span>
      <strong class="num">{formatCount(held.length)}</strong>
      <span class="faint">{bonds ? t('of every validator') : t('of the set of this epoch')}</span>
    </div>
  </section>

  <!-- The rules beside the proofs of double signing, as the row below (user decision, 2026-10-16). -->
  <div class="side-cols">
  <section class="panel rules" aria-labelledby="fb-rules-title">
    <div class="panel-head"><h2 id="fb-rules-title">The rules</h2></div>
    <div class="panel-body">
      <ul>
        <li><strong>{t('Jailed')}</strong> — {t('a validator that misses too many of the slots it leads in an epoch is left out of the validator sets of the next epochs for a while. No coins are lost; it comes back by itself.')}</li>
        <li><strong>{t('Slashed and banned')}</strong> — {t('a leader that signs two blocks for one slot loses a tenth of its stake once, and is never in a set again. Its coins can still be unbonded.')}</li>
        <li><strong>{t('Reported')}</strong> — {t('any node that sees two such blocks keeps the proof and relays it; the block of stake that carries it earns the reporter’s share of the penalty (not a validator reporting itself).')}</li>
      </ul>
    </div>
  </section>

  <section class="panel" aria-labelledby="fb-evidence-title">
    <div class="panel-head fb-listbar">
      <h2 id="fb-evidence-title">Proofs of double signing</h2>
      <Pager total={evidence.length} bind:page={evPage} bind:size={evSize} sizes={SIZES} label="proofs" />
    </div>
    {#if evidenceError}
      <p class="side-empty fb-error"><SafeText text={evidenceError} /></p>
    {:else if evidence.length === 0}
      <p class="side-empty">This node knows no proof that a leader signed two blocks for one slot.</p>
    {:else}
      <div class="side-table-wrap">
        <table class="side-table" use:stacked aria-label="Proofs of double signing">
          <thead><tr><th>Validator</th><th class="end">Slot</th><th>The two blocks</th><th>Proof</th></tr></thead>
          <tbody>
            {#each evShown as e (`${e.pubkey}:${e.slot}`)}
              <tr>
                <td><Key key={e.pubkey} mine={localKeys.has(e.pubkey)} /></td>
                <td class="end num">{formatCount(e.slot)}</td>
                <td>
                  {#each e.hashes as h (h)}<button type="button" class="fb-hash" onclick={() => go('explorer', `search:${h}`)} translate="no">{shortHash(h, 8)}</button> {/each}
                </td>
                <td><CopyButton text={e.hex} label="Copy the proof" iconOnly={false} /></td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
    {/if}
  </section>
  </div>

  <div class="side-cols lower">
    <section class="panel" aria-labelledby="fb-held-title">
      <div class="panel-head fb-listbar">
        <h2 id="fb-held-title">Jailed and banned validators</h2>
        <Pager total={held.length} bind:page={heldPage} bind:size={heldSize} sizes={SIZES} label="validators" />
      </div>
      {#if held.length === 0}
        <p class="side-empty">{bonds ? t('No validator is jailed or banned.') : t('No validator of this epoch’s set is jailed or banned. Open a wallet to look at every validator.')}</p>
      {:else}
        <div class="side-table-wrap">
          <table class="side-table" use:stacked aria-label="Jailed and banned validators">
            <thead><tr><th>Validator</th><th>State</th><th class="end">Bonded</th></tr></thead>
            <tbody>
              {#each heldShown as h (h.key)}
                <tr>
                  <td><Key key={h.key} mine={localKeys.has(h.key)} /></td>
                  <td><span class="status bad">{h.banned ? t('Banned') : t('Jailed until epoch %1', formatCount(h.until))}</span></td>
                  <td class="end">{#if h.bonded !== undefined}<Amount sats={coinsToSats(h.bonded)} />{:else}–{/if}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
    </section>

    <section class="panel" aria-labelledby="fb-submit-title">
      <div class="panel-head"><h2 id="fb-submit-title">Give a proof</h2></div>
      <form class="panel-body side-form" onsubmit={(e) => { e.preventDefault(); submit(); }}>
        <p class="side-note fb-small">A proof from another node (its two block headers, in hex, as listevidence gives them): this node checks it, keeps it and relays it. A wrong one is refused; nothing is spent.</p>
        <textarea class="mono" rows="4" spellcheck="false" aria-label="The proof, in hex" placeholder="Two extended headers, in hex" bind:value={proof}></textarea>
        <div class="row"><button type="submit" class="btn" disabled={!/^[0-9a-fA-F]+$/.test(proof.trim()) || submitting}>Give the proof</button></div>
      </form>
    </section>
  </div>
</div>

<style>
  .rules ul {
    margin: 0;
    padding-left: 1.1rem;
    display: flex;
    flex-direction: column;
    gap: 0.4rem;
    font-size: 0.92rem;
    max-width: 90ch;
  }
  .rules {
    margin-bottom: 1rem;
  }
  .lower {
    margin-top: 1rem;
  }
  textarea {
    width: 100%;
    resize: vertical;
  }
</style>
