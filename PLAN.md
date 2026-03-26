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

### Phase 11: Deterministic Dungeon Generation in C++
**Goal**: Port dungeon generation from JS to C++ so the GSP can validate dungeon state.

- Port room-based generation from dungeon.js (80x40 grid, rooms, corridors, gates)
- Use libxayagame's seeded Random class (from Context::GetRandom()) instead of Math.random()
- Generate monster/item placement deterministically from segment seed + depth
- This is prerequisite for on-chain dungeon state or channel-based play

### Phase 12: On-Chain Dungeon Exploration (Slow Mode)
**Goal**: Let players explore dungeons with on-chain moves (one action per block).

- New move types: `{"move": {"dir": "north"}}`, `{"attack": {"target": [x,y]}}`, `{"pickup": {}}`, etc.
- GSP tracks player position, HP, monsters, items within each segment
- Turn-based: each block processes one action per player per segment
- Slow (~30s per action on Polygon) but fully functional and verifiable
- This validates the game rules before adding channels for speed

### Phase 13: Game Channels for Real-Time Play
**Goal**: Off-chain real-time dungeon exploration via libxayagame's channel framework.

- Implement BoardRules subclass for dungeon gameplay
- Protobuf definitions for dungeon state (map, player positions, monsters, items)
- Channel opens when player enters a segment, resolves when they exit or die
- Monster turns via auto-moves (deterministic AI between player turns)
- Co-op PvE: all participants agree on state, disputes unlikely
- Channel resolution records visit results on-chain (XP, loot, survival)
- This replaces the discoverer-only settlement with cryptographic proofs

### Phase 14: Timed Events (Raids / Battlegrounds)
**Goal**: Temporary competitive/cooperative instances with time limits.

- Channel opens with a timer and fixed participant roster
- Competitive PvP or cooperative PvE with scoring
- Channel closes at time limit or when objective is met
- Results (rankings, rewards) settled on-chain
- Maps cleanly to the Ships channel model from libxayagame

---

## Open Design Questions

1. **Loot model**: First-come-first-served (turn order) vs. roll-based vs. predetermined per segment?
2. **Death penalty**: Drop inventory? Lose gold? Respawn at segment entrance? Persistent death?
3. **Segment connectivity**: How do gates between segments work on-chain? Is the world graph stored in the GSP?
4. **Monster respawning**: Do monsters come back when a segment is revisited? Timer-based? Never?
5. **Economy**: Is gold the only currency? Any crafting? Trading between players?
6. **Frontend tech**: Pure JS canvas (like existing roguelike) or move to something like Phaser/PixiJS?

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
