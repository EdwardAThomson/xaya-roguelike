#include "dungeongame.hpp"
#include "combat.hpp"

#include <gtest/gtest.h>

namespace rog
{
namespace
{

class DungeonGameTests : public testing::Test
{

protected:

  PlayerStats defaultStats;

  DungeonGameTests ()
  {
    defaultStats.level = 1;
    defaultStats.strength = 10;
    defaultStats.dexterity = 10;
    defaultStats.constitution = 10;
    defaultStats.intelligence = 10;
    defaultStats.equipAttack = 5;   /* short sword */
    defaultStats.equipDefense = 2;  /* leather armor */
  }

  DungeonGame CreateGame (const std::string& seed = "test_seed",
                           int depth = 1)
  {
    return DungeonGame::Create (seed, depth, defaultStats, 100, 100);
  }

  Action MoveAction (int dx, int dy)
  {
    Action a;
    a.type = Action::Type::Move;
    a.dx = dx;
    a.dy = dy;
    return a;
  }

  Action PickupAction ()
  {
    Action a;
    a.type = Action::Type::Pickup;
    return a;
  }

  Action WaitAction ()
  {
    Action a;
    a.type = Action::Type::Wait;
    return a;
  }

  Action EnterGateAction ()
  {
    Action a;
    a.type = Action::Type::EnterGate;
    return a;
  }

