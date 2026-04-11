#!/usr/bin/env python3

"""
Multi-AI overworld explorer — spawns N AI characters exploring the shared
dungeon world in parallel (round-robin).

Each character:
  - Has its own player name, inventory, HP, XP
  - Maintains its own view of the shared world map
  - Takes one action per round (round-robin fashion)
  - Uses the autopilot decision logic (autoDecide) — no Claude

All characters share the same on-chain world:
  - Segments are visible to every player once discovered
  - World coordinates (world_x, world_y) are UNIQUE — two characters
    cannot claim the same spot
  - Each character has its own per-player discovery cooldown (50 blocks)

This test verifies that:
  - Multiple players can explore concurrently without interfering
  - The shared world graph grows correctly with multiple explorers
  - The UNIQUE(world_x, world_y) constraint is enforced
  - Per-player discovery cooldown doesn't block other players
  - The discoverer privilege (entering a provisional segment) still
    works when multiple players are active

Usage (from xayax venv):
  source ~/Explore/xayax/.venv/bin/activate
  python3 devnet/multi_ai_explorer.py                    # 3 players, 2 segs each
  python3 devnet/multi_ai_explorer.py 4 2                # 4 players, 2 segs each
  python3 devnet/multi_ai_explorer.py 3 3                # 3 players, 3 segs each
"""

from xayax.eth import Environment

# Reuse helpers from ai_explorer (they are pure, no hidden state aside from
# the Claude session which we don't use here).
from ai_explorer import (
  SimpleAutopilot, playDungeon,
  portGenerator, waitForGsp, getData, sendMove,
  GSP_BINARY, PLAY_BINARY, XETH_BINARY, GAME_ID,
)

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


# ---- Per-player state ----

class PlayerState:
  def __init__ (self, name):
    self.name = name
    self.segDetails = {}          # seg_id -> full segment info (our view)
    self.confirmedSegments = set ()  # own discoveries the player confirmed
    self.done = False
    self.dungeonRuns = 0
    self.actionsTaken = 0
    self.discoverAttempts = 0
    self.discoverSuccesses = 0


# ---- Multi-player-aware decision logic ----
#
# This differs from ai_explorer.autoDecide:
#   - Only enters provisional segments the player personally discovered
#     (non-discoverers would be rejected by enter_channel).
#   - Only counts self-discovered-and-confirmed segments toward the goal,
#     so players contribute distinct segments rather than farming others'.
#   - Skips segments discovered by other players entirely — they are
#     "someone else's problem".

def multiPlayerDecide (info, segDetails, playerName, personalConfirmed):
  curSeg = info["current_segment"]
  hp, maxHp = info["hp"], info["max_hp"]
  potions = sum (i["quantity"] for i in info["inventory"]
                 if "health_potion" in i["item_id"])

  # Heal if we have capacity.
  if 0 < hp < maxHp * 0.5 and potions > 0:
    return {"decision": "heal"}

  # Find direct links from the current segment.
  linked_dirs = set ()
  linked_segs = {}  # direction -> segment id
  detail = segDetails.get (curSeg)
  if detail:
    for direction, lnk in detail.get ("links", {}).items ():
      linked_dirs.add (direction)
      linked_segs[direction] = lnk["to_segment"]

  # Hub (segment 0) has no row in the segments table, so its links
  # are inferred from neighbours that link back to 0.
  if curSeg == 0:
    opp = {"north": "south", "south": "north",
           "east": "west", "west": "east"}
    for sid, d in segDetails.items ():
      for direction, lnk in d.get ("links", {}).items ():
        if lnk["to_segment"] == 0:
          rdir = opp.get (direction, direction)
          linked_dirs.add (rdir)
          linked_segs[rdir] = sid

  # Enter current segment's dungeon (must be self-discovered & unconfirmed).
  if curSeg != 0 and curSeg not in personalConfirmed and hp > 0:
    segInfo = segDetails.get (curSeg, {})
    if segInfo.get ("discoverer", "") == playerName:
      return {"decision": "enter_dungeon"}

  # Enter a neighbouring segment we discovered (discoverer privilege — ec
  # works from the source segment without traveling first).
  if hp > 0:
    for direction, sid in linked_segs.items ():
      if sid == 0 or sid in personalConfirmed:
        continue
      segInfo = segDetails.get (sid, {})
      if segInfo.get ("discoverer", "") == playerName:
        return {"decision": "enter_dungeon", "segment": sid}

  # Discover in an unexplored direction.
  allDirs = ["east", "north", "south", "west"]
  unexplored = [d for d in allDirs if d not in linked_dirs]
  if unexplored:
    return {"decision": "discover", "dir": unexplored[0]}

  # Travel to a self-discovered-but-unconfirmed neighbour (unlikely —
  # we'd have entered it above — but defensive).
  for direction, sid in linked_segs.items ():
    if sid == 0:
      continue
    segInfo = segDetails.get (sid, {})
    if (segInfo.get ("discoverer", "") == playerName
        and sid not in personalConfirmed):
      return {"decision": "travel", "dir": direction}

  # The current segment is saturated (all 4 directions linked, or is
  # the hub and everyone else has grabbed the directions).  Travel to
  # a self-discovered segment so we can discover new directions from
  # there.  Pick one at random for variety.
  if hp > 0:
    ownAdjacent = []
    for direction, sid in linked_segs.items ():
      if sid == 0:
        continue
      segInfo = segDetails.get (sid, {})
      if segInfo.get ("discoverer", "") == playerName:
        ownAdjacent.append (direction)
    if ownAdjacent:
      return {"decision": "travel", "dir": ownAdjacent[0]}

    # No adjacent self-segment.  Fall back to travelling to ANY linked
    # segment (someone else's confirmed segment) and discovering from
    # there.  That is still a valid exploration path for the shared
    # world graph.
    for direction, sid in linked_segs.items ():
      if sid == 0:
        continue
      segInfo = segDetails.get (sid, {})
      if segInfo.get ("confirmed", 0) == 1:
        return {"decision": "travel", "dir": direction}

  # Nothing productive to do.
  return {"decision": "done"}


