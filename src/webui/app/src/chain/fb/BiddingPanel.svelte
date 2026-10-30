<script lang="ts" module>
  // Bidding (Drivechains, Merged mining; doc/fastblocks-anchor-bids.md): the node's one bid policy, for every node.
  //
  // The panel leads with the plain choice of how much a bid is (four cards: the recommended fee-funded plan, the
  // minimum on every block, a fixed amount, off), then what the node bids for (two separate switches: "Anchor my
  // validator", on a validator, and merged mining as a backup producer, setbmm, the page's own controls passed in as
  // `mining`), then a live status line, and every number under Advanced (collapsed). One policy and one wallet serve
  // both. Read with getanchorbidpolicy and getanchorinfo's bid, and from 8.3.7 getbmminfo's "anchoring" (passed in);
  // changed with setanchorbidpolicy, which spends the mainchain wallet and so is confirmed on the desktop (local.cpp
  // ConfirmedRpcMethods), live, kept in the node's settings over its options. The 8.3.7 fields (batch, heartbeat, value
  // at risk, the anchor reserve, turns) are optional: an 8.3.6 node does not have them, and their controls are hidden.

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
    stallreserve?: number;
    // 8.3.7 (demand-driven anchoring): absent on older nodes.
    /** The Fees strategy bids once the fee share reaches batch x the landing price. */
    batch?: number;
    /** A bid at the landing price after this many hours without an anchor (0: off). */
    heartbeathours?: number;
    /** A bid once the value moved in unanchored blocks of stake reaches this (coins; 0: off). */
    valueatrisk?: number;
    /** The share of a fee-funded bid held back for the anchor reserve (0-100). */
    reservepct?: number;
    /** The reserve's target (coins). */
    reservetarget?: number;
    /** Take turns among the validators. */
    turns?: boolean;
    /** Mainchain blocks a validator not designated waits before it bids. */
    turnbackup?: number;
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
    pot?: number;
    holdback?: number;
  }

  export type { AnchoringInfo, AnchoringWindow };

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
    anchoring?: AnchoringInfo;
  }

  /** The plain choices of the panel: each one or two strategies (Fees + minimum is Recommended with a minimum, under Advanced). */
  export type Plan = 'recommended' | 'minimum' | 'fixed' | 'off';

  /** The four cards, in the words of the panel (%1: the mainchain's name). Recommended is the default. */
  export const PLANS: { id: Plan; label: string; text: string; older?: string }[] = [
    {
      id: 'recommended',
      label: 'Recommended: pay from the fees',
      text: 'Pays for anchors from the fees the chain collects, in batches, with a heartbeat every few hours and a reserve for quiet times. Bids more only to keep the chain from stalling.',
      older: 'Pays for anchors from the fees the chain collects: no fees, no bid. Bids more only to keep the chain from stalling.',
    },
    { id: 'minimum', label: 'Anchor every block (minimum)', text: 'Bids the minimum on every block of %1, whatever the fees: the steadiest, and the dearest.' },
    { id: 'fixed', label: 'Fixed amount', text: 'Bids an amount you choose on every block of %1.' },
    { id: 'off', label: 'Off', text: 'No bids at all. The chain then relies on other bidders: without anchors its blocks of stake halt.' },
  ];

  /** The card of a strategy. */
  export function planOf(strategy: Strategy): Plan {
    return strategy === 'fees' || strategy === 'feesminimum' ? 'recommended' : strategy;
  }

  /** The recommended plan: Fees, Prevent stalling, and the 8.3.7 defaults (set only where the node has the field). */
  export const RECOMMENDED = { strategy: 'fees', preventstall: true, batch: 10, heartbeathours: 6, valueatrisk: 1, reservepct: 20, reservetarget: 0.0001, turns: true, turnbackup: 2 } as const;
  export const NEW_FIELDS = ['batch', 'heartbeathours', 'valueatrisk', 'reservepct', 'reservetarget', 'turns', 'turnbackup'] as const;

  /** Whether a policy is the recommended plan as it stands (any field the node does not have is not compared). */
  export function isRecommended(policy: BidPolicy): boolean {
    if (policy.strategy !== RECOMMENDED.strategy || policy.preventstall !== RECOMMENDED.preventstall) return false;
    return NEW_FIELDS.every((key) => policy[key] === undefined || policy[key] === RECOMMENDED[key]);
  }

  /** The policy with the recommended plan's fields. */
  export function recommended(policy: BidPolicy): BidPolicy {
    const out: BidPolicy = { ...policy, strategy: RECOMMENDED.strategy, preventstall: RECOMMENDED.preventstall };
    for (const key of NEW_FIELDS) if (policy[key] !== undefined) (out as unknown as Record<string, unknown>)[key] = RECOMMENDED[key];
    return out;
  }

  /** The curve as the node takes it in text: "0.5:1,0.75:2,0.9:4". */
  export function curveText(curve: { pressure: number; multiplier: number }[]): string {
    return curve.map((c) => `${c.pressure}:${c.multiplier}`).join(',');
  }

  /** The fields that are amounts: sent in coins, as text. */
  const AMOUNTS = ['floor', 'max', 'fixed', 'periodcap', 'valueatrisk', 'reservetarget'] as const;

  /**
   * The fields of `next` that differ from `now`, as setanchorbidpolicy takes them (amounts in coins, as text). The strategy
   * and "Prevent stalling" always go together: an old strategy name alone says whether to prevent stalling, and the node
   * reads a strategy without `preventstall` that way. A field the node did not report (an 8.3.6 node) is never sent.
   */
  export function changedFields(now: BidPolicy, next: BidPolicy): Record<string, unknown> {
    const out: Record<string, unknown> = {};
    for (const key of Object.keys(next) as (keyof BidPolicy)[]) {
      if (next[key] === undefined) continue;
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
    for (const key of AMOUNTS) {
      if (key in out) out[key] = satsToCoins(coinsToSats(next[key] as number));
    }
    return out;
  }

  /** The 24 h and 7 d windows of "anchoring", under either spelling. */
  export function anchoringWindows(a: AnchoringInfo | undefined): { day?: AnchoringWindow; week?: AnchoringWindow } {
    return { day: a?.day ?? a?.['24h'], week: a?.week ?? a?.['7d'] };
  }

  import { coinsToSats, satsToCoins } from '../../lib/units';
  import type { AnchoringInfo, AnchoringWindow } from '../../lib/side';
</script>

<script lang="ts">
  import type { Snippet } from 'svelte';
  import { t } from '../../lib/i18n.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import Amount from '../../components/Amount.svelte';
  import AmountInput from '../../components/AmountInput.svelte';
  import { errorText } from '../../lib/dialogs.svelte';
  import { formatCount, shortDuration } from '../../lib/format';
  import { rpc } from '../../lib/rpc';
  import { node, toast } from '../../lib/state.svelte';
  import { declined } from '../../lib/side';
  import { CHAIN } from '../config';
  import { stacked } from '../../lib/actions';
  import './fb.css';

  // `mining`: the merged miner's switch (setbmm), the page's controls. `anchoring`: getbmminfo's "anchoring" (8.3.7).
  let { mining = undefined, anchoring = undefined }: { mining?: Snippet; anchoring?: AnchoringInfo } = $props();

  const mainName = CHAIN.mainchain?.name ?? '';
  const HISTORY = 10;

  let info = $state<BidPolicyInfo | null>(null);
  let bid = $state<BidStatus | null>(null);
  let slotSeconds = $state(0);
  let error = $state('');
  let busy = $state(false);
  let advanced = $state(false);
  // The form: a copy of the policy in force, edited.
  let form = $state<BidPolicy | null>(null);
  let floor = $state<bigint | null>(null);
  let max = $state<bigint | null>(null);
  let fixed = $state<bigint | null>(null);
  let periodcap = $state<bigint | null>(null);
  let valueatrisk = $state<bigint | null>(null);
  let reservetarget = $state<bigint | null>(null);
  let curve = $state('');
  let dirty = $state(false);
  let now = $state(Date.now() / 1000);

  function fill(policy: BidPolicy) {
    form = { ...policy, curve: policy.curve.map((c) => ({ ...c })) };
    floor = coinsToSats(policy.floor);
    max = coinsToSats(policy.max);
    fixed = coinsToSats(policy.fixed);
    periodcap = coinsToSats(policy.periodcap);
    valueatrisk = policy.valueatrisk !== undefined ? coinsToSats(policy.valueatrisk) : null;
    reservetarget = policy.reservetarget !== undefined ? coinsToSats(policy.reservetarget) : null;
    curve = curveText(policy.curve);
    dirty = false;
  }

  export async function load() {
    try {
      const [p, a] = await Promise.all([rpc.raw<BidPolicyInfo>('getanchorbidpolicy', []), rpc.raw<{ bid?: BidStatus }>('getanchorinfo', [HISTORY])]);
      info = p;
      bid = a?.bid ?? null;
      now = Date.now() / 1000;
      // An edit in progress (a typed amount too) is kept.
      if (!dirty && !Object.keys(changes).length && p?.policy) fill(p.policy);
      error = '';
    } catch (e) {
      error = errorText(e);
    }
    if (!slotSeconds) {
      // The length of a slot, once: the limit of the unanchored run in hours.
      try {
        const pos = await rpc.raw<{ slotseconds?: number }>('getposinfo', []);
        if (pos?.slotseconds && pos.slotseconds > 0) slotSeconds = pos.slotseconds;
      } catch {
        // In slots then.
      }
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
    if (form.valueatrisk !== undefined && valueatrisk === null) return t('Every amount is needed (0 is allowed)');
    if (form.reservetarget !== undefined && reservetarget === null) return t('Every amount is needed (0 is allowed)');
    if (floor > max) return t('The minimum is above the maximum per bid');
    if (fixed > max) return t('The fixed amount is above the maximum per bid');
    if (!(form.feeshare >= 0 && form.feeshare <= 100)) return t('The fee share is a percentage from 0 to 100');
    if (form.reservepct !== undefined && !(Number.isInteger(form.reservepct) && form.reservepct >= 0 && form.reservepct <= 100)) return t('The reserve share is a whole percentage from 0 to 100');
    if (form.batch !== undefined && !(form.batch >= 0)) return t('The batch is a number of landing prices, 0 or more');
    if (form.heartbeathours !== undefined && !(form.heartbeathours >= 0)) return t('The heartbeat is a number of hours, 0 or more');
    if (form.turnbackup !== undefined && !(Number.isInteger(form.turnbackup) && form.turnbackup >= 0)) return t('The turn backup is a whole number of blocks');
    const points = curve.split(',').map((part) => part.split(':').map(Number));
    if (!points.length || points.some((p) => p.length !== 2 || p.some((n) => !Number.isFinite(n)))) return t('The curve is pressure:multiplier pairs, as 0.5:1,0.75:2,0.9:4');
    const out: BidPolicy = {
      ...form,
      floor: Number(satsToCoins(floor)),
      max: Number(satsToCoins(max)),
      fixed: Number(satsToCoins(fixed)),
      periodcap: Number(satsToCoins(periodcap)),
      curve: points.map(([pressure, multiplier]) => ({ pressure, multiplier })),
    };
    if (form.valueatrisk !== undefined && valueatrisk !== null) out.valueatrisk = Number(satsToCoins(valueatrisk));
    if (form.reservetarget !== undefined && reservetarget !== null) out.reservetarget = Number(satsToCoins(reservetarget));
    return out;
  }

  // edited() reads every field of the form and the amounts: they are what this depends on.
  const result = $derived.by(() => edited());
  const problem = $derived(typeof result === 'string' ? result : '');
  const changes = $derived(info && typeof result !== 'string' ? changedFields(info.policy, result) : {});

  async function send(fields: Record<string, unknown>, done: string): Promise<boolean> {
    busy = true;
    try {
      // Confirmed on the desktop: the host shows what the mainchain wallet will spend, from now on.
      await rpc.raw('setanchorbidpolicy', [fields]);
      toast('success', done);
      return true;
    } catch (e) {
      if (!declined(e)) error = errorText(e);
      return false;
    } finally {
      busy = false;
    }
  }

  async function apply() {
    if (!info || busy) return;
    const fields = changes;
    if (!Object.keys(fields).length) return;
    if (await send(fields, t('Bid policy changed'))) {
      dirty = false;
      await load();
      if (info?.policy) fill(info.policy);
    }
  }

  /** "Anchor my validator" takes effect at once (as merged mining's Start and Stop do), through the same confirmation. */
  async function setAnchor(e: Event) {
    const box = e.currentTarget as HTMLInputElement;
    if (!info || busy) {
      box.checked = info?.policy.anchor ?? box.checked;
      return;
    }
    const on = box.checked;
    const ok = await send({ anchor: on }, on ? t('Your validator bids for its anchors again') : t('Your validator no longer bids for its anchors'));
    if (ok) {
      if (form) form.anchor = on;
      await load();
    }
    box.checked = info?.policy.anchor ?? on;
  }

  async function reset() {
    if (busy) return;
    busy = true;
    try {
      await rpc.raw('setanchorbidpolicy', [{}, true]);
      dirty = false;
      await load();
      if (info?.policy) fill(info.policy);
    } catch (e) {
      if (!declined(e)) error = errorText(e);
    } finally {
      busy = false;
    }
  }

  function choose(id: Plan) {
    if (!form) return;
    if (id === 'recommended') useRecommended();
    else form.strategy = id;
    dirty = true;
  }

  /** The recommended plan, its 8.3.7 values too; the limits and the rest stay as they are. */
  function useRecommended() {
    if (!form) return;
    const next = recommended(form);
    Object.assign(form, next);
    if (next.valueatrisk !== undefined) valueatrisk = coinsToSats(next.valueatrisk);
    if (next.reservetarget !== undefined) reservetarget = coinsToSats(next.reservetarget);
    dirty = true;
  }

  /** Recommended's own check: a minimum on every block too (Fees + minimum). */
  function setWithMinimum(on: boolean) {
    if (!form || planOf(form.strategy) !== 'recommended') return;
    form.strategy = on ? 'feesminimum' : 'fees';
    dirty = true;
  }

  function number(key: 'batch' | 'heartbeathours' | 'reservepct' | 'turnbackup' | 'stallreserve' | 'boost', e: Event) {
    if (!form) return;
    form[key] = Number((e.currentTarget as HTMLInputElement).value);
    dirty = true;
  }

  const plan = $derived(form ? planOf(form.strategy) : null);
  const off = $derived(form?.strategy === 'off');
  const has837 = $derived(!!form && NEW_FIELDS.some((key) => form![key] !== undefined));
  // The form, with any typed amounts, against the recommended plan.
  const adjusted = $derived(plan === 'recommended' && typeof result !== 'string' && !isRecommended(result));
  const startPercent = $derived((bid?.escalationstart ?? info?.policy.escalationstart ?? 0.5) * 100);
  const overridden = $derived(info ? Object.keys(info.overrides ?? {}) : []);
  const hours = $derived(info?.policy.periodhours ?? 24);
  const capSats = $derived(info ? coinsToSats(info.policy.periodcap) : 0n);
  const spentSats = $derived(bid ? coinsToSats(bid.spentperiod) : 0n);
  const leftSats = $derived(capSats > spentSats ? capSats - spentSats : 0n);
  // The policy in force: a validator whose anchorer bids nothing leaves its anchors to others.
  const inForce = $derived(info?.policy ?? null);
  const anchorerOff = $derived(!!info?.validator && !!inForce && (inForce.strategy === 'off' || !inForce.anchor));
  const a = $derived<AnchoringInfo | undefined>(anchoring ?? bid?.anchoring);
  const windows = $derived(anchoringWindows(a));
  const feeStrategy = $derived(inForce?.strategy === 'fees' || inForce?.strategy === 'feesminimum');
  /** How long the chain goes without an anchor before blocks of stake halt: hours when the slot is known. */
  const haltAfter = $derived(!bid ? '–' : slotSeconds > 0 ? t('%1 h', String(Math.round((bid.maxunanchoredslots * slotSeconds) / 360) / 10)) : t('%1 slots', formatCount(bid.maxunanchoredslots)));
  const statusWords: Record<BidRecord['status'], string> = { pending: 'pending', landed: 'landed', missed: 'missed', outbid: 'outbid' };
</script>

<section class="panel" aria-labelledby="bid-title" id="bid-panel">
  <div class="panel-head">
    <h2 id="bid-title">Bidding</h2>
    {#if info}<span class="side-link" class:on={info.bidding} class:off={!info.bidding}><span class="dot"></span>{info.bidding ? t('Bidding') : t('Not bidding')}</span>{/if}
  </div>
  <div class="panel-body side-form">
    <p class="side-note" id="bid-intro">
      Every block of stake needs an anchor on {mainName}: a BMM request that a miner of {mainName} takes for a fee. Choose how much this node offers, then what it bids for. One policy and one wallet serve both.
    </p>
    {#if error}<p class="side-note side-bad" role="alert"><SafeText text={error} /></p>{/if}
    {#if !info}
      <p class="side-note">Reading the bid policy…</p>
      <!-- Merged mining goes on or off whether the policy could be read or not. -->
      <div class="bid-switch" id="bid-trigger-mine">
        <span><strong>Merged-mine as a backup producer</strong></span>
        {#if mining}{@render mining()}{/if}
      </div>
    {:else}
      {#if info.explanation}<p class="side-note" id="bid-explanation"><SafeText text={info.explanation} /></p>{/if}

      <!-- What the anchorer is doing now, in one line. -->
      {#if anchorerOff}
        <p class="side-note side-warn bid-line" role="alert" id="bid-anchorer-off">
          {inForce?.strategy === 'off' ? t('Bidding is off: this node bids for no anchor. The chain relies on other bidders, and halts after %1 without an anchor.', haltAfter) : t('Anchor my validator is off: your validator bids for none of its anchors. The chain relies on other bidders, and halts after %1 without an anchor.', haltAfter)}
        </p>
      {:else if bid}
        <p class="side-note bid-line" id="bid-line">
          {#if feeStrategy && a?.pot !== undefined && a?.batchneeded !== undefined}
            {#if a.pot < a.batchneeded}
              <span id="bid-line-batch">Waiting for fees: <Amount sats={coinsToSats(a.pot)} /> of <Amount sats={coinsToSats(a.batchneeded)} /> needed</span>
            {:else}
              <span id="bid-line-batch">Fees ready for an anchor: <Amount sats={coinsToSats(a.pot)} /></span>
            {/if}
          {:else}
            <span>Bid now: {#if bid.bidding}<Amount sats={coinsToSats(bid.effective)} />{:else}{t('none')}{/if}</span>
          {/if}
          {#if a?.nextheartbeat !== undefined && (inForce?.heartbeathours ?? 1) > 0}
            <span id="bid-line-heartbeat">{a.nextheartbeat > now ? t('heartbeat in %1', shortDuration(a.nextheartbeat - now)) : t('heartbeat due')}</span>
          {/if}
          {#if a?.valueatrisk !== undefined && a.valueatrisk > 0}
            <span id="bid-line-risk">unanchored value <Amount sats={coinsToSats(a.valueatrisk)} />{#if inForce?.valueatrisk}&nbsp;/ <Amount sats={coinsToSats(inForce.valueatrisk)} />{/if}</span>
          {/if}
          {#if a?.reserve !== undefined}
            <span id="bid-line-reserve">reserve <Amount sats={coinsToSats(a.reserve)} />{#if a.reservetarget !== undefined}&nbsp;/ <Amount sats={coinsToSats(a.reservetarget)} />{/if}{#if a.deficit}, <span class="side-warn">short by <Amount sats={coinsToSats(a.deficit)} /></span>{/if}</span>
          {/if}
          {#if a?.turn && inForce?.turns !== false && info.validator}
            <span id="bid-line-turn">{a.turn.designated ? t('your turn to bid') : t("another validator's turn")}</span>
          {/if}
          <span id="bid-line-spent">{hours === 24 ? t('spent today') : t('spent in %1 h', hours)} <Amount sats={spentSats} />{#if capSats > 0n}&nbsp;{t('of')} <Amount sats={capSats} />{/if}</span>
        </p>
      {/if}

      {#if !form}
        <div class="bid-switch" id="bid-trigger-mine">
          <span><strong>Merged-mine as a backup producer</strong></span>
          {#if mining}{@render mining()}{/if}
        </div>
      {:else}
        <!-- 1. How much a bid is: four plain cards. -->
        <fieldset class="bid-plans">
          <legend>How to pay for anchors</legend>
          <div class="bid-cards">
            {#each PLANS as p (p.id)}
              <label class="bid-card" class:on={plan === p.id}>
                <input type="radio" name="bid-plan" value={p.id} checked={plan === p.id} onchange={() => choose(p.id)} />
                <span>
                  <strong>{t(p.label)}</strong>
                  <span class="faint">{t(!has837 && p.older ? p.older : p.text, mainName)}</span>
                  {#if p.id === 'recommended' && plan === 'recommended'}
                    {#if form.strategy === 'feesminimum'}<span class="badge" id="bid-with-minimum">with a minimum on every block</span>{/if}
                    {#if adjusted}
                      <span class="faint bid-adjusted" id="bid-adjusted">Adjusted under Advanced. <button type="button" class="link" id="bid-recommended-reset" onclick={useRecommended}>Use the recommended values</button></span>
                    {/if}
                  {/if}
                </span>
              </label>
            {/each}
          </div>
        </fieldset>

        {#if off}
          <p class="side-note side-warn" role="alert" id="bid-off-warning">With no bids of its own, this node leaves its anchors to other bidders: if none bids, blocks of stake halt after {haltAfter} without one.</p>
        {:else}
          {#if form.strategy === 'feesminimum' || form.strategy === 'minimum' || form.strategy === 'fixed'}
            <div class="anchor-grid" id="bid-amounts">
              {#if form.strategy === 'fixed'}
                <AmountInput id="bid-fixed" label="Fixed amount" bind:value={fixed} />
              {:else}
                <AmountInput id="bid-floor" label="Minimum (floor)" bind:value={floor} />
              {/if}
            </div>
            {#if form.strategy !== 'fixed'}<p class="side-note fb-small">The minimum is your floor, or the relay minimum of a request on {mainName} if higher (0: the relay minimum).</p>{/if}
          {/if}
          <label class="check bid-stall">
            <input id="bid-preventstall" type="checkbox" bind:checked={form.preventstall} onchange={() => (dirty = true)} />
            <span>
              <strong>Prevent stalling</strong>
              <span class="faint">When deposits, withdrawals or validator changes wait, or the chain nears its limit without an anchor ({haltAfter}), bids enough to land one, whatever the plan, within the limits.</span>
            </span>
          </label>
        {/if}

        <!-- 2. What the node bids for: two separate switches. -->
        <h3 class="bid-sub">What this node bids for</h3>
        <div class="bid-switches">
          {#if info.validator}
            <label class="check bid-switch" class:on={info.policy.anchor && info.policy.strategy !== 'off'} id="bid-trigger-anchor">
              <input id="bid-anchor" type="checkbox" checked={info.policy.anchor} disabled={busy || info.policy.strategy === 'off'} onchange={setAnchor} />
              <span>
                <strong>Anchor my validator</strong>
                <span class="faint">Bids to anchor your blocks of stake on {mainName}. That keeps the chain alive (it halts after {haltAfter} without an anchor) and takes deposits, withdrawals and validator changes in. Keep this on.</span>
                {#if info.policy.strategy === 'off'}<span class="faint">Bidding is off: choose a plan above to bid.</span>{/if}
              </span>
            </label>
          {/if}
          <div class="bid-switch" id="bid-trigger-mine">
            <span>
              <strong>Merged-mine as a backup producer</strong>
              <span class="faint">Builds its own blocks and bids for them. While blocks of stake run, these carry no transactions: they take over only if blocks of stake stop. For pools, or nodes without stake.</span>
            </span>
            {#if mining}{@render mining()}{/if}
          </div>
        </div>

        <!-- 3. Every number, collapsed. -->
        <label class="check"><input id="bid-advanced" type="checkbox" bind:checked={advanced} /><span>Advanced</span></label>
        {#if advanced}
          <div class="bid-advanced" id="bid-advanced-fields">
            <h3 class="bid-sub">Limits</h3>
            <div class="anchor-grid">
              <AmountInput id="bid-max" label="Maximum per bid" bind:value={max} />
              <AmountInput id="bid-periodcap" label={hours === 24 ? t('Daily cap (0: none)') : t('Cap per %1 h (0: none)', hours)} bind:value={periodcap} />
              <div class="field"><label for="bid-stallreserve">Kept for stall prevention (% of the cap)</label><input id="bid-stallreserve" type="number" min="0" max="100" step="1" value={form.stallreserve ?? 50} oninput={(e) => number('stallreserve', e)} /></div>
            </div>
            <p class="side-note fb-small" id="bid-wallet">Paid from your mainchain wallet ({mainName}). Only bids that land count in the cap; stall prevention stays within it too. Every other bid spends at most the cap less the share kept, so cheap work on the chain cannot spend what keeps it from halting.</p>

            <h3 class="bid-sub">Paying from the fees</h3>
            <div class="anchor-grid">
              <div class="field"><label for="bid-feeshare">Fee share (%)</label><input id="bid-feeshare" type="number" min="0" max="100" step="0.01" bind:value={form.feeshare} oninput={() => (dirty = true)} /></div>
              {#if form.batch !== undefined}<div class="field"><label for="bid-batch">Batch (times the landing price)</label><input id="bid-batch" type="number" min="0" step="1" value={form.batch} oninput={(e) => number('batch', e)} /></div>{/if}
              {#if form.heartbeathours !== undefined}<div class="field"><label for="bid-heartbeat">Heartbeat after hours without an anchor (0: off)</label><input id="bid-heartbeat" type="number" min="0" step="0.5" value={form.heartbeathours} oninput={(e) => number('heartbeathours', e)} /></div>{/if}
              {#if form.valueatrisk !== undefined}<AmountInput id="bid-valueatrisk" label="Bid when unanchored value reaches (0: off)" bind:value={valueatrisk} />{/if}
              {#if form.reservepct !== undefined}<div class="field"><label for="bid-reservepct">Reserve: share of fee bids held back (%)</label><input id="bid-reservepct" type="number" min="0" max="100" step="1" value={form.reservepct} oninput={(e) => number('reservepct', e)} /></div>{/if}
              {#if form.reservetarget !== undefined}<AmountInput id="bid-reservetarget" label="Reserve target" bind:value={reservetarget} />{/if}
              {#if form.turnbackup !== undefined}<div class="field"><label for="bid-turnbackup">Turn backup (mainchain blocks)</label><input id="bid-turnbackup" type="number" min="0" step="1" value={form.turnbackup} disabled={form.turns === false} oninput={(e) => number('turnbackup', e)} /></div>{/if}
            </div>
            {#if form.turns !== undefined}
              <label class="check"><input id="bid-turns" type="checkbox" bind:checked={form.turns} onchange={() => (dirty = true)} /><span>Take turns with the other validators (the one whose turn it is bids first; the others after the backup)</span></label>
            {/if}
            <label class="check" class:disabled={plan !== 'recommended'}>
              <input id="bid-feesminimum" type="checkbox" checked={form.strategy === 'feesminimum'} disabled={plan !== 'recommended'} onchange={(e) => setWithMinimum((e.currentTarget as HTMLInputElement).checked)} />
              <span>Also bid the minimum on every block when the fees are short (Fees + minimum)</span>
            </label>
            {#if has837}
              <p class="side-note fb-small">With the recommended plan, a bid goes out when the fee share reaches the batch, when the heartbeat is due, when the value moved in unanchored blocks reaches the threshold, or when work waits. The reserve, filled from fee bids, pays for the bids without fees.</p>
            {:else}
              <p class="side-note fb-small">You keep the rest of the fees. A share too small to pay for a request is no bid.</p>
            {/if}

            <h3 class="bid-sub">Amounts</h3>
            <div class="anchor-grid">
              {#if form.strategy !== 'feesminimum' && form.strategy !== 'minimum'}<AmountInput id="bid-floor-advanced" label="Minimum (floor; also stall prevention's base)" bind:value={floor} />{/if}
              {#if form.strategy !== 'fixed'}<AmountInput id="bid-fixed-advanced" label="Fixed amount" bind:value={fixed} />{/if}
            </div>

            <h3 class="bid-sub">Stall prevention and missed bids</h3>
            <div class="anchor-grid">
              <div class="field"><label for="bid-start">Prevent stalling from pressure</label><input id="bid-start" type="number" min="0.01" max="1" step="0.01" bind:value={form.escalationstart} oninput={() => (dirty = true)} /></div>
              <div class="field"><label for="bid-flat"><input id="bid-flat" type="checkbox" checked={form.flatboost ?? true} onchange={(e) => { form!.flatboost = (e.currentTarget as HTMLInputElement).checked; dirty = true; }} /> Flat boost (the competitive base times the boost; off: the curve and rebids)</label></div>
              <div class="field"><label for="bid-boost">Boost (times the base)</label><input id="bid-boost" type="number" min="1" max="1000" step="0.5" value={form.boost ?? 4} oninput={(e) => number('boost', e)} /></div>
              <div class="field"><label for="bid-curve">Escalation curve (p:multiplier)</label><input id="bid-curve" type="text" class="mono" spellcheck="false" bind:value={curve} oninput={() => (dirty = true)} /></div>
              <div class="field"><label for="bid-vsize">Request size (vB)</label><input id="bid-vsize" type="number" min="50" max="10000" bind:value={form.requestvsize} oninput={() => (dirty = true)} /></div>
              <div class="field"><label for="bid-rebid">Raise after missed blocks (0: no raises)</label><input id="bid-rebid" type="number" min="0" max="1000" bind:value={form.rebidafter} oninput={() => (dirty = true)} /></div>
              {#if form.flatboost === false}
                <div class="field"><label for="bid-rebidstep">Raise by (x)</label><input id="bid-rebidstep" type="number" min="1" max="100" step="0.1" bind:value={form.rebidstep} oninput={() => (dirty = true)} /></div>
                <div class="field"><label for="bid-rebidmax">Raises at most (x)</label><input id="bid-rebidmax" type="number" min="1" max="1000" step="0.1" bind:value={form.rebidmax} oninput={() => (dirty = true)} /></div>
              {/if}
              <div class="field"><label for="bid-warn">Warn after missed blocks (0: never)</label><input id="bid-warn" type="number" min="0" max="1000" bind:value={form.warnmissed} oninput={() => (dirty = true)} /></div>
              <div class="field"><label for="bid-periodhours">Period of the cap (hours)</label><input id="bid-periodhours" type="number" min="1" max="744" bind:value={form.periodhours} oninput={() => (dirty = true)} /></div>
              <div class="field"><label for="bid-lowbalance">Low balance below (blocks covered)</label><input id="bid-lowbalance" type="number" min="0" bind:value={form.lowbalanceblocks} oninput={() => (dirty = true)} /></div>
            </div>
            <label class="check"><input id="bid-interpolate" type="checkbox" bind:checked={form.interpolate} onchange={() => (dirty = true)} /><span>Interpolate between points (else steps)</span></label>
            <label class="check"><input id="bid-estimate" type="checkbox" bind:checked={form.usefeeestimate} onchange={() => (dirty = true)} /><span>Use the next-block fee estimate of {mainName} for stall prevention</span></label>
            {#if form.flatboost !== false}<p class="side-note fb-small" id="bid-rebid-flat">With the flat boost, a raise is x2 for every two missed blocks, at most the boost; 0 above: none.</p>{/if}
          </div>
        {/if}
        {#if problem}<p class="side-note side-bad" id="bid-problem">{problem}</p>{/if}
        <div class="row">
          <button type="button" class="btn primary" id="bid-apply" disabled={busy || !!problem || !Object.keys(changes).length} onclick={apply}>Apply</button>
          <button type="button" class="btn" id="bid-reset" disabled={busy || !overridden.length} onclick={reset}>Back to the node's options</button>
        </div>
        <p class="side-note fb-small">
          The plan and Advanced take effect with Apply; Anchor my validator and merged mining at once. Nothing needs a restart, and changes are kept in the node's settings over its options (-validatoranchorbid…). The desktop asks first: they spend the wallet of the {mainName} node.
          {#if overridden.length}{t('Changed from the options: %1.', overridden.join(', '))}{/if}
        </p>
      {/if}

      <!-- The details: the numbers behind the status line, and the last bids. -->
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
          {#if windows.day}
            <div id="bid-day">
              <span>Anchors (24 h)</span>
              <strong>{#if windows.day.collected !== undefined}<Amount sats={coinsToSats(windows.day.collected)} />{:else}–{/if}</strong>
              <span class="faint">{#if windows.day.paid !== undefined}{t('collected; paid')} <Amount sats={coinsToSats(windows.day.paid)} />{/if}</span>
            </div>
          {/if}
          {#if windows.week}
            <div id="bid-week">
              <span>Anchors (7 d)</span>
              <strong>{#if windows.week.collected !== undefined}<Amount sats={coinsToSats(windows.week.collected)} />{:else}–{/if}</strong>
              <span class="faint">{#if windows.week.paid !== undefined}{t('collected; paid')} <Amount sats={coinsToSats(windows.week.paid)} />{/if}</span>
            </div>
          {/if}
        </div>
        {#each bid.warnings as warning, i (i)}
          <p class="side-note side-bad bid-warning" role="alert"><SafeText text={warning} /></p>
        {/each}
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
  .bid-sub {
    margin: 0;
    font-size: 0.95rem;
    font-weight: 600;
  }
  .bid-line {
    display: flex;
    flex-wrap: wrap;
    gap: 0.2rem 0;
    max-width: none;
    padding: 0.55rem 0.8rem;
    border-radius: var(--radius-m);
    background: var(--surface-2);
    border: 1px solid var(--line);
  }
  .bid-line > span + span::before {
    content: '·';
    margin: 0 0.5rem;
    color: var(--text-faint);
  }
  p.bid-line.side-warn {
    background: var(--warning-soft);
    border-color: transparent;
    display: block;
  }
  .bid-plans {
    border: 0;
    padding: 0;
    margin: 0;
    min-width: 0;
  }
  .bid-plans legend {
    font-weight: 600;
    margin-bottom: 6px;
    padding: 0;
  }
  .bid-cards {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(min(14rem, 100%), 1fr));
    gap: 0.6rem;
  }
  .bid-card,
  .bid-switch {
    display: flex;
    gap: 0.6rem;
    align-items: flex-start;
    padding: 0.7rem 0.8rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-m);
    background: var(--surface-2);
    min-width: 0;
  }
  .bid-card {
    cursor: pointer;
  }
  .bid-card:hover,
  .bid-card.on,
  .bid-switch.on {
    border-color: var(--accent);
    background: var(--raised);
  }
  .bid-card > span,
  .bid-stall > span,
  .bid-switch > span {
    display: grid;
    gap: 3px;
    min-width: 0;
  }
  .bid-card input,
  .bid-switch input[type='checkbox'] {
    margin-top: 0.15rem;
    flex: none;
  }
  .bid-card .badge {
    justify-self: start;
  }
  .bid-adjusted .link {
    padding: 0;
    border: 0;
    background: none;
    color: var(--link);
    cursor: pointer;
    font: inherit;
  }
  .bid-switches {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(22rem, 100%), 1fr));
    gap: 0.6rem;
    align-items: start;
  }
  div.bid-switch {
    flex-direction: column;
  }
  label.bid-switch {
    cursor: pointer;
  }
  .bid-stall {
    display: flex;
    gap: 8px;
    align-items: flex-start;
    padding: 0 8px;
  }
  .check.disabled {
    opacity: 0.6;
  }
  .bid-advanced {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
  }
  .anchor-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(min(220px, 100%), 1fr));
    gap: 8px 12px;
  }
</style>
