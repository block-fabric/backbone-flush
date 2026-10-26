<script lang="ts" module>
  // The anchor bid policy of this node's validator (doc/fastblocks-anchor-bids.md), under Drivechains, Merged mining: what its
  // own anchorer bids for a BMM commitment to its ticket in every mainchain block, and why. Read with getanchorbidpolicy
  // and getanchorinfo's bid; changed with setanchorbidpolicy, which spends the mainchain wallet and so is confirmed on the
  // desktop (local.cpp ConfirmedRpcMethods), live, kept in the node's settings over its options.

  export type Strategy = 'fixed' | 'minimal' | 'fees' | 'feespreventstall' | 'off';

  export interface BidPolicy {
    strategy: Strategy;
    floor: number;
    max: number;
    fixed: number;
    feeshare: number;
    escalationstart: number;
    curve: { pressure: number; multiplier: number }[];
    flatboost?: boolean;
    boost?: number;
    interpolate: boolean;
    usefeeestimate: boolean;
    requestvsize: number;
    rebidafter: number;
    rebidstep: number;
    rebidmax: number;
    warnmissed: number;
    periodcap: number;
    periodhours: number;
    lowbalanceblocks: number;
  }

  export interface BidPolicyInfo {
    validator: boolean;
    bidding: boolean;
    explanation?: string;
    policy: BidPolicy;
    defaults: BidPolicy;
    overrides: Record<string, unknown>;
  }

  export interface BidRecord {
    mainheight: number;
    ticket: string;
    offer: number;
    kind: string;
    reason: string;
    pressure: number;
    status: 'pending' | 'landed' | 'missed' | 'outbid';
    cost: number;
    time: number;
  }

  export interface BidStatus {
    validator: boolean;
    bidding: boolean;
    explanation?: string;
    strategy?: Strategy;
    effective: number;
    normal: number;
    kind: string;
    reason: string;
    pressure: number;
    multiplier: number;
    escalationstart?: number;
    firstunanchoredslot: number;
    unanchoredslots: number;
    maxunanchoredslots: number;
    feerate?: number;
    feesource: string;
    missed: number;
    balance?: number;
    coverblocks?: number;
    balanceerror?: string;
    spentperiod: number;
    periodcap?: number;
    periodhours?: number;
    history: BidRecord[];
    warnings: string[];
  }

  /** The presets: the five strategies, in the words of the panel. */
  export const PRESETS: { id: Strategy; label: string; text: string }[] = [
    { id: 'fixed', label: 'Fixed', text: 'The fixed amount for every mainchain block, whatever the fees or the stall pressure (at most the maximum).' },
    { id: 'minimal', label: 'Minimal: keep the chain moving', text: 'The minimum for every mainchain block; raised as the chain nears its halt (the stall pressure) and after missed blocks, never above the maximum.' },
    { id: 'fees', label: 'Fees', text: 'The fee share the anchor owes when there are fees, else the minimum. No stall boost.' },
    {
      id: 'feespreventstall',
      label: 'Fees + prevent stalling',
      text: 'The base follows the next-block fee estimate rounded up to whole sat/vB, times the request size, and covers the relay minimum and your floor. Bids the fee share when higher; you cover any shortfall. Near a stall, boosts the base 4 times, or bids the fee share when higher. Skips when there are no fees, pending peg actions or staking maintenance and no stall risk. Never above the maximum.',
    },
    { id: 'off', label: 'Off', text: 'No bids. The chain then relies on other bidders: without anchors its blocks of stake halt.' },
  ];

  /** The curve as the node takes it in text: "0.5:1,0.75:2,0.9:4". */
  export function curveText(curve: { pressure: number; multiplier: number }[]): string {
    return curve.map((c) => `${c.pressure}:${c.multiplier}`).join(',');
  }

  /** The fields of `next` that differ from `now`, as setanchorbidpolicy takes them (amounts in coins, as text). */
  export function changedFields(now: BidPolicy, next: BidPolicy): Record<string, unknown> {
    const out: Record<string, unknown> = {};
    for (const key of Object.keys(next) as (keyof BidPolicy)[]) {
      if (key === 'curve') {
        if (curveText(now.curve) !== curveText(next.curve)) out.curve = curveText(next.curve);
      } else if (now[key] !== next[key]) {
        out[key] = next[key];
      }
    }
    for (const key of ['floor', 'max', 'fixed', 'periodcap'] as const) {
      if (key in out) out[key] = satsToCoins(coinsToSats(next[key]));
    }
    return out;
  }

  import { coinsToSats, satsToCoins } from '../../lib/units';
