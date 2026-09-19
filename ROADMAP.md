# Roadmap — Xaya Roguelike (backend GSP)

_Status: active · updated 2026-09-19_

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
- [x] 275 unit tests + devnet E2E / adversarial tooling
- [x] Gate-walk atomic move (settle + transit + enter-session in one transaction)
- [x] Cross-border gate alignment + entry-gate spawn (constrained replay, frontend parity)
- [x] Deterministic winning-run generator (`roguelike-play --solve`) for proofs/tests
- [x] Hosted sandbox deployment: single-origin move proxy with GSP read relay, Caddy behind a Cloudflare Tunnel, systemd (native or containerized) (`docs/DEPLOY.md`)
- [x] Temporary claim-token demo auth (proxy-layer, `ROG_REQUIRE_CLAIM_TOKEN`); removed for production in favour of wallet signing

## Next: multiplayer channels

Phased plan; the normative protocol is `docs/SPEC_multiplayer_coop.md`.
Work happens on a feature branch, merged to `main` only when a phase is
playable end to end (`coop-engine` for Phases 1 and 2, now merged).

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
        **Frontend mirror done** too: `combat.ts`, the session engine's
        duel mode, `settle.ts`'s two new entry kinds, the transport's
        `commit`/`reveal` kinds, the runner's three-step round with the
        fixed-tick commit deadline (spec section 2c, also the prerequisite
        for ever running a party larger than 2), the duel lobby and arena
        UI, and the parity vectors reproduced byte-for-byte on the TS
        side. Remaining before the merge: the two-browser Playwright run,
        both suites run together, and a duel-flavoured devnet smoke pass.
        Tracked item by item, with the open decisions the backend left
        behind, in `docs/PVP_4a_checklist.md`
  - [ ] 4b: fog of war between duelists (PSI, `STRATEGY_psi_fog_of_war.md`),
        deliberately deferred until 4a has proved the duel mechanics
  - [ ] 4c: duels above 1v1 (a 4-way free-for-all, or 2v2 teams). Blocked
        deliberately today: `moveparser.cpp` refuses `mode: "duel"` unless
        the arena seats exactly two. The engine is closer than that implies,
        since `ReseedForRound` already folds in every participant's salt in
        canonical order, `CheckDuelEnd` is written as "at most one active
        participant left", commit/reveal state is per-participant, and
        escrow became per-participant with the asymmetric-stake work. Four
        things are genuinely undesigned, and they are why the check stays:
        winner-takes-all stops being obvious once stakes are uneven and
        there are more than two (someone who put up 1 taking a pot of 300 is
        a lottery, so placement-based or proportional payouts need deciding);
        collusion has no on-chain answer, because three players can focus one
        and settle up off-chain and no replay can tell that from bad luck;
        death ordering has to become a full ranking rather than a two-way
        tie-break, since it decides money; and one staller freezes everyone,
        because a round needs every active participant's commit before any
        reveal, while the abandonment machinery's duel half still assumes
        1v1 ("an absent duellist has already lost")
- [ ] Phase 5: join a run already in progress, plus the consensus changes
      that two days of play turned up. Hosting a co-op run currently trades
      a live run for an empty lobby; duellists spawn on top of each other at
      the gate they came through; participants cannot walk past each other
      in a corridor; and monster count does not scale with party size, so a
      bigger party is strictly worse per head. Four changes, one
      `RULES_VERSION` bump, one genesis reset, because each re-pins the same
      cross-language fixtures. Design in `docs/SPEC_multiplayer_coop.md`
      section 12; work order in `docs/ENGINE_BATCH_checklist.md`. Round
      latency was measured across N = 2 to 8 and is flat (a 299ms protocol
      floor set by the poll interval, about 1.3s with human think time), so
      party size is gated by these four items and by the N on-chain moves a
      settlement costs, not by responsiveness.

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
- [ ] Party size above 2, target 4, after Phase 5. Measurement on 2026-09-16
      corrected two assumptions here: round latency is flat from N = 2 to 8,
      so the fixed tick (spec section 2c) is not a prerequisite (the 700ms
      auto-wait already covers an idle player at any N; what stalls a big
      party is a CLOSED browser, which is the abandonment gap below). What
      does gate it: rewards must scale with the party (batch item 18),
      abandonment must generalise past 2 (below), and settlement still costs
      one on-chain move per participant, which is what makes 8 uncomfortable
      rather than 4. `segments.max_players` is the only limit today and
      nothing ever writes it, so every segment is 2-player by schema default
- [ ] Generalise abandonment past 2 players. `moveprocessor.cpp` requires the
      solo suffix after a checkpoint to contain only the submitter's actions,
      so one of four dropping forces the other three to stop and one to
      continue alone. Blocks any party larger than 2
- [ ] Timed events (raids / battlegrounds)
- [ ] Crafting & trading economy
- [ ] VRF-based loot generation
- [ ] Monster respawning / seasonal world events
- [ ] Visual polish (sprite assets, animations)
