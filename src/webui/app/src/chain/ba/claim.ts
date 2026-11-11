// What a launch owes its creator, paid (claimlaunch: its fees, its allocation minted once graduated, the tokens of a
// split; anyone may send it, nothing goes anywhere but to the creator): asked first, then sent by the wallet. An asset's
// Manage ("Claim creator fees") and its Details (ba/AssetView.svelte, ba/LaunchFacts.svelte).
import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
import { t } from '../../lib/i18n.svelte';
import { toast, wallets } from '../../lib/state.svelte';
import { claimable, launchDecimals, type Launch } from './launch';
import { assetArg, formatAmount, type Amount } from './model';
import { nameOf, refreshBitAssets } from './store.svelte';
import { TICKER } from '../config';

export async function claimLaunch(launch: Launch): Promise<boolean> {
  const wallet = wallets.current;
  if (wallet === null) return false;
  const owed = claimable(launch);
  const r = await ask({
    title: t('Pay a launch\'s creator'),
    message: t('Send the creator of %1 what it is owed?', nameOf(launch.token, launch.ticker)),
    informative: t('Its fees (%1 %3)%2. Anyone may send this: nothing goes anywhere but to the creator.', formatAmount(launch.creator_fees_owed, 8), `${owed.allocation ? t(', and its allocation of %1% of the supply, minted now', String(launch.allocation)) : ''}${owed.tokens ? t(', and %1 tokens owed', formatAmount(launch.creator_tokens_owed ?? 0, launchDecimals(launch), { max: 2 })) : ''}`, TICKER),
    level: 'question',
    choices: [
      { id: 'no', label: 'Cancel' },
      { id: 'yes', label: 'Pay', kind: 'primary' },
    ],
  });
  if (r.choice !== 'yes') return false;
  try {
    const done = await walletRpc<{ txid: string; fees: Amount; allocation: Amount }>(wallet, 'claimlaunch', [assetArg(launch.token)]);
    if (!done) return false;
    toast('success', t('Paying the creator'), t('%1 %2, with the next block.', formatAmount(done.fees, 8), TICKER));
    void refreshBitAssets();
    return true;
  } catch (e) {
    await inform(t('Pay a launch\'s creator'), errorText(e), 'error');
    return false;
  }
}
