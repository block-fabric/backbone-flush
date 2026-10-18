# Layers

Layers is a sidechain of Chains (CHN) in slot 7: **assets on a fast chain**. It is
[FastBlocks](fastblocks.md) exactly (blocks of stake every 15 seconds, signed by
validators who stake pegged CHN, anchored by the mainchain through BMM, with BMM
blocks as the fallback) with every feature of [BitAssets](bitassets.md) (assets
registered under a name, minted, updated, burned and sent; pools that trade any two
assets, CHN included; Dutch auctions). The tree is the merge of the two: FastBlocks'
`main` with BitAssets' `main` merged in (both are the sidechain template with their
own rules), plus the identity of Layers and what makes the two work together,
below. Both stay mergeable: `git merge fastblocks/main` and `git merge
bitassets/main` bring their later work.

## Identity

| | Main | Testnet | Signet | Regtest |
|---|---|---|---|---|
| Slot | 7 | 7 | 7 | `-sidechainslot` |
| P2P port | 9255 | 19255 | 39255 | 29255 |
| RPC port | 9254 | 19254 | 39254 | 29254 |
| Magic bytes | `6c 79 72 01` ("lyr") | `6c 79 72 02` | `6c 79 72 03` | `6c 79 72 04` |
| Address prefix | `ly` | `tly` | `tly` | `rsc` (the template's, as the other sidechains') |
| First block | "Layers: assets on a fast chain" | "Layers testnet 18/Oct/2026: assets on a fast chain" (1792337400) | "Layers signet 18/Oct/2026: assets on a fast chain" | the template's |
| Hash of the first block | `164aeae68926b8814474e6f1580a6253df0c04d53826ea715400a8070dab0857` | `252a644146ba279b9b1a86dc957d42c4ff57b9ad4ca7be1218173afc92e9aff5` | `68c4854ae0f132e80760c9089066ffb4a7ef6ded09387e941f3430edbf45e101` | `0f9188f13cb7b2c71f2a335e3a4fc328bf5beb436012afca590b1a11466e2206` |
| Assets' reveal depth | 6 blocks | 3 | 3 | 2 |

Programs: `layersd`, `layers-cli`, `layers-app` (the wallet, in the `layers-app`
tree), `layers-tx`, `layers-util`, `layers-wallet`; configuration `layers.conf`,
data directory `~/.layers` (the Chains app runs it under its sidechains folder,
`~/.chains/sidechains/layers/data`). Base58 prefixes are the template's (28/88 on
main, 111/196 elsewhere).

