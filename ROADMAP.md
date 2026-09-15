# Roadmap — Xaya Roguelike (backend GSP)

_Status: active · updated 2026-09-09_

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
- [x] 233 unit tests + devnet E2E / adversarial tooling
- [x] Gate-walk atomic move (settle + transit + enter-session in one transaction)
- [x] Cross-border gate alignment + entry-gate spawn (constrained replay, frontend parity)
- [x] Deterministic winning-run generator (`roguelike-play --solve`) for proofs/tests
- [x] Hosted sandbox deployment: single-origin move proxy with GSP read relay, Caddy behind a Cloudflare Tunnel, systemd (native or containerized) (`docs/DEPLOY.md`)
- [x] Temporary claim-token demo auth (proxy-layer, `ROG_REQUIRE_CLAIM_TOKEN`); removed for production in favour of wallet signing

## Next: multiplayer channels

Phased plan; the normative protocol is `docs/SPEC_multiplayer_coop.md`.
Work happens on the `coop-engine` branch, merged to the deploy branch only
when a phase is playable end to end.

- [x] Phase 0: spec (canonical turn/RNG order, merged log, mutual-consent
      settlement, pro-rata reward pools, pacing/transport model)
- [x] Phase 1: 2-player co-op, happy path
  - [x] N-participant engine (players[], round structure, ring spawn,
        multi-target monster AI) behind a byte-identical solo gate
  - [x] Replay-verified settlement: `sc` confirm + `s` with the merged log,
        full multi-party replay, all-or-nothing claim checks
  - [x] Pro-rata kill rewards: damage tracking, XP/gold pools, SplitPool at
        the settlement layer
  - [x] TypeScript engine mirror in the frontend (N-participant
        session.ts, settle.ts with the canonical hash, SplitPool and
        claims) with pinned 2-player parity fixtures on both sides
        (`tests/coop_parity_tests.cpp`, frontend `npm test`)
  - [x] Transport interface (`CoopTransport` in the frontend; the devnet
        proxy's `relay_send`/`relay_recv` is the first implementation,
        WebRTC / gamechannel broadcast later) and the v/j/sc/s lobby +
        settle flow in the UI, verified by a two-browser Playwright run
        against the devnet (frontend `npm run coop`)
- [x] Phase 2: robustness. Checkpoint confirms (`sc` carries `n`), a
      20-block staleness window, and abandonment settles (`s` with
      `solo_from`: the partner's last checkpoint plus the survivor's solo
      continuation; the partner is marked absent and banked as a forfeit).
      Spec section 11; parity vector on both sides; the frontend's
      `npm run coop` closes one browser mid-run and settles the other
- [ ] Phase 3: true state channels (WASM channelcore client, gamechannel
      ChannelManager/broadcast, N-player board rules) if calldata cost or
      trustlessness demands it
- [ ] Phase 4: PvP (needs its own combat, stakes/escrow, and per-turn
      commit-reveal entropy)
  - [x] Phase 0: `docs/SPEC_multiplayer_pvp.md`, **adopted 2026-09-15**.
        1v1 duels with commit-reveal action choice and per-round entropy,
        gold stakes in escrow, concession, and refusal-to-reveal resolved
        by the existing abandonment machinery. Every question answered in
        its section 12; section 13 is the build order
  - [ ] 4a: duels with public positions. **Backend done**: schema and
        stake escrow, the commit/reveal round protocol with its per-round
        reseed, player-vs-player combat, concession and death ordering,
        settlement (winner takes the pot, loser takes the death outcome,
        a staller loses through the existing abandonment machinery), and
        the cross-language vectors in `tests/duel_parity_tests.cpp`.
        Remaining: the frontend mirror (`combat.ts`, the session engine's
        duel mode), the transport's `commit`/`reveal` kinds, the runner
        with the fixed-tick commit deadline (spec section 2c, also the
        prerequisite for ever running a party larger than 2), the UI, and
        the two-browser Playwright run. Tracked item by item, with the
        open decisions the backend left behind, in
        `docs/PVP_4a_checklist.md`
  - [ ] 4b: fog of war between duelists (PSI, `STRATEGY_psi_fog_of_war.md`),
        deliberately deferred until 4a has proved the duel mechanics

## Later (production)

Deferred for now: the sandbox demo runs without wallets, so these do not block
current work. They are prerequisites for any real-stakes deployment on a public
chain.

- [ ] MetaMask / wallet integration (replace devnet HTTP proxy + remove claim-token demo auth)
- [x] Compact action encoding for settlement proofs (`actions` as a string; about a quarter of the JSON size, see `docs/STRATEGY_action_proofs.md`)
- [ ] Hash-commitment settlement with a dispute window if calldata cost still bites on a public chain (`docs/STRATEGY_action_proofs.md` option B)

## Backlog

- [ ] Make confirming a segment cost something. Discovering claims a cell
      and walking straight back out confirms it, which is free: monsters are
      cleared within 5 tiles of every spawn point, so the gate you enter by
      is always safe at any depth. Deferred by decision on 2026-09-15.
      Preferred fix is a minimum distance from the entry point reached
      during the run, computed at settlement; requiring a different exit
      gate is stronger but breaks on single-gate segments
- [ ] Party size above 2. Needs the fixed tick (spec section 2c) so the
      round rate stops depending on the slowest player, AND a consent scheme
      that is not one on-chain move per participant. `segments.max_players`
      is the only limit today and nothing ever writes it, so every segment
      is 2-player by schema default
- [ ] Timed events (raids / battlegrounds)
- [ ] Crafting & trading economy
- [ ] VRF-based loot generation
- [ ] Monster respawning / seasonal world events
- [ ] Visual polish (sprite assets, animations)
