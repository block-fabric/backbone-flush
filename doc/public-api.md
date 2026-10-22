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
- **24 hours** (volume, trades, change): from the hourly candles.
- **Treasury inflows**: what each asset's treasury entry gained in a block (from swaps' and launches' fees, and the
  security parts held in assets), newest first.

Tests: `dex_market_tests` (trades, candles, holders, pages, a reorganisation that restores the index byte for byte,
the index wiped and refused as incomplete), `interface_publicapi.py` (against the RPC truth, across a real reorg,
and a node that turns `-dexindex` on derives the same trades, candles and holders).

## REST: `/api/v1`

Conventions: ids are 64 lowercase hex digits; ECX has none (`null`); a launch is named by its ticker (any case) or its
id. Display amounts are numbers in whole units; exact amounts are `*_units` strings of integers. Lists:
`{ "items": […], "next_cursor": "…" | null, "total": n }`, `cursor` passed back. Errors:
`{ "error": { "code": "not_found", "message": "…" } }` with the Connect HTTP status (400, 403, 404, 409, 429, 501,
503). An unknown query parameter is refused (400). Reads carry `Cache-Control: public, max-age=5` (logos 300,
quotes 2, builds `no-store`) and an `ETag` (`If-None-Match` → 304).

| GET | Method | Returns |
|---|---|---|
| `status` | GetStatus | network, Flush tip, eCash tip, slot, escrow, launches today, `market_index` |
| `stats` | GetStats | launch counts, 24 h and total volume, fees (trade bps, split), the curve's parameters, pools, liquidity |
| `fees` | GetFees | fee tiers, kinds, the launch fee and its parts, the swap fee's parts (liquidity providers, security, treasury), the mempool's relay fee |
| `treasury` | GetTreasury | `gettreasury`: balances by asset (held by the chain, no key, no spending rule yet), inflows by block (the market index), the security parts paid |
| `launches?filter=all\|new\|near\|curve\|graduated&sort=newest\|oldest\|progress\|mcap\|volume\|trades&q=&cursor=&limit≤100` | ListLaunches | `Page<LaunchSummary>` |
| `launches/stats` | GetLaunchStats | getlaunchstats, and 24 h counts |
| `launches/log?from_height=&to_height=` | GetLaunchLog | getlaunchlog (≤ 1000 blocks) |
| `launches/{id}` | GetLaunch | `LaunchDetail`: description and links (safe links only, below), curve, fees owed, allocation, first buy |
| `launches/{id}/trades?before=&cursor=&limit≤100` | ListLaunchTrades | `Page<Trade>`, newest first |
| `launches/{id}/candles?interval=5m\|1h\|4h\|1d&from=&to=` | GetLaunchCandles | `{ items: Candle[] }`, oldest first, ≤ 500 |
| `launches/{id}/holders?cursor=&limit≤100` | ListLaunchHolders | `Page<Holder>`: the curve's (or pool's) tokens first, tagged, then addresses largest first |
| `launches/{id}/quote?side=buy\|sell&amount=&slippage_bps=` | QuoteLaunch | `quotelaunch` + exact units and the floor |
| `launches/{id}/logo`, `assets/{id}/logo` | GetLaunchLogo, GetAssetLogo | the image (PNG/JPEG after its magic bytes; `nosniff`) |
| `pools?sort=liquidity\|volume\|change\|newest&q=&kind=&fee_ppm=&ecx_only=&include_abandoned=&cursor=&limit≤200` | ListPools | the market index (`searchpools`) with the pool's kind and 24 h measures |
| `pools/{id}`, `pools/{asset}/{asset}` | GetPool | `getpool` + launch, locked, 24 h |
| `pools/{id}/trades`, `pools/{id}/candles` | ListPoolTrades, GetPoolCandles | as for launches |
| `pools/{id}/ticks?limit≤500` | ListPoolTicks | a concentrated pool's liquidity by tick (`getpoolticks`) |
| `positions/{id}` | GetPosition | a position (`getposition`) |
| `books/{base}[/{quote}]?depth≤100` | GetBook | the ladder (`getbook`), totals, spread |
| `tickers/{ticker}` | GetTicker | `{ ticker, available, reason }` (reservations are hidden: available can still lose a race) |
| `addresses/{address}/holdings` | GetHoldings | assets (value in ECX), LP shares, positions, orders of an address |
| `verifications?assets=A,B&curators=<id>,…` | GetVerifications | per **the client's own** curator list (1–20), 1–50 assets (`getverifications`) |
| `quote/swap?from=&to=&amount=&slippage_bps=&max_hops=` | QuoteSwap | every pool of the pair (books included: `getswapquote`) and routes (`quoteroute`), best first, the floor |
| `quote/deposit?pool=&amount_a=&amount_b=`, `quote/withdraw?pool=&shares=&only=a\|b` | QuoteLiquidity | `quotepoolliquidity`, floors |
| `quote/position?pool=&price_lower=&price_upper=&amount_a=&amount_b=` | QuotePosition | `previewposition` |
| `build/{buy\|sell\|swap\|deposit\|withdraw\|launch}?…` | BuildAction | the action, unsigned (below) |
| POST `broadcast` `{"hex": "…"}` | Broadcast | `{ txid }` |

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
  links with the app's own parser. Verbs the app takes today: `buy`, `sell`, `swap` (one pool), `launch`
  (`app_ready: true`). Built for wallet extensions and later apps, refused by today's app (it fails closed):
  `route` (several pools: `pools=<id>,<id>`), `deposit` (`pool`, `asset_a`, `asset_b`, `amount_a`, `amount_b`,
  `min_shares`), `withdraw` (`pool`, `asset_a`, `asset_b`, `shares`, `min_a`, `min_b`, `only`).
