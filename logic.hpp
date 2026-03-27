#ifndef ROG_LOGIC_HPP
#define ROG_LOGIC_HPP

#include "statejson.hpp"

#include <gamechannel/channelgame.hpp>
#include <xayagame/sqlitestorage.hpp>

#include <json/json.h>

#include <functional>
#include <string>

namespace rog
{

/**
 * The game logic implementation for the roguelike GSP.
 * Extends ChannelGame to support game channels for dungeon exploration.
 */
class RoguelikeLogic : public xaya::ChannelGame
{

private:

  unsigned genesisHeight = 0;
  std::string genesisHash = "";

protected:

  void SetupSchema (xaya::SQLiteDatabase& db) override;

  void GetInitialStateBlock (unsigned& height,
                             std::string& hashHex) const override;
  void InitialiseState (xaya::SQLiteDatabase& db) override;

  void UpdateState (xaya::SQLiteDatabase& db,
                    const Json::Value& blockData) override;

  Json::Value GetStateAsJson (const xaya::SQLiteDatabase& db) override;

  const xaya::BoardRules& GetBoardRules () const override;

public:

  /**
   * Type for a callback that extracts custom JSON from the game state
   * through a StateJsonExtractor instance.
   */
  using StateCallback
      = std::function<Json::Value (const StateJsonExtractor& ext)>;

  RoguelikeLogic () = default;

  RoguelikeLogic (const RoguelikeLogic&) = delete;
  void operator= (const RoguelikeLogic&) = delete;

  /**
   * Sets the genesis block configuration.  If not called, defaults to
   * height 0 with an empty hash (accepts any block at height 0).
   */
  void SetGenesisBlock (unsigned height, const std::string& hashHex);

  /**
   * Extracts custom JSON from the current game-state database using
   * the provided extractor callback.
   */
  Json::Value GetCustomStateData (xaya::Game& game, const StateCallback& cb);

  /* Expose the super-class variant as well, used for state hashing.  */
  using SQLiteGame::GetCustomStateData;

};

} // namespace rog

#endif // ROG_LOGIC_HPP
