# The mainchain over the bip300301_enforcer's interface

> Since branch `ecash-enforcer`: `-mainchaingrpc=host:port` follows the mainchain and makes every request
> through the interface alone, on the eCash networks, whose peg rules now take only the enforcer's events (no
> scores): `doc/ecash-enforcer.md`. What follows describes `-mainchaingrpcport` (requests only), kept.

A sidechain node of this template follows its mainchain node over JSON-RPC (`-mainchainrpcport`): Backbone
on the eCash networks, Chains on the Chains networks. Backbone also serves the bip300301_enforcer's interface
(gRPC, gRPC-Web and the Connect protocol on one port, `-grpcport`, default 50051; Backbone's
`doc/ecash-grpc.md`), which the LayerTwo sidechains use. This is what that interface can do for this template.

## What is implemented (branch `ecash-grpc`)

`-mainchaingrpcport=<port>` (and `-mainchaingrpcauth=<token>`, Backbone's `-grpcauth`): this node's
**requests** to the mainchain go through the enforcer's interface, with the Connect protocol (JSON over
HTTP/1.1, `MainClient::CallEnforcer`: no HTTP/2 or protobuf library needed); **following** stays on JSON-RPC.
JSON-RPC stays the default for everything.

| What the node does | JSON-RPC (default) | With -mainchaingrpcport |
|---|---|---|
| Follow the mainchain, block by block | `getsidechainevents`, `getblockchaininfo`, `getblockcount` | the same (below) |
| Check its slot | `getsidechain` | the same |
| Ask for blind merged mining | `createbmmrequest` (wallet) | `ValidatorService.GetChainTip` (the tip must be the block the commitment is for), `WalletService.CreateBmmCriticalDataTransaction`; INVALID_ARGUMENT naming `prev_bytes` read as "the mainchain moved", "Outbid:" kept |
| Hand over a withdrawal bundle | `receivewithdrawalbundle` | `BlockProducerService.ProposeWithdrawalBundle` (the blind bundle, legacy serialization, base64) |
| Vouch for its bundle, or for none | `vouchwithdrawalbundle slot [hash]` | **the same, over JSON-RPC** (security review 2026-10-20): the enforcer's interface cannot take a sidechain's word back (no "none", no per-sidechain alarm; its `SetWithdrawalBundleAck` is the operator's vote, over any policy), and a stale bundle left upvoted could be paid twice. The node remembers its word only once the mainchain took it |
| Broadcast the payout once the bundle has its votes | `sendwithdrawalbundle` | nothing: the block producer that holds the bundle pays it out (M6) itself |
| Mainchain addresses of withdrawals | decoded by the node itself (`MainchainScript`, the formats of its own network); the mainchain node is not asked | the same |

Vouching stays on JSON-RPC, which following needs anyway: so a pure bitcoind + enforcer mainchain is not
supported in this mode (it was not before: bitcoind has no `getsidechainevents`). A full gRPC mode would have to
refuse to propose bundles, or need a way to withdraw the word (a per-sidechain abstain/alarm in
`SetWithdrawalBundlePolicy`, asked of LayerTwo Labs: audit 2026-10-19, open question 4).

Chunked replies of the interface are checked (`DecodeChunked`: a chunk's size within the reply, its CRLF, the
256 MiB cap), `sidechain_mainclient_tests/chunked_replies`.

With Backbone the two ports are one node. Tested by `feature_sidechain_ecash.py --mainchaingrpc` (Backbone's
`-grpc -grpcauth` on regtest `-testecash`): BMM blocks requested by the sidechain node itself, the deposit, a
withdrawal proposed, voted and paid, a failed bundle refunded, the reorg case, in which the node itself takes
back its word on the bundle of the branch the mainchain dropped (it is no longer upvoted).

## Why following stays on JSON-RPC

The template's consensus needs, for every mainchain block, the withdrawal bundles of its slot pending after it
**with their scores** (`getsidechainevents` "pending": refund gating and the pending bundle rules,
`sidechain/state.cpp`, `pending_min_score`). The enforcer's interface reports, per block, what was proposed
(Submitted), failed and paid (Succeeded), deposits and the BMM commitment (`GetBlockInfo`,
`SubscribeEvents`, `GetTwoWayPegData`), but scores only as they are now (`GetWithdrawalBundleProposals`).
Two nodes deriving the scores of past blocks differently would fork the sidechain, so a follower over the
enforcer's interface alone needs one of:

1. **Replay the votes** from the blocks (M3/M4 by the enforcer's rules: `handle_m4_ack_bundles`,
   `LeadingBy50`, `RepeatPrevious`, the failure step), from raw blocks of a plain eCash node (`getblock` 2):
   ~600 lines, consensus-critical, and it needs the active sidechains in slot order at every height (M4
   entries are by position), which `GetSidechains` gives only for now (a slot replaced in the past is lost).
2. **The enforcer reporting them**: per-block pending m6ids with vote counts in `BlockInfo` (a new field), or
   `GetWithdrawalBundleProposals` at a block hash. Asked of LayerTwo Labs (audit 2026-10-19, open question 3).
3. **Backbone's JSON-RPC** for following, which is what this branch does; with bitcoind + enforcer as the
   mainchain, the sidechain node then cannot run (bitcoind has no `getsidechainevents`).

The rest of a full gRPC mode is mechanical (≈1,500 lines over the follower's calls):

| Follower need | Over the enforcer's interface |
|---|---|
| `getblockchaininfo`, `getblockcount` (sync, IBD, reorg) | `GetChainTip`, `GetBlockHeaderInfo` (ancestors) |
| per-block hash, previous hash, time | `BlockHeaderInfo` (no median time: `GetBlockHeaderInfo` of the 11 blocks before, computed) |
| `bmm` | `BlockInfo.bmm_commitment` (ConsensusHex: bytes in order) |
| deposits (`destination`, `amount`, `txid`, `burnindex`) | `Deposit` events (`output.address` hex of the destination bytes, `value_sats` what was added, `outpoint`) |
| withdrawal change ("D", bundle, payouts) | `Succeeded` (the M6 transaction: output 0 the new treasury, the payouts after) |
| `bundles` closed (`paid`), `proposed` | `Succeeded`, `Failed`, `Submitted` |
| `pending` with scores | **not available** (above) |
| `getsidechain` (activation height, proposal hash) | `GetSidechains` (`activation_height`, sha256d of `description`) |
| stream instead of polling | `SubscribeEvents` (ConnectBlock / DisconnectBlock) |

Transport: the Connect protocol (unary, JSON) over this template's HTTP/1.1 client is enough for unary calls;
`SubscribeEvents` would need Connect's streaming form (`application/connect+json`, enveloped messages over a
chunked HTTP/1.1 reply), which Backbone and the enforcer serve; polling `GetBlockInfo` with `max_ancestors` is the
simpler start.
