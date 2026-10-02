# SPEC: fog-of-war duels with PSI, phase 1

_Status: **draft** 2026-10-02. Phase 1 of `STRATEGY_psi_fog_of_war.md`
("duel PvP in a 2-party channel"), which is Phase 4b of
`SPEC_multiplayer_pvp.md`. Sequenced after the minimal Phase 5 of
`PLAN_after_4a.md` (duel spawn separation and the multi-tile travel action),
which it builds on and must not be merged ahead of. Nothing here is built._

A Phase 4a duel is played with public positions: every round reveals both
actions, so each client runs the whole shared simulation. This document
specifies the variant in which neither duellist learns where the other is
until they are in sight of each other, while the GSP can still replay the
run and bank the result exactly as it banks a 4a duel. It reuses the 4a
round (commit, reveal, apply), the per-round reseed, the merged log format,
`sc`/`s` consent, checkpoints and abandonment. What it adds is one
consensus rule (how a round is played while the duellists are apart), one
consensus function (a symmetric field of view), and one off-chain exchange
(a PSI query per hidden round). The GSP does not link a PSI library in
phase 1.

## 1. The problem a fog duel has to solve

Hiding the opponent's position from the screen is not enough. In 4a every
round reveals both actions, and the spawn points are public, so either
client can dead-reckon the opponent's position from the log with no error at
all. And the 4a engine needs both positions to run at all: bumping, combat,
monster targeting and the travel action's stop condition all read them.

So a fog duel has to change three things at once:

- **Actions in a hidden round are not revealed during play.** They are
  committed (as in 4a) and opened later, at the next point where the two
  duellists are in sight of each other anyway (section 5).
- **A hidden round must not need the opponent's state to apply.** Section 3
  makes that true by construction: nothing in a hidden round can interact
  with the opponent, and the arena is cleared of the shared things that
  would (section 2).
- **Each client must learn, every hidden round, whether the opponent came
  into sight, and nothing else.** That is the PSI query (section 4).

## 2. Definitions

- **Fog duel.** A duel visit (`mode: "duel"`) opened with `"fog": true` on
  `v`. The flag is stored on the visit row (`visits.fog`, default 0), shown
  by `listvisits` and `getvisitinfo`, and fixed for the visit's life, so
  whether a run is fogged is committed on-chain state and never either
  player's claim. Every 4a duel and every co-op visit has `fog = 0` and
  takes none of the paths below.
- **Hostility.** Both participants of a fog duel are hostile for the whole
  visit, as in 4a (SPEC_multiplayer_pvp section 1). There is no
  ally-then-betray transition, which answers the strategy document's first
  open question for this phase: the predicate is `mode == "duel"`, and it
  never changes mid-visit.
