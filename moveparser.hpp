#ifndef ROG_MOVEPARSER_HPP
#define ROG_MOVEPARSER_HPP

#include <json/json.h>
#include <sqlite3.h>

#include <string>

namespace rog
{

/**
 * Checks whether a player with the given name exists in the database.
 */
bool PlayerExists (sqlite3* db, const std::string& name);

/**
 * Checks whether a player is currently participating in any open or active
 * segment.
 */
bool PlayerInActiveSegment (sqlite3* db, const std::string& name);

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
  void HandleJoin (const std::string& name, const Json::Value& op);
  void HandleLeave (const std::string& name, const Json::Value& op);
  void HandleSettle (const std::string& name, const Json::Value& op);
  void HandleAllocateStat (const std::string& name, const Json::Value& op);

protected:

  /** The database handle used for reading current state.  */
  sqlite3* db;

  /** Current block height.  */
  unsigned currentHeight;

  virtual void ProcessRegister (const std::string& name) = 0;
  virtual void ProcessDiscover (const std::string& name, int depth,
                                 const std::string& txid) = 0;
  virtual void ProcessJoin (const std::string& name, int64_t segmentId) = 0;
  virtual void ProcessLeave (const std::string& name, int64_t segmentId) = 0;
  virtual void ProcessSettle (const std::string& name, int64_t segmentId,
                               const Json::Value& results) = 0;
  virtual void ProcessAllocateStat (const std::string& name,
                                     const std::string& stat) = 0;

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
