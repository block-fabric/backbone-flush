# Sidechain template

This tree is a sidechain of the [Chains](../../chains) mainchain, with nothing
in it but what every sidechain needs. It is meant to be copied: Thunder and
Hivemind are this tree with their own parameters and features on top.

It is the Chains mainchain tree (Bitcoin Core 32 with drivechains) with the
sidechain rules added, so everything a mainchain node has is here too: the
wallet, the GUI with its tools and themes, the RPC interface, the tests.

## What makes the chain a sidechain

A sidechain has no miners and no coins of its own.

- **Blind merged mining.** A block is valid only if a block of the mainchain
  committed to its hash. Whoever wants a block mined builds it, and pays
  mainchain miners a fee, on the mainchain, to commit to it. The miners need
  not run the sidechain or know what the block contains. There is at most one
  sidechain block per mainchain block. The proof of work field of the header
  is kept as a formality that takes a couple of hashes.
- **Deposits.** Coins sent to the escrow of the sidechain on the mainchain,
  with an address of the sidechain as the destination, are created on the
  sidechain by the coinbase of its next block. There is no block subsidy: the
  miner of a block earns the transaction fees.
- **Withdrawals.** A transaction burns coins in a withdrawal output that names
  a mainchain address and a fee for mainchain miners. Blocks commit to
  withdrawal *bundles*: the mainchain transaction that pays the waiting
  withdrawals. Which withdrawals a bundle pays is fixed by the rules, so every
  node builds the same bundle from the same chain. The mainchain miners vote
  on the bundle for a long time (three months on the Chains main network),
  then the mainchain pays it out of the escrow, or lets it fail.
- **Refunds.** A withdrawal that is not in the bundle being voted on can be
  taken back by whoever made it. The withdrawals of a failed bundle go in a
  later one, or can be taken back.

A node validates all of this itself. It does so against a record of the
mainchain that it keeps by following a mainchain node, so **a sidechain node
needs a mainchain node** (`chainsd`), reached over its RPC interface.

## Where the code is

| What | Where |
|---|---|
| Parameters of the rules | `Consensus::SidechainParams` in `src/consensus/params.h` |
| Identity of the chain on each network | `MakeSidechain(...)` calls in `src/kernel/chainparams.cpp` |
| The rules: state, deposits, withdrawals, bundles, refunds | `src/sidechain/state.{h,cpp}` |
| Script formats of withdrawals, refunds, bundle commitments | `src/sidechain/script.cpp` |
| Record of the mainchain | `src/sidechain/mainchain.{h,cpp}` |
| Keeping the record current, mainchain reorganisations, merged mining | `src/sidechain/follower.{h,cpp}` |
| RPC client for the mainchain node | `src/sidechain/mainclient.{h,cpp}` |
| Hooks in validation | `ContextualCheckBlockHeader`, `ConnectBlock`, `GetBlockSubsidy`, mempool checks in `src/validation.cpp` |
| Block assembly | `src/node/miner.cpp` |
| RPC commands | `src/rpc/sidechain.cpp`, `src/wallet/rpc/sidechain.cpp` |
| GUI page | `src/qt/sidechainpage.{h,cpp}` |
| Tests | `test/functional/feature_sidechain.py` |

The state of the sidechain rules (`sidechain::State`) is stored, copied and
reverted together with the drivechain database of the mainchain code
(`drivechain::SidechainDB`), which is otherwise unused here: a sidechain has
no sidechains of its own.

The validity of a block depends on the block, the blocks before it, and the
record of the mainchain; never on asking the mainchain node during validation.
A block committed to by the mainchain block at height H acts on what the
mainchain did up to height H - 1: it pays the deposits of those blocks and
takes note of the outcome of the bundle.

## Commands

Node: `getmainchaininfo`, `syncmainchain`, `createbmmblock`, `setbmm`,
`getbmminfo`, `listwithdrawals`, `getwithdrawalbundle`.

Wallet: `createwithdrawal`, `refundwithdrawal`.