The hashes of the first blocks are as `getblockhash 0` shows them (and `layers-util
getchainparams`, which printed them in the reverse byte order, upstream's `HexStr` of
the hash, until 2026-10-18: the test network's showed as `f5afe992…2a25`).
`feature_layers_genesis.py` checks that the daemon, the util, its test data and this
table agree on every network.

The test network's `main_activation_height` (and with it
`pos.main_activation_height`, which FastBlocks' leader seed and first record use) is
`TESTNET_MAIN_ACTIVATION_HEIGHT` in `src/kernel/testnet_activation.h`: 4997, the Chains
testnet block that activated slot 7 (`chains-cli -testnet getsidechain 7`'s
`activationheight`). The main network refuses to start until its height is named.

## What is FastBlocks'

Everything about blocks and consensus order, unchanged: blocks of stake, leaders
and epochs (15-second slots, 1.5 million weight units a block of which 400,000 for
transactions other than the coinbase), anchors (tickets, acknowledgements), the fork
choice, finality, staking (bonds, unbonds, jailing, slashing, evidence), the fee pot
and the fee split, the producer and the anchorer, `-validator*` options, every PoS RPC
(`getposinfo`, `getstakinginfo`, `getvalidators`, `getleaderschedule`,
`getanchorinfo`, `listevidence`, `submitevidence`, `setstaking`, `createbond`,
`createunbond`, `listbonds`, `signbondpop`, `checkpegbalance`), the parameters of
each network (doc/fastblocks.md, "Parameters"), and FastBlocks' tests
(`feature_fastblocks*.py`, `fastblocks_tests`, the fuzz target). FastBlocks is in
`src/pos` and plugs into the template through its hooks (`sidechain/hooks.h`).

## What is BitAssets'

Everything about assets, unchanged: the marker output, tokens, the asset registry
(reserve and register by commit and reveal, mint, update, transfer of control, fix
the supply, burn, retire), pools (`addliquidity` with `sets_price`,
`removeliquidity`, `swapasset` with `min_out`), Dutch auctions (`createauction`,
`bidauction`, `collectauction`), their rules (`src/bitassets/state.cpp`), the mempool
rules (one registration and one release of an asset at a time, tokens of unconfirmed
outputs refused, the payout policy, look-alike names refused as non-standard, the
per-tip recheck of asset transactions by the state keys their check read), paged
`listpools` and `listauctions`, exact amounts, the wallet (`src/wallet/rpc/bitassets.cpp`:
coins that carry tokens are never spent as CHN), every RPC, and BitAssets' tests
(`feature_bitassets*.py`, `bitassets_tests`, `sidechain_state_golden_tests`, the fuzz
target). BitAssets works in the template's state and validation code
(`sidechain/state.cpp`, `validation.cpp`, `node/miner.cpp`, `txmempool.cpp`), not
through the hooks.

## The combination

The two never touched the same code: FastBlocks lives in its hooks, BitAssets in the
template's state transition. Where they meet:

- **One state, one journal.** The assets' tables (store keys `0x30`–`0x3f`) and
  FastBlocks' (`0x50`, `0x53`–`0x5e`) are in the same sidechain store as the
  template's (letters: `w q r Q R b h x y`, and Layers' `a`, below); none overlaps.
  Every block's changes to all of them go through the store's journal, which is the
  undo data of the block: a block of stake or a BMM block taken back undoes its
  asset operations with its staking, down to FastBlocks' undo depth (5760 blocks).
- **Order in a transaction.** `State::ApplyTxSteps`: the template's withdrawals and
  refunds, then the assets (`bitassets::State::ApplyTx`), then FastBlocks' bonds and
  unbonds (`ChainHooks::ApplyTx`), all or nothing on the transaction's overlay. A
  transaction may carry a marker and a bond: each rule set reads its own outputs
  (a bond is an OP_RETURN output, which can carry no token).
- **Order in a block.** FastBlocks' peg actions first (`BeforePegStep`: the pot, the
  unbonds and rewards it owes), then the mainchain's, then the transactions, then
  the payouts, then FastBlocks' `AfterBlock`: the template's order, which both
  followed already.
- **Payouts of CHN.** What pools and auctions pay in CHN is paid by the coinbase, as
  on BitAssets, but only blocks that take the peg actions pay (BMM blocks and blocks
  of stake that acknowledge an anchor, about one a mainchain block): FastBlocks' rule
  for every payout. A swap or a bid is final at once for its asset leg; its CHN comes
  with the next anchor. The bound BitAssets puts on those payouts
  (`MAX_TX_PAYOUTS_PER_BLOCK`, 500) counts, in Layers, the assets' payouts still
  waiting from blocks that paid none with the block's own, so that what waits is
  never more than the next anchor is sure to pay (half of its 1000), whatever the
  number of blocks of stake between two anchors. What waits is counted for the
  assets alone, apart from what FastBlocks' own peg actions owe in the same queue
  (state key `a`: the positions of the assets' payouts in it, one range a block,
  undone by the journal; `sidechain::State::AssetPayoutsQueued`). A transaction past
  the bound is valid again in a later block: the miner and the producer leave it out,
  the mempool keeps it. This is a rule of Layers from its genesis (a new chain: no
  activation height).
