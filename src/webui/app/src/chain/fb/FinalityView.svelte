<script lang="ts">
  // Staking, Network, Finality & BMM (the page of its own FastBlocks had): how far the chain is BMM-confirmed and final, the blocks still waiting for a BMM commitment,
  // the newest BMM blocks (blocks of stake that record a BMM commitment, and merged mined ones), the tickets this
  // node knows (what BMM bidders ask the mainchain to commit to), commitments the active chain leaves out, the fee
  // pot BMM bidders win, and what this node itself bids (-validatoranchorbid). getanchorinfo, getposinfo, getblock;
  // the bid from the host (fastblocks.anchoring), as no RPC says it.
  import Amount from '../../components/Amount.svelte';
  import Icon from '../../components/Icon.svelte';
  import Pager from '../../components/Pager.svelte';
  import { stacked } from '../../lib/actions';
  import { formatCount, formatTime, shortHash } from '../../lib/format';
  import { t, tn } from '../../lib/i18n.svelte';
  import { go } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { node } from '../../lib/state.svelte';
  import { side, startSide } from '../../lib/sidestate.svelte';
  import { coinsToSats } from '../../lib/units';
  import { CHAIN } from '../config';
  import './fb.css';
  import { blockKind, finalityText, kindText, type BlockKind } from './model';
  import { fb, followTip, startFb } from './store.svelte';

  $effect(() => startFb());
  $effect(() => {
    void node.tip?.hash;
    followTip();
  });
  startSide();

  const SIZES = [10, 25, 50, 100];
  const WAITING_POINTS = [10, 25, 50, 100, 0];
  const BMM_POINTS = [10, 25, 50, 100];

  const pos = $derived(fb.pos);
  const anchors = $derived(fb.anchors);
  const tip = $derived(node.tip?.height ?? fb.height);
  const waitingCount = $derived(anchors && tip >= 0 ? Math.max(0, tip - Math.max(anchors.anchoredheight, -1)) : 0);
  const sinceSlots = $derived(pos && pos.lastposslot >= 0 && pos.firstunanchoredslot >= 0 ? pos.slot - pos.firstunanchoredslot : null);
  const mainName = CHAIN.mainchain?.name ?? 'the mainchain';
  const explore = (hash: string) => go('explorer', `search:${hash}`);

  // --- The blocks waiting for a BMM commitment: from the BMM-confirmed tip up to the tip, newest first. ---
  interface Waiting {
    height: number;
    hash: string;
    time: number;
    kind: BlockKind;
    slot?: number;
    nTx: number;
  }
  let waitingPoints = $state(25);
  let waiting = $state<Waiting[]>([]);
  const waitingCache = new Map<string, Waiting>();
  $effect(() => {
    const top = tip;
    const from = anchors ? anchors.anchoredheight + 1 : -1;
    const n = waitingPoints;
    if (from < 0 || top < from) {
      waiting = [];
      return;
    }
    const low = n === 0 ? from : Math.max(from, top - n + 1);
    (async () => {
      const out: Waiting[] = [];
      for (let h = top; h >= low; h--) {
        try {
          const hash = await rpc.call('getblockhash', [h]);
          let w = waitingCache.get(hash);
          if (!w) {
            const b = await rpc.raw<Record<string, unknown> & { height: number; time: number; nTx: number }>('getblock', [hash, 1]);
            w = { height: b.height, hash, time: b.time, kind: blockKind(b), slot: typeof b.slot === 'number' ? b.slot : undefined, nTx: b.nTx };
            waitingCache.set(hash, w);
          }
          out.push(w);
        } catch {
          break;
        }
      }
      if (tip === top) waiting = out;
    })();
  });
  let waitingPage = $state(0);
  let waitingSize = $state(10);
  const waitingShown = $derived(waiting.slice(waitingPage * waitingSize, (waitingPage + 1) * waitingSize));

  // --- The newest BMM blocks ---
  let bmmPoints = $state(25);
  let bmmPage = $state(0);
  let bmmSize = $state(10);
  const recent = $derived((anchors?.recent ?? []).slice(0, bmmPoints));
  const recentShown = $derived(recent.slice(bmmPage * bmmSize, (bmmPage + 1) * bmmSize));

  let ticketsPage = $state(0);
  let ticketsSize = $state(10);
  const tickets = $derived(anchors?.tickets ?? []);
  const ticketsShown = $derived(tickets.slice(ticketsPage * ticketsSize, (ticketsPage + 1) * ticketsSize));

  const anchoring = $derived(fb.anchoring);
