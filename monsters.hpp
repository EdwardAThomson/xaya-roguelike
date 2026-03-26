#ifndef ROG_MONSTERS_HPP
#define ROG_MONSTERS_HPP

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace rog
{

/**
 * A monster instance in the dungeon.
 */
struct Monster
{
  std::string name;
  std::string symbol;

  int x, y;

  int hp, maxHp;
  int attack;
  int defense;
  int critChance;      /* percent (0-100) */
  int detectionRange;  /* Manhattan distance */
  int xpValue;

  bool alive;
  bool awareOfPlayer;
};

/**
 * A template for creating monsters of a given type.
 */
struct MonsterTemplate
{
  std::string name;
  std::string symbol;
  int minDepth;        /* minimum dungeon depth to appear */
  int maxHp;
  int attack;
  int defense;
  int critChance;
  int detectionRange;
  int xpValue;
};

/**
 * Returns the global monster template database.
 */
const std::vector<MonsterTemplate>& GetMonsterTemplates ();

/**
 * Creates a monster instance from a template, optionally scaled
 * by difficulty (depth).  At depth 1, stats are base values.
 * Higher depths scale HP by (1 + (depth-1)*0.4) and attack
 * by (1 + (depth-1)*0.3).
 */
Monster CreateMonster (const MonsterTemplate& tmpl, int x, int y, int depth);

/**
 * Spawns monsters for a dungeon at the given depth.  Uses the RNG
 * for deterministic placement.  Returns the spawned monsters.
 * Count = 8 + depth * 2.
 */
std::vector<Monster> SpawnMonsters (const class Dungeon& dungeon,
                                     int depth, std::mt19937& rng);

} // namespace rog

#endif // ROG_MONSTERS_HPP
