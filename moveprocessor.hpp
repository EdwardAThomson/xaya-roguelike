#ifndef ROG_MOVEPROCESSOR_HPP
#define ROG_MOVEPROCESSOR_HPP

#include "moveparser.hpp"

#include <json/json.h>
#include <sqlite3.h>

#include <cstdint>
#include <string>

namespace rog
{

/**
 * Processor for moves in confirmed blocks.  Validates via MoveParser
 * and then mutates the game-state database.
 */
class MoveProcessor : private MoveParser
{

private:

  /** Next segment ID (simple counter, deterministic across nodes).  */
  int64_t& nextSegmentId;

  /**
   * Inserts the starting items for a newly registered player.
   */
  void GiveStartingItems (const std::string& name);

  /**
   * Returns the number of participants currently in a segment.
   */
  int64_t CountParticipants (int64_t segmentId);

  /**
   * Returns the max_players for a segment.
   */
  int64_t GetMaxPlayers (int64_t segmentId);

protected:

  void ProcessRegister (const std::string& name) override;
  void ProcessDiscover (const std::string& name, int depth,
                         const std::string& txid) override;
  void ProcessJoin (const std::string& name, int64_t segmentId) override;
  void ProcessLeave (const std::string& name, int64_t segmentId) override;
  void ProcessSettle (const std::string& name, int64_t segmentId,
                       const Json::Value& results) override;
  void ProcessAllocateStat (const std::string& name,
                             const std::string& stat) override;

public:

  MoveProcessor (sqlite3* d, unsigned height, int64_t& nextId)
    : MoveParser(d, height), nextSegmentId(nextId)
  {}

  /** Blocks before an open segment expires (not enough players joined).  */
  static constexpr unsigned SEGMENT_OPEN_TIMEOUT = 100;

  /** Blocks before an active segment force-settles (too long without
      a settlement submission).  */
  static constexpr unsigned SEGMENT_ACTIVE_TIMEOUT = 1000;

  MoveProcessor () = delete;
  MoveProcessor (const MoveProcessor&) = delete;
  void operator= (const MoveProcessor&) = delete;

  /**
   * Processes all moves from a block, then runs timeout processing.
   */
  void ProcessAll (const Json::Value& moves);

  /**
   * Expires open segments that have been waiting too long and
   * force-settles active segments that have exceeded the active timeout.
   * Called automatically at the end of ProcessAll.
   */
  void ProcessTimeouts ();

};

} // namespace rog

#endif // ROG_MOVEPROCESSOR_HPP
