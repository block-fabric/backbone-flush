<script lang="ts">
  // Staking, Bonds: the bonds of the wallet (bonded, unbonding and when it is paid, the state of the key) and the
  // form that bonds, adds to a bond, unbonds or unbonds all (createbond, createunbond through the host, which asks
  // the desktop with the key and the amount in full; the passphrase of a locked wallet typed in that dialog). A bond to the key of
  // another node takes the proof of possession that node signs: this node signs one for an owner here.
  import Amount from '../../components/Amount.svelte';
  import AmountInput from '../../components/AmountInput.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { stacked } from '../../lib/actions';
  import { BridgeError, request } from '../../lib/bridge';
  import { errorText, inform, withUnlock } from '../../lib/dialogs.svelte';
  import { formatCount } from '../../lib/format';
  import { t } from '../../lib/i18n.svelte';
  import { go } from '../../lib/nav.svelte';
  import { toast, wallets } from '../../lib/state.svelte';
  import { coinsToSats, satsToCoins } from '../../lib/units';
  import Key from './Key.svelte';
  import { anchorsTime, bondProblem, isKey, stateText, stateTip, stateTone, validatorState, type BondAction, type WalletBond } from './model';
  import { fb, refreshFb } from './store.svelte';

  const ACTIONS: { id: BondAction; label: string; tip: string }[] = [
    { id: 'bond', label: 'Bond', tip: 'Bond coins of this wallet to a validator key: they leave the wallet, and the key takes part in the validator sets from the epoch after next (on eCash, about ten hours later)' },
    { id: 'add', label: 'Add to bond', tip: "Add coins to this wallet's bond to the key" },
    { id: 'unbond', label: 'Unbond', tip: 'Take the amount back from the bond: paid to this wallet some BMM commitments later' },
    { id: 'unbondAll', label: 'Unbond all', tip: 'Take all of the bond back (all that is bonded when the unbond is mined)' },
  ];

  const pos = $derived(fb.pos);
  const staking = $derived(fb.staking);
  const bonds = $derived(fb.bonds ?? []);
  const localKeys = $derived(new Set(staking?.keys.map((k) => k.pubkey) ?? []));
  const minStake = $derived(coinsToSats(pos?.staking.minstake ?? 0));
  const unbondingAnchors = $derived(pos?.staking.unbondinganchors ?? 0);

  let action = $state<BondAction>('bond');
  let key = $state('');
  let amount = $state<bigint | null>(null);
  let amountInvalid = $state('');
  let amountKey = $state(0);
  let otherNode = $state(false);
  let pop = $state('');
  let owner = $state('');
  let busy = $state(false);
  let selected = $state<string | null>(null);
  /** The user typed in the form: what is wrong is said from then on. */
  let touched = $state(false);

  // The key by default: this node's first, so that a validator bonds to itself in one step.
  $effect(() => {
    if (!key && staking?.keys.length) key = staking.keys[0].pubkey;
  });

  const keyNorm = $derived(key.trim().toLowerCase());
  const existing = $derived(bonds.find((b) => b.validator === keyNorm));
  // A first bond goes to "Bond", one the wallet has to "Add to bond" (the node refuses neither, but says it).
  const problem = $derived(bondProblem(action, keyNorm, action === 'unbondAll' ? null : amountInvalid ? null : amount, existing, minStake));
  const needsPop = $derived(otherNode && (action === 'bond' || action === 'add') && isKey(keyNorm) && !localKeys.has(keyNorm) && !existing);

  function pick(b: WalletBond) {
    selected = b.validator;
    key = b.validator;
    if (action === 'bond') action = 'add';
  }

  async function submit() {
    const wallet = wallets.current;
    if (wallet === null || problem || busy) return;
    if (needsPop && (!/^[0-9a-fA-F]{128}$/.test(pop.trim()) || !owner.trim())) {
      await inform(t('Staking'), t('A bond to the key of another node takes the proof of possession its node signed, and the owner it was signed for.'), 'warning');
      return;
    }
    busy = true;
    try {
      const unbond = action === 'unbond' || action === 'unbondAll';
      const params: Record<string, unknown> = { wallet, pubkey: keyNorm, amount: action === 'unbondAll' ? 'all' : satsToCoins(amount ?? 0n) };
      if (needsPop) {
        params.pop = pop.trim();
        params.owner = owner.trim();
      }
      const result = await withUnlock(wallet, () => request<Record<string, unknown>>('host', unbond ? 'fastblocks.unbond' : 'fastblocks.bond', params));
      if (!result) return;
      amountKey++;
      amount = null;
      if (unbond) {
        toast('success', t('Unbond sent'), t('%1 is paid back to %2 at BMM commitment %3 or so.', typeof result.amount === 'number' || typeof result.amount === 'string' ? satsToCoins(coinsToSats(result.amount)) : String(result.amount), String(result.owner), String(result.releaseanchor)));
      } else {
        const short = result.eligible === false ? t('That is less than the minimum stake: the key takes part once it has that much.') : t('The key takes part from the epoch after next (on eCash, about ten hours later).');
        toast('success', t('Bond sent'), `${t('The key has %1 bonded once it is mined.', String(result.bonded))} ${short}`);
      }
      await refreshFb();
    } catch (e) {
      const err = BridgeError.from(e);
      if (err.code !== -32800) await inform(t('Staking'), errorText(e), 'warning');
    } finally {
      busy = false;
    }
  }

  // The proof of possession this node signs for an owner (fastblocks.pop): for a bond from a wallet elsewhere.
  let popOwner = $state('');
  let popKey = $state('');
  let popSigned = $state<{ pubkey: string; pop: string } | null>(null);
  let popError = $state('');
  async function signPop() {
    popError = '';
    popSigned = null;
    try {
      popSigned = await request<{ pubkey: string; pop: string }>('host', 'fastblocks.pop', popKey ? { owner: popOwner.trim(), pubkey: popKey } : { owner: popOwner.trim() });
    } catch (e) {
      if (BridgeError.from(e).code !== -32800) popError = errorText(e);
    }
  }
