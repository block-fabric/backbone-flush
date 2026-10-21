#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A local Starbase playground on regtest with eCash's rules, in one command.

    starbase-playground.sh start [--block-seconds 5] [--no-app] [--no-demo]
    starbase-playground.sh status | stop | reset
    starbase-playground.sh mine N          N mainchain blocks now
    starbase-playground.sh withdraw-demo   a withdrawal from request to payout
    starbase-playground.sh cli <node> <method> [args...]   node: main, 0, 1, 2

`start` runs, under the playground folder (~/.starbase-playground, or --dir):

- the mainchain: backboned -regtest -testecash -grpc (eCash's drivechain rules with the enforcer's regtest
  thresholds: a slot activates with more than 5 acks in 10 blocks, a bundle is paid with more than 5 votes and
  fails older than 10 blocks), its wallet "miner" behind the enforcer's interface;
- three Starbase validators on regtest as a sidechain in slot 46 (-sidechainslot=46 -testmainenforcer), each
  with its key in the genesis validator set (-posvalidators), following the mainchain through the enforcer's
  interface alone (-mainchaingrpc), as on eCash with a plain eCash node and bip300301_enforcer. Validator 0 is
  starbase-app (unless --no-app), the others starbased;
- a miner that makes a mainchain block every --block-seconds (default 5); validator 0 asks for anchors (BMM).

Then (unless --no-demo) it activates slot 46 (M1 and acks), deposits 100 ECX to validator 0 and 20 ECX to
validator 1, registers the demo token STAR and opens a STAR/ECX pool, and has validator 1 swap ECX for STAR.

