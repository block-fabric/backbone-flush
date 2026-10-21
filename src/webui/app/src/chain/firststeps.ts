// The three first steps of Layers, shown on Home in Simple mode until dismissed: coins in, then what the chain is
// for, tokens traded fast (staking is a quick action of the home, Stake and earn)
// (components/FirstRunTips.svelte; lib/firststeps.ts). English here: the card translates them.
import type { FirstStep } from '../lib/firststeps';
import { CHAIN } from './config';
import './ba/terms';

const main = CHAIN.mainchain?.name ?? 'the mainchain';

export const FIRST_STEPS: readonly FirstStep[] = [
  { title: 'Bring coins here', text: `Coins come from ${main}. Drivechains gives a deposit address: paste it in the ${main} app (Sidechains, Move coins). They arrive here a block or two later.`, icon: 'down', action: { label: 'Get a deposit address', page: 'peg', tab: 'deposit' } },
  { title: 'Swap ECX for a token', text: 'A block every 15 seconds: a swap pays ECX and gets you a token at the price of its market, quoted before you confirm, and is confirmed almost at once.', icon: 'swap', action: { label: 'Swap', page: 'trade', tab: 'swap' } },
  { title: 'Create a token of your own', text: 'Points, tickets, shares: give it a name and a supply, and it is yours to send, sell or add to.', icon: 'issue', action: { label: 'Create a token', page: 'issue' } },
];
