# Test Checklist & Status

Tracks what is tested and what works. Update the "last run" date and the
manual checkboxes as you verify things.

**Legend:** ✅ automated & passing · 🟢 verified via E2E/by hand · 🟡 needs a
manual/browser check · ⬜ not yet covered

---

## Automated suites

Last full run: **2026-07-28**. C++, parity, and typecheck green. Full-stack
suites deferred this pass (see note below).

| Suite | Covers | Command | Result |
|-------|--------|---------|--------|
| C++ unit tests (173) | move processing, dungeon gen, gameplay, schema, state JSON | `cmake --build build -j$(nproc) && (cd build && ctest --output-on-failure)` | ✅ 173/173 |
| Cross-language parity | C++ ↔ TS dungeon/combat byte-for-byte (hash `1455554007`) | `npx tsc && node dist/game/parity_test.js` (frontend) | ✅ both sides `1455554007`; C++ `DungeonTests.CrossLanguageParity` also passes |
| Frontend typecheck | TS compiles clean | `npx tsc` (frontend) | ✅ exit 0 |
| Full-stack smoke | register → discover → enter → winning-run confirm → potion | `python3 devnet/smoke_test.py` | ⬜ not re-run this pass (live devnet occupies the ports) |
| Adversarial / anti-cheat (50 checks) | fabricated results, griefing, provisional access, input validation, free transit | `python3 devnet/adversarial_test.py` | ⬜ not re-run this pass (live devnet occupies the ports) |
| Browser soak agent | real frontend: explore, fight, equip, discover new + transit old, come back, invariants | `npm run agent` (frontend) | ⬜ not re-run this pass (live devnet + bot soak running) |
| Browser E2E scenario | scripted single flow | `npm run e2e` (frontend) | 🟢 (targeted; agent supersedes) |

Full-stack suites (`smoke`, `adversarial`) spin up their own stacks but pick
random ports in ranges (10000-30000 / 10000-20000) that overlap the live
devnet's fixed ports (GSP 18332, move proxy 18380) and would spawn a second
anvil alongside the running one. They were **not re-run this pass** to avoid
disturbing the live devnet and its bot soak; re-run them when the stack is
free. The browser harnesses (`agent`, `e2e`, `multi`, `persist`) need a
running devnet (`frontend_devnet.py` + `serve.py 8000`); the rate limit is
off locally by default. See `xaya-roguelike-frontend/tests/e2e/README.md`.

C++ unit-test groups: MoveProcessor 86, DungeonGame 18, StateJson 17,
Dungeon 16, Schema 11, StatAlloc 8, GateTraversal 8, Settle 7, Playthrough 2
(173 total).

---

## Feature coverage

| Area | Covered by | Status |
|------|-----------|--------|
| Register / players / stats / level-up | MoveProcessor, StatAlloc unit tests; smoke | ✅ |
| Discovery, segments, bidirectional links | MoveProcessor unit tests; smoke; agent | ✅ |
| Discovery cooldown (50 blk) + **no first-discovery cooldown** | `DiscoverCooldown`, `FirstDiscoverHasNoCooldown`; agent | ✅ |
| Provisional → confirmed lifecycle + pruning | `SurvivalConfirmsSegment`, prune/timeout tests; smoke; adversarial | ✅ |
| Enter / exit channel; visits | MoveProcessor unit tests; smoke | ✅ |
| Deterministic dungeon gen / combat / monsters / items | Dungeon, DungeonGame, Playthrough unit tests; parity | ✅ |
| Action-replay verification (anti-cheat core) | `Fabricated*`, `XcWithoutActionsRejected`; adversarial | ✅ |
| Gate alignment + entry-gate spawn (Option B) | parity + gate-traversal tests; agent | ✅ |
| **Free transit between confirmed segments** | `TransitGateWalkFromConfirmedSegment/...Provisional`; adversarial Cat 9; agent | ✅ |
| **Transit into an adjacent unlinked confirmed segment (+ link creation)** | `GateWalkToUnlinkedConfirmedNeighbourTransits`, `GateWalkFromHubToConfirmedNeighbour`, `GateWalkFromDungeonToConfirmedNeighbour`, `SchemaTests.InsertSegmentLinks`, `StateJsonTests.SegmentInfoWithLinks`; frontend mirror `8ff5c89` | ✅ |
| **Reject transit into another player's unlinked provisional segment** | `GateWalkToUnlinkedOthersProvisionalRejected` | ✅ |
| **Loot / XP banked on a confirmed-segment re-run (proof on transit)** | `GateWalkFromConfirmedSegmentBanksLoot`, `TransitGateWalkFromConfirmedBanksNoLoot` | ✅ |
| **Loot persistence + potion consumption on settle** | `WinningRunPersistsLootAndConsumesPotions`, `ChannelExitWithLoot` | ✅ |
| **Balance pass (eased XP curve, +2 stat pts/level, survival heal on gate-walk, full heal on level-up, flatter monster curve, bigger potions, depth-scaled XP)** | `SettleTests.XpAndLevelUp`, `SettleTests.LootDistribution`, DungeonGame tests; parity hash unchanged; frontend mirror `70761c0` | ✅ (constants; parity + replay hold) |
| **Reconnect-desync fix: gate-walk into a freshly discovered segment** | frontend `924ef4d`; agent/E2E path | 🟡 manual/browser check needed |
| **In-dungeon potion use from the modal + full-HP guard feedback** | frontend `1d2a9ff`, `bd0fab8` | 🟡 manual/browser check needed |
| **Map-view snap-back (M key; no movement trap during a run)** | frontend `c2a4c57`, `d1f4715` | 🟡 manual/browser check needed |
| **Overworld presence icons + Players tab (active-first)** | frontend `05ce239`, `f22b0ee`, `bd0fab8` | 🟡 manual/browser check needed |
| **Unified tabbed modal (Inventory/Players/Help) + standalone homepage help + title screen** | frontend `bd0fab8`, `d1f4715`, `08dbbe2` | 🟡 manual/browser check needed |
| Effective-stat sync (equipment ↔ replay) | statejson + frontend uses effective stats | ✅ |
| Inventory: equip / unequip / use / **discard (`di`)** | equip/unequip/`Discard*` unit tests | ✅ |
| Death penalty / respawn / forfeit | `ChannelDeath*`, `ForceSettle*` unit tests; adversarial | ✅ |
| Overworld travel + random encounters | MoveProcessor unit tests | ✅ |
| RPC / state JSON endpoints | StateJson unit tests | ✅ |
| Hosting: single-origin proxy, GSP read relay, `stop` blocked | manual curl (Phase B) | 🟢 |
| Rate limit (off local / on deploy) | manual; agent confirmed throttle | 🟢 |

