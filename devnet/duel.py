"""
Shared duel helpers for the devnet tests (SPEC_multiplayer_pvp.md).

A duel is settled exactly like a co-op run (the loser's `sc` consents to the
merged log, the winner's `s` settles it), but the log carries the round
protocol's commit and reveal entries and the GSP replays it as a duel.  The
fight itself is played by `roguelike-play --duel`, which runs the real
engine with the exact inputs the GSP will replay with; everything here is
plumbing between that tool and the chain:

  - reading the duel's replay inputs back off the GSP (`DuelSpec`),
  - turning a finished fight into the `s` claims (`Claims`),
  - recomputing the commitment and settle-log hashes, so a test can forge a
    log and still hand the GSP a consent that matches it (`SettleLogHash`),
  - earning gold for a stake (`FarmGold`), since a new character has none.

The two hash functions mirror `DuelCommitHash` and `SettleLogHash` in the
backend.  `SelfCheck` pins them to the cross-language vectors in
tests/duel_parity_tests.cpp, so a drift fails loudly before any move is
sent rather than as a mysteriously rejected settlement.
"""

import hashlib
import json
import subprocess

# MoveProcessor::DUEL_XP_BASE: the winner gains this times the loser's level.
DUEL_XP_BASE = 20

# MoveProcessor::DUEL_RAKE_PERCENT: the share of the pot burned at settlement.
DUEL_RAKE_PERCENT = 0


def unwrap (resp):
  """GSP RPC responses are sometimes wrapped in a {data: ...} envelope."""
  return resp["data"] if isinstance (resp, dict) and "data" in resp else resp


# ---- Hashes (mirrors of the backend; see SelfCheck) ----

def CanonicalBody (entry):
  """The canonical encoding of one merged-log entry without its index
  (CanonicalActionBody): "move 1 0", "commit <h>", ..."""
  t = entry["type"]
  if t == "move":
    return "move %d %d" % (entry["dx"], entry["dy"])
  if t == "use":
    return "use %s" % entry["item"]
  if t == "equip":
    return "equip %d %s" % (entry["rowid"], entry["slot"])
  if t == "unequip":
    return "unequip %d" % entry["rowid"]
  if t == "commit":
    return "commit %s" % entry["h"]
  if t == "reveal":
    return "reveal %s" % entry["s"]
  if t in ("pickup", "gate", "wait"):
    return t
  raise ValueError ("unknown action type %r" % t)


def CommitHash (visitId, rnd, participant, action, salt):
  """DuelCommitHash: what a round's commit entry carries."""
  pre = "rog-duel-commit-v1\n%d\n%d\n%d\n%s\n%s" % (
      visitId, rnd, participant, CanonicalBody (action), salt)
  return hashlib.sha256 (pre.encode ()).hexdigest ()


def SettleLogHash (visitId, entries):
  """SettleLogHash: the consent hash an `sc` move carries."""
  data = "rog-settle-v1\n%d\n" % visitId
  for e in entries:
    data += "%d %s\n" % (e["i"], CanonicalBody (e))
  return hashlib.sha256 (data.encode ()).hexdigest ()


def SelfCheck ():
  """Pins the two hashes to the backend's parity vectors."""
  move = {"type": "move", "dx": 1, "dy": -1}
  got = CommitHash (11, 7, 1, move, "00112233445566778899aabbccddeeff")
  assert got == ("08b987326245a8e68dd716b02f79a801"
                 "812cc7db9a7069b0b835001b07f445f3"), \
      "duel commitment drifted from PARITY-DUEL-COMMIT: %s" % got

  log = [
    {"i": 0, "type": "commit", "h": "a" * 64},
    {"i": 1, "type": "commit", "h": "a" * 64},
    {"i": 0, "type": "reveal", "s": "b" * 32},
    {"i": 1, "type": "reveal", "s": "b" * 32},
    {"i": 0, "type": "move", "dx": 0, "dy": 1},
    {"i": 1, "type": "wait"},
  ]
  got = SettleLogHash (11, log)
  assert got == ("6756cb0830f2e74460a9459d09acda93"
                 "0a685469903aaed3ac384c3b9c6b247f"), \
      "settle-log hash drifted from PARITY-DUEL-SETTLEHASH: %s" % got


# ---- Replay inputs and the fight ----

def EffectiveStats (p):
  eff = p["effective_stats"]
  return {
    "level": p["level"],
    "strength": eff["strength"], "dexterity": eff["dexterity"],
    "constitution": eff["constitution"],
    "intelligence": eff["intelligence"],
    "equip_attack": eff["equip_attack"],
    "equip_defense": eff["equip_defense"],
  }


def Constraints (seg):
  """The alignment constraint a segment was generated with, if any."""
  cdir = seg.get ("constraint_dir", "")
  if cdir and cdir in seg.get ("gates", {}):
    g = seg["gates"][cdir]
    return [{"x": g["x"], "y": g["y"], "direction": cdir}]
  return []


