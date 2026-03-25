#include "pending.hpp"

#include "logic.hpp"

#include <glog/logging.h>

namespace rog
{

/* ************************************************************************** */

PendingState::PendingState ()
  : pendingDiscovers(Json::arrayValue),
    pendingJoins(Json::arrayValue)
{}

void
PendingState::AddRegistration (const std::string& name)
{
  pendingRegistrations.insert (name);
}

void
PendingState::AddDiscover (const std::string& name, const int depth)
{
  Json::Value entry (Json::objectValue);
  entry["name"] = name;
  entry["depth"] = depth;
  pendingDiscovers.append (entry);
}

void
PendingState::AddJoin (const std::string& name, const int64_t segmentId)
{
  Json::Value entry (Json::objectValue);
  entry["name"] = name;
  entry["segment_id"] = static_cast<Json::Int64> (segmentId);
  pendingJoins.append (entry);
}

Json::Value
PendingState::ToJson () const
{
  Json::Value res (Json::objectValue);

  Json::Value regs (Json::arrayValue);
  for (const auto& name : pendingRegistrations)
    regs.append (name);
  res["registrations"] = regs;

  res["discovers"] = pendingDiscovers;
  res["joins"] = pendingJoins;

  return res;
}

/* ************************************************************************** */

PendingMoves::PendingMoves (RoguelikeLogic& rules)
  : xaya::SQLiteGame::PendingMoves(rules)
{}

void
PendingMoves::Clear ()
{
  state = PendingState ();
}

void
PendingMoves::AddPendingMove (const Json::Value& mv)
{
  if (!mv.isObject ())
    return;

  const auto& nameVal = mv["name"];
  if (!nameVal.isString ())
    return;
  const std::string name = nameVal.asString ();

  const auto& move = mv["move"];
  if (!move.isObject () || move.size () != 1)
    return;

  if (move.isMember ("r") && move["r"].isObject () && move["r"].empty ())
    {
      state.AddRegistration (name);
    }
  else if (move.isMember ("d") && move["d"].isObject ()
           && move["d"].isMember ("depth") && move["d"]["depth"].isInt ())
    {
      state.AddDiscover (name, move["d"]["depth"].asInt ());
    }
  else if (move.isMember ("j") && move["j"].isObject ()
           && move["j"].isMember ("id") && move["j"]["id"].isInt64 ())
    {
      state.AddJoin (name, move["j"]["id"].asInt64 ());
    }
  /* Other move types (settle, leave, stat alloc) are less interesting
     to show as pending — they either happen rarely or are instant.  */
}

Json::Value
PendingMoves::ToJson () const
{
  return state.ToJson ();
}

/* ************************************************************************** */

} // namespace rog
