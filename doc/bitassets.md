# BitAssets: assets on a sidechain

BitAssets is a sidechain of Chains (slot 4) on which anyone can create assets — shares, points,
tickets, stablecoins — and trade them: in pools (an automated market maker) and by Dutch auction.
It is inspired by **BitAssets by LayerTwo Labs** (plain-bitassets), built anew on this Bitcoin Core
based sidechain, with Bitcoin's keys and transactions, and without the defects of the original.

## Assets

- **Creating one takes two steps**, so that nobody can see a name coming and take it first. A
  *reservation* commits to the name without showing it (HMAC-SHA256, under a nonce only the wallet
  can make, of the name's hash and of the script of the output that carries the reservation); once
  it is in a block, the *registration* reveals the nonce and creates the asset: its initial supply
  (zero is allowed) and its **control coin**. The app (`bitassets-app`, its Issue page) does both steps.
  Bound to its output's script, a reservation cannot be copied to any use: a copy made by someone
  else, with an output of their own, commits to nothing they can reveal.
- **A reservation is revealed only once it is deep enough** (`bitassets_reveal_depth` blocks: 6 on
  the main network, 3 on the test network and signet, 2 on regtest, as for BitNames): a
  registration in block *h* of a reservation made in block *r* needs *h - r* ≥ the depth. Whoever
  makes a block sees every registration waiting for it; without this, it could reserve the same
  name under a nonce of its own and register it in its own block, ahead of the one it saw.
  `registerasset` refuses to register sooner and says from which block it can; `listmyassets`
  gives each reservation's `registers_from` and `wait`, and `getbitassetsinfo` the depth
  (`reveal_depth`). The app waits as long by itself, and registers then.
- **A name may not read as another asset**: not CHN (in any case), not like a number
  (`1739-0029`, `0001-1739-0029`), not starting with `0x`.
- **Nor carry another asset's number** where names are shown with their numbers (`GOLD (#1739-0029)`):
  the wallet does not reserve or publish a name holding `#`, `(`, `)` or four digits, a dash and
  four digits, and the mempool does not relay the registration of one (`ba-name-look-alike`). This
  is policy, not consensus: a block producer may still publish one, so the app shows every asset
  number first (`#5555-1234 · "GOLD (#1739-0029)"`). Text shaped as a number that is none
  (`0000-1739-0029`) is an error in every command, never the asset of that name.
- **The front-running window that remains**: a registration not mined within the reveal depth can
  be seen in the mempool, and a block producer who reserved the same name meanwhile (after seeing
  it) could register it in its own block once its own reservation is deep enough. This is inherent
  to commit-reveal; registering as soon as the depth allows (the app does) keeps the window short.
- **An asset is known by the SHA-256 hash of its name.** The registration publishes the name
  unless asked not to; an asset can be named by its name, `0x` and its hash, or its number
  (`1739-0029` for the first), and CHN by `CHN`, in the node's commands and the wallet's alike.
- **Decimals** (0 to 12) are set at registration and only change how amounts are shown: with 2,
  1.5 is 150 units.
- **The control coin** mints more (`mintasset`) and changes the asset's data (`updateasset`): a
  description, a commitment to documents kept elsewhere, addresses and keys of the issuer. It is
  moved like any coin (`transferassetcontrol`). Burning it (`fixassetsupply`) fixes the supply for
  good.
- **Anyone can burn** coins they hold (`burnasset`). Each asset keeps what was minted and burned;
  the supply is the difference.

## How it is carried

Tokens — asset coins, control coins, reservations, pool shares and auction receipts — are carried
by outputs of no value. A *marker*, an OP_RETURN output of the transaction (`BAST`), lists each such
output with what it carries, and says what the transaction does, if anything. The rules keep the
books: for every token, what the transaction spends plus what its operation creates is exactly
what it outputs plus what its operation takes in. Nothing is burned by accident; a transaction that
spends a token without carrying it on, or outputs one it does not have, is invalid.

## Pools

- A pool trades two assets, CHN included: a constant-product market maker (Uniswap v2). Each trade
  leaves 0.3% in the pool, for the liquidity providers.
- `addliquidity` puts both assets in, for shares of the pool (tokens like any other). The first
  deposit makes the pool and sets its price; it keeps 1000 shares for good, so that a pool is never
  emptied. Later deposits go in at the pool's price.
- **The amounts of a deposit are the most that goes in** (the fourth audit's rules). Into a pool someone provides liquidity to, the side that gives fewer
  shares goes in whole and, of the other, only what those shares are worth at the pool's price
  (rounded up: the pool never loses); the rest comes back. The marker lists two result outputs: the
  shares, then what comes back — asset coins, or CHN paid by the coinbase to that output's address
  (less than the least CHN paid out stays in the pool); it carries nothing if nothing comes back.
  Nothing a deposit offers is given away to the pool's providers. (Before these rules, both amounts
  went in whole, for the shares of the side that gave fewer: the wallet offers only what the pool's
  price takes.)
- A pool everyone has left (*abandoned*: only the 1000 shares nobody holds) takes no trades, and is
  reopened as a new pool is made: shares are the square root of the product of what it holds after
  the deposit, 1000 of them kept for good, and both amounts go in. What it held — dust, at whatever
  price the last trade or provider left it — is merged in: the wallet takes the amounts given as what
  the pool reopens with, and puts in the rest, so that they set its price exactly (it refuses
  amounts not above the dust). Before these rules, a deposit into an abandoned pool went in at the
  dust's price, for the shares of the side that gave fewer: someone who reopened it first at the
  dust's price, just before a reopening, took the reopener's excess.
- **A reopening cannot be front-run.** Whoever's deposit opens the pool first sets its price;
  another deposit made for another price then goes in at that price, its excess back, and is refused
  if it gives fewer shares than the slippage allows (the shares of a deposit fall with the square
  root of how far the price is from its own). Making a reopening fail takes reopening the pool
  oneself, with at least 0.01 CHN on a CHN side and 100000 shares, at a price anyone can trade
  against, or arbitrage by a swap before adding; leaving again leaves the 1000 shares' part behind,
  as dust for the next reopener.
- `swapasset` pays one asset in and takes at least an amount of the other out (the slippage the
  trade accepts); `quoteswap` says what a trade gives now. `removeliquidity` gives shares back for
  both assets; a side that rounds to nothing is left out, so that a small provider can always leave.

## Dutch auctions

- `createauction` sells an amount of one asset for another. The price of all of it starts at the
  start price and falls in a straight line, block by block, to the end price; a bid buys at the
  price of its block (`bidauction`), rounded in the seller's favour.
- The transaction outputs a **receipt**. Its holder collects (`collectauction`) what the auction
  brought in and what is left of what it sold, once it has ended or sold out — or at once, before
  any bid, which cancels it.
- `getauction` and `quotebid` give what all that is left costs in the next block
  (`cost_of_remaining`: the least amount whose bid buys it all). `bidauction` with `buy_all` pays
  exactly that, never more (with an amount, at most that amount), and refuses when no bid buys
  exactly what is left (the least that buys it all buys more, which the rules refuse); a bid of an
  amount that buys all that is left pays only what that costs. The app's "Buy all that is left" pays the
  amount it shows (less, should the price fall first).
- An auction starts within 52560 blocks (about a year) of the one that makes it.
- A closed (collected) auction stays in the state for good, as its history; `listauctions` leaves
  closed ones out unless asked (`include_closed`).

## Retiring an asset

`releaseasset` retires a dead asset: its supply fixed, nobody holding any, every unit in pools
nobody provides liquidity to, and no auction of it to collect. An auction that only sells something
for it, and has taken none of it in, does not count; once the asset is retired (and even if its name
is registered again, as another asset), that auction takes no more bids, and its seller collects
what it sells.

## Logos

An asset can have a logo: a PNG or a JPEG of exactly 256 x 256 pixels, at most 32768 bytes, kept on
the chain. Whoever holds the asset's control coin sets it — at the registration (`registerasset`
with `logo`) or later (`setassetlogo`, which also removes it with an empty logo) — and a new one
replaces the one before. A fixed asset (control coin burned) keeps the logo it has.

