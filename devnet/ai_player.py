#!/usr/bin/env python3

"""
AI-powered dungeon player with two-tier decision system.

Autopilot handles tactics (BFS pathfinding, auto-fight, auto-heal).
Claude handles strategy (which gate, fight or flee, explore vs exit).
Claude is called only at decision points, not every turn.

Usage: python3 devnet/ai_player.py [seed] [depth]
"""

import json
import os
import subprocess
import sys
import uuid
from collections import deque

PROJECT_DIR = os.path.dirname (os.path.dirname (os.path.abspath (__file__)))
PLAY_BINARY = os.path.join (PROJECT_DIR, "build", "roguelike-play")

SYSTEM_PROMPT = """You are the strategic brain of a roguelike dungeon explorer. You make high-level decisions; an autopilot handles movement and routine combat.

## Your Role
- On the first turn, you see the FULL dungeon map and choose which gate to exit through
- During play, you're consulted at decision points (monster encounters, low HP, blocked path)
- You respond with a JSON decision object

## Response Format (always JSON, nothing else)
For gate selection:   {"decision": "gate", "target": "south"}
For combat:           {"decision": "fight"} or {"decision": "avoid"}
For item detour:      {"decision": "detour"} or {"decision": "skip"}
For healing:          {"decision": "heal"} or {"decision": "push_on"}
For gate arrival:     {"decision": "exit"} or {"decision": "explore"}
For new gate choice:  {"decision": "gate", "target": "east"}

## Strategy Guide
- Surviving and exiting is the primary goal
- Fight monsters only if they block your path or are weak
- Use health potions when HP drops below 40%
- Choose the gate with the clearest path (fewest monsters)
- If a monster is much stronger than you, avoid it"""


class Autopilot:
    """Handles tactical decisions: pathfinding, auto-fight, auto-heal."""

    def __init__ (self, aggressive=False):
        self.target_gate = None   # (x, y, direction) set by Claude
        self.fight_policy = True  # fight adjacent monsters on path
        self.blocked_gates = set ()  # gates that BFS can't reach
        self.aggressive = aggressive  # hunt monsters before heading to gate

    def get_action (self, state):
        """Returns an action dict, or None if a decision point is reached."""
        px, py = state["player_x"], state["player_y"]
        hp = state["hp"]
        max_hp = state["max_hp"]

        # 1. Auto-heal if HP < 40% and have potions.
        if hp < max_hp * 0.4:
            for l in state.get ("loot", []):
                if "health_potion" in l["item"] and l["qty"] > 0:
                    return {"action": "use", "item": l["item"]}

        # 2. Auto-pickup if standing on item.
        for gi in state.get ("ground_items", []):
            if gi["x"] == px and gi["y"] == py:
                return {"action": "pickup"}

        # 3. Auto-enter gate if on target gate.
        if self.target_gate:
            gx, gy, _ = self.target_gate
            if px == gx and py == gy:
                return {"action": "gate"}

        # 4. Auto-fight adjacent monster.
        for m in state.get ("monsters", []):
            dx = m["x"] - px
            dy = m["y"] - py
            if abs (dx) <= 1 and abs (dy) <= 1 and (dx != 0 or dy != 0):
                return {"action": "move", "dx": dx, "dy": dy}

        # 5. Aggressive mode: hunt the nearest monster if HP > 60%.
        if self.aggressive and hp > max_hp * 0.6:
            monsters = state.get ("monsters", [])
            if monsters:
                nearest = min (monsters,
                    key=lambda m: abs (m["x"] - px) + abs (m["y"] - py))
                step = self._bfs_step (state, px, py, nearest["x"], nearest["y"])
                if step:
                    return {"action": "move", "dx": step[0], "dy": step[1]}

        # 6. BFS toward target gate.
        if self.target_gate:
            gx, gy, _ = self.target_gate
            step = self._bfs_step (state, px, py, gx, gy)
            if step:
                return {"action": "move", "dx": step[0], "dy": step[1]}

        # No target or path blocked — need a decision.
        return None

    def needs_decision (self, state, prev_state, turn):
        """Check if we need to consult Claude."""
        if turn == 0:
            return "initial"
        if self.target_gate is None:
            return "no_target"

        px, py = state["player_x"], state["player_y"]

        # Monster on path (within 4 tiles of player, toward gate).
        if self.target_gate:
            gx, gy, _ = self.target_gate
            for m in state.get ("monsters", []):
                mdist = abs (m["x"] - px) + abs (m["y"] - py)
                if mdist <= 4:
                    # Is it roughly between us and the gate?
                    our_dist = abs (gx - px) + abs (gy - py)
                    m_to_gate = abs (gx - m["x"]) + abs (gy - m["y"])
                    if m_to_gate < our_dist:
                        return "monster_on_path"

        # HP critically low (< 25%) and no potions.
        hp = state["hp"]
        max_hp = state["max_hp"]
        has_potions = any ("health_potion" in l["item"] and l["qty"] > 0
                          for l in state.get ("loot", []))
        if hp < max_hp * 0.25 and not has_potions:
            return "critical_hp"

        # Reached target gate.
        if self.target_gate:
            gx, gy, _ = self.target_gate
            if px == gx and py == gy:
                return "at_gate"

        # Path to gate is blocked (BFS returns None).
        if self.target_gate:
            gx, gy, gdir = self.target_gate
            step = self._bfs_step (state, px, py, gx, gy)
            if step is None:
                self.blocked_gates.add (gdir)
                return "path_blocked"

        return None

    def _bfs_step (self, state, fx, fy, tx, ty):
        """BFS next step from (fx,fy) to (tx,ty). Returns (dx,dy) or None."""
        if fx == tx and fy == ty:
            return (0, 0)

        # Parse the map from state for walkability.
        map_str = state.get ("map", "")
        ox = state.get ("map_origin_x", 0)
        oy = state.get ("map_origin_y", 0)
        rows = map_str.strip ().split ("\n")

        def walkable (x, y):
            ry = y - oy
            rx = x - ox
            if ry < 0 or ry >= len (rows) or rx < 0 or rx >= len (rows[ry]):
                return False
            ch = rows[ry][rx]
            return ch != '#'

        queue = deque ()
        parent = {}
        start = (fx, fy)
        goal = (tx, ty)
        queue.append (start)
        parent[start] = None

        dirs = [(-1,-1),(-1,0),(-1,1),(0,-1),(0,1),(1,-1),(1,0),(1,1)]

        while queue:
            cx, cy = queue.popleft ()
            if (cx, cy) == goal:
                # Trace back.
                cur = goal
                while parent[cur] != start:
                    cur = parent[cur]
                return (cur[0] - fx, cur[1] - fy)

            for ddx, ddy in dirs:
                nx, ny = cx + ddx, cy + ddy
                if (nx, ny) in parent:
                    continue
                if not walkable (nx, ny):
                    continue
                parent[(nx, ny)] = (cx, cy)
                queue.append ((nx, ny))

        return None  # No path.


