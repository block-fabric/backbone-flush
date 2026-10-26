# FastBlocks: the anchor bid policy

A validator's own anchorer bids, from the mainchain wallet, for a BMM commitment to
its ticket when fees, peg work, maintenance or stall pressure call for one (see "The anchorer" in [fastblocks.md](fastblocks.md)).
Blocks of stake halt after the configured unanchored limit (24 hours on upgraded
slot 99, formerly 8 hours) without an anchor, so a bid that miners leave out for
hours halts the chain. The bid policy says what the anchorer offers, and how it
raises its offer as the chain nears that halt. It is consensus-neutral: what a node
bids is its own business; the chain only sees which commitment landed.

Only validators bid (a node with `-validatorkeyfile`); on any other node the RPCs
and the app say so. A node's merged miner (`setbmm`), when on, bids instead, with
its own offer: the "Offer for each block" of Merged mining's Automatic mining is
`setbmm`'s, not the anchorer's.

## Strategies

| Strategy (`strategy`) | What it bids |
|---|---|
| Fixed (`fixed`) | `fixed` for every mainchain block, whatever the fees or the pressure. |
| Minimal: keep the chain moving (`minimal`) | `floor`; the stall boost and the rebids below. |
| Fees (`fees`) | The fee share the anchor owes (`feeshare` % of the pot it will pay, 99 % by default: the operator keeps 1 %), at least `floor`. No boost. `-validatoranchorbidmode=fees` is this at 100 %. |
| Fees + prevent stalling (`feespreventstall`, the default) | The higher of the estimated base and fee share, a 4x base boost near a stall, and no bid when there is nothing to anchor. `-validatoranchorbidmode=standard` is this. |
| Off (`off`) | Nothing: the chain relies on other bidders, and halts without them. Logged as a warning, shown in the app. |

Whatever the strategy, the bid is at most `max`, and within the period's cap.

## Automatic base and stall boost

For `feespreventstall` with `flatboost` (the default), the base is the largest of:

- the operator's `floor` (default 0);
- the mainchain relay minimum times `requestvsize`, rounded up to a satoshi;
- the next-block fee estimate rounded up to whole sat/vB, times `requestvsize` (default 189 vB).

The estimate is refreshed each mainchain block using `estimatesmartfee 1`, then
`estimatefee`, then `getmempoolinfo.mempoolminfee`. If the backend exposes none,
the relay minimum is used; if it cannot expose that either, 1 sat/vB is assumed.
`usefeeestimate=false` disables the estimate, retaining the relay minimum and floor.
For example, 1.41 sat/vB becomes 2 x 189 = 378 sat. The size is configurable:
188 vB would give 376 sat. It is a size allowance, not a measurement of the signed request.

The normal bid is `max(base, fee share)`: fees count toward the base, with the
validator covering the difference. At stall pressure
`p = (slot - firstunanchoredslot) / maxunanchoredslots` of `escalationstart` (0.5),
the bid becomes `max(boost x base, fee share)`; `boost` defaults to 4. Large fee
shares are never multiplied. `max` and the spending cap still apply.

The default strategy skips a bid with reason **"skipped: nothing to anchor"** when
there is no fee pot, no pending peg/staking work and pressure is below the threshold.
Pending work includes uncredited mainchain deposits, withdrawals/bundles, staking
payouts, and validator-set refresh once per epoch. A long record catch-up is handled
conservatively by bidding. An idle decision is checked again within the same mainchain
block: new work or stall pressure can start a bid without waiting for another block.
Fixed bidding remains available for operators who want a request every block.

With `flatboost=false`, the legacy floor/curve/rebid policy remains available.
Its escalation base uses the unrounded estimate times the size, or the floor
(at least 564 sat) without an estimate. The curve defaults to `0.5:1,0.75:2,0.9:4`.

The bid is made again for the same mainchain block whenever the policy bids a tenth
more than the request in the mempool (and 500 sat more): mainchain blocks may be
hours apart, and p rises in between. The new request replaces the old one.

## Missed blocks

A bid is settled when the mainchain block after the one it followed comes: landed
(its commitment is the bid's ticket), outbid (another commitment of this chain, or
the mainchain went another way) or missed (no commitment of this chain at all).

- Rebid (`minimal`, `feespreventstall`): after `rebidafter` (3) missed blocks in a
  row the bid is multiplied by `rebidstep` (2) for each such run, at most `rebidmax`
  (8) times. 0 turns it off.
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
  left, then nothing until the period frees some. 0: no cap. Counted since the node
  started.
