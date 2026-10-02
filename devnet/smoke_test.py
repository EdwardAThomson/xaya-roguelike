#!/usr/bin/env python3

"""
Smoke test: starts the full local stack (anvil + xayax-eth + rogueliked)
and verifies basic game operations work end-to-end, finishing with a
staked duel fought to a result and settled on chain.

Usage (from the xayax venv):
  source ~/Explore/xayax/.venv/bin/activate
  python3 devnet/smoke_test.py
"""

from xayax.eth import Environment
from xayagametest import testcase

import duel

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
PLAY_BINARY = os.path.join (PROJECT_DIR, "build", "roguelike-play")
XETH_BINARY = "/usr/local/bin/xayax-eth"

GAME_ID = "rog"


def unwrap (resp):
  """GSP RPC responses are sometimes wrapped in a {data: ...} envelope."""
  return resp["data"] if isinstance (resp, dict) and "data" in resp else resp


def solveRun (gsp, name, seg_x, seg_y):
  """Generates a winning action proof for `name`'s active run on the
  segment at (`seg_x`, `seg_y`), driving the deterministic dungeon AI (roguelike-play
  --solve) with the exact effective stats / hp / potions / layout the GSP
  replays with.  Returns the parsed {survived, xp, gold, kills, actions}.
  A surviving run (reaching a gate) is the only way to confirm a
  provisional segment."""
  seg = unwrap (gsp.getsegmentinfo (seg_x, seg_y))
  p = unwrap (gsp.getplayerinfo (name))
  eff = p["effective_stats"]
  potions = sum (it["quantity"] for it in p["inventory"]
                 if it["item_id"] == "health_potion")
  constraints = []
  cdir = seg.get ("constraint_dir", "")
  if cdir and cdir in seg.get ("gates", {}):
    g = seg["gates"][cdir]
    constraints.append ({"x": g["x"], "y": g["y"], "direction": cdir})
  spec = {
    "seed": seg["seed"], "depth": seg["depth"],
    "hp": p["hp"], "max_hp": p["max_hp"],
    "stats": {
      "level": p["level"],
      "strength": eff["strength"], "dexterity": eff["dexterity"],
      "constitution": eff["constitution"], "intelligence": eff["intelligence"],
      "equip_attack": eff["equip_attack"], "equip_defense": eff["equip_defense"],
    },
    "potions": potions, "entry_direction": "", "constraints": constraints,
  }
  out = subprocess.run ([PLAY_BINARY, "--solve", json.dumps (spec)],
                        capture_output=True, text=True)
  return json.loads (out.stdout.strip ().splitlines ()[-1])


