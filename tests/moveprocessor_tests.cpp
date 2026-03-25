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

    MoveProcessor proc (GetHandle (), height, nextSegmentId);
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

  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `segments`"), 1);
  EXPECT_EQ (QueryString (
    "SELECT `discoverer` FROM `segments` WHERE `id` = 1"), "alice");
  EXPECT_EQ (QueryInt (
    "SELECT `depth` FROM `segments` WHERE `id` = 1"), 3);
  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `segments` WHERE `id` = 1"), "open");
  EXPECT_EQ (QueryString (
    "SELECT `seed` FROM `segments` WHERE `id` = 1"), "abc123");
  EXPECT_EQ (QueryInt (
    "SELECT `created_height` FROM `segments` WHERE `id` = 1"), 200);

  /* Discoverer is automatically first participant.  */
  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `segment_participants` WHERE `segment_id` = 1"), 1);
  EXPECT_EQ (QueryString (
    "SELECT `name` FROM `segment_participants` WHERE `segment_id` = 1"),
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

TEST_F (MoveProcessorTests, DiscoverWhileInSegment)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 200);

  /* Second discover should fail — alice is already in a segment.  */
  ProcessMove ("alice", R"({"d": {"depth": 2}})", 201);
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `segments`"), 1);
}

// ============================================================
// Segment join tests
// ============================================================

TEST_F (MoveProcessorTests, JoinValid)
{
  RegisterPlayer ("alice");
  RegisterPlayer ("bob");
  ProcessMove ("alice", R"({"d": {"depth": 2}})", 200);

  ProcessMove ("bob", R"({"j": {"id": 1}})", 201);

  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `segment_participants` WHERE `segment_id` = 1"), 2);

  /* Segment should still be open (2/4 players).  */
  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `segments` WHERE `id` = 1"), "open");
}

TEST_F (MoveProcessorTests, JoinFillsSegment)
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
    "SELECT COUNT(*) FROM `segment_participants` WHERE `segment_id` = 1"), 4);

  /* Segment should now be active.  */
  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `segments` WHERE `id` = 1"), "active");
  EXPECT_EQ (QueryInt (
    "SELECT `started_height` FROM `segments` WHERE `id` = 1"), 203);
}

TEST_F (MoveProcessorTests, JoinNonexistentSegment)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"j": {"id": 999}})");

  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `segment_participants`"), 0);
}

TEST_F (MoveProcessorTests, JoinAlreadyInSegment)
{
  RegisterPlayer ("alice");
  RegisterPlayer ("bob");
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 200);

  /* Bob joins, then tries to join a second segment.  */
  ProcessMove ("bob", R"({"j": {"id": 1}})", 201);

  RegisterPlayer ("charlie");
  ProcessMove ("charlie", R"({"d": {"depth": 2}})", 202);

  ProcessMove ("bob", R"({"j": {"id": 2}})", 203);
  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `segment_participants` WHERE `segment_id` = 2"), 1);
}

// ============================================================
// Segment leave tests
// ============================================================

TEST_F (MoveProcessorTests, LeaveValid)
{
  RegisterPlayer ("alice");
  RegisterPlayer ("bob");
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 200);
  ProcessMove ("bob", R"({"j": {"id": 1}})", 201);

  ProcessMove ("bob", R"({"lv": {"id": 1}})", 202);

  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `segment_participants` WHERE `segment_id` = 1"), 1);
}

TEST_F (MoveProcessorTests, LeaveDiscovererBlocked)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 200);

  /* Discoverer cannot leave.  */
  ProcessMove ("alice", R"({"lv": {"id": 1}})", 201);

  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `segment_participants` WHERE `segment_id` = 1"), 1);
}

TEST_F (MoveProcessorTests, LeaveNotInSegment)
{
  RegisterPlayer ("alice");
  RegisterPlayer ("bob");
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 200);

  /* Bob never joined.  */
  ProcessMove ("bob", R"({"lv": {"id": 1}})", 201);

  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `segment_participants` WHERE `segment_id` = 1"), 1);
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
    "SELECT COUNT(*) FROM `segment_participants` WHERE `segment_id` = 1"), 2);
}

// ============================================================
// Helper to set up a full active segment with 4 players
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

  /* Segment should be completed.  */
  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `segments` WHERE `id` = 1"), "completed");
  EXPECT_EQ (QueryInt (
    "SELECT `settled_height` FROM `segments` WHERE `id` = 1"), 300);

  /* Check segment results recorded.  */
  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `segment_results` WHERE `segment_id` = 1"), 4);

  /* Check player stats updated.  */
  EXPECT_EQ (QueryInt (
    "SELECT `gold` FROM `players` WHERE `name` = 'alice'"), 100);
  EXPECT_EQ (QueryInt (
    "SELECT `kills` FROM `players` WHERE `name` = 'alice'"), 3);
  EXPECT_EQ (QueryInt (
    "SELECT `segments_completed` FROM `players` WHERE `name` = 'alice'"), 1);
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
    "SELECT COUNT(*) FROM `loot_claims` WHERE `segment_id` = 1"), 3);

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

