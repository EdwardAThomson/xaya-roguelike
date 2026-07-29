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
 */
class DungeonGame
{

private:

  Dungeon dungeon;
  std::mt19937 rng;

  /* Player state.  */
  int playerX, playerY;
  int playerHp, playerMaxHp;
  PlayerStats stats;

  /* Equipment carried into the run, mutated by Equip/Unequip actions.
     `equipped` maps slot -> {rowid,itemId}; `bag` is the un-equipped
     banked inventory.  This-run pickups live in `loot`, NOT `bag`, so
     they are never equippable.  Populated from the entry inventory in
     Create; empty for callers that pass none (existing behaviour).  */
  struct EquippedItem { int64_t rowid; std::string itemId; };
  struct BagItem { int64_t rowid; std::string itemId; };
  std::map<std::string, EquippedItem> equipped;
  std::vector<BagItem> bag;

  /** Recomputes max HP from effective constitution and clamps current HP. */
  void RecomputeMaxHp ();

  /* Dungeon entities.  */
  std::vector<Monster> monsters;
  std::vector<GroundItem> groundItems;

  /* Session tracking.  */
  int turnCount;
  int totalXp;
  int totalGold;
  int totalKills;
  std::vector<CollectedItem> loot;

  bool gameOver;
  bool survived;
  std::string exitGate;  /* direction of exit gate, or "" */

  int depth;

  /** Recorded action history for replay verification.  */
  std::vector<Action> actionLog;

  /** Processes all monster actions for one turn.  */
  void ProcessMonsterTurns ();

  /** Single monster AI step.  */
  void MonsterAct (Monster& m);

  /** Checks if (x,y) is walkable (floor or gate, no monster).  */
  bool IsWalkable (int x, int y) const;

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

  /** Player dies.  */
  void PlayerDied ();

public:

  DungeonGame () = default;

  /**
   * Creates a new dungeon game session.
   * seed + depth determine the dungeon layout and monster/item placement.
   * stats determine the player's combat capabilities.
   */
  /**
   * Starting potions the player brings into the dungeon.
   * Each pair is (itemId, quantity).
   */
  using PotionList = std::vector<std::pair<std::string, int>>;

  using EntryInventory = std::vector<EntryInventoryItem>;

  static DungeonGame Create (const std::string& seed, int depth,
                              const PlayerStats& stats, int hp, int maxHp,
                              const PotionList& startingPotions = {},
                              const std::vector<Gate>& constraints = {},
                              const std::string& entryDir = "",
                              const EntryInventory& entryInventory = {});

  /**
   * Replays an action sequence on a fresh game and returns the resulting
   * game state.  Used for channel verification: the GSP creates a game
   * from the seed, replays the player's claimed actions, and checks
   * that the results match.
   */
  static DungeonGame Replay (const std::string& seed, int depth,
                              const PlayerStats& stats, int hp, int maxHp,
                              const PotionList& startingPotions,
                              const std::vector<Action>& actions,
                              const std::vector<Gate>& constraints = {},
                              const std::string& entryDir = "",
                              const EntryInventory& entryInventory = {});

  /**
   * Processes one player action.  Returns true if the action was valid
   * and processed, false if invalid (game continues, turn not consumed).
   * After processing, monsters take their turn.
   */
  bool ProcessAction (const Action& action);

  /* Accessors.  */
  int GetPlayerX () const { return playerX; }
  int GetPlayerY () const { return playerY; }
  int GetPlayerHp () const { return playerHp; }
  int GetPlayerMaxHp () const { return playerMaxHp; }
  int GetTurnCount () const { return turnCount; }
  bool IsGameOver () const { return gameOver; }
  bool HasSurvived () const { return survived; }
  const std::string& GetExitGate () const { return exitGate; }
  int GetTotalXp () const { return totalXp; }
  int GetTotalGold () const { return totalGold; }
  int GetTotalKills () const { return totalKills; }
  const std::vector<CollectedItem>& GetLoot () const { return loot; }
  const Dungeon& GetDungeon () const { return dungeon; }
  const std::vector<Monster>& GetMonsters () const { return monsters; }
  const std::vector<GroundItem>& GetGroundItems () const { return groundItems; }
  int GetDepth () const { return depth; }
  const std::vector<Action>& GetActionLog () const { return actionLog; }

  /**
   * Returns the run's final loadout: for every inventory row carried into
   * the run, which slot it ended up in ("bag" if un-equipped).  Empty when
   * no entry inventory was supplied.  Used on settlement to persist the
   * effect of mid-run equip/unequip actions to the inventory table.
   */
  std::vector<LoadoutEntry> GetFinalInventory () const;

  /** Returns a serialized snapshot of the RNG state.  */
  std::string SerializeRng () const;

  /** Restores the RNG state from a serialized snapshot.  */
  void RestoreRng (const std::string& data);

  /** Provides mutable access to the RNG (for state restoration).  */
  std::mt19937& GetRng () { return rng; }

  /** Sets all state fields (for deserialization from proto).  */
  void SetState (int px, int py, int hp, int maxHp,
                 int turns, int xp, int gold, int kills,
                 bool over, bool surv, const std::string& gate);

  /** Sets the player stats.  */
  void SetStats (const PlayerStats& s) { stats = s; }

  /** Mutable access to monsters (for deserialization).  */
  std::vector<Monster>& MutableMonsters () { return monsters; }

  /** Mutable access to ground items (for deserialization).  */
  std::vector<GroundItem>& MutableGroundItems () { return groundItems; }

  /** Mutable access to loot (for deserialization).  */
  std::vector<CollectedItem>& MutableLoot () { return loot; }

  /** Sets the dungeon (for deserialization).  */
  void SetDungeon (Dungeon&& d) { dungeon = std::move (d); }

  /** Sets the depth.  */
  void SetDepth (int d) { depth = d; }

};

} // namespace rog

#endif // ROG_DUNGEONGAME_HPP
