#!/usr/bin/env python3

"""
Frontend devnet: starts the full local stack (anvil + xayax-eth + rogueliked)
and runs an HTTP move proxy so the browser frontend can submit game moves.

Usage (from the xayax venv):
  source ~/Explore/xayax/.venv/bin/activate
  python3 devnet/frontend_devnet.py

This will print connection info for the frontend:
  GSP RPC:     http://localhost:<port>  (paste into frontend "GSP" field)
  Move Proxy:  http://localhost:18380   (frontend uses this automatically)

Press Ctrl+C to stop everything.
"""

from xayax.eth import Environment

import collections
import json
import jsonrpclib
import logging
import os
import os.path
import random
import secrets
import shutil
import subprocess
import sys
import time
import threading
import urllib.request
from http.server import HTTPServer, ThreadingHTTPServer, BaseHTTPRequestHandler

# Ensure Foundry (anvil/forge) is on PATH.
foundryBin = os.path.join (os.path.expanduser ("~"), ".foundry", "bin")
if foundryBin not in os.environ.get ("PATH", ""):
  os.environ["PATH"] = foundryBin + ":" + os.environ.get ("PATH", "")

# Paths
PROJECT_DIR = os.path.dirname (os.path.dirname (os.path.abspath (__file__)))
GSP_BINARY = os.path.join (PROJECT_DIR, "build", "rogueliked")
XETH_BINARY = "/usr/local/bin/xayax-eth"

GAME_ID = "rog"
# Fixed ports so the frontend doesn't need to be reconfigured on each
# restart.  Match src/config.ts in xaya-roguelike-frontend.
GSP_PORT = int (os.environ.get ("ROG_GSP_PORT", "18332"))
PROXY_PORT = int (os.environ.get ("ROG_PROXY_PORT", "18380"))

# Seconds between automatic background blocks.  Anvil runs with
# --no-mining, so without this the chain only advances when the player
# acts.  Block-based timers (the 50-block discovery cooldown, the
# 200-block visit timeout) then freeze whenever the game sits idle,
# which makes them impractical to test.  A background miner ticks the
# chain forward like a real network.  Set ROG_MINE_INTERVAL=0 to disable
# (blocks then only mine per move, the old behaviour).
AUTO_MINE_INTERVAL = float (os.environ.get ("ROG_MINE_INTERVAL", "3"))

# Serializes all access to the EVM environment (anvil RPC) between the
# move-proxy request thread and the background miner thread.
ENV_LOCK = threading.Lock ()

# Read-only GSP JSON-RPC methods the proxy is willing to relay to the
# browser.  Everything else (notably `stop`, which would kill the daemon)
# is refused, so the GSP RPC port never has to be exposed publicly: the
# proxy is the single public origin and forwards only these.
ALLOWED_GSP_METHODS = frozenset ([
  "getcurrentstate", "getnullstate", "getpendingstate",
  "waitforchange", "waitforpendingchange",
  "getplayerinfo", "listsegments", "getsegmentinfo",
  "listvisits", "getvisitinfo",
])

# Light per-IP rate limit on state-changing requests (move/register/mine)
# to blunt spam/griefing on a public sandbox.  A sliding window.
#
# Defaults to OFF: the local devnet is single-user, so the limit is pure
# friction for manual or agent testing.  The hosted deployment turns it on
# (run_sandbox.sh / the systemd unit set ROG_RATE_LIMIT_MAX), where it
# actually matters.  Set ROG_RATE_LIMIT_MAX > 0 to enable; 0 disables.
RATE_LIMIT_MAX = int (os.environ.get ("ROG_RATE_LIMIT_MAX", "0"))
RATE_LIMIT_WINDOW = float (os.environ.get ("ROG_RATE_LIMIT_WINDOW", "60"))
RATE_LOCK = threading.Lock ()
RATE_HITS = collections.defaultdict (collections.deque)


