#include "statejson.hpp"
#include "moveprocessor.hpp"
#include "testutils.hpp"

#include <glog/logging.h>
#include <gtest/gtest.h>

namespace rog
{
namespace
{

class StateJsonTests : public DBTest
{

protected:

  int64_t nextSegmentId = 1;
  int64_t nextVisitId = 1;

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

  StateJsonExtractor
  Extractor ()
  {
    return StateJsonExtractor (GetHandle ());
  }

};

// ============================================================
// GetPlayerInfo
// ============================================================

TEST_F (StateJsonTests, PlayerNotFound)
{
  auto info = Extractor ().GetPlayerInfo ("nobody");
  EXPECT_TRUE (info.isNull ());
}

TEST_F (StateJsonTests, BasicPlayerInfo)
{
  ProcessMove ("alice", R"({"r": {}})");

  auto info = Extractor ().GetPlayerInfo ("alice");
  ASSERT_FALSE (info.isNull ());

  EXPECT_EQ (info["name"].asString (), "alice");
  EXPECT_EQ (info["level"].asInt (), 1);
  EXPECT_EQ (info["xp"].asInt (), 0);
  EXPECT_EQ (info["gold"].asInt (), 0);

  EXPECT_EQ (info["stats"]["strength"].asInt (), 10);
  EXPECT_EQ (info["stats"]["dexterity"].asInt (), 10);
  EXPECT_EQ (info["stats"]["constitution"].asInt (), 10);
  EXPECT_EQ (info["stats"]["intelligence"].asInt (), 10);

  EXPECT_EQ (info["skill_points"].asInt (), 0);
  EXPECT_EQ (info["stat_points"].asInt (), 0);

  EXPECT_EQ (info["combat_record"]["kills"].asInt (), 0);
  EXPECT_EQ (info["combat_record"]["deaths"].asInt (), 0);
  EXPECT_EQ (info["combat_record"]["visits_completed"].asInt (), 0);

  EXPECT_EQ (info["hp"].asInt (), 100);
  EXPECT_EQ (info["max_hp"].asInt (), 100);
  EXPECT_EQ (info["current_segment"].asInt (), 0);
  EXPECT_EQ (info["in_channel"].asBool (), false);

  /* Effective stats from equipment (short_sword = 5 atk, leather_armor = 2 def).  */
  ASSERT_TRUE (info.isMember ("effective_stats"));
  EXPECT_EQ (info["effective_stats"]["equip_attack"].asInt (), 5);
  EXPECT_EQ (info["effective_stats"]["equip_defense"].asInt (), 2);
  EXPECT_GT (info["effective_stats"]["attack_power"].asInt (), 0);
  EXPECT_GT (info["effective_stats"]["defense"].asInt (), 0);
}

TEST_F (StateJsonTests, PlayerInventory)
{
  ProcessMove ("alice", R"({"r": {}})");

  auto info = Extractor ().GetPlayerInfo ("alice");
  const auto& inv = info["inventory"];

  ASSERT_EQ (inv.size (), 3u);

  /* Inventory is ordered by slot then item_id.  */
  bool hasWeapon = false, hasArmor = false, hasPotion = false;
  for (const auto& item : inv)
    {
      if (item["item_id"].asString () == "short_sword")
        {
          EXPECT_EQ (item["slot"].asString (), "weapon");
          EXPECT_EQ (item["quantity"].asInt (), 1);
          hasWeapon = true;
        }
      else if (item["item_id"].asString () == "leather_armor")
        {
          EXPECT_EQ (item["slot"].asString (), "body");
          hasArmor = true;
        }
      else if (item["item_id"].asString () == "health_potion")
        {
          EXPECT_EQ (item["slot"].asString (), "bag");
          EXPECT_EQ (item["quantity"].asInt (), 3);
          hasPotion = true;
        }
    }
  EXPECT_TRUE (hasWeapon);
  EXPECT_TRUE (hasArmor);
  EXPECT_TRUE (hasPotion);
}

TEST_F (StateJsonTests, PlayerKnownSpells)
{
  ProcessMove ("alice", R"({"r": {}})");

  /* Manually insert a known spell.  */
  Execute ("INSERT INTO `known_spells` (`name`, `spell_id`)"
           " VALUES ('alice', 'magic_dart')");

  auto info = Extractor ().GetPlayerInfo ("alice");
  const auto& spells = info["known_spells"];

  ASSERT_EQ (spells.size (), 1u);
  EXPECT_EQ (spells[0].asString (), "magic_dart");
}

TEST_F (StateJsonTests, PlayerActiveVisit)
{
  ProcessMove ("alice", R"({"r": {}})");

  auto info = Extractor ().GetPlayerInfo ("alice");
  EXPECT_TRUE (info["active_visit"].isNull ());

  ProcessMove ("alice", R"({"d": {"depth": 2}})", 200);

  info = Extractor ().GetPlayerInfo ("alice");
  ASSERT_FALSE (info["active_visit"].isNull ());
  EXPECT_EQ (info["active_visit"]["visit_id"].asInt (), 1);
  EXPECT_EQ (info["active_visit"]["segment_id"].asInt (), 1);
}

// ============================================================
// ListSegments
// ============================================================

TEST_F (StateJsonTests, ListSegmentsEmpty)
{
  auto segs = Extractor ().ListSegments ();
  EXPECT_EQ (segs.size (), 0u);
}

TEST_F (StateJsonTests, ListSegmentsAll)
{
  ProcessMove ("alice", R"({"r": {}})");
  ProcessMove ("bob", R"({"r": {}})");
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 200, "s1");
  ProcessMove ("bob", R"({"d": {"depth": 3}})", 201, "s2");