</script>

{#if wallets.current === null}
  <section class="panel">
    <p class="side-empty">Bonds are made from a wallet: open one, or <button type="button" class="btn small" onclick={() => go('wallets', 'create')}>create one</button>.</p>
  </section>
{:else}
  <div class="side-cols bonds">
    <section class="panel" aria-labelledby="fb-form-title">
      <div class="panel-head"><h2 id="fb-form-title">Bond and unbond</h2></div>
      <form class="panel-body side-form" oninput={() => (touched = true)} onsubmit={(e) => { e.preventDefault(); submit(); }}>
        <div class="fb-segments" role="group" aria-label="What to do">
          {#each ACTIONS as a (a.id)}
            <button type="button" aria-pressed={action === a.id} title={t(a.tip)} onclick={() => (action = a.id)}>{t(a.label)}</button>
          {/each}
        </div>
        <div class="field">
          <label for="fb-bond-key">Validator key</label>
          <input id="fb-bond-key" class="mono" type="text" spellcheck="false" autocomplete="off" placeholder="x-only key, 64 hex digits" bind:value={key} list="fb-keys" />
          <datalist id="fb-keys">
            {#each staking?.keys ?? [] as k (k.pubkey)}<option value={k.pubkey}>{t('this node')}</option>{/each}
            {#each bonds as b (b.validator)}<option value={b.validator}>{t('a bond of this wallet')}</option>{/each}
          </datalist>
          <span class="faint fb-small">
            {#if localKeys.has(keyNorm)}{t("This node's key: it signs the proof of possession itself.")}
            {:else if existing}{t('This wallet has %1 bonded to it.', String(existing.bonded))}
            {:else if isKey(keyNorm)}{t("Another node's key: a first bond takes the proof of possession its node signs.")}
            {/if}
          </span>
        </div>
        {#if action !== 'unbondAll'}
          {#key amountKey}<AmountInput id="fb-bond-amount" label="Amount" bind:value={amount} bind:invalid={amountInvalid} />{/key}
        {:else if existing}
          <p class="side-note">{t('All of the bond: %1 now, and whatever is added before the unbond is mined.', String(existing.bonded))}</p>
        {/if}
        {#if (action === 'bond' || action === 'add') && isKey(keyNorm) && !localKeys.has(keyNorm) && !existing}
          <label class="check"><input type="checkbox" bind:checked={otherNode} /> {t('The key is of another node: I have its proof of possession')}</label>
          {#if otherNode}
            <div class="field">
              <label for="fb-bond-pop">Proof of possession</label>
              <input id="fb-bond-pop" class="mono" type="text" spellcheck="false" autocomplete="off" placeholder="128 hex digits" bind:value={pop} />
            </div>
            <div class="field">
              <label for="fb-bond-owner">Owner it was signed for</label>
              <input id="fb-bond-owner" class="mono" type="text" spellcheck="false" autocomplete="off" placeholder="an address of this wallet" bind:value={owner} />
              <span class="faint fb-small">The validator's node signs it for an address of this wallet (Bonds, Proof of possession, there).</span>
            </div>
          {/if}
        {/if}
        <p class="side-note fb-small">
          {#if action === 'unbond' || action === 'unbondAll'}
            {t('Unbonded coins leave the stake at once, and are paid back to the bond’s owner %1 BMM commitments later (%2 on the main network).', unbondingAnchors, anchorsTime(unbondingAnchors))}
          {:else}
            {t('A key takes part with at least %1 bonded. The coins leave this wallet now; unbonding them takes %2 BMM commitments.', String(pos?.staking.minstake ?? '–'), unbondingAnchors)}
          {/if}
        </p>
        {#if problem && touched}<p class="fb-error fb-small" role="alert">{problem}</p>{/if}
        <div class="row">
          <button type="submit" class="btn primary" id="fb-bond-submit" disabled={!!problem || busy}>
            <Icon name={action === 'unbond' || action === 'unbondAll' ? 'up' : 'lock'} size={15} /> {t(ACTIONS.find((a) => a.id === action)?.label ?? 'Bond')}
          </button>
          <span class="faint fb-small">The desktop asks you to confirm.</span>
        </div>
      </form>
    </section>

    <section class="panel" aria-labelledby="fb-bonds-title">
      <div class="panel-head"><h2 id="fb-bonds-title">Bonds of this wallet</h2><span class="muted fb-small">{t('pick one to add to it or unbond')}</span></div>
      {#if bonds.length === 0}
        <p class="side-empty">This wallet has no bonds.</p>
      {:else}
        <div class="side-table-wrap">
          <table class="side-table" use:stacked aria-label="Bonds of this wallet">
            <thead><tr><th>Key</th><th class="end">Bonded</th><th class="end">Unbonding</th><th>State</th><th>Owner</th></tr></thead>
            <tbody>
              {#each bonds as b (b.validator)}
                {@const st = validatorState(b, b.inset, pos?.epoch ?? 0)}
                <tr class:chosen={selected === b.validator} onclick={() => pick(b)}>
                  <td><Key key={b.validator} mine={localKeys.has(b.validator)} /></td>
                  <td class="end"><Amount sats={coinsToSats(b.bonded)} /></td>
                  <td class="end" title={b.unbonds.map((u) => t('%1 at BMM commitment %2', satsToCoins(coinsToSats(u.amount)), u.releaseanchor)).join('\n')}>
                    <Amount sats={coinsToSats(b.unbonding)} />
                    {#if b.unbonds.length && pos}<span class="faint fb-small"><br />{t('paid at commitment %1 (now %2)', formatCount(Math.min(...b.unbonds.map((u) => u.releaseanchor))), formatCount(pos.anchors))}</span>{/if}
                  </td>
                  <td><span class="status {stateTone(st)}" title={stateTip(st)}>{stateText(st, b)}</span></td>
                  <td class="cut mono fb-small" title={b.owner}><span translate="no">{b.owner}</span></td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
        <p class="side-note fb-small pad">The owner of a bond is the address of this wallet that alone may add to it or unbond it, and is paid what is unbonded.</p>
      {/if}
    </section>
  </div>

  {#if staking?.validator}
    <section class="panel" aria-labelledby="fb-pop-title">
      <div class="panel-head"><h2 id="fb-pop-title">Proof of possession</h2></div>
      <form class="panel-body side-form" onsubmit={(e) => { e.preventDefault(); signPop(); }}>
        <p class="side-note">
          To bond to this node's key from a wallet on another node, that wallet needs the key's signature of its owner address: no one can bond a key that is not theirs. Sign it here for an address of that wallet, then give the proof and the address to it (Bonds, “The key is of another node”).
        </p>
        <div class="pop-row">
          <div class="field grow">
            <label for="fb-pop-owner">Owner address (P2WPKH)</label>
            <input id="fb-pop-owner" class="mono" type="text" spellcheck="false" autocomplete="off" bind:value={popOwner} />
          </div>
          {#if (staking.keys.length ?? 0) > 1}
            <div class="field">
              <label for="fb-pop-key">Key</label>
              <select id="fb-pop-key" bind:value={popKey}>
                <option value="">{t('The first')}</option>
                {#each staking.keys as k (k.pubkey)}<option value={k.pubkey}>{k.pubkey.slice(0, 16)}…</option>{/each}
              </select>
            </div>
          {/if}
        </div>
        <div class="row"><button type="submit" class="btn" disabled={!popOwner.trim()}>Sign the proof</button></div>
        {#if popError}<p class="fb-error fb-small" role="alert"><SafeText text={popError} /></p>{/if}
        {#if popSigned}
          <div class="side-code"><span class="mono" translate="no">{popSigned.pop}</span><CopyButton text={popSigned.pop} label="Copy the proof" /></div>
          <p class="faint fb-small">{t('For the key %1.', popSigned.pubkey)}</p>
        {/if}
      </form>
    </section>
  {/if}
{/if}

<style>
  .pad {
    padding: 0.6rem 0.8rem 0.9rem;
  }
  tbody tr {
    cursor: pointer;
  }
  .pop-row {
    display: flex;
    flex-wrap: wrap;
    gap: 0.8rem;
  }
</style>
