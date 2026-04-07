#!/usr/bin/env python3

"""
AI overworld explorer — multi-segment traversal test.

Starts a full devnet stack, registers a player, then runs an exploration
loop: discover segments, travel through the overworld, enter dungeons,
play them via the C++ engine, settle results on-chain, and repeat.

Claude makes strategic decisions:
  - Which direction to discover or travel
  - Whether to enter a dungeon or keep exploring the overworld
  - Which gate to target inside dungeons
  - When to heal vs push forward

The autopilot handles dungeon tactics (BFS, auto-combat, auto-heal).

Usage:
  source ~/Explore/xayax/.venv/bin/activate
  python3 devnet/ai_explorer.py [player_name] [num_segments]
"""

from xayax.eth import Environment
from collections import deque

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
import uuid

PROJECT_DIR = os.path.dirname (os.path.dirname (os.path.abspath (__file__)))
GSP_BINARY = os.path.join (PROJECT_DIR, "build", "rogueliked")
PLAY_BINARY = os.path.join (PROJECT_DIR, "build", "roguelike-play")
XETH_BINARY = "/usr/local/bin/xayax-eth"
GAME_ID = "rog"

foundryBin = os.path.join (os.path.expanduser ("~"), ".foundry", "bin")
if foundryBin not in os.environ.get ("PATH", ""):
  os.environ["PATH"] = foundryBin + ":" + os.environ.get ("PATH", "")


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
  raise RuntimeError ("GSP not available within %ds" % timeout)


def getData (resp):
  return resp["data"] if "data" in resp else resp


def sendMove (env, name, move):
  strval = json.dumps ({"g": {GAME_ID: move}})
  # Large moves (action replay proofs) need more gas than the default
  # 500K in xayax/eth.py. Estimate: ~16 gas per byte of calldata.
  gas = 500_000
  if len (strval) > 2000:
    gas = 1_500_000
  try:
    # Call the contract directly with custom gas limit.
    maxUint256 = 2**256 - 1
    zeroAddr = "0x" + "00" * 20
    env.contracts.registry.functions.move (
        "p", name, strval, maxUint256, 0, zeroAddr
    ).transact ({"from": env.contracts.account, "gas": gas})
  except Exception:
    # Fallback to default env.move if direct call fails.
    env.move ("p", name, strval)
  env.generate (1)
  time.sleep (0.3)


# ---- Dungeon Autopilot (reused from channel_daemon.py) ----

class SimpleAutopilot:
  def __init__ (self, gx, gy, gdir):
    self.gx, self.gy, self.gdir = gx, gy, gdir

  def get_action (self, state):
    px, py = state["player_x"], state["player_y"]
    hp, max_hp = state["hp"], state["max_hp"]

    if hp < max_hp * 0.4:
      for l in state.get ("loot", []):
        if "health_potion" in l["item"] and l["qty"] > 0:
          return {"action": "use", "item": l["item"]}

    for gi in state.get ("ground_items", []):
      if gi["x"] == px and gi["y"] == py:
        return {"action": "pickup"}

    if px == self.gx and py == self.gy:
      return {"action": "gate"}

    for m in state.get ("monsters", []):
      dx, dy = m["x"] - px, m["y"] - py
      if abs (dx) <= 1 and abs (dy) <= 1 and (dx or dy):
        return {"action": "move", "dx": dx, "dy": dy}

    step = self._bfs (state, px, py, self.gx, self.gy)
    if step:
      return {"action": "move", "dx": step[0], "dy": step[1]}
    return {"action": "wait"}

  def _bfs (self, state, fx, fy, tx, ty):
    if fx == tx and fy == ty:
      return (0, 0)
    rows = state.get ("map", "").strip ().split ("\n")
    ox, oy = state.get ("map_origin_x", 0), state.get ("map_origin_y", 0)
    def walkable (x, y):
      ry, rx = y - oy, x - ox
      if ry < 0 or ry >= len (rows) or rx < 0 or rx >= len (rows[ry]):
        return False
      return rows[ry][rx] != '#'
    queue = deque ([(fx, fy)])
    parent = {(fx, fy): None}
    dirs = [(-1,-1),(-1,0),(-1,1),(0,-1),(0,1),(1,-1),(1,0),(1,1)]
    while queue:
      cx, cy = queue.popleft ()
      if (cx, cy) == (tx, ty):
        cur = (tx, ty)
        while parent[cur] != (fx, fy):
          cur = parent[cur]
        return (cur[0] - fx, cur[1] - fy)
      for ddx, ddy in dirs:
        nx, ny = cx + ddx, cy + ddy
        if (nx, ny) not in parent and walkable (nx, ny):
          parent[(nx, ny)] = (cx, cy)
          queue.append ((nx, ny))
    return None