def stakedDuel (e, gsp, log):
  """Hosts a duel with a gold and item stake, joins it, fights it to a
  result and settles it, then checks the payout: the winner banked the pot
  and both staked items, the loser took the ordinary death outcome on what
  the stake left them (SPEC_multiplayer_pvp.md sections 5, 5a and 7).

  Both duellists walk in from the hub through the west gate of the
  confirmed segment (1, 0), so they spawn next to each other and the fight
  starts at once."""
  arena = {"x": 1, "y": 0}

  def move (name, data):
    e.move ("p", name, json.dumps ({"g": {GAME_ID: data}}))

  def mine ():
    e.generate (1)
    time.sleep (1)

  def player (name):
    return unwrap (gsp.getplayerinfo (name))

  def gold (name):
    return player (name)["gold"]

  log.info ("=== Test 8: Staked duel ===")
  duel.SelfCheck ()

  e.register ("p", "bob")
  move ("bob", {"r": {}})
  mine ()

  # The solver may have walked alice out of (1, 0) through any gate; bring
  # her back to the hub, the side both duellists enter from.
  if player ("alice")["segment"] != {"x": 0, "y": 0}:
    move ("alice", {"gw": {"dir": "west"}})
    mine ()
  assert player ("alice")["segment"] == {"x": 0, "y": 0}, \
      "alice could not get back to the hub"

  # A new character has no gold, so each duellist plays one run on the
  # arena collecting coins before staking any.
  for name in ("alice", "bob"):
    earned = duel.FarmGold (gsp, PLAY_BINARY, move, mine, name, arena, "west")
    log.info ("  %s earned %d gold, holds %d" % (name, earned, gold (name)))

  # The item stake: each takes off their starting leather armour (equipped
  # gear cannot be staked) and puts it up.
  armour = {}
  for name in ("alice", "bob"):
    rowid = next (it["rowid"] for it in player (name)["inventory"]
                  if it["item_id"] == "leather_armor")
    move (name, {"uq": {"rowid": rowid}})
    armour[name] = rowid
  mine ()
  for name in ("alice", "bob"):
    row = next (it for it in player (name)["inventory"]
                if it["rowid"] == armour[name])
    assert row["slot"] == "bag", "%s's armour was not unequipped" % name

  # Half of each one's gold goes into the pot.  The stakes need not match:
  # the host's floor is just the armour's worth, which bob's armour meets.
  stakeA = gold ("alice") // 2
  stakeB = gold ("bob") // 2
  if stakeA + stakeB == 0:
    log.warning ("  neither gold run found any coins; the duel stakes items"
                 " only and the gold half of the payout is not exercised")

  goldA0, goldB0 = gold ("alice"), gold ("bob")
  move ("alice", {"v": {"dir": "east", "mode": "duel", "stake": stakeA,
                        "min_stake": 25, "stake_items": [armour["alice"]]}})
  mine ()
  hosted = [v for v in unwrap (gsp.listvisits ("open"))
            if v["initiator"] == "alice"]
  assert len (hosted) == 1, "alice's duel did not open: %s" % hosted
  vid = hosted[0]["id"]
  assert gold ("alice") == goldA0 - stakeA, "alice's stake was not escrowed"

  move ("bob", {"j": {"id": vid, "dir": "east", "stake": stakeB,
                      "stake_items": [armour["bob"]]}})
  mine ()
  v = unwrap (gsp.getvisitinfo (vid))
  assert v["status"] == "active", "bob's join did not start the duel: %s" % v
  assert v["mode"] == "duel"
  assert v["pot"] == stakeA + stakeB, \
      "pot is %d, expected %d" % (v["pot"], stakeA + stakeB)
  assert sorted (it["item_id"] for it in v["staked_items"]) \
      == ["leather_armor", "leather_armor"], v["staked_items"]
  assert gold ("bob") == goldB0 - stakeB, "bob's stake was not escrowed"
  log.info ("PASS: duel %d active, pot %d gold + 2 leather armour"
            % (vid, v["pot"]))

  # Fight it out with the real engine and the real round protocol.
  spec, names = duel.DuelSpec (gsp, vid, "smoke-%d" % vid)
  fight = duel.Fight (PLAY_BINARY, spec)
  assert fight["decided"], "the duel did not reach a result: %s" % {
      k: fight[k] for k in ("winner", "rounds")}
  assert duel.SettleLogHash (vid, fight["actions"]) == fight["settle_hash"]
  winner = names[fight["winner"]]
  loser = names[1 - fight["winner"]]
  log.info ("  %s beat %s in %d rounds (%d log entries)"
            % (winner, loser, fight["rounds"], len (fight["actions"])))

  pot = v["pot"]
  winnerBefore = player (winner)
  loserBefore = player (loser)
  claims = duel.Claims (fight, names, pot, loserBefore["level"])

  # The loser consents to the log, then the winner settles it.
  move (loser, {"sc": {"id": vid, "h": fight["settle_hash"],
                       "n": len (fight["actions"])}})
  mine ()
  move (winner, {"s": {"id": vid, "results": claims,
                       "actions": fight["actions"]}})
  mine ()

  v = unwrap (gsp.getvisitinfo (vid))
  assert v["status"] == "completed", "the duel did not settle: %s" % v
  assert v["pot"] == 0, "the pot was not paid out"
  results = {r["name"]: r for r in v["results"]}
  winClaim = next (c for c in claims if c["p"] == winner)
  assert results[winner]["survived"] and not results[loser]["survived"]
  assert results[winner]["gold_gained"] == winClaim["gold"]
  log.info ("PASS: duel settled, %s banked as the winner" % winner)

  w = player (winner)
  l = player (loser)
  assert w["gold"] == winnerBefore["gold"] + winClaim["gold"], \
      "winner holds %d gold, expected %d" % (
          w["gold"], winnerBefore["gold"] + winClaim["gold"])
  assert w["combat_record"]["deaths"] \
      == winnerBefore["combat_record"]["deaths"]
  log.info ("PASS: %s took the %d-gold pot (holds %d)"
            % (winner, pot, w["gold"]))

  # The staked rows move as they are: the winner now owns both.
  winnerRows = {it["rowid"] for it in w["inventory"]}
  loserRows = {it["rowid"] for it in l["inventory"]}
  for name, rowid in armour.items ():
    assert rowid in winnerRows, "%s's staked armour did not go to %s" % (
        name, winner)
    assert rowid not in loserRows
  log.info ("PASS: %s won both staked leather armours" % winner)

  # The loser died the ordinary death on what the stake left them: one
  # more death, a quarter of the remaining gold, back to the hub.
  lossClaim = next (c for c in claims if c["p"] == loser)
  assert l["combat_record"]["deaths"] \
      == loserBefore["combat_record"]["deaths"] + 1, "loser did not die"
  assert l["gold"] == (loserBefore["gold"] + lossClaim["gold"]) * 75 // 100, \
      "loser holds %d gold, expected the death tax on %d" % (
          l["gold"], loserBefore["gold"] + lossClaim["gold"])
  assert not l["in_channel"] and l["active_visit"] is None
  log.info ("PASS: %s took the death outcome (deaths %d, gold %d, hp %d/%d)"
            % (loser, l["combat_record"]["deaths"], l["gold"], l["hp"],
               l["max_hp"]))


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
        assert info["segment"] == {"x": 0, "y": 0}
        log.info ("PASS: alice has %d/%d HP at the hub (%d, %d)"
                  % (info["hp"], info["max_hp"],
                     info["segment"]["x"], info["segment"]["y"]))

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
            {"g": {GAME_ID: {"ec": {"x": 1, "y": 0}}}}))
        e.generate (1)
        time.sleep (1)

        resp = gsp.getplayerinfo ("alice")
        info = resp["data"] if "data" in resp else resp
        assert info["in_channel"], "alice should be in channel"
        assert info["segment"] == {"x": 1, "y": 0}, \
            "alice should be at segment (1, 0)"
        log.info ("PASS: discoverer entered provisional segment")

        # === Test 6: Exit channel with a winning run to confirm segment ===
        # A provisional segment is confirmed only by a surviving run that
        # reaches a gate (a failed/empty exit prunes it), so generate a
        # real winning proof and submit it.
        log.info ("=== Test 6: Exit channel (winning run confirms segment) ===")
        resp2 = gsp.listvisits ("active")
        visits = resp2["data"] if "data" in resp2 else resp2
        visit_id = visits[0]["id"]

        proof = solveRun (gsp, "alice", 1, 0)
        assert proof["survived"], "solver failed to win segment (1, 0)"
        e.move ("p", "alice", json.dumps (
            {"g": {GAME_ID: {"xc": {"id": visit_id, "results": {
                "survived": proof["survived"], "xp": proof["xp"],
                "gold": proof["gold"], "kills": proof["kills"]
            }, "actions": proof["actions"]}}}}))
        e.generate (1)
        time.sleep (1)

        info = unwrap (gsp.getplayerinfo ("alice"))
        assert not info["in_channel"], "alice should not be in channel"
        seg = unwrap (gsp.getsegmentinfo (1, 0))
        assert seg["confirmed"], "segment (1, 0) should be confirmed after a win"
        log.info ("PASS: channel exited via gate, segment (1, 0) confirmed")

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

        # === Test 8: A staked duel, fought out and settled ===
        stakedDuel (e, gsp, log)

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
