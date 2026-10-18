# chains-app: parity contract with chains-qt

> **History.** chains-qt (`src/qt`) was removed from the tree by the commit "Remove the Qt GUI"
> (`git log --grep='Remove the Qt GUI'`), after the work this document records: chains-app is the GUI of the
> build. This document stays as the record of the parity; the `src/qt` sources it cites are those
> of the parent of that commit (`git show <commit>^:src/qt/<file>`).

`chains-app` replaces `chains-qt`. It must do everything the Qt GUI does. This list
is the contract: every function of `src/qt` is one line, with the Qt source it
comes from. A line is ticked once `chains-app` does it and a test or a screenshot
shows it.

Legend: `[x]` done in chains-app, `[~]` partly done (the note says what is left),
`[ ]` not done yet, `[–]` left out by a decision of the user (the note says which). IDs are stable: later commits and tests refer to them.

How chains-app is built and tested: doc/webui.md. Tests that cover an item:
unit tests of the host (`test_chains-app`: bridge, events, app settings, assets,
single instance, node events), of the interface (`npm test` in src/webui/app:
units, addresses, URIs, sync label, tables, bridge, state, components) and the
end-to-end smoke test (src/webui/test/e2e: A05, A12, B01, D01, D04, D05, E11,
E12, F01, F05, T01–T03, the bridge and the escaping of chain text), and the
end-to-end wallet test (src/webui/test/e2e/wallet.js: E01–E04, E06–E10, F02, G01–G05,
G07, G08, H01–H03, I01–I06, J02, K01–K03, N02). The wallet functions of the
host are tested in `test_chains-app` (walletapi_tests: the same calls as chains-qt,
its rules and messages), the pages in `npm test` (wallet, dialogs, send page,
transactions page). The node window, the options, the tools and the sidechain nodes of
the host are tested in `test_chains-app` (nodeapi_tests: the console with the cases of
rpcnestedtests.cpp, the options with the rules of OptionsModel, hashes, merkle trees,
deposit addresses, stamps, sidechain nodes with pretend programs), their pages in
`npm test` (node-tools.test.ts, node-pages.test.ts), and all of it end to end in
src/webui/test/e2e/node.js (run_node.py): a sidechain proposed, acked and activated,
deposits, a withdrawal bundle voted on, the explorer, solo mining, a stamp, a proof of
funds, a multisig spend, the crypto tools, the console, peers banned and unbanned, the
settings and their reset, the tray and the notifications of a pretend desktop, the
sidechain nodes. The fixes of the audit (doc/webui-audit.md) are tested end to end in
src/webui/test/e2e/run_audit.py (audit.js and the other audit_*.js scripts), their parts
in `test_chains-app` (hardening_tests, i18n_tests, the audit suite of walletapi_tests, the
options journal) and `npm test` (audit-ui, history, rpcpolicy, contrast, i18n).

## A. Start-up, shutdown, process

| ID | Function | Qt source | Status |
|----|----------|-----------|--------|
| A01 | Parse the command line like chainsd; errors shown and exit 1 | bitcoin.cpp `GuiMain` | [x] |
| A02 | `-help` / `-version` print the help and exit (Qt shows a dialog; chains-app prints to stdout) | bitcoin.cpp, utilitydialog.cpp `HelpMessageDialog::showOrPrint` | [x] |
| A03 | Reject unexpected tokens on the command line, and options after a payment URI | bitcoin.cpp `GuiMain` | [x] |
| A04 | GUI options `-choosedatadir`, `-lang`, `-min`, `-resetguisettings`, `-splash` (`-uiplatform` is Qt only) | bitcoin.cpp `SetupUIArgs` | [x] `-lang` and the language option choose the catalog (src/webui/i18n.cpp), made from the catalogs of chains-qt (scripts/i18n.mjs); the page and the messages of the node are translated (e2e: run_audit.py `-lang=fr`) |
| A05 | Intro: choose the data directory on first start or with `-choosedatadir`; default or custom dir; size estimate; prune choice; create dir and `wallets/` | intro.cpp `Intro::showIfNeeded` | [x] |
| A06 | Remember the chosen data directory (Qt: QSettings `strDataDir`, `fReset`) | intro.cpp | [x] (app config file, see design) |
| A07 | Read chains.conf and settings.json (`InitConfig`); on a corrupt settings file ask to reset or abort; on a write failure say so | bitcoin.cpp `ErrorSettingsRead`, `ErrorSettingsWrite` | [x] |
| A08 | Network style: window title and app name per network (`[testnet]`, `[signet]`, `[regtest]`), icon tint | networkstyle.cpp | [x] title and badge; the icon of the window and of the tray tinted as NetworkStyle tints it (src/webui/icontint.cpp) |
| A09 | Splash / init progress: init messages, wallet loading progress, "press q to shut down" | splashscreen.cpp | [x] (init view; Esc/q asks to shut down) |
| A10 | Node init in-process on its own thread (`baseInitialize`, `appInitMain`), runaway exception dialog | bitcoin.cpp, initexecutor.cpp | [x] |
| A11 | Load the wallets at start (`LoadWalletsActivity`), `-min` keeps it minimized | walletcontroller.cpp | [x] |
| A12 | Clean shutdown: hide windows, show "shutting down, do not shut down the computer", `appShutdown` on a thread, exit status of the node | bitcoin.cpp `requestShutdown`, utilitydialog.cpp `ShutdownWindow` | [x] |
| A13 | Poll for a shutdown requested from elsewhere (RPC `stop`, signal) | bitcoin.cpp `pollShutdownTimer`, bitcoingui.cpp `detectShutdown` | [x] |
| A14 | Windows: block the session end until the node has stopped | winshutdownmonitor.cpp | [x] (window_win32.cpp: WM_QUERYENDSESSION) |
| A15 | macOS: App Nap off while syncing, dock icon click shows the window | macos_appnap.mm, macdockiconhandler.mm | [ ] (macOS port) |
| A16 | Prune setting chosen in the intro applied (`InitPruneSetting`) | bitcoin.cpp | [x] |
| A17 | Fatal error / non-fatal internal error dialogs with the bug report link | bitcoin.cpp `handleRunawayException`, `handleNonFatalException` | [x] fatal error view; an exception a host method did not expect shows the internal error dialog with the bug report address (Bridge::SetInternalError) |
| A18 | `-resetguisettings` resets the GUI settings (and shows the intro again) | optionsmodel.cpp `Reset`, intro.cpp | [x] |

## B. Single instance, URIs, files

| ID | Function | Qt source | Status |
|----|----------|-----------|--------|
| B01 | A second start with a `chains:` URI hands the URI to the running app and exits | paymentserver.cpp `ipcSendCommandLine` | [x] |
| B02 | The running app listens for URIs from other starts (one listener per data directory and network) | paymentserver.cpp `startLocalServer` | [x] |
| B03 | URIs given at start are kept until the UI is ready, then handled | paymentserver.cpp `uiReady`, `savedPaymentRequests` | [x] |
| B04 | Parse BIP21 `chains:` URIs (address, amount, label, message, unknown `req-` params rejected) | guiutil.cpp `parseBitcoinURI` | [x] |
| B05 | `chains://` rejected with "use chains: instead" | paymentserver.cpp | [x] |
| B06 | Open URI dialog (Banking > Open URI…) | openuridialog.cpp | [x] Send page, "Open URI…" (pages/Send.svelte) |
| B07 | URI fills the Send page, after checking the address is valid for this network | paymentserver.cpp `handleURIOrFile`, bitcoingui.cpp `handlePaymentRequest` | [x] pages/Send.svelte `pasteUri` (fills the first empty entry, checks the network) |
| B08 | Drag and drop a URI onto the window | bitcoingui.cpp `dropEvent` | [x] App.svelte `ondrop`: `chains:` links dropped on the window go to the send page (text dropped into a field stays there) |
| B09 | Register as the handler of `chains:` (desktop file / registry) | share/, paymentserver.cpp | [~] share/chains-app.desktop (installed on Linux with `MimeType=x-scheme-handler/chains`); Windows: urischeme_win.cpp (HKCU, at start) and the NSIS installer; the Info.plist of macOS comes with that port |
| B10 | A second start without URI brings the running window to the front (Qt fails on the datadir lock) | (improvement) | [x] |

