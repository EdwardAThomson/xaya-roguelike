#include "dungeongame.hpp"
#include "dungeon.hpp"
#include "moveprocessor.hpp"
#include "items.hpp"
#include "testutils.hpp"

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <queue>
#include <map>
#include <set>
#include <json/json.h>

namespace rog
{
namespace
{

/**
 * BFS pathfinding: find action sequence from player start to a target gate.
 * Returns the actions as a Json::Value array for use in "xc" moves.
 */
Json::Value
FindPathToGate (const std::string& seed, int depth,
                const PlayerStats& stats, int hp, int maxHp,
                const std::string& targetGateDir)
{
  /* Include starting potions (3x health_potion from registration)
     to match what the GSP replay does.  */
  DungeonGame::PotionList potions = {{"health_potion", 3}};
  auto game = DungeonGame::Create (seed, depth, stats, hp, maxHp, potions);

  /* Find the target gate position.  */
  int gateX = -1, gateY = -1;
  for (const auto& g : game.GetDungeon ().GetGates ())
    if (g.direction == targetGateDir)
      {
        gateX = g.x;
        gateY = g.y;
        break;
      }

  if (gateX < 0)
    return Json::Value (Json::arrayValue);

  /* BFS from player position to gate (ignores monsters — the simulation
     handles fighting through them with retry logic below).  */
  using Pos = std::pair<int, int>;
  std::queue<Pos> queue;
  std::map<Pos, Pos> parent;
  Pos start = {game.GetPlayerX (), game.GetPlayerY ()};
  Pos goal = {gateX, gateY};

  queue.push (start);
  parent[start] = {-1, -1};

  static const int dx8[] = {-1, -1, -1, 0, 0, 1, 1, 1};
  static const int dy8[] = {-1, 0, 1, -1, 1, -1, 0, 1};

  while (!queue.empty ())
    {
      auto [cx, cy] = queue.front ();
      queue.pop ();

      if (cx == gateX && cy == gateY)
        break;

      for (int i = 0; i < 8; i++)
        {
          int nx = cx + dx8[i];
          int ny = cy + dy8[i];
          Pos next = {nx, ny};

          if (nx < 0 || nx >= Dungeon::WIDTH || ny < 0 || ny >= Dungeon::HEIGHT)
            continue;
          if (game.GetDungeon ().GetTile (nx, ny) == Tile::Wall)
            continue;
          if (parent.count (next))
            continue;
          /* Monsters are handled by simulation retry below.  */

          parent[next] = {cx, cy};
          queue.push (next);
        }
    }

  /* Trace path back to start.  */
  std::vector<Pos> path;
  Pos cur = goal;
  while (cur != start && parent.count (cur))
    {
      path.push_back (cur);
      cur = parent[cur];
    }
  std::reverse (path.begin (), path.end ());

  /* Convert path to actions.  Walk to the gate, then enter it.
     We actually simulate the game to handle monsters on the path
     correctly (attacks instead of movement, waits to kill adjacent).  */
  DungeonGame::PotionList simPotions = {{"health_potion", 3}};
  auto simGame = DungeonGame::Create (seed, depth, stats, hp, maxHp, simPotions);
  Json::Value actions (Json::arrayValue);
  Pos prev = start;

  for (const auto& p : path)
    {
      Json::Value a (Json::objectValue);
      a["type"] = "move";
      a["dx"] = p.first - prev.first;
      a["dy"] = p.second - prev.second;
      actions.append (a);

      Action sa;
      sa.type = Action::Type::Move;
      sa.dx = p.first - prev.first;
      sa.dy = p.second - prev.second;
      simGame.ProcessAction (sa);

      if (simGame.IsGameOver ())
        return actions;  /* Died — return what we have.  */

      /* If we didn't actually move (attacked a monster), retry.  */
      while (simGame.GetPlayerX () != p.first
             || simGame.GetPlayerY () != p.second)
        {
          /* Wait for monster to die from repeated attacks,
             or just try moving again.  */
          Json::Value retry (Json::objectValue);
          retry["type"] = "move";
          retry["dx"] = p.first - simGame.GetPlayerX ();
          retry["dy"] = p.second - simGame.GetPlayerY ();
          actions.append (retry);

          Action retryA;
          retryA.type = Action::Type::Move;
          retryA.dx = p.first - simGame.GetPlayerX ();
          retryA.dy = p.second - simGame.GetPlayerY ();
          simGame.ProcessAction (retryA);

          if (simGame.IsGameOver ())
            return actions;

          /* Safety: avoid infinite loops.  */
          if (actions.size () > 500)
            return actions;
        }

      prev = {simGame.GetPlayerX (), simGame.GetPlayerY ()};
    }

  /* Enter the gate.  */
  Json::Value gateAction (Json::objectValue);
  gateAction["type"] = "gate";
  actions.append (gateAction);

  return actions;
}

/**
 * Replay actions and return the expected results for the xc move.
 */
Json::Value
GetReplayResults (const std::string& seed, int depth,
                   const PlayerStats& stats, int hp, int maxHp,
                   const Json::Value& actions)
{
  std::vector<Action> replayActions;
  for (const auto& aj : actions)
    {
      Action a;
      const std::string type = aj["type"].asString ();
      if (type == "move")
        {
          a.type = Action::Type::Move;
          a.dx = aj["dx"].asInt ();
          a.dy = aj["dy"].asInt ();
        }
      else if (type == "gate")
        a.type = Action::Type::EnterGate;
      else if (type == "wait")
        a.type = Action::Type::Wait;
      else if (type == "pickup")
        a.type = Action::Type::Pickup;
      else
        continue;
      replayActions.push_back (a);
    }

  DungeonGame::PotionList potions = {{"health_potion", 3}};
  auto game = DungeonGame::Replay (seed, depth, stats, hp, maxHp,
                                    potions, replayActions);

  Json::Value results (Json::objectValue);
  results["survived"] = game.HasSurvived ();
  results["xp"] = static_cast<Json::Int64> (game.GetTotalXp ());
  results["gold"] = static_cast<Json::Int64> (game.GetTotalGold ());
  results["kills"] = static_cast<Json::Int64> (game.GetTotalKills ());
  results["hp_remaining"] = static_cast<Json::Int64> (game.GetPlayerHp ());
  if (game.HasSurvived () && !game.GetExitGate ().empty ())
    results["exit_gate"] = game.GetExitGate ();

  return results;
}


class GateTraversalTests : public DBTest
{

protected:

