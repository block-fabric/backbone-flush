// Pictures of Flush's own looks, Launchpad (the default of a first start) and Launchpad Day, on the pages they were
// drawn for: the splash, Home, Trade, Assets, Staking, Settings (Appearance and Advanced theme), in Advanced and Simple
// mode, at the width of the window. Checks on the way: Launchpad is the look of a first start, both looks are offered
// (chain.json: exclusiveThemes), the root carries data-look="launchpad", the font of words is Barlow, nothing animates
// for ever. Coins, the demo token and its pool come from the chain this app follows (contrib/playground, or the runner).
(async () => {
  const { sleep, T, waitFor, check, snapshot, finish, rpc } = E2E;
  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
  const W = window.__E2E_WIDTH || 1180;
  const settle = async (ms = 900) => {
    // The synchronizing overlay of a node catching up, out of the way of the pictures.
    const overlay = $$('dialog[open]').find((d) => /Synchroniz/.test(d.textContent));
    if (overlay) {
      const hide = $$('button', overlay).find((b) => /Hide/.test(b.textContent));
      if (hide) hide.click();
    }
    await sleep(ms);
    // The notices of the coins received, out of the way too.
    T().toasts.list.splice(0);
    await sleep(150);
  };
  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
    await check('Launchpad is the look of a first start', T().prefs.theme === 'launchpad', String(T().prefs.theme));
    await check('the root draws the Launchpad family', document.documentElement.dataset.look === 'launchpad', String(document.documentElement.dataset.look));
    await check('words in Barlow', getComputedStyle(document.body).fontFamily.includes('Barlow'), getComputedStyle(document.body).fontFamily);

    // A wallet with coins, sent by the chain's validator (the runner waits for its address in the log).
    const wallets = await rpc('listwallets');
    if (!wallets.includes('demo')) await rpc('createwallet', ['demo']);
    await waitFor('the wallet in the page', () => T().wallets.current === 'demo');
    const address = await rpc('getnewaddress', ['Launchpad'], 'demo');
    await E2E.log(`ADDRESS ${address}`);
    await waitFor('the coins of the wallet', async () => (await rpc('getbalances', [], 'demo')).mine.trusted > 0, 240000);

    // The splash, as the app shows it while the node starts: drawn by the page from the phase.
    for (const theme of ['launchpad', 'launchpadday']) {
      T().savePrefs({ theme });
      const phase = T().app.phase;
      T().app.initMessage = 'Loading block index…';
      T().app.phase = 'init';
      await sleep(900);
      await check(`${theme}: the splash, still`, !!$('.splash') && $$('.splash *').every((el) => getComputedStyle(el).animationName === 'none'), 'an animation runs on the splash');
      await snapshot(`splash-${theme}-${W}`);
      T().app.phase = phase;
      await sleep(600);
    }

    const pages = [
      ['home', ''],
      ['trade', ''],
      ['assets', ''],
      ['staking', ''],
      ['settings', 'display'],
      ['settings', 'theme'],
    ];
    for (const mode of ['advanced', 'simple']) {
      for (const theme of ['launchpad', 'launchpadday']) {
        T().savePrefs({ theme, uiMode: mode });
        for (const [page, tab] of pages) {
          T().go(page, tab);
          await settle(page === 'home' ? 1600 : 1000);
          await snapshot(`${mode}-${theme}-${page}${tab && tab !== 'display' ? '-' + tab : ''}-${W}`);
        }
      }
    }
    // Nothing moves for ever on these pages (CPU: software GL under WSLg).
    T().savePrefs({ theme: 'launchpad', uiMode: 'advanced' });
    T().go('home');
    await settle(1200);
    const running = document.getAnimations ? document.getAnimations().filter((a) => a.effect && a.effect.getTiming().iterations === Infinity && a.playState === 'running') : [];
    await check('no animation runs for ever on the home', running.length === 0, running.map((a) => a.animationName || a.id).join(', '));
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