class StrategicBrain:
    """Manages Claude interactions for strategic decisions."""

    def __init__ (self):
        self.session_id = str (uuid.uuid4 ())
        self.first_call = True

    def ask (self, prompt):
        """Call Claude with conversation memory."""
        args = ["claude", "-p", "--output-format", "json",
                "--model", "haiku", "--bare",
                "--append-system-prompt", SYSTEM_PROMPT]

        if self.first_call:
            args.extend (["--session-id", self.session_id])
            self.first_call = False
        else:
            args.extend (["--resume", self.session_id])

        try:
            result = subprocess.run (
                args, input=prompt, capture_output=True,
                text=True, timeout=60,
            )
        except subprocess.TimeoutExpired:
            return {"decision": "gate", "target": "south"}

        if result.returncode != 0:
            return {"decision": "gate", "target": "south"}

        try:
            response = json.loads (result.stdout)
            text = response.get ("result", "")
        except json.JSONDecodeError:
            text = result.stdout.strip ()

        return extract_json (text) or {"decision": "gate", "target": "south"}


def extract_json (text):
    """Extract JSON from LLM response."""
    text = text.strip ()
    if "```" in text:
        lines = text.split ("\n")
        text = "\n".join (l for l in lines if not l.startswith ("```")).strip ()
    try:
        return json.loads (text)
    except json.JSONDecodeError:
        pass
    start = text.find ("{")
    end = text.rfind ("}") + 1
    if start >= 0 and end > start:
        try:
            return json.loads (text[start:end])
        except json.JSONDecodeError:
            pass
    return None


