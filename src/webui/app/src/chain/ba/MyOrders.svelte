<script lang="ts">
  // The limit orders this wallet holds (listorders, each read whole with getorder: its assets, its limit, what it sold and
  // got): what it sells and buys, its limit, what is left and what waits to be collected, its last block, its status (Open,
  // Filled, Expired, Evicted: a better and larger order took its place in a full book); and what can be done with it
  // (settleorder): collect what it got (it stays open), cancel it (what is left comes back too), or, once it trades no
  // more, collect everything (the order is gone).
  import SafeText from '../../components/SafeText.svelte';
  import { stacked } from '../../lib/actions';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { formatCount } from '../../lib/format';
  import { rpc } from '../../lib/rpc';
  import { toast, wallets } from '../../lib/state.svelte';
  import { orderActions, orderStatus, ORDER_STATUS, ORDER_STATUS_TAG, type BookOrder, type WalletOrder } from './book';
  import { formatAmount, formatPrice, type Amount } from './model';
  import { ba, decimalsOf, refreshBitAssets } from './store.svelte';
  import { TICKER } from '../config';

  let list = $state<WalletOrder[] | null>(null);
  let error = $state('');
  /** Each order read whole (getorder), by id. */
  let details = $state<Record<string, BookOrder>>({});
  let busy = $state(false);

  $effect(() => {
    const wallet = wallets.current;
    void ba.version;
    if (wallet === null) {
      list = null;
      return;
    }
    let live = true;
    rpc
      .raw<WalletOrder[]>('listorders', [], { wallet })
      .then((r) => {
        if (!live) return;
        list = Array.isArray(r) ? r : [];
        error = '';
        for (const o of list) {
          rpc
            .raw<BookOrder>('getorder', [o.order])
            .then((d) => {
              if (live && d && typeof d === 'object') details = { ...details, [o.order]: d };
            })
            .catch(() => undefined);
        }
      })
      .catch((e) => {
        if (live) error = errorText(e);
      });
    return () => {
      live = false;
    };
  });

  /** An amount of an asset by id when the order was read whole (else as the node wrote it). */
  const amountOf = (value: Amount | undefined, id: string | undefined) => (value === undefined ? '–' : id ? formatAmount(value, decimalsOf(id), { max: 8 }) : formatAmount(value, 8));
  const anyEvicted = $derived((list ?? []).some((o) => o.evicted));

  async function settle(o: WalletOrder, cancel: boolean) {
    const wallet = wallets.current;
    if (wallet === null || busy) return;
    const status = orderStatus(o);
    const closes = cancel || status !== 'open';
    const d = details[o.order];
    const got = amountOf(o.to_collect, d?.buys_id);
    const left = amountOf(o.remaining, d?.sells_id);
    const r = await ask({
      title: cancel ? t('Cancel an order') : t('Collect an order'),
      message: closes ? t('Take back %1 %2 and collect %3 %4? The order is gone.', left, o.sells, got, o.buys) : t('Collect %1 %2? The order stays in the book.', got, o.buys),
      informative: t('%1 comes by the coinbase of the next block; assets as coins of the transaction.', TICKER),
      level: cancel ? 'warning' : 'question',
      choices: [
        { id: 'no', label: 'Back' },
        { id: 'yes', label: cancel ? 'Cancel it' : 'Collect', kind: cancel ? 'danger' : 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      const done = await walletRpc<{ txid: string }>(wallet, 'settleorder', [o.order, cancel]);
      if (!done) return;
      toast('success', cancel ? t('Cancelling an order') : t('Collecting an order'), t('With the next block.'));
      void refreshBitAssets({ node: false });
    } catch (e) {
      await inform(cancel ? t('Cancel an order') : t('Collect an order'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }
</script>

<section class="panel" aria-label="My orders">
  <div class="panel-head"><h3>My orders</h3><span class="muted small">limit orders of this wallet, in every book</span></div>
  {#if wallets.current === null}
    <p class="ba-empty">No wallet loaded.</p>
  {:else if error}
    <p class="ba-problems bad" role="alert"><SafeText text={error} /></p>
  {:else if list === null}
    <p class="ba-empty">Reading the orders…</p>
  {:else if list.length === 0}
    <p class="ba-empty">No order: place one above.</p>
  {:else}
    <div class="ba-wrap">
      <table class="ba-table" use:stacked aria-label="My orders">
        <thead><tr><th>Sells</th><th>For</th><th class="end">Limit</th><th class="end">Left</th><th class="end">To collect</th><th class="end">Until block</th><th>Status</th><th></th></tr></thead>
        <tbody>
          {#each list as o (o.order)}
            {@const d = details[o.order]}
            {@const status = orderStatus(o)}
            {@const can = orderActions(o)}
            <tr data-testid="my-order">
              <td><SafeText text={o.sells} /></td>
              <td><SafeText text={o.buys} /></td>
              <td class="end" translate="no">{d?.price !== undefined ? formatPrice(d.price, 8) : '–'}</td>
              <td class="end" translate="no">{amountOf(o.remaining, d?.sells_id)}</td>
              <td class="end" translate="no">{amountOf(o.to_collect, d?.buys_id)}</td>
              <td class="end" translate="no">{formatCount(o.expiry)}</td>
              <td>
                <span class="ba-tag {ORDER_STATUS_TAG[status]}" data-testid="order-status" title={status === 'evicted' ? t('A better and larger order took its place in a full book: it trades no more. Settle it to take everything back.') : undefined}>{t(ORDER_STATUS[status])}</span>
              </td>
              <td class="actions">
                {#if can.settle}
                  <button type="button" class="btn small primary" disabled={busy} title={t('Collect what it got and take back what is left: the order is gone')} onclick={() => settle(o, false)}>Collect</button>
                {:else}
                  <button type="button" class="btn small primary" disabled={busy || !can.collect} onclick={() => settle(o, false)}>Collect</button>
                  <button type="button" class="btn small danger" disabled={busy} onclick={() => settle(o, true)}>Cancel</button>
                {/if}
              </td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
    {#if anyEvicted}<p class="ba-help note">Evicted: a better and larger order took its place in a full book, and it trades no more. Collect it to take everything back: what it got and what is left.</p>{/if}
  {/if}
</section>

<style>
  .actions {
    display: flex;
    flex-wrap: wrap;
    gap: 0.35rem;
    justify-content: flex-end;
  }
  .note {
    padding: 0.6rem 1rem;
  }
  .small {
    font-size: 0.85rem;
  }
</style>
