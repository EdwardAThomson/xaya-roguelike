# Roadmap — Xaya Roguelike (backend GSP)

_Status: active · updated 2026-06-13_

A blockchain roguelike on the Xaya framework (Polygon EVM via Xaya X). C++17 Game
State Processor with on-chain persistent world state and off-chain dungeon
sessions verified by action-replay proofs. See `PLAN.md` for the full phase plan.

## Shipped

- [x] Player system (registration, persistent stats, XP, level-up, skill points)
- [x] Persistent world map (segments with depth, seed, discoverer)
- [x] Segment discovery & linking (bidirectional gated graph)
- [x] Visit system (gameplay sessions per segment)
- [x] Deterministic dungeon generation (80×40, byte-for-byte match with the frontend)
- [x] Full dungeon gameplay (movement, combat, 12 monster types, 30+ items, fog of war)
- [x] Inventory management (equip/unequip, stat bonuses, consumables)
- [x] Action-replay verification (anti-cheat)
- [x] Overworld travel (random encounters, channel entry/exit)
- [x] Security hardening (provisional segments, discovery cooldown, permission checks)
- [x] Death mechanics (respawn at origin, 25% gold penalty)
- [x] JSON-RPC API (11 methods)
- [x] SQLite schema (11 tables)
- [x] Pending-move / mempool tracking
- [x] AI tooling (`roguelike-play` binary, `ai_player.py`, `ai_explorer.py`)
- [x] 173 unit tests + devnet E2E / adversarial tooling
- [x] Gate-walk atomic move (settle + transit + enter-session in one transaction)
- [x] Cross-border gate alignment + entry-gate spawn (constrained replay, frontend parity)
- [x] Deterministic winning-run generator (`roguelike-play --solve`) for proofs/tests
- [x] Hosted sandbox deployment: single-origin move proxy with GSP read relay, Caddy + systemd (`docs/DEPLOY.md`)

## Next

- [ ] MetaMask / wallet integration for production (replace devnet HTTP proxy)
- [ ] Calldata optimization for large action proofs (settlement moves are ~25 KB)

## Backlog

- [ ] Multi-player channels (co-op + PvP dungeon sessions, WASM channel client)
- [ ] Timed events (raids / battlegrounds)
- [ ] Crafting & trading economy
- [ ] VRF-based loot generation
- [ ] Monster respawning / seasonal world events
- [ ] Visual polish (sprite assets, animations)