</script>

<script lang="ts">
  import { t } from '../../lib/i18n.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import Amount from '../../components/Amount.svelte';
  import AmountInput from '../../components/AmountInput.svelte';
  import { errorText } from '../../lib/dialogs.svelte';
  import { formatCount } from '../../lib/format';
  import { rpc } from '../../lib/rpc';
  import { node, toast } from '../../lib/state.svelte';
  import { declined } from '../../lib/side';
  import { CHAIN } from '../config';

  const mainName = CHAIN.mainchain?.name ?? '';
  const HISTORY = 10;

  let info = $state<BidPolicyInfo | null>(null);
  let bid = $state<BidStatus | null>(null);
  let error = $state('');
  let busy = $state(false);
  let advanced = $state(false);
  // The form: a copy of the policy in force, edited.
  let form = $state<BidPolicy | null>(null);
  let floor = $state<bigint | null>(null);
  let max = $state<bigint | null>(null);
  let fixed = $state<bigint | null>(null);
  let periodcap = $state<bigint | null>(null);
  let curve = $state('');
  let dirty = $state(false);

  function fill(policy: BidPolicy) {
    form = { ...policy, curve: policy.curve.map((c) => ({ ...c })) };
    floor = coinsToSats(policy.floor);
    max = coinsToSats(policy.max);
    fixed = coinsToSats(policy.fixed);
    periodcap = coinsToSats(policy.periodcap);
    curve = curveText(policy.curve);
    dirty = false;
  }

  export async function load() {
    try {
      const [p, a] = await Promise.all([rpc.raw<BidPolicyInfo>('getanchorbidpolicy', []), rpc.raw<{ bid?: BidStatus }>('getanchorinfo', [HISTORY])]);
      info = p;
      bid = a?.bid ?? null;
      if (!dirty && p?.policy) fill(p.policy);
      error = '';
    } catch (e) {
      error = errorText(e);
    }
  }

  $effect(() => {
    void node.tip?.height;
    void load();
  });

  /** The form as a policy, or what is wrong with it. */
  function edited(): BidPolicy | string {
    if (!form) return t('Nothing to change');
    if (floor === null || max === null || fixed === null || periodcap === null) return t('Every amount is needed (0 is allowed)');
    if (floor > max) return t('The minimum is above the maximum');
    if (fixed > max) return t('The fixed amount is above the maximum');
    const points = curve.split(',').map((part) => part.split(':').map(Number));
    if (!points.length || points.some((p) => p.length !== 2 || p.some((n) => !Number.isFinite(n)))) return t('The curve is pressure:multiplier pairs, as 0.5:1,0.75:2,0.9:4');
    return {
      ...form,
      floor: Number(satsToCoins(floor)),
      max: Number(satsToCoins(max)),
      fixed: Number(satsToCoins(fixed)),
      periodcap: Number(satsToCoins(periodcap)),
      curve: points.map(([pressure, multiplier]) => ({ pressure, multiplier })),
    };
  }

  const problem = $derived.by(() => {
    void [floor, max, fixed, periodcap, curve, form?.strategy];
    const e = edited();
    return typeof e === 'string' ? e : '';
  });

  async function apply() {
    if (!info || busy) return;
    const next = edited();
    if (typeof next === 'string') return;
    const fields = changedFields(info.policy, next);
    if (!Object.keys(fields).length) return;
    busy = true;
    try {
      // Confirmed on the desktop: the host shows what the mainchain wallet will spend, from now on.
      await rpc.raw('setanchorbidpolicy', [fields]);
      toast('success', t('Anchor bid policy changed'));
      dirty = false;
      await load();
    } catch (e) {
      if (!declined(e)) error = errorText(e);
    } finally {
      busy = false;
    }
  }

  async function reset() {
    if (busy) return;
    busy = true;
    try {
      await rpc.raw('setanchorbidpolicy', [{}, true]);
      dirty = false;
      await load();
    } catch (e) {
      if (!declined(e)) error = errorText(e);
    } finally {
      busy = false;
    }
  }

  function choose(id: Strategy) {
    if (!form) return;
    form.strategy = id;
    dirty = true;
  }

  const pressurePercent = $derived(bid ? Math.min(100, Math.max(0, bid.pressure * 100)) : 0);
  const startPercent = $derived((bid?.escalationstart ?? info?.policy.escalationstart ?? 0.5) * 100);
  const overridden = $derived(info ? Object.keys(info.overrides ?? {}) : []);
  const statusWords: Record<BidRecord['status'], string> = { pending: 'pending', landed: 'landed', missed: 'missed', outbid: 'outbid' };