  int64_t nextSegmentId = 1;
  int64_t nextVisitId = 1;
  PlayerStats defaultStats;

  GateTraversalTests ()
  {
    /* Stats matching what ComputePlayerStats will read from
       the on-chain player (boosted base + starting equipment).  */
    defaultStats.level = 10;
    defaultStats.strength = 20;
    defaultStats.dexterity = 20;
    defaultStats.constitution = 20;
    defaultStats.intelligence = 10;
    defaultStats.equipAttack = 5;   /* short_sword */
    defaultStats.equipDefense = 2;  /* leather_armor */
  }

  void ProcessMove (const std::string& name, const std::string& moveJson,
                    unsigned height = 100,
                    const std::string& txid = "deadbeef")
  {
    Json::Value obj (Json::objectValue);
    obj["name"] = name;
    obj["txid"] = txid;
    obj["move"] = ParseJson (moveJson);

    Json::Value moves (Json::arrayValue);
    moves.append (obj);

    MoveProcessor proc (GetHandle (), height, nextSegmentId, nextVisitId);
    proc.ProcessAll (moves);
  }

  void RegisterPlayer (const std::string& name, unsigned height = 100)
  {
    ProcessMove (name, R"({"r": {}})", height);
    /* Boost on-chain stats to match defaultStats so replay verification
       uses the same stats as our pathfinding.  */
    Execute (
      "UPDATE `players` SET"
      " `level` = 10, `strength` = 20, `dexterity` = 20,"
      " `constitution` = 20, `hp` = 500, `max_hp` = 500"
      " WHERE `name` = '" + name + "'");
  }

};

TEST_F (GateTraversalTests, ExitThroughWestGateReturnsToOrigin)
{
  RegisterPlayer ("alice");

  /* Discover east from segment 0.  */
  ProcessMove ("alice", R"({"d": {"depth": 1, "dir": "east"}})", 200, "gate_test_1");

  /* Get the segment seed.  */
  const std::string seed = QueryString (
    "SELECT `seed` FROM `segments` WHERE `id` = 1");

  /* Find actions to reach the west gate (which links back to segment 0).  */
  Json::Value actions = FindPathToGate (seed, 1, defaultStats, 500, 500, "west");
  ASSERT_GT (actions.size (), 0u) << "Could not find path to west gate";

  /* Get expected replay results.  */
  Json::Value results = GetReplayResults (seed, 1, defaultStats, 500, 500, actions);
  ASSERT_TRUE (results["survived"].asBool ()) << "Replay should survive via gate";
  ASSERT_EQ (results["exit_gate"].asString (), "west");

  /* Enter channel as discoverer.  */
  ProcessMove ("alice", R"({"ec": {"id": 1}})", 300);
  EXPECT_EQ (QueryInt (
    "SELECT `current_segment` FROM `players` WHERE `name` = 'alice'"), 1);

  /* Exit through west gate with valid action proof.  */
  Json::Value xcMove (Json::objectValue);
  xcMove["id"] = 1;
  xcMove["results"] = results;
  xcMove["actions"] = actions;

  Json::Value mv (Json::objectValue);
  mv["xc"] = xcMove;

  Json::Value obj (Json::objectValue);
  obj["name"] = "alice";
  obj["txid"] = "xc_tx";
  obj["move"] = mv;

  Json::Value moves (Json::arrayValue);
  moves.append (obj);

  MoveProcessor proc (GetHandle (), 400, nextSegmentId, nextVisitId);
  proc.ProcessAll (moves);

  /* Alice should be back at segment 0 (exited west, which links to 0).  */
  EXPECT_EQ (QueryInt (
    "SELECT `current_segment` FROM `players` WHERE `name` = 'alice'"), 0);
  EXPECT_EQ (QueryInt (
    "SELECT `in_channel` FROM `players` WHERE `name` = 'alice'"), 0);

  /* Segment should be confirmed now.  */
  EXPECT_EQ (QueryInt (
    "SELECT `confirmed` FROM `segments` WHERE `id` = 1"), 1);
}

TEST_F (GateTraversalTests, TravelBackAndForth)
{
  RegisterPlayer ("alice");

  /* Discover and confirm segment 1 east from origin.  */
  ProcessMove ("alice", R"({"d": {"depth": 1, "dir": "east"}})", 200, "bidir_test");

  const std::string seed = QueryString (
    "SELECT `seed` FROM `segments` WHERE `id` = 1");

  /* Enter and exit through west gate to confirm.  */
  ProcessMove ("alice", R"({"ec": {"id": 1}})", 300);

  Json::Value actions = FindPathToGate (seed, 1, defaultStats, 500, 500, "west");
  Json::Value results = GetReplayResults (seed, 1, defaultStats, 500, 500, actions);

  {
    Json::Value xcMove (Json::objectValue);
    xcMove["id"] = 1;
    xcMove["results"] = results;
    xcMove["actions"] = actions;

    Json::Value mv (Json::objectValue);
    mv["xc"] = xcMove;

    Json::Value obj (Json::objectValue);
    obj["name"] = "alice";
    obj["txid"] = "xc1";
    obj["move"] = mv;

    Json::Value moves (Json::arrayValue);
    moves.append (obj);
    MoveProcessor p (GetHandle (), 400, nextSegmentId, nextVisitId);
    p.ProcessAll (moves);
  }

  /* Alice at segment 0, segment 1 confirmed.  */
  EXPECT_EQ (QueryInt (
    "SELECT `current_segment` FROM `players` WHERE `name` = 'alice'"), 0);
  EXPECT_EQ (QueryInt (
    "SELECT `confirmed` FROM `segments` WHERE `id` = 1"), 1);

  /* Heal alice (might have taken damage from monsters on the path).  */
  Execute ("UPDATE `players` SET `hp` = 500, `max_hp` = 500 WHERE `name` = 'alice'");

  /* Travel east to segment 1.  */
  ProcessMove ("alice", R"({"t": {"dir": "east"}})", 500, "travel_east");
  EXPECT_EQ (QueryInt (
    "SELECT `current_segment` FROM `players` WHERE `name` = 'alice'"), 1);

  /* Travel west back to segment 0.  */
  /* Segment 0 is the origin — check if the reverse link exists.  */
  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `segment_links`"
    " WHERE `from_segment` = 1 AND `from_direction` = 'west'"
    " AND `to_segment` = 0"), 1);

