#include "moveparser.hpp"
#include "items.hpp"

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
PlayerInChannel (sqlite3* db, const std::string& name)
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `in_channel` FROM `players` WHERE `name` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  if (sqlite3_step (stmt) != SQLITE_ROW)
    {
      sqlite3_finalize (stmt);
      return false;
    }
  const int64_t val = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);
  return val != 0;
}

SegmentKey
CurrentSegment (sqlite3* db, const std::string& name)
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `current_x`, `current_y` FROM `players` WHERE `name` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  SegmentKey seg;
  if (sqlite3_step (stmt) == SQLITE_ROW)
    {
      seg.x = static_cast<int> (sqlite3_column_int64 (stmt, 0));
      seg.y = static_cast<int> (sqlite3_column_int64 (stmt, 1));
    }
  sqlite3_finalize (stmt);
  return seg;
}

bool
SegmentExists (sqlite3* db, const SegmentKey& seg)
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT COUNT(*) FROM `segments`"
    " WHERE `world_x` = ?1 AND `world_y` = ?2",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, seg.x);
  sqlite3_bind_int64 (stmt, 2, seg.y);
  sqlite3_step (stmt);
  const int64_t count = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);
  return count > 0;
}

namespace
{

/**
 * Parses a segment reference from a move: {"x": <int>, "y": <int>}.  A
 * segment is named by its world coordinate and nothing else, so both
 * members are required and must be integers.
 */
bool
ParseSegmentRef (const Json::Value& op, SegmentKey& out)
{
  if (!op.isMember ("x") || !op["x"].isInt ()
      || !op.isMember ("y") || !op["y"].isInt ())
    return false;
  out = SegmentKey (op["x"].asInt (), op["y"].asInt ());
  return true;
}

} // anonymous namespace

bool
PlayerInActiveVisit (sqlite3* db, const std::string& name)
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT COUNT(*) FROM `visit_participants` vp"
    " JOIN `visits` v ON vp.`visit_id` = v.`id`"
    " WHERE vp.`name` = ?1"
    " AND (v.`status` = 'open' OR v.`status` = 'active')",
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
  else if (mv.isMember ("v"))
    HandleVisit (name, mv["v"]);
  else if (mv.isMember ("j"))
    HandleJoin (name, mv["j"]);
  else if (mv.isMember ("lv"))
    HandleLeave (name, mv["lv"]);
  else if (mv.isMember ("s"))
    HandleSettle (name, mv["s"]);
  else if (mv.isMember ("as"))
    HandleAllocateStat (name, mv["as"]);
  else if (mv.isMember ("t"))
    HandleTravel (name, txid, mv["t"]);
  else if (mv.isMember ("ui"))
    HandleUseItem (name, mv["ui"]);
  else if (mv.isMember ("eq"))
    HandleEquip (name, mv["eq"]);
  else if (mv.isMember ("uq"))
    HandleUnequip (name, mv["uq"]);
  else if (mv.isMember ("di"))
    HandleDiscard (name, mv["di"]);
  else if (mv.isMember ("gw"))
    HandleGateWalk (name, txid, mv["gw"]);
  else if (mv.isMember ("ec"))
    HandleEnterChannel (name, mv["ec"]);
  else if (mv.isMember ("xc"))
    HandleExitChannel (name, mv["xc"]);
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

  if (PlayerInActiveVisit (db, name))
    {
      LOG (WARNING) << "Player " << name << " already in an active visit";
      return;
    }

  if (PlayerInChannel (db, name))
    {
      LOG (WARNING) << "Player " << name << " is in a channel";
      return;
    }

  /* Discovery cooldown check.  */
  {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2 (db,
      "SELECT `last_discover_height` FROM `players` WHERE `name` = ?1",
      -1, &stmt, nullptr);
    sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
    sqlite3_step (stmt);
    const unsigned lastDiscover
        = static_cast<unsigned> (sqlite3_column_int64 (stmt, 0));
    sqlite3_finalize (stmt);

    if (lastDiscover > 0 && currentHeight < lastDiscover + 50)
      {
        LOG (WARNING) << "Player " << name << " discovery cooldown active"
                      << " (last=" << lastDiscover << " now=" << currentHeight << ")";
        return;
      }
  }

  /* A direction is required: discovering means claiming the neighbouring
     cell through a gate, and without a direction there is no cell to
     claim (the one you stand on is already taken -- by you).  */
  if (!op.isMember ("dir") || !op["dir"].isString ())
    {
      LOG (WARNING) << "Discover move missing dir: " << op;
      return;
    }

  const std::string dir = op["dir"].asString ();
  if (dir != "north" && dir != "south" && dir != "east" && dir != "west")
    {
      LOG (WARNING) << "Invalid direction: " << dir;
      return;
    }

  {
      /* Check the player's current segment doesn't already have a link
         in that direction.  */
      const SegmentKey curSeg = CurrentSegment (db, name);

      sqlite3_stmt* stmt;
      sqlite3_prepare_v2 (db,
        "SELECT COUNT(*) FROM `segment_links`"
        " WHERE `from_x` = ?1 AND `from_y` = ?2 AND `from_direction` = ?3",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, curSeg.x);
      sqlite3_bind_int64 (stmt, 2, curSeg.y);
      sqlite3_bind_text (stmt, 3, dir.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_step (stmt);
      const int64_t linkExists = sqlite3_column_int64 (stmt, 0);
      sqlite3_finalize (stmt);

      if (linkExists > 0)
        {
          LOG (WARNING) << "Segment " << curSeg
                        << " already has a link " << dir;
          return;
        }

      /* Check the target world coordinate isn't already occupied.  */
      const SegmentKey target = Neighbour (curSeg, dir);
      if (SegmentExists (db, target))
        {
          LOG (WARNING) << "World position " << target
                        << " already has a segment";
          return;
        }
    }

  ProcessDiscover (name, depth, txid, dir);
}

