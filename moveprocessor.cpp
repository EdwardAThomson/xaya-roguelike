#include "moveprocessor.hpp"
#include "dungeon.hpp"
#include "dungeongame.hpp"
#include "hash.hpp"
#include "items.hpp"

#include <glog/logging.h>

#include <algorithm>
#include <cmath>
#include <random>

namespace rog
{

namespace
{

/**
 * XP required to reach a given level.  Matches the JS formula:
 * calculateExperienceForLevel(level) = floor(100 * pow(level, 1.5))
 *
 * Note: the JS code calls this with (level + 1) to get the threshold
 * for the *next* level.  We do the same here.
 */
int64_t
XpForLevel (const int level)
{
  return static_cast<int64_t> (std::floor (100.0 * std::pow (level, 1.5)));
}

} // anonymous namespace

void
MoveProcessor::GiveStartingItems (const std::string& name)
{
  /* Starting loadout matching the JS Player class:
     - Short Sword (weapon slot)
     - Leather Armor (body slot)
     - 3x Health Potion (bag)  */

  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "INSERT INTO `inventory` (`name`, `item_id`, `quantity`, `slot`)"
    " VALUES (?1, ?2, ?3, ?4)",
    -1, &stmt, nullptr);

  auto insertItem = [&] (const char* itemId, int qty, const char* slot)
    {
      sqlite3_reset (stmt);
      sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text (stmt, 2, itemId, -1, SQLITE_STATIC);
      sqlite3_bind_int64 (stmt, 3, qty);
      sqlite3_bind_text (stmt, 4, slot, -1, SQLITE_STATIC);
      sqlite3_step (stmt);
    };

  insertItem ("short_sword", 1, "weapon");
  insertItem ("leather_armor", 1, "body");
  insertItem ("health_potion", 3, "bag");

  sqlite3_finalize (stmt);
}

int64_t
MoveProcessor::CountParticipants (const int64_t visitId)
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT COUNT(*) FROM `visit_participants`"
    " WHERE `visit_id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visitId);
  sqlite3_step (stmt);
  const int64_t count = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);
  return count;
}

int64_t
MoveProcessor::GetMaxPlayers (const int64_t visitId)
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT s.`max_players` FROM `visits` v"
    " JOIN `segments` s ON v.`segment_id` = s.`id`"
    " WHERE v.`id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visitId);
  sqlite3_step (stmt);
  const int64_t max = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);
  return max;
}

void
MoveProcessor::ProcessRegister (const std::string& name)
{
  const int maxHp = BASE_HP + 10 * HP_PER_CON;  /* con=10 at registration */

  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "INSERT INTO `players`"
    " (`name`, `registered_height`, `hp`, `max_hp`)"
    " VALUES (?1, ?2, ?3, ?3)",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 2, currentHeight);
  sqlite3_bind_int64 (stmt, 3, maxHp);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  GiveStartingItems (name);

  LOG (INFO) << "Registered player " << name
             << " at height " << currentHeight
             << " with " << maxHp << " HP";
}

