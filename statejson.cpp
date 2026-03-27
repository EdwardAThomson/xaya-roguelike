#include "statejson.hpp"
#include "combat.hpp"
#include "items.hpp"

#include <glog/logging.h>

namespace rog
{

Json::Value
StateJsonExtractor::GetPlayerInfo (const std::string& name) const
{
  /* Query player row.  */
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `level`, `xp`, `gold`,"
    " `strength`, `dexterity`, `constitution`, `intelligence`,"
    " `skill_points`, `stat_points`,"
    " `kills`, `deaths`, `visits_completed`, `registered_height`,"
    " `hp`, `max_hp`, `current_segment`, `in_channel`"
    " FROM `players` WHERE `name` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);

  if (sqlite3_step (stmt) != SQLITE_ROW)
    {
      sqlite3_finalize (stmt);
      return Json::Value ();
    }

  Json::Value res (Json::objectValue);
  res["name"] = name;
  res["level"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 0));
  res["xp"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 1));
  res["gold"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 2));

  Json::Value stats (Json::objectValue);
  stats["strength"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 3));
  stats["dexterity"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 4));
  stats["constitution"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 5));
  stats["intelligence"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 6));
  res["stats"] = stats;

  res["skill_points"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 7));
  res["stat_points"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 8));

  Json::Value combat (Json::objectValue);
  combat["kills"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 9));
  combat["deaths"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 10));
  combat["visits_completed"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 11));
  res["combat_record"] = combat;

  res["registered_height"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 12));
  res["hp"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 13));
  res["max_hp"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 14));
  res["current_segment"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 15));
  res["in_channel"] = sqlite3_column_int64 (stmt, 16) != 0;
  sqlite3_finalize (stmt);

  /* Effective combat stats (base + equipment).  */
  const auto effectiveStats = ComputePlayerStats (db, name);
  Json::Value effective (Json::objectValue);
  effective["attack_power"] = PlayerAttackPower (effectiveStats);
  effective["defense"] = PlayerDefense (effectiveStats);
  effective["equip_attack"] = effectiveStats.equipAttack;
  effective["equip_defense"] = effectiveStats.equipDefense;
  res["effective_stats"] = effective;

  /* Query inventory.  */
  Json::Value inventory (Json::arrayValue);
  sqlite3_prepare_v2 (db,
    "SELECT `item_id`, `quantity`, `slot`, `item_data`"
    " FROM `inventory` WHERE `name` = ?1"
    " ORDER BY `slot`, `item_id`",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);

  while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      Json::Value item (Json::objectValue);
      item["item_id"] = reinterpret_cast<const char*> (
          sqlite3_column_text (stmt, 0));
      item["quantity"] = static_cast<Json::Int64> (
          sqlite3_column_int64 (stmt, 1));
      item["slot"] = reinterpret_cast<const char*> (
          sqlite3_column_text (stmt, 2));
      if (sqlite3_column_type (stmt, 3) != SQLITE_NULL)
        item["item_data"] = reinterpret_cast<const char*> (
            sqlite3_column_text (stmt, 3));
      inventory.append (item);
    }
  sqlite3_finalize (stmt);
  res["inventory"] = inventory;

  /* Query known spells.  */
  Json::Value spells (Json::arrayValue);
  sqlite3_prepare_v2 (db,
    "SELECT `spell_id` FROM `known_spells`"
    " WHERE `name` = ?1 ORDER BY `spell_id`",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);

  while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      spells.append (reinterpret_cast<const char*> (
          sqlite3_column_text (stmt, 0)));
    }
  sqlite3_finalize (stmt);
  res["known_spells"] = spells;

  /* Check if player is currently in an active visit.  */
  sqlite3_prepare_v2 (db,
    "SELECT v.`id`, v.`segment_id` FROM `visit_participants` vp"
    " JOIN `visits` v ON vp.`visit_id` = v.`id`"
    " WHERE vp.`name` = ?1"
    " AND (v.`status` = 'open' OR v.`status` = 'active')"
    " LIMIT 1",
    -1, &stmt, nullptr);
  sqlite3_bind_text (stmt, 1, name.c_str (), -1, SQLITE_TRANSIENT);

  if (sqlite3_step (stmt) == SQLITE_ROW)
    {
      Json::Value av (Json::objectValue);
      av["visit_id"] = static_cast<Json::Int64> (
          sqlite3_column_int64 (stmt, 0));
      av["segment_id"] = static_cast<Json::Int64> (
          sqlite3_column_int64 (stmt, 1));
      res["active_visit"] = av;
    }
  else
    res["active_visit"] = Json::Value ();
  sqlite3_finalize (stmt);

  return res;
}