void
MoveParser::HandleVisit (const std::string& name, const Json::Value& op)
{
  if (!op.isObject ())
    {
      LOG (WARNING) << "Invalid visit move: " << op;
      return;
    }

  SegmentKey seg;
  if (!ParseSegmentRef (op, seg))
    {
      LOG (WARNING) << "Visit move missing segment coordinate: " << op;
      return;
    }

  if (!PlayerExists (db, name))
    {
      LOG (WARNING) << "Player " << name << " not registered";
      return;
    }

  if (PlayerInActiveVisit (db, name))
    {
      LOG (WARNING) << "Player " << name << " already in an active visit";
      return;
    }

  if (!SegmentExists (db, seg))
    {
      LOG (WARNING) << "Segment " << seg << " does not exist";
      return;
    }

  /* Check no open or active visit already exists for this segment.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT COUNT(*) FROM `visits`"
    " WHERE `segment_x` = ?1 AND `segment_y` = ?2"
    " AND (`status` = 'open' OR `status` = 'active')",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, seg.x);
  sqlite3_bind_int64 (stmt, 2, seg.y);
  sqlite3_step (stmt);
  const int64_t activeVisits = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);

  if (activeVisits > 0)
    {
      LOG (WARNING) << "Segment " << seg
                    << " already has an open or active visit";
      return;
    }

  ProcessVisit (name, seg);
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
      LOG (WARNING) << "Join move missing visit id: " << op;
      return;
    }

  const int64_t visitId = op["id"].asInt64 ();

  if (!PlayerExists (db, name))
    {
      LOG (WARNING) << "Player " << name << " not registered";
      return;
    }

  if (PlayerInActiveVisit (db, name))
    {
      LOG (WARNING) << "Player " << name << " already in an active visit";
      return;
    }

  /* Check visit exists and is open.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT v.`status`, s.`max_players`,"
    " (SELECT COUNT(*) FROM `visit_participants`"
    "  WHERE `visit_id` = ?1)"
    " FROM `visits` v"
    " JOIN `segments` s"
    "   ON v.`segment_x` = s.`world_x` AND v.`segment_y` = s.`world_y`"
    " WHERE v.`id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visitId);

  if (sqlite3_step (stmt) != SQLITE_ROW)
    {
      sqlite3_finalize (stmt);
      LOG (WARNING) << "Visit " << visitId << " does not exist";
      return;
    }

  const std::string status
      = reinterpret_cast<const char*> (sqlite3_column_text (stmt, 0));
  const int64_t maxPlayers = sqlite3_column_int64 (stmt, 1);
  const int64_t currentPlayers = sqlite3_column_int64 (stmt, 2);
  sqlite3_finalize (stmt);

  if (status != "open")
    {
      LOG (WARNING) << "Visit " << visitId << " is not open (status: "
                    << status << ")";
      return;
    }

  if (currentPlayers >= maxPlayers)
    {
      LOG (WARNING) << "Visit " << visitId << " is full";
      return;
    }

  /* Check player not already in this visit.  */
  sqlite3_prepare_v2 (db,
    "SELECT COUNT(*) FROM `visit_participants`"
    " WHERE `visit_id` = ?1 AND `name` = ?2",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visitId);
  sqlite3_bind_text (stmt, 2, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  const int64_t already = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);

  if (already > 0)
    {
      LOG (WARNING) << "Player " << name << " already in visit " << visitId;
      return;
    }

  ProcessJoin (name, visitId);
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
      LOG (WARNING) << "Leave move missing visit id: " << op;
      return;
    }

  const int64_t visitId = op["id"].asInt64 ();

  /* Check visit is open.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `status`, `initiator` FROM `visits` WHERE `id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visitId);

  if (sqlite3_step (stmt) != SQLITE_ROW)
    {
      sqlite3_finalize (stmt);
      LOG (WARNING) << "Visit " << visitId << " does not exist";
      return;
    }

  const std::string status
      = reinterpret_cast<const char*> (sqlite3_column_text (stmt, 0));
  const std::string initiator
      = reinterpret_cast<const char*> (sqlite3_column_text (stmt, 1));
  sqlite3_finalize (stmt);

  if (status != "open")
    {
      LOG (WARNING) << "Cannot leave visit " << visitId
                    << " (status: " << status << ")";
      return;
    }

  if (name == initiator)
    {
      LOG (WARNING) << "Initiator " << name
                    << " cannot leave their own visit";
      return;
    }

  /* Check player is actually in this visit.  */
  sqlite3_prepare_v2 (db,
    "SELECT COUNT(*) FROM `visit_participants`"
    " WHERE `visit_id` = ?1 AND `name` = ?2",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visitId);
  sqlite3_bind_text (stmt, 2, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  const int64_t count = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);

  if (count == 0)
    {
      LOG (WARNING) << "Player " << name
                    << " is not in visit " << visitId;
      return;
    }

  ProcessLeave (name, visitId);
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
      LOG (WARNING) << "Settle move missing visit id: " << op;
      return;
    }

  const int64_t visitId = op["id"].asInt64 ();

  if (!op.isMember ("results") || !op["results"].isArray ())
    {
      LOG (WARNING) << "Settle move missing results array: " << op;
      return;
    }

  /* Check visit exists and is active.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `status`, `initiator` FROM `visits` WHERE `id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visitId);

  if (sqlite3_step (stmt) != SQLITE_ROW)
    {
      sqlite3_finalize (stmt);
      LOG (WARNING) << "Visit " << visitId << " does not exist";
      return;
    }

  const std::string status
      = reinterpret_cast<const char*> (sqlite3_column_text (stmt, 0));
  const std::string initiator
      = reinterpret_cast<const char*> (sqlite3_column_text (stmt, 1));
  sqlite3_finalize (stmt);

  if (status != "active")
    {
      LOG (WARNING) << "Visit " << visitId
                    << " is not active (status: " << status << ")";
      return;
    }

  if (name != initiator)
    {
      LOG (WARNING) << "Only initiator " << initiator
                    << " can settle visit " << visitId
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
        "SELECT COUNT(*) FROM `visit_participants`"
        " WHERE `visit_id` = ?1 AND `name` = ?2",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, visitId);
      sqlite3_bind_text (stmt, 2, playerName.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_step (stmt);
      const int64_t count = sqlite3_column_int64 (stmt, 0);
      sqlite3_finalize (stmt);

      if (count == 0)
        {
          LOG (WARNING) << "Player " << playerName
                        << " is not a participant of visit " << visitId;
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

  ProcessSettle (name, visitId, results);
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

void
MoveParser::HandleTravel (const std::string& name, const std::string& txid,
                           const Json::Value& op)
{
  if (!op.isObject ())
    {
      LOG (WARNING) << "Invalid travel move: " << op;
      return;
    }

  if (!op.isMember ("dir") || !op["dir"].isString ())
    {
      LOG (WARNING) << "Travel move missing dir: " << op;
      return;
    }

  const std::string dir = op["dir"].asString ();
  if (dir != "north" && dir != "south" && dir != "east" && dir != "west")
    {
      LOG (WARNING) << "Invalid travel direction: " << dir;
      return;
    }

  if (!PlayerExists (db, name))
    {
      LOG (WARNING) << "Player " << name << " not registered";
      return;
    }

  if (PlayerInChannel (db, name))
    {
      LOG (WARNING) << "Player " << name << " is in a channel";
      return;
    }

  if (PlayerInActiveVisit (db, name))
    {
      LOG (WARNING) << "Player " << name << " is in an active visit";
      return;
    }

  /* Check HP > 0.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `hp` FROM `players` WHERE `name` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  const int64_t hp = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);

  const SegmentKey curSeg = CurrentSegment (db, name);

  if (hp <= 0)
    {
      LOG (WARNING) << "Player " << name << " has 0 HP, cannot travel";
      return;
    }

  /* Check link exists and destination is confirmed (not provisional).
     The hub has no `segments` row, so a link into it reads as confirmed.  */
  sqlite3_prepare_v2 (db,
    "SELECT COALESCE(s.`confirmed`, 1)"
    " FROM `segment_links` sl"
    " LEFT JOIN `segments` s"
    "   ON sl.`to_x` = s.`world_x` AND sl.`to_y` = s.`world_y`"
    " WHERE sl.`from_x` = ?1 AND sl.`from_y` = ?2"
    "   AND sl.`from_direction` = ?3",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, curSeg.x);
  sqlite3_bind_int64 (stmt, 2, curSeg.y);
  sqlite3_bind_text (stmt, 3, dir.c_str (), -1, SQLITE_TRANSIENT);

  if (sqlite3_step (stmt) != SQLITE_ROW)
    {
      sqlite3_finalize (stmt);
      LOG (WARNING) << "No link from segment " << curSeg
                    << " in direction " << dir;
      return;
    }

  const int64_t confirmed = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);

  if (!confirmed)
    {
      LOG (WARNING) << "Destination segment is provisional (not confirmed)";
      return;
    }

  ProcessTravel (name, dir, txid);
}

