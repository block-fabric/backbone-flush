<script lang="ts">
  // The logo of an asset, picked from any picture of the user: cropped to a square (drag it, or the arrow keys; the
  // zoom), scaled to 256 x 256, encoded as small as it gets (logo.ts: a palette PNG, a full-colour PNG or a JPEG, no
  // metadata), checked as the rules check it, here and by the host (bitassets.checkLogo). Shows the result, its size
  // and about the fee: the logo goes whole into the transaction. The picture is decoded by the web view (an image
  // only); nothing of the file but the pixels goes on the chain.
  import Icon from '../../components/Icon.svelte';
  import { request } from '../../lib/bridge';
  import { t } from '../../lib/i18n.svelte';
  import { checkLogo, cropRect, encodeLogo, LOGO_SIZE, logoVsize, MAX_LOGO_BYTES, toBase64, type LogoFormat } from './logo';

  export interface PickedLogo {
    base64: string;
    size: number;
    format: LogoFormat;
    /** How it was made ("PNG, 12 colours", "JPEG 80%"). */
    kind: string;
    /** A data: URL of it, for the previews (AssetBadge preview). */
    url: string;
  }

  let { logo = $bindable(null), id = 'logo' }: { logo?: PickedLogo | null; id?: string } = $props();

  /** The largest file read, and the largest picture (pixels a side): beyond, the web view is asked for too much. */
  const MAX_FILE = 25 * 1024 * 1024;
  const MAX_SIDE = 12000;
  const VIEW = 192;

  let bitmap = $state<ImageBitmap | null>(null);
  let zoom = $state(1);
  let cx = $state(0.5);
  let cy = $state(0.5);
  let error = $state('');
  let busy = $state(false);
  let canvas = $state<HTMLCanvasElement | null>(null);
  let input = $state<HTMLInputElement | null>(null);

  async function pick(file: File | undefined) {
    error = '';
    if (!file) return;
    if (file.size > MAX_FILE) {
      error = t('The file is too large to read (at most 25 MB).');
      return;
    }
    try {
      const b = await createImageBitmap(file);
      if (b.width > MAX_SIDE || b.height > MAX_SIDE || b.width < 1 || b.height < 1) {
        b.close();
        error = t('The picture is too large (at most 12000 pixels a side).');
        return;
      }
      bitmap?.close();
      bitmap = b;
      zoom = 1;
      cx = 0.5;
      cy = 0.5;
    } catch {
      error = t('This file is not a picture the app can read.');
    }
  }

  /** The 256 x 256 pixels of the crop, drawn into the canvas shown. */
  function draw(): Uint8ClampedArray | null {
    if (!bitmap || !canvas) return null;
    const ctx = canvas.getContext('2d', { willReadFrequently: true });
    if (!ctx) return null;
    const r = cropRect(bitmap.width, bitmap.height, zoom, cx, cy);
    ctx.clearRect(0, 0, LOGO_SIZE, LOGO_SIZE);
    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = 'high';
    ctx.drawImage(bitmap, r.sx, r.sy, r.size, r.size, 0, 0, LOGO_SIZE, LOGO_SIZE);
    return ctx.getImageData(0, 0, LOGO_SIZE, LOGO_SIZE).data;
  }

  function jpegOf(source: HTMLCanvasElement) {
    return (quality: number) =>
      new Promise<Uint8Array | null>((resolve) => {
        // On white: a JPEG has no transparency.
        const flat = document.createElement('canvas');
        flat.width = flat.height = LOGO_SIZE;
        const ctx = flat.getContext('2d');
        if (!ctx) return resolve(null);
        ctx.fillStyle = '#fff';
        ctx.fillRect(0, 0, LOGO_SIZE, LOGO_SIZE);
        ctx.drawImage(source, 0, 0);
        flat.toBlob((blob) => (blob ? blob.arrayBuffer().then((a) => resolve(new Uint8Array(a)), () => resolve(null)) : resolve(null)), 'image/jpeg', quality);
      });
  }

  /** What a reason of the rules (bad-ba-logo-*) means, in words. */
  function reasonText(reason: string): string {
    if (reason === 'bad-ba-logo-oversize') return t('The file is larger than %1 bytes.', String(MAX_LOGO_BYTES));
    if (reason === 'bad-ba-logo-size') return t('The picture is not 256 x 256 pixels.');
    if (reason === 'bad-ba-logo-format') return t('Not a PNG or a JPEG.');
    if (reason === 'bad-ba-logo-trailing') return t('Something follows the end of the picture.');
    if (reason.includes('metadata') || reason.includes('chunk')) return t('The file holds more than the picture (metadata).');
    return t('The chain does not take this file (%1).', reason);
  }

  let run = 0;
  async function encode() {
    const mine = ++run;
    const pixels = draw();
    if (!pixels || !canvas) return;
    busy = true;
    try {
      const made = await encodeLogo(new Uint8Array(pixels.buffer.slice(0)), jpegOf(canvas));
      if (mine !== run) return;
      if (!made) {
        logo = null;
        error = t('This picture does not fit in %1 bytes, even as a JPEG of low quality: try a simpler one.', String(MAX_LOGO_BYTES));
        return;
      }
      const local = checkLogo(made.bytes);
      if (!local.ok) {
        logo = null;
        error = reasonText(local.reason);
        return;
      }
      const base64 = toBase64(made.bytes);
      // The host checks it as the rules do (and as the confirmation will): its word decides.
      try {
        const host = await request<{ ok: boolean; reason?: string }>('host', 'bitassets.checkLogo', { base64 });
        if (mine !== run) return;
        if (host && host.ok === false) {
          logo = null;
          error = reasonText(host.reason ?? '');
          return;
        }
      } catch {
        // Without the host (the tests), the page's check stands; the confirmation checks again.
      }
      error = '';
      logo = { base64, size: made.bytes.length, format: made.format, kind: made.kind, url: `data:image/${made.format};base64,${base64}` };
    } finally {
      if (mine === run) busy = false;
    }
  }

  // Encoded again a moment after the crop last changed: dragging stays smooth.
  $effect(() => {
    void [bitmap, zoom, cx, cy, canvas];
    if (!bitmap) return;
    draw();
    const timer = setTimeout(() => void encode(), 250);
    return () => clearTimeout(timer);
  });

  let dragging: { x: number; y: number; cx: number; cy: number } | null = null;
  function scale(): { fx: number; fy: number } {
    if (!bitmap) return { fx: 0, fy: 0 };
    const r = cropRect(bitmap.width, bitmap.height, zoom, cx, cy);
    return { fx: r.size / VIEW / bitmap.width, fy: r.size / VIEW / bitmap.height };
  }
  function down(e: PointerEvent) {
    if (!bitmap) return;
    dragging = { x: e.clientX, y: e.clientY, cx, cy };
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
  }
  function move(e: PointerEvent) {
    if (!dragging) return;
    const { fx, fy } = scale();
    cx = Math.min(1, Math.max(0, dragging.cx - (e.clientX - dragging.x) * fx));
    cy = Math.min(1, Math.max(0, dragging.cy - (e.clientY - dragging.y) * fy));
  }
  function up() {
    dragging = null;
  }
  function key(e: KeyboardEvent) {
    const step = e.shiftKey ? 0.1 : 0.02;
    const moves: Record<string, [number, number]> = { ArrowLeft: [-step, 0], ArrowRight: [step, 0], ArrowUp: [0, -step], ArrowDown: [0, step] };
    if (e.key === '+' || e.key === '=') zoom = Math.min(4, zoom + 0.1);
    else if (e.key === '-') zoom = Math.max(1, zoom - 0.1);
    else if (moves[e.key]) {
      cx = Math.min(1, Math.max(0, cx + moves[e.key][0]));
      cy = Math.min(1, Math.max(0, cy + moves[e.key][1]));
    } else return;
    e.preventDefault();
  }

  function clear() {
    run++;
    bitmap?.close();
    bitmap = null;
    logo = null;
    error = '';
    busy = false;
    if (input) input.value = '';
  }

  const vsize = $derived(logo ? logoVsize(logo.size) : 0);
  const kb = (n: number) => (n / 1024).toFixed(1);