Json::Value
StateJsonExtractor::ListSegments () const
{
  Json::Value result (Json::arrayValue);

  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `id`, `discoverer`, `depth`, `max_players`, `created_height`,"
    " (SELECT COUNT(*) FROM `visits` WHERE `segment_id` = s.`id`)"
    " FROM `segments` s ORDER BY `id`",
    -1, &stmt, nullptr);

  while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      Json::Value seg (Json::objectValue);
      seg["id"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 0));
      seg["discoverer"] = reinterpret_cast<const char*> (
          sqlite3_column_text (stmt, 1));
      seg["depth"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 2));
      seg["max_players"] = static_cast<Json::Int64> (
          sqlite3_column_int64 (stmt, 3));
      seg["created_height"] = static_cast<Json::Int64> (
          sqlite3_column_int64 (stmt, 4));
      seg["visit_count"] = static_cast<Json::Int64> (
          sqlite3_column_int64 (stmt, 5));
      result.append (seg);
    }
  sqlite3_finalize (stmt);

  return result;
}

Json::Value
StateJsonExtractor::GetSegmentInfo (const int64_t segmentId) const
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `discoverer`, `seed`, `depth`, `max_players`, `created_height`"
    " FROM `segments` WHERE `id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, segmentId);

  if (sqlite3_step (stmt) != SQLITE_ROW)
    {
      sqlite3_finalize (stmt);
      return Json::Value ();
    }

  Json::Value res (Json::objectValue);
  res["id"] = static_cast<Json::Int64> (segmentId);
  res["discoverer"] = reinterpret_cast<const char*> (
      sqlite3_column_text (stmt, 0));
  res["seed"] = reinterpret_cast<const char*> (
      sqlite3_column_text (stmt, 1));
  res["depth"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 2));
  res["max_players"] = static_cast<Json::Int64> (
      sqlite3_column_int64 (stmt, 3));
  res["created_height"] = static_cast<Json::Int64> (
      sqlite3_column_int64 (stmt, 4));
  sqlite3_finalize (stmt);

  /* Gates for this segment.  */
  Json::Value gates (Json::objectValue);
  sqlite3_prepare_v2 (db,
    "SELECT `direction`, `x`, `y` FROM `segment_gates`"
    " WHERE `segment_id` = ?1 ORDER BY `direction`",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, segmentId);

  while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      Json::Value g (Json::objectValue);
      const std::string dir = reinterpret_cast<const char*> (
          sqlite3_column_text (stmt, 0));
      g["x"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 1));
      g["y"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 2));
      gates[dir] = g;
    }
  sqlite3_finalize (stmt);
  res["gates"] = gates;

  /* Links from this segment.  */
  Json::Value links (Json::objectValue);
  sqlite3_prepare_v2 (db,
    "SELECT `from_direction`, `to_segment`, `to_direction`"
    " FROM `segment_links` WHERE `from_segment` = ?1"
    " ORDER BY `from_direction`",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, segmentId);

  while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      Json::Value lnk (Json::objectValue);
      const std::string dir = reinterpret_cast<const char*> (
          sqlite3_column_text (stmt, 0));
      lnk["to_segment"] = static_cast<Json::Int64> (
          sqlite3_column_int64 (stmt, 1));
      lnk["to_direction"] = reinterpret_cast<const char*> (
          sqlite3_column_text (stmt, 2));
      links[dir] = lnk;
    }
  sqlite3_finalize (stmt);
  res["links"] = links;

  /* Visit history for this segment.  */
  Json::Value visits (Json::arrayValue);
  sqlite3_prepare_v2 (db,
    "SELECT `id`, `initiator`, `status`, `created_height`"
    " FROM `visits` WHERE `segment_id` = ?1 ORDER BY `id`",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, segmentId);

  while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      Json::Value v (Json::objectValue);
      v["id"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 0));
      v["initiator"] = reinterpret_cast<const char*> (
          sqlite3_column_text (stmt, 1));
      v["status"] = reinterpret_cast<const char*> (
          sqlite3_column_text (stmt, 2));
      v["created_height"] = static_cast<Json::Int64> (
          sqlite3_column_int64 (stmt, 3));
      visits.append (v);
    }
  sqlite3_finalize (stmt);
  res["visits"] = visits;

  return res;
}