---

## Playtest checklist — single player

Tag per item: **(auto)** covered by an automated suite · **(agent)** the
browser soak agent exercises it · **(manual)** needs a human in the browser.
Check the box once you've confirmed it end-to-end in the browser.

> Balance note: the progression/sustain balance was **just changed** this
> session (eased XP curve, +2 stat points/level, survival heal on gate-walk,
> full heal on level-up, flatter monster curve, bigger potions, depth-scaled
> XP per kill). The max-depth soak is still measuring against the new
> numbers, so the "how far can you get" figure is **pending** and any prior
> depth numbers are stale.

### Movement & exploration
- [ ] Move around a dungeon with arrows / WASD / diagonals (manual)
- [ ] Fog of war reveals around the player as you move (manual)
- [ ] Revisiting a segment keeps the previously-explored map (no full reset) (manual)
- [ ] Map / World view shows correct (x, y) coordinates and your current location (manual)
- [ ] Gates render with direction arrows; walking onto one prompts to leave (manual)

### Combat
- [ ] Fight a mob by moving into it; it takes damage and dies (agent, manual)
- [ ] You take damage; HP bar/number drops; crits and misses happen (manual)
- [ ] Gain XP and kills; level up; receive skill/stat points (manual)
- [ ] Allocate a stat point (`as`) and see the stat increase (auto, manual)

### Items, inventory & equipment
- [ ] Pick up an item off the ground (agent, manual)
- [ ] Equip a weapon / armor / ring / amulet / shield (auto, manual)
- [ ] Unequip an item back to the bag (auto, manual)
- [ ] Drink a health potion in a dungeon; HP rises; count drops (manual)
- [ ] Discard a bag item (`di`) with the confirm prompt (auto, manual)
- [ ] Inventory modal (press **I**): equip / use / drop active **only in the hub**; read-only in a dungeon (manual)
- [ ] In a dungeon the modal shows "collected this run (pending)" (manual)
- [ ] Inventory is identical in the hub and inside a segment (manual)
- [ ] Pick up an item, exit through a gate, the item is in your inventory afterward (auto, manual)
- [ ] Potions drunk during a run are deducted on a successful exit (auto, manual)
- [ ] Equip a stat item (e.g. ring of strength), play, settle — run is accepted (effective-stat path) (manual)
- [ ] Inventory cap (20) enforced; excess loot dropped with a message (auto, manual)

### Segments & traversal
- [ ] Discover a new segment by gate-walking into unexplored territory (agent, manual)
- [ ] Confirm a new segment with a real winning run to a gate (auto, agent)
- [ ] Hub spawn: gate-walk back to the hub, appear at the matching gate (not room center) (manual)
- [ ] Go to a NEW segment and come back (manual)
- [ ] Go to an OLD (confirmed) segment and come back via free transit (no rejection) (auto, agent, manual)
- [ ] Abandon a provisional segment (die/forfeit) → it is pruned (auto)
- [ ] Monsters/items regenerate on revisit (re-runnable model — known/expected) (agent, manual)
- [ ] Discovery cooldown (50 blocks) blocks a quick second discovery; first discovery is free (auto, manual)

