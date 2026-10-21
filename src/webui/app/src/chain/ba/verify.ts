// Verified issuers (Layers' DEX, not a rule of the chain: what wallets show): curators, oracles that attest that an asset is
// what it says (src/rpc/bitassets.cpp getverifications). The user names the curators they trust; an asset is shown
// "Verified" when one of them verifies it now. With none named, no badge and no lookup (getverifications takes 1 to 200
// curators, never all the oracles). Pure: the ids kept, the cache's rule, who
// verifies (tested in chain/ba/verify.test.ts); the state is verify.svelte.ts.

/** Most curators kept (getverifications asks at most 200). */
export const MAX_CURATORS = 50;
/** How long a lookup of an asset is kept, in milliseconds. */
export const VERIFY_TTL = 60_000;

/** What getverifications answers. */
export interface Verifications {
  asset: string;
  verified: boolean;
  curators: { curator: string; name: string; verified: boolean; round: number; height?: number }[];
}

/** A curator kept: its id (an oracle's, 64 hex digits) and the name the chain gave it when added (a label, text of the chain). */
export interface Curator {
  id: string;
  name: string;
}

/** Whether text is an oracle's id: 64 hex digits. */
export function isCuratorId(text: string): boolean {
  return /^[0-9a-f]{64}$/i.test(text.trim());
}

/** The curators of the preferences, as kept: valid ids (lower case), once each, at most MAX_CURATORS. */
export function cleanCurators(raw: unknown): Curator[] {
  if (!Array.isArray(raw)) return [];
  const out: Curator[] = [];
  const seen = new Set<string>();
  for (const x of raw) {
    const id = typeof x === 'string' ? x : x && typeof x === 'object' && typeof (x as Curator).id === 'string' ? (x as Curator).id : '';
    if (!isCuratorId(id)) continue;
    const key = id.trim().toLowerCase();
    if (seen.has(key)) continue;
    seen.add(key);
    const name = x && typeof x === 'object' && typeof (x as Curator).name === 'string' ? (x as Curator).name.slice(0, 64) : '';
    out.push({ id: key, name });
    if (out.length >= MAX_CURATORS) break;
  }
  return out;
}

/** The key of a lookup: the asset and the curators asked (another list of curators asks again). */
export function lookupKey(asset: string, curators: readonly string[]): string {
  return `${asset}|${[...curators].sort().join(',')}`;
}

/** Whether a lookup made at `at` still holds at `now`. */
export function fresh(at: number, now: number, ttl = VERIFY_TTL): boolean {
  return now - at >= 0 && now - at < ttl;
}

/**
 * The trusted curators that verify an asset now, each by name (its id's start when nameless) and the round of its word
 * ("Alpha #2"): none when no badge.
 */
export function verifiedBy(v: Verifications | null | undefined): string[] {
  if (!v || !Array.isArray(v.curators)) return [];
  return v.curators.filter((c) => c.verified).map((c) => `${c.name ? c.name : `${c.curator.slice(0, 12)}…`} #${c.round}`);
}
