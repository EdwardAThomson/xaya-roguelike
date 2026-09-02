# SPEC: 2-player co-op determinism and settlement (Phase 0)

_Status: draft for review · 2026-09-02_

This is the normative specification for multiplayer (initially 2-player co-op)
dungeon runs. It fixes, before any code is written, the two things that cannot
be changed later without breaking consensus: the canonical turn and RNG order,
and the merged action log plus the mutual-consent settlement flow. Both the C++
engine (`dungeongame.cpp`) and the TypeScript engine (frontend
`src/game/session.ts`) MUST implement this byte-for-byte; the parity fixtures
are the gate.

Scope: the happy path of Phase 1 in the multiplayer plan (both players finish
and settle). Disputes, abandonment recovery, PvP, and true state channels are
out of scope here and noted at the end.

## 1. Definitions

- **Participants.** The players recorded in `visit_participants` for the
  visit. Participant count N is fixed at activation (join auto-activates the
  visit at `segments.max_players`; Phase 1 uses N = 2).
- **Canonical participant order.** Participants sorted by name in ascending
  byte order (plain `<` on the raw UTF-8 bytes, no locale). The participant
  index `i` (0-based) is a player's rank in that order. This matches the
  `ORDER BY name` convention the GSP already relies on; no second ordering
  convention may be introduced.
- **Active participant.** A participant who is neither dead nor exited. Only
  active participants take turns.

## 2. Round structure

The engine advances in **rounds**. One round is:

1. For each participant `i = 0 .. N-1` in canonical order, skipping
   non-active participants: apply exactly one action by participant `i`.
2. After the last active participant's action, run `ProcessMonsterTurns()`
   exactly once (monsters iterate in monster-vector order, as today).

Rules:

- An action that the engine rejects as invalid (blocked move, empty pickup,
  unknown item) invalidates the whole replay, exactly as in the solo path.
  Clients must never emit invalid actions.
- A participant who dies or exits mid-round is skipped for the rest of the
  run, including later in the same round.
- The run ends when no active participants remain. `survived` is per
  participant: true iff that participant exited through a gate.
- **N = 1 degenerates to today's solo loop** (player acts, monsters act).
  This is a hard requirement, not a happy accident: the solo replay of every
  already-settled move on existing chains must remain byte-identical after
  the refactor (see section 9).

Turn pacing (how long a client waits for the partner's action before nudging
them, auto-waiting, etc.) is a transport concern and is NOT part of consensus.
Whatever the clients do in real time, the log they settle must satisfy the
round structure above.

## 3. RNG discipline

- One shared `std::mt19937` stream for the whole run, seeded exactly as
  today: `HashSeed(seed + ":game:" + depth)` (FNV-1a, cross-language).
  There are no per-player streams.
- Draw order is fully determined by section 2: draws occur inside the
  applied actions (combat rolls) in canonical action order, then inside
  `ProcessMonsterTurns` in monster-vector order.
- No new draw call sites may be added to one engine without the identical
  site in the other. Any conditional that gates a draw must use identical
  conditions in both languages.

## 4. Monster AI with N players

Replaces the single-target logic minimally:

- **Awareness.** A monster becomes aware when ANY active participant is
  within `detectionRange` Manhattan distance with line of sight. Awareness
  checks iterate participants in canonical order and stop at the first hit
  (the early-out matters: a LOS check that draws no RNG today must not
  change how many checks run in either engine; LOS is deterministic, so
  early-out is safe).
- **Target selection.** An aware monster targets the nearest active
  participant by Manhattan distance; ties break to the lower participant
  index. Target is recomputed every `MonsterAct`, not cached.
- Unaware wander behaviour (25% move roll) is unchanged; the "don't step on
  the player" check becomes "don't step on any participant".

## 5. Player interaction rules

- **Collision.** Participants block each other: `IsWalkable` excludes tiles
  occupied by any other participant, same as monsters. No stacking, no
  swapping.
- **Ground items.** First pickup wins; a later `pickup` on an emptied tile
  is an invalid action. Determined entirely by canonical action order.