Every process it starts has its pid in <dir>/pids; `stop` stops exactly those and waits for them. Ports:
--base-port (default 49500) and the 22 after it, which must be free (not 49x54 nor 49660-49669).
The programs: --starbase-bin (default: build/bin of this tree), --backbone-bin (default: $BACKBONE_BIN_DIR,
else ../chains-ecash/build_node/bin next to this tree).
"""

import argparse
import base64
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from decimal import Decimal
from pathlib import Path

SLOT = 46
SRCDIR = Path(__file__).resolve().parents[2]
# The genesis validators: three fixed regtest keys (never use them anywhere else) and their x-only public keys.
KEYS = ["11" + "00" * 30 + f"{i:02x}" for i in (1, 2, 3)]
RPC_USER, RPC_PASSWORD, GRPC_TOKEN = "playground", "playground", "playground"
STATE = "state.json"


# --- ports and processes ---

def port_free(port):
    with socket.socket() as s:
        # As the nodes bind (a port in TIME_WAIT after a stop is free for them).
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind(("127.0.0.1", port))
            return True
        except OSError:
            return False


def ports(base):
    """main RPC, main P2P, gRPC, a port nothing listens on (no JSON-RPC to the mainchain); per validator P2P and RPC."""
    # (A node also binds its P2P port + 1 for Tor: those are left free too.)
    p = {"main_rpc": base, "main_p2p": base + 1, "grpc": base + 3, "no_rpc": base + 5}
    for i in range(3):
        p[f"p2p{i}"] = base + 10 + 3 * i
        p[f"onion{i}"] = base + 11 + 3 * i
        p[f"rpc{i}"] = base + 20 + i
    return p


def check_ports(p):
    bad = [v for v in p.values() if v % 100 == 54 or 49660 <= v <= 49669 or not 49000 <= v <= 49999]
    if bad:
        sys.exit(f"ports {bad} are not allowed here (49000-49999, not 49x54 nor 4966x): choose another --base-port")
    busy = [v for k, v in p.items() if k != "no_rpc" and not port_free(v)] + ([p["no_rpc"]] if not port_free(p["no_rpc"]) else [])
    if busy:
        sys.exit(f"ports {busy} are in use: choose another --base-port")


def pid_alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    # A zombie child of nobody's: gone for our purposes.
    try:
        with open(f"/proc/{pid}/stat") as f:
            return f.read().split(") ")[-1].split()[0] != "Z"
    except OSError:
        return False


def read_pids(d):
    path = d / "pids"
    if not path.exists():
        return {}
    return {name: int(pid) for name, pid in (line.split() for line in path.read_text().splitlines() if line.strip())}


def add_pid(d, name, pid):
    with open(d / "pids", "a") as f:
        f.write(f"{name} {pid}\n")


def spawn(d, name, argv, env=None, log=None):
    out = open(log or (d / f"{name}.out"), "ab")
    proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT, env=env, start_new_session=True)
    add_pid(d, name, proc.pid)
    return proc.pid


# --- JSON-RPC ---

class Rpc:
    def __init__(self, port, wallet=None):
        self.url = f"http://127.0.0.1:{port}" + (f"/wallet/{wallet}" if wallet else "")
        self.auth = "Basic " + base64.b64encode(f"{RPC_USER}:{RPC_PASSWORD}".encode()).decode()

    def call(self, method, *params):
        body = json.dumps({"jsonrpc": "1.0", "id": 1, "method": method, "params": list(params)}, default=str).encode()
        req = urllib.request.Request(self.url, body, {"Authorization": self.auth, "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=300) as r:
                reply = json.loads(r.read(), parse_float=Decimal)
        except urllib.error.HTTPError as e:
            reply = json.loads(e.read() or b"{}", parse_float=Decimal)
            if not reply.get("error"):
                raise
        if reply.get("error"):
            raise RuntimeError(f"{method}: {reply['error'].get('message')}")
        return reply["result"]

    def __getattr__(self, method):
        return lambda *params: self.call(method, *params)


def wait_rpc(rpc, what, timeout=180):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return rpc.getblockcount()
        except (OSError, urllib.error.URLError, RuntimeError, ConnectionError):
            time.sleep(0.5)
    sys.exit(f"{what} did not answer in {timeout} s (see its log)")


def wait_for(cond, what, timeout=120, step=0.5):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if cond():
                return
        except (RuntimeError, OSError):
            pass
        time.sleep(step)
    sys.exit(f"timed out waiting for {what}")


# --- the playground ---

def load_state(d):
    path = d / STATE
    if not path.exists():
        sys.exit(f"no playground in {d}: start one first")
    return json.loads(path.read_text())


def nodes(state):
    main = Rpc(state["ports"]["main_rpc"], "miner")
    sides = [Rpc(state["ports"][f"rpc{i}"], "w") for i in range(3)]
    return main, sides


def find_bin(path, name):
    binary = Path(path) / name
    if not binary.is_file():
        sys.exit(f"no {name} in {path}: build it, or give the folder of the programs")
    return binary


def pubkeys():
    """The x-only public keys of KEYS (starbase-util has no such command: computed here, BIP340)."""
    p = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F
    n = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
    g = (0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798, 0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8)

    def add(a, b):
        if a is None:
            return b
        if b is None:
            return a
        if a[0] == b[0] and (a[1] + b[1]) % p == 0:
            return None
        lam = (3 * a[0] * a[0] * pow(2 * a[1], p - 2, p)) % p if a == b else ((b[1] - a[1]) * pow(b[0] - a[0], p - 2, p)) % p
        x = (lam * lam - a[0] - b[0]) % p
        return (x, (lam * (a[0] - x) - a[1]) % p)

    def mul(k):
        r, q = None, g
        while k:
            if k & 1:
                r = add(r, q)
            q = add(q, q)
            k >>= 1
        return r

    return [f"{mul(int(k, 16) % n)[0]:064x}" for k in KEYS]


def start(args):
    d = Path(args.dir).expanduser()
    if d.exists() and read_pids(d) and any(pid_alive(pid) for pid in read_pids(d).values()):
        sys.exit(f"a playground runs already in {d}: stop it first (starbase-playground.sh stop)")
    if d.exists():
        shutil.rmtree(d)
    d.mkdir(parents=True)
    p = ports(args.base_port)
    check_ports(p)
    sb = Path(args.starbase_bin)
    bb = Path(args.backbone_bin)
    starbased, starbase_app = find_bin(sb, "starbased"), sb / "starbase-app"
    backboned = find_bin(bb, "backboned")
    if "-testecash" not in subprocess.run([backboned, "-help-debug"], capture_output=True, text=True).stdout:
        sys.exit(f"{backboned} has no eCash regtest mode (-testecash)")
    use_app = not args.no_app and starbase_app.is_file() and (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"))
    t0 = time.time()
    state = {"ports": p, "block_seconds": args.block_seconds, "app": bool(use_app), "starbase_bin": str(sb), "started": t0}
    (d / STATE).write_text(json.dumps(state, indent=1))

    print(f"Playground in {d}, ports {args.base_port}-{args.base_port + 22}")
    main_dir = d / "backbone"
    main_dir.mkdir()
    spawn(d, "backboned", [str(backboned), "-regtest", "-testecash", f"-datadir={main_dir}", f"-rpcport={p['main_rpc']}",
                           f"-port={p['main_p2p']}", f"-rpcuser={RPC_USER}", f"-rpcpassword={RPC_PASSWORD}", "-fallbackfee=0.0002",
                           "-listen=0", "-server", "-printtoconsole=0", "-grpc", f"-grpcport={p['grpc']}", "-grpcwallet=miner",
                           f"-grpcauth={GRPC_TOKEN}", "-txindex"])
    main_node = Rpc(p["main_rpc"])
    wait_rpc(main_node, "backboned")
    main_node.createwallet("miner")
    main = Rpc(p["main_rpc"], "miner")
    state["main_address"] = main.getnewaddress()
    (d / STATE).write_text(json.dumps(state, indent=1))
    main.generatetoaddress(110, state["main_address"])
    print(f"  backboned (eCash regtest, gRPC on {p['grpc']}): 110 blocks")

    keys = pubkeys()
    no_cookie = d / "no-mainchain-datadir"
    no_cookie.mkdir()
    common = [
        "-regtest", f"-sidechainslot={SLOT}", "-testmainenforcer", f"-posvalidators={','.join(keys)}",
        f"-posslotseconds={args.slot_seconds}", f"-mainchaingrpc=127.0.0.1:{p['grpc']}", f"-mainchaingrpcauth={GRPC_TOKEN}",
        f"-mainchainrpcport={p['no_rpc']}", f"-mainchaindatadir={no_cookie}",
        # Every rule of the real networks from the first blocks (regtest leaves them off unless asked).
        "-testactivationheight=feesplit@1", "-testactivationheight=feecurve2@1", "-testactivationheight=dex@1",
        "-testactivationheight=ticker@1",
        f"-rpcuser={RPC_USER}", f"-rpcpassword={RPC_PASSWORD}", "-server", "-fallbackfee=0.0002", "-listenonion=0",
        "-dnsseed=0", "-fixedseeds=0", "-discover=0", "-printtoconsole=0",
    ]
    for i in range(3):
        nd = d / f"validator{i}"
        nd.mkdir()
        (nd / "validator.key").write_text("# validator key (regtest playground only)\n" + KEYS[i] + "\n")
        # Its wallet "w" made beforehand, loaded at the start: the producer's share of the fees goes to it.
        subprocess.run([str(sb / "starbase-wallet"), "-regtest", f"-datadir={nd}", "-wallet=w", "create"], check=True, capture_output=True)
        argv = common + [f"-datadir={nd}", f"-port={p[f'p2p{i}']}", f"-rpcport={p[f'rpc{i}']}", "-wallet=w",
                         f"-validatorkeyfile={nd / 'validator.key'}"]
        argv += [f"-addnode=127.0.0.1:{p[f'p2p{j}']}" for j in range(3) if j != i]
        state[f"argv{i}"] = argv
        if i == 0 and use_app:
            env = dict(os.environ)
            env.setdefault("LIBGL_ALWAYS_SOFTWARE", "1")  # WSLg
            spawn(d, "starbase-app", [str(starbase_app)] + argv + [f"-webuiconfig={nd / 'starbase-app.json'}"], env=env)
        else:
            spawn(d, f"starbased{i}", [str(starbased)] + argv)
    (d / STATE).write_text(json.dumps(state, indent=1))
    main_side, sides = nodes(state)
    for i in range(3):
        wait_rpc(Rpc(p[f"rpc{i}"]), f"validator {i}")
    print(f"  3 Starbase validators (slot {SLOT}, through the enforcer's interface), RPC {p['rpc0']}, {p['rpc1']}, {p['rpc2']}"
          + (" — validator 0 is starbase-app" if use_app else ""))

    spawn(d, "miner", [sys.executable, __file__, "_miner", "--dir", str(d)])
    print(f"  miner: a mainchain block every {args.block_seconds} s")
    if not args.no_demo:
        demo(d, state)
    print(f"Ready in {time.time() - t0:.0f} s. Try: starbase-playground.sh status | withdraw-demo | cli 0 getbalance | stop")


def demo(d, state):
    main, (n0, n1, n2) = nodes(state)
    print(f"Activating slot {SLOT} (M1, then acks of the next blocks)")
    main.createsidechainproposal(SLOT, "Starbase", "Tokens, assets and a DEX on a fast chain over eCash", "11" * 32, "22" * 20)
    wait_for(lambda: SLOT in [s["slot"] for s in main.listactivesidechains()], "the slot's activation", timeout=180)
    print("Deposits: 100 ECX to validator 0, 20 ECX to validator 1 (M5)")
    main.createsidechaindeposit(SLOT, n0.getdepositaddress()["depositaddress"], 100)
    main.createsidechaindeposit(SLOT, n1.getdepositaddress()["depositaddress"], 20)
    # The first block of the chain: merged mined (BMM), it takes the deposits; the blocks of stake follow it.
    deposit_height = main.getblockcount() + 1
    wait_for(lambda: main.getblockcount() >= deposit_height, "the deposits in a mainchain block", timeout=60)
    n0.requestbmmblock(n0.getnewaddress(), Decimal("0.001"))
    wait_for(lambda: n0.getbalance() >= 100 and n1.getbalance() >= 20, "the deposits", timeout=240)
    # Anchors (BMM tickets) asked for by validator 0 in every mainchain block.
    n0.setbmm(True, n0.getnewaddress(), True, Decimal("0.0001"))
    print(f"  validator 0: {n0.getbalance()} ECX, validator 1: {n1.getbalance()} ECX (Starbase height {n0.getblockcount()})")
    print("The demo token STAR: reserved, registered (1,000,000, 2 decimals), a STAR/ECX pool")
    n0.reserveasset("STAR")
    height = n0.getblockcount()
    wait_for(lambda: n0.getblockcount() >= height + 3, "two blocks for the reservation", timeout=120)
    for _ in range(60):
        try:
            n0.registerasset("STAR", 1000000, 2, {"info": "The demo token of the Starbase playground"})
            break
        except RuntimeError as e:
            if "deep" not in str(e) and "wait" not in str(e).lower() and "reveal" not in str(e).lower():
                raise
            time.sleep(2)
    wait_for(lambda: holding(n0, "STAR") > 0, "STAR in the wallet", timeout=120)
    n0.addliquidity("STAR", 100000, "ECX", 10)
    wait_for(lambda: len(n0.listpools()) == 1, "the pool", timeout=180)
    quote = n1.quoteswap("ECX", "1", "STAR")
    n1.swapasset("ECX", "1", "STAR")
    wait_for(lambda: holding(n1, "STAR") > 0, "validator 1's swap", timeout=180)
    print(f"  validator 1 swapped 1 ECX for {quote['amount_out']} STAR")


def holding(node, label):
    for a in node.listmyassets()["assets"]:
        if a["label"] == label:
            return Decimal(str(a["balance"]))
    return Decimal(0)


def miner(args):
    d = Path(args.dir).expanduser()
    state = load_state(d)
    main = Rpc(state["ports"]["main_rpc"], "miner")
    while True:
        try:
            main.generatetoaddress(1, state["main_address"])
        except (OSError, RuntimeError, urllib.error.URLError):
            pass
        time.sleep(state["block_seconds"])


def mine(args):
    d = Path(args.dir).expanduser()
    state = load_state(d)
    main = Rpc(state["ports"]["main_rpc"], "miner")
    for _ in range(args.count):
        main.generatetoaddress(1, state["main_address"])
        time.sleep(0.5)
    print(f"mainchain at {main.getblockcount()}")


def status(args):
    d = Path(args.dir).expanduser()
    pids = read_pids(d)
    if not pids:
        print(f"no playground in {d}")
        return
    for name, pid in pids.items():
        print(f"  {name:14} pid {pid:7} {'running' if pid_alive(pid) else 'stopped'}")
    state = load_state(d)
    main, sides = nodes(state)
    try:
        print(f"mainchain: height {main.getblockcount()}, active slots {[s['slot'] for s in main.listactivesidechains()]}")
        for i, n in enumerate(sides):
            pos = n.getposinfo()
            print(f"validator {i}: height {n.getblockcount()}, balance {n.getbalance()} ECX, anchors {pos.get('anchors')}, "
                  f"stalled {pos.get('stalled')}, RPC 127.0.0.1:{state['ports'][f'rpc{i}']} ({RPC_USER}/{RPC_PASSWORD})")
        b = sides[0].getwithdrawalbundle()
        print(f"withdrawal bundle of validator 0: {b.get('status')}")
    except (OSError, RuntimeError, urllib.error.URLError) as e:
        print(f"(not answering: {e})")


def stop(args):
    d = Path(args.dir).expanduser()
    pids = read_pids(d)
    if not pids:
        print(f"nothing runs from {d}")
        return
    state = json.loads((d / STATE).read_text()) if (d / STATE).exists() else None
    # The miner first, then the sidechain nodes (RPC stop), then the mainchain; SIGTERM for whatever is left.
    order = sorted(pids.items(), key=lambda kv: (kv[0] != "miner", kv[0] == "backboned"))
    for name, pid in order:
        if not pid_alive(pid):
            continue
        if name == "miner":
            os.kill(pid, signal.SIGTERM)
        elif state is not None:
            try:
                port = state["ports"]["main_rpc"] if name == "backboned" else state["ports"][f"rpc{0 if name == 'starbase-app' else int(name[-1])}"]
                Rpc(port).stop()
            except (OSError, RuntimeError, urllib.error.URLError, ValueError):
                os.kill(pid, signal.SIGTERM)
        else:
            os.kill(pid, signal.SIGTERM)
        end = time.time() + 120
        while pid_alive(pid) and time.time() < end:
            time.sleep(0.25)
        if pid_alive(pid):
            os.kill(pid, signal.SIGTERM)
            time.sleep(5)
        if pid_alive(pid):
            os.kill(pid, signal.SIGKILL)
        print(f"  stopped {name} ({pid})")
    (d / "pids").unlink()


def reset(args):
    d = Path(args.dir).expanduser()
    stop(args)
    if d.exists():
        shutil.rmtree(d)
    print(f"removed {d}")


def cli(args):
    state = load_state(Path(args.dir).expanduser())
    p = state["ports"]
    rpc = Rpc(p["main_rpc"], "miner") if args.node == "main" else Rpc(p[f"rpc{int(args.node)}"], "w")
    params = []
    for a in args.params:
        try:
            params.append(json.loads(a, parse_float=Decimal))
        except ValueError:
            params.append(a)
    result = rpc.call(args.method, *params)
    print(result if isinstance(result, str) else json.dumps(result, indent=2, default=str))


def withdraw_demo(args):
    d = Path(args.dir).expanduser()
    state = load_state(d)
    main, (n0, n1, n2) = nodes(state)
    t0 = time.time()
    to = main.getnewaddress()
    print(f"1. Request: validator 0 withdraws 2 ECX to {to} on eCash (fee 0.01 for eCash miners)")
    w = n0.createwithdrawal(to, 2, Decimal("0.01"))
    wait_for(lambda: n0.gettransaction(w["txid"])["confirmations"] > 0, "the withdrawal in a block", timeout=120)
    print(f"   in a Starbase block ({time.time() - t0:.0f} s)")
    seen = set()
    end = time.time() + 900
    paid = False
    while time.time() < end:
        b = n0.getwithdrawalbundle()
        status_ = b.get("status")
        if status_ not in seen:
            seen.add(status_)
            print(f"2. Bundle (sidechain): {status_} {b.get('hash', '')} ({time.time() - t0:.0f} s)")
        if b.get("hash"):
            mb = main.getwithdrawalbundle(SLOT, b["hash"])
            st = mb.get("status")
            if st == "pending":
                key = f"votes {mb.get('score')}"
                if "submitted" not in seen:
                    seen.add("submitted")
                    print(f"3. Submitted on eCash (M3) at mainchain height {main.getblockcount()}: votes needed: more than 5 in 10 blocks ({time.time() - t0:.0f} s)")
                if key not in seen:
                    seen.add(key)
                    print(f"   votes (M4) {mb.get('score')}, {mb.get('blocksleft')} blocks left{', payable' if mb.get('payable') else ''} ({time.time() - t0:.0f} s)")
            elif st == "paid" and "paid" not in seen:
                seen.add("paid")
                print(f"   paid by the M6 of mainchain block {main.getblockcount()} ({time.time() - t0:.0f} s)")
            if mb.get("status") == "paid":
                paid = True
                break
        time.sleep(1)
    if not paid:
        sys.exit("the bundle was not paid in 15 minutes: see status")
    wait_for(lambda: main.getreceivedbyaddress(to, 0) >= 2, "the payout", timeout=120)
    print(f"4. Succeeded: {to} received {main.getreceivedbyaddress(to, 0)} ECX on eCash (M6) — {time.time() - t0:.0f} s in all")
    wait_for(lambda: n0.listwithdrawals() == [], "the withdrawal closed on Starbase", timeout=120)
    print("   closed on Starbase")


def main():
    default_bb = os.environ.get("BACKBONE_BIN_DIR", str(SRCDIR.parent / "chains-ecash" / "build_node" / "bin"))
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dir", default=os.environ.get("STARBASE_PLAYGROUND", "~/.starbase-playground"))
    sub = parser.add_subparsers(dest="command", required=True)
    s = sub.add_parser("start")
    s.add_argument("--block-seconds", type=float, default=5)
    s.add_argument("--slot-seconds", type=int, default=2, help="seconds of a slot of the blocks of stake (default 2; 15 on eCash)")
    s.add_argument("--base-port", type=int, default=49500)
    s.add_argument("--starbase-bin", default=str(SRCDIR / "build" / "bin"))
    s.add_argument("--backbone-bin", default=default_bb)
    s.add_argument("--no-app", action="store_true")
    s.add_argument("--no-demo", action="store_true")
    s.set_defaults(func=start)
    for name, func in (("status", status), ("stop", stop), ("reset", reset), ("withdraw-demo", withdraw_demo)):
        sub.add_parser(name).set_defaults(func=func)
    m = sub.add_parser("mine")
    m.add_argument("count", type=int)
    m.set_defaults(func=mine)
    c = sub.add_parser("cli")
    c.add_argument("node", choices=["main", "0", "1", "2"])
    c.add_argument("method")
    c.add_argument("params", nargs="*")
    c.set_defaults(func=cli)
    mi = sub.add_parser("_miner")
    mi.set_defaults(func=miner)
    # --dir after the command too.
    for p in (s, m, c, mi, *[sub.choices[n] for n in ("status", "stop", "reset", "withdraw-demo")]):
        p.add_argument("--dir", default=argparse.SUPPRESS)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
