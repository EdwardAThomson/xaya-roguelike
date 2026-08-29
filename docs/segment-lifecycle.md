# Segment Lifecycle

## Overview

Segments are permanent dungeon locations in the world graph. Each segment
has a deterministic layout generated from its seed. This document describes
the full lifecycle of a segment from discovery through confirmation, retry,
and pruning.

A segment is named by its world coordinate `(x, y)` and has no other
identity — the flows below refer to segments that way. The hub is `(0, 0)`.
Visits, being sessions rather than places, do still carry an integer `id`.

---

## States

```
                    ┌──────────────┐
   discover move →  │  PROVISIONAL │  ← confirmed=0
                    │  (locked to  │
                    │  discoverer) │
                    └──────┬───────┘
                           │
              ┌────────────┼────────────┐
              │            │            │
              ▼            ▼            ▼
        ┌──────────┐ ┌──────────┐ ┌──────────┐
        │ CONFIRMED│ │  RETRY   │ │  PRUNED  │
        │ (perm.)  │ │ (still   │ │ (deleted)│
        │          │ │  prov.)  │ │          │
        └──────────┘ └──────────┘ └──────────┘
        survived      died/failed   timeout +
        via gate      or timeout    no retry
```

---

## Flow 1: Successful Discovery

The happy path — discoverer completes the dungeon and exits via a gate.

```
Block 100: Alice at segment (1, 0), facing unknown territory east.

Block 100: Alice submits {"d": {"depth": 2, "dir": "east"}}
   → Provisional segment (2, 0) created (confirmed=0)
   → The cell claimed is fixed by the direction, not by the move: east of
     (1, 0) is (2, 0) and nothing else. The claimed "depth" is ignored;
     the GSP derives depth 2 from the coordinate.
   → seed = dungeon_id + ":" + txid
   → Gates stored, bidirectional links created ((1,0) ↔ (2,0))
   → Alice's last_discover_height = 100
   → No visit created

Block 101: Alice submits {"ec": {"x": 2, "y": 0}}
   → Discoverer privilege: Alice can enter from (1, 0)
   → in_channel = 1, current_x/current_y = the segment's coordinate
   → Solo active visit created

Block 101-???: Alice plays dungeon locally
   → Frontend records action log
   → Alice fights monsters, picks up items, reaches south gate

Block 105: Alice submits {"xc": {"id": 1, "results": {...}, "actions": [...]}}
   → GSP replays 47 actions on fresh DungeonGame
   → Results match claims → ACCEPTED
   → Alice: xp += 68, gold += 15, kills += 3, hp = 72
   → Alice: in_channel = 0, position moves to the cell beyond the exit gate
   → Visit completed
   → SEGMENT (2, 0) CONFIRMED (confirmed=1)
   → Other players can now travel to (2, 0)
```

---

## Flow 2: Death + Retry

Discoverer dies but tries again honestly.

```
Block 100: Alice discovers (2, 0) (provisional)

Block 101: Alice enters channel for (2, 0)

Block 101-???: Alice plays locally, gets killed by a Minotaur on turn 31

Block 105: Alice submits honest death:
   {"xc": {"id": 1, "results": {
     "survived": false, "xp": 20, "gold": 0, "kills": 1
   }, "actions": [... 31 actions ...]}}
   → GSP replays 31 actions → player dies on action 31 → matches
   → ACCEPTED
   → Alice: hp = 0, deaths += 1, in_channel = 0
   → Visit completed (but segment NOT confirmed — she didn't survive)
   → Alice is back at (1, 0) (source segment)
   → (2, 0) remains provisional

Block 106: Alice uses health potion → hp = 20

Block 107: Alice enters channel for (2, 0) again
   → Still the discoverer, still provisional, still linked → allowed
   → New visit created

Block 107-???: Alice plays again (same dungeon — same seed = same layout)
   → Alice knows the Minotaur's location from her first attempt
   → She takes a different path, avoids the Minotaur, exits via gate

Block 112: Alice submits successful run
   → Replayed, verified → ACCEPTED
   → SEGMENT (2, 0) CONFIRMED
```

---

## Flow 3: Abandonment + Pruning

Discoverer fails and doesn't retry. Segment is pruned.

