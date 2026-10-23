// The asset registry, the one create wizard, an asset's page with Open a market, and the order book's charts, in pictures,
// against a live mainchain (run_sidechain.py --script assets_shots.js, the app with -testactivationheight=dex@0
// -testactivationheight=dexcurves@0 -testactivationheight=ticker@0): GOLD and USDX registered and MOON launched by RPC,
// limit orders of GOLD for USDX on both sides; then Browse assets (every asset with its market), the wizard (Create an
// asset) at its market step, GOLD's page and its Open a market, and the book of GOLD with USDX (depth, candles, the
// wallet's orders).
(async () => {
  const { host, rpc, sleep, T, waitFor, check, snapshot, finish } = E2E;
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
  const W = 'shots';
  const button = (label, root = document) => $$('button', root).find((b) => b.textContent.trim().replace(/\s+/g, ' ').startsWith(label));
  async function go(page, tab = '') {
    T().go(page, tab);
    await sleep(600);
  }
  async function shot(name) {
    if (!document.getElementById('e2e-no-toasts')) {
      const hide = document.createElement('style');
      hide.id = 'e2e-no-toasts';
      hide.textContent = '.toasts { display: none !important; }';
      document.head.appendChild(hide);
    }
    $$('dialog[open]').filter((d) => /Synchronizing/.test(d.textContent)).forEach((d) => button('Hide', d) && button('Hide', d).click());
    await sleep(700);
    await snapshot(name);
  }
  async function pick(field, id) {
    const trigger = await waitFor(`the picker ${field}`, () => $(`#${field}`), 30000);
    trigger.click();
    const option = await waitFor(`the asset ${id.slice(0, 8)} in ${field}`, () => $(`[role="option"][data-id="${id}"]`), 30000);
    option.click();
    await sleep(300);
  }
  const ids = {};
  async function register(ticker, supply, decimals) {
    await rpc('reserveasset', [ticker], W);
    await waitFor(`${ticker} registered`, async () => (await rpc('registerasset', [ticker, supply, decimals, {}, true], W)).txid, 300000);
    ids[ticker] = (await waitFor(`${ticker} in a block`, async () => rpc('getasset', [ticker]), 300000)).asset;
  }
  try {
    await host('test.confirmAnswer', { always: true });
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
    await waitFor('the node to reach the mainchain', async () => (await rpc('getmainchaininfo')).connected, 60000);
    T().savePrefs({ layout: 'dashboard', tipsDone: true, theme: 'midnight', uiMode: 'advanced' });
    await rpc('createwallet', [W]);
    await waitFor('the wallet in the page', () => T().wallets.current === W);
    const deposit = await rpc('getdepositaddress', [], W);
    await host('test.confirmAnswer', { yes: true });
    await host('peg.deposit', { destination: deposit.depositaddress, amount: '900' });
    await rpc('setbmm', [true, await rpc('getnewaddress', [], W), true]);
    await waitFor('the deposit credited', async () => (await rpc('getbalances', [], W)).mine.trusted >= 900, 300000);
    await register('GOLD', 1000000, 2);
    await register('USDX', 1000000, 2);
    await rpc('reserveasset', ['MOON'], W);
    await waitFor('MOON launched', async () => rpc('launchtoken', ['MOON', { display_name: 'To the moon', first_buy: 2 }], W), 300000);
    for (const [sell, amount, buy, price] of [['GOLD', 100, 'USDX', 1.05], ['GOLD', 80, 'USDX', 1.1], ['USDX', 120, 'GOLD', 1.05], ['USDX', 60, 'GOLD', 1.1]]) {
      const at = await rpc('getblockcount', []);
      await waitFor('placeorder accepted', async () => rpc('placeorder', [sell, amount, buy, price, 5760], W), 300000);
      await waitFor('the order in a block', async () => (await rpc('getblockcount', [])) > at + 1, 300000);
    }
    await waitFor('the book with both sides', async () => {
      const b = await rpc('getbook', ['GOLD', 'USDX']);
      return b.asks.length && b.bids.length ? b : null;
    }, 300000);

    await go('assets');
    await waitFor('the registry with its badges', () => $$('[data-testid="market-badge"]').length >= 3, 60000);
    await check('a Create an asset button', !!button('Create an asset'));
    await shot('assets-registry');

    button('Create an asset').click();
    await sleep(800);
    await shot('create-wizard');

    await go('assets');
    const tile = await waitFor('the GOLD tile', () => $$('button.tile').find((b) => b.textContent.includes('GOLD')), 60000);
    tile.click();
    await waitFor('the asset page with Open a market', () => $$('h2, h3').find((h) => /Open a market/i.test(h.textContent)), 60000);
    $$('h2, h3').find((h) => /Open a market/i.test(h.textContent)).scrollIntoView({ block: 'start' });
    await shot('asset-page');

    await go('trade', 'swap');
    await go('trade', 'book');
    await pick('book-base', ids.GOLD);
    await pick('book-quote', ids.USDX);
    await waitFor('the depth chart', () => $('[data-testid="depth-readout"]'), 60000);
    await shot('order-book-charts');
    await finish();
  } catch (e) {
    await snapshot('debug-failure').catch(() => null);
    await finish(e);
  }
})();
