# TEMPORARY: Flush on eCash betanet slot 99 (Photon's)

Betanet's slot 99 was voted in for **Photon** ("Sidechain using post-quantum cryptography", activated at mainchain
height 968,998, escrow script `b7016351`). Nobody deposited to it or proposed a bundle in it. Flush's own slot, 46,
is not active on betanet. Until it is, this branch (`photon-slot`) runs Flush in slot 99. Drop the whole variant
once Flush has slot 46 (search for "TEMPORARY" and `IsBetanetSlot99`).

## How to select it

```
flushd -chain=ecashbeta -sidechainslot=99      # or flush-app; or chain=ecashbeta + sidechainslot=99 in flush.conf
```

`-sidechainslot` was a regtest-only option. On the eCash networks it now accepts only `ecashbeta` with `99`.
Any other value, or `ecash`/`ecashtest`, is refused at startup. It is a startup option on an existing network,
not a new network name, so the RPC `chain`, the app's network table and the address prefixes stay betanet's.
A new `ChainType` would have touched every switch on networks, the app's `chain.json`, the links and the public
API, and all of that would have to be removed again later.

What the variant changes (`CEcashSidechainParams`, `CreateBaseChainParams`):

| | slot 46 betanet | slot 99 (this variant) |
|---|---|---|
| First block | "Flush on eCash betanet 21/Oct/2026" | "TEMPORARY Flush on eCash betanet slot 99 (Photon) 22/Oct/2026", time 1792627200 |
| Message start | `flx` 0x22 | `flx` 0x99 |
| P2P / RPC ports | 38255 / 38254 | 38995 / 38994 |
| Data folder | `betanet/` | `betanet-photon/` |
| `main_activation_height` | 0 (not active) | 968998: the node stops if another sidechain takes slot 99 |
| Addresses | `bflx` | `bflx` (the same, so the app's address checks keep working) |

Because of its own first block and message start, it never mixes with a slot-46 Flush, nor with a Photon chain.
Every other rule comes from the same code: enforcer mode (`-mainchaingrpc`, `main_events_only`), the 6-hour gap
tolerance, the fee splits, the DEX, launches and bundles.

The node sets a warning (`node::Warning::TEMPORARY_NETWORK`): "Temporary: Flush on betanet slot 99 (Photon)...".
It shows in `getblockchaininfo`/`getnetworkinfo` "warnings", and in the app as an orange strip above every page
(`TemporaryNetwork.svelte`).

## Bootstrap on betanet

- **Validators**: the variant has three genesis validators, keys made each on its own machine (the lab node server,
  5.78.75.193, the UI PC). It also has `min_active_validators` 3 and `min_stake` 0.1 ECX, where slot-46 betanet has
  no genesis validators, 4 and 1 ECX. So blocks of stake run from the first block, and anyone can join by bonding
  0.1 ECX or more (the bond counts about ten hours later, the set lead). If fewer than 3 validators are active, the
  chain falls back to BMM blocks, as any Flush network does.
- **BMM / anchors**: the lab validator bids `-validatoranchorbid=0.00001` per betanet block through the wallet
  "flush-bmm" of the lab's backboned (WalletService of its gRPC). The others bid 0. Only a bid that gets mined is
  paid. Budget: about 0.01-0.02 ECX a week, with the M8 fees.
- **First deposit**: an M5 into slot 99 creates the Ctip (Photon's slot had none). Flush credits the payload as
  the wrapped `s99_<address>_<check>` and as the bare address alike (`sidechain::DepositScript`).

The deployment files are in `contrib/photon-slot/`: the validator unit, the SSH tunnel unit for a machine without
a betanet node, and `make-conf.sh`.