- **Clean arena.** A fog duel spawns no monsters and no ground items
  (`SpawnMonsters` and `SpawnGroundItems` are skipped; this is the "one-line
  spawn switch" of SPEC_multiplayer_pvp decision 6). Monsters chase the
  nearest player and ground items can be taken by either side, so either
  would let one client's world depend on where the hidden opponent is, and
  would leak it besides. Bag items (potions, equip, unequip) stay, since
  they touch only the player who uses them. Skipping the spawns changes the
  RNG stream after setup, which does not matter: a duel reseeds before every
  round's actions (SPEC_multiplayer_pvp section 3).
- **Sight.** `Sees(p, q)` for two floor tiles p and q is true when
  `dx*dx + dy*dy <= SIGHT_RADIUS*SIGHT_RADIUS` and the Bresenham line from p
  to q, OR the Bresenham line from q to p, has no wall tile strictly between
  its endpoints. `SIGHT_RADIUS` is 8, the radius the frontend already
  renders. The "or" makes `Sees` symmetric by construction, which the
  protocol depends on (section 4). It is integer-only, so it can live in
  both engines byte-identically. Two consequences used below: a tile always
  sees itself, and two adjacent tiles (including diagonally) always see each
  other, because there is no tile strictly between them.
- **In contact.** The duellists are in contact at a moment when
  `Sees(position_0, position_1)`.
- **Open round, hidden round.** Round t is **open** if the duellists are in
  contact at the start of the round (the positions after round t-1, or the
  spawn positions for t = 0), and **hidden** otherwise. The mode of every
  round is therefore a function of the replayed state; nobody declares it.
- **Opening.** The message in which a duellist reveals the actions and
  salts of every hidden round since its last opening (section 5).
- **`N_TRAVEL`.** The most tiles one action can move a player: the cap on
  the multi-tile travel action of the minimal Phase 5, or 1 if that action
  does not exist.

## 3. Rounds

### 3a. Open rounds

An open round is a 4a round with no change at all: commit, reveal, reseed,
apply both actions in canonical order (SPEC_multiplayer_pvp section 2),
including bump-to-attack and whatever the travel action does in 4a. Both
clients know both positions at its start, so they can run it.

### 3b. Hidden rounds

1. **Commit.** Each duellist chooses its action and a fresh 16-byte salt
   and computes the commitment with the 4a preimage, unchanged
   (`DuelCommitPreimage`, SPEC_multiplayer_pvp section 2). The commitment is
   sent attached to the duellist's first PSI flight of the round (section
   4c), not in a step of its own.
2. **Apply locally.** Each client applies **its own** action only, under the
   step rules below, and records its positions at every sub-step. It does
   not know the opponent's action and does not need it.
3. **Query.** The two run the round's PSI exchange (section 4). Both learn
   `k*`, the first sub-step at which the duellists were in contact, or that
   there was none.
4. **Resolve.** No contact: the round is over, nothing is revealed, and the
   next round is hidden. Contact at `k*`: both duellists' movement this
   round is cut at sub-step `k*` (step rules below), and each sends its
   opening (section 5). The next round's mode is then decided by `Sees` on
   the positions after the cut, which both now know.

Nothing is reseeded by the clients during a hidden round, because nothing
draws: there are no monsters, no ground items, no combat (the duellists are
never adjacent at the start of a hidden round, by the contact rule), and
using or equipping a bag item draws nothing. The GSP's replay still reseeds
at the 4a point, since it holds both salts by then; the reseed is harmless
because no draw follows it before the next round's reseed. The engine
asserts that a hidden round makes no draw.

**Step rules (consensus).** A hidden round is played in `N_TRAVEL`
sub-steps, k = 1 .. N_TRAVEL, by both duellists in lockstep, not in
canonical order:

- A non-move action (use, equip, unequip, wait) takes effect before
  sub-step 1 and the player then holds still for every sub-step.
- A one-tile move happens at sub-step 1; the player holds still after it.
- A travel action advances one tile per sub-step under the Phase 5 rules
  for walls, and holds still once it stops.
- After each sub-step k, if the duellists are in contact, `k* = k` and both
  stop: neither takes sub-steps after k*, whatever its action had left.
  This is the travel action's "stop when the opponent comes into view"
  applied where it can be: in a hidden round neither client knows where the
  opponent is, so the check happens once both positions are known to the
  replay, and the PSI query (section 4) is how the clients learn the same
  `k*` the replay computes.
- If the two would occupy the same tile at k*, the one who stepped onto it
  at k* stays on its tile from sub-step k* - 1 instead (participant 1 if
  both stepped). That tile is free, because at k* - 1 they were not in
  contact, and two players who are not in contact are at least two tiles
  apart. This is the only way a hidden round can put them on one tile, and
  it is resolved after both are revealed, so both clients can apply it.
- A gate exit is a concession, exactly as in 4a, and takes effect before
  sub-step 1. It ends the duel, and the conceder's client sends its opening
  at once so the duel can settle. A conceder who does not is a staller
  (section 7), and the outcome is the same loss.