def format_initial_briefing (state):
    """Format the first-turn briefing with full map."""
    px, py = state["player_x"], state["player_y"]
    s = f"=== DUNGEON BRIEFING ===\n"
    s += f"Depth {state['depth']} dungeon. HP: {state['hp']}/{state['max_hp']}\n"
    s += f"Monsters: {len (state.get ('monsters', []))}\n"
    s += f"Items on ground: {len (state.get ('ground_items', []))}\n"
    s += f"You start at ({px}, {py}) marked as '@'\n\n"

    s += "Full map (# wall, . floor, + gate, letters=monsters, !=item):\n"
    s += state.get ("map", "") + "\n"

    s += "Gates (exits):\n"
    for g in state.get ("gates", []):
        dist = abs (g["x"] - px) + abs (g["y"] - py)
        s += f"  {g['dir']:5s} at ({g['x']:2d},{g['y']:2d}) — {dist} tiles away\n"

    s += "\nMonsters:\n"
    for m in state.get ("monsters", []):
        dist = abs (m["x"] - px) + abs (m["y"] - py)
        s += f"  {m['name']} ({m['symbol']}) at ({m['x']},{m['y']}) HP:{m['hp']} ATK:{m['attack']} — {dist} tiles\n"

    s += "\nCarrying: 3x health_potion\n"
    s += "\nWhich gate should we head toward? Respond with JSON: {\"decision\": \"gate\", \"target\": \"south\"}"
    return s


def format_decision_prompt (reason, state, autopilot):
    """Format a decision-point prompt."""
    px, py = state["player_x"], state["player_y"]
    hp, max_hp = state["hp"], state["max_hp"]

    s = f"Turn {state['turn']} | HP {hp}/{max_hp} | Kills {state['kills']} | XP {state['xp']}\n"
    s += f"Position: ({px},{py})"
    if autopilot.target_gate:
        gx, gy, gdir = autopilot.target_gate
        dist = abs (gx - px) + abs (gy - py)
        s += f" | Target: {gdir} gate ({gx},{gy}) — {dist} tiles\n"
    else:
        s += "\n"

    if reason == "monster_on_path":
        nearby = [m for m in state.get ("monsters", [])
                  if abs (m["x"] - px) + abs (m["y"] - py) <= 5]
        s += "\nMonster blocking path:\n"
        for m in nearby:
            s += f"  {m['name']} HP:{m['hp']} ATK:{m['attack']} at ({m['x']},{m['y']})\n"
        s += "\nFight or avoid? {\"decision\": \"fight\"} or {\"decision\": \"avoid\"}"

    elif reason == "critical_hp":
        s += f"\nCRITICAL: HP at {hp}/{max_hp} with no potions!\n"
        s += "Rush to nearest gate or keep fighting?\n"
        s += "{\"decision\": \"gate\", \"target\": \"nearest\"} or {\"decision\": \"push_on\"}"

    elif reason == "at_gate":
        s += f"\nYou're at the gate! Exit or keep exploring?\n"
        s += f"Kills: {state['kills']}, XP: {state['xp']}, Gold: {state['gold']}\n"
        s += "{\"decision\": \"exit\"} or {\"decision\": \"explore\"}"

    elif reason == "path_blocked":
        cur = autopilot.target_gate[2] if autopilot.target_gate else "?"
        s += f"\nPath to {cur} gate is BLOCKED. Choose a DIFFERENT gate:\n"
        for g in state.get ("gates", []):
            dist = abs (g["x"] - px) + abs (g["y"] - py)
            blocked = " (CURRENT - BLOCKED)" if g["dir"] == cur else ""
            s += f"  {g['dir']} at ({g['x']},{g['y']}) — {dist} tiles{blocked}\n"
        s += f"Do NOT choose {cur} again. Pick a different one: {{\"decision\": \"gate\", \"target\": \"east\"}}"

    elif reason == "no_target":
        s += "\nNo target gate set. Choose one:\n"
        for g in state.get ("gates", []):
            dist = abs (g["x"] - px) + abs (g["y"] - py)
            s += f"  {g['dir']} at ({g['x']},{g['y']}) — {dist} tiles\n"
        s += "{\"decision\": \"gate\", \"target\": \"south\"}"

    return s


