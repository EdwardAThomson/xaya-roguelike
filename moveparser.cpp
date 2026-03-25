#include "moveparser.hpp"

#include <glog/logging.h>

namespace rog
{

bool
PlayerExists (sqlite3* db, const std::string& name)
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT COUNT(*) FROM `players` WHERE `name` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  const int64_t count = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);
  return count > 0;
}

bool
PlayerInActiveSegment (sqlite3* db, const std::string& name)
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT COUNT(*) FROM `segment_participants` sp"
    " JOIN `segments` s ON sp.`segment_id` = s.`id`"
    " WHERE sp.`name` = ?1 AND (s.`status` = 'open' OR s.`status` = 'active')",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  const int64_t count = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);
  return count > 0;
}

void
MoveParser::ProcessOne (const Json::Value& obj)
{
  if (!obj.isObject ())
    {
      LOG (WARNING) << "Move is not an object: " << obj;
      return;
    }

  const auto& nameVal = obj["name"];
  if (!nameVal.isString ())
    {
      LOG (WARNING) << "Move has no name: " << obj;
      return;
    }
  const std::string name = nameVal.asString ();

  /* txid or mvid for segment seed generation.  */
  std::string txid;
  if (obj.isMember ("mvid"))
    txid = obj["mvid"].asString ();
  else if (obj.isMember ("txid"))
    txid = obj["txid"].asString ();

  const auto& mv = obj["move"];
  if (!mv.isObject ())
    {
      LOG (WARNING) << "Invalid move from " << name << ": " << mv;
      return;
    }

  HandleOperation (name, txid, mv);
}

void
MoveParser::HandleOperation (const std::string& name, const std::string& txid,
                              const Json::Value& mv)
{
  if (mv.size () != 1)
    {
      LOG (WARNING) << "Move must have exactly one action key: " << mv;
      return;
    }

  if (mv.isMember ("r"))
    HandleRegister (name, mv["r"]);
  else if (mv.isMember ("d"))
    HandleDiscover (name, txid, mv["d"]);
  else if (mv.isMember ("j"))
    HandleJoin (name, mv["j"]);
  else if (mv.isMember ("lv"))
    HandleLeave (name, mv["lv"]);
  else if (mv.isMember ("s"))
    HandleSettle (name, mv["s"]);
  else if (mv.isMember ("as"))
    HandleAllocateStat (name, mv["as"]);
  else
    LOG (WARNING) << "Unknown action in move: " << mv;
}

void
MoveParser::HandleRegister (const std::string& name, const Json::Value& op)
{
  if (!op.isObject () || op.size () != 0)
    {
      LOG (WARNING) << "Invalid register move: " << op;
      return;
    }

  if (PlayerExists (db, name))
    {
      LOG (WARNING) << "Player " << name << " already registered";
      return;
    }

  ProcessRegister (name);
}

void
MoveParser::HandleDiscover (const std::string& name, const std::string& txid,
                             const Json::Value& op)
{
  if (!op.isObject ())
    {
      LOG (WARNING) << "Invalid discover move: " << op;
      return;
    }

  if (!op.isMember ("depth") || !op["depth"].isInt ())
    {
      LOG (WARNING) << "Discover move missing depth: " << op;
      return;
    }

  const int depth = op["depth"].asInt ();
  if (depth < 1 || depth > 20)
    {
      LOG (WARNING) << "Discover depth out of range: " << depth;
      return;
    }

  if (!PlayerExists (db, name))
    {
      LOG (WARNING) << "Player " << name << " not registered";
      return;
    }

  if (PlayerInActiveSegment (db, name))
    {
      LOG (WARNING) << "Player " << name << " already in an active segment";
      return;
    }

  ProcessDiscover (name, depth, txid);
}

