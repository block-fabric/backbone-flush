<script lang="ts">
  // Typing an amount of an asset: checked as the chain checks it, with the decimals of the asset (at most
  // that many; a whole number for an asset with none). The value is in units (null while the text is not an
  // amount). "Max" fills in what the wallet holds, when given.
  import { t } from '../../lib/i18n.svelte';
  import { formatUnits, parseUnits } from './model';

  let {
    id,
    label,
    decimals,
    units = $bindable(null),
    text = $bindable(''),
    max = null,
    allowZero = false,
    placeholder = '0',
    readonly = false,
    disabled = false,
    unit = '',
    hint = '',
    error = $bindable(''),
  }: {
    id: string;
    label: string;
    decimals: number;
    units?: bigint | null;
    text?: string;
    /** What the wallet holds: "Max", and more than it refused. */
    max?: bigint | null;
    allowZero?: boolean;
    placeholder?: string;
    readonly?: boolean;
    disabled?: boolean;
    /** The ticker shown in the field (text of the chain). */
    unit?: string;
    hint?: string;
    /** Out: what is wrong with the text typed ('' when it is an amount, or empty). */
    error?: string;
  } = $props();

  const parsed = $derived(text.trim() === '' ? null : parseUnits(text, decimals, { allowZero }));
  const problem = $derived.by(() => {
    if (parsed === null) return '';
    if (!parsed.ok) return parsed.error;
    if (max !== null && max !== undefined && parsed.units > max) return 'More than this wallet holds.';
    return '';
  });
  $effect(() => {
    error = problem;
    units = parsed && parsed.ok && !problem ? parsed.units : null;
  });
</script>

<div class="field units-input">
  <label for={id}>{label}</label>
  <div class="box" class:bad={!!problem}>
    <input {id} type="text" inputmode="decimal" autocomplete="off" spellcheck="false" {placeholder} {readonly} {disabled} bind:value={text} aria-invalid={problem ? 'true' : undefined} aria-describedby={problem || hint ? `${id}-note` : undefined} />
    {#if unit}<span class="unit" translate="no">{unit}</span>{/if}
    {#if max !== null && max !== undefined && !readonly}
      <button type="button" class="btn ghost small" disabled={disabled || max <= 0n} onclick={() => (text = formatUnits(max ?? 0n, decimals, { group: false }))}>Max</button>
    {/if}
  </div>
  {#if problem}<span class="error" id="{id}-note">{t(problem)}</span>{:else if hint}<span class="hint" id="{id}-note">{t(hint)}</span>{/if}
</div>

<style>
  .box {
    display: flex;
    align-items: center;
    gap: 0.35rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-s);
    background: var(--surface);
    padding-right: 0.3rem;
  }
  .box:focus-within {
    border-color: var(--accent);
  }
  .box.bad {
    border-color: var(--negative);
  }
  .box input {
    flex: 1 1 auto;
    min-width: 0;
    border: 0;
    background: transparent;
    font-variant-numeric: tabular-nums;
  }
  .box input:focus-visible {
    outline: none;
    box-shadow: none;
  }
  .unit {
    color: var(--text-muted);
    font-size: 0.85rem;
    font-weight: 600;
    max-width: 9rem;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
</style>