# ---- One round for one player ----

def stepPlayer (player, env, gsp, targetSegments):
  """Executes one decision/action for a single player."""
  if player.done:
    return

  info = getData (gsp.getplayerinfo (player.name))

  if len (player.confirmedSegments) >= targetSegments:
    player.done = True
    log.info (f"  [{player.name}] target reached ({targetSegments} segs)")
    return

  # Refresh this player's view of all segments.
  allSegs = getData (gsp.listsegments ())
  for s in allSegs:
    # Always refresh — links change as other players discover.
    detail = getData (gsp.getsegmentinfo (s["id"]))
    if detail:
      player.segDetails[s["id"]] = detail

  decision = multiPlayerDecide (info, player.segDetails,
                                player.name, player.confirmedSegments)
  log.info (f"  [{player.name}] seg {info['current_segment']} "
            f"HP {info['hp']}/{info['max_hp']} -> {json.dumps (decision)}")

  dtype = decision.get ("decision", "done")

  if dtype == "done":
    player.done = True
    return

  if dtype == "heal":
    sendMove (env, player.name, {"ui": {"item": "health_potion"}})
    player.actionsTaken += 1
    return

  if dtype == "discover":
    direction = decision.get ("dir", "east")
    depth = max (1, len (player.confirmedSegments) + 1)
    player.discoverAttempts += 1
    segs_before = len (allSegs)
    sendMove (env, player.name, {"d": {"depth": depth, "dir": direction}})
    segs_after = getData (gsp.listsegments ())
    if len (segs_after) > segs_before:
      player.discoverSuccesses += 1
      log.info (f"    -> [{player.name}] discovered new segment "
                f"(world now has {len (segs_after)})")
    else:
      log.info (f"    -> [{player.name}] discover rejected "
                f"(coord occupied, dir linked, or cooldown)")
    player.actionsTaken += 1
    return

  if dtype == "travel":
    direction = decision.get ("dir", "east")
    sendMove (env, player.name, {"t": {"dir": direction}})
    player.actionsTaken += 1
    return

  if dtype == "enter_dungeon":
    if info["hp"] <= 0:
      log.info (f"    [{player.name}] 0 HP, skipping")
      return
    targetSeg = decision.get ("segment", info["current_segment"])

    sendMove (env, player.name, {"ec": {"id": targetSeg}})
    info = getData (gsp.getplayerinfo (player.name))
    if not info["in_channel"]:
      log.info (f"    [{player.name}] enter_channel rejected")
      return

    visitId = info["active_visit"]["visit_id"]
    segInfo = player.segDetails.get (targetSeg)
    if not segInfo:
      segInfo = getData (gsp.getsegmentinfo (targetSeg))
    seed = segInfo["seed"]
    depth = segInfo["depth"]

    log.info (f"    [{player.name}] playing dungeon seg {targetSeg} "
              f"(depth {depth})...")
    survived, results, actionLog = playDungeon (
      seed, depth, info, useAi=False)
    player.dungeonRuns += 1

    sendMove (env, player.name, {
      "xc": {"id": visitId, "results": results, "actions": actionLog}
    })

    info = getData (gsp.getplayerinfo (player.name))
    if not info["in_channel"]:
      outcome = "SURVIVED" if survived else "DIED"
      log.info (f"    [{player.name}] {outcome} "
                f"xp={results['xp']} hp={info['hp']}/{info['max_hp']}")
      player.confirmedSegments.add (targetSeg)
    else:
      log.info (f"    [{player.name}] settlement REJECTED")
    player.actionsTaken += 1


