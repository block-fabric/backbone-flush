# The page script of FastBlocks' own pages, run against a live mainchain as webui_e2e_fastblocks
# (webui_local_e2e of test/CMakeLists.txt, from the template). fastblocks.js: this node a validator of the
# genesis set (test_framework/fastblocks.py's first key), a second key in its key file to bond to, its own
# BMM bids on; staking, bonds, the explorer's kinds, the income, finality, slashing. The pictures at 700 px:
# run_sidechain.py --script fastblocks.js --width 700 with the same options.
webui_local_e2e(fastblocks 29840
  --file=validator.key=1100000000000000000000000000000000000000000000000000000000000001\\n1100000000000000000000000000000000000000000000000000000000000002
  --app-arg=-posvalidators=100f6d8cbf94afb6fc58e9c384b9b3a6516091373a83c869f4e24a9d2bb4a494
  --app-arg=-validatorkeyfile={tmp}/validator.key
  --app-arg=-validatorpayout=rsc1qqghd0tuw5wc9k0ec22t8qr7gy3tz5c5sx6gvwx
  --app-arg=-validatoranchorbid=0.00001)
# The page scripts of BitAssets' own pages, run against a live mainchain as webui_e2e_<name>
# (webui_local_e2e of test/CMakeLists.txt, from the template).
# bitassets.js: an asset issued, minted, pooled, swapped, auctioned and sent.
webui_local_e2e(bitassets 29770)