  ProcessMove ("alice", R"({"t": {"dir": "west"}})", 600, "travel_west");
  EXPECT_EQ (QueryInt (
    "SELECT `current_segment` FROM `players` WHERE `name` = 'alice'"), 0);

  /* Travel east again to verify it's repeatable.  */
  Execute ("UPDATE `players` SET `hp` = 500, `max_hp` = 500 WHERE `name` = 'alice'");
  ProcessMove ("alice", R"({"t": {"dir": "east"}})", 700, "travel_east2");
  EXPECT_EQ (QueryInt (
    "SELECT `current_segment` FROM `players` WHERE `name` = 'alice'"), 1);
}

TEST_F (GateTraversalTests, ThreeSegmentChainTravel)
{
  /* Test that the world graph supports chaining: 0 ↔ 1 ↔ 2.
     Segments confirmed manually to focus on link/travel mechanics.  */
  RegisterPlayer ("alice");

  /* Discover segment 1 east from origin.  */
  ProcessMove ("alice", R"({"d": {"depth": 1, "dir": "east"}})", 200, "chain_s1");
  Execute ("UPDATE `segments` SET `confirmed` = 1 WHERE `id` = 1");

  /* Verify links: 0 east → 1, 1 west → 0.  */
  EXPECT_EQ (QueryInt (
    "SELECT `to_segment` FROM `segment_links`"
    " WHERE `from_segment` = 0 AND `from_direction` = 'east'"), 1);
  EXPECT_EQ (QueryInt (
    "SELECT `to_segment` FROM `segment_links`"
    " WHERE `from_segment` = 1 AND `from_direction` = 'west'"), 0);

  /* Travel east to segment 1.  */
  Execute ("UPDATE `players` SET `hp` = 500, `max_hp` = 500 WHERE `name` = 'alice'");
  ProcessMove ("alice", R"({"t": {"dir": "east"}})", 300, "t1");
  EXPECT_EQ (QueryInt (
    "SELECT `current_segment` FROM `players` WHERE `name` = 'alice'"), 1);

  /* Discover segment 2 east from segment 1 (after cooldown).  */
  ProcessMove ("alice", R"({"d": {"depth": 1, "dir": "east"}})", 351, "chain_s2");
  Execute ("UPDATE `segments` SET `confirmed` = 1 WHERE `id` = 2");
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `segments`"), 2);

  /* Verify links: 1 east → 2, 2 west → 1.  */
  EXPECT_EQ (QueryInt (
    "SELECT `to_segment` FROM `segment_links`"
    " WHERE `from_segment` = 1 AND `from_direction` = 'east'"), 2);
  EXPECT_EQ (QueryInt (
    "SELECT `to_segment` FROM `segment_links`"
    " WHERE `from_segment` = 2 AND `from_direction` = 'west'"), 1);

  /* Travel full chain: 1 → 2 → 1 → 0 → 1 → 2.  */
  Execute ("UPDATE `players` SET `hp` = 500, `max_hp` = 500 WHERE `name` = 'alice'");
  ProcessMove ("alice", R"({"t": {"dir": "east"}})", 400, "t_to_2");
  EXPECT_EQ (QueryInt (
    "SELECT `current_segment` FROM `players` WHERE `name` = 'alice'"), 2);

  Execute ("UPDATE `players` SET `hp` = 500, `max_hp` = 500 WHERE `name` = 'alice'");
  ProcessMove ("alice", R"({"t": {"dir": "west"}})", 500, "t_back_1");
  EXPECT_EQ (QueryInt (
    "SELECT `current_segment` FROM `players` WHERE `name` = 'alice'"), 1);

  Execute ("UPDATE `players` SET `hp` = 500, `max_hp` = 500 WHERE `name` = 'alice'");
  ProcessMove ("alice", R"({"t": {"dir": "west"}})", 600, "t_back_0");
  EXPECT_EQ (QueryInt (
    "SELECT `current_segment` FROM `players` WHERE `name` = 'alice'"), 0);

  Execute ("UPDATE `players` SET `hp` = 500, `max_hp` = 500 WHERE `name` = 'alice'");
  ProcessMove ("alice", R"({"t": {"dir": "east"}})", 700, "t_to_1b");
  EXPECT_EQ (QueryInt (
    "SELECT `current_segment` FROM `players` WHERE `name` = 'alice'"), 1);

  Execute ("UPDATE `players` SET `hp` = 500, `max_hp` = 500 WHERE `name` = 'alice'");
  ProcessMove ("alice", R"({"t": {"dir": "east"}})", 800, "t_to_2b");
  EXPECT_EQ (QueryInt (
    "SELECT `current_segment` FROM `players` WHERE `name` = 'alice'"), 2);

  /* Total links in the graph: 4 (two per connection, two connections).  */
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `segment_links`"), 4);
}

