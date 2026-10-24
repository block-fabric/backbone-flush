# TEMPORARY: Flush on eCash betanet slot 99 (Photon's)

Betanet's slot 99 was voted in for **Photon** ("Sidechain using post-quantum cryptography", activated at mainchain
height 968,998, escrow script `b7016351`). Nobody deposited to it or proposed a bundle in it. Flush's own slot, 46,
is not active on betanet. Until it is, this branch (`photon-slot`) runs Flush in slot 99. Drop the whole variant
once Flush has slot 46 (search for "TEMPORARY" and `IsBetanetSlot99`).

## How to select it

```
flushd -chain=ecashbeta -sidechainslot=99      # or flush-app; or chain=ecashbeta + sidechainslot=99 in flush.conf
```

`-sidechainslot` was a regtest-only option. On the eCash networks it now accepts only `ecashbeta` with `99`.
Any other value, or `ecash`/`ecashtest`, is refused at startup. It is a startup option on an existing network,
not a new network name, so the RPC `chain`, the app's network table and the address prefixes stay betanet's.
A new `ChainType` would have touched every switch on networks, the app's `chain.json`, the links and the public
API, and all of that would have to be removed again later.

What the variant changes (`CEcashSidechainParams`, `CreateBaseChainParams`):

| | slot 46 betanet | slot 99 (this variant) |
|---|---|---|
| First block | "Flush on eCash betanet 21/Oct/2026" | "TEMPORARY Flush on eCash betanet slot 99 (Photon) 22/Oct/2026", time 1792627200 |
| Message start | `flx` 0x22 | `flx` 0x99 |
| P2P / RPC ports | 38255 / 38254 | 38995 / 38994 |
| Data folder | `betanet/` | `betanet-photon/` |
| `main_activation_height` | 0 (not active) | 968998: the node stops if another sidechain takes slot 99 |
| Addresses | `bflx` | `bflx` (the same, so the app's address checks keep working) |

Because of its own first block and message start, it never mixes with a slot-46 Flush, nor with a Photon chain.
Every other rule comes from the same code: enforcer mode (`-mainchaingrpc`, `main_events_only`), the 6-hour gap
tolerance, the fee splits, the DEX, launches and bundles.

The node sets a warning (`node::Warning::TEMPORARY_NETWORK`): "Temporary: Flush on betanet slot 99 (Photon)...".
It shows in `getblockchaininfo`/`getnetworkinfo` "warnings", and in the app as an orange strip above every page
(`TemporaryNetwork.svelte`).

## Bootstrap on betanet

- **Validators**: the variant has three genesis validators, keys made each on its own machine (the lab node server,
  5.78.75.193, the UI PC). It also has `min_active_validators` 3 and `min_stake` 0.1 ECX, where slot-46 betanet has
  no genesis validators, 4 and 1 ECX. So blocks of stake run from the first block, and anyone can join by bonding
  0.1 ECX or more (the bond counts about ten hours later, the set lead). If fewer than 3 validators are active, the
  chain falls back to BMM blocks, as any Flush network does.
- **BMM / anchors**: the lab validator bids `-validatoranchorbid=0.00001` per betanet block through the wallet
  "flush-bmm" of the lab's backboned (WalletService of its gRPC). The others bid 0. Only a bid that gets mined is
  paid. Budget: about 0.01-0.02 ECX a week, with the M8 fees.
- **First deposit**: an M5 into slot 99 creates the Ctip (Photon's slot had none). Flush credits the payload as
  the wrapped `s99_<address>_<check>` and as the bare address alike (`sidechain::DepositScript`).

The deployment files are in `contrib/photon-slot/`: the validator unit, the SSH tunnel unit for a machine without
a betanet node, and `make-conf.sh`.

## Upgrade v2

Branch `photon-v2` (photon-slot, the unified assets of `assets-unified`, the anchor bids of `anchor-bid`, and the
liveness fix of `jail-fix`): from block `SLOT99_V2_HEIGHT` (src/kernel/chainparams.cpp, in the slot 99 branch of
the betanet params) both:

- `launch_params_height`: launches with parameters (supply, decimals, the curve's fee) and an existing asset put
  on a curve;
- `jail_fix_height`: the first block of stake charges no slot before it (at the chain's start it charged every
  genesis validator an epoch of misses, and all three were jailed), and a peg step gives no liveness jails that
  would leave fewer than `min_active_validators`.

The height is in the sidechain parameters' fingerprint: on its first start, a node of v2 rebuilds its sidechain
state from its blocks once. Every node is to run v2 before that block; one that does not forks off at it.

## Upgrade v3

Branch `photon-v3` (photon-v2 and allocations of up to 10 %): from block `SLOT99_V3_HEIGHT`
(src/kernel/chainparams.cpp) `allocation10_height`: a launch's creator allocation may be up to 10 % of its supply
(`bitassets::MAX_LAUNCH_ALLOCATION`; 5 % before it, `MAX_LAUNCH_ALLOCATION_BEFORE_10`), minted on top of the supply
at graduation and claimed by the creator (`claimlaunch`), with no vesting yet (rug protection to come). The height is
in the fingerprint: a node of v3 rebuilds its sidechain state once on its first start. A node of v2 follows the chain
until the first launch with more than 5 % at or after the height, and forks off at that block.

## Upgrade v4

Branch `photon-v4` (photon-v3 and smaller payouts): from block `SLOT99_V4_HEIGHT` (src/kernel/chainparams.cpp)
`small_payout_height`: the least ECX a pool, a book, a curve or an auction pays out is 0.00005 ECX (5 000 sat,
`bitassets::MIN_CHN_PAYOUT_SMALL`; 0.0005 before), and the least order of a pair with ECX is worth that much on its ECX
side (0.01 before): an order of about $0.50 at $10 000 an ECX. The payout cap per block is unchanged (500). The height is
in the fingerprint: a node of v4 rebuilds its sidechain state once on its first start.

From the same block, `no_treasury_height`: there is no treasury. Swap fees go 89 % to the liquidity providers and 11 % to
the chain's security (the 4 % protocol part stays in the pool; a fee's security part in an asset stays in its pool too),
launch curve fees 80 % to the creator and 20 % to security. At that block what the treasury held of ECX (0.00007559 ECX
on 2026-10-24) is paid with its fees as their security part; its entries stay as a record, and what it held of assets
(none from swaps of assets so far, or what accrued) stays held, unspendable. A node of v3 forks off at that very block
(its coinbase pays the treasury's ECX: bad-cb-amount on v3), or at the first order or payout under the old minimums.

## The halt of 2026-10-24 (the long-gap rule)

eCash betanet made no block after 973972 (14:55:43 UTC); its blocks had been sparse all day (about one an hour), so
that block's median time was 09:35:20, 5 h 20 min behind it. Flush's blocks of stake stopped after block 8964 (slot
16079, 18:59:45), the last slot of epoch 66: epoch 67 starts at 19:00, its seed time (19:00 less the 9-hour lookahead) is
10:00, and no block on record has a median time past 10:00, so its seed, and so its leaders, were unknown
(`pos-seed-unknown`; `getposinfo` showed no leader). The set and the unanchored limit (8 hours from slot 14874, 13:58:30)
were not the cause. `stalled` stayed false: it compares the mainchain's median time with the last block of stake, and
without an eCash block there is no BMM block to carry transactions anyway.

Branch `photon-v4-gap` (photon-v4-ui and the long-gap rule, doc/ecash-sidechains.md "A gap of the mainchain"): from epoch
`SLOT99_GAP_EPOCH` = 67, the seed lookahead and the set lead are 48 hours and the unanchored limit 24 hours (5760 slots).
Epochs 0 to 66 keep their seeds and sets, so no block of before changes and nothing is in the fingerprint. With it, epoch
67 and every epoch after it whose start is before 09:35:20 + 48 h has its seed, and its set is the last anchor's before
its start less 48 hours (the genesis validators, as at the halt). The chain resumes with the first slot a validator of
the new release leads, without an eCash block, and goes on until 13:58:30 + 24 h (2026-10-25 13:58:30 UTC) if eCash stays
silent; eCash's next block anchors the run (its commitment, to a ticket made before the gap, is acknowledged), and the
run is final 3 anchors later. A node of before does not follow: for it epoch 67 has no seed (the blocks wait as
`pos-seed-unknown`, not invalid), so every node of slot 99 takes the release; one left behind catches up once it does.
Upgrade v4 (block 9000) is independent of it: it counts blocks, not epochs.
