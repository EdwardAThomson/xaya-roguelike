#ifndef ROG_PENDING_HPP
#define ROG_PENDING_HPP

#include <xayagame/sqlitegame.hpp>

#include <json/json.h>

#include <set>
#include <string>
#include <vector>

namespace rog
{

class RoguelikeLogic;

/**
 * Tracks the current pending state from unconfirmed moves in the mempool.
 * This is intentionally simple — we just record what actions are pending
 * so the UI can show "registration pending", "join pending", etc.
 */
class PendingState
{

private:

  /** Names with pending registrations.  */
  std::set<std::string> pendingRegistrations;

  /** Pending segment discoveries: (name, depth) pairs.  */
  Json::Value pendingDiscovers;

  /** Pending visits to existing segments: (name, segment_id) pairs.  */
  Json::Value pendingVisits;

  /** Pending visit joins: (name, visit_id) pairs.  */
  Json::Value pendingJoins;

public:

  PendingState ();

  void AddRegistration (const std::string& name);
  void AddDiscover (const std::string& name, int depth);
  void AddVisit (const std::string& name, int64_t segmentId);
  void AddJoin (const std::string& name, int64_t visitId);

  Json::Value ToJson () const;

};

/**
 * The tracker for pending moves, using the libxayagame framework.
 */
class PendingMoves : public xaya::SQLiteGame::PendingMoves
{

private:

  PendingState state;

protected:

  void Clear () override;
  void AddPendingMove (const Json::Value& mv) override;

public:

  explicit PendingMoves (RoguelikeLogic& rules);

  Json::Value ToJson () const override;

};

} // namespace rog

#endif // ROG_PENDING_HPP
