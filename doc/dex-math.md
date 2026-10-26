# The DEX's arithmetic

Every formula of Layers' exchange in one place, as the consensus code computes it. The rules around them (markers,
activation, what is refused, the store) are in [layers.md](layers.md); a user's view in [dex-guide.md](dex-guide.md).

## Conventions

- Amounts are integers in units (an asset's smallest unit; CHN in satoshis), at most `MAX_AMOUNT` = 2^63 − 1.
- A pool's assets are ordered by id: asset0 < asset1 (CHN, the null id, is always asset0). Prices are of asset0 in
  asset1 unless said otherwise.
- Fees are in millionths: F = 10^6, a tier f ∈ {100, 500, 3000, 10000}; the default pool's (AddLiquidity's) is 3000.
- ⌊·⌋ and ⌈·⌉ are of exact rationals. Every quantity of a curve is the floor or ceiling of an exact quantity, compared
  exactly (`curves::BigUint`, integers of any size): no floating point and no fixed point anywhere in consensus.
  Rounding is always toward the pool (what goes in up, what comes out down).
- *Bisection* (`SearchMax` / `SearchMin`): the largest (smallest) integer in a range satisfying a monotone predicate,
  each step one exact comparison; the result is unique, so any implementation agrees bit for bit.
- Determinism vectors: `src/test/data/dex_curves.json` (138), `dex_launch.json` (60), `dex_cl.json` (80), checked by
  the unit tests and by an independent Python reference (`contrib/dex-curves/reference.py`).

## Invariants and levels

| Kind | Invariant | A point (x, y) holds the level of (x0, y0) if |
|---|---|---|
| Constant product | x·y | x·y ≥ x0·y0 |
| Weighted wa:wb (lowest terms) | x^wa · y^wb | x^wa·y^wb ≥ x0^wa·y0^wb (for rationals x = a/p, y = b/q: a^wa·b^wb·p0 ≥ k·p^wa·q^wb) |
| Stable, amplification A | D: 4A(X+Y) + D = 4AD + D³/(4XY) | Φ(X, Y, Dc) ≥ 0 |

