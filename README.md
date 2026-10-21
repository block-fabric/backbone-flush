# Starbase

Starbase is a sidechain of **eCash** (ECX): **tokens, assets and a DEX on a fast chain**. Anyone can create
tokens and assets (shares, points, tickets, stablecoins) and trade them on a decentralized exchange (pools,
routes through several pools, batch clearing, price feeds, oracles) and by Dutch auction, on a chain with a
block every 15 seconds: blocks of stake signed by validators who stake pegged ECX, anchored to eCash through
blind merged mining (BMM), with merged mined blocks as the fallback.

It is a drivechain (BIP300/301) sidechain: ECX comes in by a deposit on eCash and goes back by a withdrawal
that eCash miners vote on. It follows its mainchain either through [Backbone](https://github.com/block-fabric/backbone)
(`backboned`, our client of the eCash networks) or through a plain eCash node with LayerTwo Labs'
`bip300301_enforcer`, over the enforcer's interface alone (`-mainchaingrpc`).

| | `ecash` (eCash main network) | `ecashbeta` (betanet, **the default**) | `ecashtest` (test network) |
|---|---|---|---|
| Slot on the mainchain | 46 | 46 | 46 |
| Coin | ECX, deposited from eCash and withdrawn back to it | the same | the same |
| Addresses | `sbx1…` | `bsbx1…` | `tsbx1…` |
| Deposit addresses | `s46_<address>_<checksum>` | the same | the same |
| P2P port | 8255 | 38255 | 48255 |
| RPC port | 8254 | 38254 | 48254 |
| Message start | `73627321` | `73627322` | `73627323` |
| First block | `7c873d949d0cac915173259dd7312ae7dc075438f8be5a0da459c6632bf0d2e0` | `122710c06ae769c8d18a7ad8038a345173cfbf6b3d720fcbad9a7ac28a8afebc` | `5a463afcefde5455ddd78498cd139fe2f4e8f216d28710912d126d0f9d941d22` |
| Data folder | `~/.starbase` | `~/.starbase/betanet` | `~/.starbase/testnet4` |

None of these networks is launched yet: slot 46 has to be proposed and activated on eCash first (below). The
main network refuses to start until this release names the height of the block that activated slot 46.

Starbase is the eCash edition of [Layers](https://github.com/block-fabric/layers) (FastBlocks' blocks of stake
with every feature of BitAssets): the same rules, with an identity of its own and the parameters tied to
mainchain blocks scaled for eCash's ten-minute blocks. What comes from where: [doc/layers.md](doc/layers.md),
[doc/fastblocks.md](doc/fastblocks.md), [doc/bitassets.md](doc/bitassets.md); everything specific to eCash:
[doc/ecash-sidechains.md](doc/ecash-sidechains.md) and [doc/ecash-enforcer.md](doc/ecash-enforcer.md).

Its GUI is `starbase-app`: the node and its wallet with an HTML interface in a native web view (WebKitGTK on
Linux, WebView2 on Windows), with the Staking section and the pages of the assets and the DEX
([doc/webui.md](doc/webui.md)).

## Building from source

On Debian or Ubuntu (24.04):

```sh
sudo apt install build-essential cmake pkgconf python3 libevent-dev libboost-dev \
    libsqlite3-dev libzmq3-dev libgtk-3-dev libwebkit2gtk-4.1-dev nodejs npm
git clone https://github.com/block-fabric/backbone-starbase.git
cd backbone-starbase
cmake -B build
cmake --build build -j$(nproc)
```

`-DBUILD_WEBUI=OFF` (and no GTK, WebKitGTK or Node.js) for a node without a window. Windows: cross-built
with the depends system (`make -C depends HOST=x86_64-w64-mingw32 NO_QT=1`), see [doc/webui.md](doc/webui.md).

The programs, in `build/bin`:

| Program | What it is |
|---|---|
| `starbase-app` | the wallet, with its window |
| `starbased` | the node, without a window |
| `starbase-cli` | commands for a running node or wallet |
| `starbase-wallet`, `starbase-tx`, `starbase-util` | tools that work without a node |

Tests: `ctest --test-dir build` (unit tests) and `build/test/functional/test_runner.py`. The tests against a
mainchain need a built Backbone tree (`MAINCHAIN_BIN_DIR`, the folder of `backboned`; see
[doc/ecash-sidechains.md](doc/ecash-sidechains.md)).

## Running it

Starbase needs an eCash mainchain on the same network, in one of two ways.

**With Backbone** (`backboned -chain=ecashbeta`, RPC on): Starbase finds it by itself, over JSON-RPC at
127.0.0.1 on Backbone's port (8532 for `ecash` and `ecashbeta`, 48532 for `ecashtest`) with the cookie in
Backbone's data folder (`~/.backbone`, `~/.backbone/betanet`, `~/.backbone/testnet4`):

```sh
starbase-app                       # betanet, the default (-chain=ecashbeta)
starbased -chain=ecashtest         # the test network, without a window
```

**With a plain eCash node and the enforcer** (ecash-com/bitcoin with `bip300301_enforcer`): Starbase follows
the mainchain through the enforcer's gRPC interface alone, with no JSON-RPC access to the eCash node:

```sh
starbased -chain=ecashbeta -mainchaingrpc=127.0.0.1:8123        # the enforcer's port on betanet
starbased -chain=ecash -mainchaingrpc=127.0.0.1:50051 -mainchaingrpcauth=<token>   # or Backbone's -grpc
```

Withdrawal bundles are handed to the enforcer's block producer once the mainchain block that committed to
them is a day deep (144 blocks; 12 on `ecashtest`). How the peg rules work from the enforcer's events alone,
and why they are safe: [doc/ecash-enforcer.md](doc/ecash-enforcer.md).

Options for the mainchain link:

| Option | What it is |
|---|---|
| `-mainchaingrpc=[http://]host:port` | follow through the enforcer's (or Backbone's) gRPC interface alone |
| `-mainchaingrpcauth=<token>` | Backbone's `-grpcauth` token (the enforcer has none) |
| `-mainchainrpcconnect=<ip>`, `-mainchainrpcport=<port>` | Backbone's JSON-RPC (default 127.0.0.1, 8532 / 48532) |
| `-mainchainrpccookiefile=<file>`, `-mainchaindatadir=<dir>` | its cookie, or the folder it is looked for in (default `~/.backbone`) |
| `-mainchainrpcuser=<user>`, `-mainchainrpcpassword=<pw>` | credentials, instead of the cookie |
| `-mainchainrpcwallet=<name>` | the Backbone wallet that pays for merged mining (default: its only loaded wallet) |

