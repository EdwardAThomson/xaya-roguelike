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

import json
import jsonrpclib
import logging
import os
import os.path
import random
import shutil
import subprocess
import sys
import time
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler

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

  env = None  # Set before starting the server.

  def do_GET (self):
    if self.path == "/ping":
      self._respond (200, {"ok": True})
    else:
      self._respond (404, {"error": "not found"})

  def do_POST (self):
    try:
      length = int (self.headers.get ("Content-Length", 0))
      body = json.loads (self.rfile.read (length)) if length > 0 else {}

      action = body.get ("action", "")

      if action == "register":
        name = body["name"]
        with ENV_LOCK:
          self.env.register ("p", name)
          self.env.generate (1)
        self._respond (200, {"ok": True, "name": name})

      elif action == "move":
        name = body["name"]
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
  # ports (anvil, xayax) come from the random pool to avoid clashes.
  startPort = random.randint (20000, 30000)
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

        # Start the move proxy.
        MoveProxyHandler.env = e
        proxy = HTTPServer (("0.0.0.0", PROXY_PORT), MoveProxyHandler)

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
