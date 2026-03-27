#include "channelboard.hpp"

#include "dungeonstate.pb.h"
#include "dungeonmove.pb.h"

#include <glog/logging.h>

#include <sstream>

namespace rog
{

namespace
{

/** Converts a DungeonGame to a proto::DungeonState.  */
void
GameToProto (const DungeonGame& game, proto::DungeonState& state)
{
  state.set_player_x (game.GetPlayerX ());
  state.set_player_y (game.GetPlayerY ());
  state.set_player_hp (game.GetPlayerHp ());
  state.set_player_max_hp (game.GetPlayerMaxHp ());
  state.set_turn_count (game.GetTurnCount ());
  state.set_total_xp (game.GetTotalXp ());
  state.set_total_gold (game.GetTotalGold ());
  state.set_total_kills (game.GetTotalKills ());
  state.set_game_over (game.IsGameOver ());
  state.set_survived (game.HasSurvived ());
  state.set_exit_gate (game.GetExitGate ());
  state.set_depth (game.GetDepth ());

  for (const auto& m : game.GetMonsters ())
    {
      auto* pm = state.add_monsters ();
      pm->set_name (m.name);
      pm->set_symbol (m.symbol);
      pm->set_x (m.x);
      pm->set_y (m.y);
      pm->set_hp (m.hp);
      pm->set_max_hp (m.maxHp);
      pm->set_attack (m.attack);
      pm->set_defense (m.defense);
      pm->set_crit_chance (m.critChance);
      pm->set_detection_range (m.detectionRange);
      pm->set_xp_value (m.xpValue);
      pm->set_alive (m.alive);
      pm->set_aware_of_player (m.awareOfPlayer);
    }

  for (const auto& gi : game.GetGroundItems ())
    {
      auto* pi = state.add_ground_items ();
      pi->set_x (gi.x);
      pi->set_y (gi.y);
      pi->set_item_id (gi.itemId);
      pi->set_quantity (gi.quantity);
    }

  for (const auto& l : game.GetLoot ())
    {
      auto* pl = state.add_loot ();
      pl->set_item_id (l.itemId);
      pl->set_quantity (l.quantity);
    }

  state.set_rng_state (game.SerializeRng ());
}

/** Reconstructs a DungeonGame from a proto::DungeonState.  */
DungeonGame
ProtoToGame (const proto::DungeonState& state)
{
  DungeonGame game;

  /* Regenerate the dungeon from seed+depth.  */
  game.SetDungeon (Dungeon::Generate (state.seed (), state.depth ()));
  game.SetDepth (state.depth ());

  /* Restore player stats.  */
  if (state.has_player_stats ())
    {
      PlayerStats stats;
      stats.level = state.player_stats ().level ();
      stats.strength = state.player_stats ().strength ();
      stats.dexterity = state.player_stats ().dexterity ();
      stats.constitution = state.player_stats ().constitution ();
      stats.intelligence = state.player_stats ().intelligence ();
      stats.equipAttack = state.player_stats ().equip_attack ();
      stats.equipDefense = state.player_stats ().equip_defense ();
      game.SetStats (stats);
    }

  /* Restore game state.  */
  game.SetState (
    state.player_x (), state.player_y (),
    state.player_hp (), state.player_max_hp (),
    state.turn_count (), state.total_xp (),
    state.total_gold (), state.total_kills (),
    state.game_over (), state.survived (),
    state.exit_gate ());

  /* Restore monsters.  */
  auto& monsters = game.MutableMonsters ();
  monsters.clear ();
  for (const auto& pm : state.monsters ())
    {
      Monster m;
      m.name = pm.name ();
      m.symbol = pm.symbol ();
      m.x = pm.x ();
      m.y = pm.y ();
      m.hp = pm.hp ();
      m.maxHp = pm.max_hp ();
      m.attack = pm.attack ();
      m.defense = pm.defense ();
      m.critChance = pm.crit_chance ();
      m.detectionRange = pm.detection_range ();
      m.xpValue = pm.xp_value ();
      m.alive = pm.alive ();
      m.awareOfPlayer = pm.aware_of_player ();
      monsters.push_back (m);
    }

  /* Restore ground items.  */
  auto& groundItems = game.MutableGroundItems ();
  groundItems.clear ();
  for (const auto& pi : state.ground_items ())
    {
      GroundItem gi;
      gi.x = pi.x ();
      gi.y = pi.y ();
      gi.itemId = pi.item_id ();
      gi.quantity = pi.quantity ();
      groundItems.push_back (gi);
    }

  /* Restore loot.  */
  auto& loot = game.MutableLoot ();
  loot.clear ();
  for (const auto& pl : state.loot ())
    loot.push_back ({pl.item_id (), pl.quantity ()});

  /* Restore RNG state.  */
  if (!state.rng_state ().empty ())
    game.RestoreRng (state.rng_state ());

  return game;
}

} // anonymous namespace

/* ************************************************************************** */

void
DungeonBoardState::EnsureGame () const
{
  if (game != nullptr)
    return;
  game = std::make_unique<DungeonGame> (ProtoToGame (GetState ()));
}

int
DungeonBoardState::WhoseTurn () const
{
  EnsureGame ();
  if (game->IsGameOver ())
    return NO_TURN;
  /* Solo channel: player is always participant 0.  */
  return 0;
}

unsigned
DungeonBoardState::TurnCount () const
{
  return GetState ().turn_count ();
}

bool
DungeonBoardState::ApplyMoveProto (const proto::DungeonMove& mv,
                                    proto::DungeonState& newState) const
{
  EnsureGame ();

  /* Convert proto move to Action.  */
  Action action;
  if (mv.has_move ())
    {
      action.type = Action::Type::Move;
      action.dx = mv.move ().dx ();
      action.dy = mv.move ().dy ();
    }
  else if (mv.has_pickup ())
    action.type = Action::Type::Pickup;
  else if (mv.has_use_item ())
    {
      action.type = Action::Type::UseItem;
      action.itemId = mv.use_item ();
    }
  else if (mv.has_enter_gate ())
    action.type = Action::Type::EnterGate;
  else if (mv.has_wait ())
    action.type = Action::Type::Wait;
  else
    return false;

  /* We need to work on a copy of the game since EnsureGame caches it.
     Make a fresh copy from the current state proto.  */
  auto gameCopy = ProtoToGame (GetState ());
  if (!gameCopy.ProcessAction (action))
    return false;

  /* Serialize the new state.  Copy identity fields from current state.  */
  newState = GetState ();
  GameToProto (gameCopy, newState);

  return true;
}

Json::Value
DungeonBoardState::ToJson () const
{
  EnsureGame ();

  Json::Value res (Json::objectValue);
  res["turn"] = game->GetTurnCount ();
  res["player_x"] = game->GetPlayerX ();
  res["player_y"] = game->GetPlayerY ();
  res["player_hp"] = game->GetPlayerHp ();
  res["player_max_hp"] = game->GetPlayerMaxHp ();
  res["game_over"] = game->IsGameOver ();
  res["survived"] = game->HasSurvived ();
  res["kills"] = game->GetTotalKills ();
  res["xp"] = game->GetTotalXp ();
  res["gold"] = game->GetTotalGold ();

  Json::Value monstersJson (Json::arrayValue);
  for (const auto& m : game->GetMonsters ())
    if (m.alive)
      {
        Json::Value mj (Json::objectValue);
        mj["name"] = m.name;
        mj["x"] = m.x;
        mj["y"] = m.y;
        mj["hp"] = m.hp;
        monstersJson.append (mj);
      }
  res["monsters"] = monstersJson;

  return res;
}

/* ************************************************************************** */

xaya::ChannelProtoVersion
DungeonBoardRules::GetProtoVersion (
    const xaya::proto::ChannelMetadata& meta) const
{
  /* Use the default / latest version.  */
  return xaya::ChannelProtoVersion::ORIGINAL;
}

} // namespace rog
