#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A client of flushd's public DEX API (doc/public-api.md) for the functional tests: REST over HTTP/1.1, the Connect
protocol (JSON, and binary through protoc's own codec), gRPC through flush-grpc-client (gRPC's own C++ library)."""

import hashlib
import http.client
import json
import os
import shutil
import subprocess
from urllib.parse import quote, urlencode

PROTO_DIR = os.path.join(os.path.dirname(os.path.realpath(__file__)), "..", "..", "..", "src", "grpc", "proto")
PROTO_FILE = "flush/dex/v1/dex.proto"
SERVICE = "flush.dex.v1.DexService"


class Reply:
    def __init__(self, status, headers, body):
        self.status = status
        self.headers = {k.lower(): v for k, v in headers}
        self.body = body

    def json(self):
        return json.loads(self.body)

    def __repr__(self):
        return f"Reply({self.status}, {self.body[:300]!r})"


class PublicApi:
    def __init__(self, port, host="127.0.0.1"):
        self.host = host
        self.port = port

    def request(self, method, path, body=None, headers=None):
        conn = http.client.HTTPConnection(self.host, self.port, timeout=60)
        try:
            conn.request(method, path, body=body, headers=headers or {})
            r = conn.getresponse()
            return Reply(r.status, r.getheaders(), r.read())
        finally:
            conn.close()

    def get(self, path, params=None, headers=None):
        """GET /api/v1/<path>?<params>: the reply (any status)."""
        qs = ("?" + urlencode(params, doseq=True, quote_via=quote)) if params else ""
        return self.request("GET", "/api/v1/" + path + qs, headers=headers)

    def rest(self, path, params=None, headers=None):
        """GET a resource that must answer 200: its JSON."""
        r = self.get(path, params, headers)
        assert r.status == 200, f"{path}: {r}"
        return r.json()

    def post(self, path, obj, headers=None):
        h = {"Content-Type": "application/json"}
        h.update(headers or {})
        return self.request("POST", "/api/v1/" + path, body=json.dumps(obj), headers=h)

    def connect(self, method, request, headers=None):
        """A Connect unary call with JSON: (status, JSON)."""
        h = {"Content-Type": "application/json"}
        h.update(headers or {})
        r = self.request("POST", f"/{SERVICE}/{method}", body=json.dumps(request), headers=h)
        return r.status, (json.loads(r.body) if r.body else None)

    def connect_get(self, method, request):
        """Connect's GET form of a read."""
        path = f"/{SERVICE}/{method}?" + urlencode({"encoding": "json", "message": json.dumps(request)}, quote_via=quote)
        r = self.request("GET", path)
        return r.status, (json.loads(r.body) if r.body else None)

    def connect_proto(self, method, request_bytes):
        r = self.request("POST", f"/{SERVICE}/{method}", body=request_bytes, headers={"Content-Type": "application/proto"})
        return r.status, r.body


def protoc_available():
    return shutil.which("protoc") is not None


def encode(message_type, text):
    """Protocol buffers' text format to binary, by protoc (an implementation other than the node's)."""
    return subprocess.run(["protoc", f"-I{PROTO_DIR}", f"--encode=flush.dex.v1.{message_type}", PROTO_FILE], input=text.encode(), capture_output=True, check=True).stdout


def decode(message_type, data):
    """Binary to protoc's text format."""
    return subprocess.run(["protoc", f"-I{PROTO_DIR}", f"--decode=flush.dex.v1.{message_type}", PROTO_FILE], input=data, capture_output=True, check=True).stdout.decode()


def grpc_call(client, port, method, request_bytes, timeout=30):
    """A gRPC call through flush-grpc-client (grpc++): (status code, message, [reply bytes])."""
    p = subprocess.run([client, f"127.0.0.1:{port}", f"/{method}" if method.startswith(("grpc.", "flush.")) else f"/{SERVICE}/{method}", "--timeout", str(timeout)],
                       input=(request_bytes.hex() + "\n").encode(), capture_output=True, timeout=timeout + 10)
    replies, status, message = [], None, ""
    for line in p.stdout.decode().splitlines():
        if line.startswith("MSG "):
            replies.append(bytes.fromhex(line[4:].strip()))
        elif line.startswith("STATUS "):
            parts = line.split(" ", 2)
            status = int(parts[1])
            message = parts[2] if len(parts) > 2 else ""
    return status, message, replies


def action_hash(payload):
    return hashlib.sha256(b"flush-action/v1\n" + payload.encode()).hexdigest()


# The parameters each verb of the app's links takes (flush.markets docs/uri.md; the app's parser refuses any other).
URI_PARAMS = {
    "buy": {"launch", "asset", "ecx", "min_tokens", "slippage_bps"},
    "sell": {"launch", "asset", "tokens", "min_ecx", "slippage_bps"},
    "swap": {"pool", "from", "to", "amount", "min_out", "slippage_bps"},
    "launch": {"ticker", "name", "logo", "logo_url", "desc", "link", "ecx", "min_tokens", "slippage_bps", "supply", "decimals", "fee_bps", "allocation_bps"},
    "register": {"ticker", "name", "logo", "logo_url", "desc", "link", "supply", "decimals"},
    "curve": {"asset", "ticker", "amount", "fee_bps", "min_buy", "max_buy"},
    "pool": {"asset_a", "ticker_a", "asset_b", "ticker_b", "kind", "fee_ppm", "amount_a", "amount_b", "amp", "weight_a"},
    "auction": {"base", "base_ticker", "amount", "quote", "quote_ticker", "start_price", "end_price", "duration", "delay"},
    "order": {"sell", "sell_ticker", "buy", "buy_ticker", "amount", "price", "expiry_blocks"},
    "route": {"pools", "from", "to", "amount", "min_out", "slippage_bps"},
    "deposit": {"pool", "asset_a", "asset_b", "amount_a", "amount_b", "min_shares", "slippage_bps"},
    "withdraw": {"pool", "asset_a", "asset_b", "shares", "min_a", "min_b", "only", "slippage_bps"},
    "position": {"pool", "asset_a", "asset_b", "price_lower", "price_upper", "amount_a", "amount_b", "price", "slippage_bps"},
    "reposition": {"position", "action", "percent", "slippage_bps"},
    "settle": {"order", "cancel"},
    "bid": {"auction", "amount", "buy_all"},
    "collect": {"auction"},
    "claim": {"launch", "asset"},
    "pegin": {"amount"},
    "pegout": {"address", "amount", "fee"},
}
UNRESERVED = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~%")


def parse_uri(uri):
    """A flush: action URI as the app reads it: (verb, {param: value}, [links]); asserts its form."""
    from urllib.parse import unquote
    assert uri.startswith("flush:"), uri
    verb, _, query = uri[len("flush:"):].partition("?")
    assert verb.isalpha() and verb.islower(), verb
    params, links = {}, []
    for pair in query.split("&"):
        key, _, value = pair.partition("=")
        assert set(value) <= UNRESERVED, f"{key} not percent-encoded: {value}"
        if key == "link":
            links.append(unquote(value))
            continue
        assert key not in params, f"{key} twice"
        params[key] = unquote(value)
    assert params.pop("v") == "1"
    return verb, params, links
