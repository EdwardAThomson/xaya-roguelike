#!/usr/bin/env python3

"""
AI-powered dungeon player.  Uses Claude Code CLI to make decisions each turn.

Usage:
  python3 devnet/ai_player.py [seed] [depth]

Requires: claude CLI (Claude Code) in PATH.
"""

import json
import os
import subprocess
import sys

PROJECT_DIR = os.path.dirname (os.path.dirname (os.path.abspath (__file__)))
PLAY_BINARY = os.path.join (PROJECT_DIR, "build", "roguelike-play")

SYSTEM_PROMPT = """You are playing a turn-based roguelike dungeon game. Each turn you receive the game state and must choose ONE action.

## Game Rules
- You are '@' on the ASCII map
- '#' = wall, '.' = floor, '+' = gate (exit), '!' = item on ground
- Letters (r, s, b, g, S, k, w, O, P, C, M, D) are monsters
- Moving into a monster attacks it. Monsters attack you if adjacent after your turn.
- Gates ('+') are exits — walk onto one and use "gate" to leave the dungeon alive.
- Pick up items by standing on '!' and using "pickup"
- You carry health potions — use them when HP is low

## Available Actions
- Move: {"action": "move", "dx": 1, "dy": 0}  (dx/dy each -1, 0, or 1)
- Wait: {"action": "wait"}
- Pick up item: {"action": "pickup"}
- Use health potion: {"action": "use", "item": "health_potion"}
- Enter gate (when on '+'): {"action": "gate"}

## Strategy
- Navigate toward a gate to exit safely — surviving is the goal
- Fight monsters in your path, avoid unnecessary fights
- Use health potions when HP drops below 40%
- Pick up items you walk over

Respond with ONLY a JSON action object. No explanation."""


def ask_claude (prompt):
    """Call Claude Code CLI in print mode and get the response."""
    try:
        result = subprocess.run (
            ["claude", "-p", "--output-format", "json",
             "--model", "haiku", "--bare",
             "--append-system-prompt", SYSTEM_PROMPT],
            input=prompt,
            capture_output=True,
            text=True,
            timeout=60,
        )
    except subprocess.TimeoutExpired:
        print (f"  [Claude timeout — waiting]", flush=True)
        return '{"action": "wait"}'

    if result.returncode != 0:
        print (f"  [Claude error: {result.stderr[:200]}]", file=sys.stderr)
        return '{"action": "wait"}'

    try:
        response = json.loads (result.stdout)
        # Claude Code JSON output has a "result" field.
        text = response.get ("result", "")
        if text:
            return text
    except json.JSONDecodeError:
        pass

    return result.stdout.strip ()


def extract_json (text):
    """Try to extract a JSON object from LLM response text."""
    text = text.strip ()

    # Remove markdown code fences.
    if "```" in text:
        lines = text.split ("\n")
        text = "\n".join (l for l in lines if not l.startswith ("```"))
        text = text.strip ()

    try:
        return json.loads (text)
    except json.JSONDecodeError:
        pass

    # Find JSON object in text.
    start = text.find ("{")
    end = text.rfind ("}") + 1
    if start >= 0 and end > start:
        try:
            return json.loads (text[start:end])
        except json.JSONDecodeError:
            pass

    return None


def format_state (state):
    """Format game state as a concise prompt."""
    px, py = state["player_x"], state["player_y"]

    s = f"Turn {state['turn']} | HP: {state['hp']}/{state['max_hp']}"
    s += f" | Kills: {state['kills']} | XP: {state['xp']} | Gold: {state['gold']}\n"
    s += f"Position: ({px}, {py})\n"

    s += "\nMap:\n" + state.get ("map", "(no map)")

    # Nearby monsters.
    nearby = [m for m in state.get ("monsters", [])
              if abs (m["x"] - px) <= 12 and abs (m["y"] - py) <= 12]
    if nearby:
        s += "\nNearby monsters:\n"
        for m in nearby:
            dist = abs (m["x"] - px) + abs (m["y"] - py)
            s += f"  {m['name']} ({m['symbol']}) at ({m['x']},{m['y']}) HP:{m['hp']} ATK:{m['attack']} dist:{dist}\n"

    # Items at feet.
    items_here = [i for i in state.get ("ground_items", [])
                  if i["x"] == px and i["y"] == py]
    if items_here:
        s += "\nItems at your feet:\n"
        for i in items_here:
            s += f"  {i['item']} x{i['qty']}\n"

    # Gates.
    s += "\nGates (exits):\n"
    for g in state.get ("gates", []):
        dist = abs (g["x"] - px) + abs (g["y"] - py)
        s += f"  {g['dir']} at ({g['x']},{g['y']}) dist:{dist}\n"

    # Inventory.
    if state.get ("loot"):
        s += "\nCarrying:\n"
        for l in state["loot"]:
            s += f"  {l['item']} x{l['qty']}\n"

    s += "\nChoose your action (JSON only):"
    return s


def run_game (seed, depth):
    if not os.path.exists (PLAY_BINARY):
        print (f"ERROR: Build first: cd {PROJECT_DIR}/build && cmake .. && make roguelike-play")
        sys.exit (1)

    # Check claude CLI.
    try:
        subprocess.run (["claude", "--version"], capture_output=True, timeout=5)
    except FileNotFoundError:
        print ("ERROR: claude CLI not found in PATH")
        sys.exit (1)

    proc = subprocess.Popen (
        [PLAY_BINARY, seed, str (depth)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )

    print (f"=== AI DUNGEON PLAYER (via Claude Code) ===", flush=True)
    print (f"Seed: {seed}, Depth: {depth}", flush=True)
    print (flush=True)

    turn = 0
    max_turns = 150

    try:
        while turn < max_turns:
            line = proc.stdout.readline ().strip ()
            if not line:
                break

            state = json.loads (line)

            if "error" in state:
                print (f"  [Game error: {state['error']}]")
                continue

            # Print map.
            print (f"\nTurn {state['turn']:3d} | HP {state['hp']:3d}/{state['max_hp']}"
                   f" | Kills {state['kills']} | XP {state['xp']} | Gold {state['gold']}",
                   flush=True)
            if state.get ("map"):
                for ml in state["map"].strip ().split ("\n"):
                    print (f"  {ml}")

            if state.get ("game_over"):
                if state.get ("survived"):
                    print (f"\n  SURVIVED! Exited via {state.get ('exit_gate', '?')} gate")
                else:
                    print (f"\n  DIED on turn {state['turn']}")
                print (f"  Kills: {state['kills']} | XP: {state['xp']} | Gold: {state['gold']}")
                break

            # Ask Claude.
            prompt = format_state (state)
            response = ask_claude (prompt)

            action = extract_json (response)
            if action is None:
                print (f"  [Bad response: {str(response)[:80]}]")
                action = {"action": "wait"}

            print (f"  -> {json.dumps (action)}", flush=True)

            proc.stdin.write (json.dumps (action) + "\n")
            proc.stdin.flush ()
            turn += 1

        if turn >= max_turns:
            print (f"\n  Time limit ({max_turns} turns)")

    finally:
        proc.stdin.close ()
        proc.wait ()


if __name__ == "__main__":
    seed = sys.argv[1] if len (sys.argv) > 1 else "ai_adventure"
    depth = int (sys.argv[2]) if len (sys.argv) > 2 else 1
    run_game (seed, depth)
