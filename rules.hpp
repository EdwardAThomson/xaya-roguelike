#ifndef ROG_RULES_HPP
#define ROG_RULES_HPP

namespace rog
{

/**
 * Version handshake between the GSP and its clients.
 *
 * The frontend reimplements the dungeon engine so it can play a run
 * locally, and the GSP replays that run to verify it.  When the two sides
 * disagree about the rules, NOTHING fails at connect time: the player
 * plays a whole run and the settle move is rejected, with the reason in a
 * GSP log line they never see.  These two numbers exist so a client can
 * find that out in the lobby instead, where it costs nobody a run.
 *
 * They are deliberately SEPARATE, because the two kinds of change have
 * different consequences for a client (see docs/PVP_4a_checklist.md, "How
 * we undertake this work"):
 *
 *  - RULES_VERSION covers anything the REPLAY depends on: a draw, an
 *    action, a seed derivation, the round structure, the canonical
 *    encodings, the commitment preimage.  A mismatch means the client's
 *    engine would produce a run this GSP will reject, so the client must
 *    REFUSE TO START a run and say so.  Compare with equality; "newer" is
 *    not "compatible".
 *
 *  - BANKING_VERSION covers what settlement awards: reward pools, the duel
 *    pot and XP, the survival heal, death penalties, timeouts.  The
 *    client's engine is unaffected, so a mismatch is NOT a reason to block
 *    play -- but the client PROJECTS these numbers in its HUD, and a
 *    projection that quietly disagrees with the chain is worse than none.
 *    On a mismatch a client should keep playing and stop predicting.
 *
 * Bump exactly one of them in the same commit as the change it describes.
 * A change that touches both (a new action that also awards something)
 * bumps both.
 */

/**
 * History, so a bump is never guesswork about what it covered:
 *
 *   1  Phase 4a duels.  Commit/reveal round protocol, the per-round
 *      reseed, player-vs-player combat, the commit and reveal log entry
 *      kinds in both the canonical and compact encodings, and the duel
 *      commitment preimage ("rog-duel-commit-v1").
 */
constexpr int RULES_VERSION = 1;

/**
 * History:
 *
 *   1  Phase 4a settlement.  The duel pot payout, DUEL_XP_BASE, the burned
 *      rake, DUEL_ABANDON_TIMEOUT, the clearance-scaled survival heal, and
 *      a duel win taking no heal at all.
 *   2  Asymmetric duel stakes.  Duellists need not match: the host sets a
 *      floor (`visits.min_stake`) rather than a price, each participant
 *      escrows their own amount (`visit_participants.stake`), the pot is
 *      the sum, and a void refunds each of them exactly what they put in
 *      instead of splitting the pot proportionally.  The replay is
 *      untouched, so RULES_VERSION does not move.
 */
constexpr int BANKING_VERSION = 2;

} // namespace rog

#endif // ROG_RULES_HPP
