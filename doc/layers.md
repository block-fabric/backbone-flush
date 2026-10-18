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
  to set later; regtest keeps FastBlocks' option `-testactivationheight=feesplit@<height>`). The BMM bidder
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
- **Heights.** BitAssets counts in blocks (an auction's start, its duration, the reveal
  depth of a registration): on Layers a block is 15 seconds, so an auction of 240
  blocks runs an hour, and a registration is revealed 45 seconds after its
  reservation on the test network.

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