- **Fees.** An asset transaction is an ordinary transaction for the fees: FastBlocks' fee split applies to it
  as to any other, from Layers' first block on every network (`fee_split_height` 0: a new chain, no height
  to set later; regtest keeps FastBlocks' option `-testactivationheight=feesplit@<height>`). FastBlocks' second fee curve (`fee_curve2_height`, the bidder's
  share 50 % down to 20 %, linear) holds from the first block on the main network and signet; Layers' test
  network, running since before it with the first curve, takes it from `TESTNET_FEE_CURVE2_HEIGHT`
  (`src/kernel/testnet_activation.h`, `INT_MAX` until the coordinator sets it with `TESTNET_DEX_HEIGHT`);
  regtest `-testactivationheight=feecurve2@<height>`. The BMM bidder
  gets the fees of peg transactions whole and a share of ordinary fees by the mainchain's pace; the rest goes
  to the validator pool, paid to every validator by stake each epoch (doc/fastblocks.md, "Fees, the pot and
  the validator pool").
- **The peg balance.** `getsidechainstate`'s `pegbalance` and `checkpegbalance` count, besides
  FastBlocks' parts (the validator pool among them), `assets`: the CHN the pools and the auctions not collected hold (put in by markers,
  out of the UTXO set). CHN freed by retiring an asset becomes a withdrawal (to
  mainchain miners), so is in `withdrawals`; CHN owed by pools and auctions and not
  paid yet is in `payouts`.
- **Mempool.** Both rechecks run after every change of the tip: BitAssets' by the
  state keys each asset transaction's check read, FastBlocks' by `TxRuleKeys`; a
  block taken back rechecks all of both.
- **Children of asset transactions.** The outputs that carry tokens (of no value by design) are no
  ephemeral dust (`CheckEphemeralSpends` uses `GetDust`, which leaves them out): a transaction that
  spends only the CHN change of an unconfirmed asset transaction, a plain send say, is taken by the
  mempool (it was refused, `missing-ephemeral-spends`, and the wallet kept it out of the mempool).
- **Heights.** BitAssets counts in blocks (an auction's start, its duration, the reveal
  depth of a registration): on Layers a block is 15 seconds, so an auction of 240
  blocks runs an hour, and a registration is revealed 45 seconds after its
  reservation on the test network.

## The DEX

Rules of the assets from `dex_height` (`Consensus::SidechainParams::dex_height`,
`src/bitassets/dex.h`): the main network and signet from their first block (0); the test network
from `TESTNET_DEX_HEIGHT` in `src/kernel/testnet_activation.h` (`INT_MAX`, never, until it is set
a little ahead of the tip when the test network's nodes upgrade: it runs since 2026-10-18 under the
rules before it); regtest never, unless `-testactivationheight=dex@<height>`. Before the height
every rule is the one above, exactly (a marker of the new kinds is `bad-ba-marker`, as it always
was). `getdexinfo` shows the height and whether the next block has the rules. The height goes into
the drivechain database's fingerprint once it is set (a database of a chain connected without the
rules is made again); while it is never, nothing changes. Store keys `0x80`–`0x89` (below), none of
any other table's.

**Orders and routes.** A `swapasset` (Swap) and a `swaproute` (Route, marker kind 13: a path of 2
to 5 assets, all different, so 1 to 4 pools; `amount_in`, one `min_out` for the whole path,
`chn_to`) are *orders*. An order has exactly one result output; `chn_to` is required when either
end is CHN (CHN out, or CHN back, is paid there by the coinbase); an order paying CHN in pays at
least `MIN_CHN_PAYOUT` (what comes back is paid out). Its transaction takes what it pays in at once
(the marker burns the CHN), checks that every pool of the path exists and is open, takes one place
among the block's payouts if an end is CHN (counted against `MAX_TX_PAYOUTS_PER_BLOCK` whether used
or not), and waits for the end of the block. A block clears at most `MAX_BLOCK_ORDERS` (500)
orders; one beyond is `bad-ba-block-orders-full`, left for the next block by the miner. The result
output of an order cannot be spent in its own block (`bad-ba-result-pending`). `quoteroute from
amount to (max_hops) (count)` finds the best paths over the pools there are (bounded: each asset's
16 deepest pools, 20000 partial paths), each with what it gives alone; consensus only checks the
path it is given.

**Batch clearing.** After the block's last transaction and before its payouts
(`sidechain::State::FinishTxs`, in `ConnectBlock` and in block assembly alike, for blocks of stake
and BMM blocks), every order of the block clears together (`bitassets::State::ClearOrders`):

