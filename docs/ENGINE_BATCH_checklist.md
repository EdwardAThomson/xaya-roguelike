# Engine batch: join-in-progress, duel arena, swap, reward scaling

_Opened 2026-09-18. Four consensus changes that must ship together because
each one re-pins the cross-language parity fixtures, and re-pinning them is
the expensive part. One `RULES_VERSION` bump, one `BANKING_VERSION` bump, one
genesis reset._

Every item here changes `dungeongame.cpp` and `src/game/session.ts` in
lockstep, or changes what settlement awards. Nothing in this file is a
frontend-only fix; those were landed separately during the 2026-09-16/18 play
sessions.

Normative specs: `SPEC_multiplayer_coop.md` section 12 (join in progress),
`SPEC_multiplayer_pvp.md` (duels). Read those first; this file is the work
order, not the design.

## Why these four and not others

They were found by playing, not by reading. Each one is a case where the
implemented rules are defensible in isolation but wrong at the table:

1. **Join in progress.** Hosting traded a live run for an empty lobby.
2. **Duel arena spawn.** Both duellists spawn at the gate they walked in
   through, so a duel starts with the two of them on top of each other,
   next to a tile where one keystroke concedes the pot.
3. **Walking past each other.** Participants block each other's tiles, so in
   a one-wide corridor you cannot get past your partner, and the move is not
   merely refused, it is invalid and costs you the round.
4. **Rewards do not scale with the party.** Monsters are `8 + depth * 2`
   regardless of headcount and the pools split pro rata, so a bigger party
   is strictly worse per head. This is what actually caps N, not latency.

## Decisions already taken (do not reopen)

- [x] **Join index is the host's next checkpoint, not the last, and not a
      client-declared index.** Reasoning in spec section 12. Clients must
      force a checkpoint on seeing a join, so it feels like walking through
      the door.
- [x] **Duels stay agree-to-start.** Escrow and the per-round reseed assume
      both parties from round zero. `j` against an `active` visit is co-op
      only.
- [x] **A run nobody joins is just a solo run.** No penalty, no refund, no
      timeout. `VISIT_OPEN_TIMEOUT` stops applying to co-op.
- [x] **Co-op gets swap, duels do not get move-past.** Bumping an ally
      exchanges places; bumping a hostile attacks, and someone standing
      their ground blocks you. Decided 2026-09-16.
- [x] **Party size target is 4**, with every piece written N-general and the
      cap as a constant, to be chosen by playtesting rather than protocol.
      Measured: round latency is flat in N from 2 to 8 (299ms protocol floor,
      about 1.3s with human think time, both unchanged across that range), so
      latency is not the constraint.

## Open questions to answer before coding

- [ ] **1. Does the swap rule apply to a pending participant?** It cannot:
      a pending participant is not on the board. Confirm there is no window
      where a joiner is placed onto a tile someone swaps into in the same
      round. The ring scan runs at activation, before the entry at that
      index is consumed, so this should be structurally impossible. Verify
      rather than assume.
- [ ] **2. How far apart is "maximally separated" for a duel spawn?** Needs
      a deterministic rule both languages implement identically. Candidate:
      of the room centres, in generation order, take the pair with the
      greatest Manhattan distance, excluding any tile within `k` of a gate;
      assign by participant index. Pick `k` and pin it.
- [ ] **3. Does monster count scale linearly with seats?** `count * n` makes
      an 8-party as rich per head as a solo run but turns the segment into a
      slaughterhouse. A sublinear factor keeps some of the co-op discount.
      Decide the curve and whether the "clear every monster within Manhattan
      5 of any participant" spawn rule (`dungeongame.cpp:518`) should stop
      compounding with party size, since a bigger party currently deletes
      more of the population on the way in.
- [ ] **4. Asymmetric duel stakes ride along or not?** Design is settled
      (host ante plus a `min_stake` floor, per-participant escrow in
      `visit_participants.stake`, pot is the sum, winner takes all). It is
      `BANKING_VERSION` only and re-pins nothing, so it could ship alone,
      but it needs the same schema reset. Recommend including it.

## Group A: join in progress (spec section 12)

- [ ] **5. Schema.** `visit_participants.joined_at_action INTEGER NOT NULL
      DEFAULT 0`. Zero means present from the start, which is every existing
      row. A mid-run join inserts `-1`, meaning pending resolution.
- [ ] **6. `moveparser.cpp`: accept a join against an active visit.**
      `HandleJoin` currently refuses anything whose status is not `open`.
      Accept `active` for co-op only; duels keep requiring `open`. Adjacency
      and `max_players` checks unchanged.
