# The page scripts of BitAssets' own pages, run against a live mainchain as webui_e2e_<name>
# (webui_local_e2e of test/CMakeLists.txt, from the template).
# bitassets.js: an asset issued, minted, pooled, swapped, auctioned and sent.
webui_local_e2e(bitassets 29770)
# bitassets_simple.js: Simple mode, as a user chooses it: Create a token, Swap from its plain quote, My assets.
webui_local_e2e(bitassets_simple 41760 --js-var SIMPLE=true)
# bitassets_logo.js: an asset registered with a logo (picker, confirmation, lists, Home, Overview) and sent (Transactions).
webui_local_e2e(bitassets_logo 41790)
