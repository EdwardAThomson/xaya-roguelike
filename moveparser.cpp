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

  /* Direction is optional.  If provided, the new segment is linked
     to the player's current segment via that gate direction.  */
  std::string dir;
  if (op.isMember ("dir"))
    {
      if (!op["dir"].isString ())
        {
          LOG (WARNING) << "Invalid dir in discover: " << op;
          return;
        }
      dir = op["dir"].asString ();
      if (dir != "north" && dir != "south" && dir != "east" && dir != "west")
        {
          LOG (WARNING) << "Invalid direction: " << dir;
          return;
        }

      /* Check the player's current segment doesn't already have a link
         in that direction.  */
      sqlite3_stmt* stmt;
      sqlite3_prepare_v2 (db,
        "SELECT `current_segment` FROM `players` WHERE `name` = ?1",
        -1, &stmt, nullptr);
      sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_step (stmt);
      const int64_t curSeg = sqlite3_column_int64 (stmt, 0);
      sqlite3_finalize (stmt);

      sqlite3_prepare_v2 (db,
        "SELECT COUNT(*) FROM `segment_links`"
        " WHERE `from_segment` = ?1 AND `from_direction` = ?2",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, curSeg);
      sqlite3_bind_text (stmt, 2, dir.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_step (stmt);
      const int64_t linkExists = sqlite3_column_int64 (stmt, 0);
      sqlite3_finalize (stmt);

      if (linkExists > 0)
        {
          LOG (WARNING) << "Segment " << curSeg
                        << " already has a link " << dir;
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

  if (!op.isMember ("id") || !op["id"].isInt64 ())
    {
      LOG (WARNING) << "Visit move missing segment id: " << op;
      return;
    }

  const int64_t segmentId = op["id"].asInt64 ();

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

  /* Check segment exists.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT COUNT(*) FROM `segments` WHERE `id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, segmentId);
  sqlite3_step (stmt);
  const int64_t count = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);

  if (count == 0)
    {
      LOG (WARNING) << "Segment " << segmentId << " does not exist";
      return;
    }

  /* Check no open or active visit already exists for this segment.  */
  sqlite3_prepare_v2 (db,
    "SELECT COUNT(*) FROM `visits`"
    " WHERE `segment_id` = ?1"
    " AND (`status` = 'open' OR `status` = 'active')",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, segmentId);
  sqlite3_step (stmt);
  const int64_t activeVisits = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);

  if (activeVisits > 0)
    {
      LOG (WARNING) << "Segment " << segmentId
                    << " already has an open or active visit";
      return;
    }

  ProcessVisit (name, segmentId);
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
    " JOIN `segments` s ON v.`segment_id` = s.`id`"
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
    "SELECT `hp`, `current_segment` FROM `players` WHERE `name` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  const int64_t hp = sqlite3_column_int64 (stmt, 0);
  const int64_t curSeg = sqlite3_column_int64 (stmt, 1);
  sqlite3_finalize (stmt);

  if (hp <= 0)
    {
      LOG (WARNING) << "Player " << name << " has 0 HP, cannot travel";
      return;
    }

  /* Check link exists in that direction.  */
  sqlite3_prepare_v2 (db,
    "SELECT COUNT(*) FROM `segment_links`"
    " WHERE `from_segment` = ?1 AND `from_direction` = ?2",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, curSeg);
  sqlite3_bind_text (stmt, 2, dir.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  const int64_t linkCount = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);

  if (linkCount == 0)
    {
      LOG (WARNING) << "No link from segment " << curSeg
                    << " in direction " << dir;
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
MoveParser::HandleEnterChannel (const std::string& name, const Json::Value& op)
{
  if (!op.isObject ())
    {
      LOG (WARNING) << "Invalid enter channel move: " << op;
      return;
    }

  if (!op.isMember ("id") || !op["id"].isInt64 ())
    {
      LOG (WARNING) << "Enter channel missing segment id: " << op;
      return;
    }

  const int64_t segmentId = op["id"].asInt64 ();

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
    "SELECT `hp`, `current_segment` FROM `players` WHERE `name` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  const int64_t hp = sqlite3_column_int64 (stmt, 0);
  const int64_t curSeg = sqlite3_column_int64 (stmt, 1);
  sqlite3_finalize (stmt);

  if (hp <= 0)
    {
      LOG (WARNING) << "Player " << name << " has 0 HP, cannot enter channel";
      return;
    }

  /* Player must be at the segment they want to enter.  */
  if (curSeg != segmentId)
    {
      LOG (WARNING) << "Player " << name << " is at segment " << curSeg
                    << ", not " << segmentId;
      return;
    }

  /* Segment must exist (not origin 0).  */
  sqlite3_prepare_v2 (db,
    "SELECT COUNT(*) FROM `segments` WHERE `id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, segmentId);
  sqlite3_step (stmt);
  const int64_t segExists = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);

  if (segExists == 0)
    {
      LOG (WARNING) << "Segment " << segmentId << " does not exist";
      return;
    }

  ProcessEnterChannel (name, segmentId);
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

  ProcessExitChannel (name, visitId, op["results"]);
}

} // namespace rog