# ---- Claude Strategic Brain ----

SYSTEM_PROMPT = """You are an AI explorer of a blockchain roguelike. You make strategic decisions about overworld navigation and dungeon exploration.

You receive a JSON summary of the world state and must respond with ONLY a JSON decision.

## Overworld Decisions
When at a segment, you choose what to do next:
- Discover new segment: {"decision": "discover", "dir": "east"}
- Travel to adjacent segment: {"decision": "travel", "dir": "north"}
- Enter dungeon at current segment: {"decision": "enter_dungeon"}
- Use health potion: {"decision": "heal"}
- Done exploring: {"decision": "done"}

## Dungeon Decisions
- Choose gate: {"decision": "gate", "target": "south"}

## Strategy
- You want to explore outward, discovering new segments and completing dungeons
- Each segment must be discovered then confirmed (complete one dungeon run)
- After confirming a segment, you can discover new segments from it
- Watch your HP — heal before entering dungeons if below 50%
- Segments at higher depth have harder monsters but more XP
- Balance exploration with survival
- If you've visited enough segments, say done"""

_claude_session = None

def askClaude (prompt):
  global _claude_session
  args = ["claude", "-p", "--output-format", "json",
          "--model", "haiku", "--bare",
          "--append-system-prompt", SYSTEM_PROMPT]
  if _claude_session is None:
    _claude_session = str (uuid.uuid4 ())
    args.extend (["--session-id", _claude_session])
  else:
    args.extend (["--resume", _claude_session])
  try:
    result = subprocess.run (args, input=prompt, capture_output=True,
                              text=True, timeout=60)
  except subprocess.TimeoutExpired:
    return {"decision": "done"}

  try:
    resp = json.loads (result.stdout)
    text = resp.get ("result", "")
  except:
    text = result.stdout.strip ()

  text = text.strip ()
  if "```" in text:
    text = "\n".join (l for l in text.split ("\n")
                       if not l.startswith ("```")).strip ()
  try:
    return json.loads (text)
  except:
    start = text.find ("{")
    end = text.rfind ("}") + 1
    if start >= 0 and end > start:
      try:
        return json.loads (text[start:end])
      except:
        pass
  return {"decision": "done"}


