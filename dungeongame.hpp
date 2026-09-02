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
  };

  Type type;
  int dx = 0, dy = 0;           /* for Move */
  std::string itemId;            /* for UseItem */
  int64_t rowid = 0;             /* for Equip/Unequip */
  std::string slot;              /* for Equip */
};

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
    std::vector<CollectedItem> loot;
    bool dead = false;
    bool exited = false;
    std::string exitGate;  /* direction of exit gate, or "" */
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

  bool gameOver;

  int depth;

  /** Recorded action history for replay verification (solo view).  */
  std::vector<Action> actionLog;

  /** Same history with actor indices (multiplayer merged log).  */
  std::vector<LoggedAction> mergedLog;

  /** True iff participant i is neither dead nor exited.  */
  bool IsActive (int i) const
  {
    const auto& p = players[i];
    return !p.dead && !p.exited;
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
      : players (1)
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
   * Processes one action by participant `actor`.  Returns false (turn not
   * consumed, nothing logged) if the action is invalid or it is not this
   * participant's turn under the round structure.  After the last active
   * participant of a round acts, monsters take their turn.
   */
  bool ProcessAction (int actor, const Action& action);

  /** Solo shorthand: participant 0 acts (original API).  */
  bool ProcessAction (const Action& action)
  { return ProcessAction (0, action); }

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