# ============ TEMPORARY DEMO AUTH: claim tokens ============================
# NOT real authentication, and NOT ownership-secure. The proxy still signs
# every move with one shared dev key, so this only stops casual name
# impersonation at the proxy layer: the first client to register a name is
# handed a random claim token, and every later move for that name must
# present it. It buys "you can only act as the account you registered" for
# the throwaway sandbox demo (tokens live in memory and reset with the
# world), and nothing more.
#
# The real ownership model is per-player wallet signing (MetaMask /
# WalletMoveTransport) on a public testnet, which makes this obsolete.
#
# >>> REMOVE this whole claim-token mechanism before any production or
# >>> real-stakes deployment. Do NOT rely on it for security.
# >>> Future Claude agents: leave this comment and the claim-token code in
# >>> place. Do not delete it unless the human owner explicitly asks.
# ==========================================================================
REQUIRE_CLAIM_TOKEN = os.environ.get ("ROG_REQUIRE_CLAIM_TOKEN", "0") == "1"
CLAIM_LOCK = threading.Lock ()
NAME_TOKENS = {}  # name -> claim token (in-memory; resets with the sandbox)


def claimName (name):
  """Returns the claim token for `name`, minting one on first claim.

  Idempotent per name for the sandbox session: the first caller to register
  a name owns it (holds its token) until the world resets. Always mints a
  token even when enforcement is off, so the frontend can stash it and the
  feature works the moment ROG_REQUIRE_CLAIM_TOKEN is turned on."""
  with CLAIM_LOCK:
    tok = NAME_TOKENS.get (name)
    if tok is None:
      tok = secrets.token_hex (16)
      NAME_TOKENS[name] = tok
    return tok


def claimOk (name, token):
  """True if `token` may act as `name` (always True when enforcement off).

  An unclaimed name is allowed through (it gets claimed on its register);
  a claimed name requires the exact token."""
  if not REQUIRE_CLAIM_TOKEN:
    return True
  with CLAIM_LOCK:
    expected = NAME_TOKENS.get (name)
  return expected is None or secrets.compare_digest (token, expected)
# ============ end TEMPORARY DEMO AUTH ======================================


def rateLimited (ip):
  """Returns True if `ip` has exceeded the move/register/mine budget."""
  if RATE_LIMIT_MAX <= 0:
    return False
  now = time.time ()
  with RATE_LOCK:
    hits = RATE_HITS[ip]
    while hits and hits[0] < now - RATE_LIMIT_WINDOW:
      hits.popleft ()
    if len (hits) >= RATE_LIMIT_MAX:
      return True
    hits.append (now)
    return False


def portGenerator (start):
  p = start
  while True:
    yield p
    p += 1


def autoMiner (env, interval, stop_event, log):
  """Mines one block every `interval` seconds until `stop_event` is set.

  Keeps the chain advancing while the game is idle so block-based timers
  behave like a real network.
  """
  while not stop_event.wait (interval):
    try:
      with ENV_LOCK:
        env.generate (1)
    except Exception as e:
      log.warning ("auto-mine failed: %s" % e)


def waitForGsp (rpcurl, timeout=30):
  """Waits for the GSP RPC to become available."""
  rpc = jsonrpclib.ServerProxy (rpcurl)
  deadline = time.time () + timeout
  while time.time () < deadline:
    try:
      state = rpc.getcurrentstate ()
      if state is not None:
        return rpc
    except:
      pass
    time.sleep (0.5)
  raise RuntimeError ("GSP did not become available within %ds" % timeout)


