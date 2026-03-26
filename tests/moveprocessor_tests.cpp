#include "moveprocessor.hpp"
#include "testutils.hpp"

#include <glog/logging.h>
#include <gtest/gtest.h>

namespace rog
{
namespace
{

class MoveProcessorTests : public DBTest
{

protected:

  int64_t nextSegmentId = 1;
  int64_t nextVisitId = 1;

  /**
   * Processes a single move at the given block height.
   */
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

  /**
   * Registers a player at the given height (convenience helper).
   */
  void RegisterPlayer (const std::string& name, unsigned height = 100)
  {
    ProcessMove (name, R"({"r": {}})", height);
  }

};

// ============================================================
// Player registration tests
// ============================================================

TEST_F (MoveProcessorTests, RegisterValid)
{
  RegisterPlayer ("alice");

  EXPECT_EQ (QueryInt (
    "SELECT `level` FROM `players` WHERE `name` = 'alice'"), 1);
  EXPECT_EQ (QueryInt (
    "SELECT `xp` FROM `players` WHERE `name` = 'alice'"), 0);
  EXPECT_EQ (QueryInt (
    "SELECT `gold` FROM `players` WHERE `name` = 'alice'"), 0);
  EXPECT_EQ (QueryInt (
    "SELECT `strength` FROM `players` WHERE `name` = 'alice'"), 10);
  EXPECT_EQ (QueryInt (
    "SELECT `dexterity` FROM `players` WHERE `name` = 'alice'"), 10);
  EXPECT_EQ (QueryInt (
    "SELECT `constitution` FROM `players` WHERE `name` = 'alice'"), 10);
  EXPECT_EQ (QueryInt (
    "SELECT `intelligence` FROM `players` WHERE `name` = 'alice'"), 10);
  EXPECT_EQ (QueryInt (
    "SELECT `registered_height` FROM `players` WHERE `name` = 'alice'"), 100);

  /* HP initialized from constitution: 50 + 10*5 = 100.  */
  EXPECT_EQ (QueryInt (
    "SELECT `hp` FROM `players` WHERE `name` = 'alice'"), 100);
  EXPECT_EQ (QueryInt (
    "SELECT `max_hp` FROM `players` WHERE `name` = 'alice'"), 100);
  EXPECT_EQ (QueryInt (
    "SELECT `current_segment` FROM `players` WHERE `name` = 'alice'"), 0);
  EXPECT_EQ (QueryInt (
    "SELECT `in_channel` FROM `players` WHERE `name` = 'alice'"), 0);
}

TEST_F (MoveProcessorTests, RegisterStartingItems)
{
  RegisterPlayer ("alice");

  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `inventory` WHERE `name` = 'alice'"), 3);

  EXPECT_EQ (QueryString (
    "SELECT `slot` FROM `inventory`"
    " WHERE `name` = 'alice' AND `item_id` = 'short_sword'"), "weapon");

  EXPECT_EQ (QueryString (
    "SELECT `slot` FROM `inventory`"
    " WHERE `name` = 'alice' AND `item_id` = 'leather_armor'"), "body");

  EXPECT_EQ (QueryInt (
    "SELECT `quantity` FROM `inventory`"
    " WHERE `name` = 'alice' AND `item_id` = 'health_potion'"), 3);

  EXPECT_EQ (QueryString (
    "SELECT `slot` FROM `inventory`"
    " WHERE `name` = 'alice' AND `item_id` = 'health_potion'"), "bag");
}

TEST_F (MoveProcessorTests, RegisterDuplicate)
{
  RegisterPlayer ("alice");
  RegisterPlayer ("alice", 200);

  /* Still only one player row.  */
  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `players` WHERE `name` = 'alice'"), 1);

  /* Height is still from the first registration.  */
  EXPECT_EQ (QueryInt (
    "SELECT `registered_height` FROM `players` WHERE `name` = 'alice'"), 100);
}

TEST_F (MoveProcessorTests, RegisterMultiplePlayers)
{
  RegisterPlayer ("alice", 100);
  RegisterPlayer ("bob", 101);
  RegisterPlayer ("charlie", 102);

  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `players`"), 3);

  /* Each player gets their own starting items.  */
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `inventory`"), 9);
}

TEST_F (MoveProcessorTests, RegisterInvalidMoveFormat)
{
  /* Non-empty register object should be rejected.  */
  ProcessMove ("alice", R"({"r": {"extra": 1}})");
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `players`"), 0);

  /* Non-object register value.  */
  ProcessMove ("alice", R"({"r": 42})");
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `players`"), 0);
}

TEST_F (MoveProcessorTests, RegisterIgnoresMultipleActions)
{
  /* Move with multiple action keys should be rejected.  */
  ProcessMove ("alice", R"({"r": {}, "d": {"depth": 1}})");
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `players`"), 0);
}

// ============================================================
// Segment discovery tests
// ============================================================

TEST_F (MoveProcessorTests, DiscoverValid)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"d": {"depth": 3}})", 200, "abc123");

  /* Permanent segment created.  */
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `segments`"), 1);
  EXPECT_EQ (QueryString (
    "SELECT `discoverer` FROM `segments` WHERE `id` = 1"), "alice");
  EXPECT_EQ (QueryInt (
    "SELECT `depth` FROM `segments` WHERE `id` = 1"), 3);
  EXPECT_EQ (QueryString (
    "SELECT `seed` FROM `segments` WHERE `id` = 1"), "abc123");
  EXPECT_EQ (QueryInt (
    "SELECT `created_height` FROM `segments` WHERE `id` = 1"), 200);

  /* First visit created.  */
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `visits`"), 1);
  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `visits` WHERE `id` = 1"), "open");
  EXPECT_EQ (QueryString (
    "SELECT `initiator` FROM `visits` WHERE `id` = 1"), "alice");
  EXPECT_EQ (QueryInt (
    "SELECT `segment_id` FROM `visits` WHERE `id` = 1"), 1);

  /* Discoverer is first participant of the visit.  */
  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `visit_participants` WHERE `visit_id` = 1"), 1);
  EXPECT_EQ (QueryString (
    "SELECT `name` FROM `visit_participants` WHERE `visit_id` = 1"),
    "alice");
}

