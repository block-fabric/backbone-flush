# Sidechains of Backbone on eCash

A sidechain made from this template runs on two families of networks:

- the **Chains** (CHN) networks, `main`, `test`, `signet`, `regtest`, sidechains of chainsd, as before;
- the **Backbone** networks on eCash (ECX), `-chain=ecash`, `ecashbeta`, `ecashtest`: sidechains of
  backboned on eCash's main network, its betanet and its test network (backboned's own `-chain` names).

The rules of a sidechain are the same on both. What differs is the mainchain: Backbone on eCash follows the
drivechain rules of the bip300301_enforcer (votes counted per period; a bundle is paid with more than 13,150
votes in 26,300 blocks, more than 5 in 10 on the test network and the eCash regtest), has ten-minute blocks,
uses `OP_NOP8` as `OP_DRIVECHAIN` and, above all, **does not pay one bundle per slot** (see below).

## Networks

| `-chain=` | Mainchain (backboned) | RPC / P2P (template; a sidechain: 8x54/8x55...) | Mainchain RPC (default) | Data directory | Address prefix (template) |
|---|---|---|---|---|---|
| `ecash` | `-chain=ecash` | 8654 / 8655 | 8532 | `~/.backbone-<name>` (Windows `%LOCALAPPDATA%\Backbone-<Name>`, macOS `~/Library/Application Support/Backbone-<Name>`), at its root | `esc` |
| `ecashbeta` | `-chain=ecashbeta` | 38654 / 38655 | 8532 | the same, `betanet/` | `besc` |
| `ecashtest` | `-chain=ecashtest` | 48654 / 48655 | 48532 | the same, `testnet4/` | `tesc` |
| `main`, `test`, `signet`, `regtest` | chainsd | 9654... (unchanged) | 9554, 19554, 39554, 29554 | `~/.<name>` (unchanged) | `sc`, `tsc`, `rsc` |

- **Ports.** A sidechain's eCash ports are those of its Chains main network less 1000 (8x54 RPC, 8x55 P2P),
  betanet's 30,000 above them (38x54), the test network's 40,000 above (48x54, beside Backbone's 48532).
- **The mainchain node.** The cookie is looked for in Backbone's data directory (`~/.backbone`, and its
  `betanet/`, `testnet4/`) for the eCash networks, in Chains' for the others; `-mainchaindatadir`,
  `-mainchainrpcport` and `-mainchainrpccookiefile` as before. The network check of the link reads
  `getblockchaininfo`'s `network` (Backbone's name: `ecash`, `ecashbeta`, `ecashtest`; its `chain` is eCash's
  `main`/`testnet4`), else its `chain` (chainsd). A sidechain on regtest follows either a Chains regtest node or
  backboned `-regtest -testecash` (with `-testmainenforcer`, below).
- **Identity.** Each eCash network has a genesis block of its own (its own message), message start, ports and
  address prefix (`CEcashSidechainParams`, `src/kernel/chainparams.cpp`; the TODO there says what a new
  sidechain sets). The rules are those of the sidechain's main network (its test network's for `ecashtest`).
- The app's supervisor of sidechain nodes passes `-chain=ecash|ecashbeta|ecashtest` for a mainchain on those
  networks (`webui/sidechainnodes.cpp`). The app's own network list (`chain.json`) has the Chains networks only
  so far (follow-up).

## Single payout, kept by the sidechain

The Chains mainchain fails a slot's other pending bundles when it pays one, and never lets a paid bundle be
proposed again. The enforcer does neither, until it adopts our safety rules at a height
(`getdrivechaininfo`'s `singlepayout`, Backbone's `safety_activation_height`): a bundle of another branch of the
sidechain (one a reorg left behind) and a bundle of this branch holding the same withdrawals can both be paid.
`SidechainParams::main_single_payout` (false on the eCash networks and with `-testmainenforcer`) makes the
sidechain keep the rule on its side:

- **No new bundle while another one of the slot has support** (`State::NextBundle`, consensus of the sidechain,
  from the deterministic record of pending bundles and scores): a score of `pending_min_score`, or the lead of
  the slot on the rise, as for refunds. It waits until that bundle is paid (its payouts are then matched to the
  withdrawals, which are paid once) or fails. `getwithdrawalbundle` says `held` meanwhile. A bundle nobody
  supports (a bare M3, downvoted by the miners who vouch for this chain) holds nothing back.
