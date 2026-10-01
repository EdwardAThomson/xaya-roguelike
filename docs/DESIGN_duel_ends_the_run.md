# Should winning a duel end the dungeon run?

_Status: open question, not scheduled. Raised 2026-10-01 while playtesting
duels. Not a bug: the current behaviour is consistent and settles correctly.
The question is whether it is the game we want._

## What happens today

A duel is a **mode of a visit**, not a thing that happens inside one. So when
the duel resolves, the visit resolves with it: the pot pays out, both sides
are banked, the visit is marked completed, and the arena is finished. A
winner who wants to play that dungeon has to enter it again from scratch.

The winner is banked as having survived *without reaching a gate*
(`SPEC_multiplayer_pvp.md`), which is deliberately thin: the pot, duel XP, no
survival heal. They take nothing from the segment itself -- no loot, no
kills, no exploration -- because the run ended the moment the fight did.

## Why it is worth questioning

A duel is currently a self-contained transaction bolted onto a dungeon nobody
plays. You walk into an arena, fight, and leave. The dungeon around you is
scenery: its monsters, its loot and its layout exist only as terrain for the
fight.

That is almost certainly an artefact rather than a decision. Duels were built
on the visit machinery because co-op already was, and "the visit ends when
the duel ends" fell out of that rather than being chosen.

## The alternative

The duel resolves, the run continues. The pot settles and the loser is out;
the **winner carries on alone** in that dungeon, exploring and looting, and
settles later by walking out through a gate with whatever they collected.

A duel then becomes a fight *over* a dungeon rather than a fight *instead of*
one, and winning buys the right to clear it.

## What makes it hard

- **Settlement timing.** The escrow and the pot hang off the visit. Paying
  out mid-visit means a settlement that is not the end of the visit, which is
  new ground for the replay, for the confirm/stale machinery, and for the
  abandonment rules. Today "settled" and "visit over" are the same event.
- **Two settlements or one?** Either the pot pays immediately and the run
  settles again later (two on-chain settlements for one visit), or the pot is
  held until the winner walks out -- in which case a winner who never leaves
  keeps the loser's stake hostage, which the void timeout then has to cover.
- **Risk the duellist did not sign up for.** A winner left alone on low
  health in a half-cleared dungeon can die and lose the run, including the
  pot they just won, unless the pot is banked separately. That might be good
  (stakes continue) or bad (the duel's result is not final when it is
  decided). It needs choosing, not discovering.
- **The loser.** Knocked back as now, presumably. But if the winner keeps
  playing, the loser is waiting on a settlement they cannot influence, which
  interacts with everything the abandonment work covers.

## Interactions

- **Duel spawn separation** (`ENGINE_BATCH_checklist.md` item 14) and the
  per-choice travel change: both make the arena matter more, which makes
  "the dungeon is scenery" more obviously wasteful.
- **Dungeon persistence** (see the dungeon-persistence decision): if
  segments ever stop regenerating per visit, "the duel ate your run" gets
  worse, because the run was a scarce thing.

## Recommendation

Park it. It is a genuine improvement to the game and it is not a bug, so it
does not compete with the spawn and pacing work, which fix things players
notice as broken. Revisit when settlement timing is being touched anyway --
if a future change already splits "pot paid" from "visit closed", most of the
cost here is already being paid.