void
MoveParser::HandleJoin (const std::string& name, const Json::Value& op)
{
  if (!op.isObject ())
    {
      LOG (WARNING) << "Invalid join move: " << op;
      return;
    }

  if (!op.isMember ("id") || !op["id"].isInt64 ())
    {
      LOG (WARNING) << "Join move missing segment id: " << op;
      return;
    }

  const int64_t segmentId = op["id"].asInt64 ();

  if (!PlayerExists (db, name))
    {
      LOG (WARNING) << "Player " << name << " not registered";
      return;
    }

  if (PlayerInActiveSegment (db, name))
    {
      LOG (WARNING) << "Player " << name << " already in an active segment";
      return;
    }

  /* Check segment exists and is open.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `status`, `max_players`,"
    " (SELECT COUNT(*) FROM `segment_participants`"
    "  WHERE `segment_id` = ?1)"
    " FROM `segments` WHERE `id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, segmentId);

  if (sqlite3_step (stmt) != SQLITE_ROW)
    {
      sqlite3_finalize (stmt);
      LOG (WARNING) << "Segment " << segmentId << " does not exist";
      return;
    }

  const std::string status
      = reinterpret_cast<const char*> (sqlite3_column_text (stmt, 0));
  const int64_t maxPlayers = sqlite3_column_int64 (stmt, 1);
  const int64_t currentPlayers = sqlite3_column_int64 (stmt, 2);
  sqlite3_finalize (stmt);

  if (status != "open")
    {
      LOG (WARNING) << "Segment " << segmentId << " is not open (status: "
                    << status << ")";
      return;
    }

  if (currentPlayers >= maxPlayers)
    {
      LOG (WARNING) << "Segment " << segmentId << " is full";
      return;
    }

  /* Check player not already in this segment.  */
  sqlite3_prepare_v2 (db,
    "SELECT COUNT(*) FROM `segment_participants`"
    " WHERE `segment_id` = ?1 AND `name` = ?2",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, segmentId);
  sqlite3_bind_text (stmt, 2, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  const int64_t already = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);

  if (already > 0)
    {
      LOG (WARNING) << "Player " << name << " already in segment " << segmentId;
      return;
    }

  ProcessJoin (name, segmentId);
}

void
MoveParser::HandleLeave (const std::string& name, const Json::Value& op)
{
  if (!op.isObject ())
    {
      LOG (WARNING) << "Invalid leave move: " << op;
      return;
    }

  if (!op.isMember ("id") || !op["id"].isInt64 ())
    {
      LOG (WARNING) << "Leave move missing segment id: " << op;
      return;
    }

  const int64_t segmentId = op["id"].asInt64 ();

  /* Check segment is open.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `status`, `discoverer` FROM `segments` WHERE `id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, segmentId);

  if (sqlite3_step (stmt) != SQLITE_ROW)
    {
      sqlite3_finalize (stmt);
      LOG (WARNING) << "Segment " << segmentId << " does not exist";
      return;
    }

  const std::string status
      = reinterpret_cast<const char*> (sqlite3_column_text (stmt, 0));
  const std::string discoverer
      = reinterpret_cast<const char*> (sqlite3_column_text (stmt, 1));
  sqlite3_finalize (stmt);

  if (status != "open")
    {
      LOG (WARNING) << "Cannot leave segment " << segmentId
                    << " (status: " << status << ")";
      return;
    }

  if (name == discoverer)
    {
      LOG (WARNING) << "Discoverer " << name
                    << " cannot leave their own segment";
      return;
    }

  /* Check player is actually in this segment.  */
  sqlite3_prepare_v2 (db,
    "SELECT COUNT(*) FROM `segment_participants`"
    " WHERE `segment_id` = ?1 AND `name` = ?2",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, segmentId);
  sqlite3_bind_text (stmt, 2, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  const int64_t count = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);

  if (count == 0)
    {
      LOG (WARNING) << "Player " << name
                    << " is not in segment " << segmentId;
      return;
    }

  ProcessLeave (name, segmentId);
}

