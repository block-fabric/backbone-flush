// Verified issuers, as the pages show them (ba/verify.ts): the curators the user trusts, kept with the app's preferences
// (per network, as oracle ids are; key trustedCurators, which the kit leaves alone), and what getverifications says of each
// asset with them, asked once a minute at most per asset. No curator: no lookup and no badge.
import { host, rpc } from '../../lib/rpc';
import { assetArg } from './model';
import { cleanCurators, fresh, lookupKey, MAX_CURATORS, type Curator, type Verifications } from './verify';

export const verify = $state({ curators: [] as Curator[], loaded: false, version: 0 });

/** Lookups by asset: the key asked (asset and curators), when, and the answer (null: the node refused it). */
const cache = $state<Record<string, { key: string; at: number; value: Verifications | null }>>({});
const asking = new Map<string, Promise<void>>();

let loading: Promise<void> | null = null;

/** Read the curators from the preferences, once. */
export function loadCurators(): Promise<void> {
  loading ??= host
    .prefs()
    .then((raw) => {
      verify.curators = cleanCurators(raw?.trustedCurators);
    })
    .catch(() => undefined)
    .finally(() => {
      verify.loaded = true;
    });
  return loading;
}

function save(): void {
  verify.version++;
  host.setPrefs({ trustedCurators: verify.curators.map((c) => ({ id: c.id, name: c.name })) }).catch(() => undefined);
}

/** Trust a curator (its id; its name as the chain gives it). */
export function addCurator(id: string, name: string): void {
  const key = id.trim().toLowerCase();
  if (verify.curators.some((c) => c.id === key)) return;
  verify.curators = cleanCurators([...verify.curators, { id: key, name }]).slice(0, MAX_CURATORS);
  save();
}

export function removeCurator(id: string): void {
  verify.curators = verify.curators.filter((c) => c.id !== id);
  save();
}

/** The ids of the curators trusted. */
export function curatorIds(): string[] {
  return verify.curators.map((c) => c.id);
}

/** What the curators trusted say of an asset, as last asked (null: not asked yet, or no curator). */
export function verificationOf(asset: string): Verifications | null {
  const ids = curatorIds();
  if (ids.length === 0) return null;
  const entry = cache[asset];
  return entry && entry.key === lookupKey(asset, ids) ? entry.value : null;
}

/** Ask the node of an asset, unless asked within the minute with the same curators. */
export function ensureVerification(asset: string): void {
  const ids = curatorIds();
  if (ids.length === 0 || !asset) return;
  const key = lookupKey(asset, ids);
  const entry = cache[asset];
  if (entry && entry.key === key && fresh(entry.at, Date.now())) return;
  if (asking.has(key)) return;
  const done = rpc
    .raw<Verifications>('getverifications', [assetArg(asset), ids])
    .then((v) => {
      cache[asset] = { key, at: Date.now(), value: v && typeof v === 'object' && Array.isArray(v.curators) ? v : null };
    })
    .catch(() => {
      cache[asset] = { key, at: Date.now(), value: null };
    })
    .finally(() => asking.delete(key));
  asking.set(key, done);
}

/** For the tests: forget the curators and the lookups. */
export function resetVerify(): void {
  loading = null;
  verify.curators = [];
  verify.loaded = false;
  for (const k of Object.keys(cache)) delete cache[k];
  asking.clear();
}
