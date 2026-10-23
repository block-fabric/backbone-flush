// Add liquidity (ba/LiquidityPanel.svelte) as a user does it, against a live mainchain (run_sidechain.py --script
// ba_liquidity.js, the app with -testactivationheight=dex@0 -testactivationheight=dexcurves@0): an asset whose pair with
// the coin has only a concentrated pool (the betanet's BROSEVELT/ECX, 2026-10-23: the form took that pool for the one
// addliquidity names and refused with "Nobody provides liquidity to this pool any more"); the form says a new pool,
// both amounts, and makes the constant-product pool beside it; then adds to that pool at its price; then, all its
// shares taken out (abandoned), reopens it.
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
      const el = button(label, typeof root === 'function' ? root() : root);
      return el && !el.disabled ? el : null;
    });
    b.click();
    await sleep(150);
  }
  async function submit(what) {
    const b = await waitFor(what, () => {
      const f = $('#lq-amount-a');
      const el = f && f.closest('form') && $('button[type="submit"]', f.closest('form'));
      return el && !el.disabled && el.textContent.includes(what) ? el : null;
    }, 30000);
    b.click();
    await sleep(150);
  }
  /** The dialog the submit opens: the one expected, never a warning instead. */
  async function answerDialog(title, label) {
    const d = await waitFor(`a dialog after the submit (expected "${title}")`, () => {
      const el = openDialog();
      return el && $('h2', el) ? el : null;
    }, 20000);
    const got = $('h2', d).textContent.trim();
    await check(`the dialog is "${title}"`, got === title && !!button(label, d), `${got}: ${d.textContent.trim().slice(0, 300)}`);
    await click(label, d);
    await waitFor(`the dialog "${title}" to go`, () => openDialog() !== d || !d.open);
  }
  const go = async (page, tab = '') => {
    T().go(page, tab);
    await sleep(700);
  };
  const retry = (method, params) => waitFor(`${method} accepted`, async () => rpc(method, params, W), 300000);
  const form = () => {
    const root = $('#lq-a') && $('#lq-a').closest('.ba-split');
    return root ? root.textContent.replace(/\s+/g, ' ') : '';
  };
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

    // An asset as on the betanet (2 decimals, 1,000,000), and a concentrated pool of it with the coin, alone.
    await rpc('reserveasset', ['BROS'], W);
    await waitFor('BROS registered', async () => (await rpc('registerasset', ['BROS', 1000000, 2, {}, true], W)).txid, 300000);
    const bros = await waitFor('BROS in a block', async () => rpc('getasset', ['BROS']), 300000);
    const made = await retry('createpool', ['BROS', 100, COIN, 0.01, { curve: 'concentrated', fee: 0.05 }]);
    await waitFor('the concentrated pool in a block', async () => rpc('getpool', [made.pool]), 300000);
    const none = await rpc('getpool', ['BROS', COIN]).then(() => 'a pool', (e) => e.message);
    await check('getpool by the pair: no pool (the concentrated one is not the kind addliquidity names)', /There is no pool of these assets/.test(none), none);

    // --- A new pool, beside the concentrated one ---
    await go('trade', 'liquidity');
    await waitFor('the liquidity form', () => $('#lq-a'));
    $('#lq-a').click();
    set(await waitFor('the asset search', () => $('.asset-picker input[type="search"]')), 'BROS');
    (await waitFor('BROS in the picker', () => $(`[role="option"][data-id="${bros.asset}"]`), 30000)).click();
    await waitFor('BROS picked', () => $('#lq-a').dataset.value === bros.asset);
    await waitFor('the form says a new pool', () => form().includes('A new pool:') && form().includes('Concentrated pool'), 30000);
    await check('both amounts asked (the second not worked out)', !$('#lq-amount-b').readOnly && !form().includes('Nobody provides liquidity'), form().slice(0, 400));
    set($('#lq-amount-a'), '1000');
    set($('#lq-amount-b'), '1');
    await waitFor('the preview with its price', () => form().includes('Its price') && form().includes('Share of the pool'), 20000);
    await snapshot('ba-liquidity-new');
    await submit('Make the pool');
    await answerDialog('Make a pool', 'Make it');
    const plain = await waitFor('the constant-product pool made', async () => rpc('getpool', ['BROS', COIN]).catch(() => null), 120000);
    await check('made at 0.001 ECX, the default kind, beside the concentrated pool', plain.default_kind !== false && Math.abs(plain.price - 0.001) < 1e-9 && plain.pool !== made.pool, JSON.stringify(plain));

    // --- Into that pool, at its price ---
    await waitFor('the form shows the pool', () => form().includes('the pool holds'), 60000);
    await check('the second amount worked out from the pool\'s price', $('#lq-amount-b').readOnly, form().slice(0, 400));
    set($('#lq-amount-a'), '100');
    await waitFor('the second amount filled', () => Math.abs(Number($('#lq-amount-b').value) - 0.1) < 0.001, 20000);
    await snapshot('ba-liquidity-add');
    const shares0 = Number(plain.shares);
    await submit('Add liquidity');
    await answerDialog('Add liquidity', 'Add');
    await waitFor('the liquidity added', async () => Number((await rpc('getpool', ['BROS', COIN])).shares) > shares0, 120000);
    await check('added to the pool at its price', true);

    // --- Every share taken out: abandoned, then reopened from the form ---
    await retry('removeliquidity', ['BROS', COIN, 100]);
    await waitFor('the pool abandoned', async () => (await rpc('getpool', ['BROS', COIN])).abandoned, 120000);
    await waitFor('the form says nobody provides to it', () => form().includes('Nobody provides liquidity to this pool any more.'), 60000);
    await check('both amounts asked again', !$('#lq-amount-b').readOnly);
    set($('#lq-amount-a'), '500');
    set($('#lq-amount-b'), '1');
    await waitFor('the preview with its price', () => form().includes('Its price'), 20000);
    await snapshot('ba-liquidity-reopen');
    await submit('Make the pool');
    await answerDialog('Reopen a pool', 'Make it');
    const reopened = await waitFor('the pool reopened', async () => {
      const p = await rpc('getpool', ['BROS', COIN]);
      return p.abandoned ? null : p;
    }, 120000);
    await check('reopened at 0.002 ECX', Math.abs(reopened.price - 0.002) < 1e-6, JSON.stringify(reopened));
    await rpc('setbmm', [false]);
    await finish();
  } catch (e) {
    await finish(e instanceof Error ? e : new Error(e && e.message ? `${e.message} (${e.code})` : JSON.stringify(e)));
  }
})();
