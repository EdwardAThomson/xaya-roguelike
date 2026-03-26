#include "items.hpp"

#include <unordered_map>

namespace rog
{

const std::vector<ItemDef>&
GetAllItems ()
{
  static const std::vector<ItemDef> items = {

    /* === MELEE WEAPONS === */
    {"dagger", "Dagger", "weapon", "weapon",
      3, 0, 0, 1, 0, 0, 0, 0, 10, false, false},
    {"short_sword", "Short Sword", "weapon", "weapon",
      5, 0, 0, 0, 0, 0, 0, 0, 25, false, false},
    {"iron_sword", "Iron Sword", "weapon", "weapon",
      6, 0, 1, 0, 0, 0, 0, 0, 35, false, false},
    {"long_sword", "Long Sword", "weapon", "weapon",
      8, 0, 1, 0, 0, 0, 0, 0, 50, false, false},
    {"scimitar", "Scimitar", "weapon", "weapon",
      7, 0, 0, 1, 0, 0, 0, 0, 38, false, false},
    {"battle_axe", "Battle Axe", "weapon", "weapon",
      10, 0, 2, -1, 0, 0, 0, 0, 65, false, false},
    {"mace", "Iron Mace", "weapon", "weapon",
      7, 0, 1, 0, 1, 0, 0, 0, 42, false, false},
    {"staff", "Wooden Staff", "weapon", "weapon",
      4, 0, 0, 0, 0, 2, 0, 0, 40, false, false},

    /* === HEAD ARMOR === */
    {"leather_cap", "Leather Cap", "armor", "head",
      0, 1, 0, 0, 0, 0, 0, 0, 15, false, false},
    {"iron_helmet", "Iron Helmet", "armor", "head",
      0, 3, 0, 0, 1, 0, 0, 0, 40, false, false},

    /* === BODY ARMOR === */
    {"leather_armor", "Leather Armor", "armor", "body",
      0, 2, 0, 1, 0, 0, 0, 0, 25, false, false},
    {"studded_leather", "Studded Leather", "armor", "body",
      0, 3, 0, 1, 0, 0, 0, 0, 40, false, false},
    {"scale_mail", "Scale Mail", "armor", "body",
      0, 5, 0, 0, 1, 0, 0, 0, 50, false, false},
    {"chainmail", "Chainmail", "armor", "body",
      0, 4, 0, -1, 1, 0, 0, 0, 60, false, false},
    {"plate_armor", "Plate Armor", "armor", "body",
      0, 7, 1, -2, 2, 0, 0, 0, 100, false, false},

    /* === FEET ARMOR === */
    {"leather_boots", "Leather Boots", "armor", "feet",
      0, 1, 0, 1, 0, 0, 0, 0, 20, false, false},
    {"iron_boots", "Iron Boots", "armor", "feet",
      0, 2, 0, -1, 1, 0, 0, 0, 35, false, false},
    {"reinforced_boots", "Reinforced Boots", "armor", "feet",
      0, 1, 0, 1, 1, 0, 0, 0, 28, false, false},

    /* === SHIELDS (offhand) === */
    {"wooden_shield", "Wooden Shield", "armor", "offhand",
      0, 2, 0, 0, 0, 0, 0, 0, 25, false, false},
    {"reinforced_shield", "Reinforced Shield", "armor", "offhand",
      0, 3, 0, 0, 1, 0, 0, 0, 40, false, false},
    {"iron_shield", "Iron Shield", "armor", "offhand",
      0, 4, 0, 0, 1, 0, 0, 0, 60, false, false},
    {"tower_shield", "Tower Shield", "armor", "offhand",
      0, 7, 0, -1, 2, 0, 0, 0, 120, false, false},

    /* === RINGS === */
    {"silver_ring", "Silver Ring", "accessory", "ring",
      0, 2, 3, 1, 0, 0, 0, 0, 45, false, false},
    {"ring_of_protection", "Ring of Protection", "accessory", "ring",
      0, 2, 0, 0, 0, 0, 0, 0, 80, false, false},
    {"ring_of_strength", "Ring of Strength", "accessory", "ring",
      0, 0, 2, 0, 0, 0, 0, 0, 80, false, false},

    /* === AMULETS === */
    {"bone_necklace", "Bone Necklace", "accessory", "amulet",
      2, 0, 1, 0, 0, 0, 0, 0, 35, false, false},
    {"amulet_of_health", "Amulet of Health", "accessory", "amulet",
      0, 0, 0, 0, 1, 0, 15, 0, 75, false, false},

    /* === POTIONS === */
    {"health_potion", "Health Potion", "potion", "",
      0, 0, 0, 0, 0, 0, 0, 20, 15, true, true},
    {"greater_health_potion", "Greater Health Potion", "potion", "",
      0, 0, 0, 0, 0, 0, 0, 50, 40, true, true},
    {"mana_potion", "Mana Potion", "potion", "",
      0, 0, 0, 0, 0, 0, 0, 0, 15, true, true},

    /* === MISC === */
    {"gold_coins", "Gold Coins", "misc", "",
      0, 0, 0, 0, 0, 0, 0, 0, 1, false, true},
  };

  return items;
}

const ItemDef*
LookupItem (const std::string& id)
{
  /* Build a lookup map on first call.  */
  static std::unordered_map<std::string, const ItemDef*> lookup;
  if (lookup.empty ())
    {
      for (const auto& item : GetAllItems ())
        lookup[item.id] = &item;
    }

  auto it = lookup.find (id);
  return it != lookup.end () ? it->second : nullptr;
}

std::vector<const ItemDef*>
GetSpawnableItems (const int depth)
{
  std::vector<const ItemDef*> result;

  for (const auto& item : GetAllItems ())
    {
      /* Skip gold (spawned separately) and non-spawnable types.  */
      if (item.id == "gold_coins" || item.id == "mana_potion")
        continue;

      /* Potions always spawn.  */
      if (item.type == "potion")
        {
          if (item.id == "greater_health_potion" && depth < 3)
            continue;
          result.push_back (&item);
          continue;
        }

      /* Equipment spawns based on value/depth.
         Rough rule: items with value <= depth * 20 can spawn.  */
      if (item.type == "weapon" || item.type == "armor"
          || item.type == "accessory")
        {
          if (item.value <= depth * 20 + 30)
            result.push_back (&item);
        }
    }

  return result;
}

/* ************************************************************************** */

PlayerStats
ComputePlayerStats (sqlite3* db, const std::string& name)
{
  PlayerStats stats;

  /* Read base stats.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `level`, `strength`, `dexterity`, `constitution`, `intelligence`"
    " FROM `players` WHERE `name` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);

  if (sqlite3_step (stmt) == SQLITE_ROW)
    {
      stats.level = static_cast<int> (sqlite3_column_int64 (stmt, 0));
      stats.strength = static_cast<int> (sqlite3_column_int64 (stmt, 1));
      stats.dexterity = static_cast<int> (sqlite3_column_int64 (stmt, 2));
      stats.constitution = static_cast<int> (sqlite3_column_int64 (stmt, 3));
      stats.intelligence = static_cast<int> (sqlite3_column_int64 (stmt, 4));
    }
  sqlite3_finalize (stmt);

  /* Sum equipment bonuses from all equipped items (slot != 'bag').  */
  sqlite3_prepare_v2 (db,
    "SELECT `item_id` FROM `inventory`"
    " WHERE `name` = ?1 AND `slot` != 'bag'",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);

  while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      const char* itemId = reinterpret_cast<const char*> (
          sqlite3_column_text (stmt, 0));
      const ItemDef* def = LookupItem (itemId);
      if (def == nullptr)
        continue;

      stats.equipAttack += def->attackPower;
      stats.equipDefense += def->defense;
      stats.strength += def->strength;
      stats.dexterity += def->dexterity;
      stats.constitution += def->constitution;
      stats.intelligence += def->intelligence;
    }
  sqlite3_finalize (stmt);

  return stats;
}

int
CountInventory (sqlite3* db, const std::string& name)
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT COUNT(*) FROM `inventory` WHERE `name` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  const int count = static_cast<int> (sqlite3_column_int64 (stmt, 0));
  sqlite3_finalize (stmt);
  return count;
}

std::vector<std::pair<std::string, int>>
GetPlayerPotions (sqlite3* db, const std::string& name)
{
  std::vector<std::pair<std::string, int>> potions;

  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `item_id`, `quantity` FROM `inventory`"
    " WHERE `name` = ?1 AND `slot` = 'bag'"
    " AND `item_id` IN ('health_potion', 'greater_health_potion')",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);

  while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      const char* id = reinterpret_cast<const char*> (
          sqlite3_column_text (stmt, 0));
      const int qty = static_cast<int> (sqlite3_column_int64 (stmt, 1));
      potions.push_back ({id, qty});
    }
  sqlite3_finalize (stmt);

  return potions;
}

} // namespace rog
