# Security Model — Attack Vectors & Mitigations

## Overview

This document covers known attack vectors against the blockchain roguelike
and the mitigations in place or planned. The game runs on an EVM chain via
Xaya X, with game logic processed by a deterministic GSP.

---

## Segment Scenarios

### Protocol: Clean Segment Run

A legitimate player discovering and completing a dungeon segment.

```
1. DISCOVER — Player submits: {"d": {"depth": 1, "dir": "east"}}
   → On-chain tx mined in block N
   → GSP creates provisional segment (confirmed=0)
   → Segment seed = dungeon_id + ":" + txid
   → Gates stored in segment_gates, links in segment_links
   → Player's last_discover_height set to N
   → No visit created (player must enter channel separately)

2. ENTER CHANNEL — Player submits: {"ec": {"id": 1}}
   → GSP checks: player is discoverer of linked provisional segment
   → GSP sets in_channel=1, current_segment=1
   → Solo active visit created (visit_id=1)

3. LOCAL PLAY — Player's frontend runs DungeonGame locally
   → DungeonGame::Create(seed, depth, stats, hp, maxHp, potions)
   → Seed produces identical dungeon to what GSP would generate
   → Player takes actions: move, attack, pickup, use potion, enter gate
   → Each action recorded in actionLog[]
   → Example: 47 actions over ~3 minutes of play
   → Result: survived=true, xp=68, gold=15, kills=3, exit_gate="south"

4. EXIT CHANNEL — Player submits: {"xc": {"id": 1, "results": {
     "survived": true, "xp": 68, "gold": 15, "kills": 3,
     "hp_remaining": 72, "exit_gate": "south"
   }, "actions": [
     {"type": "move", "dx": 1, "dy": 0},
     {"type": "move", "dx": 0, "dy": 1},
     {"type": "move", "dx": 1, "dy": 0},  ← attack (monster at target)
     {"type": "pickup"},
     {"type": "use", "item": "health_potion"},
     ... (47 actions total)
     {"type": "gate"}
   ]}}
   → On-chain tx mined

5. GSP VERIFICATION — ProcessExitChannel runs:
   a. Look up segment seed + depth from visit → segment table
   b. Read player stats, HP, potions from on-chain state
   c. Parse the 47 actions from the tx
   d. Create fresh DungeonGame from seed (identical dungeon)
   e. Replay all 47 actions one by one
   f. Compare replay result to claimed result:
      - survived: true == true ✓
      - xp: 68 == 68 ✓
      - gold: 15 == 15 ✓
      - kills: 3 == 3 ✓
   g. ACCEPTED — results match

6. STATE UPDATE — GSP applies replay results:
   → Player: xp += 68, gold += 15, kills += 3, hp = 72
   → Player: in_channel = 0, visits_completed += 1
   → Player: current_segment updated via exit_gate link
   → Visit marked completed
   → Segment confirmed (confirmed=1) — now permanent
   → Loot from replay added to inventory (subject to MAX_INVENTORY=20)
   → Other players can now travel to this segment
```

---

### Protocol: Malicious Segment Run (Fabricated Results)

A cheating player tries to claim rewards they didn't earn.

```
1. DISCOVER + ENTER — Same as clean run, steps 1-2.

2. CHEAT ATTEMPT — Player modifies frontend or fabricates results:
   → Claims: survived=true, xp=999, gold=999, kills=50
   → But submits empty actions: []
   → Or submits actions that don't match the claimed outcome

3. EXIT CHANNEL — Player submits: {"xc": {"id": 1, "results": {
     "survived": true, "xp": 999, "gold": 999, "kills": 50,
     "hp_remaining": 100
   }, "actions": []}}

4. GSP VERIFICATION — ProcessExitChannel runs:
   a. Look up seed, create fresh DungeonGame
   b. Parse actions: empty list (0 actions)
   c. Replay: game never started, player at spawn point
   d. Replay result: survived=false, xp=0, gold=0, kills=0
   e. Compare to claimed: survived=true vs false ✗ MISMATCH
   f. REJECTED — move is invalid, no state changes

5. CONSEQUENCE:
   → Player remains in_channel=1 (stuck)
   → Visit remains active (not settled)
   → No XP, gold, or loot awarded
   → Player must submit honest action proof to exit
   → Or wait for SOLO_VISIT_ACTIVE_TIMEOUT (200 blocks)
     → Force-settle: death, in_channel cleared, hp=0
     → Provisional segment pruned (no valid completion)

6. COST TO ATTACKER:
   → Gas spent on the rejected transaction (wasted)
   → Stuck in channel until timeout (~6 min on Polygon)
   → Death penalty (hp=0, must heal before next action)
   → Discovery cooldown still applies (50 blocks)
   → No rewards gained
```

