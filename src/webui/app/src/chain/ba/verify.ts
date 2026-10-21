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

/** The start of an id people compare at a glance: 12 hex digits. */
export function idPrefix(id: string): string {
  return `${id.slice(0, 12)}…`;
}

/**
 * The trusted curators that verify an asset now, each by name and its id's start (names are labels anyone may give an
 * oracle; the id is the curator) and the round of its word ("Alpha (68905ccbb1f5…) #2"): none when no badge.
 */
export function verifiedBy(v: Verifications | null | undefined): string[] {
  if (!v || !Array.isArray(v.curators)) return [];
  return v.curators.filter((c) => c.verified).map((c) => `${c.name ? `${c.name} (${idPrefix(c.curator)})` : idPrefix(c.curator)} #${c.round}`);
}

/** Look-alike letters and digits, to one form each (a name like a trusted one is flagged, whatever its spelling). */
const CONFUSABLE: Record<string, string> = {
  '0': 'o', '1': 'l', i: 'l', '|': 'l', '!': 'l', '3': 'e', '4': 'a', '5': 's', '$': 's', '7': 't', '8': 'b', '9': 'g', '6': 'g',
  а: 'a', е: 'e', о: 'o', р: 'p', с: 'c', у: 'y', х: 'x', і: 'l', ј: 'j', ѕ: 's', ԁ: 'd', ɡ: 'g', ο: 'o', α: 'a', ν: 'v', ρ: 'p', τ: 't', κ: 'k', ι: 'l',
};

/**
 * A name's skeleton: Unicode compatibility form, lower case, marks and everything but letters and digits dropped,
 * look-alikes to one form, "rn" as "m", "vv" as "w". Two names of the same skeleton read alike.
 */
export function nameSkeleton(name: string): string {
  const base = name.normalize('NFKD').replace(/\p{M}/gu, '').toLowerCase();
  let out = '';
  for (const ch of base) {
    const mapped = CONFUSABLE[ch] ?? ch;
    if (/[\p{L}\p{N}]/u.test(mapped)) out += mapped;
  }
  return out.replace(/rn/g, 'm').replace(/vv/g, 'w');
}

/** An oracle as the node lists it (listoracles) or names it (getverifications). */
export interface OracleSeen {
  oracle: string;
  name: string;
}

/**
 * What to warn of before trusting an oracle by its name: other oracles that go by a name alike (a name is a label, anyone
 * may take one), and a trusted curator of a name alike under another id (one posing as it).
 */
export function curatorWarnings(candidate: OracleSeen, registered: readonly OracleSeen[], trusted: readonly Curator[]): string[] {
  const warnings: string[] = [];
  const id = candidate.oracle.toLowerCase();
  const skeleton = nameSkeleton(candidate.name);
  if (!skeleton) {
    warnings.push('It has no name: compare its id with the one its curator publishes.');
    return warnings;
  }
  const alike = registered.filter((o) => o.oracle.toLowerCase() !== id && nameSkeleton(o.name) === skeleton);
  if (alike.length) warnings.push(`${alike.length === 1 ? 'Another oracle goes' : `${alike.length} other oracles go`} by a name like it: only the id tells them apart.`);
  const posing = trusted.filter((c) => c.id !== id && c.name && nameSkeleton(c.name) === skeleton);
  if (posing.length) warnings.push(`You already trust a curator of a name like it, with another id (${posing.map((c) => idPrefix(c.id)).join(', ')}): this may be one posing as it.`);
  return warnings;
}