- **The payload** is the same action as canonical JSON: keys sorted, no spaces, every amount a decimal string,
  integers for `v`, `expires`, `slippage_bps`, `quote_height`; `format: "flush-action"`. Its hash is
  SHA-256(`"flush-action/v1\n"` ‖ payload bytes as given), hex; `check_code` its first 8 digits, which a wallet
  extension shows beside the action so that the user sees the page and the wallet speak of the same thing. It is an
  intent, not a transaction (PSBT-like signing would need the user's coins, which only the wallet knows): the wallet
  picks the coins, builds the transaction from the payload's fields, checks the floor against its own quote, signs.
- **Broadcast** takes a signed transaction (hex, ≤ 100,000 bytes), refuses one that burns coins outside an asset
  marker, and hands it to the node's own mempool and relay (`BroadcastTransaction`, the node's policy, the default
  maximum fee rate). Rate-limited per client and in all; off with `-publicapibroadcast=0`.

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
| 9 | Request floods, oversized requests and replies | server | Per-client token buckets (`-publicapirate`, `-publicapiburst`; 429 with Retry-After); requests ≤ 256 KiB; HTTP/2: 32 streams a connection, 1 MiB buffered a connection, 64 MiB in all; replies ≤ 4 MiB (else RESOURCE_EXHAUSTED, nothing sent) |
| 10 | Slow clients (slowloris, slow reads, Rapid Reset, MadeYouReset) | server | A request must be all in within 15 s of its first byte however slowly it trickles (HTTP/2: each stream from its own HEADERS); a connection with no running call that begins or completes no request for 30 s is closed with a GOAWAY (bytes alone, PINGs, are not activity); a client whose replies wait must take at least 4 KiB a second of them (or all that waited) over each 30 s, else it is closed, a byte now and then does not keep it open (in the socket, the calls or behind HTTP/2 flow control; a stream whose window stays closed 30 s is reset); above 1 MiB waiting for the socket nothing more is taken from the calls (backpressure), and a connection holding more than 32 MiB of replies in all is closed; past 100 streams reset before their call was over, by the client or by the server because of it (each counted once), closed with a GOAWAY (nghttp2's own reset rate limit too; an nghttp2 before 1.66, without its MadeYouReset fix, is warned about); past 10,000 requests no new one (GOAWAY), closed once its calls are over; 16 connections per address (IPv6 per /64, `-publicapiipv6prefix`; the named proxies excepted), 256 in all |
| 11 | Spoofed client address (rate-limit bypass) | server | The client is the socket's peer unless the operator names both the proxy (`-publicapiproxy`) and its header (`-publicapiproxyheader`); X-Forwarded-For: the right-most entry only. The proxy must set the header itself (nginx below) |
| 12 | Rate-limit bypass through a client stream | server | Each message of a client-streaming call (reflection) is charged by the guard again, at most 32 a stream; reflection off by default |
| 13 | Reorgs serving stale or wrong market data | market index | Written in the block's journaled overlay: undone exactly with the block (tested byte for byte); the per-tip cache is keyed by the tip's hash |
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
- `test/functional/interface_publicapi.py`: every endpoint against the RPC truth on a regtest chain (launches,
  trades, candles, holders, pools of every kind, books, positions, ticks, verifications, holdings, quotes, builds and
  the app's floors, broadcast), a reorganisation across a graduation, CORS, rate limits, slow clients, connection
  caps, Connect (JSON, GET, binary through protoc), gRPC through `flush-grpc-client` (gRPC's own library), and a
  node that turns `-dexindex` on deriving the same index. With `--site=<flush-web> --playwright=<package>` it also
  runs the website's end-to-end test (`scripts/e2e-live.mjs`) against this chain's API.
