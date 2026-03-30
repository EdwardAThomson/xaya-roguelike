# Segment Lifecycle

## Overview

Segments are permanent dungeon locations in the world graph. Each segment
has a deterministic layout generated from its seed. This document describes
the full lifecycle of a segment from discovery through confirmation, retry,
and pruning.

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
Block 100: Alice at segment 3, facing unknown territory east.

Block 100: Alice submits {"d": {"depth": 2, "dir": "east"}}
   → Provisional segment 4 created (confirmed=0)
   → seed = dungeon_id + ":" + txid
   → Gates stored, bidirectional links created (3↔4)
   → Alice's last_discover_height = 100
   → No visit created

Block 101: Alice submits {"ec": {"id": 4}}
   → Discoverer privilege: Alice can enter from segment 3
   → in_channel = 1, current_segment = 4
   → Solo active visit created

Block 101-???: Alice plays dungeon locally
   → Frontend records action log
   → Alice fights monsters, picks up items, reaches south gate

Block 105: Alice submits {"xc": {"id": 1, "results": {...}, "actions": [...]}}
   → GSP replays 47 actions on fresh DungeonGame
   → Results match claims → ACCEPTED
   → Alice: xp += 68, gold += 15, kills += 3, hp = 72
   → Alice: in_channel = 0, current_segment updated via exit gate link
   → Visit completed
   → SEGMENT 4 CONFIRMED (confirmed=1)
   → Other players can now travel to segment 4
```

---

## Flow 2: Death + Retry

Discoverer dies but tries again honestly.

```
Block 100: Alice discovers segment 4 (provisional)

Block 101: Alice enters channel for segment 4

Block 101-???: Alice plays locally, gets killed by a Minotaur on turn 31

Block 105: Alice submits honest death:
   {"xc": {"id": 1, "results": {
     "survived": false, "xp": 20, "gold": 0, "kills": 1
   }, "actions": [... 31 actions ...]}}
   → GSP replays 31 actions → player dies on action 31 → matches
   → ACCEPTED
   → Alice: hp = 0, deaths += 1, in_channel = 0
   → Visit completed (but segment NOT confirmed — she didn't survive)
   → Alice is back at segment 3 (source segment)
   → Segment 4 remains provisional

Block 106: Alice uses health potion → hp = 20

Block 107: Alice enters channel for segment 4 again
   → Still the discoverer, still provisional, still linked → allowed
   → New visit created

Block 107-???: Alice plays again (same dungeon — same seed = same layout)
   → Alice knows the Minotaur's location from her first attempt
   → She takes a different path, avoids the Minotaur, exits via gate

Block 112: Alice submits successful run
   → Replayed, verified → ACCEPTED
   → SEGMENT 4 CONFIRMED
```

---

## Flow 3: Abandonment + Pruning

Discoverer fails and doesn't retry. Segment is pruned.

```
Block 100: Alice discovers segment 4 (provisional)

Block 101: Alice enters channel

Block 101-???: Alice plays, dies, closes browser (disconnect)
   → No "xc" move submitted

Block 301: Solo channel timeout fires (200 blocks)
   → Force-settle: Alice gets death, in_channel = 0, hp = 0
   → Visit completed (no rewards)
   → Segment 4 still provisional

Block 301-400: Alice doesn't retry (maybe she went to bed)

Block 400: Prune timer fires (VISIT_OPEN_TIMEOUT + SOLO_VISIT_ACTIVE_TIMEOUT = 300 blocks)
   → Segment 4 has confirmed=0 and no active visits
   → Segment 4 PRUNED: segment, gates, links all deleted
   → The east direction from segment 3 is now open

Block 500: Bob discovers east from segment 3
   → NEW segment created (different txid → different seed → different dungeon)
   → Bob's segment is a completely fresh dungeon
   → Alice's failed dungeon is gone forever
```

---

## Flow 4: Another Player Wants the Same Direction

Only one player can discover in a given direction from a segment.

```
Block 100: Alice discovers east from segment 3 → segment 4 (provisional)

Block 101: Bob tries to discover east from segment 3
   → REJECTED: segment_links already has (3, "east") → 4
   → Bob must wait for Alice to confirm or for pruning

Block 101: Bob discovers north from segment 3 instead → segment 5 (provisional)
   → Different direction, no conflict
   → Bob and Alice explore in parallel

Block 105: Alice confirms segment 4
   → Bob can now TRAVEL to segment 4 (it's confirmed)
   → Bob can also discover further east from segment 4
```

---

## Flow 5: Post-Confirmation

Once confirmed, a segment is permanent. Any player can visit.

```
Block 200: Segment 4 is confirmed (Alice discovered it)

Block 200: Bob travels east to segment 4
   → Link exists, segment confirmed → travel allowed

Block 201: Bob enters channel for segment 4
   → Same seed → same dungeon layout
   → But monsters/items are fresh (each visit is independent)
   → Bob plays and exits → his results recorded

Block 300: Charlie enters segment 4
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

4. **Pruning is permanent**: Once pruned, the segment is gone. A new
   discovery in the same direction creates a completely new dungeon.

5. **No scouting for others**: Because pruning deletes the segment entirely,
   a player can't scout a dungeon, die, and send a friend to clear the
   known layout. The friend would need to discover again, getting a new seed.

6. **Parallel exploration**: Multiple players can discover in different
   directions simultaneously. The world graph grows in parallel.

7. **Natural progression**: Deeper segments (higher depth parameter) have
   harder monsters. Players naturally progress outward from the origin
   as they level up and acquire better equipment.
