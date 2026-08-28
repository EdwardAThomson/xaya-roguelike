#ifndef ROG_MOVEPARSER_HPP
#define ROG_MOVEPARSER_HPP

#include "segmentkey.hpp"

#include <json/json.h>
#include <sqlite3.h>

#include <cstdint>
#include <optional>
#include <string>

namespace rog
{

/**
 * Checks whether a player with the given name exists in the database.
 */
bool PlayerExists (sqlite3* db, const std::string& name);

/**
 * Checks whether a player is currently participating in any open or active
 * visit.
 */
bool PlayerInActiveVisit (sqlite3* db, const std::string& name);

/**
 * Checks whether a player is currently inside a channel session.
 */
bool PlayerInChannel (sqlite3* db, const std::string& name);

/**
 * Returns the world coordinate the player is currently on.  A player who
 * does not exist reads as the hub, (0, 0).
 */
SegmentKey CurrentSegment (sqlite3* db, const std::string& name);

/**
 * Checks whether a segment exists at the given coordinate.  The hub, (0, 0),
 * has no row in `segments` and so reads as non-existent here; callers that
 * accept the hub test for it explicitly.
 */
bool SegmentExists (sqlite3* db, const SegmentKey& seg);

/**
 * Core move parser and validator.  Validates moves against the current
 * database state and dispatches to virtual Process* methods that subclasses
 * implement to either update the DB or track pending state.
 */
class MoveParser
{

private:

  void HandleOperation (const std::string& name, const std::string& txid,
                        const Json::Value& mv);

  void HandleRegister (const std::string& name, const Json::Value& op);
  void HandleDiscover (const std::string& name, const std::string& txid,
                       const Json::Value& op);
  void HandleVisit (const std::string& name, const Json::Value& op);
  void HandleJoin (const std::string& name, const Json::Value& op);
  void HandleLeave (const std::string& name, const Json::Value& op);
  void HandleSettle (const std::string& name, const Json::Value& op);
  void HandleAllocateStat (const std::string& name, const Json::Value& op);
  void HandleTravel (const std::string& name, const std::string& txid,
                     const Json::Value& op);
  void HandleUseItem (const std::string& name, const Json::Value& op);
  void HandleEquip (const std::string& name, const Json::Value& op);
  void HandleUnequip (const std::string& name, const Json::Value& op);
  void HandleDiscard (const std::string& name, const Json::Value& op);
  void HandleEnterChannel (const std::string& name, const Json::Value& op);
  void HandleExitChannel (const std::string& name, const Json::Value& op);
  void HandleGateWalk (const std::string& name, const std::string& txid,
                       const Json::Value& op);

protected:

  /** The database handle used for reading current state.  */
  sqlite3* db;

  /** Current block height.  */
  unsigned currentHeight;

  virtual void ProcessRegister (const std::string& name) = 0;
  virtual void ProcessDiscover (const std::string& name, int depth,
                                 const std::string& txid,
                                 const std::string& dir) = 0;
  virtual void ProcessVisit (const std::string& name,
                              const SegmentKey& seg) = 0;
  virtual void ProcessJoin (const std::string& name, int64_t visitId) = 0;
  virtual void ProcessLeave (const std::string& name, int64_t visitId) = 0;
  virtual void ProcessSettle (const std::string& name, int64_t visitId,
                               const Json::Value& results) = 0;
  virtual void ProcessAllocateStat (const std::string& name,
                                     const std::string& stat) = 0;
  virtual void ProcessTravel (const std::string& name,
                               const std::string& dir,
                               const std::string& txid) = 0;
  virtual void ProcessUseItem (const std::string& name,
                                const std::string& itemId) = 0;
  virtual void ProcessEquip (const std::string& name,
                              int64_t rowid, const std::string& slot) = 0;
  virtual void ProcessUnequip (const std::string& name, int64_t rowid) = 0;
  virtual void ProcessDiscardItem (const std::string& name, int64_t rowid) = 0;
  virtual void ProcessEnterChannel (const std::string& name,
                                     const SegmentKey& seg,
                                     const std::string& entryDir) = 0;
  virtual void ProcessExitChannel (const std::string& name,
                                    int64_t visitId,
                                    const Json::Value& results,
                                    const Json::Value& actions) = 0;

  /**
   * Atomic gate-walk: settles the current dungeon (if any), then transits
   * to the neighbouring segment in the given direction (discovering it
   * first if it doesn't exist) and enters its channel.  See
   * MoveProcessor::ProcessGateWalk for the dispatch table.  `settlement`
   * is the {results, actions} object when the player is in a channel,
   * or Json::Value::nullSingleton () when walking from the hub.
   */
  virtual void ProcessGateWalk (const std::string& name,
                                 const std::string& txid,
                                 const std::string& dir,
                                 const Json::Value& settlement) = 0;

public:

  MoveParser (sqlite3* d, unsigned height)
    : db(d), currentHeight(height)
  {}

  virtual ~MoveParser () = default;

  MoveParser () = delete;
  MoveParser (const MoveParser&) = delete;
  void operator= (const MoveParser&) = delete;

  /**
   * Processes a single move from the block's moves array.
   */
  void ProcessOne (const Json::Value& obj);

};

} // namespace rog

#endif // ROG_MOVEPARSER_HPP
