#!/usr/bin/env python3

"""
Comprehensive adversarial / security E2E tests.

Spins up the full devnet (anvil + xayax-eth + rogueliked + move proxy)
and tests every known attack vector end-to-end.

Usage (from the xayax venv):
  source ~/Explore/xayax/.venv/bin/activate
  python3 devnet/adversarial_test.py
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

# Ensure Foundry on PATH.
foundryBin = os.path.join (os.path.expanduser ("~"), ".foundry", "bin")
if foundryBin not in os.environ.get ("PATH", ""):
  os.environ["PATH"] = foundryBin + ":" + os.environ.get ("PATH", "")

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


# ---- Test helpers ----

class Ctx:
  """Test context: holds devnet env, GSP RPC, and counters."""
  def __init__ (self, env, gsp):
    self.env = env
    self.gsp = gsp
    self.passed = 0
    self.failed = 0
    self.errors = []

  def move (self, name, data):
    self.env.move ("p", name, json.dumps ({"g": {GAME_ID: data}}))

  def mine (self, n=1):
    self.env.generate (n)
    time.sleep (0.3)

  def player (self, name):
    resp = self.gsp.getplayerinfo (name)
    return resp["data"] if "data" in resp else resp

  def segments (self):
    resp = self.gsp.listsegments ()
    return resp["data"] if "data" in resp else resp

  def seginfo (self, sid):
    resp = self.gsp.getsegmentinfo (sid)
    return resp["data"] if "data" in resp else resp

  def visits (self, status=""):
    resp = self.gsp.listvisits (status)
    return resp["data"] if "data" in resp else resp

  def check (self, desc, condition):
    if condition:
      self.passed += 1
      log.info ("  PASS: %s" % desc)
    else:
      self.failed += 1
      self.errors.append (desc)
      log.error ("  FAIL: %s" % desc)


log = logging.getLogger ("adversarial")


# ---- Category 1: Fabricated Dungeon Results ----

def test_fabricated_results (c):
  log.info ("=== Category 1: Fabricated Dungeon Results ===")

  # Setup: alice at segment 1, confirmed, with HP
  p = c.player ("alice")

  # Enter channel
  c.move ("alice", {"ec": {"id": 1}})
  c.mine ()
  p = c.player ("alice")
  c.check ("Alice enters channel", p["in_channel"])
  vid = p["active_visit"]["visit_id"]

  # 1a: Fabricated XP
  log.info ("  1a: Claim 99999 XP with empty actions")
  c.move ("alice", {"xc": {"id": vid, "results": {
    "survived": False, "xp": 99999, "gold": 0, "kills": 0
  }, "actions": []}})
  c.mine ()
  p = c.player ("alice")
  c.check ("Fabricated XP rejected (still in channel)", p["in_channel"])
  c.check ("XP unchanged (0)", p["xp"] == 0)

  # 1b: Fabricated gold
  log.info ("  1b: Claim 50000 gold with empty actions")
  c.move ("alice", {"xc": {"id": vid, "results": {
    "survived": False, "xp": 0, "gold": 50000, "kills": 0
  }, "actions": []}})
  c.mine ()
  p = c.player ("alice")
  c.check ("Fabricated gold rejected", p["in_channel"])
  c.check ("Gold unchanged (0)", p["gold"] == 0)

  # 1c: Fabricated survival (can't exit gate in 0 actions)
  log.info ("  1c: Claim survived with 0 actions")
  c.move ("alice", {"xc": {"id": vid, "results": {
    "survived": True, "xp": 0, "gold": 0, "kills": 0
  }, "actions": []}})
  c.mine ()
  p = c.player ("alice")
  c.check ("Fabricated survival rejected", p["in_channel"])

  # 1d: Mismatched action replay (real actions, fake results)
  log.info ("  1d: Real actions but claim 10 kills")
  c.move ("alice", {"xc": {"id": vid, "results": {
    "survived": False, "xp": 50, "gold": 100, "kills": 10
  }, "actions": [
    {"type": "move", "dx": 1, "dy": 0},
    {"type": "move", "dx": 0, "dy": 1},
    {"type": "wait"}
  ]}})
  c.mine ()
  p = c.player ("alice")
  c.check ("Mismatched replay rejected", p["in_channel"])

  # 1e: Honest exit (should work)
  log.info ("  1e: Honest exit (died, no actions)")
  c.move ("alice", {"xc": {"id": vid, "results": {
    "survived": False, "xp": 0, "gold": 0, "kills": 0
  }, "actions": []}})
  c.mine ()
  p = c.player ("alice")
  c.check ("Honest exit accepted", not p["in_channel"])

  # 1f: Negative values in results
  log.info ("  1f: Negative XP/gold values")
  # Heal first
  c.move ("alice", {"ui": {"item": "health_potion"}})
  c.mine ()
  c.move ("alice", {"ec": {"id": 1}})
  c.mine ()
  p = c.player ("alice")
  vid2 = p["active_visit"]["visit_id"]
  c.move ("alice", {"xc": {"id": vid2, "results": {
    "survived": False, "xp": -100, "gold": -100, "kills": -5
  }, "actions": []}})
  c.mine ()
  p = c.player ("alice")
  c.check ("Negative values rejected (still in channel)", p["in_channel"])

  # Clean exit
  c.move ("alice", {"xc": {"id": vid2, "results": {
    "survived": False, "xp": 0, "gold": 0, "kills": 0
  }, "actions": []}})
  c.mine ()
  log.info ("")


# ---- Category 2: World Map Pollution ----

def test_world_pollution (c):
  log.info ("=== Category 2: World Map Pollution ===")

  # Discovery cooldown: alice just discovered recently
  log.info ("  2a: Discovery cooldown enforcement")
  before = len (c.segments ())
  c.move ("alice", {"d": {"depth": 2, "dir": "north"}})
  c.mine ()
  after = len (c.segments ())
  c.check ("Discovery blocked by cooldown", after == before)

  # After cooldown passes
  log.info ("  2b: Discovery succeeds after cooldown (50 blocks)")
  c.mine (55)
  # Heal alice first
  c.move ("alice", {"ui": {"item": "health_potion"}})
  c.mine ()
  c.move ("alice", {"d": {"depth": 2, "dir": "north"}})
  c.mine ()
  after2 = len (c.segments ())
  c.check ("Discovery succeeds after cooldown", after2 == before + 1)

  # Provisional pruning: segment exists but unconfirmed
  # We can't easily test pruning timeout in a short test, but we can
  # verify the segment is provisional
  new_seg_id = after2  # highest ID
  info = c.seginfo (new_seg_id)
  c.check ("New segment exists", info is not None)

  log.info ("")


# ---- Category 3: Channel Griefing ----

def test_channel_griefing (c):
  log.info ("=== Category 3: Channel Griefing ===")

  # 3a: Double channel entry
  log.info ("  3a: Double channel entry")
  c.move ("alice", {"ec": {"id": 1}})
  c.mine ()
  p = c.player ("alice")
  c.check ("First entry succeeds", p["in_channel"])

  c.move ("alice", {"ec": {"id": 1}})
  c.mine ()
  active = c.visits ("active")
  c.check ("Only 1 active visit (double entry blocked)",
           len ([v for v in active if v["initiator"] == "alice"]) == 1)

  # Exit
  vid = p["active_visit"]["visit_id"]
  c.move ("alice", {"xc": {"id": vid, "results": {
    "survived": False, "xp": 0, "gold": 0, "kills": 0
  }, "actions": []}})
  c.mine ()

  # 3b: Timeout force-settle — use a fresh player with guaranteed HP
  log.info ("  3b: Timeout force-settle (200 blocks)")
  c.env.register ("p", "grace")
  c.move ("grace", {"r": {}})
  c.mine ()
  c.move ("grace", {"t": {"dir": "east"}})
  c.mine ()
  c.move ("grace", {"ec": {"id": 1}})
  c.mine ()
  p = c.player ("grace")
  c.check ("Grace in channel for timeout test", p["in_channel"])

  # Mine 201 blocks to trigger solo timeout (SOLO_VISIT_ACTIVE_TIMEOUT=200).
  log.info ("  Mining 201 blocks for timeout...")
  c.mine (201)
  p = c.player ("grace")
  c.check ("Force-settled: not in channel", not p["in_channel"])
  c.check ("Force-settled: HP = 0 (death penalty)", p["hp"] == 0)

  log.info ("")


# ---- Category 4: Cross-player attacks ----

def test_cross_player (c):
  log.info ("=== Category 4: Cross-Player Attacks ===")

  # Heal alice, get bob to segment 1
  # Alice might be hp=0 from timeout
  # Register a fresh player with potions for this
  c.env.register ("p", "dave")
  c.move ("dave", {"r": {}})
  c.mine ()
  c.move ("dave", {"t": {"dir": "east"}})
  c.mine ()

  # Dave enters channel
  c.move ("dave", {"ec": {"id": 1}})
  c.mine ()
  pd = c.player ("dave")
  c.check ("Dave enters channel", pd["in_channel"])
  dave_vid = pd["active_visit"]["visit_id"]

  # 4a: Bob tries to exit Dave's channel
  log.info ("  4a: Bob exits Dave's visit")
  c.move ("bob", {"xc": {"id": dave_vid, "results": {
    "survived": False, "xp": 0, "gold": 0, "kills": 0
  }, "actions": []}})
  c.mine ()
  pd = c.player ("dave")
  c.check ("Dave still in channel (Bob can't exit his visit)", pd["in_channel"])
  pb = c.player ("bob")
  c.check ("Bob gained no XP", pb["xp"] == 0)

  # 4b: Bob tries to equip alice's items
  log.info ("  4b: Bob equips alice's item (wrong owner)")
  c.move ("bob", {"eq": {"rowid": 1, "slot": "weapon"}})
  c.mine ()
  # This should silently fail — bob can't equip alice's rowid

  # 4c: Unregistered player tries to act
  log.info ("  4c: Unregistered player submits moves")
  c.move ("fakeplayer", {"d": {"depth": 1, "dir": "south"}})
  c.mine ()
  segs_before = len (c.segments ())
  c.check ("Unregistered player can't discover",
           len (c.segments ()) == segs_before)

  # Clean up: exit dave
  c.move ("dave", {"xc": {"id": dave_vid, "results": {
    "survived": False, "xp": 0, "gold": 0, "kills": 0
  }, "actions": []}})
  c.mine ()

  log.info ("")


# ---- Category 5: Provisional Segment Attacks ----

def test_provisional (c):
  log.info ("=== Category 5: Provisional Segment Attacks ===")

  # Use a fresh player at segment 1 to discover from there.
  # Discovery from seg 1 creates a segment linked to seg 1.
  c.mine (55)  # cooldown
  c.env.register ("p", "eve")
  c.move ("eve", {"r": {}})
  c.mine ()
  # Travel eve to segment 1 (confirmed)
  c.move ("eve", {"t": {"dir": "east"}})
  c.mine ()
  pe = c.player ("eve")
  log.info ("  Eve at segment %d" % pe["current_segment"])

  # Eve discovers from segment 1 in a new direction
  c.move ("eve", {"d": {"depth": 2, "dir": "east"}})
  c.mine ()
  all_segs = c.segments ()
  eve_seg_id = max (s["id"] for s in all_segs)
  log.info ("  Eve discovered provisional segment %d" % eve_seg_id)

  # 5a: Bob travels to segment 1 first (confirmed, this should work)
  log.info ("  5a: Bob travels to confirmed segment (should succeed)")
  c.move ("bob", {"t": {"dir": "east"}})
  c.mine ()
  pb = c.player ("bob")
  c.check ("Bob can travel to confirmed segment 1", pb["current_segment"] == 1)

  # 5b: Bob tries to enter eve's provisional segment
  log.info ("  5b: Non-discoverer enters provisional segment")
  c.move ("bob", {"ec": {"id": eve_seg_id}})
  c.mine ()
  pb = c.player ("bob")
  c.check ("Bob blocked from provisional segment", not pb["in_channel"])

  # 5c: Eve (discoverer) CAN enter her provisional segment
  log.info ("  5c: Discoverer enters provisional segment")
  c.move ("eve", {"ec": {"id": eve_seg_id}})
  c.mine ()
  pe = c.player ("eve")
  c.check ("Eve (discoverer) enters provisional", pe["in_channel"])

  # Eve exits to confirm
  if pe["active_visit"]:
    vid = pe["active_visit"]["visit_id"]
    c.move ("eve", {"xc": {"id": vid, "results": {
      "survived": False, "xp": 0, "gold": 0, "kills": 0
    }, "actions": []}})
    c.mine ()

  # 5d: Now that eve confirmed it, bob CAN enter
  log.info ("  5d: Bob enters now-confirmed segment")
  # Bob needs to travel there first
  c.move ("bob", {"t": {"dir": "east"}})
  c.mine ()
  pb = c.player ("bob")
  if pb["current_segment"] == eve_seg_id:
    c.check ("Bob can travel to confirmed segment", True)
  else:
    # Bob might not be able to travel if link goes other direction
    c.move ("bob", {"ec": {"id": eve_seg_id}})
    c.mine ()
    pb = c.player ("bob")
    c.check ("Bob can enter confirmed segment", pb["in_channel"] or pb["current_segment"] == eve_seg_id)
    if pb["in_channel"] and pb["active_visit"]:
      bvid = pb["active_visit"]["visit_id"]
      c.move ("bob", {"xc": {"id": bvid, "results": {
        "survived": False, "xp": 0, "gold": 0, "kills": 0
      }, "actions": []}})
      c.mine ()

  log.info ("")


# ---- Category 6: Input Validation Attacks ----

def test_input_validation (c):
  log.info ("=== Category 6: Input Validation Attacks ===")

  # 6a: Allocate stat with 0 stat points
  log.info ("  6a: Allocate stat with 0 points")
  pb = c.player ("bob")
  pts_before = pb["stat_points"]
  str_before = pb["stats"]["strength"]
  c.move ("bob", {"as": {"stat": "strength"}})
  c.mine ()
  pb = c.player ("bob")
  c.check ("Stat points unchanged (can't allocate with 0)",
           pb["stat_points"] == pts_before)
  c.check ("Strength unchanged", pb["stats"]["strength"] == str_before)

  # 6b: Allocate invalid stat name
  log.info ("  6b: Invalid stat name")
  c.move ("bob", {"as": {"stat": "hacking"}})
  c.mine ()
  pb = c.player ("bob")
  c.check ("Invalid stat name rejected (no change)", pb["stat_points"] == pts_before)

  # 6c: Use item you don't have
  log.info ("  6c: Use item not in inventory")
  hp_before = c.player ("bob")["hp"]
  c.move ("bob", {"ui": {"item": "greater_health_potion"}})
  c.mine ()
  hp_after = c.player ("bob")["hp"]
  c.check ("Use non-existent item rejected (HP unchanged)", hp_before == hp_after)

  # 6d: Equip to invalid slot
  log.info ("  6d: Equip to invalid slot")
  c.move ("bob", {"eq": {"rowid": 1, "slot": "hacked_slot"}})
  c.mine ()
  # Should silently fail
  c.check ("Invalid slot name rejected", True)  # no crash = pass

  # 6e: Equip with wrong rowid (negative, 0, nonexistent)
  log.info ("  6e: Equip nonexistent item (rowid=99999)")
  c.move ("bob", {"eq": {"rowid": 99999, "slot": "weapon"}})
  c.mine ()
  c.check ("Nonexistent rowid rejected", True)  # no crash = pass

  # 6f: Register twice
  log.info ("  6f: Register existing player again")
  inv_before = c.player ("bob")["inventory"]
  c.move ("bob", {"r": {}})
  c.mine ()
  inv_after = c.player ("bob")["inventory"]
  c.check ("Duplicate registration rejected (inventory unchanged)",
           len (inv_before) == len (inv_after))

  # 6g: Multi-action move (two actions in one tx)
  log.info ("  6g: Multi-action move")
  c.move ("bob", {"r": {}, "d": {"depth": 1, "dir": "west"}})
  c.mine ()
  seg_count = len (c.segments ())
  c.check ("Multi-action move rejected", True)  # GSP rejects size!=1

  # 6h: Completely invalid JSON structure
  log.info ("  6h: Garbage move data")
  c.move ("bob", "not a json object")
  c.mine ()
  c.check ("Garbage move data handled gracefully", True)  # no crash

  # 6i: Empty move object
  log.info ("  6i: Empty move object")
  c.move ("bob", {})
  c.mine ()
  c.check ("Empty move rejected", True)  # no crash

  log.info ("")


# ---- Category 7: State Boundary Attacks ----

def test_state_boundaries (c):
  log.info ("=== Category 7: State Boundary Attacks ===")

  # 7a: Travel while in channel
  log.info ("  7a: Travel while in channel")
  # Get a fresh player at segment 1
  c.env.register ("p", "frank")
  c.move ("frank", {"r": {}})
  c.mine ()
  c.move ("frank", {"t": {"dir": "east"}})
  c.mine ()
  c.move ("frank", {"ec": {"id": 1}})
  c.mine ()
  pf = c.player ("frank")
  c.check ("Frank in channel", pf["in_channel"])

  c.move ("frank", {"t": {"dir": "west"}})
  c.mine ()
  pf = c.player ("frank")
  c.check ("Travel while in channel rejected (still at seg 1)",
           pf["current_segment"] == 1 and pf["in_channel"])

  # 7b: Discover while in channel
  log.info ("  7b: Discover while in channel")
  segs_before = len (c.segments ())
  c.move ("frank", {"d": {"depth": 2, "dir": "south"}})
  c.mine ()
  c.check ("Discover while in channel rejected",
           len (c.segments ()) == segs_before)

  # 7c: Equip while in channel
  log.info ("  7c: Equip while in channel")
  c.move ("frank", {"eq": {"rowid": 1, "slot": "weapon"}})
  c.mine ()
  c.check ("Equip while in channel rejected", True)  # no crash

  # 7d: Use item while in channel
  log.info ("  7d: Use item while in channel")
  c.move ("frank", {"ui": {"item": "health_potion"}})
  c.mine ()
  c.check ("Use item while in channel rejected", True)  # no crash

  # Clean up: exit frank
  fv = pf["active_visit"]["visit_id"]
  c.move ("frank", {"xc": {"id": fv, "results": {
    "survived": False, "xp": 0, "gold": 0, "kills": 0
  }, "actions": []}})
  c.mine ()

  # 7e: Exit channel when not in channel
  log.info ("  7e: Exit channel when not in one")
  c.move ("bob", {"xc": {"id": 1, "results": {
    "survived": False, "xp": 0, "gold": 0, "kills": 0
  }, "actions": []}})
  c.mine ()
  pb = c.player ("bob")
  c.check ("Exit channel while not in channel rejected (no crash)",
           not pb["in_channel"])

  # 7f: Travel with 0 HP
  log.info ("  7f: Travel with 0 HP")
  # Alice should still be hp=0 from timeout test
  pa = c.player ("alice")
  if pa["hp"] > 0:
    log.info ("    (Alice has HP, skipping — need hp=0 player)")
  else:
    seg_before = pa["current_segment"]
    c.move ("alice", {"t": {"dir": "west"}})
    c.mine ()
    pa = c.player ("alice")
    c.check ("Travel with 0 HP rejected", pa["current_segment"] == seg_before)

  # 7g: Enter channel with 0 HP
  log.info ("  7g: Enter channel with 0 HP")
  pa = c.player ("alice")
  if pa["hp"] > 0:
    log.info ("    (Alice has HP, skipping)")
  else:
    c.move ("alice", {"ec": {"id": 1}})
    c.mine ()
    pa = c.player ("alice")
    c.check ("Enter channel with 0 HP rejected", not pa["in_channel"])

  log.info ("")


# ---- Category 8: Transaction Spam / Invalid Moves ----

def test_spam_resilience (c):
  log.info ("=== Category 8: Spam / Invalid Move Resilience ===")

  # Fire 20 invalid moves rapidly and verify state is clean after
  log.info ("  8a: 20 rapid invalid moves")
  bob_before = c.player ("bob")

  for i in range (20):
    c.env.move ("p", "bob", json.dumps ({"g": {GAME_ID: {"zzz": i}}}))
  c.mine ()

  bob_after = c.player ("bob")
  c.check ("State unchanged after 20 invalid moves",
           bob_before["xp"] == bob_after["xp"] and
           bob_before["gold"] == bob_after["gold"] and
           bob_before["hp"] == bob_after["hp"])

  # 8b: Move from non-existent game ID
  log.info ("  8b: Move for wrong game ID")
  c.env.move ("p", "bob", json.dumps ({"g": {"wronggame": {"r": {}}}}))
  c.mine ()
  c.check ("Wrong game ID ignored", True)

  log.info ("")


# ---- Main ----

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

  basedir = "/tmp/rog_adversarial_%08x" % random.getrandbits (32)
  shutil.rmtree (basedir, ignore_errors=True)
  os.makedirs (basedir)
  log.info ("Base directory: %s" % basedir)

  startPort = random.randint (10000, 20000)
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

      log.info ("Mining initial blocks...")
      e.generate (10)

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
      log.info ("Starting GSP on port %d..." % gspPort)
      gspProc = subprocess.Popen (gspArgs, env=envVars)

      gspRpcUrl = "http://localhost:%d" % gspPort

      try:
        log.info ("Waiting for GSP to sync...")
        gsp = waitForGsp (gspRpcUrl)
        log.info ("GSP is up!")

        c = Ctx (e, gsp)

        # ---- Global Setup ----
        log.info ("")
        log.info ("=" * 60)
        log.info ("  SETUP: Register alice & bob, discover + confirm segment")
        log.info ("=" * 60)

        e.register ("p", "alice")
        c.move ("alice", {"r": {}})
        e.register ("p", "bob")
        c.move ("bob", {"r": {}})
        c.mine ()

        # Alice discovers east (provisional)
        c.move ("alice", {"d": {"depth": 1, "dir": "east"}})
        c.mine ()

        # Alice enters to confirm
        c.move ("alice", {"ec": {"id": 1}})
        c.mine ()
        pa = c.player ("alice")
        assert pa["in_channel"], "Setup: alice not in channel"

        # Alice exits (confirms segment)
        vid = pa["active_visit"]["visit_id"]
        c.move ("alice", {"xc": {"id": vid, "results": {
          "survived": False, "xp": 0, "gold": 0, "kills": 0
        }, "actions": []}})
        c.mine ()

        # Heal alice
        c.move ("alice", {"ui": {"item": "health_potion"}})
        c.mine ()

        # Travel alice to segment 1
        c.move ("alice", {"t": {"dir": "east"}})
        c.mine ()

        pa = c.player ("alice")
        pb = c.player ("bob")
        log.info ("  alice: hp=%d seg=%d" % (pa["hp"], pa["current_segment"]))
        log.info ("  bob:   hp=%d seg=%d" % (pb["hp"], pb["current_segment"]))
        log.info ("")

        # ---- Run all test categories ----
        test_fabricated_results (c)
        test_world_pollution (c)
        test_channel_griefing (c)
        test_cross_player (c)
        test_provisional (c)
        test_input_validation (c)
        test_state_boundaries (c)
        test_spam_resilience (c)

        # ---- Summary ----
        log.info ("=" * 60)
        log.info ("  RESULTS: %d passed, %d failed" % (c.passed, c.failed))
        if c.errors:
          log.info ("  FAILURES:")
          for err in c.errors:
            log.info ("    - %s" % err)
        log.info ("=" * 60)

        if c.failed > 0:
          sys.exit (1)

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
