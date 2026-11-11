// The routes of the asset pages (owner decision, 2026-11-11: every list row opens the asset's page, and every action on
// an asset starts from it). Pure: no state, no calls (tested in chain/ba/assetactions.test.ts).

/**
 * The route of each place an action leads (nav.page, nav.tab: chain/local.ts dexRoute), with the asset in it, so that the
 * page reads it from the route and back, forward and a reload keep it: an asset's page, Assets "asset/<id>", with the
 * box it opens on, "asset/<id>/buy", "/sell" (its Trade box on that side) or "/liquidity" (its Add liquidity box);
 * Swap "swap/get/<id>" or "swap/pay/<id>"; a pool's page, Pools "pools/<pool>". A form always opens from an asset or a
 * pool with that asset fixed (owner decision, 2026-11-11): no page hands another an asset any other way.
 */
export type AssetBox = '' | 'buy' | 'sell' | 'liquidity';
export function assetRoute(id: string, box: AssetBox = ''): string {
  return box ? `asset/${id}/${box}` : `asset/${id}`;
}
export function swapRoute(id: string, side: 'get' | 'pay'): string {
  return `swap/${side}/${id}`;
}
export function poolRoute(pool: string): string {
  return `pools/${pool}`;
}

/** What a route names (the parts after its head): an asset and its side for Swap, an asset's page and its box, a pool. */
export function parseDexRoute(tab: string): { swap?: { side: 'get' | 'pay'; id: string }; asset?: { id: string; box: AssetBox }; pool?: string } {
  const [head, a, b] = tab.split('/');
  // An id: 64 characters (hex on the chain; letters or digits in the tests).
  const hex = (x: string | undefined) => !!x && /^[0-9a-z]{64}$/i.test(x);
  if (head === 'swap' && (a === 'get' || a === 'pay') && hex(b)) return { swap: { side: a, id: b } };
  if (head === 'asset' && hex(a)) return { asset: { id: a, box: b === 'buy' || b === 'sell' || b === 'liquidity' ? b : '' } };
  if (head === 'pools' && hex(a)) return { pool: a };
  return {};
}