def apply_decision (decision, state, autopilot):
    """Apply Claude's strategic decision to the autopilot."""
    dtype = decision.get ("decision", "")

    if dtype == "gate":
        target_dir = decision.get ("target", "south")

        # If Claude picked a blocked gate, find nearest unblocked one.
        if target_dir in autopilot.blocked_gates or target_dir == "nearest":
            px, py = state["player_x"], state["player_y"]
            best = None
            best_dist = 9999
            for g in state.get ("gates", []):
                if g["dir"] in autopilot.blocked_gates:
                    continue
                d = abs (g["x"] - px) + abs (g["y"] - py)
                if d < best_dist:
                    best_dist = d
                    best = g
            if best:
                target_dir = best["dir"]

        for g in state.get ("gates", []):
            if g["dir"] == target_dir:
                autopilot.target_gate = (g["x"], g["y"], g["dir"])
                print (f"  [Strategy: head to {g['dir']} gate at ({g['x']},{g['y']})]",
                       flush=True)
                return

    elif dtype == "exit":
        # Autopilot will auto-enter gate next turn.
        pass

    elif dtype == "explore":
        # Clear target to keep exploring.
        autopilot.target_gate = None
        print (f"  [Strategy: keep exploring]", flush=True)

    elif dtype == "fight":
        print (f"  [Strategy: fight the monster]", flush=True)

    elif dtype == "avoid":
        # Pick a different gate to route around.
        print (f"  [Strategy: avoid and reroute]", flush=True)

    elif dtype == "heal":
        print (f"  [Strategy: heal up]", flush=True)


def run_game (seed, depth, aggressive=False):
    if not os.path.exists (PLAY_BINARY):
        print (f"ERROR: Build first: cd {PROJECT_DIR}/build && cmake .. && make roguelike-play")
        sys.exit (1)

    proc = subprocess.Popen (
        [PLAY_BINARY, seed, str (depth)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, bufsize=1,
    )

    autopilot = Autopilot (aggressive=aggressive)
    brain = StrategicBrain ()
    prev_state = None
    claude_calls = 0

    mode_str = "AGGRESSIVE" if aggressive else "standard"
    print (f"=== AI DUNGEON PLAYER v2 ({mode_str}) ===", flush=True)
    print (f"Seed: {seed}, Depth: {depth}", flush=True)
    print (f"Session: {brain.session_id[:8]}...", flush=True)
    print (flush=True)

    max_turns = 300

    try:
        for turn in range (max_turns):
            line = proc.stdout.readline ().strip ()
            if not line:
                break

            state = json.loads (line)
            if "error" in state:
                continue

            # Game over?
            if state.get ("game_over"):
                if state.get ("survived"):
                    print (f"\n  SURVIVED! Exited via {state.get ('exit_gate')} gate",
                           flush=True)
                else:
                    print (f"\n  DIED on turn {state['turn']}", flush=True)
                print (f"  Kills: {state['kills']} | XP: {state['xp']}"
                       f" | Gold: {state['gold']} | Claude calls: {claude_calls}",
                       flush=True)
                break

            # Check if we need a strategic decision.
            reason = autopilot.needs_decision (state, prev_state, state["turn"])

            if reason:
                # Print status.
                print (f"\nTurn {state['turn']:3d} | HP {state['hp']:3d}/{state['max_hp']}"
                       f" | Kills {state['kills']} | [{reason}]", flush=True)

                if reason == "initial":
                    prompt = format_initial_briefing (state)
                else:
                    prompt = format_decision_prompt (reason, state, autopilot)

                print (f"  [Asking Claude...]", flush=True)
                decision = brain.ask (prompt)
                claude_calls += 1
                print (f"  [Claude: {json.dumps (decision)}]", flush=True)
                apply_decision (decision, state, autopilot)

            # Get autopilot action.
            action = autopilot.get_action (state)
            if action is None:
                action = {"action": "wait"}

            # Log periodically.
            if state["turn"] % 20 == 0 and reason is None:
                tgt = f"-> {autopilot.target_gate[2]}" if autopilot.target_gate else "no target"
                print (f"  Turn {state['turn']:3d} | HP {state['hp']:3d}"
                       f" | Kills {state['kills']} | {tgt}", flush=True)

            proc.stdin.write (json.dumps (action) + "\n")
            proc.stdin.flush ()
            prev_state = state

    finally:
        proc.stdin.close ()
        proc.wait ()

    print (f"\nTotal Claude API calls: {claude_calls}", flush=True)


if __name__ == "__main__":
    aggressive = "--aggressive" in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith ("--")]
    seed = args[0] if len (args) > 0 else "ai_v2"
    depth = int (args[1]) if len (args) > 1 else 1
    run_game (seed, depth, aggressive=aggressive)
