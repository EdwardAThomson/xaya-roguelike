# Blockchain Roguelike — Development Plan

## Project Overview

A blockchain roguelike built on the Xaya game framework (`libxayagame`), targeting EVM chains (Polygon) via the Xaya X bridge. The game has a permanent dungeon map discovered by players, with persistent characters, inventory, and progression stored on-chain.

**Location**: `~/Projects/xayaroguelike/`
**Game ID**: `"rog"`
**Existing JS roguelike** (reference for game mechanics): `~/Projects/NewRoguelike/`
**libxayagame source**: `~/Explore/libxayagame/`

---

## Layer 1: On-Chain GSP — COMPLETE

All 7 phases done. 54 unit tests passing. Daemon builds and links.

### Phase 1: Project Skeleton — DONE
- CMakeLists.txt with FetchContent for deps + optional full daemon build
- Schema embedding via concat_files.cmake
- 7 SQLite tables: players, inventory, known_spells, segments, segment_participants, segment_results, loot_claims

### Phase 2: Player Registration — DONE
- `{"r": {}}` move creates player with default stats (str/dex/con/int = 10)
- Starting items: short_sword (weapon), leather_armor (body), 3x health_potion (bag)
- Rejects duplicates, invalid formats, multi-action moves

### Phase 3: State JSON + RPC — DONE
- StateJsonExtractor: GetPlayerInfo, ListSegments, GetSegmentInfo, FullState
- Custom RPC server: getplayerinfo, listsegments, getsegmentinfo
- Framework RPC: getcurrentstate, getpendingstate, waitforchange, stop, etc.
- RPC stub auto-generated from rpc-stubs/rog.json

### Phase 4: Segment Discovery & Joining — DONE
- `{"d": {"depth": N}}` discovers segment, discoverer is first participant
- `{"j": {"id": N}}` joins open segment, auto-activates when full (4/4)
- `{"lv": {"id": N}}` leaves segment (discoverer blocked from leaving)
- Validation: registered player, not in another segment, depth 1-20

### Phase 5: Settlement — DONE
- `{"s": {"id": N, "results": [...]}}` settles active segments
- XP/gold/loot distribution, multi-level-up (JS formula: floor(100*pow(level,1.5)))
- Each level-up grants +1 skill point, +1 stat point
- Loot inserted into inventory, claims recorded
- NOTE: discoverer-only settlement is placeholder — will be replaced by channel proofs

### Phase 6: Stat Allocation + Timeouts — DONE
- `{"as": {"stat": "strength"}}` spends stat point on str/dex/con/int
- Open segments expire after 100 blocks → status "expired"
- Active segments force-settle after 1000 blocks → all players die, no rewards

### Phase 7: Pending Moves — DONE
- Tracks pending registrations, discovers, joins in mempool
- Wired into main.cpp via PendingMoves class

### Post-plan: Chain-Agnostic Genesis — DONE
- `--genesis_height` and `--genesis_hash` flags replace hardcoded chain switch
- Same binary works on Polygon, Ganache, or any future chain
- No chain-specific code in game logic

---

## Layer 2: Next Development Phases

### Phase 8: End-to-End Testing on Local EVM — DONE

Full stack validated: local Anvil chain → Xaya X bridge → rogueliked.

- Installed Foundry (anvil/forge), mypp, Xaya X (xayax-eth) from source
- Xaya contracts (WCHI, XayaAccounts, XayaPolicy) deployed automatically via Python
- rogueliked connects to Xaya X with `--xaya_rpc_protocol=2`; version check disabled for Xaya X
- Smoke test (`devnet/smoke_test.py`): registers player, discovers segment, verifies state via RPC
- All documented in SETUP.md

### Phase 9: Persistent World Map Refactor — DONE

Segments are now permanent map locations. Visits are the temporal entity.

