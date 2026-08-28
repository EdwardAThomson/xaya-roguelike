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
 * XP required to reach a given level.
 *
 * Early-game easing: the curve was softened from the original
 * floor(100 * pow(level, 1.5)) to a gentler coefficient and exponent so a
 * fresh level-1 character reaches levels 2-4 with noticeably less grinding
 * (level 2: 282 -> 152, level 3: 519 -> 264, level 4: 800 -> 389).  This is
 * on-chain progression only and is NOT part of the deterministic dungeon
 * replay, so it does not affect the parity hash.
 *
 * Note: callers pass (level + 1) to get the threshold for the *next* level.
 */
int64_t
XpForLevel (const int level)
{
  return static_cast<int64_t> (std::floor (60.0 * std::pow (level, 1.35)));
}

/**
 * Stat points granted per level gained on level-up.  Bumped from 1 to 2 to
 * give each early level a meaningful durability boost: players can pump
 * constitution (each point = +HP_PER_CON max HP) roughly twice as fast.
 * On-chain progression only; no parity impact.
 */
constexpr int STAT_POINTS_PER_LEVEL = 2;

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
    " JOIN `segments` s"
    "   ON v.`segment_x` = s.`world_x` AND v.`segment_y` = s.`world_y`"
    " WHERE v.`id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visitId);
  sqlite3_step (stmt);
  const int64_t max = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);
  return max;
}

void
MoveProcessor::SetPlayerSegment (const std::string& name,
                                  const SegmentKey& seg)
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "UPDATE `players` SET `current_x` = ?2, `current_y` = ?3"
    " WHERE `name` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 2, seg.x);
  sqlite3_bind_int64 (stmt, 3, seg.y);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);
}

