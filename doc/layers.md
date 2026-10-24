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
| First block | "Layers: assets on a fast chain" | "Layers testnet 19/Oct/2026: every rule from the first block" (1792439100) | "Layers signet 18/Oct/2026: assets on a fast chain" | the template's |
| Hash of the first block | `164aeae68926b8814474e6f1580a6253df0c04d53826ea715400a8070dab0857` | `6ad3a3ff068fecfba95fc4aeec8404ac1db31cb840b852164db054ff31daa382` | `68c4854ae0f132e80760c9089066ffb4a7ef6ded09387e941f3430edbf45e101` | `0f9188f13cb7b2c71f2a335e3a4fc328bf5beb436012afca590b1a11466e2206` |
| Assets' reveal depth | 6 blocks | 3 | 3 | 2 |

Programs: `layersd`, `layers-cli`, `layers-app` (the wallet, in the `layers-app`
tree), `layers-tx`, `layers-util`, `layers-wallet`; configuration `layers.conf`,
data directory `~/.layers` (the Chains app runs it under its sidechains folder,
`~/.chains/sidechains/layers/data`). Base58 prefixes are the template's (28/88 on
main, 111/196 elsewhere).

The hashes of the first blocks are as `getblockhash 0` shows them (and `layers-util
getchainparams`, which printed them in the reverse byte order, upstream's `HexStr` of
the hash, until 2026-10-18: the test network's first one, 252a6441…aff5, showed as
`f5afe992…2a25`). The test network restarted from a new genesis on 19/Oct/2026 (every rule
from its first block; the first ran from 18/Oct with the DEX and the second fee curve from
1374, logos from 4373).
`feature_layers_genesis.py` checks that the daemon, the util, its test data and this
table agree on every network.

The test network's `main_activation_height` (and with it
`pos.main_activation_height`, which FastBlocks' leader seed and first record use) is
`TESTNET_MAIN_ACTIVATION_HEIGHT` in `src/kernel/testnet_activation.h`: 4997, the Chains
testnet block that activated slot 7 (`chains-cli -testnet getsidechain 7`'s
`activationheight`; the restart of 19/Oct/2026 keeps the slot). It is the only height of the
test network's own: every rule of Layers holds from the first block there, as on the main
network and signet. The main network refuses to start until its height is named.

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
  share 50 % down to 20 %, linear) holds from the first block on every network (the test network since its
  restart of 19/Oct/2026); regtest `-testactivationheight=feecurve2@<height>`. The BMM bidder
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
`src/bitassets/dex.h`): every network from its first block (0; the test network since its
restart of 19/Oct/2026); regtest never, unless `-testactivationheight=dex@<height>`. Before the height
every rule is the one above, exactly (a marker of the new kinds is `bad-ba-marker`, as it always
was). `getdexinfo` shows the height and whether the next block has the rules. The height goes into
the drivechain database's fingerprint once it is set (a database of a chain connected without the
rules is made again); while it is never, nothing changes. Store keys `0x80`–`0x89` (below), none of
any other table's.

