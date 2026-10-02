/**
 * The travel action (SPEC_multiplayer_pvp.md section 2e): one action that
 * walks up to TRAVEL_MAX_STEPS tiles in a direction, stopping early on a
 * wall, an occupied tile, a gate, a ground item, or something coming into
 * view.  Engine semantics here, plus a solo parity vector the frontend
 * must reproduce byte-for-byte (PARITY-TRAVEL); the duel vector lives with
 * the other duel vectors in duel_parity_tests.cpp.
 */

#include "dungeongame.hpp"
#include "moveprocessor.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

namespace rog
{
namespace
{

Action
Travel (const int dx, const int dy)
{
  Action a;
  a.type = Action::Type::Travel;
  a.dx = dx;
  a.dy = dy;
  return a;
}

Action
Move (const int dx, const int dy)
{
  Action a;
  a.type = Action::Type::Move;
  a.dx = dx;
  a.dy = dy;
  return a;
}

Action
Wait ()
{
  Action a;
  a.type = Action::Type::Wait;
  return a;
}

/* The solo parity-equip fixture: depth 3 entered from the south, which
   puts the player on the gate mouth at (14, 38) at the foot of a long
   north-south corridor (x = 13) with a monster waiting at its top.  */
PlayerStats
FixtureStats ()
{
  PlayerStats stats;
  stats.level = 3;
  stats.strength = 12;
  stats.dexterity = 11;
  stats.constitution = 10;
  stats.intelligence = 10;
  return stats;
}

DungeonGame
SoloFixture ()
{
  return DungeonGame::Create ("parity-equip", 3, FixtureStats (), 80, 100,
                              {}, {}, "south");
}

TEST (TravelTests, CanonicalForm)
{
  EXPECT_EQ (CanonicalActionBody (Travel (-1, 1)), "travel -1 1");
  EXPECT_EQ (CanonicalActionLine (1, Travel (0, -1)), "1 travel 0 -1\n");
}

TEST (TravelTests, BadDirectionRejected)
{
  auto game = SoloFixture ();
  EXPECT_FALSE (game.ProcessAction (Travel (0, 0)));
  EXPECT_FALSE (game.ProcessAction (Travel (2, 0)));
  EXPECT_FALSE (game.ProcessAction (Travel (0, -2)));
  EXPECT_EQ (game.GetTurnCount (), 0);
}

TEST (TravelTests, BlockedFirstStepIsInapplicable)
{
  auto game = SoloFixture ();
  ASSERT_EQ (game.GetPlayerX (), 14);
  ASSERT_EQ (game.GetPlayerY (), 38);

  /* Straight north of the mouth is wall: nothing to walk, so the action
     is not applicable, exactly like the same blocked move.  */
  EXPECT_FALSE (game.ProcessAction (Travel (0, -1)));
  EXPECT_FALSE (game.ProcessAction (Move (0, -1)));
  EXPECT_EQ (game.GetTurnCount (), 0);
}

TEST (TravelTests, StopsAtWallAndCapsAtMaxSteps)
{
  auto game = SoloFixture ();

  /* One diagonal step into the corridor; the next tile on that diagonal
     is wall, so it stops after one step and still counts as one action.  */
  ASSERT_TRUE (game.ProcessAction (Travel (-1, -1)));
  EXPECT_EQ (game.GetPlayerX (), 13);
  EXPECT_EQ (game.GetPlayerY (), 37);
  EXPECT_EQ (game.GetTurnCount (), 1);

  /* Then up the corridor: TRAVEL_MAX_STEPS tiles and no more.  */
  ASSERT_TRUE (game.ProcessAction (Travel (0, -1)));
  EXPECT_EQ (game.GetPlayerX (), 13);
  EXPECT_EQ (game.GetPlayerY (), 37 - DungeonGame::TRAVEL_MAX_STEPS);
  EXPECT_EQ (game.GetTurnCount (), 2);
  EXPECT_EQ (game.GetActionLog ().size (), 2u);
}

TEST (TravelTests, StopsWhenAMonsterComesIntoView)
{
  auto game = SoloFixture ();
  ASSERT_TRUE (game.ProcessAction (Travel (-1, -1)));
  ASSERT_TRUE (game.ProcessAction (Travel (0, -1)));
  ASSERT_TRUE (game.ProcessAction (Travel (0, -1)));
  ASSERT_EQ (game.GetPlayerY (), 21);

  /* Four more tiles would reach the corridor's end; the monster in the
     room at its top comes into view first.  */
  ASSERT_TRUE (game.ProcessAction (Travel (0, -1)));
  EXPECT_EQ (game.GetPlayerX (), 13);
  EXPECT_EQ (game.GetPlayerY (), 17);

  bool seen = false;
  for (const auto& m : game.GetMonsters ())
    {
      const int dx = m.x - 13, dy = m.y - 17;
      if (m.alive && dx * dx + dy * dy
                       <= DungeonGame::TRAVEL_VIEW_RADIUS
                            * DungeonGame::TRAVEL_VIEW_RADIUS)
        seen = true;
    }
  EXPECT_TRUE (seen);
}

TEST (TravelTests, NeverAttacks)
{
  auto game = SoloFixture ();
  for (const auto& a : {Travel (-1, -1), Travel (0, -1), Travel (0, -1),
                        Travel (0, -1), Travel (1, 0)})
    ASSERT_TRUE (game.ProcessAction (a));

  /* A monster now stands right next to the player, in the way.  */
  const Monster* adjacent = nullptr;
  for (const auto& m : game.GetMonsters ())
    if (m.alive && m.x == game.GetPlayerX () + 1
          && m.y == game.GetPlayerY ())
      adjacent = &m;
  ASSERT_NE (adjacent, nullptr);
  const int hp = adjacent->hp;

  /* Travelling into it is not an attack, just a blocked first step; the
     same direction as a move is.  */
  const int turns = game.GetTurnCount ();
  EXPECT_FALSE (game.ProcessAction (Travel (1, 0)));
  EXPECT_EQ (game.GetTurnCount (), turns);
  EXPECT_EQ (adjacent->hp, hp);
  EXPECT_TRUE (game.ProcessAction (Move (1, 0)));
}

std::vector<DungeonGame::PlayerSetup>
PairThroughSouthGate ()
{
  DungeonGame::PlayerSetup s;
  s.stats = FixtureStats ();
  s.hp = 100;
  s.maxHp = 100;
  s.entryDir = "south";
  return {s, s};
}

TEST (TravelTests, DrawsNoRng)
{
  /* With two participants the monsters wait for the end of the round, so
     the RNG state right after participant 0's travel shows what the
     travel itself drew: nothing.  Participant 1 spawns in the first room,
     out of the way.  */
  auto setups = PairThroughSouthGate ();
  setups[1].entryDir = "";
  auto game = DungeonGame::CreateMulti ("duel-parity-1", 3, setups);
  ASSERT_EQ (game.GetPlayerY (0), 38);

  const std::string before = game.SerializeRng ();
  ASSERT_TRUE (game.ProcessAction (0, Travel (0, -1)));
  EXPECT_LT (game.GetPlayerY (0), 37);
  EXPECT_EQ (game.SerializeRng (), before);
}

TEST (TravelTests, AnAllyDoesNotInterruptButAnOpponentDoes)
{
  /* Co-op: participant 1 starts one tile above the mouth and walks north
     with participant 0 in plain view the whole way; only the wall at the
     corridor's top stops it.  */
  auto coop = DungeonGame::CreateMulti ("duel-parity-1", 3,
                                         PairThroughSouthGate ());
  ASSERT_EQ (coop.GetPlayerY (1), 37);
  ASSERT_TRUE (coop.ProcessAction (0, Wait ()));
  ASSERT_TRUE (coop.ProcessAction (1, Travel (0, -1)));
  EXPECT_EQ (coop.GetPlayerX (1), 56);
  EXPECT_EQ (coop.GetPlayerY (1), 34);

  /* Duel: participant 1 is eight steps in, at (51, 34) on an east-west
     corridor that runs on well past the foot of participant 0's
     corridor.  Walking east, it stops on the first tile from which it
     sees its opponent, short of the eight steps the corridor allows.  */
  auto duel = DungeonGame::CreateDuel ("duel-parity-1", 3,
                                        PairThroughSouthGate (), 11);
  ASSERT_EQ (duel.GetPlayerX (1), 51);
  ASSERT_EQ (duel.GetPlayerY (1), 34);
  const std::string salt0 (32, 'a'), salt1 (32, 'b');
  const Action stay = Wait (), go = Travel (1, 0);
  Action c;
  c.type = Action::Type::Commit;
  c.hex = DuelCommitHash (11, 0, 0, stay, salt0);
  ASSERT_TRUE (duel.ProcessAction (0, c));
  c.hex = DuelCommitHash (11, 0, 1, go, salt1);
  ASSERT_TRUE (duel.ProcessAction (1, c));
  Action r;
  r.type = Action::Type::Reveal;
  r.hex = salt0;
  ASSERT_TRUE (duel.ProcessAction (0, r));
  r.hex = salt1;
  ASSERT_TRUE (duel.ProcessAction (1, r));
  ASSERT_TRUE (duel.ProcessAction (0, stay));
  ASSERT_TRUE (duel.ProcessAction (1, go));
  EXPECT_EQ (duel.GetPlayerX (1), 56);
  EXPECT_EQ (duel.GetPlayerY (1), 34);
  EXPECT_EQ (duel.GetDungeon ().GetTile (57, 34), Tile::Floor);
  for (const auto& m : duel.GetMonsters ())
    {
      const int dx = m.x - 56, dy = m.y - 34;
      EXPECT_GT (dx * dx + dy * dy, 64) << "a monster stopped it, not p0";
    }
}

/* ************************************************************************** */

/**
 * Plays a solo action list on a fresh game, then replays it the way
 * settlement does, and renders the canonical parity line: where the player
 * stood after every action, so a disagreement says which stop rule the two
 * engines read differently, then the outcome and the log's consent hash.
 */
std::string
SoloTravelLine (const char* tag, const std::string& seed, const int depth,
                const std::string& entryDir,
                const std::vector<Action>& actions)
{
  auto game = DungeonGame::Create (seed, depth, FixtureStats (), 80, 100,
                                   {}, {}, entryDir);
  std::string stops;
  std::vector<LoggedAction> log;
  for (const auto& a : actions)
    {
      EXPECT_TRUE (game.ProcessAction (a))
          << tag << ": action " << log.size () << " rejected";
      log.push_back ({0, a});
      stops += " " + std::to_string (game.GetPlayerX ())
             + "," + std::to_string (game.GetPlayerY ());
    }

  auto replay = DungeonGame::Replay (seed, depth, FixtureStats (), 80, 100,
                                     {}, actions, {}, entryDir);
  EXPECT_EQ (replay.GetActionLog ().size (), actions.size ());
  EXPECT_EQ (replay.GetPlayerX (), game.GetPlayerX ());
  EXPECT_EQ (replay.GetPlayerY (), game.GetPlayerY ());
  EXPECT_EQ (replay.GetPlayerHp (), game.GetPlayerHp ());

  char buf[256];
  std::snprintf (buf, sizeof (buf),
                 " hp=%d xp=%d gold=%d kills=%d turns=%d hash=",
                 game.GetPlayerHp (), game.GetTotalXp (),
                 game.GetTotalGold (), game.GetTotalKills (),
                 game.GetTurnCount ());
  const std::string line = tag + stops + buf + SettleLogHash (0, log);
  std::printf ("%s\n", line.c_str ());
  return line;
}

/**
 * Solo parity vectors, diffed byte-for-byte against the frontend's
 * parity_test.ts.  The first is the corridor walk above: a one-step
 * diagonal stopped by a wall, two full eight-step legs, one cut short by a
 * monster coming into view, a single step that ends next to it, a fight
 * with plain moves, and a last single step with the monster still in view.
 */
TEST (TravelTests, ParityTravelVector)
{
  const std::string line = SoloTravelLine (
      "PARITY-TRAVEL", "parity-equip", 3, "south",
      {Travel (-1, -1), Travel (0, -1), Travel (0, -1), Travel (0, -1),
       Travel (1, 0), Move (1, 0), Move (1, 0), Move (1, 0), Move (1, 0),
       Travel (-1, 0)});
  EXPECT_EQ (line,
             "PARITY-TRAVEL 13,37 13,29 13,21 13,17 14,17 14,17 14,17"
             " 14,17 14,17 13,17 hp=44 xp=0 gold=0 kills=0 turns=10"
             " hash=9b8c4c22bb48ec923c1b9230e71e17107d9bb706c72e17013552466e2b22a8dc");
}

/**
 * The item stop: walking north from the south gate on this arena, the
 * travel ends seven steps in, on a ground item, although the corridor
 * goes on.  Then the pickup, and the walk resumes from the item's tile
 * until the corridor ends.
 */
TEST (TravelTests, ParityTravelItemVector)
{
  auto probe = DungeonGame::Create ("travel-36", 2, FixtureStats (), 80, 100,
                                    {}, {}, "south");
  ASSERT_EQ (probe.GetPlayerX (), 42);
  ASSERT_EQ (probe.GetPlayerY (), 38);
  ASSERT_TRUE (probe.ProcessAction (Travel (0, -1)));
  ASSERT_EQ (probe.GetPlayerY (), 31);
  bool onItem = false;
  for (const auto& gi : probe.GetGroundItems ())
    if (gi.x == 42 && gi.y == 31)
      onItem = true;
  EXPECT_TRUE (onItem);
  EXPECT_EQ (probe.GetDungeon ().GetTile (42, 30), Tile::Floor);

  Action pickup;
  pickup.type = Action::Type::Pickup;
  const std::string line = SoloTravelLine (
      "PARITY-TRAVEL-ITEM", "travel-36", 2, "south",
      {Travel (0, -1), pickup, Travel (0, -1)});
  EXPECT_EQ (line,
             "PARITY-TRAVEL-ITEM 42,31 42,31 42,29 hp=80 xp=0 gold=0 kills=0"
             " turns=3 hash=a5721c8f8748755a20a32864732194e4bef0340ba1e39dfe26147094f90154c6");
}

} // anonymous namespace
} // namespace rog