TEST_F (MoveProcessorTests, DiscoverUnregistered)
{
  ProcessMove ("nobody", R"({"d": {"depth": 2}})");
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `segments`"), 0);
}

TEST_F (MoveProcessorTests, DiscoverDepthOutOfRange)
{
  RegisterPlayer ("alice");

  ProcessMove ("alice", R"({"d": {"depth": 0}})");
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `segments`"), 0);

  ProcessMove ("alice", R"({"d": {"depth": 21}})");
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `segments`"), 0);
}

TEST_F (MoveProcessorTests, DiscoverWhileInVisit)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 200);

  /* Second discover should fail — alice is already in a visit.  */
  ProcessMove ("alice", R"({"d": {"depth": 2}})", 201);
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `segments`"), 1);
}

// ============================================================
// Revisit tests (new "v" move)
// ============================================================

TEST_F (MoveProcessorTests, VisitExistingSegment)
{
  RegisterPlayer ("alice");
  RegisterPlayer ("bob");
  RegisterPlayer ("charlie");
  RegisterPlayer ("dave");

  /* Discover segment 1, creating visit 1.  */
  ProcessMove ("alice", R"({"d": {"depth": 2}})", 200, "seed1");
  ProcessMove ("bob", R"({"j": {"id": 1}})", 201);
  ProcessMove ("charlie", R"({"j": {"id": 1}})", 202);
  ProcessMove ("dave", R"({"j": {"id": 1}})", 203);

  /* Settle visit 1.  */
  ProcessMove ("alice", R"({"s": {"id": 1, "results": [
    {"p": "alice", "survived": true, "xp": 0, "gold": 0, "kills": 0},
    {"p": "bob", "survived": true, "xp": 0, "gold": 0, "kills": 0},
    {"p": "charlie", "survived": true, "xp": 0, "gold": 0, "kills": 0},
    {"p": "dave", "survived": true, "xp": 0, "gold": 0, "kills": 0}
  ]}})", 300);

  /* Now alice revisits segment 1, creating visit 2.  */
  ProcessMove ("alice", R"({"v": {"id": 1}})", 400);

  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `visits`"), 2);
  EXPECT_EQ (QueryInt (
    "SELECT `segment_id` FROM `visits` WHERE `id` = 2"), 1);
  EXPECT_EQ (QueryString (
    "SELECT `initiator` FROM `visits` WHERE `id` = 2"), "alice");
  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `visits` WHERE `id` = 2"), "open");

  /* Alice is the first participant of visit 2.  */
  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `visit_participants` WHERE `visit_id` = 2"), 1);
}

TEST_F (MoveProcessorTests, CannotVisitNonexistentSegment)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"v": {"id": 999}})", 200);

  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `visits`"), 0);
}

TEST_F (MoveProcessorTests, CannotVisitWithActiveVisit)
{
  RegisterPlayer ("alice");

  /* Discover segment 1 (creates open visit 1 with alice in it).  */
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 200);

  /* Try to visit segment 1 again — alice is already in visit 1.  */
  ProcessMove ("alice", R"({"v": {"id": 1}})", 201);

  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `visits`"), 1);
}

TEST_F (MoveProcessorTests, CannotVisitSegmentWithOpenVisit)
{
  RegisterPlayer ("alice");
  RegisterPlayer ("bob");

  /* Alice discovers segment 1, creating open visit 1.  */
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 200);

  /* Bob tries to start a new visit to segment 1 — but visit 1 is still open.  */
  ProcessMove ("bob", R"({"v": {"id": 1}})", 201);

  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `visits`"), 1);
}

// ============================================================
// Visit join tests
// ============================================================

TEST_F (MoveProcessorTests, JoinValid)
{
  RegisterPlayer ("alice");
  RegisterPlayer ("bob");
  ProcessMove ("alice", R"({"d": {"depth": 2}})", 200);

  ProcessMove ("bob", R"({"j": {"id": 1}})", 201);

  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `visit_participants` WHERE `visit_id` = 1"), 2);

  /* Visit should still be open (2/4 players).  */
  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `visits` WHERE `id` = 1"), "open");
}

TEST_F (MoveProcessorTests, JoinFillsVisit)
{
  RegisterPlayer ("alice");
  RegisterPlayer ("bob");
  RegisterPlayer ("charlie");
  RegisterPlayer ("dave");

  ProcessMove ("alice", R"({"d": {"depth": 1}})", 200);
  ProcessMove ("bob", R"({"j": {"id": 1}})", 201);
  ProcessMove ("charlie", R"({"j": {"id": 1}})", 202);
  ProcessMove ("dave", R"({"j": {"id": 1}})", 203);

  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `visit_participants` WHERE `visit_id` = 1"), 4);

  /* Visit should now be active.  */
  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `visits` WHERE `id` = 1"), "active");
  EXPECT_EQ (QueryInt (
    "SELECT `started_height` FROM `visits` WHERE `id` = 1"), 203);
}

TEST_F (MoveProcessorTests, JoinNonexistentVisit)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"j": {"id": 999}})");

  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `visit_participants`"), 0);
}

TEST_F (MoveProcessorTests, JoinAlreadyInVisit)
{
  RegisterPlayer ("alice");
  RegisterPlayer ("bob");
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 200);

  /* Bob joins, then tries to join a second visit.  */
  ProcessMove ("bob", R"({"j": {"id": 1}})", 201);

  RegisterPlayer ("charlie");
  ProcessMove ("charlie", R"({"d": {"depth": 2}})", 202);

  ProcessMove ("bob", R"({"j": {"id": 2}})", 203);
  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `visit_participants` WHERE `visit_id` = 2"), 1);
}