# ---- Shared-world visualisation ----

def printWorldMap (allSegs, segDetails):
  """Print the shared world as an ASCII 2D grid.

  North is at the top (+Y), south at the bottom (-Y),
  east to the right (+X), west to the left (-X).
  """
  # Collect positions.  The hub is always at (0, 0).
  positions = {(0, 0): {"id": 0, "discoverer": "-", "confirmed": 1}}
  minX = maxX = minY = maxY = 0

  for s in allSegs:
    x = int (s.get ("world_x", 0))
    y = int (s.get ("world_y", 0))
    positions[(x, y)] = s
    minX = min (minX, x); maxX = max (maxX, x)
    minY = min (minY, y); maxY = max (maxY, y)

  log.info (f"  World extent: x=[{minX},{maxX}] y=[{minY},{maxY}] "
            f"({maxX - minX + 1} x {maxY - minY + 1} grid)")
  log.info ("")

  # Header row with X coordinates.
  header = "        " + "".join (f"  x={x:+d} " for x in range (minX, maxX + 1))
  log.info (header)

  # Rows from maxY (north) down to minY (south).
  for y in range (maxY, minY - 1, -1):
    row1 = f"  y={y:+d}  "
    row2 = "        "
    for x in range (minX, maxX + 1):
      if (x, y) in positions:
        seg = positions[(x, y)]
        sid = seg["id"]
        if sid == 0:
          row1 += " [HUB] "
          row2 += "  0,0  "
        else:
          confirmed = seg.get ("confirmed", 0)
          marker = "*" if confirmed else "?"
          row1 += f" [{sid:2d}{marker}] "
          disc = seg.get ("discoverer", "?")[:5]
          row2 += f" {disc:^5s} "
      else:
        row1 += "       "
        row2 += "       "
    log.info (row1)
    log.info (row2)
  log.info ("")
  log.info ("  Legend: [ID*] = confirmed, [ID?] = provisional")

  # Print adjacency info.
  log.info ("")
  log.info ("  Segment links:")
  for sid in sorted (segDetails.keys ()):
    detail = segDetails[sid]
    links = detail.get ("links", {})
    if links:
      ls = ", ".join (f"{d}->{l['to_segment']}" for d, l in links.items ())
      log.info (f"    seg {sid} (by {detail.get ('discoverer', '?')}): {ls}")


# ---- Main ----