</script>

<div class="fb-view finality">


  <section class="side-stats" aria-label="Finality">
    <div class="panel side-stat">
      <span class="muted">BMM-confirmed up to</span>
      <strong class="num" id="fb-anchored-height">{anchors && anchors.anchoredheight >= 0 ? t('block %1', formatCount(anchors.anchoredheight)) : '–'}</strong>
      <span class="faint">{t('best block %1', tip >= 0 ? formatCount(tip) : '–')}</span>
    </div>
    <div class="panel side-stat">
      <span class="muted">Final up to</span>
      <strong class="num">{anchors && anchors.finalheight >= 0 ? t('block %1', formatCount(anchors.finalheight)) : '–'}</strong>
      <span class="faint">{anchors ? tn('final after %n BMM commitment(s)', anchors.finalanchors) : '–'}</span>
    </div>
    <div class="panel side-stat">
      <span class="muted">Waiting for BMM</span>
      <strong class="num">{tn('%n block(s)', waitingCount)}</strong>
      <span class="faint">{sinceSlots !== null ? tn('%n slot(s) since the first one not covered', sinceSlots) : t('every block is covered')}</span>
    </div>
    <div class="panel side-stat">
      <span class="muted">BMM commitments</span>
      <strong class="num">{pos ? formatCount(pos.anchors) : '–'}</strong>
      <span class="faint">{pos && pos.lastanchormainheight >= 0 ? t('the last at %1 block %2', mainName, formatCount(pos.lastanchormainheight)) : t('none yet')}</span>
    </div>
  </section>

  <!-- The blocks waiting for BMM and the newest BMM blocks side by side (user decision, 2026-10-16). -->
  <div class="side-cols fb-pair">
    <section class="panel gap" aria-labelledby="fb-waiting-title">
      <div class="panel-head fb-listbar">
        <h2 id="fb-waiting-title">Blocks waiting for BMM confirmation</h2>
        <label><span class="muted">Latest</span>
          <select aria-label="Blocks read" bind:value={waitingPoints} onchange={() => (waitingPage = 0)}>
            {#each WAITING_POINTS as n (n)}<option value={n}>{n === 0 ? t('All') : n}</option>{/each}
          </select>
        </label>
      </div>
      {#if waiting.length === 0}
        <p class="side-empty">{anchors && anchors.anchoredheight >= 0 ? t('None: a BMM commitment covers the best block.') : t('No BMM commitment covers a block yet.')}</p>
      {:else}
        <div class="side-table-wrap">
          <table class="side-table" use:stacked aria-label="Blocks waiting for BMM confirmation">
            <thead><tr><th class="end">Height</th><th>Kind</th><th class="end">Slot</th><th>Time</th><th>Block</th></tr></thead>
            <tbody>
              {#each waitingShown as w (w.hash)}
                <tr>
                  <td class="end num">{formatCount(w.height)}</td>
                  <td>{#if w.kind}<span class="badge fb-kind-{w.kind}">{kindText(w.kind)}</span>{/if}</td>
                  <td class="end num">{w.slot !== undefined ? formatCount(w.slot) : '–'}</td>
                  <td>{formatTime(w.time, true)}</td>
                  <td><button type="button" class="fb-hash" onclick={() => explore(w.hash)} translate="no">{shortHash(w.hash, 10)}</button></td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
        <div class="panel-body"><Pager total={waiting.length} bind:page={waitingPage} bind:size={waitingSize} sizes={SIZES} label="blocks" /></div>
      {/if}
    </section>


  <section class="panel gap" aria-labelledby="fb-bmm-title">
    <div class="panel-head fb-listbar">
      <h2 id="fb-bmm-title">Newest BMM blocks</h2>
      <label><span class="muted">Latest</span>
        <select aria-label="BMM blocks read" bind:value={bmmPoints} onchange={() => (bmmPage = 0)}>
          {#each BMM_POINTS as n (n)}<option value={n}>{n}</option>{/each}
        </select>
      </label>
      <Pager total={recent.length} bind:page={bmmPage} bind:size={bmmSize} sizes={SIZES} label="BMM blocks" />
    </div>
    {#if recent.length === 0}
      <p class="side-empty">No BMM block on the active chain yet.</p>
    {:else}
      <div class="side-table-wrap">
        <table class="side-table" use:stacked aria-label="Newest BMM blocks">
          <thead><tr><th class="end">Block</th><th>Kind</th><th class="end">Covers block</th><th class="end">{t('%1 block', mainName)}</th><th>Finality of what it covers</th></tr></thead>
          <tbody>
            {#each recentShown as a (a.block)}
              <tr>
                <td class="end"><button type="button" class="fb-hash" onclick={() => explore(a.block)}>{formatCount(a.height)}</button></td>
                <td><span class="badge {a.kind === 'bmm' ? 'fb-kind-pow' : 'fb-kind-bmm'}" title={a.kind === 'bmm' ? t('A merged mined block (proof of work): its own BMM commitment covers its parent') : t('A block of stake that records the BMM commitment of a mainchain block')}>{a.kind === 'bmm' ? t('PoW') : t('BMM')}</span></td>
                <td class="end"><button type="button" class="fb-hash" onclick={() => explore(a.anchored)}>{formatCount(a.anchoredheight)}</button></td>
                <td class="end num">{formatCount(a.mainheight)}</td>
                <td>{#if anchors}{@const f = anchors.finalheight >= a.anchoredheight ? 'final' : 'anchored'}<span class="badge {f === 'final' ? 'fb-final' : 'fb-anchored'}">{finalityText(f)}</span>{/if}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
    {/if}
  </section>
  </div>

  <div class="side-cols fb-pair">
    <section class="panel gap" aria-labelledby="fb-tickets-title">
      <div class="panel-head fb-listbar">
        <h2 id="fb-tickets-title">Tickets this node knows</h2>
        <Pager total={tickets.length} bind:page={ticketsPage} bind:size={ticketsSize} sizes={SIZES} label="tickets" />
      </div>
      <p class="side-note fb-small pad">A ticket is what a BMM bidder asks the mainchain to commit to: a block of this chain and who wins the pot. A block of stake acknowledges a committed one, and so becomes a BMM block.</p>
      {#if tickets.length === 0}
        <p class="side-empty">None.</p>
      {:else}
        <div class="side-table-wrap">
          <table class="side-table" use:stacked aria-label="Tickets">
            <thead><tr><th>Ticket</th><th>Covers block</th><th class="end">{t('Committed at %1 block', mainName)}</th></tr></thead>
            <tbody>
              {#each ticketsShown as k (k.hash)}
                <tr>
                  <td class="mono fb-small" title={k.hash} translate="no">{shortHash(k.hash, 10)}</td>
                  <td><button type="button" class="fb-hash" onclick={() => explore(k.anchored)} translate="no">{shortHash(k.anchored, 10)}</button></td>
                  <td class="end num">{k.committedheight !== undefined ? formatCount(k.committedheight) : t('not yet')}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
    </section>

  <!-- Beside the tickets: the bids of this node and the fee pot, one above the other (user decision). -->
  <div class="fb-stack">
    <section class="panel" aria-labelledby="fb-bid-title">
      <div class="panel-head"><h2 id="fb-bid-title">BMM bids of this node</h2></div>
      <div class="panel-body side-form">
        <div class="fb-facts">
          <div>
            <span>As a validator (-validatoranchorbid)</span>
            <strong>{#if anchoring?.on}{t('%1 %2 a mainchain block', anchoring.bid ?? '', CHAIN.mainchain?.ticker ?? '')}{:else if anchoring?.validator}{t('off')}{:else}{t('not a validator')}{/if}</strong>
          </div>
          <div>
            <span>Merged mining (setbmm)</span>
            <strong>{side.bmm ? (side.bmm.mining ? t('on') : t('off')) : '–'}</strong>
          </div>
        </div>
        <p class="side-note fb-small">
          {#if anchoring?.on}
            {t('This node bids %1 %2 at least for a BMM commitment to its ticket in every mainchain block, whatever the fees; the pot it wins goes to %3. The merged miner, if on, bids instead.', anchoring.bid ?? '', CHAIN.mainchain?.ticker ?? '', anchoring.payee ?? '')}
          {:else if anchoring?.validator}
            {t('The validator’s own bidder is off (-validatoranchorbid=0, or on regtest by default; %1 elsewhere). Blocks of stake stop without BMM commitments: someone must bid for them.', anchoring.defaultbid)}
          {:else}
            {t('A validator bids for a BMM commitment in every mainchain block (-validatoranchorbid, %1 by default), so that blocks of stake never wait for fees. This node bids only if merged mining is on.', anchoring?.defaultbid ?? '0.00001')}
          {/if}
        </p>
        <div class="row"><button type="button" class="btn small" onclick={() => go('bmm')}><Icon name="merged" size={14} /> Merged mining</button></div>
      </div>
    </section>
    <section class="panel" aria-labelledby="fb-pot-title">
      <div class="panel-head"><h2 id="fb-pot-title">The fee pot</h2></div>
      <div class="panel-body side-form">
        <p class="side-note fb-small">Half the fees of each block of stake go to the pot; a BMM block pays it to the BMM bidder who won the mainchain's commitment, so that its value reaches the mainchain's miners.</p>
        {#if anchors && pos}
          <dl class="fb-pairs">
            <dt>What a BMM block of the tip would pay</dt><dd><Amount sats={coinsToSats(anchors.pot.due)} /></dd>
            <dt>Put in since the chain began</dt><dd><Amount sats={coinsToSats(anchors.pot.accrued)} /></dd>
            <dt>Paid out of it</dt><dd><Amount sats={coinsToSats(anchors.pot.paid)} /></dd>
            <dt title={t('Penalties not paid out yet: each BMM block pays a share of them with the pot')}>Penalties in reserve</dt><dd><Amount sats={coinsToSats(pos.pot.reserve)} /></dd>
          </dl>
        {/if}
        {#if anchors?.skipped.length}
          <h3>Commitments the active chain leaves out</h3>
          <ul class="plain">
            {#each anchors.skipped as s (s.mainheight)}
              <li>{t('%1 block %2', mainName, formatCount(s.mainheight))}: {s.fork === 'any' ? t('its block is not known here') : s.fork === 'at' ? t('a chain with it leaves this one at block %1', formatCount(s.forkheight ?? 0)) : t('a chain with it leaves this one at block %1 or above', formatCount(s.forkheight ?? 0))}</li>
            {/each}
          </ul>
        {/if}
      </div>
    </section>
  </div>
  </div>
</div>

<style>
  .pad {
    padding: 0 1.1rem 0.6rem;
  }
  .fb-pair > :global(.panel) {
    margin-bottom: 0;
  }
  .fb-stack {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    min-width: 0;
  }
  .gap {
    margin-bottom: 1rem;
  }
  .plain {
    margin: 0;
    padding-left: 1.1rem;
    font-size: 0.88rem;
  }
  h3 {
    margin: 0.4rem 0 0;
    font-size: 0.95rem;
  }
</style>
