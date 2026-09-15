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

/**
 * True iff the segment has a gate in the given direction.  The hub has all
 * four and no `segment_gates` rows, so callers check IsHub() first.
 */
bool
GateExists (sqlite3* db, const SegmentKey& seg, const std::string& dir)
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT COUNT(*) FROM `segment_gates`"
    " WHERE `segment_x` = ?1 AND `segment_y` = ?2 AND `direction` = ?3",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, seg.x);
  sqlite3_bind_int64 (stmt, 2, seg.y);
  sqlite3_bind_text (stmt, 3, dir.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  const int64_t count = sqlite3_column_int64 (stmt, 0);
  sqlite3_finalize (stmt);
  return count > 0;
}

/** True iff the segment exists and is confirmed (the hub counts).  */
bool
SegmentConfirmed (sqlite3* db, const SegmentKey& seg)
{
  if (seg.IsHub ())
    return true;
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `confirmed` FROM `segments`"
    " WHERE `world_x` = ?1 AND `world_y` = ?2",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, seg.x);
  sqlite3_bind_int64 (stmt, 2, seg.y);
  bool confirmed = false;
  if (sqlite3_step (stmt) == SQLITE_ROW)
    confirmed = sqlite3_column_int64 (stmt, 0) != 0;
  sqlite3_finalize (stmt);
  return confirmed;
}

/**
 * Shape check for a settlement body carried by a move that walks through a
 * gate (`gw`, and the co-op `v`/`j`).  Those moves are live transitions, so
 * the claim must be a survived exit; a death uses `xc`, which applies the
 * death penalty.
 */