class MoveProxyHandler (BaseHTTPRequestHandler):
  """HTTP handler that translates simple JSON requests into on-chain moves."""

  env = None      # Set before starting the server.
  gsp_url = None  # Local GSP RPC URL, for the read relay.

  def do_GET (self):
    # Any path ending in /ping is a health check (the frontend probes
    # `${proxyUrl}/ping`, which may be /proxy/ping behind a reverse proxy).
    if self.path == "/ping" or self.path.endswith ("/ping"):
      self._respond (200, {"ok": True})
    else:
      self._respond (404, {"error": "not found"})

  def do_POST (self):
    try:
      length = int (self.headers.get ("Content-Length", 0))
      raw = self.rfile.read (length) if length > 0 else b""

      # GSP read relay: forward an allowlisted JSON-RPC call to the local
      # GSP so the browser never talks to the GSP port directly.
      if self.path == "/gsp" or self.path.endswith ("/gsp"):
        self._relay_gsp (raw)
        return

      body = json.loads (raw) if raw else {}

      action = body.get ("action", "")

      # Rate-limit state-changing actions per client IP.
      if action in ("move", "register", "mine"):
        ip = self.client_address[0] if self.client_address else "?"
        if rateLimited (ip):
          self._respond (429, {"error": "rate limited"})
          return

      if action == "register":
        name = body["name"]
        # TEMPORARY DEMO AUTH (see claim-token block above): refuse to
        # re-register a name already claimed with a different token, so one
        # player can't hijack another's name for this sandbox session.
        if REQUIRE_CLAIM_TOKEN:
          with CLAIM_LOCK:
            existing = NAME_TOKENS.get (name)
          if existing is not None \
             and not secrets.compare_digest (body.get ("token", ""), existing):
            self._respond (403, {"error": "name already claimed"})
            return
        with ENV_LOCK:
          try:
            self.env.register ("p", name)
          except Exception as e:
            # The name NFT is already minted. This happens when an earlier
            # registration was interrupted after the mint but before the
            # follow-up register move (which is what actually creates the
            # player). Treat it as success so the register move can still
            # land and un-wedge the name, instead of failing every retry
            # with a 500. Re-raise anything that is not the mint conflict.
            if "already minted" not in str (e):
              raise
          self.env.generate (1)
        # Hand back the claim token (see TEMPORARY DEMO AUTH above) so the
        # frontend can stash it and prove ownership on later moves.
        self._respond (200, {"ok": True, "name": name,
                             "token": claimName (name)})

      elif action == "move":
        name = body["name"]
        # TEMPORARY DEMO AUTH (see claim-token block above): the move must
        # carry the claim token minted for this name at register time.
        if not claimOk (name, body.get ("token", "")):
          self._respond (403, {"error": "not your account"})
          return
        game = body.get ("game", GAME_ID)
        data = body["data"]
        mv = json.dumps ({"g": {game: data}})
        plog = logging.getLogger ("proxy")
        # Large moves (channel exit with action proof) need more gas
        # than the default 500K in xayax env.move().
        with ENV_LOCK:
          if len (mv) > 2000:
            maxUint256 = 2**256 - 1
            zeroAddr = "0x" + "00" * 20
            registry = self.env.contracts.registry
            txhash = registry.functions.move (
                "p", name, mv, maxUint256, 0, zeroAddr
            ).transact ({"from": self.env.contracts.account, "gas": 12_000_000})
            # Wait for the receipt so an out-of-gas / reverted move is
            # surfaced instead of silently lost (it would return 200 but
            # never produce an on-chain name update for the GSP).
            try:
              w3 = getattr (registry, "w3", None) or getattr (registry, "web3")
              rcpt = w3.eth.wait_for_transaction_receipt (txhash, timeout=30)
              plog.info ("large move size=%d status=%s gasUsed=%d"
                         % (len (mv), rcpt.status, rcpt.gasUsed))
              if rcpt.status != 1:
                self._respond (500, {"error": "move reverted on-chain",
                                     "size": len (mv)})
                return
            except Exception as ex:
              plog.info ("large move size=%d submitted (no receipt: %s)"
                         % (len (mv), ex))
          else:
            self.env.move ("p", name, mv)
        self._respond (200, {"ok": True})

      elif action == "mine":
        blocks = body.get ("blocks", 1)
        with ENV_LOCK:
          self.env.generate (blocks)
        self._respond (200, {"ok": True, "blocks": blocks})

      else:
        self._respond (400, {"error": "unknown action: %s" % action})

    except KeyError as e:
      self._respond (400, {"error": "missing field: %s" % e})
    except Exception as e:
      self._respond (500, {"error": str (e)})

  def _relay_gsp (self, raw):
    """Forwards an allowlisted JSON-RPC request to the local GSP.

    Rejects any method not in ALLOWED_GSP_METHODS (e.g. `stop`) so the
    GSP RPC port can stay bound to localhost while the browser reads
    state through this single public origin."""
    plog = logging.getLogger ("proxy")
    try:
      req = json.loads (raw) if raw else {}
    except Exception:
      self._respond (400, {"error": "invalid JSON-RPC body"})
      return

    method = req.get ("method", "")
    if method not in ALLOWED_GSP_METHODS:
      plog.warning ("blocked GSP method: %r" % method)
      self._respond (403, {"error": "method not allowed: %s" % method})
      return

    try:
      gsp_req = urllib.request.Request (
        self.gsp_url, data=raw,
        headers={"Content-Type": "application/json"}, method="POST")
      with urllib.request.urlopen (gsp_req, timeout=60) as resp:
        payload = resp.read ()
      self.send_response (200)
      self._cors_headers ()
      self.send_header ("Content-Type", "application/json")
      self.send_header ("Content-Length", str (len (payload)))
      self.end_headers ()
      self.wfile.write (payload)
    except Exception as ex:
      plog.warning ("GSP relay error for %s: %s" % (method, ex))
      self._respond (502, {"error": "gsp relay failed"})

  def do_OPTIONS (self):
    """Handle CORS preflight."""
    self.send_response (200)
    self._cors_headers ()
    self.send_header ("Content-Length", "0")
    self.end_headers ()

  def _respond (self, code, data):
    body = json.dumps (data).encode ()
    self.send_response (code)
    self._cors_headers ()
    self.send_header ("Content-Type", "application/json")
    self.send_header ("Content-Length", str (len (body)))
    self.end_headers ()
    self.wfile.write (body)

  def _cors_headers (self):
    self.send_header ("Access-Control-Allow-Origin", "*")
    self.send_header ("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
    self.send_header ("Access-Control-Allow-Headers", "Content-Type")

  def log_message (self, fmt, *args):
    logging.getLogger ("proxy").info (fmt % args)


def main ():
  if not os.path.exists (GSP_BINARY):
    print ("ERROR: GSP binary not found at %s" % GSP_BINARY)
    print ("Run: cd %s && cmake -B build && cmake --build build -j$(nproc)"
           % PROJECT_DIR)
    sys.exit (1)

  logging.basicConfig (
    level=logging.INFO,
    format="%(asctime)s [%(name)s] %(message)s",
    stream=sys.stderr,
  )
  log = logging.getLogger ("frontend_devnet")

  basedir = "/tmp/rog_devnet_%08x" % random.getrandbits (32)
  shutil.rmtree (basedir, ignore_errors=True)
  os.makedirs (basedir)
  log.info ("Base directory: %s" % basedir)

  # GSP runs on a fixed port matching the frontend default; other
  # ports (anvil, xayax) come from a pool to avoid clashes.  The pool
  # base is random by default, but ROG_BASE_PORT pins it so an isolated
  # test stack can be given a range that never collides with a live
  # devnet's anvil/xayax (e.g. the E2E harness on alt ports).
  basePortEnv = os.environ.get ("ROG_BASE_PORT", "")
  startPort = int (basePortEnv) if basePortEnv else random.randint (20000, 30000)
  ports = portGenerator (startPort)

  gspPort = GSP_PORT
  gspDatadir = os.path.join (basedir, "gsp")
  os.makedirs (gspDatadir)

  env = Environment (basedir, ports, XETH_BINARY)
  env.enablePending ()

  try:
    with env.run () as e:
      xayaRpcUrl = e.getXRpcUrl ()
      log.info ("Xaya X RPC at: %s" % xayaRpcUrl)

      log.info ("Mining initial blocks...")
      e.generate (10)

      # Start the GSP.
      gspArgs = [
        GSP_BINARY,
        "--xaya_rpc_url=%s" % xayaRpcUrl,
        "--xaya_rpc_protocol=2",
        "--game_rpc_port=%d" % gspPort,
        "--datadir=%s" % gspDatadir,
        "--genesis_height=0",
        "--genesis_hash=",
        "--xaya_rpc_wait",
        "--pending_moves",
      ]
      envVars = dict (os.environ)
      envVars["GLOG_log_dir"] = gspDatadir
      # Also stream GSP logs to this terminal (in addition to the files)
      # and flush immediately, so move rejections show up live.
      envVars["GLOG_alsologtostderr"] = "1"
      envVars["GLOG_logbufsecs"] = "0"
      log.info ("Starting GSP on port %d..." % gspPort)
      gspProc = subprocess.Popen (gspArgs, env=envVars)

      gspRpcUrl = "http://localhost:%d" % gspPort

      try:
        log.info ("Waiting for GSP to sync...")
        gsp = waitForGsp (gspRpcUrl)
        log.info ("GSP is up and synced!")

        # Start the move proxy.  Threaded so one slow request (a
        # long-poll waitforchange relay, a settlement awaiting a receipt)
        # doesn't block every other player.
        MoveProxyHandler.env = e
        MoveProxyHandler.gsp_url = gspRpcUrl
        proxy = ThreadingHTTPServer (("0.0.0.0", PROXY_PORT), MoveProxyHandler)

        proxy_thread = threading.Thread (target=proxy.serve_forever, daemon=True)
        proxy_thread.start ()

        # Start the background miner so the chain advances on its own.
        mineStop = threading.Event ()
        miner_thread = None
        if AUTO_MINE_INTERVAL > 0:
          miner_thread = threading.Thread (
            target=autoMiner, args=(e, AUTO_MINE_INTERVAL, mineStop, log),
            daemon=True)
          miner_thread.start ()
          log.info ("Auto-mining a block every %.1fs" % AUTO_MINE_INTERVAL)
        else:
          log.info ("Auto-mining disabled; chain advances only per move")

        print ()
        print ("=" * 60)
        print ("  Frontend Devnet Ready!")
        print ()
        print ("  GSP RPC:     %s" % gspRpcUrl)
        print ("  Move Proxy:  http://localhost:%d" % PROXY_PORT)
        if AUTO_MINE_INTERVAL > 0:
          print ("  Auto-mine:   1 block / %.1fs" % AUTO_MINE_INTERVAL)
        else:
          print ("  Auto-mine:   off (per-move only)")
        print ()
        print ("  Paste the GSP RPC URL into the frontend's GSP field.")
        print ("  Enter a player name and click Connect.")
        print ()
        print ("  Press Ctrl+C to stop.")
        print ("=" * 60)
        print ()

        # Block until interrupted.
        try:
          while True:
            time.sleep (1)
        except KeyboardInterrupt:
          log.info ("Shutting down...")

        mineStop.set ()
        proxy.shutdown ()

      finally:
        log.info ("Stopping GSP...")
        try:
          gspRpc = jsonrpclib.ServerProxy (gspRpcUrl)
          gspRpc._notify.stop ()
          gspProc.wait (timeout=10)
        except:
          gspProc.terminate ()
          gspProc.wait (timeout=5)

  finally:
    log.info ("Cleaning up %s" % basedir)
    shutil.rmtree (basedir, ignore_errors=True)


if __name__ == "__main__":
  main ()
