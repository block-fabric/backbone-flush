// The logos of assets as the pages show them: a data: URL the host makes of the bytes the chain has (bitassets.logo,
// src/webui/local.cpp: checked as the rules check them, of the type they found), for an <img> only, never markup.
// Asked once per asset and hash (listassets gives the hash), a few at a time, so that a page of a thousand assets
// stays quick; an asset without one is shown by its initials (AssetBadge).
import { request } from '../../lib/bridge';
import { ba } from './store.svelte';

/** What the host gives: the logo of an asset, or null. */
export interface LogoImage {
  asset: string;
  hash: string;
  mime: 'image/png' | 'image/jpeg';
  size: number;
  url: string;
}

/** At most so many asked of the host at once, and kept. */
export const MAX_CONCURRENT = 4;
export const MAX_KEPT = 1000;
const DATA_URL = /^data:image\/(png|jpeg);base64,[A-Za-z0-9+/]+={0,2}$/;

const shown = $state<Record<string, string>>({});
const asked = new Map<string, Promise<void>>();
const queue: (() => Promise<void>)[] = [];
let running = 0;

let hashes = new Map<string, string>();
let hashesOf: unknown = null;

/** The hash of the logo of an asset, as the list of assets says it (none: no logo). */
export function logoHashOf(id: string): string | undefined {
  // Read through ba.assets, so that a page shows a new logo after the refresh that lists it.
  const assets = ba.assets;
  if (hashesOf !== assets) {
    hashes = new Map(assets.filter((a) => a.logo?.hash).map((a) => [a.asset, a.logo!.hash]));
    hashesOf = assets;
  }
  return hashes.get(id);
}

/** Whether a value is a data: URL the pages show as a logo: PNG or JPEG in base64, nothing else. */
export function isLogoUrl(url: unknown): url is string {
  return typeof url === 'string' && url.length <= 44 * 1024 && DATA_URL.test(url);
}

function pump(): void {
  while (running < MAX_CONCURRENT && queue.length) {
    const job = queue.shift()!;
    running++;
    void job().finally(() => {
      running--;
      pump();
    });
  }
}

/**
 * The data: URL of the logo of an asset with this hash, once the host gave it ('' until then, and when it has none): a
 * reactive read, which asks the host the first time.
 */
export function logoUrl(id: string, hash: string | undefined): string {
  if (!hash || !/^[0-9a-f]{64}$/.test(id)) return '';
  const key = `${id}:${hash}`;
  const url = shown[key];
  if (url !== undefined) return url;
  if (!asked.has(key)) {
    asked.set(
      key,
      new Promise<void>((resolve) => {
        queue.push(async () => {
          try {
            const got = await request<LogoImage | null>('host', 'bitassets.logo', { asset: id, hash });
            shown[key] = got && got.hash === hash && isLogoUrl(got.url) ? got.url : '';
          } catch {
            shown[key] = '';
          } finally {
            const keys = Object.keys(shown);
            if (keys.length > MAX_KEPT) for (const k of keys.slice(0, keys.length - MAX_KEPT)) {
              delete shown[k];
              asked.delete(k);
            }
            resolve();
          }
        });
        pump();
      }),
    );
  }
  return '';
}

/** Forget what was shown (a reset of the chain's state, the tests). */
export function resetLogos(): void {
  for (const k of Object.keys(shown)) delete shown[k];
  asked.clear();
  queue.length = 0;
}