**Carrier.** The logo goes whole into the marker of the transaction that sets it (operation
`SetLogo`, kind 32; a registration with a logo is kind 33: the registration, then the logo). One
transaction, signed like any other, spending and outputting the control coin as `updateasset` does.
A 32 KB OP_RETURN is standard here (`-datacarriersize` defaults to 98 000 bytes, a transaction to
392 000 weight units) and a full logo is about 131 000 weight units, 3% of a block's 4 000 000 for
transactions. Witness data (an inscription-style envelope in a tapscript) would cost a quarter of
that, but takes two transactions (a commitment, then a reveal spending it) that a reorganization or
a replacement can separate (the commitment's coins stranded), and a signer the wallet does not have
(the envelope is no miniscript); at 1 sat/vB a full logo costs about 0.00033 CHN in a marker against
0.00009 in a witness, and logos are set rarely. The marker was chosen: atomic, one confirmation of
exactly the bytes shown, nothing new to sign.

**Rules** (`bitassets/logo.h`; consensus checks the structure of the file, it decodes no image):

- At most 32768 bytes, not empty. PNG (signature `89 50 4E 47 0D 0A 1A 0A`) or JPEG (`FF D8`);
  anything else (SVG, GIF, WebP, BMP, HTML...) is refused (`bad-ba-logo-format`).
- PNG: every chunk's length within the file and its CRC-32 right; the chunks IHDR, PLTE, tRNS,
  IDAT, IEND only (no text, time, gamma, colour profile, Exif, animation or private chunk). IHDR
  first, 13 bytes: 256 x 256, a bit depth and colour type the PNG standard pairs (grey 1/2/4/8/16,
  RGB 8/16, palette 1/2/4/8, grey+alpha and RGBA 8/16), compression and filter 0, **not
  interlaced**. PLTE once, before the image data, for a palette image only (and required by one),
  at most 2^depth entries. tRNS once, before the image data, after PLTE, of its colour type's size
  (no more entries than the palette), never with an alpha channel. IDAT chunks one after another,
  more than 0 bytes in all, starting with a zlib header (deflate, window up to 32 KiB, no preset
  dictionary, its check right). IEND empty and last: nothing after it.
- JPEG: SOI, then segments of SOF0 (baseline) or SOF2 (progressive), DQT, DHT, DRI and SOS only,
  each of the length its content gives; no APPn (JFIF, Exif, XMP, ICC, Adobe), no COM, no other
  frame type (extended, lossless, hierarchical, arithmetic), DNL, DHP, EXP, fill bytes or stray
  markers. One SOF before the first scan: 8-bit, 256 x 256, 1 or 3 components (different ids,
  sampling factors 1–4, quantization tables 0–3). DQT: 8-bit tables of 64 values filling the
  segment; DHT: tables filling the segment, of at most 256 values, each a prefix code; DRI 4 bytes.
  SOS: 1 to 4 of the frame's components, each once; baseline 0–63 with no approximation, progressive
  a DC scan (0–0) or an AC scan of one component (s–e, 1 ≤ s ≤ e ≤ 63). Its entropy-coded data
  holds FF only before 00 or a restart marker (FFD0–FFD7). At least one scan; EOI last, nothing after.
- The reasons: `bad-ba-logo-empty`, `-oversize`, `-format`, `-size` (not 256 x 256), `-trailing`,
  `-png-*` and `-jpeg-*`; `bad-ba-logo-none` (removing a logo there is not), `bad-ba-logo-asset`,
  and `bad-ba-no-control` / `bad-ba-asset-unknown` as for `updateasset`.

**Activation.** From `SidechainParams::logo_height`: before it a marker that sets a logo is not
valid (`bad-ba-logo-inactive`), as no such marker was; nodes have to be upgraded before the chain
reaches it (an older node refuses a block with one). Main network and signet: 0 (not launched).
Test network: `TESTNET_LOGO_HEIGHT` in `src/kernel/testnet_activation.h`, a placeholder (never)
until the deploy sets it a few blocks above the tip. Regtest: 0, or
`-testactivationheight=logo@<height>`. `getbitassetsinfo` says `logo_height` (-1: never).

**State and storage.** The state records each asset's logo (table 0x90: SHA-256 of the bytes,
format, size, height, transaction) and keeps its bytes apart (0x91), only the current logo of each
asset; no rule reads them. They are kept with the sidechain state, not read from the blocks, so a
pruned node serves them too; the journal of each block puts the logo before back when a
reorganization disconnects it, and a state derived again from the blocks has them again. Retiring
an asset drops its logo (the name registered again starts without one).