Why lockstep rather than canonical order: in canonical order, participant
0's whole travel happens before participant 1 moves, so "did they come into
sight during the round" would depend on an order that neither player
experiences. Lockstep is what both screens show, and it makes the contact
check symmetric. Open rounds keep canonical order because their actions
can interact (an attack, a block), and 4a already defines that.

Why nothing in a hidden round can interact: at its start the duellists are
not in contact, so they are at least two tiles apart (adjacent tiles always
see each other). At every later sub-step either they are still not in
contact, so still apart, or contact is detected and both stop there. No
bump, block or attack can happen, so each client's local application of its
own action is exactly what the replay will compute for that player.

## 4. The PSI query

### 4a. What each side puts in

The round's question is "is there a sub-step k at which participant 1's
position was in participant 0's sight?". By the symmetry of `Sees` that is
the same as asking it the other way round, so one exchange answers it for
both:

- Participant 0's set X_0: every pair (k, q) with k in 1 .. N_TRAVEL and q a
  floor tile with `Sees(position_0[k], q)`.
- Participant 1's set X_1: the pairs (k, position_1[k]) for k in
  1 .. N_TRAVEL. Exactly N_TRAVEL elements, always.

X_0 ∩ X_1 is non-empty exactly when there was contact, and its smallest k
is `k*`. The sets are built from the uncut walk, since neither side knows
`k*` yet; nothing after `k*` affects the smallest match, and the cut is
applied once it is known.

Each pair is encoded as the element string

```
"rog-psi-elem-v1\n" || visitId || "\n" || round || "\n" || k || "\n" || x || "\n" || y
```

with decimal integers. Binding the visit and round means an element can
never be matched across duels or rounds.

**Padding.** X_0's real size varies with the terrain, and its size would
leak where participant 0 stands (a corridor sees less than a hall). So X_0
is padded to exactly `N_TRAVEL * SIGHT_CELLS` elements, where
`SIGHT_CELLS` = 197 is the number of lattice points with
`dx*dx + dy*dy <= 64`, an upper bound on any one sub-step's sight set.
Padding element j (j = 0, 1, ...) is

```
"rog-psi-pad-v1\n" || visitId || "\n" || round || "\n" || j
```

which can never equal a real element, so padding never produces a false
match, and is a deterministic function of public data, so an audit can
check it. X_1 needs no padding. With `N_TRAVEL` = 8 that is 1576 elements
against 8; at PSI_Cpp's measured rate (about 5,000 cells a side in 129 ms)
that is tens of milliseconds of crypto, and roughly 50 KB per flight for
the large side at 32 bytes an element. If the relay finds that heavy, the
lever is sampling contact at fewer sub-steps, not dropping the padding.

### 4b. Randomness and binding

All of a duellist's randomness for the round's exchange (blinding scalars,
any shuffle) is derived, as PSI_Cpp's commit-reveal layer requires, from
one per-round seed:

```
psiSeed_i,t = SHA-256("rog-psi-seed-v1\n" || visitId || "\n" || t || "\n" || i || "\n" || hex(s_i))
```

with s_i the round's salt from section 3b. So the 4a commitment, which
binds the action and the salt, already binds both things PSI_Cpp's per-turn
commitment has to bind: the input set (a function of the committed action
and the state before the round) and the seed (a function of the salt). No
second commitment is needed. Elements are handed to the PSI implementation
in ascending byte order of their element strings, so that given the seed
every byte a duellist sends is fixed, which is what makes a transcript
auditable.

The salt is never revealed while its round is hidden, because on an 80x40
grid a revealed blinding scalar is a revealed position (the strategy
document's disclosure note): anyone holding it can test candidate cells
against the transcript. It is revealed in the opening, when the position is
revealed anyway.

### 4c. The exchange

Phase 1 uses PSI_Cpp's 2-party exchange at a single mesh level (the cascade
buys nothing at segment scale), participant 0 as the holder of X_0. The
requirement on it:

- **Both sides compute the intersection from the flights themselves.**
  Neither relies on the other's word for `k*`; a flight that announces a
  result rather than carrying the material to compute it is not acceptable,
  because the receiver could not check it until the opening.
