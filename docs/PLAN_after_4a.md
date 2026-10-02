# What to build after duels: the route to PSI

_Written 2026-10-02, after Phase 4a landed and a full afternoon of playing
duels. This is a sequencing decision, not a spec: each item already has, or
will get, its own document._

## Where we are

Phase 4a is built and merged on both sides. Duels work end to end against a
real chain: escrow, asymmetric and item stakes, the commit/reveal round with
its per-round reseed, concession, abandonment, settlement, and a two-browser
Playwright run covering both a fought-out duel and a stall. What is left of
4a is two tightening jobs, not a phase (see `PVP_4a_checklist.md`).

## The decision

**PSI fog of war between duellists (4b) is the next thing worth building.**
It is the only item on the roadmap that changes what the game IS rather than
how smoothly it runs: cryptographically hidden positions, where neither
client learns where you are without revealing where they are, and the GSP can
still audit a cheat. Nothing else queued is a headline.

**But it has a prerequisite that is currently parked, and that is the whole
point of this document.**

## PSI needs the duellists to start apart

Both duellists currently spawn on the mouth of the gate they walked in
through, one tile from each other (`PlacePlayer` in `dungeongame.cpp`; the
second arrival ring-scans to the nearest free tile). Hiding positions between
two players who are already in contact buys nothing. The spawn separation is
not a nice-to-have that happens to be nearby: it is what CREATES the
hidden-information game that PSI then protects.

Measured on three real arenas, the gap between the two most distant room
centres is 76, 87 and 81 tiles, against 1 today.

## And separation needs the walking to be cheap

A duel round is two message exchanges, so at today's pacing a round is about
a second even with the relay pushing and the client listening continuously
(both landed 2026-10-01). Spawning 80 tiles apart at a second a step is over
a minute of holding a direction key before anything happens. Separating the
duellists without fixing the walking would trade one bad experience for
another.

Two answers, and they are not exclusive:

- **Moderate separation rather than maximal.** Keep the gate anchor, so you
  still arrive where you walked in and co-op is untouched, but space the
  participants a fixed number of tiles apart along the way in. Eight is the
  current guess. Simpler than a room-pair search, and it generalises to more
  than two (0, 8, 16, 24).
- **One sealed choice covering several tiles.** Instead of "step east",
  commit "walk east until something happens" -- up to N tiles, stopping on a
  wall or on the opponent coming into view. Secrecy is unchanged (you still
  seal a choice), the log keeps one entry per round, and crossing a gap costs
  a few rounds instead of eighty.

## The proposed slice

A **minimal Phase 5**, not the whole batch:

1. Duel spawn separation (engine batch item 14; question 2 -- the spacing and
   the gate exclusion radius -- answered by picking numbers and playing).
2. The multi-tile travel action.

One `RULES_VERSION` bump, one regeneration of the parity vectors in both
repos, one genesis reset. The rest of the Phase 5 batch -- join-in-progress,
corridor pass-through, monster count scaling -- stays parked: all are
worthwhile, none is needed for PSI, and none changes how the game feels in
the way these two do.

Then PSI, whose own phasing (`STRATEGY_psi_fog_of_war.md`) starts exactly
where this leaves off: duel PvP in a 2-party channel, reusing the existing
plumbing, with the sibling `psi-demo` repo holding a protocol-parity JS
implementation to seed the frontend half.

## What is NOT next, and why

**Phase 3, true state channels.** Buys nothing a player can see. Its real
value is calldata cost, and the measurements say that is a problem we do not
have yet: about 270 gas per byte of move JSON, which only bites on a public
chain. `STRATEGY_action_proofs.md` already names the cheaper answer first --
Option B, hash commitment plus a dispute window -- and lists Phase 3 as "if
needed" behind it. There is also a wrinkle specific to this game: the state
is a whole dungeon, so posting the latest signed state is not small either.
Revisit if and when calldata cost actually hurts.

**4c, duels above 1v1.** Still blocked on four undesigned questions (payouts
under uneven stakes, collusion, death ranking, one staller freezing a round).
None of them is closer than it was.

**The rest of the Phase 5 batch.** Worth doing, not transformative. As the
owner put it: the game would broadly look and feel the same.

**Duel-ends-the-run** (`DESIGN_duel_ends_the_run.md`). A genuine improvement
and not a bug; parked until settlement timing is being touched anyway.

## Order

1. Finish 4a's two tightening jobs and tick the phase.
2. Minimal Phase 5: spawn separation + multi-tile travel, one reset.
3. PSI phase 1: duel PvP in a 2-party channel (`SPEC_psi_fog_duels.md`).
