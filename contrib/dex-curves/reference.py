#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A reference of the DEX's pool kinds (doc/layers.md, "Pool kinds"), written from the definitions, in
Python's integers and fractions: no code of the node's. It checks the vectors the node's unit tests commit
(src/test/data/dex_curves.json):

    contrib/dex-curves/reference.py src/test/data/dex_curves.json
    contrib/dex-curves/reference.py src/test/data/dex_launch.json   (launch curves)

Every quantity is the floor or ceiling of an exact one; how it is found (here a bisection) does not matter.
"""
import json
import sys
from fractions import Fraction
from math import gcd

F = 1_000_000          # fees in millionths
MAX_AMOUNT = 2**63 - 1
CP, STABLE, WEIGHTED = 0, 1, 2
LEGACY_FEE = 3000


def search_max(lo, hi, ok):
    """The largest v in [lo, hi] with ok(v), ok true then false."""
    while lo < hi:
        mid = lo + (hi - lo) // 2 + 1
        if ok(mid):
            lo = mid
        else:
            hi = mid - 1
    return lo


def search_min(lo, hi, ok):
    while lo < hi:
        mid = lo + (hi - lo) // 2
        if ok(mid):
            hi = mid
        else:
            lo = mid + 1
    return lo


def phi(amp, x, y, d):
    """StableSwap: 16A·XY(X+Y) + 4XY·D − 16A·D·XY − D³ (x, y, d may be fractions)."""
    return 16 * amp * x * y * (x + y) + 4 * x * y * d - 16 * amp * d * x * y - d ** 3


def stable_d(amp, x, y):
    """(floor, ceil) of the root D of Φ(x, y, ·)."""
    if x == 0 or y == 0:
        return 0, 0
    lo = search_max(0, x + y, lambda d: phi(amp, x, y, d) >= 0)
    return lo, (lo if phi(amp, x, y, lo) == 0 else lo + 1)


def weights(k):
    if k['curve'] != WEIGHTED:
        return 1, 1
    w0, w1 = k['weight0'], 100 - k['weight0']
    g = gcd(w0, w1)
    return w0 // g, w1 // g


class Level:
    def __init__(self, k, x0, y0):
        self.k = k
        self.s0, self.s1 = 10 ** k['scale0'], 10 ** k['scale1']
        if k['curve'] == STABLE:
            self.d = stable_d(k['amp'], x0 * self.s0, y0 * self.s1)[1]
        else:
            self.wa, self.wb = weights(k)
            self.inv = x0 ** self.wa * y0 ** self.wb

    def holds(self, x, y, ratio=Fraction(1)):
        x, y = Fraction(x), Fraction(y)
        if self.k['curve'] == STABLE:
            return phi(self.k['amp'], x * self.s0, y * self.s1, self.d * ratio) >= 0
        return x ** self.wa * y ** self.wb >= self.inv * ratio ** (self.wa + self.wb)


def clear(k, fee, r0, r1, orders):
    if r0 == 0 or r1 == 0 or r0 > MAX_AMOUNT or r1 > MAX_AMOUNT:
        return None
    in0 = sum(a for s, a in orders if s == 0)
    in1 = sum(a for s, a in orders if s == 1)
    if in0 > MAX_AMOUNT - r0 or in1 > MAX_AMOUNT - r1:
        return None
    keep = F - fee
    ah, bh = in0 * keep, in1 * keep
    if k['curve'] == CP:
        num, den = r1 * F + bh, r0 * F + ah
        outs = [(a * keep * num) // (den * F) if s == 0 else (a * keep * den) // (num * F) for s, a in orders]
    else:
        level = Level(k, r0, r1)
        priced0 = True
        dy = 0
        if ah > 0:
            dy = search_max(0, r1 - 1, lambda d: level.holds(r0 + Fraction(ah, F) if bh == 0 else Fraction(r0 * (F * d + bh) + ah * d, F * d + bh), r1 - d))
        if ah > 0 and (dy > 0 or bh == 0):
            num, den = F * dy + bh, ah
        else:
            dx = 0
            if bh > 0:
                dx = search_max(0, r0 - 1, lambda d: level.holds(r0 - d, r1 + Fraction(bh, F) if ah == 0 else Fraction(r1 * (F * d + ah) + bh * d, F * d + ah)))
            if bh > 0 and (dx > 0 or ah == 0):
                priced0 = False
                num, den = F * dx + ah, bh
            else:
                num, den = bh, ah
        outs = []
        for s, a in orders:
            paid = a * keep
            if (s == 0) == priced0:
                outs.append(0 if den == 0 else paid * num // (den * F))
            else:
                outs.append(0 if num == 0 else paid * den // (num * F))
    out0 = sum(o for (s, a), o in zip(orders, outs) if s == 1)
    out1 = sum(o for (s, a), o in zip(orders, outs) if s == 0)
    if out0 >= r0 + in0 or out1 >= r1 + in1:
        return None
    return outs, r0 + in0 - out0, r1 + in1 - out1


def opening_shares(k, r0, r1):
    if k['curve'] == STABLE:
        d = stable_d(k['amp'], r0 * 10 ** k['scale0'], r1 * 10 ** k['scale1'])[0]
        return d if d <= MAX_AMOUNT else None
    wa, wb = weights(k)
    inv = r0 ** wa * r1 ** wb
    return search_max(0, max(r0, r1), lambda v: v ** (wa + wb) <= inv)


def deposit_shares(k, fee, r0, r1, total, a0, a1):
    wa, wb = weights(k)
    lhs, rhs = a0 * r1, a1 * r0
    zero_excess = lhs >= rhs
    excess = lhs - rhs if zero_excess else rhs - lhs
    phi_n, phi_d = (wb if zero_excess else wa), wa + wb
    owed_num, owed_den = excess * phi_n * fee, (r1 if zero_excess else r0) * phi_d * F
    owed = -(-owed_num // owed_den)
    e0, e1 = a0, a1
    if zero_excess:
        if owed >= e0:
            return None
        e0 -= owed
    else:
        if owed >= e1:
            return None
        e1 -= owed
    level = Level(k, r0, r1)
    s = search_max(0, MAX_AMOUNT - total, lambda s: level.holds(r0 + e0, r1 + e1, Fraction(total + s, total)))
    return s or None


def withdraw_only1(k, fee, r0, r1, total, shares):
    out0, out1 = r0 * shares // total, r1 * shares // total
    left0, left1 = r0 - out0, r1 - out1
    bought = 0
    if out0 > 0:
        c = clear(k, fee, left0, left1, [(0, out0)])
        bought = c[0][0] if c else 0
    return 0, out1 + bought


def spot(k, r0, r1, inverse):
    if k['curve'] == CP:
        num, den = r1, r0
    elif k['curve'] == WEIGHTED:
        wa, wb = weights(k)
        num, den = r1 * wa, r0 * wb
    else:
        s0, s1 = 10 ** k['scale0'], 10 ** k['scale1']
        x, y = r0 * s0, r1 * s1
        d = stable_d(k['amp'], x, y)[1]
        a4 = 4 * k['amp']
        t0, t1 = a4 * (2 * x + y) + d - a4 * d, a4 * (x + 2 * y) + d - a4 * d
        if t0 <= 0 or t1 <= 0:
            num, den = r1, r0
        else:
            num, den = s0 * y * t0, s1 * x * t1
    if inverse:
        num, den = den, num
    return min((num << 64) // den, 2**256 - 1)


def clear_cp0(x, y, orders):
    """The constant product of fee-free amounts (ClearPool's formula, fee 0)."""
    in0 = sum(a for s, a in orders if s == 0)
    in1 = sum(a for s, a in orders if s == 1)
    if in0 > MAX_AMOUNT - x or in1 > MAX_AMOUNT - y:
        return None
    num, den = y * F + in1 * F, x * F + in0 * F
    outs = [(a * F * num) // (den * F) if s == 0 else (a * F * den) // (num * F) for s, a in orders]
    out0 = sum(o for (s, a), o in zip(orders, outs) if s == 1)
    out1 = sum(o for (s, a), o in zip(orders, outs) if s == 0)
    if out0 >= x + in0 or out1 >= y + in1:
        return None
    return outs, x + in0 - out0, y + in1 - out1


def clear_launch(c, x, y, orders):
    """A launch curve's round (curves::ClearLaunch), from its definition."""
    fee, end, max_buy, min_buy = c['fee'], int(c['end']), int(c['max_buy']), int(c['min_buy'])
    if fee >= F or y <= end or x == 0 or x > MAX_AMOUNT or y > MAX_AMOUNT:
        return None
    fee_of = lambda a: -(-a * fee // F)
    play, net = [], []
    for s, a, d in orders:
        over = s == 0 and ((max_buy and a > max_buy) or a < min_buy)
        paid = a - fee_of(a) if s == 0 else a
        ok = not over and paid > 0
        play.append(ok)
        if ok:
            net.append((s, paid))
    n = len(orders)
    outs, refunds = [0] * n, [0] * n
    if not net:
        return outs, refunds, x, y, 0, False
    cp = clear_cp0(x, y, net)
    if cp is None:
        return None
    fees = 0
    if cp[2] >= end:
        k = 0
        for i, (s, a, d) in enumerate(orders):
            if not play[i]:
                continue
            o = cp[0][k]
            k += 1
            if s == 0:
                outs[i] = o
                fees += fee_of(a)
            else:
                f = min(o, fee_of(o))
                outs[i] = o - f
                fees += f
        return outs, refunds, cp[1], cp[2], fees, cp[2] == end
    x_end = -(-(x * y) // end)
    if x_end > MAX_AMOUNT or x_end <= x:
        return None
    dx, dy = x_end - x, y - end
    sold_in = coin_out = 0
    for i, (s, a, d) in enumerate(orders):
        if play[i] and s == 1:
            gross = a * dx // dy
            f = min(gross, fee_of(gross))
            outs[i] = gross - f
            fees += f
            coin_out += gross
            sold_in += a
    buyers = [i for i, (s, a, d) in enumerate(orders) if play[i] and s == 0 and d]
    offered = sum(orders[i][1] - fee_of(orders[i][1]) for i in buyers)
    tokens = dy + sold_in
    got = [0] * n
    cost = lambda t: -(-(t * dx) // dy)
    given = 0
    for i in buyers:
        if offered == 0:
            break
        budget = orders[i][1] - fee_of(orders[i][1])
        got[i] = min(budget * dy // dx, tokens * budget // offered)
        given += got[i]
    for i in buyers:
        if given >= tokens:
            break
        budget = orders[i][1] - fee_of(orders[i][1])
        if cost(got[i] + 1) <= budget:
            got[i] += 1
            given += 1
    coin_in = 0
    for i in buyers:
        if got[i] == 0:
            continue
        a = orders[i][1]
        pays = cost(got[i])
        f = min(a - pays, -(-(pays * fee) // (F - fee)))
        outs[i] = got[i]
        refunds[i] = a - pays - f
        fees += f
        coin_in += pays
    if coin_in < coin_out or given > tokens:
        return None
    return outs, refunds, x + coin_in - coin_out, y + sold_in - given, fees, True


def check_launch(case):
    orders = [(s, int(a), d) for s, a, d in case['orders']]
    c = clear_launch(case, int(case['x']), int(case['y']), orders)
    got = {} if c is None else {'outs': [str(o) for o in c[0]], 'refunds': [str(r) for r in c[1]], 'x': str(c[2]), 'y': str(c[3]), 'fees': str(c[4]), 'reached_end': c[5]}
    return [] if got == case['clearing'] else [f"launch clearing {got} != {case['clearing']}"]


def check(case):
    if case.get('launch'):
        return check_launch(case)
    k = case
    r0, r1 = int(case['reserve0']), int(case['reserve1'])
    orders = [(s, int(a)) for s, a in case['orders']]
    errors = []
    c = clear(k, k['fee'], r0, r1, orders)
    want = case['clearing']
    got = {} if c is None else {'outs': [str(o) for o in c[0]], 'reserve0': str(c[1]), 'reserve1': str(c[2])}
    if got != want:
        errors.append(f"clearing {got} != {want}")
    o = opening_shares(k, r0, r1)
    if (str(o) if o is not None else "") != case['opening_shares']:
        errors.append(f"opening shares {o} != {case['opening_shares']}")
    for inverse, key in ((False, 'spot_price_q64'), (True, 'spot_price_inverse_q64')):
        if spot(k, r0, r1, inverse) != int(case[key], 16):
            errors.append(f"{key} {spot(k, r0, r1, inverse):x} != {case[key]}")
    if o is not None:
        d0, d1 = (int(v) for v in case['deposit'].split())
        s = deposit_shares(k, k['fee'], r0, r1, o, d0, d1)
        if (str(s) if s else "") != case['deposit_shares']:
            errors.append(f"deposit shares {s} != {case['deposit_shares']}")
        w = withdraw_only1(k, k['fee'], r0, r1, o, o // 5)
        if f"{w[0]} {w[1]}" != case['withdraw_only1']:
            errors.append(f"withdraw {w} != {case['withdraw_only1']}")
    return errors


def main():
    cases = json.load(open(sys.argv[1]))
    bad = 0
    for i, case in enumerate(cases):
        for e in check(case):
            bad += 1
            print(f"vector {i}: {e}")
    print(f"{len(cases)} vectors, {bad} differences")
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