// ============================================================
// Visit leave tests
// ============================================================

TEST_F (MoveProcessorTests, LeaveValid)
{
  RegisterPlayer ("alice");
  RegisterPlayer ("bob");
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 200);
  ProcessMove ("bob", R"({"j": {"id": 1}})", 201);

  ProcessMove ("bob", R"({"lv": {"id": 1}})", 202);

  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `visit_participants` WHERE `visit_id` = 1"), 1);
}

TEST_F (MoveProcessorTests, LeaveInitiatorBlocked)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 200);

  /* Initiator cannot leave.  */
  ProcessMove ("alice", R"({"lv": {"id": 1}})", 201);

  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `visit_participants` WHERE `visit_id` = 1"), 1);
}

TEST_F (MoveProcessorTests, LeaveNotInVisit)
{
  RegisterPlayer ("alice");
  RegisterPlayer ("bob");
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 200);

  /* Bob never joined.  */
  ProcessMove ("bob", R"({"lv": {"id": 1}})", 201);

  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `visit_participants` WHERE `visit_id` = 1"), 1);
}

TEST_F (MoveProcessorTests, JoinAfterLeave)
{
  RegisterPlayer ("alice");
  RegisterPlayer ("bob");
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 200);
  ProcessMove ("bob", R"({"j": {"id": 1}})", 201);
  ProcessMove ("bob", R"({"lv": {"id": 1}})", 202);

  /* Bob can rejoin after leaving.  */
  ProcessMove ("bob", R"({"j": {"id": 1}})", 203);

  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `visit_participants` WHERE `visit_id` = 1"), 2);
}

// ============================================================
// Helper to set up a full active visit with 4 players
// ============================================================

class SettleTests : public MoveProcessorTests
{

protected:

  void SetUp () override
  {
    RegisterPlayer ("alice");
    RegisterPlayer ("bob");
    RegisterPlayer ("charlie");
    RegisterPlayer ("dave");
    ProcessMove ("alice", R"({"d": {"depth": 3}})", 200, "seed123");
    ProcessMove ("bob", R"({"j": {"id": 1}})", 201);
    ProcessMove ("charlie", R"({"j": {"id": 1}})", 202);
    ProcessMove ("dave", R"({"j": {"id": 1}})", 203);
  }

};

TEST_F (SettleTests, BasicSettle)
{
  ProcessMove ("alice", R"({"s": {"id": 1, "results": [
    {"p": "alice", "survived": true, "xp": 50, "gold": 100, "kills": 3},
    {"p": "bob", "survived": true, "xp": 30, "gold": 60, "kills": 2},
    {"p": "charlie", "survived": false, "xp": 10, "gold": 0, "kills": 1},
    {"p": "dave", "survived": true, "xp": 40, "gold": 80, "kills": 4}
  ]}})", 300);

  /* Visit should be completed.  */
  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `visits` WHERE `id` = 1"), "completed");
  EXPECT_EQ (QueryInt (
    "SELECT `settled_height` FROM `visits` WHERE `id` = 1"), 300);

  /* Segment is still there (permanent).  */
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `segments`"), 1);

  /* Check visit results recorded.  */
  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `visit_results` WHERE `visit_id` = 1"), 4);

  /* Check player stats updated.  */
  EXPECT_EQ (QueryInt (
    "SELECT `gold` FROM `players` WHERE `name` = 'alice'"), 100);
  EXPECT_EQ (QueryInt (
    "SELECT `kills` FROM `players` WHERE `name` = 'alice'"), 3);
  EXPECT_EQ (QueryInt (
    "SELECT `visits_completed` FROM `players` WHERE `name` = 'alice'"), 1);
  EXPECT_EQ (QueryInt (
    "SELECT `deaths` FROM `players` WHERE `name` = 'alice'"), 0);

  /* Charlie died.  */
  EXPECT_EQ (QueryInt (
    "SELECT `deaths` FROM `players` WHERE `name` = 'charlie'"), 1);
  EXPECT_EQ (QueryInt (
    "SELECT `gold` FROM `players` WHERE `name` = 'charlie'"), 0);
}

TEST_F (SettleTests, XpAndLevelUp)
{
  /* Level 2 requires floor(100 * pow(2, 1.5)) = 282 XP.
     Give alice 300 XP — should level up to 2 with 18 XP remaining.  */
  ProcessMove ("alice", R"({"s": {"id": 1, "results": [
    {"p": "alice", "survived": true, "xp": 300, "gold": 0, "kills": 0},
    {"p": "bob", "survived": true, "xp": 0, "gold": 0, "kills": 0},
    {"p": "charlie", "survived": true, "xp": 0, "gold": 0, "kills": 0},
    {"p": "dave", "survived": true, "xp": 0, "gold": 0, "kills": 0}
  ]}})", 300);

  EXPECT_EQ (QueryInt (
    "SELECT `level` FROM `players` WHERE `name` = 'alice'"), 2);
  EXPECT_EQ (QueryInt (
    "SELECT `xp` FROM `players` WHERE `name` = 'alice'"), 300 - 282);
  EXPECT_EQ (QueryInt (
    "SELECT `skill_points` FROM `players` WHERE `name` = 'alice'"), 1);
  EXPECT_EQ (QueryInt (
    "SELECT `stat_points` FROM `players` WHERE `name` = 'alice'"), 1);
}