Data and the configuration file `starbase.conf` are in `~/.starbase` (Windows `%LOCALAPPDATA%\Starbase`,
macOS `~/Library/Application Support/Starbase`); betanet in its subfolder `betanet`, the test network in
`testnet4`. The app keeps its own settings in `~/.config/Starbase` (Windows `%LOCALAPPDATA%\Starbase`). The
networks have no DNS seeds: give the node a peer with `addnode`. A sample `~/.starbase/starbase.conf`:

```ini
server=1
fallbackfee=0.0002
# The network (ecashbeta when not set):
# chain=ecashtest

[ecashbeta]
addnode=<peer>:38255
mainchaingrpc=127.0.0.1:8123

[ecashtest]
addnode=<peer>:48255
```

## Trying it: the local playground

One command starts a whole Starbase on your computer, on regtest with eCash's rules: `backboned -regtest
-testecash -grpc` (Backbone's eCash regtest serving the enforcer's interface), three Starbase validators that
follow it through that interface alone (`-mainchaingrpc`, as against a plain eCash node with the enforcer), a
mainchain block every 5 seconds, and `starbase-app` as validator 0.

```sh
contrib/playground/starbase-playground.sh start      # about a minute: slot 46 activated, 100 ECX deposited,
                                                     # the token STAR with a STAR/ECX pool, a swap
