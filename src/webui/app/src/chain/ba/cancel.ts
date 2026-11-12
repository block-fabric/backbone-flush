// A launch cancelled by its creator (cancellaunch; upgrade v10, owner decision 2026-11-11): only one nothing is bought
// from now (the node's getlaunch `cancellable`), only by the wallet that is its creator. Asked first, saying what comes
// back, then sent by the wallet. Two transactions a block apart: the rules know the creator by a reservation made at
// the creator's address, so the first call makes that proof (step "authorise") and the second, once it is in a block,
// cancels (step "cancel"). An asset's Manage (ba/AssetView.svelte, "Cancel launch").
import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
import { t } from '../../lib/i18n.svelte';
import { toast, wallets } from '../../lib/state.svelte';
import { launchDecimals, type Launch } from './launch';
import { assetArg, formatAmount, type Amount } from './model';
import { nameOf, refreshBitAssets } from './store.svelte';
import { TICKER } from '../config';

export interface CancelDone {
  txid: string;
  step: 'authorise' | 'cancel';
  returned?: Amount;
  control?: boolean;
  mintable?: boolean;
  fees?: Amount;
}

/**
 * What the confirmation says of the supply after the cancel, from what the page knows: fixed stays fixed unless the
 * wallet holds every other unit (the node decides at signing: its answer says which).
 */
export function cancelSupply(o: { fixed: boolean; holdsAllOther: boolean }): 'open' | 'unlocked' | 'fixed' {
  return !o.fixed ? 'open' : o.holdsAllOther ? 'unlocked' : 'fixed';
}

/** Asks, then sends one step. Returns the step done, or null (declined, or failed and said). */
export async function cancelLaunch(launch: Launch, o: { fixed: boolean; holdsAllOther: boolean; hasControl: boolean; units: number }): Promise<CancelDone | null> {
  const wallet = wallets.current;
  if (wallet === null) return null;
  const name = nameOf(launch.token, launch.ticker);
  const r = await ask({
    title: t('Cancel launch'),
    message: t('Cancel the launch of %1?', name),
    informative: [
      t('Nothing is bought from its curve now, so it can be taken back. What comes back to this wallet: the %1 %2 of the launch (the curve\'s and those set aside for its pool)%3.', formatAmount(o.units, launchDecimals(launch), { max: 2 }), launch.ticker, o.hasControl ? '' : t(', and a control coin of the asset')),
      cancelSupply(o) === 'open'
        ? t('Its supply is not fixed: the control coin mints more, as before.')
        : cancelSupply(o) === 'unlocked'
          ? t('Nobody else holds any of it (every other unit is in this wallet), so its supply is unlocked again: the control coin can mint more.')
          : t('Its supply stays fixed: other wallets hold units of it, so the control coin mints no more.'),
      t('The asset can then be put on a launch curve again, with other settings, or get a pool.'),
      t('It takes two transactions, a block apart: the first proves this wallet is the launch\'s creator, the second cancels. If the first is already in a block, this sends the second.'),
    ].join('\n\n'),
    level: 'warning',
    choices: [
      { id: 'no', label: 'Keep the launch' },
      { id: 'yes', label: 'Cancel the launch', kind: 'danger' },
    ],
  });
  if (r.choice !== 'yes') return null;
  try {
    const done = await walletRpc<CancelDone>(wallet, 'cancellaunch', [assetArg(launch.token)]);
    if (!done) return null;
    if (done.step === 'authorise') {
      toast('success', t('Step 1 of 2 sent'), t('The proof that this wallet created the launch of %1. Once it is in a block, press Cancel launch again to finish.', name));
    } else {
      toast('success', t('Launch cancelled'), `${t('%1 %2 come back with the next block.', formatAmount(done.returned ?? 0, launchDecimals(launch), { max: 2 }), launch.ticker)} ${done.mintable ? t('Its supply is unlocked again.') : t('Its supply stays fixed.')}${done.fees && Number(done.fees) > 0 ? ` ${t('%1 %2 of creator fees are paid.', formatAmount(done.fees, 8), TICKER)}` : ''}`);
    }
    void refreshBitAssets();
    return done;
  } catch (e) {
    await inform(t('Cancel launch'), errorText(e), 'error');
    return null;
  }
}
