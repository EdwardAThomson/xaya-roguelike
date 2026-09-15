# Phase 4a follow-up checklist

Working checklist for finishing Phase 4a (1v1 duels). Raised while building
the backend on `claude/charming-lamport-dcuta7`; see `SPEC_multiplayer_pvp.md`
for the design and ROADMAP.md for where 4a sits overall.

**How to use it.** Items are numbered and stable — refer to them by number.
Groups are ordered so each can be done in one sitting without holding the
others in your head. Group A is decisions only (no code); B is small
consensus-critical corrections that should land before any chain carries a
duel; C is housekeeping; D is the long pole. Tick items as they land and
record decisions in the log at the bottom, so a later reader sees what was
chosen and why rather than rediscovering it.

Status: **17 of 23 done** (2026-09-15). Group A decided; group B, item 21 and
the version handshake landed in the backend; items 10-16 landed in the
frontend, the parity gate PASSES, two clients converge over a delaying relay,
and a duel is playable in the UI. The frontend repo is at
`/home/user/xaya-roguelike-frontend`, branch `claude/charming-lamport-dcuta7`.
Left: the two-browser e2e (17), the merge checks (18-20), item 9, and item 22
after 4a merges.

---

## How we undertake this work

**Every change here is one of two classes, and the class decides who has to
move.** State the class in the checklist item and in the commit message;
getting this wrong is how a player ends up unable to bank a run.

- **Class 1 — banking only.** Changes what the GSP writes when it settles,
  but draws no RNG and touches no action. The replay stays byte-identical,
  the parity vectors do not move, and the frontend ENGINE needs no change.
  It still changes chain history (a node re-syncing from genesis computes
  different state), so it needs a genesis reset or a height-gated rule.
  The frontend may still need to know, because its HUD predicts what
  settlement will award — a prediction that silently disagrees with the
  chain is worse than no prediction. Examples: the duel XP, the pot payout,
  the survival-heal gate (item 21).
- **Class 2 — replay and parity.** Changes a draw, an action, or a seed.
  Both engines must change identically, every affected vector re-pins, the
  two sides ship TOGETHER, and it needs a genesis reset. Example: the fresh
  per-visit seed (item 22).

**Class 2 never ships one-sided.** A frontend generating a dungeon from one
seed derivation while the GSP replays with another does not fail loudly: the
player plays a whole run and the settle move is rejected, with the reason in
a GSP log line they never see. That is the failure this file exists to stop,
and item 23 is the mechanism.

**What 4a's backend already changed, and why it is safe.** All of it is
additive: `v` gains optional `mode`/`stake` (absent = co-op, exactly as
before), `s` claims gain `duel` (duel visits only), and the RPC gains
`mode`/`stake`/`pot` on visits (new fields an older client ignores). An
existing frontend therefore keeps working for solo and co-op and simply
cannot open a duel. That is the property to preserve deliberately, not by
luck — see item 23.

**Sequencing.** Group B and item 21 together (same file, same class), then
item 23 BEFORE the frontend starts, since its whole purpose is to protect
that work. Then group D. Item 22 last, after 4a has merged: it touches
shipped co-op behaviour and deserves isolated review and its own genesis
reset, and bundling a change with that blast radius into an in-flight
feature makes both harder to review and to roll back.

---

## Group A — decisions, no code (one sitting)

**All four decided 2026-09-15** — the answers and the reasoning are in the
decisions log at the bottom. Kept here so the questions stay legible next to
what they unblocked. Item 3 grew a second half in the process (item 21).

- [x] **1. Where does raked gold go?**
      `DUEL_RAKE_PERCENT` is 0 for 4a, so this is inert today. But
      `ProcessSettle` subtracts the rake from the pot, pays the winner the
      remainder, and then zeroes the visit's pot — the raked slice goes
      nowhere. The moment the constant is non-zero that is a silent burn.
      *Decided: **burn**. Unblocks item 6.*

- [x] **2. What should `DUEL_ABANDON_TIMEOUT` actually be?**
      Spec section 12.5 mandates the timeout but names no value. The
      implementation uses 1000 blocks, matching `VISIT_ACTIVE_TIMEOUT`,
      which is a default rather than a decision. It is a consensus
      constant: changing it later needs a coordinated upgrade.
      *Decided: **1000 blocks**, contingent on item 5 landing first so it
      measures silence rather than age. Unblocks item 7.*