Json::Value
StateJsonExtractor::ListVisits (const std::string& status) const
{
  Json::Value result (Json::arrayValue);

  sqlite3_stmt* stmt;
  if (status.empty ())
    {
      sqlite3_prepare_v2 (db,
        "SELECT v.`id`, v.`segment_id`, v.`initiator`, v.`status`,"
        " s.`depth`, s.`max_players`, v.`created_height`,"
        " (SELECT COUNT(*) FROM `visit_participants`"
        "  WHERE `visit_id` = v.`id`)"
        " FROM `visits` v"
        " JOIN `segments` s ON v.`segment_id` = s.`id`"
        " ORDER BY v.`id`",
        -1, &stmt, nullptr);
    }
  else
    {
      sqlite3_prepare_v2 (db,
        "SELECT v.`id`, v.`segment_id`, v.`initiator`, v.`status`,"
        " s.`depth`, s.`max_players`, v.`created_height`,"
        " (SELECT COUNT(*) FROM `visit_participants`"
        "  WHERE `visit_id` = v.`id`)"
        " FROM `visits` v"
        " JOIN `segments` s ON v.`segment_id` = s.`id`"
        " WHERE v.`status` = ?1"
        " ORDER BY v.`id`",
        -1, &stmt, nullptr);
      sqlite3_bind_text (stmt, 1, status.c_str (), -1, SQLITE_TRANSIENT);
    }

  while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      Json::Value vis (Json::objectValue);
      vis["id"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 0));
      vis["segment_id"] = static_cast<Json::Int64> (
          sqlite3_column_int64 (stmt, 1));
      vis["initiator"] = reinterpret_cast<const char*> (
          sqlite3_column_text (stmt, 2));
      vis["status"] = reinterpret_cast<const char*> (
          sqlite3_column_text (stmt, 3));
      vis["depth"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 4));
      vis["max_players"] = static_cast<Json::Int64> (
          sqlite3_column_int64 (stmt, 5));
      vis["created_height"] = static_cast<Json::Int64> (
          sqlite3_column_int64 (stmt, 6));
      vis["players"] = static_cast<Json::Int64> (
          sqlite3_column_int64 (stmt, 7));
      result.append (vis);
    }
  sqlite3_finalize (stmt);

  return result;
}

