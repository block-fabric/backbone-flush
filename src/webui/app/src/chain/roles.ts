// What an output of a coinbase of the wallet pays, by the role the host names (local::CoinbaseRoles in
// src/webui/local.cpp): the history shows its label instead of "Mined", its details the source, and the type
// filter has one entry per role. FastBlocks: the producer's share of a block of stake, the fee pot paid to a
// BMM bidder, a reporter's share of a penalty, a validator's share of the validator pool (doc/fastblocks.md, "Staking income").
export interface CoinbaseRole {
  /** In the Type column and the type filter. */
  label: string;
  /** The source the details of a transaction give. */
  source: string;
}

export const COINBASE_ROLES: Record<string, CoinbaseRole> = {
  staked: { label: 'Staked', source: "Staked: the producer's share of a block of stake" },
  bmm: { label: 'BMM reward', source: 'BMM reward: the fee pot, paid to the BMM bidder' },
  slashing: { label: 'Slashing reward', source: "Slashing reward: the reporter's share of a penalty" },
  validator: { label: 'Validator share', source: "Validator share: this validator's share by stake of the validator pool of an epoch (the fee split)" },
  // Past a coinbase's first output, what else the chain pays the wallet (wallet/payouts.h): none of it mined.
  payout: { label: 'Order payout', source: 'Order payout: the coin an order or a launch of this wallet was paid by the chain (a sale, a refund, a pool, a position, a creator\'s fees), in a coinbase' },
  refund: { label: 'Withdrawal refund', source: 'Withdrawal refund: a withdrawal to the mainchain given back, in a coinbase' },
  unbond: { label: 'Unbonded', source: 'Unbonded: what a bond of this wallet unbonded, paid back to its owner in a coinbase' },
  deposit: { label: 'Deposit', source: 'Deposit: coins deposited from the mainchain, paid in a coinbase' },
  chain: { label: 'Chain payout', source: 'Chain payout: coins the chain paid this wallet in a coinbase (a deposit from the mainchain, or an order of another wallet)' },
};

/** What a send of the wallet to no address is, by the role the host names (SendRole in src/webui/walletapi.cpp). */
export const SEND_ROLES: Record<string, { label: string; to: string }> = {
  bond: { label: 'Bond', to: "a validator's bond" },
  unbond: { label: 'Unbond', to: "a validator's bond (paid back later)" },
  withdrawal: { label: 'Withdrawal', to: 'the mainchain' },
  refund_request: { label: 'Withdrawal refund request', to: 'a withdrawal of this wallet' },
  marker: { label: 'Asset operation', to: 'an asset operation' },
};