- [x] **3. Does a duel winner get the survival heal?**
      Section 5 says the winner is banked "as survived at their current
      HP". `BankPlayerSettlement` gives every survivor +30% of max HP on
      top, and the winner goes through that same path, so today they walk
      away healthier than the replay left them. Settlement-layer and
      outside the replay, so either answer is free to implement.
      *Decided: **no heal on a duel win**. Also raised the wider problem
      that the heal is farmable at all — item 21. Unblocks item 8.*

- [x] **4. Confirm the reseed generalisation reads as intended.**
      Section 3 gives the two-salt form only. The engines need a rule for
      the general case, so section 3 now states it: the salts of the
      round's ACTIVE participants, canonical order, each followed by `:`,
      then the round number. Identical to the spec's formula for any real
      duel (a duel always has exactly two active participants), but the
      frontend must mirror this exact generalisation or a future N-party
      change diverges silently.
      *Confirmed as-is, with an N-party caveat recorded in the log and in
      spec section 3. Unblocks item 10.*

## Group B — consensus corrections, small code (one sitting)

Both are latent rather than live: neither is reachable in normal play
today. Both are consensus rules, so they are much cheaper to correct before
a chain carries a duel than after.

- [x] **5. Void the duel on liveness, not on age.** DONE: the select now
      keys on `COALESCE(MAX(settle_confirms.height), started_height)`, and
      `DuelMoveTests.CheckpointingDuelIsNotVoided` pins that a duel running
      past the window while checkpointing survives, then is voided once the
      checkpoint itself goes stale. Spec decision 5 records the rule.
      (Original note below.)
      `ProcessTimeouts` selects duels to void with
      `started_height + DUEL_ABANDON_TIMEOUT <= currentHeight`, so a duel
      is voided a fixed span after it BEGAN, regardless of whether both
      sides are still checkpointing. Section 12.5's rule is "neither side
      able to settle", which is liveness, not age. Not reachable today
      (a live duel settles long before 1000 blocks), but the rule does not
      currently mean what it says.
      **Fix:** key the select on the newest `settle_confirms.height` for
      the visit, falling back to `started_height` when no participant has
      checkpointed at all. Add a test for a long duel that keeps
      checkpointing and is NOT voided.

- [x] **6. Route or document the rake.** Implement whatever item 1 decided.
      If burn: say so in the code and in section 12.3, so the next reader
      does not file it as a bug. If revenue: pay it to the sink and assert
      conservation of gold in a test.
      *Blocked by item 1.*

- [x] **7. Record `DUEL_ABANDON_TIMEOUT` in the spec.** Put whatever item 2
      decided alongside `DUEL_XP_BASE` and the rake in section 12.3, with
      the reasoning, so it reads as a decision rather than a default.
      *Blocked by item 2.*

- [x] **8. Duel wins do not take the survival heal.** DECIDED: thread a
      flag through `BankPlayerSettlement` so a duel win skips the +30%.
      The heal was introduced by `f2e776d` explicitly as sustain for "every
      surviving gate-walk", to stop HP erosion capping how deep one
      expedition can go. A duel winner never walks through a gate — section
      5 says they win without needing one — so applying it leaks the
      mechanic outside the boundary its author drew. Class 1.
      *Do together with item 21: same UPDATE statement.*

## Group C — housekeeping (minutes)

- [ ] **9. Branch name.** The work is on `claude/charming-lamport-dcuta7`;
      spec section 13 says `pvp-duels`. Rename on the remote, or cherry-pick
      onto a `pvp-duels` branch, or amend section 13 to match reality.

## Group D — the frontend half (the long pole)

Steps 6 to 9 of spec section 13. None of this exists yet, and until it does
a duel cannot be played at all — the backend can verify a duel nobody can
fight. Items 10 to 13 are consensus-critical: they must reproduce
`tests/duel_parity_tests.cpp` byte-for-byte. Items 14 to 16 are not, and
can be iterated on freely.

Frontend repo: `~/Projects/xaya-roguelike-frontend/`.

- [x] **10. `combat.ts`: `playerAttackPlayer`.** DONE. The section 4 draw order —
      miss (returning BEFORE the dodge draw), dodge, variance, critical,
      damage. The draw COUNT is consensus, not just the outcome.
      *Blocked by item 4 only insofar as the reseed is concerned; the
      combat function itself is independent.*

- [x] **11. Session engine: duel mode.** DONE. `processActionBy` split the
      same way C++ `ProcessAction` was (`applyActionEffects` + `advanceTurn`
      + the phase machine) so the two read side by side. The commit/reveal/apply phase
      machine, the per-round reseed at exactly one site, commitment
      verification at apply time, player-vs-player attack on a bump, death
      ordering, and the winner latch (decided the moment at most one
      participant is active, never overturned later).

