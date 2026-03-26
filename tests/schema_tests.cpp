#include "testutils.hpp"

#include <glog/logging.h>
#include <gtest/gtest.h>

extern "C" const char* GetSchemaSQL ();

namespace rog
{
namespace
{

using SchemaTests = DBTest;

TEST_F (SchemaTests, Valid)
{
  /* DBTest constructor already applies the schema.  */
}

TEST_F (SchemaTests, MultipleTimesIsOk)
{
  Execute (GetSchemaSQL ());
  Execute (GetSchemaSQL ());
}

TEST_F (SchemaTests, InsertPlayer)
{
  InsertPlayer ("alice", 100);

  EXPECT_EQ (QueryInt (
    "SELECT `level` FROM `players` WHERE `name` = 'alice'"), 1);
  EXPECT_EQ (QueryInt (
    "SELECT `strength` FROM `players` WHERE `name` = 'alice'"), 10);
  EXPECT_EQ (QueryInt (
    "SELECT `dexterity` FROM `players` WHERE `name` = 'alice'"), 10);
  EXPECT_EQ (QueryInt (
    "SELECT `constitution` FROM `players` WHERE `name` = 'alice'"), 10);
  EXPECT_EQ (QueryInt (
    "SELECT `intelligence` FROM `players` WHERE `name` = 'alice'"), 10);
  EXPECT_EQ (QueryInt (
    "SELECT `gold` FROM `players` WHERE `name` = 'alice'"), 0);
  EXPECT_EQ (QueryInt (
    "SELECT `xp` FROM `players` WHERE `name` = 'alice'"), 0);
}

TEST_F (SchemaTests, InsertSegment)
{
  InsertPlayer ("alice", 100);

  Execute (
    "INSERT INTO `segments`"
    " (`id`, `discoverer`, `seed`, `depth`, `created_height`)"
    " VALUES (1, 'alice', 'abc123', 3, 100)");

  EXPECT_EQ (QueryString (
    "SELECT `discoverer` FROM `segments` WHERE `id` = 1"), "alice");
  EXPECT_EQ (QueryInt (
    "SELECT `max_players` FROM `segments` WHERE `id` = 1"), 4);
}

TEST_F (SchemaTests, InsertVisit)
{
  InsertPlayer ("alice", 100);

  Execute (
    "INSERT INTO `segments`"
    " (`id`, `discoverer`, `seed`, `depth`, `created_height`)"
    " VALUES (1, 'alice', 'abc123', 3, 100)");

  Execute (
    "INSERT INTO `visits`"
    " (`id`, `segment_id`, `initiator`, `created_height`)"
    " VALUES (1, 1, 'alice', 100)");

  EXPECT_EQ (QueryString (
    "SELECT `status` FROM `visits` WHERE `id` = 1"), "open");
  EXPECT_EQ (QueryString (
    "SELECT `initiator` FROM `visits` WHERE `id` = 1"), "alice");
  EXPECT_EQ (QueryInt (
    "SELECT `segment_id` FROM `visits` WHERE `id` = 1"), 1);
}

TEST_F (SchemaTests, InsertInventory)
{
  InsertPlayer ("alice", 100);
  InsertItem ("alice", "short_sword", 1, "weapon");
  InsertItem ("alice", "health_potion", 3, "bag");

  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `inventory` WHERE `name` = 'alice'"), 2);
  EXPECT_EQ (QueryInt (
    "SELECT `quantity` FROM `inventory`"
    " WHERE `name` = 'alice' AND `item_id` = 'health_potion'"), 3);
}

TEST_F (SchemaTests, InsertKnownSpells)
{
  InsertPlayer ("alice", 100);

  Execute (
    "INSERT INTO `known_spells` (`name`, `spell_id`)"
    " VALUES ('alice', 'magic_dart')");

  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `known_spells` WHERE `name` = 'alice'"), 1);
}

TEST_F (SchemaTests, VisitParticipants)
{
  InsertPlayer ("alice", 100);
  InsertPlayer ("bob", 101);

  Execute (
    "INSERT INTO `segments`"
    " (`id`, `discoverer`, `seed`, `depth`, `created_height`)"
    " VALUES (1, 'alice', 'abc', 2, 100)");

  Execute (
    "INSERT INTO `visits`"
    " (`id`, `segment_id`, `initiator`, `created_height`)"
    " VALUES (1, 1, 'alice', 100)");

  Execute (
    "INSERT INTO `visit_participants` (`visit_id`, `name`, `joined_height`)"
    " VALUES (1, 'alice', 100), (1, 'bob', 101)");

  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `visit_participants` WHERE `visit_id` = 1"), 2);
}

TEST_F (SchemaTests, InsertSegmentGates)
{
  InsertPlayer ("alice", 100);

  Execute (
    "INSERT INTO `segments`"
    " (`id`, `discoverer`, `seed`, `depth`, `created_height`)"
    " VALUES (1, 'alice', 'abc', 2, 100)");

  Execute (
    "INSERT INTO `segment_gates`"
    " (`segment_id`, `direction`, `x`, `y`)"
    " VALUES (1, 'north', 30, 0),"
    "        (1, 'south', 50, 39),"
    "        (1, 'east', 79, 20),"
    "        (1, 'west', 0, 15)");

  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `segment_gates` WHERE `segment_id` = 1"), 4);
  EXPECT_EQ (QueryInt (
    "SELECT `x` FROM `segment_gates`"
    " WHERE `segment_id` = 1 AND `direction` = 'north'"), 30);
}

TEST_F (SchemaTests, InsertSegmentLinks)
{
  InsertPlayer ("alice", 100);

  Execute (
    "INSERT INTO `segments`"
    " (`id`, `discoverer`, `seed`, `depth`, `created_height`)"
    " VALUES (1, 'alice', 'abc', 2, 100),"
    "        (2, 'alice', 'def', 3, 101)");

  Execute (
    "INSERT INTO `segment_links`"
    " (`from_segment`, `from_direction`, `to_segment`, `to_direction`)"
    " VALUES (1, 'east', 2, 'west'),"
    "        (2, 'west', 1, 'east')");

  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `segment_links`"), 2);
  EXPECT_EQ (QueryInt (
    "SELECT `to_segment` FROM `segment_links`"
    " WHERE `from_segment` = 1 AND `from_direction` = 'east'"), 2);
  EXPECT_EQ (QueryString (
    "SELECT `to_direction` FROM `segment_links`"
    " WHERE `from_segment` = 1 AND `from_direction` = 'east'"), "west");
}

TEST_F (SchemaTests, PlayerOverworldColumns)
{
  InsertPlayer ("alice", 100);

  EXPECT_EQ (QueryInt (
    "SELECT `hp` FROM `players` WHERE `name` = 'alice'"), 100);
  EXPECT_EQ (QueryInt (
    "SELECT `max_hp` FROM `players` WHERE `name` = 'alice'"), 100);
  EXPECT_EQ (QueryInt (
    "SELECT `current_segment` FROM `players` WHERE `name` = 'alice'"), 0);
  EXPECT_EQ (QueryInt (
    "SELECT `in_channel` FROM `players` WHERE `name` = 'alice'"), 0);
}

} // anonymous namespace
} // namespace rog