### Death & respawn
- [ ] Die in a dungeon → respawn at hub, ~half HP, −25% gold, deaths++, XP/equipment kept (auto, manual)
- [ ] Forfeit a run → death penalty applied (auto, manual)
- [ ] Disconnect/idle → 200-block timeout force-settles the visit (auto)
- [ ] Reload mid-dungeon → reconnect modal (resume vs forfeit) (manual)

### Illegal / rejected actions (must reject cleanly with a clear message)
- [ ] Enter another player's provisional segment → rejected (auto, manual)
- [ ] Transit-leave a provisional segment (didn't confirm) → rejected (auto)
- [ ] Gate-walk to an already-claimed coordinate → "coord occupied" (auto)
- [ ] Discover while on cooldown → rejected with remaining blocks (auto, manual)
- [ ] Travel / equip / use / discover / discard while in a channel → rejected (auto)
- [ ] Act with 0 HP (enter channel, gate-walk) → rejected (auto)
- [ ] Discard an equipped item without unequipping → rejected (auto)
- [ ] Fabricated results / loot / survival (crafted move) → rejected by replay (auto)

---

## Playtest checklist — multiplayer / competition

Two or more players sharing the world. The **multi-agent harness**
(`npm run multi`) runs N players in N browser contexts against one stack
with a **referee** asserting global invariants (coordinate uniqueness, no
segment on the hub coord, players on valid segments, hp in range). Tag
**(multi)** = exercised by it; **(multi+)** = exercised but not yet
explicitly asserted; **(manual)** = needs a human / two browsers.

Verified run: 3 concurrent agents, 4 segments discovered between them,
world consistent.

> Referee note: earlier "0 referee violations" runs were **vacuous**. The
> referee read `gs.players` / `gs.segments` when `getcurrentstate` returns
> `{ gamestate: { players, segments } }`, so it iterated empty arrays and
> could never fire. This is now **fixed** in the frontend harness (reads
> `gs.gamestate`, commit `c924c81`), so the referee actually asserts. The
> `compete` scenarios used the correct path and were always valid. A
> `npm run persist` harness keeps N bots in the world indefinitely
> (heartbeat + working referee) for longer-horizon observation, and a
> MAX DIST metric now tracks how far bots push the frontier. These browser
> harnesses were **not re-run this pass** (a live devnet + bot soak is
> already running); a fresh `npm run multi` with the fixed referee against a
> free stack still needs to be recorded.

### Discovery & coordinate contention
- [x] N players competing for hub/segment directions, no two segments ever share a coordinate (multi — referee)
- [x] Two players race to the SAME empty coordinate → exactly one segment, exactly one winner (compete — Scenario 1)
- [x] Players discovering DIFFERENT directions in parallel both succeed (multi, compete)
- [x] Concurrent submissions resolve deterministically / no double-claim (compete — Scenario 1; referee would also catch a dup)

### Provisional access control
- [x] Non-discoverer cannot enter another player's provisional segment (auto: adversarial/unit; compete — Scenario 2)
- [x] Discoverer confirms it → other players can now enter it (compete — Scenario 2)
- [ ] Abandoned provisional segment blocks that direction until pruned (~300 blocks), then frees up (manual — long timer)

### Shared confirmed segments
- [ ] Two players visit the same confirmed segment at the same time (multi+ — define & assert intended behaviour)
- [x] Concurrent runs stay isolated: distinct segments, correct ownership, independent stats (compete — Scenario 3)
- [ ] One player's death does not affect another's state (multi+)

### World visibility & soak
- [x] N agents concurrently for a sustained run: GSP stays consistent, no crashes, no stuck players (multi / persist — re-run pending after referee fix)
- [ ] Players see each other in the world state: overworld presence tokens + Players tab, active-first (shipped this session; manual browser check)
- [ ] Griefing resistance under load: can't lock others out by holding provisional segments (multi+ / manual)

> Next refinements to the harness: targeted assertions for the coordinate
> race (two agents aimed at one coord), provisional-confirm-unlocks-others,
> and per-player reward reconciliation, to turn the (multi+) items into (multi).

---

## Deployment checklist (hosted demo)

- [ ] Provision VPS + domain
- [ ] Build GSP + frontend (`docs/DEPLOY.md`)
- [ ] `run_sandbox.sh` under systemd (rate limit on)
- [ ] Caddy TLS + static + reverse-proxy
- [ ] Live smoke: register, discover, gate-walk, die, reconnect
- [ ] `curl https://domain/gsp -d '{"method":"stop"...}'` returns 403
