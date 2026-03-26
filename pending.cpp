#include "pending.hpp"

#include "logic.hpp"

#include <glog/logging.h>

namespace rog
{

/* ************************************************************************** */

PendingState::PendingState ()
  : pendingDiscovers(Json::arrayValue),
    pendingVisits(Json::arrayValue),
    pendingJoins(Json::arrayValue),
    pendingTravels(Json::arrayValue),
    pendingChannelEntries(Json::arrayValue)
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
PendingState::AddVisit (const std::string& name, const int64_t segmentId)
{
  Json::Value entry (Json::objectValue);
  entry["name"] = name;
  entry["segment_id"] = static_cast<Json::Int64> (segmentId);
  pendingVisits.append (entry);
}

void
PendingState::AddJoin (const std::string& name, const int64_t visitId)
{
  Json::Value entry (Json::objectValue);
  entry["name"] = name;
  entry["visit_id"] = static_cast<Json::Int64> (visitId);
  pendingJoins.append (entry);
}

void
PendingState::AddTravel (const std::string& name, const std::string& dir)
{
  Json::Value entry (Json::objectValue);
  entry["name"] = name;
  entry["direction"] = dir;
  pendingTravels.append (entry);
}

void
PendingState::AddEnterChannel (const std::string& name, const int64_t segmentId)
{
  Json::Value entry (Json::objectValue);
  entry["name"] = name;
  entry["segment_id"] = static_cast<Json::Int64> (segmentId);
  pendingChannelEntries.append (entry);
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
  res["visits"] = pendingVisits;
  res["joins"] = pendingJoins;
  res["travels"] = pendingTravels;
  res["channel_entries"] = pendingChannelEntries;

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
  else if (move.isMember ("v") && move["v"].isObject ()
           && move["v"].isMember ("id") && move["v"]["id"].isInt64 ())
    {
      state.AddVisit (name, move["v"]["id"].asInt64 ());
    }
  else if (move.isMember ("j") && move["j"].isObject ()
           && move["j"].isMember ("id") && move["j"]["id"].isInt64 ())
    {
      state.AddJoin (name, move["j"]["id"].asInt64 ());
    }
  else if (move.isMember ("t") && move["t"].isObject ()
           && move["t"].isMember ("dir") && move["t"]["dir"].isString ())
    {
      state.AddTravel (name, move["t"]["dir"].asString ());
    }
  else if (move.isMember ("ec") && move["ec"].isObject ()
           && move["ec"].isMember ("id") && move["ec"]["id"].isInt64 ())
    {
      state.AddEnterChannel (name, move["ec"]["id"].asInt64 ());
    }
  /* Other move types (settle, leave, stat alloc, use item, equip,
     exit channel) are less interesting to show as pending.  */
}

Json::Value
PendingMoves::ToJson () const
{
  return state.ToJson ();
}

/* ************************************************************************** */

} // namespace rog