TEST_F (SettleTests, MultipleLevelUps)
{
  /* Level 2 = 282 XP, level 3 = floor(100*pow(3,1.5)) = 519 XP.
     Total to reach level 3 = 282 + 519 = 801.
     Give alice 1000 XP — should be level 3 with 1000-801 = 199 remaining.  */
  ProcessMove ("alice", R"({"s": {"id": 1, "results": [
    {"p": "alice", "survived": true, "xp": 1000, "gold": 0, "kills": 0},
    {"p": "bob", "survived": true, "xp": 0, "gold": 0, "kills": 0},
    {"p": "charlie", "survived": true, "xp": 0, "gold": 0, "kills": 0},
    {"p": "dave", "survived": true, "xp": 0, "gold": 0, "kills": 0}
  ]}})", 300);

  EXPECT_EQ (QueryInt (
    "SELECT `level` FROM `players` WHERE `name` = 'alice'"), 3);
  EXPECT_EQ (QueryInt (
    "SELECT `xp` FROM `players` WHERE `name` = 'alice'"), 199);
  /* 2 level-ups = 2 skill points and 2 stat points.  */
  EXPECT_EQ (QueryInt (
    "SELECT `skill_points` FROM `players` WHERE `name` = 'alice'"), 2);
  EXPECT_EQ (QueryInt (
    "SELECT `stat_points` FROM `players` WHERE `name` = 'alice'"), 2);
}

TEST_F (SettleTests, LootDistribution)
{
  ProcessMove ("alice", R"({"s": {"id": 1, "results": [
    {"p": "alice", "survived": true, "xp": 0, "gold": 0, "kills": 0,
     "loot": [{"item": "iron_helmet", "n": 1}, {"item": "mana_potion", "n": 2}]},
    {"p": "bob", "survived": true, "xp": 0, "gold": 0, "kills": 0,
     "loot": [{"item": "battle_axe", "n": 1}]},
    {"p": "charlie", "survived": true, "xp": 0, "gold": 0, "kills": 0},
    {"p": "dave", "survived": true, "xp": 0, "gold": 0, "kills": 0}
  ]}})", 300);

  /* Loot claims recorded.  */
  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `loot_claims` WHERE `visit_id` = 1"), 3);

  /* Items added to inventory in bag slot.  */
  EXPECT_EQ (QueryInt (
    "SELECT `quantity` FROM `inventory`"
    " WHERE `name` = 'alice' AND `item_id` = 'iron_helmet'"), 1);
  EXPECT_EQ (QueryInt (
    "SELECT `quantity` FROM `inventory`"
    " WHERE `name` = 'alice' AND `item_id` = 'mana_potion'"), 2);
  EXPECT_EQ (QueryInt (
    "SELECT `quantity` FROM `inventory`"
    " WHERE `name` = 'bob' AND `item_id` = 'battle_axe'"), 1);

  /* Alice had 3 starting items + 2 loot items = 5 total rows.  */
  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `inventory` WHERE `name` = 'alice'"), 5);
}

TEST_F (SettleTests, OnlyInitiatorCanSettle)
{
  /* Bob is not the initiator.  */
  ProcessMove ("bob", R"({"s": {"id": 1, "results": [
    {"p": "alice", "survived": true, "xp": 0, "gold": 0, "kills": 0},
    {"p": "bob", "survived": true, "xp": 0, "gold": 0, "kills": 0},
    {"p": "charlie", "survived": true, "xp": 0, "gold": 0, "kills": 0},
    {"p": "dave", "survived": true, "xp": 0, "gold": 0, "kills": 0}
  ]}})", 300);

  /* Should still be active — settle was rejected.  */
  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `visits` WHERE `id` = 1"), "active");
}

TEST_F (SettleTests, CannotSettleOpenVisit)
{
  /* Create a new visit that stays open (only eve in it).  */
  RegisterPlayer ("eve");
  ProcessMove ("eve", R"({"d": {"depth": 1}})", 300, "seed456");

  ProcessMove ("eve", R"({"s": {"id": 2, "results": [
    {"p": "eve", "survived": true, "xp": 0, "gold": 0, "kills": 0}
  ]}})", 301);

  /* Should still be open — can't settle an open visit.  */
  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `visits` WHERE `id` = 2"), "open");
}

TEST_F (SettleTests, NonParticipantInResults)
{
  /* Eve is not in visit 1.  */
  RegisterPlayer ("eve");

  ProcessMove ("alice", R"({"s": {"id": 1, "results": [
    {"p": "eve", "survived": true, "xp": 50, "gold": 0, "kills": 0}
  ]}})", 300);

  /* Should still be active — settle was rejected.  */
  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `visits` WHERE `id` = 1"), "active");
}

// ============================================================
// Stat allocation tests
// ============================================================

class StatAllocTests : public MoveProcessorTests
{

protected:

  void SetUp () override
  {
    /* Register alice and give her stat points via a settled visit.  */
    RegisterPlayer ("alice");
    RegisterPlayer ("bob");
    RegisterPlayer ("charlie");
    RegisterPlayer ("dave");
    ProcessMove ("alice", R"({"d": {"depth": 1}})", 100, "s1");
    ProcessMove ("bob", R"({"j": {"id": 1}})", 101);
    ProcessMove ("charlie", R"({"j": {"id": 1}})", 102);
    ProcessMove ("dave", R"({"j": {"id": 1}})", 103);

    /* Settle with enough XP for 2 level-ups (1000 XP) = 2 stat points.  */
    ProcessMove ("alice", R"({"s": {"id": 1, "results": [
      {"p": "alice", "survived": true, "xp": 1000, "gold": 0, "kills": 0},
      {"p": "bob", "survived": true, "xp": 0, "gold": 0, "kills": 0},
      {"p": "charlie", "survived": true, "xp": 0, "gold": 0, "kills": 0},
      {"p": "dave", "survived": true, "xp": 0, "gold": 0, "kills": 0}
    ]}})", 200);
  }

};

TEST_F (StatAllocTests, AllocateStrength)
{
  ProcessMove ("alice", R"({"as": {"stat": "strength"}})", 300);

  EXPECT_EQ (QueryInt (
    "SELECT `strength` FROM `players` WHERE `name` = 'alice'"), 11);
  EXPECT_EQ (QueryInt (
    "SELECT `stat_points` FROM `players` WHERE `name` = 'alice'"), 1);
}