// ============================================================
// 2D world coordinate tests
// ============================================================

TEST_F (GateTraversalTests, SegmentCoordinatesEast)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"d": {"depth": 1, "dir": "east"}})", 200, "coord1");

  /* Segment discovered east from origin (0,0) → should be at (1,0).  */
  EXPECT_EQ (QueryInt ("SELECT `world_x` FROM `segments` WHERE `id` = 1"), 1);
  EXPECT_EQ (QueryInt ("SELECT `world_y` FROM `segments` WHERE `id` = 1"), 0);
}

TEST_F (GateTraversalTests, SegmentCoordinatesNorth)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"d": {"depth": 1, "dir": "north"}})", 200, "coord2");

  /* North from origin (0,0) → should be at (0,1).  */
  EXPECT_EQ (QueryInt ("SELECT `world_x` FROM `segments` WHERE `id` = 1"), 0);
  EXPECT_EQ (QueryInt ("SELECT `world_y` FROM `segments` WHERE `id` = 1"), 1);
}

TEST_F (GateTraversalTests, SegmentCoordinatesChain)
{
  /* Build a chain: origin → east (1,0) → north (1,1).  */
  RegisterPlayer ("alice");

  ProcessMove ("alice", R"({"d": {"depth": 1, "dir": "east"}})", 200, "chain_c1");
  Execute ("UPDATE `segments` SET `confirmed` = 1 WHERE `id` = 1");

  EXPECT_EQ (QueryInt ("SELECT `world_x` FROM `segments` WHERE `id` = 1"), 1);
  EXPECT_EQ (QueryInt ("SELECT `world_y` FROM `segments` WHERE `id` = 1"), 0);

  /* Travel east to (1,0).  */
  Execute ("UPDATE `players` SET `hp` = 500, `max_hp` = 500 WHERE `name` = 'alice'");
  ProcessMove ("alice", R"({"t": {"dir": "east"}})", 300, "t1");
  EXPECT_EQ (QueryInt (
    "SELECT `current_segment` FROM `players` WHERE `name` = 'alice'"), 1);

  /* Discover north from (1,0) → should be (1,1).  */
  ProcessMove ("alice", R"({"d": {"depth": 1, "dir": "north"}})", 351, "chain_c2");

  EXPECT_EQ (QueryInt ("SELECT `world_x` FROM `segments` WHERE `id` = 2"), 1);
  EXPECT_EQ (QueryInt ("SELECT `world_y` FROM `segments` WHERE `id` = 2"), 1);
}