def formatOverworldPrompt (info, segments, segDetails, gsp):
  """Build the overworld decision prompt."""
  seg = info["current_segment"]
  hp = info["hp"]
  maxHp = info["max_hp"]
  potions = sum (i["quantity"] for i in info["inventory"]
                 if "health_potion" in i["item_id"])

  s = f"=== OVERWORLD STATE ===\n"
  s += f"Player: {info['name']} | Level {info['level']}\n"
  s += f"HP: {hp}/{maxHp} | Potions: {potions}\n"
  s += f"XP: {info['xp']} | Gold: {info['gold']}\n"
  s += f"Kills: {info['combat_record']['kills']} | "
  s += f"Visits: {info['combat_record']['visits_completed']}\n"
  s += f"Current segment: {seg}\n\n"

  # Current segment details.
  if seg == 0:
    s += "You are at the HUB (segment 0, safe zone).\n"
  else:
    detail = segDetails.get (seg)
    if detail:
      s += f"Segment {seg}: depth {detail['depth']}\n"

  # Links from current segment.
  detail = segDetails.get (seg)
  if detail and detail.get ("links"):
    s += "\nConnected segments:\n"
    for dir, lnk in detail["links"].items ():
      to_seg = lnk["to_segment"]
      to_detail = segDetails.get (to_seg)
      depth_str = f"depth {to_detail['depth']}" if to_detail else "?"
      s += f"  {dir} -> segment {to_seg} ({depth_str})\n"
  elif seg == 0:
    # Segment 0 links are inferred — check which segments link back to 0.
    s += "\nConnected segments:\n"
    for sid, d in segDetails.items ():
      for dir, lnk in d.get ("links", {}).items ():
        if lnk["to_segment"] == 0:
          # Reverse: from seg 0, the opposite direction goes to this segment.
          opp = {"north":"south","south":"north","east":"west","west":"east"}
          s += f"  {opp.get(dir,dir)} -> segment {sid} (depth {d['depth']})\n"

  # Available actions.
  s += "\nAvailable actions:\n"
  if seg != 0 and not info["in_channel"]:
    s += "  - Enter dungeon at this segment\n"
  if hp < maxHp and potions > 0:
    s += "  - Heal (use health potion)\n"
  s += "  - Travel to adjacent segment\n"
  s += "  - Discover new segment in an unexplored direction\n"
  s += "  - Done (finish exploring)\n"

  # Discovery cooldown hint.
  s += "\nDiscovery creates a provisional segment in the chosen direction.\n"
  s += "You must complete a dungeon run there to confirm it.\n"

  s += "\nWhat do you want to do? Respond with JSON."
  return s


def formatDungeonGatePrompt (state):
  px, py = state["player_x"], state["player_y"]
  s = f"Dungeon map ({len (state['monsters'])} monsters):\n"
  s += state.get ("map", "") + "\n"
  s += "Gates:\n"
  for g in state["gates"]:
    dist = abs (g["x"] - px) + abs (g["y"] - py)
    s += f"  {g['dir']} at ({g['x']},{g['y']}) dist {dist}\n"
  s += 'Pick a gate: {"decision": "gate", "target": "south"}'
  return s


def playDungeon (seed, depth, playerInfo, useAi=True):
  """Run dungeon locally, return (survived, results, actionLog)."""
  hp = playerInfo["hp"]
  maxHp = playerInfo["max_hp"]
  s = playerInfo["stats"]
  es = playerInfo["effective_stats"]
  potions = sum (i["quantity"] for i in playerInfo["inventory"]
                 if i["item_id"] == "health_potion" and i["slot"] == "bag")

  args = [
    PLAY_BINARY, seed, str (depth), str (hp), str (maxHp),
    str (playerInfo["level"]),
    str (s["strength"]), str (s["dexterity"]),
    str (s["constitution"]), str (s["intelligence"]),
    str (es["equip_attack"]), str (es["equip_defense"]),
    str (potions),
  ]
  proc = subprocess.Popen (
    args,
    stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    stderr=subprocess.PIPE, text=True, bufsize=1,
  )

  line = proc.stdout.readline ().strip ()
  state = json.loads (line)

  px, py = state["player_x"], state["player_y"]

  if useAi:
    prompt = formatDungeonGatePrompt (state)
    decision = askClaude (prompt)
    target_dir = decision.get ("target", "south")
  else:
    target_dir = "south"

  target = None
  for g in state["gates"]:
    if g["dir"] == target_dir:
      target = (g["x"], g["y"], g["dir"])
      break
  if not target:
    best = min (state["gates"],
                key=lambda g: abs (g["x"] - px) + abs (g["y"] - py))
    target = (best["x"], best["y"], best["dir"])

  log.info (f"    Dungeon: {len (state['monsters'])} monsters, "
            f"targeting {target[2]} gate")

  autopilot = SimpleAutopilot (*target)
  action_log = []
  turns = 0

  while turns < 300:
    if state.get ("game_over"):
      break
    action = autopilot.get_action (state)
    replay = {"type": action["action"]}
    if action["action"] == "move":
      replay["dx"] = action.get ("dx", 0)
      replay["dy"] = action.get ("dy", 0)
    elif action["action"] == "use":
      replay["type"] = "use"
      replay["item"] = action.get ("item", "")
    action_log.append (replay)

    proc.stdin.write (json.dumps (action) + "\n")
    proc.stdin.flush ()
    turns += 1

    line = proc.stdout.readline ().strip ()
    if not line:
      break
    state = json.loads (line)

  proc.stdin.close ()
  proc.wait ()

  results = {
    "survived": state.get ("survived", False),
    "xp": state.get ("xp", 0),
    "gold": state.get ("gold", 0),
    "kills": state.get ("kills", 0),
    "hp_remaining": max (0, state.get ("hp", 0)),
  }
  if state.get ("survived") and state.get ("exit_gate"):
    results["exit_gate"] = state["exit_gate"]
  loot = [{"item": l["item"], "n": l["qty"]}
          for l in state.get ("loot", []) if l["qty"] > 0]
  if loot:
    results["loot"] = loot

  return state.get ("survived", False), results, action_log