TEST_F (StatAllocTests, AllocateAllStats)
{
  ProcessMove ("alice", R"({"as": {"stat": "dexterity"}})", 300);
  ProcessMove ("alice", R"({"as": {"stat": "constitution"}})", 301);

  EXPECT_EQ (QueryInt (
    "SELECT `dexterity` FROM `players` WHERE `name` = 'alice'"), 11);
  EXPECT_EQ (QueryInt (
    "SELECT `constitution` FROM `players` WHERE `name` = 'alice'"), 11);
  EXPECT_EQ (QueryInt (
    "SELECT `stat_points` FROM `players` WHERE `name` = 'alice'"), 0);
}

TEST_F (StatAllocTests, NoPointsLeft)
{
  ProcessMove ("alice", R"({"as": {"stat": "strength"}})", 300);
  ProcessMove ("alice", R"({"as": {"stat": "strength"}})", 301);

  /* Third attempt should fail — no points left.  */
  ProcessMove ("alice", R"({"as": {"stat": "strength"}})", 302);

  EXPECT_EQ (QueryInt (
    "SELECT `strength` FROM `players` WHERE `name` = 'alice'"), 12);
  EXPECT_EQ (QueryInt (
    "SELECT `stat_points` FROM `players` WHERE `name` = 'alice'"), 0);
}

TEST_F (StatAllocTests, InvalidStatName)
{
  ProcessMove ("alice", R"({"as": {"stat": "charisma"}})", 300);

  /* All stats unchanged.  */
  EXPECT_EQ (QueryInt (
    "SELECT `stat_points` FROM `players` WHERE `name` = 'alice'"), 2);
}

TEST_F (StatAllocTests, UnregisteredPlayer)
{
  ProcessMove ("nobody", R"({"as": {"stat": "strength"}})", 300);

  /* No crash, just ignored.  */
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `players`"), 4);
}

TEST_F (StatAllocTests, AllocateIntelligence)
{
  ProcessMove ("alice", R"({"as": {"stat": "intelligence"}})", 300);

  EXPECT_EQ (QueryInt (
    "SELECT `intelligence` FROM `players` WHERE `name` = 'alice'"), 11);
}

TEST_F (StatAllocTests, ConstitutionUpdatesMaxHp)
{
  /* Before: con=10, max_hp=100, hp=100 (at max).  */
  EXPECT_EQ (QueryInt (
    "SELECT `max_hp` FROM `players` WHERE `name` = 'alice'"), 100);

  ProcessMove ("alice", R"({"as": {"stat": "constitution"}})", 300);

  /* After: con=11, max_hp=50+11*5=105, hp should also be 105 (was at max).  */
  EXPECT_EQ (QueryInt (
    "SELECT `constitution` FROM `players` WHERE `name` = 'alice'"), 11);
  EXPECT_EQ (QueryInt (
    "SELECT `max_hp` FROM `players` WHERE `name` = 'alice'"), 105);
  EXPECT_EQ (QueryInt (
    "SELECT `hp` FROM `players` WHERE `name` = 'alice'"), 105);
}

TEST_F (StatAllocTests, ConstitutionDoesNotOverhealDamagedPlayer)
{
  /* Simulate damage: set hp to 50 (max is 100).  */
  Execute ("UPDATE `players` SET `hp` = 50 WHERE `name` = 'alice'");

  ProcessMove ("alice", R"({"as": {"stat": "constitution"}})", 300);

  /* max_hp increases to 105, but hp stays at 50 (was not at max).  */
  EXPECT_EQ (QueryInt (
    "SELECT `max_hp` FROM `players` WHERE `name` = 'alice'"), 105);
  EXPECT_EQ (QueryInt (
    "SELECT `hp` FROM `players` WHERE `name` = 'alice'"), 50);
}

// ============================================================
// Timeout tests
// ============================================================

TEST_F (MoveProcessorTests, OpenVisitExpires)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 100);

  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `visits` WHERE `id` = 1"), "open");

  /* Process an empty block at height 100 + 100 = 200 (timeout boundary).  */
  Json::Value empty (Json::arrayValue);
  MoveProcessor proc (GetHandle (), 200, nextSegmentId, nextVisitId);
  proc.ProcessAll (empty);

  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `visits` WHERE `id` = 1"), "expired");

  /* Segment is still there (permanent).  */
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `segments`"), 1);
}

TEST_F (MoveProcessorTests, OpenVisitNotExpiredYet)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 100);

  /* One block before timeout — should still be open.  */
  Json::Value empty (Json::arrayValue);
  MoveProcessor proc (GetHandle (), 199, nextSegmentId, nextVisitId);
  proc.ProcessAll (empty);

  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `visits` WHERE `id` = 1"), "open");
}

TEST_F (MoveProcessorTests, ActiveVisitForceSettles)
{
  RegisterPlayer ("alice");
  RegisterPlayer ("bob");
  RegisterPlayer ("charlie");
  RegisterPlayer ("dave");
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 100);
  ProcessMove ("bob", R"({"j": {"id": 1}})", 101);
  ProcessMove ("charlie", R"({"j": {"id": 1}})", 102);
  ProcessMove ("dave", R"({"j": {"id": 1}})", 103);

  /* Visit became active at height 103. Timeout at 103 + 1000 = 1103.  */
  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `visits` WHERE `id` = 1"), "active");

  Json::Value empty (Json::arrayValue);
  MoveProcessor proc (GetHandle (), 1103, nextSegmentId, nextVisitId);
  proc.ProcessAll (empty);

  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `visits` WHERE `id` = 1"), "completed");

  /* Segment is still there (permanent).  */
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `segments`"), 1);

  /* All players get a death.  */
  EXPECT_EQ (QueryInt (
    "SELECT `deaths` FROM `players` WHERE `name` = 'alice'"), 1);
  EXPECT_EQ (QueryInt (
    "SELECT `deaths` FROM `players` WHERE `name` = 'bob'"), 1);

  /* Results recorded with survived=0 and no rewards.  */
  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `visit_results` WHERE `visit_id` = 1"), 4);
  EXPECT_EQ (QueryInt (
    "SELECT `survived` FROM `visit_results`"
    " WHERE `visit_id` = 1 AND `name` = 'alice'"), 0);
  EXPECT_EQ (QueryInt (
    "SELECT `xp_gained` FROM `visit_results`"
    " WHERE `visit_id` = 1 AND `name` = 'alice'"), 0);
}

