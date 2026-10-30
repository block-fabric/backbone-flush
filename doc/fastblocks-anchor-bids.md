# FastBlocks: the anchor bid policy

A node bids, from the mainchain wallet, for BMM commitments: a validator's own anchorer for its tickets, and
the merged miner (`setbmm`) of any node (see "The anchorer" in [fastblocks.md](fastblocks.md)).
Blocks of stake halt after the configured unanchored limit (24 hours on upgraded
slot 99, formerly 8 hours) without an anchor, so a bid that miners leave out for
hours halts the chain. The bid policy says what the anchorer offers, and how it
raises its offer as the chain nears that halt. It is consensus-neutral: what a node
bids is its own business; the chain only sees which commitment landed.

## Since 8.3.6: a strategy, and "Prevent stalling"

The user picks one strategy of five, and a checkbox beside it (Louis: "bid only fees, no minimum, and prevent
stalling; strategies are not really convenient"):

| Strategy (`strategy`) | What it bids |
|---|---|
| **Fees** (`fees`, the default) | The fee share (`feeshare` %, 99 by default: the operator keeps 1 %) of what the anchor, or the BMM block, pays out. No minimum, no floor: no fees, no bid. A share too small to pay for its own request (below 500 sat, or the relay minimum of a request) is no bid either: it is never raised to a minimum. |
| **Fees + minimum** (`feesminimum`) | The fee share, or the minimum when the share is below it: max(fee share, minimum), on every mainchain block. |
| **Minimum every block** (`minimum`) | The minimum on every mainchain block, whatever the fees. |
| **Fixed amount** (`fixed`) | `fixed` on every mainchain block. |
| **Off** (`off`) | No bids at all, from either trigger (one by hand still goes). The chain relies on other bidders, and halts without them. |

**The minimum** is the user's floor (`floor`), or the relay minimum of a request if higher: the mainchain's relay
rate (`getnetworkinfo` `relayfee`, raised by `getmempoolinfo` `mempoolminfee`) times the larger of `requestvsize` and
250 vB (the largest request a wallet makes), rounded up. 1 sat/vB: 250 sat. A floor of 0 means the relay minimum.

**☑ Prevent stalling** (`preventstall`, on by default), whatever the strategy but Off:

- **Near the unanchored limit** (16 h on eCash mainnet): from stall pressure
  `p = (slot - firstunanchoredslot) / maxunanchoredslots` of `escalationstart` (0.5), a bid that lands an anchor
  even if the strategy would not bid (Fees without fees): `boost` (4) times the competitive base, or the strategy's
  bid if higher (a large fee share is never multiplied). With `flatboost=false`, the legacy curve instead (below).
- **Peg work waiting** (peg in and peg out alone: a deposit waiting to be credited, withdrawals or a bundle waiting):
  the competitive base, fees or not, as 8.3.5's feespreventstall did; otherwise a deposit would wait for fees or for
  the stall bid, hours on mainnet. (Flat policy only.) Nothing else counts as work (the owner's rule, 2026-10-30): not
  the staking accounting, the pot's reserve, the fee pool, the rewards owed or the unbonds, nor a change of the
  validator set or a stall of blocks of stake by itself. A fee pool of 0.0001479 ECX counted as work made a node bid
  every mainchain block and spend its daily cap in 4 hours. So unbond releases, staking payouts and set changes advance
  only as anchors land: with fees, or a deposit or a withdrawal, at once; otherwise at the next stall bid, which
  Prevent stalling makes from p = 0.5 of a run without an anchor: about 8 hours into it on eCash mainnet (a limit of
  16 h), so one anchor at least every ~8-16 hours.
- **Missed blocks**, for the strategies that bid every block (Fees + minimum, Minimum): the minimum raised, x2 for
  every two mainchain blocks missed in a row, at most `boost` (legacy: the rebid rules below). A fee share or a fixed
  amount is what it is.

**The competitive base** (stall prevention's): the minimum; the next-block estimate (with `usefeeestimate`) rounded up
to whole sat/vB times `requestvsize` (189 vB), else what recent mainchain blocks took for a BMM request (the robust
median of the other active slots, clamped to 10 x the network's least rate); never below the network's least
competitive rate (`min_bid_rate_per_kvb`, 2 sat/vB on eCash). The estimate comes each mainchain block from
`estimatesmartfee 1`, then `estimatefee`. For example 1.41 sat/vB: 2 x 189 = 378 sat, the stall bid 1512 sat.

**Off means off**: no stall prevention either. The checkbox keeps its value and is greyed out in the app; choosing a
strategy again brings it back. A node that wants bids only for its validator's stalls has Fees with Prevent stalling.

**Within the limits, always**: every bid, the stall bid too, is at most `max` and within what is left of the period's
cap. The cap is the user's word on what the wallet spends; a stall bid past it would make it a soft limit. And past it
little is at stake: only landed bids count, and one landed stall bid ends the unanchored run (the pressure drops to
0), so a cap that cannot cover one stall bid is a cap set too low, which the app warns of (`periodcap`) rather than
override. (8.3.5 was the same.)

### The names before 8.3.6

The old strategy names keep their meaning: each also says whether to prevent stalling, unless `preventstall` is given
too (in any order). Three words are both an old name and a new one, `fees`, `fixed` and `off`; where they are said tells
which:

- **`setanchorbidpolicy`, the app, and the settings 8.3.6 writes**: the new names. `fees` is Fees and `fixed` Fixed
  amount, saying nothing of stall prevention (it stays as it was, on by default: `"preventstall": false` is the only
  way to turn it off); `off` is Off, no bids at all.
- **`-validatoranchorbidstrategy`**: the new names too (`fees`: Fees, Prevent stalling as
  `-validatoranchorbidpreventstall` says, on by default), but `off` is 8.3.5's: "Anchor my validator" off, the merged
  miner (`setbmm`) still bids, as `-novalidatoranchorbid`. (To stop every bid of the node: `setanchorbidpolicy
  '{"strategy":"off"}'`.)
- **The settings 8.3.5 wrote** (an `anchorbidpolicy` in `settings.json` without `preventstall` and without `anchor`:
  8.3.6 always saves `preventstall`): 8.3.5's meaning of every word, and 8.3.5's default strategy when none is saved.

| Old name (as 8.3.5 meant it) | Now |
|---|---|
| `feespreventstall` (8.3.5's default, anywhere) | Fees, Prevent stalling on; **Fees + minimum** if the policy has a floor above 0 (it bid at least its floor) |
| `minimal` (anywhere) | Minimum every block, Prevent stalling on |
| `fees` (8.3.5's settings) | Fees, Prevent stalling off; Fees + minimum with a floor above 0 (it bid at least its floor) |
| `fixed` (8.3.5's settings) | Fixed amount, Prevent stalling off |
| `off` (8.3.5's settings, `-validatoranchorbidstrategy`) | "Anchor my validator" off (`anchor` false), the strategy as it was: the merged miner still bids, as in 8.3.5 |
| no strategy (8.3.5's settings, which saved only the fields changed) | the options' strategy, 8.3.5's default `feespreventstall` without one: with the settings' floor above 0, Fees + minimum |
| `-validatoranchorbidmode=standard` | as `feespreventstall` |
| `-validatoranchorbidmode=fees` | 8.3.5's `fees` at 100 %: Prevent stalling off, Fees + minimum with a floor above 0 |
| `-validatoranchorbid=<amount>` alone (no strategy) | the old default with that floor: Fees + minimum above 0, else Fees; Prevent stalling on |
| `-novalidatoranchorbid`, `-validatoranchorbid=off` | "Anchor my validator" off (`anchor` false): the merged miner still bids, as before |

The new names `feesminimum` and `minimum` say nothing of stall prevention: it stays as it was. `setanchorbidpolicy`
keeps the fields it changed in the settings in the new words, always with `preventstall`, so the settings read back
the same; the app always sends both. 8.3.5's settings are read as 8.3.5 meant them, then kept in the new words with
the next change; a strategy that depends on the floor stays `feespreventstall` there, so a floor set later (by
`setanchorbidpolicy` without a strategy) gives Fees + minimum at once, as it does after a restart.

The live validators (`minimal`) do **not** bid exactly as before: Minimum every block with Prevent stalling on bids
more in four cases, always within `max` and the period's cap:

- the minimum is max(floor, the relay minimum of a request: the relay rate x 250 vB), not the floor alone (the same
  for a floor above 250 sat at 1 sat/vB);
- when peg work waits (a deposit to credit, withdrawals or a bundle), the competitive base (at least 2 sat/vB x 189 vB
  on eCash), if above the minimum;
- near the unanchored limit, the stall boost is 4 x the competitive base, not 4 x the floor;
- after missed mainchain blocks, the minimum is raised x2 for every two missed in a row, at most the boost (8.3.5's
  flat policy raised nothing for `minimal`).
The app of 8.3.5 run as `feespreventstall`, floor 0, `usefeeestimate` false, `feeshare` 99, `max` 0.0001,
`periodcap` 0.001 is Fees with Prevent stalling on and no minimum: the fee share when there are fees, nothing
otherwise, and near a stall (or for waiting peg work) the relay minimum's 250 sat x 4 = 1000 sat, within 0.0001 a
bid and 0.001 a day. Two things changed for it: a quiet chain with nothing waiting no longer gets a bid of the base
when the fee share is small but positive (Fees has no minimum: a share below what pays for a request is no bid), and
the merged miner with strategy Off (set in 8.3.6's app or `setanchorbidpolicy`) no longer bids (Off means off; uncheck
"Anchor my validator" instead). An 8.3.5 `off` keeps the merged miner bidding (the table above).

## One policy, two triggers

A validator's anchor bids and the merged miner's bids are the same thing: BMM requests paid from the same mainchain
wallet (the enforcer's, or Backbone's). Since 8.3.5 they follow **one bid policy** (`setanchorbidpolicy`, the
`-validatoranchorbid*` options, `anchorbidpolicy` in `settings.json`), on every node, validator or not. What sets a bid
off:

- **Mine blocks when fees pay** (`setbmm true "address"`, any node): the policy's bid for each mainchain block (with
  Fees, only when fees pay); `always` + `amount` (for tests): `amount` for every mainchain block, within max and the cap.
- **Anchor my validator** (`anchor`, validators, on by default): the validator's own anchorer.

Both bid the policy's bid. At most one request per mainchain block: one ticket serves both (its anchor anchors the
chain and pays its pot to the payee: the `setbmm` address when merged mining is on, else the validator's payout). A
block of stake stall (BMM blocks instead of tickets) is bid for the same way, the policy deciding from the block's fees
(an empty mempool too: no fees, but Prevent stalling near the limit, or peg work, still bids). Before the node has a
mainchain block it bids nothing, merged miner included (no bid without the policy).
A bid by hand (`requestbmmblock`) may not exceed `max`, and it counts in the day's spending. On regtest without
anchor options: no anchorer, Fees without stall prevention, and no period cap (`setbmm` as before).

The daily cap counts every landed bid of the node together (both triggers, and by hand); `getbmminfo`'s `bidpolicy`
and `getanchorinfo`'s `bid` show it (`spentperiod`, `left`, `trigger`). The bids are kept across restarts in
`anchorbids.json`, beside `settings.json` in the network's data directory, written at each bid and each settlement:
the last 50 bids (the history, and the stale rule) and what the landed ones cost over the last 31 days (the longest
period), so a restart does not start the cap again.

### Coins a stale bid tied up

A bid that did not land (missed, or outbid) cannot be mined any more, and Backbone
drops it from its own mempool and its wallet abandons it, freeing its coin. Other
nodes may keep it until their mempool expiry (two weeks by default): a new request
spending the same coin is then a replacement there, refused unless it pays more
than the old one plus the incremental relay fee. Seen on mainnet: Backbone's wallet
reused the abandoned coin at the same fee.

- Flush (8.3.5): the next bid is at least the highest stale bid (not landed since
  the last that did, within two weeks) plus 500 sat (`kind` `replace`), but never
  above `max` nor the cap's rest; when it cannot be, the bid goes as it is, and the
  reason (and a warning) says that nodes holding the stale bid refuse it if the
  wallet spends the same coin (a refused bid costs nothing). `stalebid` in the RPCs.
- Backbone (not done here): the real fix is in its wallet's coin selection for BMM
  requests, which Flush cannot choose: prefer coins no abandoned stale request spent,
  and spend such a coin only above that request's fee plus the incremental relay
  fee. Then Flush's raise is rarely needed.

## The legacy curve

With `flatboost=false`, stall prevention follows the curve instead of the flat boost: from `escalationstart`, the
next-block estimate times `requestvsize` (else the floor, 564 sat at least) times the curve's multiplier
(`0.5:1,0.75:2,0.9:4` by default, interpolated); and the rebids below for the strategies that bid every block.

## Missed blocks

A bid is settled when the mainchain block after the one it followed comes: landed
(its commitment is the bid's ticket), outbid (another commitment of this chain, or
the mainchain went another way) or missed (no commitment of this chain at all).

- Raises (Prevent stalling, for Fees + minimum and Minimum): with the flat boost (the default), the minimum x2 for
  every two missed blocks in a row, at most `boost`; `rebidafter`, `rebidstep` and `rebidmax` do not apply (the app
  shows them only with the flat boost off). With `flatboost=false` (the legacy curve): after `rebidafter` (3) missed
  blocks in a row the minimum is multiplied by `rebidstep` (2) for each such run, at most `rebidmax` (8) times; 0
  turns that off.
- Warning: after `warnmissed` (3) missed blocks in a row the log says "your anchor
  bid is too low" (with the bid in sat/vB and the mainchain's estimate) and
  `getanchorinfo` and the app show it. The node sees only its own slot's
  commitments, so "while other slots got in" is not checked: a miss is a mainchain
  block that came without any commitment of this chain while the request waited.

## Limits

- `max` (default 0.001 coins, 100 000 sat, ~526 sat/vB for 190 vB): the most any
  one bid is. Raise it if your pots are larger and you bid the fee share.
- `periodcap` (default 0.01 coins) in `periodhours` (24): the most the landed bids
  cost (bids that did not land cost nothing). Above it the bid is cut to what is
  left, then nothing until the period frees some. 0: no cap. Kept across restarts
  (`anchorbids.json`).
- `lowbalanceblocks` (1008, about a week of eCash blocks): a warning when the bid
  wallet (the mainchain wallet the requests are paid from, `getbalances`) covers
  fewer mainchain blocks at the current bid.

## Defaults and options

The options are the defaults; `setanchorbidpolicy` changes the policy at once,
without a restart, and keeps the fields it changed in the node's settings
(`settings.json`, `anchorbidpolicy`), over the options; `reset` goes back to them.

| Field | Option | Default |
|---|---|---|
| `strategy` | `-validatoranchorbidstrategy` (old and new names; or the older `-validatoranchorbidmode`) | `fees` |
| `preventstall` | `-validatoranchorbidpreventstall` | on (off on regtest unless an anchor option is given) |
| `anchor` | `-novalidatoranchorbid` / `-validatoranchorbid=off` turn it off | on (off on regtest unless an anchor option is given) |
| `floor` | `-validatoranchorbid` | 0 (the relay minimum) |
| `max` | `-validatoranchorbidmax` | 0.001 (raised to the floor or the fixed bid if below them) |
| `fixed` | `-validatoranchorbidfixed` | the floor |
| `feeshare` | `-validatoranchorbidshare` | 99 |
| `escalationstart` | `-validatoranchorbidescalation` | 0.5 |
| `flatboost`, `boost` | | true, 4 |
| `curve` | `-validatoranchorbidcurve` | `0.5:1,0.75:2,0.9:4` |
| `interpolate` | `-validatoranchorbidinterpolate` | true |
| `usefeeestimate` | `-validatoranchorbidfeeestimate` | true |
| `requestvsize` | | 189 |
| `rebidafter`, `rebidstep`, `rebidmax` (with `flatboost=false` only) | `-validatoranchorbidrebid` (the first) | 3, 2, 8 |
| `warnmissed` | | 3 |
| `periodcap`, `periodhours` | `-validatoranchorbidperiodcap` (the first) | 0.01, 24 (no cap on regtest unless an anchor option is given) |
| `lowbalanceblocks` | `-validatoranchorbidlowbalance` | 1008 |

Checks: `floor ≤ max`, `fixed ≤ max`, amounts within `MAX_MONEY`, `feeshare`
0–100, `escalationstart` in (0, 1], the curve 1–8 points with pressures in [0, 1]
ascending and multipliers 1–1000 not falling, `requestvsize` 50–10000,
`periodhours` 1–744.

## RPCs

- `getanchorbidpolicy`: `validator`, `bidding`, `explanation` (when not bidding),
  `policy` (in force), `defaults` (the options'), `overrides` (the fields set),
  `strategies`.
- `setanchorbidpolicy {fields} (reset)`: the fields given change, the others stay;
  returns as `getanchorbidpolicy`. `flush-cli setanchorbidpolicy '{"strategy":"fees","preventstall":true}'`, `flush-cli -named setanchorbidpolicy floor=0.00001 max=0.0005`.
- `getanchorinfo`'s `bid`: `effective` (the bid now) and `kind`/`reason` (`fees`,
  `minimum`, `fixed`, `rebid`, `pending`, `stall`, `escalation`, `max`, `periodcap`, `replace`, `always`, `idle`, `off`), `normal`,
  `pressure`, `multiplier`, `escalationstart`, `firstunanchoredslot`,
  `unanchoredslots`, `maxunanchoredslots`, `pot`, `feerate` (sat/vB) and
  `feesource`, `missed`, `balance` and `coverblocks`, `spentperiod`, `periodcap`,
  `periodhours`, `history` (the last bids: `mainheight`, `ticket`, `offer`, `kind`,
  `reason`, `pressure`, `status`, `cost`, `time`) and `warnings`.
- `getanchorticket <hash>`: what a mainchain commitment of this chain is: on
  FastBlocks the mainchain commits to a ticket (h*), and this says the block it
  anchors, its height and whether it is in the active chain.

Every bid is logged with its reason: `FastBlocks: anchor bid 0.00001512 for mainchain
block 146 (anchor, stall pressure 0.60): preventing a stall at p=0.60: x4.00 the competitive base 0.00000378 (no bid: no fees)`.

## The app

Drivechains → Merged mining, **Bidding**, for every node: the bid now and its reason, the unanchored run with a gauge
of p, the day's spending and what is left of the cap, the bid wallet's balance and the blocks it covers, the missed
blocks, the warnings and the last bids. The form: the five strategies (a radio) and "Prevent stalling" (greyed out
with Off); the amount of the strategy chosen only (the fee share for Fees and Fees + minimum, the minimum for Fees +
minimum and Minimum, the fixed amount for Fixed); the maximum per bid and the daily cap; "Paid from your mainchain
wallet"; the two triggers, "Mine blocks when fees pay" (merged mining's address, Start and Stop: `setbmm`) and
"Anchor my validator" (validators); the rest under Advanced. "One block by hand" below it. A change goes through
`setanchorbidpolicy`, which the desktop confirms (it spends the mainchain wallet), the strategy always sent with
`preventstall`; `getanchorbidpolicy` is a read of the page.

## The incident this answers

eCash's mempool congested (median 0.136 sat/vB, ~1 sat/vB estimated, blocks 1.5–4.5
hours apart): a bid of 25 sat (~0.13 sat/vB) missed 8 mainchain blocks and the chain
halted at the unanchored limit. With Prevent stalling, from p = 0.5 a bid
of 4 x the competitive base (never below 2 sat/vB on eCash) lands whatever the strategy, within 0.001 coins.

`feature_fastblocks_anchorpolicy.py` tests it (Fees + minimum, the legacy curve): a mainchain whose miners take
nothing below 50 sat/vB, the minimum and its rebids left out until p crosses 0.5, then the escalated bid lands and
the chain goes on past where it would have halted. It also tests the old names, the new form, and an 8.3.5
`settings.json` read by 8.3.6.