---

### Protocol: Malicious Segment Run (Tampered Actions)

A player tries to submit a modified action sequence.

```
1. SETUP — Player plays honestly for 30 actions, finds good loot.
   Player dies on action 31 (monster kills them).

2. CHEAT ATTEMPT — Player removes the last action (the one where
   they walked into the monster that killed them) and claims survived.
   Submits 30 actions instead of 31.

3. GSP VERIFICATION:
   a. Replay 30 actions — player is alive at action 30
   b. Replay result: survived=false (game didn't end via gate)
   c. Player claims survived=true
   d. MISMATCH → REJECTED

   Note: Even if the player claims survived=false to match, the
   xp/gold/kills from the replay of 30 actions may differ from
   the claimed values (the 31st action may have killed a monster
   that gave XP). Any mismatch → rejected.

4. ALTERNATIVE CHEAT — Player replays honestly but adds extra
   actions at the end (trying to get more kills/loot):
   → The extra actions execute on the replay game state
   → If the game already ended (via gate), extra actions return false
   → If the game didn't end, extra actions may produce different
     RNG outcomes than expected (because the monster drops are
     deterministic — adding an extra "wait" changes all future RNG)
   → The claimed results won't match → REJECTED
```

---

## List of attacks and mitigations

### 1. Fabricated Dungeon Results

**Attack**: Player claims XP/gold/loot they didn't earn by submitting
false results in the `"xc"` (exit channel) move.

**Mitigation**: Action replay verification. The `"xc"` move requires the
player's complete action sequence. The GSP replays the actions on a fresh
DungeonGame from the segment seed. Claimed results must match the replay
exactly — mismatches are rejected. The deterministic dungeon (SHA-256 seed
→ MT19937 RNG → identical layout) makes fabrication impossible.

**Status**: Implemented.

---

### 2. World Map Pollution (Garbage Segments)

**Attack**: Player rapidly discovers segments with no intention of completing
them, polluting the world graph with provisional segments that block other
players.

**Mitigations**:

- **Discovery cooldown**: 50 blocks (~1.5 min on Polygon) between discoveries.
  Legitimate players spend 5-10 minutes per dungeon, so this is invisible to
  honest play but makes spam expensive and slow.
- **Provisional segments**: A discovered segment is provisional until the
  discoverer completes a valid run. If the run times out or fails validation,
  the segment is pruned from the world graph entirely (segment, gates, and
  links all deleted).
- **Gas cost**: Each discovery is an on-chain transaction. Spamming costs real
  money.

**Status**: Implemented. E2E tested.

---

### 3. Channel Griefing (Blocking Segments)

**Attack**: Player enters a channel for a segment and never completes it,
blocking other players from visiting that segment.

**Mitigations**:

- **Solo channel timeout**: 200 blocks (~6 minutes). If the channel isn't
  settled within this window, the visit is force-settled with no rewards and
  the player receives a death.
- **Active visit limit**: Only one active visit per segment at a time. Once
  the timeout fires, the segment is free for others.
- **Cooldown after channel exit**: Prevents rapid re-entry to grief the same
  segment repeatedly.

**Status**: Implemented. E2E tested (solo timeout at 200 blocks).

---