TEST_F (MoveProcessorTests, ActiveVisitNotTimedOutYet)
{
  RegisterPlayer ("alice");
  RegisterPlayer ("bob");
  RegisterPlayer ("charlie");
  RegisterPlayer ("dave");
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 100);
  ProcessMove ("bob", R"({"j": {"id": 1}})", 101);
  ProcessMove ("charlie", R"({"j": {"id": 1}})", 102);
  ProcessMove ("dave", R"({"j": {"id": 1}})", 103);

  /* One block before timeout.  */
  Json::Value empty (Json::arrayValue);
  MoveProcessor proc (GetHandle (), 1102, nextSegmentId, nextVisitId);
  proc.ProcessAll (empty);

  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `visits` WHERE `id` = 1"), "active");
}

// ============================================================
// Item usage tests
// ============================================================

TEST_F (MoveProcessorTests, UseHealthPotion)
{
  RegisterPlayer ("alice");

  /* Damage alice.  */
  Execute ("UPDATE `players` SET `hp` = 50 WHERE `name` = 'alice'");

  /* Alice starts with 3 health potions.  */
  ProcessMove ("alice", R"({"ui": {"item": "health_potion"}})", 200);

  EXPECT_EQ (QueryInt (
    "SELECT `hp` FROM `players` WHERE `name` = 'alice'"), 75);
  EXPECT_EQ (QueryInt (
    "SELECT `quantity` FROM `inventory`"
    " WHERE `name` = 'alice' AND `item_id` = 'health_potion'"), 2);
}

TEST_F (MoveProcessorTests, UseHealthPotionCapsAtMax)
{
  RegisterPlayer ("alice");

  /* Alice is at 90/100 HP.  Potion heals 25 but should cap at 100.  */
  Execute ("UPDATE `players` SET `hp` = 90 WHERE `name` = 'alice'");
  ProcessMove ("alice", R"({"ui": {"item": "health_potion"}})", 200);

  EXPECT_EQ (QueryInt (
    "SELECT `hp` FROM `players` WHERE `name` = 'alice'"), 100);
}

TEST_F (MoveProcessorTests, UseHealthPotionNoneLeft)
{
  RegisterPlayer ("alice");
  /* Remove all potions.  */
  Execute ("DELETE FROM `inventory`"
           " WHERE `name` = 'alice' AND `item_id` = 'health_potion'");

  Execute ("UPDATE `players` SET `hp` = 50 WHERE `name` = 'alice'");
  ProcessMove ("alice", R"({"ui": {"item": "health_potion"}})", 200);

  /* HP unchanged — no potion available.  */
  EXPECT_EQ (QueryInt (
    "SELECT `hp` FROM `players` WHERE `name` = 'alice'"), 50);
}

// ============================================================
// Equip / Unequip tests
// ============================================================

TEST_F (MoveProcessorTests, EquipItem)
{
  RegisterPlayer ("alice");

  /* Unequip the short_sword first (it starts in weapon slot).  */
  const int64_t swordRowid = QueryInt (
    "SELECT `rowid` FROM `inventory`"
    " WHERE `name` = 'alice' AND `item_id` = 'short_sword'");

  ProcessMove ("alice",
    R"({"uq": {"rowid": )" + std::to_string (swordRowid) + R"(}})", 200);

  EXPECT_EQ (QueryString (
    "SELECT `slot` FROM `inventory` WHERE `rowid` = "
    + std::to_string (swordRowid)), "bag");

  /* Re-equip it.  */
  ProcessMove ("alice",
    R"({"eq": {"rowid": )" + std::to_string (swordRowid)
    + R"(, "slot": "weapon"}})", 201);

  EXPECT_EQ (QueryString (
    "SELECT `slot` FROM `inventory` WHERE `rowid` = "
    + std::to_string (swordRowid)), "weapon");
}

// ============================================================
// Directed discover + segment links tests
// ============================================================

TEST_F (MoveProcessorTests, DiscoverWithDirection)
{
  RegisterPlayer ("alice");

  /* Discover from origin (segment 0) going east.  */
  ProcessMove ("alice", R"({"d": {"depth": 1, "dir": "east"}})", 200, "seed1");

  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `segments`"), 1);

  /* Gate positions should be stored.  */
  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `segment_gates` WHERE `segment_id` = 1"), 4);

  /* Bidirectional link should exist.  */
  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `segment_links`"
    " WHERE `from_segment` = 0 AND `from_direction` = 'east'"), 1);
  EXPECT_EQ (QueryInt (
    "SELECT `to_segment` FROM `segment_links`"
    " WHERE `from_segment` = 0 AND `from_direction` = 'east'"), 1);
  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `segment_links`"
    " WHERE `from_segment` = 1 AND `from_direction` = 'west'"), 1);
  EXPECT_EQ (QueryInt (
    "SELECT `to_segment` FROM `segment_links`"
    " WHERE `from_segment` = 1 AND `from_direction` = 'west'"), 0);
}

TEST_F (MoveProcessorTests, DiscoverWithoutDirection)
{
  RegisterPlayer ("alice");

  /* Legacy discover without direction — no links created.  */
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 200, "seed1");

  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `segments`"), 1);
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `segment_gates`"), 4);
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `segment_links`"), 0);
}

