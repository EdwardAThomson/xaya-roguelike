# Item staking checklist (duels)

Working checklist for letting a duellist stake ITEMS instead of, or as well
as, gold. See `SPEC_multiplayer_pvp.md` section 5 for the stake model as it
stands, `PVP_4a_checklist.md` for the conventions this file follows, and
ROADMAP.md for where duels sit overall.

**How to use it.** Items are numbered and stable, refer to them by number.
Groups are ordered so each can be done in one sitting. Group A is decisions
only (no code); B is the escrow, which is the bulk of the work; C is the
space precondition; D is settlement; E is the frontend mirror; F is the
gate before merging. Tick items as they land and record decisions in the log
at the bottom.

Status: **5 of 19 done** (2026-09-20). Group A decided; no code yet.
Group B, the escrow, is next.

---

## Why this is worth doing

Gold is not what players care about, and more practically it is not what a
new player HAS. `GiveStartingItems` (moveprocessor.cpp:375) hands every
fresh character a short sword (value 25), leather armor (value 25) and 3
health potions, while `players.gold` starts at zero. Today that character
cannot enter a staked duel at all, and the e2e scripts prove the point:
both `duel.mjs` and `duel_adversarial.mjs` run at stake 0 because there is
nothing to stake without farming first. Item staking gives a duel real
consequence from the first minute of play.

It also closes a real gap in the test suite. With item stakes the two-
browser duel can escrow, fight and transfer property against a live chain
with no gold-farming preamble, which is the one consensus path in 4a that
has never run outside the unit tests.

## Class: banking only

Every item here is **Class 1** in the `PVP_4a_checklist.md` taxonomy. The
replay never sees a stake: `ApplySettlementBody` verifies the run, and the
stake is applied around it. So RULES_VERSION does NOT move, the parity
vectors do not re-pin, and the frontend engine needs no change. It does
change what settlement awards, so:

- **BANKING_VERSION goes to 3**, one bump, same commit, with a line added to
  the history comment in `rules.hpp`.
- It changes chain history, so it needs a genesis reset or a height gate.
- The frontend must know, because its HUD projects the payout. On a
  BANKING mismatch a client keeps playing and stops predicting, which is
  already the handshake's contract.

The move shape stays additive, the way 4a's did: `v` and `j` gain an
optional `stake_items`, absent means exactly today's behaviour. An older
client keeps working and simply cannot stake an item.

---

## Group A: decisions, no code (one sitting)

**All five decided 2026-09-20.** Answers and reasoning in the log at the
bottom.

- [x] **1. Can a stake include EQUIPPED gear, or only bag rows?**
      Recommendation: **bag rows only**. Equipped gear is the character's
      loadout; staking the armor you are standing in raises an ordering
      question at settlement (the loser is mid-death-penalty, the winner may
      have no free slot) for no gain, since a player who wants to stake
      their sword can unequip it first with `uq`. Bag-only also makes the
      space check in group C a simple row count against `MAX_INVENTORY`.

- [x] **2. How does an item stake compare against the host's floor?**
      `visits.min_stake` is an integer and the host sets a floor rather than
      a price (BANKING_VERSION 2). `ItemDef.value` (items.hpp:33) is
      populated for every definition, so the natural answer is that a
      stake's worth is `sum(value * quantity)` over its rows, and gold
      counts at face value. Decide whether a mixed stake (some gold, some
      items) is allowed; recommendation is yes, because it falls out of the
      same sum and refusing it is extra code.

- [x] **3. Are partial stacks stakeable?** Staking 2 of 3 health potions
      means splitting a row at escrow time and merging it back on refund.
      Recommendation: **whole rows only** for the first version. It costs
      the player nothing (they can discard down) and removes a class of
      quantity-accounting bugs from the escrow.

- [x] **4. What does a void or cancel return?** `RefundStakesToParticipants`
      (moveprocessor.cpp:526) already pays each participant back exactly
      what they put in rather than a share of the pot, which is the right
      precedent: refund the EXACT rows. Confirm that an item refund is
      whole-row identity, not "an item of equal value".