- [x] **12. `settle.ts`: the two new entry kinds.** DONE; `canonicalActionLine`
      now delegates to `canonicalActionBody`, so the settle hash and the duel
      commitment cannot drift apart. Canonical lines
      `<i> commit <h>` and `<i> reveal <s>`, and the compact codes `c<h>`
      and `r<s>`. The commit preimage builder must match
      `DuelCommitPreimage` exactly — it is pinned literally in the
      `CommitHashVector` test, so a mismatch reports itself directly.

- [x] **13. Run the duel parity vectors on the TS side.** DONE — all five
      match byte-for-byte, verified by diffing the two suites' printed
      PARITY lines (9 shared lines, zero differences). Every pre-existing
      vector unchanged.
      Trap found: `createMulti` uses `Object.create`, which bypasses class
      field initialisers, so new fields must be assigned by hand there — an
      undefined `duelWinner` read as "already decided" and the duel never
      ended. Noticed, not fixed: the equip vector prints in a different
      format on each side, so it is the one vector that cannot be diffed
      mechanically (the values do agree). All five must
      match byte-for-byte: the fought-out duel, the concession, the stall,
      the commit preimage, and the settle-hash with duel entries. Plus the
      existing co-op and solo vectors, unchanged.
      **This is the gate for items 10 to 12** — do not move to the UI
      until it passes.
      *Blocked by items 10, 11, 12.*

- [x] **14. Transport: `commit` and `reveal` message kinds.** DONE — and it
      needed NO transport change. Modelling the two as action TYPES rather
      than message kinds (a deliberate deviation from the spec's section 9
      wording, recorded at the top of `coop.ts`) means they ride the
      existing ordinal/queue/drain path: one mechanism orders all three, a
      reloading client rebuilds them from relay history for free, and the
      relay still has no idea duels exist. Client-side
      only — the devnet relay forwards arbitrary JSON objects, so it needs
      no change, and deliberately should not learn the protocol (a relay
      that understood rounds could stall or reorder one).

- [x] **15. Runner: the three-step round.** DONE as duel mode on
      `CoopRunner` (transport, queues, outbox and drain are shared
      verbatim). The player chooses ONCE per round at the commit step; the
      runner emits the reveal and the action from the sealed choice as the
      opponent's messages land. Salt comes from the platform CSPRNG, never
      `Math.random`. The section 2c tick runs from the round opening
      unconditionally — gating it on "the opponent committed" leaves the
      first mover with no trigger at all, and a commit is opaque until it
      arrives.
      Covered by `src/net/duel_test.ts`: two runners over a delaying
      in-memory relay converge on the same log, winner, HP and pvp damage,
      the log replays to the same place, and every round in it is well
      formed. Its first version had players dawdle per loop iteration (8ms)
      rather than per round, so the 120ms tick never fired and the test
      covered nothing of section 2c while appearing to; it is now sticky per
      round and FAILS if the tick never fires. Either a `DuelRunner` or a mode
      of `CoopRunner`, preferring the latter if the shared parts stay
      legible. Includes the section 2c fixed-tick commit deadline, the
      invalid-action substitution after reveal (apply a wait, log the
      committed action), and never revealing before holding the opponent's
      commit or committing round t+1 before applying round t.
      *Blocked by items 13, 14.*

- [x] **16. UI.** DONE, together with the client half of item 23 (the lobby
      is what has to refuse a stale client, so it belonged here). Stake
      presets filtered to what the player can afford; a joiner sees mode and
      stake before joining and gets a disabled choice with the reason when
      they cannot cover it (`showChoiceModal` grew `disabled`); both HP bars
      and the round phase in the arena; the gate says CONCEDE twice, on the
      nudge and in the confirm.
      The non-cosmetic part: a duel visit must BUILD a duel session, read
      from the visit row, and `coopSetup` remembers it — replaying a duel's
      prefix through the co-op engine after an abandonment would reject its
      commit entries and silently truncate the run. Mode and stake in the lobby (the joiner sees both before
      joining and must cover the stake), both HP bars in the arena, bump to
      attack, Enter on a gate labelled "concede", and the round phase shown
      ("choose" / "waiting for opponent" / "revealing").
      *Blocked by item 15.*

- [ ] **17. End to end: two-browser duel in Playwright.** Mirroring
      `coop.mjs`, both scenarios: a fought-out duel, and a stall resolved by
      abandonment.
      *Blocked by item 16.*

## Group F — raised while reviewing group A (new)

