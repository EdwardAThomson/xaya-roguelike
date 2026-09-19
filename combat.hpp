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

/**
 * Resolves one hostile participant attacking another in a duel
 * (SPEC_multiplayer_pvp.md section 4).  Composes the existing formulas in
 * a fixed draw order so that cross-language parity is mechanical:
 *
 *   1. miss   (attacker's roll, as against a monster), returning BEFORE
 *      the dodge roll is drawn -- the draw count is consensus;
 *   2. dodge  (defender's roll, as against a monster);
 *   3. variance 80-120% of the attacker's power;
 *   4. critical, 5 + dex/5 percent, multiplying by 1.5;
 *   5. damage max(1, floor(dmg) - defender's defense).
 *
 * There is no retaliation roll: the defender answers on its own action.
 * Mirrored byte-for-byte by the frontend (combat.ts).
 */
AttackResult PlayerAttackPlayer (const PlayerStats& attacker,
                                  const PlayerStats& defender,
                                  std::mt19937& rng);

} // namespace rog

#endif // ROG_COMBAT_HPP
