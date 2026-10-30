# The interfaces of a Flush node

What you get by running `flushd` (or `flush-app`) yourself: its JSON-RPC (what `flush-cli` speaks), the public DEX API
(REST/JSON, gRPC, gRPC-Web and Connect on one port), Bitcoin Core's REST interface, and P2P. Nothing here needs
flush.markets: [flush.markets](https://flush.markets) is one site that reads a Flush node's public API.

The method lists below come from the binary itself (`flush-cli help`, then `flush-cli help <method>` for each) of
`flushd` 8.2.0 on this branch; the gRPC services from the `.proto` files in `src/grpc/proto`. On another release, run
`flush-cli help` for its own list; `flush-cli getopenrpcinfo` returns an OpenRPC document of every method.

How to use Flush, task by task: [using-flush.md](using-flush.md).

## Interfaces at a glance

| Interface | Turned on by | Port | Auth | Reaches the wallet |
|---|---|---|---|---|
| JSON-RPC | `-server=1` (`flushd` serves it by default; `flush-app` with `-server`) | `-rpcport`: 8254 `ecash`, 38254 `ecashbeta`, 48254 `ecashtest`; 38994 on the betanet slot-99 chain | cookie file, or `-rpcauth` / `-rpcuser` and `-rpcpassword` | yes: `/wallet/<name>` |
| Public DEX API | `-publicapi=<[addr:]port>` (off by default) | the one you give | none: per-client rate limits, CORS by `-publicapiorigin` | no |
| REST (Bitcoin Core's) | `-rest` (off by default) | the RPC port, under `/rest/` | none | no |
| P2P | `-listen` (on by default) | `-port`: 8255 `ecash`, 38255 `ecashbeta`, 48255 `ecashtest`; 38995 on the betanet slot-99 chain | none | no |

All of them bind to localhost by default except P2P. The JSON-RPC and REST share Bitcoin Core's HTTP server
([JSON-RPC-interface.md](JSON-RPC-interface.md), [REST-interface.md](REST-interface.md)). The public API has a server
of its own ([public-api.md](public-api.md)).

The node's link to eCash is a client, not a server: `-mainchaingrpc=host:port` (the bip300301_enforcer's gRPC, or
Backbone's), or Backbone's JSON-RPC (`-mainchainrpcport`, default 8532). See the README, "Running it".

## JSON-RPC

```sh
flushd -daemon                         # mainnet; RPC on 127.0.0.1:8254, cookie in ~/.flush/.cookie
flush-cli getblockchaininfo
flush-cli -rpcwallet=mywallet getbalance
flush-cli -named swapasset asset_in=ECX amount_in=1 asset_out=GOLD slippage=1
flush-cli help swapasset               # arguments, result, examples
```

Over HTTP, with the cookie (`__cookie__:<password>`, in the data folder; `betanet/` or `testnet4/` below it for those
networks):

```sh
curl -s -u "$(cat ~/.flush/.cookie)" -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getdexinfo","params":[]}' http://127.0.0.1:8254/
curl -s -u "$(cat ~/.flush/.cookie)" -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"swapasset","params":{"asset_in":"ECX","amount_in":1,"asset_out":"GOLD"}}' \
  http://127.0.0.1:8254/wallet/mywallet
```

Options: `-rpcbind`, `-rpcallowip` (to listen beyond localhost), `-rpcauth`, `-rpccookiefile`, `-rpcwhitelist`
(which methods a user may call), `-rpcthreads`. The RPC grants full control of the node and its wallets: do not
expose it. Use the public API for anything outside the machine.

Conventions of Flush's methods:

- An asset is `"ECX"`, its ticker, `"0x"` and its id, or its number.
- Amounts are in whole units (ECX with 8 decimals; a token with its own decimals).
- A trade is an order of its block's batch. Methods that trade take a `slippage` in percent and an optional `min_out`
  (the least the caller showed: the wallet refuses to sign for less).
- Wallet methods (marked *wallet* below) need a loaded wallet, and `walletpassphrase` if it is encrypted. The others
  read the node only.

## The methods

Grouped by area. *node*: a read of the chain or a control of the node. *wallet*: signs and sends, or reads the
wallet. The first sentence of each method's help, with `{...}` for a JSON object of named options.

### Assets: the registry

| Method | | What it does |
|---|---|---|
| `getasset "asset" ( height )` | node | A registered asset: its supply, its data, and who controls it. |
| `getassethistory "asset"` | node | Every value each field of the data of an asset has had, with the transaction and height that set it. |
| `getassetlogo "asset"` | node | The logo of an asset: a PNG or JPEG of 256x256 pixels, at most 32768 bytes, as the chain has it. |
| `getbitassetsinfo` | node | How many assets, reservations, pools and auctions there are (counted once per block). |
| `getassetindexinfo` | node | The search index of the assets (searchassets, searchpools, searchauctions). |
| `listassets ( count skip "match" )` | node | The registered assets, in the order of registration. |
| `searchassets ( "query" {...} "sort" "cursor" limit )` | node | Search, filter and sort the assets, a page at a time, by the node's search index (getassetindexinfo): a page never reads the assets whole, however many there are. |
| `decodeassettx "tx"` | node | What a transaction does to the assets, as its marker says (for explorers). |
| `getverifications "asset" ["curator",...]` | node | Verified issuers (not a rule: what wallets show): what curators say of an asset. |
| `reserveasset "name"` | wallet | Reserve the name of an asset: the first of two steps to register it. |
| `registerasset "name" supply ( decimals {...} public "logo" "display_name" )` | wallet | Register an asset reserved with reserveasset, once the reservation is deep enough: create its initial supply and its control coin, which mints more and changes its data. |
| `releaseassetreservation "name"` | wallet | Give up a reservation of this wallet: for a name another registered first, or one no longer wanted. |
| `mintasset "asset" amount ( "address" )` | wallet | Mint more of an asset this wallet controls. |
| `fixassetsupply "asset" confirm` | wallet | Burn the control coin of an asset: nobody can mint more of it or change its data, ever. |
| `updateasset "asset" {...}` | wallet | Change the data of an asset this wallet controls. |
| `setassetdisplayname "asset" "display_name" ( {...} )` | wallet | Set the display name of an asset this wallet controls, or remove it (an empty one). |
| `setassetlogo "asset" "logo" ( {...} )` | wallet | Set the logo of an asset this wallet controls, or remove it (an empty logo). |
| `transferassetcontrol "asset" "address"` | wallet | Give the control coin of an asset to another address: its holder mints and changes the data. |
| `sendasset "address" "asset" amount` | wallet | Send an asset to an address. |
| `burnasset "asset" amount` | wallet | Destroy some of an asset this wallet holds: the supply goes down by as much. |
| `releaseasset "asset"` | wallet | Retire a dead asset: its supply fixed, every unit of it in pools nobody provides liquidity to. |
| `listmyassets` | wallet | What this wallet holds, in blocks: asset coins, control coins, reservations, pool shares and auction receipts. |
| `searchmyassets ( "query" {...} "sort" "cursor" limit )` | wallet | searchassets among the assets this wallet holds or controls, in blocks: the same arguments, filters, sorts, cursor and result, by the node's search index. |
| `listassetactivity ( count )` | wallet | The BitAssets transactions of this wallet, newest first. |
| `listassettransactions ( count skip "asset" )` | wallet | The asset transactions of this wallet, newest first, one entry per asset each moves. |
| `listholdingaddresses` | wallet | The addresses of this wallet that hold something in blocks: ECX or tokens (assets, control coins, pool shares, positions, orders, auction receipts). |
| `verifyasset "curator" "asset" ( verified )` | wallet | As a curator (an oracle of this wallet), verify an asset, or take a verification back. |

### Launches: the bonding curve

| Method | | What it does |
|---|---|---|
| `getlaunchstats` | node | Launches in all: how many, on their curves and graduated, the ECX traded and raised, and the rules of new launches. |
| `listlaunches ( {...} )` | node | The launches, filtered and sorted, a page at a time (next_cursor gives the next page of the same query). |
| `getlaunch "token"` | node | A launch (launchtoken): its asset, curve, progress to graduation, price, fees and creator. |
| `quotelaunch "token" amount ( "side" )` | node | What a buy (ECX in) or a sale (units of the asset in) on a launch's curve gives now, alone in its block. |
| `getlaunchlog from_height ( to_height )` | node | For indexers: what blocks did on launch curves, a row per block and launch: made, ECX and units of the asset in and out, fees, the curve after, graduated. |
| `launchtoken "ticker" ( {...} )` | wallet | Launch an asset on a bonding curve, from a reservation of its ticker (reserveasset, then this once the reservation is deep enough, as registerasset). |
| `curveasset "asset" amount ( {...} )` | wallet | Put part of an existing asset on a bonding curve, as a launch's (where launches with parameters apply: getlaunchstats' rules.params_active). |
| `buyfromlaunch "token" amount ( slippage min_out )` | wallet | Buy an asset from its launch curve with ECX, for at least the quote less the slippage: an order of the block's batch. |
| `selltolaunch "token" amount ( slippage min_out )` | wallet | Sell an asset back to its launch curve for ECX (the fee taken out of what comes out), for at least the quote less the slippage. |
| `claimlaunch "token"` | wallet | Pay a launch's creator what it is owed: its part of the curve's fees (ECX, by the coinbase to its address) and, once graduated, its allocation (to its address). |

### DEX: pools, swaps and routes

| Method | | What it does |
|---|---|---|
| `getdexinfo` | node | Flush's DEX: whether its rules apply to the next block, and its bounds (doc/layers.md, "The DEX"). |
| `listpools ( "asset" count skip )` | node | The pools, with what they hold and their prices, a page at a time (pools in getbitassetsinfo says how many). |
| `searchpools ( "query" {...} "sort" "cursor" limit )` | node | Search and sort the pools, a page at a time, by the node's search index. |
| `getpool "asset_a" ( "asset_b" )` | node | A pool: of two assets, the kind addliquidity makes (constant product, 0.3 %); or any pool by its id (a pool of another kind: createpool). |
| `previewpool "asset_a" amount_a "asset_b" amount_b ( {...} )` | node | What createpool would make, without making it. |
| `getswapquote "asset_in" amount "asset_out" ( {...} )` | node | What a trade gives now in every pool of a pair, of every kind, and through the best route over all pools. |
| `quoteswap "asset_in" amount "asset_out" ( exact_out )` | node | What a trade in a pool gives now: for an amount in, what comes out; or for an amount out, what has to go in. |
| `quoteroute "asset_in" amount "asset_out" ( max_hops count )` | node | The best routes through the pools from one asset to another, and what each gives alone (as the only order of a block). |
| `quotepoolliquidity "pool" ( {...} shares "only" )` | node | What putting liquidity into a pool (by id, any kind) or taking it out would give now, as the rules work it out. |
| `simulateclearing "pool" [{...},...]` | node | What a round of a pool's batch clearing would give these orders together now (doc/layers.md, "Pool kinds"): one price for all, the pool's net trade on its curve. |
| `getpoolcurve "pool" ( points )` | node | A pool's depth: what trades of growing size, each way, would get alone now, and the price after each (for a depth chart). |
| `getpoolcandles "id" ( "interval" from to )` | node | The candles (OHLCV) of a pool, a book or a launch's curve, from the market index (-dexindex). |
| `getpooltwap "pool" ( from_height seconds )` | node | The time-weighted average price of a pool over a window that ends at the tip's time: from the time of a block, or over the last seconds. |
| `createpool "asset_a" amount_a "asset_b" amount_b ( {...} slippage )` | wallet | Make a pool of two assets of a kind (curve, fee tier, amplification or weights) and put its first liquidity in, for shares of it (previewpool shows what it would make). |
| `addliquidity "asset_a" amount_a "asset_b" ( amount_b slippage sets_price )` | wallet | Put two assets in their pool, for shares of it; this makes the pool if there is none, at the price the amounts set. |
| `addpoolliquidity "pool" {...} ( slippage min_shares )` | wallet | Put liquidity into a pool by its id (any kind; createpool, getpool, listpools): any amounts of its assets, or one of them alone. |
| `removeliquidity "asset_a" "asset_b" ( percent slippage )` | wallet | Give back shares of a pool this wallet holds, for its two assets. |
| `removepoolliquidity "pool" ( percent "only" slippage shares {...} )` | wallet | Give back shares of a pool by its id (any kind), for both its assets, or for one of them alone (what the shares are worth of it, plus what their part of the other buys of it in the pool, its fee paid). |
| `swapasset "asset_in" amount_in "asset_out" ( slippage max_impact min_out )` | wallet | Trade in a pool: pay in an amount of one asset, get at least the quoted amount of another, less the slippage. |
| `swaproute ["asset",...] amount_in ( slippage min_out ["pool",...] )` | wallet | Trade through several pools in one transaction, from the first asset of a path to its last (quoteroute finds one), for at least the quote less the slippage. |
| `swappools "asset_in" amount_in ["pool",...] ( slippage min_out )` | wallet | Trade through named pools of any kind (getswapquote's or quoteroute's pools), from an asset, for at least the quote less the slippage. |

### DEX: concentrated liquidity (positions)

| Method | | What it does |
|---|---|---|
| `previewposition "pool" price_lower price_upper {...}` | node | What openposition would make in a concentrated pool: the range in ticks (to the pool's spacing), the liquidity the amounts give, and what goes in of each. |
| `getposition "position"` | node | A position of concentrated liquidity: its pool, range, liquidity, what it holds now, the fees it earned, whether it is in range. |
| `getpoolticks "pool" ( count )` | node | A concentrated pool's liquidity by price. |
| `openposition "pool" price_lower price_upper {...} ( slippage expected_price )` | wallet | Open a position of concentrated liquidity in a pool of that curve. |
| `changeposition "position" {...}` | wallet | Change a position of concentrated liquidity this wallet holds. |
| `listpositions` | wallet | The positions of concentrated liquidity this wallet holds: their pools, ranges, liquidity, whether the price is in range, what they hold and the fees they earned. |

### DEX: order books (limit orders)

| Method | | What it does |
|---|---|---|
| `getbook "asset_a" "asset_b" ( levels )` | node | The order book of two assets: its open orders on each side by price (a ladder), the best of each, and the spread. |
| `getorder "order"` | node | A limit order of a book (placeorder): what it sells and buys, its limit, what is left, what it got. |
| `placeorder "asset_sell" amount "asset_buy" price ( blocks )` | wallet | A limit order in the book of two assets: sell an amount of one for the other at a price at least, until a block. |
| `settleorder "order" ( cancel )` | wallet | Collect what a limit order of this wallet got; cancel it (or once it expired or filled): what is left of what it sells too, and the order is gone. |
| `listorders` | wallet | The limit orders this wallet holds (their tokens), with what is left and what they got. |

### DEX: Dutch auctions

| Method | | What it does |
|---|---|---|
| `listauctions ( include_closed count skip "asset" )` | node | The auctions, newest first, a page at a time (auctions and auctions_all in getbitassetsinfo say how many). |
| `searchauctions ( "query" {...} "sort" "cursor" limit )` | node | Search and sort the auctions, a page at a time, by the node's search index. |
| `getauction "auction"` | node | An auction. |
| `quotebid "auction" amount` | node | What a bid on an auction buys in the next block. |
| `createauction "asset" amount "quote" start_price end_price duration ( start_in )` | wallet | Sell an amount of an asset by Dutch auction. |
| `bidauction "auction" ( amount buy_all )` | wallet | Bid on an auction: pay an amount of what it sells for, get what that buys at the price of the block that takes the bid (at least what it buys in the next block: the price only falls). |
| `collectauction "auction"` | wallet | Close an auction this wallet holds the receipt of: take what it brought in and what is left of what it sold. |

### DEX: oracles and oracle contracts

| Method | | What it does |
|---|---|---|
| `listoracles ( "start" count )` | node | The oracles registered, by id. |
| `getoraclevalue "oracle" "event"` | node | What an oracle attested for an event, if it did. |
| `getoraclecontract "contract"` | node | An oracle contract: what it holds, its outcomes, and who gets it. |
| `registeroracle "name" ( "pubkey" "proof" )` | wallet | Register an oracle under a name, with the key it signs its attestations with. |
| `publishoracle "oracle" "event" value ( height "signature" )` | wallet | Publish an oracle's attestation of an event: its value, signed (BIP340) over the tagged hash of (oracle, event, value, height). |
| `createoraclecontract "oracle" "event" "asset" amount [{...},...] "refund_address" expiry` | wallet | Lock an amount of an asset (ECX included) until an oracle attests an event; then the first outcome whose range holds the value gets it all (none: the refund address). |
| `settleoraclecontract "contract"` | wallet | Pay an oracle contract out to whom the oracle's attestation (or the expiry) names; anyone may (this wallet pays the fee). |

### Peg and mainchain

| Method | | What it does |
|---|---|---|
| `getmainchaininfo` | node | Returns how this node follows the mainchain, of which this chain is a sidechain. |
| `syncmainchain ( allowdeepreorg )` | node | Ask the mainchain node for its new blocks now, instead of waiting for the next time this node does so by itself, and act on them. |
| `getpeginfo` | node | Returns what the mainchain node says of the peg of this sidechain: the bundles of withdrawals being voted on, the votes they need, the escrow, the sidechain's slot. |
| `listwithdrawals` | node | Returns the withdrawals that the mainchain has not paid yet. |
| `getwithdrawalbundle` | node | Returns the withdrawal bundle that the mainchain is voting on, if there is one, and otherwise the one that the next block could commit to, if any. |
| `checkpegbalance` | node | Compare the coins the chain holds against the mainchain's escrow (getsidechainstate's pegbalance) with what the mainchain held in escrow for this sidechain's slot after the mainchain block the chain acted on, up to (asked of the mainchain node). |
| `getsidechainstate` | node | Returns a hash of the whole state of this chain as a sidechain, as of the chain tip (withdrawals, the pending bundle, the payouts owed, and whatever this sidechain keeps: names, assets, markets, notes...), and how many entries each table has. |
| `getdepositaddress ( "address" )` | wallet | Returns a deposit address: what to give the wallet of the mainchain as the destination of a deposit to this sidechain. |
| `createwithdrawal "mainchainaddress" amount ( mainchainfee )` | wallet | Withdraw coins to the mainchain. |
| `refundwithdrawal "txid" vout` | wallet | Take back a withdrawal that this wallet made, as long as it is not in a withdrawal bundle. |

### Merged mining (BMM) and anchors

| Method | | What it does |
|---|---|---|
| `setbmm mine ( "address" always amount )` | node | Turn continuous blind merged mining on or off. |
| `getbmminfo` | node | Returns the state of continuous blind merged mining. |
| `requestbmmblock "address" amount` | node | Mine one block of this chain by hand: build it, and have the wallet of the mainchain node offer the miners of the mainchain a fee to commit to it in their next block. |
| `createbmmblock "address"` | node | Build the next block of this chain and keep it. |
| `getanchorinfo ( count )` | node | Returns FastBlocks' anchors: the newest ones on the active chain, the blocks they make anchored and final, the fee pot, and the tickets this node knows. |
| `getanchorticket "hash"` | node | Returns what a mainchain BMM commitment of this chain is: on FastBlocks a commitment is to an anchor ticket (h*), not to a block, so the block it anchors is found here. |
| `getanchorbidpolicy` | node | Returns the node's bid policy (both triggers: a validator's anchorer and the merged miner): its strategy (fees, feesminimum, minimum, fixed, off), preventstall, its limits (see doc/fastblocks-anchor-bids.md). |
| `setanchorbidpolicy ( {...} reset )` | node | Changes the node's bid policy, at once and without a restart (old strategy names accepted). |

