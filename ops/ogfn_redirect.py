# ogfn_redirect.py -- LawinServer redirect rule as a mitmproxy addon.
# Mirrors the official LawinServer FiddlerScript: rewrite every request whose host is an
# Epic domain (minus /v1/item/") to http://127.0.0.1:3551.
# Deviation: only *.epicgames.com hosts are rewritten so modern EGS on epicgames.dev keeps
# working while an OGFN session is live.
# REQUIRES: --set connection_strategy=lazy so the upstream is chosen at request time,
# after this hook; eager mode connects upstream at CONNECT and the rewrite cannot apply.

from mitmproxy import http

LOG = r"C:\ogfn\requests.log"
BACKEND_HOST = "127.0.0.1"
BACKEND_PORT = 3551

def _log(line):
    try:
        with open(LOG, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass

def request(flow: http.HTTPFlow) -> None:
    host = flow.request.pretty_host.lower()
    _log("REQ host=%s port=%s path=%s" % (host, flow.request.port, flow.request.path))
    if (host.endswith("epicgames.com") or host.endswith("epicgames.com.")) \
            and "/v1/item/" not in flow.request.path:
        flow.request.scheme = "http"
        flow.request.host = BACKEND_HOST
        flow.request.port = BACKEND_PORT
        _log("REWRITE -> %s:%s%s" % (BACKEND_HOST, flow.request.port, flow.request.path))

def connect_error(data):
    _log("CONNECT_ERROR: %s" % (data,))

def error(data):
    _log("FLOW_ERROR: %s" % (data,))