- Each duellist's commitment (section 3b) travels with its first flight, so
  it is on the wire before the duellist has learned anything from the
  round's exchange.

If PSI_Cpp's tag mode gives the result to one side only, phase 1 runs it in
both directions each round (each side's sight set against the other's
positions). By the symmetry of `Sees` the two answers must agree, which
doubles the crypto and costs no extra round trip, since the two directions
run interleaved. Which of these PSI_Cpp's mode actually is needs
confirming against that repository (`docs/commit_reveal_spec.md`), which is
not part of this project.

A hidden round is therefore three one-way flights, about a round trip and a
half, against two full round trips for an open round. Hiding positions does
not slow the duel down.

### 4d. What leaks

Per hidden round, each duellist learns whether the two came into sight and,
if they did, at which sub-step and (in the intersection) where participant
1 stood at those sub-steps, which the opening that follows reveals in full
anyway. That "no contact" is itself information is inherent to fog of war,
not to this protocol. Set sizes are constant, so a flight's length says
nothing. Commit timing says when a player chose, not what, exactly as in
4a.

The opening reveals the route each player took since the last opening,
which is more than "where are you now". In a clean arena the route carries
little (there is nothing to have picked up or found), and it buys immediate
auditing (section 6). A later version could open only the current state at
contact and defer the route to settlement; that is noted in section 10, not
built.

## 5. Openings and the merged log

When contact is detected in a hidden round, and at the end of the duel,
each duellist sends an **opening**: for every hidden round since its last
opening, in order, the round number, its action and its salt. The opening
is checked on receipt (section 6). Once both openings are in, each client
writes those rounds into the merged log in exactly the 4a per-round form
(SPEC_multiplayer_pvp section 6):

```
{ "i": 0, "type": "commit", "h": "..." }
{ "i": 1, "type": "commit", "h": "..." }
{ "i": 0, "type": "reveal", "s": "..." }
{ "i": 1, "type": "reveal", "s": "..." }
{ "i": 0, ...action... }
{ "i": 1, ...action... }
```

The settled log is therefore indistinguishable in format from a 4a duel's.
No entry records whether a round was hidden, because the replay recomputes
it from positions, and none carries a PSI flight: the transcripts are a
parallel record each client keeps (section 6), not part of the log. That
answers the strategy document's third open question: transcripts do not
ride in the consented state. They would cost tens of kilobytes of calldata a
round for nothing the phase 1 GSP checks.

The action recorded for a hidden round is the one committed to, as in 4a.
The cut rule of section 3b is an engine rule, like 4a's invalid-action
substitution, not a log rewrite.

**Checkpoints only at openings.** A `sc` checkpoint covers a log prefix,
and the hidden rounds since the last opening are not in either client's log
yet, so checkpoints can only be taken at the duel's start (the length-0
confirm activation already records), after each opening, and at the end.
During a hidden stretch the heartbeat re-sends the confirm at the same `n`,
which the GSP already accepts: it refuses only a shorter prefix than the
one on file.

## 6. What is audited, and by whom

**Each client audits the other at every opening.** For each opened round it
checks the commitment against the revealed action and salt (as 4a does at a
reveal), replays the opponent's moves from the last known state to get its
positions, recomputes the opponent's input set and seed, recomputes every
byte the opponent should have sent in that round's exchange, and compares
them with what arrived: PSI_Cpp's `psi_audit`, run by the client. It also
checks that the `k*` each round produced is the one the replay gives. Any
mismatch is fraud by the opponent. The client then stops: it does not sign
anything that contains the opened rounds, keeps its transcripts, and keeps
heartbeating its last good checkpoint (section 7).

Every client keeps every flight it sent and received for the life of the
duel and after it, alongside the merged log. Phase 1 has no use for them
beyond its own audit; phase 2 is where they become evidence.

**The GSP audits the outcome, not the transcripts.** A fog duel is settled
by the existing `s`/`sc` moves over a 4a-format log, and the replay in
`ApplySettlementBody` checks, in fog mode:

- the clean arena (no monster or ground-item spawn);
- every commitment against its reveal (existing 4a check);
- each round's mode, recomputed from the replayed positions with `Sees`;
- hidden rounds under the section 3b step rules, including the cut at
  `k*` and the same-tile rule, with the replay's own `k*`;
- open rounds as 4a;
- the winner and every claim, as 4a.

What the GSP does not do in phase 1 is look at a PSI flight. It does not
need to: a lie in an exchange changes what a client believes, never what
the replay computes, so a settled fog duel always has the outcome honest
play would have produced from the committed actions. The only thing a lie
can buy is information during play, and section 7 is about why that does
not turn into a banked win.

## 7. Cheating, stalling and abandonment

The ways to deviate, and where each ends:

- **Lying in an exchange** (a wrong set, a wrong seed, a corrupted flight),
  whether to hide or to see more. It is caught at the next opening by the
  victim's audit. Inflating your sight set to see more also produces
  contact, which forces an opening at once, so the lie is caught the same
  round. A caught cheat can never get the victim's consent to a log that
  contains it, and the GSP would not bank a different outcome anyway.
- **Refusing to open, or to finish an exchange.** Indistinguishable from
  vanishing, and the existing machinery handles it: the staller's last
  confirm goes stale after `ABANDON_WINDOW_BLOCKS`, the opponent settles the
  prefix up to the last opening with `solo_from` equal to its length, and
  the absent duellist is the loser (SPEC_multiplayer_pvp section 7). The
  hidden rounds since the last opening are simply not in the settled log,
  exactly as an unrevealed 4a round is not.
- **Lying, getting caught, and then outlasting the victim.** This is the
  one gap phase 1 leaves open. After a detected fraud both sides hold the
  same last checkpoint. The victim must keep heartbeating it or the cheat
  can abandon the victim and settle that prefix as the winner; the cheat
  must keep heartbeating or be abandoned. It is a war of attrition, paid in
  gas, that ends in a loss for whoever stops first, or in a void
  (`DUEL_ABANDON_TIMEOUT`, both stakes refunded) if both stop. A cheat
  therefore never banks a win through a caught lie, but can turn a likely
  loss into a refund at the cost of out-waiting the victim. The same
  stand-off already exists in 4a for a player who refuses to reveal and
  keeps heartbeating; fog duels widen it because a hidden stretch is longer
  than one round. Phase 2 closes it (section 9).

## 8. What is reused, and what is new

Reused unchanged:

- the 4a commitment preimage, salt, per-round reseed, concession, winner
  rule, stakes and escrow, settlement awards, and the `sc`/`s` moves;
- the merged log format and its canonical hash and compact encoding (no new
  entry kinds);
- checkpoints, staleness, `solo_from` and the absent-is-loser rule;
- the relay as a dumb pipe, and the duel runner's commit tick
  (SPEC_multiplayer_pvp section 2c), which in a hidden round bounds the
  commit-plus-first-flight step;
- the Phase 5 spawn separation and travel action, as they ship.

New, consensus (both engines, byte-identical, one `RULES_VERSION` bump):

- `visits.fog`, the `fog` field on `v`, and its RPC exposure;
- the clean-arena switch for fog duels;
- `Sees` and `SIGHT_RADIUS`;
- the hidden-round step rules: lockstep sub-steps, the contact cut, the
  same-tile rule, and the round-mode computation.

New, client only (frontend):

- the PSI exchange, seeded from `psi-demo` (section 11): element encoding,
  padding, seed derivation, flights, intersection, and the audit;
- two relay message kinds, a PSI flight and an opening, or the same two
  modelled as action types the way 4a models commit and reveal; that choice
  is the runner's, not consensus;
- the runner's hidden-round path: commit with the first flight, apply own
  action, read `k*`, cut, open, fill in the log, checkpoint;
