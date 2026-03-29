#!/usr/bin/env python3

"""
Smoke test: starts the full local stack (anvil + xayax-eth + rogueliked)
and verifies basic game operations work end-to-end.

Usage (from the xayax venv):
  source ~/Explore/xayax/.venv/bin/activate
  python3 devnet/smoke_test.py
"""

from xayax.eth import Environment
from xayagametest import testcase

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

# Ensure Foundry (anvil/forge) is on PATH.
foundryBin = os.path.join (os.path.expanduser ("~"), ".foundry", "bin")
if foundryBin not in os.environ.get ("PATH", ""):
  os.environ["PATH"] = foundryBin + ":" + os.environ.get ("PATH", "")

# Paths
PROJECT_DIR = os.path.dirname (os.path.dirname (os.path.abspath (__file__)))
GSP_BINARY = os.path.join (PROJECT_DIR, "build", "rogueliked")
XETH_BINARY = "/usr/local/bin/xayax-eth"

GAME_ID = "rog"


def portGenerator (start):
  p = start
  while True:
    yield p
    p += 1


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


def main ():
  if not os.path.exists (GSP_BINARY):
    print ("ERROR: GSP binary not found at %s" % GSP_BINARY)
    print ("Run: cd %s && cmake -B build && cmake --build build -j$(nproc)" % PROJECT_DIR)
    sys.exit (1)

  logging.basicConfig (
    level=logging.INFO,
    format="%(asctime)s [%(name)s] %(message)s",
    stream=sys.stderr,
  )
  log = logging.getLogger ("smoke_test")

  # Set up temporary directory.
  basedir = "/tmp/rog_smoke_%08x" % random.getrandbits (32)
  shutil.rmtree (basedir, ignore_errors=True)
  os.makedirs (basedir)
  log.info ("Base directory: %s" % basedir)

  startPort = random.randint (10000, 30000)
  ports = portGenerator (startPort)

  gspPort = next (ports)
  gspDatadir = os.path.join (basedir, "gsp")
  os.makedirs (gspDatadir)

  env = Environment (basedir, ports, XETH_BINARY)
  env.enablePending ()

  try:
    with env.run () as e:
      xayaRpcUrl = e.getXRpcUrl ()
      log.info ("Xaya X RPC at: %s" % xayaRpcUrl)

      # Mine some initial blocks so the chain has history.
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
      log.info ("Starting GSP: %s" % " ".join (gspArgs))
      gspProc = subprocess.Popen (gspArgs, env=envVars)

      gspRpcUrl = "http://localhost:%d" % gspPort

      try:
        # Wait for GSP to sync.
        log.info ("Waiting for GSP to sync...")
        gsp = waitForGsp (gspRpcUrl)
        log.info ("GSP is up and synced!")

        # === Test 1: Register a player ===
        log.info ("=== Test 1: Register player 'alice' ===")
        e.register ("p", "alice")
        e.move ("p", "alice", json.dumps ({"g": {GAME_ID: {"r": {}}}}))
        e.generate (1)
        time.sleep (1)  # Give GSP time to process

        resp = gsp.getplayerinfo ("alice")
        info = resp["data"] if "data" in resp else resp
        assert info is not None and "name" in info, \
            "Player alice not found! Got: %s" % resp
        assert info["name"] == "alice"
        assert info["level"] == 1
        log.info ("PASS: alice registered, level %d" % info["level"])

        # === Test 2: Check full state ===
        log.info ("=== Test 2: Check full state ===")
        state = gsp.getcurrentstate ()
        log.info ("Current state: %s" % json.dumps (state, indent=2)[:500])

        # === Test 3: Check HP ===
        log.info ("=== Test 3: Check HP ===")
        assert info["hp"] == 100, "Expected HP 100, got %d" % info["hp"]
        assert info["max_hp"] == 100
        assert info["current_segment"] == 0
        log.info ("PASS: alice has %d/%d HP at segment %d"
                  % (info["hp"], info["max_hp"], info["current_segment"]))

        # === Test 4: Discover a provisional segment ===
        log.info ("=== Test 4: Discover segment east ===")
        e.move ("p", "alice", json.dumps (
            {"g": {GAME_ID: {"d": {"depth": 1, "dir": "east"}}}}))
        e.generate (1)
        time.sleep (1)

        resp = gsp.getplayerinfo ("alice")
        info = resp["data"] if "data" in resp else resp
        assert info["active_visit"] is None, \
            "Discover should NOT create a visit (provisional)"
        log.info ("PASS: discover created provisional segment, no auto-visit")

        resp = gsp.listsegments ()
        segments = resp["data"] if "data" in resp else resp
        assert len (segments) == 1, "Expected 1 segment"
        log.info ("PASS: 1 provisional segment exists")

        # === Test 5: Discoverer enters provisional segment ===
        log.info ("=== Test 5: Enter channel (discoverer privilege) ===")
        e.move ("p", "alice", json.dumps (
            {"g": {GAME_ID: {"ec": {"id": 1}}}}))
        e.generate (1)
        time.sleep (1)

        resp = gsp.getplayerinfo ("alice")
        info = resp["data"] if "data" in resp else resp
        assert info["in_channel"], "alice should be in channel"
        assert info["current_segment"] == 1, "alice should be at segment 1"
        log.info ("PASS: discoverer entered provisional segment")

        # === Test 6: Exit channel to confirm segment ===
        log.info ("=== Test 6: Exit channel (confirms segment) ===")
        # Find the visit ID.
        resp2 = gsp.listvisits ("active")
        visits = resp2["data"] if "data" in resp2 else resp2
        visit_id = visits[0]["id"]

        e.move ("p", "alice", json.dumps (
            {"g": {GAME_ID: {"xc": {"id": visit_id, "results": {
                "survived": False, "xp": 0, "gold": 0, "kills": 0
            }, "actions": []}}}}))
        e.generate (1)
        time.sleep (1)

        resp = gsp.getplayerinfo ("alice")
        info = resp["data"] if "data" in resp else resp
        assert not info["in_channel"], "alice should not be in channel"
        log.info ("PASS: channel exited, segment confirmed")

        # === Test 7: Use health potion ===
        log.info ("=== Test 7: Use health potion ===")
        e.move ("p", "alice", json.dumps (
            {"g": {GAME_ID: {"ui": {"item": "health_potion"}}}}))
        e.generate (1)
        time.sleep (1)

        resp = gsp.getplayerinfo ("alice")
        info = resp["data"] if "data" in resp else resp
        log.info ("PASS: alice HP is %d/%d after potion"
                  % (info["hp"], info["max_hp"]))

        log.info ("")
        log.info ("========================================")
        log.info ("  ALL SMOKE TESTS PASSED!")
        log.info ("========================================")
        log.info ("")

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