## C. Main window, navigation, menus, shortcuts

| ID | Function | Qt source | Status |
|----|----------|-----------|--------|
| C01 | Sidebar: Wallet (Overview, Send, Receive, Transactions), Chain (Sidechains, Sidechain Nodes, Mine, Block Explorer), Node (Node Window, Settings), Theme | bitcoingui.cpp `createToolBars` | [x] shell; pages land in later phases |
| C02 | Shortcuts Alt+1..5 for the pages, Ctrl+Q quit, Ctrl+Shift+C console, Ctrl+Shift+D node window, Ctrl+Shift+M mask values, Ctrl+M minimize, Ctrl+W closes a dialog, Ctrl+/Ctrl- font size | bitcoingui.cpp, guiutil.cpp | [x] pages, quit, console, node window and its tabs (Ctrl+I/T/N/P), mask, Ctrl+M, font size, Esc closes dialogs (lib/keyboard.ts) |
| C03 | Menu "Your Wallet": create, open (list of wallets on disk), close, close all, migrate (list + restore-and-migrate a file), receiving addresses (sending addresses left out: J01), backup, restore, export watch-only, encrypt, change passphrase, mask values | bitcoingui.cpp `createMenuBar`, `createActions` | [x] lib/commands.svelte.ts (the menus in every layout and the palette, doc/webui-layouts.md) → pages/Wallets.svelte, Receive → Addresses |
| C04 | Menu "Banking": send, receive, transactions, open URI, proof of funds, multisig lounge, load PSBT from file, from clipboard | bitcoingui.cpp | [x] send, receive, transactions, open URI, PSBT from file/clipboard (pages/Psbt.svelte), proof of funds (pages/Proof.svelte), multisig lounge (pages/Multisig.svelte) |
| C05 | Menu "Use Chains": timestamp file, sign message, verify message, sidechains | bitcoingui.cpp | [x] timestamp file (pages/Timestamp.svelte), sign and verify message (pages/Messages.svelte), sidechains (pages/Sidechains.svelte) |
| C06 | Menu "Chains": sidechain nodes, solo mine, sidechain proposals and votes | bitcoingui.cpp | [x] sidechain nodes (pages/SidechainNodes.svelte), solo mine (pages/Mine.svelte), proposals and withdrawal votes (pages/Sidechains.svelte) |
| C07 | Menu "Crypto Tools": block explorer, hash calculator, merkle tree, address decoder | bitcoingui.cpp | [x] block explorer (pages/Explorer.svelte), hash calculator, merkle tree, address decoder (pages/Crypto.svelte) |
| C08 | Menu "Window": minimize, (macOS zoom, main window), node window tabs with their shortcuts | bitcoingui.cpp | [x] minimize, the node window tabs with their shortcuts; the zoom and main-window items of macOS come with that port |
| C09 | Menu "This Node": node window, options, theme (looks + custom…), font size +/-, command-line options, about, quit | bitcoingui.cpp | [x] node window, options, sync status, theme, font size, command-line options, about, quit |
| C10 | Wallet selector when more than one wallet is loaded | bitcoingui.cpp `m_wallet_selector` | [x] |
| C11 | Window title: client name, network, current wallet | bitcoingui.cpp `updateWindowTitle` | [x] |
| C12 | Actions enabled only with a wallet loaded; history disabled in privacy mode | bitcoingui.cpp `setWalletActionsEnabled`, `enableHistoryAction` | [x] wallet actions disabled without a wallet, transactions disabled in privacy mode (views/Shell.svelte) |
| C13 | Remember the window geometry | bitcoingui.cpp `MainWindowGeometry` | [x] size and maximized state kept in chains-app.json (`window`), restored at the next start (app.cpp, window_gtk.cpp) |
| C14 | Status tips of every action (shown in the status bar on hover) | bitcoingui.cpp | [x] the status tips of chains-qt on the menu items, and the summaries of the pages, show in the status bar under the pointer |
| C15 | About dialog (version, licence) and command-line options dialog | utilitydialog.cpp `HelpMessageDialog` | [x] |
| C16 | Themed window frame with the menus in the title bar | themedframe.cpp | [x] the integrated title bar of the layouts (doc/webui-layouts.md): the page draws it in the colours of the theme (minimize, maximize or restore, close; drag to move, double click to maximize, handles at the edges resize) on Linux (GTK) and Windows (window_win32.cpp); the menus of chains-qt in the Menus button of each layout (in the workspace's title bar itself) and in the palette; Settings → Appearance → "Use the title bar of the desktop" gives the frame back; macOS keeps the desktop frame until its port |

## D. Status bar and notifications

| ID | Function | Qt source | Status |
|----|----------|-----------|--------|
| D01 | Unit selector in the status bar (click to choose CHN, mCHN, µCHN, sat) | bitcoingui.cpp `UnitDisplayStatusBarControl` | [x] |
| D02 | HD and encryption status icons of the current wallet (locked / unlocked) | bitcoingui.cpp `setHDStatus`, `setEncryptionStatus` | [x] components/StatusBar.svelte: HD (on / off) and the lock of an encrypted wallet (locked / unlocked), watch-only |
| D03 | Proxy icon when a proxy is used | bitcoingui.cpp `updateProxyIcon` | [x] components/StatusBar.svelte: an icon with "Proxy is enabled: <ip:port>" when IPv4 peers are reached through a proxy (getnetworkinfo) |
| D04 | Connections icon with peer count; menu: show peers tab, enable/disable network activity | bitcoingui.cpp `updateNetworkState`, `setNetworkActive` | [x] |
| D05 | Sync icon and label: connecting, syncing headers (%), pre-syncing headers, processing blocks on disk, reindexing, "x behind", up to date | bitcoingui.cpp `setNumBlocks`, `updateHeadersSyncProgressLabel` | [x] |
| D06 | Progress bar for long operations (rescan, wallet load) via `showProgress` | bitcoingui.cpp `showProgress` | [x] |
| D07 | Modal sync overlay: blocks left, last block time, progress, progress per hour, estimated time; hide; opened from the out-of-sync warning | modaloverlay.cpp | [x] components/SyncOverlay.svelte (lib/syncoverlay.ts: blocks left, last block time, progress, per hour, time left), from the out-of-sync warning, the sync label and This node → Sync status |
| D08 | Message boxes from the node (`ThreadSafeMessageBox`, `ThreadSafeQuestion`), errors, warnings | bitcoingui.cpp `message`, `ThreadSafeMessageBox` | [x] |
| D09 | Desktop notification for incoming and sent transactions (date, amount, wallet, type, label, address) | bitcoingui.cpp `incomingTransaction`, notificator.cpp | [x] components/TxNotifier.svelte → `app.notify` → org.freedesktop.Notifications over D-Bus (tray_gtk.cpp `DesktopNotify`, with the icon and the urgency of Notificator); none during IBD; many make one; tested with a pretend desktop (e2e/fake_desktop.py) |
| D10 | Alerts / warnings from the node (`getWarnings`) | clientmodel.cpp | [x] |
| D11 | Tray icon with menu (show/hide, send, receive, sign, verify, options, node window, quit); minimize to tray; click toggles | bitcoingui.cpp `createTrayIcon`, `createTrayIconMenu` | [x] tray_gtk.cpp: StatusNotifierItem with a com.canonical.dbusmenu menu (show/hide, send, receive, sign, verify, options, node window, quit), the XEmbed tray of GTK where there is no StatusNotifierWatcher; a click shows or hides the window; minimize to the tray, minimize on close (Settings → Window); tested with a pretend desktop |
| D12 | Out-of-sync warning labels on overview and send | overviewpage.cpp, walletview.cpp | [x] pages/Overview.svelte: the out-of-sync warnings of the balances and the recent transactions (labelWalletStatus, labelTransactionsStatus), which open the sync overlay, as chains-qt; the send page has the fee-estimation warning of SendCoinsDialog (the fallback fee), as chains-qt, and also a notice of its own while the node catches up (Send.svelte), which chains-qt does not have |

## E. Wallet management

| ID | Function | Qt source | Status |
|----|----------|-----------|--------|
| E01 | Create wallet: name, encrypt, disable private keys, blank, external signer | createwalletdialog.cpp, walletcontroller.cpp `CreateWalletActivity` | [x] pages/Wallets.svelte (options and their rules of CreateWalletDialog), src/webui/walletapi.cpp `wallets.create` |
| E02 | Open wallet from the list of wallets in the data directory | walletcontroller.cpp `OpenWalletActivity` | [x] pages/Wallets.svelte list, src/webui/walletapi.cpp `wallets.list`/`wallets.open` |
| E03 | Close wallet / close all wallets (with the pruning warning) | walletcontroller.cpp `closeWallet`, `closeAllWallets` | [x] pages/Wallets.svelte, src/webui/walletapi.cpp `wallets.close` (as WalletController: not loaded at start any more) |
| E04 | Restore wallet from a backup file | walletcontroller.cpp `RestoreWalletActivity` | [x] pages/Wallets.svelte, src/webui/walletapi.cpp `wallets.restore` |
| E05 | Migrate a legacy wallet; restore-and-migrate a file; watch-only/solvable wallets reported | walletcontroller.cpp `MigrateWalletActivity` | [x] pages/Wallets.svelte, src/webui/walletapi.cpp `wallets.migrate`/`wallets.restoreMigrate` (load option, passphrase, watch-only/solvables reported); not tested end to end: v32 cannot make the legacy wallet to migrate |
| E06 | Backup wallet to a file | walletview.cpp `backupWallet` | [x] pages/Wallets.svelte, src/webui/walletapi.cpp `wallet.backup` |
| E07 | Export a watch-only wallet | bitcoingui.cpp `m_export_watchonly_action` | [x] pages/Wallets.svelte, src/webui/walletapi.cpp `wallet.exportWatchOnly` |
| E08 | Encrypt wallet (passphrase twice, strength hint, caps-lock warning, final warning) | askpassphrasedialog.cpp | [x] components/DialogHost.svelte (twice, caps lock, show) and pages/Wallets.svelte (the warnings); no strength meter, as Qt has none |
| E09 | Change passphrase | askpassphrasedialog.cpp | [x] pages/Wallets.svelte `changePassphrase` |
| E10 | Unlock on demand for an operation that needs the keys | askpassphrasedialog.cpp, walletmodel.cpp `requestUnlock` | [x] lib/dialogs.svelte.ts `withUnlock`: unlocked for the call and locked again (UnlockContext); also "Unlock for a while" (walletpassphrase with a timeout) and "Lock now" |
| E11 | Mask values (privacy mode) | bitcoingui.cpp `m_mask_values_action`, overviewpage.cpp | [x] |
| E12 | Wallet list follows wallets loaded/unloaded by RPC | walletcontroller.cpp | [x] |

## F. Overview page

| ID | Function | Qt source | Status |
|----|----------|-----------|--------|
| F01 | Balances: available, pending, immature, total | overviewpage.cpp, forms/overviewpage.ui | [x] |
| F02 | Recent transactions of the wallet, click opens transactions | overviewpage.cpp, transactionoverviewwidget.cpp | [x] pages/Overview.svelte (5 latest; a click opens the transactions with it selected); and a balance chart (components/BalanceChart.svelte) |
| F03 | Latest blocks and latest mempool transactions of the chain; double click opens the block explorer | chainactivity.cpp | [x] pages/Overview.svelte: latest blocks and latest mempool transactions (only new ones looked up, as ChainActivity); a click opens the block explorer |
| F04 | Out-of-sync warning, click shows the sync overlay | overviewpage.cpp | [x] pages/Overview.svelte (opens the node window until the sync overlay comes) |
| F05 | Privacy mode masks the amounts | overviewpage.cpp `setPrivacy` | [x] |

## G. Send page

| ID | Function | Qt source | Status |
|----|----------|-----------|--------|
| G01 | Several recipients: address (validated, paste, address book), label, amount, subtract fee, use available balance, remove entry | sendcoinsentry.cpp, sendcoinsdialog.cpp | [x] pages/Send.svelte (paste, label, unit, subtract fee, use available balance, remove; URI message shown). **User decision (2026-10-15):** no address book button on the recipient field (no sending addresses, J01) |
| G02 | Fee: recommended with confirmation target, custom fee per kvB, minimum fee warning, fee section collapsible | sendcoinsdialog.cpp | [x] pages/Send.svelte (folding fee section, targets 2–1008 blocks, smart fee and its fallback warning, custom fee per kvB not below the required fee), src/webui/walletapi.cpp `wallet.fee` |
| G03 | Coin control: inputs dialog (tree/list, lock/unlock, copy), custom change address, quantity/bytes/amount/fee/after fee/change labels with copy | coincontroldialog.cpp, sendcoinsdialog.cpp | [x] **User decision (2026-10-15):** in the Send page, not in a dialog nor behind a setting: a **Choose coins** switch (kept as `prefs.coinControl`, the setting of Settings → Wallet) opens components/CoinPanel.svelte inline — the coins of the wallet in a virtual list (checkbox, amount with dimmed decimals, address, label, confirmations, age, lock), sorted by any column, filtered (search of address/label/transaction; all, spendable, unconfirmed, locked), select all/none, what the coins chosen hold against the amount to send with a warning when they are not enough (also for the fee, from the host's estimate), lock/unlock in the row and its menu, copies; only the coins chosen are spent; components/CoinSummary.svelte; custom change address with its checks; switched off, the wallet chooses. The tree mode of the dialog is gone. src/webui/walletapi.cpp `wallet.coins`/`wallet.coins.lock`/`wallet.coinControl.summary` |
| G04 | Confirmation dialog with total, fee and a send delay | sendcoinsdialog.cpp `SendConfirmationDialog` | [x] pages/Send.svelte → components/DialogHost.svelte (fee, kvB, total in every unit, recipients, 3 s delay) |
| G05 | Create unsigned (PSBT): copy or save | sendcoinsdialog.cpp | [x] pages/Send.svelte `presentPsbt` (copied, save as binary .psbt), src/webui/walletapi.cpp `wallet.send.psbt` |
| G06 | External signer (hardware wallet) sign on device | sendcoinsdialog.cpp | [x] src/webui/walletapi.cpp `wallet.send.commit` signs on the device as Qt (fill, sign, finalize; partial → PSBT), "Sign on device" button; tested end to end with the mock signer of the functional tests (run_audit.py) |
| G07 | Errors: invalid address, duplicate, amount exceeds balance, absurd fee, creation failed | sendcoinsdialog.cpp `processSendCoinsReturn` | [x] src/webui/walletapi.cpp `wallet.send.prepare` (same checks and messages as prepareTransaction) |
| G08 | Clear all; balance display | sendcoinsdialog.cpp | [x] pages/Send.svelte (clear all, balance or "External balance") |

## H. Receive page

| ID | Function | Qt source | Status |
|----|----------|-----------|--------|
| H01 | Request form: label, amount, message, address type (bech32, bech32m, p2sh-segwit, legacy) | receivecoinsdialog.cpp | [x] pages/Receive.svelte (address types as Qt, taproot when enabled), src/webui/walletapi.cpp `wallet.newAddress` |
| H02 | Request dialog: QR code, URI, address, amount, label, message, copy, save image | receiverequestdialog.cpp, qrimagewidget.cpp | [x] components/RequestDialog.svelte (QR, URI, address, copy, save PNG with the address, verify on device) |
| H03 | Requested payments history: show, remove, copy URI/address/label/message/amount | receivecoinsdialog.cpp, recentrequeststablemodel.cpp | [x] pages/Receive.svelte, stored in the wallet in the format of chains-qt (src/webui/walletapi.cpp `EncodeReceiveRequest`) |

## I. Transactions page

| ID | Function | Qt source | Status |
|----|----------|-----------|--------|
| I01 | Table: status, date, type, label/address, amount; sortable; large histories | transactionview.cpp, transactiontablemodel.cpp | [x] pages/Transactions.svelte (records decomposed as TransactionRecord, src/webui/walletapi.cpp `wallet.transactions`; status ring, brackets for amounts that do not count yet) |
| I02 | Filters: date (today, week, month, last month, year, range), type, search (address, txid, label), min amount | transactionview.cpp, transactionfilterproxy.cpp | [x] pages/Transactions.svelte, lib/wallet.ts `dateRange`/`filterRecords`; plus totals of what is shown |
| I03 | Context menu: copy address, label, amount, txid, raw tx, full details; edit label; details; bump fee; abandon; show in third-party explorer | transactionview.cpp | [x] pages/Transactions.svelte context menu (all copies, details, bump, abandon, edit label, show in the block explorer, third-party explorer links of Settings → Display) |
| I04 | Transaction details dialog (status, date, from/to, credit/debit, fee, sizes, inputs, debug) | transactiondesc.cpp, transactiondescdialog.cpp | [x] components/TxDetailsDialog.svelte, src/webui/walletapi.cpp `Describe` (TransactionDesc, debug section when logging) |
| I05 | Export to CSV | transactionview.cpp `exportClicked` | [x] pages/Transactions.svelte, lib/wallet.ts `transactionsCsv` (columns and quoting of CSVModelWriter) |
| I06 | Increase fee (bumpfee, PSBT for watch-only) | walletmodel.cpp `bumpFee` | [x] lib/txactions.ts `bumpFee` (confirmation, unsigned PSBT copied), src/webui/walletapi.cpp `wallet.bump.*` |

## J. Address book

**User decision (2026-10-15):** no Addresses page and no sending addresses. The receiving addresses are a
tab (**Addresses**) of the Receive page; the menu item "Receiving addresses", the palette, the old page id
`addresses` (`PageInfo.tabOf`) and the Wallets page lead to it. "Sending addresses" is gone from the menus,
the palette, the Send page (the button of the recipient field) and the verify form of Sign and verify. The
host keeps `wallet.addressBook.*` (a label typed on the Send page still goes in the book of the wallet;
the label of a sent payment can be edited in the history).

| ID | Function | Qt source | Status |
|----|----------|-----------|--------|
| J01 | Sending addresses: list, new, edit, delete, copy, choose (for the send page), export CSV | addressbookpage.cpp, editaddressdialog.cpp | [–] Left out (user decision above). Was pages/Addresses.svelte; components/EditAddressDialog.svelte stays (labels of the receiving addresses and of the history), src/webui/walletapi.cpp `wallet.addressBook.*` stays |
| J02 | Receiving addresses: list, edit label, copy, export CSV | addressbookpage.cpp | [x] Receive → Addresses (components/ReceivingAddresses.svelte): label, address, type, used or not, amount received and last payment (from the history: lib/wallet.ts `receivingAddresses`), search, copy, QR code, edit label, export CSV, sign a message; **Generate new address** with a label and an address type (as H01) |

## K. Messages, PSBT

| ID | Function | Qt source | Status |
|----|----------|-----------|--------|
| K01 | Sign message (address, message, signature, copy) | signverifymessagedialog.cpp | [x] (a tab of **Advanced transactions**, pages/Advanced.svelte: user decision 2026-10-15) pages/Messages.svelte, src/webui/walletapi.cpp `wallet.signMessage` (P2PKH and P2WPKH, as chains-qt) |
| K02 | Verify message | signverifymessagedialog.cpp | [x] (a tab of **Advanced transactions**, pages/Advanced.svelte: user decision 2026-10-15) pages/Messages.svelte, src/webui/walletapi.cpp `message.verify` |
| K03 | Load PSBT from file / clipboard; PSBT operations dialog: sign, broadcast, copy, save, status and amounts | psbtoperationsdialog.cpp, walletframe.cpp `gotoLoadPSBT` | [x] (a tab of **Advanced transactions**, pages/Advanced.svelte: user decision 2026-10-15) pages/Psbt.svelte, src/webui/walletapi.cpp `psbt.decode`/`wallet.psbt.open`/`wallet.psbt.sign`/`psbt.broadcast` |

## L. Drivechain (Chains-specific)

| ID | Function | Qt source | Status |
|----|----------|-----------|--------|
| L01 | Sidechains tab: active sidechains (slot, title, description, escrow, active since); deposit to a sidechain with deposit address validation (slot, checksum, sidechain match) | sidechainpage.cpp | [x] pages/Sidechains.svelte: the slots (an overview of the 256), the active sidechains with their escrow and their latest deposits, the deposit form with the checks of a deposit address (`drivechain.depositAddress`: slot, checksum, a plain destination questioned), confirmation, unlock for the call |
| L02 | Proposals tab: proposals in the chain (score, age, acks, acked by this node); ack / stop acking / remove; propose a sidechain (slot, title, description, hashes, replaces) | sidechainpage.cpp | [x] pages/Sidechains.svelte "Proposals": proposals in the chain with a bar of their acks, failures, blocks left, replacement, ack of this node; ack / stop acking; queued proposals and remove; the propose form with its checks |
| L03 | Withdrawals tab: pending bundles (slot, hash, score, blocks left, payable, failures); vote upvote / downvote / abstain / default | sidechainpage.cpp | [x] pages/Sidechains.svelte "Withdrawals": bundles with a bar of their score out of the minimum (red when they cannot make it), blocks left, payable, known, vote of this node, payouts and fee; upvote / downvote / abstain / default; the default vote of this node |
| L04 | Sidechain Nodes window: built-in and added sidechains; locate programs, download a release with hash check, add/remove; start/stop node, start wallet window; auto mining on/off; mine one block with a fee; log | sidechainnodes.cpp | [x] src/webui/sidechainnodes.cpp + pages/SidechainNodes.svelte: built-in and added sidechains, locate (folder chooser), download with a hash check (curl, tar; https only, no user name before the host; the dialog of the desktop says first, in fixed words, whether the release is built in or typed, then the host alone on its line), add/remove (a name is one line of at most 64 printable characters), start node (detached, sharing no descriptors) / wallet window / stop, status (blocks, follows this node, mining), auto mining on/off, mine one block for a fee, the end of the log |
| L05 | Solo mine: address (new from wallet), threads, start/stop, status, hash rate, network stats; remembered address and threads | miningdialog.cpp | [x] pages/Mine.svelte: address (new from the wallet), threads, start/stop, status, hash rate, expected time per block, blocks found (and not accepted), network figures; address and threads remembered |
| L06 | Block explorer: search by height/hash/txid, blocks list, block details, transaction details, waiting transactions (mempool) | blockexplorer.cpp | [x] pages/Explorer.svelte: search by height, block hash or transaction id; the latest N blocks; waiting transactions; a block (facts, previous/next, its transactions) and a transaction (inputs, outputs, status), with their raw JSON |
| L07 | Timestamp file: fingerprint, stamp (publish), find the stamp in the last N blocks | timestampdialog.cpp | [x] (a tab of **Tools**, pages/Tools.svelte: user decision 2026-10-15) pages/Timestamp.svelte + `timestamp.hashFile`/`timestamp.search`: fingerprint of a chosen file, stamp (send with an OP_RETURN "STAMP"+hash), the oldest stamp found in the last N blocks, with progress and Stop |
| L08 | Proof of funds: create (statement, sign with every funded address), copy, check (valid / partly / not) against the chain | proofoffundsdialog.cpp | [x] (a tab of **Tools**, pages/Tools.svelte: user decision 2026-10-15) pages/Proof.svelte: create (statement signed by every funded address, the wallet unlocked once), copy, check (valid / partly / not, coins on the addresses now with scantxoutset) An addition (user decision, 2026-10-15): "Choose what to prove" (off: every funded address, as chains-qt) by address (the addresses holding coins, their balance and label, search, all/none) or by coin (the coin list of Send, locked coins too); the total shown before; chosen coins on addresses that cannot sign are left out with a warning. A proof of chosen coins lists them between the statement and the signatures (`-----COINS-----`, one `txid:vout amount` a line) and they are signed with it (the message is the statement, a newline, the coin lines); the check looks each up with `gettxout` (unspent on the chain now, paid to an address that signed, of the amount listed) and gives their total apart from the addresses' whole balance (`scantxoutset`). A proof without coins is the proof of chains-qt, made and checked exactly as before (chains-qt does not read the extension). test/proof.test.ts; wallet.js |
| L09 | Multisig lounge: keys (partners, own keys), shared addresses (create, import descriptor, balances, copy), spend (prepare, sign, pass PSBT, broadcast) | multisigdialog.cpp | [x] (a tab of **Advanced transactions**, pages/Advanced.svelte: user decision 2026-10-15) pages/Multisig.svelte: keys (own keys with their paths, partners), shared addresses (sortedmulti, from a descriptor, balances, copy), spend (prepare in exact satoshis, sign, pass the PSBT, broadcast) |
| L10 | Crypto tools: hash calculator, merkle tree (from txids or a block), address decoder | cryptotoolsdialog.cpp | [x] (a tab of **Tools**, pages/Tools.svelte: user decision 2026-10-15) pages/Crypto.svelte + `tools.*` (src/webui/nodeapi.cpp: the hashes, merkle levels and Base58Check/Bech32 decoding of CryptoToolsDialog) |

## M. Node window

| ID | Function | Qt source | Status |
|----|----------|-----------|--------|
| M01 | Information: client version, user agent, datadir, blocksdir, startup time, network, connections, chain, blocks, last block time, mempool, wallet, debug log open | rpcconsole.cpp, forms/debugwindow.ui | [x] pages/NodeWindow.svelte: also blocksdir, the mempool from its events, alerts, and the debug log opened with the application of the desktop (`app.open`) |
| M02 | Console: commands with history, wallet selector, help-console, nested commands and parentheses syntax, font size, clear, sensitive command filtering in history | rpcconsole.cpp `RPCExecutor`, `RPCParseCommandLine` | [x] components/ConsoleView.svelte + src/webui/console.cpp (RPCParseCommandLine and RPCExecutor without Qt): history (arrows, sensitive commands filtered), completion of command names (Tab), wallet selector, help-console, nested commands and queries, font size, clear (Ctrl+L), `stop` while a command runs |
| M03 | Network traffic graph with time ranges and totals | trafficgraphwidget.cpp | [x] components/TrafficGraph.svelte, lib/traffic.svelte.ts: rates in and out from the `traffic` events (1 s), range 5 min to 24 h, totals, Clear |
| M04 | Peers table and detail panel (all fields of the Qt view); context menu copy address, disconnect, ban 1h/1d/1w/1y | rpcconsole.cpp, peertablemodel.cpp | [x] components/PeersView.svelte: the peers (getpeerinfo) with every field of the detail panel of Qt; menu copy address, disconnect, ban 1 h / 1 d / 1 w / 1 y (`peers.ban`: the address of the peer, as banSelectedNode) |
| M05 | Banned peers table: copy, unban | rpcconsole.cpp, bantablemodel.cpp | [x] components/PeersView.svelte: banned peers (listbanned), copy, unban (`bans.unban`) |

## N. Options

| ID | Function | Qt source | Status |
|----|----------|-----------|--------|
| N01 | Main: start on system login, prune and size, database cache, script verification threads, open configuration file, overridden-by-command-line list | optionsdialog.cpp, optionsmodel.cpp | [x] pages/Settings.svelte "Main" + src/webui/options.cpp (rules of OptionsModel): start on login (autostart entry), prune and size (prune-prev), dbcache, par, RPC server, open the configuration file, the options overridden on the command line |
| N02 | Wallet: coin control, spend unconfirmed change, PSBT controls, subtract fee by default, external signer path | optionsdialog.cpp | [x] pages/Settings.svelte "Wallet" (coin control: the "Choose coins" switch of the Send page, on at the start, spend unconfirmed change and signer path in settings.json, PSBT controls, subtract fee by default) |
| N03 | Network: map port (PCP/NAT-PMP), listen, RPC server, SOCKS5 proxy, separate Tor proxy, reachable networks | optionsdialog.cpp | [x] pages/Settings.svelte "Network": map port (natpmp, at once), listen, SOCKS5 proxy and separate Tor proxy (proxy-prev, onion-prev, addresses checked as ProxyAddressValidator), the networks the proxy is used for |
| N04 | Window: show tray icon, minimize to tray, minimize on close | optionsdialog.cpp | [x] pages/Settings.svelte "Window": show tray icon, minimize to the tray, minimize on close (kept in the preferences of the app, used by the host) |
| N05 | Display: language, unit, third-party tx URLs, money font, colours, font, size, themed frame | optionsdialog.cpp, theme.cpp | [x] pages/Settings.svelte "Display": unit, third-party transaction URLs, font of amounts (embedded or the monospaced one of the system), colours, a font apart from them, size; the language (the languages of chains-qt, by their own names; applied with Apply), and "Use the title bar of the desktop" (instead of the integrated title bar of C16; it waits for Apply as the other settings) |
| N06 | Reset options (restarts), restart-required notice, settings in settings.json as Qt does | optionsdialog.cpp, optionsmodel.cpp | [x] Reset Options (settings.json backed up and reset, the settings of the app backed up to guisettings.json.bak and cleared, the intro next time, the app shuts down); the restart-required notice, and "Restart now" (the app starts again with its options) |

## T. Themes and look

| ID | Function | Qt source | Status |
|----|----------|-----------|--------|
| T01 | Looks: system, Light, Dark, Ocean Teal, Matrix Green, Dark (classic Drivechain) | theme.cpp `THEMES` | [x] (plus Midnight, the default of chains-app, Catppuccin Mocha, Dracula, and the themes of the user: doc/webui-layouts.md, "The looks") |
| T02 | Look selector in the sidebar and the menu; saved and applied at once | bitcoingui.cpp, theme.cpp | [x] |
| T03 | Font family per look and a size adjustment (-6..+12) | theme.cpp `SaveFont` | [x] |
| T04 | Custom combination of colours and font | optionsdialog.cpp | [x] Settings → Display: any colours with any font (Inter, Nunito, JetBrains Mono, the font of the desktop), lib/themes.ts `FONTS` |

## S. The sidechain template (its Qt GUI, `sidechain-qt`)

The template's Qt GUI was the one of chains-qt with the changes of a sidechain (`git show <"Remove the Qt
GUI" commit>^:src/qt/...`). Its inventory, item by item, and where the app of a sidechain does each
(the pages: doc/webui.md, "The app of a sidechain"):

| ID | Function | Qt source | Status |
|----|----------|-----------|--------|
| S01 | Mainchain page, header: following the mainchain node at host:port, its height, the slot of this sidechain; "cannot be reached" with the error; "not running as a sidechain" | sidechainpage.cpp `refreshPage` | [x] pages/Peg.svelte: the peg drawn (the mainchain, the connection, its height and node; this chain, its slot, the mainchain block it acted on), the error, the stats; the home's Peg card; the connection also on the Merged mining page |
| S02 | Deposit tab: instructions, "New deposit address" (getdepositaddress), copy | sidechainpage.cpp `createDepositTab` | [x] Peg → Deposit: the instructions, a new deposit address, copy, its QR code, the address it credits; and more: the deposit made by the wallet of the mainchain node (`peg.deposit`), the deposits to this chain credited or on their way, those to the wallet marked |
| S03 | Withdraw tab: mainchain address, amount, mainchain fee, confirmation, createwithdrawal; the bundle status (pending, next, none, last failure); withdrawals table (status, amount, fee, block, refund address, txid, vout); take the selected one back (refundwithdrawal) | sidechainpage.cpp `createWithdrawTab`, `withdraw`, `refund` | [x] Peg → Withdraw: the address checked as an address of the mainchain (or one of the mainchain wallet a click away), amount, fee, what leaves the wallet, confirmation, the passphrase of a locked wallet for the call; the withdrawals not paid yet, those of the wallet marked and taken back (only those waiting); Peg → Bundle: the bundle and its state, and more: its votes on the mainchain against the score it needs, the blocks it has left, the bundles before it, paid or failed |
| S04 | Mine tab: the address of the fees (new one of the wallet), start / stop automatic mining (setbmm), one block by hand for an offer (requestbmmblock), the status (requests, blocks, outbid, idle, last fees and offer, error) | sidechainpage.cpp `createMiningTab` | [x] pages/Bmm.svelte: the same, with "always" and its offer (setbmm's test mode), candidates, the blocks of the mainchain and what each committed to, the log of what happened since the start |
| S05 | Mining in the sidebar: automatic mining on/off, one block by hand with its fee, a status line | sidebarmining.cpp | [x] the home's Merged mining card (on or off, blocks, requests, the share of mainchain blocks committed; the page a click away), the Merged mining page; the menu items of the chain (Peg, Deposit…, Withdraw…, Withdrawal bundle, Merged mining) and the palette |
| S06 | Menu "Use <name>" with "Merged Mining"; the Mainchain page at Alt+5 | bitcoingui.cpp `createMenuBar` | [x] the menu of the chain (named after it), Alt+5 opens Peg |
| S07 | No Sidechain Nodes, no solo mining in the toolbar or the menus, no sidechain proposals or votes | bitcoingui.cpp | [x] chain.json `drop`: the pages Sidechains, Sidechain nodes and Mine are not in the app of a sidechain (nor their menu items, quick actions, cards and RPC methods); the host does not register the sidechain nodes |
| S08 | Names: window title, `<name>-Qt` settings, `<stem>:` URIs, the dummy address of the network | guiconstants.h, guiutil.cpp | [x] chain.json (name, bin, uriScheme, networks): the title, the settings file `<stem>-app.json`, the URIs, the example addresses of the fields |
| S09 | The connection to the mainchain node: -mainchainrpc* in the configuration file only | (none in the Qt GUI) | [x] new: Settings → the mainchain (address, RPC port, wallet, user name; the cookie file and the password file from the file chooser) |

L01–L05 (the sidechains of the mainchain, its proposals and votes, the sidechain nodes, solo mining)
are the mainchain's: not in the app of a sidechain (S07). L06–L10 and every other section above hold
for the app of a sidechain as they are.

## FB. FastBlocks (its Qt GUI, `fastblocks-qt`: the Staking page, the explorer's kinds, the staking income)

`fastblocks-qt` had the GUI of the template (section S) and, from commits 610d3ed15e..b6068b3208 and the BMM
names of 08af58bc41 (`git show <"Remove the Qt GUI" commit>^:src/qt/...`): the **Staking** page
(`stakingpage.{h,cpp}`, 725 lines, Alt+6, its tests `test/stakingtests.cpp`), the block explorer's kinds of
blocks (`blockexplorer.cpp`), and the labels of the staking income in the transactions
(`transactionrecord.cpp`, `transactiontablemodel.cpp`, `transactiondesc.cpp`, `transactionview.cpp`). Each item,
and where `fastblocks-app` does it (`src/webui/app/src/chain`: `pages/` the pages, `fb/` what they share,
`LocalCards.svelte` the cards of the home, `explorer.ts` and `roles.ts` the hooks of the kit; the host side in
`src/webui/local.cpp`). The pages are new in their look: stats over tabs, tables that become cards when narrow,
long lists paged (25/50/100 rows, 10 on the smaller panels) with Latest-N selectors where they grow. Tests:
`chain/fastblocks.test.ts` (`npm test`) and the end-to-end `e2e/fastblocks.js` (run_sidechain.py `--script
fastblocks.js`, CTest `webui_e2e_fastblocks`; `--width 700` for the narrow window).

| ID | Qt | Source | fastblocks-app |
|----|----|--------|----------------|
| FB01 | The Staking action: sidebar and Use menu, Alt+6, its status tip | `bitcoingui.cpp` (`stakingAction`) | [x] three pages in the Chain section of the sidebar and of the workspace, after Drivechains (with its Merged mining tab), before the explorer and the tools (`chain/local.ts`): Staking (Alt+6), Finality & BMM (Alt+7), Slashing & jail (Alt+8); the palette and the menus (Bond coins…, This node as a validator, Validator set, Leader schedule, BMM blocks and finality, Proofs of double signing); a Stake quick action on the home |
| FB02 | The summary: blocks of stake every N s, slot, epoch, leader; off for want of validators; BMM commitments and the last mainchain block; "no blocks of stake lately" | `apply` | [x] Staking's head (slot, epoch, a slot every N s), the notes when blocks of stake are off or stalled; Finality & BMM's stats (BMM commitments, the last at mainchain block N) |
| FB03 | Refreshed every 3 s while shown, off the GUI thread, one refresh at a time | `refresh`, `NodeRpc::RunAsync` | [x] `fb/store.svelte.ts`: one refresh at a time, every 3 s and with each block while a page or a card of FastBlocks is shown, resting while the window is hidden |
| FB04 | Start staking / Stop staking (setstaking) | `setStaking` | [x] This node: Start staking, Stop staking; stopping goes through the host (`fastblocks.staking`), which asks the desktop first (the validator may be jailed); `setstaking` is not on the page's list |
| FB05 | This node as a validator: keys (shortened, whole in the tooltip), bonded, unbonding, eligible stake (and the minimum), in the set (yes / n of m), jailed or banned, share of the set (and of the slots), next slot (and when), blocks made (today, on the active chain, since start, acknowledgements), missed slots, fees paid to | `showValidator` | [x] This node: each key with its copy button and its state (active, jailed until epoch N, banned, joins a coming set, unbonding, too little bonded), fees paid to (copy), blocks made today, on the active chain and since start (of them BMM blocks), missed slots, the last block made, the highest slot signed (slashing protection), the last problem; the stats over the tabs: the node's state, bonded and unbonding, share of the set, next slot (a countdown), income today; a node without keys says how to become one (-validatorkeyfile) and why the producer could not start |
| FB06 | (the next slot only) | `showValidator` | [x] new: Slots it leads next: a timeline of every slot of this epoch and the next (this node's lit, the empty ones outlined, now marked) and the next 8 it leads with their countdown |
| FB07 | Staking income of this wallet, today and in all: Staked, BMM rewards, Slashing rewards, Total; from N blocks of stake | `showIncome`, `WalletIncome` | [x] This node: the same table, from the history's records by role (`fb/model.ts` incomeOf), a link to the transactions; the home's Staking card (income today and in all) |
| FB08 | Coins of this wallet by finality: soft, BMM-confirmed, final, what each means; BMM-confirmed and final up to block N | `apply` (getbalances finality) | [x] This node: a bar of the three and their amounts, what each means, how far the chain is BMM-confirmed and final |
| FB09 | Validator set of this epoch: key, stake, share, slots, status (this node, in the set, jailed until, banned) | `apply` (getvalidators) | [x] Validators: the set of any epoch (back and forth, or by number): first slot, fixed or not, seed known, blocks of stake on or off, total stake, count; each validator's key (this node marked), stake, share (a bar), slots, status; a filter by key; paged |
| FB10 | (none) | | [x] new: Every validator on the chain (listbonds all): bonded (genesis stake), unbonding, state, owner; paged |
| FB11 | Newest BMM blocks: block, kind (BMM block / merged mined (PoW)), covers block, mainchain block | `apply` (getanchorinfo 10) | [x] Finality & BMM: the newest BMM blocks (up to 100, Latest 10/25/50/100), with what they cover, the mainchain block, the finality of what they cover; each block opens in the explorer |
| FB12 | Bonds of this wallet: key, bonded, unbonding (the unbonds and their BMM commitment in the tooltip), state (banned, in the set, joins at a coming epoch, too little, jailed until) | `apply` (listbonds) | [x] Bonds: the same, with when the first unbond is paid (and the commitment now), the owner of each bond (copy); a row picked fills the form |
| FB13 | Bond, Add to bond, Unbond, Unbond all: the key (entered, the bond selected, or this node's), the amount; checks before sending (a key, an amount, a bond to add to or unbond from, at most what is bonded); asked first; the node's error | `bond`, `unbond`, `bondKey` | [x] Bonds: the four as a segmented choice, the key (this node's by default, a list of its keys and the wallet's bonds), the amount in the unit of the app; the same checks and more (a banned key; what stays bonded nothing or at least the minimum stake), said as typed; sent through the host (`fastblocks.bond`, `fastblocks.unbond`), which asks the desktop with the wallet, the key in full and the amount (Qt asked in the page), after the passphrase of a locked wallet; createbond and createunbond are not on the page's list; a notice of the transaction |
| FB14 | (none: a bond needed this node's key) | | [x] new: a bond to another node's key with the proof of possession its node signed and the owner it names; Proof of possession: this node signs one for an owner (`fastblocks.pop`, asked on the desktop; signbondpop is not on the page's list) |
| FB15 | Block explorer: Kind after Height (PoS: a blue disc; BMM: the disc with an amber diamond; PoW: an amber diamond, outlined), its tooltip | `blockexplorer.cpp` `applyBlocks`, `KindIcon` | [x] the kit's explorer with the hook `chain/explorer.ts`: Kind after Height as a badge told apart by its mark as well as its colour (PoS a filled disc, BMM an amber disc, PoW an outlined diamond), its tooltip; the ribbon's boxes tagged with the kind |
| FB16 | Slot, Producer ("this node", else the key shortened, whole in the tooltip), Mainchain block, Finality (soft, BMM-confirmed, final, as of the anchored and final tips) | `applyBlocks`, `finalityOf` | [x] the same columns (Mainchain: the block whose commitment a BMM block records, or that committed to a merged mined one); finality as of getanchorinfo's tips, read again with each block; Difficulty left out (a merged mined sidechain's is the lowest there is) and Weight (on the ribbon's boxes) |
| FB17 | A block's details: kind, slot and producer, the BMM commitment it records or makes, finality (and how many commitments cover it), proofs; then the whole of getblock | `displayBlock` | [x] the facts at the top of a block's details, then the kit's (hash, time, ...) and Raw JSON |
| FB18 | The FastBlocks columns hidden on a chain without blocks of stake | `applyBlocks` | [x] the columns stay empty for a block getblock says no kind of (and fill as the blocks are read) |
| FB19 | Transactions: Staked, Anchor (BMM) reward, Slashing reward instead of Generated/Mined; their icons and colours as mined | `transactiontablemodel.cpp` | [x] the history's Type, the home's and the overview's lists, the notices and the CSV export: Staked, BMM reward, Slashing reward (`chain/roles.ts`; the host's records carry each coinbase output's role, `local::CoinbaseRoles`) |
| FB20 | The type filter has the three | `transactionview.cpp` | [x] the same three entries in the type filter |
| FB21 | The details: Source: Staked: the producer's share of a block of stake (BMM reward: the fee pot, paid to the BMM bidder; Slashing reward: the reporter's share of a penalty) | `transactiondesc.cpp` | [x] the same source in the details of a transaction |
| FB22 | The page's errors under it, and in a message box for the bond buttons | `tell` | [x] notices for what was done, a dialog for what the node refused; a no on the desktop says nothing |
| FB23 | (none) | | [x] new: Finality & BMM: BMM-confirmed and final up to block N, the blocks waiting for BMM confirmation (Latest 10/25/50/100/All, paged; kind, slot, time; each opens in the explorer), the slots since the first one not covered, the tickets this node knows (paged), the commitments the active chain leaves out, the fee pot (what a BMM block of the tip would pay, accrued, paid, penalties in reserve), and the node's own BMM bid (-validatoranchorbid, from the host, `fastblocks.anchoring`: no RPC says it) beside merged mining |
| FB24 | (none) | | [x] new: Schedule: the leader of every slot of an epoch (this node's only, those to come only, the current one marked, a countdown), slots nobody leads (a banned leader, no set or seed yet), and the epochs (Latest 5/10/25/50/100: first slot, start, validators, total stake, seed, blocks of stake on or off) |
| FB25 | (none) | | [x] new: Slashing & jail: the rules (jailing, slashing and banning, reports), penalties so far, rewards owed to reporters, the proofs this node knows (listevidence: the validator, the slot, the two blocks, the proof to copy; paged), the validators jailed or banned now (every validator with a wallet open, else the set's own word), a proof given by hand (submitevidence) |
| FB26 | (none) | | [x] new: the cards of the home: Staking (state, this node's bond, the wallet's bonds, income today and in all), Finality (the last BMM-confirmed block, blocks waiting, slots since, final up to), Next slots (the slots this node leads next with their countdown; on a node that is not a validator, the next leaders); the payment URIs of FastBlocks (`fastblocks:`, chain.json) |
| FB27 | The Qt staking test: the page offscreen on a node with a validator key; the producer, the set, the deposit final, the buttons, the bonds | `test/stakingtests.cpp` | [x] `chain/fastblocks.test.ts` against a pretend node, and `e2e/fastblocks.js` on a live regtest mainchain: a genesis validator with a second key bonded through the form (the desktop asked, declined once), more added, some unbonded, the key joining the set, staking off and on, the explorer's kinds and finality, the Staked income in the history, every page in Midnight, Dracula and Light, dashboard and workspace, at 1180 and 700 px |

Every user RPC of FastBlocks' blocks of stake is on a page: getposinfo, getstakinginfo, getvalidators,
getvalidator (on the list for the pages; the pages read a key's state from getstakinginfo and listbonds),
getleaderschedule, getanchorinfo, listevidence, submitevidence (node); listbonds, getbalances' finality (wallet);
getblock's kind, slot, proposer, ack, evidence and finality (the explorer). setstaking, createbond, createunbond and
signbondpop go through host methods that ask the desktop first. setbmm, getbmminfo, createbmmblock and
requestbmmblock are the template's Merged mining page.
## BA. BitAssets (its Qt GUI, `bitassets-qt`: the Assets page)

`bitassets-qt` had the GUI of the template (section S) and one page more, Assets (sidebar "Assets", Alt+6:
`src/qt/bitassetspage.{h,cpp}`, 1763 lines, and its tests `src/qt/test/bitassetstests.cpp`), removed by the
commit "Remove the Qt GUI" of this tree. Its functions, item by item, in the pages of `bitassets-app`
(`src/webui/app/src/chain/pages`, `chain/ba`, `chain/LocalSend.svelte`, `chain/LocalCards.svelte`). Tests:
`chain/bitassets.test.ts` (`npm test`), `webui_local_tests` (`test_bitassets-app`) and the end-to-end
`e2e/bitassets.js` (run_sidechain.py `--script bitassets.js`, ctest `webui_e2e_bitassets`).

| ID | Qt | Source | bitassets-app |
|----|----|--------|---------------|
| BA01 | The page: title, the summary line (assets, pools, auctions, next block), five tabs, a status line for what was done or went wrong; refreshed every 5 s while shown, a panel rebuilt only when not typed in | constructor, `refresh`, `refreshInfo`, `say`, `TypingIn` | [x] five pages in the Chain section (Assets, My assets, Issue, Trade, Auctions), before the explorer and the tools; the figures of Assets; every page follows the chain (chain/ba/store.svelte.ts: one refresh at a time, with each block and every 5 s, resting while the window is hidden); what was done in notices, what went wrong in dialogs; forms keep what is typed (Svelte state) |
| BA02 | The lists of assets to pick from, by id (labels may look alike), CHN first, sensible starting pairs | `refreshAssetChoices`, `AssetArg`, `SelectAsset` | [x] ba/AssetSelect.svelte: by id, the label with its number, CHN first; every command is given "CHN" or "0x"+hash, never a label (model.ts `assetArg`); each page picks a sensible pair (the asset that trades with what is paid, the first asset held) |
| BA03 | My assets: the list (balance, "+n waiting", ★ controlled), "Being created" (reserving, registering in n blocks, taken by another), pool shares, auctions to collect; "Nothing yet" | `createMineTab`, `refreshMine` | [x] My assets → Holdings (the balance, what is on its way, the crown of control, the share of the supply, the worth in CHN; sorted by worth), Being issued, Pool shares, My auctions; the figures (assets held and controlled, worth, pool shares, being issued) |
| BA04 | Send & receive panel: receive at an address of the wallet (copy, new address, QR code); send an asset held (asset, "you hold", to, amount, All), confirmation, sendasset; "CHN is sent from the Send page" | `showTransferPanel` | [x] Send: the asset chosen at the top of the Send page (chain/LocalSend.svelte), the address checked, the amount checked with the asset's decimals and at most what is held (Max), a confirmation with a delay, sendasset; receiving: any address of the wallet (Receive, with its QR code, a button of My assets away) |
| BA05 | Create panel: the name (live check: rules, "taken", "free"), initial supply, decimals (0–12), description, "Publish the name"; Create reserves; how it works (two steps, reveal depth) | `showCreatePanel` | [x] Issue: a wizard (name, supply, about it, review): the rules of the chain (model.ts `nameProblem`), free or taken asked of the node, the decimals with the smallest and the largest amount, the description with its bytes, and more: every field of the data (commitment, IPv4, IPv6, encryption and signing keys), a preview in the colour of its hash, the timeline with the reveal depth in blocks and time |
| BA06 | The names reserved, kept per wallet (QSettings), registered by themselves once deep enough (registerasset with supply, decimals, info, public); kept until registered; "registered by someone else first" | `registerPending` | [x] the journal of the app (a file of the data directory, bitassets.store.*: local.cpp), written at once (the user told if it cannot be); registered by itself once `wait` is 0; on a locked wallet "Register now" with the passphrase; a reservation the app did not make registered with a supply typed in; taken ones said so |
| BA07 | A reservation's panel: being created (registering in n blocks, keep the page open), taken; Release this reservation (releaseassetreservation), forgotten | `showReservationPanel` | [x] My assets → Being issued: each asset's steps (Reserved, Deep enough, Registered, In your assets), the blocks left, Release (confirmed), forgotten in the journal |
| BA08 | An asset's panel: label, #number, YOU CONTROL IT, FIXED SUPPLY, PRIVATE NAME; balance, "+n on the way"; the control coin moving; supply, minted, burned, decimals; its description (links); its market with CHN (price, reserves, Trade) or "Make a pool" | `showAssetPanel` | [x] ba/ManagePanel.svelte (My assets) and ba/AssetView.svelte (Assets): the same, and its value in CHN, its share of the supply, all its markets, its price as the app saw it, its auctions; the description shown as text (a link is not opened from text of the chain) |
| BA09 | Send from the asset's panel (address, amount, All, confirmation) | `showAssetPanel` | [x] the Send button of the panel opens Send with the asset chosen (BA04) |
| BA10 | Mint more (amount, to this wallet or an address), mintasset | `showAssetPanel` | [x] My assets → Mint more: the amount checked with the decimals, the address checked, a confirmation |
| BA11 | Its description: description and commitment, saved as changes (a field emptied deleted), updateasset | `showAssetPanel` | [x] My assets → Change its data: every field of the data (and the IPv4, IPv6, encryption and signing keys), each checked as the chain checks it, the changes listed in the confirmation |
| BA12 | Control and supply: give the control coin (transferassetcontrol, warned); fix the supply for good (fixassetsupply, warned) | `showAssetPanel` | [x] My assets → Control and supply: the same, the address checked, both warned |
| BA13 | Burn some (burnasset, warned) | `showAssetPanel` | [x] My assets → Burn some: at most what is held, warned |
| BA14 | A pool share's panel: percent of the pool, shares, worth, 0.3% of trades; take out 1–100% (removeliquidity); Add more | `showLiquidityPanel` | [x] My assets → Pool shares: the same, a slider and 25/50/75/100%, a slippage, what it gives worked out as the chain does (amm::WithdrawPaid: CHN dust stays), a confirmation; Add more opens Trade → Add liquidity with the pair |
| BA15 | An auction to collect: status, left, brought in, bids, blocks; Collect, or "Cancel it: take everything back" before any bid; when it can be | `showReceiptPanel` | [x] My assets → My auctions: the same, with its sold bar, and a confirmation saying what comes back |
| BA16 | Trade → Swap: pay (amount, asset), flip, get (asset, amount quoted), the quote line (price, impact coloured, at least, pool fee), "no pool yet", "closed" (abandoned), CHN payout too small, too large (>10%), slippage, Swap (asked about above 2%), swapasset | `createTradeTab`, `updateQuote` | [x] Trade → Swap (ba/SwapPanel.svelte): the same, quoted by the node (quoteswap), slippage chips and a custom one, Max from what is held, and the curve of the pool with where the trade moves it; swapasset with the slippage and the largest impact (10%) |
| BA17 | Trade → Add liquidity: two assets and amounts; the second at the pool's price; a new pool, or an abandoned one (dust, reopen) setting the price; asked again if the pool changed meanwhile; addliquidity | `createTradeTab`, `updateLiquidityQuote` | [x] Trade → Add liquidity (ba/LiquidityPanel.svelte): the same, and the shares and the share of the pool worked out as the chain does (amm::Provide), what goes in of each, the least to open a pool, a slippage; the pool asked again before sending |
| BA18 | Trade → Pools table (pool, holds, price, traded, trades; closed ones greyed), a row picks the pair | `refreshTrade` | [x] Trade → Pools (ba/PoolsPanel.svelte): searched, the closed ones on demand, sorted by volume; a pool opened: price, holdings, trades, the share of the wallet, its price over time and its curve, Swap and Add liquidity |
| BA19 | Auctions table (selling, for, price now, status, ends); an auction's panel: price now, all of what is left, the line from start to end; Bid (quotebid as typed), "Buy all that is left" (getauction then bidauction buy_all, confirmed) | `createAuctionsTab`, `refreshAuctions`, `showAuctionDetail` | [x] Auctions → Browse: filtered (running, open, upcoming, sold out or ended, collected); an auction (ba/AuctionView.svelte): the same, its price drawn block by block with the next block marked, the sold and time bars, when it starts or ends in blocks and time |
| BA20 | Sell by auction: asset and amount, for, start and end prices (all of it), runs for, starts in; checks; createauction | `createAuctionsTab` | [x] Auctions → Sell by auction: the same, checked as the chain checks it (model.ts `auctionProblems`), the price of one at the start and the end, the blocks it takes bids in, the line drawn before it is made |
| BA21 | Explore: search by name, number or hash (Enter asks the node: a private name), the list (label, #number) | `createExploreTab`, `refreshExplore` | [x] Assets: cards (mark, label, number, description, supply, price, tags), search, filters with counts, sorts; Enter finds a private name by its exact name, number or hash |
| BA22 | An asset in Explore: #number, FIXED SUPPLY, supply, minted, burned, decimals, registered; description; controlled by; markets with Trade; dead: retire (releaseasset, warned); hash; the history of its description and commitment | `showExploreDetail` | [x] ba/AssetView.svelte: the same, every field of the data and its history, the registration, and more (BA08); Retire it, warned, with the CHN its pools pay to mainchain miners |
| BA23 | Activity: when, what, confirmations ("waiting"), transaction (listassetactivity 200) | `createActivityTab`, `refreshActivity` | [x] My assets → Activity: the same, the kind of operation, the transaction copied |
| BA24 | Errors of the node shown as they are; asked first where it matters (confirm, danger) | `call`, `say`, `confirm` | [x] dialogs of the app; errors of the node and text of the chain with SafeText; every transaction confirmed; a locked wallet asks for its passphrase for the call alone (wallet.withKeys) |
| BA25 | The Assets action in the sidebar, the Use menu, Alt+6 | bitcoingui.cpp | [x] the five pages in the sidebars and the workspace (the Chain section), the menu of the chain (Issue an asset…, Swap assets…, Add liquidity…, Sell by auction…, Asset activity), the palette, the quick actions of the home (Explore assets, Issue an asset, Swap); Alt+6 opens Assets (`shortcut` in chain/local.ts) |
| BA26 | (none) | | [x] new: the cards of the home (My assets and their worth, the markets, the auctions ending soonest); the price of each pool as the app saw it (the journal); the colour of each asset from its hash; a locked wallet shows its assets once unlocked |