- [x] **5. Does the rake apply?** `DUEL_RAKE_PERCENT` is 0 today
      (moveprocessor.hpp:360), so this is dormant, but it cannot stay
      unanswered in the code: you cannot burn 10% of a sword. Decide now
      whether a non-zero rake applies to the gold portion only, or whether
      item stakes are simply exempt, and put the answer in a comment next
      to the constant so a later change to it does not have to rediscover
      the problem.

---

## Group B: escrow (the bulk of the work)

- [ ] **6. Schema: an escrow marker on inventory rows.** Gold escrow is a
      scalar decrement (`DeductStake`, moveprocessor.cpp:481). An item
      escrow has to name rows. Add `inventory.escrowed_visit` (INTEGER
      NULL, the visit id holding it) rather than a separate table, so the
      row keeps its identity and rowid across escrow and refund, which is
      what decision 4 requires. Edit `schema.sql` only; CMake generates
      both variants.

- [ ] **7. `DeductItemStake` / `RefundItemStake`.** Mirror the two gold
      functions. Deduct sets `escrowed_visit`; refund clears it. Both must
      be all-or-nothing: a stake that cannot be fully escrowed (a row that
      is already escrowed, equipped, or not owned) fails the whole move, the
      way `DeductStake` returning false refuses the visit at
      moveprocessor.cpp:849.

- [ ] **8. Guard every move that touches inventory.** An escrowed row must
      not be equipped, used, unequipped into, or discarded while the duel
      is live. Four handlers need the check:
      `ProcessUseItem` (:1775), `ProcessEquip` (:1824),
      `ProcessUnequip` (:1868), `ProcessDiscardItem` (:1884).
      This is the item that makes escrow real; missing one of the four is
      how a player stakes a sword and discards it in the same block.

- [ ] **9. Timeout and prune paths.** The sweeper that voids stale open
      duels (moveprocessor.cpp:2810) refunds the pot; it must refund item
      escrow too, and pruning a provisional segment must not orphan an
      escrowed row. An item stuck as `escrowed_visit = <dead visit>` is
      permanently lost to its owner, which is worse than any payout bug
      because nothing surfaces it.

---

## Group C: the space precondition

The full-bag case must be impossible at settlement, not resolved there.
Settlement has to close, always. Today the loot path silently drops the
overflow (moveprocessor.cpp:2285 and :2295, `inventory full, dropping`),
which is a fine policy for treasure a player found and a terrible one for
property the winner just won.

- [ ] **10. Compute the rows a win would need.** Once the challenger has
      joined, both stakes are known, so the requirement is exact: the count
      of non-stackable staked rows, plus one row per stackable item type the
      receiver does not already hold a bag stack of (a potion merges into an
      existing stack and costs zero rows, per the `merged` branch at :2272).

- [ ] **11. Check it on `v` and `j`, in the GSP.** Both sides must pass,
      since neither knows in advance who wins. Refuse the move if
      `CountInventory(db, name) + needed > MAX_INVENTORY` (50, items.hpp:60).
      This belongs in the move handlers, NOT only in the lobby: a
      client-side check alone is a client that can lie.

- [ ] **12. Mirror it in the lobby for a decent error.** "Make room before
      you can duel for this" in the UI beats a move the chain silently
      refuses. In practice needing two or three free rows out of fifty
      almost never bites, which is the point: it is a precondition that
      nearly always passes.

---

## Group D: settlement

- [ ] **13. Transfer the staked rows to the winner.** In
      `BankPlayerSettlement` (:2175), reassign `inventory.name` for every
      row escrowed to this visit and clear `escrowed_visit`.

- [ ] **14. Stake BEFORE run loot. This is what makes group C sound.**
      The winner's bag also receives the duel run's own loot at settlement,
      so space verified at open can be eaten before the pot is awarded
      unless the staked rows go in first. Transfer at :2175 must run
      ahead of the loot loop at :2255, and the existing drop-on-overflow
      policy then applies only to found treasure. That is the correct
      priority regardless of the space check.

- [ ] **15. Bump BANKING_VERSION to 3** with its history line in
      `rules.hpp`, in the same commit as group D.

