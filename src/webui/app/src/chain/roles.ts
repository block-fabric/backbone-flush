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
};