- `segments` table: permanent (id, discoverer, seed, depth, max_players, created_height)
- `visits` table: temporary expeditions (id, segment_id, initiator, status, heights)
- `visit_participants`, `visit_results`, `loot_claims` reference visit IDs
- New move: `{"v": {"id": N}}` starts a visit to an existing segment
- Join/leave/settle now reference visit IDs, not segment IDs
- "discoverer" role on visits → "initiator" (may differ from segment discoverer)
- One active visit per segment at a time
- New RPCs: `listvisits`, `getvisitinfo`; `listsegments` returns permanent map data
- 63 unit tests passing

### Phase 10: JS/TS Frontend
**Goal**: A browser client that connects to the GSP and renders game state.

- Connect to GSP via WebSocket (using gsp-websocket-server.py from libxayagame)
- Display player stats, inventory, segment map
- Submit moves via wallet connection (Xaya X / MetaMask for EVM)
- Render the dungeon map from segment seeds (port dungeon.js generation)
- Could reuse renderer/sprites from the existing JS roguelike

### Phase 11: Deterministic Dungeon Generation in C++ — DONE

Ported dungeon generation from JS roguelike to C++.

- 80x40 grid with Wall/Floor/Gate tiles, generated deterministically from seed+depth
- 8-15 rooms (4-8 wide, 4-7 tall) with overlap checking and 1-tile buffer
- L-shaped corridors connecting consecutive rooms + loop closure
- 4 cardinal gates on dungeon edges, each connected to nearest room
- Seeded via std::mt19937 from hash of seed string — same seed = identical dungeon
- `Dungeon::Generate(seed, depth)` static factory, `GetRandomFloorPosition()` for spawns
- 12 unit tests: determinism, room bounds, no overlap, connectivity (flood fill), gates, tile counts
- 75 total tests passing

### Phase 12: On-Chain Overworld Layer — DONE
**Goal**: On-chain bookkeeping for player position, HP, inventory, and segment traversal.

The on-chain world is a safe meta-layer. Actual dungeon exploration (movement, combat, items, monsters) happens in solo channels (Phase 13). The overworld tracks where players are, lets them travel between segments, manage inventory, and enter/exit channel sessions.

**On-chain state added:**
- Player HP (derived from constitution: `50 + con * 5`), current_segment, in_channel
- Segment gates table (cached gate positions per segment)
- Segment links table (overworld graph connecting segments via gates)
- Visit results now include hp_remaining and exit_gate
- 92 unit tests passing

**New moves:**
- `{"t": {"dir": "east"}}` — travel to adjacent segment (random encounter chance seeded by txid)
- `{"ui": {"item": "health_potion"}}` — use consumable item
- `{"eq": {"rowid": N, "slot": "weapon"}}` — equip item
- `{"uq": {"rowid": N}}` — unequip item to bag
- `{"ec": {"id": N}}` — enter segment for channel play (sets in_channel=1, creates solo visit)
- `{"xc": {"id": N, "results": {...}}}` — exit channel, settle results (XP/gold/loot/hp/exit_gate)

**Modified moves:**
- Discover now requires direction: `{"d": {"depth": N, "dir": "east"}}` — creates linked segment with constrained gate

**Design decisions:**
- On-chain world = safe overworld (travel, inventory, trading)
- Dungeon exploration = solo channels (Phase 13) — full gameplay, local execution
- Random encounters during travel: 20% chance, 5-15 damage, never kills (clamps to HP=1)
- Segment 0 is the origin safe zone, not a real dungeon
- Death in channel: HP=0, must heal before traveling again

### Phase 13: Solo Dungeon Channels — IN PROGRESS
**Goal**: Real-time dungeon exploration via solo game channels.

**Sub-phase 13a: Dungeon Gameplay Engine — DONE**
- DungeonGame class: turn-based dungeon session (movement, combat, items, gates)
- Monster database: 12 types scaled by depth (Giant Rat through Dark Mage)
- Combat math: attack/defense/crit/dodge from player stats + equipment bonuses
- Monster AI: detect → chase → attack (deterministic, seeded)
- Ground items: health potions, equipment, gold — spawned deterministically
- Actions: Move (8-dir, into monster = attack), Pickup, UseItem, EnterGate, Wait

