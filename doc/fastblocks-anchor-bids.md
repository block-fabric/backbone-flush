# FastBlocks: the anchor bid policy

A validator's own anchorer bids, from the mainchain wallet, for a BMM commitment to
its ticket in every mainchain block (see "The anchorer" in [fastblocks.md](fastblocks.md)).
Blocks of stake halt after `max_unanchored_slots` (1920 on the eCash networks, 8
hours of 15-second slots) without an anchor, so a bid that miners leave out for
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
| Fees + prevent stalling (`feespreventstall`, the default) | Exactly the fee share it owes when there are fees, at least `floor`, and a stall boost only when needed. The minimum may be 0 (nothing bid while there are no fees and no stall pressure); 500 sat is the default. `-validatoranchorbidmode=standard` is this. |
| Off (`off`) | Nothing: the chain relies on other bidders, and halts without them. Logged as a warning, shown in the app. |

Whatever the strategy, the bid is at most `max`, and within the period's cap.

## The stall boost

The stall pressure is `p = (slot − firstunanchoredslot) / maxunanchoredslots`: 0
while anchors land, 1 at the halt. Below `escalationstart` (0.5) the bid is never
raised for the mempool, whatever it says. From there the bid is raised to

    base × multiplier(p)

if that is more, where the base is the mainchain's next-block fee estimate
(`estimatesmartfee 1`, or `estimatefee` on a Bitcoin ABC node) times the request's
size (`requestvsize`, 190 vB), or, without an estimate (an enforcer-only node, a
regtest mainchain, `usefeeestimate` false), the floor (500 sat at least, so that a
floor of 0 escalates too). `multiplier` follows the curve (default
`0.5:1,0.75:2,0.9:4`), linearly between its points (`interpolate`, or in steps),
its last value beyond. Once an anchor lands the run ends, p falls to 0, and the bid
is the strategy's own again.

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
| `floor` | `-validatoranchorbid` | 0.000005 (500 sat) |
| `max` | `-validatoranchorbidmax` | 0.001 (raised to the floor or the fixed bid if below them) |
| `fixed` | `-validatoranchorbidfixed` | the floor |
| `feeshare` | `-validatoranchorbidshare` | 99 |
| `escalationstart` | `-validatoranchorbidescalation` | 0.5 |
| `curve` | `-validatoranchorbidcurve` | `0.5:1,0.75:2,0.9:4` |
| `interpolate` | `-validatoranchorbidinterpolate` | true |
| `usefeeestimate` | `-validatoranchorbidfeeestimate` | true |
| `requestvsize` | | 190 |
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