void
MoveParser::HandleUseItem (const std::string& name, const Json::Value& op)
{
  if (!op.isObject ())
    {
      LOG (WARNING) << "Invalid use item move: " << op;
      return;
    }

  if (!op.isMember ("item") || !op["item"].isString ())
    {
      LOG (WARNING) << "Use item move missing item: " << op;
      return;
    }

  const std::string itemId = op["item"].asString ();

  if (!PlayerExists (db, name))
    {
      LOG (WARNING) << "Player " << name << " not registered";
      return;
    }

  if (PlayerInChannel (db, name))
    {
      LOG (WARNING) << "Player " << name << " is in a channel";
      return;
    }

  /* Check player has the item in bag with qty >= 1.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `quantity` FROM `inventory`"
    " WHERE `name` = ?1 AND `item_id` = ?2 AND `slot` = 'bag'"
    " LIMIT 1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text (stmt, 2, itemId.c_str (), -1, SQLITE_TRANSIENT);

  if (sqlite3_step (stmt) != SQLITE_ROW)
    {
      sqlite3_finalize (stmt);
      LOG (WARNING) << "Player " << name << " has no " << itemId << " in bag";
      return;
    }

  const int64_t qty = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);

  if (qty <= 0)
    {
      LOG (WARNING) << "Player " << name << " has no " << itemId;
      return;
    }

  ProcessUseItem (name, itemId);
}

void
MoveParser::HandleEquip (const std::string& name, const Json::Value& op)
{
  if (!op.isObject ())
    {
      LOG (WARNING) << "Invalid equip move: " << op;
      return;
    }

  if (!op.isMember ("rowid") || !op["rowid"].isInt64 ())
    {
      LOG (WARNING) << "Equip move missing rowid: " << op;
      return;
    }
  if (!op.isMember ("slot") || !op["slot"].isString ())
    {
      LOG (WARNING) << "Equip move missing slot: " << op;
      return;
    }

  const int64_t rowid = op["rowid"].asInt64 ();
  const std::string slot = op["slot"].asString ();

  if (slot != "weapon" && slot != "offhand" && slot != "head"
      && slot != "body" && slot != "feet" && slot != "ring"
      && slot != "amulet")
    {
      LOG (WARNING) << "Invalid equip slot: " << slot;
      return;
    }

  if (!PlayerExists (db, name))
    {
      LOG (WARNING) << "Player " << name << " not registered";
      return;
    }

  if (PlayerInChannel (db, name))
    {
      LOG (WARNING) << "Player " << name << " is in a channel";
      return;
    }

  /* Check item belongs to player and is in bag.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `slot` FROM `inventory`"
    " WHERE `rowid` = ?1 AND `name` = ?2",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, rowid);
  sqlite3_bind_text (stmt, 2, name.c_str (), -1, SQLITE_TRANSIENT);

  if (sqlite3_step (stmt) != SQLITE_ROW)
    {
      sqlite3_finalize (stmt);
      LOG (WARNING) << "Item " << rowid << " not found for " << name;
      return;
    }

  const std::string currentSlot
      = reinterpret_cast<const char*> (sqlite3_column_text (stmt, 0));
  sqlite3_finalize (stmt);

  if (currentSlot != "bag")
    {
      LOG (WARNING) << "Item " << rowid << " is not in bag (in " << currentSlot << ")";
      return;
    }

  ProcessEquip (name, rowid, slot);
}

void
MoveParser::HandleUnequip (const std::string& name, const Json::Value& op)
{
  if (!op.isObject ())
    {
      LOG (WARNING) << "Invalid unequip move: " << op;
      return;
    }

  if (!op.isMember ("rowid") || !op["rowid"].isInt64 ())
    {
      LOG (WARNING) << "Unequip move missing rowid: " << op;
      return;
    }

  const int64_t rowid = op["rowid"].asInt64 ();

  if (!PlayerExists (db, name))
    {
      LOG (WARNING) << "Player " << name << " not registered";
      return;
    }

  if (PlayerInChannel (db, name))
    {
      LOG (WARNING) << "Player " << name << " is in a channel";
      return;
    }

  /* Check item belongs to player and is NOT in bag.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `slot` FROM `inventory`"
    " WHERE `rowid` = ?1 AND `name` = ?2",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, rowid);
  sqlite3_bind_text (stmt, 2, name.c_str (), -1, SQLITE_TRANSIENT);

  if (sqlite3_step (stmt) != SQLITE_ROW)
    {
      sqlite3_finalize (stmt);
      LOG (WARNING) << "Item " << rowid << " not found for " << name;
      return;
    }

  const std::string currentSlot
      = reinterpret_cast<const char*> (sqlite3_column_text (stmt, 0));
  sqlite3_finalize (stmt);

  if (currentSlot == "bag")
    {
      LOG (WARNING) << "Item " << rowid << " is already in bag";
      return;
    }

  ProcessUnequip (name, rowid);
}

void
MoveParser::HandleDiscard (const std::string& name, const Json::Value& op)
{
  if (!op.isObject ())
    {
      LOG (WARNING) << "Invalid discard move: " << op;
      return;
    }

  if (!op.isMember ("rowid") || !op["rowid"].isInt64 ())
    {
      LOG (WARNING) << "Discard move missing rowid: " << op;
      return;
    }

  const int64_t rowid = op["rowid"].asInt64 ();

  if (!PlayerExists (db, name))
    {
      LOG (WARNING) << "Player " << name << " not registered";
      return;
    }

  if (PlayerInChannel (db, name))
    {
      LOG (WARNING) << "Player " << name << " is in a channel";
      return;
    }

  /* Only bag items can be discarded.  Equipped gear must be unequipped
     first, so a discard never silently changes the player's stats.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `slot` FROM `inventory`"
    " WHERE `rowid` = ?1 AND `name` = ?2",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, rowid);
  sqlite3_bind_text (stmt, 2, name.c_str (), -1, SQLITE_TRANSIENT);

  if (sqlite3_step (stmt) != SQLITE_ROW)
    {
      sqlite3_finalize (stmt);
      LOG (WARNING) << "Item " << rowid << " not found for " << name;
      return;
    }

  const std::string currentSlot
      = reinterpret_cast<const char*> (sqlite3_column_text (stmt, 0));
  sqlite3_finalize (stmt);

  if (currentSlot != "bag")
    {
      LOG (WARNING) << "Item " << rowid << " is equipped; unequip before "
                    << "discarding";
      return;
    }

  ProcessDiscardItem (name, rowid);
}

void
MoveParser::HandleEnterChannel (const std::string& name, const Json::Value& op)
{
  if (!op.isObject ())
    {
      LOG (WARNING) << "Invalid enter channel move: " << op;
      return;
    }

  SegmentKey seg;
  if (!ParseSegmentRef (op, seg))
    {
      LOG (WARNING) << "Enter channel missing segment coordinate: " << op;
      return;
    }

  if (!PlayerExists (db, name))
    {
      LOG (WARNING) << "Player " << name << " not registered";
      return;
    }

  if (PlayerInChannel (db, name))
    {
      LOG (WARNING) << "Player " << name << " already in a channel";
      return;
    }

  if (PlayerInActiveVisit (db, name))
    {
      LOG (WARNING) << "Player " << name << " is in an active visit";
      return;
    }

  /* Check HP > 0.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `hp` FROM `players` WHERE `name` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  const int64_t hp = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);

  if (hp <= 0)
    {
      LOG (WARNING) << "Player " << name << " has 0 HP, cannot enter channel";
      return;
    }

  const SegmentKey curSeg = CurrentSegment (db, name);

  /* Player must be at the segment, OR be the discoverer of a provisional
     segment linked from their current segment (so they can enter to
     confirm it).  */
  if (curSeg != seg)
    {
      /* Check if this is the discoverer entering a linked provisional segment.  */
      sqlite3_prepare_v2 (db,
        "SELECT s.`discoverer`, s.`confirmed` FROM `segments` s"
        " JOIN `segment_links` sl"
        "   ON sl.`to_x` = s.`world_x` AND sl.`to_y` = s.`world_y`"
        " WHERE s.`world_x` = ?1 AND s.`world_y` = ?2"
        "   AND sl.`from_x` = ?3 AND sl.`from_y` = ?4",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, seg.x);
      sqlite3_bind_int64 (stmt, 2, seg.y);
      sqlite3_bind_int64 (stmt, 3, curSeg.x);
      sqlite3_bind_int64 (stmt, 4, curSeg.y);

      bool allowed = false;
      if (sqlite3_step (stmt) == SQLITE_ROW)
        {
          const std::string discoverer = reinterpret_cast<const char*> (
              sqlite3_column_text (stmt, 0));
          const int64_t confirmed = sqlite3_column_int64 (stmt, 1);
          if (discoverer == name && !confirmed)
            allowed = true;
        }
      sqlite3_finalize (stmt);

      if (!allowed)
        {
          LOG (WARNING) << "Player " << name << " is at segment " << curSeg
                        << ", not " << seg;
          return;
        }
    }

  /* Segment must exist.  The hub, (0, 0), has no row and so is not
     enterable as a channel -- it is played locally, with no visit.  */
  if (!SegmentExists (db, seg))
    {
      LOG (WARNING) << "Segment " << seg << " does not exist";
      return;
    }

  /* `ec` carries no direction, so the player spawns at the room centre.  */
  ProcessEnterChannel (name, seg, "");
}

