// The links of flush.markets (doc/flush-links.md), against a live mainchain (run_sidechain.py --script links.js, the app
// with -testactivationheight=dex@0 -testactivationheight=dexcurves@0 -testactivationheight=ticker@0): two launches by RPC
// (TIDE on its curve, GRAD graduated into its pool), then each verb as a link of the site would come: from the browser (a
// second start of the app hands it over: test.openLink, what SingleInstance delivers), pasted into Open URI, or a launch
// file dropped on the window. Each one's question in the host's dialog window, in the Launchpad look, read there (where
// it comes from, what it asks, the quote, the floor, the command, the assets resolved), pictured and accepted there; then
// what it did on the chain: a buy, a sale, a swap in the graduated pool, a launch from a launch file (both steps, its logo,
// its first buy), a launch from a link, a connect (the callback produced; a test run opens no browser). Then the refusals:
// another network, expired, used twice, a ticker that is not the token's, a floor the curve cannot meet, an unknown
// parameter, a callback elsewhere, a launch file with another logo.
(async () => {
  const { host, rpc, sleep, T, waitFor, check, snapshot, finish } = E2E;
  const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
  const dialog = (action, more = {}) => host('test.dialog', { action, ...more });
  const kind = () => dialog('kind');
  const W = 'links';
  const NET = 'regtest';
  const ARMED = 2600;
  const now = () => Math.floor(Date.now() / 1000);
  const enc = (v) => encodeURIComponent(v).replace(/[!'()*]/g, (c) => `%${c.charCodeAt(0).toString(16).toUpperCase()}`);
  /** A link as the site writes it (flush-web src/lib/wallet/uri.ts). */
  const link = (verb, params, { exp = now() + 600, network = NET } = {}) =>
    `flush:${verb}?` + [['v', '1'], ['network', network], ['exp', String(exp)], ...params].map(([k, v]) => `${k}=${enc(v)}`).join('&');
  /** An amount as the site writes it: rounded down to `places`, no trailing zeros. */
  const amount = (n, places) => {
    const s = (Math.floor(Number(n) * 10 ** places + 1e-6) / 10 ** places).toFixed(places);
    return s.includes('.') ? s.replace(/0+$/, '').replace(/\.$/, '') : s;
  };
  const STATE = `(() => {
    const q = (s) => document.querySelector(s);
    return {
      title: q('#dialog-title') && q('#dialog-title').textContent,
      message: [...document.querySelectorAll('.message p')].map((p) => p.textContent).join(' / '),
      accept: q('#dialog-accept') && q('#dialog-accept').textContent,
      rows: [...document.querySelectorAll('#dialog-rows .row')].map((r) => {
        const dt = r.querySelector('dt'), dd = r.querySelector('dd');
        return (dt ? dt.textContent + ' ' + (dd ? dd.textContent : '') : r.textContent).replace(/\\s+/g, ' ').trim();
      }),
      img: !!document.querySelector('#dialog-rows img'),
      theme: document.documentElement.dataset.theme,
      loading: document.body.classList.contains('loading'),
    };
  })()`;
  async function shown(what) {
    await waitFor(`${what}: the dialog window`, async () => (await kind()) === 'themed', 120000);
    return waitFor(`${what}: its page`, async () => {
      const s = await dialog('eval', { script: STATE });
      return s && !s.loading && s.title ? s : null;
    }, 30000);
  }
  /** A link's question: shown in the window, checked, pictured, accepted there as a user would. */
  async function confirmLink(name, open, expect) {
    await host('test.confirmAnswer', { show: true });
    const pending = open();
    // The window, or a message of the app saying why the link was refused (then failed at once, with it).
    const first = await waitFor(`${name}: the dialog window`, async () => {
      if ((await kind()) === 'themed') return 'window';
      const said = $$('dialog[open]').find((x) => /refused|not done/i.test(x.textContent));
      return said ? said.textContent : false;
    }, 120000);
    if (first !== 'window') throw new Error(`${name}: the app refused it: ${first.slice(0, 300)}`);
    const s = await shown(name);
    const rows = s.rows.join('\n');
    await check(`${name}: in the Launchpad look`, s.theme === 'launchpad', s.theme);
    for (const [label, ok] of expect(s, rows)) await check(`${name}: ${label}`, ok, `${s.title} | ${s.message.slice(0, 300)} | ${rows.slice(0, 1500)}`);
    await dialog('snapshot', { name: `link-${name}` });
    await sleep(ARMED);
    await dialog('eval', { script: `document.getElementById('dialog-accept').click(); true` });
    await waitFor(`${name}: the window gone`, async () => (await kind()) === 'none', 30000);
    return pending;
  }
  /** A link refused: the app says why (a message of the app), nothing asked. */
  async function refused(name, open, why, picture = false) {
    const asked = (await host('test.confirmed')).length;
    await open();
    // Messages said before (a launch sent) wait their turn in the page: dismissed until this one shows.
    const d = await waitFor(`${name}: the app says why`, () => {
      const open = $$('dialog[open]');
      const it = open.find((x) => x.textContent.includes(why));
      if (it) return it;
      const other = open.find((x) => /Launched|Registered|Link refused/.test(x.textContent));
      const ok = other && [...other.querySelectorAll('button')].find((b) => b.textContent.trim() === 'OK');
      if (ok) ok.click();
      return null;
    }, 30000).catch(() => null);
    await check(`refused, ${name}: "${why}"`, !!d, $$('dialog[open]').map((x) => x.textContent).join(' | ').slice(0, 400));
    await check(`refused, ${name}: nothing asked`, (await kind()) === 'none' && (await host('test.confirmed')).length === asked);
    if (d && picture) await snapshot(`link-refused-${name.replace(/[^a-z0-9]+/gi, "-")}`);
    const ok = d && [...d.querySelectorAll('button')].find((b) => b.textContent.trim() === 'OK');
    if (ok) ok.click();
    await sleep(300);
  }
  const fromBrowser = (uri) => () => host('test.openLink', { uri });
  /** A wallet call made again until a block took the change before it. */
  const retry = (method, params) => waitFor(`${method} accepted`, async () => rpc(method, params, W), 300000);
  async function launch(ticker, options) {
    await rpc('reserveasset', [ticker], W);
    const done = await waitFor(`${ticker} launched`, async () => rpc('launchtoken', [ticker, options], W), 300000);
    await waitFor(`${ticker}'s launch in a block`, async () => rpc('getlaunch', [`0x${done.token}`]), 300000);
    return done.token;
  }
  /** What the wallet holds of an asset, in blocks and coming. */
  const balance = async (id) => {
    const held = await rpc('listmyassets', [], W).catch(() => ({ assets: [] }));
    const a = (held.assets || []).find((x) => x.asset === id);
    return a ? Number(a.balance) + Number(a.pending || 0) : 0;
  };
  // A logo the rules take (src/test/bitassets_logo_samples.h, LOGO_PNG_PALETTE: 256 x 256, no ancillary chunk).
  const LOGO_HEX =
    '89504e470d0a1a0a0000000d4948445200000100000001000203000000211c40f500000009504c5445f0b4281e78c8141e3c18fdc304000002274944' +
    '415478daed9b4b6ec42010444b5efa283e452e91fbcc5158a29c32511689cdc7ee7261819866cf53f783896228f0250e38c0010e7080031cf04e8088' +
    '9fb1de06fc4ebf429c0002fec67207b09b7f46c065fd175d540148c646025e29001c2066f36b4dc0d640bd095cafc0f94ac05c40a504980ba894007b' +
    '01e512602fa05c024c7be0642fc0b4074ef60298024a25c0aeb0ac114c07a51e407550e8015407851e407550e8015c07790fe03ac87b00d741de03ac' +
    'bf83daef01a4824c0248059904900a320920156412c02a48258055904a00ab20950056412a01b482440268058904d00a1209a0152412da026c0e8f16' +
    'c13b3c5a04eff068b1292058014b05607578b0881b0e0f165b02a21db03e030876c05204d81761bf0c2d01c4fcdd3234044406b03e01080c607902c0' +
    'ace26e1d1b02a8f9ffeb3810207280b53d207080654040ba8f3e93f151d94903014002b6e1009105acf301020b581c90015e2c000e9811001ab039c0' +
    '010e70c0a800ffb3ee00ff37af11c03f38a6f86e9ce1eb7d823394fe0751139ce60d7024daff58b8ffd178ffeb8101ae48e45b9efe1755fd2febf4fb' +
    '46f9c6b3ffa5ad7cefacdf7c07de6163809c3f9013107a0623d00a5ae750e4248c9ec509ac82e679243911a567b202a9a07d2e4d4ec6e9d93c391d28' +
    'e713f584a49cd19453a27a4e554ecaca595d3d2d2ce795f5c4b49cd9d653e3726e5d4fcecbd97dfdf580fe7e417e41a1bfe1d05f91e8ef584cc3010e' +
    '7080031ce080b100dfb12b5a1a10a74c9e0000000049454e44ae426082';
  async function logo() {
    const bytes = new Uint8Array(LOGO_HEX.match(/../g).map((b) => parseInt(b, 16)));
    const hash = [...new Uint8Array(await crypto.subtle.digest('SHA-256', bytes))].map((b) => b.toString(16).padStart(2, '0')).join('');
    let bin = '';
    for (const b of bytes) bin += String.fromCharCode(b);
    return { base64: btoa(bin), sha256: hash, size: bytes.length };
  }
  /** A launch file dropped on the window, as a user drops the file the site saved. */
  function drop(name, text) {
    const dt = new DataTransfer();
    dt.items.add(new File([text], name, { type: 'application/json' }));
    window.dispatchEvent(new DragEvent('drop', { dataTransfer: dt, bubbles: true, cancelable: true }));
  }

  try {
    await host('test.confirmAnswer', { always: true });
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
    await waitFor('the node to reach the mainchain', async () => (await rpc('getmainchaininfo')).connected, 60000);
    T().savePrefs({ layout: 'dashboard', tipsDone: true, theme: 'launchpad', uiMode: 'advanced' });

    await rpc('createwallet', [W]);
    await waitFor('the wallet in the page', () => T().wallets.current === W);
    const deposit = await rpc('getdepositaddress', [], W);
    await host('test.confirmAnswer', { yes: true });
    await host('peg.deposit', { destination: deposit.depositaddress, amount: '900' });
    await rpc('setbmm', [true, await rpc('getnewaddress', [], W), true]);
    await waitFor('the deposit credited', async () => (await rpc('getbalances', [], W)).mine.trusted >= 900, 300000);
    await check('launches are in the rules (dexcurves@0)', (await rpc('getlaunchstats')).rules.active === true);

    // --- Two launches: TIDE on its curve (the wallet holds some), GRAD past its curve's end, into its pool ---
    const tide = await launch('TIDE', { display_name: 'Tide Pool', first_buy: 2 });
    const grad = await launch('GRAD', { display_name: 'Graduate', first_buy: 1 });
    await retry('buyfromlaunch', [`0x${grad}`, 600]);
    const graduated = await waitFor('GRAD graduated', async () => {
      const l = await rpc('getlaunch', [`0x${grad}`]);
      return l.status === 'graduated' ? l : null;
    }, 300000);
    const pool = graduated.pool;
    await waitFor('TIDE held', async () => (await balance(tide)) > 0 || (await rpc('getlaunch', [`0x${tide}`])).trades >= 1, 300000);
    await sleep(3000);
    // The questions of the links are shown (each queued answer: show); anything else stays answered by the test.
    await host('test.confirmAnswer', { clear: true });

    // --- buy, from the browser ---
    const q = await rpc('quotelaunch', [`0x${tide}`, 5]);
    const minTokens = amount(Number(q.amount_out) * 0.99, 6);
    const buy = link('buy', [['launch', 'TIDE'], ['asset', tide], ['ecx', '5'], ['min_tokens', minTokens], ['slippage_bps', '100']]);
    const before = await rpc('getblockcount');
    const tideHeld = await balance(tide);
    await confirmLink('buy', fromBrowser(buy), (s, rows) => [
      ['where it comes from, first', /^From a link opened in your browser — check it/.test(s.rows[0])],
      ['what it asks', rows.includes('It asks Buy TIDE with 5 ECX')],
      ['the quote of now and the floor', rows.includes('Quote now') && rows.includes(`Least it gives ${minTokens} TIDE`)],
      ['the slippage the site showed, shown only', rows.includes('1.00 % (shown only: the floor binds)')],
      ['the token resolved by its id', rows.includes(`TIDE — Tide Pool`) && rows.includes(tide.slice(0, 8))],
      ['the command and its floor', rows.includes('Command buyfromlaunch') && rows.includes(minTokens)],
      ['the warning', rows.includes('Anyone can make a link like this')],
      ['the title and the button of the spend', /Buy from a launch/.test(s.title) && s.accept === 'Buy'],
    ]);
    await waitFor('the buy in a block', async () => (await balance(tide)) > tideHeld && (await rpc('getblockcount')) > before, 300000);
    await check('buy: TIDE received, at least the floor', (await balance(tide)) - tideHeld >= Number(minTokens));

    // --- sell, pasted into Open URI ---
    await waitFor('TIDE to sell', async () => (await balance(tide)) >= 1000000, 300000);
    const tideBefore = await balance(tide);
    const qs = await rpc('quotelaunch', [`0x${tide}`, 1000000, 'sell']);
    const minEcx = amount(Number(qs.amount_out) * 0.99, 8);
    const sell = link('sell', [['launch', 'TIDE'], ['asset', tide], ['tokens', '1000000'], ['min_ecx', minEcx], ['slippage_bps', '100']]);
    T().go('send', 'uri');
    const input = await waitFor('the Open URI field', () => document.getElementById('uri-input'), 20000);
    input.value = sell;
    input.dispatchEvent(new Event('input', { bubbles: true }));
    await confirmLink('sell', async () => {
      await sleep(200);
      document.getElementById('uri-form').requestSubmit();
    }, (s, rows) => [
      ['pasted, it says so', /^From a link pasted or dropped into the app — check it/.test(s.rows[0])],
      ['what it asks', rows.includes('It asks Sell 1000000 TIDE')],
      ['the floor in ECX', rows.includes(`Least it gives ${minEcx} ECX`)],
      ['the command', rows.includes('Command selltolaunch')],
    ]);
    await waitFor('the sale in a block', async () => (await balance(tide)) <= tideBefore - 1000000, 300000);
    await check('sell: 1000000 TIDE went', (await balance(tide)) <= tideBefore - 1000000);

    // --- swap, in the graduated pool, from the browser ---
    const swapQuote = await rpc('getswapquote', ['ECX', 2, `0x${grad}`, { max_hops: 0 }]);
    const inPool = (swapQuote.pools || []).find((p) => p.pool === pool) || swapQuote.pools[0];
    const minOut = amount(Number(inPool.amount_out) * 0.99, 6);
    const swap = link('swap', [['pool', pool], ['from', 'ECX'], ['to', 'GRAD'], ['amount', '2'], ['min_out', minOut], ['slippage_bps', '100']]);
    const gradBefore = await balance(grad);
    await confirmLink('swap', fromBrowser(swap), (s, rows) => [
      ['what it asks', rows.includes('It asks Swap 2 ECX for GRAD')],
      ['the pool resolved: its two assets and its id', rows.includes(`Pool 1`) && rows.includes(pool.slice(0, 8)) && rows.includes('GRAD')],
      ['the floor', rows.includes(`Least it gives ${minOut} GRAD`)],
      ['the command', rows.includes('Command swaproute')],
    ]);
    await waitFor('the swap in a block', async () => (await balance(grad)) > gradBefore, 300000);
    await check('swap: GRAD received', (await balance(grad)) > gradBefore);

    // --- launch, from a launch file dropped on the window ---
    const art = await logo();
    await check('the logo drawn is within the rules', art.size <= 32768, String(art.size));
    const file = {
      format: 'flush-launch', version: 1, network: NET, ticker: 'LUMEN', name: 'Lumen Works', description: 'Light for night markets.',
      links: ['https://lumen.example'], logo: { type: 'image/png', sha256: art.sha256, base64: art.base64 },
      first_buy: { ecx: '3', min_tokens: '1', slippage_bps: 100 },
    };
    await confirmLink('launch-file', async () => drop('LUMEN.flushlaunch.json', JSON.stringify(file)), (s, rows) => [
      ['a launch file, it says so', /^From a launch file you opened it — check it/.test(s.rows[0])],
      ['both steps in one question', rows.includes('Step 1 of 2') && rows.includes('Step 2 of 2') && s.accept === 'Reserve and launch'],
      ['the ticker, the display name, the description and links', rows.includes('Ticker LUMEN') && rows.includes('Display name Lumen Works') && rows.includes('Light for night markets.')],
      ['the logo as a picture, checked against its hash', s.img && rows.includes(art.sha256.slice(0, 16))],
      ['the first buy and its floor on chain', rows.includes('First buy, paid now') && rows.includes('First buy gets at least 1 units')],
    ]);
    const lumen = await waitFor('LUMEN launched by the app once its reservation was deep enough', async () => rpc('getlaunch', ['LUMEN']), 600000);
    const lumenAsset = await rpc('getasset', ['LUMEN']);
    await check('launch-file: the token has its logo, display name and first buy', !!lumenAsset.logo && lumenAsset.display_name === 'Lumen Works' && Number(lumen.trades) >= 1, JSON.stringify({ logo: lumenAsset.logo, name: lumenAsset.display_name, trades: lumen.trades }));

    // --- launch, from a link (no logo), from the browser ---
    const launchLink = link('launch', [['ticker', 'COMET'], ['name', 'Comet'], ['desc', 'A comet.'], ['link', 'https://comet.example'], ['ecx', '1'], ['min_tokens', '1'], ['slippage_bps', '100']], { exp: now() + 3600 });
    await confirmLink('launch-link', fromBrowser(launchLink), (s, rows) => [
      ['what it asks', rows.includes('It asks Launch COMET (Comet) and buy with 1 ECX')],
      ['both steps', rows.includes('Step 1 of 2') && rows.includes('Command reserveasset') && rows.includes('Command launchtoken')],
    ]);
    await waitFor('COMET launched', async () => rpc('getlaunch', ['COMET']), 600000);
    await check('launch-link: COMET is on its curve', true);

    // --- register (the same wizard as a launch, no market), then that asset put on a curve, then an order: from the browser ---
    const regLink = link('register', [['ticker', 'REGONE'], ['name', 'Reg One'], ['desc', 'Registered from a link.'], ['supply', '1000000'], ['decimals', '6']], { exp: now() + 3600 });
    await confirmLink('register-link', fromBrowser(regLink), (s, rows) => [
      ['what it asks', rows.includes('It asks Register REGONE (Reg One): 1000000 of 6 decimals to this wallet, no market')],
      ['both steps', rows.includes('Step 1 of 2') && rows.includes('Command reserveasset') && rows.includes('Command registerasset')],
    ]);
    const reg = await waitFor('REGONE registered by the app once its reservation was deep enough', async () => rpc('getasset', ['REGONE']), 600000);
    await check('register-link: its supply and display name', Number(reg.supply) === 1000000 && reg.display_name === 'Reg One', JSON.stringify(reg));
    await waitFor('REGONE in the wallet', async () => (await balance(reg.asset)) >= 1000000, 300000);
    const curveLink = link('curve', [['asset', reg.asset], ['ticker', 'REGONE'], ['amount', '800000'], ['fee_bps', '100']]);
    await confirmLink('curve-link', fromBrowser(curveLink), (s, rows) => [
      ['what it asks', rows.includes('It asks Put 800000 REGONE on a bonding curve at 1.00 %')],
      ['the parts', rows.includes('600000 REGONE sold for ECX') && rows.includes('200000 REGONE into the locked pool')],
      ['the command', rows.includes('Command curveasset')],
    ]);
    const onCurve = await waitFor('REGONE on its curve', async () => rpc('getlaunch', ['REGONE']), 300000);
    await check('curve-link: an existing asset on a curve, its supply fixed', onCurve.from_asset === true && (await rpc('getasset', ['REGONE'])).fixed === true, JSON.stringify(onCurve));
    const orderLink = link('order', [['sell', grad], ['sell_ticker', 'GRAD'], ['buy', 'ECX'], ['buy_ticker', 'ECX'], ['amount', '10'], ['price', '0.5'], ['expiry_blocks', '1000']]);
    await confirmLink('order-link', fromBrowser(orderLink), (s, rows) => [
      ['what it asks', rows.includes('It asks Sell 10 GRAD for ECX at 0.5 ECX each at least, for 1000 blocks')],
      ['the command', rows.includes('Command placeorder')],
    ]);
    await waitFor('the order in its book', async () => (await rpc('listorders', [], W)).length > 0 || null, 300000);
    await refused('curve of an asset on a curve', fromBrowser(link('curve', [['asset', reg.asset], ['ticker', 'REGONE'], ['amount', '8'], ['fee_bps', '100']])), 'was launched or put on a curve already');

    // --- connect, from the browser: the callback with the address, the nonce and the network ---
    const nonce = 'e2e0123456789abcdef01234';
    const connect = link('connect', [['origin', 'https://flush.markets'], ['callback', 'https://flush.markets/connect'], ['nonce', nonce]]);
    let shared = '';
    await confirmLink('connect', fromBrowser(connect), (s, rows) => {
      const row = s.rows.find((r) => r.startsWith('Address shared'));
      shared = row ? row.replace('Address shared', '').trim() : '';
      return [
        ['the site and where the answer goes', rows.includes('Site https://flush.markets') && rows.includes('The answer goes to https://flush.markets/connect')],
        ['the address shared, whole', /^rflx1[a-z0-9]{20,}$|^[a-z]+1[a-z0-9]{20,}$/.test(shared)],
        ['the title and the button', s.title === 'Share an address with flush.markets' && s.accept === 'Share'],
      ];
    });
    const opened = await waitFor('the callback opened', async () => {
      const list = await host('test.linksOpened');
      return list.length ? list : null;
    }, 30000);
    await check('connect: the browser opens exactly the callback with the address, the nonce and the network', opened[0] === `https://flush.markets/connect#address=${shared}&nonce=${nonce}&network=${NET}`, JSON.stringify(opened));
    await check('connect: the address is the wallet\'s', (await rpc('getaddressinfo', [shared], W)).ismine === true);

    // --- The refusals: nothing asked, the app says why ---
    await refused('network', fromBrowser(link('buy', [['launch', 'TIDE'], ['asset', tide], ['ecx', '1'], ['min_tokens', '1']], { network: 'ecashbeta' })), 'This link is for the network ecashbeta, and this app runs regtest', true);
    await refused('expired', fromBrowser(link('buy', [['launch', 'TIDE'], ['asset', tide], ['ecx', '1'], ['min_tokens', '1']], { exp: now() - 3600 })), 'This link has expired');
    await refused('used twice', fromBrowser(buy), 'This link was used already');
    // The same connect, as another link (a later expiry): its nonce was answered.
    await refused('connect answered', fromBrowser(link('connect', [['origin', 'https://flush.markets'], ['callback', 'https://flush.markets/connect'], ['nonce', nonce]], { exp: now() + 900 })), 'This connect was answered already');
    await refused('ticker of another token', fromBrowser(link('buy', [['launch', 'GRAD'], ['asset', tide], ['ecx', '1'], ['min_tokens', '1']])), 'the ticker and the id must be of one asset', true);
    await refused('floor the curve cannot meet', fromBrowser(link('buy', [['launch', 'TIDE'], ['asset', tide], ['ecx', '1'], ['min_tokens', '999999999']])), 'The curve moved', true);
    await refused('unknown parameter', fromBrowser(link('buy', [['launch', 'TIDE'], ['asset', tide], ['ecx', '1'], ['min_tokens', '1'], ['referrer', 'x']])), 'referrer is not a parameter of buy');
    await refused('callback elsewhere', fromBrowser(link('connect', [['origin', 'https://flush.markets'], ['callback', 'https://evil.example/connect'], ['nonce', 'e2e0123456789abcdef09999']])), 'the app sends it to https://flush.markets/connect only', true);
    await refused('another logo', async () => drop('BAD.flushlaunch.json', JSON.stringify({ ...file, ticker: 'BADLOGO', logo: { ...file.logo, sha256: '0'.repeat(64) } })), 'do not have the SHA-256 the file says');
    await refused('pasted, wrong network', async () => {
      T().go('send', 'uri');
      const field = await waitFor('the Open URI field', () => document.getElementById('uri-input'), 20000);
      field.value = link('swap', [['pool', pool], ['from', 'ECX'], ['to', 'GRAD'], ['amount', '1'], ['min_out', '1']], { network: 'ecash' });
      field.dispatchEvent(new Event('input', { bubbles: true }));
      await sleep(200);
      document.getElementById('uri-form').requestSubmit();
    }, 'This link is for the network ecash');
    await check('nothing was opened but the one callback', (await host('test.linksOpened')).length === 1);
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