- transcript retention;
- rendering: the opponent is drawn only while in contact, with a "last
  seen" marker otherwise, and the player's own field of view in a fog duel
  is drawn from `Sees`, not from the floating-point ray caster in
  `render/fov.ts`, so that what the screen shows is exactly what the
  protocol reveals.

Not needed in phase 1: a libxayagame state channel. "2-party channel" in
the strategy document means the duel's existing session (relay plus `sc`
checkpoints), which is what 4a runs on. The true state channel is Phase 3,
which `PLAN_after_4a.md` deliberately puts off.

## 9. Phase 2, for orientation

Phase 2 closes the attrition gap with an on-chain challenge:

- each flight and opening is signed with a per-duel session key that `v`
  and `j` register, so a transcript is attributable;
- a challenge move posts the disputed round's signed flights, and the
  accused must post that round's opening within a window or lose;
- the GSP links PSI_Cpp's `audit` module, recomputes the disputed round's
  bytes, and settles against whoever produced the first wrong byte;
- bonds or challenge deposits price frivolous challenges.

Evidence exposes only the disputed round, because the seed is per round.
Transcripts retained in phase 1 should be auditable by phase 2's C++ audit,
which is why section 10 pins PSI vectors across languages now.

## 10. Parity vectors and tests

Same PARITY-line discipline as `tests/duel_parity_tests.cpp`, reproduced
byte-for-byte by the frontend's `npm test`:

- `Sees`: the sight set from a handful of pinned tiles of the pinned dungeon
  (as a hash and a count), plus its symmetry checked over every pair of
  floor tiles of that dungeon in the C++ test.
- A scripted fog duel with pinned salts: a hidden stretch with no contact,
  a travel cut at `k*` > 1, a same-tile resolution, an open round with an
  attack, and a win; the settle hash and the claims.
- An abandonment settle during a hidden stretch (prefix up to the last
  opening, staller absent).
- Element strings, padding strings and `psiSeed` for a pinned visit, round
  and salt.
- PSI flights for a pinned seed and pinned sets, matched between the
  frontend's implementation and PSI_Cpp's, so phase 2 can audit what a
  browser sent in phase 1. This one needs PSI_Cpp's own vectors.

Every existing vector (solo, co-op, 4a duel) must stay byte-identical: a
visit with `fog = 0` takes none of the new paths.

The adversarial side lives in the frontend's convergence test rather than
`devnet/adversarial_test.py`, since the GSP never sees an exchange: a
runner that lies in each of the section 7 ways, against an honest runner
that must detect it at the opening.

Later options deliberately not in phase 1: opening only the current state
at contact and deferring the route to settlement; folding the commitment
into the first flight entirely; sampling contact at fewer sub-steps.

## 11. Dependencies and open points

- **Spawn separation is smaller than sight.** The minimal Phase 5's working
  number is 8 tiles along the way in from the gate, and `SIGHT_RADIUS` is
  8, so a fog duel will usually start in contact and play its first rounds
  open. The protocol is correct either way; the hidden game starts when
  someone breaks line of sight. If the hunt should start from the first
  round, fog duels want a larger spacing, or a placement that puts the two
  out of line of sight. That is a playtesting call, best made together with
  the Phase 5 spacing.
- **The travel action's open-round rules come from Phase 5.** This spec
  only adds the hidden-round behaviour. If Phase 5's "stops when the
  opponent comes into view" is defined over a different sight rule, it
  should adopt `Sees` so that the two modes agree on what "in view" means.
- **psi-demo and PSI_Cpp are outside this project.** The frontend half
  needs `EdwardAThomson/psi-demo` (the protocol-parity JS implementation)
  vendored or added as a dependency, and the PSI vectors need PSI_Cpp. Both
  repositories would have to be added to the project's sources for the
  build to start. Neither was read in writing this; section 4c's question
  about which side learns the result needs answering from them.
- **`SIGHT_RADIUS` and `N_TRAVEL` are consensus.** Changing either after
  launch is a `RULES_VERSION` bump, and the padding size follows from both.