  auto segs = Extractor ().ListSegments ();
  ASSERT_EQ (segs.size (), 2u);

  EXPECT_EQ (segs[0]["id"].asInt (), 1);
  EXPECT_EQ (segs[0]["discoverer"].asString (), "alice");
  EXPECT_EQ (segs[0]["depth"].asInt (), 1);
  EXPECT_EQ (segs[0]["visit_count"].asInt (), 1);

  EXPECT_EQ (segs[1]["id"].asInt (), 2);
  EXPECT_EQ (segs[1]["discoverer"].asString (), "bob");
}

// ============================================================
// GetSegmentInfo
// ============================================================

TEST_F (StateJsonTests, SegmentNotFound)
{
  auto info = Extractor ().GetSegmentInfo (999);
  EXPECT_TRUE (info.isNull ());
}

TEST_F (StateJsonTests, SegmentInfoBasic)
{
  ProcessMove ("alice", R"({"r": {}})");
  ProcessMove ("alice", R"({"d": {"depth": 3}})", 200, "myseed");

  auto info = Extractor ().GetSegmentInfo (1);
  ASSERT_FALSE (info.isNull ());

  EXPECT_EQ (info["id"].asInt (), 1);
  EXPECT_EQ (info["discoverer"].asString (), "alice");
  EXPECT_EQ (info["seed"].asString (), "myseed");
  EXPECT_EQ (info["depth"].asInt (), 3);
  EXPECT_EQ (info["created_height"].asInt (), 200);

  /* Gates should be stored (4 directions).  */
  ASSERT_TRUE (info.isMember ("gates"));
  EXPECT_EQ (info["gates"].size (), 4u);
  EXPECT_TRUE (info["gates"].isMember ("north"));
  EXPECT_TRUE (info["gates"].isMember ("south"));
  EXPECT_TRUE (info["gates"].isMember ("east"));
  EXPECT_TRUE (info["gates"].isMember ("west"));

  /* Links and visits.  */
  EXPECT_TRUE (info.isMember ("links"));
  EXPECT_TRUE (info.isMember ("visits"));

  const auto& visits = info["visits"];
  ASSERT_EQ (visits.size (), 1u);
  EXPECT_EQ (visits[0]["id"].asInt (), 1);
  EXPECT_EQ (visits[0]["initiator"].asString (), "alice");
  EXPECT_EQ (visits[0]["status"].asString (), "open");
}

TEST_F (StateJsonTests, SegmentInfoWithLinks)
{
  ProcessMove ("alice", R"({"r": {}})");
  ProcessMove ("alice", R"({"d": {"depth": 1, "dir": "east"}})", 200, "s1");

  auto info = Extractor ().GetSegmentInfo (1);
  ASSERT_FALSE (info.isNull ());

  /* Should have a west link back to segment 0.  */
  ASSERT_TRUE (info["links"].isMember ("west"));
  EXPECT_EQ (info["links"]["west"]["to_segment"].asInt (), 0);
  EXPECT_EQ (info["links"]["west"]["to_direction"].asString (), "east");
}

// ============================================================
// ListVisits
// ============================================================

TEST_F (StateJsonTests, ListVisitsEmpty)
{
  auto vis = Extractor ().ListVisits ("");
  EXPECT_EQ (vis.size (), 0u);
}

TEST_F (StateJsonTests, ListVisitsAll)
{
  ProcessMove ("alice", R"({"r": {}})");
  ProcessMove ("bob", R"({"r": {}})");
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 200, "s1");
  ProcessMove ("bob", R"({"d": {"depth": 3}})", 201, "s2");

  auto vis = Extractor ().ListVisits ("");
  ASSERT_EQ (vis.size (), 2u);

  EXPECT_EQ (vis[0]["id"].asInt (), 1);
  EXPECT_EQ (vis[0]["segment_id"].asInt (), 1);
  EXPECT_EQ (vis[0]["initiator"].asString (), "alice");
  EXPECT_EQ (vis[0]["status"].asString (), "open");
  EXPECT_EQ (vis[0]["players"].asInt (), 1);

  EXPECT_EQ (vis[1]["id"].asInt (), 2);
  EXPECT_EQ (vis[1]["segment_id"].asInt (), 2);
  EXPECT_EQ (vis[1]["initiator"].asString (), "bob");
}

