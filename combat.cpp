#include "combat.hpp"

#include <algorithm>

namespace rog
{

int
PlayerAttackPower (const PlayerStats& stats)
{
  return stats.strength + stats.level / 2 + stats.equipAttack;
}

int
PlayerDefense (const PlayerStats& stats)
{
  return stats.constitution / 2 + stats.level / 3 + stats.equipDefense;
}

AttackResult
PlayerAttackMonster (const PlayerStats& stats, const int monsterDefense,
                      std::mt19937& rng)
{
  AttackResult res;
  res.hit = true;
  res.critical = false;
  res.damage = 0;

  const int baseDmg = PlayerAttackPower (stats);

  /* Miss chance: min(25%, monsterDef / (baseDmg + monsterDef) * 40%).  */
  const double missChance = std::min (0.25,
      static_cast<double> (monsterDefense)
      / (baseDmg + monsterDefense) * 0.4);

  std::uniform_int_distribution<int> pctDist (1, 100);
  if (pctDist (rng) <= static_cast<int> (missChance * 100))
    {
      res.hit = false;
      return res;
    }

  /* Damage variance: 80-120%.  */
  std::uniform_int_distribution<int> varDist (80, 120);
  double dmg = baseDmg * varDist (rng) / 100.0;

  /* Critical hit: 5 + dex/5 percent.  */
  const int critChance = 5 + stats.dexterity / 5;
  if (pctDist (rng) <= critChance)
    {
      res.critical = true;
      dmg *= 1.5;
    }

  /* Subtract monster defense.  */
  res.damage = std::max (1, static_cast<int> (dmg) - monsterDefense);
  return res;
}

AttackResult
MonsterAttackPlayer (const int monsterAttack, const int monsterCritChance,
                      const PlayerStats& stats, std::mt19937& rng)
{
  AttackResult res;
  res.hit = true;
  res.critical = false;
  res.damage = 0;

  const int playerDef = PlayerDefense (stats);

  /* Dodge chance: 5 + dex*0.5, max 50%.  */
  const int dodgeChance = std::min (50,
      5 + static_cast<int> (stats.dexterity * 0.5));

  std::uniform_int_distribution<int> pctDist (1, 100);
  if (pctDist (rng) <= dodgeChance)
    {
      res.hit = false;
      return res;
    }

  /* Damage variance: 90-110%.  */
  std::uniform_int_distribution<int> varDist (90, 110);
  double dmg = monsterAttack * varDist (rng) / 100.0;

  /* Monster crit.  */
  if (pctDist (rng) <= monsterCritChance)
    {
      res.critical = true;
      dmg *= 1.5;
    }

  /* Subtract player defense.  */
  res.damage = std::max (1, static_cast<int> (dmg) - playerDef);
  return res;
}

} // namespace rog