TEST_F (SettleTests, OnlyDiscovererCanSettle)
{
  /* Bob is not the discoverer.  */
  ProcessMove ("bob", R"({"s": {"id": 1, "results": [
    {"p": "alice", "survived": true, "xp": 0, "gold": 0, "kills": 0},
    {"p": "bob", "survived": true, "xp": 0, "gold": 0, "kills": 0},
    {"p": "charlie", "survived": true, "xp": 0, "gold": 0, "kills": 0},
    {"p": "dave", "survived": true, "xp": 0, "gold": 0, "kills": 0}
  ]}})", 300);

  /* Should still be active — settle was rejected.  */
  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `segments` WHERE `id` = 1"), "active");
}

TEST_F (SettleTests, CannotSettleOpenSegment)
{
  /* Create a new segment that stays open (only alice in it).
     First, we need alice free — but she's already in segment 1.
     Register a fresh player.  */
  RegisterPlayer ("eve");
  ProcessMove ("eve", R"({"d": {"depth": 1}})", 300, "seed456");

  ProcessMove ("eve", R"({"s": {"id": 2, "results": [
    {"p": "eve", "survived": true, "xp": 0, "gold": 0, "kills": 0}
  ]}})", 301);

  /* Should still be open — can't settle an open segment.  */
  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `segments` WHERE `id` = 2"), "open");
}

TEST_F (SettleTests, NonParticipantInResults)
{
  /* Eve is not in segment 1.  */
  RegisterPlayer ("eve");

  ProcessMove ("alice", R"({"s": {"id": 1, "results": [
    {"p": "eve", "survived": true, "xp": 50, "gold": 0, "kills": 0}
  ]}})", 300);

  /* Should still be active — settle was rejected.  */
  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `segments` WHERE `id` = 1"), "active");
}

// ============================================================
// Stat allocation tests
// ============================================================

class StatAllocTests : public MoveProcessorTests
{

protected:

  void SetUp () override
  {
    /* Register alice and give her stat points via a settled segment.  */
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

// ============================================================
// Timeout tests
// ============================================================

TEST_F (MoveProcessorTests, OpenSegmentExpires)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 100);

  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `segments` WHERE `id` = 1"), "open");

  /* Process an empty block at height 100 + 100 = 200 (timeout boundary).  */
  Json::Value empty (Json::arrayValue);
  MoveProcessor proc (GetHandle (), 200, nextSegmentId);
  proc.ProcessAll (empty);

  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `segments` WHERE `id` = 1"), "expired");
}

TEST_F (MoveProcessorTests, OpenSegmentNotExpiredYet)
{
  RegisterPlayer ("alice");
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 100);

  /* One block before timeout — should still be open.  */
  Json::Value empty (Json::arrayValue);
  MoveProcessor proc (GetHandle (), 199, nextSegmentId);
  proc.ProcessAll (empty);

  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `segments` WHERE `id` = 1"), "open");
}

TEST_F (MoveProcessorTests, ActiveSegmentForceSettles)
{
  RegisterPlayer ("alice");
  RegisterPlayer ("bob");
  RegisterPlayer ("charlie");
  RegisterPlayer ("dave");
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 100);
  ProcessMove ("bob", R"({"j": {"id": 1}})", 101);
  ProcessMove ("charlie", R"({"j": {"id": 1}})", 102);
  ProcessMove ("dave", R"({"j": {"id": 1}})", 103);

  /* Segment became active at height 103. Timeout at 103 + 1000 = 1103.  */
  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `segments` WHERE `id` = 1"), "active");

  Json::Value empty (Json::arrayValue);
  MoveProcessor proc (GetHandle (), 1103, nextSegmentId);
  proc.ProcessAll (empty);

  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `segments` WHERE `id` = 1"), "completed");

  /* All players get a death.  */
  EXPECT_EQ (QueryInt (
    "SELECT `deaths` FROM `players` WHERE `name` = 'alice'"), 1);
  EXPECT_EQ (QueryInt (
    "SELECT `deaths` FROM `players` WHERE `name` = 'bob'"), 1);

  /* Results recorded with survived=0 and no rewards.  */
  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `segment_results` WHERE `segment_id` = 1"), 4);
  EXPECT_EQ (QueryInt (
    "SELECT `survived` FROM `segment_results`"
    " WHERE `segment_id` = 1 AND `name` = 'alice'"), 0);
  EXPECT_EQ (QueryInt (
    "SELECT `xp_gained` FROM `segment_results`"
    " WHERE `segment_id` = 1 AND `name` = 'alice'"), 0);
}

TEST_F (MoveProcessorTests, ActiveSegmentNotTimedOutYet)
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
  MoveProcessor proc (GetHandle (), 1102, nextSegmentId);
  proc.ProcessAll (empty);

  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `segments` WHERE `id` = 1"), "active");
}

} // anonymous namespace
} // namespace rog
