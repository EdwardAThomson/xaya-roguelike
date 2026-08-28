# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A blockchain roguelike Game State Processor (GSP) built on the Xaya framework (`libxayagame`), targeting EVM chains via the Xaya X bridge. Game ID is `"rog"`. C++17. On-chain overworld (players, segments, inventory) plus off-chain dungeon sessions verified by deterministic action-replay proofs.

## Build and test

```bash
cmake -B build
cmake --build build -j$(nproc)

# All tests
cd build && ctest --output-on-failure

# Single test / suite (GoogleTest)
./build/roguelike_tests --gtest_filter='MoveProcessorTests.*'
```

The build is **dual-mode**: unit tests (`roguelike_tests`) and the standalone play binary (`roguelike-play`) build with no libxayagame dependency. The daemon (`rogueliked`) and channel support only build if libxayagame/libxayautil/gamechannel are installed and found via pkg-config. libxayagame source lives at `~/Explore/libxayagame` (`XAYAGAME_SOURCE_DIR` cache var).

## Devnet (end-to-end)

Requires Foundry (anvil) and the xayax Python venv:

```bash
source ~/Explore/xayax/.venv/bin/activate
python3 devnet/smoke_test.py        # full-stack smoke test, self-tearing-down
python3 devnet/frontend_devnet.py   # persistent stack + HTTP move proxy for the browser frontend
```

Other devnet tools: `ai_player.py` / `ai_explorer.py` / `multi_ai_explorer.py` (Claude-driven players), `adversarial_test.py` (cheat attempts that must be rejected).

## Architecture

**Two layers.** Layer 1 is the on-chain GSP: moves arrive as JSON name-updates in blocks, are validated and applied to a SQLite database. Layer 2 is off-chain dungeon sessions: a player enters a channel (`ec` move), plays the dungeon locally in real time, then exits (`xc` or `gw` move) submitting claimed results **plus the full action sequence**. The GSP replays the actions deterministically (`ApplySettlementBody` in moveprocessor.cpp) and rejects the move if the replay disagrees with the claims. This replay is the anti-cheat core — see `docs/STRATEGY_action_proofs.md`.

**Move pipeline.** `moveparser.cpp` validates JSON shape and calls virtual `Process*` hooks; `moveprocessor.cpp` overrides them to mutate the database. Moves use single-letter keys (`r` register, `d` discover, `t` travel, `ec`/`xc` enter/exit channel, `gw` gate-walk, `ui`/`eq`/`uq` items, `as` stats — full table in README.md). `pending.cpp` mirrors a subset for mempool tracking. Segments carry no id at all (see the world model below); visit ids come from a monotonic counter persisted in `meta.next_visit_id` and threaded through `MoveProcessor`, so a pruned segment's deleted visits never free an id for reuse.

**Determinism is consensus-critical.** Dungeon generation (`dungeon.cpp`, 80×40 grid), gameplay (`dungeongame.cpp`), combat, monsters, and items must be byte-for-byte identical to the TypeScript frontend (`~/Projects/xaya-roguelike-frontend/`), which reimplements them for local play. RNG is MT19937 seeded from SHA-256. Any change to these files breaks frontend parity AND replay verification of already-settled moves on existing chains — keep both sides in sync, never introduce floating-point, locale-, or platform-dependent behavior in them.

**World model.** `segments` are permanent map nodes (graph linked by directional gates); `visits` are temporal gameplay sessions on a segment. Newly discovered segments are provisional (`confirmed=0`) until the discoverer completes a channel run, and are pruned otherwise. Discovery has a 50-block cooldown.

**A segment IS its world coordinate.** `(world_x, world_y)` is the primary key of `segments` and the only identity a segment ever has — in moves (`ec` takes `{"x", "y"}`), in the RPC output (`{"x", "y"}` everywhere a segment is referenced), in the frontend cache (keyed by `"x,y"`), and in every foreign key (`visits.segment_x/y`, `segment_gates.segment_x/y`, `segment_links.from_x/from_y/to_x/to_y`, `players.current_x/current_y`). There is deliberately no surrogate id: an allocated integer gets reused after a provisional segment is pruned, silently repointing every cached reference at a different place. The hub is `(0, 0)` and has no row. Two consequences: a gate always leads to the neighbouring cell (nothing can disagree with the coordinate), and pruning a segment also deletes its visits, so a later segment at the same cell cannot inherit the dead one's history. `d` moves therefore require a `dir` — without one there is no cell to claim.

**Traversal model (design intent).** The world is a graph of real segments (dungeons) joined by gates; the overworld "Map" is only a meta-view, not a place you travel on. You move by walking to a gate and stepping through, and you always arrive **on the other side of that gate** (the neighbour's matching gate) — this invariant must always hold. The only thing that gates traversal is the **frontier**: stepping into unexplored territory creates a provisional segment that must be confirmed by a genuine completed run (survive to a gate), with bail = forfeit + prune (anti-grief on coordinate claims). Crossing between **already-confirmed** segments (incl. back to the hub) must be **free**: a plain transit, no survive-requirement and no penalty, landing on the other side of the gate (rewards only if a valid proof is attached). Re-entering an old, confirmed segment carries no penalty. (Status: the free-transit-between-confirmed rule is implemented; gate-walk with `"transit": true` crosses between confirmed segments with no settlement, including into an adjacent confirmed segment that has no `segment_links` row yet (the bidirectional link is created on transit). Separately, confirmed segments still regenerate monsters/items per visit — the persistence/respawn-cooldown decision is deferred; see the dungeon-persistence memory.)

**Schema.** Edit `schema.sql` only; CMake concatenates it with head/tail wrappers into two generated variants — one for the daemon (uses `xaya::SQLiteDatabase`) and one for tests (raw `sqlite3*`, no xaya dependency). Tests run against raw SQLite via `tests/testutils.cpp`.

**Daemon wiring.** `logic.cpp` (`RoguelikeLogic : xaya::ChannelGame`) plugs into the framework; `statejson.cpp` extracts state for the custom RPC methods in `rpcserver.cpp` (stub generated from `rpc-stubs/rog.json`); `channelboard.cpp` + `proto/` define channel board rules. Genesis is chain-agnostic via `--genesis_height`/`--genesis_hash` flags — no chain-specific code in game logic.

## Reference docs

- `PLAN.md` — phase-by-phase development history and status
- `ROADMAP.md` — shipped / next / backlog
- `docs/SETUP.md` — system package install for building the full stack
- `docs/SECURITY_Attack_and_Mitigations.md` — attack vector analysis
- `docs/segment-lifecycle.md` — segment discovery and confirmation lifecycle