**Commands.** `getasset` and `listassets` say `logo` {`hash`, `mime`, `size`, `height`, `txid`}
(never the bytes); `getassetlogo <asset>` gives them with `base64` (at most 32768 bytes).
`setassetlogo <asset> <base64> ({"fee_rate": sat/vB})` (an empty logo removes it) and
`registerasset ... <logo>` check the logo against the rules before they pay for anything.

**Showing a logo.** The bytes are the chain's: show them only as an image of the MIME type the node
gives, never as a document or markup — the app makes a `data:image/png;base64,...` (or jpeg) URL
for an `<img>`, and its own pages never put a logo in HTML. A file the rules take is one image and
nothing else, but its pixels are anyone's: data hidden in the image data itself (steganography)
cannot be told apart, and a deflate stream may be unusual (a decoder stops at 256 x 256: at most
256 x (1 + 256 x 8) bytes, about 0.5 MB, whatever the stream claims). The app (`bitassets-app`)
crops and scales any picture to 256 x 256, encodes it as PNG (a palette when that is smaller) and
as JPEG (quality about 80), keeps the smaller, and strips every metadata segment.

## CHN in pools and auctions

CHN that goes into a pool or an auction leaves circulation: the marker burns it (its value is the
CHN the operation takes in, exactly). CHN that comes out is paid by the coinbase of the block, as
deposits are, to the address the transaction names (for a deposit into a pool, the address of its
second result output). Asset coins that come out go to the *result*
outputs the marker lists, in the amount the state gives when the block is connected — never less
than the transaction asks for.