**Orders and routes.** A `swapasset` (Swap) and a `swaproute` (Route, marker kind 13: a path of 2
to 5 assets, all different, so 1 to 4 pools; `amount_in`, one `min_out` for the whole path,
`chn_to`) are *orders*. An order has exactly one result output; `chn_to` is required when either
end is CHN (CHN out, or CHN back, is paid there by the coinbase); an order paying CHN in pays at
least `MinChnPayout` (what comes back is paid out). Its transaction takes what it pays in at once
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
   gives nothing, or if what comes out is below its `min_out` (CHN: below `MinChnPayout`).
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
expiry` (kind 16) locks an amount of an asset or CHN (CHN at least `MinChnPayout`: 0.0005 before `small_payout_height`, 0.00005 from it) with 1 to 8
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

## Pool kinds

Rules of the DEX from `dex_curves_height` (`Consensus::SidechainParams::dex_curves_height`, with the DEX's
rules only; `src/bitassets/curves.{h,cpp}`): main network and signet from the first block (0); the running
test network not until a deployment names `TESTNET_DEX_CURVES_HEIGHT` (`src/kernel/testnet_activation.h`;
never until then, so a node of this release follows the test network exactly as one of before; the next
restart of the test network from a new genesis takes them from its first block and the height goes); regtest
from the DEX's height, or `-testactivationheight=dexcurves@<height>`. Before the height a marker of their
kinds (18 to 21) is `bad-ba-marker`, as it always was. The height goes into the drivechain database's
fingerprint once set. `getdexinfo` shows `curves_height` and `curves_active`. Nothing that exists changes:
the pools AddLiquidity makes keep their id (`PoolId`), record, fee and arithmetic (the clearing calls
`ClearPool` for them, bit for bit), and every transaction of before means what it meant.

**Kinds.** A pool is made of a *curve*, a *fee tier* and the curve's parameter:

| | Curve | Parameter | Use |
|---|---|---|---|
| Constant product | x·y = k | none | any pair (the kind of every pool AddLiquidity makes, at 0.30 %) |
| StableSwap (Curve) | 4A(X+Y) + D = 4AD + D³/(4XY) | amplification A, 1 to 10000 | assets meant to trade near one for one |
| Weighted (Balancer) | x^wa · y^wb = k | weight of an asset: 2, 5, 10, 20, 25, 40, 60, 75, 80, 90, 95 or 98 % | a pool that holds mostly one asset (80/20: less loss to price moves of the 80) |

Fee tiers: 0.01 %, 0.05 %, 0.30 %, 1.00 % (`FEE_TIERS`, in millionths: 100, 500, 3000, 10000). Most of a fee stays
in the pool, for its liquidity providers; the chain's security and the protocol's parts leave it ("Fees of the DEX").
A 50/50 weighted pool is the constant product: it is made as one.

A pair has a pool of each kind. Its id is `PoolIdOf(a, b, kind)`: `PoolId(a, b)` for constant product at
0.30 %, otherwise `SHA256(ser("Layers pool") ‖ asset0 ‖ asset1 ‖ curve ‖ fee ‖ amp ‖ weight of asset0 ‖ flags)`
(the weight is asset0's whichever order the assets are named in). The kind of a pool other than the default is
kept in table `0x8a` (`PoolParams`; a pool with none is the default kind). A stable pool counts the units of
its two assets alike after their decimals: X = x·10^scale0, Y = y·10^scale1, the asset with fewer decimals
scaled up to the other (CHN has 8); the scales follow from the assets' decimals, which never change.

**Exact arithmetic.** Every number the rules take from a curve is the floor or ceiling of an exact quantity,
compared exactly (`curves::BigUint`, integers of any size; no fixed point, no rounding but at the end, toward
the pool):

- Constant product and weighted: a point (x, y) *holds the level* of a pool at (x0, y0) if
  x^wa · y^wb ≥ x0^wa · y0^wb, wa:wb the weights in lowest terms (80/20 is 4:1, 98/2 49:1, constant
  product 1:1). With rational points, multiplied out: a^wa · b^wb · p0 ≥ k · p^wa · q^wb.
- Stable: Φ(X, Y, D) = 16A·XY(X+Y) + 4XY·D − 16A·D·XY − D³ falls as D grows (∂Φ/∂D = 4XY(1−4A) − 3D² < 0),
  so the pool's D is its one root. A pool's level is Dc, the ceiling of its D (the smallest integer with
  Φ(X0, Y0, Dc) ≤ 0); a point holds it if Φ(X, Y, Dc) ≥ 0. Using the ceiling, the pool never takes less than
  the exact curve gives it (by under one unit of D).

A quantity is found by bisection over the integers (`SearchMax`, `SearchMin`), each step one exact
comparison: the predicates are monotone, so the result is the unique extreme integer, whatever the
iteration. Any implementation that computes the same floors agrees bit for bit; the vectors of
`src/test/data/dex_curves.json` (138 cases over every kind, clearing, opening and deposit shares, withdrawals,
prices) are checked by the node's unit tests and by an independent reference in Python
(`contrib/dex-curves/reference.py src/test/data/dex_curves.json`, written from these definitions).

**Batch clearing of every kind.** A pool's round (a hop of every order through it in a round of a pass, as
above) has one uniform price. With A' and B' the round's inputs of asset0 and asset1 less the fee (exactly:
Â = Σ a·(F − fee), B̂ = Σ b·(F − fee), F = 10^6):

- Constant product (any tier): ClearPool's formula, P = (R1·F + B̂)/(R0·F + Â) (at 0.30 % it *is* ClearPool).
- Stable and weighted: if asset0 comes in on balance, the pool pays out dy of asset1 net and every order gets
  P = (dy + B')/A' of asset0 in asset1: one paying a of asset0 gets ⌊a·(F−fee)·(F·dy + B̂)/(Â·F)⌋ of asset1, one
  paying b of asset1 ⌊b·(F−fee)·Â/((F·dy + B̂)·F)⌋ of asset0. The asset1 sellers then take at most
  B'·A'/(dy + B') of asset0, so the pool gains at least g(dy) = A'·dy/(dy + B') of asset0 net for at most dy of
  asset1. dy is the largest in [0, y0 − 1] for which the pool's worst point (x0 + g(dy), y0 − dy) holds its
  level. As dy grows that point moves along a hyperbola centred on (x0 + A', y0 + B'), concave in x, which
  meets the convex level curve once: the predicate is monotone and the bisection exact. If no dy > 0 holds,
  asset1 comes in on balance (the same, sides swapped); if neither, the two sides cross at P = B'/A' and the
  pool trades nothing. One order alone (B' = 0) gets dy itself: the most the curve gives for its input.
- **Conservation and the pool's value.** Each order's output is a floor of its share of the price, so the
  outputs of a side are at most its total at P: what the pool pays is at most dy of asset1 (plus the B' that
  came in) and what it keeps of asset0 is at least g(dy) more than before; the fees (the inputs less A', B')
  stay in it. The reserves after are the reserves before plus all that went in less all that came out
  (exactly; tested), and they hold the level of before (the invariant never falls: the curve's point, the
  rounding and the fees all raise it; property-tested over every kind, the extremes included).
- Orders that miss, passes, the sequential fallback, payouts: as for every pool (above).

**Routes across kinds.** `SwapPools` (kind 21) is an order through named pools of any kind, 1 to 4 (each
holding what the hop before gave, no asset twice); `Route` and `Swap` name assets and so the default pool of
each two, as before. Every hop clears in its pool's round at that pool's price. `quoteroute` and
`getswapquote` search over all pools of every kind (the 16 deepest of each asset) and say each route's pools
and whether the default pools alone make it (`default_pools`: then `swaproute`'s path alone names it).

**Liquidity.** `CreatePool` (kind 18) makes a pool of a kind (refused while one of that kind of the pair has
providers: `bad-ba-pool-exists`; one nobody provides for any more is opened again). Its shares are the floor of
the invariant's first degree at the reserves after: isqrt(x·y) for constant product (as AddLiquidity), the
floor of the weighted geometric mean (the largest v with v^(wa+wb) ≤ x^wa·y^wb), D for a stable pool (in units
of the asset with more decimals); MIN_LIQUIDITY kept for good, MIN_OPEN_SHARES and MIN_OPEN_CHN as for any
pool. `PoolDeposit` (19) puts any amounts into a pool by id, one side alone included: of the side out of the
pool's proportions, the excess e pays the fee on φ·e (φ the part a swap to the proportions would trade: the
other asset's weight, one half for constant product and stable; rounded up), and the deposit gets the most s
shares for which the pool after holds its level times (S + s)/S (S the shares before). At the pool's
proportions this is what AddLiquidity gives. `PoolWithdraw` (20): both sides, what the shares are worth
(rounded down), or one side alone: its part plus what the other side's part buys of it in the pool after
(the pool's fee paid). The single-sided parts trade at once, like a deposit, not in the batch.

**Prices and TWAP.** A pool's price of asset0 in asset1 is its curve's slope at its point: the reserves'
ratio y/x for constant product (as the accumulators always added), y·wa/(x·wb) weighted, and stable
(s0·Y·(4A(2X+Y) + Dc − 4A·Dc)) / (s1·X·(4A(X+2Y) + Dc − 4A·Dc)); the price of asset1 in asset0 is the floor
of the inverse quotient. The accumulators add these (UQ64.64) as before; `getpooltwap` extends an
observation with the price of the pool's kind.

**Bounds.** Reserves and amounts at most MAX_AMOUNT (2^63 − 1); every product is computed exactly, whatever its
size (a weighted 98/2 pool compares numbers of about 6,000 bits). The work of searched curves is bounded per
block: `MAX_BLOCK_CURVE_WORK` (200): each hop of an order through a stable or weighted pool, and each
CreatePool, PoolDeposit and PoolWithdraw, counts one (constant product of any tier: none); a transaction beyond
is not valid in that block (`bad-ba-block-curve-work-full`) and the miner leaves it for the next, so no valid
block holds more and no miner makes one. The clearing does not search a round again in a later pass (same
pool, reserves and orders). The slowest round measured (adversarial kinds, reserves and orders;
`dex_curves_tests/worst_case_round`) takes about 0.4 ms here, so a block's clearing stays under about
(MAX_CLEARING_PASSES + 1) · 200 · 0.4 ms ≈ 0.7 s; a typical round takes about 0.15 ms.

**RPCs.** Node: `getpool` (by assets, or by id), every pool with `curve`, `fee`, `fee_millionths`, `amp`,
`weight_a`/`weight_b`, `default_kind` (and `price` its curve's); `getswapquote` (every pool of a pair and the best
route, exact in or out), `simulateclearing pool orders` (a round of a batch now), `previewpool` (a pool before
it is made: id, shares, price, slippage), `getpoolcurve pool (points)` (depth), `decodeassettx` (what a
transaction's marker does, every kind, for explorers). Wallet: `createpool a amount_a b amount_b {curve, fee,
amp, weight_a}`, `addpoolliquidity pool {asset: amount}`, `removepoolliquidity pool (percent) (only)`,
`swappools asset_in amount [pools]`, and `swaproute ... (pools)`. The app's confirmations resolve every asset
(by id only) and pool they show.

**Store keys:** `0x8a` the kind of each pool made by CreatePool (none for the default kind); during a block only
`0x8b` the work of the pool kinds (`MAX_BLOCK_CURVE_WORK`).

## Concentrated liquidity

A pool kind of its own (`Curve::CONCENTRATED`, with the pool kinds: same height; markers 24 and 25;
`src/bitassets/clmath.{h,cpp}`, `State::ClearCl`, `State::PlanPosition`), after Uniswap v3.

**Arithmetic.** Prices are square roots in Q64.96 (√(asset1 per asset0, in units)·2^96); ticks are powers of
1.0001 (the price at tick t is 1.0001^t, −887272 ≤ t ≤ 887272); liquidity L is 128 bits. `SqrtRatioAtTick` is
Uniswap v3's `TickMath.getSqrtRatioAtTick` bit for bit (its twenty constants, each 2^128·1.0001^(−2^(k−1)) to within
one unit, are checked against an exact computation by `contrib/dex-curves/reference.py`); `TickAtSqrtRatio` is the
largest tick whose ratio is at most the price. The amounts between two prices for a liquidity, the price after an
amount, and a step of a swap are `SqrtPriceMath` and `SwapMath`'s, rounded as they round (in up, out down), with
every product exact. 80 vectors (`src/test/data/dex_cl.json`) are checked by the reference too.

**Pools and positions.** `createpool a x b y {"curve":"concentrated","fee":…}` makes a pool at the price y/x,
nothing in; its tick spacing follows its fee tier (0.01 %: 1, 0.05 %: 10, 0.30 %: 60, 1.00 %: 200). Liquidity comes
by *positions*: `OpenPosition` (24) puts a liquidity L between two ticks (multiples of the spacing), paying what L
needs at the pool's price (rounded up; at most the amounts the transaction offers, the rest back); a range above the
price holds asset0 alone, below it asset1 alone. A position is a token of its own (`Token::Kind::POSITION`, id
`PositionId(txid)`): it can be sent, and whoever spends it changes it. `ChangePosition` (25) adds liquidity, takes
some out (its amounts at the price, rounded down, paid at once), collects its fees, or closes it (all out, all
collected, the token gone). The pool keeps Uniswap v3's state: the price, its tick, the liquidity in range, the
fees per unit of liquidity ever (`ClState`, table `0x93`); each tick in use, its liquidity in all, what crossing it
changes, the fees on its other side (`ClTick`, `0x94` in order and `0x97` reversed, so the next one either way is
one ordered read); each position (`ClPosition`, `0x95`). The fees a position earned are its liquidity times the
growth of the fees inside its range since it last counted them (Uniswap's fee growth inside).

**Batch clearing.** A concentrated pool's round has one uniform price, as every pool's: with A', B' the inputs
less the fee, the pool's net trade is walked through its ticks (each tick crossed brings its liquidity in or out
and turns its fees over, as Uniswap's swap does; at most `MAX_CL_CROSSINGS`, 64, a round), and the net out is the
largest dy that the walk gives for the net in it gets, ⌊A'·dy/(dy + B')⌋ (bisection over that predicate, the
same procedure for every implementation: a walk's output is monotone in its input, up to one unit of rounding at a
tick, where the procedure decides). Every order then gets the price P = (dy + B')/A' (or the reverse), rounded down.
The walk is computed once a round, a stretch per tick, so each step of the search is one swap step. What the pool
kept beyond the walk (the fees, the roundings, the matched orders' differences) is credited to the liquidity in
range at the price after the round (or, if none, at the price before): its positions earn the round's fees; a
position out of range earns nothing. The reserves are what went in less what came out; every position can always be
closed (property-tested: positions in and out of range, twenty blocks of trades and batches crossing ticks, then
all closed; the pool never owes more than it holds). A hop through a concentrated pool counts two units of the
block's work (`MAX_BLOCK_CURVE_WORK`). Routes and quotes go through them as through every pool; their prices feed
the TWAP (the square root price, squared, as it was before each block that changed the pool).

**RPCs.** `getpool` (its `concentrated` state), `getpoolticks pool` (the liquidity by price: a distribution chart),
`previewposition pool lower upper {amounts}`, `getposition id`; wallet `openposition pool lower upper {amounts}`,
`changeposition id {add|remove|collect|close}`, `listpositions` (in range or not, amounts, fees owed). Prices are of
the pool's asset0 (the lower id; CHN in a pool with CHN) in its asset1, whole units.

**Security review (2026-10-21) and how it is met.** (1) *Liquidity bounds*: a tick carries at most
`MaxLiquidityPerTick(spacing)` = (2^128 − 1) / (the ticks of the spacing), as Uniswap v3's maxLiquidityPerTick, so the
liquidity in range, a sum over ticks, never overflows; a position or tick past it is refused (`bad-ba-position`), every
change of the liquidity in range is checked both ways, and a crossing that would take it below zero or past 128 bits is
not clamped: the round fails and its orders are refunded (a state the caps make impossible; failing the round rather
than the block keeps a chain with such a state from stopping for good). (2) *Fees across ranges*: a round's fees and
roundings are credited stretch by stretch along the walk, in proportion to the input each stretch took, to that
stretch's liquidity, before the tick at its end is crossed (tested against a computation by hand across two
ranges). (3) *The search's arithmetic*: Â·dy is computed in 256 bits (it overflows 128). (4) *Tokens*: position and
order tokens parse in a marker's outputs as their serialization writes them, and a marker that names one is not valid
before the pool kinds' height (`CheckCurvesActivation`); both tested.

## Dynamic fees

A pool of any kind may be made with `POOL_FLAG_DYNAMIC_FEE` (`createpool … {"dynamic_fee": true}`; another pool
than the one without it, its id names the flag). Its fee in a block is its tier's plus half its *volatility*, at
most `DYNAMIC_MAX_EXTRA` (2 %) more: the volatility is a moving average (window `DYNAMIC_WINDOW`, 8) of the change of
the pool's price, in millionths, from one block that changed it to the next (`FeeState`, table `0x8f`), halving
every `DYNAMIC_HALF_LIFE` (40) blocks without a change. The fee of a block is fixed by the state before it (the
clearing reads it once per pool); quotes take it as of the next block. A pool whose price moves 1 % a block charges
about 0.5 % more; quiet, it returns to its tier.

## Launches

Bonding-curve launches of assets, with the pool kinds (same height, `dex_curves_height`; markers 22 and 23); with their
own supply, decimals and fee, and an existing asset on a curve, from `launch_params_height` (markers 36 and 28;
doc/dex-math.md "Launch curve").
`src/bitassets/curves.cpp` (`ClearLaunch`), `src/bitassets/state.cpp` (`PlanLaunch`, `Graduate`, `ClearOrders`).

**A launch** (`LaunchToken`, kind 22) registers a token from a reservation of its ticker, exactly as
`Register` does (reserve, then reveal once the reservation is deep enough: nobody can take a ticker they saw
in the mempool; the ticker and look-alike rules of assets apply), with its display name, logo and description
(at most `MAX_INFO_SIZE`, links included). Its supply is the rules', `LAUNCH_SUPPLY`: a billion tokens of 6
decimals, fixed for good (no control coin). None of it goes to the creator at launch: the tokens are issued
against the chain's coin, from a curve that buyers pay and that pays sellers. Optionally the launch buys some
of its own token in its own block (`first_buy`, an order of that block like any other).

**The curve.** A constant product of *virtual* reserves, x·y = k, x the chain's coin (asset0), y the token:
from (V, Tv) to its end (V + G, yE), with G = `launch_graduation`, V = G/2, Tv = 9/8·S and yE = 3/8·S
(S = `LAUNCH_SUPPLY`). Then V·Tv = (V + G)·yE exactly: selling the curve's 3/4·S raises exactly G. The last
quarter, 1/4·S, goes with the raised coin into the pool at graduation; that pool, (G, S/4), starts at the
curve's last price, (V + G)²/k = 4G/S (the marginal price is continuous; tested). The price rises 9× along the
curve; the market value of the supply at graduation is 4G. The curve's pool is a pool of its own kind
(`Curve::LAUNCH`), so orders reach it through the batch (`SwapPools`, routes from other assets via the coin
included) and its prices feed the TWAP; nobody can put liquidity into it, and no other pool of the token can
be made until it graduates (`bad-ba-launch-pooled`, `bad-ba-launch-pool`).

**Clearing (uniform price per block).** A launch's round in the batch is a constant-product round of the
fee-free amounts (ClearPool's formula on the virtual reserves: buyers' coin less the fee, sellers' tokens
whole), unless it would sell past yE. Then the round goes *exactly* to the end, at the price of its last
stretch P = Δx/Δy (Δy = y − yE, Δx = ⌈x·y/yE⌉ − x: a chord of the convex curve, so the curve never takes less
than it would): sellers get ⌊b·P⌋; buyers that pay the coin in directly share the Δy + B tokens there are in
proportion to what they pay, rounded down, the units left given one each, in order, to those that can pay for
one more; each pays ⌈t·P⌉ (and the fee on it) and gets the rest of its coin back by the coinbase. A buy that
reaches the curve through another pool first misses at that round (it is refunded whole: a part could not be
given back through the route). The fee is taken in the coin from every trade (on the way in for a buy, on the
way out for a sale, rounded up) and leaves the curve. Conservation (coin in = curve's coin after + fees + coin
out; tokens likewise), the product never falling, never selling past yE, and the end reached exactly are
property-tested; 60 vectors (`src/test/data/dex_launch.json`) are checked by the Python reference too.

**Graduation.** At the end of a block whose clearing reached the curve's end (or once the curve's coin reached
V + G), the launch graduates: its curve's pool goes, and the token's pool with the coin (the kind AddLiquidity
makes: constant product at 0.30 %, the pool every `swapasset`, route and default liquidity names; a tier that
lets every wallet and route find it, a fee high enough for a young, volatile token's LPs, and the same as
every other default pool) is made with the coin raised plus the fees kept for it and the tokens left plus S/4.
Its shares (isqrt of the product) are held by nobody, and nobody can ever hold them: the liquidity is locked
for good. Anyone may add liquidity on top (their own shares). The raised coin at graduation is G plus a few
satoshis of rounding (the chord's ceiling); `getlaunch` says it exactly.

**Fees** (consensus parameters, kept by each launch as made; see "Fees of the DEX"): `launch_fee` of every trade on the
curve, split between the creator (`ClaimLaunch`, kind 23, sent by anyone: the coin by the coinbase to the creator's
script), the protocol (the treasury) and the chain's security (the block's BMM bidder and validators), the remainder into
the pool at graduation. No protocol token, no buybacks (the treasury has no spending rule yet). A creator's allocation is
optional, at most `MAX_LAUNCH_ALLOCATION` (10 % from `SidechainParams::allocation10_height`, 5 % before it;
the supply and its allocation together within `MAX_AMOUNT`), minted only at graduation (to the creator, by a claim): until
then no token exists but those the curve sold, so the curve always holds the coin to buy them back; the
allocation is shown on every launch (`allocation`). Anti-abuse: the ticker rules of assets (reservation and
reveal, no look-alikes), per-launch `min_buy` and `max_buy` (CHN; a buy outside is refunded), and the batch
itself (every buy of a block, the creator's first included, at one price).

**Parameters.** `launch_graduation` = 500 coins on every network (Layers: 500 CHN, the "CHN equivalent" until a
rate is chosen; the eCash chain: 500 ECX), `launch_fee` and its parts as in "Fees of the DEX"; in the drivechain
database's fingerprint with the pool kinds' height.

**State.** `0x8c` launches (`LaunchRecord`), `0x8d` the log (height, token) of the last `LAUNCH_LOG_BLOCKS`
(10 080) blocks, `0x8e` launches by height; the curve's pool and kind in `0x34`/`0x8a`. `HeldChn` counts a curve's
coin less its virtual part, and the fees not paid out.

**RPCs.** Node: `getlaunch token`, `listlaunches {status: all|curve|graduated|new, min_progress, sort:
newest|oldest|volume|progress|market_cap, cursor, limit}` (`next_cursor`), `quotelaunch token amount (buy|sell)`
(out, back, fee, price, impact, progress after, whether it graduates), `getlaunchstats` (counts, volume, raised,
the rules), `getlaunchlog from (to)` (a row per block and launch for indexers: created, graduated, orders, coin
and tokens in and out, fees, the curve after), `decodeassettx` (launch_token, claim_launch). Wallet: `launchtoken
ticker {display_name, logo, info, allocation, first_buy, min_buy, max_buy}`, `buyfromlaunch token amount`,
`selltolaunch token amount`, `claimlaunch token`. Tests: `dex_launch_tests`, `feature_layers_launch.py` (launch,
buys and a sale, graduation at 500 into a locked pool, a reorganisation across it, the creator's claim).

## Order books

Limit orders beside the pools, with the pool kinds (same height, `dex_curves_height`; markers 26 `PlaceOrder` and 27
`SettleOrder`). `src/bitassets/state.cpp` (`PlanPools`, `ClearBook`, `CrossBooks`, `SweepBooks`).

**The book.** The first order of a pair makes its book: a pool record of curve `BOOK` (fee 0) at `BookId(a, b)`; its
reserves are what its orders hold (what is left to sell, what they got and not collected, the roundings), its shares
`MIN_LIQUIDITY` plus its open orders. An order sells `amount` of one asset for the other at a limit `price_num /
price_den` (units bought per unit sold, at least), until a block `expiry` (at most `MAX_ORDER_DURATION` ahead); its id
is `OrderId(txid)`, and a token of kind `ORDER` (one unit) is its holder's: it collects and cancels. Open orders are
indexed by book, side, limit (UQ128.128, big-endian: best first) and id (table `0x99`), and by expiry (`0x9b`).

**Clearing: a call auction.** In a block's batch a book is a venue as a pool is (`swappools`, routes, quotes): a round
of the book has one price, chosen among the limits of the best `MAX_BOOK_ROUND` (256) live orders of each side: the one
that trades the most of asset0 (min of what sells at it and what buys at it), then the least imbalance, then the lowest.
Takers (orders of the batch through the book) fill whole or not at all (they miss, and are refunded, if the book cannot
take them whole); makers fill by limit, then id, partially. What the buy side gets is ⌊paid/P⌋, what each seller gets
⌊sold·P⌋: the roundings stay in the book, which never pays more than its orders hold (`random_books_stay_solvent`).
At a block's end, the books it placed orders in clear once more without takers: resting orders that cross trade
(`CrossBooks`). A book's hop costs 2 units of `MAX_BLOCK_CURVE_WORK`, as does the first order of a block in a book
(its crossing at the block's end); a worst round (256 + 256 orders at different limits) takes about 0.8 ms.

**Settling.** `settleorder order (cancel)` spends the order's token: it pays what the order got and has not collected;
cancelled, expired or filled, what is left of what it sells comes back too and the order is gone; else its token comes
back. CHN goes by the coinbase (`MinChnPayout` at least; less waits).

**Security.** (1) *A side's limit is a count of its index*: each side's entries are counted in the state (table
`0x9c`: one more when placed, one less when filled, settled, swept or evicted; undone with a block like the rest), and a
side never holds more than `MAX_BOOK_SIDE` (1000) entries, live or expired. Should the index not show as many entries as
counted, a placement is refused (fail closed). (2) *Expired orders never crowd out live ones, nor lock a book*: the end
of each block sweeps expired orders out of their books' indexes, oldest first, at most `MAX_BOOK_SWEEP` (5000) a
block, the rest at the next (a backlog; no cap on how many may expire at a height, so nobody can fill a height for
others); and an order placed on a full side first takes that side's own expired entries out itself, so no backlog
elsewhere can keep a book full. A side is read with expired, filled and evicted entries skipped before a round's limit
counts them (`BookSide`; a side has at most 1000 entries, within `MAX_BOOK_SCAN`). (3) *A full side is no lock, and
no cheap grief*: full of live orders, an order takes the place of the side's worst (the highest limit, then the highest
id) only if its limit is better and it sells at least what the worst has left (the same asset: worth as much at least);
the worst leaves the book flagged `evicted` (`getorder`'s `evicted_height`, `listorders`' `evicted`) and its holder
settles it for all it holds, nothing lost; anything else is refused (`bad-ba-book-full`). (4) *No dust*: an order buys
something at its limit; of a pair with CHN, its CHN side is worth `MinBookChn` at least (0.01 CHN before `small_payout_height`, 0.00005 from it); of another pair,
it sells a whole unit (10^decimals) at least (`bad-ba-order-dust`). (5) *State written before the counts*: orders can
exist only from the pool kinds' height, which no running network has reached; a store with orders and without the
books' layout marker (`0x9d`, written with the first order: a development build) is not current
(`StoreLayoutCurrent`), so the node derives it again from the blocks, counts included; an order record without its
`evicted` field reads as never evicted. Tests: `stuffed_with_expiring_orders`, `full_of_unswept_expired_orders`,
`dust_refused`, `full_side_evicts_its_worst` (with the block undone: the eviction, the counts and the shares back),
`layout_of_books`, `sweep_bounded`, `worst_round`.

RPCs: `getbook a b (levels)` (the ladder of each side, best bid and ask), `getorder order`; wallet `placeorder sell
amount buy price (blocks)`, `settleorder order (cancel)`, `listorders`.

## Verified issuers

Not a rule: what wallets show. A *curator* is an oracle (above): it verifies an asset by attesting the event
`Layers verification <asset id> #<round>` (rounds from 1) with 1, and takes the verification back with 0 at the next
round; its word on an asset is that of its last round attested (rounds read in order until one is not, at most
`dex::MAX_VERIFICATION_ROUNDS`, 64). Nobody can verify in a curator's name (the attestation is its key's signature),
and a curator can only say yes or no of an asset id (not of a ticker or a look-alike). Each wallet chooses the curators
it trusts (none by default: no badges); the app shows a badge on an asset that one of them verifies now. RPCs:
`getverifications asset curators` (only the curators named count, 1 to 200: anyone can register an oracle, so there is
no verification without the user's own list; the result says which of them verify it); wallet `verifyasset curator asset (verified)` (the next round, signed with the wallet's key of the oracle).