### 4. Cross-Instance Replay

**Attack**: Submit moves from one game instance to another (different
deployment on the same chain, or different chain entirely).

**Mitigations**:

- **dungeon_id**: Each game instance has a unique `--dungeon_id` mixed into
  all segment seeds. Different dungeon IDs → different dungeon layouts →
  action replays fail verification.
- **Chain isolation**: Different EVM chains have different XayaAccounts
  contracts. Transactions can't cross chains.
- **Segment seeds from txid**: Each segment's seed includes the transaction
  hash, which is unique per chain and per deployment.

**Status**: Implemented.

---

### 5. Provisional Segment Travel Race

**Attack**: Alice discovers a provisional segment. Bob travels to it before
Alice completes her run. Alice's run fails/times out → segment is pruned.
Bob is now stranded at a non-existent segment.

**Mitigations**:

- **Block travel to provisional segments**: Players cannot travel to a segment
  that hasn't been validated yet. The `"t"` move should check that the
  destination segment is confirmed (not provisional).
- **Discoverer exclusivity**: Only the discoverer can be in the provisional
  segment. Others must wait for confirmation.

**Status**: Implemented. E2E tested.

---

### 6. Front-Running Segment Discovery

**Attack**: Alice broadcasts a discover transaction. Bob sees it in the
mempool and submits his own discover in the same direction, hoping his gets
mined first.

**Mitigations**:

- **Transaction ordering**: On EVM chains, transaction ordering within a block
  is determined by the block producer (typically by gas price). This is a
  general blockchain problem, not specific to our game.
- **Different seeds**: Even if both discover the same direction, each gets a
  different seed (from their txid), producing a different dungeon.
- **Direction locking**: The `segment_links` table prevents duplicate links.
  The first transaction to be processed wins; the second is rejected.

**Status**: Partially mitigated by existing checks. Full front-running
protection would require commit-reveal or similar schemes.

---

### 7. Modified Frontend

**Attack**: Player modifies the frontend to reveal the full map (ignoring FOV),
see through walls, or show monster positions.

**Mitigations**:

- **Not a real threat**: The dungeon is generated from a known seed. A
  determined player could always regenerate the full map from the seed.
  FOV is a gameplay experience feature, not a security boundary.
- **Anti-cheat is server-side**: All results are verified by action replay.
  Seeing the map doesn't let you fabricate results.

**Status**: Accepted risk. FOV is cosmetic, not security.

---

### 8. Transaction Spam / Denial of Service

**Attack**: Flood the chain with invalid moves to slow down the GSP or
bloat the database.

**Mitigations**:

- **Gas cost**: Every transaction costs gas. Spamming is expensive.
- **Move validation**: Invalid moves are rejected early in the parser
  (HandleOperation) before any database writes. Minimal processing cost.
- **Existing chain protections**: EVM chains have block gas limits that
  naturally throttle transaction volume.

**Status**: Mitigated by blockchain economics and early validation.

---

### 9. Inventory Manipulation

**Attack**: Exploit the channel exit to add items beyond the inventory limit,
or duplicate items.

**Mitigations**:

- **Inventory limit (MAX_INVENTORY=20)**: Enforced on channel exit and
  settlement. Excess items are dropped.
- **Replay verification**: Loot comes from the replay, not from claims.
  Can't fabricate items that the replay didn't produce.
- **Item consumption tracking**: Potions used during the dungeon session
  are consumed from the starting inventory. The replay tracks this.

**Status**: Implemented.

---

### 10. Block Timing Attacks

**Attack**: Manipulate block timestamps to affect randomness or timeouts.

**Mitigations**:

- **Block-based timeouts**: All timeouts use block counts, not timestamps.
  Block counts are monotonically increasing and consensus-agreed.
- **Seed from txid**: Random seeds come from transaction hashes, not
  timestamps. Miners can't predict transaction hashes.

**Status**: Mitigated by design.

---

### 11. Input Validation Attacks

**Attack**: Submit malformed or boundary-case moves to corrupt state
or crash the GSP.

