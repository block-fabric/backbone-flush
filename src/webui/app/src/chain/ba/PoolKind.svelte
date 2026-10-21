<script lang="ts">
  // The kind of a pool, wherever pools are listed (Layers' DEX: ba/kinds.ts): a badge of its curve, its fee, and its
  // amplification (stable) or its weights (weighted); a dynamic fee at its value for the next block, tagged; the kind
  // addliquidity makes marked "default" when asked.
  import { t } from '../../lib/i18n.svelte';
  import { CURVE_NAMES, DYNAMIC_FEE_HELP, feeNow, feePercent, type PoolKind } from './kinds';

  let { kind, showDefault = false }: { kind: PoolKind; showDefault?: boolean } = $props();
</script>

<span class="pool-kind" data-curve={kind.curve}>
  <span class="ba-kind {kind.curve}">{t(CURVE_NAMES[kind.curve])}</span>
  <span class="fee" translate="no">{feePercent(feeNow(kind))}</span>
  {#if kind.dynamic_fee}<span class="ba-tag warn" title={t(DYNAMIC_FEE_HELP)}>dynamic</span>{/if}
  {#if kind.curve === 'stable' && kind.amp !== undefined}<span class="param">A {kind.amp}</span>{/if}
  {#if kind.curve === 'weighted' && kind.weight_a !== undefined}<span class="param" translate="no">{kind.weight_a}/{kind.weight_b ?? 100 - kind.weight_a}</span>{/if}
  {#if showDefault && kind.default_kind}<span class="ba-tag">default</span>{/if}
</span>

<style>
  .pool-kind {
    display: inline-flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.3rem;
    font-size: 0.8rem;
  }
  .fee,
  .param {
    color: var(--text-muted);
    font-variant-numeric: tabular-nums;
    white-space: nowrap;
  }
</style>
