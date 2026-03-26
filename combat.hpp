#ifndef ROG_COMBAT_HPP
#define ROG_COMBAT_HPP

#include <random>

namespace rog
{

/**
 * Stats relevant to combat for the player character.
 */
struct PlayerStats
{
  int level = 1;
  int strength = 10;
  int dexterity = 10;
  int constitution = 10;
  int intelligence = 10;

  /* Equipment bonuses (added to base stats for combat calculation).  */
  int equipAttack = 0;
  int equipDefense = 0;
};

/**
 * Result of a single attack.
 */
struct AttackResult
{
  bool hit;       /* false = miss/dodge */
  bool critical;  /* true = critical hit */
  int damage;     /* final damage dealt (0 if miss) */
};

/**
 * Calculates the player's total attack power.
 * Formula: strength + level/2 + equipment bonus.
 */
int PlayerAttackPower (const PlayerStats& stats);

/**
 * Calculates the player's total defense.
 * Formula: constitution/2 + level/3 + equipment bonus.
 */
int PlayerDefense (const PlayerStats& stats);

/**
 * Resolves a player attacking a monster.
 */
AttackResult PlayerAttackMonster (const PlayerStats& stats,
                                   int monsterDefense,
                                   std::mt19937& rng);

/**
 * Resolves a monster attacking the player.
 * Returns the damage dealt (0 if dodged).
 */
AttackResult MonsterAttackPlayer (int monsterAttack, int monsterCritChance,
                                   const PlayerStats& stats,
                                   std::mt19937& rng);

} // namespace rog

#endif // ROG_COMBAT_HPP
