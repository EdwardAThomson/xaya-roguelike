#include "monsters.hpp"
#include "dungeon.hpp"

#include <algorithm>
#include <cmath>

namespace rog
{

const std::vector<MonsterTemplate>&
GetMonsterTemplates ()
{
  static const std::vector<MonsterTemplate> templates = {
    /* Depth 1 monsters.  */
    {"Giant Rat",    "r", 1, 24, 5,  2, 3,  5, 10},
    {"Giant Spider", "s", 1, 24, 7,  3, 5,  5, 12},
    {"Cave Bat",     "b", 1, 15, 8,  2, 12, 6, 8},

    /* Depth 2 monsters.  */
    {"Goblin",   "g", 2, 30, 8,  4, 5,  6, 18},
    {"Skeleton", "S", 2, 40, 10, 4, 3,  5, 22},
    {"Kobold",   "k", 2, 45, 9,  3, 4,  6, 20},
    {"Wolf",     "w", 2, 40, 10, 3, 6,  7, 20},

    /* Depth 3+ monsters.  */
    {"Orc Warrior", "O", 3, 65, 12, 6, 5, 7, 35},
    {"Specter",     "P", 3, 60, 15, 5, 8, 8, 40},
    {"Centaur",     "C", 3, 65, 14, 7, 5, 7, 38},

    /* Depth 4+ monsters.  */
    {"Minotaur",  "M", 4, 85, 16, 8, 5, 8, 55},
    {"Dark Mage", "D", 5, 70, 18, 5, 7, 8, 65},
  };

  return templates;
}

Monster
CreateMonster (const MonsterTemplate& tmpl, const int x, const int y,
               const int depth)
{
  Monster m;
  m.name = tmpl.name;
  m.symbol = tmpl.symbol;
  m.x = x;
  m.y = y;

  /* Scale stats by depth.  */
  const double hpScale = 1.0 + (depth - 1) * 0.4;
  const double atkScale = 1.0 + (depth - 1) * 0.3;

  m.maxHp = static_cast<int> (std::floor (tmpl.maxHp * hpScale));
  m.hp = m.maxHp;
  m.attack = static_cast<int> (std::floor (tmpl.attack * atkScale));
  m.defense = tmpl.defense;
  m.critChance = tmpl.critChance;
  m.detectionRange = tmpl.detectionRange;
  m.xpValue = tmpl.xpValue;

  m.alive = true;
  m.awareOfPlayer = false;

  if (depth >= 7)
    m.name = "Elite " + m.name;

  return m;
}

std::vector<Monster>
SpawnMonsters (const Dungeon& dungeon, const int depth, std::mt19937& rng)
{
  /* Collect eligible templates for this depth.  */
  const auto& allTemplates = GetMonsterTemplates ();
  std::vector<const MonsterTemplate*> eligible;
  for (const auto& t : allTemplates)
    if (t.minDepth <= depth)
      eligible.push_back (&t);

  if (eligible.empty ())
    return {};

  const int count = 8 + depth * 2;
  std::vector<Monster> monsters;

  /* Collect all valid floor positions for spawning.  */
  std::vector<std::pair<int, int>> floorTiles;
  for (int y = 0; y < Dungeon::HEIGHT; y++)
    for (int x = 0; x < Dungeon::WIDTH; x++)
      if (dungeon.GetTile (x, y) == Tile::Floor)
        floorTiles.push_back ({x, y});

  if (floorTiles.empty ())
    return {};

  /* Track occupied positions to avoid stacking.  */
  std::vector<std::pair<int, int>> occupied;

  for (int i = 0; i < count && !floorTiles.empty (); i++)
    {
      /* Pick a random template.  */
      std::uniform_int_distribution<size_t> tmplDist (0, eligible.size () - 1);
      const auto* tmpl = eligible[tmplDist (rng)];

      /* Pick a random floor position.  */
      std::uniform_int_distribution<size_t> posDist (0, floorTiles.size () - 1);
      const size_t posIdx = posDist (rng);
      const auto [mx, my] = floorTiles[posIdx];

      /* Check not already occupied.  */
      bool taken = false;
      for (const auto& [ox, oy] : occupied)
        if (ox == mx && oy == my)
          {
            taken = true;
            break;
          }

      if (taken)
        continue;

      monsters.push_back (CreateMonster (*tmpl, mx, my, depth));
      occupied.push_back ({mx, my});
    }

  return monsters;
}

} // namespace rog
