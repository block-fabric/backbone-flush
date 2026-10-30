<script lang="ts" module>
  // Bidding (Drivechains, Merged mining; doc/fastblocks-anchor-bids.md): the node's one bid policy, for every node. What it
  // bids for a BMM commitment (a strategy of five, and "Prevent stalling"), its limits, and what starts a bid: merged
  // mining ("Mine blocks when fees pay", setbmm, the page's own controls passed in as `mining`) and, on a validator,
  // "Anchor my validator". Read with getanchorbidpolicy and getanchorinfo's bid; changed with setanchorbidpolicy, which
  // spends the mainchain wallet and so is confirmed on the desktop (local.cpp ConfirmedRpcMethods), live, kept in the
  // node's settings over its options.

  export type Strategy = 'fees' | 'feesminimum' | 'minimum' | 'fixed' | 'off';

  export interface BidPolicy {
    strategy: Strategy;
    preventstall: boolean;
    anchor: boolean;
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
    anchortrigger?: boolean;
    minetrigger?: boolean;
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
    trigger?: string;
  }

  export interface BidStatus {
    validator: boolean;
    bidding: boolean;
    explanation?: string;
    strategy?: Strategy;
    trigger?: string;
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

  /** The five strategies, in the words of the panel (%1: the mainchain's name). Fees is the default. */
  export const STRATEGIES: { id: Strategy; label: string; text: string }[] = [
    { id: 'fees', label: 'Fees', text: 'Bids the fee share of what each anchor or block pays out. No fees, no bid: no minimum.' },
    { id: 'feesminimum', label: 'Fees + minimum', text: 'Bids the fee share, or the minimum when the share is below it: a bid on every block of %1, more when there are fees.' },
    { id: 'minimum', label: 'Minimum every block', text: 'Bids the minimum on every block of %1, whatever the fees.' },
    { id: 'fixed', label: 'Fixed amount', text: 'Bids the fixed amount on every block of %1.' },
    { id: 'off', label: 'Off', text: 'No bids at all, from either trigger. The chain then relies on other bidders: without anchors its blocks of stake halt.' },
  ];

  /** The curve as the node takes it in text: "0.5:1,0.75:2,0.9:4". */
  export function curveText(curve: { pressure: number; multiplier: number }[]): string {
    return curve.map((c) => `${c.pressure}:${c.multiplier}`).join(',');
  }

  /**
   * The fields of `next` that differ from `now`, as setanchorbidpolicy takes them (amounts in coins, as text). The strategy
   * and "Prevent stalling" always go together: an old strategy name alone says whether to prevent stalling, and the node
   * reads a strategy without `preventstall` that way.
   */
  export function changedFields(now: BidPolicy, next: BidPolicy): Record<string, unknown> {
    const out: Record<string, unknown> = {};
    for (const key of Object.keys(next) as (keyof BidPolicy)[]) {
      if (key === 'curve') {
        if (curveText(now.curve) !== curveText(next.curve)) out.curve = curveText(next.curve);
      } else if (now[key] !== next[key]) {
        out[key] = next[key];
      }
    }
    if ('strategy' in out || 'preventstall' in out) {
      out.strategy = next.strategy;
      out.preventstall = next.preventstall;
    }
    for (const key of ['floor', 'max', 'fixed', 'periodcap'] as const) {
      if (key in out) out[key] = satsToCoins(coinsToSats(next[key]));
    }
    return out;
  }

  import { coinsToSats, satsToCoins } from '../../lib/units';
</script>

<script lang="ts">
  import type { Snippet } from 'svelte';
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
  import { stacked } from '../../lib/actions';
  import './fb.css';

  // `mining`: the merged miner's trigger ("Mine blocks when fees pay"), the page's controls (setbmm).
  let { mining = undefined }: { mining?: Snippet } = $props();

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
    if (floor > max) return t('The minimum is above the maximum per bid');
    if (fixed > max) return t('The fixed amount is above the maximum per bid');
    if (!(form.feeshare >= 0 && form.feeshare <= 100)) return t('The fee share is a percentage from 0 to 100');
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
    void [floor, max, fixed, periodcap, curve, form?.strategy, form?.feeshare];
    const e = edited();
    return typeof e === 'string' ? e : '';
  });

  const changes = $derived.by(() => {
    void [floor, max, fixed, periodcap, curve, form?.strategy, form?.preventstall, form?.anchor, form?.feeshare, dirty];
    const next = edited();
    return info && typeof next !== 'string' ? changedFields(info.policy, next) : {};
  });

  async function apply() {
    if (!info || busy) return;
    const fields = changes;
    if (!Object.keys(fields).length) return;
    busy = true;
    try {
      // Confirmed on the desktop: the host shows what the mainchain wallet will spend, from now on.
      await rpc.raw('setanchorbidpolicy', [fields]);
      toast('success', t('Bid policy changed'));
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

  const off = $derived(form?.strategy === 'off');
  const startPercent = $derived((bid?.escalationstart ?? info?.policy.escalationstart ?? 0.5) * 100);
  const overridden = $derived(info ? Object.keys(info.overrides ?? {}) : []);
  const hours = $derived(info?.policy.periodhours ?? 24);
  const capSats = $derived(info ? coinsToSats(info.policy.periodcap) : 0n);
  const spentSats = $derived(bid ? coinsToSats(bid.spentperiod) : 0n);
  const leftSats = $derived(capSats > spentSats ? capSats - spentSats : 0n);
  const statusWords: Record<BidRecord['status'], string> = { pending: 'pending', landed: 'landed', missed: 'missed', outbid: 'outbid' };
</script>

{#snippet mineTrigger()}
  <div class="bid-trigger" id="bid-trigger-mine">
    <strong>Mine blocks when fees pay</strong>
    {#if mining}{@render mining()}{/if}
  </div>
{/snippet}

<section class="panel" aria-labelledby="bid-title" id="bid-panel">
  <div class="panel-head">
    <h2 id="bid-title">Bidding</h2>
    {#if info}<span class="side-link" class:on={info.bidding} class:off={!info.bidding}><span class="dot"></span>{info.bidding ? t('Bidding') : t('Not bidding')}</span>{/if}
  </div>
  <div class="panel-body side-form">
    <p class="side-note" id="bid-intro">
      One policy for every bid of this node: merged mining's blocks and, on a validator, the anchors of its blocks of stake. Each bid is a BMM request on {mainName}, at most one a block.
    </p>
    {#if error}<p class="side-note side-bad" role="alert"><SafeText text={error} /></p>{/if}
    {#if !info}
      <p class="side-note">Reading the bid policy…</p>
      <!-- Merged mining goes on or off whether the policy could be read or not. -->
      {@render mineTrigger()}
    {:else}
      {#if info.explanation}<p class="side-note" id="bid-explanation"><SafeText text={info.explanation} /></p>{/if}
      {#if bid}
        <div class="fb-facts" id="bid-status">
          <div>
            <span>Bid now</span>
            <strong id="bid-effective">{#if bid.bidding}<Amount sats={coinsToSats(bid.effective)} />{:else}{t('off')}{/if}</strong>
            <span class="faint" id="bid-reason"><SafeText text={bid.reason} /></span>
          </div>
          <div>
            <span>Unanchored run</span>
            <strong>{formatCount(bid.unanchoredslots)} / {formatCount(bid.maxunanchoredslots)} slots</strong>
            <span class="bid-gauge" title="Stall pressure p: the chain halts at 1; stall prevention bids from {startPercent / 100}">
              <meter id="bid-pressure" min="0" max="1" low={startPercent / 100} high="0.9" optimum="0" value={Math.min(1, bid.pressure)}></meter>
              <span class="num">p = {bid.pressure.toFixed(2)}</span>
            </span>
          </div>
          <div>
            <span>Spent ({hours} h)</span>
            <strong id="bid-spent"><Amount sats={spentSats} /></strong>
            <span class="faint" id="bid-left">{#if capSats > 0n}{t('left')} <Amount sats={leftSats} /> {t('of')} <Amount sats={capSats} />{:else}{t('no cap')}{/if}</span>
          </div>
          <div>
            <span>Bid wallet ({mainName})</span>
            <strong id="bid-balance">{#if bid.balance !== undefined}<Amount sats={coinsToSats(bid.balance)} />{:else}–{/if}</strong>
            <span class="faint">{#if bid.coverblocks !== undefined}{t('%1 mainchain blocks at this bid', formatCount(bid.coverblocks))}{:else if bid.balanceerror}<SafeText text={bid.balanceerror} />{/if}</span>
          </div>
          <div>
            <span>Missed in a row</span>
            <strong id="bid-missed">{formatCount(bid.missed)}</strong>
            <span class="faint">{#if bid.feerate !== undefined}{t('fee estimate %1 sat/vB', bid.feerate.toFixed(2))}{:else}{t('no fee estimate')}{/if}</span>
          </div>
        </div>
        {#each bid.warnings as warning, i (i)}
          <p class="side-note side-bad bid-warning" role="alert"><SafeText text={warning} /></p>
        {/each}
      {/if}

      {#if !form}
        {@render mineTrigger()}
      {:else}
        <div class="bid-cols">
          <div class="bid-col">
            <fieldset class="bid-strategies">
              <legend>Strategy</legend>
              {#each STRATEGIES as s (s.id)}
                <label class="bid-strategy" class:on={form.strategy === s.id}>
                  <input type="radio" name="bid-strategy" value={s.id} checked={form.strategy === s.id} onchange={() => choose(s.id)} />
                  <span><strong>{t(s.label)}</strong><span class="faint">{t(s.text, mainName)}</span></span>
                </label>
              {/each}
            </fieldset>
            <label class="check bid-stall" class:disabled={off}>
              <input id="bid-preventstall" type="checkbox" bind:checked={form.preventstall} disabled={off} onchange={() => (dirty = true)} />
              <span>
                <strong>Prevent stalling</strong>
                <span class="faint">{#if off}Off means off: no stall prevention either.{:else}As the chain nears its limit without an anchor ({bid ? formatCount(bid.maxunanchoredslots) : '–'} slots), and when a deposit or a withdrawal waits for one, bids enough to land an anchor, whatever the strategy, within the limits.{/if}</span>
              </span>
            </label>
            {#if off}
              <p class="side-note side-warn" role="alert" id="bid-off-warning">With no bids of its own, this node leaves its anchors to other bidders: if none bids, blocks of stake halt after {bid ? formatCount(bid.maxunanchoredslots) : '–'} slots without one.</p>
            {:else}
              <div class="anchor-grid" id="bid-amounts">
                {#if form.strategy === 'fees' || form.strategy === 'feesminimum'}
                  <div class="field"><label for="bid-feeshare">Fee share (%)</label><input id="bid-feeshare" type="number" min="0" max="100" step="0.01" bind:value={form.feeshare} oninput={() => (dirty = true)} /></div>
                {/if}
                {#if form.strategy === 'feesminimum' || form.strategy === 'minimum'}
                  <AmountInput id="bid-floor" label="Minimum (floor)" bind:value={floor} />
                {/if}
                {#if form.strategy === 'fixed'}
                  <AmountInput id="bid-fixed" label="Fixed amount" bind:value={fixed} />
                {/if}
              </div>
              {#if form.strategy === 'feesminimum' || form.strategy === 'minimum'}
                <p class="side-note fb-small">The minimum is your floor, or the relay minimum of a request on {mainName} if higher (0: the relay minimum).</p>
              {:else if form.strategy === 'fees'}
                <p class="side-note fb-small">You keep the rest of the fees. A share too small to pay for a request is no bid.</p>
              {/if}
            {/if}
          </div>

          <div class="bid-col">
            <h3 class="bid-sub">Limits</h3>
            <div class="anchor-grid">
              <AmountInput id="bid-max" label="Maximum per bid" bind:value={max} />
              <AmountInput id="bid-periodcap" label={hours === 24 ? t('Daily cap (0: none)') : t('Cap per %1 h (0: none)', hours)} bind:value={periodcap} />
            </div>
            <p class="side-note fb-small" id="bid-wallet">Paid from your mainchain wallet ({mainName}). Only bids that land count in the cap; stall prevention stays within it too.</p>

            <h3 class="bid-sub">What starts a bid</h3>
            {@render mineTrigger()}
            {#if info.validator}
              <label class="check bid-trigger" id="bid-trigger-anchor">
                <input id="bid-anchor" type="checkbox" bind:checked={form.anchor} disabled={off} onchange={() => (dirty = true)} />
                <span><strong>Anchor my validator</strong><span class="faint">Your validator's own bids for the anchors of its blocks of stake, as the strategy says. One request serves both triggers.</span></span>
              </label>
            {/if}
          </div>
        </div>

        <label class="check"><input id="bid-advanced" type="checkbox" bind:checked={advanced} /><span>Advanced</span></label>
        {#if advanced}
          <div class="anchor-grid" id="bid-advanced-fields">
            {#if form.strategy !== 'feesminimum' && form.strategy !== 'minimum'}<AmountInput id="bid-floor-advanced" label="Minimum (floor; also stall prevention's base)" bind:value={floor} />{/if}
            {#if form.strategy !== 'fixed'}<AmountInput id="bid-fixed-advanced" label="Fixed amount" bind:value={fixed} />{/if}
            {#if form.strategy !== 'fees' && form.strategy !== 'feesminimum'}<div class="field"><label for="bid-feeshare-advanced">Fee share (%)</label><input id="bid-feeshare-advanced" type="number" min="0" max="100" step="0.01" bind:value={form.feeshare} oninput={() => (dirty = true)} /></div>{/if}
            <div class="field"><label for="bid-start">Prevent stalling from pressure</label><input id="bid-start" type="number" min="0.01" max="1" step="0.01" bind:value={form.escalationstart} oninput={() => (dirty = true)} /></div>
            <div class="field"><label for="bid-flat"><input id="bid-flat" type="checkbox" checked={form.flatboost ?? true} onchange={(e) => { form!.flatboost = (e.currentTarget as HTMLInputElement).checked; dirty = true; }} /> Flat boost (the competitive base times the boost; off: the curve and rebids)</label></div>
            <div class="field"><label for="bid-boost">Boost (times the base)</label><input id="bid-boost" type="number" min="1" max="1000" step="0.5" value={form.boost ?? 4} oninput={(e) => { form!.boost = Number((e.currentTarget as HTMLInputElement).value); dirty = true; }} /></div>
            <div class="field"><label for="bid-curve">Escalation curve (p:multiplier)</label><input id="bid-curve" type="text" class="mono" spellcheck="false" bind:value={curve} oninput={() => (dirty = true)} /></div>
            <label class="check"><input id="bid-interpolate" type="checkbox" bind:checked={form.interpolate} onchange={() => (dirty = true)} /><span>Interpolate between points (else steps)</span></label>
            <label class="check"><input id="bid-estimate" type="checkbox" bind:checked={form.usefeeestimate} onchange={() => (dirty = true)} /><span>Use the next-block fee estimate of {mainName} for stall prevention</span></label>
            <div class="field"><label for="bid-vsize">Request size (vB)</label><input id="bid-vsize" type="number" min="50" max="10000" bind:value={form.requestvsize} oninput={() => (dirty = true)} /></div>
            <div class="field"><label for="bid-rebid">Raise after missed blocks (0: never)</label><input id="bid-rebid" type="number" min="0" max="1000" bind:value={form.rebidafter} oninput={() => (dirty = true)} /></div>
            <div class="field"><label for="bid-rebidstep">Raise by (x)</label><input id="bid-rebidstep" type="number" min="1" max="100" step="0.1" bind:value={form.rebidstep} oninput={() => (dirty = true)} /></div>
            <div class="field"><label for="bid-rebidmax">Raises at most (x)</label><input id="bid-rebidmax" type="number" min="1" max="1000" step="0.1" bind:value={form.rebidmax} oninput={() => (dirty = true)} /></div>
            <div class="field"><label for="bid-warn">Warn after missed blocks (0: never)</label><input id="bid-warn" type="number" min="0" max="1000" bind:value={form.warnmissed} oninput={() => (dirty = true)} /></div>
            <div class="field"><label for="bid-periodhours">Period of the cap (hours)</label><input id="bid-periodhours" type="number" min="1" max="744" bind:value={form.periodhours} oninput={() => (dirty = true)} /></div>
            <div class="field"><label for="bid-lowbalance">Low balance below (blocks covered)</label><input id="bid-lowbalance" type="number" min="0" bind:value={form.lowbalanceblocks} oninput={() => (dirty = true)} /></div>
          </div>
        {/if}
        {#if problem}<p class="side-note side-bad" id="bid-problem">{problem}</p>{/if}
        <div class="row">
          <button type="button" class="btn primary" id="bid-apply" disabled={busy || !!problem || !Object.keys(changes).length} onclick={apply}>Apply</button>
          <button type="button" class="btn" id="bid-reset" disabled={busy || !overridden.length} onclick={reset}>Back to the node's options</button>
        </div>
        <p class="side-note fb-small">
          Changes take effect at once, without a restart, and are kept in the node's settings over its options (-validatoranchorbid…). The desktop asks first: they spend the wallet of the {mainName} node.
          {#if overridden.length}{t('Changed from the options: %1.', overridden.join(', '))}{/if}
        </p>
      {/if}

      {#if bid?.history.length}
        <div class="side-table-wrap">
          <table class="side-table" use:stacked aria-label="Last bids">
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
  .bid-gauge {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .bid-gauge meter {
    width: 120px;
    max-width: 100%;
  }
  .bid-cols {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(22rem, 100%), 1fr));
    gap: 1rem 1.5rem;
    align-items: start;
  }
  .bid-col {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
    min-width: 0;
  }
  .bid-sub {
    margin: 0;
    font-size: 0.95rem;
    font-weight: 600;
  }
  .bid-strategies {
    border: 0;
    padding: 0;
    margin: 0;
    display: grid;
    gap: 6px;
    min-width: 0;
  }
  .bid-strategies legend {
    font-weight: 600;
    margin-bottom: 4px;
  }
  .bid-strategy,
  .bid-trigger {
    display: flex;
    gap: 8px;
    align-items: flex-start;
    padding: 6px 8px;
    border-radius: 6px;
    border: 1px solid transparent;
  }
  .bid-strategy.on {
    border-color: currentColor;
  }
  .bid-strategy > span,
  .bid-stall > span,
  .bid-trigger > span {
    display: grid;
    gap: 2px;
  }
  .bid-trigger {
    flex-direction: column;
    border-color: var(--border, rgba(127, 127, 127, 0.3));
  }
  label.bid-trigger {
    flex-direction: row;
  }
  .bid-stall {
    display: flex;
    gap: 8px;
    align-items: flex-start;
    padding: 0 8px;
  }
  .bid-stall.disabled {
    opacity: 0.6;
  }
  .anchor-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(min(220px, 100%), 1fr));
    gap: 8px 12px;
  }
</style>
