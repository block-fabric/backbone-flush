// The action links of flush.markets (doc/flush-links.md): the page only hands them over, as they are, to the host
// (src/webui/linkhost.h), which reads them, resolves every id, quotes again from its node and asks the user in its own
// dialog. Here: what the page does with the answer (an error said; a decline is not one).
import { BridgeError, ErrorCode } from './bridge';
import { errorText, inform } from './dialogs.svelte';
import { t } from './i18n.svelte';
import { host } from './rpc';

/** The most bytes of a launch file the page hands over (the host checks again). */
export const MAX_LAUNCH_FILE = 64 * 1024;

async function said(title: string, run: () => Promise<unknown>): Promise<void> {
  try {
    await run();
  } catch (e) {
    if (e instanceof BridgeError && e.code === ErrorCode.CANCELLED) return;
    await inform(t(title), errorText(e), 'error');
  }
}

/** An action link pasted or dropped into the app. */
export function openLink(uri: string): Promise<void> {
  return said('Link refused', () => host.openLink(uri));
}

/** A launch file dropped on the window: its text, to the host. */
export async function importLaunchText(file: File): Promise<void> {
  if (file.size > MAX_LAUNCH_FILE) {
    await inform(t('Launch file refused'), t('The file is too large for a launch file.'), 'error');
    return;
  }
  const text = await file.text();
  await said('Launch file refused', () => host.importLaunch(text));
}

/** File > Import launch: the host's file chooser, then the same. */
export function importLaunchFile(): Promise<void> {
  return said('Launch file refused', () => host.importLaunch());
}