Json::Value
StateJsonExtractor::GetVisitInfo (const int64_t visitId) const
{
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT v.`segment_id`, v.`initiator`, v.`status`,"
    " v.`created_height`, v.`started_height`, v.`settled_height`,"
    " s.`depth`, s.`seed`"
    " FROM `visits` v"
    " JOIN `segments` s ON v.`segment_id` = s.`id`"
    " WHERE v.`id` = ?1",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visitId);

  if (sqlite3_step (stmt) != SQLITE_ROW)
    {
      sqlite3_finalize (stmt);
      return Json::Value ();
    }

  Json::Value res (Json::objectValue);
  res["id"] = static_cast<Json::Int64> (visitId);
  res["segment_id"] = static_cast<Json::Int64> (
      sqlite3_column_int64 (stmt, 0));
  res["initiator"] = reinterpret_cast<const char*> (
      sqlite3_column_text (stmt, 1));
  res["status"] = reinterpret_cast<const char*> (
      sqlite3_column_text (stmt, 2));
  res["created_height"] = static_cast<Json::Int64> (
      sqlite3_column_int64 (stmt, 3));

  if (sqlite3_column_type (stmt, 4) != SQLITE_NULL)
    res["started_height"] = static_cast<Json::Int64> (
        sqlite3_column_int64 (stmt, 4));
  if (sqlite3_column_type (stmt, 5) != SQLITE_NULL)
    res["settled_height"] = static_cast<Json::Int64> (
        sqlite3_column_int64 (stmt, 5));

  res["depth"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 6));
  res["seed"] = reinterpret_cast<const char*> (
      sqlite3_column_text (stmt, 7));
  sqlite3_finalize (stmt);

  /* Participants.  */
  Json::Value participants (Json::arrayValue);
  sqlite3_prepare_v2 (db,
    "SELECT `name` FROM `visit_participants`"
    " WHERE `visit_id` = ?1 ORDER BY `joined_height`",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visitId);

  while (sqlite3_step (stmt) == SQLITE_ROW)
    participants.append (reinterpret_cast<const char*> (
        sqlite3_column_text (stmt, 0)));
  sqlite3_finalize (stmt);
  res["participants"] = participants;

  /* Results (if settled).  */
  Json::Value results (Json::arrayValue);
  sqlite3_prepare_v2 (db,
    "SELECT `name`, `survived`, `xp_gained`, `gold_gained`, `kills`"
    " FROM `visit_results` WHERE `visit_id` = ?1"
    " ORDER BY `name`",
    -1, &stmt, nullptr);
  sqlite3_bind_int64 (stmt, 1, visitId);

  while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      const std::string pName = reinterpret_cast<const char*> (
          sqlite3_column_text (stmt, 0));

      Json::Value r (Json::objectValue);
      r["name"] = pName;
      r["survived"] = sqlite3_column_int64 (stmt, 1) != 0;
      r["xp_gained"] = static_cast<Json::Int64> (
          sqlite3_column_int64 (stmt, 2));
      r["gold_gained"] = static_cast<Json::Int64> (
          sqlite3_column_int64 (stmt, 3));
      r["kills"] = static_cast<Json::Int64> (
          sqlite3_column_int64 (stmt, 4));

      /* Loot for this player in this visit.  */
      Json::Value loot (Json::arrayValue);
      sqlite3_stmt* lStmt;
      sqlite3_prepare_v2 (db,
        "SELECT `item_id`, `quantity` FROM `loot_claims`"
        " WHERE `visit_id` = ?1 AND `name` = ?2",
        -1, &lStmt, nullptr);
      sqlite3_bind_int64 (lStmt, 1, visitId);
      sqlite3_bind_text (lStmt, 2, pName.c_str (), -1, SQLITE_TRANSIENT);

      while (sqlite3_step (lStmt) == SQLITE_ROW)
        {
          Json::Value l (Json::objectValue);
          l["item_id"] = reinterpret_cast<const char*> (
              sqlite3_column_text (lStmt, 0));
          l["quantity"] = static_cast<Json::Int64> (
              sqlite3_column_int64 (lStmt, 1));
          loot.append (l);
        }
      sqlite3_finalize (lStmt);
      r["loot"] = loot;

      results.append (r);
    }
  sqlite3_finalize (stmt);

  if (!results.empty ())
    res["results"] = results;

  return res;
}

Json::Value
StateJsonExtractor::FullState () const
{
  Json::Value res (Json::objectValue);

  /* All players (summary).  */
  Json::Value players (Json::arrayValue);
  sqlite3_stmt* stmt;
  sqlite3_prepare_v2 (db,
    "SELECT `name`, `level`, `gold`, `kills`, `deaths`, `visits_completed`,"
    " `hp`, `max_hp`, `current_segment`, `in_channel`"
    " FROM `players` ORDER BY `name`",
    -1, &stmt, nullptr);

  while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      Json::Value p (Json::objectValue);
      p["name"] = reinterpret_cast<const char*> (
          sqlite3_column_text (stmt, 0));
      p["level"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 1));
      p["gold"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 2));
      p["kills"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 3));
      p["deaths"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 4));
      p["visits_completed"] = static_cast<Json::Int64> (
          sqlite3_column_int64 (stmt, 5));
      p["hp"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 6));
      p["max_hp"] = static_cast<Json::Int64> (sqlite3_column_int64 (stmt, 7));
      p["current_segment"] = static_cast<Json::Int64> (
          sqlite3_column_int64 (stmt, 8));
      p["in_channel"] = sqlite3_column_int64 (stmt, 9) != 0;
      players.append (p);
    }
  sqlite3_finalize (stmt);
  res["players"] = players;

  /* All permanent segments.  */
  res["segments"] = ListSegments ();

  /* Open and active visits.  */
  res["visits"] = ListVisits ("");

  /* Dungeon ID (world instance identifier).  */
  sqlite3_prepare_v2 (db,
    "SELECT `value` FROM `meta` WHERE `key` = 'dungeon_id'",
    -1, &stmt, nullptr);
  if (sqlite3_step (stmt) == SQLITE_ROW)
    res["dungeon_id"] = reinterpret_cast<const char*> (
        sqlite3_column_text (stmt, 0));
  sqlite3_finalize (stmt);

  return res;
}

} // namespace rog