- [x] **21. Gate the survival heal on clearing the segment.** DONE:
      `SurvivalHealPercent` in moveprocessor.hpp, exposed so the frontend
      HUD can mirror the exact curve; the banking UPDATE takes the percent
      as a bind parameter instead of a hardcoded 30. Covered by
      `SurvivalHealTests.ScalesWithClearance` (the curve, including
      monotonicity and the empty-segment case) and
      `MoveProcessorTests.SurvivalHealScalesWithSegmentClearance` (a real
      settlement, asserting the banked HP and that it is strictly less than
      the old flat heal). Worth knowing: NO test covered the flat 30% heal
      before this, which is why changing it passed silently first time.
      Zero-monster visits count as cleared; the metric is per-party.
      (Original note below.) Today any
      surviving gate-walk pays +30% of max HP, so "enter, step onto the
      gate, leave" is a heal button costing only block time. Gate it on
      progress actually made:

      ```
      heal = 30% × min(1, killed / (0.75 × spawned))
      ```

      Full heal at 75% of the visit's monsters killed, scaling smoothly
      below it. Deliberately NOT a hard cliff at 75%: a cliff inverts the
      incentive at the boundary, where a player takes a fight they should
      walk away from to reach the threshold — the opposite of what the
      mechanic is for.

      Metric is monsters killed ÷ monsters spawned in that visit, both
      already available at settle time from `GetMonsters()` and its `alive`
      flags, so no schema change. Spawn count is `8 + depth*2` minus the
      near-player cull, so it varies per visit — the fraction is the right
      shape and the denominator cannot be inflated by the player.

      Class 1: no draw changes, no vector re-pins, GSP-only. Needs a
      genesis reset or height gate, and the frontend HUD should mirror the
      formula so its predicted heal does not disagree with the chain.

      Two edges to settle when implementing: a visit where the cull leaves
      zero monsters (suggest: treat as fully cleared), and co-op — per-party
      or per-player? Per-party is simpler and is a property of the run, but
      lets a freeloader ride along; damage share fixes that if playtesting
      says it matters. No perfect answer here; the goal is that "touch the
      gate and leave" stops paying, not that it is airtight.
      *Do together with item 8.*

- [ ] **22. Fresh per-visit seed for monsters and items.** Today the game
      stream is seeded `HashSeed(seed + ":game:" + depth)`, so every visit
      to a segment regenerates the SAME monsters and the SAME floor loot —
      there is no reason to run a place twice. Derive the game stream from
      the visit as well, so each run is freshly populated.

      The map survives untouched: `Dungeon::Generate` builds its own stream
      from `SeedFromString(seed, depth)`, so changing only the game-stream
      seed gives stable geography with fresh inhabitants, and the geometry
      parity hash in `tests/dungeon_tests.cpp` does not move. What does
      re-pin: the solo equip vector, the co-op vectors, the duel vectors.

      **Seed source is the real decision, because it opens a new attack
      that does not exist today — fishing for a favourable instance.**
      - Visit id: monotonic, so `v` then `lv` repeatedly advances the
        counter until the roll is liked. A move per attempt, but real.
      - Creation txid: worst. The player builds the transaction, so nonces
        can be ground offline for a txid they like.
      - Activation block hash: cannot be ground offline, cannot be chosen.
        Strongest; the client reads it over RPC before playing.

      Suggested: `HashSeed(seed + ":game:" + depth + ":" + visitId + ":"
      + blockHash)` — the segment seed keeps each place's character, the
      visit parts make each run fresh. Needs the hash stored on the visit
      row at activation so the settle-time replay can reconstruct it: one
      schema column.

      Closes the deferred dungeon-persistence / respawn-cooldown question
      in CLAUDE.md rather than opening a new one.

      Class 2. Ships with the frontend, re-pins every affected vector,
      genesis reset. **After 4a merges** — see Sequencing above.

