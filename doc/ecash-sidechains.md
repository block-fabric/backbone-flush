# Sidechains of Backbone on eCash

A sidechain made from this template runs on two families of networks:

- the **Chains** (CHN) networks, `main`, `test`, `signet`, `regtest`, sidechains of chainsd, as before;
- the **Backbone** networks on eCash (ECX), `-chain=ecash`, `ecashbeta`, `ecashtest`: sidechains of
  backboned on eCash's main network, its betanet and its test network (backboned's own `-chain` names).

The rules of a sidechain are the same on both. What differs is the mainchain: Backbone on eCash follows the
drivechain rules of the bip300301_enforcer (votes counted per period; a bundle is paid with more than 6,048
votes within 12,096 blocks on `ecash`, more than 13,150 in 26,300 on betanet, more than 5 in 10 on the test network and the eCash regtest), has ten-minute blocks,
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

## Without single payout: rules from the enforcer's events

The Chains mainchain fails a slot's other pending bundles when it pays one, and never lets a paid bundle be
proposed again. The enforcer does neither, until it adopts our safety rules at a height
(`getdrivechaininfo`'s `singlepayout`, Backbone's `safety_activation_height`). On the eCash networks the
sidechain's peg rules take only what the enforcer reports per block (`SidechainParams::main_events_only`), so
that a node can follow an eCash node and the enforcer through its interface alone (`-mainchaingrpc`):
no scores, no hold for another branch's bundle, this chain's bundle kept until the mainchain closes it, a
foreign payout matched outside it first, and bundles handed to an enforcer only once their commitment is a day
deep. Everything, with the safety argument: `doc/ecash-enforcer.md`.

- `getwithdrawalbundle` never says `held` on the eCash networks.
- **Votes.** Over JSON-RPC the sidechain node tells Backbone to upvote its bundle and downvote every other one of
  the slot (`vouchwithdrawalbundle`); Backbone's block template never votes for, proposes or pays a bundle that was
  paid, or pending at a payout of its slot, and does not broadcast its withdrawal (`sendwithdrawalbundle`). Through
  the enforcer alone there is no vouching: its block producer upvotes the bundles handed to it.
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
| `pending_min_score` | score with which a foreign bundle holds refunds back: a tenth of the payout threshold (not used where `main_events_only`) | 6480 (30) | 1315 (of 13,150), unused | 2 (of 5), unused | 2 (of 5), unused |
| `bundle_retry_delay` | sidechain blocks after a failed bundle before the next | 144 = 2.4 h (20) | 15 = 2.5 h | 3 = 0.5 h | 5 |
| `unproposed_expiry_blocks` | mainchain blocks in which the mainchain must propose a committed bundle (not used where `main_events_only`) | 1440 = 1 day (60) | unused | unused | unused |
| `main_events_only` | the rules take only the enforcer's events (`doc/ecash-enforcer.md`) | false | true | true | true |
| `bundle_handoff_depth` (policy) | mainchain blocks deep a bundle's commitment is before it is handed to an enforcer (`-mainchaingrpc`) | (6, unused) | 144 = 1 day | 12 = 2 h | 2 |
| `main_single_payout` | the mainchain pays one bundle per slot | true | false | false | false |
| `refund_grace_blocks` (policy) | blocks in which a new withdrawal can still be refunded ahead of a bundle | 6 | 1 | 1 | 6 |
| `main_behind_margin` (policy) | mainchain blocks behind its headers before the link is "syncing" | 6 | 2 | 2 | 6 |
| `nPowTargetSpacing` | the sidechain's block interval: stale-tip and block download timers, the app's estimates | 60 s | 600 s | 600 s | 600 s |
| `PENDING_TREND_BLOCKS`/`MIN_RISE` | a rising foreign leader: 3 net upvotes in 12 mainchain blocks | 12/3 | 12/3 (same ratio: 6,048 in 12,096 on `ecash`, like 13,150 in 26,300, is half a vote per block, as 64,800 in 129,600) | 12/3 | 12/3 |

