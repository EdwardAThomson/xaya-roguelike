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

Status: 0 of 20 done.

---

## Group A — decisions, no code (one sitting)

These are questions, not work. Each one currently has a default baked in by
the implementation; the point is to make it deliberate. Answer them in the
decisions log below, then the Group B items that depend on them become
mechanical.

- [ ] **1. Where does raked gold go?**
      `DUEL_RAKE_PERCENT` is 0 for 4a, so this is inert today. But
      `ProcessSettle` subtracts the rake from the pot, pays the winner the
      remainder, and then zeroes the visit's pot — the raked slice goes
      nowhere. The moment the constant is non-zero that is a silent burn.
      Decide: burn (and say so), or revenue (and name the sink account).
      Cheaper to settle now than after a chain carries duels.
      *Depends on nothing. Unblocks item 6.*

- [ ] **2. What should `DUEL_ABANDON_TIMEOUT` actually be?**
      Spec section 12.5 mandates the timeout but names no value. The
      implementation uses 1000 blocks, matching `VISIT_ACTIVE_TIMEOUT`,
      which is a default rather than a decision. It is a consensus
      constant: changing it later needs a coordinated upgrade.
      *Unblocks item 7.*

- [ ] **3. Does a duel winner get the survival heal?**
      Section 5 says the winner is banked "as survived at their current
      HP". `BankPlayerSettlement` gives every survivor +30% of max HP on
      top, and the winner goes through that same path, so today they walk
      away healthier than the replay left them. Settlement-layer and
      outside the replay, so either answer is free to implement.
      Decide whether "at their current HP" is literal.
      *Unblocks item 8.*

- [ ] **4. Confirm the reseed generalisation reads as intended.**
      Section 3 gives the two-salt form only. The engines need a rule for
      the general case, so section 3 now states it: the salts of the
      round's ACTIVE participants, canonical order, each followed by `:`,
      then the round number. Identical to the spec's formula for any real
      duel (a duel always has exactly two active participants), but the
      frontend must mirror this exact generalisation or a future N-party
      change diverges silently. Read it and confirm, or restate it.
      *Blocks item 10 — the frontend should not mirror it until confirmed.*

## Group B — consensus corrections, small code (one sitting)

Both are latent rather than live: neither is reachable in normal play
today. Both are consensus rules, so they are much cheaper to correct before
a chain carries a duel than after.

- [ ] **5. Void the duel on liveness, not on age.**
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

- [ ] **6. Route or document the rake.** Implement whatever item 1 decided.
      If burn: say so in the code and in section 12.3, so the next reader
      does not file it as a bug. If revenue: pay it to the sink and assert
      conservation of gold in a test.
      *Blocked by item 1.*

- [ ] **7. Record `DUEL_ABANDON_TIMEOUT` in the spec.** Put whatever item 2
      decided alongside `DUEL_XP_BASE` and the rake in section 12.3, with
      the reasoning, so it reads as a decision rather than a default.
      *Blocked by item 2.*

- [ ] **8. Apply the survival-heal decision.** If item 3 says the winner
      keeps exactly their replay HP, thread a flag through
      `BankPlayerSettlement` so a duel win skips the +30%. If it says the
      heal stands, add a line to section 5 saying so, because the current
      wording reads the other way.
      *Blocked by item 3.*

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

- [ ] **10. `combat.ts`: `PlayerAttackPlayer`.** The section 4 draw order —
      miss (returning BEFORE the dodge draw), dodge, variance, critical,
      damage. The draw COUNT is consensus, not just the outcome.
      *Blocked by item 4 only insofar as the reseed is concerned; the
      combat function itself is independent.*

- [ ] **11. Session engine: duel mode.** The commit/reveal/apply phase
      machine, the per-round reseed at exactly one site, commitment
      verification at apply time, player-vs-player attack on a bump, death
      ordering, and the winner latch (decided the moment at most one
      participant is active, never overturned later).

- [ ] **12. `settle.ts`: the two new entry kinds.** Canonical lines
      `<i> commit <h>` and `<i> reveal <s>`, and the compact codes `c<h>`
      and `r<s>`. The commit preimage builder must match
      `DuelCommitPreimage` exactly — it is pinned literally in the
      `CommitHashVector` test, so a mismatch reports itself directly.

- [ ] **13. Run the duel parity vectors on the TS side.** All five must
      match byte-for-byte: the fought-out duel, the concession, the stall,
      the commit preimage, and the settle-hash with duel entries. Plus the
      existing co-op and solo vectors, unchanged.
      **This is the gate for items 10 to 12** — do not move to the UI
      until it passes.
      *Blocked by items 10, 11, 12.*

- [ ] **14. Transport: `commit` and `reveal` message kinds.** Client-side
      only — the devnet relay forwards arbitrary JSON objects, so it needs
      no change, and deliberately should not learn the protocol (a relay
      that understood rounds could stall or reorder one).

- [ ] **15. Runner: the three-step round.** Either a `DuelRunner` or a mode
      of `CoopRunner`, preferring the latter if the shared parts stay
      legible. Includes the section 2c fixed-tick commit deadline, the
      invalid-action substitution after reveal (apply a wait, log the
      committed action), and never revealing before holding the opponent's
      commit or committing round t+1 before applying round t.
      *Blocked by items 13, 14.*

- [ ] **16. UI.** Mode and stake in the lobby (the joiner sees both before
      joining and must cover the stake), both HP bars in the arena, bump to
      attack, Enter on a gate labelled "concede", and the round phase shown
      ("choose" / "waiting for opponent" / "revealing").
      *Blocked by item 15.*

- [ ] **17. End to end: two-browser duel in Playwright.** Mirroring
      `coop.mjs`, both scenarios: a fought-out duel, and a stall resolved by
      abandonment.
      *Blocked by item 16.*

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
| 1    | _open_   |      |           |
| 2    | _open_   |      |           |
| 3    | _open_   |      |           |
| 4    | _open_   |      |           |
