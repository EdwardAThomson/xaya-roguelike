#ifndef ROG_ITEMS_HPP
#define ROG_ITEMS_HPP

#include "combat.hpp"

#include <sqlite3.h>

#include <string>
#include <vector>

namespace rog
{

/**
 * Static definition of an item type.
 */
struct ItemDef
{
  std::string id;
  std::string name;
  std::string type;     /* weapon, armor, accessory, potion, misc */
  std::string slot;     /* weapon, head, body, feet, offhand, ring, amulet, "" */

  int attackPower = 0;
  int defense = 0;
  int strength = 0;
  int dexterity = 0;
  int constitution = 0;
  int intelligence = 0;
  int maxHealth = 0;

  int healAmount = 0;   /* for potions */
  int value = 0;        /* gold value */

  bool consumable = false;
  bool stackable = false;
};

/**
 * Looks up an item definition by id.  Returns nullptr if not found.
 */
const ItemDef* LookupItem (const std::string& id);

/**
 * Returns all item definitions in the database.
 */
const std::vector<ItemDef>& GetAllItems ();

/**
 * Returns item definitions suitable for ground spawning at the given
 * dungeon depth.  Higher depth = chance of better items.
 */
std::vector<const ItemDef*> GetSpawnableItems (int depth);

/**
 * Maximum number of inventory rows per player.
 */
static constexpr int MAX_INVENTORY = 20;

/**
 * Computes a player's effective combat stats by reading their base stats
 * from the database and adding bonuses from all equipped items.
 */
PlayerStats ComputePlayerStats (sqlite3* db, const std::string& name);

/**
 * Counts the number of inventory rows for a player.
 */
int CountInventory (sqlite3* db, const std::string& name);

/**
 * Returns the player's consumable potions (from inventory bag slot)
 * as a vector of (itemId, quantity) pairs.  Used when entering a dungeon
 * so the player can use their potions inside the session.
 */
std::vector<std::pair<std::string, int>>
GetPlayerPotions (sqlite3* db, const std::string& name);

} // namespace rog

#endif // ROG_ITEMS_HPP
