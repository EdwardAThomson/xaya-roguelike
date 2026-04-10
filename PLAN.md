# Blockchain Roguelike — Development Plan

## Project Overview

A blockchain roguelike built on the Xaya game framework (`libxayagame`), targeting EVM chains (Polygon) via the Xaya X bridge. The game has a permanent dungeon map discovered by players, with persistent characters, inventory, and progression stored on-chain.

**Location**: `~/Projects/xayaroguelike/`
**Game ID**: `"rog"`
**Existing JS roguelike** (reference for game mechanics): `~/Projects/NewRoguelike/`
**libxayagame source**: `~/Explore/libxayagame/`

---

## Layer 1: On-Chain GSP — COMPLETE

All 7 phases done.

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
### Phase 10: JS/TS Frontend — DONE

**Repo**: `~/Projects/xaya-roguelike-frontend/`

Zero-dependency TypeScript frontend (pure Canvas, no npm runtime packages).
See that repo's PLAN.md for detailed sub-phases (F1–F6).

**Phase F1: Dungeon Renderer — DONE**
- Canvas rendering: 24px procedural tiles (brick walls, stone floors, gold gates)
- Camera/viewport centered on player, resize handling

**Phase F2: Local Dungeon Play — DONE**
- Full TS port of dungeon generation, verified identical to C++ (3200/3200 tiles match)
- MT19937 RNG + SHA-256 match C++ output byte-for-byte
- Turn-based gameplay: 8-dir movement, combat, items, monster AI, gate exits
- 14 monster types, 35 item definitions, fog of war (8-tile LOS)
- Action log recording for replay proof

**Phase F3: GSP Connection — DONE**
- JSON-RPC 2.0 client for all GSP endpoints (getplayerinfo, listsegments, etc.)
- Connection manager with auto-polling for state changes
- Overworld segment map renderer (BFS grid layout, depth-colored nodes, links)
- Segment 0 hub handling, click-to-select segments, player position indicator
- Dual-mode UI: overworld (on-chain state) vs dungeon (local play)
- Top bar: GSP URL, player name, connect/disconnect, mode toggle

**Phase F4a: Devnet Move Submission — DONE**
- Move submission client with typed convenience methods
- HTTP move proxy (`devnet/frontend_devnet.py`) translates browser requests
  into XayaAccounts smart contract calls via the xayax Python framework
- Supports: register, discover, travel, enter/exit channel, equip, use item
- Full devnet launcher: starts anvil + Xaya X + rogueliked + move proxy

**Phase F5: Channel Play Integration — DONE**
- Enter channel from overworld (on-chain `ec` move)
- Dungeon session uses real on-chain player stats, HP, inventory
- On dungeon exit, submits `xc` move with results + full action replay proof
- Results verified on-chain by GSP replay; reflected in overworld state

**Phase F4b: MetaMask / Wallet Integration**
- Replace devnet move proxy with direct `window.ethereum` calls
- ABI encoding for XayaAccounts contract (register, move)
- Transaction status tracking, error handling

**Phase F6: Visual Polish**
- Sprite image assets instead of procedural drawing
- Smooth camera scrolling, attack/damage animations
- Particle effects, sound effects

### Phase 11: Deterministic Dungeon Generation in C++ — DONE

Ported dungeon generation from JS roguelike to C++.

- 80x40 grid with Wall/Floor/Gate tiles, generated deterministically from seed+depth
- 8-15 rooms (4-8 wide, 4-7 tall) with overlap checking and 1-tile buffer
- L-shaped corridors connecting consecutive rooms + loop closure
- 4 cardinal gates on dungeon edges, each connected to nearest room
- Seeded via std::mt19937 from hash of seed string — same seed = identical dungeon
- `Dungeon::Generate(seed, depth)` static factory, `GetRandomFloorPosition()` for spawns
- Constrained gates: adjacent segments align gates at matching positions
- Unit tests: determinism, room bounds, no overlap, connectivity (flood fill), gates, constrained alignment

### Phase 12: On-Chain Overworld Layer — DONE
**Goal**: On-chain bookkeeping for player position, HP, inventory, and segment traversal.

The on-chain world is a safe meta-layer. Actual dungeon exploration (movement, combat, items, monsters) happens in solo channels (Phase 13). The overworld tracks where players are, lets them travel between segments, manage inventory, and enter/exit channel sessions.

**On-chain state added:**
- Player HP (derived from constitution: `50 + con * 5`), current_segment, in_channel
- Segment gates table (cached gate positions per segment)
- Segment links table (overworld graph connecting segments via gates)
- Visit results now include hp_remaining and exit_gate

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