The least CHN paid out is 50000 satoshis, 0.0005 CHN (each payout takes a place in the coinbase's
queue, which pays a bounded number per block: 500 of the sidechain's own; filling it with payouts of
1000 satoshis, as under the second audit's rules, took next to nothing): a swap or a bid that would
pay less is refused; less taken out of a pool, or coming back from a deposit, stays in it, for its
providers; less from collecting an auction is not paid (it was burned when it went in).

## Rules from genesis

The rules of the audits apply from block 0 on every network: there are no activation heights for
them, and the rules before them are gone. (The logos of assets, added after, have one: Logos.)

- A reservation registers only bound to its output's script; one of the name alone registers
  nothing (it can only be released, and the name reserved again).
- Whether an asset can be retired is read from counts its record keeps (auctions that hold some of
  it, its pools with providers), not by going through every auction that quotes it.
- Retiring an asset whose pools hold 1 satoshi of CHN gives it to mainchain miners too (a
  withdrawal of 1 satoshi, without fee).

A node stores the assets in a layout of its version (`bitassets::STORE_LAYOUT`, 4 since the audits'
rules apply from genesis); one that finds them in an earlier layout derives the sidechain state again
from its blocks at startup (a pruned node cannot, and has to be resynced).

## Commands

Node: `getasset`, `listassets`, `getassethistory`, `getassetlogo`, `getbitassetsinfo`, `listpools`, `getpool`,
`quoteswap`, `listauctions`, `getauction`, `quotebid`.
`listassets`, `listpools` and `listauctions` return a page at a time (`count`, at most 1000 for
pools and auctions, and `skip`; `listauctions` also by asset). The pools and auctions are ordered,
and `getbitassetsinfo` counted, once per block from a snapshot of the state read without the chain
lock, so polling them costs a page, not the whole table.

`swapasset` takes the least the caller showed (`min_out`): if the pool moved since its quote, so
that what the wallet would sign as the least is lower, it refuses instead of signing for less.
`addliquidity` takes what the caller expects (`sets_price`): a deposit that was to make a pool is
refused if someone made it meanwhile, rather than going in at their price.
Wallet: `reserveasset`, `registerasset`, `releaseassetreservation`, `listmyassets`, `sendasset`,
`mintasset`, `burnasset`, `updateasset`, `setassetlogo`, `transferassetcontrol`, `fixassetsupply`, `swapasset`,
`addliquidity`, `removeliquidity`, `createauction`, `bidauction`, `collectauction`,
`listassetactivity`, `listassettransactions`.

`listassettransactions (count skip asset)` lists the asset moves of the wallet, newest first, one entry per asset a
transaction moves: its category (send, receive, register, mint, burn, swap, pool_add, pool_remove, auction, bid,
collect, logo, update, control, reserve, release), the asset with its label and number, the amount (exact) and its sign,
the network fee when the wallet paid it, and the other side: for a send the addresses it went to, for a receive the
addresses that paid the transaction, read from the undo data of its block (or the mempool) — no index; "unknown" when
the block is pruned. A result of a swap or a bid spent since is shown as the least the transaction asked (`at_least`).

## Differences from the original

- Bitcoin's keys, addresses and transactions instead of ed25519 and BLAKE3; SHA-256 for asset ids,
  HMAC-SHA256 for reservations.
- Every output names the token it carries, and the books must balance exactly, instead of the
  original's inferred assets, where an operation could claim more than it spent and leftovers were
  burned without a word.
- Only the control coin of an asset mints it or changes its data (the original let the control
  coin of any asset change any asset's data). Assets without initial supply work, and minting needs
  no coins of the asset.
- CHN in and out of pools and auctions is accounted for (the original counted CHN put in as a fee
  the block's miner could take, and could never pay any out).
- No division by zero in a swap, no stop in an auction's last block, auction quantities rounded
  down (the original rounded up, for the bidder), single 0.3% fee (the original took it twice),
  minimum liquidity, receipts required, checked arithmetic throughout.
- Transactions are checked against the state when they enter the mempool, and stale ones leave it
  after each block; two registrations of a name in one block are refused. After a block, only the
  transactions whose check read what the block changed (the keys of the state noted when each was
  last checked), bids and auctions made to start at a height, and those not checked yet are checked
  again: the work is that of what blocks change, not that of the mempool.
- Added: burning, fixing the supply, giving control, auction cancellation, prices and quotes,
  the wallet's activity.
