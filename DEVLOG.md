# Development Log

## 2026-07-01

A documentation-only day: a full-audit docs sweep reconciled the README, ROADMAP, and security doc with what the code actually contains, since several counts had drifted during the recent gate-walk and free-transit work. The README's move table gained the missing gate-walk (`gw`) entry, including its free-transit form for crossing between confirmed segments, and its move-type and unit-test counts were corrected (15 move types, 169 tests). The ROADMAP's inflated "20+ endpoints" claim was pinned to the real 11 JSON-RPC methods and the schema to its actual 11 tables. The security doc's adversarial suite table was updated from 45 tests in 8 categories to 53 in 9, with corrected per-category counts and a new "Free Transit" category covering free crossings between confirmed segments versus frontier crossings that still require a settled run.

**Decisions & notes:** No code changed; this was purely bringing the docs back in line with the code after the 2026-06-28 feature work. The new adversarial "Free Transit" test category confirms the free-transit rule now has E2E coverage.

## 2026-06-28

A heavy day spanning two themes: tightening the traversal/anti-cheat model to match the agreed design intent, and making the whole stack deployable as a single public sandbox.

On the gameplay side, the big move was overhauling how players cross between segments. Cross-border gate alignment plus entry-gate spawn ("Option B") landed first, so a player always arrives on the matching gate on the other side, with combat-relevant effective stats (str/dex/con/int) now exposed so replay verification stays byte-for-byte in sync with the frontend. Building on that, gate-walk now allows **free transit between already-confirmed segments**: crossing back into known territory (including the hub) is a plain transit with no survive-requirement and no penalty, while stepping into the frontier still requires a genuine completed run. This is the free-transit rule from the traversal design intent, which was also written up in the docs. A first discovery was locked to have no cooldown, and dungeon loot now persists on settle, accompanied by a new discard move for managing inventory.

On the ops side, the stack was made hostable as a single public sandbox origin (Caddy config, a systemd unit, a run script, and a DEPLOY doc), the move rate limit now defaults OFF locally but ON in the hosted deploy, and `roguelike-play` gained a `--solve` mode alongside repairs to stale devnet test scripts. The day closed out with a new test checklist / status doc covering single-player playtests and multiplayer scenarios, with several multiplayer items (contention, access, isolation) marked as now covered by the multi-agent `compete.mjs` harness.

**Decisions & notes:** Free transit between confirmed segments is the agreed target behavior, replacing the prior rule where gate-walk enforced strict settlement for every transit; frontier crossings keep the bail = forfeit + prune anti-grief rule. Any change touching dungeon generation, combat, or stat math must stay in lockstep with the TypeScript frontend, hence exposing effective stats for replay parity. Dungeon loot now persists on settle, but full dungeon persistence (shared vs instanced, monster/item respawn cooldown) remains deferred.