1. A *pass*: round k (k = 1..4) takes the k-th hop of every order still in, grouped by pool, the
   pools in the order of their ids. A pool's round has one price: with R0, R1 its reserves, A' and
   B' the round's inputs of asset0 and asset1 times 997/1000, the price of asset0 in asset1 is
   P = (R1 + B') / (R0 + A'); an order paying a of asset0 gets ⌊a·997/1000·P⌋ of asset1, one
   paying b of asset1 gets ⌊b·997/1000/P⌋ of asset0 (`dex::ClearPool`, in 256-bit integers). The
   pool's net trade is on its curve (the product of the reserves before the fees is exactly the
   one before), every order of the round has the same price, the pool keeps the 0.3 % of every
   order of every hop, and rounding is down, so the product of the reserves never falls; one order
   alone gets exactly what `amm::SwapOut` gave before the DEX. What a hop gives is the next hop's
   input. An order *misses* if a pool of its path is gone or closed (no providers) by then, if a hop
   gives nothing, or if what comes out is below its `min_out` (CHN: below `MIN_CHN_PAYOUT`).
2. If no order missed, the pass is the result. Otherwise the orders that missed are taken out and
   the rest cleared again from the reserves the block's transactions left, up to
   `MAX_CLEARING_PASSES` (8) passes. If it still does not settle, the block's orders execute one
   after another in block order, each alone against the pools as the ones before left them (the
   rules before the DEX), each that misses refunded.
3. An order that fills gets what came out on its result output (an asset) or by the coinbase to
   `chn_to` (CHN). One that misses gets what it paid in back, the same way: it is a valid no-op that
   paid its fee. So no `min_out` can make a block invalid. Unused payout places are dropped (the
   last first), so the coinbase pays no output of nothing.

The mempool takes an order only if it would fill alone against the pools as they are
(`State::OrderFillsAlone`, policy `ba-swap-price`, also at every recheck of the tip): it keeps
what can fill; the miner takes orders as it finds them, and a block that has one that misses is
still valid. The per-tip recheck reads the keys the check read (the pools), as before; every asset
transaction is checked again when the next block's rules change (the DEX's height, either way).