- `lowbalanceblocks` (1008, about a week of eCash blocks): a warning when the bid
  wallet (the mainchain wallet the requests are paid from, `getbalances`) covers
  fewer mainchain blocks at the current bid.

## Defaults and options

The options are the defaults; `setanchorbidpolicy` changes the policy at once,
without a restart, and keeps the fields it changed in the node's settings
(`settings.json`, `anchorbidpolicy`), over the options; `reset` goes back to them.

| Field | Option | Default |
|---|---|---|
| `strategy` | `-validatoranchorbidstrategy` (or the older `-validatoranchorbidmode`) | `feespreventstall`; off on regtest unless an anchor option is given; `-novalidatoranchorbid` / `-validatoranchorbid=off`: `off` |
| `floor` | `-validatoranchorbid` | 0 (the relay/estimated base still applies) |
| `max` | `-validatoranchorbidmax` | 0.001 (raised to the floor or the fixed bid if below them) |
| `fixed` | `-validatoranchorbidfixed` | the floor |
| `feeshare` | `-validatoranchorbidshare` | 99 |
| `escalationstart` | `-validatoranchorbidescalation` | 0.5 |
| `curve` | `-validatoranchorbidcurve` | `0.5:1,0.75:2,0.9:4` |
| `interpolate` | `-validatoranchorbidinterpolate` | true |
| `usefeeestimate` | `-validatoranchorbidfeeestimate` | true |
| `requestvsize` | | 189 |
| `rebidafter`, `rebidstep`, `rebidmax` | `-validatoranchorbidrebid` (the first) | 3, 2, 8 |
| `warnmissed` | | 3 |
| `periodcap`, `periodhours` | `-validatoranchorbidperiodcap` (the first) | 0.01, 24 |
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
  returns as `getanchorbidpolicy`. `flush-cli -named setanchorbidpolicy floor=0.00001 max=0.0005`.
- `getanchorinfo`'s `bid`: `effective` (the bid now) and `kind`/`reason` (`fixed`,
  `floor`, `fees`, `rebid`, `escalation`, `max`, `periodcap`, `off`), `normal`,
  `pressure`, `multiplier`, `escalationstart`, `firstunanchoredslot`,
  `unanchoredslots`, `maxunanchoredslots`, `pot`, `feerate` (sat/vB) and
  `feesource`, `missed`, `balance` and `coverblocks`, `spentperiod`, `periodcap`,
  `periodhours`, `history` (the last bids: `mainheight`, `ticket`, `offer`, `kind`,
  `reason`, `pressure`, `status`, `cost`, `time`) and `warnings`.
- `getanchorticket <hash>`: what a mainchain commitment of this chain is: on
  FastBlocks the mainchain commits to a ticket (h*), and this says the block it
  anchors, its height and whether it is in the active chain.

Every bid is logged with its reason: `FastBlocks: anchor bid 0.0002 for mainchain
block 146 (stall pressure 0.60): escalation at p=0.60: x40.00 the floor (no fee
estimate)`.

## The app

Drivechains → Merged mining, "Anchor bids of this validator": the bid now and its
reason, the unanchored run with a gauge of p, the bid wallet's balance and the
blocks it covers, the spending of the period, the missed blocks, the warnings and
the last bids; a picker of the five strategies and an Advanced view with every
field. A change goes through `setanchorbidpolicy`, which the desktop confirms (it
spends the mainchain wallet); `getanchorbidpolicy` is a read of the page. A node
that is not a validator sees why it does not bid.

## The incident this answers

eCash's mempool congested (median 0.136 sat/vB, ~1 sat/vB estimated, blocks 1.5–4.5
hours apart): a bid of 25 sat (~0.13 sat/vB) missed 8 mainchain blocks and the chain
halted at the unanchored limit. With the defaults, the floor is 500 sat
(~2.6 sat/vB), the rebids double it after 3 missed blocks, and from p = 0.5 the bid
follows the mainchain's estimate (×2 at 0.75, ×4 at 0.9), within 0.001 coins.

`feature_fastblocks_anchorpolicy.py` tests it: a mainchain whose miners take nothing
below 50 sat/vB, the floor and its rebids left out until p crosses 0.5, then the
escalated bid lands and the chain goes on past where it would have halted.