- **Refunds** wait for such a bundle, as on Chains.
- **Votes.** The sidechain node tells its mainchain node to upvote its bundle and downvote every other one of the
  slot (`vouchwithdrawalbundle`); Backbone's block template never votes for, proposes or pays a bundle that was
  paid, or pending at a payout of its slot, and does not broadcast its withdrawal (`sendwithdrawalbundle`).
- The warning "paid a withdrawal bundle of this sidechain and left another one pending" is said only where the
  mainchain is meant to pay one bundle per slot.

What remains: a majority of eCash's hashrate can still pay a bundle twice (a paid bundle proposed again, or a
leftover one), against the votes of the miners who run the sidechain; the sidechain then matches the payout to
withdrawals as far as it can, and the escrow pays the difference. That is the enforcer's rule until it adopts
single payout and paid-bundle memory.

## Parameters tied to the mainchain's blocks

The mainchain's blocks are ten minutes on eCash against one on Chains; one block of the sidechain per mainchain
block. Per network (`Consensus::SidechainParams`, `nPowTargetSpacing`):

| Parameter | What | Chains main (test) | eCash `ecash`, `ecashbeta` | `ecashtest` | regtest `-testmainenforcer` |
|---|---|---|---|---|---|
| `pending_min_score` | score with which a foreign bundle holds refunds (and, without single payout, new bundles) back: a tenth of the payout threshold | 6480 (30) | 1315 (of 13,150) | 2 (of 5) | 2 (of 5) |
| `bundle_retry_delay` | sidechain blocks after a failed bundle before the next | 144 = 2.4 h (20) | 15 = 2.5 h | 3 = 0.5 h | 5 |
| `unproposed_expiry_blocks` | mainchain blocks in which the mainchain must propose a committed bundle | 1440 = 1 day (60) | 144 = 1 day | 12 = 2 h | 20 |
| `main_single_payout` | the mainchain pays one bundle per slot | true | false | false | false |
| `refund_grace_blocks` (policy) | blocks in which a new withdrawal can still be refunded ahead of a bundle | 6 | 1 | 1 | 6 |
| `main_behind_margin` (policy) | mainchain blocks behind its headers before the link is "syncing" | 6 | 2 | 2 | 6 |
| `nPowTargetSpacing` | the sidechain's block interval: stale-tip and block download timers, the app's estimates | 60 s | 600 s | 600 s | 600 s |
| `PENDING_TREND_BLOCKS`/`MIN_RISE` | a rising foreign leader: 3 net upvotes in 12 mainchain blocks | 12/3 | 12/3 (same ratio: 13,150 in 26,300 is half a vote per block, as 64,800 in 129,600) | 12/3 | 12/3 |

Unchanged, as they count no time: BMM requests are good for the next mainchain block only (both mainchains);
deposits are paid by the first sidechain block after the mainchain block that has them (no maturity:
`coinbase_maturity` 0); withdrawal bundles live as the mainchain says (`bundle_max_age` 26,300 blocks, half a year,
on eCash; payment after more than 13,150 votes, at least about 91 days); the follower polls every second.

The parameter fingerprint of the drivechain database changes only where `main_single_payout` is false.

## Slots on eCash

LayerTwo Labs' sidechains on eCash (`LayerTwo-Labs/drivechain-frontends`, `sidechain_core/lib/config/sidechains.dart`,
read 2026-10-20): BBC 1, BitNames 2, BitAssets 4, Thunder 9, Truthcoin 13, Liquid signet 24, zSide 98, Photon 99,
FreeBank 130, CoinShift 255. Ours on Chains collide at 2 (Thunder), 4 (BitAssets) and 9 (Hivemind). Betanet's
list of active sidechains could not be read from public data here (no betanet node with its blocks on this
machine, and no public page found that lists betanet's slots); check it with `listactivesidechains` on a synced backboned
`-chain=ecashbeta` before proposing.

Proposed map, a block of our own away from LayerTwo's (so that their next sidechains, at low numbers, do not
collide either):

| Sidechain | Chains slot | eCash slot |
|---|---|---|
| Thunder | 2 | **40** |
| BitNames | 3 | **41** |
| BitAssets | 4 | **42** |
| zSide | 6 | **43** |
| Hivemind | 9 | **44** |
| FastBlocks | 5 | **45** |
| Layers | 7 | **46** |
| template | 0 | 0 (placeholder) |

