# The links of flush.markets in the app

[flush.markets](https://flush.markets) never holds a key and signs nothing: it writes what it wants done as a
`flush:` link (or a launch file), and `flush-app` reads it, checks it against its own node, and asks the user in its
own dialog. The format is the site's `docs/uri.md` (repository flush-web); this page is the app's side of it.

| Part | Where |
|---|---|
| The strict parser of the links and of the launch files | `src/webui/links.{h,cpp}` |
| Resolution against the node, the questions, the calls | `src/webui/linkhost.{h,cpp}` |
| The preface of a question that comes from a link | `confirm::Preface` (`src/webui/confirm.h`) |
| A launch's two steps in one question | `reserveasset` with the second step `launchtoken` (`LaunchAfterReserve`, `src/webui/local.cpp`) |
| The first buy's floor on chain | `launchtoken`'s option `first_min_out` (the consensus field `LaunchToken::first_min_out`) |
| The scheme handler of the desktop | Windows `src/webui/urischeme_win.cpp`, Linux `src/webui/urischeme_xdg.cpp` |
| The page: hand a link or a launch file over, as it is | `app/src/lib/links.ts`, `lib/uri.ts` (`isActionUri`) |
| Tests | `src/webui/test/links_tests.cpp`; e2e `src/webui/test/e2e/links.js` |

## The links

```
flush:<address>?amount=…                         a payment (BIP 21): the page's, as before
flush:<verb>?v=1&network=<net>&exp=<time>&…      an action: connect, launch, buy, sell, swap, register, curve, pool, auction, order,
                                                 route, deposit, withdraw, position, reposition, settle, bid, collect, claim,
                                                 pegin, pegout
```

What the parser takes (anything else is refused with what is wrong, before anything is resolved or shown):

- the scheme in any case, then a verb of lowercase letters and `?`; one of the twenty-one verbs;
- `v=1`, `network` equal to the app's own network (`ecash`, `ecashbeta`, `ecashtest`; `regtest` for the tests), and
  `exp`, a Unix time not past (two minutes of clock difference forgiven) and at most a day ahead;
- each parameter of the verb once (`link` up to 4 times), **no other parameter** (`req-…` included);
- values of `A–Z a–z 0–9 - . _ ~` and `%XX` only (no `+`, no `#`, no space), percent-decoded to UTF-8 without
  controls (a line break only in `desc`), marks that turn or hide text (U+200B–U+200F, U+202A–U+202E,
  U+2060–U+2069, U+FEFF) or private planes; at most 4096 bytes in all;
- ids of exactly 64 lowercase hex digits; tickers of 2 to 12 `A–Z 0–9` (`ECX` only as a side of a swap); amounts as
  decimals of at most 8 places, no sign or exponent, more than 0, at most 15 whole digits; `slippage_bps` 0 to 5000;
- `connect`: `origin` exactly `https://flush.markets`, `callback` exactly `https://flush.markets/connect` (a constant
  list), a `nonce` of 16 to 64 `A–Z a–z 0–9 - _`;
- `launch`: a display name the chain's rules take (1 to 64 bytes), a description of at most 512 bytes, `https://`
  links of at most 512 characters with a plain host (no user name) and printable ASCII, the description and links at
  most 512 bytes together (the chain's info), `logo=sha256:<hex>`, a first buy with both `ecx` and `min_tokens`; and,
  from the chain's launch parameters, `supply` (whole units, digits only, at most 19), `decimals` (0–12, with a supply),
  `fee_bps` (30–500) and `allocation_bps` (0–1000 from `allocation10_height`, 0–500 before);
- `register`: `ticker`, `name`?, `desc`?, `link`*, `logo`?, `supply`, `decimals`; `curve`: `asset` (id), `ticker`, `amount`,
  `fee_bps`, `min_buy`?, `max_buy`?; `pool`: `asset_a`, `ticker_a`, `asset_b`, `ticker_b` (an id, or `ECX` for the coin in
  both, and only for it), `kind` (`constant`, `stable`, `weighted`), `fee_ppm` (a fee tier: 100, 500, 3000, 10000),
  `amount_a`, `amount_b`, `amp`? (stable only), `weight_a` (weighted only, required); `auction`: `base` (id),
  `base_ticker`, `amount`, `quote`, `quote_ticker`, `start_price`, `end_price` (of all of it, falling), `duration`
  (blocks), `delay`?; `order`: `sell`, `sell_ticker`, `buy`, `buy_ticker`, `amount`, `price` (bought per whole unit sold),
  `expiry_blocks`;
- the site's DEX: `route`: `pools` (2 to 4 ids, comma-separated, none twice), `from`, `to`, `amount`, `min_out`,
  `slippage_bps`?; `deposit`: `pool`, `asset_a`, `asset_b` (the pool's two tickers, either order), `amount_a`, `amount_b`,
  `min_shares` (units); `withdraw`: `pool`, `asset_a`, `asset_b`, `shares` (units), `min_a`, `min_b` (0 allowed), `only`?
  (`a` or `b`); `position`: `pool`, `asset_a`, `asset_b` (the pool's order), `price_lower`, `price_upper`, `amount_a`,
  `amount_b` (one may be 0), `slippage_bps` (binds: at most 1000); `reposition`: `position`, `action` (`collect`,
  `remove`, `close`), `percent` (1–100, remove), `slippage_bps` (binds: at most 1000); `settle`: `order`, `cancel` (0 or 1);
  `bid`: `auction`, `amount`, `buy_all`? (0 or 1); `collect`: `auction`; `claim`: `launch` (ticker), `asset` (id);
  `pegin`: `amount`; `pegout`: `address` (an eCash address: letters, digits and its prefix, at most 128), `amount`, `fee`.

A launch file (`*.flushlaunch.json`, at most 64 KB) is read as strictly: exactly the keys of version 1 (`supply`, `decimals`, `fee_bps`,
`allocation_bps`, `split` and `keep_control` optional), `network` required, amounts as text, its logo's bytes with the SHA-256 it says.
`split` (8.0.0) is a link's `split=`: a list of at most 5 `{"to": "creator" | "burn" | <address>, "bps": <whole number>}`, e.g.
`"split": [{"to": "creator", "bps": 7500}, {"to": "bflx1…", "bps": 2500}]`, shown in full and accepted or declined with the launch;
`keep_control` (8.0.0) is `true` or `false`, a link's `keep_control=1`. An app before 8.0.0 refuses a file with either key (an unknown
key): a launchpad that must reach those apps gives a link instead.

## What each verb does

Every action is resolved against the node and asked in the host's dialog window (`Window::Ask`: a window of the host,
in the look of the app — Launchpad in Flush — which the page can neither draw nor answer). The question starts with
where it comes from: **From a link: opened in your browser — check it** (or *pasted or dropped into the app*, or
**From a launch file: you opened it**), what it asks, until when the link is good, then the facts below, the describer's own rows
(wallet, network, command, every asset and pool resolved by id) and a warning: *anyone can make a link like this, and
any page can open one*. Nothing runs unless the user accepts there.

| Verb | Resolved | Runs |
|---|---|---|
| `buy` | The token by its id; the link's ticker must be its name; a launch, not graduated; `ecx` exactly in 8 places (at least the node's least payout: 0.0005 ECX, 0.00005 from `small_payout_height`); the floor in the token's places (rounded up); the curve's quote now must meet it. | `buyfromlaunch ["0x<id>", ecx, slippage, floor]` |
| `sell` | The same; `tokens` exactly in the token's places; the floor in ECX. | `selltolaunch ["0x<id>", tokens, slippage, floor]` |
| `swap` | The pool by its id; `from` and `to` must be its two assets, by their tickers; not abandoned; the amount exactly in its asset's places; the floor rounded up; the pool's quote now must meet it. | `swaproute [[from, to], amount, slippage, floor, [pool]]` |
| `launch` | The ticker free; launches active; the logo the link names (a file chooser of the host: refused if its SHA-256 differs, or the rules refuse it); the first buy's quote on a new curve must meet `min_tokens`. | One question for `reserveasset [ticker]` and `launchtoken [ticker, {display_name, info, logo, first_buy, first_min_out}]`; the launch is sent by the app once the reservation is deep enough (`confirm::Authorizations`: within an hour, for at most the fee shown). |
| `register` | The ticker free; the supply within what an asset can have at its decimals; the logo as a launch's. | One question for `reserveasset [ticker]` and `registerasset [ticker, supply, decimals, {info}, true, logo, display_name]`, the registration sent by the app once the reservation is deep enough (as a launch). |
| `curve` | Launch parameters active; the asset by id, its ticker the link's; never launched nor on a curve, no pool with ECX of the default kind; the amount exactly in its places, a curve's supply (a multiple of 8 units, at least 10^6). | `curveasset ["0x<id>", amount, {fee, min_buy, max_buy}]` (the control coin goes with it: the supply fixed for good). |
| `pool` | Both sides by id with their tickers (ECX only as itself); the amounts exactly in their places. | `createpool [a, amount_a, b, amount_b, {curve, fee, amp, weight_a}]` |
| `auction` | Both sides by id with their tickers; amounts and prices exactly in their places; the price falls. | `createauction [base, amount, quote, start_price, end_price, duration, delay]` |
| `order` | Both sides by id with their tickers; the amount exactly in its places. | `placeorder [sell, amount, buy, price, expiry_blocks]` |
| `route` | Each pool by id, not abandoned; the path from `from` through each pool to its other side must end in `to`; the amount exactly in its places; the quote hop by hop (each pool alone) must meet the floor. | `swaproute [[from, …, to], amount, slippage, floor, [pools]]` |
| `deposit` | The pool by id, its two tickers the link's (either order); the amounts exactly in their places; the shares the node quotes now (`quotepoolliquidity`) must meet `min_shares`. | `addpoolliquidity [pool, {a: amount_a, b: amount_b}, null, slippage]` (the slippage that makes its floor the link's) |
| `withdraw` | The same; what exactly `shares` take out now must meet both floors. | `removepoolliquidity [pool, 100, only, slippage, shares]` (`shares`: exactly the link's, of those the wallet holds) |
| `position` | The pool by id, its tickers in its own order; previewed by the node (`previewposition`): in range or not said. | `openposition [pool, lower, upper, amounts, null, slippage]` |
| `reposition` | The position read whole (`getposition`): its pool, its fees owed. | `changeposition [position, {collect} \| {remove, collect, slippage} \| {close, slippage}]` |
| `settle` | The order read whole (`getorder`). | `settleorder [order, cancel]` |
| `bid`, `collect` | The auction read whole (`getauction`): what it sells, for what, its price. | `bidauction [auction, amount, buy_all]`, `collectauction [auction]` |
| `claim` | The token by id with its ticker; a launch; what is owed read (`getlaunch`): fees, allocation. | `claimlaunch ["0x<id>"]` |
| `pegin` | The amount exactly in 8 places; the deposit address of the wallet's `flush.markets` address (`getdepositaddress`: never the link's). | The link's question (where from, the amount, the address credited, the deposit address), then the deposit's own question (`peg.deposit`: the eCash node's wallet pays `createsidechaindeposit`). |
| `pegout` | The address as text (the eCash node reads it), the amount and fee in 8 places; a warning that it takes long and that a payment on eCash cannot be undone. | `createwithdrawal [address, amount, fee]` |
| `connect` | The wallet's address of the label `flush.markets` (made once). | A question: the site, the address shared (whole), the wallet, where the answer goes. On **Share**: the system's browser opens `https://flush.markets/connect#address=…&nonce=…&network=…`. |

The floor binds: `slippage_bps` is shown only. The host gives the node the slippage that makes the order's own
minimum exactly the floor (`floor(quote × (10000 − s) / 10000) ≥ floor`), so the transaction never accepts less than
the link's floor, and the node refuses at signing time if the pool or curve moved below it. A launch's first buy has
its floor on chain (`first_min_out`; the rules refuse the order below it, its ECX back).

What an action of a link brings in (its tokens, the coin paid out by the coinbase, a launch's creator address) goes to
the wallet's `flush.markets` address, the one connect shares: the link host sets it for its one call, on that call's
thread alone (`ScopedLinkDestination`, rpc/request.h: thread-local, never set by any RPC), and the wallet uses it when the
call runs (after the user accepted), refusing the call if the address is not its own. The site, which knows that address
only, shows it in its portfolio. Without such an address (no connect yet), the wallet's own new addresses, as before.
(`setlinkdestination`, a hidden wallet method of regtest only, does the same for the functional tests.)

The wallet: the one the app shows (its preferences' `lastWallet`) if it is open, else the only open wallet; with none,
or several and none chosen, the link is refused ("choose one in the app first").

## Where links come in

- **The browser.** The scheme opens `flush-app <link>`. A second start of the app hands its link to the app that
  runs for the data directory (`SingleInstance`, a socket of this user), or, if none runs there, to the app of the
  network the link names, if that one runs (a browser starts the app on the default network). The host takes it
  (`App::DeliverUri`): an action link goes to the thread of the links (one at a time, at most 4 waiting), a payment
  to the page as before.
- **Pasted.** *Paste link…* (and *Open URI…*, Banking menu): the page hands an action link over to the host as it is
  (`links.open`); so does a link dropped on the window.
- **A launch file.** *Import launch file…* (Banking menu: the host's file chooser) or the file dropped on the window
  (its text to the host, `links.importLaunch`).

## The handler of the scheme

- **Windows**: on each start (unless `-webuiregisteruri=0`), `HKCU\Software\Classes\flush` opens with this
  program (`"…\flush-app.exe" "%1"`), unless another program that still exists is registered there; the installer
  (NSIS) writes the same for all users.
- **Linux**: on each start (the same option), `$XDG_DATA_HOME/applications/flush-app.desktop`
  (`~/.local/share/applications`) with `Exec="<this program>" %u` and `MimeType=x-scheme-handler/flush;`, and
  `x-scheme-handler/flush=flush-app.desktop` in `[Default Applications]` of `$XDG_CONFIG_HOME/mimeapps.list`
  (`~/.config`), unless the scheme opens with another desktop entry that exists. Files only: no `xdg-mime`, no shell.
  The release tarball also has `share/applications/flush-app.desktop` for an install into a prefix (`cmake --install`).
  By hand: `xdg-mime default flush-app.desktop x-scheme-handler/flush`.
- Never in a test run (`-webuitestscript`).

## Security review

| # | Risk | What stops it |
|---|---|---|
| 1 | **Link spoofing**: a page (any site, an ad, a phishing copy of flush.markets) opens a `flush:` link the user did not ask for. | Nothing runs on a link alone: each verb is a question of the host's own dialog window, which a page cannot draw or answer, headed *From a link: opened in your browser — check it*, with what it asks first and a warning that anyone can make such a link. The accept button arms only after a second (audit 7 #7); declines in a row block the kind for the session (audit 7 #8: the kind is `link:…`). |
| 2 | **Network mismatch**: a link of mainnet opened by a betanet app (or the reverse). | `network` is required and must equal the app's network, else refused with what it is and what the app runs; nothing is resolved. A second start hands a link to the app of its network if that one runs. |
| 3 | **Replay** of an old link (a trade link in the browser's history, a connect answered). | `exp` is required (at most a day ahead; the site writes 10 minutes, an hour for a launch); a link that ran is refused again in the session (its SHA-256 kept), a connect's nonce answered once; the site checks its own nonce on the callback. Each run is still a question. |
| 4 | **Amount or asset substitution**: a ticker retired and taken again, a lookalike name, a pool of other assets, a floor in the wrong places. | Assets and pools by **id** only, the link's ticker checked against the id's name (both must match); swaps: `from`/`to` must be the pool's two assets; the call names assets by id (`0x…`, `ECX`), as the describers resolve them (security review 2026-10-21: no name or number in a confirmed call); amounts paid exactly in their asset's places (refused otherwise), floors rounded up; the fresh quote must meet the floor before anything is asked, and the node refuses at signing if it moved. What is shown is what runs (`confirm::CanonicalParams`). |
| 5 | **URI injection** into the shell or the browser call: a callback or an address that adds parameters, a fragment, a `javascript:` or `file:` URL, a newline into the single-instance socket. | The callback is a constant, never the link's; the answer is built from checked text only (a lowercase bech32 address, a nonce of `[A-Za-z0-9_-]`, a network name); it is opened with the system's call (`g_app_info_launch_default_for_uri`, `ShellExecuteW` of an `https:` URL), never a shell; the desktop entry's `Exec` path is quoted as the specification says; a line with a line break is never sent to the running app (`SingleInstance::Send`). |
| 6 | **Size limits**: a huge link, a huge launch file, a flood of links. | 4096 bytes a link, 64 KB a launch file (and 64 KB a socket message), every text bounded (name 64 bytes, description and links 512, a link 512, a nonce 64), at most 4 links of a launch; one link handled at a time, at most 4 waiting, more refused. |
| 7 | **Phishing copy in the dialog**: text of the link made to look like the app's own (a name with a newline, a right-to-left mark, a zero-width space, a lookalike ticker). | Control and hidden characters are refused in the link; display names follow the chain's rules (and say when they are not plain ASCII); the dialog shows text as text (`dialog::Shown`, `textContent`); the ticker and the id are shown together, short; the address shared is shown whole. |
| 8 | **A logo other than the one shown on the site** (a launch link names the logo by hash). | The file the user picks must have that SHA-256, and the chain's logo rules (PNG/JPEG, 256×256, at most 32 KB, no metadata); the dialog shows the picture itself (`dialog::Row::Kind::IMAGE`). |
| 9 | **The page as a relay**: a page script calls `links.open` itself. | It gets the same question as the browser's link (the preface says *pasted or dropped into the app*): no way around the host's dialog, the same checks. |
| 10 | **The wrong wallet**. | The question names the wallet (and the address shared, whole); with several open and none chosen, the link is refused. |
| 11 | **A hostile route or pool** (a link through pools of other assets, a pool that is not the one shown, a lookalike ticker). | Pools by id only; the route's path is the app's own walk from the link's `from` through each pool's other side, which must end at its `to`; liquidity links name the pool's two tickers, checked against its assets; floors checked against the node's quote and given to the call. |
| 12 | **Slippage as a weapon** where no floor binds (positions). | `position`, `reposition`: at most 10 % (`MAX_BINDING_SLIPPAGE_BPS`), shown as binding. |
| 13 | **A withdrawal to someone else's address** (a phishing link with the attacker's eCash address). | The address is shown whole as the destination, with a warning that a payment on eCash cannot be undone; the describer of `createwithdrawal` shows it again; the usual arming delay and blocking after declines. A withdrawal stays refundable until it is in a bundle. |
| 14 | **A deposit credited elsewhere**. | The credited address is never the link's: the wallet's own `flush.markets` address, its deposit address made by the node; two questions (the link's, then the deposit's own with the paying eCash wallet). |
| 15 | **Results of another action sent to the shared address** (security review 2026-10-23: a destination as wallet-wide state would apply to calls running at the same time). | Thread-local, for the link's one call (`ScopedLinkDestination`, cleared by RAII, on an error too): a call of another thread (the pages, RPC) never reads it; no RPC sets it on a real network; the wallet checks the address is its own (`rpc_tests/link_destination_is_of_one_thread_and_one_scope`). |
| 16 | **A link's floor lowered to a slippage** (security review 2026-10-23: a deposit's or withdrawal's floor given as a percent of the app's quote would let a move between the quote and the signing go below what the user accepted). | The absolute floors go to the call (`addpoolliquidity` `min_shares`, `removepoolliquidity` `min_amounts`): the wallet refuses below them at signing and the transaction's own floors are never under them; swaps and routes (`min_out`), buys and sales (their floor) did so already; orders (a limit) and bids (a falling price) have none to lower. |

## Tests

- `test_flush-app --run_test=webui_links_tests`: the parser (each verb, malformed links, unknown verbs and
  parameters, versions, other networks, expiry, ids, amounts and overflows, texts and their characters, links of a
  launch, connect's callback and nonce, launch files), the units of amounts, the resolution of each verb against a
  chain (ids, tickers, graduated launches, pools and their two assets, the floor against the quote, places), the
  host: each verb's question (preface, then the describer's rows, the call), a link used once, a decline, another
  network, a connect answered once with the one callback, the bound on waiting links, and the Linux handler (the
  desktop entry, mimeapps.list, another installation kept).
- `src/webui/test/e2e/links.js` (`webui_e2e_links`, run_sidechain.py against a regtest Flush and a regtest
  mainchain): a launch from a launch file (both steps, the reservation, the launch, the first buy), a launch from a
  link with its logo chosen, a buy, a sale, a swap in the graduated pool, each opened as from the browser (the
  single-instance hand-over), its question read and accepted in the dialog window and its picture taken; a connect
  (the callback produced, no browser opened in a test run); and the refusals (another network, expired, used twice,
  a ticker that is not the token's, a floor the curve cannot meet, an unknown parameter, a callback elsewhere).
