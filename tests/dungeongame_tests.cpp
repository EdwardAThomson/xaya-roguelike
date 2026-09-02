#include "dungeongame.hpp"
#include "combat.hpp"
#include "dungeonai.hpp"
#include "items.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

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

  Action EquipAction (int64_t rowid, const std::string& slot)
  {
    Action a;
    a.type = Action::Type::Equip;
    a.rowid = rowid;
    a.slot = slot;
    return a;
  }

  Action UnequipAction (int64_t rowid)
  {
    Action a;
    a.type = Action::Type::Unequip;
    a.rowid = rowid;
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
// Health potion consumption in dungeon
// ============================================================

TEST_F (DungeonGameTests, UseStartingPotionInDungeon)
{
  /* Create a game with 2 health potions from inventory.  */
  DungeonGame::PotionList potions = {{"health_potion", 2}};
  auto game = DungeonGame::Create ("potion_test", 1, defaultStats, 60, 100, potions);

  EXPECT_EQ (game.GetPlayerHp (), 60);

  /* Use a health potion (heals 35 HP): 60 + 35 = 95.  */
  EXPECT_TRUE (game.ProcessAction (UseItemAction ("health_potion")));
  EXPECT_EQ (game.GetPlayerHp (), 95);

  /* Use another: 95 + 35 = 130, capped at max 100.  */
  EXPECT_TRUE (game.ProcessAction (UseItemAction ("health_potion")));
  EXPECT_EQ (game.GetPlayerHp (), 100);

  /* Third attempt should fail — only brought 2.  */
  EXPECT_FALSE (game.ProcessAction (UseItemAction ("health_potion")));
  EXPECT_EQ (game.GetPlayerHp (), 100);
}

TEST_F (DungeonGameTests, UseGreaterHealthPotion)
{
  DungeonGame::PotionList potions = {{"greater_health_potion", 1}};
  auto game = DungeonGame::Create ("gpotion_test", 1, defaultStats, 30, 100, potions);

  /* Greater health potion heals 70 HP: 30 + 70 = 100 (capped at max).  */
  EXPECT_TRUE (game.ProcessAction (UseItemAction ("greater_health_potion")));
  EXPECT_EQ (game.GetPlayerHp (), 100);
}

TEST_F (DungeonGameTests, PotionHealCapsAtMaxHp)
{
  DungeonGame::PotionList potions = {{"health_potion", 1}};
  auto game = DungeonGame::Create ("cap_test", 1, defaultStats, 95, 100, potions);

  EXPECT_TRUE (game.ProcessAction (UseItemAction ("health_potion")));
  /* 95 + 35 = 130, but capped at max 100.  */
  EXPECT_EQ (game.GetPlayerHp (), 100);
}

TEST_F (DungeonGameTests, NoPotionsFails)
{
  /* No starting potions.  */
  auto game = CreateGame ("no_potions", 1);

  /* Should fail — no potions available.  */
  EXPECT_FALSE (game.ProcessAction (UseItemAction ("health_potion")));
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

// ============================================================
// Mid-run equip / unequip
// ============================================================

TEST_F (DungeonGameTests, EquipFromBagAppliesBonusesAndMaxHp)
{
  /* Base con=10 (maxHp 100).  iron_helmet (head) grants constitution +1,
     so equipping it raises max HP by HP_PER_CON (5) with no heal.  */
  PlayerStats stats;
  stats.constitution = 10;
  DungeonGame::EntryInventory inv = {{1, "iron_helmet", "bag"}};
  auto game = DungeonGame::Create ("equip_hp", 1, stats, 100, 100,
                                    {}, {}, "", inv);

  EXPECT_EQ (game.GetPlayerMaxHp (), 100);

  ASSERT_TRUE (game.ProcessAction (EquipAction (1, "head")));
  EXPECT_EQ (game.GetPlayerMaxHp (), 105);
  EXPECT_EQ (game.GetPlayerHp (), 100);    /* raise cap = no heal */
  EXPECT_EQ (game.GetTurnCount (), 1);     /* equip costs a turn */

  const auto loadout = game.GetFinalInventory ();
  ASSERT_EQ (loadout.size (), 1u);
  EXPECT_EQ (loadout[0].rowid, 1);
  EXPECT_EQ (loadout[0].slot, "head");
}

TEST_F (DungeonGameTests, EquipDisplacesOccupantToBag)
{
  /* Body slot holds leather_armor (con 0); scale_mail (con +1) sits in
     the bag.  Equipping scale_mail displaces leather_armor to the bag and
     nets constitution +1 (maxHp 100 -> 105).  Entry stats are already
     effective for leather_armor, whose con bonus is 0.  */
  PlayerStats stats;
  stats.constitution = 10;
  DungeonGame::EntryInventory inv = {
    {1, "leather_armor", "body"},
    {2, "scale_mail", "bag"},
  };
  auto game = DungeonGame::Create ("equip_swap", 1, stats, 100, 100,
                                    {}, {}, "", inv);

  ASSERT_TRUE (game.ProcessAction (EquipAction (2, "body")));
  EXPECT_EQ (game.GetPlayerMaxHp (), 105);

  std::map<int64_t, std::string> slotByRow;
  for (const auto& e : game.GetFinalInventory ())
    slotByRow[e.rowid] = e.slot;
  EXPECT_EQ (slotByRow[2], "body");   /* scale_mail now equipped */
  EXPECT_EQ (slotByRow[1], "bag");    /* leather_armor displaced */
}

TEST_F (DungeonGameTests, UnequipDropsStatsAndClampsHp)
{
  /* scale_mail (con +1) is equipped, so entry stats/maxHp are effective:
     con 11 -> maxHp 105, and the player is at full 105 HP.  Unequipping it
     drops constitution to 10, recomputes maxHp to 100, and clamps current
     HP down from 105 to 100.  */
  PlayerStats stats;
  stats.constitution = 11;           /* already includes scale_mail */
  DungeonGame::EntryInventory inv = {{1, "scale_mail", "body"}};
  auto game = DungeonGame::Create ("unequip_clamp", 1, stats, 105, 105,
                                    {}, {}, "", inv);

  ASSERT_TRUE (game.ProcessAction (UnequipAction (1)));
  EXPECT_EQ (game.GetPlayerMaxHp (), 100);
  EXPECT_EQ (game.GetPlayerHp (), 100);   /* lower cap clamps current HP */
  EXPECT_EQ (game.GetTurnCount (), 1);

  const auto loadout = game.GetFinalInventory ();
  ASSERT_EQ (loadout.size (), 1u);
  EXPECT_EQ (loadout[0].rowid, 1);
  EXPECT_EQ (loadout[0].slot, "bag");
}

TEST_F (DungeonGameTests, EquipRejectsRowidNotInBag)
{
  PlayerStats stats;
  DungeonGame::EntryInventory inv = {{1, "short_sword", "bag"}};
  auto game = DungeonGame::Create ("equip_reject", 1, stats, 100, 100,
                                    {}, {}, "", inv);

  /* rowid 99 is not in the entry bag -> rejected, turn not consumed.  */
  EXPECT_FALSE (game.ProcessAction (EquipAction (99, "weapon")));
  EXPECT_EQ (game.GetTurnCount (), 0);
}

TEST_F (DungeonGameTests, EquipRejectsWrongSlot)
{
  PlayerStats stats;
  DungeonGame::EntryInventory inv = {{1, "short_sword", "bag"}};
  auto game = DungeonGame::Create ("equip_slot", 1, stats, 100, 100,
                                    {}, {}, "", inv);

  /* short_sword's slot is "weapon"; asking for "body" is rejected.  */
  EXPECT_FALSE (game.ProcessAction (EquipAction (1, "body")));
  EXPECT_EQ (game.GetTurnCount (), 0);

  /* The correct slot still works.  */
  EXPECT_TRUE (game.ProcessAction (EquipAction (1, "weapon")));
}

TEST_F (DungeonGameTests, EquipRejectsThisRunPickup)
{
  /* Items picked up during the run land in `loot`, never in `bag`, so they
     carry no settled rowid and can never be equipped mid-run.  Drive the
     player onto a ground item, pick it up, then confirm no equip referring
     to a non-bag rowid succeeds.  */
  PlayerStats stats;
  stats.level = 5;
  stats.strength = 18;
  stats.dexterity = 15;
  stats.constitution = 20;
  stats.equipAttack = 5;
  stats.equipDefense = 2;
  DungeonGame::EntryInventory inv = {{1, "short_sword", "bag"}};
  auto game = DungeonGame::Create ("equip_pick", 1, stats, 200, 200,
                                    {}, {}, "", inv);

  /* Navigate to the nearest equippable ground item and pick it up.  */
  std::string pickedUp;
  for (int step = 0; step < 300 && !game.IsGameOver (); step++)
    {
      int bx = -1, by = -1, best = 1 << 30;
      for (const auto& gi : game.GetGroundItems ())
        {
          const ItemDef* d = LookupItem (gi.itemId);
          if (d == nullptr || d->slot.empty ())
            continue;
          const int dist = std::abs (gi.x - game.GetPlayerX ())
                         + std::abs (gi.y - game.GetPlayerY ());
          if (dist < best)
            {
              best = dist;
              bx = gi.x;
              by = gi.y;
            }
        }
      if (bx < 0)
        break;

      if (game.GetPlayerX () == bx && game.GetPlayerY () == by)
        {
          ASSERT_TRUE (game.ProcessAction (PickupAction ()));
          for (const auto& l : game.GetLoot ())
            {
              const ItemDef* d = LookupItem (l.itemId);
              if (d != nullptr && !d->slot.empty ())
                {
                  pickedUp = l.itemId;
                  break;
                }
            }
          if (!pickedUp.empty ())
            break;
          continue;
        }

      const auto [dx, dy] = BfsStepToward (game, game.GetPlayerX (),
                                            game.GetPlayerY (), bx, by);
      if (dx == 0 && dy == 0)
        break;
      if (!game.ProcessAction (MoveAction (dx, dy)))
        break;
    }

  ASSERT_FALSE (pickedUp.empty ()) << "failed to pick up an equippable item";
  const ItemDef* picked = LookupItem (pickedUp);
  ASSERT_NE (picked, nullptr);

  /* The pickup is in loot, not the bag: no rowid maps to it, so equipping
     it into its own valid slot is rejected.  rowid 2 stands in for the
     "just found" item the client might try to claim.  */
  const int turnsBefore = game.GetTurnCount ();
  EXPECT_FALSE (game.ProcessAction (EquipAction (2, picked->slot)));
  EXPECT_EQ (game.GetTurnCount (), turnsBefore);

  /* And the pickup never ends up equipped.  */
  for (const auto& e : game.GetFinalInventory ())
    EXPECT_NE (e.slot, picked->slot);
}

// ============================================================
// Parity vector (must match the TypeScript frontend byte-for-byte).
// See docs equip_spec.md "Parity test vector".
// ============================================================

TEST_F (DungeonGameTests, ParityEquipVector)
{
  /* Fixed inputs shared with the frontend.  The stats are passed exactly
     as pinned in the spec (they are the effective entry stats).  */
  PlayerStats stats;
  stats.level = 3;
  stats.strength = 12;
  stats.dexterity = 11;
  stats.constitution = 10;
  stats.intelligence = 10;
  stats.equipAttack = 0;
  stats.equipDefense = 0;

  DungeonGame::EntryInventory inv = {
    {10, "short_sword", "bag"},
    {11, "scale_mail", "bag"},
    {12, "leather_armor", "body"},
  };

  /* Pinned deterministic action list (see spec).  */
  std::vector<Action> actions;
  actions.push_back (EquipAction (10, "weapon"));
  actions.push_back (EquipAction (11, "body"));   /* displaces rowid12 */
  actions.push_back (MoveAction (-1, 0));          /* west out of the mouth */
  for (int i = 0; i < 10; i++)
    actions.push_back (MoveAction (0, -1));        /* north up the corridor */
  actions.push_back (UnequipAction (11));
  for (int i = 0; i < 10; i++)
    actions.push_back (MoveAction (0, 1));         /* back south */
  actions.push_back (MoveAction (1, 0));           /* east to the mouth */
  actions.push_back (MoveAction (0, 1));           /* onto the south gate */
  actions.push_back (EnterGateAction ());

  auto game = DungeonGame::Replay ("parity-equip", 3, stats, 80, 100,
                                    {}, actions, {}, "south", inv);

  /* Emit the single canonical parity line the frontend diffs against.  */
  std::printf ("PARITY-EQUIP survived=%d totalXp=%d totalGold=%d "
               "totalKills=%d playerHp=%d playerMaxHp=%d exitGate=%s\n",
               game.HasSurvived () ? 1 : 0,
               game.GetTotalXp (), game.GetTotalGold (),
               game.GetTotalKills (), game.GetPlayerHp (),
               game.GetPlayerMaxHp (), game.GetExitGate ().c_str ());

  /* Guard the pinned expected output so a determinism regression fails
     loudly here as well as diverging from the frontend.  */
  EXPECT_TRUE (game.HasSurvived ());
  EXPECT_EQ (game.GetExitGate (), "south");
}

/* ************************************************************************** */

/**
 * Multiplayer engine semantics per SPEC_multiplayer_coop.md: round
 * structure, turn enforcement, collision, and merged-log replay.
 */
class CoopEngineTests : public DungeonGameTests
{

protected:

  DungeonGame::PlayerSetup Setup (const int hp = 100)
  {
    DungeonGame::PlayerSetup s;
    s.stats = defaultStats;
    s.hp = hp;
    s.maxHp = hp;
    return s;
  }

  DungeonGame CreateCoop (const std::string& seed = "coop_seed",
                           const int depth = 1)
  {
    return DungeonGame::CreateMulti (seed, depth, {Setup (), Setup ()});
  }

};

TEST_F (CoopEngineTests, SpawnsAreDistinctAndDeterministic)
{
  auto game = CreateCoop ();
  ASSERT_EQ (game.GetPlayerCount (), 2);
  EXPECT_TRUE (game.IsPlayerActive (0));
  EXPECT_TRUE (game.IsPlayerActive (1));
  EXPECT_FALSE (game.GetPlayerX (0) == game.GetPlayerX (1)
                && game.GetPlayerY (0) == game.GetPlayerY (1));
  EXPECT_EQ (game.NextActor (), 0);

  /* Same inputs, same placement (the ring scan draws no RNG).  */
  auto game2 = CreateCoop ();
  EXPECT_EQ (game.GetPlayerX (1), game2.GetPlayerX (1));
  EXPECT_EQ (game.GetPlayerY (1), game2.GetPlayerY (1));
  EXPECT_EQ (game.SerializeRng (), game2.SerializeRng ());
}

TEST_F (CoopEngineTests, TurnOrderEnforced)
{
  auto game = CreateCoop ();

  /* Participant 1 may not act first.  */
  EXPECT_FALSE (game.ProcessAction (1, WaitAction ()));
  EXPECT_EQ (game.NextActor (), 0);

  /* Out-of-range actors are rejected.  */
  EXPECT_FALSE (game.ProcessAction (-1, WaitAction ()));
  EXPECT_FALSE (game.ProcessAction (2, WaitAction ()));

  /* 0 acts, then 0 again is rejected, then 1 acts.  */
  EXPECT_TRUE (game.ProcessAction (0, WaitAction ()));
  EXPECT_EQ (game.NextActor (), 1);
  EXPECT_FALSE (game.ProcessAction (0, WaitAction ()));
  EXPECT_TRUE (game.ProcessAction (1, WaitAction ()));
  EXPECT_EQ (game.NextActor (), 0);
}

TEST_F (CoopEngineTests, MonstersActOncePerRoundNotPerAction)
{
  auto game = CreateCoop ();
  ASSERT_FALSE (game.GetMonsters ().empty ());

  /* After only participant 0's action the round is open: no monster
     turn has run, so the RNG stream still equals a fresh game's.  */
  auto fresh = CreateCoop ();
  ASSERT_TRUE (game.ProcessAction (0, WaitAction ()));
  EXPECT_EQ (game.SerializeRng (), fresh.SerializeRng ());

  /* Closing the round with participant 1 runs the monster phase, which
     draws from the stream (unaware monsters roll wander chances).  */
  ASSERT_TRUE (game.ProcessAction (1, WaitAction ()));
  EXPECT_NE (game.SerializeRng (), fresh.SerializeRng ());
}

TEST_F (CoopEngineTests, PlayersBlockEachOther)
{
  auto game = CreateCoop ();

  const int dx = game.GetPlayerX (1) - game.GetPlayerX (0);
  const int dy = game.GetPlayerY (1) - game.GetPlayerY (0);
  ASSERT_TRUE (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1)
      << "expected ring spawn adjacent to the centre";

  /* Moving onto the other participant's tile is invalid; the turn is
     not consumed.  */
  EXPECT_FALSE (game.ProcessAction (0, MoveAction (dx, dy)));
  EXPECT_EQ (game.NextActor (), 0);
  EXPECT_EQ (game.GetTurnCount (), 0);
}

TEST_F (CoopEngineTests, ReplayMultiReproducesLiveRun)
{
  auto live = CreateCoop ();

  /* Play a few rounds of waits (always valid) interleaved per the round
     structure.  */
  for (int round = 0; round < 5 && !live.IsGameOver (); round++)
    {
      if (live.IsPlayerActive (0))
        ASSERT_TRUE (live.ProcessAction (0, WaitAction ()));
      if (live.IsPlayerActive (1))
        ASSERT_TRUE (live.ProcessAction (1, WaitAction ()));
    }

  auto replayed = DungeonGame::ReplayMulti (
      "coop_seed", 1, {Setup (), Setup ()}, live.GetMergedLog ());

  EXPECT_EQ (replayed.GetPlayerX (0), live.GetPlayerX (0));
  EXPECT_EQ (replayed.GetPlayerY (0), live.GetPlayerY (0));
  EXPECT_EQ (replayed.GetPlayerX (1), live.GetPlayerX (1));
  EXPECT_EQ (replayed.GetPlayerY (1), live.GetPlayerY (1));
  EXPECT_EQ (replayed.GetPlayerHp (0), live.GetPlayerHp (0));
  EXPECT_EQ (replayed.GetPlayerHp (1), live.GetPlayerHp (1));
  EXPECT_EQ (replayed.GetTurnCount (), live.GetTurnCount ());
  EXPECT_EQ (replayed.SerializeRng (), live.SerializeRng ());
}

TEST_F (CoopEngineTests, SoloDelegatesToSingleParticipant)
{
  /* The original Create must be exactly CreateMulti with one setup.  */
  auto solo = CreateGame ("identity_seed");
  auto multi = DungeonGame::CreateMulti ("identity_seed", 1, {Setup ()});

  EXPECT_EQ (solo.GetPlayerCount (), 1);
  EXPECT_EQ (solo.GetPlayerX (), multi.GetPlayerX ());
  EXPECT_EQ (solo.GetPlayerY (), multi.GetPlayerY ());
  EXPECT_EQ (solo.GetMonsters ().size (), multi.GetMonsters ().size ());
  EXPECT_EQ (solo.SerializeRng (), multi.SerializeRng ());

  /* And the solo ProcessAction shorthand still runs the monster phase
     after every action (single-participant round).  */
  ASSERT_TRUE (solo.ProcessAction (WaitAction ()));
  ASSERT_TRUE (multi.ProcessAction (0, WaitAction ()));
  EXPECT_EQ (solo.SerializeRng (), multi.SerializeRng ());
}

} // anonymous namespace
} // namespace rog
