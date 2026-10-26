# Flush on eCash mainnet: readiness

Flush runs in its own drivechain slot of eCash: **46** (`MakeSidechain`, src/kernel/chainparams.cpp, the same on
ecash, ecashbeta and ecashtest; `-sidechainslot` overrides it on regtest only). Betanet slot 99 ("Photon") was a
temporary chain with its own branch (photon-*) and upgrade heights; none of it is on main.

## Rules from genesis

A fresh chain with the final rules only: every rule is active from Flush's first block (height 0, epoch 0).

- DEX with every pool kind; launches with parameters; allocations up to 10 %; payouts and book orders of
  0.00005 ECX at least.
- No treasury, ever. Swap fees: 85 % providers / 15 % security, all taken in ECX in a pool of ECX. Launch curve:
  1.15 %, 80 % creator / 20 % security. Graduation into a locked full-range position at 1 % (60 % creator /
  40 % security). Fee destinations (creator, buy back & burn, split). Keep-control launches are marked Mintable.
- Blocks of stake: 24 hours without BMM before they halt (5760 slots of 15 s); seed and set 48 hours ahead; the
  drivechain undo depth is 12960 blocks; the least prune target is 24000 MiB; the liveness fix is on.
- Unbonding: a week of anchors (1008; 432 on ecashtest).

## Defaults (not consensus)

- Anchor bid ("Fees + prevent stalling"):
  - The base is max(relay floor, ceil(next-block fee estimate in sat/vB) × 189 vB).
  - The bid is max(base, fee share), with a flat 4 × base boost near a stall.
  - No bid when there is nothing to anchor: no fees, no pending peg or staking action, no stall pressure.
  - The maximum is 0.001 ECX; the cap is 0.01 ECX a day.

## To launch

1. **Activate slot 46 on eCash:** a proposal mined, then the acks to the threshold. Set `main_activation_height`
   to the block that activated it, and set the genesis time.
2. **Genesis validators:** keys made on their own machines, listed in `pos.genesis_validators`, with
   `min_active_validators` set to match.
3. **Seeds and nodes:** DNS seeds or fixed seeds for port 8255; the API node and the site pointed at mainnet.
4. **Release:** builds of the node and the app.

Not ready yet:
- The mainnet genesis and activation height (step 1).
- The genesis validator keys (step 2).
- The seeds (step 3).
- A mainnet site deployment.