void
MoveProcessor::LinkSegments (const SegmentKey& from, const std::string& fromDir,
                              const SegmentKey& to, const std::string& toDir)
{
  auto insertIfAbsent = [this] (const SegmentKey& a, const std::string& aDir,
                                const SegmentKey& b, const std::string& bDir)
    {
      sqlite3_stmt* stmt;
      sqlite3_prepare_v2 (db,
        "SELECT 1 FROM `segment_links`"
        " WHERE `from_x` = ?1 AND `from_y` = ?2 AND `from_direction` = ?3",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, a.x);
      sqlite3_bind_int64 (stmt, 2, a.y);
      sqlite3_bind_text (stmt, 3, aDir.c_str (), -1, SQLITE_TRANSIENT);
      const bool present = sqlite3_step (stmt) == SQLITE_ROW;
      sqlite3_finalize (stmt);
      if (present)
        return;

      sqlite3_prepare_v2 (db,
        "INSERT INTO `segment_links`"
        " (`from_x`, `from_y`, `from_direction`, `to_x`, `to_y`,"
        "  `to_direction`) VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, a.x);
      sqlite3_bind_int64 (stmt, 2, a.y);
      sqlite3_bind_text (stmt, 3, aDir.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64 (stmt, 4, b.x);
      sqlite3_bind_int64 (stmt, 5, b.y);
      sqlite3_bind_text (stmt, 6, bDir.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    };

  insertIfAbsent (from, fromDir, to, toDir);
  insertIfAbsent (to, toDir, from, fromDir);
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
  /* The source segment, and therefore the coordinate being claimed.  */
  const SegmentKey srcSeg = CurrentSegment (db, name);
  const SegmentKey seg = Neighbour (srcSeg, dir);

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
      ? std::to_string (seg.x) + ":" + std::to_string (seg.y)
      : txid;
  const std::string seed = dungeonId.empty ()
      ? baseSeed
      : dungeonId + ":" + baseSeed;

  /* Depth is the distance from the hub, NOT the discovery-path length: the
     hub is (0,0) and gates are N/S/E/W, so the minimum gate-steps out is the
     Manhattan distance |x| + |y|.  This keeps depth symmetric about the hub
     and direction-aware (a segment discovered while heading back toward the
     hub is shallower), instead of always incrementing per hop.  The client's
     claimed `depth` is ignored, so it can't inflate difficulty either.
     The passed `depth` is unused.  */
  const int computedDepth = std::abs (seg.x) + std::abs (seg.y);

  /* Create the permanent segment.  Its coordinate is its identity, so the
     primary key rejects a second claim on the same cell outright.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "INSERT INTO `segments`"
    " (`world_x`, `world_y`, `discoverer`, `seed`, `depth`,"
    "  `created_height`)"
    " VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, seg.x);
  sqlite3_bind_int64 (stmt, 2, seg.y);
  sqlite3_bind_text (stmt, 3, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (stmt, 4, seed.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 5, computedDepth);
  sqlite3_bind_int64 (stmt, 6, currentHeight);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  /* Generate the dungeon to get gate positions, aligning the gate facing
     the source segment with the one we walked out of.  */
  std::vector<Gate> constraints;
  const std::string oppositeDir = OppositeDirection (dir);

  {

      /* Get the source gate position so we can align the new segment's
         opposite gate.  The hub has no stored gates, so a discovery out of
         it is unconstrained.  */
      sqlite3_prepare_v2 (db,
        "SELECT `x`, `y` FROM `segment_gates`"
        " WHERE `segment_x` = ?1 AND `segment_y` = ?2 AND `direction` = ?3",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, srcSeg.x);
      sqlite3_bind_int64 (stmt, 2, srcSeg.y);
      sqlite3_bind_text (stmt, 3, dir.c_str (), -1, SQLITE_TRANSIENT);

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
            "UPDATE `segments` SET `constraint_dir` = ?3"
            " WHERE `world_x` = ?1 AND `world_y` = ?2",
            -1, &stmt, nullptr);
          sqlite3_bind_int64 (stmt, 1, seg.x);
          sqlite3_bind_int64 (stmt, 2, seg.y);
          sqlite3_bind_text (stmt, 3, oppositeDir.c_str (), -1,
                             SQLITE_TRANSIENT);
          sqlite3_step (stmt);
          sqlite3_finalize (stmt);
        }
    }

  const auto dungeon = constraints.empty ()
      ? Dungeon::Generate (seed, computedDepth)
      : Dungeon::Generate (seed, computedDepth, constraints);

  /* Store gate positions.  */
  for (const auto& gate : dungeon.GetGates ())
    {
      sqlite3_prepare_v2 (db,
        "INSERT INTO `segment_gates`"
        " (`segment_x`, `segment_y`, `direction`, `x`, `y`)"
        " VALUES (?1, ?2, ?3, ?4, ?5)",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, seg.x);
      sqlite3_bind_int64 (stmt, 2, seg.y);
      sqlite3_bind_text (stmt, 3, gate.direction.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64 (stmt, 4, gate.x);
      sqlite3_bind_int64 (stmt, 5, gate.y);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }

  LinkSegments (srcSeg, dir, seg, oppositeDir);

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
             << seg << " (depth " << computedDepth << ")";
}

void
MoveProcessor::ProcessVisit (const std::string& name,
                              const SegmentKey& seg)
{
  const int64_t visId = nextVisitId++;

  /* Create a new visit to this segment.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "INSERT INTO `visits`"
    " (`id`, `segment_x`, `segment_y`, `initiator`, `created_height`)"
    " VALUES (?1, ?2, ?3, ?4, ?5)",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visId);
  sqlite3_bind_int64 (stmt, 2, seg.x);
  sqlite3_bind_int64 (stmt, 3, seg.y);
  sqlite3_bind_text (stmt, 4, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 5, currentHeight);
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
             << " to segment " << seg;
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

          /* Write back updated xp, level, skill_points, stat_points.
             Full heal on any level gained (backend-only, on-chain, not part
             of the replay/parity).  */
          sqlite3_prepare_v2 (db,
            "UPDATE `players` SET"
            " `xp` = ?2, `level` = ?3,"
            " `skill_points` = `skill_points` + ?4,"
            " `stat_points` = `stat_points` + ?5,"
            " `hp` = CASE WHEN ?4 > 0 THEN `max_hp` ELSE `hp` END"
            " WHERE `name` = ?1",
            -1, &stmt, nullptr);
          sqlite3_bind_text (stmt, 1, playerName.c_str (),
                             -1, SQLITE_TRANSIENT);
          sqlite3_bind_int64 (stmt, 2, xp);
          sqlite3_bind_int64 (stmt, 3, level);
          sqlite3_bind_int64 (stmt, 4, levelsGained);
          sqlite3_bind_int64 (stmt, 5, levelsGained * STAT_POINTS_PER_LEVEL);
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
  const SegmentKey srcSeg = CurrentSegment (db, name);
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `to_x`, `to_y` FROM `segment_links`"
    " WHERE `from_x` = ?1 AND `from_y` = ?2 AND `from_direction` = ?3",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, srcSeg.x);
  sqlite3_bind_int64 (stmt, 2, srcSeg.y);
  sqlite3_bind_text (stmt, 3, dir.c_str (), -1, SQLITE_TRANSIENT);
  const bool hasLink = (sqlite3_step (stmt) == SQLITE_ROW);
  SegmentKey destSeg;
  if (hasLink)
    destSeg = SegmentKey (
        static_cast<int> (sqlite3_column_int64 (stmt, 0)),
        static_cast<int> (sqlite3_column_int64 (stmt, 1)));
  sqlite3_finalize (stmt);

  /* No link in that direction: reject the move rather than silently
     teleporting the player to the hub.  */
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
  SetPlayerSegment (name, destSeg);

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
                                     const SegmentKey& seg,
                                     const std::string& entryDir)
{
  const int64_t visId = nextVisitId++;

  /* Set player as in-channel and move to the segment.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "UPDATE `players` SET `in_channel` = 1,"
    " `current_x` = ?2, `current_y` = ?3"
    " WHERE `name` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 2, seg.x);
  sqlite3_bind_int64 (stmt, 3, seg.y);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  /* Create a solo active visit.  `entry_direction` records the gate the
     player came in through (empty -> NULL -> centre spawn) so the replay
     and frontend spawn at the same tile.  */
  sqlite3_prepare_v2 (db,
    "INSERT INTO `visits`"
    " (`id`, `segment_x`, `segment_y`, `initiator`, `status`,"
    "  `created_height`, `started_height`, `entry_direction`)"
    " VALUES (?1, ?2, ?3, ?4, 'active', ?5, ?5, ?6)",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visId);
  sqlite3_bind_int64 (stmt, 2, seg.x);
  sqlite3_bind_int64 (stmt, 3, seg.y);
  sqlite3_bind_text (stmt, 4, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 5, currentHeight);
  if (entryDir.empty ())
    sqlite3_bind_null (stmt, 6);
  else
    sqlite3_bind_text (stmt, 6, entryDir.c_str (), -1, SQLITE_TRANSIENT);
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

  LOG (INFO) << name << " entered channel for segment " << seg
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
    "SELECT s.`seed`, s.`depth`, s.`world_x`, s.`world_y`,"
    "       v.`entry_direction`, s.`constraint_dir`"
    " FROM `visits` v"
    " JOIN `segments` s"
    "   ON v.`segment_x` = s.`world_x` AND v.`segment_y` = s.`world_y`"
    " WHERE v.`id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visitId);
  sqlite3_step (stmt);
  const std::string seed = reinterpret_cast<const char*> (
      sqlite3_column_text (stmt, 0));
  const int segDepth = static_cast<int> (sqlite3_column_int64 (stmt, 1));
  const SegmentKey seg (
      static_cast<int> (sqlite3_column_int64 (stmt, 2)),
      static_cast<int> (sqlite3_column_int64 (stmt, 3)));
  const char* entryDirRaw
      = reinterpret_cast<const char*> (sqlite3_column_text (stmt, 4));
  const std::string entryDir = entryDirRaw ? entryDirRaw : "";
  const char* constraintDirRaw
      = reinterpret_cast<const char*> (sqlite3_column_text (stmt, 5));
  const std::string constraintDir = constraintDirRaw ? constraintDirRaw : "";
  sqlite3_finalize (stmt);

  /* Rebuild the gate constraint (if any) from the stored gate position so
     replay regenerates the exact layout the player played.  */
  std::vector<Gate> constraints;
  if (!constraintDir.empty ())
    {
      sqlite3_prepare_v2 (db,
        "SELECT `x`, `y` FROM `segment_gates`"
        " WHERE `segment_x` = ?1 AND `segment_y` = ?2 AND `direction` = ?3",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, seg.x);
      sqlite3_bind_int64 (stmt, 2, seg.y);
      sqlite3_bind_text (stmt, 3, constraintDir.c_str (), -1, SQLITE_TRANSIENT);
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

  /* Read the player's on-chain inventory (bag + equipped) so mid-run
     equip/unequip actions can be verified and replayed.  ORDER BY rowid
     so the replay input is canonical.  */
  DungeonGame::EntryInventory entryInventory;
  sqlite3_prepare_v2 (db,
    "SELECT `rowid`, `item_id`, `slot` FROM `inventory`"
    " WHERE `name` = ?1 ORDER BY `rowid`",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      EntryInventoryItem item;
      item.rowid = sqlite3_column_int64 (stmt, 0);
      item.itemId = reinterpret_cast<const char*> (
          sqlite3_column_text (stmt, 1));
      item.slot = reinterpret_cast<const char*> (
          sqlite3_column_text (stmt, 2));
      entryInventory.push_back (item);
    }
  sqlite3_finalize (stmt);

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
      else if (type == "equip")
        {
          a.type = Action::Type::Equip;
          a.rowid = aj.get ("rowid", 0).asInt64 ();
          a.slot = aj.get ("slot", "").asString ();
        }
      else if (type == "unequip")
        {
          a.type = Action::Type::Unequip;
          a.rowid = aj.get ("rowid", 0).asInt64 ();
        }
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
                                    constraints, entryDir, entryInventory);

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

  /* Persist the final loadout from any mid-run equip/unequip actions.  The
     replay tracked which inventory rowid ended up in which slot; write that
     back so the gear the player finished the run with is what they now have
     equipped on-chain.  These are rearrangements of already-owned items, so
     they apply regardless of survival.  With no equip actions the entry
     inventory produces the same slots, so each UPDATE is a harmless no-op.
     The effective-stats/max_hp recompute done elsewhere then reflects it.  */
  for (const auto& fi : game.GetFinalInventory ())
    {
      sqlite3_prepare_v2 (db,
        "UPDATE `inventory` SET `slot` = ?3"
        " WHERE `rowid` = ?2 AND `name` = ?1",
        -1, &stmt, nullptr);
      sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64 (stmt, 2, fi.rowid);
      sqlite3_bind_text (stmt, 3, fi.slot.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }

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
     respawns at the hub, (0, 0), with half HP and loses 25% of their
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
    /* Per-segment survival heal: on a survived settlement, recover 30% of
       max HP (floored) on top of the HP carried out of the run, capped at
       max.  This is applied on-chain AFTER the deterministic replay, so it
       is NOT part of the replay/parity and never touches the frontend
       session.  On death the half-HP respawn is unchanged.  */
    " `hp` = CASE WHEN ?6"
    "              THEN MIN(`max_hp`, ?5 + `max_hp` * 30 / 100)"
    "              ELSE MAX(`max_hp` / 2, 1) END,"
    " `in_channel` = 0,"
    " `current_x` = CASE WHEN ?6 THEN `current_x` ELSE 0 END,"
    " `current_y` = CASE WHEN ?6 THEN `current_y` ELSE 0 END"
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

  /* On death, land back in the segment we came from rather than the hub
     (a free teleport home would be a meta-exploit).  Runs after the penalty
     UPDATE, which set the hub default it may override.  */
  if (!survived)
    RespawnAfterDeath (name, seg, entryDir);

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
        " `stat_points` = `stat_points` + ?5,"
        /* Full heal on any level gained (backend-only, on-chain, not part
           of the replay/parity).  Gate-walks award XP mid-expedition, so a
           level-up mid-run tops the player back up to full.  */
        " `hp` = CASE WHEN ?4 > 0 THEN `max_hp` ELSE `hp` END"
        " WHERE `name` = ?1",
        -1, &stmt, nullptr);
      sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64 (stmt, 2, xp);
      sqlite3_bind_int64 (stmt, 3, level);
      sqlite3_bind_int64 (stmt, 4, levelsGained);
      sqlite3_bind_int64 (stmt, 5, levelsGained * STAT_POINTS_PER_LEVEL);
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

  if (survived)
    {
      /* Confirm the segment (provisional → permanent) now that a valid
         run has been completed.  Makes it accessible for others.  */
      sqlite3_prepare_v2 (db,
        "UPDATE `segments` SET `confirmed` = 1"
        " WHERE `world_x` = ?1 AND `world_y` = ?2 AND `confirmed` = 0",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, seg.x);
      sqlite3_bind_int64 (stmt, 2, seg.y);
      sqlite3_step (stmt);
      if (sqlite3_changes (db) > 0)
        LOG (INFO) << "Segment " << seg
                   << " confirmed after valid run in visit " << visitId;
      sqlite3_finalize (stmt);
    }
  else
    {
      /* Failed run on a provisional segment: free the world coord so
         the discoverer can't perpetually re-enter to hold it hostage
         (would otherwise need to wait ~300 blocks for the time-based
         pruner).  Confirmed segments are unaffected by this call.  */
      PruneProvisionalSegment (seg);
    }

  LOG (INFO) << "Channel exit: " << name << " visit " << visitId
             << " survived=" << survived << " xp=" << xpGained
             << " gate=" << exitGate;

  return exitGate;
}

void
MoveProcessor::PruneProvisionalSegment (const SegmentKey& seg)
{
  /* Guard: only delete provisional segments.  Confirmed segments
     persist forever.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `confirmed` FROM `segments`"
    " WHERE `world_x` = ?1 AND `world_y` = ?2",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, seg.x);
  sqlite3_bind_int64 (stmt, 2, seg.y);
  if (sqlite3_step (stmt) != SQLITE_ROW)
    {
      sqlite3_finalize (stmt);
      return;  /* already gone */
    }
  const bool confirmed = sqlite3_column_int64 (stmt, 0) != 0;
  sqlite3_finalize (stmt);

  if (confirmed) return;

  /* Delete the visits that happened here, and everything hanging off them.
     Because a segment is addressed by its coordinate, a later segment
     discovered at this same cell would otherwise inherit the dead one's
     visit history.  The segment never existed as far as the world is
     concerned, so neither did its runs.  */
  std::vector<int64_t> visitIds;
  sqlite3_prepare_v2 (db,
    "SELECT `id` FROM `visits`"
    " WHERE `segment_x` = ?1 AND `segment_y` = ?2 ORDER BY `id`",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, seg.x);
  sqlite3_bind_int64 (stmt, 2, seg.y);
  while (sqlite3_step (stmt) == SQLITE_ROW)
    visitIds.push_back (sqlite3_column_int64 (stmt, 0));
  sqlite3_finalize (stmt);

  for (const auto visId : visitIds)
    {
      for (const char* sql : {
             "DELETE FROM `visit_participants` WHERE `visit_id` = ?1",
             "DELETE FROM `visit_results` WHERE `visit_id` = ?1",
             "DELETE FROM `loot_claims` WHERE `visit_id` = ?1",
             "DELETE FROM `visits` WHERE `id` = ?1",
           })
        {
          sqlite3_prepare_v2 (db, sql, -1, &stmt, nullptr);
          sqlite3_bind_int64 (stmt, 1, visId);
          sqlite3_step (stmt);
          sqlite3_finalize (stmt);
        }
    }

  /* Delete links (both directions).  */
  sqlite3_prepare_v2 (db,
    "DELETE FROM `segment_links`"
    " WHERE (`from_x` = ?1 AND `from_y` = ?2)"
    "    OR (`to_x` = ?1 AND `to_y` = ?2)",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, seg.x);
  sqlite3_bind_int64 (stmt, 2, seg.y);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  /* Delete gates.  */
  sqlite3_prepare_v2 (db,
    "DELETE FROM `segment_gates`"
    " WHERE `segment_x` = ?1 AND `segment_y` = ?2",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, seg.x);
  sqlite3_bind_int64 (stmt, 2, seg.y);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  /* Delete segment row itself.  */
  sqlite3_prepare_v2 (db,
    "DELETE FROM `segments`"
    " WHERE `world_x` = ?1 AND `world_y` = ?2",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, seg.x);
  sqlite3_bind_int64 (stmt, 2, seg.y);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  LOG (INFO) << "Pruned provisional segment " << seg;
}

