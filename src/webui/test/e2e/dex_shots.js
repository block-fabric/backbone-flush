// The DEX's pool kinds and the launches, in pictures, against a live mainchain (run_sidechain.py --script dex_shots.js, the
// app with -testactivationheight=dex@0 -testactivationheight=dexcurves@0 -testactivationheight=ticker@0): four assets and
// pools of every kind made by RPC (a default GOLD/CHN pool, a stable USDX/USDY at A = 200 and 0.05 %, a weighted GOLD/CHN at
// 80/20, a constant-product GOLD/SILVER at 1 %); the pool wizard at its review for each curve, typed through the page so that
// the node previews it; the pages of the stable and the weighted pools with their depth; a swap whose best route mixes the
// 1 % pool and the weighted one; a concentrated USDX/USDY pool with three positions (one out of range) and a pool with a
// dynamic fee, a few trades, the range picker with a preview and the positions; the launch wizard at its review; then two tokens launched by RPC (MOON, with a display name,
// a description, an allocation and a first buy; STAR, with a few buys), the list of launches, MOON's page on its curve with
// a buy quoted, and once a buy of 600 CHN takes it past the curve's end, its page graduated; then limit orders of GOLD for
// USDX in its order book (the ladder, the wallet's orders), the market index, and a curator that verifies GOLD, trusted
// through the page: its Verified badge. Pictures in Midnight, Advanced.
(async () => {
  const { host, log, rpc, sleep, T, waitFor, check, snapshot, text, finish } = E2E;
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
  const set = (el, value) => {
    el.value = value;
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  };
  const button = (label, root = document) => $$('button', root).find((b) => b.textContent.trim().replace(/\s+/g, ' ').startsWith(label));
  async function click(label, root) {
    const b = await waitFor(`the button "${label}"`, () => {
      const el = button(label, typeof root === 'function' ? root() : root);
      return el && !el.disabled ? el : null;
    }, 60000);
    b.click();
    await sleep(250);
  }
  async function go(page, tab = '') {
    T().go(page, tab);
    await sleep(500);
  }
  async function shot(name) {
    const overlay = $$('dialog[open]').find((d) => /Synchronizing/.test(d.textContent));
    if (overlay && button('Hide', overlay)) {
      button('Hide', overlay).click();
      await sleep(300);
    }
    for (let i = 0; i < 10; i++) {
      const close = $$('.toasts button[aria-label="Dismiss"]');
      if (!close.length) break;
      close.forEach((b) => b.click());
      await sleep(150);
    }
    // Notices that arrive in the meantime (a block's coinbase) are not in the picture: hidden for the whole run.
    if (!document.getElementById('e2e-no-toasts')) {
      const hide = document.createElement('style');
      hide.id = 'e2e-no-toasts';
      hide.textContent = '.toasts { display: none !important; }';
      document.head.appendChild(hide);
    }
    await sleep(600);
    await snapshot(name);
  }
  /** Pick an asset in a picker of the page (ba/AssetPicker.svelte): open it, click the asset by its id. */
  async function pick(field, id) {
    const trigger = await waitFor(`the picker ${field}`, () => $(`#${field}`), 30000);
    trigger.click();
    const option = await waitFor(`the asset ${id.slice(0, 8)} in ${field}`, () => $(`[role="option"][data-id="${id}"]`), 30000);
    option.click();
    await sleep(300);
  }
  const W = 'dex';
  const CHN = '0'.repeat(64);
  const ids = {};
  /** Reserve a ticker, then register it once the reservation is deep enough (retried until the node takes it). */
  /** A wallet call made again until a block took the change before it (one asset change of the wallet waits a block). */
  async function retry(method, params, wallet = W) {
    return waitFor(`${method} accepted`, async () => rpc(method, params, wallet), 300000);
  }
  async function register(ticker, supply, decimals) {
    await rpc('reserveasset', [ticker], W);
    await waitFor(`${ticker} registered`, async () => (await rpc('registerasset', [ticker, supply, decimals, {}, true], W)).txid, 300000);
    const a = await waitFor(`${ticker} in a block`, async () => rpc('getasset', [ticker]), 300000);
    ids[ticker] = a.asset;
  }
  /** Launch a token once its reservation is deep enough (retried until the wallet takes it). */
  async function launch(ticker, options) {
    await rpc('reserveasset', [ticker], W);
    const done = await waitFor(`${ticker} launched`, async () => rpc('launchtoken', [ticker, options], W), 300000);
    await waitFor(`${ticker}'s launch in a block`, async () => rpc('getlaunch', [`0x${done.token}`]), 300000);
    ids[ticker] = done.token;
    return done.token;
  }
  /** The wizard of a pool (Trade, Create pool) at its review: two assets, a curve and its parameters, two amounts. */
  async function wizard(name, a, b, curve, setup, amountA, amountB) {
    await go('trade', 'swap');
    await go('trade', 'create');
    await pick('cp-a', a);
    await pick('cp-b', b);
    await click('Next');
    const radio = await waitFor(`the curve ${curve}`, () => $$('label.curve').find((l) => l.textContent.includes(curve)), 20000);
    $('input', radio).click();
    await sleep(300);
    if (setup) await setup();
    await click('Next');
    set(await waitFor('the first amount', () => $('#cp-amount-a'), 20000), amountA);
    set($('#cp-amount-b'), amountB);
    await waitFor('the preview of the node', () => $('[data-testid="pool-preview"]'), 60000);
    await click('Next');
    await waitFor('the review', () => text('.ba-page').includes('Review') && $('[data-testid="pool-preview"]'), 30000);
    await shot(name);
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
    const dex = await rpc('getdexinfo');
    await check('the pool kinds are in the rules (dexcurves@0)', dex.curves_active === true && dex.active === true, JSON.stringify(dex));

    // --- Four assets, and pools of every kind ---
    await register('USDX', 100000, 2);
    await register('USDY', 100000, 2);
    await register('GOLD', 100000, 2);
    await register('SILVER', 100000, 2);
    // A shallow default pool, a deep weighted one beside it at the same price: the route from SILVER takes the weighted one.
    await retry('addliquidity', ['GOLD', 100, 'CHN', 5], W);
    await retry('createpool', ['USDX', 5000, 'USDY', 5000, { curve: 'stable', fee: 0.05, amp: 200 }], W);
    await retry('createpool', ['GOLD', 4000, 'CHN', 50, { curve: 'weighted', weight_a: 80 }], W);
    await retry('createpool', ['GOLD', 3000, 'SILVER', 3000, { curve: 'constant_product', fee: 1 }], W);
    await waitFor('the four pools in a block', async () => (await rpc('listpools', [null, 100, 0])).length >= 4, 300000);
    await sleep(4000);

    // --- The pool wizard, one picture per curve ---
    await wizard('create-pool-constant-product', ids.GOLD, ids.SILVER, 'Constant product', async () => click('0.3%', () => $('[aria-label="Fee tier"]')), '10000', '12000');
    await wizard('create-pool-stable', ids.USDX, ids.USDY, 'Stable', null, '10000', '10000');
    await wizard('create-pool-weighted', ids.SILVER, CHN, 'Weighted', async () => click('80/20', () => $('[aria-label="Weights"]')), '40000', '20');

    // --- The pages of the stable and the weighted pools ---
    for (const [name, kind] of [['pool-page-stable', 'Stable'], ['pool-page-weighted', 'Weighted']]) {
      await go('trade', 'swap');
      await go('trade', 'pools');
      const row = await waitFor(`the ${kind} pool in the list`, () => $$('table[aria-label="Pools"] tbody tr').find((r) => r.textContent.includes(kind)), 60000);
      row.click();
      await waitFor('its depth', () => $('table[aria-label="Slippage"]') && $('[role="img"][aria-label^="Price impact"]'), 60000);
      await shot(name);
    }

    // --- A swap whose best route mixes the 1 % pool and the weighted one ---
    await go('trade', 'pools');
    await go('trade', 'swap');
    await pick('swap-pay', ids.SILVER);
    await pick('swap-get', CHN);
    set(await waitFor('the amount', () => $('#swap-amount')), '50');
    const ways = await waitFor('the ways to trade', () => $('ul[aria-label="Ways to trade"]'), 60000);
    const mixed = await waitFor('a route of the 1 % pool and the weighted one', () => $$('button', ways).find((b) => b.textContent.includes('Weighted') && b.textContent.includes('1%')), 30000);
    if (mixed.getAttribute('aria-pressed') !== 'true') mixed.click();
    await waitFor('the route panel', () => $('ol[aria-label="Route"]'), 20000);
    await check('the route goes through the 1 % pool and the weighted one', /1%/.test(text('ol[aria-label="Route"]')) && /Weighted/.test(text('ol[aria-label="Route"]')), text('ol[aria-label="Route"]'));
    await shot('trade-mixed-route');

    // --- Concentrated liquidity and a dynamic fee: a concentrated USDX/USDY pool at 1 with three positions (one out of
    // range), a constant-product GOLD/SILVER pool with a dynamic fee, a few trades through both ---
    const made = await retry('createpool', ['USDX', 1, 'USDY', 1, { curve: 'concentrated', fee: 0.05 }], W);
    const dyn = await retry('createpool', ['GOLD', 3000, 'SILVER', 3000, { curve: 'constant_product', fee: 0.3, dynamic_fee: true }], W);
    const clPool = await waitFor('the concentrated pool in a block', async () => rpc('getpool', [made.pool]), 300000);
    await waitFor('the dynamic pool in a block', async () => rpc('getpool', [dyn.pool]), 300000);
    const both = (x, y) => ({ [`0x${ids.USDX}`]: x, [`0x${ids.USDY}`]: y });
    await retry('openposition', [made.pool, 0.98, 1.02, both(500, 500)], W);
    await retry('openposition', [made.pool, 0.9, 1.1, both(300, 300)], W);
    // Above the price (the node's prices are of the pool's lower id in the other): out of range, one asset alone.
    await retry('openposition', [made.pool, 1.1, 1.2, both(200, 200)], W);
    await waitFor('the three positions in a block', async () => (await rpc('listpositions', [], W)).length >= 3, 300000);
    for (const [asset, amount, pool] of [['USDX', 40, made.pool], ['USDY', 30, made.pool], ['USDX', 25, made.pool], ['GOLD', 60, dyn.pool], ['SILVER', 40, dyn.pool]]) {
      await retry('swappools', [asset, amount, [pool]], W);
    }
    await waitFor('fees earned in range', async () => (await rpc('listpositions', [], W)).some((p) => Number(p.fees0) > 0 || Number(p.fees1) > 0), 300000);
    await check('the concentrated pool says its state', !!clPool.concentrated && clPool.curve === 'concentrated', JSON.stringify(clPool.concentrated));

    await go('trade', 'swap');
    await go('trade', 'pools');
    const clRow = await waitFor('the concentrated pool in the list', () => $$('table[aria-label="Pools"] tbody tr').find((r) => r.textContent.includes('Concentrated')), 60000);
    clRow.click();
    await waitFor('the liquidity by price', () => $('#cl-lower') && $$('rect.bar').length > 0, 60000);
    await click('±1%', () => $('[aria-label="Range presets"]'));
    set($('#cl-amount-a'), '100');
    set($('#cl-amount-b'), '100');
    await waitFor('the preview of the position', () => $('[data-testid="position-preview"]'), 60000);
    // The picker and its chart in view (below the pool's summary), from their headings.
    const heading = $$('h2, h3, h4, .title, strong').find((h) => h.textContent.trim() === 'Liquidity by price');
    (heading || $('#cl-lower')).scrollIntoView({ block: 'start' });
    await sleep(500);
    await shot('cl-range-picker');

    await go('trade', 'positions');
    await waitFor('the positions, in range and out of it', () => $('[data-testid="in-range"]') && $('[data-testid="out-of-range"]'), 60000);
    await shot('cl-positions');

    // --- The launch wizard, at its review ---
    await go('trade', 'launch');
    set(await waitFor('the ticker field', () => $('#launch-ticker'), 30000), 'COMET');
    await waitFor('the ticker free', () => text('.ba-page').includes('is free'), 30000);
    await click('Reserve the ticker');
    set(await waitFor('the display name', () => $('#launch-display'), 30000), 'Comet, a fair launch');
    set($('#launch-info'), 'A token launched on a bonding curve.\nhttps://example.org/comet');
    await click('Next');
    set(await waitFor('the first buy', () => $('#launch-first'), 20000), '0.5');
    set($('#launch-max'), '20');
    set($('#launch-allocation'), '1');
    await click('Next');
    await waitFor('the review', () => $('[data-testid="launch-review"]'), 20000);
    await waitFor('the reservation deep enough', () => text('.ba-page').includes('ready to launch'), 300000);
    await shot('launch-wizard');

    // --- Two launches by RPC ---
    const moon = await launch('MOON', { display_name: 'To the moon', info: 'A token for the moon.\nhttps://example.org/moon', allocation: 2, first_buy: 1 });
    const star = await launch('STAR', { display_name: 'Star dust', first_buy: 0.5 });
    // One buy a block: the launch's chart and log have a few points.
    for (const amount of [2, 5, 3]) {
      const at = await rpc('getblockcount', []);
      await retry('buyfromlaunch', [`0x${star}`, amount], W);
      await waitFor('the buy in a block', async () => (await rpc('getblockcount', [])) > at + 1, 300000);
    }
    await waitFor('STAR bought', async () => (await rpc('getlaunch', [`0x${star}`])).trades >= 2, 300000);

    await go('trade', 'swap');
    await go('trade', 'launches');
    await waitFor('the launches listed', () => $$('table[aria-label="Launches"] tbody tr').length >= 2, 60000);
    await shot('launches-list');

    const moonRow = await waitFor('MOON in the list', () => $$('table[aria-label="Launches"] tbody tr').find((r) => r.textContent.includes('MOON')), 30000);
    moonRow.click();
    set(await waitFor('the buy panel', () => $('#launch-amount'), 30000), '10');
    await waitFor('the quote of the node', () => $('[data-testid="launch-quote"]'), 30000);
    await shot('launch-page-curve');

    // --- A buy past the curve's end: MOON graduates ---
    await retry('buyfromlaunch', [`0x${moon}`, 600], W);
    await waitFor('MOON graduated', async () => (await rpc('getlaunch', [`0x${moon}`])).status === 'graduated', 300000);
    await waitFor('the page says it', () => $('[data-testid="launch-graduated"]'), 60000);
    await check('graduated: it trades in its pool', text('[data-testid="launch-graduated"]').includes('Graduated'), text('[data-testid="launch-graduated"]'));
    await shot('launch-page-graduated');

    // --- Order books: limit orders of GOLD for USDX, one a block, prices about 1: the bid at 1 crosses the ask at 0.98, the
    // others rest (a bid is USDX sold for GOLD, its limit in GOLD per USDX: 1 / its price in USDX) ---
    const orders = [
      ['GOLD', 20, 'USDX', 1.05],
      ['GOLD', 15, 'USDX', 1.1],
      ['GOLD', 10, 'USDX', 0.98],
      ['USDX', 19, 'GOLD', 1.25],
      ['USDX', 18, 'GOLD', 1.052631578947],
      ['USDX', 12, 'GOLD', 1],
    ];
    for (const [sell, amount, buy, price] of orders) {
      const at = await rpc('getblockcount', []);
      await retry('placeorder', [sell, amount, buy, price, 5760], W);
      await waitFor('the order in a block', async () => (await rpc('getblockcount', [])) > at + 1, 300000);
    }
    await waitFor('the orders in the wallet', async () => (await rpc('listorders', [], W)).length >= orders.length, 300000);
    const book = await waitFor('the book with both sides', async () => {
      const b = await rpc('getbook', ['GOLD', 'USDX']);
      return b.asks.length >= 2 && b.bids.length >= 2 ? b : null;
    }, 300000);
    await check('the book has asks and bids', book.asks.length >= 2 && book.bids.length >= 2, JSON.stringify(book));

    await go('trade', 'swap');
    await go('trade', 'book');
    await pick('book-base', ids.GOLD);
    await pick('book-quote', ids.USDX);
    await waitFor('the ladder', () => $$('[aria-label="Order book"] button.lvl.ask').length >= 2 && $$('[aria-label="Order book"] button.lvl.bid').length >= 2, 60000);
    set($('#order-amount'), '5');
    set($('#order-price'), '0.97');
    await waitFor('the preview of the order', () => $('[data-testid="order-preview"]'), 20000);
    await shot('order-book');

    const mine = await waitFor('my orders', () => $$('table[aria-label="My orders"] [data-testid="my-order"]').length >= 2 && $('table[aria-label="My orders"]'), 60000);
    mine.scrollIntoView({ block: 'center' });
    await sleep(400);
    await shot('my-orders');

    // --- The market index, every pair (newest first) ---
    await go('trade', 'markets');
    set(await waitFor('the sort of the pairs', () => $('select[aria-label="Sort pairs"]'), 30000), 'newest');
    await waitFor('the pairs', () => $$('table[aria-label="Markets"] tbody tr').length >= 4, 60000);
    await shot('markets');

    // --- Verified issuers: a curator verifies GOLD, trusted through the page ---
    const curator = await retry('registeroracle', ['curator'], W);
    await waitFor('the curator in a block', async () => {
      try {
        return (await rpc('getverifications', ['GOLD', [curator.oracle]])).curators.length === 1;
      } catch (_) {
        return false;
      }
    }, 300000);
    await retry('verifyasset', [curator.oracle, 'GOLD'], W);
    await waitFor('GOLD verified by the curator', async () => (await rpc('getverifications', ['GOLD', [curator.oracle]])).verified === true, 300000);
    await go('trade', 'swap');
    await go('trade', 'markets');
    const panel = await waitFor('the curators panel', () => $('details[aria-label="Verified issuers"]'), 30000);
    panel.open = true;
    set(await waitFor('the curator field', () => $('#curator-id'), 10000), curator.oracle);
    await click('Trust', () => panel);
    // Every trust is confirmed, the whole id shown (a name is a label anyone may give an oracle).
    const confirmBox = await waitFor('the confirmation of the curator', () => $('[data-testid="curator-confirm"]'), 30000);
    await check('the confirmation shows the whole id', confirmBox.textContent.includes(curator.oracle), confirmBox.textContent);
    await shot('curator-confirm');
    await click('Trust this id', () => confirmBox);
    await waitFor('the curator trusted', () => $('ul[aria-label="Curators trusted"]') && text('ul[aria-label="Curators trusted"]').includes('curator'), 30000);
    set($('select[aria-label="Sort pairs"]'), 'newest');
    await waitFor('the Verified badge', () => $('table[aria-label="Markets"] [data-testid="verified-badge"]'), 60000);
    await check('the badge names the curator', /curator/.test($('[data-testid="verified-badge"]').getAttribute('title') || ''), $('[data-testid="verified-badge"]').getAttribute('title'));
    await shot('verified-badge');
    await finish();
  } catch (e) {
    // The page as it was when it failed, and what it said.
    try {
      await log(`FAILED at: ${document.querySelector('main') ? document.querySelector('main').innerText.slice(0, 1500) : ''}`);
      await snapshot('debug-failure');
    } catch (_) {}
    await finish(e);
  }
})();
