#!/usr/bin/env python3

"""
Probe the new GSP response fields that the frontend depends on
(after Pass 1/2/3 of the frontend parity work).

Verifies:
  1. getcurrentstate envelope has {gamestate, height, blockhash}
  2. getplayerinfo returns `last_discover_height`
  3. listsegments entries have `world_x`, `world_y`, `confirmed`
  4. getsegmentinfo returns `world_x`, `world_y`, `confirmed`, `discoverer`
  5. last_discover_height bumps when a discover succeeds

Run from the xayax venv:
  source ~/Explore/xayax/.venv/bin/activate
  python3 devnet/probe_frontend_fields.py
"""

from xayax.eth import Environment

import json
import jsonrpclib
import logging
import os
import random
import shutil
import subprocess
import sys
import time

PROJECT_DIR = os.path.dirname (os.path.dirname (os.path.abspath (__file__)))
GSP_BINARY = os.path.join (PROJECT_DIR, "build", "rogueliked")
XETH_BINARY = "/usr/local/bin/xayax-eth"
GAME_ID = "rog"

foundryBin = os.path.join (os.path.expanduser ("~"), ".foundry", "bin")
if foundryBin not in os.environ.get ("PATH", ""):
  os.environ["PATH"] = foundryBin + ":" + os.environ.get ("PATH", "")


def portGen (start):
  p = start
  while True:
    yield p
    p += 1


def waitForGsp (rpcurl, timeout=30):
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
  raise RuntimeError ("GSP not available within %ds" % timeout)


def main ():
  logging.basicConfig (level=logging.INFO, format="%(message)s")
  log = logging.getLogger ("probe")

  basedir = "/tmp/rog_probe_%08x" % random.getrandbits (32)
  shutil.rmtree (basedir, ignore_errors=True)
  os.makedirs (basedir)

  try:
    ports = portGen (random.randint (10000, 30000))
    gspPort = next (ports)
    gspDatadir = os.path.join (basedir, "gsp")
    os.makedirs (gspDatadir)

    env = Environment (basedir, ports, XETH_BINARY)
    env.enablePending ()

    with env.run () as e:
      e.generate (10)

      gspArgs = [
        GSP_BINARY,
        "--xaya_rpc_url=%s" % e.getXRpcUrl (),
        "--xaya_rpc_protocol=2",
        "--game_rpc_port=%d" % gspPort,
        "--datadir=%s" % gspDatadir,
        "--genesis_height=0", "--genesis_hash=",
        "--xaya_rpc_wait", "--pending_moves",
        "--dungeon_id=probe_test",
      ]
      envVars = dict (os.environ)
      envVars["GLOG_log_dir"] = gspDatadir
      gspProc = subprocess.Popen (gspArgs, env=envVars)
      gspUrl = "http://localhost:%d" % gspPort

      try:
        gsp = waitForGsp (gspUrl)
        log.info ("GSP synced")

        failures = 0

        # ---- Test 1: getcurrentstate envelope shape ----
        log.info ("\n=== Test 1: getcurrentstate envelope ===")
        envelope = gsp.getcurrentstate ()
        log.info ("Envelope keys: %s", sorted (envelope.keys ()))
        for key in ("gamestate", "height", "blockhash"):
          if key not in envelope:
            log.error ("  FAIL: envelope missing '%s'", key)
            failures += 1
          else:
            val = envelope[key]
            log.info ("  OK: %s = %s",
                      key,
                      val if isinstance (val, (int, str)) else "<obj>")

        # ---- Test 2: listsegments world_x/world_y/confirmed ----
        # Register a player and discover a segment so we have data to check.
        log.info ("\n=== Test 2: register + discover for field checks ===")
        e.register ("p", "alice")
        e.move ("p", "alice", json.dumps ({"g": {GAME_ID: {"r": {}}}}))
        e.generate (1)
        time.sleep (0.5)

        # Player info BEFORE discover.
        p = gsp.getplayerinfo ("alice")
        info = p["data"] if "data" in p else p
        log.info ("\n=== Test 3: getplayerinfo has last_discover_height ===")
        if "last_discover_height" not in info:
          log.error ("  FAIL: getplayerinfo missing 'last_discover_height'")
          failures += 1
        else:
          log.info ("  OK: last_discover_height = %d (before discover)",
                    info["last_discover_height"])
          if info["last_discover_height"] != 0:
            log.error ("  FAIL: expected 0 before discover, got %d",
                       info["last_discover_height"])
            failures += 1

        # Do a discover.
        e.move ("p", "alice", json.dumps (
            {"g": {GAME_ID: {"d": {"depth": 1, "dir": "east"}}}}))
        e.generate (1)
        time.sleep (0.5)

        # Player info AFTER discover — last_discover_height should bump.
        log.info ("\n=== Test 4: last_discover_height bumps on discover ===")
        p = gsp.getplayerinfo ("alice")
        info = p["data"] if "data" in p else p
        if info["last_discover_height"] <= 0:
          log.error ("  FAIL: last_discover_height still 0 after discover")
          failures += 1
        else:
          log.info ("  OK: last_discover_height = %d (non-zero after discover)",
                    info["last_discover_height"])

        # listsegments fields.
        log.info ("\n=== Test 5: listsegments has world_x/world_y/confirmed ===")
        ss = gsp.listsegments ()
        segs = ss["data"] if "data" in ss else ss
        if not segs:
          log.error ("  FAIL: no segments")
          failures += 1
        else:
          seg = segs[0]
          log.info ("  Segment keys: %s", sorted (seg.keys ()))
          for key in ("world_x", "world_y", "confirmed"):
            if key not in seg:
              log.error ("  FAIL: listsegments entry missing '%s'", key)
              failures += 1
            else:
              log.info ("  OK: seg[%s] = %s", key, seg[key])
          # After just a discover (no channel exit), segment should be
          # provisional (confirmed=False).
          if seg.get ("confirmed") is not False:
            log.error ("  FAIL: expected confirmed=False, got %s",
                       seg.get ("confirmed"))
            failures += 1
          if seg.get ("world_x") != 1 or seg.get ("world_y") != 0:
            log.error ("  FAIL: expected world_x=1, world_y=0 for east; got (%s, %s)",
                       seg.get ("world_x"), seg.get ("world_y"))
            failures += 1

        # getsegmentinfo fields.
        log.info ("\n=== Test 6: getsegmentinfo has world_x/world_y/confirmed ===")
        si = gsp.getsegmentinfo (1)
        info = si["data"] if "data" in si else si
        for key in ("world_x", "world_y", "confirmed", "discoverer"):
          if key not in info:
            log.error ("  FAIL: getsegmentinfo missing '%s'", key)
            failures += 1
          else:
            log.info ("  OK: %s = %s", key, info[key])

        # ---- Summary ----
        log.info ("\n%s", "=" * 60)
        if failures == 0:
          log.info ("  ALL FIELD PROBES PASSED")
        else:
          log.error ("  %d FAILURES", failures)
          sys.exit (1)
        log.info ("%s", "=" * 60)

      finally:
        try:
          r = jsonrpclib.ServerProxy (gspUrl)
          r._notify.stop ()
          gspProc.wait (timeout=10)
        except:
          gspProc.terminate ()
          gspProc.wait (timeout=5)

  finally:
    shutil.rmtree (basedir, ignore_errors=True)


if __name__ == "__main__":
  main ()
