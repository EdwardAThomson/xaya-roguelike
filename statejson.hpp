#ifndef ROG_STATEJSON_HPP
#define ROG_STATEJSON_HPP

#include <json/json.h>
#include <sqlite3.h>

#include <cstdint>
#include <string>

namespace rog
{

/**
 * Read-only extractor for game state JSON from the database.
 */
class StateJsonExtractor
{

private:

  sqlite3* db;

public:

  explicit StateJsonExtractor (sqlite3* d)
    : db(d)
  {}

  /**
   * Returns player info including stats, inventory, and known spells.
   * Returns null JSON if the player doesn't exist.
   */
  Json::Value GetPlayerInfo (const std::string& name) const;

  /**
   * Lists all permanent segments.  Each entry includes a visit_count.
   */
  Json::Value ListSegments () const;

  /**
   * Returns detailed info about a permanent segment including its
   * visit history.  Returns null JSON if the segment doesn't exist.
   */
  Json::Value GetSegmentInfo (int64_t segmentId) const;

  /**
   * Lists visits, optionally filtered by status.
   * Pass empty string for all visits.
   */
  Json::Value ListVisits (const std::string& status) const;

  /**
   * Returns detailed info about a visit including participants and results.
   * Returns null JSON if the visit doesn't exist.
   */
  Json::Value GetVisitInfo (int64_t visitId) const;

  /**
   * Returns the full game state (all players, segments, and active visits).
   */
  Json::Value FullState () const;

};

} // namespace rog

#endif // ROG_STATEJSON_HPP
