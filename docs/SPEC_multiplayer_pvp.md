# SPEC: 1v1 duels (PvP), Phase 4

_Status: **adopted** 2026-09-15; nothing is implemented yet. Written as the
Phase 0 of Phase 4 in ROADMAP.md, the way `SPEC_multiplayer_coop.md`
preceded the co-op code. Every question that was open in the draft is
answered in section 12, so the consensus-critical parts (sections 2, 3, 4
and 6) are frozen and can be built against. Section 13 is the build order.
Phase 4b (fog of war) is deliberately deferred until 4a has proved the duel
mechanics; positions are public in 4a, which is honest, since the merged log
is public at settlement anyway._

This document specifies competitive 1v1 dungeon runs ("duels") on top of the
co-op machinery that already ships: the N-participant engine, the merged
action log, `sc`/`s` mutual consent, checkpoints and abandonment settles, and
the relay transport. It fixes the three things a duel needs that co-op does
not: **simultaneous action choice** (neither player may see the other's move
before choosing their own), **entropy neither player controls** (the shared
RNG stream is public, so an attacker who can read ahead would only ever swing
on a guaranteed hit), and **stakes** (something to win). Hidden positions (fog
of war between hostile players) are deliberately a later step; see section 11.

## 1. Definitions

- **Duel visit.** A visit opened with `{"v": {"x", "y", "mode": "duel",
  "stake": G}}` on a confirmed segment, `max_players` 2. `mode` defaults to
  `"coop"`, which is exactly today's behaviour; everything below applies only
  to `mode == "duel"`. The visit row records the mode and the stake, so
  hostility is a property of committed on-chain state, never of either
  player's claim (the PSI design in `STRATEGY_psi_fog_of_war.md` requires
  exactly this).
- **Participants, canonical order, active.** As in the co-op spec, section 1.
  The two participants are hostile for the whole visit; there is no
  ally-then-betray transition in Phase 4.
- **Round.** A duel advances in rounds like co-op (one action per active
  participant in canonical order, then one monster pass), but every round is
  preceded by a commit phase and a reveal phase (section 2). "Round t" means
  the t-th round, t = 0, 1, 2, ...

## 2. Round protocol: commit, reveal, apply

Each round has three steps. Steps 1 and 2 are exchanged over the transport;
step 3 is the engine.

1. **Commit.** Each participant i picks its action a_i and a 16-byte random
   salt s_i, and sends `commit_i = SHA-256("rog-duel-commit-v1\n" ||
   visitId || "\n" || t || "\n" || i || "\n" || canonical(a_i) || "\n" ||
   hex(s_i))` where `canonical(a_i)` is the co-op section 7 action line
   without the leading index (`move 1 0`, `wait`, ...).
2. **Reveal.** Once BOTH commits have arrived, each participant sends
   `(a_i, s_i)`. A reveal that does not hash to the commit on file is
   invalid, and the round cannot close; the revealer's client must never
   emit one (section 7 says what happens if a party stops revealing).
3. **Apply.** With both reveals in hand, the engine reseeds the round
   (section 3) and applies a_0 then a_1 (canonical order, skipping
   inactive participants), then `ProcessMonsterTurns()`, exactly as co-op.
   The order of application is fixed and public; what is hidden is only the
   choice, which is what matters.

Rules:

- A participant may not send its round-t commit before round t-1 has been
  applied on its own client (there is nothing to choose against otherwise),
  and may not send its reveal before it holds the other side's commit. Both
  clients enforce this; the log format (section 6) makes any violation
  visible to the replay.
- Waits are still self-authored, but the grace-window auto-wait of co-op
  section 2b applies to the COMMIT step only: if the opponent has committed
  and this player idles past the window, the client commits a wait. Reveals
  are always immediate once the other commit is in.
- An action that is invalid when its turn comes (opponent stepped into the
  tile first, monster died first) is replaced by a wait, as in co-op. This
  happens after reveal, on public data, identically on both clients.
- Mid-round inactivity (death, exit) is handled as in co-op section 2: the
  dead participant simply has no further commits. If a participant dies
  during round t's application, its round t+1 commit (if already sent) is
  ignored by both clients and never enters the log.

## 3. Entropy

The RNG stream stays a single `std::mt19937` seeded as today from
`HashSeed(seed + ":game:" + depth)`; dungeon generation, monster spawns and
item spawns are unchanged and public (the map must be generated before the
run). The change is one **reseed per round** before step 3:

```
roundSeed_t = HashSeed(hex(s_0) + ":" + hex(s_1) + ":" + t)
rng.seed(roundSeed_t)
```

with `s_i` the round-t salts revealed in section 2. Consequences:

- Neither player can predict any combat roll of round t when choosing its
  action: each knows its own salt only, and the other salt is committed but
  unrevealed. A player cannot bias the seed either, because the commitment
  binds the salt before the other salt is known.
- Reseeding discards the stream's previous state deliberately: the monster
  pass of round t also runs on `roundSeed_t`, so a player cannot read ahead
  into monster behaviour across rounds either.
- Both engines must call the reseed at exactly the same point (after both
  reveals, before the first action of the round) and nowhere else. This is
  the only new draw-affecting site; the co-op section 3 rules on draw
  discipline apply unchanged.
- Solo (`mode == "coop"`, N = 1 or 2) never reseeds, so all already-settled
  runs re-verify byte-identically.

Why not a VRF here either: a VRF would let one party produce unpredictable
randomness alone, but the duel needs randomness that NEITHER party can
predict or bias, and two-party commit-reveal is the minimal construction for
that. The PSI commit-reveal layer cited in `STRATEGY_psi_fog_of_war.md` uses
the same per-turn seed binding, so the two mechanisms compose later.

## 4. Combat between players

Moving into a tile occupied by a hostile participant is an attack (today it
is a blocked move; that stays true for `mode == "coop"`). A new
`PlayerAttackPlayer(attacker, defender, rng)` in `combat.cpp`, mirrored in
`combat.ts`, resolves it with the existing formulas composed in a fixed draw
order so that parity is mechanical:

1. **Miss** (attacker's roll, as against a monster): missChance =
   min(25%, def / (atk + def) * 40%) with atk = `PlayerAttackPower(attacker)`
   and def = `PlayerDefense(defender)`; one `uniform_int(1,100)` draw.
2. **Dodge** (defender's roll, as against a monster): dodgeChance =
   min(50, 5 + floor(defender.dexterity * 0.5)); one `uniform_int(1,100)`
   draw. A miss returns before the dodge roll is drawn (draw count matters).
3. **Variance**: `uniform_int(80,120)` percent of atk.
4. **Critical**: `uniform_int(1,100)` <= 5 + attacker.dexterity / 5 doubles
   to 150%.
5. **Damage**: max(1, floor(dmg) - def), applied to the defender's HP; death
   at HP <= 0 exactly as monsters kill players today (`PlayerDied`).

Damage dealt to players does NOT count towards the co-op damage pools
(section 8). There is no retaliation roll: the defender answers on its own
action. Potions, equip and unequip work as in co-op, per participant.

## 5. Stakes and outcomes

- **Escrow.** `stake` is an integer gold amount, 0 allowed (a friendly duel).
  Hosting deducts the host's stake into `visits.pot`; joining deducts the
  joiner's matching stake. A join by a player who cannot cover the stake is
  rejected. Cancelling an open duel (`lv` by the host, or the open-visit
  timeout) refunds the pot to the host.
- **Winner.** The duel ends when at most one participant is active. The last
  active participant is the winner and is banked as **survived at their
  current HP without needing a gate**: the arena is the fight, not the exit.
  Reaching a gate and exiting is a **concession**: the exiter loses the duel
  (banked as not survived, section 5a) and the opponent wins on the spot.
  If both are dead after a monster pass (a monster kills the second player
  in the same pass), the one who died LATER in the pass wins; the engine
  records death order, so this is deterministic.
- **Settlement of the pot.** The winner receives the whole pot. A protocol
  rake is a tunable at the settlement layer (0 in Phase 4a); like the co-op
  pool split it is outside the replay, so it can change by coordinated
  upgrade without breaking already-settled duels.
- **XP.** The winner gains `DUEL_XP_BASE * loserLevel` XP (tunable, initial
  value 20) at the settlement layer, in addition to any monster XP under
  section 8. The loser gains nothing from the duel itself.

### 5a. Loser's penalty

The loser takes the ordinary death outcome (`BankPlayerSettlement` with
survived = false: half HP respawn, knock-back, 25% of carried gold) computed
AFTER the stake has left their balance, so a duel never costs more than the
stake plus the usual death tax. Loot picked up during the duel is discarded
for the loser (as any death) and kept by the winner (as any survived run).

## 6. Merged log and consent hash

The merged log gains two entry kinds, so that the replay can verify the
commit-reveal itself. Per round, in this order:

```
{ "i": 0, "type": "commit", "h": "<64 hex>" }
{ "i": 1, "type": "commit", "h": "<64 hex>" }
{ "i": 0, "type": "reveal", "s": "<32 hex>" }
{ "i": 1, "type": "reveal", "s": "<32 hex>" }
{ "i": 0, "type": "move", "dx": 1, "dy": 0 }      # the revealed actions,
{ "i": 1, "type": "wait" }                         # canonical order
```

Canonical hash lines (co-op section 7 encoding) are `<i> commit <h>` and
`<i> reveal <s>`; the action lines are unchanged. Compact encoding (see
`STRATEGY_action_proofs.md`) gains `c<h>` and `r<s>`.

Replay rules, enforced by the engine in duel mode so no separate checker is
needed:

- Round structure per round: exactly one commit per active participant,
  then one reveal per active participant, then the actions. Anything else
  fails the replay.
- Each reveal must satisfy `commit == SHA-256(...)` of section 2 over the
  action that follows for that participant. A mismatch fails the replay.
- The reseed of section 3 happens between the last reveal and the first
  action.

Because the log is consented to by both parties (`sc`), a participant can
never claim the other committed to something else: the commit is in the log
the other side signed off on. Checkpoints (co-op section 11) are only ever
taken at round boundaries (after a full apply), which the log format makes
unambiguous.

## 7. Settlement, refusal to reveal, abandonment

- **Normal end.** Same as co-op section 7: the non-submitter sends `sc` over
  the whole log, the submitter sends `s`. Claims per participant gain
  `"duel": "won" | "lost"`; the GSP recomputes the winner from the replay
  and rejects mismatches like any other claim.
- **Refusing to reveal.** A player who sees the opponent's reveal and dislikes
  the resulting round can only stall: its own reveal is due and the round
  cannot close without it. Stalling looks exactly like vanishing, and the
  co-op section 11 machinery resolves it: the staller's last checkpoint goes
  stale, the opponent continues alone from that checkpoint with the staller
  marked absent, and an absent participant in a duel is the loser (section
  5). The unrevealed round is simply not in the settled log. So a stall costs
  the stake. The window `ABANDON_WINDOW_BLOCKS` therefore bounds how long a
  duel can be held hostage; it should stay short for duels (the same 20
  blocks, or a duel-specific constant).
- **Both vanish.** Whoever returns first continues alone and wins; if neither
  returns, the visit stays active (as co-op) and the pot is locked. A duel
  open-ended timeout that refunds both is an open question (section 12).

## 8. Monsters, items, rewards

The segment is played as-is: monsters, ground items and drops are exactly as
in co-op, and monsters treat both duelists as targets (they are "active
participants" to the AI). Kill XP and kill gold accrue to the co-op pools and
are split by damage dealt TO MONSTERS at settlement (co-op section 5a);
damage dealt to the other player is tracked separately and does not enter
the pools. Ground items are raced as in co-op. This keeps the arena lively
and reuses every existing rule; a "clean arena" variant with no monsters is a
one-line spawn switch if playtesting wants it.

## 9. Transport and frontend

- Transport: the relay carries two new message kinds, `commit` and
  `reveal`, alongside `action`; per-participant ordinals as today. Nothing
  server-side changes: the relay stays a dumb, claim-token-checked pipe.
- Runner: `DuelRunner` (or a mode of `CoopRunner`) implements the three-step
  round of section 2, including the commit-step auto-wait, the invalid-action
  substitution after reveal, and the log entries of section 6. It never
  reveals before holding the opponent's commit, and never commits round t+1
  before applying round t.
- UI: the host chooses mode and stake; the joiner sees both before joining
  and must cover the stake. In the arena both HP bars are shown, bumping the
  opponent attacks, and Enter on a gate is labelled "concede". The sidebar
  shows the round phase ("choose", "waiting for opponent", "revealing").
- Latency: a round costs two relay round trips instead of one. On the proxy
  relay (about 300 ms poll) that is roughly a second per round, which is
  acceptable for a turn-based duel; the WebRTC transport in the backlog is
  the fix if it is not.

## 10. Backward compatibility and parity gates

- `mode == "coop"` (the default, and every existing visit) takes none of the
  new paths: no reseed, no player-vs-player attack, no commit/reveal entries.
  All existing parity vectors (solo equip, co-op run, absent partner,
  compact fixture) must stay byte-identical.
- New cross-language vectors before merge: a scripted duel with pinned salts
  (so the commits are pinned too), covering at least one player-vs-player
  hit, one miss, one dodge, one critical, a concession, and a win by death;
  plus a refusal-to-reveal abandonment settle. Same PARITY-line discipline
  as `tests/coop_parity_tests.cpp`.
- The compact encoding and the settle-hash vectors gain the two new entry
  kinds.

## 11. Phasing

- **4a (this document).** Duels with public positions: both players see the
  whole shared state, as the merged log is public anyway. Stakes, commit-
  reveal actions and entropy, concession, abandonment.
- **4b.** Fog of war between the duelists using the PSI protocol of
  `STRATEGY_psi_fog_of_war.md`: positions hidden, "in sight" queries per
  round, PSI transcripts riding in the channel log, GSP-side audit on
  dispute. Requires the true state-channel work (Phase 3) or at least
  signed per-turn transcripts, so it is sequenced after 4a.
- **Later.** Party-vs-party and free-for-all brawls (N-party commit-reveal
  is a straightforward extension of section 2, but the stake and outcome
  rules are not), and PvP inside the overworld (a different game).

## 12. Decisions (was: open questions)

All settled 2026-09-15. Recorded with the reasoning so a later change is a
deliberate one rather than a rediscovery.

1. **Simultaneous, not alternating initiative.** Section 2 keeps
   commit-reveal simultaneity. It costs an extra relay round trip per round,
   but it solves hidden choice and unpredictable entropy with one mechanism,
   whereas alternating initiative leaks the first mover's action every round
   and *still* needs commit-reveal for the entropy. If playtesting finds the
   latency intolerable, alternating initiative is the fallback, and it is a
   transport-layer change rather than a consensus one.
2. **Gold-only stakes in 4a.** Item stakes (anteing gear) need the escrow to
   move inventory rows and to survive a disputed settlement; deferred.
3. **Constants.** `DUEL_XP_BASE` = 20, rake = 0. Both are settlement-layer
   values outside the replay, so they can be retuned by coordinated upgrade
   without breaking already-settled duels. No level-gap scaling in 4a.
4. **No level matching.** The stake is the only matchmaking signal; the
   lobby shows the host's level and the joiner decides.
5. **A locked pot is refunded.** If a duel goes unsettled past
   `DUEL_ABANDON_TIMEOUT` with neither side able to settle, both stakes are
   returned and the duel is void. Co-op's "stays active forever" rule is
   tolerable when nothing is at stake; with money in escrow it is not.
   Note this is the one place a duel needs a timeout that co-op does not.
6. **Monsters stay in the arena** (section 8). Revisit after playtesting; a
   clean arena is a one-line spawn switch if it turns out to be better.
7. **Exit through a gate is a concession** (section 5). The conceder loses
   the stake and is banked as not survived, so "survived" keeps meaning the
   same thing it does everywhere else in the game.

## 13. Build order

Roughly the size of co-op Phase 1. Each step is independently testable, and
the parity vectors gate the consensus-critical ones.

1. **Schema.** `visits.mode` ("coop" | "duel", default "coop"),
   `visits.stake`, `visits.pot`. A duel outcome on `visit_results`.
2. **Moves.** `v` accepts `mode` and `stake`, deducting the host's stake into
   the pot; `j` must match the stake and be affordable; `lv` and the
   open-visit timeout refund. Claims gain `"duel": "won" | "lost"`. Every
   existing co-op path must be untouched when `mode` is absent.
3. **Engine, both sides byte-identical.** `PlayerAttackPlayer` in
   `combat.cpp` / `combat.ts` with the section 4 draw order; the section 3
   per-round reseed; the section 6 commit and reveal log entries and their
   replay rules; death ordering within a monster pass, for the both-died
   case in section 5.
4. **Parity vectors** before anything is wired up: a scripted duel with
   pinned salts covering a player-vs-player hit, miss, dodge and critical,
   a concession, and a win by death; plus the new entry kinds in the
   settle-hash and compact-encoding vectors.
5. **Settlement.** Winner takes the pot, `DUEL_XP_BASE * loserLevel` XP, the
   loser's ordinary death penalty computed after the stake has left, and
   refusal-to-reveal resolved through the existing abandonment machinery
   (an absent duellist is the loser).
6. **Transport.** `commit` and `reveal` message kinds alongside `action` in
   `CoopTransport` and the devnet relay. The relay stays a dumb pipe.
7. **Runner.** The three-step round with the section 2c tick deadline, the
   invalid-action substitution after reveal, and the log entries. Either a
   `DuelRunner` or a mode of `CoopRunner`; prefer the latter if the shared
   parts stay legible.
8. **UI.** Mode and stake in the lobby, both HP bars in the arena, bump to
   attack, Enter on a gate labelled "concede", and the round phase shown
   ("choose", "waiting", "revealing").
9. **End to end.** A two-browser duel in the Playwright suite, mirroring
   `coop.mjs`: both scenarios being a fought-out duel and a stall resolved by
   abandonment.

Work on a branch (`pvp-duels`), merged to `main` only when a duel is
playable end to end, exactly as `coop-engine` was.