### Staking and validators

| Method | | What it does |
|---|---|---|
| `getposinfo` | node | Returns the state of FastBlocks' blocks of stake: the validators, the slot and its leader, anchors, the fee pot, and this node's block producer. |
| `getstakinginfo ( since )` | node | Returns what this node does as a validator of FastBlocks: its keys, whether it makes blocks of stake, what it made, and its next slot. |
| `getvalidators ( epoch )` | node | Returns the validator set of an epoch of FastBlocks' blocks of stake, as the active chain fixes it, and how many slots each validator leads in it. |
| `getvalidator "pubkey"` | node | Returns what this node knows of a validator of FastBlocks: its bond, whether it is in the set of the current epoch, its stake, and the next slot it leads. |
| `getleaderschedule ( epoch )` | node | Returns the leader of each slot of an epoch of FastBlocks' blocks of stake. |
| `setstaking enabled` | node | Turns the making of blocks of stake by this node's validator keys (-validatorkeyfile) on or off. |
| `signbondpop "owner" ( "pubkey" )` | node | Signs, with a validator key of this node's block producer (-validatorkeyfile), the proof of possession that a bond to it owned by an address needs (createbond on the owner's wallet). |
| `listevidence` | node | Lists the proofs this node knows that a leader of FastBlocks signed two blocks for one slot (found by watching headers, or received). |
| `submitevidence "hex"` | node | Gives the node a proof that a leader of FastBlocks signed two blocks for one slot (two extended headers, serialized): it keeps it and relays it. |
| `createbond "validator_pubkey" amount ( "pop" "owner" )` | wallet | Bond coins of this wallet to a validator key of FastBlocks. |
| `createunbond "validator_pubkey" ( amount )` | wallet | Unbond coins this wallet bonded to a validator key of FastBlocks. |
| `listbonds ( all )` | wallet | Lists the bonds this wallet owns on FastBlocks (or every validator's, with all): what is bonded, what is unbonding and when it is paid. |

### The app (flush-app)

| Method | | What it does |
|---|---|---|
| `describeconfirmation "method" ( ["param",...] "wallet" )` | node | What a call that pays, signs or changes what the node does would do, as the user is to be asked about it, described by this chain's own describers (the questions of its app). |
| `webuievents ( since timeout )` | node | The events of the node for the pages of this chain (its tip, its wallets, their balances), after the number `since`. |
| `webuihost {...} ( "token" "passphrase" ["token",...] )` | node | One request of the pages of this chain, from the app that shows them (the Chains app). |
| `webuichosenfile "path" ( write overwrite )` | node | A file the user just chose in the native chooser of the app that shows this chain's pages. |

### The other methods

The rest are Bitcoin Core v32's (amounts in ECX), with the template's built-in CPU miner (`getgenerate`, `setgenerate`), by `flush-cli help` category. `flush-cli help <method>` describes each.

- **Blockchain** (38): `dumptxoutset`, `getbestblockhash`, `getblock`, `getblockchaininfo`, `getblockcount`, `getblockfilter`, `getblockfrompeer`, `getblockhash`, `getblockheader`, `getblockstats`, `getchainstates`, `getchaintips`, `getchaintxstats`, `getdeploymentinfo`, `getdescriptoractivity`, `getdifficulty`, `getmempoolancestors`, `getmempoolcluster`, `getmempooldescendants`, `getmempoolentry`, `getmempoolinfo`, `getrawmempool`, `gettxout`, `gettxoutproof`, `gettxoutsetinfo`, `gettxspendingprevout`, `importmempool`, `loadtxoutset`, `preciousblock`, `pruneblockchain`, `savemempool`, `scanblocks`, `scantxoutset`, `verifychain`, `verifytxoutproof`, `waitforblock`, `waitforblockheight`, `waitfornewblock`
- **Control** (8): `getmemoryinfo`, `getopenrpcinfo`, `getrpcinfo`, `help`, `logging`, `rpc.discover`, `stop`, `uptime`
- **Mining** (9): `getblocktemplate`, `getgenerate`, `getmininginfo`, `getnetworkhashps`, `getprioritisedtransactions`, `prioritisetransaction`, `setgenerate`, `submitblock`, `submitheader`
- **Network** (15): `addnode`, `clearbanned`, `disconnectnode`, `exportasmap`, `getaddednodeinfo`, `getaddrmaninfo`, `getconnectioncount`, `getnettotals`, `getnetworkinfo`, `getnodeaddresses`, `getpeerinfo`, `listbanned`, `ping`, `setban`, `setnetworkactive`
- **Rawtransactions** (21): `abortprivatebroadcast`, `analyzepsbt`, `combinepsbt`, `combinerawtransaction`, `converttopsbt`, `createpsbt`, `createrawtransaction`, `decodepsbt`, `decoderawtransaction`, `decodescript`, `descriptorprocesspsbt`, `finalizepsbt`, `fundrawtransaction`, `getprivatebroadcastinfo`, `getrawtransaction`, `joinpsbts`, `sendrawtransaction`, `signrawtransactionwithkey`, `submitpackage`, `testmempoolaccept`, `utxoupdatepsbt`
- **Signer** (1): `enumeratesigners`
- **Util** (8): `createmultisig`, `deriveaddresses`, `estimatesmartfee`, `getdescriptorinfo`, `getindexinfo`, `signmessagewithprivkey`, `validateaddress`, `verifymessage`
- **Wallet** (58): `abandontransaction`, `abortrescan`, `addhdkey`, `backupwallet`, `bumpfee`, `createwallet`, `createwalletdescriptor`, `derivehdkey`, `encryptwallet`, `exportwatchonlywallet`, `getaddressesbylabel`, `getaddressinfo`, `getbalance`, `getbalances`, `gethdkeys`, `getnewaddress`, `getrawchangeaddress`, `getreceivedbyaddress`, `getreceivedbylabel`, `gettransaction`, `getwalletinfo`, `importdescriptors`, `importprunedfunds`, `keypoolrefill`, `listaddressgroupings`, `listdescriptors`, `listlabels`, `listlockunspent`, `listreceivedbyaddress`, `listreceivedbylabel`, `listsinceblock`, `listtransactions`, `listunspent`, `listwalletdir`, `listwallets`, `loadwallet`, `lockunspent`, `migratewallet`, `psbtbumpfee`, `removeprunedfunds`, `rescanblockchain`, `restorewallet`, `send`, `sendall`, `sendmany`, `sendtoaddress`, `setlabel`, `setwalletflag`, `signmessage`, `signrawtransactionwithwallet`, `simulaterawtransaction`, `unloadwallet`, `walletcreatefundedpsbt`, `walletdisplayaddress`, `walletlock`, `walletpassphrase`, `walletpassphrasechange`, `walletprocesspsbt`

## Examples

**A swap, quoted then sent.** The quote shows every pool of the pair and the best route; the swap pays in 1 ECX and
takes at least the quote less 0.5 %.

```sh
flush-cli getswapquote ECX 1 GOLD
flush-cli swapasset ECX 1 GOLD 0.5
flush-cli -named getswapquote asset_in=GOLD amount=5 asset_out=ECX exact_out=true   # what 5 ECX out costs
```

**A launch, from the ticker to the creator's claim.**

```sh
flush-cli reserveasset MOON
flush-cli getbitassetsinfo                 # reveal_depth: blocks until the reservation can be used (6)
flush-cli launchtoken MOON '{"supply":21000000,"decimals":8,"fee":2,"fee_split":[{"to":"creator","bps":7500},{"to":"burn","bps":2500}]}'
flush-cli quotelaunch MOON 5
flush-cli buyfromlaunch MOON 5
flush-cli getlaunch MOON                   # progress, raised, fees paid and owed per recipient
flush-cli claimlaunch MOON
```

**The peg.**

```sh
flush-cli getdepositaddress                # give this to a BIP300 deposit on eCash
flush-cli getmainchaininfo                 # how the node follows eCash, and how far behind it is
flush-cli createwithdrawal "<eCash address>" 1.5 0.001
flush-cli getwithdrawalbundle
flush-cli getpeginfo
```

**For indexers.** `getlaunchlog from_height to_height` gives a row per block and launch; `decodeassettx` what a
transaction does to assets; `getpoolcandles` candles from the market index (needs `-dexindex`).

## The public DEX API (`-publicapi`)

The node serves its DEX to websites and apps: launches, pools, books, positions, trades, candles, holders, quotes,
unsigned actions for a wallet to sign, and the broadcast of signed transactions. It never reads or calls a wallet.
One service, `flush.dex.v1.DexService`, on one port, in two forms:

- **REST/JSON** under `/api/v1/…`, field names in `snake_case`, every field present;
- **gRPC** (HTTP/2), **gRPC-Web** and the **Connect protocol** (JSON with `lowerCamelCase` names, or binary; reads also
  as Connect's `GET`), for any client generated from the proto.

```sh
flushd -publicapi=49100                                   # 127.0.0.1:49100 and [::1]:49100
curl -s http://127.0.0.1:49100/api/v1/status
curl -s 'http://127.0.0.1:49100/api/v1/launches?filter=near&limit=10'
curl -s -H 'Content-Type: application/json' -d '{"id":"MOON"}' \
  http://127.0.0.1:49100/flush.dex.v1.DexService/GetLaunch                     # Connect, JSON
curl -s -G http://127.0.0.1:49100/flush.dex.v1.DexService/GetStats \
  --data-urlencode encoding=json --data-urlencode 'message={}'                 # Connect GET
```

To serve it to the public, put it behind a reverse proxy on the same machine and name the proxy, its header and the
origins that may call it:

```sh
flushd -publicapi=49100 -publicapiorigin=https://example.org \
       -publicapiproxy=127.0.0.1 -publicapiproxyheader=X-Real-IP
```

| Option | Default | |
|---|---|---|
| `-publicapi=<[addr:]port>` | off | a port alone binds 127.0.0.1 and ::1; another address needs `-publicapiallowip` |
| `-publicapiorigin=<origin>` | none | web pages of these origins may call it (CORS, no credentials); other origins get 403 |
| `-publicapihost=<name>` | none | `Host` names accepted beside IP literals and localhost |
| `-publicapiallowip=<subnet>` | none | clients beyond localhost |
| `-publicapiproxy=<subnet>`, `-publicapiproxyheader=<name>` | none | the proxy in front and the header it sets with the client's address |
| `-publicapirate=<n>`, `-publicapiburst=<n>` | 10, 40 | each client's budget: a call costs 1, a list 2, a quote or build 4 |
| `-publicapibroadcast` | 1 | `POST /api/v1/broadcast`: 6 a minute per client, 120 in all |
| `-publicapireflection` | 0 | gRPC server reflection |
| `-dexindex` | 1 with `-publicapi` | the market index (trades, candles, holders, outcomes); turning it on derives it from the blocks once |

flush.markets is one instance of this API: its nginx passes `/api/` on its own origin to a `flushd -publicapi` (today a
node of the betanet slot-99 chain). Any node can serve the same routes. Every route, its parameters and replies, the
limits, the security review and an nginx block: [public-api.md](public-api.md).

### REST routes (`/api/v1`)

| Area | Routes (GET unless marked) |
|---|---|
| The chain | `status`, `stats`, `fees` |
| Launches | `launches`, `launches/stats`, `launches/log`, `launches/{id}`, `launches/{id}/trades`, `launches/{id}/candles`, `launches/{id}/holders`, `launches/{id}/quote`, `launches/{id}/logo` |
| Assets | `assets`, `assets/{id}`, `assets/{id}/logo`, `tickers/{ticker}`, `verifications` |
| Pools | `pools`, `pools/{id}`, `pools/{asset}/{asset}`, `pools/{id}/trades`, `pools/{id}/candles`, `pools/{id}/ticks`, `positions/{id}`, `books/{base}[/{quote}]` |
| Addresses | `addresses/{address}/holdings`, `addresses/{address}/portfolio`, `addresses/portfolio` (GET or POST), `addresses/{address}/trades` |
| Trades | `trades/{txid}` |
| Quotes | `quote/swap`, `quote/deposit`, `quote/withdraw`, `quote/position` |
| Builds | `build/{buy,sell,swap,deposit,withdraw,launch,register,curve,pool,auction,order,position,reposition,settle,bid,collect,claim,pegin,pegout}` |
| Writes | POST `broadcast`, POST `logos`; GET `logos/{sha256}` |

A build returns the action unsigned: a `flush:` link for the Flush app ([flush-links.md](flush-links.md)) and the
same action as a canonical JSON payload with its hash, for a wallet extension. The node holds no key.

### gRPC services

On the public API's port, from [`src/grpc/proto`](../src/grpc/proto):

| Service | Methods |
|---|---|
| `flush.dex.v1.DexService` ([dex.proto](../src/grpc/proto/flush/dex/v1/dex.proto)) | below |
| `grpc.health.v1.Health` | `Check`, `Watch` |
| `grpc.reflection.v1.ServerReflection`, `grpc.reflection.v1alpha.ServerReflection` | `ServerReflectionInfo`, with `-publicapireflection` only |

`DexService`, by area, with the REST route each answers:

| Method | REST |
|---|---|
| `GetStatus`, `GetStats`, `GetFees` | `status`, `stats`, `fees` |
| `ListLaunches`, `GetLaunch`, `ListLaunchTrades`, `GetLaunchCandles`, `ListLaunchHolders`, `QuoteLaunch`, `GetLaunchLogo`, `GetLaunchStats`, `GetLaunchLog` | `launches…` |
| `ListPools`, `GetPool`, `ListPoolTrades`, `GetPoolCandles`, `ListPoolTicks`, `GetPosition`, `GetBook` | `pools…`, `positions/{id}`, `books/…` |
| `GetTicker`, `GetHoldings`, `GetPortfolio`, `GetVerifications`, `GetAssetLogo`, `ListAssets`, `GetAsset` | `tickers/…`, `addresses/…`, `verifications`, `assets…` |
| `QuoteSwap`, `QuoteLiquidity`, `QuotePosition`, `BuildAction` | `quote/…`, `build/…` |
| `Broadcast` | POST `broadcast` |
| `UploadLogo`, `GetUploadedLogo` (8.1.0) | POST `logos`, `logos/{sha256}` |
| `GetTrade`, `ListAddressTrades` (8.2.0) | `trades/{txid}`, `addresses/{address}/trades` |

Every method but `Broadcast` and `UploadLogo` is marked `NO_SIDE_EFFECTS` (so Connect's GET form works for it).
gRPC itself, with no generated client, through `protoc` and the test client built beside `flushd`:

```sh
P=src/grpc/proto
echo "" | build/bin/flush-grpc-client 127.0.0.1:49100 /flush.dex.v1.DexService/GetStatus \
  | sed -n 's/^MSG //p' | xxd -r -p | protoc -I$P --decode=flush.dex.v1.ChainStatus $P/flush/dex/v1/dex.proto
echo "" | build/bin/flush-grpc-client 127.0.0.1:49100 /grpc.health.v1.Health/Check     # MSG 0801: SERVING
```

(`flush-grpc-client` is built only where gRPC's C++ library is installed.)

## REST (Bitcoin Core's)

`-rest` serves Bitcoin Core's read-only REST interface on the RPC port, without authentication: blocks,
transactions, headers, block filters, the chain's info, the mempool, UTXOs, spent outputs.

```sh
flushd -rest
curl -s http://127.0.0.1:8254/rest/chaininfo.json
curl -s http://127.0.0.1:8254/rest/block/<hash>.json
```

It knows nothing of assets or the DEX: use the public API for those. Its routes: [REST-interface.md](REST-interface.md).