On the mainchain node, for the sidechain: `createsidechaindeposit`,
`createbmmrequest`, `listwithdrawalbundles`, `setwithdrawalvote`,
`getsidechainevents`.

Options: `-mainchainrpcconnect`, `-mainchainrpcport`, `-mainchainrpcuser`,
`-mainchainrpcpassword`, `-mainchainrpccookiefile`, `-mainchaindatadir`,
`-mainchainrpcwallet`. Without user and password, the cookie file of the
mainchain node is used, looked for in `~/.chains`.

## Trying it on regtest

On regtest the chain is an ordinary chain unless `-sidechainslot=<n>` is
given, so that the tests inherited from the mainchain keep working.

```sh
# The mainchain: a wallet with coins, and the sidechain activated in slot 3.
chainsd -regtest -daemon -fallbackfee=0.0002
chains-cli -regtest createwallet miner
MAIN=$(chains-cli -regtest getnewaddress)
chains-cli -regtest generatetoaddress 110 $MAIN
chains-cli -regtest createsidechainproposal 3 "My sidechain"
chains-cli -regtest generatetoaddress 21 $MAIN

# The sidechain node follows it.
sidechaind -regtest -daemon -sidechainslot=3 -fallbackfee=0.0002
sidechain-cli -regtest getmainchaininfo
sidechain-cli -regtest createwallet side
SIDE=$(sidechain-cli -regtest getnewaddress)

# Deposit, and mine: one sidechain block per mainchain block.
chains-cli -regtest createsidechaindeposit 3 $SIDE 10
sidechain-cli -regtest setbmm true $SIDE 0.001
chains-cli -regtest generatetoaddress 5 $MAIN
sidechain-cli -regtest getbalance

# Withdraw.
sidechain-cli -regtest createwithdrawal $MAIN 2
chains-cli -regtest generatetoaddress 3 $MAIN
sidechain-cli -regtest getwithdrawalbundle
chains-cli -regtest listwithdrawalbundles 3
```

`sidechain-qt` does the same from its Mainchain tab.

## Making your own sidechain

1. Copy this tree.
2. Name it: `CLIENT_NAME` and `CLIENT_BIN_NAME` at the top of `CMakeLists.txt`.
   The programs, the data directory and the configuration file are named
   after `CLIENT_BIN_NAME`.
3. Give it an identity on each network: the `MakeSidechain(...)` calls in
   `src/kernel/chainparams.cpp` (message of the first block, magic bytes,
   port, address prefix), and the RPC ports in `src/chainparamsbase.cpp`. The
   first block is derived from the message, so a new message is a new chain.
   The slot is the one the mainchain activates your sidechain in.
4. Bring the tests along:
   - `contrib/sidechain/rename-hrp.py <old> <new>` re-encodes the addresses in
     the tests for each address prefix you changed;
   - the names of the programs and the magic bytes of the networks are in
     `test/functional/test_framework/` (`util.py`, `test_node.py`,
     `messages.py`), the ports in `p2p_addrfetch.py`;
   - regenerate `test/functional/data/util/getchainparams-*.json` with
     `<name>-util [-testnet|-signet|-regtest] getchainparams`.
5. Add what makes your sidechain worth having. Consensus rules of your own
   go next to the sidechain rules in `sidechain::State::ConnectBlock`, with
   their state in `sidechain::State` if they have any; it is stored and
   reverted with the rest.
6. Have the sidechain proposed and activated on the mainchain
   (`createsidechainproposal` there), with the hash of your release as its
   identifier.

Run `ctest --test-dir build` and `build/test/functional/test_runner.py`; the
sidechain test needs a built mainchain tree next to this one (`../chains`),
or `MAINCHAIN_BIN_DIR` set to the directory of its programs.

## Limits to know

- A sidechain trusts the miners of the mainchain with its coins: a majority of
  them can vote through a bundle that the sidechain did not make. That is the
  drivechain security model; the long voting period is what gives users and
  honest miners time to object.
- Deposits to something that is not an address of the sidechain are burned.
- A withdrawal refund pays a P2WPKH address of the wallet that made the
  withdrawal, and needs that wallet to sign the request.