bool
ValidSettlementBody (const Json::Value& s)
{
  if (!s.isObject ()
      || !s.isMember ("results") || !s["results"].isObject ()
      || !s.isMember ("actions")
      || !(s["actions"].isArray () || s["actions"].isString ()))
    return false;
  return s["results"].get ("survived", false).asBool ();
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
  else if (mv.isMember ("sc"))
    HandleSettleConfirm (name, mv["sc"]);
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

/**
 * Host a co-op run on the confirmed segment through one of the gates where
 * the player is standing (SPEC_multiplayer_coop.md section 8a).  Hosting is
 * a gate-walk that waits: from inside a run it settles that run and leaves
 * the player standing at their segment with the door open; from out of a
 * run (the hub, or a segment they are standing in after an earlier run)
 * there is nothing to settle.  Nobody teleports: the visit opens on the
 * cell next door, and the host walks in through that gate when it fills.
 */
void
MoveParser::HandleVisit (const std::string& name, const Json::Value& op)
{
  if (!op.isObject ())
    {
      LOG (WARNING) << "Invalid visit move: " << op;
      return;
    }

  if (!op.isMember ("dir") || !op["dir"].isString ())
    {
      LOG (WARNING) << "Visit move missing dir: " << op;
      return;
    }
  const std::string dir = op["dir"].asString ();
  if (dir != "north" && dir != "south" && dir != "east" && dir != "west")
    {
      LOG (WARNING) << "Invalid visit direction: " << dir;
      return;
    }

  /* Duel options (SPEC_multiplayer_pvp.md section 1).  Absent `mode` is a
     co-op run, which is exactly the behaviour every existing visit had.  */
  std::string mode = "coop";
  if (op.isMember ("mode"))
    {
      if (!op["mode"].isString ())
        {
          LOG (WARNING) << "Visit move has a non-string mode: " << op;
          return;
        }
      mode = op["mode"].asString ();
      if (mode != "coop" && mode != "duel")
        {
          LOG (WARNING) << "Unknown visit mode: " << mode;
          return;
        }
    }

  int64_t stake = 0;
  if (op.isMember ("stake"))
    {
      if (!op["stake"].isInt64 () || op["stake"].asInt64 () < 0)
        {
          LOG (WARNING) << "Visit move has an invalid stake: " << op;
          return;
        }
      stake = op["stake"].asInt64 ();
    }
  if (mode != "duel" && stake != 0)
    {
      LOG (WARNING) << "Visit move stakes gold outside a duel: " << op;
      return;
    }

  if (!PlayerExists (db, name))
    {
      LOG (WARNING) << "Player " << name << " not registered";
      return;
    }

  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `in_channel`, `hp`, `current_x`, `current_y`, `gold`"
    " FROM `players` WHERE `name` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  const bool inChannel = sqlite3_column_int64 (stmt, 0) != 0;
  const int64_t hp = sqlite3_column_int64 (stmt, 1);
  const SegmentKey curSeg (
      static_cast<int> (sqlite3_column_int64 (stmt, 2)),
      static_cast<int> (sqlite3_column_int64 (stmt, 3)));
  const int64_t gold = sqlite3_column_int64 (stmt, 4);
  sqlite3_finalize (stmt);

  if (hp <= 0)
    {
      LOG (WARNING) << name << " has 0 HP, cannot host a co-op run";
      return;
    }

  const bool hasSettlement = op.isMember ("settlement");
  if (inChannel && !hasSettlement)
    {
      LOG (WARNING) << name << " is in a run and must settle it (walk out "
                    << "through the gate) to host a co-op run";
      return;
    }
  if (!inChannel && hasSettlement)
    {
      LOG (WARNING) << name << " is not in a run but sent a settlement";
      return;
    }
  if (hasSettlement && !ValidSettlementBody (op["settlement"]))
    {
      LOG (WARNING) << "Visit settlement malformed, or not a survived exit: "
                    << op["settlement"];
      return;
    }

  /* Out of a run, ANY open or active visit blocks hosting another: waiting
     as a host already, or playing a co-op run.  In a channel the only visit
     is the solo run the settlement closes.  */
  if (!inChannel && PlayerInActiveVisit (db, name))
    {
      LOG (WARNING) << "Player " << name << " is already in a visit";
      return;
    }

  const SegmentKey target = Neighbour (curSeg, dir);
  if (target.IsHub ())
    {
      LOG (WARNING) << name << " cannot host a co-op run in the hub";
      return;
    }
  if (!SegmentExists (db, target))
    {
      LOG (WARNING) << "Segment " << target << " does not exist";
      return;
    }

  /* Co-op runs are restricted to confirmed segments (spec section 8): the
     provisional-confirmation flow stays solo-only, so who confirms a group
     discovery never arises.  */
  if (!SegmentConfirmed (db, target))
    {
      LOG (WARNING) << "Segment " << target
                    << " is provisional; co-op runs need a confirmed segment";
      return;
    }

  /* There has to be a gate to walk through.  The hub has all four.  */
  if (!curSeg.IsHub () && !GateExists (db, curSeg, dir))
    {
      LOG (WARNING) << name << " has no " << dir << " gate at " << curSeg;
      return;
    }

  if (mode == "duel")
    {
      /* A duel is 1v1 (spec section 1), so the arena must seat exactly
         two.  Anything else would need the N-party stake and outcome
         rules that Phase 4a deliberately does not have.  */
      sqlite3_prepare_v2 (db,
        "SELECT `max_players` FROM `segments`"
        " WHERE `world_x` = ?1 AND `world_y` = ?2",
        -1, &stmt, nullptr);
      sqlite3_bind_int64 (stmt, 1, target.x);
      sqlite3_bind_int64 (stmt, 2, target.y);
      sqlite3_step (stmt);
      const int64_t maxPlayers = sqlite3_column_int64 (stmt, 0);
      sqlite3_finalize (stmt);

      if (maxPlayers != 2)
        {
          LOG (WARNING) << "Segment " << target << " seats " << maxPlayers
                        << " players; a duel is 1v1";
          return;
        }

      /* The host antes up front: the stake goes into escrow when the duel
         opens, so the pot is real before anyone can join it.  */
      if (gold < stake)
        {
          LOG (WARNING) << name << " cannot cover a stake of " << stake
                        << " (holds " << gold << ")";
          return;
        }
    }

  ProcessVisit (name, target, dir,
                hasSettlement ? op["settlement"] : Json::Value (),
                mode, stake);
}

/**
 * Join an open co-op run through one of the gates where the player is
 * standing.  The joiner must be adjacent to the visited segment with a gate
 * into it, so the two players meet by walking in from their own sides; the
 * settlement rules are the host's (see HandleVisit).
 */
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

  if (!op.isMember ("dir") || !op["dir"].isString ())
    {
      LOG (WARNING) << "Join move missing dir: " << op;
      return;
    }
  const std::string dir = op["dir"].asString ();
  if (dir != "north" && dir != "south" && dir != "east" && dir != "west")
    {
      LOG (WARNING) << "Invalid join direction: " << dir;
      return;
    }

  if (!PlayerExists (db, name))
    {
      LOG (WARNING) << "Player " << name << " not registered";
      return;
    }

  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `in_channel`, `hp`, `current_x`, `current_y`, `gold`"
    " FROM `players` WHERE `name` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  const bool inChannel = sqlite3_column_int64 (stmt, 0) != 0;
  const int64_t hp = sqlite3_column_int64 (stmt, 1);
  const SegmentKey curSeg (
      static_cast<int> (sqlite3_column_int64 (stmt, 2)),
      static_cast<int> (sqlite3_column_int64 (stmt, 3)));
  const int64_t gold = sqlite3_column_int64 (stmt, 4);
  sqlite3_finalize (stmt);

  if (hp <= 0)
    {
      LOG (WARNING) << name << " has 0 HP, cannot join a co-op run";
      return;
    }

  const bool hasSettlement = op.isMember ("settlement");
  if (inChannel && !hasSettlement)
    {
      LOG (WARNING) << name << " is in a run and must settle it (walk out "
                    << "through the gate) to join a co-op run";
      return;
    }
  if (!inChannel && hasSettlement)
    {
      LOG (WARNING) << name << " is not in a run but sent a settlement";
      return;
    }
  if (hasSettlement && !ValidSettlementBody (op["settlement"]))
    {
      LOG (WARNING) << "Join settlement malformed, or not a survived exit: "
                    << op["settlement"];
      return;
    }
  if (!inChannel && PlayerInActiveVisit (db, name))
    {
      LOG (WARNING) << "Player " << name << " is already in a visit";
      return;
    }

  /* Check the visit exists, is open, and has room.  */
  sqlite3_prepare_v2 (db,
    "SELECT v.`status`, v.`segment_x`, v.`segment_y`, s.`max_players`,"
    " (SELECT COUNT(*) FROM `visit_participants`"
    "  WHERE `visit_id` = ?1), v.`mode`, v.`stake`"
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
  const SegmentKey visitSeg (
      static_cast<int> (sqlite3_column_int64 (stmt, 1)),
      static_cast<int> (sqlite3_column_int64 (stmt, 2)));
  const int64_t maxPlayers = sqlite3_column_int64 (stmt, 3);
  const int64_t currentPlayers = sqlite3_column_int64 (stmt, 4);
  const std::string visitMode
      = reinterpret_cast<const char*> (sqlite3_column_text (stmt, 5));
  const int64_t visitStake = sqlite3_column_int64 (stmt, 6);
  sqlite3_finalize (stmt);

  /* Joining a duel means matching its stake (spec section 5): the joiner
     sees the mode and the stake in the lobby before deciding, and the
     move is rejected outright if they cannot cover it.  */
  if (visitMode == "duel" && gold < visitStake)
    {
      LOG (WARNING) << name << " cannot cover visit " << visitId
                    << "'s stake of " << visitStake << " (holds " << gold
                    << ")";
      return;
    }

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

  /* Adjacency: the gate the joiner walks through must open onto the visited
     segment.  A gate always leads to the cell next door, so this is a plain
     coordinate comparison and no link row can disagree with it.  */
  if (Neighbour (curSeg, dir) != visitSeg)
    {
      LOG (WARNING) << name << " is at " << curSeg << "; walking " << dir
                    << " leads to " << Neighbour (curSeg, dir)
                    << ", not to visit " << visitId << "'s segment "
                    << visitSeg;
      return;
    }
  if (!curSeg.IsHub () && !GateExists (db, curSeg, dir))
    {
      LOG (WARNING) << name << " has no " << dir << " gate at " << curSeg;
      return;
    }

  ProcessJoin (name, visitId, dir,
               hasSettlement ? op["settlement"] : Json::Value ());
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

  /* The initiator leaving an OPEN visit cancels it for everyone (nothing
     is at stake before activation); other participants just drop out.
     Both go through ProcessLeave, which tells them apart.  */
  if (name == initiator)
    {
      ProcessLeave (name, visitId);
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

  /* The merged action log is mandatory: settlement without a replayable
     proof would be a trust-the-client reward faucet (spec §7).  */
  if (!op.isMember ("actions")
      || !(op["actions"].isArray () || op["actions"].isString ()))
    {
      LOG (WARNING) << "Settle move missing merged actions: " << op;
      return;
    }

  /* Optional abandonment settle (spec section 11): the first `solo_from`
     actions are the partner's last checkpoint, the rest the submitter's
     own solo continuation.  */
  int64_t soloFrom = -1;
  if (op.isMember ("solo_from"))
    {
      if (!op["solo_from"].isInt64 () || op["solo_from"].asInt64 () < 0)
        {
          LOG (WARNING) << "Settle move has invalid solo_from: " << op;
          return;
        }
      soloFrom = op["solo_from"].asInt64 ();
    }
  if (op["actions"].isArray ())
    for (const auto& a : op["actions"])
      {
        if (!a.isObject ()
            || !a.isMember ("i") || !a["i"].isInt ()
            || !a.isMember ("type") || !a["type"].isString ())
          {
            LOG (WARNING) << "Invalid merged-log entry in settle move: " << a;
            return;
          }
      }

  /* Check visit exists and is active.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `status` FROM `visits` WHERE `id` = ?1",
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
  sqlite3_finalize (stmt);

  if (status != "active")
    {
      LOG (WARNING) << "Visit " << visitId
                    << " is not active (status: " << status << ")";
      return;
    }

  /* Any participant may submit the settlement (the other participants
     consent via their `sc` confirms, checked in the processor).  */
  sqlite3_prepare_v2 (db,
    "SELECT COUNT(*) FROM `visit_participants`"
    " WHERE `visit_id` = ?1 AND `name` = ?2",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visitId);
  sqlite3_bind_text (stmt, 2, name.c_str (), -1, SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  const bool isParticipant = sqlite3_column_int64 (stmt, 0) > 0;
  sqlite3_finalize (stmt);
  if (!isParticipant)
    {
      LOG (WARNING) << name << " is not a participant of visit " << visitId
                    << " and cannot settle it";
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

      /* Duel outcome claim (SPEC_multiplayer_pvp.md section 7).  The
         processor checks it against the replay's winner; here only the
         shape.  */
      if (r.isMember ("duel"))
        {
          if (!r["duel"].isString ()
              || (r["duel"].asString () != "won"
                  && r["duel"].asString () != "lost"))
            {
              LOG (WARNING) << "Invalid duel outcome in result: " << r;
              return;
            }
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

  ProcessSettle (name, visitId, results, op["actions"], soloFrom);
}

void
MoveParser::HandleSettleConfirm (const std::string& name,
                                  const Json::Value& op)
{
  if (!op.isObject ())
    {
      LOG (WARNING) << "Invalid settle-confirm move: " << op;
      return;
    }

  if (!op.isMember ("id") || !op["id"].isInt64 ())
    {
      LOG (WARNING) << "Settle-confirm missing visit id: " << op;
      return;
    }
  const int64_t visitId = op["id"].asInt64 ();

  /* The hash is 64 lowercase hex chars (SHA-256 of the canonical log
     encoding, spec §7).  */
  if (!op.isMember ("h") || !op["h"].isString ())
    {
      LOG (WARNING) << "Settle-confirm missing hash: " << op;
      return;
    }
  const std::string hash = op["h"].asString ();
  if (hash.size () != 64
      || hash.find_first_not_of ("0123456789abcdef") != std::string::npos)
    {
      LOG (WARNING) << "Settle-confirm hash malformed: " << op;
      return;
    }

  /* The number of actions the hash covers (spec section 11): a checkpoint
     prefix, or the whole log for the final confirm.  */
  if (!op.isMember ("n") || !op["n"].isInt64 () || op["n"].asInt64 () < 0)
    {
      LOG (WARNING) << "Settle-confirm missing/invalid length: " << op;
      return;
    }
  const int64_t len = op["n"].asInt64 ();

  /* Visit must exist and be active, and the sender a participant.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT v.`status`,"
    " (SELECT COUNT(*) FROM `visit_participants`"
    "  WHERE `visit_id` = v.`id` AND `name` = ?2)"
    " FROM `visits` v WHERE v.`id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visitId);
  sqlite3_bind_text (stmt, 2, name.c_str (), -1, SQLITE_TRANSIENT);
  if (sqlite3_step (stmt) != SQLITE_ROW)
    {
      sqlite3_finalize (stmt);
      LOG (WARNING) << "Settle-confirm for unknown visit " << visitId;
      return;
    }
  const std::string status
      = reinterpret_cast<const char*> (sqlite3_column_text (stmt, 0));
  const bool isParticipant = sqlite3_column_int64 (stmt, 1) > 0;
  sqlite3_finalize (stmt);

  if (status != "active")
    {
      LOG (WARNING) << "Settle-confirm for visit " << visitId
                    << " which is not active (status: " << status << ")";
      return;
    }
  if (!isParticipant)
    {
      LOG (WARNING) << name << " is not a participant of visit " << visitId
                    << " and cannot confirm its settlement";
      return;
    }

  ProcessSettleConfirm (name, visitId, hash, len);
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

  /* The settlement replay runs with the on-chain stats as they are at
     settle time (ComputePlayerStats), so a stat change during a visit
     (solo channel or co-op) would desync the verified run.  */
  if (PlayerInActiveVisit (db, name))
    {
      LOG (WARNING) << "Player " << name << " is in an active visit";
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

  /* The settlement replay runs with the on-chain stats and inventory as
     they are at settle time, so nothing may change them while a visit
     (solo channel or co-op) is open or active.  */
  if (PlayerInActiveVisit (db, name))
    {
      LOG (WARNING) << "Player " << name << " is in an active visit";
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

  /* The settlement replay runs with the on-chain stats and inventory as
     they are at settle time, so nothing may change them while a visit
     (solo channel or co-op) is open or active.  */
  if (PlayerInActiveVisit (db, name))
    {
      LOG (WARNING) << "Player " << name << " is in an active visit";
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

  /* The settlement replay runs with the on-chain stats and inventory as
     they are at settle time, so nothing may change them while a visit
     (solo channel or co-op) is open or active.  */
  if (PlayerInActiveVisit (db, name))
    {
      LOG (WARNING) << "Player " << name << " is in an active visit";
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

  /* The settlement replay runs with the on-chain stats and inventory as
     they are at settle time, so nothing may change them while a visit
     (solo channel or co-op) is open or active.  */
  if (PlayerInActiveVisit (db, name))
    {
      LOG (WARNING) << "Player " << name << " is in an active visit";
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

  /* The action proof: a JSON array of action objects, or the compact
     string encoding (docs/STRATEGY_action_proofs.md).  */
  if (!op.isMember ("actions")
      || !(op["actions"].isArray () || op["actions"].isString ()))
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
          || !s.isMember ("actions")
          || !(s["actions"].isArray () || s["actions"].isString ()))
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