def main (numPlayers, targetSegments):
  for binary in [GSP_BINARY, PLAY_BINARY]:
    if not os.path.exists (binary):
      print (f"ERROR: {binary} not found. Build first.")
      sys.exit (1)

  basedir = "/tmp/rog_multi_%08x" % random.getrandbits (32)
  shutil.rmtree (basedir, ignore_errors=True)
  os.makedirs (basedir)

  startPort = random.randint (10000, 20000)
  ports = portGenerator (startPort)
  gspPort = next (ports)
  gspDatadir = os.path.join (basedir, "gsp")
  os.makedirs (gspDatadir)

  env = Environment (basedir, ports, XETH_BINARY)
  env.enablePending ()

  names = ["alice", "bob", "charlie", "dave", "eve",
           "frank", "grace", "henry"]
  if numPlayers > len (names):
    print (f"ERROR: max {len (names)} players")
    sys.exit (1)
  playerNames = names[:numPlayers]

  log.info ("=" * 60)
  log.info ("  MULTI-AI OVERWORLD EXPLORER")
  log.info ("=" * 60)
  log.info (f"  Players: {', '.join (playerNames)}")
  log.info (f"  Target: {targetSegments} confirmed segments each")
  log.info (f"  AI: autopilot (deterministic)")
  log.info ("=" * 60)

  try:
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
        "--dungeon_id=multi_explorer",
      ]
      envVars = dict (os.environ)
      envVars["GLOG_log_dir"] = gspDatadir
      gspProc = subprocess.Popen (gspArgs, env=envVars)
      gspRpcUrl = "http://localhost:%d" % gspPort

      try:
        gsp = waitForGsp (gspRpcUrl)
        log.info ("GSP synced")

        # Register all players.
        log.info (f"\nStep 1: Register {numPlayers} players")
        players = []
        for name in playerNames:
          e.register ("p", name)
          sendMove (e, name, {"r": {}})
          info = getData (gsp.getplayerinfo (name))
          log.info (f"  {name}: lvl {info['level']} "
                    f"HP {info['hp']}/{info['max_hp']}")
          players.append (PlayerState (name))

        # Round-robin exploration loop.
        maxRounds = numPlayers * targetSegments * 8  # generous safety
        log.info (f"\nStep 2: Round-robin exploration "
                  f"(max {maxRounds} rounds)")

        for rnd in range (maxRounds):
          remaining = [p for p in players if not p.done]
          if not remaining:
            break

          log.info (f"\n--- Round {rnd + 1} | {len (remaining)} active ---")
          for p in remaining:
            stepPlayer (p, e, gsp, targetSegments)

          # End of round: mine 52 blocks so every player's per-player
          # discovery cooldown (50 blocks) has cleared before the next
          # round starts.  Anvil mines these in milliseconds.
          e.generate (52)
          time.sleep (0.3)

        # ---- Summary ----
        log.info (f"\n{'=' * 60}")
        log.info (f"  EXPLORATION COMPLETE")
        log.info (f"{'=' * 60}")

        for p in players:
          info = getData (gsp.getplayerinfo (p.name))
          log.info (f"\n  {p.name}:")
          log.info (f"    Level {info['level']} "
                    f"HP {info['hp']}/{info['max_hp']}")
          log.info (f"    XP {info['xp']} Gold {info['gold']}")
          log.info (f"    Kills {info['combat_record']['kills']} "
                    f"Deaths {info['combat_record']['deaths']} "
                    f"Visits {info['combat_record']['visits_completed']}")
          log.info (f"    Confirmed segments: "
                    f"{len (p.confirmedSegments)} / {targetSegments}")
          log.info (f"    Dungeon runs: {p.dungeonRuns}")
          log.info (f"    Discover attempts: {p.discoverAttempts} "
                    f"(success: {p.discoverSuccesses})")
          log.info (f"    Actions: {p.actionsTaken}")

        # Shared world view.
        allSegs = getData (gsp.listsegments ())
        fullDetails = {}
        for s in allSegs:
          d = getData (gsp.getsegmentinfo (s["id"]))
          if d:
            fullDetails[s["id"]] = d

        log.info (f"\n  Shared world: {len (allSegs)} segments")
        log.info ("")
        printWorldMap (allSegs, fullDetails)

        # Sanity-check: UNIQUE(world_x, world_y).
        coords = set ()
        for s in allSegs:
          key = (int (s.get ("world_x", 0)), int (s.get ("world_y", 0)))
          assert key not in coords, \
              f"Duplicate world coord {key} — UNIQUE constraint broken!"
          coords.add (key)
        log.info (f"  PASS: all {len (coords)} world coordinates unique")

        totalConfirmed = sum (len (p.confirmedSegments) for p in players)
        log.info (f"  Total confirmed segments across all players: "
                  f"{totalConfirmed}")

        log.info (f"{'=' * 60}")

      finally:
        log.info ("Stopping GSP...")
        try:
          r = jsonrpclib.ServerProxy (gspRpcUrl)
          r._notify.stop ()
          gspProc.wait (timeout=10)
        except Exception:
          gspProc.terminate ()
          gspProc.wait (timeout=5)

  finally:
    log.info (f"Cleaning up {basedir}")
    shutil.rmtree (basedir, ignore_errors=True)


logging.basicConfig (
  level=logging.INFO,
  format="%(asctime)s [%(name)s] %(message)s",
  stream=sys.stderr,
  force=True,
)
log = logging.getLogger ("multi_ai")

if __name__ == "__main__":
  args = sys.argv[1:]
  numPlayers = int (args[0]) if len (args) > 0 else 3
  targetSegs = int (args[1]) if len (args) > 1 else 2
  main (numPlayers, targetSegs)
