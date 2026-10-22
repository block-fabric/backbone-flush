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
flush:<verb>?v=1&network=<net>&exp=<time>&…      an action: connect, launch, buy, sell, swap
```

What the parser takes (anything else is refused with what is wrong, before anything is resolved or shown):

- the scheme in any case, then a verb of lowercase letters and `?`; one of the five verbs;
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
  most 512 bytes together (the chain's info), `logo=sha256:<hex>`, a first buy with both `ecx` and `min_tokens`.

A launch file (`*.flushlaunch.json`, at most 64 KB) is read as strictly: exactly the keys of version 1, `network`
required, amounts as text, its logo's bytes with the SHA-256 it says.

## What each verb does

Every action is resolved against the node and asked in the host's dialog window (`Window::Ask`: a window of the host,
in the look of the app — Launchpad in Flush — which the page can neither draw nor answer). The question starts with
where it comes from: **From a link: opened in your browser — check it** (or *pasted or dropped into the app*, or
**From a launch file: you opened it**), what it asks, until when the link is good, then the facts below, the describer's own rows
(wallet, network, command, every asset and pool resolved by id) and a warning: *anyone can make a link like this, and
any page can open one*. Nothing runs unless the user accepts there.

| Verb | Resolved | Runs |
|---|---|---|
| `buy` | The token by its id; the link's ticker must be its name; a launch, not graduated; `ecx` exactly in 8 places (at least 0.0005); the floor in the token's places (rounded up); the curve's quote now must meet it. | `buyfromlaunch ["0x<id>", ecx, slippage, floor]` |
| `sell` | The same; `tokens` exactly in the token's places; the floor in ECX. | `selltolaunch ["0x<id>", tokens, slippage, floor]` |
| `swap` | The pool by its id; `from` and `to` must be its two assets, by their tickers; not abandoned; the amount exactly in its asset's places; the floor rounded up; the pool's quote now must meet it. | `swaproute [[from, to], amount, slippage, floor, [pool]]` |
| `launch` | The ticker free; launches active; the logo the link names (a file chooser of the host: refused if its SHA-256 differs, or the rules refuse it); the first buy's quote on a new curve must meet `min_tokens`. | One question for `reserveasset [ticker]` and `launchtoken [ticker, {display_name, info, logo, first_buy, first_min_out}]`; the launch is sent by the app once the reservation is deep enough (`confirm::Authorizations`: within an hour, for at most the fee shown). |
| `connect` | The wallet's address of the label `flush.markets` (made once). | A question: the site, the address shared (whole), the wallet, where the answer goes. On **Share**: the system's browser opens `https://flush.markets/connect#address=…&nonce=…&network=…`. |

The floor binds: `slippage_bps` is shown only. The host gives the node the slippage that makes the order's own
minimum exactly the floor (`floor(quote × (10000 − s) / 10000) ≥ floor`), so the transaction never accepts less than
the link's floor, and the node refuses at signing time if the pool or curve moved below it. A launch's first buy has
its floor on chain (`first_min_out`; the rules refuse the order below it, its ECX back).

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