TEST_F (GateTraversalTests, SegmentCoordinatesSouthWest)
{
  /* Discover south and west to get negative coordinates.  */
  RegisterPlayer ("alice");

  ProcessMove ("alice", R"({"d": {"depth": 1, "dir": "south"}})", 200, "sw1");
  EXPECT_EQ (QueryInt ("SELECT `world_x` FROM `segments` WHERE `id` = 1"), 0);
  EXPECT_EQ (QueryInt ("SELECT `world_y` FROM `segments` WHERE `id` = 1"), -1);

  ProcessMove ("alice", R"({"d": {"depth": 1, "dir": "west"}})", 251, "sw2");
  EXPECT_EQ (QueryInt ("SELECT `world_x` FROM `segments` WHERE `id` = 2"), -1);
  EXPECT_EQ (QueryInt ("SELECT `world_y` FROM `segments` WHERE `id` = 2"), 0);
}

TEST_F (GateTraversalTests, CoordinateOccupiedRejected)
{
  /* Two segments can't occupy the same world coordinate.  */
  RegisterPlayer ("alice");
  RegisterPlayer ("bob");

  ProcessMove ("alice", R"({"d": {"depth": 1, "dir": "east"}})", 200, "occ1");
  Execute ("UPDATE `segments` SET `confirmed` = 1 WHERE `id` = 1");

  /* Bob also at origin. Tries to discover east → (1,0) already taken.  */
  ProcessMove ("bob", R"({"d": {"depth": 1, "dir": "east"}})", 260, "occ2");

  /* Should be rejected — only 1 segment exists.  */
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `segments`"), 1);
}

} // anonymous namespace
} // namespace rog
