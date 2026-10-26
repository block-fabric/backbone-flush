# Trading on Layers: a user guide

This guide is for people who trade, provide liquidity or launch assets on Layers' exchange, in the app (Trade) or
from the command line (`layers-cli`). The rules themselves, with their arithmetic, are in [layers.md](layers.md)
("The DEX" to "The market index"); the formulas, gathered, in [dex-math.md](dex-math.md). On the eCash chain the
coin is ECX where this guide says CHN; everything else is the same.

## How a trade happens: one price per block

Every trade on Layers is an *order* that waits for the end of its block. At the end of the block all orders through
the same pool (or book) trade together at **one price**: the first and the last order of a block in a pool get the
same price, so nobody can buy just before you and sell just after you in the same block (no sandwiching), and the
order of transactions in a block does not matter.

- You set the least you accept (`min_out`, the app's *slippage*). If the block's price would give you less, your
  order does not trade and **everything you paid in comes back** (the coin by the block's coinbase). You lose only
  the transaction's fee.
- What a quote shows is what your order gets **alone**. Others trading the same way in the same block move the
  price for everyone; others trading the other way improve it for everyone.
- An order's result can be spent from the next block on.

## Swapping

App: **Trade → Swap**. Pick what you pay and what you get; the app finds the best way: one pool, or a route through
up to four pools, of any kind, and order books. *Ways to trade* lists the others with what each gives.

CLI: `getswapquote GOLD 10 CHN` (every pool of the pair and the best route), `swapasset GOLD 10 CHN` (the default
pool), `swaproute ...`, or `swappools GOLD 10 '["<pool>", "<pool>"]'` (exactly the pools you name).

## Pools and their kinds

A pair of assets can have several pools, one of each kind:

| Kind | Best for | What to know |
|---|---|---|
| Constant product (x·y = k) | any pair | the classic pool; 0.30 % is the default kind every wallet finds |
| Stable | two assets meant to be worth the same (two dollars) | very little slippage near 1:1; *amplification* A sets how flat (higher: flatter, but worse if the peg breaks) |
| Weighted | a pool that holds mostly one asset (80/20, 95/5…) | less loss when the heavy asset's price moves; deeper on one side |
| Concentrated | active providers who choose a price range | far deeper where the price trades; earns only while the price is in your range |
| Launch curve | assets on their launch curve (below) | made by a launch only; nobody adds liquidity to it |
| Order book | limit orders (below) | not a pool you provide to: orders at your price |

**Fee tiers**: 0.01 %, 0.05 %, 0.30 %, 1.00 % of what goes in. Of each fee, most stays with the pool's providers; a
part pays the chain's security (the miners who anchor it and the validators): on Flush 85 % providers / 15 % security
(on Layers 89 / 11), all taken in ECX in a pool of ECX. Of a fee paid in an asset of a pool of two assets the security
part stays in the pool: the providers keep it all.
Lower tiers suit stable pairs, higher ones volatile pairs. A pool may be made with a **dynamic fee**: its tier plus up to 2 %
more while its price moves a lot, back to its tier when it is quiet.

**Making a pool** (Trade → Create pool, or `createpool A amount_a B amount_b '{"curve":"stable","fee":0.05,"amp":200}'`):
the amounts set the opening price (for a stable pool, put in amounts of equal value). A pool keeps a tiny number of
its shares forever so it can never be emptied; a new pool needs a minimum size.

**Adding and removing liquidity** (Trade → Add liquidity, `addpoolliquidity pool '{"GOLD": 10}'`,
`removepoolliquidity pool 50`): you get *shares*, a token you can send; they are worth your part of the pool. You can
add one asset alone (the part that the pool would have to swap pays the pool's fee), and remove into one asset alone.

**Risks**: a pool always holds the asset that went down: when prices move, your shares are worth less than holding
the two assets would have been ("impermanent loss"); fees compensate only if there is enough trading. A stable pool
whose peg breaks fills up with the weaker asset.

## Concentrated liquidity: positions

App: open a concentrated pool (Trade → Pools) and use its **range picker**: pick the lowest and highest price you
want to provide between (presets such as ±1 %, or typed prices; the chart shows where the liquidity is), the amounts, and open a *position*.

- While the price is in your range, your liquidity trades and earns its share of the fees. Outside it, it does not
  trade, earns nothing, and is all in one asset (the one the price moved away from).
- A narrow range earns more per coin while in range, and leaves it sooner.
- A position is a token: it can be sent; whoever holds it manages it. **Trade → Positions** shows each (in range or
  not, what it holds, fees earned): *Collect* the fees, *Add*, *Remove* a part, or *Close* it.

CLI: `previewposition pool 0.98 1.02 '{"USDX":500,"USDY":500}'`, `openposition ...`, `listpositions`,
`changeposition <id> '{"collect":true}'`, `'{"close":true}'`.

## Limit orders: the order book

App: **Trade → Order book**. Pick two assets: the ladder shows who sells (asks) and who buys (bids) at which price.
Place an order: what you sell, how much, your price (the least you accept per unit), and for how long.

- Your order rests in the book and trades at the end of blocks: with other orders that cross yours, and with swaps
  that route through the book. Every trade in a block's round is at one price, **never worse than your limit**, often
  better. It can fill in parts.
- *My orders*: **Collect** what it got (it stays open for the rest), **Cancel** it (what is left comes back with what
  it got). Once it expires, collect it the same way to get everything back.
- A book holds at most 1000 orders per side. When a side is full, a new order can only take the place of the
  *worst-priced* order and only if it is at least as large; that order is marked **Evicted**: it trades no more, and
  settling it returns everything it holds. Orders have a minimum size (0.01 CHN worth on the CHN side, 0.00005 from `small_payout_height`; one whole unit
  otherwise).

CLI: `getbook GOLD CHN`, `placeorder GOLD 10 CHN 0.05 (blocks)`, `listorders`, `settleorder <order> (true to cancel)`.

## Creating an asset: register or launch

App: **Assets → Create an asset** (also *Register an asset* and *Launch*: the same wizard). First reserve a ticker (it
becomes yours to reveal a few blocks later, so nobody can copy it from the mempool), then: a display name, a logo, a
description with links; the supply and its decimals; and its market: **none** (registered to your wallet with its control
coin), **a bonding curve** (a launch, below), **a pool** or **a Dutch auction** (registered first, then the pool or the
auction made from it). flush.markets has the same wizard; it writes a `flush:` link that the app shows and runs.

- A launch sells **3/4 of its supply** on a *bonding curve* (a billion units of 6 decimals by default; from the chain's
  launch parameters any supply that is a multiple of 8 units, at least 10^6 units, and any decimals): the price rises as
  people buy, falls as they sell; the curve always holds the coin to buy back every unit it sold, and never more. The
  price rises nine-fold along the curve, from a value of about 222 CHN for the whole supply to 2000, whatever the supply.
- When buyers have paid **500 CHN** into the curve, the asset **graduates**: the raised coin and the last quarter of the
  supply become its ordinary pool with CHN, whose liquidity is **locked for good** (nobody holds its shares). Trading goes
  on there.
- Fees: of every trade on the curve, 1 % on Layers (80 % of it to the creator, 20 % to the chain's security) and 1.15 % on
  Flush by default, adjustable per launch from 0.3 % to 5 % with the launch parameters (80 % to the creator, 20 % to the
  chain's security). The creator claims theirs any time. The creator's optional allocation (at most
  5 %) is created only at graduation, and every launch shows it.
- **An existing asset on a curve**: its controller can put part of its supply on a curve (an asset's page → Open a market
  → Bonding curve; `curveasset`), if it was never launched and has no pool with CHN yet. Its control coin goes with it:
  the supply is fixed for good. The units it keeps trade in the pool once it graduates (the curve buys back only what it
  sold).
- Buying: Trade → Launches lists them (progress to graduation, price, market value, volume); a launch's page has its
  curve, a buy/sell panel and a quote. CLI: `listlaunches`, `getlaunch MOON`, `quotelaunch MOON 5`, `buyfromlaunch`,
  `selltolaunch`, `claimlaunch`, `launchtoken MOON '{"supply":21000000,"decimals":8,"fee":2}'`, `curveasset GOLD 800000`.
- Any asset's page also opens a pool (any kind and fee tier), an order-book listing or a Dutch auction.

**Risks**: most new assets lose their value. Anyone can launch anything; a display name or logo proves nothing (see
verified issuers below). Buys in the same block as yours get the same price as yours.

## Markets and verified issuers

**Trade → Markets** lists every pair: its price, change over about a day, volume and liquidity (counted in CHN, for
pairs with CHN), with search and sorting (`searchpools` on the command line).

**Verified issuers** are badges, not rules. A *curator* (anyone who registers an oracle) can verify an asset; your app
shows a badge only for curators **you** chose to trust (none by default). A badge means a curator you trust says the
asset is what it claims to be (its issuer is who it says); it says nothing of its price. Curators can take a
verification back. CLI: `getverifications GOLD`; a curator: `verifyasset <oracle> GOLD (false)`.

## Where the fees go

A swap fee goes 85 % to the pool's liquidity providers and 15 % to the chain's security (the miners who anchor it and
the validators, paid with each block's fees). A fee paid in an asset rather than ECX cannot pay them: its security part
stays in the pool, so on that side the providers keep the whole fee. A launch curve's fee goes 80 % to the creator and 20 % to the chain's security.

## Prices over time

Every pool keeps time-weighted average prices (TWAP) that contracts and tools can read (`getpooltwap pool`), and
oracle contracts can settle on attested values. A TWAP over a window resists a single block's manipulation.

## When something does not trade

- *Missed* (`min_out` not met, a pool gone, a book that could not fill a swap whole): everything you paid comes back,
  in the same block.
- *The block is full* (the DEX bounds the work of a block): the miner leaves your transaction for the next block.
- Wallet refusals say why (too little to open a pool, an order below the minimum, a book full with better orders…).
