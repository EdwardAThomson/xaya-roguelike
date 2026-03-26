#include "logic.hpp"
#include "pending.hpp"
#include "rpcserver.hpp"

#include <xayagame/defaultmain.hpp>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace
{

DEFINE_string (xaya_rpc_url, "",
               "URL at which Xaya Core's JSON-RPC interface is available");
DEFINE_int32 (xaya_rpc_protocol, 1,
              "JSON-RPC version for connecting to Xaya Core");
DEFINE_bool (xaya_rpc_wait, false,
             "whether to wait on startup for Xaya Core to be available");

DEFINE_int32 (game_rpc_port, 0,
              "the port at which the GSP JSON-RPC server will be started"
              " (if non-zero)");
DEFINE_bool (game_rpc_listen_locally, true,
             "whether the GSP's JSON-RPC server should listen locally");

DEFINE_int32 (enable_pruning, -1,
              "if non-negative (including zero), old undo data will be pruned"
              " and only as many blocks as specified will be kept");

DEFINE_string (datadir, "",
               "base data directory for state data"
               " (will be extended by 'rog' and the chain)");

DEFINE_bool (pending_moves, true,
             "whether or not pending moves should be tracked");

DEFINE_uint64 (genesis_height, 0,
               "block height at which the game's initial state is defined");
DEFINE_string (genesis_hash, "",
               "block hash (hex) at the genesis height"
               " (empty accepts any block at that height)");

class RogInstanceFactory : public xaya::CustomisedInstanceFactory
{

private:

  rog::RoguelikeLogic& logic;

public:

  explicit RogInstanceFactory (rog::RoguelikeLogic& l)
    : logic(l)
  {}

  std::unique_ptr<xaya::RpcServerInterface>
  BuildRpcServer (xaya::Game& game,
                  jsonrpc::AbstractServerConnector& conn) override
  {
    std::unique_ptr<xaya::RpcServerInterface> res;
    res.reset (new xaya::WrappedRpcServer<rog::RpcServer> (
        game, logic, conn));
    return res;
  }

};

} // anonymous namespace

int
main (int argc, char** argv)
{
  google::InitGoogleLogging (argv[0]);

  gflags::SetUsageMessage ("Run roguelike GSP");
  gflags::SetVersionString ("0.1.0");
  gflags::ParseCommandLineFlags (&argc, &argv, true);

  if (FLAGS_xaya_rpc_url.empty ())
    {
      std::cerr << "Error: --xaya_rpc_url must be set" << std::endl;
      return EXIT_FAILURE;
    }
  if (FLAGS_datadir.empty ())
    {
      std::cerr << "Error: --datadir must be specified" << std::endl;
      return EXIT_FAILURE;
    }

  xaya::GameDaemonConfiguration config;
  config.XayaRpcUrl = FLAGS_xaya_rpc_url;
  config.XayaJsonRpcProtocol = FLAGS_xaya_rpc_protocol;
  config.XayaRpcWait = FLAGS_xaya_rpc_wait;
  if (FLAGS_game_rpc_port != 0)
    {
      config.GameRpcServer = xaya::RpcServerType::HTTP;
      config.GameRpcPort = FLAGS_game_rpc_port;
      config.GameRpcListenLocally = FLAGS_game_rpc_listen_locally;
    }
  config.EnablePruning = FLAGS_enable_pruning;
  config.DataDirectory = FLAGS_datadir;

  /* When running against Xaya X, the reported version is lower than
     what libxayagame expects from a real Xaya Core node.  Disable
     the version check so the GSP works with any backend.  */
  if (FLAGS_xaya_rpc_protocol == 2)
    config.MinXayaVersion = 0;

  rog::RoguelikeLogic logic;
  logic.SetGenesisBlock (FLAGS_genesis_height, FLAGS_genesis_hash);

  RogInstanceFactory fact (logic);
  config.InstanceFactory = &fact;

  rog::PendingMoves pending (logic);
  if (FLAGS_pending_moves)
    config.PendingMoves = &pending;

  return xaya::SQLiteMain (config, "rog", logic);
}