### Phase 13: Solo Dungeon Channels — DONE
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
- Channel board code integrated into main roguelike library

**Sub-phase 13d: GSP ChannelGame Integration — DONE**
- RoguelikeLogic now extends ChannelGame (instead of SQLiteGame)
- GetBoardRules() returns DungeonBoardRules for channel state validation
- SetupGameChannelsSchema() creates channel tables alongside game tables
- Channel daemon binary deferred — GSP is ready to accept channel operations
**AI Player — DONE**
- `roguelike-play`: standalone C++ binary for dungeon sessions via JSON stdin/stdout
  - Accepts full player stats as CLI args: seed depth hp maxhp level str dex con int eqAtk eqDef potions
- `ai_player.py`: two-tier AI — Python autopilot (BFS pathfinding, auto-combat, auto-heal) + Claude Code for strategic decisions (gate selection, fight/flee, rerouting)
- Claude called only at decision points (~5 calls per session vs 100+ in v1)
- Session memory via `claude -p --resume` maintains conversation context
- Playthrough tested: navigates dungeons, fights monsters, picks up items, exits gates

**AI Overworld Explorer — DONE**
- `ai_explorer.py`: multi-segment traversal — discover, travel, enter dungeons,
  play, settle on-chain, repeat. Claude makes overworld strategy decisions.
- Full loop working: discover → enter channel (discoverer privilege) → play
  dungeon with real on-chain stats → settle with action replay proof → confirm
  segment → travel → discover next → repeat.
- Fixed gas limit issue: large action proofs (>2KB) need >500K gas for the
  EVM `move()` call. Devnet scripts override to 1.5M for large payloads.
- See `docs/STRATEGY_action_proofs.md` for long-term calldata cost analysis.

**Replay Protection — DONE**
- `--dungeon_id` flag uniquely identifies each game world instance
- Mixed into all segment seeds: different dungeon IDs → different worlds
- Stored in `meta` table, included in FullState JSON for player verification
- Prevents cross-instance replay on same chain

### Phase 13e: Channel Verification — DONE
**Goal**: Prevent players from fabricating dungeon results.

Implemented **Option B: Action replay proof**. The `"xc"` move now requires
an `"actions"` array — the complete list of player actions taken during the
dungeon session. The GSP replays these actions on a fresh DungeonGame from
the segment seed. Claimed results must match the replay exactly — mismatches
are rejected (move invalid, player stays in channel).

- Player submits: `{"xc": {"id": N, "results": {...}, "actions": [...]}}`
- GSP creates DungeonGame from seed + depth + player stats
- GSP replays every action deterministically
- Claimed results compared to replay — mismatch → REJECTED
- Player must submit honest proof to exit, or wait for timeout

### Security Hardening — DONE
**Goal**: Prevent griefing and world map pollution.

- **Provisional segments**: Discovered segments start unconfirmed. Only become
  permanent after the discoverer completes a valid channel run.
- **Discovery cooldown**: 50 blocks between discoveries. Prevents spam.
- **Discoverer privilege**: Only the discoverer can enter a provisional segment
  from the linked source segment. Others must wait for confirmation.
- **Travel blocked to provisional**: `"t"` rejects moves to unconfirmed segments.
- **Provisional pruning**: Unconfirmed segments with no active visits are pruned
  after VISIT_OPEN_TIMEOUT + SOLO_VISIT_ACTIVE_TIMEOUT blocks.
- **Force-settle clears in_channel**: Timeout properly resets player state.
- **Solo channel timeout**: 200 blocks (reduced from 1000).
- **8 attack vector tests**: Fabricated XP, loot, survival, non-discoverer entry,
  provisional travel, cooldown spam, missing actions, double entry.
- **SECURITY_Attack_and_Mitigations.md**: 10 attack vectors documented with
  mitigations, plus clean and malicious protocol run traces.

### Phase 14: Multi-Player Channels
**Goal**: Co-op and PvP dungeon sessions via multi-party channels.

- Multiple players enter the same segment channel
- All parties must agree on state transitions (multi-party consensus)
- Co-op PvE: shared dungeon, collaborative monster fighting
- PvP: competitive scoring or direct combat
- Channel disputes resolved via on-chain verification
- Maps to libxayagame's channel framework with multiple signers

