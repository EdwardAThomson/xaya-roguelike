#!/usr/bin/env python3

"""
Minimal solo channel daemon.  Connects to the full stack (Anvil + Xaya X +
rogueliked), creates a player, enters a dungeon segment via channel, plays
it locally via roguelike-play, and settles the results on-chain.

This is the player-side process for solo dungeon exploration.

Usage:
  source ~/Explore/xayax/.venv/bin/activate
  python3 devnet/channel_daemon.py [player_name] [seed_suffix]

Requires the full devnet stack running (or starts it automatically).
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

PROJECT_DIR = os.path.dirname (os.path.dirname (os.path.abspath (__file__)))
GSP_BINARY = os.path.join (PROJECT_DIR, "build", "rogueliked")
PLAY_BINARY = os.path.join (PROJECT_DIR, "build", "roguelike-play")
XETH_BINARY = "/usr/local/bin/xayax-eth"
GAME_ID = "rog"

# Ensure Foundry is on PATH.
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
    """Extract the data field from a GSP RPC response."""
    return resp["data"] if "data" in resp else resp


def sendMove (env, name, move):
    """Send a game move and mine a block."""
    env.move ("p", name, json.dumps ({"g": {GAME_ID: move}}))
    env.generate (1)
    time.sleep (0.5)


class SimpleAutopilot:
    """BFS autopilot for playing through the dungeon."""

    def __init__ (self, target_gate):
        self.gx, self.gy, self.gdir = target_gate

    def get_action (self, state):
        px, py = state["player_x"], state["player_y"]
        hp, max_hp = state["hp"], state["max_hp"]

        # Auto-heal.
        if hp < max_hp * 0.4:
            for l in state.get ("loot", []):
                if "health_potion" in l["item"] and l["qty"] > 0:
                    return {"action": "use", "item": l["item"]}

        # Auto-pickup.
        for gi in state.get ("ground_items", []):
            if gi["x"] == px and gi["y"] == py:
                return {"action": "pickup"}

        # Enter gate.
        if px == self.gx and py == self.gy:
            return {"action": "gate"}

        # Fight adjacent monster.
        for m in state.get ("monsters", []):
            dx, dy = m["x"] - px, m["y"] - py
            if abs (dx) <= 1 and abs (dy) <= 1 and (dx or dy):
                return {"action": "move", "dx": dx, "dy": dy}

        # BFS toward gate.
        step = self._bfs (state, px, py, self.gx, self.gy)
        if step:
            return {"action": "move", "dx": step[0], "dy": step[1]}

        return {"action": "wait"}

    def _bfs (self, state, fx, fy, tx, ty):
        if fx == tx and fy == ty:
            return (0, 0)
        map_str = state.get ("map", "")
        ox, oy = state.get ("map_origin_x", 0), state.get ("map_origin_y", 0)
        rows = map_str.strip ().split ("\n")

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


def playDungeon (seed, depth, hp, maxHp):
    """
    Run a dungeon session locally via roguelike-play.
    Returns (survived, results_dict) for on-chain settlement.
    """
    proc = subprocess.Popen (
        [PLAY_BINARY, seed, str (depth), str (hp), str (maxHp)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, bufsize=1,
    )

    # Read initial state to pick a gate.
    line = proc.stdout.readline ().strip ()
    state = json.loads (line)

    # Pick nearest gate.
    px, py = state["player_x"], state["player_y"]
    best_gate = min (state["gates"],
                     key=lambda g: abs (g["x"] - px) + abs (g["y"] - py))
    target = (best_gate["x"], best_gate["y"], best_gate["dir"])

    log.info (f"  Dungeon: {len(state['monsters'])} monsters, "
              f"targeting {best_gate['dir']} gate")

    autopilot = SimpleAutopilot (target)
    turns = 0
    max_turns = 300

    while turns < max_turns:
        if state.get ("game_over"):
            break

        action = autopilot.get_action (state)
        proc.stdin.write (json.dumps (action) + "\n")
        proc.stdin.flush ()
        turns += 1

        line = proc.stdout.readline ().strip ()
        if not line:
            break
        state = json.loads (line)
        if "error" in state:
            continue

    proc.stdin.close ()
    proc.wait ()

    # Build results for on-chain settlement.
    results = {
        "survived": state.get ("survived", False),
        "xp": state.get ("xp", 0),
        "gold": state.get ("gold", 0),
        "kills": state.get ("kills", 0),
        "hp_remaining": max (0, state.get ("hp", 0)),
    }

    if state.get ("survived") and state.get ("exit_gate"):
        results["exit_gate"] = state["exit_gate"]

    # Loot.
    loot = []
    for l in state.get ("loot", []):
        if l["qty"] > 0:
            loot.append ({"item": l["item"], "n": l["qty"]})
    if loot:
        results["loot"] = loot

    return state.get ("survived", False), results, state


def main (playerName, seedSuffix):
    for binary in [GSP_BINARY, PLAY_BINARY]:
        if not os.path.exists (binary):
            print (f"ERROR: {binary} not found. Build first.")
            sys.exit (1)

    basedir = "/tmp/rog_channel_%08x" % random.getrandbits (32)
    shutil.rmtree (basedir, ignore_errors=True)
    os.makedirs (basedir)

    startPort = random.randint (10000, 30000)
    ports = portGenerator (startPort)
    gspPort = next (ports)
    gspDatadir = os.path.join (basedir, "gsp")
    os.makedirs (gspDatadir)

    env = Environment (basedir, ports, XETH_BINARY)
    env.enablePending ()

    log.info (f"=== SOLO CHANNEL DAEMON ===")
    log.info (f"Player: {playerName}")
    log.info (f"Base dir: {basedir}")

    try:
        with env.run () as e:
            xayaRpcUrl = e.getXRpcUrl ()
            e.generate (10)

            # Start GSP.
            gspArgs = [
                GSP_BINARY,
                "--xaya_rpc_url=%s" % xayaRpcUrl,
                "--xaya_rpc_protocol=2",
                "--game_rpc_port=%d" % gspPort,
                "--datadir=%s" % gspDatadir,
                "--genesis_height=0", "--genesis_hash=",
                "--xaya_rpc_wait", "--pending_moves",
                "--dungeon_id=channel_test_%s" % seedSuffix,
            ]
            envVars = dict (os.environ)
            envVars["GLOG_log_dir"] = gspDatadir
            gspProc = subprocess.Popen (gspArgs, env=envVars)
            gspRpcUrl = "http://localhost:%d" % gspPort

            try:
                gsp = waitForGsp (gspRpcUrl)
                log.info ("GSP synced")

                # 1. Register player.
                log.info (f"Step 1: Register {playerName}")
                e.register ("p", playerName)
                sendMove (e, playerName, {"r": {}})

                info = getData (gsp.getplayerinfo (playerName))
                log.info (f"  Registered: level {info['level']}, "
                          f"HP {info['hp']}/{info['max_hp']}")

                # 2. Discover segment.
                log.info ("Step 2: Discover segment east")
                sendMove (e, playerName, {"d": {"depth": 1, "dir": "east"}})

                info = getData (gsp.getplayerinfo (playerName))
                log.info (f"  Active visit: {info.get ('active_visit')}")

                # 3. Expire the discover visit and travel.
                log.info ("Step 3: Travel to segment")
                e.generate (101)  # expire open visit
                time.sleep (1)
                sendMove (e, playerName, {"t": {"dir": "east"}})

                info = getData (gsp.getplayerinfo (playerName))
                log.info (f"  Current segment: {info['current_segment']}")
                segmentId = info["current_segment"]

                # 4. Enter channel.
                log.info ("Step 4: Enter channel")
                sendMove (e, playerName, {"ec": {"id": segmentId}})

                info = getData (gsp.getplayerinfo (playerName))
                assert info["in_channel"], "Player should be in channel"
                log.info (f"  In channel: {info['in_channel']}")

                # Find the visit ID.
                visits = getData (gsp.listvisits ("active"))
                visitId = None
                for v in visits:
                    if v.get ("initiator") == playerName:
                        visitId = v["id"]
                        break
                assert visitId is not None, "No active visit found"
                log.info (f"  Visit ID: {visitId}")

                # 5. Get segment info for seed.
                segInfo = getData (gsp.getsegmentinfo (segmentId))
                seed = segInfo["seed"]
                depth = segInfo["depth"]
                log.info (f"  Segment seed: {seed}, depth: {depth}")

                # 6. Play dungeon locally!
                log.info ("Step 5: Playing dungeon locally...")
                survived, results, finalState = playDungeon (
                    seed, depth, info["hp"], info["max_hp"])

                log.info (f"  Result: {'SURVIVED' if survived else 'DIED'}")
                log.info (f"  Kills: {results['kills']}, XP: {results['xp']}, "
                          f"Gold: {results['gold']}")
                if results.get ("loot"):
                    log.info (f"  Loot: {results['loot']}")

                # 7. Exit channel — settle results on-chain.
                log.info ("Step 6: Settle channel results on-chain")
                sendMove (e, playerName, {
                    "xc": {"id": visitId, "results": results}
                })

                # 8. Verify final state.
                info = getData (gsp.getplayerinfo (playerName))
                log.info (f"\n=== FINAL STATE ===")
                log.info (f"  HP: {info['hp']}/{info['max_hp']}")
                log.info (f"  XP: {info['xp']}, Gold: {info['gold']}")
                log.info (f"  Kills: {info['combat_record']['kills']}")
                log.info (f"  Deaths: {info['combat_record']['deaths']}")
                log.info (f"  Visits completed: "
                          f"{info['combat_record']['visits_completed']}")
                log.info (f"  In channel: {info['in_channel']}")
                log.info (f"  Current segment: {info['current_segment']}")

                # Verify channel was settled.
                assert not info["in_channel"], "Should no longer be in channel"
                assert info["combat_record"]["visits_completed"] >= 1

                log.info (f"\n=== CHANNEL DAEMON TEST PASSED ===")

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


logging.basicConfig (
    level=logging.INFO,
    format="%(asctime)s [%(name)s] %(message)s",
    stream=sys.stderr,
)
log = logging.getLogger ("channel_daemon")

if __name__ == "__main__":
    name = sys.argv[1] if len (sys.argv) > 1 else "hero"
    suffix = sys.argv[2] if len (sys.argv) > 2 else "test1"
    main (name, suffix)