Stable: X = x·10^s0, Y = y·10^s1 (the asset with fewer decimals scaled to the other's), and
Φ(X, Y, D) = 16A·XY(X+Y) + 4XY·D − 16A·D·XY − D³, strictly decreasing in D, so D is its unique root; the pool's level
is Dc = ⌈D⌉ = the least integer with Φ(X0, Y0, Dc) ≤ 0.

**Spot price** (the slope; `curves::SpotPriceQ64`, UQ64.64 = ⌊price·2^64⌋):
constant product y/x; weighted y·wa/(x·wb); stable
(s0·Y·(4A(2X+Y) + Dc − 4A·Dc)) / (s1·X·(4A(X+2Y) + Dc − 4A·Dc)); concentrated ⌊sqrtP²·2^64/2^192⌋. The inverse price is
the floor of the inverse quotient (not the inverse of the floor).

## One round of a batch

A round is every hop of the block's orders through one venue in one pass. With orders paying a_i of asset0 and b_j of
asset1:

  Â = Σ a_i·(F − f),  B̂ = Σ b_j·(F − f),  A' = Â/F,  B' = B̂/F  (the inputs less the fee, exact rationals).

**Constant product** (`ClearPool`, any tier): P = (R1·F + B̂)/(R0·F + Â) of asset0 in asset1. An order paying a of
asset0 gets ⌊a·(F−f)·P/F⌋ of asset1; one paying b of asset1 gets ⌊b·(F−f)/(P·F)⌋ of asset0. At f = 3000 this is the
original DEX's formula bit for bit.

**Stable and weighted** (`ClearInvariant`): suppose asset0 comes in on balance. The pool pays dy of asset1 net; the
price is P = (dy + B')/A'. An asset0 seller gets ⌊a·(F−f)·(F·dy + B̂)/(Â·F)⌋, an asset1 seller
⌊b·(F−f)·Â/((F·dy + B̂)·F)⌋. The pool's worst point is (x0 + g(dy), y0 − dy) with g(dy) = A'·dy/(dy + B'); dy is the
largest integer in [0, y0 − 1] for which that point holds the level. As dy grows the point moves on a hyperbola
centred at (x0 + A', y0 + B'), concave in x, which meets the convex level curve once: the predicate is monotone. If no
dy > 0 holds, the same with the sides swapped; if neither, the sides cross at P = B'/A' and the pool trades nothing.
One order alone (B' = 0): dy is exactly the curve's output for its input.

**Proof sketch (conservation, the pool never loses).** Each output is the floor of its share at P, so a side's
outputs total at most its value at P: the pool pays out at most dy (+ the B' that came in), and keeps at least g(dy)
more asset0. The fees (inputs less A', B') stay. Reserves after = reserves before + all in − all out, exactly; the
level after ≥ the level before because the worst point holds it and every rounding and fee only adds. Property-tested
over every kind and the extremes.

**Concentrated** (`ClearCl`): the net trade walks the ticks (each crossing adds or removes the tick's liquidity and
turns its fee growth over; at most 64 crossings a round). dy is the largest integer with walk_out(⌊A'·dy/(dy + B')⌋)
≥ dy, by bisection; every order gets P = (dy + B')/A' (or the reverse), rounded down. The walk is a sequence of
`SwapStep`s:

- amount0 between prices a < b for liquidity L: L·2^96·(b − a)/(b·a) (up: ⌈⌈L·2^96·(b−a)/b⌉/a⌉; down: floors);
- amount1: L·(b − a)/2^96;
- next price after amount in: asset0 in, ⌈L·2^96·p/(L·2^96 + amount·p)⌉; asset1 in, p + ⌊amount·2^96/L⌋;
- `SqrtRatioAtTick` is Uniswap v3's (twenty constants 2^128·1.0001^(−2^(k−1)), each within one unit of exact),
  rounded up to Q64.96; `TickAtSqrtRatio` the largest tick at or below a price.

The round's fees and roundings are credited stretch by stretch, in proportion to the input each stretch took, to that
stretch's liquidity: fee_growth += ⌊fees·2^128/L⌋ (modulo 2^256). A position earns
L·(growth_inside − growth_inside_last)/2^128, growth_inside as Uniswap v3's (outside values flipped at each crossing).
A tick carries at most (2^128 − 1)/(number of ticks at the spacing) liquidity, so the in-range sum never overflows.

**Launch curve** (`ClearLaunch`): a constant product of virtual reserves (x, y). With the fee (1 % on Layers, 1.15 % on Flush by
default; a launch with parameters sets its own) taken in the coin (buyers: on the way in; sellers: on the way out, rounded up), a
round is ClearPool's formula on the fee-free amounts, unless it would sell past yE. Then it goes exactly to the end at the chord
price P = Δx/Δy with Δy = y − yE, Δx = ⌈x·y/yE⌉ − x; sellers get ⌊b·P⌋; direct buyers share Δy + B units pro rata, rounded
down, the units left given one each in order to those who can pay one more; each pays ⌈t·P⌉ plus the fee, the rest back.

Parameters, for a launch of supply S units (on the curve and in its pool), G = graduation (500 coins), V = G/2:

  Tv = 9S/8,  yE = 3S/8,  curve part C = Tv − yE = 3S/4,  pool part P = S/4,  k = V·Tv.

- **Graduation exact.** V·Tv = (G/2)(9S/8) = 9GS/16 = (3G/2)(3S/8) = (V + G)·yE: the curve reaches yE exactly when its coin
  reaches V + G, so selling C raises exactly G, for any S.
- **No jump at graduation.** The curve's last marginal price is (V + G)²/k = (9G²/4)/(9GS/16) = 4G/S; the pool it graduates
  into, (G, S/4) (plus the fees kept for it, and the units not sold), opens at G/(S/4) = 4G/S.
- **The same curve in coin for any S.** Prices per unit scale as 1/S: the start V/Tv = 4G/(9S), the end 4G/S (9×), so the
  value of the whole supply goes from 4G/9 (≈ 222 coins at G = 500) to 4G (2000) whatever S and its decimals are. Per whole
  unit of d decimals: multiply by 10^d. A first buy of b coins (fee-free b') gets Tv·b'/(V + b') units.
- **Exact parts.** S is a multiple of 8 (`LAUNCH_SUPPLY_MULTIPLE`), so 9S/8, 3S/8, 3S/4 and S/4 are integers and C = 3P.
- **Bounds (no overflow).** `MIN_LAUNCH_SUPPLY` = 10^6 units ≤ S ≤ `MAX_LAUNCH_SUPPLY` = 8·⌊(2^63 − 1)/9⌋: then Tv ≤ 2^63 − 1,
  so every reserve is an amount. The products the round takes are of a coin side ≤ 2·(V + G) + fees < 2^37 and a unit side
  < 2^63 (x·y, b·Δx, t·Δx: < 2^100), or of two unit amounts (the pro-rata share Δy·budget: < 2^126), all in 128 bits. The
  floor of 10^6 units bounds the rounding: a unit costs at most 4G/S ≤ 0.02 coin at the end. S + the allocation
  (⌊S·a/10^4⌋, a ≤ 500 basis points) < 2^63 − 1.
- **Fee.** f ∈ [3000, 50000] millionths (0.3 % to 5 %), `MIN_LAUNCH_FEE`..`MAX_LAUNCH_FEE`; its parts as the network's
  (Flush: creator 80 %, security 20 %; the roundings' remainder into the pool at graduation).
- **Never below the virtual coin** (`LaunchCurve::start` = Tv, launches from `launch_params_height`): the curve takes back no
  more units than it sold. A round that would leave y > Tv (so x < V: coin it never took in) clears its buys with the sales
  that fit, taken in the round's order while their units B stay within the room (y + B)·x ≤ (Tv − n)·(x + A) (A the buys'
  fee-free coin, n their count: the round then ends at y' ≤ (y + B)·x/(x + A) + n ≤ Tv, floors included), the others missing
  (0 out, their units back from the batch): one oversized sale holds no honest seller back, in one pass (O(n)) and one
  clearing; the batch's work budget bounds the orders of a block as ever; a round past the end that would do it does not clear. Since k never falls,
  y ≤ Tv gives x ≥ k/y ≥ V: the curve can only ever pay out coin buyers paid in. Units held outside the curve (an existing
  asset's other units) are alike to it, and bounded the same.
- **An existing asset on a curve** (`CurveAsset`): S is the amount its controller puts in (taken from its coins); the same
  curve and pool parts; its control coin goes with it (the supply is fixed for good: nobody mints more into the curve or the
  pool); the asset keeps its other units, which trade in the pool once it graduates (on the curve only up to what it sold,
  by the bound above); no allocation. Allowed for an asset never launched whose default pool with the coin does not exist
  (graduation makes that pool).
- A launch without parameters (and every launch before `launch_params_height`) has S = 10^15 (10^9 units of 6 decimals).

Property tests (`dex_launch_tests`): `curve_constants_any_supply` (the identities above, at the bounds and 500 random S),
`graduation_exact_any_supply_and_fee` (one buy of 2G: exactly C out for exactly G in, the fee, the refund, at three fees and
57 supplies), `rounds_conserve_any_supply` (300 runs of random rounds at random S and f: every round clears, coin and units
conserved, the product never falls, the end never passed), and a launch of 21 million of 8 decimals and an existing asset
traded to graduation on a state (every unit accounted for), its issuer dumping its retained units into the curve (they
miss), and `outside_sales_never_drain_a_curve` (300 runs of sales of up to the whole supply: x ≥ V, y ≤ Tv, conservation).

**Order book** (`ClearBook`, a call auction): candidates are the limits of the best 256 live orders of each side.
For a price P (asset1 per asset0): supply(P) = takers' asset0 + Σ asks with limit ≤ P; spend(P) = takers' asset1 + Σ bids
with limit (as asset1 per asset0) ≥ P. Chosen: the P maximising volume = min(supply·P, spend)/P, then the least
|supply·P − spend|, then the lowest P (all compared as exact rationals, O(n log n) with sorted prefix sums). The buy
side (takers first, all or nothing; then bids by limit, then id) gets ⌊paid/P⌋ each and pays ⌈got·P⌉; the sell side
delivers what the buy side got (takers whole or the round fails for them; then asks in order), each getting ⌊sold·P⌋.
Roundings stay in the book, which therefore always holds what its orders are owed.

## Passes and fallback

Round k of a pass takes every order's k-th hop (k = 1..4), venues in id order. Orders that miss (a gone venue, nothing
out, below `min_out`) are removed and the pass recomputed from the block's reserves, at most 8 passes, then the orders
execute sequentially, each alone. A round's result is cached across passes (same venue, reserves, orders). Work per
block is bounded: 200 units (stable/weighted hop or liquidity op 1; concentrated or book hop 2; first order of a block
in a book 2).

## Liquidity

- Opening shares: constant product isqrt(x·y); weighted the largest v with v^(wa+wb) ≤ x^wa·y^wb; stable D (in units of
  the asset with more decimals). `MIN_LIQUIDITY` kept for good.
- Deposit of any amounts: the excess e beyond the pool's proportions pays the fee on ⌈φ·e⌉ (φ = the other asset's
  weight; ½ for constant product and stable); the deposit gets the largest s for which the pool after holds its level
  scaled by (S + s)/S.
- Withdrawal: both sides ⌊share·reserve⌋; one side: its part plus what the other part buys in the pool after.
- Concentrated: liquidity for amounts = min(amount0·(a·b/2^96)/(b − a), amount1·2^96/(b − a)) in range (floors);
  amounts for liquidity rounded up going in, down coming out.

## Fees

- A swap fee's parts, per round and per asset: fee = ⌊in·f/F⌋; security = ⌊fee·s/F⌋, protocol = ⌊fee·p/F⌋ (s, p the
  network's `dex_fee_security_share`, `dex_fee_protocol_share`); providers = fee − security − protocol (the remainder
  theirs). Both parts leave the reserves after the round (a concentrated pool: out of what it kept beyond its walk,
  security first, at most that; its positions are credited the rest).
- A launch fee's parts: creator ⌊fee·c/F⌋, protocol ⌊fee·p/F⌋, security ⌊fee·s/F⌋, the remainder to the pool at
  graduation. Flush: fee 1.15 % (11 500), c = 50 %, p = 30 %, s = 20 %: of 1 ECX, 1 150 000 satoshis = 575 000 +
  345 000 + 230 000. Layers: 1 %, c = 80 %, s = 20 %, p = 0.
- The block's security parts in the chain's coin E join its ordinary fees F: the bidder gets ⌊(F + E)·bps/10^4⌋ (the
  fee curve: bps = 5000 at S ≤ 60 s, 5000 − ⌈500(S − 60)/60⌉ to 2000 at S ≥ 420 s), the validators the rest.
- Dynamic: fee(h) = f + min(⌊vol(h)/2⌋, 20000), vol updated in each block that changes the pool:
  step = min(⌊|p1 − p0|·10^6/p0⌋, 10^6) (p the UQ64.64 spot price), vol ← ⌊(decayed·7 + step)/8⌋, decayed =
  vol >> ⌊(gap − 1)/40⌋ for a gap of blocks since the last change.

## TWAP

price0_cumulative += ⌊spot·2^64⌋ · seconds (and price1 with the inverse), modulo 2^256, at the end of each block that
changes the pool, with the price the pool had at the block's start; an observation per such block, the last 1024 kept.
Average over a window: (c(t1) − c(t0)) / (t1 − t0) / 2^64, differences modulo 2^256.

## Bounds and costs measured

Worst stable/weighted round ≈ 0.4 ms; worst book round (256 + 256 orders) ≈ 0.8 ms; a block's clearing ≲ 0.7 s at
the worst. Weighted 98/2 comparisons handle numbers of about 6,000 bits exactly.
