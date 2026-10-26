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

Changelog: until block 9000 a protocol part of the fees was held back by the chain; at that block it was paid with the
block's fees as security, and from the patch after v5 nothing of it is exposed (no RPC, API or page). A node of v3 forks
off at block 9000, or at the first order or payout under the old minimums.

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

## Upgrade v5 (the fee-flow release)

Branch `photon-v5` (photon-v4-gap and the fee-flow release): from block `SLOT99_V5_HEIGHT` = 9100
(`fee_flow_height`; in the fingerprint: a node of v5 rebuilds its sidechain state once):

- **Fees on the ECX side.** Every pool paired with ECX (constant product, stable, weighted, concentrated) takes its
  whole fee in ECX (`State::ClearCoinSide`): a buy's fee out of the ECX it pays in, a sale's out of the ECX it gets (its
  units go whole through the curve). The batch round is the pool's own at no fee on the buys' ECX less their fee and the
  sales' units whole, all at one price; then each sale pays the fee, rounded up, out of what it gets; `min_out` counts
  the net. Of the fees, 15 % to the block's security, 85 % back into the pool's ECX (a concentrated pool: its in-range
  positions' fee growth in ECX). Quotes use the same. Pools of two assets keep v4's rule. Launch curves already did so.
- **Graduation.** A launch made from H graduates into a locked full-range position of a concentrated pool of ECX at 1 %
  (`GraduateLocked`): the same reserves as before, no position token, so nobody can withdraw. Its fees (in ECX) are
  collected each block it trades: 60 % to the creator (`GRADUATED_CREATOR_SHARE`, by its fee destination, claimlaunch),
  40 % to that block's security. Launches made before H graduate as before (their terms were fixed when made).
- **Fee destination** (launchtoken `fee_destination`, fixed at creation): `creator` (default), `burn` (the creator's
  ECX buys the token back in its own market each block, by a protocol order, and the units bought are burned: the
  supply falls, `burned` counts them; what an order misses waits for the next block), or `split` (`burn_percent`).
  The 20 % security part of a curve's fees is unchanged.
- **Keep control** (launchtoken `keep_control`, off by default): the creator keeps the control coin and can mint;
  every page marks the launch "Mintable: the creator can create more" (the wizard asks to confirm). The curve still
  buys back only what it sold.
- No vesting (the allocation is the creator's at graduation, as before) and no anchor reserve (user decisions).

A node of v4 forks off at the first block after H with a trade in a pool of ECX, a graduation, or a launch with the new
options. The anchorer's "Fees + prevent stalling" bids max(564 sat, the fee share), and under stall pressure a flat boost:
max(4 × 564, the fee share), capped by the maximum (not consensus; `flatboost`, `boost` in setanchorbidpolicy).

## Upgrade v6 (graduation at 100 ECX)

Branch `photon-v6`, from block `SLOT99_V6_HEIGHT` (`graduation100_height`; in the fingerprint: a node of v6 rebuilds its
sidechain state once): a launch made from it graduates at 100 ECX instead of 500, its curve scaled with it (virtual coin
50 ECX: the whole supply starts at about 44 ECX and graduates at about 400). A launch made before keeps 500: its record
says what it graduates at (`getlaunch` `graduation`, the API's `graduation_ecx`), and the pages read it per launch. A node
of v5 forks off at the first block with a launch made at or after the height.

## Upgrade v7 (instant DEX payouts, the launch fee split)

Branch `photon-v7`, from block `SLOT99_V7_HEIGHT` (`v7_height`; in the fingerprint: a node of v7 rebuilds its sidechain
state once).

- **Instant DEX payouts.** Before v7 the ECX the assets pay out (swaps, curve sales, pools, auctions) waited in the
  coinbase's queue for a block with the peg actions (an anchor's acknowledgement). From v7 the block pays them itself,
  every block (`sidechain::State::PayBlock`): they are in the coinbase of the block with the trade. The bound is
  re-derived for it: a block's transactions (and its DEX's end, the split's addresses included) give rise to at most
  `MAX_TX_PAYOUTS_PER_BLOCK` (500) such payouts with what still waits (`AssetPayoutsQueued`), and all of them are paid in
  the block; a block with the peg actions pays at most `MAX_PAYOUTS_PER_BLOCK` (1000) in all, the mainchain's queue
  (deposits, refunds) keeping its half, the block's own payouts out of the other half with the queue of the peg actions'
  owed (unbonds, rewards), which keeps the anchor rule; a block without them pays the 500 at most. (Any payout beyond
  the bound would wait in the queue as before; none can be, by the transactions' bound.) The coinbase's sigops are
  reserved for 1000 as before. **Finality**: such a payout is as soft as the trade until an anchor covers its block; a
  reorganisation before then takes both back. Real peg actions (deposits, withdrawals, unbond refunds, validator
  rewards, the pot) keep FastBlocks' anchor rule.
- **The launch fee split.** A launch made from v7 has a curve fee 25 % security (`launch_security_share_v7`), 75 %
  tunable, split as its launch transaction says (`FeeShare`, marker `LAUNCH_V7_KIND`): the creator, up to 3 addresses,
  a buyback and burn, in basis points of the 75 % summing to 10000 (default: all to the creator). The same split takes
  the creator's 60 % of the locked graduated position's fees. Each share gets the floor of its part, the roundings'
  remainder to the creator (else the burn, else the first): no satoshi lost or made (`SplitCoin`). The creator's share
  is claimed (`claimlaunch`, as before: simplest and safe, it was already the creator's flow); the burn's as v5's;
  an address's is paid by the coinbase once 0.001 ECX is owed (`MIN_FEE_SHARE_PAYOUT`), within the block's room for the
  assets' payouts (what does not fit waits for the next block). v5's destinations map onto a split
  (`SplitOfDestination`). A launch made before v7 keeps its terms.
- **Interfaces**: `launchtoken` `fee_split`; `getlaunch`/API `fee_split` (with `paid`/`owed`); `getlaunchstats`
  `rules.fee_split_active`, `instant_payouts_active`, `v7_height`; the API's `BuildActionRequest.split` and the link's
  `split=`; the app's confirmation shows every recipient in full (doc/dex-guide.md "Launchpads").
- A node of v6 forks off at the first block of stake without the peg actions that pays an asset's ECX, or at the first
  launch with a split (`bad-ba-marker` for it).