**Tooling note (upstream, 2026-04-10):** xaya/libxayagame#143 added a
Docker image (`wasm/docker/Dockerfile`) that ships Emscripten plus all
dependencies (OpenSSL, Protobuf, jsoncpp, secp256k1, eth-utils)
pre-cross-compiled into the Emscripten sysroot. This is the intended
starting point for the browser-side channel client we'll need here:
unlike Phase 13 (solo, one signer, replay proof), real multi-party
channels require each participant to sign state proofs locally, which
in practice means running `channelcore` in the browser via WASM. Same
PR also fixed the `libethutils.a` filename bug in
`XayaGameWasmConfig.cmake.in` so the provided CMake config actually
links. Build/run commands in `docs/SETUP.md` under "WASM build
environment". Not blocking until we start this phase — flagged so we
remember the path exists.

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
2. **Death mechanics**: See section below — needs design decision.
3. ~~**Segment connectivity**~~: RESOLVED — segment_links table, bidirectional
4. **Monster respawning**: Do monsters come back when a segment is revisited? Timer-based? Never?
5. **Economy**: Is gold the only currency? Any crafting? Trading between players?
6. ~~**Frontend tech**~~: RESOLVED — Pure TS/Canvas, zero dependencies, TS port of game engine
7. ~~**Segment 0 seed**~~: RESOLVED — `--dungeon_id` flag
8. **VRF-based loot**: Verifiable Random Function for private loot generation (discussed, repo to review)

---

## Death Mechanics — Design Discussion

Currently death is very cheap: HP=0, player must use a health potion before
traveling again. No items lost, no gold lost, no XP penalty. This reduces
tension and removes consequences from risky play.

### Options to consider:

**A) Gold penalty**: Lose a percentage of gold on death (e.g. 25-50%). Gold has
value, so this creates real risk. Simple to implement. Could be deposited into
the dungeon for the next visitor to find ("death loot").

**B) Item risk**: Random equipped item has a chance of being destroyed or dropped
on death. Creates meaningful equipment decisions (do I risk my best sword in a
deep dungeon?). More impactful but potentially frustrating.

**C) XP penalty**: Lose some XP on death, potentially de-leveling. Very punishing.
Common in older MMOs. Probably too harsh for this game.

**D) Respawn cost**: Death moves you back to segment 0 (origin). You lose your
position in the world graph — have to travel back to where you were. A time
penalty rather than an item/gold penalty.

**E) Tiered penalty by depth**: Shallow dungeons (depth 1-3) have mild penalties
(gold loss only). Deep dungeons (depth 4+) have harsher penalties (gold + item
risk). Scales risk with reward.

**F) Hardcore mode (optional)**: Permanent death — character deleted. Only for
players who opt in. Creates extreme tension and makes survival truly meaningful.

### Current recommendation:

**D + A combined**: Death moves player to segment 0 and costs 25% of carried gold.
This creates meaningful risk (you lose progress and gold) without being
devastating (no item destruction, no XP loss). The gold penalty scales naturally
with wealth — rich players lose more, poor players lose less.

### Implementation needed:
- Update ProcessExitChannel: if !survived, set current_segment=0, deduct gold
- Update force-settle timeout: same death penalty
- Frontend: show death screen with gold lost, "respawned at origin"

---

## Architecture Diagram

```
                    ┌─────────────────┐
                    │  Polygon Node   │
                    │  (or Anvil)     │
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
              │     (ChannelGame GSP)       │
              │                             │
              │  SQLite DB ◄── game logic   │
              │  RPC server ──► JSON state  │
              │  Pending moves              │
              │  BoardRules (channels)      │
              │  Dungeon generation         │
              │  --dungeon_id (world ID)    │
              └──────────┬──────────────────┘
                         │ JSON-RPC
         ┌───────────────┼───────────────────┐
         │               │                   │
┌────────▼────────┐ ┌────▼───────────┐ ┌─────▼───────────┐
│  roguelike-play │ │  Browser       │ │  AI Player      │
│  (JSON stdin/   │ │  Frontend      │ │  (ai_player.py) │
│   stdout)       │ │  (TS/Canvas)   │ │  autopilot +    │
│                 │ │                │ │  Claude Code    │
└─────────────────┘ │  overworld map │ └─────────────────┘
                    │  dungeon play  │
                    │  move proxy ◄──── devnet only
                    └────────────────┘
```

## Current Stats

- **132 unit tests** passing (including 8 attack vector tests)
- **7 E2E tests** via smoke_test.py (Anvil → Xaya X → rogueliked)
- **Channel daemon** tested with full replay verification
- **30 item definitions** with real stats
- **12 monster types** scaled by depth
- **16 on-chain move types** (register, discover, travel, equip, channels, etc.)
- **AI player** completes dungeons with ~5 Claude API calls per session
- **Frontend** connects to GSP, overworld map, channel play with real stats
- **Full loop**: browser → register → discover → travel → enter dungeon → play → submit proof → see results
- **Security**: 10 attack vectors documented, replay verification, provisional segments
- **Zero runtime deps** in frontend — pure TypeScript, no npm packages in production