# ---- Main exploration loop ----

def main (playerName, targetSegments, useAi=True):
  for binary in [GSP_BINARY, PLAY_BINARY]:
    if not os.path.exists (binary):
      print (f"ERROR: {binary} not found. Build first.")
      sys.exit (1)

  basedir = "/tmp/rog_explorer_%08x" % random.getrandbits (32)
  shutil.rmtree (basedir, ignore_errors=True)
  os.makedirs (basedir)

  startPort = random.randint (10000, 20000)
  ports = portGenerator (startPort)
  gspPort = next (ports)
  gspDatadir = os.path.join (basedir, "gsp")
  os.makedirs (gspDatadir)

  env = Environment (basedir, ports, XETH_BINARY)
  env.enablePending ()

  log.info (f"=== AI OVERWORLD EXPLORER ===")
  log.info (f"Player: {playerName}")
  log.info (f"Target: explore {targetSegments} segments")
  log.info (f"AI: {'Claude' if useAi else 'autopilot only'}")

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
        "--dungeon_id=explorer_%s" % playerName,
      ]
      envVars = dict (os.environ)
      envVars["GLOG_log_dir"] = gspDatadir
      gspProc = subprocess.Popen (gspArgs, env=envVars)
      gspRpcUrl = "http://localhost:%d" % gspPort

      try:
        gsp = waitForGsp (gspRpcUrl)
        log.info ("GSP synced")

        # Register.
        log.info (f"\nStep 1: Register {playerName}")
        e.register ("p", playerName)
        sendMove (e, playerName, {"r": {}})
        info = getData (gsp.getplayerinfo (playerName))
        log.info (f"  Level {info['level']}, HP {info['hp']}/{info['max_hp']}")

        segDetails = {}  # id -> full segment info
        confirmedSegments = set ()
        dungeonRuns = 0
        claudeCalls = 0
        maxSteps = targetSegments * 5  # safety limit

        for step in range (maxSteps):
          info = getData (gsp.getplayerinfo (playerName))
          curSeg = info["current_segment"]

          # Refresh segment details.
          allSegs = getData (gsp.listsegments ())
          for s in allSegs:
            if s["id"] not in segDetails:
              detail = getData (gsp.getsegmentinfo (s["id"]))
              if detail:
                segDetails[s["id"]] = detail

          log.info (f"\n--- Step {step + 1} | Seg {curSeg} | "
                    f"HP {info['hp']}/{info['max_hp']} | "
                    f"Confirmed: {len (confirmedSegments)}/{targetSegments} ---")

          if len (confirmedSegments) >= targetSegments:
            log.info ("Target reached!")
            break

          # Ask Claude what to do.
          if useAi:
            prompt = formatOverworldPrompt (info, allSegs, segDetails, gsp)
            decision = askClaude (prompt)
            claudeCalls += 1
            log.info (f"  [AI: {json.dumps (decision)}]")
          else:
            # Simple autopilot: discover, enter, confirm, repeat.
            decision = autoDecide (info, segDetails, confirmedSegments)
            log.info (f"  [Auto: {json.dumps (decision)}]")

          dtype = decision.get ("decision", "done")

          if dtype == "done":
            log.info ("  AI says done.")
            break

          elif dtype == "heal":
            log.info ("  Using health potion...")
            sendMove (e, playerName, {"ui": {"item": "health_potion"}})

          elif dtype == "discover":
            dir = decision.get ("dir", "east")
            depth = max (1, len (confirmedSegments) + 1)
            log.info (f"  Waiting for discovery cooldown (52 blocks)...")
            e.generate (52)
            time.sleep (0.3)
            log.info (f"  Discovering {dir} (depth {depth})...")
            segs_before = len (getData (gsp.listsegments ()))
            sendMove (e, playerName, {"d": {"depth": depth, "dir": dir}})
            segs_after_list = getData (gsp.listsegments ())
            if len (segs_after_list) > segs_before:
              log.info (f"  New segment created!")
              # Refresh ALL segment details (links changed).
              segDetails.clear ()
              for s in segs_after_list:
                detail = getData (gsp.getsegmentinfo (s["id"]))
                if detail:
                  segDetails[s["id"]] = detail
            else:
              log.info (f"  Discovery failed (dir already linked?)")

          elif dtype == "travel":
            dir = decision.get ("dir", "east")
            log.info (f"  Traveling {dir}...")
            sendMove (e, playerName, {"t": {"dir": dir}})

          elif dtype == "enter_dungeon":
            if info["hp"] <= 0:
              log.info ("  Can't enter dungeon at 0 HP!")
              continue

            targetSeg = decision.get ("segment", curSeg)
            log.info (f"  Entering dungeon at segment {targetSeg}...")
            sendMove (e, playerName, {"ec": {"id": targetSeg}})

            info = getData (gsp.getplayerinfo (playerName))
            if not info["in_channel"]:
              log.info ("  Failed to enter channel (rejected by GSP)")
              continue

            visitId = info["active_visit"]["visit_id"]
            segInfo = segDetails.get (targetSeg)
            if not segInfo:
              segInfo = getData (gsp.getsegmentinfo (targetSeg))
            seed = segInfo["seed"]
            depth = segInfo["depth"]

            log.info (f"  Playing dungeon (seed={seed[:16]}..., depth={depth})...")
            survived, results, actionLog = playDungeon (
              seed, depth, info, useAi=useAi)
            dungeonRuns += 1

            outcome = "SURVIVED" if survived else "DIED"
            log.info (f"  {outcome} | Kills: {results['kills']} | "
                      f"XP: {results['xp']} | Gold: {results['gold']}")

            log.info (f"  Settling on-chain ({len (actionLog)} actions)...")
            sendMove (e, playerName, {
              "xc": {"id": visitId, "results": results,
                     "actions": actionLog}
            })

            info = getData (gsp.getplayerinfo (playerName))
            if not info["in_channel"]:
              log.info (f"  Settled! HP: {info['hp']}/{info['max_hp']} "
                        f"Seg: {info['current_segment']}")
              confirmedSegments.add (targetSeg)
            else:
              log.info (f"  Settlement REJECTED (replay mismatch?)")

          # (cooldown mining is handled inside the discover branch above)

        # ---- Summary ----
        info = getData (gsp.getplayerinfo (playerName))
        allSegs = getData (gsp.listsegments ())

        log.info (f"\n{'=' * 60}")
        log.info (f"  EXPLORATION COMPLETE")
        log.info (f"{'=' * 60}")
        log.info (f"  Player: {info['name']}")
        log.info (f"  Level: {info['level']}")
        log.info (f"  HP: {info['hp']}/{info['max_hp']}")
        log.info (f"  XP: {info['xp']} | Gold: {info['gold']}")
        log.info (f"  Kills: {info['combat_record']['kills']}")
        log.info (f"  Deaths: {info['combat_record']['deaths']}")
        log.info (f"  Visits: {info['combat_record']['visits_completed']}")
        log.info (f"  Current segment: {info['current_segment']}")
        log.info (f"  World segments: {len (allSegs)}")
        log.info (f"  Confirmed: {len (confirmedSegments)}")
        log.info (f"  Dungeon runs: {dungeonRuns}")
        log.info (f"  Claude calls: {claudeCalls}")
        log.info (f"{'=' * 60}")

      finally:
        log.info ("Stopping GSP...")
        try:
          r = jsonrpclib.ServerProxy (gspRpcUrl)
          r._notify.stop ()
          gspProc.wait (timeout=10)
        except:
          gspProc.terminate ()
          gspProc.wait (timeout=5)

  finally:
    log.info (f"Cleaning up {basedir}")
    shutil.rmtree (basedir, ignore_errors=True)


