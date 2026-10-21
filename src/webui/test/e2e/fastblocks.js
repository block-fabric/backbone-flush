// FastBlocks against a live mainchain (run_sidechain.py --script fastblocks.js, with this node a validator): the
// node runs with a validator key of the genesis set (-posvalidators) and a second key in its key file, its own BMM
// bids on (-validatoranchorbid), so that blocks of stake come every second and BMM blocks follow the mainchain.
// Driven through the pages as a user would: a wallet that sees the producer's share of fees (the payout key
// imported), a deposit, Staking (this node active, its next slots; a bond to the second key through the form, the
// desktop asked, more added, some unbonded; the key joining the set; staking off and on again), the explorer's
// kinds and finality, the history's Staked income, Finality & BMM, Slashing & jail, the cards of the home.
// Pictures of every page of FastBlocks in Midnight, Dracula and Light, the dashboard and the workspace, at the
// width of the window (run once at 1180 px, once at 700 with --width 700: names end in the width).
(async () => {
  const { host, rpc, sleep, log, T, waitFor, check, snapshot, text, finish } = E2E;
  const W = window.innerWidth >= 1000 ? 1180 : window.innerWidth;
  // The keys of test_framework/fastblocks.py (KEYS[0], KEYS[1]): the genesis validator, and one to bond to.
  const X0 = '100f6d8cbf94afb6fc58e9c384b9b3a6516091373a83c869f4e24a9d2bb4a494';
  const X1 = '45d3b9ce0f54f4d6a17edfe3f9e0993b94d6b299c1a6e5a728ff036ecd9e139f';
  // KEYS[0] as WIF, and its P2WPKH address (-validatorpayout): the wallet imports it to see its income.
  const WIF0 = 'cN9kJa4dPECMjy7UAXBzFp4beQxNj4n4nxkMZiMKbGGDu1Bh3aTu';
  const PAYOUT = 'rsc1qqghd0tuw5wc9k0ec22t8qr7gy3tz5c5sx6gvwx';
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
      const el = button(label, root);
      return el && !el.disabled ? el : null;
    });
    b.click();
    await sleep(150);
  }
  async function clickId(id, ms = 30000) {
    const b = await waitFor(`#${id} enabled`, () => {
      const el = document.getElementById(id);
      return el && !el.disabled ? el : null;
    }, ms);
    b.click();
    await sleep(150);
  }
  async function go(page, tab = '') {
    T().go(page, tab);
    await sleep(400);
  }
  async function shot(name) {
    const overlay = $$('dialog[open]').find((d) => /Synchronizing/.test(d.textContent));
    if (overlay) {
      const hide = button('Hide', overlay);
      if (hide) hide.click();
      await sleep(300);
    }
    await snapshot(`${name}-${W}`);
  }
  /** What the page says in its dialogs (the node's refusals). */
  const said = () => $$('dialog[open]').map((d) => d.textContent.replace(/\s+/g, ' ').trim()).join(' | ');
  /**
   * One change through the bond form: its kind, the key, the amount; the desktop answers yes. Waits for the form to
   * be idle (the previous change done, its amount field new), then for the desktop to be asked.
   */
  async function formStep(kind, key, amount) {
    await waitFor('the form idle', () => $('#fb-bond-amount') && $('#fb-bond-amount').value === '' && !$$('dialog[open]').length, 30000);
    await click(kind);
    set($('#fb-bond-key'), key);
    await sleep(200);
    set(await waitFor('the amount', () => $('#fb-bond-amount')), amount);
    await sleep(300);
    const before = (await host('test.confirmed')).length;
    await host('test.confirmAnswer', { yes: true });
    await clickId('fb-bond-submit');
    await waitFor(`the desktop asked (${kind})`, async () => (await host('test.confirmed')).length > before || (said() ? Promise.reject(new Error(said())) : false), 30000);
    await sleep(1500);
    if (said()) throw new Error(`${kind}: the page says ${said()}`);
  }
  /** Nothing of the page wider than the window (the pages turn tables into cards when narrow). */
  function overflow() {
    const out = [];
    const doc = document.documentElement;
    if (doc.scrollWidth > doc.clientWidth + 1) out.push(`the page scrolls sideways (${doc.scrollWidth} > ${doc.clientWidth})`);
    // A figure of a stat or a card whole in its box (an amount and its unit).
    for (const el of $$('.fb-page .side-stat strong, .fb-cards .value, .fb-cards dd')) {
      if (el.scrollWidth > el.clientWidth + 1) out.push(`"${el.textContent.trim().slice(0, 30)}" is cut (${el.scrollWidth} > ${el.clientWidth})`);
    }
    for (const el of $$('.fb-page .panel, .fb-page button, .fb-page input, .fb-page select, .fb-page h1, .fb-cards .panel')) {
      const r = el.getBoundingClientRect();
      if (r.width && r.right > window.innerWidth + 1 && !el.closest('[role="grid"], .side-table-wrap')) out.push(`${el.tagName.toLowerCase()} "${(el.textContent || '').trim().slice(0, 30)}" ends at ${Math.round(r.right)}`);
      if (out.length > 5) break;
    }
    return out;
  }
  const W_ = 'fb';

  try {
    // What pays or signs is confirmed in the dialog of the host (audit 7 #1): every question accepted here, checked at the end.
    await host('test.confirmAnswer', { always: true });
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
    await waitFor('the node to reach the mainchain', async () => (await rpc('getmainchaininfo')).connected, 60000);
    await check(`Starbase (the pages of FastBlocks), the dashboard and Midnight by default (${W} px)`, T().app.info.name === 'Starbase' && T().prefs.layout === 'dashboard', `${T().app.info.name} ${T().prefs.layout}`);

    // --- A wallet that sees the validator's income, and coins ---
    await rpc('createwallet', [W_]);
    await waitFor('the wallet in the page', () => T().wallets.current === W_);
    const desc = (await rpc('getdescriptorinfo', [`wpkh(${WIF0})`])).checksum;
    const imported = await rpc('importdescriptors', [[{ desc: `wpkh(${WIF0})#${desc}`, timestamp: 0, label: 'validator payout' }]], W_);
    await check('the payout key of the validator imported into the wallet', imported[0].success === true, JSON.stringify(imported));
    const deposit = await rpc('getdepositaddress', [], W_);
    await host('test.confirmAnswer', { yes: true });
    await host('peg.deposit', { destination: deposit.depositaddress, amount: '20' });

    // --- Blocks of stake ---
    const pos = await waitFor('blocks of stake', async () => {
      const p = await rpc('getposinfo');
      return p.active && p.lastposslot >= 0 && p.anchors > 0 ? p : null;
    }, 180000);
    await check('blocks of stake come, with BMM commitments', pos.active && pos.anchors > 0, JSON.stringify({ slot: pos.slot, anchors: pos.anchors }));
    await waitFor('the deposit credited', async () => (await rpc('getbalances', [], W_)).mine.trusted >= 20, 240000);
    await check('the deposit credited', true);
    const staking = await rpc('getstakinginfo');
    await check('this node is a validator with two keys, its payout the imported address', staking.validator && staking.keys.length === 2 && staking.payout === PAYOUT, JSON.stringify({ keys: staking.keys.length, payout: staking.payout }));

    // --- Home: the cards of FastBlocks ---
    await go('home');
    // (A genesis validator that missed its slots while the chain waited for its first BMM commitments is jailed
    // for a few epochs of 20 s: the card says so, then Active again.)
    await waitFor('the cards of FastBlocks on the home', () => text('.fb-cards').includes('Next slots') && text('.fb-cards').includes('Active'), 150000).catch(async (e) => {
      await log(`the home says: ${text('.home').replace(/\s+/g, ' ').slice(0, 1500)}`);
      throw e;
    });
    await check('the Staking card says Active', text('.fb-cards').includes('Active'));

    // --- Staking, Validator (Layers: one Staking section; this node as a validator in its Validator tab) ---
    await go('staking', 'validator');
    await waitFor('the state of this node', () => text('#fb-node-state') === 'Active', 30000);
    await waitFor('the slots it leads next', () => $('ol[aria-label="Slots this node leads next"] li'), 30000);
    await check('Staking: this node active, its payout address shown, its next slots listed', text('.staking').includes(PAYOUT));
    // Fees for the producer: a few payments of the wallet (half of a block's fees is the producer's share, paid
    // to the imported payout address: Staked; the other half the pot, which a BMM block pays to the BMM bidder,
    // this validator's own: BMM reward).
    for (let i = 0; i < 4; i++) await rpc('sendtoaddress', [await rpc('getnewaddress', [], W_), 1], W_);
    // The producer's share of fees, paid to the imported payout address: Staked, in all (the third cell).
    // (The wallet's income is in the Overview tab.)
    await go('staking', 'overview');
    const stakedInAll = () => $('table[aria-label="Staking income"] tbody tr td:nth-child(3)')?.textContent ?? '';
    await waitFor('the staking income of the wallet', () => /[1-9]/.test(stakedInAll()), 120000);
    await check('Overview: the fee split and the leader of this slot', /%/.test(text('#fb-fee-split')) && text('#fb-leader-now') !== '');
    await check('the income of the validator shows as Staked', true);

    // --- Staking, Bonds: a bond to the second key, more, some back ---
    await go('staking', 'bonds');
    const key = await waitFor('the key field', () => $('#fb-bond-key'));
    set(key, X1);
    set(await waitFor('the amount', () => $('#fb-bond-amount')), '1');
    await host('test.confirmAnswer', { yes: false });
    const asked = (await host('test.confirmed')).length;
    await clickId('fb-bond-submit');
    // The desktop asked (and was told no) before anything else.
    await waitFor('the desktop asked about the bond', async () => (await host('test.confirmed')).length > asked, 30000);
    await sleep(500);
    await check('a bond the desktop declined is not made', (await rpc('listbonds', [], W_)).length === 0);
    await host('test.confirmAnswer', { yes: true });
    await waitFor('the form ready again', () => !$('#fb-bond-submit').disabled, 10000);
    set($('#fb-bond-amount'), '1');
    const asked2 = (await host('test.confirmed')).length;
    await clickId('fb-bond-submit');
    await waitFor('the desktop asked about the bond again', async () => (await host('test.confirmed')).length > asked2, 30000);
    await sleep(2000);
    if (said()) await log(`the page says: ${said()}`);
    const bonded = await waitFor('the bond mined', async () => {
      const b = (await rpc('listbonds', [], W_)).find((x) => x.validator === X1);
      return b && b.bonded >= 1 ? b : null;
    }, 120000);
    await check('a bond of 1 to the second key, through the form and the desktop', bonded.bonded === 1, JSON.stringify(bonded));
    await formStep('Add to bond', X1, '0.5');
    await waitFor('more bonded', async () => (await rpc('listbonds', [], W_)).find((x) => x.validator === X1)?.bonded === 1.5, 120000);
    await check('0.5 more added to the bond', true);
    await formStep('Unbond', X1, '0.25');
    const unbonding = await waitFor('the unbond mined', async () => {
      const b = (await rpc('listbonds', [], W_)).find((x) => x.validator === X1);
      return b && b.unbonding > 0 ? b : null;
    }, 120000);
    await check('0.25 unbonding, paid at a later BMM commitment', unbonding.bonded === 1.25 && unbonding.unbonds.length === 1, JSON.stringify(unbonding));
    await waitFor('the bonds table', () => /1\.25/.test(text('table[aria-label="Bonds of this wallet"]')), 30000);
    const confirmations = await host('test.confirmed');
    await check('every bond and unbond asked the desktop, with the key in full', confirmations.filter((c) => /Bond|Unbond|Add to a bond/.test(c.title)).length >= 4 && confirmations.some((c) => c.message.replace(/\s/g, '').includes(X1)), JSON.stringify(confirmations.map((c) => c.title)));

    // --- The second key joins the set (the epoch after next: 20 slots of a second each) ---
    await waitFor('the second key in the set', async () => (await rpc('getvalidators')).validators.some((v) => v.pubkey === X1), 180000);
    await go('staking', 'validators');
    await waitFor('the set of two', () => $$('table[aria-label="Validator set"] tbody tr').length === 2, 30000);
    await check('Validators: the set of this epoch has both keys of this node', $$('table[aria-label="Validator set"] .fb-mine').length === 2);
    await waitFor('every validator on the chain', () => $$('table[aria-label="Every validator on the chain"] tbody tr').length >= 2, 30000);
    await go('staking', 'schedule');
    await waitFor('the leader schedule', () => $$('table[aria-label="Leader schedule"] tbody tr').length > 0, 30000);
    await waitFor('the epochs', () => $$('table[aria-label="Epochs"] tbody tr').length > 3, 60000);
    await check('Schedule: the leaders of the epoch and the epochs', true);

    // --- Staking off and on: the desktop asked first ---
    await go('staking', 'validator');
    await host('test.confirmAnswer', { yes: true });
    await clickId('fb-staking-stop');
    await waitFor('staking off', async () => !(await rpc('getstakinginfo')).enabled, 20000);
    await waitFor('the page says it', () => text('#fb-node-state') === 'Staking off', 20000);
    await check('Stop staking turns it off, after the desktop', true);
    await shot('fb-staking-off');
    await clickId('fb-staking-start');
    await waitFor('staking on again', async () => (await rpc('getstakinginfo')).enabled, 20000);
    await waitFor('the page says it', () => text('#fb-node-state') === 'Active', 20000);

    // --- The explorer: the kind and the finality of each block ---
    await go('explorer');
    await waitFor('the kinds of the blocks', () => $$('.badge.fb-kind-pos').length > 0, 30000);
    await waitFor('a BMM block or finality', () => $$('.badge.fb-kind-bmm, .badge.fb-anchored, .badge.fb-final').length > 0, 60000);
    const heads = $$('[role="columnheader"]').map((h) => h.textContent.replace(/[▲▼]/g, '').trim());
    await check('the explorer: Kind, Slot, Producer, Mainchain, Finality after the height, no difficulty', heads.slice(0, 6).join(',') === 'Height,Kind,Slot,Producer,Mainchain,Finality' && !heads.includes('Difficulty'), heads.join(','));

    // --- The history: the producer's share is Staked ---
    await go('transactions');
    await waitFor('Staked in the history', () => /Staked/.test(text('.page')), 60000);
    await check('the history labels the validator income Staked', true);

    // --- Finality & BMM, Slashing & jail ---
    // Finality & BMM and Slashing & jail: views of Staking's Network tab (the pages FastBlocks had).
    await go('staking', 'finality');
    await waitFor('the BMM-confirmed height', () => /block \d/.test(text('#fb-anchored-height')), 30000);
    await waitFor('the newest BMM blocks', () => $$('table[aria-label="Newest BMM blocks"] tbody tr').length > 0, 30000);
    await check('Finality: the BMM-confirmed height and the newest BMM blocks; the validator bids', /a mainchain block/.test(text('.finality')));
    await go('staking', 'slashing');
    await waitFor('the slashing page', () => /Proofs of double signing/.test(text('.slashing')), 20000);
    await check('Slashing: no proof, no validator jailed', /knows no proof/.test(text('.slashing')));

    // --- Every page, in the layouts and looks, at this width ---
    const PAGES = [
      ['home', ''],
      ['staking', 'overview'],
      ['staking', 'validator'],
      ['staking', 'bonds'],
      ['staking', 'validators'],
      ['staking', 'schedule'],
      ['staking', 'finality'],
      ['staking', 'slashing'],
      ['explorer', ''],
      ['transactions', ''],
    ];
    for (const layout of ['dashboard', 'workspace']) {
      for (const theme of ['midnight', 'dracula', 'light']) {
        T().savePrefs({ theme, layout });
        await sleep(600);
        for (const [page, tab] of PAGES) {
          await go(page, tab);
          await sleep(page === 'explorer' ? 1500 : 900);
          if (page !== 'explorer' && page !== 'transactions') {
            const problems = overflow();
            await check(`${W} px ${layout} ${theme}: ${page}${tab ? ` ${tab}` : ''} fits`, problems.length === 0, problems.join('; '));
          }
          await shot(`fb-${layout}-${theme}-${page}${tab ? `-${tab}` : ''}`);
        }
      }
    }
    T().savePrefs({ theme: 'midnight', layout: 'dashboard' });
    {
      // The confirmations of the desktop: what the chain's pages paid or signed, each with its facts.
      const titles = ["Add to a bond", "Bond coins", "Unbond"];
      const asked = (await host('test.confirmed', {})).filter((q) => titles.includes(q.title));
      await check(`the desktop confirmed what the pages paid or signed (${asked.length})`, asked.length > 0 && asked.every((q) => q.rows.length > 1), JSON.stringify(asked.map((q) => q.title)));
    }
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
