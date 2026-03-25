#include "moveprocessor.hpp"

#include <glog/logging.h>

#include <cmath>

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
MoveProcessor::CountParticipants (const int64_t segmentId)
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT COUNT(*) FROM `segment_participants`"
    " WHERE `segment_id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, segmentId);
  sqlite3_step (stmt);
  const int64_t count = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);
  return count;
}

int64_t
MoveProcessor::GetMaxPlayers (const int64_t segmentId)
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `max_players` FROM `segments` WHERE `id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, segmentId);
  sqlite3_step (stmt);
  const int64_t max = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);
  return max;
}

void
MoveProcessor::ProcessRegister (const std::string& name)
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "INSERT INTO `players` (`name`, `registered_height`)"
    " VALUES (?1, ?2)",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 2, currentHeight);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  GiveStartingItems (name);

  LOG (INFO) << "Registered player " << name
             << " at height " << currentHeight;
}

void
MoveProcessor::ProcessDiscover (const std::string& name, const int depth,
                                 const std::string& txid)
{
  const int64_t id = nextSegmentId++;

  /* Use the txid as the seed (deterministic across all nodes).  */
  const std::string seed = txid.empty ()
      ? std::to_string (id)
      : txid;

  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "INSERT INTO `segments`"
    " (`id`, `discoverer`, `seed`, `depth`, `created_height`)"
    " VALUES (?1, ?2, ?3, ?4, ?5)",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, id);
  sqlite3_bind_text (stmt, 2, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (stmt, 3, seed.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 4, depth);
  sqlite3_bind_int64 (stmt, 5, currentHeight);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  /* Discoverer is automatically the first participant.  */
  sqlite3_prepare_v2 (db,
    "INSERT INTO `segment_participants`"
    " (`segment_id`, `name`, `joined_height`)"
    " VALUES (?1, ?2, ?3)",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, id);
  sqlite3_bind_text (stmt, 2, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 3, currentHeight);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  LOG (INFO) << "Player " << name << " discovered segment " << id
             << " (depth " << depth << ")";
}

void
MoveProcessor::ProcessJoin (const std::string& name, const int64_t segmentId)
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "INSERT INTO `segment_participants`"
    " (`segment_id`, `name`, `joined_height`)"
    " VALUES (?1, ?2, ?3)",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, segmentId);
  sqlite3_bind_text (stmt, 2, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 3, currentHeight);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  LOG (INFO) << "Player " << name << " joined segment " << segmentId;

  /* If segment is now full, set status to active.  */
  const int64_t count = CountParticipants (segmentId);
  const int64_t max = GetMaxPlayers (segmentId);

  if (count >= max)
    {
      sqlite3_prepare_v2 (db,
        "UPDATE `segments`"
        " SET `status` = 'active', `started_height` = ?2"
        " WHERE `id` = ?1",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, segmentId);
      sqlite3_bind_int64 (stmt, 2, currentHeight);
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);

      LOG (INFO) << "Segment " << segmentId << " is now active (full)";
    }
}

void
MoveProcessor::ProcessLeave (const std::string& name, const int64_t segmentId)
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "DELETE FROM `segment_participants`"
    " WHERE `segment_id` = ?1 AND `name` = ?2",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, segmentId);
  sqlite3_bind_text (stmt, 2, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  LOG (INFO) << "Player " << name << " left segment " << segmentId;
}

void
MoveProcessor::ProcessSettle (const std::string& name,
                               const int64_t segmentId,
                               const Json::Value& results)
{
  for (const auto& r : results)
    {
      const std::string playerName = r["p"].asString ();
      const bool survived = r.get ("survived", false).asBool ();
      const int64_t xpGained = r.get ("xp", 0).asInt64 ();
      const int64_t goldGained = r.get ("gold", 0).asInt64 ();
      const int64_t killsGained = r.get ("kills", 0).asInt64 ();

      /* Insert segment result.  */
      sqlite3_stmt* stmt;
      sqlite3_prepare_v2 (db,
        "INSERT INTO `segment_results`"
        " (`segment_id`, `name`, `survived`, `xp_gained`,"
        "  `gold_gained`, `kills`)"
        " VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, segmentId);
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
                " (`segment_id`, `name`, `item_id`, `quantity`)"
                " VALUES (?1, ?2, ?3, ?4)",
                -1, &stmt, nullptr);
              sqlite3_bind_int64 (stmt, 1, segmentId);
              sqlite3_bind_text (stmt, 2, playerName.c_str (),
                                 -1, SQLITE_TRANSIENT);
              sqlite3_bind_text (stmt, 3, itemId.c_str (),
                                 -1, SQLITE_TRANSIENT);
              sqlite3_bind_int64 (stmt, 4, qty);
              sqlite3_step (stmt);
              sqlite3_finalize (stmt);

              /* Add to player inventory (bag slot).  */
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

      /* Update player stats: add gold, kills, segments_completed,
         deaths (if not survived).  */
      sqlite3_prepare_v2 (db,
        "UPDATE `players` SET"
        " `gold` = `gold` + ?2,"
        " `kills` = `kills` + ?3,"
        " `segments_completed` = `segments_completed` + 1,"
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

      LOG (INFO) << "Settled " << playerName << " in segment " << segmentId
                 << ": survived=" << survived
                 << " xp=" << xpGained << " gold=" << goldGained;
    }

  /* Mark segment as completed.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "UPDATE `segments`"
    " SET `status` = 'completed', `settled_height` = ?2"
    " WHERE `id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, segmentId);
  sqlite3_bind_int64 (stmt, 2, currentHeight);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  LOG (INFO) << "Segment " << segmentId << " settled by " << name;
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

  LOG (INFO) << name << " increased " << stat << " by 1";
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
  /* Expire open segments that have been waiting too long for players.  */
  {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2 (db,
      "UPDATE `segments` SET `status` = 'expired'"
      " WHERE `status` = 'open'"
      " AND `created_height` + ?1 <= ?2",
      -1, &stmt, nullptr);
    sqlite3_bind_int64 (stmt, 1, SEGMENT_OPEN_TIMEOUT);
    sqlite3_bind_int64 (stmt, 2, currentHeight);
    sqlite3_step (stmt);
    const int changed = sqlite3_changes (db);
    sqlite3_finalize (stmt);

    if (changed > 0)
      LOG (INFO) << "Expired " << changed << " open segment(s) at height "
                 << currentHeight;
  }

  /* Force-settle active segments that have exceeded the active timeout.
     All participants get survived=false, no rewards.  */
  {
    sqlite3_stmt* query;
    sqlite3_prepare_v2 (db,
      "SELECT `id` FROM `segments`"
      " WHERE `status` = 'active'"
      " AND `started_height` + ?1 <= ?2",
      -1, &query, nullptr);
    sqlite3_bind_int64 (query, 1, SEGMENT_ACTIVE_TIMEOUT);
    sqlite3_bind_int64 (query, 2, currentHeight);

    std::vector<int64_t> timedOut;
    while (sqlite3_step (query) == SQLITE_ROW)
      timedOut.push_back (sqlite3_column_int64 (query, 0));
    sqlite3_finalize (query);

    for (const auto segId : timedOut)
      {
        /* Record failure results for all participants.  */
        sqlite3_stmt* pQuery;
        sqlite3_prepare_v2 (db,
          "SELECT `name` FROM `segment_participants`"
          " WHERE `segment_id` = ?1",
          -1, &pQuery, nullptr);
        sqlite3_bind_int64 (pQuery, 1, segId);

        while (sqlite3_step (pQuery) == SQLITE_ROW)
          {
            const char* pName
                = reinterpret_cast<const char*> (sqlite3_column_text (pQuery, 0));

            sqlite3_stmt* ins;
            sqlite3_prepare_v2 (db,
              "INSERT INTO `segment_results`"
              " (`segment_id`, `name`, `survived`, `xp_gained`,"
              "  `gold_gained`, `kills`)"
              " VALUES (?1, ?2, 0, 0, 0, 0)",
              -1, &ins, nullptr);
            sqlite3_bind_int64 (ins, 1, segId);
            sqlite3_bind_text (ins, 2, pName, -1, SQLITE_TRANSIENT);
            sqlite3_step (ins);
            sqlite3_finalize (ins);

            /* Increment death count.  */
            sqlite3_prepare_v2 (db,
              "UPDATE `players` SET"
              " `deaths` = `deaths` + 1,"
              " `segments_completed` = `segments_completed` + 1"
              " WHERE `name` = ?1",
              -1, &ins, nullptr);
            sqlite3_bind_text (ins, 1, pName, -1, SQLITE_TRANSIENT);
            sqlite3_step (ins);
            sqlite3_finalize (ins);
          }
        sqlite3_finalize (pQuery);

        /* Mark segment as completed.  */
        sqlite3_stmt* upd;
        sqlite3_prepare_v2 (db,
          "UPDATE `segments`"
          " SET `status` = 'completed', `settled_height` = ?2"
          " WHERE `id` = ?1",
          -1, &upd, nullptr);
        sqlite3_bind_int64 (upd, 1, segId);
        sqlite3_bind_int64 (upd, 2, currentHeight);
        sqlite3_step (upd);
        sqlite3_finalize (upd);

        LOG (INFO) << "Force-settled segment " << segId
                   << " due to active timeout at height " << currentHeight;
      }
  }
}

} // namespace rog
