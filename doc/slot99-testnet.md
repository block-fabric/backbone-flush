# Flush slot-99 test network (TEMPORARY)

This branch (`photon-slot`) runs **Flush on eCash betanet, sidechain slot 99**. Slot 99 was voted in for
Photon, and nobody uses it. The network is temporary. Flush will move to its own slot (46) later, and this
chain will then be dropped. Do not keep value on it.

## Build and run

Build as on main (README.md). Then select the variant with one option:

```
flushd -chain=ecashbeta -sidechainslot=99 -mainchaingrpc=127.0.0.1:50051
flush-app -chain=ecashbeta -sidechainslot=99 -mainchaingrpc=127.0.0.1:50051
```

Or put it in `flush.conf`. Put `chain=ecashbeta` and `sidechainslot=99` at the top, and the other options under
`[ecashbeta]`.

- Data goes to `~/.flush/betanet-photon`. The ports are P2P 38995 and RPC 38994.
- The app shows an orange "Temporary: Flush on betanet slot 99 (Photon)" strip above every page.
- Addresses start with `bflx`.

For the mainchain you need either of these, both reachable over gRPC:

- any eCash betanet node with **bip300301_enforcer** (`--network-preset=betanet`, its `--serve-grpc-addr`);
- **Backbone** (`backboned -chain=ecashbeta -grpc`).

Point `-mainchaingrpc` at it. Neither serves TLS, so to reach one on another machine, use an SSH tunnel
(`ssh -N -L 50051:127.0.0.1:50051 host`). Following the mainchain needs no wallet there. BMM bids and deposits
made by the node itself do need one.

## Seed peers

```
addnode=nodes.blockfab.org:38995
addnode=5.78.75.193:38995
```

## Deposit (betanet to slot 99)

1. In flush-app, open **Receive** and copy the deposit address, `s99_bflx1..._xxxxxx`. You can also run
   `flush-cli getdepositaddress`.
2. Make a BIP300 deposit into **slot 99** with that string. Any of these works: BitWindow, the enforcer's
   `WalletService.CreateDepositTransaction`, or `backbone-cli createsidechaindeposit 99 "<s99_...>" <amount>`.
   - A plain send to the escrow is not a deposit.
   - Make one deposit per betanet block.
   - Try a small amount first.
3. The coins appear on Flush once a Flush block includes the deposit, a block or two after it confirms on
   betanet.

## Withdraw (slot 99 to betanet)

1. In flush-app, open **Withdraw**: a betanet address, the amount and the mainchain fee. You can also run
   `flush-cli createwithdrawal <betanet address> <amount> <fee>`.
2. Withdrawals are batched into a bundle, which betanet miners must vote for. It is paid after more than 13,150
   votes in 26,300 blocks, so on betanet expect it to take a long time. Until the bundle is proposed, you can
   take a withdrawal back.

## Caveats

- **Temporary.** The chain will be dropped when Flush gets slot 46. Its first block and message start are its
  own, so it never mixes with slot-46 Flush or with Photon.
- If Photon (or anything else) ever takes slot 99 with a new activation, these nodes stop by design. They pin the
  activation at height 968998.
- **Validators.** There are three genesis validators (operators' machines) and a 0.1 ECX minimum bond. With fewer
  than 3 active validators, blocks come only from BMM, one per betanet block.
- Betanet ECX are test coins of eCash's betanet.
