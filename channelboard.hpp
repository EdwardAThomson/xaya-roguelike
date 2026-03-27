#ifndef ROG_CHANNELBOARD_HPP
#define ROG_CHANNELBOARD_HPP

#include "dungeongame.hpp"

#include "dungeonstate.pb.h"
#include "dungeonmove.pb.h"

#include <gamechannel/boardrules.hpp>
#include <gamechannel/protoboard.hpp>
#include <gamechannel/protoversion.hpp>

namespace rog
{

/**
 * Parsed board state for a dungeon channel.  Wraps a DungeonGame instance
 * and implements the channel framework interface.
 */
class DungeonBoardState
    : public xaya::ProtoBoardState<proto::DungeonState, proto::DungeonMove>
{

private:

  /** The underlying game session (lazy-initialised from proto state).  */
  mutable std::unique_ptr<DungeonGame> game;

  /** Ensures the game is initialised from the proto state.  */
  void EnsureGame () const;

protected:

  bool ApplyMoveProto (const proto::DungeonMove& mv,
                        proto::DungeonState& newState) const override;

public:

  using ProtoBoardState::ProtoBoardState;

  int WhoseTurn () const override;
  unsigned TurnCount () const override;
  Json::Value ToJson () const override;

};

/**
 * Board rules for the dungeon channel game.
 */
class DungeonBoardRules : public xaya::ProtoBoardRules<DungeonBoardState>
{

public:

  xaya::ChannelProtoVersion GetProtoVersion (
      const xaya::proto::ChannelMetadata& meta) const override;

};

} // namespace rog

#endif // ROG_CHANNELBOARD_HPP
