#ifndef ROG_DUNGEONGAME_HPP
#define ROG_DUNGEONGAME_HPP

#include "combat.hpp"
#include "dungeon.hpp"
#include "monsters.hpp"

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
  };

  Type type;
  int dx = 0, dy = 0;           /* for Move */
  std::string itemId;            /* for UseItem */
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

  static DungeonGame Create (const std::string& seed, int depth,
                              const PlayerStats& stats, int hp, int maxHp,
                              const PotionList& startingPotions = {});

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

};

} // namespace rog

#endif // ROG_DUNGEONGAME_HPP