```
Block 100: Alice discovers (2, 0) (provisional)

Block 101: Alice enters channel

Block 101-???: Alice plays, dies, closes browser (disconnect)
   → No "xc" move submitted

Block 301: Solo channel timeout fires (started_height + 200 blocks)
   → Force-settle: the abandoned run ends PENALTY-FREE — no death, no HP
     loss, no gold loss (a timeout is a disconnect/idle, not a death). The
     only cost is that the unsettled run's loot/xp is never banked.
   → in_channel = 0; Alice is stepped back one segment (to (1, 0), the
     side she entered from), exactly like the death knock-back but with no
     penalty
   → Visit completed (no rewards)
   → (2, 0) is provisional, so it is PRUNED immediately at force-settle
     (anti-grief: its world coordinate must be released): segment, gates,
     links, and the segment's visits all deleted
   → The east direction from (1, 0) is now open again

   (Confirmed segments have no coordinate to release, so their abandoned
   runs are NOT force-settled at all — the visit stays active and the
   player resumes exactly where they were on reconnect.)

Block 500: Bob discovers east from (1, 0)
   → NEW segment created at (2, 0) — same cell, different txid
     → different seed → different dungeon
   → Bob's segment is a completely fresh dungeon with no visit history
   → Alice's failed dungeon is gone forever
```

---

## Flow 4: Another Player Wants the Same Direction

Only one player can discover in a given direction from a segment.

```
Block 100: Alice discovers east from (1, 0) → (2, 0) (provisional)

Block 101: Bob tries to discover east from (1, 0)
   → REJECTED: segment_links already has ((1,0), "east") → (2,0)
   → Bob must wait for Alice to confirm or for pruning

Block 101: Bob discovers north from (1, 0) instead → (1, 1) (provisional)
   → Different direction, different cell, no conflict
   → Bob and Alice explore in parallel

Block 105: Alice confirms (2, 0)
   → Bob can now TRAVEL to (2, 0) (it's confirmed)
   → Bob can also discover further east from (2, 0), claiming (3, 0)
```

---

## Flow 5: Post-Confirmation

Once confirmed, a segment is permanent. Any player can visit.

```
Block 200: (2, 0) is confirmed (Alice discovered it)

Block 200: Bob travels east to (2, 0)
   → Link exists, segment confirmed → travel allowed

Block 201: Bob enters channel for (2, 0)
   → Same seed → same dungeon layout
   → But monsters/items are fresh (each visit is independent)
   → Bob plays and exits → his results recorded

Block 300: Charlie enters (2, 0)
   → Same dungeon layout, fresh monsters
   → Charlie plays independently

Note: Each visit is a separate channel with its own DungeonGame instance.
The segment seed is shared, but each player's experience is independent.
Monster drops are RNG-dependent on the player's action sequence, so
different players get different drops even in the same dungeon layout.
```

---

## Timing Constants

| Parameter | Blocks | ~Time (Polygon 2s blocks) |
|-----------|--------|--------------------------|
| Discovery cooldown | 50 | ~1.5 minutes |
| Visit open timeout | 100 | ~3 minutes |
| Solo channel timeout | 200 | ~6 minutes |
| Prune timer | 300 (100+200) | ~10 minutes |

---

## Key Properties

1. **Discoverer exclusivity**: Only the discoverer can enter a provisional
   segment. Others must wait for confirmation.

2. **Retry allowed**: The discoverer can retry their provisional segment
   after dying, as long as the segment hasn't been pruned.

3. **Same dungeon on retry**: Retrying the same segment produces the same
   dungeon (same seed). The player has knowledge from previous attempts.
   This is intentional — learning from failure is part of the game.

4. **Pruning is permanent**: Once pruned, the segment is gone, along with
   its visits and their results. A new discovery into the same cell creates
   a completely new dungeon that inherits nothing from the dead one.

5. **No scouting for others**: Because pruning deletes the segment entirely,
   a player can't scout a dungeon, die, and send a friend to clear the
   known layout. The friend would need to discover again, getting a new seed.

6. **Parallel exploration**: Multiple players can discover in different
   directions simultaneously. The world graph grows in parallel.

7. **Natural progression**: Deeper segments have harder monsters. A
   segment's depth is its Manhattan distance from the hub
   (`|world_x| + |world_y|`) — the minimum number of gate-steps out —
   computed authoritatively at discovery from the segment's coordinates, not
   taken from the client's claimed depth (which is only range-checked). This
   makes depth symmetric and direction-aware: heading back toward the hub
   lowers depth, and two segments equidistant from the hub share a depth
   regardless of the path taken to find them. Players naturally progress
   outward from the origin as they level up and acquire better equipment.
