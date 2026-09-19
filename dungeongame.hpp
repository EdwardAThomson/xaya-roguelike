#ifndef ROG_DUNGEONGAME_HPP
#define ROG_DUNGEONGAME_HPP

#include "combat.hpp"
#include "dungeon.hpp"
#include "monsters.hpp"

#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace rog
{

/**
 * An item lying on the dungeon floor.
 */
struct GroundItem
{
  int x, y;
  std::string itemId;
  int quantity;
};

/**
 * A player action in the dungeon.
 */
struct Action
{
  enum class Type
  {
    Move,       /* dx, dy: -1/0/1 each for 8-directional */
    Pickup,     /* pick up item at current position */
    UseItem,    /* use a consumable (itemId) */
    EnterGate,  /* exit through gate at current position */
    Wait,       /* skip turn */
    Equip,      /* equip a banked bag item (rowid) into slot */
    Unequip,    /* unequip an equipped item (rowid) back to the bag */
    Commit,     /* duel only: SHA-256 commitment to this round's action */
    Reveal,     /* duel only: the salt that opens this round's commitment */
  };

  Type type;
  int dx = 0, dy = 0;           /* for Move */
  std::string itemId;            /* for UseItem */
  int64_t rowid = 0;             /* for Equip/Unequip */
  std::string slot;              /* for Equip */
  /** Lowercase hex payload: the 64-char commitment for Commit, the
      32-char (16-byte) salt for Reveal.  */
  std::string hex;
};

/**
 * The canonical encoding of one action WITHOUT the leading participant
 * index: "move 1 0", "use health_potion", "wait", "commit <h>", ...
 * (SPEC_multiplayer_coop.md section 7).  The settlement consent hash
 * prefixes the index (see CanonicalActionLine in moveprocessor.hpp), and
 * a duel commitment binds exactly this string, so the two encodings can
 * never drift apart.
 */
std::string CanonicalActionBody (const Action& a);

/**
 * The preimage a duel commitment covers (SPEC_multiplayer_pvp.md
 * section 2):
 *
 *   "rog-duel-commit-v1\n" || visitId || "\n" || round || "\n"
 *   || participant || "\n" || canonical(action) || "\n" || hex(salt)
 *
 * Binding the visit and the round means a commitment can never be
 * replayed into another duel or another round of the same one.
 */
std::string DuelCommitPreimage (int64_t visitId, int round, int participant,
                                 const Action& action,
                                 const std::string& saltHex);

/** SHA-256 hex of DuelCommitPreimage: what a `commit` entry carries.  */
std::string DuelCommitHash (int64_t visitId, int round, int participant,
                             const Action& action,
                             const std::string& saltHex);

/**
 * One entry of a multiplayer merged action log: which participant
 * (canonical index, see SPEC_multiplayer_coop.md) performed the action.
 */
struct LoggedAction
{
  int actor;
  Action action;
};

/**
 * A row of the player's on-chain inventory carried into the run so
 * mid-run equip/unequip actions can be verified and replayed.
 * slot in {"bag","weapon","offhand","head","body","feet","ring","amulet"}.
 */
struct EntryInventoryItem
{
  int64_t rowid;
  std::string itemId;
  std::string slot;
};

/**
 * A single entry of the run's final loadout: which inventory rowid ended
 * up in which slot ("bag" for un-equipped).  Written back on settlement.
 */
struct LoadoutEntry
{
  int64_t rowid;
  std::string slot;
};

/**
 * Items collected during the dungeon session (for settlement).
 */
struct CollectedItem
{
  std::string itemId;
  int quantity;
};

/**
 * A complete dungeon game session.  Deterministic: same seed + depth +
 * player stats + action sequence = identical outcome on every node.
 *
 * The engine holds N participants (SPEC_multiplayer_coop.md); the
 * long-standing single-player API is preserved as delegates to
 * participant 0, and with one participant every code path degenerates
 * to the original solo behaviour byte-for-byte (this is consensus:
 * settled solo runs re-verify through this class).
 */
class DungeonGame
{

public:

  /**
   * Starting potions a player brings into the dungeon.
   * Each pair is (itemId, quantity).
   */
  using PotionList = std::vector<std::pair<std::string, int>>;

  using EntryInventory = std::vector<EntryInventoryItem>;

  /**
   * Co-op (the default: every existing visit, and every solo run) or a
   * hostile 1v1 duel (SPEC_multiplayer_pvp.md).  Duel mode is the ONLY
   * thing that enables the commit/reveal round protocol, the per-round
   * reseed and player-vs-player attacks, so a co-op replay takes none of
   * those paths and stays byte-identical.
   */
  enum class Mode
  {
    Coop,
    Duel,
  };

  /**
   * Where a duel round stands (spec section 2).  Co-op is always in Act.
   */
  enum class Phase
  {
    Commit,
    Reveal,
    Act,
  };

  /**
   * Everything one participant carries into a run.  The stats passed in
   * are ALREADY effective (base + entry-equipped bonuses).
   */
  struct PlayerSetup
  {
    PlayerStats stats;
    int hp;
    int maxHp;
    PotionList potions;
    EntryInventory inventory;
    /** Entry gate direction ("" = spawn at the room centre).  */
    std::string entryDir;
  };

private:

  struct EquippedItem { int64_t rowid; std::string itemId; };
  struct BagItem { int64_t rowid; std::string itemId; };

  /**
   * Per-participant state.  `loot` holds this-run pickups (never
   * equippable); `bag` is the banked un-equipped inventory carried in.
   */
  struct PlayerState
  {
    int x = 0, y = 0;
    int hp = 0, maxHp = 0;
    PlayerStats stats;
    std::map<std::string, EquippedItem> equipped;
    std::vector<BagItem> bag;
    int totalXp = 0;
    int totalGold = 0;
    int totalKills = 0;
    /** Total damage dealt to monsters, capped at each target's remaining
        HP (spec §5): the basis of the pro-rata pool split at settlement. */
    int damageDealt = 0;
    std::vector<CollectedItem> loot;
    bool dead = false;
    bool exited = false;
    /** Marked absent by an abandonment settle (spec section 11): inactive
        from that point on, banked as a forfeit.  */
    bool absent = false;
    std::string exitGate;  /* direction of exit gate, or "" */
    /** Damage dealt to other participants (duels only).  Deliberately NOT
        part of `damageDealt`: the co-op pools split by damage dealt to
        MONSTERS (pvp spec section 8).  */
    int pvpDamage = 0;
    /** 1-based order of death within the run, 0 while alive.  The duel
        tie-break when a monster pass kills both duellists needs to know
        who died later (pvp spec section 5).  */
    int deathSeq = 0;
  };

  Dungeon dungeon;
  std::mt19937 rng;

  /** Participants in canonical order (index = canonical index).  */
  std::vector<PlayerState> players;

  /** Next participant expected to act (round structure, spec §2).  */
  int curTurn = 0;

  /* Dungeon entities.  */
  std::vector<Monster> monsters;
  std::vector<GroundItem> groundItems;

  /* Session tracking.  */
  int turnCount;

  /* Run-level kill-reward pools (spec §5/5a).  XP accrues here for every
     kill in addition to the killer's own counter; monster gold drops
     accrue here instead of the floor ONLY with more than one participant
     (solo keeps floor drops: solo replay must stay byte-identical).  */
  int xpPool = 0;
  int killGoldPool = 0;

  bool gameOver;

  int depth;

  /** Recorded action history for replay verification (solo view).  */
  std::vector<Action> actionLog;

  /** Same history with actor indices (multiplayer merged log).  */
  std::vector<LoggedAction> mergedLog;

  /* ---- Duel state (all inert in Mode::Coop) ---------------------------- */

  Mode mode = Mode::Coop;

  /** The visit this duel belongs to; bound into every commitment so a
      commit can never be replayed into another duel.  */
  int64_t duelVisitId = 0;

  /** Step of the current round's commit/reveal/apply protocol.  */
  Phase phase = Phase::Act;

  /** 0-based round counter, the `t` of the commitment and the reseed.  */
  int roundIndex = 0;

  /** This round's commitments and revealed salts, per participant
      (empty = not supplied this round).  */
  std::vector<std::string> roundCommits;
  std::vector<std::string> roundSalts;

  /** Deaths so far, for PlayerState::deathSeq.  */
  int deathCounter = 0;

  /** The decided duel winner's canonical index, or -1 while undecided.
      Latched the first time at most one participant is active, so a
      concession settles the duel on the spot and nothing later can
      overturn it.  */
  int duelWinner = -1;

  /** Number of participants that are currently active.  */
  int ActiveCount () const;

  /**
   * Latches the duel result if it is now decided (at most one active
   * participant), and ends the run when it is.  Evaluated after each
   * applied action and once at the end of each monster pass -- never
   * between two monsters, so a pass that kills both duellists plays out
   * in full and is resolved by death order (spec section 5).
   */
  void CheckDuelEnd ();

  /**
   * Reseeds the shared stream from the round's revealed salts (spec
   * section 3): HashSeed(s_0 + ":" + s_1 + ... + ":" + t), with the salts
   * of the round's active participants in canonical order.  Called once
   * per duel round, between the last reveal and the first action, and
   * nowhere else.
   */
  void ReseedForRound ();

  /**
   * Applies one action's effects for participant `actor`, with no turn
   * bookkeeping and nothing logged.  Returns false, having changed
   * nothing, if the action is not applicable (blocked move, empty
   * pickup, potion the participant does not hold, ...).  Co-op fails the
   * replay on a false; a duel substitutes a wait (spec section 2).
   */
  bool ApplyActionEffects (int actor, const Action& action);

  /** Passes the turn on after an action in the Act phase: monsters act
      once the round's last active participant has gone.  */
  void AdvanceTurn (int actor);

  /** True iff participant i is neither dead nor exited.  */
  bool IsActive (int i) const
  {
    const auto& p = players[i];
    return !p.dead && !p.exited && !p.absent;
  }

  /** First active participant index, or -1 if none.  */
  int FirstActive () const;

  /** Next active participant strictly after i, or -1 if none.  */
  int NextActiveAfter (int i) const;

  /** Active participant occupying (x,y), or -1.  */
  int PlayerAt (int x, int y) const;

  /** Recomputes a player's max HP from effective constitution.  */
  static void RecomputeMaxHp (PlayerState& p);

  /** Processes all monster actions for one turn.  */
  void ProcessMonsterTurns ();

  /** Single monster AI step.  */
  void MonsterAct (Monster& m);

  /** Checks (x,y) is walkable for `self` (no wall, monster, or other
      active participant).  */
  bool IsWalkable (int x, int y, int self) const;

  /** Returns pointer to monster at (x,y), or nullptr.  */
  Monster* MonsterAt (int x, int y);

  /** Returns pointer to ground item at (x,y), or nullptr.  */
  GroundItem* ItemAt (int x, int y);

  /** Manhattan distance.  */
  static int ManhattanDist (int x1, int y1, int x2, int y2);

  /** Simple line-of-sight check (Bresenham).  */
  bool HasLineOfSight (int x1, int y1, int x2, int y2) const;

  /** Spawns ground items deterministically.  */
  void SpawnGroundItems ();

  /** Participant i dies.  */
  void PlayerDied (int i);

  /** Places participant i on entry (spec §2a: gate spawn or deterministic
      ring scan around the room centre; draws no RNG).  */
  void PlacePlayer (int i, const std::string& entryDir);

public:

  DungeonGame ()
      : players (1), roundCommits (1), roundSalts (1)
  {}

  /**
   * Creates a new single-player session (the original API, byte-identical
   * behaviour; delegates to CreateMulti with one participant).
   */
  static DungeonGame Create (const std::string& seed, int depth,
                              const PlayerStats& stats, int hp, int maxHp,
                              const PotionList& startingPotions = {},
                              const std::vector<Gate>& constraints = {},
                              const std::string& entryDir = "",
                              const EntryInventory& entryInventory = {});

  /**
   * Creates a new session with N participants in canonical order.
   */
  static DungeonGame CreateMulti (const std::string& seed, int depth,
                                   const std::vector<PlayerSetup>& setups,
                                   const std::vector<Gate>& constraints = {});

  /**
   * Replays a solo action sequence on a fresh game (original API).
   */
  static DungeonGame Replay (const std::string& seed, int depth,
                              const PlayerStats& stats, int hp, int maxHp,
                              const PotionList& startingPotions,
                              const std::vector<Action>& actions,
                              const std::vector<Gate>& constraints = {},
                              const std::string& entryDir = "",
                              const EntryInventory& entryInventory = {});

  /**
   * Replays a merged multiplayer log on a fresh game.  Stops at the first
   * invalid action (including a wrong-turn actor), like the solo replay.
   */
  static DungeonGame ReplayMulti (const std::string& seed, int depth,
                                   const std::vector<PlayerSetup>& setups,
                                   const std::vector<LoggedAction>& actions,
                                   const std::vector<Gate>& constraints = {});

  /**
   * Creates a hostile duel between the participants (SPEC_multiplayer_pvp.md).
   * `visitId` is bound into every commitment, so a commit made in one duel
   * can never be replayed into another.  The dungeon, its monsters and its
   * items are generated exactly as for a co-op run on the same segment --
   * the arena stays lively (spec section 8) -- and the run opens in the
   * Commit phase of round 0.
   */
  static DungeonGame CreateDuel (const std::string& seed, int depth,
                                  const std::vector<PlayerSetup>& setups,
                                  int64_t visitId,
                                  const std::vector<Gate>& constraints = {});

  /**
   * Replays a duel's merged log (commit, reveal and action entries) on a
   * fresh game.  Stops at the first entry that breaks the round protocol,
   * opens a commitment incorrectly, or is out of turn.
   */
  static DungeonGame ReplayDuel (const std::string& seed, int depth,
                                  const std::vector<PlayerSetup>& setups,
                                  int64_t visitId,
                                  const std::vector<LoggedAction>& actions,
                                  const std::vector<Gate>& constraints = {});

  /**
   * Processes one action by participant `actor`.  Returns false (turn not
   * consumed, nothing logged) if the action is invalid or it is not this
   * participant's turn under the round structure.  After the last active
   * participant of a round acts, monsters take their turn.
   */
  bool ProcessAction (int actor, const Action& action);

  /** Solo shorthand: participant 0 acts (original API).  */
  bool ProcessAction (const Action& action)
  { return ProcessAction (0, action); }

  /**
   * Marks participant i absent (spec section 11): they take no further
   * part, monsters ignore them, and they are banked as not having exited.
   * If it was their turn, the turn passes on exactly as if they had been
   * skipped; if they were the last active participant of the round, the
   * monsters act.  Deterministic and mirrored by the frontend engine.
   */
  void MarkAbsent (int i);
  bool IsPlayerAbsent (int i) const { return players[i].absent; }

  /* Duel accessors (SPEC_multiplayer_pvp.md).  */
  bool IsDuel () const { return mode == Mode::Duel; }
  Phase GetPhase () const { return phase; }
  int GetRoundIndex () const { return roundIndex; }
  int GetPvpDamage (int i) const { return players[i].pvpDamage; }
  int GetDeathSeq (int i) const { return players[i].deathSeq; }

  /**
   * The duel's winner by canonical index, or -1 while it is still
   * undecided.  Decided the moment at most one participant is active:
   * the survivor wins, a conceder (gate exit) hands the win to the other
   * side, and a monster pass that kills both is resolved in favour of
   * whoever died later (spec section 5).
   */
  int GetDuelWinner () const { return duelWinner; }

  /* Multiplayer accessors.  */
  int GetPlayerCount () const { return players.size (); }
  int NextActor () const { return curTurn; }
  bool IsPlayerActive (int i) const { return IsActive (i); }
  bool IsPlayerDead (int i) const { return players[i].dead; }
  bool HasPlayerExited (int i) const { return players[i].exited; }
  int GetPlayerX (int i) const { return players[i].x; }
  int GetPlayerY (int i) const { return players[i].y; }
  int GetPlayerHp (int i) const { return players[i].hp; }
  int GetPlayerMaxHp (int i) const { return players[i].maxHp; }
  int GetTotalXp (int i) const { return players[i].totalXp; }
  int GetTotalGold (int i) const { return players[i].totalGold; }
  int GetTotalKills (int i) const { return players[i].totalKills; }
  const std::string& GetExitGate (int i) const { return players[i].exitGate; }
  int GetDamageDealt (int i) const { return players[i].damageDealt; }
  int GetXpPool () const { return xpPool; }
  int GetKillGoldPool () const { return killGoldPool; }

  /**
   * Monsters present in this run, and how many of them were killed.  The
   * spawn count is the post-cull one (monsters that spawned within 5 tiles
   * of a participant are removed before play), so it is what the run
   * actually had to fight; monsters are never erased once play starts,
   * only marked dead.  The settlement layer uses the ratio to scale the
   * survival heal -- see SurvivalHealPercent in moveprocessor.hpp.
   */
  int GetMonsterCount () const { return monsters.size (); }
  int GetMonstersSlain () const
  {
    int n = 0;
    for (const auto& m : monsters)
      if (!m.alive)
        n++;
    return n;
  }
  const std::vector<CollectedItem>& GetLoot (int i) const
  { return players[i].loot; }
  std::vector<LoadoutEntry> GetFinalInventory (int i) const;
  const std::vector<LoggedAction>& GetMergedLog () const { return mergedLog; }

  /* Original solo accessors (participant 0).  */
  int GetPlayerX () const { return players[0].x; }
  int GetPlayerY () const { return players[0].y; }
  int GetPlayerHp () const { return players[0].hp; }
  int GetPlayerMaxHp () const { return players[0].maxHp; }
  int GetTurnCount () const { return turnCount; }
  bool IsGameOver () const { return gameOver; }
  bool HasSurvived () const { return players[0].exited; }
  const std::string& GetExitGate () const { return players[0].exitGate; }
  int GetTotalXp () const { return players[0].totalXp; }
  int GetTotalGold () const { return players[0].totalGold; }
  int GetTotalKills () const { return players[0].totalKills; }
  const std::vector<CollectedItem>& GetLoot () const
  { return players[0].loot; }
  const Dungeon& GetDungeon () const { return dungeon; }
  const std::vector<Monster>& GetMonsters () const { return monsters; }
  const std::vector<GroundItem>& GetGroundItems () const { return groundItems; }
  int GetDepth () const { return depth; }
  const std::vector<Action>& GetActionLog () const { return actionLog; }

  /**
   * Returns the run's final loadout for participant 0 (original API).
   */
  std::vector<LoadoutEntry> GetFinalInventory () const
  { return GetFinalInventory (0); }

  /** Returns a serialized snapshot of the RNG state.  */
  std::string SerializeRng () const;

  /** Restores the RNG state from a serialized snapshot.  */
  void RestoreRng (const std::string& data);

  /** Provides mutable access to the RNG (for state restoration).  */
  std::mt19937& GetRng () { return rng; }

  /** Sets all participant-0 state fields (for deserialization from
      proto; solo channels only).  */
  void SetState (int px, int py, int hp, int maxHp,
                 int turns, int xp, int gold, int kills,
                 bool over, bool surv, const std::string& gate);

  /** Sets participant 0's stats.  */
  void SetStats (const PlayerStats& s) { players[0].stats = s; }

  /** Mutable access to monsters (for deserialization).  */
  std::vector<Monster>& MutableMonsters () { return monsters; }

  /** Mutable access to ground items (for deserialization).  */
  std::vector<GroundItem>& MutableGroundItems () { return groundItems; }

  /** Mutable access to participant 0's loot (for deserialization).  */
  std::vector<CollectedItem>& MutableLoot () { return players[0].loot; }

  /** Sets the dungeon (for deserialization).  */
  void SetDungeon (Dungeon&& d) { dungeon = std::move (d); }

  /** Sets the depth.  */
  void SetDepth (int d) { depth = d; }

};

} // namespace rog

#endif // ROG_DUNGEONGAME_HPP
