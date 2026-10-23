// Swap (ba/SwapPanel.svelte) as a user does it, against a live mainchain (run_sidechain.py --script ba_swap_kinds.js, the
// app with -testactivationheight=dex@0 -testactivationheight=dexcurves@0): a pair whose only pool is concentrated (the
// betanet's BROSEVELT/ECX, 2026-10-23: the panel took that pool for the pair's constant-product pool, quoted it by
// quoteswap and could send swapasset to a pool that does not exist) swaps in that pool, by its id, the panel naming it;
// then, a constant-product pool beside it at a better price, the panel picks that one and the swap goes there.
(async () => {
  const { rpc, host, sleep, T, waitFor, check, snapshot, finish } = E2E;
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
  const W = 'ba';
  const COIN = 'ECX';
  const set = (el, value) => {
    el.value = value;
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  };
  const openDialog = () => $$('dialog[open]').pop();
  const button = (label, root = document) => $$('button', root).find((b) => b.textContent.trim().replace(/\s+/g, ' ').startsWith(label));
  async function click(label, root) {
    const b = await waitFor(`the button "${label}"`, () => {
      const el = button(label, root);
      return el && !el.disabled ? el : null;
    });
    b.click();
    await sleep(150);
  }
  const panel = () => {
    const root = $('#swap-amount') && $('#swap-amount').closest('.ba-split');
    return root ? root.textContent.replace(/\s+/g, ' ') : '';
  };
  const venue = () => ($('[data-testid="swap-venue"]') ? $('[data-testid="swap-venue"]').textContent.replace(/\s+/g, ' ').trim() : '');
  /** The amount typed, again should the panel have cleared it meanwhile (the pair chosen, a block). */
  const typeAmount = (v) => waitFor(`the amount ${v} typed`, async () => {
    if ($('#swap-amount').value !== v) set($('#swap-amount'), v);
    await sleep(400);
    return $('#swap-amount').value === v;
  }, 20000);
  const go = async (page, tab = '') => {
    T().go(page, tab);
    await sleep(700);
  };
  const retry = (method, params) => waitFor(`${method} accepted`, async () => rpc(method, params, W), 300000);
  const held = async (asset) => {
    const h = (await rpc('listmyassets', [], W)).assets.find((a) => a.asset === asset);
    return h ? Number(h.balance) : 0;
  };
  /** Swap 0.0005 ECX from the panel; the dialog's text, once confirmed. */
  async function swap() {
    const b = await waitFor('the Swap button', () => {
      const f = $('#swap-amount');
      const el = f && f.closest('form') && $('button[type="submit"]', f.closest('form'));
      return el && !el.disabled ? el : null;
    }, 30000);
    b.click();
    const d = await waitFor('the dialog "Swap"', () => {
      const el = openDialog();
      return el && $('h2', el) && $('h2', el).textContent.trim() === 'Swap' ? el : null;
    }, 20000);
    const said = d.textContent.replace(/\s+/g, ' ');
    await click('Swap', d);
    await waitFor('the dialog to go', () => openDialog() !== d || !d.open);
    return said;
  }
  try {
    await host('test.confirmAnswer', { always: true });
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
    await waitFor('the node to reach the mainchain', async () => (await rpc('getmainchaininfo')).connected, 60000);
    await rpc('createwallet', [W]);
    await waitFor('the wallet in the page', () => T().wallets.current === W);
    const deposit = await rpc('getdepositaddress', [], W);
    await host('test.confirmAnswer', { yes: true });
    await host('peg.deposit', { destination: deposit.depositaddress, amount: '20' });
    await rpc('setbmm', [true, await rpc('getnewaddress', [], W), true]);
    await waitFor('the deposit credited', async () => (await rpc('getbalances', [], W)).mine.trusted >= 20, 240000);
    T().savePrefs({ layout: 'dashboard', theme: 'midnight' });

    // An asset as on the betanet (2 decimals), and a concentrated pool of it with the coin at 0.05%, alone.
    await rpc('reserveasset', ['BROS'], W);
    await waitFor('BROS registered', async () => (await rpc('registerasset', ['BROS', 1000000, 2, {}, true], W)).txid, 300000);
    const bros = await waitFor('BROS in a block', async () => rpc('getasset', ['BROS']), 300000);
    const cl = await retry('createpool', ['BROS', 10000, COIN, 1, { curve: 'concentrated', fee: 0.05 }]);
    const made = await waitFor('the concentrated pool in a block', async () => rpc('getpool', [cl.pool]), 300000);
    // Made at its price, nothing in it: a position around that price (half to twice it), as on the betanet. Its range is
    // of the pool's first asset (the coin) in its second: BROS per ECX, where getpool says ECX per BROS.
    const p = /^0+$/.test(made.asset_a_id) ? Number(made.price) : 1 / Number(made.price);
    await retry('openposition', [cl.pool, p / 2, p * 2, { [`0x${bros.asset}`]: 10000, [COIN]: 1 }]);
    await waitFor('the position in a block', async () => Number((await rpc('getpool', [cl.pool])).concentrated?.liquidity ?? 0) > 0, 300000);
    const none = await rpc('getpool', ['BROS', COIN]).then(() => 'a pool', (e) => e.message);
    await check('no constant-product pool of the pair', /There is no pool of these assets/.test(none), none);

    // --- The concentrated pool alone: the swap goes to it ---
    await go('trade', 'swap');
    await waitFor('the pair chosen', () => $('#swap-get') && $('#swap-get').dataset.value === bros.asset, 30000);
    await typeAmount('0.0005');
    await waitFor('the quote names the concentrated pool', () => venue().includes('Concentrated pool at 0.05%') && venue().includes(cl.pool.slice(0, 8)), 30000).catch(async (e) => {
      const dex = await rpc('getdexinfo').catch((x) => x.message);
      const q = await rpc('getswapquote', [COIN, 0.0005, bros.asset, { max_hops: 0 }]).catch((x) => x.message);
      const r = await rpc('getpool', [cl.pool]).catch((x) => x.message);
      await check('the panel', false, `${panel().slice(0, 600)} | value ${$('#swap-amount').value} | dex ${JSON.stringify(dex)} | quote ${JSON.stringify(q).slice(0, 600)} | route ${JSON.stringify(r).slice(0, 400)}`);
      throw e;
    });
    await check('no error of quoteswap shown (no pool of the default kind is asked for)', !panel().includes('There is no pool'), panel().slice(0, 500));
    await snapshot('ba-swap-cl-only');
    let before = await held(bros.asset);
    let said = await swap();
    await check('the dialog says the concentrated pool', said.includes('Concentrated') && said.includes('0.05%'), said.slice(0, 400));
    await waitFor('BROS bought', async () => (await held(bros.asset)) > before, 180000);
    const clAfter = await rpc('getpool', [cl.pool]);
    await check('the swap went through the concentrated pool', clAfter.swaps >= 1, JSON.stringify(clAfter));

    // --- A constant-product pool beside it, at a better price for the buyer (0.00001 ECX against 0.0001): picked ---
    await retry('addliquidity', ['BROS', 100000, COIN, 1]);
    const cp = await waitFor('the constant-product pool in a block', async () => rpc('getpool', ['BROS', COIN]).catch(() => null), 300000);
    await check('a pool of the default kind beside the concentrated one', cp.default_kind !== false && cp.pool !== cl.pool, JSON.stringify(cp));
    await typeAmount('0.0004');
    await typeAmount('0.0005');
    await waitFor('the quote names the constant-product pool', () => venue().includes('Constant product pool at 0.3%') && venue().includes(cp.pool.slice(0, 8)), 60000);
    await waitFor('both pools offered, the better first', () => $$('ul.ways li').length >= 2 && $('ul.ways li').textContent.includes('best'), 30000);
    await snapshot('ba-swap-cp-and-cl');
    before = await held(bros.asset);
    const clSwaps = clAfter.swaps;
    said = await swap();
    await waitFor('BROS bought again', async () => (await held(bros.asset)) > before, 180000);
    const cpAfter = await rpc('getpool', [cp.pool]);
    const clNow = await rpc('getpool', [cl.pool]);
    await check('the swap went through the constant-product pool, not the concentrated one', cpAfter.swaps >= 1 && clNow.swaps === clSwaps, `${JSON.stringify(cpAfter)} ${JSON.stringify(clNow)}`);
    // Bought at the constant-product pool's price: about 0.0005 / 0.00001 = 50 BROS (less its fee), far more than the 5 of the other.
    const got = (await held(bros.asset)) - before;
    await check('about 50 BROS for 0.0005 ECX', got > 40 && got <= 50, String(got));
    await rpc('setbmm', [false]);
    await finish();
  } catch (e) {
    await finish(e instanceof Error ? e : new Error(e && e.message ? `${e.message} (${e.code})` : JSON.stringify(e)));
  }
})();
