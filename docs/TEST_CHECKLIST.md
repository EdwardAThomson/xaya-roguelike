# Test Checklist & Status

Tracks what is tested and what works. Update the "last run" date and the
manual checkboxes as you verify things.

**Legend:** ✅ automated & passing · 🟢 verified via E2E/by hand · 🟡 needs a
manual/browser check · ⬜ not yet covered

---

## Automated suites

Last full run: **2026-06-28** — all green.

| Suite | Covers | Command | Result |
|-------|--------|---------|--------|
| C++ unit tests (169) | move processing, dungeon gen, gameplay, schema, state JSON | `cmake --build build -j$(nproc) && (cd build && ctest --output-on-failure)` | ✅ 169/169 |
| Cross-language parity | C++ ↔ TS dungeon/combat byte-for-byte (hash `1455554007`) | `npx tsc && node dist/game/parity_test.js` (frontend) | ✅ |
| Frontend typecheck | TS compiles clean | `npx tsc` (frontend) | ✅ |
| Full-stack smoke | register → discover → enter → winning-run confirm → potion | `python3 devnet/smoke_test.py` | ✅ |
| Adversarial / anti-cheat (50 checks) | fabricated results, griefing, provisional access, input validation, free transit | `python3 devnet/adversarial_test.py` | ✅ 50/0 |
| Browser soak agent | real frontend: explore, fight, equip, discover new + transit old, come back, invariants | `npm run agent` (frontend) | ✅ 0 anomalies |
| Browser E2E scenario | scripted single flow | `npm run e2e` (frontend) | 🟢 (targeted; agent supersedes) |

Full-stack suites (`smoke`, `adversarial`) spin up their own isolated
stacks. The browser harnesses (`agent`, `e2e`) need a running devnet
(`frontend_devnet.py` + `serve.py 8000`); the rate limit is off locally by
default. See `xaya-roguelike-frontend/tests/e2e/README.md`.

C++ unit-test groups: MoveProcessor 82, DungeonGame 18, StateJson 17,
Dungeon 16, Schema 11, StatAlloc 8, GateTraversal 8, Settle 7, Playthrough 2.

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
| **Loot persistence + potion consumption on settle** | `WinningRunPersistsLootAndConsumesPotions` | ✅ |
| Effective-stat sync (equipment ↔ replay) | statejson + frontend uses effective stats | ✅ |
| Inventory: equip / unequip / use / **discard (`di`)** | equip/unequip/`Discard*` unit tests | ✅ |
| Death penalty / respawn / forfeit | `ChannelDeath*`, `ForceSettle*` unit tests; adversarial | ✅ |
| Overworld travel + random encounters | MoveProcessor unit tests | ✅ |
| RPC / state JSON endpoints | StateJson unit tests | ✅ |
| Hosting: single-origin proxy, GSP read relay, `stop` blocked | manual curl (Phase B) | 🟢 |
| Rate limit (off local / on deploy) | manual; agent confirmed throttle | 🟢 |

---

## Manual / browser checklist (to confirm in the browser)

These need a human eye or aren't covered by the automated harnesses yet.

- [ ] Hub entry-gate spawn: walk back to the hub, appear at the matching gate (not room center)
- [ ] Inventory & equipment modal (press **I**): equip / unequip / use potion / drop in the hub
- [ ] Modal is read-only inside a dungeon; shows "collected this run (pending)"
- [ ] Pick up an item in a dungeon, exit through a gate, item is in inventory afterward
- [ ] Equip a stat item (e.g. ring of strength), play, settle, run is accepted (effective-stat path)
- [ ] Die in a dungeon → respawn at hub, ~half HP, −25% gold, XP/equipment kept
- [ ] Reload mid-dungeon → reconnect modal (resume vs forfeit)
- [ ] Re-enter a confirmed segment and walk back out a gate freely (no rejection)
- [ ] World map (Map view) shows correct (x, y) coordinates and current location
- [ ] General play feel over ~10 minutes (no soft-locks, clear messages on rejects)

---

## Deployment checklist (hosted demo)

- [ ] Provision VPS + domain
- [ ] Build GSP + frontend (`docs/DEPLOY.md`)
- [ ] `run_sandbox.sh` under systemd (rate limit on)
- [ ] Caddy TLS + static + reverse-proxy
- [ ] Live smoke: register, discover, gate-walk, die, reconnect
- [ ] `curl https://domain/gsp -d '{"method":"stop"...}'` returns 403