TEST_F (StateJsonTests, ListVisitsFiltered)
{
  ProcessMove ("alice", R"({"r": {}})");
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 200);

  auto open = Extractor ().ListVisits ("open");
  EXPECT_EQ (open.size (), 1u);

  auto active = Extractor ().ListVisits ("active");
  EXPECT_EQ (active.size (), 0u);
}

// ============================================================
// GetVisitInfo
// ============================================================

TEST_F (StateJsonTests, VisitNotFound)
{
  auto info = Extractor ().GetVisitInfo (999);
  EXPECT_TRUE (info.isNull ());
}

TEST_F (StateJsonTests, VisitInfoBasic)
{
  ProcessMove ("alice", R"({"r": {}})");
  ProcessMove ("bob", R"({"r": {}})");
  ProcessMove ("alice", R"({"d": {"depth": 3}})", 200, "myseed");
  ProcessMove ("bob", R"({"j": {"id": 1}})", 201);

  auto info = Extractor ().GetVisitInfo (1);
  ASSERT_FALSE (info.isNull ());

  EXPECT_EQ (info["id"].asInt (), 1);
  EXPECT_EQ (info["segment_id"].asInt (), 1);
  EXPECT_EQ (info["initiator"].asString (), "alice");
  EXPECT_EQ (info["seed"].asString (), "myseed");
  EXPECT_EQ (info["depth"].asInt (), 3);
  EXPECT_EQ (info["status"].asString (), "open");
  EXPECT_EQ (info["created_height"].asInt (), 200);

  const auto& parts = info["participants"];
  ASSERT_EQ (parts.size (), 2u);
  EXPECT_EQ (parts[0].asString (), "alice");
  EXPECT_EQ (parts[1].asString (), "bob");

  /* No results yet.  */
  EXPECT_FALSE (info.isMember ("results"));
}

TEST_F (StateJsonTests, VisitInfoWithResults)
{
  ProcessMove ("alice", R"({"r": {}})");
  ProcessMove ("bob", R"({"r": {}})");
  ProcessMove ("charlie", R"({"r": {}})");
  ProcessMove ("dave", R"({"r": {}})");
  ProcessMove ("alice", R"({"d": {"depth": 1}})", 200, "s1");
  ProcessMove ("bob", R"({"j": {"id": 1}})", 201);
  ProcessMove ("charlie", R"({"j": {"id": 1}})", 202);
  ProcessMove ("dave", R"({"j": {"id": 1}})", 203);

  ProcessMove ("alice", R"({"s": {"id": 1, "results": [
    {"p": "alice", "survived": true, "xp": 100, "gold": 50, "kills": 5,
     "loot": [{"item": "iron_helmet", "n": 1}]},
    {"p": "bob", "survived": false, "xp": 20, "gold": 10, "kills": 1},
    {"p": "charlie", "survived": true, "xp": 0, "gold": 0, "kills": 0},
    {"p": "dave", "survived": true, "xp": 0, "gold": 0, "kills": 0}
  ]}})", 300);

  auto info = Extractor ().GetVisitInfo (1);
  EXPECT_EQ (info["status"].asString (), "completed");
  EXPECT_EQ (info["settled_height"].asInt (), 300);

  ASSERT_TRUE (info.isMember ("results"));
  const auto& results = info["results"];
  ASSERT_EQ (results.size (), 4u);

  /* Results are ordered by name.  */
  EXPECT_EQ (results[0]["name"].asString (), "alice");
  EXPECT_EQ (results[0]["survived"].asBool (), true);
  EXPECT_EQ (results[0]["xp_gained"].asInt (), 100);
  EXPECT_EQ (results[0]["loot"].size (), 1u);
  EXPECT_EQ (results[0]["loot"][0]["item_id"].asString (), "iron_helmet");

  EXPECT_EQ (results[1]["name"].asString (), "bob");
  EXPECT_EQ (results[1]["survived"].asBool (), false);
}

// ============================================================
// FullState
// ============================================================

TEST_F (StateJsonTests, FullState)
{
  ProcessMove ("alice", R"({"r": {}})");
  ProcessMove ("bob", R"({"r": {}})");
  ProcessMove ("alice", R"({"d": {"depth": 2}})", 200);

  auto state = Extractor ().FullState ();

  EXPECT_EQ (state["players"].size (), 2u);
  EXPECT_EQ (state["players"][0]["name"].asString (), "alice");
  EXPECT_EQ (state["players"][1]["name"].asString (), "bob");

  /* Permanent segments.  */
  EXPECT_EQ (state["segments"].size (), 1u);
  EXPECT_EQ (state["segments"][0]["discoverer"].asString (), "alice");

  /* Active visits.  */
  EXPECT_EQ (state["visits"].size (), 1u);
  EXPECT_EQ (state["visits"][0]["initiator"].asString (), "alice");
}

} // anonymous namespace
} // namespace rog