def autoDecide (info, segDetails, confirmed):
  """Simple non-AI decision logic for testing without Claude."""
  curSeg = info["current_segment"]
  hp, maxHp = info["hp"], info["max_hp"]
  potions = sum (i["quantity"] for i in info["inventory"]
                 if "health_potion" in i["item_id"])

  # Heal if low HP and have potions.
  if hp > 0 and hp < maxHp * 0.5 and potions > 0:
    return {"decision": "heal"}

  # Find which directions are already linked from current segment.
  linked_dirs = set ()
  linked_segs = {}  # dir -> segment ID
  detail = segDetails.get (curSeg)
  if detail:
    for dir, lnk in detail.get ("links", {}).items ():
      linked_dirs.add (dir)
      linked_segs[dir] = lnk["to_segment"]

  # For segment 0, infer links from other segments that link back.
  if curSeg == 0:
    opp = {"north":"south","south":"north","east":"west","west":"east"}
    for sid, d in segDetails.items ():
      for dir, lnk in d.get ("links", {}).items ():
        if lnk["to_segment"] == 0:
          rdir = opp.get (dir, dir)
          linked_dirs.add (rdir)
          linked_segs[rdir] = sid

  # If at a non-hub segment we haven't confirmed, enter its dungeon.
  if curSeg != 0 and curSeg not in confirmed and hp > 0:
    return {"decision": "enter_dungeon"}

  # Enter a linked provisional segment (discoverer privilege — ec works
  # from source segment without traveling first).
  if hp > 0:
    for dir, sid in linked_segs.items ():
      if sid not in confirmed and sid != 0:
        return {"decision": "enter_dungeon", "segment": sid}

  # Discover in unexplored direction from current segment.
  allDirs = ["east", "north", "south", "west"]
  unexplored = [d for d in allDirs if d not in linked_dirs]
  if unexplored:
    return {"decision": "discover", "dir": unexplored[0]}

  # Travel to a confirmed segment to discover new directions from there.
  for dir, sid in linked_segs.items ():
    if sid in confirmed and sid != 0:
      return {"decision": "travel", "dir": dir}

  return {"decision": "done"}


logging.basicConfig (
  level=logging.INFO,
  format="%(asctime)s [%(name)s] %(message)s",
  stream=sys.stderr,
)
log = logging.getLogger ("ai_explorer")

if __name__ == "__main__":
  useAi = "--ai" in sys.argv
  args = [a for a in sys.argv[1:] if a != "--ai"]
  name = args[0] if len (args) > 0 else "explorer"
  numSegs = int (args[1]) if len (args) > 1 else 3
  main (name, numSegs, useAi=useAi)