void
MoveProcessor::ProcessDiscover (const std::string& name, const int depth,
                                 const std::string& txid,
                                 const std::string& dir)
{
  const int64_t segId = nextSegmentId++;

  /* Use the txid as the seed (deterministic across all nodes).
     Mix in the dungeon_id (from meta table) so that different game
     instances on the same chain produce different dungeons.  */
  std::string dungeonId;
  {
    sqlite3_stmt* metaStmt;
    sqlite3_prepare_v2 (db,
      "SELECT `value` FROM `meta` WHERE `key` = 'dungeon_id'",
      -1, &metaStmt, nullptr);
    if (sqlite3_step (metaStmt) == SQLITE_ROW)
      dungeonId = reinterpret_cast<const char*> (
          sqlite3_column_text (metaStmt, 0));
    sqlite3_finalize (metaStmt);
  }

  const std::string baseSeed = txid.empty ()
      ? std::to_string (segId)
      : txid;
  const std::string seed = dungeonId.empty ()
      ? baseSeed
      : dungeonId + ":" + baseSeed;

  /* Create permanent segment.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "INSERT INTO `segments`"
    " (`id`, `discoverer`, `seed`, `depth`, `created_height`)"
    " VALUES (?1, ?2, ?3, ?4, ?5)",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, segId);
  sqlite3_bind_text (stmt, 2, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (stmt, 3, seed.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 4, depth);
  sqlite3_bind_int64 (stmt, 5, currentHeight);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  /* Generate dungeon to get gate positions.  If we have a direction,
     constrain the opposite gate from the source segment.  */
  std::vector<Gate> constraints;
  std::string oppositeDir;

  if (!dir.empty ())
    {
      if (dir == "north") oppositeDir = "south";
      else if (dir == "south") oppositeDir = "north";
      else if (dir == "east") oppositeDir = "west";
      else if (dir == "west") oppositeDir = "east";

      /* Look up the source segment's gate position in the requested
         direction so we can align the new segment's opposite gate.  */
      sqlite3_prepare_v2 (db,
        "SELECT `current_segment` FROM `players` WHERE `name` = ?1",
        -1, &stmt, nullptr);
      sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_step (stmt);
      const int64_t srcSeg = sqlite3_column_int64 (stmt, 0);
      sqlite3_finalize (stmt);

      /* Get the source gate position.  If source is segment 0 (origin),
         there are no gate positions — just use unconstrained.  */
      sqlite3_prepare_v2 (db,
        "SELECT `x`, `y` FROM `segment_gates`"
        " WHERE `segment_id` = ?1 AND `direction` = ?2",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, srcSeg);
      sqlite3_bind_text (stmt, 2, dir.c_str (), -1, SQLITE_TRANSIENT);

      if (sqlite3_step (stmt) == SQLITE_ROW)
        {
          Gate g;
          g.x = static_cast<int> (sqlite3_column_int64 (stmt, 0));
          g.y = static_cast<int> (sqlite3_column_int64 (stmt, 1));

          /* Mirror the position for the opposite wall.  */
          if (oppositeDir == "north") g.y = 0;
          else if (oppositeDir == "south") g.y = Dungeon::HEIGHT - 1;
          else if (oppositeDir == "west") g.x = 0;
          else if (oppositeDir == "east") g.x = Dungeon::WIDTH - 1;

          g.direction = oppositeDir;
          constraints.push_back (g);
        }
      sqlite3_finalize (stmt);
    }

  const auto dungeon = constraints.empty ()
      ? Dungeon::Generate (seed, depth)
      : Dungeon::Generate (seed, depth, constraints);

  /* Store gate positions.  */
  for (const auto& gate : dungeon.GetGates ())
    {
      sqlite3_prepare_v2 (db,
        "INSERT INTO `segment_gates`"
        " (`segment_id`, `direction`, `x`, `y`)"
        " VALUES (?1, ?2, ?3, ?4)",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, segId);
      sqlite3_bind_text (stmt, 2, gate.direction.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64 (stmt, 3, gate.x);
      sqlite3_bind_int64 (stmt, 4, gate.y);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }

  /* If direction provided, create bidirectional link.  */
  if (!dir.empty ())
    {
      sqlite3_prepare_v2 (db,
        "SELECT `current_segment` FROM `players` WHERE `name` = ?1",
        -1, &stmt, nullptr);
      sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_step (stmt);
      const int64_t srcSeg = sqlite3_column_int64 (stmt, 0);
      sqlite3_finalize (stmt);

      /* src -> new */
      sqlite3_prepare_v2 (db,
        "INSERT INTO `segment_links`"
        " (`from_segment`, `from_direction`, `to_segment`, `to_direction`)"
        " VALUES (?1, ?2, ?3, ?4)",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, srcSeg);
      sqlite3_bind_text (stmt, 2, dir.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64 (stmt, 3, segId);
      sqlite3_bind_text (stmt, 4, oppositeDir.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);

      /* new -> src */
      sqlite3_prepare_v2 (db,
        "INSERT INTO `segment_links`"
        " (`from_segment`, `from_direction`, `to_segment`, `to_direction`)"
        " VALUES (?1, ?2, ?3, ?4)",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, segId);
      sqlite3_bind_text (stmt, 2, oppositeDir.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64 (stmt, 3, srcSeg);
      sqlite3_bind_text (stmt, 4, dir.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }

  /* Update discovery cooldown.  */
  sqlite3_prepare_v2 (db,
    "UPDATE `players` SET `last_discover_height` = ?2 WHERE `name` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 2, currentHeight);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  /* Segment is provisional (confirmed=0) until the discoverer completes
     a valid run.  No visit is auto-created — the player must enter
     the channel separately via "ec".  */

  LOG (INFO) << "Player " << name << " discovered provisional segment "
             << segId << " (depth " << depth << ")";
}

void
MoveProcessor::ProcessVisit (const std::string& name,
                              const int64_t segmentId)
{
  const int64_t visId = nextVisitId++;

  /* Create a new visit to this segment.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "INSERT INTO `visits`"
    " (`id`, `segment_id`, `initiator`, `created_height`)"
    " VALUES (?1, ?2, ?3, ?4)",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visId);
  sqlite3_bind_int64 (stmt, 2, segmentId);
  sqlite3_bind_text (stmt, 3, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 4, currentHeight);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  /* Initiator is the first participant.  */
  sqlite3_prepare_v2 (db,
    "INSERT INTO `visit_participants`"
    " (`visit_id`, `name`, `joined_height`)"
    " VALUES (?1, ?2, ?3)",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visId);
  sqlite3_bind_text (stmt, 2, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 3, currentHeight);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  LOG (INFO) << "Player " << name << " started visit " << visId
             << " to segment " << segmentId;
}

void
MoveProcessor::ProcessJoin (const std::string& name, const int64_t visitId)
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "INSERT INTO `visit_participants`"
    " (`visit_id`, `name`, `joined_height`)"
    " VALUES (?1, ?2, ?3)",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visitId);
  sqlite3_bind_text (stmt, 2, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 3, currentHeight);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  LOG (INFO) << "Player " << name << " joined visit " << visitId;

  /* If visit is now full, set status to active.  */
  const int64_t count = CountParticipants (visitId);
  const int64_t max = GetMaxPlayers (visitId);

  if (count >= max)
    {
      sqlite3_prepare_v2 (db,
        "UPDATE `visits`"
        " SET `status` = 'active', `started_height` = ?2"
        " WHERE `id` = ?1",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, visitId);
      sqlite3_bind_int64 (stmt, 2, currentHeight);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);

      LOG (INFO) << "Visit " << visitId << " is now active (full)";
    }
}

void
MoveProcessor::ProcessLeave (const std::string& name, const int64_t visitId)
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "DELETE FROM `visit_participants`"
    " WHERE `visit_id` = ?1 AND `name` = ?2",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visitId);
  sqlite3_bind_text (stmt, 2, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  LOG (INFO) << "Player " << name << " left visit " << visitId;
}

void
MoveProcessor::ProcessSettle (const std::string& name,
                               const int64_t visitId,
                               const Json::Value& results)
{
  for (const auto& r : results)
    {
      const std::string playerName = r["p"].asString ();
      const bool survived = r.get ("survived", false).asBool ();
      const int64_t xpGained = r.get ("xp", 0).asInt64 ();
      const int64_t goldGained = r.get ("gold", 0).asInt64 ();
      const int64_t killsGained = r.get ("kills", 0).asInt64 ();

      /* Insert visit result.  */
      sqlite3_stmt* stmt;
      sqlite3_prepare_v2 (db,
        "INSERT INTO `visit_results`"
        " (`visit_id`, `name`, `survived`, `xp_gained`,"
        "  `gold_gained`, `kills`)"
        " VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, visitId);
      sqlite3_bind_text (stmt, 2, playerName.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64 (stmt, 3, survived ? 1 : 0);
      sqlite3_bind_int64 (stmt, 4, xpGained);
      sqlite3_bind_int64 (stmt, 5, goldGained);
      sqlite3_bind_int64 (stmt, 6, killsGained);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);

      /* Process loot: insert claims and add to player inventory.  */
      if (r.isMember ("loot"))
        {
          for (const auto& loot : r["loot"])
            {
              const std::string itemId = loot["item"].asString ();
              const int64_t qty = loot["n"].asInt64 ();

              /* Record the claim.  */
              sqlite3_prepare_v2 (db,
                "INSERT INTO `loot_claims`"
                " (`visit_id`, `name`, `item_id`, `quantity`)"
                " VALUES (?1, ?2, ?3, ?4)",
                -1, &stmt, nullptr);
              sqlite3_bind_int64 (stmt, 1, visitId);
              sqlite3_bind_text (stmt, 2, playerName.c_str (),
                                 -1, SQLITE_TRANSIENT);
              sqlite3_bind_text (stmt, 3, itemId.c_str (),
                                 -1, SQLITE_TRANSIENT);
              sqlite3_bind_int64 (stmt, 4, qty);
              sqlite3_step (stmt);
              sqlite3_finalize (stmt);

              /* Add to player inventory if under limit.  */
              if (CountInventory (db, playerName) >= MAX_INVENTORY)
                {
                  LOG (INFO) << playerName << " inventory full, dropping "
                             << itemId << " x" << qty;
                  continue;
                }

              sqlite3_prepare_v2 (db,
                "INSERT INTO `inventory`"
                " (`name`, `item_id`, `quantity`, `slot`)"
                " VALUES (?1, ?2, ?3, 'bag')",
                -1, &stmt, nullptr);
              sqlite3_bind_text (stmt, 1, playerName.c_str (),
                                 -1, SQLITE_TRANSIENT);
              sqlite3_bind_text (stmt, 2, itemId.c_str (),
                                 -1, SQLITE_TRANSIENT);
              sqlite3_bind_int64 (stmt, 3, qty);
              sqlite3_step (stmt);
              sqlite3_finalize (stmt);
            }
        }

      /* Update player stats: add gold, kills, visits_completed,
         deaths (if not survived).  */
      sqlite3_prepare_v2 (db,
        "UPDATE `players` SET"
        " `gold` = `gold` + ?2,"
        " `kills` = `kills` + ?3,"
        " `visits_completed` = `visits_completed` + 1,"
        " `deaths` = `deaths` + ?4"
        " WHERE `name` = ?1",
        -1, &stmt, nullptr);
      sqlite3_bind_text (stmt, 1, playerName.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64 (stmt, 2, goldGained);
      sqlite3_bind_int64 (stmt, 3, killsGained);
      sqlite3_bind_int64 (stmt, 4, survived ? 0 : 1);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);

      /* Apply XP and handle level-ups.
         JS logic: add xp, then while xp >= threshold: level++,
         xp -= threshold, threshold = floor(100 * pow(level+1, 1.5)),
         skillPoints++, statPoints++.  */
      if (xpGained > 0)
        {
          /* Read current xp and level.  */
          sqlite3_prepare_v2 (db,
            "SELECT `xp`, `level` FROM `players` WHERE `name` = ?1",
            -1, &stmt, nullptr);
          sqlite3_bind_text (stmt, 1, playerName.c_str (),
                             -1, SQLITE_TRANSIENT);
          sqlite3_step (stmt);
          int64_t xp = sqlite3_column_int64 (stmt, 0);
          int64_t level = sqlite3_column_int64 (stmt, 1);
          sqlite3_finalize (stmt);

          xp += xpGained;

          int levelsGained = 0;
          int64_t threshold = XpForLevel (level + 1);
          while (xp >= threshold)
            {
              xp -= threshold;
              level++;
              levelsGained++;
              threshold = XpForLevel (level + 1);
            }

          /* Write back updated xp, level, skill_points, stat_points.  */
          sqlite3_prepare_v2 (db,
            "UPDATE `players` SET"
            " `xp` = ?2, `level` = ?3,"
            " `skill_points` = `skill_points` + ?4,"
            " `stat_points` = `stat_points` + ?5"
            " WHERE `name` = ?1",
            -1, &stmt, nullptr);
          sqlite3_bind_text (stmt, 1, playerName.c_str (),
                             -1, SQLITE_TRANSIENT);
          sqlite3_bind_int64 (stmt, 2, xp);
          sqlite3_bind_int64 (stmt, 3, level);
          sqlite3_bind_int64 (stmt, 4, levelsGained);
          sqlite3_bind_int64 (stmt, 5, levelsGained);
          sqlite3_step (stmt);
          sqlite3_finalize (stmt);

          if (levelsGained > 0)
            LOG (INFO) << playerName << " leveled up " << levelsGained
                       << " time(s) to level " << level;
        }

      LOG (INFO) << "Settled " << playerName << " in visit " << visitId
                 << ": survived=" << survived
                 << " xp=" << xpGained << " gold=" << goldGained;
    }

  /* Mark visit as completed.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "UPDATE `visits`"
    " SET `status` = 'completed', `settled_height` = ?2"
    " WHERE `id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visitId);
  sqlite3_bind_int64 (stmt, 2, currentHeight);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  LOG (INFO) << "Visit " << visitId << " settled by " << name;
}

void
MoveProcessor::ProcessAllocateStat (const std::string& name,
                                     const std::string& stat)
{
  /* The stat column name is validated in HandleAllocateStat so it's safe
     to interpolate here (it's one of exactly four known strings).  */
  const std::string sql
      = "UPDATE `players` SET"
        " `" + stat + "` = `" + stat + "` + 1,"
        " `stat_points` = `stat_points` - 1"
        " WHERE `name` = ?1";

  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db, sql.c_str (), -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  /* If constitution changed, update max_hp.  If hp was at max, keep it
     at the new max.  */
  if (stat == "constitution")
    {
      sqlite3_prepare_v2 (db,
        "SELECT `constitution`, `hp`, `max_hp` FROM `players`"
        " WHERE `name` = ?1",
        -1, &stmt, nullptr);
      sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_step (stmt);
      const int64_t con = sqlite3_column_int64 (stmt, 0);
      const int64_t oldHp = sqlite3_column_int64 (stmt, 1);
      const int64_t oldMaxHp = sqlite3_column_int64 (stmt, 2);
      sqlite3_finalize (stmt);

      const int64_t newMaxHp = BASE_HP + con * HP_PER_CON;
      const int64_t newHp = (oldHp == oldMaxHp) ? newMaxHp
                            : std::min (oldHp, newMaxHp);

      sqlite3_prepare_v2 (db,
        "UPDATE `players` SET `max_hp` = ?2, `hp` = ?3"
        " WHERE `name` = ?1",
        -1, &stmt, nullptr);
      sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64 (stmt, 2, newMaxHp);
      sqlite3_bind_int64 (stmt, 3, newHp);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }

  LOG (INFO) << name << " increased " << stat << " by 1";
}

void
MoveProcessor::RecalcMaxHp (const std::string& name)
{
  /* Compute effective constitution = base + equipment bonuses.  */
  const auto stats = ComputePlayerStats (db, name);

  /* Effective con includes equipment bonuses (ComputePlayerStats adds them).  */
  const int64_t newMaxHp = BASE_HP + stats.constitution * HP_PER_CON;

  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `hp`, `max_hp` FROM `players` WHERE `name` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  const int64_t oldHp = sqlite3_column_int64 (stmt, 0);
  const int64_t oldMaxHp = sqlite3_column_int64 (stmt, 1);
  sqlite3_finalize (stmt);

  /* If HP was at max, keep it at the new max.  Otherwise clamp.  */
  const int64_t newHp = (oldHp >= oldMaxHp) ? newMaxHp
                        : std::min (oldHp, newMaxHp);

  sqlite3_prepare_v2 (db,
    "UPDATE `players` SET `max_hp` = ?2, `hp` = ?3 WHERE `name` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 2, newMaxHp);
  sqlite3_bind_int64 (stmt, 3, newHp);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);
}

void
MoveProcessor::ProcessTravel (const std::string& name,
                               const std::string& dir,
                               const std::string& txid)
{
  /* Look up destination segment.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT sl.`to_segment` FROM `segment_links` sl"
    " JOIN `players` p ON sl.`from_segment` = p.`current_segment`"
    " WHERE p.`name` = ?1 AND sl.`from_direction` = ?2",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (stmt, 2, dir.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  const int64_t destSeg = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);

  /* Random encounter seeded by txid.  */
  if (!txid.empty ())
    {
      std::mt19937 rng (HashSeed (txid + ":encounter"));
      std::uniform_int_distribution<int> chanceDist (1, 100);
      if (chanceDist (rng) <= ENCOUNTER_CHANCE)
        {
          std::uniform_int_distribution<int> dmgDist (
              ENCOUNTER_MIN_DMG, ENCOUNTER_MAX_DMG);
          const int dmg = dmgDist (rng);

          /* Apply damage, clamp to 1 (travel encounters never kill).  */
          sqlite3_prepare_v2 (db,
            "UPDATE `players` SET `hp` = MAX(1, `hp` - ?2)"
            " WHERE `name` = ?1",
            -1, &stmt, nullptr);
          sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
          sqlite3_bind_int64 (stmt, 2, dmg);
          sqlite3_step (stmt);
          sqlite3_finalize (stmt);

          LOG (INFO) << name << " encountered danger while traveling, took "
                     << dmg << " damage";
        }
    }

  /* Update current segment.  */
  sqlite3_prepare_v2 (db,
    "UPDATE `players` SET `current_segment` = ?2 WHERE `name` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 2, destSeg);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  LOG (INFO) << name << " traveled " << dir << " to segment " << destSeg;
}

void
MoveProcessor::ProcessUseItem (const std::string& name,
                                const std::string& itemId)
{
  const ItemDef* def = LookupItem (itemId);
  if (def == nullptr || !def->consumable)
    {
      LOG (WARNING) << "Unknown consumable item: " << itemId;
      return;
    }

  sqlite3_stmt* stmt;

  /* Decrement quantity.  */
  sqlite3_prepare_v2 (db,
    "UPDATE `inventory` SET `quantity` = `quantity` - 1"
    " WHERE `name` = ?1 AND `item_id` = ?2 AND `slot` = 'bag'",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (stmt, 2, itemId.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  /* Remove if quantity is 0.  */
  sqlite3_prepare_v2 (db,
    "DELETE FROM `inventory`"
    " WHERE `name` = ?1 AND `item_id` = ?2 AND `quantity` <= 0",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (stmt, 2, itemId.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  /* Apply effect.  */
  if (def->healAmount > 0)
    {
      sqlite3_prepare_v2 (db,
        "UPDATE `players` SET `hp` = MIN(`hp` + ?2, `max_hp`)"
        " WHERE `name` = ?1",
        -1, &stmt, nullptr);
      sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64 (stmt, 2, def->healAmount);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }

  LOG (INFO) << name << " used " << itemId;
}

void
MoveProcessor::ProcessEquip (const std::string& name,
                              const int64_t rowid, const std::string& slot)
{
  sqlite3_stmt* stmt;

  /* Check if there's already an item in the target slot — if so, swap.  */
  sqlite3_prepare_v2 (db,
    "SELECT `rowid` FROM `inventory`"
    " WHERE `name` = ?1 AND `slot` = ?2 LIMIT 1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (stmt, 2, slot.c_str (), -1, SQLITE_TRANSIENT);

  if (sqlite3_step (stmt) == SQLITE_ROW)
    {
      const int64_t existingRowid = sqlite3_column_int64 (stmt, 0);
      sqlite3_finalize (stmt);

      /* Move existing item to bag.  */
      sqlite3_prepare_v2 (db,
        "UPDATE `inventory` SET `slot` = 'bag' WHERE `rowid` = ?1",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, existingRowid);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }
  else
    sqlite3_finalize (stmt);

  /* Move the new item to the target slot.  */
  sqlite3_prepare_v2 (db,
    "UPDATE `inventory` SET `slot` = ?2 WHERE `rowid` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, rowid);
  sqlite3_bind_text (stmt, 2, slot.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  LOG (INFO) << name << " equipped item " << rowid << " to " << slot;

  RecalcMaxHp (name);
}

void
MoveProcessor::ProcessUnequip (const std::string& name, const int64_t rowid)
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "UPDATE `inventory` SET `slot` = 'bag' WHERE `rowid` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, rowid);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  LOG (INFO) << name << " unequipped item " << rowid << " to bag";

  RecalcMaxHp (name);
}

void
MoveProcessor::ProcessEnterChannel (const std::string& name,
                                     const int64_t segmentId)
{
  const int64_t visId = nextVisitId++;

  /* Set player as in-channel and move to the segment.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "UPDATE `players` SET `in_channel` = 1, `current_segment` = ?2"
    " WHERE `name` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 2, segmentId);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  /* Create a solo active visit.  */
  sqlite3_prepare_v2 (db,
    "INSERT INTO `visits`"
    " (`id`, `segment_id`, `initiator`, `status`,"
    "  `created_height`, `started_height`)"
    " VALUES (?1, ?2, ?3, 'active', ?4, ?4)",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visId);
  sqlite3_bind_int64 (stmt, 2, segmentId);
  sqlite3_bind_text (stmt, 3, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 4, currentHeight);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  /* Add player as sole participant.  */
  sqlite3_prepare_v2 (db,
    "INSERT INTO `visit_participants`"
    " (`visit_id`, `name`, `joined_height`)"
    " VALUES (?1, ?2, ?3)",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visId);
  sqlite3_bind_text (stmt, 2, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 3, currentHeight);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  LOG (INFO) << name << " entered channel for segment " << segmentId
             << ", visit " << visId;
}

void
MoveProcessor::ProcessExitChannel (const std::string& name,
                                    const int64_t visitId,
                                    const Json::Value& results,
                                    const Json::Value& actionsJson)
{
  /* Look up the segment seed and depth for replay.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT s.`seed`, s.`depth` FROM `visits` v"
    " JOIN `segments` s ON v.`segment_id` = s.`id`"
    " WHERE v.`id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visitId);
  sqlite3_step (stmt);
  const std::string seed = reinterpret_cast<const char*> (
      sqlite3_column_text (stmt, 0));
  const int segDepth = static_cast<int> (sqlite3_column_int64 (stmt, 1));
  sqlite3_finalize (stmt);

  /* Read player stats for replay.  */
  const auto replayStats = ComputePlayerStats (db, name);

  /* Read player HP.  */
  sqlite3_prepare_v2 (db,
    "SELECT `hp`, `max_hp` FROM `players` WHERE `name` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  const int replayHp = static_cast<int> (sqlite3_column_int64 (stmt, 0));
  const int replayMaxHp = static_cast<int> (sqlite3_column_int64 (stmt, 1));
  sqlite3_finalize (stmt);

  /* Get starting potions.  */
  const auto potions = GetPlayerPotions (db, name);
  DungeonGame::PotionList potionList;
  for (const auto& [pid, pqty] : potions)
    potionList.push_back ({pid, pqty});

  /* Parse action list from JSON.  */
  std::vector<Action> replayActions;
  for (const auto& aj : actionsJson)
    {
      Action a;
      const std::string type = aj.get ("type", "").asString ();
      if (type == "move")
        {
          a.type = Action::Type::Move;
          a.dx = aj.get ("dx", 0).asInt ();
          a.dy = aj.get ("dy", 0).asInt ();
        }
      else if (type == "pickup")
        a.type = Action::Type::Pickup;
      else if (type == "use")
        {
          a.type = Action::Type::UseItem;
          a.itemId = aj.get ("item", "").asString ();
        }
      else if (type == "gate")
        a.type = Action::Type::EnterGate;
      else if (type == "wait")
        a.type = Action::Type::Wait;
      else
        {
          LOG (WARNING) << "Unknown action type in replay: " << type;
          return;
        }
      replayActions.push_back (a);
    }

  /* Replay the actions on a fresh game.  */
  auto game = DungeonGame::Replay (seed, segDepth, replayStats,
                                    replayHp, replayMaxHp,
                                    potionList, replayActions);

  /* Verify claimed results match the replay.  If they don't match,
     reject the move entirely — the player must submit an honest proof.
     This prevents both cheating and chain bloat from garbage submissions.  */
  const bool survived = game.HasSurvived ();
  const int64_t xpGained = game.GetTotalXp ();
  const int64_t goldGained = game.GetTotalGold ();
  const int64_t killsGained = game.GetTotalKills ();
  const int64_t hpRemaining = game.GetPlayerHp ();
  const std::string exitGate = game.GetExitGate ();

  {
    const bool claimedSurvived = results.get ("survived", false).asBool ();
    const int64_t claimedXp = results.get ("xp", 0).asInt64 ();
    const int64_t claimedGold = results.get ("gold", 0).asInt64 ();
    const int64_t claimedKills = results.get ("kills", 0).asInt64 ();

    if (claimedSurvived != survived
        || claimedXp != xpGained
        || claimedGold != goldGained
        || claimedKills != killsGained)
      {
        LOG (WARNING) << "Channel exit REJECTED: claimed results do not match "
                      << "replay for " << name << " visit " << visitId
                      << ". Claimed: survived=" << claimedSurvived
                      << " xp=" << claimedXp << " gold=" << claimedGold
                      << " kills=" << claimedKills
                      << ". Replay: survived=" << survived
                      << " xp=" << xpGained << " gold=" << goldGained
                      << " kills=" << killsGained;
        return;
      }
  }

  LOG (INFO) << "Replay verified: " << replayActions.size () << " actions, "
             << "survived=" << survived << " xp=" << xpGained
             << " kills=" << killsGained;

  /* Record visit result.  */
  sqlite3_prepare_v2 (db,
    "INSERT INTO `visit_results`"
    " (`visit_id`, `name`, `survived`, `xp_gained`,"
    "  `gold_gained`, `kills`, `hp_remaining`, `exit_gate`)"
    " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visitId);
  sqlite3_bind_text (stmt, 2, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 3, survived ? 1 : 0);
  sqlite3_bind_int64 (stmt, 4, xpGained);
  sqlite3_bind_int64 (stmt, 5, goldGained);
  sqlite3_bind_int64 (stmt, 6, killsGained);
  sqlite3_bind_int64 (stmt, 7, hpRemaining);
  if (exitGate.empty ())
    sqlite3_bind_null (stmt, 8);
  else
    sqlite3_bind_text (stmt, 8, exitGate.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  /* Process loot (inventory limit enforced).  */
  if (results.isMember ("loot") && results["loot"].isArray ())
    {
      for (const auto& loot : results["loot"])
        {
          const std::string itemId = loot["item"].asString ();
          const int64_t qty = loot["n"].asInt64 ();

          /* Record the claim regardless of inventory space.  */
          sqlite3_prepare_v2 (db,
            "INSERT INTO `loot_claims`"
            " (`visit_id`, `name`, `item_id`, `quantity`)"
            " VALUES (?1, ?2, ?3, ?4)",
            -1, &stmt, nullptr);
          sqlite3_bind_int64 (stmt, 1, visitId);
          sqlite3_bind_text (stmt, 2, name.c_str (), -1, SQLITE_TRANSIENT);
          sqlite3_bind_text (stmt, 3, itemId.c_str (), -1, SQLITE_TRANSIENT);
          sqlite3_bind_int64 (stmt, 4, qty);
          sqlite3_step (stmt);
          sqlite3_finalize (stmt);

          /* Only add to inventory if under the limit.  */
          if (CountInventory (db, name) < MAX_INVENTORY)
            {
              sqlite3_prepare_v2 (db,
                "INSERT INTO `inventory`"
                " (`name`, `item_id`, `quantity`, `slot`)"
                " VALUES (?1, ?2, ?3, 'bag')",
                -1, &stmt, nullptr);
              sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
              sqlite3_bind_text (stmt, 2, itemId.c_str (), -1, SQLITE_TRANSIENT);
              sqlite3_bind_int64 (stmt, 3, qty);
              sqlite3_step (stmt);
              sqlite3_finalize (stmt);
            }
          else
            {
              LOG (INFO) << name << " inventory full, dropping "
                         << itemId << " x" << qty;
            }
        }
    }

  /* Update player stats.  */
  sqlite3_prepare_v2 (db,
    "UPDATE `players` SET"
    " `gold` = `gold` + ?2,"
    " `kills` = `kills` + ?3,"
    " `visits_completed` = `visits_completed` + 1,"
    " `deaths` = `deaths` + ?4,"
    " `hp` = ?5,"
    " `in_channel` = 0"
    " WHERE `name` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 2, goldGained);
  sqlite3_bind_int64 (stmt, 3, killsGained);
  sqlite3_bind_int64 (stmt, 4, survived ? 0 : 1);
  sqlite3_bind_int64 (stmt, 5, survived ? hpRemaining : 0);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  /* Apply XP and level-ups (reuse existing logic).  */
  if (xpGained > 0)
    {
      sqlite3_prepare_v2 (db,
        "SELECT `xp`, `level` FROM `players` WHERE `name` = ?1",
        -1, &stmt, nullptr);
      sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_step (stmt);
      int64_t xp = sqlite3_column_int64 (stmt, 0);
      int64_t level = sqlite3_column_int64 (stmt, 1);
      sqlite3_finalize (stmt);

      xp += xpGained;
      int levelsGained = 0;
      int64_t threshold = XpForLevel (level + 1);
      while (xp >= threshold)
        {
          xp -= threshold;
          level++;
          levelsGained++;
          threshold = XpForLevel (level + 1);
        }

      sqlite3_prepare_v2 (db,
        "UPDATE `players` SET"
        " `xp` = ?2, `level` = ?3,"
        " `skill_points` = `skill_points` + ?4,"
        " `stat_points` = `stat_points` + ?5"
        " WHERE `name` = ?1",
        -1, &stmt, nullptr);
      sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64 (stmt, 2, xp);
      sqlite3_bind_int64 (stmt, 3, level);
      sqlite3_bind_int64 (stmt, 4, levelsGained);
      sqlite3_bind_int64 (stmt, 5, levelsGained);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }

  /* Update position based on exit gate.  */
  if (survived && !exitGate.empty ())
    {
      /* Look up the visit's segment.  */
      sqlite3_prepare_v2 (db,
        "SELECT `segment_id` FROM `visits` WHERE `id` = ?1",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, visitId);
      sqlite3_step (stmt);
      const int64_t visitSeg = sqlite3_column_int64 (stmt, 0);
      sqlite3_finalize (stmt);

      /* Look up linked segment via exit gate direction.  */
      sqlite3_prepare_v2 (db,
        "SELECT `to_segment` FROM `segment_links`"
        " WHERE `from_segment` = ?1 AND `from_direction` = ?2",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, visitSeg);
      sqlite3_bind_text (stmt, 2, exitGate.c_str (), -1, SQLITE_TRANSIENT);

      if (sqlite3_step (stmt) == SQLITE_ROW)
        {
          const int64_t destSeg = sqlite3_column_int64 (stmt, 0);
          sqlite3_finalize (stmt);

          sqlite3_prepare_v2 (db,
            "UPDATE `players` SET `current_segment` = ?2"
            " WHERE `name` = ?1",
            -1, &stmt, nullptr);
          sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
          sqlite3_bind_int64 (stmt, 2, destSeg);
          sqlite3_step (stmt);
          sqlite3_finalize (stmt);
        }
      else
        sqlite3_finalize (stmt);
    }

  /* Mark visit as completed.  */
  sqlite3_prepare_v2 (db,
    "UPDATE `visits`"
    " SET `status` = 'completed', `settled_height` = ?2"
    " WHERE `id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visitId);
  sqlite3_bind_int64 (stmt, 2, currentHeight);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  /* Confirm the segment (provisional → permanent) now that a valid
     run has been completed.  This makes the segment accessible to
     other players for travel and visits.  */
  sqlite3_prepare_v2 (db,
    "UPDATE `segments` SET `confirmed` = 1"
    " WHERE `id` = (SELECT `segment_id` FROM `visits` WHERE `id` = ?1)"
    " AND `confirmed` = 0",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visitId);
  sqlite3_step (stmt);
  if (sqlite3_changes (db) > 0)
    LOG (INFO) << "Segment confirmed after valid run in visit " << visitId;
  sqlite3_finalize (stmt);

  LOG (INFO) << "Channel exit: " << name << " visit " << visitId
             << " survived=" << survived << " xp=" << xpGained
             << " gate=" << exitGate;
}

void
MoveProcessor::ProcessAll (const Json::Value& moves)
{
  if (!moves.isArray ())
    return;

  LOG_IF (INFO, !moves.empty ())
      << "Processing " << moves.size () << " moves...";

  for (const auto& mv : moves)
    ProcessOne (mv);

  ProcessTimeouts ();
}

void
MoveProcessor::ProcessTimeouts ()
{
  /* Expire open visits that have been waiting too long for players.  */
  {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2 (db,
      "UPDATE `visits` SET `status` = 'expired'"
      " WHERE `status` = 'open'"
      " AND `created_height` + ?1 <= ?2",
      -1, &stmt, nullptr);
    sqlite3_bind_int64 (stmt, 1, VISIT_OPEN_TIMEOUT);
    sqlite3_bind_int64 (stmt, 2, currentHeight);
    sqlite3_step (stmt);
    const int changed = sqlite3_changes (db);
    sqlite3_finalize (stmt);

    if (changed > 0)
      LOG (INFO) << "Expired " << changed << " open visit(s) at height "
                 << currentHeight;
  }

  /* Force-settle active visits that have exceeded the active timeout.
     All participants get survived=false, no rewards.  */
  {
    sqlite3_stmt* query;
    sqlite3_prepare_v2 (db,
      "SELECT `id` FROM `visits`"
      " WHERE `status` = 'active'"
      " AND `started_height` + ?1 <= ?2",
      -1, &query, nullptr);
    sqlite3_bind_int64 (query, 1, VISIT_ACTIVE_TIMEOUT);
    sqlite3_bind_int64 (query, 2, currentHeight);

    std::vector<int64_t> timedOut;
    while (sqlite3_step (query) == SQLITE_ROW)
      timedOut.push_back (sqlite3_column_int64 (query, 0));
    sqlite3_finalize (query);

    for (const auto visId : timedOut)
      {
        /* Record failure results for all participants.  */
        sqlite3_stmt* pQuery;
        sqlite3_prepare_v2 (db,
          "SELECT `name` FROM `visit_participants`"
          " WHERE `visit_id` = ?1",
          -1, &pQuery, nullptr);
        sqlite3_bind_int64 (pQuery, 1, visId);

        while (sqlite3_step (pQuery) == SQLITE_ROW)
          {
            const char* pName
                = reinterpret_cast<const char*> (sqlite3_column_text (pQuery, 0));

            sqlite3_stmt* ins;
            sqlite3_prepare_v2 (db,
              "INSERT INTO `visit_results`"
              " (`visit_id`, `name`, `survived`, `xp_gained`,"
              "  `gold_gained`, `kills`)"
              " VALUES (?1, ?2, 0, 0, 0, 0)",
              -1, &ins, nullptr);
            sqlite3_bind_int64 (ins, 1, visId);
            sqlite3_bind_text (ins, 2, pName, -1, SQLITE_TRANSIENT);
            sqlite3_step (ins);
            sqlite3_finalize (ins);

            /* Increment death count and clear channel flag.  */
            sqlite3_prepare_v2 (db,
              "UPDATE `players` SET"
              " `deaths` = `deaths` + 1,"
              " `visits_completed` = `visits_completed` + 1,"
              " `in_channel` = 0, `hp` = 0"
              " WHERE `name` = ?1",
              -1, &ins, nullptr);
            sqlite3_bind_text (ins, 1, pName, -1, SQLITE_TRANSIENT);
            sqlite3_step (ins);
            sqlite3_finalize (ins);
          }
        sqlite3_finalize (pQuery);

        /* Mark visit as completed.  */
        sqlite3_stmt* upd;
        sqlite3_prepare_v2 (db,
          "UPDATE `visits`"
          " SET `status` = 'completed', `settled_height` = ?2"
          " WHERE `id` = ?1",
          -1, &upd, nullptr);
        sqlite3_bind_int64 (upd, 1, visId);
        sqlite3_bind_int64 (upd, 2, currentHeight);
        sqlite3_step (upd);
        sqlite3_finalize (upd);

        LOG (INFO) << "Force-settled visit " << visId
                   << " due to active timeout at height " << currentHeight;
      }
  }

  /* Prune provisional segments that were never confirmed.
     A segment is prunable if:
     - confirmed = 0 (provisional)
     - No open or active visits exist for it
     - It was created more than VISIT_OPEN_TIMEOUT + SOLO_VISIT_ACTIVE_TIMEOUT
       blocks ago (enough time for discovery + channel completion).  */
  {
    const unsigned pruneAge = VISIT_OPEN_TIMEOUT + SOLO_VISIT_ACTIVE_TIMEOUT;
    sqlite3_stmt* query;
    sqlite3_prepare_v2 (db,
      "SELECT `id` FROM `segments`"
      " WHERE `confirmed` = 0"
      " AND `created_height` + ?1 <= ?2"
      " AND `id` NOT IN"
      "   (SELECT `segment_id` FROM `visits`"
      "    WHERE `status` IN ('open', 'active'))",
      -1, &query, nullptr);
    sqlite3_bind_int64 (query, 1, pruneAge);
    sqlite3_bind_int64 (query, 2, currentHeight);

    std::vector<int64_t> toPrune;
    while (sqlite3_step (query) == SQLITE_ROW)
      toPrune.push_back (sqlite3_column_int64 (query, 0));
    sqlite3_finalize (query);

    for (const auto segId : toPrune)
      {
        sqlite3_stmt* del;

        /* Delete links.  */
        sqlite3_prepare_v2 (db,
          "DELETE FROM `segment_links` WHERE `from_segment` = ?1"
          " OR `to_segment` = ?1",
          -1, &del, nullptr);
        sqlite3_bind_int64 (del, 1, segId);
        sqlite3_step (del);
        sqlite3_finalize (del);

        /* Delete gates.  */
        sqlite3_prepare_v2 (db,
          "DELETE FROM `segment_gates` WHERE `segment_id` = ?1",
          -1, &del, nullptr);
        sqlite3_bind_int64 (del, 1, segId);
        sqlite3_step (del);
        sqlite3_finalize (del);

        /* Delete segment.  */
        sqlite3_prepare_v2 (db,
          "DELETE FROM `segments` WHERE `id` = ?1",
          -1, &del, nullptr);
        sqlite3_bind_int64 (del, 1, segId);
        sqlite3_step (del);
        sqlite3_finalize (del);

        LOG (INFO) << "Pruned provisional segment " << segId;
      }
  }
}

} // namespace rog