</script>

<div class="logo-picker" data-testid="logo-picker">
  <input bind:this={input} id="{id}-file" class="file" type="file" accept="image/png,image/jpeg,image/webp,image/gif,image/bmp,image/avif,image/*" onchange={(e) => void pick((e.currentTarget as HTMLInputElement).files?.[0])} />
  <div class="row">
    <label for="{id}-file" class="btn small"><Icon name="picture" size={14} /> {bitmap ? 'Choose another picture…' : 'Choose a picture…'}</label>
    {#if bitmap}<button type="button" class="btn ghost small" onclick={clear}>No logo</button>{/if}
  </div>
  {#if bitmap}
    <div class="crop">
      <canvas
        bind:this={canvas}
        width={LOGO_SIZE}
        height={LOGO_SIZE}
        class="view"
        tabindex="0"
        aria-label="The logo: drag the picture, or use the arrow keys, to choose what it shows; + and - zoom"
        onpointerdown={down}
        onpointermove={move}
        onpointerup={up}
        onpointercancel={up}
        onkeydown={key}
      ></canvas>
      <div class="controls">
        <label for="{id}-zoom">Zoom</label>
        <input id="{id}-zoom" type="range" min="1" max="4" step="0.01" bind:value={zoom} />
        <span class="hint">Drag the picture to place it; it becomes 256 x 256 pixels.</span>
        {#if busy}<span class="hint" role="status">Encoding…</span>
        {:else if logo}
          <span class="hint ba-good" role="status" data-testid="logo-size">{logo.kind}: {kb(logo.size)} KB of at most {kb(MAX_LOGO_BYTES)} KB</span>
          <span class="hint">About {vsize} vB on the chain: about {(vsize / 1e8).toFixed(8)} ECX at 1 sat/vB. The confirmation shows the fee.</span>
        {/if}
      </div>
    </div>
  {:else}
    <span class="hint">Optional. Any picture: it is cropped to a square, made 256 x 256 pixels and at most {kb(MAX_LOGO_BYTES)} KB, without its metadata, and stored on the chain.</span>
  {/if}
  {#if error}<span class="error" role="alert">{error}</span>{/if}
</div>

<style>
  .logo-picker {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
  }
  .file {
    position: absolute;
    width: 1px;
    height: 1px;
    opacity: 0;
    pointer-events: none;
  }
  .row {
    display: flex;
    gap: 0.5rem;
    align-items: center;
  }
  .crop {
    display: flex;
    gap: 1rem;
    align-items: flex-start;
    flex-wrap: wrap;
  }
  .view {
    width: 192px;
    height: 192px;
    border-radius: var(--radius-m);
    border: 1px solid var(--line-strong);
    background: var(--surface-2);
    cursor: grab;
    touch-action: none;
  }
  .view:active {
    cursor: grabbing;
  }
  .view:focus-visible {
    outline: 2px solid var(--focus);
    outline-offset: 2px;
  }
  .controls {
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
    min-width: 12rem;
    flex: 1;
  }
</style>