// ============================================================
// Travel tests
// ============================================================

TEST_F (MoveProcessorTests, TravelValid)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"d": {"depth": 1, "dir": "east"}})", 200, "s1");

  /* Settle the visit so alice is free.  */
  /* Visit was created as open with alice as participant.  Since it needs
     4 players to activate, we expire it instead.  */
  Json::Value empty (Json::arrayValue);
  MoveProcessor proc (GetHandle (), 301, nextSegmentId, nextVisitId);
  proc.ProcessAll (empty);

  /* Now travel east.  */
  ProcessMove ("alice", R"({"t": {"dir": "east"}})", 400, "txtravel");

  EXPECT_EQ (QueryInt (
    "SELECT `current_segment` FROM `players` WHERE `name` = 'alice'"), 1);
}

TEST_F (MoveProcessorTests, TravelNoLink)
{
  RegisterPlayer ("alice");

  /* No segments discovered — no links from segment 0.  */
  ProcessMove ("alice", R"({"t": {"dir": "north"}})", 200, "tx1");

  /* Should still be at segment 0.  */
  EXPECT_EQ (QueryInt (
    "SELECT `current_segment` FROM `players` WHERE `name` = 'alice'"), 0);
}

TEST_F (MoveProcessorTests, TravelBlockedByZeroHp)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"d": {"depth": 1, "dir": "east"}})", 200, "s1");

  /* Expire the visit.  */
  Json::Value empty (Json::arrayValue);
  MoveProcessor proc (GetHandle (), 301, nextSegmentId, nextVisitId);
  proc.ProcessAll (empty);

  /* Set HP to 0.  */
  Execute ("UPDATE `players` SET `hp` = 0 WHERE `name` = 'alice'");

  ProcessMove ("alice", R"({"t": {"dir": "east"}})", 400, "tx1");

  /* Should still be at segment 0.  */
  EXPECT_EQ (QueryInt (
    "SELECT `current_segment` FROM `players` WHERE `name` = 'alice'"), 0);
}

// ============================================================
// Enter / Exit channel tests
// ============================================================

TEST_F (MoveProcessorTests, EnterAndExitChannel)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"d": {"depth": 1, "dir": "east"}})", 200, "s1");

  /* Expire the discover visit.  */
  Json::Value empty (Json::arrayValue);
  MoveProcessor expProc (GetHandle (), 301, nextSegmentId, nextVisitId);
  expProc.ProcessAll (empty);

  /* Travel to segment 1.  */
  ProcessMove ("alice", R"({"t": {"dir": "east"}})", 400, "tx1");
  EXPECT_EQ (QueryInt (
    "SELECT `current_segment` FROM `players` WHERE `name` = 'alice'"), 1);

  /* Enter channel.  */
  ProcessMove ("alice", R"({"ec": {"id": 1}})", 500);
  EXPECT_EQ (QueryInt (
    "SELECT `in_channel` FROM `players` WHERE `name` = 'alice'"), 1);

  /* A solo active visit should have been created.  */
  /* The discover visit was visit 1 (expired). Channel creates visit 2.  */
  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `visits` WHERE `id` = 2"), "active");

  /* Exit channel with results.  */
  ProcessMove ("alice", R"({"xc": {"id": 2, "results": {
    "survived": true, "xp": 100, "gold": 50, "kills": 3,
    "hp_remaining": 75, "exit_gate": "east"
  }}})", 600);

  EXPECT_EQ (QueryInt (
    "SELECT `in_channel` FROM `players` WHERE `name` = 'alice'"), 0);
  EXPECT_EQ (QueryInt (
    "SELECT `hp` FROM `players` WHERE `name` = 'alice'"), 75);
  EXPECT_EQ (QueryInt (
    "SELECT `gold` FROM `players` WHERE `name` = 'alice'"), 50);
  EXPECT_EQ (QueryInt (
    "SELECT `kills` FROM `players` WHERE `name` = 'alice'"), 3);
  EXPECT_EQ (QueryInt (
    "SELECT `visits_completed` FROM `players` WHERE `name` = 'alice'"), 1);

  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `visits` WHERE `id` = 2"), "completed");
}

TEST_F (MoveProcessorTests, EnterChannelWrongSegment)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"d": {"depth": 1, "dir": "east"}})", 200, "s1");

  /* Expire discover visit.  */
  Json::Value empty (Json::arrayValue);
  MoveProcessor proc (GetHandle (), 301, nextSegmentId, nextVisitId);
  proc.ProcessAll (empty);

  /* Alice is at segment 0 but tries to enter segment 1.  */
  ProcessMove ("alice", R"({"ec": {"id": 1}})", 400);

  EXPECT_EQ (QueryInt (
    "SELECT `in_channel` FROM `players` WHERE `name` = 'alice'"), 0);
}

TEST_F (MoveProcessorTests, ChannelDeathSetsHpToZero)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"d": {"depth": 1, "dir": "east"}})", 200, "s1");

  Json::Value empty (Json::arrayValue);
  MoveProcessor expProc (GetHandle (), 301, nextSegmentId, nextVisitId);
  expProc.ProcessAll (empty);

  ProcessMove ("alice", R"({"t": {"dir": "east"}})", 400, "tx1");
  ProcessMove ("alice", R"({"ec": {"id": 1}})", 500);

  /* Die in channel.  */
  ProcessMove ("alice", R"({"xc": {"id": 2, "results": {
    "survived": false, "xp": 10, "gold": 0, "kills": 1,
    "hp_remaining": 0
  }}})", 600);

  EXPECT_EQ (QueryInt (
    "SELECT `hp` FROM `players` WHERE `name` = 'alice'"), 0);
  EXPECT_EQ (QueryInt (
    "SELECT `deaths` FROM `players` WHERE `name` = 'alice'"), 1);
  EXPECT_EQ (QueryInt (
    "SELECT `in_channel` FROM `players` WHERE `name` = 'alice'"), 0);

  /* Still at segment 1 (death doesn't move you).  */
  EXPECT_EQ (QueryInt (
    "SELECT `current_segment` FROM `players` WHERE `name` = 'alice'"), 1);
}