---

## Group E: the frontend mirror

- [ ] **16. Stake picker in the duel lobby.** `src/ui/modal.ts` holds the
      stake modal and `src/net/moves.ts` the move builder; both currently
      know only the gold amount. The picker needs the bag list, the running
      `value` total against the host's floor, and the group C space warning.

---

## Group F: before merging

- [ ] **17. Both suites, then a live staked duel.** `ctest` here and
      `npm test` in the frontend (the parity vectors must NOT move; if they
      do, something leaked into the replay and the class is wrong). Then
      `npm run duel` with a real item stake, asserting the rows actually
      changed hands, and `npm run duel:evil` extended with a stake that is
      not owned, a stake that is already escrowed, and a winner whose bag
      has no room.

- [ ] **18. Update ROADMAP, the spec's section 5, and this file's status
      line.**

---

## Decisions log

**All five decided 2026-09-20.**

**1. Bag rows only; equipped gear cannot be staked.** A player who wants to
wager their sword unequips it first with `uq`, which costs one move and
nothing else, so the restriction removes a case without removing an option.
What it buys: the group C space check stays a plain row count against
`MAX_INVENTORY`, and settlement never has to decide what happens when the
loser's death penalty and the transfer of the armor they are standing in
land in the same move.

**2. A stake is worth `sum(value * quantity)` over its rows, gold at face
value, and a mixed stake is allowed.** `ItemDef.value` is populated for every
definition and is the only common scale in the game, so the floor comparison
in `j` is that sum against `visits.min_stake`. Mixed falls out of the same
arithmetic and refusing it would be extra code.

Known and accepted: a host who set a floor of 100 gold and is met with a
sword worth 100 got something less liquid than they asked for. Value is
value for v1. A "gold only" host flag can be added additively later if that
turns out to matter in play.

**3. Whole rows only; no partial stacks. ACCEPTED FOR V1, TO BE FIXED.**
Splitting a row at escrow and merging it back on refund is a class of
quantity bugs for little gain today, so v1 does not do it. The consequence
lands on the starting loadout: a fresh character's only stackable row is 3
health potions, so their choices are the sword, the armor, or all three
potions. This is a deliberate v1 limitation and not a closed question;
see item 19.

**4. A void refunds the exact rows, by identity, never an item of equal
value.** This follows `RefundStakesToParticipants`, which already pays each
participant back what they put in rather than a share of the pot. Equal
value is not the same object, and a player who staked a specific sword is
owed that sword.

**5. The rake is charged in gold, and the requirement belongs to the POT,
not to each player.** A percentage needs something divisible and an item is
not, so the rake is never taken from an item. Instead the pot must hold
enough gold to cover it, and because the pot is both stakes combined it does
not matter whose gold that is: a goldless character can stake their sword
against an opponent who brought cash. The tax then comes off the pot the
winner receives, which is exactly the incidence it has today, so the loser
never pays tax on a loss.

Checked at `j`, where both stakes are first known, alongside the group C
space check. The one case it does not cover is a duel where both sides stake
only items and the pot holds no gold at all: that is refused at join, rather
than silently waiving the rake, because a waiver is the exemption this
decision exists to avoid.

Rejected alternatives, recorded so they are not re-proposed: destroying an
item to pay the rake (far more than a percentage), exempting item stakes
(creates an incentive to wager gear purely to dodge the tax, so the rake
collects nothing the day it goes above zero), and a per-player gold rider
(works, but denies the goldless new player the very feature this is for).

**Until it is built:** `DUEL_RAKE_PERCENT` is 0 (moveprocessor.hpp:360) and
none of this is reachable. Guard it with a `static_assert
(DUEL_RAKE_PERCENT == 0)` beside the item-staking settlement code, with a
comment pointing here. The build then fails the moment someone raises the
rake, which is when the work actually has to happen and when there will be
real duels to size it against.

---

## Follow-ups (not v1)

- [ ] **19. Partial stacks.** Let a player stake 2 of 3 potions: split the
      row at escrow, merge it back on refund. Deferred from decision 3.