</script>

<section class="panel" aria-labelledby="anchor-bid-title" id="anchor-bid-panel">
  <div class="panel-head"><h2 id="anchor-bid-title">Anchor bids of this validator</h2></div>
  <div class="panel-body side-form">
    {#if error}<p class="side-note side-bad" role="alert"><SafeText text={error} /></p>{/if}
    {#if !info}
      <p class="side-note">Reading the bid policy…</p>
    {:else if !info.validator}
      <p class="side-note" id="anchor-bid-explanation"><SafeText text={info.explanation ?? ''} /></p>
      <p class="side-note fb-small">
        A validator's own anchorer bids for a BMM commitment to its ticket in every block of {mainName}, so that blocks of stake never halt for want of an anchor. This node is not a validator: it bids only with merged mining, above.
      </p>
    {:else}
      {#if bid}
        <div class="fb-facts" id="anchor-bid-status">
          <div>
            <span>Bid now</span>
            <strong id="anchor-bid-effective">{#if bid.bidding}<Amount sats={coinsToSats(bid.effective)} />{:else}{t('off')}{/if}</strong>
            <span class="faint" id="anchor-bid-reason"><SafeText text={bid.reason} /></span>
          </div>
          <div>
            <span>Unanchored run</span>
            <strong>{formatCount(bid.unanchoredslots)} / {formatCount(bid.maxunanchoredslots)} slots</strong>
            <span class="anchor-gauge" title="Stall pressure p: the chain halts at 1; the bid escalates from {startPercent / 100}">
              <meter id="anchor-bid-pressure" min="0" max="1" low={startPercent / 100} high="0.9" optimum="0" value={Math.min(1, bid.pressure)}></meter>
              <span class="num">p = {bid.pressure.toFixed(2)}</span>
            </span>
          </div>
          <div>
            <span>Bid wallet ({mainName})</span>
            <strong id="anchor-bid-balance">{#if bid.balance !== undefined}<Amount sats={coinsToSats(bid.balance)} />{:else}–{/if}</strong>
            <span class="faint">{#if bid.coverblocks !== undefined}{t('%1 mainchain blocks at this bid', formatCount(bid.coverblocks))}{:else if bid.balanceerror}<SafeText text={bid.balanceerror} />{/if}</span>
          </div>
          <div>
            <span>Spent ({bid.periodhours ?? 24} h)</span>
            <strong><Amount sats={coinsToSats(bid.spentperiod)} /></strong>
            <span class="faint">{#if bid.periodcap}{t('cap')} <Amount sats={coinsToSats(bid.periodcap)} />{:else}{t('no cap')}{/if}</span>
          </div>
          <div>
            <span>Missed in a row</span>
            <strong id="anchor-bid-missed">{formatCount(bid.missed)}</strong>
            <span class="faint">{#if bid.feerate !== undefined}{t('fee estimate %1 sat/vB', bid.feerate.toFixed(2))}{:else}{t('no fee estimate')}{/if}</span>
          </div>
        </div>
        {#each bid.warnings as warning, i (i)}
          <p class="side-note side-bad anchor-bid-warning" role="alert"><SafeText text={warning} /></p>
        {/each}
      {/if}

      {#if form}
        <fieldset class="anchor-presets">
          <legend>Strategy</legend>
          {#each PRESETS as preset (preset.id)}
            <label class="anchor-preset" class:on={form.strategy === preset.id}>
              <input type="radio" name="anchor-strategy" value={preset.id} checked={form.strategy === preset.id} onchange={() => choose(preset.id)} />
              <span><strong>{t(preset.label)}</strong><span class="faint">{t(preset.text)}</span></span>
            </label>
          {/each}
        </fieldset>
        {#if form.strategy === 'off'}
          <p class="side-note side-warn" role="alert">With no bids of its own, this validator leaves its anchors to other bidders: if none bids, blocks of stake halt after {info.policy && bid ? formatCount(bid.maxunanchoredslots) : '–'} slots without one.</p>
        {/if}
        <div class="anchor-grid">
          <AmountInput id="anchor-floor" label="Minimum (floor)" bind:value={floor} />
          <AmountInput id="anchor-max" label="Maximum per bid" bind:value={max} />
          {#if form.strategy === 'fixed' || advanced}<AmountInput id="anchor-fixed" label="Fixed amount" bind:value={fixed} />{/if}
        </div>
        <label class="check"><input id="anchor-advanced" type="checkbox" bind:checked={advanced} /><span>Advanced</span></label>
        {#if advanced}
          <div class="anchor-grid" id="anchor-advanced-fields">
            <div class="field"><label for="anchor-feeshare">Fee share (%)</label><input id="anchor-feeshare" type="number" min="0" max="100" step="0.01" bind:value={form.feeshare} oninput={() => (dirty = true)} /></div>
            <div class="field"><label for="anchor-start">Escalation from pressure</label><input id="anchor-start" type="number" min="0.01" max="1" step="0.01" bind:value={form.escalationstart} oninput={() => (dirty = true)} /></div>
            <div class="field"><label for="anchor-flat"><input id="anchor-flat" type="checkbox" checked={form.flatboost ?? true} onchange={(e) => { form!.flatboost = (e.currentTarget as HTMLInputElement).checked; dirty = true; }} /> Flat boost (the estimated base times the boost; off: the curve and rebids)</label></div>
            <div class="field"><label for="anchor-boost">Boost (times the minimum)</label><input id="anchor-boost" type="number" min="1" max="1000" step="0.5" value={form.boost ?? 4} oninput={(e) => { form!.boost = Number((e.currentTarget as HTMLInputElement).value); dirty = true; }} /></div>
            <div class="field"><label for="anchor-curve">Escalation curve (p:multiplier)</label><input id="anchor-curve" type="text" class="mono" spellcheck="false" bind:value={curve} oninput={() => (dirty = true)} /></div>
            <label class="check"><input id="anchor-interpolate" type="checkbox" bind:checked={form.interpolate} onchange={() => (dirty = true)} /><span>Interpolate between points (else steps)</span></label>
            <label class="check"><input id="anchor-estimate" type="checkbox" bind:checked={form.usefeeestimate} onchange={() => (dirty = true)} /><span>Use the next-block fee estimate of {mainName} for the base</span></label>
            <div class="field"><label for="anchor-vsize">Request size (vB)</label><input id="anchor-vsize" type="number" min="50" max="10000" bind:value={form.requestvsize} oninput={() => (dirty = true)} /></div>
            <div class="field"><label for="anchor-rebid">Raise after missed blocks (0: never)</label><input id="anchor-rebid" type="number" min="0" max="1000" bind:value={form.rebidafter} oninput={() => (dirty = true)} /></div>
            <div class="field"><label for="anchor-rebidstep">Raise by (x)</label><input id="anchor-rebidstep" type="number" min="1" max="100" step="0.1" bind:value={form.rebidstep} oninput={() => (dirty = true)} /></div>
            <div class="field"><label for="anchor-rebidmax">Raises at most (x)</label><input id="anchor-rebidmax" type="number" min="1" max="1000" step="0.1" bind:value={form.rebidmax} oninput={() => (dirty = true)} /></div>
            <div class="field"><label for="anchor-warn">Warn after missed blocks (0: never)</label><input id="anchor-warn" type="number" min="0" max="1000" bind:value={form.warnmissed} oninput={() => (dirty = true)} /></div>
            <AmountInput id="anchor-periodcap" label="Spending cap per period (0: none)" bind:value={periodcap} />
            <div class="field"><label for="anchor-periodhours">Period (hours)</label><input id="anchor-periodhours" type="number" min="1" max="744" bind:value={form.periodhours} oninput={() => (dirty = true)} /></div>
            <div class="field"><label for="anchor-lowbalance">Low balance below (blocks covered)</label><input id="anchor-lowbalance" type="number" min="0" bind:value={form.lowbalanceblocks} oninput={() => (dirty = true)} /></div>
          </div>
        {/if}
        {#if problem}<p class="side-note side-bad" id="anchor-problem">{problem}</p>{/if}
        <div class="row">
          <button type="button" class="btn primary" id="anchor-apply" disabled={busy || !!problem} onclick={apply}>Apply</button>
          <button type="button" class="btn" id="anchor-reset" disabled={busy || !overridden.length} onclick={reset}>Back to the node's options</button>
        </div>
        <p class="side-note fb-small">
          Changes take effect at once, without a restart, and are kept in the node's settings over its options (-validatoranchorbid…). The desktop asks first: they spend the wallet of the {mainName} node.
          {#if overridden.length}{t('Changed from the options: %1.', overridden.join(', '))}{/if}
        </p>
      {/if}

      {#if bid?.history.length}
        <div class="table-wrap">
          <table class="table" aria-label="Last anchor bids">
            <thead><tr><th>Mainchain block</th><th>Bid</th><th>Why</th><th>p</th><th>Result</th><th>Cost</th></tr></thead>
            <tbody>
              {#each bid.history as h (h.ticket + h.mainheight)}
                <tr>
                  <td class="num">{formatCount(h.mainheight)}</td>
                  <td><Amount sats={coinsToSats(h.offer)} /></td>
                  <td><SafeText text={h.kind} /></td>
                  <td class="num">{h.pressure.toFixed(2)}</td>
                  <td class:side-bad={h.status === 'missed'}>{t(statusWords[h.status] ?? '')}</td>
                  <td><Amount sats={coinsToSats(h.cost)} /></td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
    {/if}
  </div>
</section>

<style>
  .anchor-gauge {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .anchor-gauge meter {
    width: 120px;
  }
  .anchor-presets {
    border: 0;
    padding: 0;
    margin: 0;
    display: grid;
    gap: 6px;
  }
  .anchor-preset {
    display: flex;
    gap: 8px;
    align-items: flex-start;
    padding: 6px 8px;
    border-radius: 6px;
    border: 1px solid transparent;
  }
  .anchor-preset.on {
    border-color: currentColor;
  }
  .anchor-preset > span {
    display: grid;
    gap: 2px;
  }
  .anchor-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(220px, 1fr));
    gap: 8px 12px;
  }
</style>