void
MoveProcessor::RespawnAfterDeath (const std::string& name,
                                   const SegmentKey& diedSeg,
                                   const std::string& entryDir)
{
  /* No recorded entry gate (centre spawn / first dive): stay at the hub,
     which the caller already set.  */
  if (entryDir.empty ())
    return;

  /* The segment on the other side of the gate we entered through is simply
     the neighbour in that direction; we spawn at its matching gate.  */
  const SegmentKey prevSeg = Neighbour (diedSeg, entryDir);
  const std::string spawnDir = OppositeDirection (entryDir);

  /* Came from the hub, or the previous segment is gone: stay at the hub.  */
  if (prevSeg.IsHub () || !SegmentExists (db, prevSeg))
    return;

  /* Open a fresh solo run in the previous segment, spawned at its gate facing
     the segment we died in.  ProcessEnterChannel resets in_channel = 1 and
     the player position, overriding the caller's hub default; the already-applied
     half-HP carries over (this does not touch hp).  */
  ProcessEnterChannel (name, prevSeg, spawnDir);
  LOG (INFO) << name << " died and was knocked back to segment " << prevSeg;
}

void
MoveProcessor::UpdatePositionFromExitGate (const std::string& name,
                                            const SegmentKey& visitSeg,
                                            const std::string& exitGate)
{
  if (exitGate.empty ()) return;

  /* Stepping out of a gate always lands on the far side of it: the
     neighbouring cell.  Only move there if it is somewhere real -- the hub
     always is, any other cell must hold a segment.  */
  const SegmentKey dest = Neighbour (visitSeg, exitGate);
  if (!dest.IsHub () && !SegmentExists (db, dest))
    return;

  SetPlayerSegment (name, dest);
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
  SegmentKey visitSeg;
  {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2 (db,
      "SELECT `segment_x`, `segment_y` FROM `visits` WHERE `id` = ?1",
      -1, &stmt, nullptr);
    sqlite3_bind_int64 (stmt, 1, visitId);
    if (sqlite3_step (stmt) == SQLITE_ROW)
      visitSeg = SegmentKey (
          static_cast<int> (sqlite3_column_int64 (stmt, 0)),
          static_cast<int> (sqlite3_column_int64 (stmt, 1)));
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

   The settlement path is source-status-agnostic: it replays and banks the
   run for a PROVISIONAL source (which the survival also confirms) AND for
   an already-CONFIRMED source (a free re-run / farm whose loot must still
   be banked).  Whether a gate-walk carries a settlement is decided by the
   caller: leaving a confirmed segment attaches a proof only when the run
   collected loot; a bare crossing takes the no-settlement transit branch
   below.  Either way the transit itself stays free (no penalty, no prune,
   no survive-REQUIREMENT to leave a confirmed segment).

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
     enforces this), so the player's position shouldn't actually move
     on us.  */
  const SegmentKey srcSeg = CurrentSegment (db, name);

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
         original segment (since survived=true means the settlement body
         left their position alone).  */
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

  /* 2. The destination is the neighbouring cell in `dir`.  A gate always
     opens onto the cell next door, so the coordinate alone decides where
     the player lands -- there is no link row to disagree with it, and no
     id that could point somewhere else.  */
  const SegmentKey target = Neighbour (srcSeg, dir);

  /* 3. Walking to the world origin puts the player on the overworld at
     the hub -- no channel, no visit.  */
  if (target.IsHub ())
    {
      sqlite3_stmt* stmt;
      sqlite3_prepare_v2 (db,
        "UPDATE `players` SET `current_x` = 0, `current_y` = 0,"
        " `in_channel` = 0"
        " WHERE `name` = ?1",
        -1, &stmt, nullptr);
      sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
      LOG (INFO) << name << " gate-walked to hub from segment " << srcSeg;
      return;
    }

  /* 4. Either the neighbour already exists -- HandleGateWalk has checked
     access, so this is a plain transit -- or it is unexplored and this
     gate-walk discovers it.  */
  if (SegmentExists (db, target))
    {
      /* Record the link if this pair has never been walked before (two
         segments discovered from different parents are neighbours by
         coordinate long before any link row exists).  */
      LinkSegments (srcSeg, dir, target, OppositeDirection (dir));
    }
  else
    {
      /* Depth of the new segment: source depth + 1, or 1 from the hub.  */
      int srcDepth = 0;
      if (!srcSeg.IsHub ())
        {
          sqlite3_stmt* stmt;
          sqlite3_prepare_v2 (db,
            "SELECT `depth` FROM `segments`"
            " WHERE `world_x` = ?1 AND `world_y` = ?2",
            -1, &stmt, nullptr);
          sqlite3_bind_int64 (stmt, 1, srcSeg.x);
          sqlite3_bind_int64 (stmt, 2, srcSeg.y);
          if (sqlite3_step (stmt) == SQLITE_ROW)
            srcDepth = static_cast<int> (sqlite3_column_int64 (stmt, 0));
          sqlite3_finalize (stmt);
        }

      ProcessDiscover (name, srcDepth + 1, txid, dir);

      /* A losing race for the coordinate leaves nothing to walk into: the
         primary key rejected the insert.  Stay put rather than entering a
         segment that belongs to someone else.  */
      if (!SegmentExists (db, target))
        {
          LOG (WARNING) << name << " gate-walk: coordinate " << target
                        << " was claimed by another player this block";
          return;
        }
    }

  /* 5. Enter the target's channel.  The player comes in through the gate
     on the opposite wall, so they spawn there.  */
  ProcessEnterChannel (name, target, OppositeDirection (dir));
  LOG (INFO) << name << " gate-walked " << dir
             << " from " << srcSeg << " to " << target;
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

  /* Force-settle active visits that have exceeded the active timeout, but
     ONLY on PROVISIONAL segments.  A confirmed segment has no coordinate to
     release, so its abandoned run is left active: the player idled or
     disconnected and should resume exactly where they were, not be relocated
     (a timeout must not move you).  A provisional segment's coordinate must be
     released (anti-grief), so it still times out; the player steps back one
     segment with no penalty (see below).
     Solo visits (1 participant) use SOLO_VISIT_ACTIVE_TIMEOUT; multiplayer
     visits use VISIT_ACTIVE_TIMEOUT.  */
  {
    sqlite3_stmt* query;
    sqlite3_prepare_v2 (db,
      "SELECT v.`id` FROM `visits` v"
      " WHERE v.`status` = 'active'"
      " AND EXISTS (SELECT 1 FROM `segments` s"
      "             WHERE s.`world_x` = v.`segment_x`"
      "               AND s.`world_y` = v.`segment_y`"
      "               AND s.`confirmed` = 0)"
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
        SegmentKey visSeg;
        std::string visEntryDir;
        {
          sqlite3_stmt* segQuery;
          sqlite3_prepare_v2 (db,
            "SELECT `segment_x`, `segment_y`, `entry_direction`"
            " FROM `visits` WHERE `id` = ?1",
            -1, &segQuery, nullptr);
          sqlite3_bind_int64 (segQuery, 1, visId);
          if (sqlite3_step (segQuery) == SQLITE_ROW)
            {
              visSeg = SegmentKey (
                  static_cast<int> (sqlite3_column_int64 (segQuery, 0)),
                  static_cast<int> (sqlite3_column_int64 (segQuery, 1)));
              const char* e = reinterpret_cast<const char*> (
                  sqlite3_column_text (segQuery, 2));
              visEntryDir = e ? e : "";
            }
          sqlite3_finalize (segQuery);
        }

        /* Record failure results for all participants.  ORDER BY name so the
           new-visit ids minted by the knock-back respawn below are assigned in
           a deterministic order across all nodes.  */
        sqlite3_stmt* pQuery;
        sqlite3_prepare_v2 (db,
          "SELECT `name` FROM `visit_participants`"
          " WHERE `visit_id` = ?1 ORDER BY `name`",
          -1, &pQuery, nullptr);
        sqlite3_bind_int64 (pQuery, 1, visId);

        /* Collect names first: the knock-back respawn below inserts into
           visit_participants (for the new run), so we must not be mid-iteration
           on that same table when it runs.  */
        std::vector<std::string> participants;
        while (sqlite3_step (pQuery) == SQLITE_ROW)
          {
            const char* pName
                = reinterpret_cast<const char*> (sqlite3_column_text (pQuery, 0));
            participants.push_back (pName ? pName : "");
          }
        sqlite3_finalize (pQuery);

        for (const auto& pName : participants)
          {
            sqlite3_stmt* ins;
            sqlite3_prepare_v2 (db,
              "INSERT INTO `visit_results`"
              " (`visit_id`, `name`, `survived`, `xp_gained`,"
              "  `gold_gained`, `kills`)"
              " VALUES (?1, ?2, 0, 0, 0, 0)",
              -1, &ins, nullptr);
            sqlite3_bind_int64 (ins, 1, visId);
            sqlite3_bind_text (ins, 2, pName.c_str (), -1, SQLITE_TRANSIENT);
            sqlite3_step (ins);
            sqlite3_finalize (ins);

            /* A timeout is an ABANDONED run, NOT a death: no penalty (no HP
               loss, no gold loss, no death count).  This only fires on a
               PROVISIONAL segment (pruned below), so the player cannot stay
               where they were; step them back one segment to where they came
               from, exactly like the death knock-back but penalty-free.
               Position (0, 0) is the hub default that RespawnAfterDeath
               overrides when a deeper previous segment exists (a first-layer
               provisional, entered from the hub, correctly lands at the hub).  */
            sqlite3_prepare_v2 (db,
              "UPDATE `players` SET"
              " `in_channel` = 0,"
              " `current_x` = 0, `current_y` = 0"
              " WHERE `name` = ?1",
              -1, &ins, nullptr);
            sqlite3_bind_text (ins, 1, pName.c_str (), -1, SQLITE_TRANSIENT);
            sqlite3_step (ins);
            sqlite3_finalize (ins);

            RespawnAfterDeath (pName, visSeg, visEntryDir);
          }

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
        PruneProvisionalSegment (visSeg);

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
      "SELECT `world_x`, `world_y` FROM `segments` s"
      " WHERE `confirmed` = 0"
      " AND `created_height` + ?1 <= ?2"
      " AND NOT EXISTS"
      "   (SELECT 1 FROM `visits` v"
      "    WHERE v.`segment_x` = s.`world_x`"
      "      AND v.`segment_y` = s.`world_y`"
      "      AND v.`status` IN ('open', 'active'))"
      " ORDER BY `world_x`, `world_y`",
      -1, &query, nullptr);
    sqlite3_bind_int64 (query, 1, pruneAge);
    sqlite3_bind_int64 (query, 2, currentHeight);

    std::vector<SegmentKey> toPrune;
    while (sqlite3_step (query) == SQLITE_ROW)
      toPrune.push_back (SegmentKey (
          static_cast<int> (sqlite3_column_int64 (query, 0)),
          static_cast<int> (sqlite3_column_int64 (query, 1))));
    sqlite3_finalize (query);

    for (const auto& seg : toPrune)
      PruneProvisionalSegment (seg);
  }
}

} // namespace rog