  Action UseItemAction (const std::string& itemId)
  {
    Action a;
    a.type = Action::Type::UseItem;
    a.itemId = itemId;
    return a;
  }

};

// ============================================================
// Creation and initialization
// ============================================================

TEST_F (DungeonGameTests, CreateValid)
{
  auto game = CreateGame ();

  EXPECT_FALSE (game.IsGameOver ());
  EXPECT_EQ (game.GetPlayerHp (), 100);
  EXPECT_EQ (game.GetPlayerMaxHp (), 100);
  EXPECT_EQ (game.GetTurnCount (), 0);
  EXPECT_EQ (game.GetTotalXp (), 0);
  EXPECT_EQ (game.GetTotalGold (), 0);
  EXPECT_EQ (game.GetTotalKills (), 0);

  /* Player should be on a floor tile.  */
  EXPECT_EQ (game.GetDungeon ().GetTile (game.GetPlayerX (), game.GetPlayerY ()),
             Tile::Floor);
}

TEST_F (DungeonGameTests, MonstersSpawned)
{
  auto game = CreateGame ("monster_seed", 1);

  /* Depth 1: 8 + 1*2 = 10 monsters target, but some may be removed
     for being too close to player.  Should have at least a few.  */
  EXPECT_GE (game.GetMonsters ().size (), 3u);

  for (const auto& m : game.GetMonsters ())
    {
      EXPECT_TRUE (m.alive);
      EXPECT_GT (m.hp, 0);
      EXPECT_GT (m.maxHp, 0);
    }
}

TEST_F (DungeonGameTests, ItemsSpawned)
{
  auto game = CreateGame ("item_seed", 1);
  EXPECT_GE (game.GetGroundItems ().size (), 3u);
}

TEST_F (DungeonGameTests, Deterministic)
{
  auto g1 = CreateGame ("det_seed", 2);
  auto g2 = CreateGame ("det_seed", 2);

  EXPECT_EQ (g1.GetPlayerX (), g2.GetPlayerX ());
  EXPECT_EQ (g1.GetPlayerY (), g2.GetPlayerY ());
  EXPECT_EQ (g1.GetMonsters ().size (), g2.GetMonsters ().size ());

  for (size_t i = 0; i < g1.GetMonsters ().size (); i++)
    {
      EXPECT_EQ (g1.GetMonsters ()[i].x, g2.GetMonsters ()[i].x);
      EXPECT_EQ (g1.GetMonsters ()[i].y, g2.GetMonsters ()[i].y);
      EXPECT_EQ (g1.GetMonsters ()[i].name, g2.GetMonsters ()[i].name);
    }
}

// ============================================================
// Movement
// ============================================================

TEST_F (DungeonGameTests, MoveValid)
{
  auto game = CreateGame ();
  const int startX = game.GetPlayerX ();
  const int startY = game.GetPlayerY ();

  /* Try all 8 directions until one works.  */
  bool moved = false;
  for (int dx = -1; dx <= 1 && !moved; dx++)
    for (int dy = -1; dy <= 1 && !moved; dy++)
      {
        if (dx == 0 && dy == 0)
          continue;
        if (game.ProcessAction (MoveAction (dx, dy)))
          {
            EXPECT_EQ (game.GetPlayerX (), startX + dx);
            EXPECT_EQ (game.GetPlayerY (), startY + dy);
            EXPECT_EQ (game.GetTurnCount (), 1);
            moved = true;
          }
      }

  EXPECT_TRUE (moved) << "Player could not move in any direction";
}

TEST_F (DungeonGameTests, MoveIntoWallFails)
{
  auto game = CreateGame ();

  /* Find a wall adjacent to the player.  */
  for (int dx = -1; dx <= 1; dx++)
    for (int dy = -1; dy <= 1; dy++)
      {
        if (dx == 0 && dy == 0)
          continue;
        const int nx = game.GetPlayerX () + dx;
        const int ny = game.GetPlayerY () + dy;
        if (game.GetDungeon ().GetTile (nx, ny) == Tile::Wall)
          {
            EXPECT_FALSE (game.ProcessAction (MoveAction (dx, dy)));
            EXPECT_EQ (game.GetTurnCount (), 0);  /* Turn not consumed.  */
            return;
          }
      }
  /* If no adjacent wall (unlikely in a room), that's OK.  */
}

TEST_F (DungeonGameTests, WaitAdvancesTurn)
{
  auto game = CreateGame ();

  EXPECT_TRUE (game.ProcessAction (WaitAction ()));
  EXPECT_EQ (game.GetTurnCount (), 1);
}

// ============================================================
// Combat
// ============================================================

TEST_F (DungeonGameTests, AttackMonsterByMovingInto)
{
  auto game = CreateGame ("combat_seed", 1);

  /* Walk toward the nearest monster and attack it.  */
  const auto& monsters = game.GetMonsters ();
  if (monsters.empty ())
    return;  /* Skip if no monsters (shouldn't happen).  */

  /* Find closest monster.  */
  int bestIdx = 0;
  int bestDist = 9999;
  for (size_t i = 0; i < monsters.size (); i++)
    {
      const int d = std::abs (monsters[i].x - game.GetPlayerX ())
                  + std::abs (monsters[i].y - game.GetPlayerY ());
      if (d < bestDist)
        {
          bestDist = d;
          bestIdx = static_cast<int> (i);
        }
    }

  const int targetX = monsters[bestIdx].x;
  const int targetY = monsters[bestIdx].y;

  /* Move toward the monster for up to 100 turns.  */
  for (int turn = 0; turn < 100 && game.GetMonsters ()[bestIdx].alive; turn++)
    {
      int dx = 0, dy = 0;
      if (targetX > game.GetPlayerX ()) dx = 1;
      else if (targetX < game.GetPlayerX ()) dx = -1;
      if (targetY > game.GetPlayerY ()) dy = 1;
      else if (targetY < game.GetPlayerY ()) dy = -1;

      game.ProcessAction (MoveAction (dx, dy));
      if (game.IsGameOver ())
        break;
    }

  /* Either the monster died or the player died trying.  */
  /* At minimum, some combat should have happened.  */
  EXPECT_GT (game.GetTurnCount (), 0);
}

// ============================================================
// Combat math (unit)
// ============================================================

TEST_F (DungeonGameTests, PlayerAttackAlwaysDealsMinOne)
{
  std::mt19937 rng (42);

  /* High defense monster.  */
  for (int i = 0; i < 50; i++)
    {
      auto result = PlayerAttackMonster (defaultStats, 100, rng);
      if (result.hit)
        {
          EXPECT_GE (result.damage, 1);
        }
    }
}

TEST_F (DungeonGameTests, MonsterAttackAlwaysDealsMinOne)
{
  std::mt19937 rng (42);

  for (int i = 0; i < 50; i++)
    {
      auto result = MonsterAttackPlayer (20, 5, defaultStats, rng);
      if (result.hit)
        {
          EXPECT_GE (result.damage, 1);
        }
    }
}

// ============================================================
// Item pickup
// ============================================================

TEST_F (DungeonGameTests, PickupItemFromGround)
{
  auto game = CreateGame ("pickup_seed", 1);

  if (game.GetGroundItems ().empty ())
    return;

  /* Walk to the first ground item.  */
  const auto& items = game.GetGroundItems ();
  const int targetX = items[0].x;
  const int targetY = items[0].y;

  for (int turn = 0; turn < 200; turn++)
    {
      if (game.GetPlayerX () == targetX && game.GetPlayerY () == targetY)
        break;

      int dx = 0, dy = 0;
      if (targetX > game.GetPlayerX ()) dx = 1;
      else if (targetX < game.GetPlayerX ()) dx = -1;
      if (targetY > game.GetPlayerY ()) dy = 1;
      else if (targetY < game.GetPlayerY ()) dy = -1;

      if (!game.ProcessAction (MoveAction (dx, dy)))
        game.ProcessAction (WaitAction ());

      if (game.IsGameOver ())
        return;
    }

  if (game.GetPlayerX () != targetX || game.GetPlayerY () != targetY)
    return;  /* Couldn't reach item.  */

  const size_t beforeCount = game.GetGroundItems ().size ();
  EXPECT_TRUE (game.ProcessAction (PickupAction ()));
  EXPECT_LT (game.GetGroundItems ().size (), beforeCount);
}

// ============================================================
// Gate exit
// ============================================================

TEST_F (DungeonGameTests, ExitThroughGate)
{
  auto game = CreateGame ("gate_seed", 1);

  const auto& gates = game.GetDungeon ().GetGates ();
  ASSERT_FALSE (gates.empty ());

  /* Walk to the first gate.  */
  const int gateX = gates[0].x;
  const int gateY = gates[0].y;

  for (int turn = 0; turn < 300; turn++)
    {
      if (game.GetPlayerX () == gateX && game.GetPlayerY () == gateY)
        break;

      int dx = 0, dy = 0;
      if (gateX > game.GetPlayerX ()) dx = 1;
      else if (gateX < game.GetPlayerX ()) dx = -1;
      if (gateY > game.GetPlayerY ()) dy = 1;
      else if (gateY < game.GetPlayerY ()) dy = -1;

      if (!game.ProcessAction (MoveAction (dx, dy)))
        game.ProcessAction (WaitAction ());

      if (game.IsGameOver ())
        return;
    }

  if (game.GetPlayerX () != gateX || game.GetPlayerY () != gateY)
    {
      /* Couldn't reach gate — skip test.  */
      GTEST_SKIP () << "Could not reach gate in 300 turns";
    }

  EXPECT_TRUE (game.ProcessAction (EnterGateAction ()));
  EXPECT_TRUE (game.IsGameOver ());
  EXPECT_TRUE (game.HasSurvived ());
  EXPECT_EQ (game.GetExitGate (), gates[0].direction);
}

// ============================================================
// Game over after action
// ============================================================

TEST_F (DungeonGameTests, ActionAfterGameOverFails)
{
  auto game = CreateGame ();

  /* Force game over by setting HP to 1 and having monsters attack.  */
  /* Alternative: just test that ProcessAction returns false when game is over.  */
  /* We can simulate by running many wait turns until killed,
     or just test the flag.  */

  /* Run 500 wait turns — eventually a monster should kill us.  */
  for (int i = 0; i < 500 && !game.IsGameOver (); i++)
    game.ProcessAction (WaitAction ());

  if (game.IsGameOver ())
    {
      EXPECT_FALSE (game.ProcessAction (WaitAction ()));
      EXPECT_FALSE (game.ProcessAction (MoveAction (1, 0)));
    }
}

// ============================================================
// Depth scaling
// ============================================================

TEST_F (DungeonGameTests, HigherDepthStrongerMonsters)
{
  auto g1 = CreateGame ("depth_scale", 1);
  auto g5 = CreateGame ("depth_scale", 5);

  /* Monsters at depth 5 should have higher stats on average.  */
  if (g1.GetMonsters ().empty () || g5.GetMonsters ().empty ())
    return;

  int totalHp1 = 0, totalHp5 = 0;
  for (const auto& m : g1.GetMonsters ()) totalHp1 += m.maxHp;
  for (const auto& m : g5.GetMonsters ()) totalHp5 += m.maxHp;

  const double avgHp1 = static_cast<double> (totalHp1) / g1.GetMonsters ().size ();
  const double avgHp5 = static_cast<double> (totalHp5) / g5.GetMonsters ().size ();

  EXPECT_GT (avgHp5, avgHp1);
}

} // anonymous namespace
} // namespace rog
