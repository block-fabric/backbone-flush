# Layers

Layers is a sidechain of Chains: **assets on a fast chain**. Anyone can create assets (shares, points,
tickets, stablecoins) and trade them, in pools (an automated market maker) and by Dutch auction, on a
chain with a block every 15 seconds: blocks of stake signed by validators who stake CHN, anchored by the
Chains mainchain through blind merged mining (BMM), with merged mined blocks as the fallback.

| | |
|---|---|
| Slot on the mainchain | 7 |
| Coin | CHN, deposited from [Chains](https://github.com/block-fabric/chains) and withdrawn back to it |
| Addresses | `ly1…` (`tly1…` on the test networks) |
| P2P port | 9255 (testnet 19255, signet 39255, regtest 29255) |
| RPC port | 9254 (testnet 19254, signet 39254, regtest 29254) |

Layers is [FastBlocks](doc/fastblocks.md) (the blocks of stake, staking, finality) with every feature
of [BitAssets](doc/bitassets.md) (the assets, the pools, the auctions), both made from the
[sidechain template](doc/sidechain.md) of Chains: what comes from which, and how they work together, is
in [doc/layers.md](doc/layers.md). A Layers node needs a Chains node (`chainsd` or `chains-app`) to
follow, reached over its RPC interface.

Its GUI is `layers-app`: the node and its wallet with an HTML interface in a native web view
(WebKitGTK on Linux, WebView2 on Windows), with the Staking section and the pages of the assets
([doc/webui.md](doc/webui.md)). There is no Qt GUI.

## Installing

There is no packaged build yet: build it from source, below.

## Building from source

On Debian or Ubuntu (24.04):

```sh
sudo apt install build-essential cmake pkgconf python3 libevent-dev libboost-dev \
    libsqlite3-dev libzmq3-dev libgtk-3-dev libwebkit2gtk-4.1-dev nodejs npm
git clone https://github.com/block-fabric/layers-app.git
cd layers-app
cmake -B build
cmake --build build -j$(nproc)
```

`-DBUILD_WEBUI=OFF` (and no GTK, WebKitGTK or Node.js) for a node without a window. Windows: cross-built
with the depends system (`make -C depends HOST=x86_64-w64-mingw32 NO_QT=1`), see [doc/webui.md](doc/webui.md).
Other systems and options: [doc/build-unix.md](doc/build-unix.md) and the other `doc/build-*.md`.

The programs, in `build/bin`:

| Program | What it is |
|---|---|
| `layers-app` | the wallet, with its window |
| `layersd` | the node, without a window |
| `layers-cli` | commands for a running node or wallet |
| `layers-wallet`, `layers-tx`, `layers-util` | tools that work without a node |

Tests: `ctest --test-dir build` (unit tests) and `build/test/functional/test_runner.py`. The
sidechain tests need a built Chains tree next to this one (`../chains`), or `MAINCHAIN_BIN_DIR`
set to the folder of its programs.

## Running it

First a Chains node, with RPC on (`server=1` in `chains.conf`, or `-server`), on the same network.

The simplest is to let the Chains wallet run Layers: in its **Sidechain Nodes** window, select
Layers, press **Locate programs…** and choose the folder with `layersd` and `layers-app`, then **Start node**
or **Open wallet**.

By hand, on the same computer as the Chains node:

```sh
layers-app                # the main network
layers-app -testnet       # the test network
```

It finds the Chains node by itself: RPC on 127.0.0.1 at the port of the network, and the cookie
file in the Chains data folder (`~/.chains/.cookie`, `~/.chains/testnet/.cookie`). Options for
anything else:

| Option | What it is |
|---|---|
| `-mainchainrpcconnect=<ip>` | address of the Chains node (default 127.0.0.1) |
| `-mainchainrpcport=<port>` | its RPC port (default 9554, testnet 19554, signet 39554, regtest 29554) |
| `-mainchainrpccookiefile=<file>` | its cookie file |
| `-mainchaindatadir=<dir>` | its data folder, where the cookie file is looked for (default `~/.chains`) |
| `-mainchainrpcuser=<user>`, `-mainchainrpcpassword=<pw>` | credentials, instead of the cookie |
| `-mainchainrpcwallet=<name>` | the Chains wallet that pays for merged mining (default: its only loaded wallet) |

The slot is fixed on the main and test networks; `-sidechainslot=<n>` is for regtest only, where
it makes the chain a sidechain in slot `n` (see [doc/sidechain.md](doc/sidechain.md)).

Data and the configuration file `layers.conf` are in `~/.layers`; the test network has a subfolder
`testnet`. The networks have no DNS seeds: give the node a peer with `addnode`. A sample
`~/.layers/layers.conf`:

```ini
server=1
fallbackfee=0.0002

# For the test network instead, uncomment:
# testnet=1

[main]
addnode=<peer>:9255
mainchainrpcwallet=<wallet of the Chains node>

[test]
addnode=<peer>:19255
mainchainrpcwallet=<wallet of the Chains node>
```

On the test network, the `-mainchainrpcport`, `-mainchainrpccookiefile` and `-mainchainrpcwallet`
options are read only from the `[test]` section.

## Coins in and out

- **Deposit.** In Layers, get a deposit address (`getdepositaddress`, or the **Mainchain** page):
  `s7_<address>_<checksum>`. In Chains, send to it from the **Sidechains** page, or
  `chains-cli createsidechaindeposit 7 <deposit address> <amount>`. The coins appear with the
  next Layers block.
- **Withdraw.** `createwithdrawal <Chains address> <amount> ( <fee for Chains miners> )`, or the
  **Mainchain** page. Withdrawals are paid in bundles that Chains miners vote on (about three
  months on the main network, 600 blocks on the test network). A withdrawal not in the bundle
  being voted on can be taken back: `refundwithdrawal <txid> <vout>`.
- **Merged mining.** Layers BMM blocks are mined by Chains miners, for a fee in CHN paid by the Chains
  wallet. `setbmm true <Layers address>` has the node ask for a block whenever the fees waiting pay
  for one; it offers the miners 99% and keeps 1% at the address. `requestbmmblock <address>
  <amount>` asks for one block now, for a fee of your choice (for a deposit on a quiet chain).
  `getbmminfo` shows how it goes.

How deposits, withdrawals, bundles and merged mining work: [doc/sidechain.md](doc/sidechain.md).

## Assets

- **Creating an asset** takes two steps, a hidden reservation then the registration, so that nobody
  can see the name coming and take it first. The asset comes with a **control coin**: its holder can
  mint more, until the supply is fixed for good.
- Assets are sent like coins, and can be burned.
- **Pools** pair an asset with CHN or another asset (constant product, 0.3% fee). Anyone can add
  liquidity and swap.
- **Dutch auctions** sell an amount of an asset at a price that falls from a start to an end price.
- An asset nobody holds any more can be **retired**: what is left in its pools goes to the miners.

How assets work: [doc/bitassets.md](doc/bitassets.md).

## Commands

Assets, on the node: `getbitassetsinfo`, `listassets`, `getasset`, `getassethistory`, `listpools`,
`getpool`, `quoteswap`, `listauctions`, `getauction`, `quotebid`.

Assets, in the wallet: `reserveasset`, `registerasset`, `releaseassetreservation`, `mintasset`,
`fixassetsupply`, `updateasset`, `transferassetcontrol`, `sendasset`, `burnasset`, `releaseasset`,
`listmyassets`, `listassetactivity`, `addliquidity`, `removeliquidity`, `swapasset`, `createauction`,
`bidauction`, `collectauction`.

Sidechain, on the node: `getmainchaininfo`, `syncmainchain`, `setbmm`, `getbmminfo`, `requestbmmblock`, `createbmmblock`, `listwithdrawals`, `getwithdrawalbundle`.

Sidechain, in the wallet: `getdepositaddress`, `createwithdrawal`, `refundwithdrawal`.

Staking, on the node: `getposinfo`, `getstakinginfo`, `getvalidators`, `getvalidator`, `getleaderschedule`,
`getanchorinfo`, `listevidence`, `submitevidence`, `setstaking`, `signbondpop`, `checkpegbalance`.

Staking, in the wallet: `createbond`, `createunbond`, `listbonds`.

## Staking

A validator runs its node with `-validatorkeyfile=<file>` and bonds CHN to its key (`createbond`, or the
**Bonds** tab of Staking); once enough stake is bonded, blocks of stake start, a slot every 15 seconds,
each led by a validator picked by stake. Fees of a block of stake are shared between its producer and the
fee pot, which BMM bidders win. Coins are *soft* in blocks of stake, *BMM-confirmed* once a mainchain
commitment covers them, and *final* after enough of them. See [doc/fastblocks.md](doc/fastblocks.md).

## Wallet

`layers-app`: the pages of the wallet (Overview, Send, which sends an asset as well as CHN, Receive,
Transactions, Wallets), **Peg** and **Merged mining** (deposits, withdrawals, merged mining), **Staking** (one section: Overview,
Validator, Bonds, Network), and the pages of
the assets: **Assets** (every asset, its supply, markets, auctions and history), **My assets** (what the
wallet holds and controls: mint, change the data, give control, fix the supply, burn; the assets being
issued; pool shares; auctions to collect; activity), **Register new asset** (a wizard), **Trade** (swaps, pools,
liquidity) and **Auctions**. See [doc/webui.md](doc/webui.md).

## Credits

Layers joins FastBlocks and BitAssets, both of Chains. BitAssets is inspired by **BitAssets by LayerTwo Labs** (plain-bitassets); FastBlocks' fee pot by sol-drivechain (no code of it used). Both are built on the sidechain template of Chains, which is based on
[Bitcoin Core](https://github.com/bitcoin/bitcoin) v32; its README is in
[doc/README-bitcoin-core.md](doc/README-bitcoin-core.md).

## License

MIT: see [COPYING](COPYING).
