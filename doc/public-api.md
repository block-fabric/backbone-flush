# The public DEX API (`flushd -publicapi`)

Flush's node serves its DEX to websites and apps itself: launches, pools of every kind, trades, candles, holders,
quotes, unsigned actions to sign elsewhere, and the broadcast of transactions signed elsewhere. One service
definition, `flush.dex.v1.DexService` ([`src/grpc/proto/flush/dex/v1/dex.proto`](../src/grpc/proto/flush/dex/v1/dex.proto)),
on one port, with two faces:

- **REST/JSON** under `/api/v1/…`: the messages printed with their own field names (`snake_case`), every field present
  (an unset optional or message is `null`, an empty list `[]`). This is what flush.markets reads (its `docs/api.md`).
- **gRPC** (HTTP/2), **gRPC-Web** and the **Connect protocol** (JSON with `lowerCamelCase` names, or binary; the reads
  also as Connect's `GET` form): for apps and other sites, with any gRPC or Connect client generated from the proto.

It never reads or calls a wallet: the methods are the node's own reads and quotes, called in process, and nothing
else is reachable. It is off by default.

```
flushd -publicapi=49100                                  # 127.0.0.1:49100 and [::1]:49100
flushd -publicapi=49100 -publicapiorigin=https://flush.markets \
       -publicapiproxy=127.0.0.1 -publicapiproxyheader=X-Real-IP   # behind nginx on the same machine
curl -s http://127.0.0.1:49100/api/v1/launches?filter=near | jq
curl -s -H 'Content-Type: application/json' -d '{"id":"MOON"}' http://127.0.0.1:49100/flush.dex.v1.DexService/GetLaunch
```

The code: `src/grpc/` (the server: Backbone's embedded gRPC server, nghttp2 and our own protobuf codec, with a public
face added), `src/publicapi/` (the service, the REST routes, the limits, the builds), `src/bitassets/market.{h,cpp}`
(the market index).

## Options

| Option | Default | |
|---|---|---|
| `-publicapi=<[addr:]port>` | off | a port alone binds 127.0.0.1 and ::1; another address needs `-publicapiallowip` |
| `-publicapiorigin=<origin>` | none | web pages of these origins may call it (CORS, never credentials); a request with any other `Origin` is refused (403) |
| `-publicapihost=<name>` | none | `Host` names accepted beside IP literals and `localhost` (DNS rebinding) |
| `-publicapiallowip=<subnet>` | none | clients beyond localhost |
| `-publicapiproxy=<subnet>`, `-publicapiproxyheader=<name>` | none | the proxy in front and the header it sets with the client's address (X-Forwarded-For: its right-most entry). Without both, the client is the socket's peer and forwarding headers are ignored |
| `-publicapirate=<n>`, `-publicapiburst=<n>` | 10, 40 | each client's budget: a call costs 1, a list 2, a quote or build 4 |
| `-publicapibroadcast` | 1 | `POST /api/v1/broadcast`: 6 a minute per client, 120 in all |
| `-publicapiipv6prefix=<bits>` | 64 | IPv6 clients counted by this prefix for the 16 connections an address may hold |
| `-publicapireflection` | 0 | gRPC server reflection (each message of its stream charged as a call) |
| `-dexindex` | 1 with `-publicapi` | the market index (below); turning it on derives the sidechain state again from the blocks once; turning it off erases it |

## The market index (`-dexindex`)

Derived data the state does not keep, written at the end of each block in the block's own overlay of the state
(`sidechain::DERIVED_TABLE`, sub-byte `X`): flushed with the state, **undone with it by a reorganisation** (the
block's undo data has its keys), left out of the state's hash, read by no rule.

- **Trades**: each order of the block's batch that filled (a swap, a route, a launch buy or sale, a launch's first
  buy), a row per pool it went through with what went in and came out of it, the block, its time, and who got what
  came out (the result output's script, or the ECX payout's); by pool, and by launch (its curve, then the pool it
  graduated into). The batch's per-order outcomes are noted while the block clears (`market::NoteFill`, from
  `State::ClearOrders`: bookkeeping only, no rule reads it) and taken by `market::ConnectBlock`. A book's resting
  orders crossing each other (not an order of the batch) make no trade row; they move the book's candles' volume.
- **Candles** (OHLCV) at 5 minutes, 1 hour, 4 hours, 1 day, per pool (price of `asset_b` in `asset_a`, whole units;
  ECX per token for a pool with ECX) and per launch across its graduation (ECX per token): each block that changes a
  pool closes the candle of its time at the pool's price after the block; the volume is what the block's trades
  moved of the first asset (ECX).