Unchanged, as they count no time: BMM requests are good for the next mainchain block only (both mainchains);
deposits are paid by the first sidechain block after the mainchain block that has them (no maturity:
`coinbase_maturity` 0); withdrawal bundles live as the mainchain says (`bundle_max_age` 12,096 blocks, twelve weeks,
on `ecash`, 26,300 on betanet; payment after more than 6,048 votes on `ecash`, at least about 42 days); the follower polls every second.

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

- `src/test/drivechain_tests.cpp` `without_main_single_payout_a_supported_bundle_holds_new_ones_back` (the rules
  with scores, kept for a mainchain without single payout that reports them) and the `events_only_*` tests (the
  rules of the eCash networks, `doc/ecash-enforcer.md`).
- `src/test/sidechain_mainclient_tests.cpp` `ecash_networks`: names, ports, folders, data directory, identities
  (no two networks alike), the per-network parameters, the network check of the link.
- `test/functional/feature_sidechain_ecash.py`: against backboned `-regtest -testecash` (`MAINCHAIN_BIN_DIR`,
  default `../chains-ecash/build_node/bin`): proposal and acks to activation, the OP_NOP8 treasury, BMM, a deposit,
  a withdrawal bundle voted and paid (M3, M4, M6), a bundle failing at its maximum age and its withdrawal
  refunded, and a leftover bundle of another branch holding nothing back, the new one paid and the other
  failed, the withdrawal paid once. `feature_sidechain_enforcer.py`: the same through the enforcer's interface
  alone (`-mainchaingrpc`), with reorgs.

## Flush (Layers on eCash)

Layers on eCash is its own product, **Flush** (this repository): slot **46** on `ecash`, `ecashbeta` (the
default network) and `ecashtest`; RPC/P2P ports 8254/8255, 38254/38255, 48254/48255; message starts `666c7821`,
`666c7822`, `666c7823`; address prefixes `flx`, `bflx`, `tflx`; first blocks `272c13b2c599343bde93ec8664df173ab036ed538448cffb390a762758f92108`, `7d66b10e351e5ad86c562ba761b5476f75c6f1b9f13aa7cf469df3a8d5701b75`,
`2ee6a1d1e193d4115dedda242b367454e028804ba88618ba96b728789466f38d`; data in `~/.flush` (`betanet/`, `testnet4/`; Windows `%LOCALAPPDATA%\Flush`), the
Chains networks (development only, `-chainsnetworks`) in `~/.flush/chains`. Its coin is shown as eCash's,
ECX. (Before the renames: Starbase `sbx`, `bsbx`, `tsbx`, `7362732x`, `~/.starbase`; Layers on eCash `lyx`, `blyx`, `tlyx`, `6c79722x`, `~/.backbone-layers`; never launched.) Its rules are those of its main network (`ecashtest`: of its test network), its blocks of
stake every 15-second slot; what counts its anchors (one per mainchain block: ten minutes on eCash, one on
Chains) is scaled (`Consensus::PosParams`, `CEcashSidechainParams`):