contrib/playground/starbase-playground.sh status
contrib/playground/starbase-playground.sh withdraw-demo   # request -> bundle -> Submitted -> votes -> paid, under a minute
contrib/playground/starbase-playground.sh cli 0 getbalance   # any RPC: node main, 0, 1 or 2
contrib/playground/starbase-playground.sh mine 10            # ten mainchain blocks now
contrib/playground/starbase-playground.sh stop        # stops exactly what it started
contrib/playground/starbase-playground.sh reset       # stop, and delete the playground
```

Options of `start`: `--block-seconds 5` (a mainchain block every so often), `--slot-seconds 2` (the slots of
the blocks of stake; 15 on eCash), `--no-app` (validator 0 without a window), `--no-demo` (no activation,
deposit or token), `--base-port 49500` (it and the 22 ports after it must be free), `--starbase-bin` (default
`build/bin`), `--backbone-bin` (default `$BACKBONE_BIN_DIR`, else `../chains-ecash/build_node/bin`). Data in
`~/.starbase-playground` (`--dir`, or `$STARBASE_PLAYGROUND`), with each node's log; on WSLg the app runs with
`LIBGL_ALWAYS_SOFTWARE=1`. The playground uses regtest's short windows: a slot activates with more than 5
acks in 10 blocks, a bundle is paid with more than 5 votes in 10 blocks, handed over at depth 2. Its validator
keys are fixed test keys: never use them elsewhere.

## Proposing the sidechain (M1)

Slot 46 is taken on eCash by a proposal (M1) that miners ack (M2): on eCash's main network more than 1,815
acks in 2,016 blocks, on betanet more than 1,008. The proposal:

| | |
|---|---|
| Slot | 46 |
| Title | `Starbase` |
| Description | `Tokens, assets and a DEX on a fast chain over eCash` |

With Backbone: `backbone-cli -chain=ecashbeta createsidechainproposal 46 "Starbase" "Tokens, assets and a DEX on a fast chain over eCash"`;
with the enforcer, its `CreateSidechainProposal`. Check first that slot 46 is free (`listactivesidechains`).
Once active, the height of the activating block goes into `SidechainParams::main_activation_height` of that
network (`src/kernel/chainparams.cpp`, `CEcashSidechainParams`).

## Coins in and out

- **Deposit.** In Starbase, get a deposit address (`getdepositaddress`, or the **Mainchain** page):
  `s46_<address>_<checksum>`. On eCash, send to it with Backbone (`backbone-cli createsidechaindeposit 46
  <deposit address> <amount>`) or the enforcer's `CreateDepositTransaction`; or from the app's **Peg** page.
  The coins appear with the next Starbase block.
- **Withdraw.** `createwithdrawal <eCash address> <amount> ( <fee for eCash miners> )`, or the **Peg** page.
  Withdrawals are paid in bundles that eCash miners vote on (more than 13,150 votes in 26,300 blocks, about
  three months, on `ecash` and `ecashbeta`; more than 5 in 10 on `ecashtest`). A withdrawal not in the bundle
  being voted on can be taken back: `refundwithdrawal <txid> <vout>`.
- **Merged mining.** Starbase BMM blocks are mined by eCash miners, for a fee in ECX. `setbmm true <Starbase
  address>` has the node ask for a block whenever the fees waiting pay for one; `requestbmmblock <address>
  <amount>` asks for one now; `getbmminfo` shows how it goes.

## Tokens, assets and the DEX

- **Creating an asset** takes two steps, a hidden reservation then the registration, so that nobody can see
  the name coming and take it first. Its name is a unique **ticker** (2 to 12 letters and digits) with an
  optional display name and logo. The asset comes with a **control coin**: its holder can mint more, until the
  supply is fixed for good. No ticker reads as the coin (`ECX`, or `CHN`).
- Assets are sent like coins, and can be burned.
- **Pools** pair an asset with ECX or another asset (constant product, 0.3% fee); **routes** trade through
  several pools in one transaction, the orders of a block clear together (**batch clearing**), and every pool
  keeps a time-weighted **price feed**. **Oracles** attest events, and **oracle contracts** pay out by them.
- **Dutch auctions** sell an amount of an asset at a price that falls from a start to an end price.
- An asset nobody holds any more can be **retired**: what is left in its pools goes to the miners.

The node and the RPCs name eCash's coin `ECX` (`CHN`, Layers' name for it, is still accepted). How assets
and the DEX work: [doc/bitassets.md](doc/bitassets.md), [doc/layers.md](doc/layers.md).

## Staking

A validator runs its node with `-validatorkeyfile=<file>` and bonds ECX to its key (`createbond`, or the
**Bonds** tab of Staking); once enough stake is bonded, blocks of stake start, a slot every 15 seconds, each
led by a validator picked by stake. Fees of a block of stake are shared between its producer and the fee pot,
which BMM bidders win. Coins are *soft* in blocks of stake, *BMM-confirmed* once an eCash block commits to
them, and *final* after three anchors (half an hour). Unbonded coins come back after 288 anchors (two days;
12 on `ecashtest`). See [doc/fastblocks.md](doc/fastblocks.md) and, for the eCash values,
[doc/ecash-sidechains.md](doc/ecash-sidechains.md).

## Commands

Assets, on the node: `getbitassetsinfo`, `listassets`, `searchassets`, `getasset`, `getassethistory`,
`listpools`, `searchpools`, `getpool`, `quoteswap`, `quoteroute`, `listauctions`, `searchauctions`,
`getauction`, `quotebid`, `getdexinfo`, `listoracles`, `getoraclevalue`, `getoraclecontract`.

Assets, in the wallet: `reserveasset`, `registerasset`, `releaseassetreservation`, `mintasset`,
`fixassetsupply`, `updateasset`, `transferassetcontrol`, `sendasset`, `burnasset`, `releaseasset`,
`listmyassets`, `listassetactivity`, `addliquidity`, `removeliquidity`, `swapasset`, `swaproute`,
`createauction`, `bidauction`, `collectauction`, `registeroracle`, `publishoracle`, `createoraclecontract`,
`settleoraclecontract`.

Sidechain, on the node: `getmainchaininfo`, `syncmainchain`, `setbmm`, `getbmminfo`, `requestbmmblock`,
`createbmmblock`, `listwithdrawals`, `getwithdrawalbundle`. In the wallet: `getdepositaddress`,
`createwithdrawal`, `refundwithdrawal`.

Staking, on the node: `getposinfo`, `getstakinginfo`, `getvalidators`, `getvalidator`, `getleaderschedule`,
`getanchorinfo`, `listevidence`, `submitevidence`, `setstaking`, `signbondpop`, `checkpegbalance`. In the
wallet: `createbond`, `createunbond`, `listbonds`.

`starbase-cli help` lists them all with their arguments.

## The Chains networks (development only)

Starbase keeps the networks of Layers on Chains (CHN: `main`, `test`, `signet`) for development, and `regtest`
for the tests. `main`, `test` and `signet` need `-chainsnetworks` (with `-chain=main|test|signet`, `-testnet`
or `-signet`), and keep their data in `~/.starbase/chains`. They are the identities of Layers on Chains
(slot 7, `ly1…`/`tly1…`): a node there joins Layers' networks.

## Following the sidechain template

The history of this repository continues that of Layers' `ecash-enforcer` branch, itself the sidechain
template's eCash branch with Layers' commits on it. Updates of the template are merged as before:

```sh
git remote add template https://github.com/block-fabric/sidechain-template.git   # locally: ~/skydoge/sidechain-template
git remote add layers https://github.com/block-fabric/layers.git                 # locally: ~/skydoge/layers
git fetch template && git merge template/ecash-enforcer     # shared fixes of the eCash template
git fetch layers && git merge layers/ecash-enforcer          # Layers' fixes on eCash
```

The renaming to Starbase is a few commits on top (`CLIENT_NAME`, `chain.json`, the eCash identities, the
default network); a merge brings the template's changes beside them.

## Credits

Starbase is Layers on eCash: Layers joins FastBlocks and BitAssets. BitAssets is inspired by **BitAssets by
LayerTwo Labs** (plain-bitassets); FastBlocks' fee pot by sol-drivechain (no code of it used). The eCash link
follows LayerTwo Labs' `bip300301_enforcer` interface. All are built on our sidechain template, which is based
on [Bitcoin Core](https://github.com/bitcoin/bitcoin) v32; its README is in
[doc/README-bitcoin-core.md](doc/README-bitcoin-core.md).

## License

MIT: see [COPYING](COPYING).
