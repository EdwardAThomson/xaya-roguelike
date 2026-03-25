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
   * Lists segments, optionally filtered by status.
   * Pass empty string for all segments.
   */
  Json::Value ListSegments (const std::string& status) const;

  /**
   * Returns detailed info about a segment including participants and results.
   * Returns null JSON if the segment doesn't exist.
   */
  Json::Value GetSegmentInfo (int64_t segmentId) const;

  /**
   * Returns the full game state (all players and open/active segments).
   */
  Json::Value FullState () const;

};

} // namespace rog

#endif // ROG_STATEJSON_HPP