## The market index

The search index (`src/bitassets/index.{h,cpp}`, derived data kept with the state, not a rule) keeps a row per pool:
its reserves, the reserves at the start of the volume window (`asset_volume_window` blocks, about a day), its volume in
the window and its last block; `searchpools text filters sort cursor limit` pages through them sorted by depth (the CHN
it holds; then the pools without CHN, newest first: every pool is listed), volume (CHN traded in the window), `price_change` (over the window) or newest, with each pool's state as
`getpool` gives it. The price change is the change of reserve0/reserve1, so for a pool of another curve than constant
product the index keeps the second reserve as the first times its spot price (`PricedReserve1`: the curve's slope; a
concentrated pool's square root price squared; 0 for a book, which has no price of its own): the ratio is then its
price, as for the others, and the rows of constant-product pools are what they always were. Volumes are counted in CHN,
so for pools with CHN only (a pair without CHN shows 0; its all-time volumes are `getpool`'s `volume_a`/`volume_b`). The
app's Markets tab is this index: price, 24-hour change, volume and liquidity, searchable and sortable.

## Fees of the DEX

Consensus parameters of each network (`Consensus::SidechainParams`, in the drivechain database's fingerprint with the
pool kinds' height), in millionths of a fee; each part is the floor of its share, and the remainder of the roundings
goes to a party named below, so that the parts add up to the fee, exactly.

| | Layers (CHN) | Flush (ECX) |
|---|---|---|
| Swap fees (every pool kind and tier; order books charge none) | providers 89 %, security 11 %, protocol 0 | providers 85 %, security 11 %, protocol 4 % |
| Launch curves: the fee of a trade | 1 % | 1.15 % |
| … its parts | creator 80 %, security 20 %, protocol 0 | creator 50 %, protocol 30 %, security 20 % |
| Network fees of transactions | the BMM bidder and the validators, by the fee curve; peg fees the bidder's | the same |

(`dex_fee_security_share`, `dex_fee_protocol_share`, `launch_fee`, `launch_creator_share`, `launch_protocol_share`,
`launch_security_share`; the rest of a swap fee is the providers', the rest of a launch's fee goes with its curve into
the pool at graduation. A launch keeps the parts it was made under.)

**Swap fees.** In a pool's round of a block's batch, the fee of what came in of an asset is ⌊in·fee/F⌋ (F = 10^6; the
dynamic fee when the pool has one); its security part is ⌊fee·s/F⌋ and its protocol part ⌊fee·p/F⌋ (`SwapFeeParts`);
both leave the pool after the round (its reserves less them: the pool still holds its level, since its worst point is
net of all the fees), and the providers keep the rest, the remainder included. A concentrated pool's round takes them
out of what it kept beyond its walk (at most that: the security part first) and credits its positions the rest.
**Launch fees**: the creator's ⌊fee·c/F⌋, the protocol's ⌊fee·p/F⌋, the security part ⌊fee·s/F⌋ (`LaunchFeeParts`),
the remainder into the pool at graduation.

**Security parts** paid in the chain's coin are fees of the block: `bitassets::State::BlockSecurityFees(height)` (cell
`0x9f`) joins the ordinary fees of its transactions in the fee split (`pos::SplitFees`'s `extra_ordinary`): the BMM
bidder and the validators share them by the same curve as network fees (50/50 at the target spacing, down to 20/80),
into the pot or kept by the merged miner, and into the validator pool (`CheckFees`, `bad-pos-fee-split`). The coinbase
may pay them beyond its transactions' fees (they are counted with what it mints: the DEX's coin, which the state held,
paid out); a block of stake's `AfterBlock` counts them in the pot and pool outputs. Security parts of fees paid in an
asset (not the chain's coin) cannot be paid by a coinbase: before `no_treasury_height` the treasury held them per asset
(`security_held`); from it they stay in their pool, for its providers.

**No treasury, from `no_treasury_height`** (`SidechainParams::NoTreasury`; never on the eCash networks until decided,
regtest from the DEX's height, slot 99 at its upgrade v4). Before it, the treasury (table `0x9e`, `TreasuryEntry` per
asset) took the protocol's parts (swaps 4 %, launches 30 % on Flush) and the assets' security parts, held by the chain
itself, never spent (no rule ever spent from it). From it: a swap fee is 89 % to the providers and 11 % to the chain's
security (the protocol's part stays in the pool), and a security part in an asset stays in its pool too; a launch's
curve fee is 80 % to its creator and 20 % to security (a launch made before keeps its own parts, its protocol's going to
its creator). At that block, once, what the treasury held of the chain's coin is paid with the block's fees as their
security part (`SecurityFees`: the BMM bidder and the validators); its entries stay as a record, and what it held of
assets stays held, unspendable (it came out of many pools: none is the one to give it back to). `gettreasury` and the
public API's `treasury` give that record, `active` false and `removed_height`; the parts in force in the next block are
`getdexinfo`'s `swap_fee_parts`, `getlaunch`/`getlaunchstats`' shares and the API's `fees`.

**Conservation, reorganisations.** Every part is an integer: the parts and the remainder add up to the fee charged
(`fee_parts_exact`); a block's pools, treasury and security parts add up to what its orders paid in and got
(`fee_parts_conserved_and_undone`, launches: `launch_trade_graduate_claim`); a block taken back takes the treasury's
accruals and its security parts back with the rest of its changes (the store's journal). The bidder/validators split of
the security parts is the fee split's, to the satoshi (`security_parts_split_as_fees`). What a round takes out of a pool is what it credits:
the parts are clamped to what was taken (at most what the pool holds; a concentrated pool: what it kept beyond its
walk), the security part first, in every branch; launches' parts come out of fees already taken out of the curve, and
books charge none. Random blocks of orders against small pools of every kind at the highest tier (one with a dynamic
fee) conserve every asset exactly: what orders paid in less what they got equals what the pools gained plus what the
treasury took (`fee_parts_conserve_every_kind`, a block taken back included).

## Logos of assets

Layers has BitAssets' logos whole (doc/bitassets.md, "Logos"): a PNG or JPEG of exactly 256 x 256 pixels, at most
32768 bytes, carried by the marker of the transaction that sets it (SetLogo, kind 32; a registration with one, kind
33 — clear of the DEX's kinds 13 to 17), by the holder of the control coin; consensus checks the file's structure only
(bitassets/logo.cpp: chunks or segments, sizes, CRCs, no metadata, nothing after the end; `bad-ba-logo-*`). The state
keeps each asset's current logo (tables 0x90 and 0x91, clear of the DEX's 0x80-0x89 and FastBlocks' 0x50-0x5f; the
test of duplicate table ids keeps them apart), so a pruned node serves it, and a reorganization puts the one before back.

- Activation: `SidechainParams::logo_height`. Main network, test network and signet 0 (from the first block);
  regtest 0 or `-testactivationheight=logo@<height>`. Like
  `dex_height`, a height set goes into the fingerprint of the drivechain database (each node makes it again once).
- RPCs: `getassetlogo`, `setassetlogo`, `registerasset ... <logo>`, `logo` in `getasset` / `listassets`, `logo_height`
  in `getbitassetsinfo`; `listassettransactions` lists the asset moves of the wallet with the other side (an operation
  of the DEX shows as what it sent and received).
- The app: the logo picker in Register new asset and Change logo, logos wherever assets show, the logo in the
  confirmation; Your assets on the Home and the Overview; asset transfers in Transactions.
- Tests: `bitassets_logo_tests` (unit), `feature_bitassets_logo.py`, `feature_bitassets_transactions.py`,
  `feature_layers_logo.py` (the same with the DEX in force).

## Tickers and display names

Layers has BitAssets' tickers and display names whole (doc/bitassets.md, "Tickers and display names"): from
`SidechainParams::ticker_height` the name of a new asset is a ticker (2 to 12 of A-Z and 0-9, published; the wallet
uppercases), assets registered before keep their names; an optional display name, not unique, at most 64 bytes of
UTF-8 from a frozen Unicode 15.0 table (no controls, bidi or zero-width characters, combining marks, nothing NFC
changes), set at the registration (marker kind 35) or by the holder of the control coin (SetDisplayName, kind 34) —
clear of the DEX's kinds 13 to 17. The state keeps it in table 0x92 (clear of the DEX's 0x80-0x89 and FastBlocks'
0x50-0x5f), journaled; the search index (layout 2) finds assets by it after the names, and a layout-1 index is
brought to layout 2 in place at startup (nothing derived again).

- Activation: main network, test network and signet 0 (from the first block); regtest never or `-testactivationheight=ticker@<height>`.
- RPCs: `registerasset ... <display_name>`, `setassetdisplayname`, `display_name` in `getasset` / `listassets` /
  `searchassets`, `ticker_height` and the limits in `getbitassetsinfo`.
- Tests: `bitassets_ticker_tests` (unit), `feature_bitassets_ticker.py`.

## Search of the assets

Layers has BitAssets' search index whole (doc/bitassets.md, "The search index"): `searchassets`, `searchpools`,
`searchauctions`, `getassetindexinfo`, derived data kept with the state block by block (key byte 0xA0, clear of
FastBlocks' 0x50-0x5f, the DEX's 0x80-0x89 and the logos' 0x90-0x91), always on, undone with a block by a
reorganisation, left out of the state's hash, no rule reads it. Blocks of stake and BMM blocks update it alike, and
what the DEX does to the pools (routes, batches) counts as activity and volume of their assets as a swap does. The
volume and price-change window, `SidechainParams::asset_volume_window`, is 5760 blocks on the main network, the test
network and signet (a day of 15-second slots), 10 on regtest. Upgrading derives the sidechain state once from the
blocks at startup (index layout 1).

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
`dex_curves_tests` (the pool kinds' arithmetic: properties, extremes, determinism vectors, the worst case) and
`dex_book_tests` and `feature_layers_book.py` (order books: crossing, takers, routes, expiry, solvency, the
security cases), `dex_pools_tests` (their activation, pools of every kind side by side, batches, routes across kinds, one-sided
liquidity, retiring an asset, the per-block bound), `feature_layers_dex_curves.py` (before and after their
height: pools of each kind, a swap and a batch against the Python reference, a route across kinds, one-sided
liquidity, the TWAP, a reorganisation across a clearing).
`feature_layers_dex.py` (before and after the DEX's height: routes of 2 and 4 hops, batches in a
BMM block and in a block of stake, a missed `min_out`, the TWAP over windows and across a
reorganisation, oracles with the wallet's key and an outside one, contracts to both outcomes and a
refund, the peg balance).