def DuelSpec (gsp, visitId, saltSeed):
  """Builds the `roguelike-play --duel` spec from chain state, exactly as
  ProcessSettle builds the replay: participants in canonical order (names
  ascending), every inventory row in rowid order (escrowed ones included),
  and the bag's potions.  Returns (spec, names)."""
  v = unwrap (gsp.getvisitinfo (visitId))
  seg = unwrap (gsp.getsegmentinfo (v["segment"]["x"], v["segment"]["y"]))
  names = sorted (v["participants"])

  players = []
  for name in names:
    p = unwrap (gsp.getplayerinfo (name))
    rows = sorted (p["inventory"], key=lambda it: it["rowid"])
    players.append ({
      "hp": p["hp"], "max_hp": p["max_hp"],
      "stats": EffectiveStats (p),
      "potions": [{"item": it["item_id"], "qty": it["quantity"]}
                  for it in rows
                  if it["slot"] == "bag"
                    and it["item_id"] in ("health_potion",
                                          "greater_health_potion")],
      "inventory": [{"rowid": it["rowid"], "item_id": it["item_id"],
                     "slot": it["slot"]} for it in rows],
      "entry_direction": v["entry_directions"][name],
    })

  spec = {
    "seed": v["seed"], "depth": v["depth"], "visit_id": visitId,
    "constraints": Constraints (seg), "salt_seed": saltSeed,
    "players": players,
  }
  return spec, names


def Fight (playBinary, spec):
  """Fights the duel out with the real engine.  Returns the tool's JSON:
  {decided, winner, rounds, settle_hash, actions, participants}."""
  out = subprocess.run ([playBinary, "--duel", json.dumps (spec)],
                        capture_output=True, text=True)
  lines = out.stdout.strip ().splitlines ()
  if not lines:
    raise RuntimeError ("roguelike-play --duel failed: %s" % out.stderr)
  return json.loads (lines[-1])


def Claims (fight, names, pot, loserLevel):
  """The `s` results array a correct client sends for a decided duel: the
  replay's numbers, plus the duel's own terms for the winner (the pot less
  the rake, and DUEL_XP_BASE times the loser's level)."""
  winner = fight["winner"]
  prize = pot - pot * DUEL_RAKE_PERCENT // 100
  results = []
  for i, name in enumerate (names):
    part = fight["participants"][i]
    won = i == winner
    results.append ({
      "p": name, "survived": won,
      "xp": part["xp"] + (DUEL_XP_BASE * loserLevel if won else 0),
      "gold": part["gold"] + (prize if won else 0),
      "kills": part["kills"],
      "duel": "won" if won else "lost",
    })
  return results


# ---- Chain helpers ----

def FarmGold (gsp, playBinary, move, mine, name, seg, entryDir):
  """Plays one solo run on confirmed segment `seg` to earn gold: walk in
  through the gate facing `entryDir`, collect every reachable pile of gold
  coins, and walk back out the same way (so the player ends where they
  started).  `move (name, data)` sends a game move and `mine ()` confirms
  it.  Returns the gold the run earned, or raises if it did not survive.

  The solver has to know the entry gate the GSP spawns the player at, and
  a plain gate-walk from the neighbouring cell gives exactly that."""
  opposite = {"north": "south", "south": "north",
              "east": "west", "west": "east"}
  move (name, {"gw": {"dir": opposite[entryDir]}})
  mine ()
  p = unwrap (gsp.getplayerinfo (name))
  assert p["in_channel"] and p["segment"] == seg, \
      "%s did not walk into %s: %s" % (name, seg, p["segment"])

  segInfo = unwrap (gsp.getsegmentinfo (seg["x"], seg["y"]))
  potions = sum (it["quantity"] for it in p["inventory"]
                 if it["item_id"] == "health_potion" and it["slot"] == "bag")
  spec = {
    "seed": segInfo["seed"], "depth": segInfo["depth"],
    "hp": p["hp"], "max_hp": p["max_hp"], "stats": EffectiveStats (p),
    "potions": potions, "entry_direction": entryDir,
    "exit_direction": entryDir, "collect_gold": True,
    "constraints": Constraints (segInfo),
  }
  out = subprocess.run ([playBinary, "--solve", json.dumps (spec)],
                        capture_output=True, text=True)
  proof = json.loads (out.stdout.strip ().splitlines ()[-1])
  if not proof["survived"]:
    raise RuntimeError ("gold run on %s did not survive for %s"
                        % (seg, name))

  gold0 = p["gold"]
  move (name, {"gw": {"dir": entryDir, "settlement": {
    "results": {"survived": True, "xp": proof["xp"], "gold": proof["gold"],
                "kills": proof["kills"]},
    "actions": proof["actions"],
  }}})
  mine ()
  p = unwrap (gsp.getplayerinfo (name))
  assert not p["in_channel"], "%s's gold run was not settled" % name
  return p["gold"] - gold0