- [x] **23. Version handshake between the GSP and the frontend.** DONE:
      `rules.hpp` holds `RULES_VERSION` and `BANKING_VERSION`, both 1, each
      with a history comment saying what its bumps covered;
      `getcurrentstate` carries them as `version: {rules, banking}`. Split
      deliberately along the two change classes above — a rules mismatch
      must BLOCK a run from starting (the client's run would be rejected at
      settlement), while a banking mismatch must NOT block play, only stop
      the HUD projecting numbers the chain will disagree with. Blocking on
      a banking change would make every reward tweak a hard client cutover
      for no safety gain. The contract for the frontend is written up in
      README (Frontend → Version handshake) and the bump discipline in
      CLAUDE.md. `getcurrentstate` was already on the devnet proxy's
      allowlist, so the sandbox needed no change.
      (Original note below.) There is
      currently NOTHING in the RPC that tells a client which rules the GSP
      is running: `getcurrentstate`, `getplayerinfo`, `listsegments`,
      `getsegmentinfo`, `listvisits` and `getvisitinfo` expose no version of
      any kind. So a frontend built against older rules does not fail when
      it connects — it fails much later and much worse: the player plays a
      whole run, and the settle move is rejected because the replay
      disagreed, with the reason in a GSP log line they never see. Every
      class 2 change makes this reachable.

      **Fix:** expose a rules version from the GSP (a constant bumped by
      any class 2 change, surfaced on `getcurrentstate` or its own method),
      have the frontend compare it against what it was built for, and
      refuse to START a run on a mismatch with a message naming the
      problem. Failing at the lobby costs a player nothing; failing at
      settlement costs them the whole run.

      Do it BEFORE the group D frontend work — it is what makes the rest of
      this list safe to land incrementally. Bump the version as part of
      items 21 and 22.
      **Client half DONE with item 16**: a RULES mismatch blocks hosting and
      joining with a message saying why; a BANKING mismatch warns once and
      lets play continue, since the engine still agrees and only the
      projected rewards would be wrong.

## Group E — before merging to main

- [ ] **18. Run both suites together.** `ctest` here and `npm test` in the
      frontend, after any engine change on either side. This is the
      standing rule in CLAUDE.md; it is listed here because 4a is the first
      change to touch the engine since the duel vectors were added.

- [ ] **19. Devnet smoke test.** `python3 devnet/smoke_test.py` plus a
      duel-flavoured pass: host with a stake, join, fight, settle, and
      confirm the pot moved and the loser took the death outcome.
      Consider extending `adversarial_test.py` with the cheat attempts the
      unit tests already cover (wrong winner claimed, forged commitment,
      action that does not open its commitment) so they are exercised
      against a real chain too.

- [ ] **20. Update ROADMAP and the spec status line.** Tick 4a, and change
      the spec's status from "backend implemented, frontend not" to
      adopted-and-shipped, the way the co-op spec reads now.

---

## Already landed (for context, not to do)

Fixed while building the backend, listed so this file shows the whole
picture:

- Spec section 2c (the fixed tick as the commit deadline) was cited by the
  build order and twice by ROADMAP but never existed. Written.
- Sections 2 and 6 contradicted each other on what the log records when an
  action is overtaken. Resolved: the engine applies a wait, the log keeps
  the committed action, or the commitment stops opening.
- Co-op's abandonment rule does not fit a duel (an absent duellist has
  already lost, so there is nothing to play out alone). Resolved:
  `solo_from` must equal the log length; enforced and documented.
- Section 1 described `v` as taking `{"x", "y"}`, predating hosting
  becoming a gate-walk. Corrected to `dir`.
- GTest had no FetchContent fallback, unlike glog, gflags and jsoncpp, so
  the suite would not configure without it installed. Added.
- `dungeonai.cpp`'s action switch went non-exhaustive when the two duel
  entry kinds were added. Handled.

## Decisions log

Record answers here as they are made, with the reasoning, so changing one
later is deliberate rather than a rediscovery.

| Item | Decision | Date | Reasoning |
|------|----------|------|-----------|
| 1 | Rake is a **burn** | 2026-09-15 | The death tax already destroys gold, so a burn needs no new concept; there is no treasury or sink account anywhere in the schema, and adding one means inventing a protocol-owned row plus a policy on who may spend it. Outside the replay, so switching to a real sink later is a coordinated upgrade, not a chain break. |
| 2 | **1000 blocks**, but only meaningful after item 5 | 2026-09-15 | The asymmetry favours patience: refunding early converts a legitimate win into a draw, since section 7 says whoever returns first continues alone and wins; refunding late only leaves gold locked. Once item 5 measures silence since the last checkpoint rather than total age, 1000 blocks of silence is unambiguously "both gone" and no live duel can trip it. |
| 3 | Duel wins take **no survival heal**; separately, gate the heal itself (item 21) | 2026-09-15 | `f2e776d` introduced the heal as sustain for "every surviving gate-walk", to stop HP erosion capping expedition depth. A duel winner never walks through a gate, so the mechanic does not apply. The heal being farmable by a trivial solo run is a real but separate problem — item 21. |
| 4 | Reseed generalisation **confirmed as-is**, with an N-party caveat | 2026-09-15 | Any rule must reduce to `s_0 + ":" + s_1 + ":" + t` for two active participants, because that is both the spec formula and what the pinned vectors hash. The current rule does, and a duel never has another case. It SKIPS inactive participants, so for N-party free-for-alls the active set changes the shape of the seed material and a participant could time their exit to influence it — the N-party extension must revisit this. |