| Parameter | What | Chains main | eCash `ecash`, `ecashbeta` | `ecashtest` (Chains test) |
|---|---|---|---|---|
| `fee_target_spacing` (new) | the mainchain spacing the fee curves are drawn for: S taken as 60 × S / target, so 50/50 at the target and below, the second curve 5 points less per target above it, 20 % from 7 × target | 60 s | 600 s | 600 s (60) |
| `max_unanchored_slots` | blocks of stake halt this many slots after the first one no anchor covers | 240 (1 h) | 1920 (8 h: a gap of the mainchain of 6 h with room) | 1920 (240) |
| `stall_seconds` | BMM blocks carry transactions after so long without blocks of stake, by the mainchain's median time (which lags the clock about 50 min on eCash, 5 on Chains) | 1800 | 3600 | 3600 (600) |
| `seed_lookahead_seconds` | the leader seed of an epoch: the highest mainchain block at least so long (median time) before it; an epoch has a leader once the record has a block with a later median time | 1200 (20 blocks) | 32400 (9 h: epochs seeded through a 6 h gap, the median time ~50 min behind) | 32400 (600) |
| `set_lead_seconds` (new) | the validator set of an epoch: the newest anchor before the epoch before it and at least so long before it starts (`pos::SetBoundary`): fixed before its seed's block can exist | 0 (an epoch) | 32400 (as the seed) | 32400 (0) |
| `unbonding_anchors` | unbonded coins are paid so many anchors later | 2880 (2 days) | 1008 (7 days) | 144 (1 day; 120: 2 hours) |
| `evidence_window_anchors` (slashing) | a double-signing proof is valid this many anchors after its epoch's set | 1440 (1 day) | 1007 (until the stake is paid back) | 143 (60: 1 hour) |
| `pot_reserve_anchors` | each anchor pays 1/this of the pot's reserve | 1440 | 144 | 6 (60) |
| `final_anchors` (not consensus) | wallets call a block final under so many anchors | 6 (6 min) | 3 (30 min) | 3 (3) |
| `slot_seconds`, `slots_per_epoch` | blocks of stake | 15, 240 | 15, 240 | 15, 40 |
| `nPowTargetSpacing` | Bitcoin's heuristics: a block a slot | 15 s | 15 s | 15 s |
| `refund_grace_blocks`, `bundle_retry_delay` | in blocks of this chain (blocks of stake), as on Chains | 6, 144 | 6, 144 | 6, 20 |

### A gap of the mainchain

Blocks of stake go on for **at least 24 hours without BMM** (user requirement, 2026-10-24; six hours from 2026-10-21,
which in practice gave four): the clock runs from the last BMM commitment of ours that landed in an eCash block (the
last acknowledged ticket or merged-mined block), whether eCash is silent or produces blocks that carry no bid of ours
(priced out). These are the long-gap values (`gap_*` in `PosParams`), from `gap_epoch`: from the first epoch on every
Flush network on eCash; on the temporary betanet slot 99 (branch photon-v4-gap, its doc/photon-slot.md "The halt of 2026-10-24") from epoch 67,
the first with no block when it halted. Before it, the values in brackets. Three limits bound how long, t0 the last commitment's
eCash block:

- **The anchors.** The first block of stake no anchor covers comes about one eCash block interval before t0 (the
  commitment's BMM block was made before its eCash block was found); blocks of stake go on for
  `gap_max_unanchored_slots` = 5760 slots, 24 hours, after it [1920, 8 h], then halt: 24 hours from just before t0
  (ten minutes or so on a healthy eCash). When BMM comes back, its first commitment is to a ticket made in the gap,
  acknowledged if still within the 24 hours, the next to a ticket of the newest block: the blocks of the gap are
  anchored, then final (3 anchors). The work of an anchor is a walk over the run it covers (at most 5760 blocks).
- **The seed.** An epoch has a leader once the mainchain record has a block whose median time is later than its start
  less the lookahead, `gap_seed_lookahead_seconds` = 48 h [9 h] (`pos::SeedHeight`). In a silence the median time stops
  where the last block's is, which lags it: about 50 minutes on a healthy eCash (a median of 11 blocks), hours when its
  blocks are sparse. On 2026-10-24 betanet's block 973972 (14:55 UTC) had a median time of 09:35, so at 9 h the last
  epoch with a seed started at 18:00 and blocks of stake stopped at 19:00, four hours in, long before the unanchored
  limit. At 48 h epochs starting up to t0 + 48 h − the lag are led: the seed outlasts the unanchored limit through a lag of
  up to 24 hours, so it never stops the chain first. When eCash produces blocks (bids priced out) the median time moves
  on and so do the seeds: the unanchored limit alone counts.
- **The set.** With the seed 48 hours ahead, `gap_set_lead_seconds` (48 h [9 h]) fixes an epoch's set before its
  seed's block can exist: an anchor whose slot, and whose commitment's median time, are before the seed's time, so a
  validator cannot pick a key or a stake for a known seed (only the mainchain's miner can bias a seed, as before).
  Bonds, unbonds and jails therefore take effect about 49 hours after they are mined [10 h]; during a gap every
  epoch's set is the last anchor's before it.

**The trade-off.** Blocks of stake that no anchor covers rest on stake alone: until an anchor covers them (and 3 anchors
make them final) the validators could, by a majority of the slots they lead, build another branch of them. The window
for that grows from 8 to 24 hours, and the leaders of an epoch are known two days ahead instead of nine hours (a longer
time to target a leader's node). What does not change: a block is final only once anchored (`final_anchors`), never in
a gap; signing two blocks for a slot is slashed by a proof taken for `evidence_window_anchors`, which counts anchors and
so pauses in a gap; finality, slashing and the fork choice keep their rules. Validators' stake changes reach the
schedule later (49 h).

What a gap does not stall: `stall_seconds` (BMM blocks take transactions when blocks of stake stop) compares the
mainchain's median time with the last block of stake, which a gap leaves fresh (and in a silence there is no eCash
block for a BMM block anyway: `stalled` stays false, the merged-mined fallback starts with eCash's next block once
blocks of stake stopped an hour before its median time); the fee split's spacing S (a median of 11 spacings) takes a
gap as one long spacing; the mainchain link (`main_behind_margin`, IBD) sees a quiet mainchain, not a lagging one;
unbonding, proofs and the reserve's payouts count anchors, so they pause. Liveness charges a block of stake for an
epoch of missed slots at most, and from `jail_fix_height` no jail leaves too few validators, so a resumption after a
halt jails nobody out of the set. The anchorer's stall boost (`-validatoranchorbidescalation`, 0.5 of the unanchored
limit) now starts 12 hours into a run; 0.15 starts it at about 3.6 hours, as before. In the app, the Finality card says
when no eCash block came for half an hour: the new blocks stay soft until one anchors them, and how long blocks of stake go on.

**Unbonding and proofs.** A week of anchors (1008; three days, 432, on `ecashtest`, which must outlast the set lead), and
a proof of double signing is taken until the stake it slashes is paid back: an unbond of a set's stake is mined at
that set's anchor or later, so its coins stay at least until that anchor's count plus `unbonding_anchors`; the window
is one less (1007, 431). Both are far past the ~300 anchors a validator still leads after it unbonds (the set lead and
an epoch, at ten-minute anchors). `pot_reserve_anchors` only smooths the payouts of penalties (unchanged).

Regtest scales these with `-testposparam=name@value` (`max_unanchored_slots`, `slots_per_epoch`, `set_lead_seconds`, the `gap_*` four,
`unbonding_anchors`, `evidence_window_anchors`, `pot_reserve_anchors`, `stall_seconds`) and `-posseedlookahead`;
`test/functional/feature_layers_ecash_gap.py` runs a gap of six hours' worth (a mainchain block every 4 s for ten
minutes: 144 s), the anchors' catch-up, finality, a week of unbonding (80 anchors) and a proof one anchor before its
window closes. `getposinfo` says `maxunanchoredslots`, `setleadseconds`, `seedlookaheadseconds` (the current epoch's), `gapepoch`, and in `gap` when blocks of stake stop if no mainchain block comes: `seedsuntil`, `unanchoreduntil`, `blocksuntil`. `test/functional/feature_layers_long_gap.py` runs silences of 23 and 25 hours' worth.

Regtest `-testmainenforcer` adds `fee_target_spacing` 600 to the template's (the PoS test values are regtest's).
`getposinfo`'s and `getstakinginfo`'s `feesplit` say `targetspacing`, and `spacing` as the curves take it.
Test: `test/functional/feature_layers_ecash.py` (three validators against backboned `-regtest -testecash`:
activation, the OP_NOP8 treasury, a deposit, blocks of stake, anchors, anchors 40 slots apart with blocks of
stake going on between them, a withdrawal from a block of stake bundled, voted and paid out, the peg balance);
`test/functional/feature_layers_enforcer.py`: the same three validators following the mainchain through the
enforcer's interface alone (`-mainchaingrpc`, no JSON-RPC: `doc/ecash-enforcer.md`), the anchors' tickets
through `CreateBmmCriticalDataTransaction`, the bundle handed over at depth 2 and paid.