- **Holders**: what each script holds of each token (asset coins, liquidity shares, positions, orders), from the
  outputs that carry tokens: by token, largest first with how many hold it, and by script (an address's holdings).
- **Outcomes** (8.2.0, layout 2): what became of each order, by its transaction and by address, newest first.
  - A **swap** (an order of the batch: any pool kind, a route, a launch buy or sale, a swap through a book), filled
    or missed: the block and its time, its route (the path's assets, and per hop the pool, its curve as the batch found
    it, the launch token if the pool is a launch curve), what went in then what came out of each hop, `back` (ECX a
    launch buy got back past the curve's end), and how it was paid: ECX by the coinbase of that block to its
    `chn_to`, a token on its result output (txid:n); a miss its refund (what it paid in, `path[0]`, the same way). Its
    scripts are its `chn_to` and its result output's (both, when they differ). Protocol orders (buybacks) are not
    noted. A row of ~250–400 bytes per order (by its hops), and one or two address entries of ~70 bytes.
  - A **limit order** (PlaceOrder, its id `OrderId(txid)`): its book, side, assets, amount, limit (num/den), expiry,
    block and time, its owner (the script of the output its token went to, in the PlaceOrder's own transaction: who
    holds the token later is not followed); each block that filled some of it (`remaining` fell: sold, got), each
    SettleOrder that only collected (a "collected" event), how it ended: evicted (the block), cancelled (a cancel
    while it still traded), settled (a closing settle of an order filled, expired or evicted), with the settle's txid,
    block and what it paid (proceeds, left). Expired is the tip's to say (`expiry` < tip, something left, not
    closed). A row of ~420 bytes and an address entry per order, ~75 bytes per fill, ~200 per collect, ~70 per settle
    (its txid to the order's).
  - Placements and settles are noted as they apply (`market::NoteOrder`, `market::NoteSettle`, from `State::ApplyTx`,
    with the result outputs of what a settle pays in assets); fills and evictions are read in `market::ConnectBlock`
    from the block's journal of the orders' table (every order a book's round or a crossing changed), against the
    order's own row. No rule reads any of it.
- **24 hours** (volume, trades, change): from the hourly candles.

Tests: `dex_market_tests` (trades, candles, holders, pages, a reorganisation that restores the index byte for byte,
the index wiped and refused as incomplete; outcomes: a route filled, a swap missed and refunded, a launch buy with
`back`, limit orders filled by crossings, collected, settled, cancelled, expired, by address a page at a time, each
undone by a reorganisation), `interface_publicapi.py` (against the RPC truth, across a real reorg,
and a node that turns `-dexindex` on derives the same trades, candles and holders).

## REST: `/api/v1`

Conventions: ids are 64 lowercase hex digits; ECX has none (`null`); a launch is named by its ticker (any case) or its
id. Display amounts are numbers in whole units; exact amounts are `*_units` strings of integers. Lists:
`{ "items": […], "next_cursor": "…" | null, "total": n }`, `cursor` passed back. Errors:
`{ "error": { "code": "not_found", "message": "…" } }` with the Connect HTTP status (400, 403, 404, 409, 429, 501,
503); some (8.3.0: `psbt/launch`'s wait) add a `reason` and numbers to the error object. An unknown query parameter is refused (400). Reads carry `Cache-Control: public, max-age=5` (logos 300,
quotes 2, builds `no-store`, uploaded logos a year: `logos/{sha256}`) and an `ETag` (`If-None-Match` → 304).

| GET | Method | Returns |
|---|---|---|
| `status` | GetStatus | network, Flush tip, eCash tip, slot, escrow, launches today, `market_index`, `launch_params` (active, height, fee bounds and default in bps, supply bounds and multiple in units, decimals; `allocation_max_bps` and `allocation10_height`), `min_payout_sat` and `min_order_sat` (the least ECX paid out, and the least an order with ECX is worth on that side, in the next block: 5000 from `small_payout_height`, 50000 and 1000000 before), `small_payout_height` |
| `stats` | GetStats | launch counts, 24 h and total volume, fees (trade bps, split), the curve's parameters, pools, liquidity |
| `fees` | GetFees | fee tiers, kinds, the launch fee and its parts, the swap fee's parts in force (liquidity providers, security), the mempool's relay fee |
| `launches?filter=all\|new\|near\|curve\|graduated&sort=newest\|oldest\|progress\|mcap\|volume\|trades&q=&cursor=&limit≤100` | ListLaunches | `Page<LaunchSummary>` |
| `launches/stats` | GetLaunchStats | getlaunchstats, and 24 h counts |
| `launches/log?from_height=&to_height=` | GetLaunchLog | getlaunchlog (≤ 1000 blocks) |
| `launches/{id}` | GetLaunch | `LaunchDetail`: description and links (safe links only, below), curve, fees owed, allocation, first buy |
| `launches/{id}/trades?before=&cursor=&limit≤100` | ListLaunchTrades | `Page<Trade>`, newest first |
| `launches/{id}/candles?interval=5m\|1h\|4h\|1d&from=&to=` | GetLaunchCandles | `{ items: Candle[] }`, oldest first, ≤ 500 |
| `launches/{id}/holders?cursor=&limit≤100` | ListLaunchHolders | `Page<Holder>`: the curve's (or pool's) tokens first, tagged, then addresses largest first |
| `launches/{id}/quote?side=buy\|sell&amount=&slippage_bps=` | QuoteLaunch | `quotelaunch` + exact units and the floor |
| `assets?q=&sort=newest\|oldest\|name\|supply\|volume\|holders\|activity&market=all\|none\|curve\|pool\|book\|auction&cursor=&limit≤100` | ListAssets | the registry: `Page<AssetSummary>`, every asset (searchassets' index) with its markets: `market.status` the first of curve (with `progress`, 0–1), pool, book, auction, none; `kinds`, counts, price in ECX, 24 h volume; supply (and `_units`), decimals, holders, logo, `fixed_supply`, controller. The market filter is of the page (go on with `next_cursor`) |
| `assets/{id}` | GetAsset | `AssetDetail`: the summary, its whole description and safe links, its launch, its pools (at most 10, read whole), books (at most 10, by id with their order counts), auctions not collected (at most 20), `can_curve` {allowed, reason}: more of them through `pools/…`, `books/…` |
| `launches/{id}/logo`, `assets/{id}/logo` | GetLaunchLogo, GetAssetLogo | the image (PNG/JPEG after its magic bytes; `nosniff`). Every logo URL the API gives is `/api/v1/assets/{id}/logo?v=<the first 12 hex digits of the logo's SHA-256>` (7.0.1): a new logo is a new URL, so a cache may keep one for long; `v` is taken and ignored |
| `pools?sort=liquidity\|volume\|change\|newest&q=&kind=&fee_ppm=&ecx_only=&include_abandoned=&cursor=&limit≤200` | ListPools | the market index (`searchpools`) with the pool's kind and 24 h measures: by default every pool of every kind but the launch curves (`launches`; `kind=launch` lists them), those with ECX by liquidity first, then the others newest first |
| `pools/{id}`, `pools/{asset}/{asset}` | GetPool | `getpool` + launch, locked, 24 h |
| `pools/{id}/trades`, `pools/{id}/candles` | ListPoolTrades, GetPoolCandles | as for launches |
| `pools/{id}/ticks?limit≤500` | ListPoolTicks | a concentrated pool's liquidity by tick (`getpoolticks`) |
| `positions/{id}` | GetPosition | a position (`getposition`) |
| `books/{base}[/{quote}]?depth≤100` | GetBook | the ladder (`getbook`), totals, spread; each level's `price_exact` (the orders' limit, 18 significant digits) and `amount_units`; `best_ask_exact`, `best_bid_exact`. A book's candles: `pools/{book}/candles` |
| `tickers/{ticker}` | GetTicker | `{ ticker, available, reason }` (reservations are hidden: available can still lose a race) |
| `addresses/{address}/holdings` | GetHoldings | assets (value in ECX), LP shares, positions, orders of an address |
| `trades/{txid}` | GetTrade | 8.2.0: the outcome of an order (below): a swap's (`kind` swap, `status` filled or missed) or a limit order's (`kind` order, `status` open, filled, expired, evicted, cancelled or settled), by the order's own transaction; a SettleOrder's txid finds its order too (`settle_txid` is then the one asked of). 404 only when the txid is no order the index knows of (no such transaction, not an order, or not in the chain any more); 400 when not 64 hex digits, and without `-dexindex` (failed_precondition) |
| `addresses/{address}/trades?cursor=&limit≤100` | ListAddressTrades | 8.2.0: `Page<TradeOutcome>` of an address, newest first (the block the order was made in), misses included: the swaps whose `chn_to` or result output is the address's, the limit orders it owns; `total` all of them; a limit order lists its first 20 fills and collects (`fill_count`, `collect_count` say how many; `trades/{txid}` lists 1000). Cost 2 |
| `addresses/{address}/portfolio` | GetPortfolio | what an address holds and made: assets (value in ECX), shares of pools (share, what they are worth, the pool), positions (as `positions/{id}`), orders (status open, filled, expired, evicted), auctions it sells, launches it created (fees owed, allocation, what is claimable); at most 500 outputs and 100 of them read whole (cost 4) |
| `addresses/portfolio?addresses=a,b,…` (GET, 64 at most), or POST `addresses/portfolio` with `{"addresses": ["a", "b", …]}` (200 at most) | GetPortfolio | 7.0.1: several addresses at once (a wallet's, as `flush:connect` shares them): `addresses` (each once, in order), `address` (the first), `assets` summed over them (`amount`, `amount_units`, `value_ecx`, and `holders`: how many of them hold it), `liquidity`, `positions`, `orders`, `auctions` and `launches` of every one with its `owner`; `truncated` when an address had more than 500 holdings, more than 100 holdings other than assets were asked, or more than 5000 in all |
| `verifications?assets=A,B&curators=<id>,…` | GetVerifications | per **the client's own** curator list (1–20), 1–50 assets (`getverifications`) |
| `quote/swap?from=&to=&amount=&slippage_bps=&max_hops=` | QuoteSwap | every pool of the pair (books included: `getswapquote`) and routes (`quoteroute`), best first, the floor |
| `quote/deposit?pool=&amount_a=&amount_b=`, `quote/withdraw?pool=&shares=&only=a\|b` | QuoteLiquidity | `quotepoolliquidity`, floors |
| `quote/position?pool=&price_lower=&price_upper=&amount_a=&amount_b=` | QuotePosition | `previewposition` |
| `build/{buy\|sell\|swap\|deposit\|withdraw\|launch\|register\|curve\|pool\|auction\|order\|position\|reposition\|settle\|bid\|collect\|claim\|pegin\|pegout}?…` | BuildAction | the action, unsigned (below); `facts` (what to show beside it), `deposit_address` (pegin with an address) |
| POST `broadcast` `{"hex": "…"}` or (8.3.0) `{"psbt": "…", "max_burn_units": "…"}` | Broadcast | `{ txid }`; a PSBT whose inputs are all signed is finalized here (as `finalizepsbt`) and extracted, else 400 `the PSBT is not complete: input N not signed`; `max_burn_units` caps what an unspendable output holds (sendrawtransaction's `maxburnamount`): a launch's first buy, the `burn_units` of its build |
| POST `psbt/reserve`, `psbt/launch`, `psbt/register` | BuildPsbt | 8.3.0: a launch's two steps (and a registration's) as unsigned PSBTs for a wallet that signs what it is given (below). Cost 8, `no-store` |
| POST `logos` `{"base64": "…"}` | UploadLogo | 8.1.0: `{ "sha256": "<64 hex>", "url": "/api/v1/logos/<sha256>" }` (200; `Cache-Control: no-store`), the logo kept in memory by its SHA-256 for an hour from its last upload (uploaded again, an hour from then), at most 1024 logos (the one uploaded longest ago dropped first). Checked with the chain's logo rules (`bitassets::CheckLogo`: PNG or JPEG, 256×256, at most 32 KiB, no metadata): else 400 with the reason (`bad-ba-logo-…`). The `url` is relative: the site prefixes its own origin and gives it as a link's `logo_url` (doc/flush-links.md). Charged as a broadcast: 6 a minute per client (3 at once), 120 in all (429, Retry-After), and 4 units of the client's rate |
| `logos/{sha256}` | GetUploadedLogo | 8.1.0: the logo uploaded with that SHA-256 (any case), as the image (`image/png` or `image/jpeg`, the type its rules found), `Cache-Control: public, max-age=31536000, immutable` (its content is its hash's); 404 when none is kept (never uploaded, or more than an hour ago), 400 when not 64 hex digits |

**Orders' outcomes** (8.2.0; the node must be 8.2.0 or later, its market index at layout 2: a node upgraded derives it
again from the blocks once). A swap that went through two pools and filled (amounts in units; ECX's id is `""`):

```json
{ "txid": "9f2c…", "kind": "swap", "height": 51234, "time": 1800000000, "status": "filled",
  "address": "ecregtest:qq…", "script": "76a914…88ac", "settle_txid": null,
  "swap": { "path": ["USDX", "ECX", "MOON"], "path_ids": ["4be1…", "", "a07d…"],
            "hops": [ { "pool": "51c0…", "curve": "constant_product", "venue": "pool", "launch": null,
                        "asset_in": "USDX", "asset_out": "ECX", "amount_in_units": "10000", "amount_out_units": "4985006" },
                      { "pool": "e3a9…", "curve": "constant_product", "venue": "pool", "launch": null,
                        "asset_in": "ECX", "asset_out": "MOON", "amount_in_units": "4985006", "amount_out_units": "2453118790" } ],
            "amounts_units": ["10000", "4985006", "2453118790"], "amount_in_units": "10000", "back_units": "0",
            "payouts": [ { "what": "out", "asset": "MOON", "asset_id": "a07d…", "amount_units": "2453118790",
                           "amount": 2453.11879, "via": "output", "address": "", "outpoint": "9f2c…:0" } ] },
  "order": null }
```

A miss has `status` missed, `amounts_units` what it paid in only, the hops' amounts "0" and one payout
`{ "what": "refund", … }` (ECX `via` coinbase to its `chn_to`, else its result output). A launch buy past the curve's
end has `back_units` and a second payout `{ "what": "back", "asset": "ECX", "via": "coinbase", … }`. A limit order
(`addresses/{address}/trades` lists the same items):

```json
{ "txid": "1d77…", "kind": "order", "height": 51240, "time": 1800000090, "status": "settled",
  "address": "ecregtest:qp…", "script": "0014…", "settle_txid": null, "swap": null,
  "order": { "order_id": "c4f0…", "book": "7b19…", "side": "ask", "sell": "USDY", "sell_id": "5e2d…", "buy": "ECX", "buy_id": "",
             "amount_units": "10000", "price_num": "10000", "price_den": "1", "price": 0.01, "expiry": 91560,
             "remaining_units": "0", "got_units": "100000000", "evicted": 0, "owner_outpoint": "1d77…:0",
             "fills": [ { "height": 51240, "time": 1800000090, "sold_units": "4000", "got_units": "40000000" },
                        { "height": 51242, "time": 1800000120, "sold_units": "6000", "got_units": "60000000" } ],
             "fill_count": 2,
             "collected": [ { "txid": "88e1…", "height": 51241, "time": 1800000105, "cancel": false,
                              "payouts": [ { "what": "proceeds", "asset": "ECX", "asset_id": "", "amount_units": "40000000",
                                             "amount": 0.4, "via": "coinbase", "address": "ecregtest:qp…", "outpoint": null } ] } ],
             "collect_count": 1,
             "settle": { "txid": "02ab…", "height": 51243, "time": 1800000135, "cancel": false,
                         "payouts": [ { "what": "proceeds", "asset": "ECX", "asset_id": "", "amount_units": "60000000",
                                        "amount": 0.6, "via": "coinbase", "address": "ecregtest:qp…", "outpoint": null } ] } } }
```

`side`: ask when it sells the pair's `asset_b` (the token against ECX; of two assets, the larger id), bid when it
sells `asset_a`, as trades' sides. `price`: whole units of what it buys per whole unit it sells. A cancel's `settle`
pays `{ "what": "left", … }` (and proceeds, if any). Through `trades/{settle txid}` the same order comes back with
`settle_txid` set.

**One implementation of quotes.** Every quote is the node's own RPC method, called in process: `quotelaunch`,
`getswapquote`, `quoteroute`, `quotepoolliquidity`, `previewposition`, `getbook` — the methods the app's pages quote
with. Those methods now read a snapshot of the state shared by the readers of the same tip (`WithDexState`), taken
under `cs_main` and read without it, so that what a quote searches (routes, curves, ticks) never holds block
validation back, for the app as for the API.

**Creators' text** is data: names and descriptions plain text (control characters dropped); a line of a creator's
info is a link only if it passes the safe-link rule (`https://`, a host, no user or password, no space, quote, angle
bracket or control character, at most 512 bytes; `publicapi::SafeLink`, tested); anything else stays text.

## The write path: builds and broadcast

The server holds no key and signs nothing. A website asks for an **action**; the user's wallet signs it.

```
website ──GET /api/v1/launches/X/quote──▶ node (the chain's quote: amount out, floor = out less the slippage)
        ──GET /api/v1/build/buy?launch=X&amount=5&slippage_bps=100──▶ node
        ◀── { uri: "flush:buy?v=1&network=…&exp=…&launch=X&asset=<id>&ecx=5&min_tokens=…&slippage_bps=100",
              payload: "{…canonical JSON…}", payload_hash, check_code, expires, summary, app_ready, the quote }
        ──▶ the Flush app (the link, or its QR code on another device), or a wallet extension (the payload)
             the app quotes again from its own node, shows the action in its own dialog, signs only for the floor or more
        ──▶ the wallet broadcasts it itself, or the page sends the signed hex: POST /api/v1/broadcast
```

- **The URI** is exactly what the app's links take (`src/webui/links.{h,cpp}`, `doc/flush-links.md`): `v=1`, the
  network, `exp` (ten minutes, an hour for a launch), each verb's parameters and no other, amounts as decimals of at
  most 8 places in the asset's own places (the token's 6), the floors rounded down by the API (the app rounds them
  up: the same number), assets by id with their tickers. The webui test `publicapi_links_tests` reads the API's
  links with the app's own parser. Verbs the app takes today: `buy`, `sell`, `swap` (one pool), `launch` (with `supply`,
  `decimals`, `fee_bps`, `allocation_bps` from the launch parameters), `register` (ticker, name, description, links,
  logo_sha256, supply, decimals; launch and register take `logo_url` with `logo_sha256`, 8.1.0: an https address of a
  DNS name, not an IP address or localhost, at most 512 characters, as the app's `logo_url=`; it goes in the URI only,
  not in the payload, whose `logo_sha256` already binds the logo; a URI over the app's 4096 bytes is refused, 400), `curve` (asset, amount, fee_bps, min_buy, max_buy), `pool` (asset_a, asset_b, kind,
  fee_ppm, amount_a, amount_b, amp, weight_a), `auction` (base, amount, quote, start_price, end_price, duration, delay),
  `order` (sell, buy, amount, price, expiry_blocks), and the site's DEX: `route` (a swap through several pools:
  `pools=<id>,<id>`), `deposit` (`pool`, `asset_a`, `asset_b`, `amount_a`, `amount_b`, `min_shares`), `withdraw`
  (`pool`, `asset_a`, `asset_b`, `shares`, `min_a`, `min_b`, `only`), `position` (`pool`, `price_lower`, `price_upper`,
  `amount_a`, `amount_b`, `slippage_bps`), `reposition` (`position`, `action`, `percent`), `settle` (`order`, `cancel`),
  `bid` (`auction`, `amount`, `buy_all`), `collect` (`auction`), `claim` (`launch`), `pegin` (`amount`, `address`),
  `pegout` (`address`, `amount`, `fee`) (`app_ready: true` for all; an app of before refuses the newer verbs: it fails
  closed).
- **The payload** is the same action as canonical JSON: keys sorted, no spaces, every amount a decimal string,
  integers for `v`, `expires`, `slippage_bps`, `quote_height`; `format: "flush-action"`. Its hash is
  SHA-256(`"flush-action/v1\n"` ‖ payload bytes as given), hex; `check_code` its first 8 digits, which a wallet
  extension shows beside the action so that the user sees the page and the wallet speak of the same thing. It is an
  intent, not a transaction (PSBT-like signing would need the user's coins, which only the wallet knows): the wallet
  picks the coins, builds the transaction from the payload's fields, checks the floor against its own quote, signs.
- **Broadcast** takes a signed transaction (hex, ≤ 100,000 bytes), refuses one that burns coins outside an asset
  marker, and hands it to the node's own mempool and relay (`BroadcastTransaction`, the node's policy, the default
  maximum fee rate). Rate-limited per client and in all; off with `-publicapibroadcast=0`.

### Unsigned transactions: a launch from a wallet extension (8.3.0)

The ECX Wallet extension signs only a PSBT someone else built. For a launch, the node builds both of its
transactions as its own wallet's `reserveasset` and `launchtoken` would (and `registerasset`'s for a
registration): the same checks of the ticker and the options, the same operation, outputs and marker
(`src/bitassets/launch.{h,cpp}`, which the wallet's RPC methods call too), from the coins of **one address**, the one
the wallet gave the site (P2WPKH only: the only kind it signs). Everything comes back to that address: the reservation,
the launch's creator, its first buy's tokens (and ECX back), a kept control coin, a registration's supply and control
coin, the change. The site gets each PSBT signed by the wallet, then `POST broadcast {"psbt": …}`.

```
site ──POST psbt/reserve {address, ticker, nonce}──▶ node ◀── { psbt, reservation, commitment, confirmations_needed, … }
     ──wallet signs (partial signatures)──▶ POST broadcast {psbt} ──▶ { txid }
     ──POST psbt/launch?<build/launch's query> {address, ticker, nonce, reservation}──▶ node  (409 until deep enough: poll)
     ◀── { psbt, burn_units, … } ──wallet signs──▶ POST broadcast {psbt, max_burn_units}
```

**The request**, a JSON body and/or the query string (both fill the same `BuildPsbtRequest`; the route sets `verb`):

| Field | | |
|---|---|---|
| `address` | all | the wallet's P2WPKH address of this chain |
| `ticker` | all | the ticker (uppercased: `NewAssetName`); refused if taken (409 `already_exists`) or if the rules refuse it (`reserveasset`'s checks) |
| `nonce` | all | 32 bytes, 64 hex digits **as written** (the HMAC key's bytes in order, not reversed), chosen by the client and kept until the reveal. The same nonce for `psbt/reserve` and `psbt/launch` |
| `reservation` | launch, register | `txid:vout`, as `psbt/reserve` answered |
| `coins` | all | optional, at most 200 `txid:vout`: the address's coins to pay with, each checked (in the UTXO set, the address's, not spent in the mempool, with value, no token, mature). None: the node scans its UTXO set for the address's script (below) |
| `name`, `description`, `links`, `logo_sha256`, `logo_url`, `supply`, `decimals`, `fee_bps`, `allocation_bps`, `first_buy_ecx`, `slippage_bps`, `split`, `keep_control`, `expires_in` | launch | exactly `build/launch`'s, same names and units (the query string the site builds for `build/launch` is taken as it is). `logo_sha256` must be a logo uploaded here (`POST logos`, kept an hour): its bytes go into the launch |
| `min_tokens` | launch | optional: the first buy's floor in the token's units (the link's `min_tokens`); none: the quote less `slippage_bps`, as `build/launch` |
| `min_buy`, `max_buy` | launch | optional: launchtoken's bounds of a buy on the curve (ECX) |
| `name`, `description`, `links`, `logo_sha256`, `supply`, `decimals` | register | exactly `build/register`'s |

The launch's parameters go through `build/launch` itself (`Build`: every check and the payload the app's link would
carry), then into launchtoken's options as the app makes them from that link (`webui/linkhost.cpp`): `display_name`,
`info` (the description, then each link on a line), `logo`, `supply`/`decimals`, `fee`, `allocation`, `keep_control`,
`fee_split`, `first_buy`/`first_min_out`; then `bitassets::ParseLaunchOptions`, launchtoken's own parser.

**The reply** (`BuiltPsbt`):

```json
{ "verb": "reserve", "psbt": "cHNidP8BAH0CAAAAA…", "txid": "5c1e…", "fee": 0.0000302, "fee_units": "3020", "fee_rate": 20.0,
  "vsize": 151, "burn": 0, "burn_units": "0", "address": "ecregtest:qq…", "ticker": "MOON", "asset": "a07d…",
  "commitment": "9b41…", "reservation": "5c1e…:0", "reservation_vout": 0, "confirmations_needed": 2,
  "inputs": ["e3a9…:1"], "height": 51234, "summary": "Reserve MOON (…)", "facts": [] }
```

- `psbt`: base64, **version 0**; every input carries its previous transaction (`non_witness_utxo`, read from its block:
  no `-txindex` needed) and `witness_utxo`; no derivation, no unknown or proprietary field. Version 2, the tip's height
  as lock time, every input replaceable (`0xfffffffd`), as the wallet makes them.
- `txid`: signing does not change it (every input is P2WPKH).
- `asset`: the hash of the ticker, as every id of the API (`uint256` hex); `commitment` the same way:
  `HMAC-SHA256(key = nonce, SHA256(ticker) ‖ scriptPubKey(address))`, shown byte-reversed like the ids.
- `reservation_vout` is 0: the reservation, then the marker, then the change. A launch: its first buy's result output
  (if a first buy), its control coin (if kept), the marker (holding the first buy: `burn`), the change; its inputs the
  reservation first, then the coins. A registration: the control coin, the supply (if any), the marker, the change.
- `confirmations_needed` (reserve): the confirmations the reservation needs before `psbt/launch` takes it (the rules'
  `bitassets_reveal_depth`, at least 1).
- `burn_units`: what the marker holds (a launch's first buy, 0 otherwise): pass it as the broadcast's `max_burn_units`.
- launch and register: `summary` and `facts` are `build/launch`'s (`build/register`'s); `reservation` echoes the request.

Before handing it out the node runs the asset rules on the unsigned transaction at the next block (`State::CheckTx`,
the mempool's own check): a PSBT the rules would refuse is never built (400 with the reason).

**Waiting for the reservation.** `psbt/launch` (and `psbt/register`) before the reservation can be revealed answers
409 with a reason and the numbers, for the site to poll (every ~20 s) and show "waiting N of M blocks":

```json
{ "error": { "code": "aborted", "message": "the reservation has 1 of the 2 confirmations it needs", "reason": "reservation_not_deep",
             "confirmations": 1, "needed": 2 } }
```

The same with `confirmations: 0` while the reservation waits in the mempool, or is found neither there nor in a block
(not relayed here yet). Other refusals: a reservation not at the address, not a reservation, or whose commitment is
not this ticker's, nonce's and address's: 400; spent by a transaction in the mempool: 400; the ticker taken: 409
`already_exists`. There is no transaction-status endpoint: poll `psbt/launch` itself.

**Coins.** Only coins in a block (not spent in the mempool): a coin carrying a token is never spent as a payment (it
would need the marker to list it again). With no `coins`, the node scans its UTXO set for the address's script (as
`scantxoutset`: the coins database flushed first, one scan at a time, else 429 `busy`; the 200 largest kept), the
largest first until they pay the burn and the fee.

**Fee.** The rate the node's wallet would pay (`wallet/fees.cpp`, `GetMinimumFeeRate` with its defaults): the estimate
for 6 blocks (economical), else the wallet's fallback (20 sat/vB), at least the mempool's minimum, the relay's minimum
and 1 sat/vB; the size counted with 72-byte signatures. A change below the dust of the wallet's discard rate (10 sat/vB)
goes to the fee.

## Security

| # | Threat | Where | What stops it |
|---|---|---|---|
| 1 | Wallet data or methods reached through the API | service | Only `flush.dex.v1.DexService`, health (and reflection if asked) are registered; its handlers call a fixed list of node RPC methods (`NODE_METHODS`, checked at each call: none is a wallet's); no wallet is read. Functional: unknown procedures and paths answer 501/404 |
| 2 | Expensive queries (whole-table walks, huge pages) | lists | Pages bounded (launches 100, pools 200, trades/holders 100, ticks 500, candles 500, launch log 1000 blocks, verifications 50×20, holdings 500); the launch table is built once per tip from a snapshot; the pool lists ride the index's own budgets (`searchpools`: SCAN_BUDGET, `partial`) and at most 5 of its pages per call |
| 3 | Unbounded pagination, cursor forgery | cursors | Opaque cursors are index keys checked against the list's own prefix (`std::invalid_argument` → 400) or bounded offsets; limits enforced |
| 4 | Quote path cost (route search, CL tick walking, curve bisection) | quotes | The node's own bounds: `FindRoutes` 20,000 partial paths and 16 pools per asset, ≤ 4 hops (the public API asks ≤ 3, builds 1); `MAX_CL_CROSSINGS` 64 ticks; exact-out not exposed; quotes cost 4 units, at most 4 expensive calls at once (`Gate`), the same request at the same tip answered from the cache; computed on a snapshot, not under `cs_main` |
| 5 | Cache poisoning | cache | The key is the method and the request's canonical bytes only (no header enters a reply); entries are per tip (a new tip drops them); bounded in entries and bytes, LRU. CORS headers are added outside the cached body; `Vary: Origin` for proxies |
| 6 | CORS, CSRF, DNS rebinding | server | An `Origin` not in `-publicapiorigin` is refused (403) for every method, preflights included; allowed origins get `Access-Control-Allow-Origin` with that origin, `Vary: Origin`, never `Allow-Credentials` (no cookies or auth exist); `Host` must be an IP literal, localhost or `-publicapihost`; no state-changing GET (GET only for `NO_SIDE_EFFECTS` methods; broadcast is POST with a JSON or protocol content type) |
| 7 | Injection into JSON consumers | replies | JSON written by UniValue (escaped); creators' text plain, control characters dropped; links only after the safe-link rule; logos served only as PNG/JPEG after their magic bytes, `X-Content-Type-Options: nosniff`, `Content-Security-Policy: default-src 'none'` on every reply |
| 8 | Broadcast abuse | broadcast | 6 a minute per client and 120 in all (429, Retry-After); ≤ 100,000 bytes; standard size; burns only in asset markers; the node's mempool policy and maximum fee rate; can be turned off |
| 8b | Logo uploads as free hosting or a memory sink (8.1.0) | logos | Only what the chain's logo rules take (a 256×256 PNG/JPEG of at most 32 KiB, no metadata: no other content can be stored or served); at most 1024 kept (32 MiB at most), each for an hour; 6 uploads a minute per client and 120 in all, as broadcasts; served by hash only, as an image after its rules, `nosniff` and `default-src 'none'` |
| 9 | Request floods, oversized requests and replies | server | Per-client token buckets (`-publicapirate`, `-publicapiburst`; 429 with Retry-After); requests ≤ 256 KiB; HTTP/2: 32 streams a connection, 1 MiB buffered a connection, 64 MiB in all; replies ≤ 4 MiB (else RESOURCE_EXHAUSTED, nothing sent) |
| 10 | Slow clients (slowloris, slow reads, Rapid Reset, MadeYouReset) | server | A request must be all in within 15 s of its first byte however slowly it trickles (HTTP/2: each stream from its own HEADERS); a connection with no running call that begins or completes no request for 30 s is closed with a GOAWAY (bytes alone, PINGs, are not activity); a client whose replies wait must take at least 4 KiB a second of them (or all that waited) over each 30 s, else it is closed, a byte now and then does not keep it open (in the socket, the calls or behind HTTP/2 flow control; a stream whose window stays closed 30 s is reset); above 1 MiB waiting for the socket nothing more is taken from the calls (backpressure), and a connection holding more than 32 MiB of replies in all is closed; past 100 streams reset before their call was over, by the client or by the server because of it (each counted once), closed with a GOAWAY (nghttp2's own reset rate limit too; an nghttp2 before 1.66, without its MadeYouReset fix, is warned about); past 10,000 requests no new one (GOAWAY), closed once its calls are over; 16 connections per address (IPv6 per /64, `-publicapiipv6prefix`; the named proxies excepted), 256 in all |
| 11 | Spoofed client address (rate-limit bypass) | server | The client is the socket's peer unless the operator names both the proxy (`-publicapiproxy`) and its header (`-publicapiproxyheader`); X-Forwarded-For: the right-most entry only. The proxy must set the header itself (nginx below) |
| 12 | Rate-limit bypass through a client stream | server | Each message of a client-streaming call (reflection) is charged by the guard again, at most 32 a stream; reflection off by default |
| 13 | Reorgs serving stale or wrong market data | market index | Written in the block's journaled overlay: undone exactly with the block (tested byte for byte); the per-tip cache is keyed by the tip's hash |
| 14b | A PSBT that spends or pays what the user did not ask (8.3.0) | psbt | Inputs only the one address's coins (the client's list checked against the UTXO set and that script), everything back to that address; every input's previous transaction in the PSBT (the wallet reads the amounts from it, checked against the txid); the rules run on it before it is handed out; the broadcast's `max_burn_units`. The wallet shows and signs it; the node holds no key |
| 14c | UTXO scans as a cost (8.3.0) | psbt | Cost 8 of the client's rate, the gate of expensive calls, one scan at a time (429 busy), 200 coins kept; or the client names its coins |
| 14 | A build that misleads the signer | builds | Assets and pools by id with their tickers (the app checks both), the floor is the chain's quote less the slippage rounded down, `exp` bounded; the app quotes again and signs only for the floor or more; the payload hash binds what the page shows to what the extension signs |

Backbone's enforcer interface (`~/skydoge/chains-ecash`, `src/grpc/server.cpp`) shares the server: it has no idle or
request deadline (row 10) — local-only by default, so a follow-up there rather than an open hole; rows 11 and 12 do
not apply to it (it reads no forwarding header; its guard checks a token, the same for every message).

## Deployment

Behind nginx on the same machine (flush.markets `docs/deploy.md` has the whole server block):

```nginx
location /api/ {
    limit_except GET POST OPTIONS { deny all; }
    proxy_pass http://127.0.0.1:49100;          # flushd -publicapi=49100
    proxy_http_version 1.1;
    proxy_set_header Connection "";
    proxy_set_header X-Real-IP $remote_addr;    # overwrites what the client sent: -publicapiproxyheader=X-Real-IP
    proxy_set_header X-Forwarded-For "";
    client_max_body_size 256k;
    proxy_read_timeout 30s;
    proxy_cache flush_api;                      # 5 s, keyed by the URI (replies are per tip)
    proxy_cache_valid 200 5s;
    proxy_cache_lock on;
}
location ~ ^/flush\.dex\.v1\.DexService/ {      # Connect and gRPC-Web from browsers (gRPC itself: grpc_pass over HTTP/2)
    proxy_pass http://127.0.0.1:49100;
    proxy_set_header X-Real-IP $remote_addr;
}
```

and `flushd -publicapi=49100 -publicapiorigin=https://flush.markets -publicapiproxy=127.0.0.1 -publicapiproxyheader=X-Real-IP`.

## Tests

- `src/test/publicapi_tests.cpp`: builds (encoding, decimals, the hash), rate limits and the cache, REST routes
  and their refusals, creators' links, REST JSON printing, the server's public face over HTTP/1.1 and HTTP/2 (REST,
  Connect, gRPC framing, CORS, preflights, ETag, reply caps, the guard and Retry-After, the proxy header, streams
  charged per message, a slow request seen as receiving).
- `src/test/dex_market_tests.cpp`: the market index on a state of its own, and a reorganisation.
- `src/webui/test/publicapi_links_tests.cpp`: the API's links read by the app's own parser.
- `test/functional/interface_publicapi_psbt.py` (8.3.0): a launch and a registration from an address whose key only
  another wallet holds: both PSBTs built by the API, checked as the extension reads them (version 0, previous
  transactions, no other field, the address's coins only, everything back to it), the commitment computed in the
  test, signed without finalizing and broadcast as PSBTs (the node finalizes), the 409 wait with its confirmations,
  the refusals (ticker, address, nonce, logo, coins, an unsigned PSBT, `max_burn_units`), `getlaunch` and the
  tokens at the address; the wallet's own `launchtoken` alongside.
- `test/functional/interface_publicapi.py`: every endpoint against the RPC truth on a regtest chain (launches,
  trades, candles, holders, pools of every kind, books, positions, ticks, verifications, holdings, quotes, builds and
  the app's floors, broadcast), a reorganisation across a graduation, CORS, rate limits, slow clients, connection
  caps, Connect (JSON, GET, binary through protoc), gRPC through `flush-grpc-client` (gRPC's own library), and a
  node that turns `-dexindex` on deriving the same index. With `--site=<flush-web> --playwright=<package>` it also
  runs the website's end-to-end test (`scripts/e2e-live.mjs`) against this chain's API.
