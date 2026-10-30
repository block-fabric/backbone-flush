# Using Flush

A task-by-task guide to Flush from the command line (`flush-cli`). The Flush app (`flush-app`) does the same through
its pages. Every command here is a wallet or node RPC of `flushd` 8.2.0; `flush-cli help <command>` gives its
arguments and defaults. What each interface of a node offers is in [node-api.md](node-api.md).

Values below are those of this branch (`main`, the mainnet code), read from the node's own `getlaunchstats`,
`getdexinfo`, `getbitassetsinfo` and `getposinfo`. Run them on your node to see the rules it applies.

## The networks

| | Slot on eCash | State | Branch |
|---|---|---|---|
| Flush mainnet (`-chain=ecash`, the default of `main`) | 46 | proposed, **not active yet**: the node refuses to start until a release names the block that activated slot 46 | `main` |
| Betanet test chain (`-chain=ecashbeta -sidechainslot=99`) | 99 | **live**, temporary: test coins, to be dropped when slot 46 activates | `flush-betanet` |
| `ecashbeta` slot 46, `ecashtest` slot 46 | 46 | defined in the code, not launched | `main` |

On mainnet every rule applies from the first block. The betanet chain in slot 99 took the same rules through upgrades
at heights (v2 to v8): what was made before an upgrade keeps the rules it was made under. For example, a launch made
there before v6 graduates at 500 ECX, one made after at 100 ECX. Its data folder is `~/.flush/betanet-photon`, its
ports P2P 38995 and RPC 38994, its deposit addresses `s99_…`, its minimum bond 0.1 ECX. See
[doc/slot99-testnet.md on the flush-betanet branch](https://github.com/block-fabric/backbone-flush/blob/flush-betanet/doc/slot99-testnet.md).

The examples use mainnet's slot (46). On the betanet chain, read 99 for 46 and add `-chain=ecashbeta
-sidechainslot=99` to each `flush-cli` call (or put both in `flush.conf`).

Facts that hold on every network:

- The coin is **ECX**: eCash's coin, deposited into Flush and withdrawn back. There is no other Flush coin.
- A block every **15 seconds** when validators produce blocks of stake. Otherwise merged mined blocks only, one per
  eCash block (about ten minutes).
- A trade is *soft* in a block of stake, *BMM-confirmed* once an eCash block commits to it, *final* after three
  anchors (about half an hour).
- Assets and pools are named by ticker, by `0x` and their id, or by number. `ECX` is the coin.

## 1. Get ECX in: the deposit

Flush holds no wrapped coin. A deposit is a BIP300 deposit on eCash into Flush's slot.

1. Get a deposit address from your Flush wallet. It names the slot and ends in a checksum, so an eCash wallet
   refuses it if it is mistyped or meant for another slot.

   ```sh
   flush-cli getdepositaddress          # s46_<address>_<checksum>; or getdepositaddress <your flx1... address>
   ```

2. On eCash, deposit to it:

   ```sh
   backbone-cli createsidechaindeposit 46 "s46_<address>_<checksum>" 10     # with Backbone
   ```

   With the bip300301_enforcer, use its `WalletService.CreateDepositTransaction`; BitWindow also makes deposits. A
   plain send to the escrow is not a deposit.

3. The ECX appears in Flush with the Flush block that includes the deposit, after it is mined on eCash.

```sh
flush-cli getbalance
flush-cli getpeginfo             # the escrow, the slot, bundles being voted on
```

## 2. Hold and send tokens

ECX is sent like any coin (`sendtoaddress`). Assets have their own commands:

```sh
flush-cli listmyassets                       # asset coins, control coins, reservations, pool shares, receipts
flush-cli sendasset "flx1q..." GOLD 12.5
flush-cli burnasset GOLD 1                   # the supply goes down by as much
flush-cli listassettransactions 20           # the wallet's asset moves, newest first
flush-cli getasset GOLD                      # anyone's view: supply, data, controller
flush-cli searchassets "gold"                # search the registry
```

Pool shares, positions and limit orders are tokens too: they can be sent, and whoever holds one manages it.

### Create a plain asset (no market)

Two steps, so that nobody can see the ticker in the mempool and take it first:

```sh
flush-cli reserveasset GOLD                  # hidden: commits to the name without showing it
# wait 6 blocks (reveal_depth in getbitassetsinfo)
flush-cli registerasset GOLD 1000000 2 '{"info":"One gram of gold"}'
```

A ticker is 2 to 12 letters A-Z and digits. The registration creates the supply and a **control coin**; its holder can
mint more (`mintasset`), change the data (`updateasset`, `setassetlogo`, `setassetdisplayname`), pass control on
(`transferassetcontrol`), or fix the supply for good (`fixassetsupply`). A logo is a PNG or JPEG of 256x256 pixels,
at most 32,768 bytes, without metadata.

## 3. Launch a token on a bonding curve

A launch sells a new token from a curve that the chain itself holds. Nobody holds its ECX.

```sh
flush-cli reserveasset MOON
# 6 blocks later:
flush-cli launchtoken MOON '{"display_name":"To the moon","info":"https://example.org","first_buy":1}'
flush-cli getlaunchstats                     # the rules for new launches (below)
```

**The supply.** By default a billion tokens with 6 decimals. A launch may choose its supply (a multiple of 8 units,
at least 1,000,000 units), its decimals (0 to 12) and its curve fee (0.3 % to 5 %). The supply is fixed: there is no
control coin, unless the launch sets `"keep_control":true`, in which case the creator can mint more and every page
marks the launch as mintable.

**The curve.** 3/4 of the supply is sold on the curve; the last 1/4 is kept for the pool. The curve is a constant
product over virtual reserves. The price rises nine-fold from the first unit to the last one sold. Buyers pay ECX
in; sellers get ECX back. The curve always holds the ECX to buy back every token it sold.

```sh
flush-cli quotelaunch MOON 5                 # what 5 ECX buys now, alone in its block
flush-cli buyfromlaunch MOON 5               # pay 5 ECX (fee included), default slippage 1 %
flush-cli quotelaunch MOON 1000000 sell
flush-cli selltolaunch MOON 1000000          # ECX comes back by the block's coinbase
flush-cli listlaunches '{"status":"curve","sort":"progress"}'
flush-cli getlaunch MOON                     # curve, progress, price, fees, creator, split
```

A buy that would take the curve past its end gets the last tokens and the rest of its ECX back.

**Graduation.** When buyers have paid **100 ECX** into the curve (`rules.graduation`), the launch graduates: the
raised ECX and the last quarter of the supply become a full-range position in a concentrated pool of the token with
ECX, at a 1 % fee. Nobody holds that position: the liquidity is locked for good. The pool starts at the curve's last
price. Trading goes on there.

**The creator's options** (all in `launchtoken`'s JSON, fixed in the launch transaction):

| Option | What it does |
|---|---|
| `supply`, `decimals`, `fee` | as above; `fee` in percent, 0.3 to 5, default 1.15 |
| `allocation` | up to 10 % of the supply, minted to the creator at graduation; shown on every launch |
| `first_buy`, `first_min_out` | ECX the launch spends on its own token in its own block, at that block's price |
| `min_buy`, `max_buy` | bounds on each buy, in ECX (0: none) |
| `fee_split` | who gets the tunable part of the fees (below) |
| `keep_control` | keep the control coin (mintable supply) |

`curveasset GOLD 800000` puts part of an existing asset on a curve instead, if it was never launched and has no pool
with ECX. Its control coin goes with it: the supply is then fixed.

### Fees and the fee split

| Where | Fee | Who gets it |
|---|---|---|
| A trade on a launch curve | the launch's fee, 1.15 % by default (0.3 % to 5 %) | 25 % the chain's security (BMM bidders and validators); 75 % the launch's split |
| The graduated, locked position | 1 % of each swap through it | 60 % the launch's split; 40 % the chain's security |
| A swap in any other pool | the pool's tier: 0.01, 0.05, 0.3 or 1 % | 85 % the pool's liquidity providers; 15 % the chain's security |

The 15 % security part of a swap fee is taken in ECX. A fee paid in an asset rather than ECX cannot pay the chain's
security: its security part stays in the pool, so on that side the providers keep the whole fee.

The **split** of a launch has 1 to 5 entries in basis points of the 75 %, summing to 10,000:

- `creator` (at most once): claimed with `claimlaunch` (anyone may send it; it pays only the creator);
- `burn` (at most once): the chain buys the token back in its own market each block and burns it;
- up to 3 addresses: paid by the chain itself in the coinbase once 0.001 ECX is owed, with no claim.

With no split, all of the 75 % goes to the creator.

```sh
flush-cli launchtoken MOON '{"fee_split":[{"to":"creator","bps":7500},{"to":"flx1q...","bps":2500}]}'
flush-cli claimlaunch MOON                   # the creator's part of the fees; its allocation once graduated
```

A launch made through flush.markets carries the site's share in its split: on betanet 2,500 bps of the 75 % to the
site's address; no address is set for mainnet yet. The app shows every recipient before you sign. A launch made
from the app directly has no site share.

Most new tokens lose their value. A name or logo proves nothing: check the asset id.

## 4. Trade

Every trade is an **order that waits for the end of its block**. All orders through the same pool or book in a block
trade at one price, so the order of transactions in a block does not matter and nobody can trade just before and
after you in the same block. You set the least you accept. If the block's price gives less, the order does not trade
and everything you paid comes back in the same block; you lose only the transaction fee. A quote shows what your
order gets alone.

### Swaps and routes

```sh
flush-cli getswapquote ECX 1 GOLD            # every pool of the pair, of every kind, and the best route
flush-cli swapasset ECX 1 GOLD               # the pair's default pool; default slippage 0.5 %, max impact 10 %
flush-cli quoteroute GOLD 10 SILVER          # the best routes through several pools
flush-cli swaproute '["GOLD","ECX","SILVER"]' 10
flush-cli swappools GOLD 10 '["<pool id>","<pool id>"]'    # exactly the pools you name
```

A route goes through at most 4 pools. ECX that a trade pays out comes in the coinbase of the block with the trade.
Like the trade, it is soft until an anchor covers that block.

### Pools

| Kind (`curve`) | For |
|---|---|
| `constant_product` | any pair; 0.3 % is the default pool every wallet and route finds |
| `stable` | two assets meant to be worth the same; `amp` 1 to 10,000 sets how flat |
| `weighted` | a pool mostly of one asset; `weight_a` one of 2, 5, 10, 20, 25, 40, 60, 75, 80, 90, 95, 98 |
| `concentrated` | liquidity between two prices (positions, below) |
| `launch` | made by a launch only; nobody adds to it |

Fee tiers: 0.01, 0.05, 0.3, 1 %. A pool may have a dynamic fee (`"dynamic_fee":true`).

```sh
flush-cli previewpool USDX 1000 USDY 1000 '{"curve":"stable","fee":0.05,"amp":200}'
flush-cli createpool  USDX 1000 USDY 1000 '{"curve":"stable","fee":0.05,"amp":200}'
flush-cli addpoolliquidity "<pool id>" '{"GOLD":10,"ECX":0.5}'    # or one asset alone
flush-cli removepoolliquidity "<pool id>" 50                     # half your shares
flush-cli removepoolliquidity "<pool id>" 100 ECX                # all of them, into ECX alone
flush-cli searchpools "gold"
flush-cli getpool GOLD ECX                                        # the default pool of a pair
```

The amounts of a new pool set its opening price. Shares are worth your part of the pool. When prices move, a pool
ends up holding more of the asset that went down; fees compensate only with enough trading.

### Positions (concentrated liquidity)

```sh
flush-cli previewposition "<pool id>" 0.98 1.02 '{"USDX":500,"USDY":500}'
flush-cli openposition    "<pool id>" 0.98 1.02 '{"USDX":500,"USDY":500}'
flush-cli listpositions
flush-cli changeposition "<position id>" '{"collect":true}'    # or {"remove":50}, {"add":{...}}, {"close":true}
```

Liquidity trades and earns fees only while the price is inside its range.

### Limit orders (order books)

```sh
flush-cli getbook GOLD ECX                         # asks, bids, spread
flush-cli placeorder GOLD 10 ECX 0.05              # sell 10 GOLD at 0.05 ECX each at least; default 40,320 blocks (about a week)
flush-cli listorders
flush-cli settleorder "<order id>"                 # collect what it got; it stays open
flush-cli settleorder "<order id>" true            # cancel: what is left comes back too
```

An order trades at the end of blocks, at the round's one price, never worse than its limit, in whole or in part. A
book side holds at most 1,000 orders; when full, a new order can take the place of the worst-priced one only if it is
at least as large, and that order is evicted (settling it returns everything). The ECX side of an order must be worth
at least 0.00005 ECX.

### Auctions and oracles

```sh
flush-cli createauction GOLD 100 ECX 50 10 1440    # sell 100 GOLD: the price of all of it falls from 50 to 10 ECX over 1440 blocks
flush-cli quotebid "<auction>" 10
flush-cli bidauction "<auction>" 10
flush-cli collectauction "<auction>"               # the seller, once it is over
```

Oracles attest events; an oracle contract locks an asset and pays it to the outcome whose range holds the attested
value (`registeroracle`, `publishoracle`, `createoraclecontract`, `settleoraclecontract`). A verified-issuer badge is
a curator's attestation (`verifyasset`, `getverifications`), not a rule: the app shows badges only from curators you
chose.

## 5. Withdraw: ECX back to eCash

```sh
flush-cli createwithdrawal "<eCash address>" 1.5            # mainchain fee 0.0001 ECX by default, on top
flush-cli createwithdrawal "<eCash address>" 1.5 0.001      # a higher fee is bundled first
flush-cli listwithdrawals                                   # not paid yet
flush-cli getwithdrawalbundle                               # the bundle being voted on
flush-cli refundwithdrawal "<txid>" <vout>                  # take it back while it is in no bundle
```

The coins leave the wallet at once. Withdrawals are paid in bundles that eCash miners vote on: more than 6,048 votes
within 12,096 blocks (at least about six weeks) on `ecash`, more than 13,150 in 26,300 on betanet; more than 5 in 10 on `ecashtest`. A bundle is handed to
the enforcer once the eCash block that committed to it is 144 blocks deep (12 on `ecashtest`).

## 6. Staking and validators

Blocks of stake are made by validators who bond ECX to a key. Values on mainnet (`getposinfo`, `getstakinginfo`):

| | |
|---|---|
| Slot | 15 seconds, an epoch 240 slots (an hour) |
| Minimum bond | 1 ECX (0.01 on `ecashtest`; 0.1 on the betanet slot-99 chain) |
| Validators | at least 4 active for blocks of stake (3 on the betanet slot-99 chain), at most 100 |
| A bond counts | in the sets fixed 48 hours ahead of their epoch: about two days after it is anchored |
| Unbonding | 1,008 anchors, about seven days (432 on `ecashtest`); a double-signing proof can slash it until paid |
| Without BMM | blocks of stake go on for up to 24 hours after the last block an anchor covers, then halt until a commitment lands ([ecash-sidechains.md](ecash-sidechains.md), "A mainchain gap") |

To run a validator:

```sh
# one 32-byte secret key in hex per line; yours and readable by you alone, or flushd refuses to start (8.3.6)
chmod 600 /path/to/keys.txt
flushd -validatorkeyfile=/path/to/keys.txt
flush-cli getstakinginfo                         # its keys, whether it produces, its next slot
flush-cli createbond "<validator x-only pubkey>" 10
flush-cli listbonds
flush-cli setstaking false                       # stop producing without a restart
flush-cli createunbond "<validator x-only pubkey>"            # all of it, paid back after the unbonding delay
```

If the bonding wallet is not on the validator's node, sign the proof of possession there and pass it:
`signbondpop "<owner address>"` on the validator's node, then `createbond <pubkey> <amount> "<pop>" "<owner address>"`
on the owner's wallet. Fees of a block of stake go to its producer and to the fee pot that BMM bidders win.
Details: [fastblocks.md](fastblocks.md), [fastblocks-anchor-bids.md](fastblocks-anchor-bids.md).

## 7. Merged mining (BMM)

eCash miners commit to Flush blocks for a fee in ECX, paid by the wallet of the mainchain node (Backbone's, or the
enforcer's). A node can bid for them:

```sh
flush-cli setbmm true "<flx1 address>"     # bid 99 % of the fees waiting whenever they pay for a block; the address gets the fees
flush-cli getbmminfo
flush-cli getanchorbidpolicy               # a validator's bids for its anchor tickets
```

## Where next

- [node-api.md](node-api.md): the JSON-RPC methods, the public API (REST, gRPC, Connect), REST and their ports.
- [dex-guide.md](dex-guide.md): more on pools, positions and books. It was written for Layers and still says CHN
  in places, and 500 for the graduation amount; Flush's values are the ones above.
- [bitassets.md](bitassets.md), [layers.md](layers.md), [dex-math.md](dex-math.md): the rules and their arithmetic.
- [flush-links.md](flush-links.md): the `flush:` links of flush.markets that the app opens.
- [ecash-sidechains.md](ecash-sidechains.md), [ecash-enforcer.md](ecash-enforcer.md): the peg on eCash.
