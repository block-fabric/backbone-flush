# Flush on eCash mainnet: readiness

Flush runs in its own drivechain slot of eCash: **46** (`MakeSidechain`, src/kernel/chainparams.cpp, the same on
ecash, ecashbeta and ecashtest; `-sidechainslot` overrides it on regtest only). Betanet slot 99 ("Photon") was a
temporary chain with its own branch (`flush-betanet`, before it `photon-*`) and upgrade heights; none of it is on main.

## Rules from genesis

A fresh chain with the final rules only: every rule is active from Flush's first block (height 0, epoch 0).

- DEX with every pool kind; launches with parameters; allocations up to 10 %; payouts and book orders of
  0.00005 ECX at least.
- No treasury, ever. Swap fees: 85 % providers / 15 % security, all taken in ECX in a pool of ECX. Launch curve:
  1.15 %, 25 % security / 75 % split as the launch says (upgrade v7's fee split: the creator, up to 3 addresses paid
  by the coinbase, buy back & burn; by default all to the creator; v5's fee destinations map onto it). Graduation at
  100 ECX into a locked full-range position at 1 % (60 % the split's / 40 % security). Keep-control launches are
  marked Mintable.
- The DEX's ECX payouts (swaps, curve sales, pools, auctions, the split's addresses) in the coinbase of the block with
  the trade, every block (upgrade v7's instant payouts); deposits, withdrawals, unbonds and rewards with the anchors.
- The buybacks scanned 64 a block from a cursor, those that cannot buy yet out of the index (upgrade v8): a block's
  work bounded whatever the count of launches.
- Upgrade v9 (Flush 8.3.2): the DEX rules of that release.
- Blocks of stake: 24 hours without BMM before they halt (5760 slots of 15 s); seed and set 48 hours ahead; the
  drivechain undo depth is 12960 blocks; the least prune target is 24000 MiB; the liveness fix is on.
- Unbonding: a week of anchors (1008; 432 on ecashtest).

## Defaults (not consensus)

- Anchor bid ("Fees + prevent stalling"):
  - The base is max(relay floor, ceil(next-block fee estimate in sat/vB) × 189 vB).
  - The bid is max(base, fee share), with a flat 4 × base boost near a stall.
  - No bid when there is nothing to anchor: no fees, no pending peg or staking action, no stall pressure.
  - The maximum is 0.001 ECX; the cap is 0.01 ECX a day.

- Network: P2P port 8255, RPC port 8254 (betanet slot 99 is 38995/38994, betanet slot 46 38255/38254). DNS seed
  `seed.mainnet.flush.markets` (in `vSeeds`; A records of 216.128.183.114, 5.78.75.193 and 46.224.180.56, which must
  then serve mainnet at 8255); `-addnode` stays the fallback (no fixed seeds).

## To launch

1. **Activate slot 46 on eCash:** a proposal mined, then the acks to the threshold. Set `main_activation_height`
   to the block that activated it, and set the genesis time.
2. **Genesis validators:** keys made on their own machines, listed in `pos.genesis_validators`, with
   `min_active_validators` set to match.
3. **Seeds and nodes:** the DNS seed `seed.mainnet.flush.markets` is in the release; its servers must run mainnet
   nodes at port 8255. The API node and the site pointed at mainnet.
4. **Release:** builds of the node and the app.

Done:
- **Step 1 (Flush 8.3.3):** slot 46 activated in eCash block 978,331
  (`000000000000000021edfa06433c6e13d6a06d026d6fdc26b3b894e8b4bf1f65`, 2026-10-29 23:17:51 UTC). That is
  `main_activation_height`, and the block's time (1793315871) is the genesis time.

Not ready yet:
- The genesis validator keys (step 2): none are listed; the chain is merged mined until validators bond.
- Mainnet nodes behind the DNS seed (step 3).
- A mainnet site deployment.