**Item System Overhaul — DONE**
- 30 item definitions with real stats (attack, defense, stat bonuses) ported from JS roguelike
- `ComputePlayerStats()` computes effective combat stats from base + equipped items
- Equip/unequip updates max_hp when constitution-boosting items change
- Health potions work from starting inventory inside dungeon sessions (20 HP / 50 HP)
- Ground items spawn from database scaled by depth
- State JSON shows effective_stats (attack_power, defense, equip bonuses)
- Playthrough tests validate full gameplay loop with real item stats
- 122 tests passing

**Sub-phase 13b+c: Protobuf + BoardRules — DONE**
- Proto definitions: DungeonState (full game state + RNG) and DungeonMove (oneof action types)
- DungeonBoardState wraps DungeonGame in channel framework (WhoseTurn, TurnCount, ApplyMoveProto)
- DungeonBoardRules creates parsed states from serialized protos
- Game state serializes/deserializes to/from protobuf (including RNG state)
- Channel library (libroguelike_channel.so) builds alongside main library and daemon

- Player opens a channel when entering a segment (on-chain `"ec"` move)
- Full dungeon gameplay happens locally: movement on 80x40 grid, monster combat, item pickup, gate traversal
- Monsters act deterministically (seeded RNG between player turns)
- Player submits actions locally, instant feedback — no blockchain latency
- Channel resolves when player exits (via gate or death) — results settled on-chain via `"xc"` move
- Solo channel = player is only signer, can't cheat because dungeon is reproducible from seed + action sequence
- Prerequisite: implement BoardRules subclass using libxayagame's channel framework

### Phase 14: Multi-Player Channels
**Goal**: Co-op and PvP dungeon sessions via multi-party channels.

- Multiple players enter the same segment channel
- All parties must agree on state transitions (multi-party consensus)
- Co-op PvE: shared dungeon, collaborative monster fighting
- PvP: competitive scoring or direct combat
- Channel disputes resolved via on-chain verification
- Maps to libxayagame's channel framework with multiple signers

### Phase 15: Timed Events (Raids / Battlegrounds)
**Goal**: Temporary competitive/cooperative instances with time limits.

- Channel opens with a timer and fixed participant roster
- Competitive PvP or cooperative PvE with scoring
- Channel closes at time limit or when objective is met
- Results (rankings, rewards) settled on-chain
- Maps cleanly to the Ships channel model from libxayagame

---

## Open Design Questions

1. **Loot model**: First-come-first-served (turn order) vs. roll-based vs. predetermined per segment?
2. **Death penalty**: Currently HP=0, must heal. Consider: drop inventory? Lose gold? Harsher?
3. ~~**Segment connectivity**~~: RESOLVED — segment_links table stores bidirectional gate connections
4. **Monster respawning**: Do monsters come back when a segment is revisited? Timer-based? Never?
5. **Economy**: Is gold the only currency? Any crafting? Trading between players?
6. **Frontend tech**: Pure JS canvas (like existing roguelike) or move to something like Phaser/PixiJS?
7. **Segment 0 seed**: Should be seeded from the chain's genesis block hash (discussed, not yet implemented)

---

## Architecture Diagram

```
                    ┌─────────────────┐
                    │  Polygon Node   │
                    │  (or Ganache)   │
                    └────────┬────────┘
                             │
                    ┌────────▼────────┐
                    │    Xaya X       │
                    │  (EVM bridge)   │
                    │  JSON-RPC + ZMQ │
                    └────────┬────────┘
                             │
              ┌──────────────▼──────────────┐
              │        rogueliked           │
              │     (Game State Provider)   │
              │                             │
              │  SQLite DB ◄── game logic   │
              │  RPC server ──► JSON state  │
              │  Pending moves              │
              └──────────────┬──────────────┘
                             │
                    ┌────────▼────────┐
                    │  WebSocket      │
                    │  server (py)    │
                    └────────┬────────┘
                             │
              ┌──────────────▼──────────────┐
              │    Browser JS/TS Client     │
              │                             │
              │  State display ◄── WS push  │
              │  Move submission ──► wallet  │
              │  Dungeon rendering           │
              └─────────────────────────────┘
```
