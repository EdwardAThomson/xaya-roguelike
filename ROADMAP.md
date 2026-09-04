# Roadmap — Xaya Roguelike (backend GSP)

_Status: active · updated 2026-09-02_

A blockchain roguelike on the Xaya framework (Polygon EVM via Xaya X). C++17 Game
State Processor with on-chain persistent world state and off-chain dungeon
sessions verified by action-replay proofs. See `PLAN.md` for the full phase plan.

**Live:** public sandbox demo at https://xayarogue.octonion.io (shared world, no
wallet; the world resets on redeploy and daily). See `docs/DEPLOY.md`.

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
- [x] Death mechanics (knock-back one segment on death, 25% gold penalty; timeouts end the run penalty-free)
- [x] JSON-RPC API (11 methods)
- [x] SQLite schema (12 tables)
- [x] Pending-move / mempool tracking
- [x] AI tooling (`roguelike-play` binary, `ai_player.py`, `ai_explorer.py`)
- [x] 194 unit tests + devnet E2E / adversarial tooling
- [x] Gate-walk atomic move (settle + transit + enter-session in one transaction)
- [x] Cross-border gate alignment + entry-gate spawn (constrained replay, frontend parity)
- [x] Deterministic winning-run generator (`roguelike-play --solve`) for proofs/tests
- [x] Hosted sandbox deployment: single-origin move proxy with GSP read relay, Caddy behind a Cloudflare Tunnel, systemd (native or containerized) (`docs/DEPLOY.md`)
- [x] Temporary claim-token demo auth (proxy-layer, `ROG_REQUIRE_CLAIM_TOKEN`); removed for production in favour of wallet signing

## Next

- [ ] Multi-player channels (co-op + PvP dungeon sessions, WASM channel client)

## Later (production)

Deferred for now: the sandbox demo runs without wallets, so these do not block
current work. They are prerequisites for any real-stakes deployment on a public
chain.

- [ ] MetaMask / wallet integration (replace devnet HTTP proxy + remove claim-token demo auth)
- [ ] Calldata optimization for large action proofs (settlement moves are ~25 KB)

## Backlog

- [ ] Timed events (raids / battlegrounds)
- [ ] Crafting & trading economy
- [ ] VRF-based loot generation
- [ ] Monster respawning / seasonal world events
- [ ] Visual polish (sprite assets, animations)