**Price accumulators (TWAP).** As Uniswap v2: each pool has `price0_cumulative` and
`price1_cumulative`, the sums of its price (asset0 in asset1 and the reverse, UQ64.64:
reserve·2^64/reserve) times the seconds it held, modulo 2^256 (`dex::Accumulate`; readers take
differences modulo 2^256, `dex::CumulativeDelta`). At the end of every block that changes the pool
(a trade, liquidity in or out), the price the pool had at the block's start is added times the
seconds since the last update, and the time moves to the block's. The time is the block's `nTime`:
for a block of stake its slot's time, for a BMM block the header's (bounded by the median time
past and the 2-hour future limit, which is what a BMM miner can skew); it never goes back (a block
earlier than the last update adds nothing). Each such block writes an *observation* (its height,
time, the accumulators, and the reserves it left); a pool keeps its last `TWAP_OBSERVATIONS` (1024)
in a ring (place = count mod 1024): older ones are overwritten, so the pruning rule is "the last
1024 blocks that changed the pool". A pool made again under the same id (after it was retired)
starts afresh (the accumulator names the pool's creation). A pool that existed before the DEX
starts at its first change after it. `getpooltwap pool (from_height) (seconds)`: the average over
the window from the time of a block (or the last seconds) to the tip's time, from the last
observation at or before the start (extended with the reserves it left) to the last one; a window
that starts before the oldest observation kept is refused.

**Oracles.** An oracle *is* its key: `registeroracle name (pubkey) (proof)` (marker kind 14)
registers an x-only key, whose id is the tagged hash of the key (`dex::OracleId`), once; the name
is a label, which other oracles may share (so nobody can squat or front-run an oracle by its name:
one who copies a registration from the mempool with another key makes another oracle). The
registration carries the key's proof of possession, a BIP340 signature over `SHA256(ser("Layers
oracle registration") ‖ the chain's first block ‖ ser(name) ‖ key)` (`dex::RegistrationHash`;
`bad-ba-oracle-proof` otherwise): nobody registers a key that is not theirs, nor reuses a proof on
another chain or under another name. The marker burns `ORACLE_REGISTRATION_FEE` (0.1 CHN), which
goes to mainchain miners as the fee of a withdrawal (as the CHN freed by retiring an asset): a cost
against spam, in the peg balance's `withdrawals`. `publishoracle oracle event value (height)
(signature)` (kind 15) publishes an attestation: the event's id is the tagged hash of its
description; the oracle's key signs (BIP340) `SHA256(ser("Layers oracle attestation") ‖ oracle id ‖
event id ‖ value (int64 LE) ‖ height (int32 LE))`; valid in a block at `height` or later, once per
(oracle, event) (`bad-ba-attested`). The wallet signs with its key of the oracle, or takes outside
signatures (the proof and the attestations). RPCs name an oracle by its id or its key. `listoracles`,
`getoraclevalue oracle event`.

**Oracle contracts.** `createoraclecontract oracle event asset amount outcomes refund_address
expiry` (kind 16) locks an amount of an asset or CHN (CHN at least `MIN_CHN_PAYOUT`) with 1 to 8
outcomes (a range of values and a script each, in order) and a refund script, expiring at most
`MAX_CONTRACT_DURATION` blocks ahead. `settleoraclecontract contract` (kind 17), by anyone: if a
block up to the expiry has the oracle's attestation of the event, the first outcome whose range
holds the value gets all of it (none: the refund); with no attestation by then, the refund after
the expiry; otherwise it waits (`bad-ba-contract-pending`). CHN are paid by the coinbase; an asset
goes to the settlement's one result output, whose script must be the winner's
(`bad-ba-contract-payee`). The funding transaction may have inputs of several parties, each
signing the whole (outcomes included). **Trust model:** the oracle decides which named outcome
wins and can lie, or say nothing (then the refund); it cannot pay anyone the contract does not
name, nor take the collateral. An asset held by an open contract cannot be retired. The CHN that
open contracts hold are in the peg balance's `assets` (`HeldChn`). `getoraclecontract`.

**Store keys (from the DEX's height):** `0x80` the pools' accumulators, `0x81` their observations
(pool, place), `0x86` oracles, `0x87` attestations (oracle, event), `0x88` contracts, `0x89` open
contracts by the asset they hold; during a block only (erased at its end): `0x82` the reserves of
the pools the block changes as they were before it, `0x83` the orders, `0x84` their count, `0x85`
their result outputs. All of them through the store's journal: a block taken back takes its
clearing, its accumulators, its observations and its oracles' records back.

## The app

`layers-app` is `fastblocks-app` with every page of `bitassets-app`. Staking is one
section (user decision, 2026-10-18), with the tabs Overview (the leader of this slot,
the fee split, your stake and rewards, the wallet's income and its coins by
finality), Validator (this node as a validator), Bonds, and Network (the validator
set, the leader schedule, Finality & BMM, Slashing & jail); then the pages of the
assets: Assets, My assets, Register new asset, Trade, Auctions, and sending an asset
from Send. Alt+6 Staking, Alt+7 Assets, Alt+8 My assets, Alt+9 Trade.

## Tests

`ctest` (unit tests, among them `layers_tests`: the bound of the assets' payouts
across blocks of stake, and undone with a block), the functional tests of FastBlocks
(`feature_fastblocks*.py`), of BitAssets (`feature_bitassets*.py`), of the template
(`feature_sidechain*.py`), and `feature_layers.py`: assets made and traded in blocks
of stake and in BMM blocks, a reorganisation across both kinds that takes the assets'
state back, finality with assets, and the peg balance with CHN in pools and auctions.
`dex_tests` (the clearing's arithmetic: one order is the curve, one price, conservation, the
product never falls; the accumulators' wrap; the oracles' signatures; routes, batches, refunds,
the bound of orders, observations, oracles and contracts on a state) and
`feature_layers_dex.py` (before and after the DEX's height: routes of 2 and 4 hops, batches in a
BMM block and in a block of stake, a missed `min_out`, the TWAP over windows and across a
reorganisation, oracles with the wallet's key and an outside one, contracts to both outcomes and a
refund, the peg balance).
