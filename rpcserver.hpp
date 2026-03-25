#ifndef ROG_RPCSERVER_HPP
#define ROG_RPCSERVER_HPP

#include "logic.hpp"

#include <xayagame/game.hpp>

#include <json/json.h>
#include <jsonrpccpp/server/abstractserverconnector.h>

#include "rpc-stubs/rogrpcserverstub.h"

#include <string>

namespace rog
{

class RpcServer : public RogRpcServerStub
{

private:

  xaya::Game& game;
  RoguelikeLogic& logic;

public:

  RpcServer (xaya::Game& g, RoguelikeLogic& l,
             jsonrpc::AbstractServerConnector& conn)
    : RogRpcServerStub(conn), game(g), logic(l)
  {}

  void stop () override;
  Json::Value getcurrentstate () override;
  Json::Value getnullstate () override;
  Json::Value getpendingstate () override;
  std::string waitforchange (const std::string& knownBlock) override;
  Json::Value waitforpendingchange (int knownVersion) override;

  Json::Value getplayerinfo (const std::string& name) override;
  Json::Value listsegments (const std::string& status) override;
  Json::Value getsegmentinfo (int segmentId) override;

};

} // namespace rog

#endif // ROG_RPCSERVER_HPP
