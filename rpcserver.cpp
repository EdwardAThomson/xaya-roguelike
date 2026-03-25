#include "rpcserver.hpp"

#include "statejson.hpp"

#include <xayagame/gamerpcserver.hpp>

#include <glog/logging.h>

namespace rog
{

void
RpcServer::stop ()
{
  LOG (INFO) << "RPC method called: stop";
  game.RequestStop ();
}

Json::Value
RpcServer::getcurrentstate ()
{
  LOG (INFO) << "RPC method called: getcurrentstate";
  return game.GetCurrentJsonState ();
}

Json::Value
RpcServer::getnullstate ()
{
  LOG (INFO) << "RPC method called: getnullstate";
  return game.GetNullJsonState ();
}

Json::Value
RpcServer::getpendingstate ()
{
  LOG (INFO) << "RPC method called: getpendingstate";
  return game.GetPendingJsonState ();
}

std::string
RpcServer::waitforchange (const std::string& knownBlock)
{
  LOG (INFO) << "RPC method called: waitforchange " << knownBlock;
  return xaya::GameRpcServer::DefaultWaitForChange (game, knownBlock);
}

Json::Value
RpcServer::waitforpendingchange (const int knownVersion)
{
  LOG (INFO) << "RPC method called: waitforpendingchange " << knownVersion;
  return game.WaitForPendingChange (knownVersion);
}

Json::Value
RpcServer::getplayerinfo (const std::string& name)
{
  LOG (INFO) << "RPC method called: getplayerinfo " << name;
  return logic.GetCustomStateData (game,
      [&name] (const StateJsonExtractor& ext)
        {
          return ext.GetPlayerInfo (name);
        });
}

Json::Value
RpcServer::listsegments ()
{
  LOG (INFO) << "RPC method called: listsegments";
  return logic.GetCustomStateData (game,
      [] (const StateJsonExtractor& ext)
        {
          return ext.ListSegments ();
        });
}

Json::Value
RpcServer::getsegmentinfo (const int segmentId)
{
  LOG (INFO) << "RPC method called: getsegmentinfo " << segmentId;
  return logic.GetCustomStateData (game,
      [segmentId] (const StateJsonExtractor& ext)
        {
          return ext.GetSegmentInfo (segmentId);
        });
}

Json::Value
RpcServer::listvisits (const std::string& status)
{
  LOG (INFO) << "RPC method called: listvisits " << status;
  return logic.GetCustomStateData (game,
      [&status] (const StateJsonExtractor& ext)
        {
          return ext.ListVisits (status);
        });
}

Json::Value
RpcServer::getvisitinfo (const int visitId)
{
  LOG (INFO) << "RPC method called: getvisitinfo " << visitId;
  return logic.GetCustomStateData (game,
      [visitId] (const StateJsonExtractor& ext)
        {
          return ext.GetVisitInfo (visitId);
        });
}

} // namespace rog