void
MoveParser::HandleExitChannel (const std::string& name, const Json::Value& op)
{
  if (!op.isObject ())
    {
      LOG (WARNING) << "Invalid exit channel move: " << op;
      return;
    }

  if (!op.isMember ("id") || !op["id"].isInt64 ())
    {
      LOG (WARNING) << "Exit channel missing visit id: " << op;
      return;
    }

  const int64_t visitId = op["id"].asInt64 ();

  if (!PlayerExists (db, name))
    {
      LOG (WARNING) << "Player " << name << " not registered";
      return;
    }

  if (!PlayerInChannel (db, name))
    {
      LOG (WARNING) << "Player " << name << " is not in a channel";
      return;
    }

  if (!op.isMember ("results") || !op["results"].isObject ())
    {
      LOG (WARNING) << "Exit channel missing results: " << op;
      return;
    }

  if (!op.isMember ("actions") || !op["actions"].isArray ())
    {
      LOG (WARNING) << "Exit channel missing actions proof: " << op;
      return;
    }

  /* Check visit exists and is active.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `status`, `initiator` FROM `visits` WHERE `id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visitId);

  if (sqlite3_step (stmt) != SQLITE_ROW)
    {
      sqlite3_finalize (stmt);
      LOG (WARNING) << "Visit " << visitId << " does not exist";
      return;
    }

  const std::string status
      = reinterpret_cast<const char*> (sqlite3_column_text (stmt, 0));
  const std::string initiator
      = reinterpret_cast<const char*> (sqlite3_column_text (stmt, 1));
  sqlite3_finalize (stmt);

  if (status != "active")
    {
      LOG (WARNING) << "Visit " << visitId << " is not active";
      return;
    }

  if (name != initiator)
    {
      LOG (WARNING) << "Only initiator can exit channel visit " << visitId;
      return;
    }

  ProcessExitChannel (name, visitId, op["results"], op["actions"]);
}

// ----------------------------------------------------------------
// Gate-walk: atomic settle + transit + enter-channel.
// ----------------------------------------------------------------

void
MoveParser::HandleGateWalk (const std::string& name, const std::string& txid,
                             const Json::Value& op)
{
  if (!op.isObject ())
    {
      LOG (WARNING) << "Invalid gate-walk move: " << op;
      return;
    }

  if (!op.isMember ("dir") || !op["dir"].isString ())
    {
      LOG (WARNING) << "Gate-walk missing dir: " << op;
      return;
    }

  const std::string dir = op["dir"].asString ();
  if (dir != "north" && dir != "south" && dir != "east" && dir != "west")
    {
      LOG (WARNING) << "Invalid gate-walk direction: " << dir;
      return;
    }

  if (!PlayerExists (db, name))
    {
      LOG (WARNING) << "Player " << name << " not registered";
      return;
    }

  /* Load player state.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `in_channel`, `hp`, `current_x`, `current_y`,"
    " `last_discover_height`"
    " FROM `players` WHERE `name` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  const bool inChannel = sqlite3_column_int64 (stmt, 0) != 0;
  const int64_t hp = sqlite3_column_int64 (stmt, 1);
  const SegmentKey curSeg (
      static_cast<int> (sqlite3_column_int64 (stmt, 2)),
      static_cast<int> (sqlite3_column_int64 (stmt, 3)));
  const unsigned lastDiscover
      = static_cast<unsigned> (sqlite3_column_int64 (stmt, 4));
  sqlite3_finalize (stmt);

  if (hp <= 0)
    {
      LOG (WARNING) << name << " has 0 HP, cannot gate-walk";
      return;
    }

  const bool hasSettlement = op.isMember ("settlement");
  const bool transit = op.get ("transit", false).asBool ();

  /* Transit-only gate-walk: a free, no-settlement pass between
     already-confirmed segments (see the "Traversal model" in CLAUDE.md).
     Crossing the frontier (a provisional segment) still requires a settled
     run to confirm it, so transit-leave is allowed only from a confirmed
     segment.  A transit move must not also carry a settlement.  */
  if (inChannel && transit)
    {
      if (hasSettlement)
        {
          LOG (WARNING) << name << " gate-walk: transit move must not carry a "
                        << "settlement";
          return;
        }
      /* The hub is always confirmed and has no row of its own.  */
      bool curConfirmed = curSeg.IsHub ();
      if (!curConfirmed)
        {
          sqlite3_prepare_v2 (db,
            "SELECT `confirmed` FROM `segments`"
            " WHERE `world_x` = ?1 AND `world_y` = ?2",
            -1, &stmt, nullptr);
          sqlite3_bind_int64 (stmt, 1, curSeg.x);
          sqlite3_bind_int64 (stmt, 2, curSeg.y);
          if (sqlite3_step (stmt) == SQLITE_ROW)
            curConfirmed = sqlite3_column_int64 (stmt, 0) != 0;
          sqlite3_finalize (stmt);
        }
      if (!curConfirmed)
        {
          LOG (WARNING) << name << " gate-walk: cannot transit-leave "
                        << "provisional segment " << curSeg
                        << " (complete a run to confirm it first)";
          return;
        }
    }
  else if (inChannel && !hasSettlement)
    {
      LOG (WARNING) << name << " is in channel but gate-walk has no settlement";
      return;
    }
  if (!inChannel && hasSettlement)
    {
      LOG (WARNING) << name << " not in channel but gate-walk has settlement";
      return;
    }
  if (!inChannel && PlayerInActiveVisit (db, name))
    {
      LOG (WARNING) << name << " is in an active visit; cannot gate-walk";
      return;
    }

  /* Validate settlement object shape if present.  Replay verification
     and exit-gate consistency are deferred to ProcessGateWalk via
     ApplySettlementBody.  */
  if (hasSettlement)
    {
      const auto& s = op["settlement"];
      if (!s.isObject ()
          || !s.isMember ("results") || !s["results"].isObject ()
          || !s.isMember ("actions") || !s["actions"].isArray ())
        {
          LOG (WARNING) << "Gate-walk settlement malformed: " << s;
          return;
        }
      /* gw is only for live transitions.  Death uses xc which applies
         the death penalty (respawn at hub, 25% gold loss).  */
      const bool claimedSurvived
          = s["results"].get ("survived", false).asBool ();
      if (!claimedSurvived)
        {
          LOG (WARNING) << name << " claimed survived=false in gate-walk; "
                        << "use xc for death.";
          return;
        }
    }

  /* The destination is simply the coordinate one step away: a gate always
     leads to the neighbouring cell, whether or not a link row exists yet.  */
  const SegmentKey target = Neighbour (curSeg, dir);

  if (target.IsHub ())
    {
      /* Walking back to the world origin is always allowed.  */
    }
  else
    {
      sqlite3_prepare_v2 (db,
        "SELECT `confirmed`, `discoverer` FROM `segments`"
        " WHERE `world_x` = ?1 AND `world_y` = ?2",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, target.x);
      sqlite3_bind_int64 (stmt, 2, target.y);
      bool occupied = false, occConfirmed = false;
      std::string occDiscoverer;
      if (sqlite3_step (stmt) == SQLITE_ROW)
        {
          occupied = true;
          occConfirmed = sqlite3_column_int64 (stmt, 0) != 0;
          const unsigned char* d = sqlite3_column_text (stmt, 1);
          if (d != nullptr)
            occDiscoverer = reinterpret_cast<const char*> (d);
        }
      sqlite3_finalize (stmt);

      if (occupied)
        {
          /* Confirmed neighbour -> free transit (see the "Traversal model"
             in CLAUDE.md).  It is not a discovery, so no cooldown;
             ProcessGateWalk creates the link row if it is missing.
             Provisional neighbour -> discoverer-only.  */
          if (!occConfirmed && occDiscoverer != name)
            {
              LOG (WARNING) << name << " gate-walk: target " << target
                            << " holds a provisional segment discovered by "
                            << occDiscoverer;
              return;
            }
          /* else: allowed; fall through to ProcessGateWalk.  */
        }
      else
        {
          /* Empty coord -> genuine frontier discovery.  Cooldown applies;
             the coordinate race is resolved by the primary key.  */
          if (lastDiscover > 0
              && currentHeight < lastDiscover + 50)
            {
              LOG (WARNING) << name << " gate-walk: discovery cooldown active"
                            << " (last=" << lastDiscover
                            << " now=" << currentHeight << ")";
              return;
            }
        }
    }

  ProcessGateWalk (name, txid, dir,
                   hasSettlement ? op["settlement"] : Json::Value ());
}

} // namespace rog
