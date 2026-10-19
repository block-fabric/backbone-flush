# Layers: the ports of its e2e scripts are 49400 and up (each takes four), apart from the other chains'.
# The page script of FastBlocks' own pages, run against a live mainchain as webui_e2e_fastblocks
# (webui_local_e2e of test/CMakeLists.txt, from the template). fastblocks.js: this node a validator of the
# genesis set (test_framework/fastblocks.py's first key), a second key in its key file to bond to, its own
# BMM bids on; staking, bonds, the explorer's kinds, the income, finality, slashing. The pictures at 700 px:
# run_sidechain.py --script fastblocks.js --width 700 with the same options.
webui_local_e2e(fastblocks 49400
  --file=validator.key=1100000000000000000000000000000000000000000000000000000000000001\\n1100000000000000000000000000000000000000000000000000000000000002
  --app-arg=-posvalidators=100f6d8cbf94afb6fc58e9c384b9b3a6516091373a83c869f4e24a9d2bb4a494
  --app-arg=-validatorkeyfile={tmp}/validator.key
  --app-arg=-validatorpayout=rsc1qqghd0tuw5wc9k0ec22t8qr7gy3tz5c5sx6gvwx
  --app-arg=-validatoranchorbid=0.00001)
# Simple mode (fastblocks_simple.js): the home's one card, the Staking card, a stake through it and Stop staking
# (a danger on the desktop), the node a validator as for fastblocks.js.
webui_local_e2e(fastblocks_simple 49410
  --js-var SIMPLE=true
  --file=validator.key=1100000000000000000000000000000000000000000000000000000000000001\\n1100000000000000000000000000000000000000000000000000000000000002
  --app-arg=-posvalidators=100f6d8cbf94afb6fc58e9c384b9b3a6516091373a83c869f4e24a9d2bb4a494
  --app-arg=-validatorkeyfile={tmp}/validator.key
  --app-arg=-validatorpayout=rsc1qqghd0tuw5wc9k0ec22t8qr7gy3tz5c5sx6gvwx
  --app-arg=-validatoranchorbid=0.00001)
# The page scripts of BitAssets' own pages, run against a live mainchain as webui_e2e_<name>
# (webui_local_e2e of test/CMakeLists.txt, from the template).
# bitassets.js: an asset issued, minted, pooled, swapped, auctioned and sent.
webui_local_e2e(bitassets 49420)
# bitassets_simple.js: Simple mode, as a user chooses it: Create a token, Swap from its plain quote, My assets.
webui_local_e2e(bitassets_simple 49430 --js-var SIMPLE=true)
# bitassets_logo.js: an asset registered with a logo (picker, confirmation, lists, Home, Overview) and sent (Transactions).
webui_local_e2e(bitassets_logo 49700)
# ba_bulk.js: 300 assets made by a script while the Transactions page shows (user feedback 2026-10-19, item 10): at most
# 10 frames over 50 ms, the UI thread of the host answering within a second.
webui_local_e2e(ba_bulk 49720 --js-var COUNT=300 --js-var "PAGE=\"transactions\"" --js-var MAXLONG=10)
# ba_ignore.js: a spam asset received, ignored from its notice, gone from Home, My assets and the Transactions page, back once unignored.
webui_local_e2e(ba_ignore 49730)
# ba_feedback.js: pictures of the pages the user feedback of 2026-10-19 changed.
webui_local_e2e(ba_feedback 49740)
# ba_tickers.js: tickers and display names (ticker@0): the wizard (ticker uppercased, display name), its confirmation, the
# list, the asset page, the compact Your assets of the Home and the Overview, the display name changed by its controller,
# the "this wallet" chip of Transactions opened in the explorer, the addresses and roles in the registration's details.
webui_local_e2e(ba_tickers 49760 --app-arg=-testactivationheight=ticker@0)