- **Kills, XP, gold.** The participant whose action lands the killing blow
  takes the kill, its XP, and its gold (killer-takes-all). This is engine
  logic and therefore consensus-critical; any future reward-sharing change
  is a hard fork of the engine, so we pick the simplest rule now.
- **Inventory.** Entry inventory, potions, equip and unequip are per
  participant, mechanically identical to solo, applied to the acting
  player only.
- **Death.** A dead participant stops acting; monsters ignore them; the run
  continues for the rest. Per-player death consequences (knock-back, gold
  penalty) apply at settlement exactly as solo.

## 6. Merged action log (wire format)

The settlement carries ONE flat array of action objects in execution order.
Each entry is the existing solo action object plus the acting participant's
index:

```json
{ "i": 0, "type": "move", "dx": 1, "dy": 0 }
{ "i": 1, "type": "use", "item": "potion_small" }
```

- `i` is the canonical participant index (section 1). All other fields are
  exactly the solo encoding (`move`, `pickup`, `use`, `gate`, `wait`,
  `equip`, `unequip`).
- The GSP does not validate interleaving with a separate checker: the replay
  engine tracks whose turn it is under section 2, and an action whose `i`
  is not the expected participant fails the replay. The structure is
  enforced by the engine itself.

## 7. Settlement: mutual consent without wallets

Neither participant may be able to forge or reorder the other's actions. We
get real authentication from the chain itself, because every Xaya move is
already name-authenticated; no extra cryptography is needed, and the design
survives the later wallet migration unchanged (the wallet only changes how
the move is signed, not this flow).

Two-move commit:

1. **Confirm** (new small move, `sc`): the non-submitting participant sends
   `{"sc": {"id": <visitId>, "h": "<hex log hash>"}}` where the hash is
   SHA-256 over the exact canonical JSON serialization of
   `[merged action log, results array]` (serialization rules pinned in the
   implementation; both engines must produce identical bytes).
2. **Settle** (`s`, extended): the other participant submits the full
   `{"s": {"id", "results", "actions"}}` as today, plus the merged log.

The GSP executes the settlement only when both are present for the same
visit and the submitted log and results hash to the confirmed `h`. Order is
flexible (confirm-then-settle or settle-then-confirm within a small block
window K, default 10 blocks); an unmatched half expires with the visit
timeout. `ProcessSettle` then performs a full multi-party replay (the
`ApplySettlementBody` pattern over the shared engine) and verifies EVERY
participant's claimed results against the replay before banking anything.
The current trust-the-client behaviour of `ProcessSettle` is removed; it
must never ship as part of multiplayer.

## 8. Segment rules for Phase 1 co-op

- Co-op visits (`v`, `j`) are restricted to **confirmed** segments. The
  provisional-segment confirmation flow (discoverer must complete a run)
  stays solo-only in Phase 1, which sidesteps the question of who confirms
  a segment in a group run.
- Per-participant settlement effects (rewards, death knock-back, position
  update) reuse the existing solo banking code path per player.

## 9. Backward compatibility (hard requirements)

- The `DungeonGame` refactor to N players must leave N = 1 behaviour
  byte-identical: same RNG draws, same outcomes, same logs. Existing solo
  settlements on any running chain re-verify through this same class, so
  this is consensus, not courtesy.
- Gate: before merging the refactor, replay the existing solo parity
  fixtures and a corpus of real settled runs from a devnet chain; all must
  produce identical results pre- and post-refactor.
- New cross-language parity fixtures: at minimum one full 2-player co-op
  run (fixed seed, scripted merged log) asserting identical final state,
  loot, XP, HP, and log acceptance in C++ and TS.

## 10. Out of scope here (later phases)

- **Abandonment and disputes** (Phase 2): signed periodic checkpoints so a
  survivor can settle up to the last mutually confirmed state; today's
  force-settle-with-nothing timeout remains the fallback until then.
- **Calldata size** (near-term dependency): a 2-party merged log roughly
  doubles the ~25 KB settlement payload; compact action encoding should
  land with or shortly after Phase 1.
- **True state channels and the WASM client** (Phase 3), **PvP** (Phase 4).
