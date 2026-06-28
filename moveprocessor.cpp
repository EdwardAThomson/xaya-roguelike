#include "moveprocessor.hpp"
#include "dungeon.hpp"
#include "dungeongame.hpp"
#include "hash.hpp"
#include "items.hpp"

#include <glog/logging.h>

#include <algorithm>
#include <cmath>
#include <map>
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

  /* Compute world coordinates for the new segment.
     Source segment position + direction offset.  */
  int worldX = 0, worldY = 0;
  if (!dir.empty ())
    {
      /* Look up source segment's position.  */
      sqlite3_stmt* posStmt;
      sqlite3_prepare_v2 (db,
        "SELECT `current_segment` FROM `players` WHERE `name` = ?1",
        -1, &posStmt, nullptr);
      sqlite3_bind_text (posStmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_step (posStmt);
      const int64_t srcSeg = sqlite3_column_int64 (posStmt, 0);
      sqlite3_finalize (posStmt);

      /* Source segment 0 (origin) is at (0,0).  Others have stored coords.  */
      if (srcSeg != 0)
        {
          sqlite3_prepare_v2 (db,
            "SELECT `world_x`, `world_y` FROM `segments` WHERE `id` = ?1",
            -1, &posStmt, nullptr);
          sqlite3_bind_int64 (posStmt, 1, srcSeg);
          sqlite3_step (posStmt);
          worldX = static_cast<int> (sqlite3_column_int64 (posStmt, 0));
          worldY = static_cast<int> (sqlite3_column_int64 (posStmt, 1));
          sqlite3_finalize (posStmt);
        }

      /* Apply direction offset.
         north = +Y, south = -Y, east = +X, west = -X.  */
      if (dir == "north") worldY += 1;
      else if (dir == "south") worldY -= 1;
      else if (dir == "east") worldX += 1;
      else if (dir == "west") worldX -= 1;
    }

  /* Create permanent segment with world coordinates.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "INSERT INTO `segments`"
    " (`id`, `discoverer`, `seed`, `depth`, `created_height`,"
    "  `world_x`, `world_y`)"
    " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, segId);
  sqlite3_bind_text (stmt, 2, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (stmt, 3, seed.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 4, depth);
  sqlite3_bind_int64 (stmt, 5, currentHeight);
  sqlite3_bind_int64 (stmt, 6, worldX);
  sqlite3_bind_int64 (stmt, 7, worldY);
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

      /* Record which gate is the alignment constraint so the replay and
         frontend can regenerate this exact constrained layout.  Only set
         it when a constraint was actually applied (empty when discovered
         from the hub, which has no gates).  */
      if (!constraints.empty ())
        {
          sqlite3_prepare_v2 (db,
            "UPDATE `segments` SET `constraint_dir` = ?2 WHERE `id` = ?1",
            -1, &stmt, nullptr);
          sqlite3_bind_int64 (stmt, 1, segId);
          sqlite3_bind_text (stmt, 2, oppositeDir.c_str (), -1,
                             SQLITE_TRANSIENT);
          sqlite3_step (stmt);
          sqlite3_finalize (stmt);
        }
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
  const bool hasLink = (sqlite3_step (stmt) == SQLITE_ROW);
  const int64_t destSeg = hasLink ? sqlite3_column_int64 (stmt, 0) : -1;
  sqlite3_finalize (stmt);

  /* No link in that direction: reject the move rather than silently
     reading column 0 (which would teleport the player to the hub).  */
  if (!hasLink)
    {
      LOG (WARNING) << name << " tried to travel " << dir
                    << " but there is no linked segment in that direction";
      return;
    }

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
MoveProcessor::ProcessDiscardItem (const std::string& name, const int64_t rowid)
{
  /* Permanently destroy the bag row.  HandleDiscard already verified the
     row belongs to the player and is in the bag, so no stat recalc is
     needed (equipped gear can't be discarded directly).  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "DELETE FROM `inventory` WHERE `rowid` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, rowid);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  LOG (INFO) << name << " discarded item " << rowid;
}

void
MoveProcessor::ProcessEnterChannel (const std::string& name,
                                     const int64_t segmentId,
                                     const std::string& entryDir)
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

  /* Create a solo active visit.  `entry_direction` records the gate the
     player came in through (empty -> NULL -> centre spawn) so the replay
     and frontend spawn at the same tile.  */
  sqlite3_prepare_v2 (db,
    "INSERT INTO `visits`"
    " (`id`, `segment_id`, `initiator`, `status`,"
    "  `created_height`, `started_height`, `entry_direction`)"
    " VALUES (?1, ?2, ?3, 'active', ?4, ?4, ?5)",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visId);
  sqlite3_bind_int64 (stmt, 2, segmentId);
  sqlite3_bind_text (stmt, 3, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 4, currentHeight);
  if (entryDir.empty ())
    sqlite3_bind_null (stmt, 5);
  else
    sqlite3_bind_text (stmt, 5, entryDir.c_str (), -1, SQLITE_TRANSIENT);
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

std::optional<std::string>
MoveProcessor::ApplySettlementBody (const std::string& name,
                                     const int64_t visitId,
                                     const Json::Value& results,
                                     const Json::Value& actionsJson)
{
  /* Look up the segment seed/depth plus the layout-reconstruction inputs:
     the visit's entry gate (for spawn) and the segment's alignment
     constraint direction (so we regenerate the same constrained layout).  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT s.`seed`, s.`depth`, s.`id`,"
    "       v.`entry_direction`, s.`constraint_dir`"
    " FROM `visits` v"
    " JOIN `segments` s ON v.`segment_id` = s.`id`"
    " WHERE v.`id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visitId);
  sqlite3_step (stmt);
  const std::string seed = reinterpret_cast<const char*> (
      sqlite3_column_text (stmt, 0));
  const int segDepth = static_cast<int> (sqlite3_column_int64 (stmt, 1));
  const int64_t segId = sqlite3_column_int64 (stmt, 2);
  const char* entryDirRaw
      = reinterpret_cast<const char*> (sqlite3_column_text (stmt, 3));
  const std::string entryDir = entryDirRaw ? entryDirRaw : "";
  const char* constraintDirRaw
      = reinterpret_cast<const char*> (sqlite3_column_text (stmt, 4));
  const std::string constraintDir = constraintDirRaw ? constraintDirRaw : "";
  sqlite3_finalize (stmt);

  /* Rebuild the gate constraint (if any) from the stored gate position so
     replay regenerates the exact layout the player played.  */
  std::vector<Gate> constraints;
  if (!constraintDir.empty ())
    {
      sqlite3_prepare_v2 (db,
        "SELECT `x`, `y` FROM `segment_gates`"
        " WHERE `segment_id` = ?1 AND `direction` = ?2",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, segId);
      sqlite3_bind_text (stmt, 2, constraintDir.c_str (), -1, SQLITE_TRANSIENT);
      if (sqlite3_step (stmt) == SQLITE_ROW)
        {
          Gate g;
          g.x = static_cast<int> (sqlite3_column_int64 (stmt, 0));
          g.y = static_cast<int> (sqlite3_column_int64 (stmt, 1));
          g.direction = constraintDir;
          constraints.push_back (g);
        }
      sqlite3_finalize (stmt);
    }

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
          return std::nullopt;
        }
      replayActions.push_back (a);
    }

  /* Replay the actions on a fresh game — same constrained layout and entry
     spawn the player actually used.  */
  auto game = DungeonGame::Replay (seed, segDepth, replayStats,
                                    replayHp, replayMaxHp,
                                    potionList, replayActions,
                                    constraints, entryDir);

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
        return std::nullopt;
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

  /* Apply the run's net inventory change, computed from the REPLAY (never
     trusted from the client, so fabricated loot is impossible).  The
     dungeon's collected-loot list is seeded with the player's starting
     potions, so the net delta = collected - starting = items picked up
     minus potions drunk.  Applied only on a surviving exit; a death or
     forfeit discards finds and keeps potions (the run is rolled back for
     the inventory).  Gold/XP/kills are handled separately below.  */
  if (survived)
    {
      std::map<std::string, int> delta;
      for (const auto& [pid, pqty] : potions)
        delta[pid] -= pqty;
      for (const auto& c : game.GetLoot ())
        delta[c.itemId] += c.quantity;

      for (const auto& [itemId, n] : delta)
        {
          if (n > 0)
            {
              /* Record the claim and add the find(s).  Stackable items
                 merge into an existing bag stack; non-stackable gear goes
                 in as separate rows.  Inventory cap is per-row.  */
              sqlite3_prepare_v2 (db,
                "INSERT INTO `loot_claims`"
                " (`visit_id`, `name`, `item_id`, `quantity`)"
                " VALUES (?1, ?2, ?3, ?4)",
                -1, &stmt, nullptr);
              sqlite3_bind_int64 (stmt, 1, visitId);
              sqlite3_bind_text (stmt, 2, name.c_str (), -1, SQLITE_TRANSIENT);
              sqlite3_bind_text (stmt, 3, itemId.c_str (), -1, SQLITE_TRANSIENT);
              sqlite3_bind_int64 (stmt, 4, n);
              sqlite3_step (stmt);
              sqlite3_finalize (stmt);

              const ItemDef* def = LookupItem (itemId);
              const bool stackable = def != nullptr && def->stackable;
              if (stackable)
                {
                  sqlite3_prepare_v2 (db,
                    "UPDATE `inventory` SET `quantity` = `quantity` + ?3"
                    " WHERE `name` = ?1 AND `item_id` = ?2 AND `slot` = 'bag'",
                    -1, &stmt, nullptr);
                  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
                  sqlite3_bind_text (stmt, 2, itemId.c_str (), -1, SQLITE_TRANSIENT);
                  sqlite3_bind_int64 (stmt, 3, n);
                  sqlite3_step (stmt);
                  const bool merged = sqlite3_changes (db) > 0;
                  sqlite3_finalize (stmt);

                  if (!merged && CountInventory (db, name) < MAX_INVENTORY)
                    {
                      sqlite3_prepare_v2 (db,
                        "INSERT INTO `inventory`"
                        " (`name`, `item_id`, `quantity`, `slot`)"
                        " VALUES (?1, ?2, ?3, 'bag')",
                        -1, &stmt, nullptr);
                      sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
                      sqlite3_bind_text (stmt, 2, itemId.c_str (), -1, SQLITE_TRANSIENT);
                      sqlite3_bind_int64 (stmt, 3, n);
                      sqlite3_step (stmt);
                      sqlite3_finalize (stmt);
                    }
                  else if (!merged)
                    LOG (INFO) << name << " inventory full, dropping "
                               << itemId << " x" << n;
                }
              else
                {
                  for (int k = 0; k < n; k++)
                    {
                      if (CountInventory (db, name) >= MAX_INVENTORY)
                        {
                          LOG (INFO) << name << " inventory full, dropping "
                                     << itemId;
                          break;
                        }
                      sqlite3_prepare_v2 (db,
                        "INSERT INTO `inventory`"
                        " (`name`, `item_id`, `quantity`, `slot`)"
                        " VALUES (?1, ?2, 1, 'bag')",
                        -1, &stmt, nullptr);
                      sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
                      sqlite3_bind_text (stmt, 2, itemId.c_str (), -1, SQLITE_TRANSIENT);
                      sqlite3_step (stmt);
                      sqlite3_finalize (stmt);
                    }
                }
            }
          else if (n < 0)
            {
              /* Potions drunk during the run: deduct from the bag stack.  */
              sqlite3_prepare_v2 (db,
                "UPDATE `inventory` SET `quantity` = `quantity` - ?3"
                " WHERE `name` = ?1 AND `item_id` = ?2 AND `slot` = 'bag'",
                -1, &stmt, nullptr);
              sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
              sqlite3_bind_text (stmt, 2, itemId.c_str (), -1, SQLITE_TRANSIENT);
              sqlite3_bind_int64 (stmt, 3, -n);
              sqlite3_step (stmt);
              sqlite3_finalize (stmt);

              sqlite3_prepare_v2 (db,
                "DELETE FROM `inventory`"
                " WHERE `name` = ?1 AND `item_id` = ?2 AND `slot` = 'bag'"
                " AND `quantity` <= 0",
                -1, &stmt, nullptr);
              sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
              sqlite3_bind_text (stmt, 2, itemId.c_str (), -1, SQLITE_TRANSIENT);
              sqlite3_step (stmt);
              sqlite3_finalize (stmt);
            }
        }
    }

  /* Update player stats.  On death, apply the death penalty: the player
     respawns at the hub (segment 0) with half HP and loses 25% of their
     carried gold (computed AFTER crediting any gold earned during the
     run).  Integer division floors, so a player with 3 gold who dies ends
     up with 2.  Respawn HP is floored at 1 so the player is never left at
     0 HP, which would block all gate-walks (see HandleGateWalk).  */
  sqlite3_prepare_v2 (db,
    "UPDATE `players` SET"
    " `gold` = CASE WHEN ?6 THEN `gold` + ?2"
    "              ELSE ((`gold` + ?2) * 75) / 100 END,"
    " `kills` = `kills` + ?3,"
    " `visits_completed` = `visits_completed` + 1,"
    " `deaths` = `deaths` + ?4,"
    " `hp` = CASE WHEN ?6 THEN ?5 ELSE MAX(`max_hp` / 2, 1) END,"
    " `in_channel` = 0,"
    " `current_segment` = CASE WHEN ?6 THEN `current_segment` ELSE 0 END"
    " WHERE `name` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 2, goldGained);
  sqlite3_bind_int64 (stmt, 3, killsGained);
  sqlite3_bind_int64 (stmt, 4, survived ? 0 : 1);
  sqlite3_bind_int64 (stmt, 5, survived ? hpRemaining : 0);
  sqlite3_bind_int64 (stmt, 6, survived ? 1 : 0);
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

  /* Look up the segment id for this visit — we'll need it for either
     the confirm-on-survival or prune-on-failure branch below.  */
  int64_t visitSegId = 0;
  {
    sqlite3_stmt* segQuery;
    sqlite3_prepare_v2 (db,
      "SELECT `segment_id` FROM `visits` WHERE `id` = ?1",
      -1, &segQuery, nullptr);
    sqlite3_bind_int64 (segQuery, 1, visitId);
    if (sqlite3_step (segQuery) == SQLITE_ROW)
      visitSegId = sqlite3_column_int64 (segQuery, 0);
    sqlite3_finalize (segQuery);
  }

  if (survived)
    {
      /* Confirm the segment (provisional → permanent) now that a valid
         run has been completed.  Makes it accessible for others.  */
      sqlite3_prepare_v2 (db,
        "UPDATE `segments` SET `confirmed` = 1"
        " WHERE `id` = ?1 AND `confirmed` = 0",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, visitSegId);
      sqlite3_step (stmt);
      if (sqlite3_changes (db) > 0)
        LOG (INFO) << "Segment " << visitSegId
                   << " confirmed after valid run in visit " << visitId;
      sqlite3_finalize (stmt);
    }
  else
    {
      /* Failed run on a provisional segment: free the world coord so
         the discoverer can't perpetually re-enter to hold it hostage
         (would otherwise need to wait ~300 blocks for the time-based
         pruner).  Confirmed segments are unaffected by this call.  */
      PruneProvisionalSegment (visitSegId);
    }

  LOG (INFO) << "Channel exit: " << name << " visit " << visitId
             << " survived=" << survived << " xp=" << xpGained
             << " gate=" << exitGate;

  return exitGate;
}

void
MoveProcessor::PruneProvisionalSegment (const int64_t segId)
{
  /* Guard: only delete provisional segments.  Confirmed segments
     persist forever.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `confirmed` FROM `segments` WHERE `id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, segId);
  if (sqlite3_step (stmt) != SQLITE_ROW)
    {
      sqlite3_finalize (stmt);
      return;  /* already gone */
    }
  const bool confirmed = sqlite3_column_int64 (stmt, 0) != 0;
  sqlite3_finalize (stmt);

  if (confirmed) return;

  /* Delete links (both directions).  */
  sqlite3_prepare_v2 (db,
    "DELETE FROM `segment_links`"
    " WHERE `from_segment` = ?1 OR `to_segment` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, segId);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  /* Delete gates.  */
  sqlite3_prepare_v2 (db,
    "DELETE FROM `segment_gates` WHERE `segment_id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, segId);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  /* Delete segment row itself.  */
  sqlite3_prepare_v2 (db,
    "DELETE FROM `segments` WHERE `id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, segId);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  LOG (INFO) << "Pruned provisional segment " << segId;
}

void
MoveProcessor::UpdatePositionFromExitGate (const std::string& name,
                                            const int64_t visitSeg,
                                            const std::string& exitGate)
{
  if (exitGate.empty ()) return;

  sqlite3_stmt* stmt;
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
        "UPDATE `players` SET `current_segment` = ?2 WHERE `name` = ?1",
        -1, &stmt, nullptr);
      sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64 (stmt, 2, destSeg);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }
  else
    sqlite3_finalize (stmt);
}

void
MoveProcessor::ProcessExitChannel (const std::string& name,
                                    const int64_t visitId,
                                    const Json::Value& results,
                                    const Json::Value& actionsJson)
{
  /* Capture the visit's segment before settling so we can look up its
     exit gate's linked neighbour afterwards (the helper marks the visit
     completed, but the row still exists).  */
  int64_t visitSeg = 0;
  {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2 (db,
      "SELECT `segment_id` FROM `visits` WHERE `id` = ?1",
      -1, &stmt, nullptr);
    sqlite3_bind_int64 (stmt, 1, visitId);
    if (sqlite3_step (stmt) == SQLITE_ROW)
      visitSeg = sqlite3_column_int64 (stmt, 0);
    sqlite3_finalize (stmt);
  }

  const auto exitGate = ApplySettlementBody (name, visitId, results, actionsJson);
  if (!exitGate.has_value ()) return;
  UpdatePositionFromExitGate (name, visitSeg, *exitGate);
}

/* ----------------------------------------------------------------
   Atomic gate-walk.

   Dispatch table (current state x target status):

     in channel  + confirmed neighbour : settle -> in_channel @ B
     in channel  + own provisional     : settle -> in_channel @ B
     in channel  + unexplored          : settle -> discover B -> in_channel @ B
     in channel  + hub neighbour       : settle -> overworld @ hub
     not in chan + confirmed neighbour : in_channel @ B
     not in chan + own provisional     : in_channel @ B
     not in chan + unexplored          : discover B -> in_channel @ B
     not in chan + hub neighbour       : overworld @ hub

   HandleGateWalk has already validated cooldown, coord-occupancy, and
   discoverer-privilege before we get here.
   ---------------------------------------------------------------- */
void
MoveProcessor::ProcessGateWalk (const std::string& name,
                                 const std::string& txid,
                                 const std::string& dir,
                                 const Json::Value& settlement)
{
  /* Snapshot the source segment before any settlement so we can route
     from it regardless of what death-penalty (etc.) logic may do.
     We do not allow settlement.survived=false in gw (HandleGateWalk
     enforces this), so current_segment shouldn't actually move on us.  */
  int64_t srcSeg = 0;
  {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2 (db,
      "SELECT `current_segment` FROM `players` WHERE `name` = ?1",
      -1, &stmt, nullptr);
    sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_step (stmt);
    srcSeg = sqlite3_column_int64 (stmt, 0);
    sqlite3_finalize (stmt);
  }

  /* 1. Settle the current dungeon (if any) and verify replay.  */
  if (!settlement.isNull ())
    {
      /* Look up player's active visit id.  */
      int64_t visitId = -1;
      sqlite3_stmt* stmt;
      sqlite3_prepare_v2 (db,
        "SELECT v.`id` FROM `visits` v"
        " JOIN `visit_participants` p ON v.`id` = p.`visit_id`"
        " WHERE v.`status` = 'active' AND p.`name` = ?1",
        -1, &stmt, nullptr);
      sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
      if (sqlite3_step (stmt) == SQLITE_ROW)
        visitId = sqlite3_column_int64 (stmt, 0);
      sqlite3_finalize (stmt);

      if (visitId < 0)
        {
          LOG (WARNING) << name << " gate-walk: no active visit to settle";
          return;
        }

      const auto exitGate = ApplySettlementBody (
          name, visitId, settlement["results"], settlement["actions"]);
      if (!exitGate.has_value ())
        return;  /* replay rejected — entire gw aborts */

      /* Verify the replay's exit gate matches the claimed direction.
         A mismatch means the player walked through a different gate
         than gw.dir claims — likely an intentional fudge.  Reject so
         the player must submit a consistent move.  Note: ApplySettlementBody
         has already mutated state at this point.  We cannot truly roll
         back, but we can refuse to take the *next* step (no transit,
         no enter-channel).  The player ends up out-of-channel at their
         original segment (since survived=true means current_segment was
         not changed by the settlement body).  */
      if (*exitGate != dir)
        {
          LOG (WARNING) << name << " gate-walk: replay's exit gate '"
                        << *exitGate << "' does not match claimed dir '"
                        << dir << "'.  Settlement applied; transit aborted.";
          return;
        }
    }
  else
    {
      /* Transit-only free pass: HandleGateWalk has already verified the
         source segment is confirmed.  End any active visit with no
         settlement — no rewards, no penalty, no prune — then transit.
         (A gate-walk that started from the hub/overworld has no active
         visit, so this is a no-op there.)  */
      sqlite3_stmt* stmt;
      int64_t visitId = -1;
      sqlite3_prepare_v2 (db,
        "SELECT v.`id` FROM `visits` v"
        " JOIN `visit_participants` p ON v.`id` = p.`visit_id`"
        " WHERE v.`status` = 'active' AND p.`name` = ?1",
        -1, &stmt, nullptr);
      sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
      if (sqlite3_step (stmt) == SQLITE_ROW)
        visitId = sqlite3_column_int64 (stmt, 0);
      sqlite3_finalize (stmt);

      if (visitId >= 0)
        {
          sqlite3_prepare_v2 (db,
            "UPDATE `visits` SET `status` = 'completed' WHERE `id` = ?1",
            -1, &stmt, nullptr);
          sqlite3_bind_int64 (stmt, 1, visitId);
          sqlite3_step (stmt);
          sqlite3_finalize (stmt);
          LOG (INFO) << name << " transit gate-walk: left confirmed segment "
                     << srcSeg << " (visit " << visitId
                     << " ended, no rewards)";
        }
    }

  /* 2. Determine target segment from srcSeg in dir.  */
  int64_t targetSeg = -1;
  bool targetExists = false;
  {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2 (db,
      "SELECT `to_segment` FROM `segment_links`"
      " WHERE `from_segment` = ?1 AND `from_direction` = ?2",
      -1, &stmt, nullptr);
    sqlite3_bind_int64 (stmt, 1, srcSeg);
    sqlite3_bind_text (stmt, 2, dir.c_str (), -1, SQLITE_TRANSIENT);
    if (sqlite3_step (stmt) == SQLITE_ROW)
      {
        targetSeg = sqlite3_column_int64 (stmt, 0);
        targetExists = true;
      }
    sqlite3_finalize (stmt);
  }

  /* 3. Discover if target doesn't exist.  ProcessDiscover reads
     current_segment from the players row; ApplySettlementBody preserved
     it (we required survived=true).  */
  if (!targetExists)
    {
      /* Capture the segId BEFORE ProcessDiscover increments the counter
         so we know which segment we'll end up entering.  */
      const int64_t newSegId = nextSegmentId;

      /* Depth of the new segment: source depth + 1, or 1 from hub.  */
      int srcDepth = 0;
      if (srcSeg != 0)
        {
          sqlite3_stmt* stmt;
          sqlite3_prepare_v2 (db,
            "SELECT `depth` FROM `segments` WHERE `id` = ?1",
            -1, &stmt, nullptr);
          sqlite3_bind_int64 (stmt, 1, srcSeg);
          if (sqlite3_step (stmt) == SQLITE_ROW)
            srcDepth = static_cast<int> (sqlite3_column_int64 (stmt, 0));
          sqlite3_finalize (stmt);
        }
      const int newDepth = srcDepth + 1;

      ProcessDiscover (name, newDepth, txid, dir);
      targetSeg = newSegId;
    }

  /* 4. If target is the hub (segment 0), drop the player on the
     overworld at the hub — no new channel.  */
  if (targetSeg == 0)
    {
      sqlite3_stmt* stmt;
      sqlite3_prepare_v2 (db,
        "UPDATE `players` SET `current_segment` = 0, `in_channel` = 0"
        " WHERE `name` = ?1",
        -1, &stmt, nullptr);
      sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
      LOG (INFO) << name << " gate-walked to hub from segment " << srcSeg;
      return;
    }

  /* 5. Enter the target's channel.  The player comes in through the gate
     on the opposite wall, so they spawn there.  */
  std::string entryDir;
  if (dir == "north") entryDir = "south";
  else if (dir == "south") entryDir = "north";
  else if (dir == "east") entryDir = "west";
  else if (dir == "west") entryDir = "east";
  ProcessEnterChannel (name, targetSeg, entryDir);
  LOG (INFO) << name << " gate-walked " << dir
             << " from " << srcSeg << " to " << targetSeg;
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
     Solo visits (1 participant) use SOLO_VISIT_ACTIVE_TIMEOUT;
     multiplayer visits use VISIT_ACTIVE_TIMEOUT.
     All participants get survived=false, no rewards.  */
  {
    sqlite3_stmt* query;
    sqlite3_prepare_v2 (db,
      "SELECT v.`id` FROM `visits` v"
      " WHERE v.`status` = 'active'"
      " AND v.`started_height` + "
      "   CASE WHEN (SELECT COUNT(*) FROM `visit_participants`"
      "              WHERE `visit_id` = v.`id`) <= 1"
      "        THEN ?1 ELSE ?3 END"
      " <= ?2",
      -1, &query, nullptr);
    sqlite3_bind_int64 (query, 1, SOLO_VISIT_ACTIVE_TIMEOUT);
    sqlite3_bind_int64 (query, 2, currentHeight);
    sqlite3_bind_int64 (query, 3, VISIT_ACTIVE_TIMEOUT);

    std::vector<int64_t> timedOut;
    while (sqlite3_step (query) == SQLITE_ROW)
      timedOut.push_back (sqlite3_column_int64 (query, 0));
    sqlite3_finalize (query);

    for (const auto visId : timedOut)
      {
        /* Look up the visit's segment id; used after the participant
           updates to prune it if still provisional.  */
        int64_t visSegId = 0;
        {
          sqlite3_stmt* segQuery;
          sqlite3_prepare_v2 (db,
            "SELECT `segment_id` FROM `visits` WHERE `id` = ?1",
            -1, &segQuery, nullptr);
          sqlite3_bind_int64 (segQuery, 1, visId);
          if (sqlite3_step (segQuery) == SQLITE_ROW)
            visSegId = sqlite3_column_int64 (segQuery, 0);
          sqlite3_finalize (segQuery);
        }

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

            /* Increment death count, clear channel, and apply the death
               penalty (respawn at hub with half HP, lose 25% gold) — same
               as the voluntary ProcessExitChannel death path.  */
            sqlite3_prepare_v2 (db,
              "UPDATE `players` SET"
              " `deaths` = `deaths` + 1,"
              " `visits_completed` = `visits_completed` + 1,"
              " `in_channel` = 0, `hp` = MAX(`max_hp` / 2, 1),"
              " `gold` = (`gold` * 75) / 100,"
              " `current_segment` = 0"
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

        /* Same anti-grief rule as voluntary survived=false: an
           abandoned provisional segment is freed immediately so its
           world coord is available again.  */
        PruneProvisionalSegment (visSegId);

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
      PruneProvisionalSegment (segId);
  }
}

} // namespace rog