The same slot on `ecash`, `ecashbeta` and `ecashtest`. A slot is taken by proposing (M1) and acking (M2) it:
on eCash's main network more than 1,815 acks in 2,016 blocks (provisional, the BIP300 specification's), on
betanet more than 1,008 in 2,016: a two-week commitment of a hashrate majority.

## Tests

- `src/test/drivechain_tests.cpp` `without_main_single_payout_a_supported_bundle_holds_new_ones_back`: with and
  without single payout on the mainchain; a block committing to a bundle while a supported foreign one is
  pending is invalid without it.
- `src/test/sidechain_mainclient_tests.cpp` `ecash_networks`: names, ports, folders, data directory, identities
  (no two networks alike), the per-network parameters, the network check of the link.
- `test/functional/feature_sidechain_ecash.py`: against backboned `-regtest -testecash` (`MAINCHAIN_BIN_DIR`,
  default `../chains-ecash/build_node/bin`): proposal and acks to activation, the OP_NOP8 treasury, BMM, a deposit,
  a withdrawal bundle voted and paid (M3, M4, M6), a bundle failing at its maximum age and its withdrawal
  refunded, and a leftover bundle of another branch holding a new one back ("held") until it is paid, the
  withdrawal paid once.

## Layers

Layers on eCash: slot **46** on `ecash`, `ecashbeta` and `ecashtest`; RPC/P2P ports 8254/8255, 38254/38255,
48254/48255; message starts `6c797221`, `6c797222`, `6c797223`; address prefixes `lyx`, `blyx`, `tlyx`; data in
`~/.backbone-layers`. Its rules are those of its main network (`ecashtest`: of its test network), its blocks of
stake every 15-second slot; what counts its anchors (one per mainchain block: ten minutes on eCash, one on
Chains) is scaled (`Consensus::PosParams`, `CEcashSidechainParams`):

| Parameter | What | Chains main | eCash `ecash`, `ecashbeta` | `ecashtest` (Chains test) |
|---|---|---|---|---|
| `fee_target_spacing` (new) | the mainchain spacing the fee curves are drawn for: S taken as 60 × S / target, so 50/50 at the target and below, the second curve 5 points less per target above it, 20 % from 7 × target | 60 s | 600 s | 600 s (60) |
| `max_unanchored_slots` | blocks of stake halt this many slots after the first one no anchor covers | 240 (1 h) | 720 (3 h: an hour without an eCash block is not rare) | 720 (240) |
| `stall_seconds` | BMM blocks carry transactions after so long without blocks of stake, by the mainchain's median time (which lags the clock about 50 min on eCash, 5 on Chains) | 1800 | 3600 | 3600 (600) |
| `seed_lookahead_seconds` | the leader seed of an epoch: the highest mainchain block at least so long (median time) before it | 1200 (20 blocks) | 3600 (6 blocks) | 3600 (600) |
| `unbonding_anchors` | unbonded coins are paid so many anchors later | 2880 (2 days) | 288 (2 days) | 12 (120: 2 hours) |
| `evidence_window_anchors` (slashing) | a double-signing proof is valid this many anchors after its epoch's set | 1440 (1 day) | 144 (1 day) | 6 (60: 1 hour) |
| `pot_reserve_anchors` | each anchor pays 1/this of the pot's reserve | 1440 | 144 | 6 (60) |
| `final_anchors` (not consensus) | wallets call a block final under so many anchors | 6 (6 min) | 3 (30 min) | 3 (3) |
| `slot_seconds`, `slots_per_epoch` | blocks of stake | 15, 240 | 15, 240 | 15, 40 |
| `nPowTargetSpacing` | Bitcoin's heuristics: a block a slot | 15 s | 15 s | 15 s |
| `refund_grace_blocks`, `bundle_retry_delay` | in blocks of this chain (blocks of stake), as on Chains | 6, 144 | 6, 144 | 6, 20 |

Regtest `-testmainenforcer` adds `fee_target_spacing` 600 to the template's (the PoS test values are regtest's).
`getposinfo`'s and `getstakinginfo`'s `feesplit` say `targetspacing`, and `spacing` as the curves take it.
Test: `test/functional/feature_layers_ecash.py` (three validators against backboned `-regtest -testecash`:
activation, the OP_NOP8 treasury, a deposit, blocks of stake, anchors, anchors 40 slots apart with blocks of
stake going on between them, a withdrawal from a block of stake bundled, voted and paid out, the peg balance).