**Vectors tested**:

- Allocate stat with 0 stat_points → rejected
- Invalid stat name ("hacking") → rejected
- Use item not in inventory → rejected
- Equip to invalid slot name → rejected
- Equip nonexistent rowid → rejected
- Equip another player's item rowid → rejected
- Register an already-registered player → rejected
- Multi-action move (two actions in one tx) → rejected
- Garbage move data (non-JSON-object) → rejected gracefully
- Empty move object → rejected

**Mitigations**: HandleX functions in moveparser.cpp validate every field
before calling ProcessX. Invalid moves are logged and silently dropped.
No state changes occur.

**Status**: Implemented. All vectors E2E tested.

---

### 12. State Boundary Violations

**Attack**: Perform actions that should be blocked in the player's
current state (in channel, dead, at wrong segment).

**Vectors tested**:

- Travel while in channel → rejected
- Discover while in channel → rejected
- Equip while in channel → rejected
- Use item while in channel → rejected
- Exit channel when not in one → rejected
- Travel with 0 HP → rejected
- Enter channel with 0 HP → rejected

**Mitigations**: Each HandleX function checks the relevant state
preconditions (PlayerInChannel, HP > 0, correct segment, etc.)
before processing.

**Status**: Implemented. All vectors E2E tested.

---

## Constants

| Parameter | Value | Purpose |
|-----------|-------|---------|
| VISIT_OPEN_TIMEOUT | 100 blocks | Open visits expire |
| VISIT_ACTIVE_TIMEOUT | 1000 blocks | Active visit force-settle |
| SOLO_VISIT_ACTIVE_TIMEOUT | 200 blocks | Solo channel timeout |
| DISCOVERY_COOLDOWN | 50 blocks | Between segment discoveries |
| MAX_INVENTORY | 20 | Inventory size limit |
| ENCOUNTER_CHANCE | 20% | Random encounters during travel |

---

## E2E Adversarial Test Suite

`devnet/adversarial_test.py` — 45 tests across 8 categories, run against a
full devnet stack (anvil + Xaya X + rogueliked).

| Category | Tests | Description |
|----------|-------|-------------|
| 1. Fabricated Results | 10 | Fake XP, gold, survival, mismatched replay, negative values |
| 2. World Map Pollution | 3 | Cooldown enforcement, discovery after cooldown, provisional existence |
| 3. Channel Griefing | 4 | Double entry, timeout force-settle (1000 blocks), death penalty |
| 4. Cross-Player | 4 | Exit another's visit, equip another's item, unregistered player |
| 5. Provisional Segments | 4 | Travel to provisional, non-discoverer entry, discoverer privilege, post-confirm access |
| 6. Input Validation | 9 | Invalid stats, items, slots, rowids, duplicates, garbage data |
| 7. State Boundaries | 7 | Actions while in channel, actions while dead, exit when not in channel |
| 8. Spam Resilience | 2 | 20 rapid invalid moves, wrong game ID |
| **Total** | **45** | All passing |

Run with:
```bash
source ~/Explore/xayax/.venv/bin/activate
python3 devnet/adversarial_test.py
```

---

## Future Considerations

- **Multi-player channels**: Require full state signing and dispute resolution
  (Phase 14). The OpenChannel framework from libxayagame handles this.
- **VRF-based loot**: Verifiable Random Function for private loot generation
  that's provably fair but hidden until revealed.
- **Boss instances / PvP**: Inside channels, require careful design around
  state agreement between multiple parties.
- **Economic attacks**: Gold inflation, market manipulation, item duplication.
  Need economic modeling before deploying with real value.
- **Front-running protection**: Two players discovering same direction in
  same block. Existing direction-locking prevents duplicate links, but
  commit-reveal would add stronger guarantees.
- **Cross-instance replay**: Tested via dungeon_id design but not E2E tested
  (would require two separate GSP instances). Architecturally sound —
  different dungeon_id → different seeds → replay fails verification.
