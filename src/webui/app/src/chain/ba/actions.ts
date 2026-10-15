// Svelte actions of the BitAssets pages.

/** The width of a bar as a fraction of its track, set through the CSS object model (no inline style: the content security policy). */
export function barWidth(node: HTMLElement, fraction: number) {
  const set = (f: number) => node.style.setProperty('width', `${Math.max(0, Math.min(1, Number.isFinite(f) ? f : 0)) * 100}%`);
  set(fraction);
  return { update: set };
}