- [ ] **7. `moveprocessor.cpp`: hosting no longer waits.** `ProcessVisit`
      creates a co-op visit as `active` with the host alone, instead of
      `open`. The activation block that fires when a visit fills stays for
      duels.
- [ ] **8. `moveprocessor.cpp`: resolve the join index.** `ProcessJoin` on an
      active visit inserts with `joined_at_action = -1`.
      `ProcessSettleConfirm` resolves every pending row to that confirm's
      `n` when the confirm is the host's. This is the whole pinning
      mechanism.
- [ ] **9. `ApplySettlementBody`: read and pass the indices.** Read
      `joined_at_action` alongside `entry_direction`. Setups keep coming
      from current player state: stat and inventory moves are frozen for the
      whole of any visit, so a joiner's state at settlement is the state they
      walked in with. No snapshot table.
- [ ] **10. Engine: pending participants.** `PlayerState.pending`;
      `IsActive` (`dungeongame.hpp:321`) false while set; `AddParticipant(i)`
      mirroring `MarkAbsent(i)`, clearing pending and calling `PlacePlayer`
      with the recorded entry direction. The replay activates at exactly the
      join index, before consuming that entry. Turn order needs no change:
      `FirstActive`/`NextActive` already skip inactive participants.
- [ ] **11. `session.ts`: the same, byte-identical.**
- [ ] **12. Frontend: force a checkpoint on join.** `maybeCheckpoint` gains a
      path that fires the moment a join is seen on chain, not on the
      16-action or 30-second interval. Without this the joiner waits out the
      heartbeat.
- [ ] **13. Frontend: runner and lobby.** `CoopRunner` must cope with a
      participant appearing partway through the log. The Co-op tab's host
      state becomes "open to company" rather than "waiting for a partner",
      and the wait-state dialog added on 2026-09-18 can go, since there is no
      longer a wait.

## Group B: duel arena spawn

- [ ] **14. Stop anchoring duel spawns to the entry gate.** In duel mode
      ignore `entryDir` for placement and use the answer to question 2.
      Co-op keeps arriving at its own gates: a party walking in together is
      the point there.
- [ ] **15. Test: same-gate arrivals.** N participants entering through one
      direction must land on distinct reachable floor tiles with no runaway
      scan. The ring scan has never been exercised this way.

## Group C: walking past each other

- [ ] **16. Co-op swap.** Moving into an active ally exchanges coordinates.
      One branch in the move handler; no tile ever holds two players, so
      monster AI, targeting and rendering are untouched. `PlayerAt` already
      filters to active participants, so the dead, the absent and the pending
      cannot be swapped with.
- [ ] **17. Duel round ordering.** Actions apply in participant order, so if
      your opponent vacates a tile and you move into it in the same round,
      whether you attack or walk in is decided by which name sorts first.
      Evaluate the bump against positions as they stood when the round
      opened, which is what a commit-reveal round already means.

## Group D: rewards

- [ ] **18. Scale monster count with seats** per question 3, and settle the
      clearing-radius interaction.
- [ ] **19. Fresh per-visit seed** (`PVP_4a_checklist.md` item 22). Same
      fixtures re-pin, so it costs nothing extra here and closes the
      dungeon-persistence question. Bring it into this batch.

## Group E: verification and release

- [ ] **20. New co-op parity vector: a mid-run join.** Pinned byte for byte
      in `tests/coop_parity_tests.cpp` and reproduced by the frontend's
      `npm test`.
- [ ] **21. Re-pin every affected fixture:** solo equip vector, co-op
      vectors, duel vectors.
- [ ] **22. Commit the N-scaling harness** so "round latency is flat in N"
      is pinned rather than remembered.
- [ ] **23. Version bumps in the same commit as the change**, with a line in
      the `rules.hpp` history comment: `RULES_VERSION` for groups A, B and C,
      `BANKING_VERSION` for group D and for asymmetric stakes.
- [ ] **24. Both suites, then the devnet.** `ctest` here and `npm test` in
      the frontend, then `devnet/smoke_test.py` plus a two-client join-in-
      progress run.
- [ ] **25. Genesis reset**, and update `ROADMAP.md` and the spec status
      lines.

## Sequencing

Group A is the biggest and everything else is independent of it, so start
there. B and C are small and can go in any order. D should be last, because
it is a balance change and wants the rest stable underneath it. E runs
alongside rather than at the end: each group re-pins its own fixtures as it
lands.