// ============================================================
// Edge case: overworld blocked while in channel
// ============================================================

TEST_F (MoveProcessorTests, TravelBlockedWhileInChannel)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"d": {"depth": 1, "dir": "east"}})", 200, "s1");

  Json::Value empty (Json::arrayValue);
  MoveProcessor expProc (GetHandle (), 301, nextSegmentId, nextVisitId);
  expProc.ProcessAll (empty);

  ProcessMove ("alice", R"({"t": {"dir": "east"}})", 400, "tx1");
  ProcessMove ("alice", R"({"ec": {"id": 1}})", 500);

  /* Try to travel while in channel — should be blocked.  */
  ProcessMove ("alice", R"({"t": {"dir": "west"}})", 501, "tx2");
  EXPECT_EQ (QueryInt (
    "SELECT `current_segment` FROM `players` WHERE `name` = 'alice'"), 1);
}

TEST_F (MoveProcessorTests, UseItemBlockedWhileInChannel)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"d": {"depth": 1, "dir": "east"}})", 200, "s1");

  Json::Value empty (Json::arrayValue);
  MoveProcessor expProc (GetHandle (), 301, nextSegmentId, nextVisitId);
  expProc.ProcessAll (empty);

  ProcessMove ("alice", R"({"t": {"dir": "east"}})", 400, "tx1");

  Execute ("UPDATE `players` SET `hp` = 50 WHERE `name` = 'alice'");
  ProcessMove ("alice", R"({"ec": {"id": 1}})", 500);

  /* Try to use potion while in channel — blocked.  */
  ProcessMove ("alice", R"({"ui": {"item": "health_potion"}})", 501);
  EXPECT_EQ (QueryInt (
    "SELECT `hp` FROM `players` WHERE `name` = 'alice'"), 50);
}

TEST_F (MoveProcessorTests, DiscoverBlockedWhileInChannel)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"d": {"depth": 1, "dir": "east"}})", 200, "s1");

  Json::Value empty (Json::arrayValue);
  MoveProcessor expProc (GetHandle (), 301, nextSegmentId, nextVisitId);
  expProc.ProcessAll (empty);

  ProcessMove ("alice", R"({"t": {"dir": "east"}})", 400, "tx1");
  ProcessMove ("alice", R"({"ec": {"id": 1}})", 500);

  /* Try to discover while in channel — blocked.  */
  ProcessMove ("alice", R"({"d": {"depth": 2, "dir": "north"}})", 501, "s2");
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `segments`"), 1);
}

TEST_F (MoveProcessorTests, DiscoverAlreadyLinkedDirection)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"d": {"depth": 1, "dir": "east"}})", 200, "s1");

  Json::Value empty (Json::arrayValue);
  MoveProcessor expProc (GetHandle (), 301, nextSegmentId, nextVisitId);
  expProc.ProcessAll (empty);

  /* Try to discover east again — already linked.  */
  ProcessMove ("alice", R"({"d": {"depth": 2, "dir": "east"}})", 400, "s2");
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `segments`"), 1);
}

TEST_F (MoveProcessorTests, ChannelExitWithLoot)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"d": {"depth": 1, "dir": "east"}})", 200, "s1");

  Json::Value empty (Json::arrayValue);
  MoveProcessor expProc (GetHandle (), 301, nextSegmentId, nextVisitId);
  expProc.ProcessAll (empty);

  ProcessMove ("alice", R"({"t": {"dir": "east"}})", 400, "tx1");
  ProcessMove ("alice", R"({"ec": {"id": 1}})", 500);

  ProcessMove ("alice", R"({"xc": {"id": 2, "results": {
    "survived": true, "xp": 50, "gold": 30, "kills": 2,
    "hp_remaining": 80,
    "loot": [{"item": "iron_helmet", "n": 1}]
  }}})", 600);

  /* Loot should be in inventory.  */
  EXPECT_EQ (QueryInt (
    "SELECT `quantity` FROM `inventory`"
    " WHERE `name` = 'alice' AND `item_id` = 'iron_helmet'"), 1);

  /* Loot claim recorded.  */
  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `loot_claims` WHERE `visit_id` = 2"), 1);
}

TEST_F (MoveProcessorTests, DeadPlayerCanHealThenTravel)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"d": {"depth": 1, "dir": "east"}})", 200, "s1");

  Json::Value empty (Json::arrayValue);
  MoveProcessor expProc (GetHandle (), 301, nextSegmentId, nextVisitId);
  expProc.ProcessAll (empty);

  /* Set HP to 0.  */
  Execute ("UPDATE `players` SET `hp` = 0 WHERE `name` = 'alice'");

  /* Can't travel with 0 HP.  */
  ProcessMove ("alice", R"({"t": {"dir": "east"}})", 400, "tx1");
  EXPECT_EQ (QueryInt (
    "SELECT `current_segment` FROM `players` WHERE `name` = 'alice'"), 0);

  /* Use potion to heal (works even at 0 HP — not in channel).  */
  ProcessMove ("alice", R"({"ui": {"item": "health_potion"}})", 401);
  EXPECT_EQ (QueryInt (
    "SELECT `hp` FROM `players` WHERE `name` = 'alice'"), 25);

  /* Now can travel.  */
  ProcessMove ("alice", R"({"t": {"dir": "east"}})", 402, "tx2");
  EXPECT_EQ (QueryInt (
    "SELECT `current_segment` FROM `players` WHERE `name` = 'alice'"), 1);
}

} // anonymous namespace
} // namespace rog