void
MoveParser::HandleSettle (const std::string& name, const Json::Value& op)
{
  if (!op.isObject ())
    {
      LOG (WARNING) << "Invalid settle move: " << op;
      return;
    }

  if (!op.isMember ("id") || !op["id"].isInt64 ())
    {
      LOG (WARNING) << "Settle move missing segment id: " << op;
      return;
    }

  const int64_t segmentId = op["id"].asInt64 ();

  if (!op.isMember ("results") || !op["results"].isArray ())
    {
      LOG (WARNING) << "Settle move missing results array: " << op;
      return;
    }

  /* Check segment exists and is active.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `status`, `discoverer` FROM `segments` WHERE `id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, segmentId);

  if (sqlite3_step (stmt) != SQLITE_ROW)
    {
      sqlite3_finalize (stmt);
      LOG (WARNING) << "Segment " << segmentId << " does not exist";
      return;
    }

  const std::string status
      = reinterpret_cast<const char*> (sqlite3_column_text (stmt, 0));
  const std::string discoverer
      = reinterpret_cast<const char*> (sqlite3_column_text (stmt, 1));
  sqlite3_finalize (stmt);

  if (status != "active")
    {
      LOG (WARNING) << "Segment " << segmentId
                    << " is not active (status: " << status << ")";
      return;
    }

  if (name != discoverer)
    {
      LOG (WARNING) << "Only discoverer " << discoverer
                    << " can settle segment " << segmentId
                    << ", not " << name;
      return;
    }

  /* Validate each result entry.  */
  const auto& results = op["results"];
  for (const auto& r : results)
    {
      if (!r.isObject ())
        {
          LOG (WARNING) << "Invalid result entry: " << r;
          return;
        }

      if (!r.isMember ("p") || !r["p"].isString ())
        {
          LOG (WARNING) << "Result missing player name: " << r;
          return;
        }

      /* Check that the player is a participant.  */
      const std::string playerName = r["p"].asString ();
      sqlite3_prepare_v2 (db,
        "SELECT COUNT(*) FROM `segment_participants`"
        " WHERE `segment_id` = ?1 AND `name` = ?2",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, segmentId);
      sqlite3_bind_text (stmt, 2, playerName.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_step (stmt);
      const int64_t count = sqlite3_column_int64 (stmt, 0);
      sqlite3_finalize (stmt);

      if (count == 0)
        {
          LOG (WARNING) << "Player " << playerName
                        << " is not a participant of segment " << segmentId;
          return;
        }

      /* Validate numeric fields.  */
      if (r.isMember ("xp") && (!r["xp"].isInt () || r["xp"].asInt () < 0))
        {
          LOG (WARNING) << "Invalid xp in result: " << r;
          return;
        }
      if (r.isMember ("gold")
          && (!r["gold"].isInt () || r["gold"].asInt () < 0))
        {
          LOG (WARNING) << "Invalid gold in result: " << r;
          return;
        }
      if (r.isMember ("kills")
          && (!r["kills"].isInt () || r["kills"].asInt () < 0))
        {
          LOG (WARNING) << "Invalid kills in result: " << r;
          return;
        }

      /* Validate loot array if present.  */
      if (r.isMember ("loot"))
        {
          if (!r["loot"].isArray ())
            {
              LOG (WARNING) << "Invalid loot in result: " << r;
              return;
            }
          for (const auto& loot : r["loot"])
            {
              if (!loot.isObject ()
                  || !loot.isMember ("item") || !loot["item"].isString ()
                  || !loot.isMember ("n") || !loot["n"].isInt ()
                  || loot["n"].asInt () <= 0)
                {
                  LOG (WARNING) << "Invalid loot entry: " << loot;
                  return;
                }
            }
        }
    }

  ProcessSettle (name, segmentId, results);
}

void
MoveParser::HandleAllocateStat (const std::string& name, const Json::Value& op)
{
  if (!op.isObject ())
    {
      LOG (WARNING) << "Invalid allocate stat move: " << op;
      return;
    }

  if (!op.isMember ("stat") || !op["stat"].isString ())
    {
      LOG (WARNING) << "Allocate stat missing stat name: " << op;
      return;
    }

  const std::string stat = op["stat"].asString ();
  if (stat != "strength" && stat != "dexterity"
      && stat != "constitution" && stat != "intelligence")
    {
      LOG (WARNING) << "Invalid stat name: " << stat;
      return;
    }

  if (!PlayerExists (db, name))
    {
      LOG (WARNING) << "Player " << name << " not registered";
      return;
    }

  /* Check player has stat points available.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `stat_points` FROM `players` WHERE `name` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  const int64_t points = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);

  if (points <= 0)
    {
      LOG (WARNING) << "Player " << name << " has no stat points";
      return;
    }

  ProcessAllocateStat (name, stat);
}

} // namespace rog
